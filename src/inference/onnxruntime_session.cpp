#include "anpr/inference/onnxruntime_session.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <stdexcept>
#include <unordered_map>

#include <onnxruntime_cxx_api.h>

#include "anpr/common/logging.hpp"

namespace anpr {
namespace {

/// One process-wide environment. Creating an Ort::Env per session leaks thread pools and log
/// sinks, which matters for a process that runs for weeks.
Ort::Env& ortEnv() {
    static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "kz_anpr");
    return env;
}

TensorType toTensorType(ONNXTensorElementDataType type, bool& supported) {
    supported = true;
    switch (type) {
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT:
            return TensorType::kFloat32;
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8:
            return TensorType::kUInt8;
        default:
            supported = false;
            return TensorType::kFloat32;
    }
}

/// Replaces dynamic dimensions with 1. Every model here runs batch 1, one plate at a time.
std::vector<std::int64_t> resolveShape(const std::vector<std::int64_t>& shape) {
    std::vector<std::int64_t> resolved = shape;
    for (std::int64_t& dim : resolved) {
        if (dim <= 0) {
            dim = 1;
        }
    }
    return resolved;
}

const char* providerName(InferenceBackend backend) {
    switch (backend) {
        case InferenceBackend::kTensorRT:
            return "TensorrtExecutionProvider";
        case InferenceBackend::kOnnxCuda:
            return "CUDAExecutionProvider";
        default:
            return "CPUExecutionProvider";
    }
}

class OnnxRuntimeSession final : public IInferenceSession {
public:
    OnnxRuntimeSession(const SessionRequest& request, InferenceBackend backend)
        : backend_(backend) {
        Ort::SessionOptions options;
        options.SetIntraOpNumThreads(std::max(1, request.inference.intra_op_threads));
        options.SetInterOpNumThreads(std::max(1, request.inference.inter_op_threads));
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        options.DisableCpuMemArena();

        appendProvider(options, request.inference, backend);

        session_ = std::make_unique<Ort::Session>(ortEnv(), request.model_path.c_str(), options);
        describeTensors();
        bindBuffers();
    }

    [[nodiscard]] const std::vector<TensorSpec>& inputs() const override { return inputs_; }
    [[nodiscard]] const std::vector<TensorSpec>& outputs() const override { return outputs_; }

    [[nodiscard]] void* inputBuffer(std::size_t index) override {
        return input_buffers_.at(index).data();
    }

    bool run(std::string& error) override {
        try {
            session_->Run(Ort::RunOptions{nullptr}, input_names_.data(), input_values_.data(),
                          input_values_.size(), output_names_.data(), output_values_.data(),
                          output_values_.size());
            return true;
        } catch (const Ort::Exception& failure) {
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

    [[nodiscard]] std::string backendName() const override { return toString(backend_); }

private:
    InferenceBackend backend_;
    std::unique_ptr<Ort::Session> session_;
    std::vector<TensorSpec> inputs_;
    std::vector<TensorSpec> outputs_;
    std::vector<std::string> input_name_storage_;
    std::vector<std::string> output_name_storage_;
    std::vector<const char*> input_names_;
    std::vector<const char*> output_names_;
    // Host buffers live for the session's lifetime; the Ort::Values below are views onto them,
    // so a frame costs one memcpy-free write plus the run itself.
    std::vector<std::vector<std::uint8_t>> input_buffers_;
    std::vector<std::vector<float>> output_buffers_;
    std::vector<Ort::Value> input_values_;
    std::vector<Ort::Value> output_values_;

    static void appendProvider(Ort::SessionOptions& options, const InferenceConfig& config,
                               InferenceBackend backend) {
        if (backend == InferenceBackend::kTensorRT) {
            OrtTensorRTProviderOptionsV2* trt = nullptr;
            Ort::ThrowOnError(Ort::GetApi().CreateTensorRTProviderOptions(&trt));
            const std::unique_ptr<OrtTensorRTProviderOptionsV2, void (*)(OrtTensorRTProviderOptionsV2*)>
                guard(trt, Ort::GetApi().ReleaseTensorRTProviderOptions);

            std::error_code ignored;
            std::filesystem::create_directories(config.engine_cache_dir, ignored);

            const std::string device_id = std::to_string(config.device_id);
            const std::string fp16 = config.fp16 ? "1" : "0";
            const std::array<const char*, 6> keys{"device_id",
                                                  "trt_fp16_enable",
                                                  "trt_engine_cache_enable",
                                                  "trt_engine_cache_path",
                                                  "trt_timing_cache_enable",
                                                  "trt_max_workspace_size"};
            const std::array<const char*, 6> values{device_id.c_str(),
                                                    fp16.c_str(),
                                                    "1",
                                                    config.engine_cache_dir.c_str(),
                                                    "1",
                                                    // 1 GiB. Orin Nano Super has 8 GB shared
                                                    // between CPU and GPU, so the builder must
                                                    // not be allowed to take all of it.
                                                    "1073741824"};
            Ort::ThrowOnError(Ort::GetApi().UpdateTensorRTProviderOptions(trt, keys.data(),
                                                                         values.data(), keys.size()));
            options.AppendExecutionProvider_TensorRT_V2(*trt);
            // TensorRT falls back to CUDA for any subgraph it cannot build, and CUDA falls back
            // to CPU, so the chain is registered in priority order.
            appendCuda(options, config);
            return;
        }
        if (backend == InferenceBackend::kOnnxCuda) {
            appendCuda(options, config);
        }
    }

    static void appendCuda(Ort::SessionOptions& options, const InferenceConfig& config) {
        OrtCUDAProviderOptionsV2* cuda = nullptr;
        Ort::ThrowOnError(Ort::GetApi().CreateCUDAProviderOptions(&cuda));
        const std::unique_ptr<OrtCUDAProviderOptionsV2, void (*)(OrtCUDAProviderOptionsV2*)> guard(
            cuda, Ort::GetApi().ReleaseCUDAProviderOptions);
        const std::string device_id = std::to_string(config.device_id);
        const std::array<const char*, 2> keys{"device_id", "arena_extend_strategy"};
        const std::array<const char*, 2> values{device_id.c_str(), "kSameAsRequested"};
        Ort::ThrowOnError(
            Ort::GetApi().UpdateCUDAProviderOptions(cuda, keys.data(), values.data(), keys.size()));
#if ORT_API_VERSION >= 12
        options.AppendExecutionProvider_CUDA_V2(*cuda);
#else
        // ORT 1.11 exposes the V2 CUDA provider in the C API but not yet in its C++ wrapper.
        Ort::ThrowOnError(
            Ort::GetApi().SessionOptionsAppendExecutionProvider_CUDA_V2(options, cuda));
#endif
    }

    void describeTensors() {
        Ort::AllocatorWithDefaultOptions allocator;

        const std::size_t input_count = session_->GetInputCount();
        inputs_.reserve(input_count);
        input_name_storage_.reserve(input_count);
        for (std::size_t i = 0; i < input_count; ++i) {
#if ORT_API_VERSION >= 12
            const Ort::AllocatedStringPtr name = session_->GetInputNameAllocated(i, allocator);
            input_name_storage_.emplace_back(name.get());
#else
            char* name = session_->GetInputName(i, allocator);
            input_name_storage_.emplace_back(name);
            allocator.Free(name);
#endif

            const Ort::TypeInfo info = session_->GetInputTypeInfo(i);
            const auto tensor_info = info.GetTensorTypeAndShapeInfo();
            bool supported = false;
            TensorSpec spec;
            spec.name = input_name_storage_.back();
            spec.type = toTensorType(tensor_info.GetElementType(), supported);
            spec.shape = resolveShape(tensor_info.GetShape());
            if (!supported) {
                throw std::runtime_error("input '" + spec.name +
                                         "' uses a tensor element type this runtime does not "
                                         "handle; only float32 and uint8 are supported");
            }
            inputs_.push_back(std::move(spec));
        }

        const std::size_t output_count = session_->GetOutputCount();
        outputs_.reserve(output_count);
        output_name_storage_.reserve(output_count);
        for (std::size_t i = 0; i < output_count; ++i) {
#if ORT_API_VERSION >= 12
            const Ort::AllocatedStringPtr name = session_->GetOutputNameAllocated(i, allocator);
            output_name_storage_.emplace_back(name.get());
#else
            char* name = session_->GetOutputName(i, allocator);
            output_name_storage_.emplace_back(name);
            allocator.Free(name);
#endif

            const Ort::TypeInfo info = session_->GetOutputTypeInfo(i);
            const auto tensor_info = info.GetTensorTypeAndShapeInfo();
            bool supported = false;
            TensorSpec spec;
            spec.name = output_name_storage_.back();
            spec.type = toTensorType(tensor_info.GetElementType(), supported);
            spec.shape = resolveShape(tensor_info.GetShape());
            if (!supported || spec.type != TensorType::kFloat32) {
                throw std::runtime_error("output '" + spec.name +
                                         "' is not float32; the pipeline only reads float outputs");
            }
            outputs_.push_back(std::move(spec));
        }

        input_names_.reserve(input_name_storage_.size());
        for (const std::string& name : input_name_storage_) {
            input_names_.push_back(name.c_str());
        }
        output_names_.reserve(output_name_storage_.size());
        for (const std::string& name : output_name_storage_) {
            output_names_.push_back(name.c_str());
        }
    }

    void bindBuffers() {
        const Ort::MemoryInfo memory_info =
            Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeCPU);

        input_buffers_.reserve(inputs_.size());
        input_values_.reserve(inputs_.size());
        for (const TensorSpec& spec : inputs_) {
            input_buffers_.emplace_back(spec.byteSize(), 0);
            std::vector<std::uint8_t>& buffer = input_buffers_.back();
            if (spec.type == TensorType::kUInt8) {
                input_values_.push_back(Ort::Value::CreateTensor<std::uint8_t>(
                    memory_info, buffer.data(), spec.elementCount(), spec.shape.data(),
                    spec.shape.size()));
            } else {
                input_values_.push_back(Ort::Value::CreateTensor<float>(
                    memory_info, reinterpret_cast<float*>(buffer.data()), spec.elementCount(),
                    spec.shape.data(), spec.shape.size()));
            }
        }

        output_buffers_.reserve(outputs_.size());
        output_values_.reserve(outputs_.size());
        for (const TensorSpec& spec : outputs_) {
            output_buffers_.emplace_back(spec.elementCount(), 0.0F);
            std::vector<float>& buffer = output_buffers_.back();
            output_values_.push_back(Ort::Value::CreateTensor<float>(
                memory_info, buffer.data(), buffer.size(), spec.shape.data(), spec.shape.size()));
        }
    }
};

}  // namespace

std::unique_ptr<IInferenceSession> createOnnxRuntimeSession(const SessionRequest& request,
                                                            InferenceBackend backend,
                                                            std::string& error) {
    const std::vector<std::string> providers = Ort::GetAvailableProviders();
    const char* required = providerName(backend);
    if (std::find(providers.begin(), providers.end(), required) == providers.end()) {
        error = std::string("provider ") + required + " is not present in this ONNX Runtime build";
        return nullptr;
    }
    try {
        return std::make_unique<OnnxRuntimeSession>(request, backend);
    } catch (const std::exception& failure) {
        error = failure.what();
        return nullptr;
    }
}

bool onnxRuntimeAvailable() {
    return true;
}

std::vector<std::string> availableProviders() {
    return Ort::GetAvailableProviders();
}

}  // namespace anpr
