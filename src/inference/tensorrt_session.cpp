#include "anpr/inference/tensorrt_session.hpp"

#include <NvInfer.h>
#include <NvInferVersion.h>
#include <NvOnnxParser.h>
#include <cuda_runtime_api.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "anpr/common/filesystem.hpp"
#include "anpr/common/logging.hpp"

namespace anpr {
namespace {

class TrtLogger final : public nvinfer1::ILogger {
public:
    void log(Severity severity, const char* message) noexcept override {
        if (severity <= Severity::kWARNING && message != nullptr) {
            messages_ << message << '\n';
        }
    }

    [[nodiscard]] std::string messages() const { return messages_.str(); }

private:
    std::ostringstream messages_;
};

template <typename T>
struct TrtDestroy {
    void operator()(T* value) const noexcept {
        if (value != nullptr) value->destroy();
    }
};

template <typename T>
using TrtPtr = std::unique_ptr<T, TrtDestroy<T>>;

void checkCuda(cudaError_t status, const char* operation) {
    if (status != cudaSuccess) {
        throw std::runtime_error(std::string(operation) + ": " + cudaGetErrorString(status));
    }
}

std::size_t volume(const nvinfer1::Dims& dims) {
    std::size_t result = 1;
    for (int index = 0; index < dims.nbDims; ++index) {
        if (dims.d[index] <= 0) {
            throw std::runtime_error("TensorRT left a dynamic tensor dimension unresolved");
        }
        result *= static_cast<std::size_t>(dims.d[index]);
    }
    return result;
}

std::vector<std::int64_t> shapeOf(const nvinfer1::Dims& dims) {
    std::vector<std::int64_t> shape;
    shape.reserve(static_cast<std::size_t>(dims.nbDims));
    for (int index = 0; index < dims.nbDims; ++index) {
        shape.push_back(dims.d[index]);
    }
    return shape;
}

std::string cacheName(const SessionRequest& request) {
    const filesystem::path model(request.model_path);
    std::ifstream input(model, std::ios::binary);
    if (!input) throw std::runtime_error("cannot read TensorRT ONNX model " + model.string());
    std::uint64_t fingerprint = 14695981039346656037ULL;
    char chunk[64 * 1024];
    while (input.read(chunk, sizeof(chunk)) || input.gcount() > 0) {
        for (std::streamsize index = 0; index < input.gcount(); ++index) {
            fingerprint ^= static_cast<unsigned char>(chunk[index]);
            fingerprint *= 1099511628211ULL;
        }
    }
    std::string stem = model.filename().string();
    for (char& ch : stem) {
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '-' && ch != '_') ch = '_';
    }
    std::ostringstream suffix;
    suffix << std::hex << fingerprint;
    return stem + "." + suffix.str() + ".trt" +
           std::to_string(NV_TENSORRT_MAJOR) + std::to_string(NV_TENSORRT_MINOR) +
           (request.inference.fp16 ? ".fp16.engine" : ".fp32.engine");
}

std::vector<char> readFile(const filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return {};
    const std::streamsize size = input.tellg();
    if (size <= 0) return {};
    std::vector<char> bytes(static_cast<std::size_t>(size));
    input.seekg(0);
    if (!input.read(bytes.data(), size)) return {};
    return bytes;
}

void writeFile(const filesystem::path& path, const void* data, std::size_t size) {
    std::error_code error;
    filesystem::create_directories(path.parent_path(), error);
    if (error) {
        throw std::runtime_error("cannot create TensorRT cache directory: " + error.message());
    }
    const filesystem::path temporary = path.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output || !output.write(static_cast<const char*>(data),
                                     static_cast<std::streamsize>(size))) {
            throw std::runtime_error("cannot write TensorRT engine cache " + temporary.string());
        }
    }
    filesystem::rename(temporary, path, error);
    if (error) {
        filesystem::remove(temporary);
        throw std::runtime_error("cannot publish TensorRT engine cache: " + error.message());
    }
}

class TensorRTSession final : public IInferenceSession {
public:
    explicit TensorRTSession(const SessionRequest& request) {
        try {
            checkCuda(cudaSetDevice(request.inference.device_id), "cudaSetDevice");
            runtime_.reset(nvinfer1::createInferRuntime(logger_));
            if (!runtime_) {
                throw std::runtime_error("TensorRT could not create an inference runtime");
            }

            const filesystem::path cache =
                filesystem::path(request.inference.engine_cache_dir) / cacheName(request);
            const std::vector<char> cached = readFile(cache);
            if (!cached.empty()) {
                engine_.reset(runtime_->deserializeCudaEngine(cached.data(), cached.size()));
            }
            if (!engine_) {
                // A first build takes minutes on the Nano; say so before the log goes quiet.
                logEvent(LogLevel::kInfo, "tensorrt_engine_build",
                         LogFields().add("tag", request.tag).add("cache", cache.string()));
                build(request, cache);
            }
            context_.reset(engine_->createExecutionContext());
            if (!context_) {
                throw std::runtime_error("TensorRT could not create an execution context");
            }
            configureBindings();
            checkCuda(cudaStreamCreateWithFlags(&stream_, cudaStreamNonBlocking),
                      "cudaStreamCreate");
        } catch (...) {
            releaseCuda();
            throw;
        }
    }

    ~TensorRTSession() override { releaseCuda(); }

private:
    void releaseCuda() noexcept {
        if (stream_ != nullptr) cudaStreamDestroy(stream_);
        stream_ = nullptr;
        for (void* buffer : device_buffers_) {
            if (buffer != nullptr) cudaFree(buffer);
        }
        device_buffers_.clear();
    }

public:

    [[nodiscard]] const std::vector<TensorSpec>& inputs() const override { return inputs_; }
    [[nodiscard]] const std::vector<TensorSpec>& outputs() const override { return outputs_; }

    [[nodiscard]] void* inputBuffer(std::size_t index) override {
        return input_buffers_.at(index).data();
    }

    bool run(std::string& error) override {
        try {
            for (std::size_t index = 0; index < input_bindings_.size(); ++index) {
                const int binding = input_bindings_[index];
                checkCuda(cudaMemcpyAsync(device_buffers_[static_cast<std::size_t>(binding)],
                                          input_buffers_[index].data(),
                                          input_buffers_[index].size() * sizeof(float),
                                          cudaMemcpyHostToDevice, stream_),
                          "TensorRT input upload");
            }
            if (!context_->enqueueV2(device_buffers_.data(), stream_, nullptr)) {
                throw std::runtime_error("TensorRT enqueueV2 returned false");
            }
            for (std::size_t index = 0; index < output_bindings_.size(); ++index) {
                const int binding = output_bindings_[index];
                checkCuda(cudaMemcpyAsync(output_buffers_[index].data(),
                                          device_buffers_[static_cast<std::size_t>(binding)],
                                          output_buffers_[index].size() * sizeof(float),
                                          cudaMemcpyDeviceToHost, stream_),
                          "TensorRT output download");
            }
            checkCuda(cudaStreamSynchronize(stream_), "TensorRT stream synchronize");
            return true;
        } catch (const std::exception& failure) {
            error = failure.what();
            return false;
        }
    }

    [[nodiscard]] const float* outputData(std::size_t index) const override {
        return output_buffers_.at(index).data();
    }

    [[nodiscard]] const std::vector<std::int64_t>& outputShape(std::size_t index) const override {
        return outputs_.at(index).shape;
    }

    [[nodiscard]] std::string backendName() const override { return "tensorrt"; }

private:
    TrtLogger logger_;
    TrtPtr<nvinfer1::IRuntime> runtime_;
    TrtPtr<nvinfer1::ICudaEngine> engine_;
    TrtPtr<nvinfer1::IExecutionContext> context_;
    cudaStream_t stream_{nullptr};
    std::vector<TensorSpec> inputs_;
    std::vector<TensorSpec> outputs_;
    std::vector<int> input_bindings_;
    std::vector<int> output_bindings_;
    std::vector<std::vector<float>> input_buffers_;
    std::vector<std::vector<float>> output_buffers_;
    std::vector<void*> device_buffers_;

    void build(const SessionRequest& request, const filesystem::path& cache) {
        TrtPtr<nvinfer1::IBuilder> builder(nvinfer1::createInferBuilder(logger_));
        if (!builder) throw std::runtime_error("TensorRT could not create a builder");
        const std::uint32_t explicit_batch =
            1U << static_cast<std::uint32_t>(
                      nvinfer1::NetworkDefinitionCreationFlag::kEXPLICIT_BATCH);
        TrtPtr<nvinfer1::INetworkDefinition> network(builder->createNetworkV2(explicit_batch));
        TrtPtr<nvonnxparser::IParser> parser(nvonnxparser::createParser(*network, logger_));
        TrtPtr<nvinfer1::IBuilderConfig> config(builder->createBuilderConfig());
        if (!network || !parser || !config) {
            throw std::runtime_error("TensorRT could not create its ONNX build objects");
        }
        std::vector<char> onnx = readFile(request.model_path);
        if (onnx.empty()) throw std::runtime_error("TensorRT ONNX model is empty");
        // The bundled detector uses only opset-12 operators understood by TensorRT 8.2, but a
        // newer exporter stamped ModelProto.ir_version=10. TensorRT 8.2's parser rejects that
        // metadata version before inspecting the graph. IR is protobuf field 1; this one-byte
        // normalization is applied to the in-memory copy only and leaves the mounted model intact.
        if (onnx.size() >= 2 && static_cast<unsigned char>(onnx[0]) == 0x08U &&
            static_cast<unsigned char>(onnx[1]) > 8U &&
            static_cast<unsigned char>(onnx[1]) < 0x80U) {
            onnx[1] = 8;
        }
        if (!parser->parse(onnx.data(), onnx.size())) {
            throw std::runtime_error("TensorRT ONNX parse failed: " + logger_.messages());
        }
        config->setMaxWorkspaceSize(256ULL * 1024ULL * 1024ULL);
        if (request.inference.fp16) {
            if (!builder->platformHasFastFp16()) {
                throw std::runtime_error("TensorRT FP16 requested but the GPU reports no fast FP16");
            }
            config->setFlag(nvinfer1::BuilderFlag::kFP16);
        }

        bool needs_profile = false;
        // TensorRT 8.2 documents that IBuilder retains ownership of this profile.
        nvinfer1::IOptimizationProfile* profile = builder->createOptimizationProfile();
        if (profile == nullptr) {
            throw std::runtime_error("TensorRT could not create an optimization profile");
        }
        for (int index = 0; index < network->getNbInputs(); ++index) {
            nvinfer1::ITensor* input = network->getInput(index);
            nvinfer1::Dims dimensions = input->getDimensions();
            bool dynamic = false;
            for (int dim = 0; dim < dimensions.nbDims; ++dim) {
                if (dimensions.d[dim] <= 0) {
                    dimensions.d[dim] = 1;
                    dynamic = true;
                }
            }
            if (dynamic) {
                needs_profile = true;
                if (!profile->setDimensions(input->getName(), nvinfer1::OptProfileSelector::kMIN,
                                            dimensions) ||
                    !profile->setDimensions(input->getName(), nvinfer1::OptProfileSelector::kOPT,
                                            dimensions) ||
                    !profile->setDimensions(input->getName(), nvinfer1::OptProfileSelector::kMAX,
                                            dimensions)) {
                    throw std::runtime_error("TensorRT rejected the batch-1 optimization profile");
                }
            }
        }
        if (needs_profile && config->addOptimizationProfile(profile) < 0) {
            throw std::runtime_error("TensorRT could not attach the optimization profile");
        }
        TrtPtr<nvinfer1::ICudaEngine> built(builder->buildEngineWithConfig(*network, *config));
        if (!built) {
            throw std::runtime_error("TensorRT engine build failed: " + logger_.messages());
        }
        TrtPtr<nvinfer1::IHostMemory> serialized(built->serialize());
        if (!serialized) throw std::runtime_error("TensorRT could not serialize the engine");
        writeFile(cache, serialized->data(), serialized->size());
        engine_ = std::move(built);
    }

    void configureBindings() {
        if (engine_->hasImplicitBatchDimension()) {
            throw std::runtime_error("implicit-batch TensorRT engines are not supported");
        }
        device_buffers_.assign(static_cast<std::size_t>(engine_->getNbBindings()), nullptr);

        for (int binding = 0; binding < engine_->getNbBindings(); ++binding) {
            if (engine_->isShapeBinding(binding)) {
                throw std::runtime_error("TensorRT shape bindings are not supported by this model");
            }
            if (engine_->getBindingDataType(binding) != nvinfer1::DataType::kFLOAT) {
                throw std::runtime_error(std::string("TensorRT binding is not float32: ") +
                                         engine_->getBindingName(binding));
            }
            nvinfer1::Dims dimensions = engine_->getBindingDimensions(binding);
            if (engine_->bindingIsInput(binding)) {
                bool dynamic = false;
                for (int dim = 0; dim < dimensions.nbDims; ++dim) {
                    if (dimensions.d[dim] <= 0) {
                        dimensions.d[dim] = 1;
                        dynamic = true;
                    }
                }
                if (dynamic && !context_->setBindingDimensions(binding, dimensions)) {
                    throw std::runtime_error("TensorRT could not set the batch-1 input shape");
                }
            }
        }
        if (!context_->allInputDimensionsSpecified()) {
            throw std::runtime_error("TensorRT input dimensions are incomplete");
        }

        for (int binding = 0; binding < engine_->getNbBindings(); ++binding) {
            const nvinfer1::Dims dimensions = context_->getBindingDimensions(binding);
            TensorSpec spec;
            spec.name = engine_->getBindingName(binding);
            spec.type = TensorType::kFloat32;
            spec.shape = shapeOf(dimensions);
            const std::size_t elements = volume(dimensions);
            void* device = nullptr;
            checkCuda(cudaMalloc(&device, elements * sizeof(float)), "TensorRT cudaMalloc");
            device_buffers_[static_cast<std::size_t>(binding)] = device;
            if (engine_->bindingIsInput(binding)) {
                input_bindings_.push_back(binding);
                inputs_.push_back(spec);
                input_buffers_.emplace_back(elements, 0.0F);
            } else {
                output_bindings_.push_back(binding);
                outputs_.push_back(spec);
                output_buffers_.emplace_back(elements, 0.0F);
            }
        }
    }
};

}  // namespace

std::unique_ptr<IInferenceSession> createTensorRTSession(const SessionRequest& request,
                                                         std::string& error) {
    try {
        return std::make_unique<TensorRTSession>(request);
    } catch (const std::exception& failure) {
        error = failure.what();
        return nullptr;
    }
}

bool tensorRTAvailable() {
    return true;
}

}  // namespace anpr
