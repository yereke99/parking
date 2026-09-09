#include "anpr/inference/opencv_dnn_session.hpp"

#include <algorithm>
#include <filesystem>
#include <stdexcept>

#include <opencv2/core.hpp>
#include <opencv2/dnn.hpp>

namespace anpr {
namespace {

class OpenCvDnnSession final : public IInferenceSession {
public:
    OpenCvDnnSession(const SessionRequest& request, int input_size) {
        net_ = cv::dnn::readNetFromONNX(request.model_path);
        if (net_.empty()) {
            throw std::runtime_error("OpenCV DNN could not load " + request.model_path);
        }
        net_.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
        net_.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
        output_layer_names_ = net_.getUnconnectedOutLayersNames();

        // OpenCV DNN does not expose the ONNX input signature, so the caller states it. Every
        // model routed here is a square NCHW RGB detector.
        TensorSpec input;
        input.name = "images";
        input.type = TensorType::kFloat32;
        input.shape = {1, 3, input_size, input_size};
        inputs_.push_back(input);

        input_buffer_.assign(input.elementCount(), 0.0F);
        blob_ = cv::Mat(4, std::array<int, 4>{1, 3, input_size, input_size}.data(), CV_32F,
                        input_buffer_.data());

        // The output signature is only known after one forward pass, so probe it once at load
        // time rather than discovering it mid-stream.
        runForward();
        captureOutputs();
    }

    [[nodiscard]] const std::vector<TensorSpec>& inputs() const override { return inputs_; }
    [[nodiscard]] const std::vector<TensorSpec>& outputs() const override { return outputs_; }

    [[nodiscard]] void* inputBuffer(std::size_t index) override {
        if (index != 0) {
            return nullptr;
        }
        return input_buffer_.data();
    }

    bool run(std::string& error) override {
        try {
            runForward();
            copyOutputs(error);
            return error.empty();
        } catch (const cv::Exception& failure) {
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

    [[nodiscard]] std::string backendName() const override { return "opencv_dnn"; }

private:
    cv::dnn::Net net_;
    std::vector<std::string> output_layer_names_;
    std::vector<TensorSpec> inputs_;
    std::vector<TensorSpec> outputs_;
    std::vector<float> input_buffer_;
    std::vector<std::vector<float>> output_buffers_;
    std::vector<cv::Mat> raw_outputs_;
    cv::Mat blob_;

    void runForward() {
        net_.setInput(blob_);
        net_.forward(raw_outputs_, output_layer_names_);
    }

    void captureOutputs() {
        outputs_.clear();
        output_buffers_.clear();
        for (std::size_t i = 0; i < raw_outputs_.size(); ++i) {
            TensorSpec spec;
            spec.name = i < output_layer_names_.size() ? output_layer_names_[i] : "output";
            spec.type = TensorType::kFloat32;
            for (int dim = 0; dim < raw_outputs_[i].dims; ++dim) {
                spec.shape.push_back(raw_outputs_[i].size[dim]);
            }
            outputs_.push_back(std::move(spec));
            output_buffers_.emplace_back(outputs_.back().elementCount(), 0.0F);
        }
        std::string error;
        copyOutputs(error);
    }

    void copyOutputs(std::string& error) {
        for (std::size_t i = 0; i < output_buffers_.size(); ++i) {
            if (i >= raw_outputs_.size() || !raw_outputs_[i].isContinuous() ||
                raw_outputs_[i].total() != output_buffers_[i].size()) {
                error = "unexpected OpenCV DNN output shape";
                return;
            }
            std::copy_n(raw_outputs_[i].ptr<float>(), output_buffers_[i].size(),
                        output_buffers_[i].data());
        }
    }
};

}  // namespace

std::unique_ptr<IInferenceSession> createOpenCvDnnSession(const SessionRequest& request,
                                                          std::string& error) {
    if (request.requires_uint8_input) {
        error =
            "the OpenCV DNN backend cannot feed a uint8 tensor, which this model requires; build "
            "with ONNX Runtime";
        return nullptr;
    }
    if (!std::filesystem::exists(request.model_path)) {
        error = "model not found: " + request.model_path;
        return nullptr;
    }
    try {
        return std::make_unique<OpenCvDnnSession>(request, request.input_size_hint);
    } catch (const std::exception& failure) {
        error = failure.what();
        return nullptr;
    }
}

}  // namespace anpr
