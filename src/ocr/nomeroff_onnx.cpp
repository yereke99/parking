#include "anpr/ocr/nomeroff_onnx.hpp"

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <utility>

#include <opencv2/imgproc.hpp>

#include "anpr/common/logging.hpp"
#include "anpr/ocr/nomeroff_decoding.hpp"

namespace anpr {

NomeroffOnnx::NomeroffOnnx(OcrConfig config, std::unique_ptr<IInferenceSession> session,
                           PipelineMetrics* metrics)
    : config_(std::move(config)), session_(std::move(session)), metrics_(metrics) {
    if (session_->inputs().size() != 1 || session_->outputs().size() != 1) {
        throw std::runtime_error("Nomeroff OCR model must have one input and one output");
    }
    const TensorSpec& input = session_->inputs().front();
    const std::vector<std::int64_t> expected{1, 3, kInputHeight, kInputWidth};
    if (input.type != TensorType::kFloat32 || input.shape != expected) {
        throw std::runtime_error("Nomeroff OCR model must take float32 1x3x50x200");
    }
    // [1, steps, classes] from the exporter; Nomeroff's own [steps, 1, classes] is the same memory.
    const std::vector<std::int64_t>& output = session_->outputs().front().shape;
    const std::int64_t classes =
        static_cast<std::int64_t>(std::string(nomeroff::kKzLetters).size()) + 1;
    if (output.size() != 3 || output[2] != classes || (output[0] != 1 && output[1] != 1)) {
        throw std::runtime_error("Nomeroff OCR model output must be 1 x steps x " +
                                 std::to_string(classes) + " logits");
    }
    classes_ = static_cast<int>(classes);
    steps_ = static_cast<int>(output[0] == 1 ? output[1] : output[0]);

    float* tensor = static_cast<float*>(session_->inputBuffer(0));
    for (int channel = 0; channel < 3; ++channel) {
        const std::size_t offset =
            static_cast<std::size_t>(channel) * kInputHeight * kInputWidth;
        planes_.emplace_back(kInputHeight, kInputWidth, CV_32FC1, tensor + offset);
    }
}

std::string NomeroffOnnx::backendName() const {
    return "nomeroff_" + session_->backendName();
}

std::string NomeroffOnnx::modelDescription() const {
    return "nomeroff 4.0.1 kz onnx 200x50, " + std::to_string(steps_) + " steps";
}

OcrResult NomeroffOnnx::recognize(const cv::Mat& plate) {
    OcrResult result;
    const auto started = std::chrono::steady_clock::now();
    const auto finish = [&]() {
        metrics_->ocr_total.add(std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - started)
                                    .count());
        ++metrics_->ocr_calls;
        if (!result.ok()) ++metrics_->ocr_empty;
        return result;
    };
    if (plate.empty() || plate.rows < 2 || plate.cols < 2 || plate.depth() != CV_8U ||
        (plate.channels() != 1 && plate.channels() != 3)) {
        result.rejection = OcrRejection::kEmptyCrop;
        return finish();
    }

    {
        ScopedTimer timer(metrics_->ocr_preprocess);
        // Nomeroff Net's pipeline passes BGR crops, and its TextDetector swaps the channels
        // (convert_cv_zones_rgb_to_bgr) before normalize_img: the network sees RGB.
        cv::cvtColor(plate, rgb_,
                     plate.channels() == 3 ? cv::COLOR_BGR2RGB : cv::COLOR_GRAY2RGB);
        cv::resize(rgb_, resized_, cv::Size(kInputWidth, kInputHeight), 0.0, 0.0,
                   cv::INTER_LINEAR);
        // normalize_img: cv2.normalize(..., 0, 1, NORM_MINMAX, CV_32F) over all three channels.
        cv::normalize(resized_, scaled_, 0.0, 1.0, cv::NORM_MINMAX, CV_32F);
        // HWC to the CHW input tensor; the planes are fixed headers over it.
        cv::split(scaled_, planes_);
    }

    std::string error;
    {
        ScopedTimer timer(metrics_->ocr_inference);
        if (!session_->run(error)) {
            logEvent(LogLevel::kWarn, "ocr_inference_failed", LogFields().add("reason", error));
            result.rejection = OcrRejection::kInferenceFailed;
            return finish();
        }
    }

    nomeroff::CtcReading reading =
        nomeroff::decodeGreedy(session_->outputData(0), steps_, classes_, nomeroff::kKzLetters);
    result.text = std::move(reading.text);
    result.confidence = reading.confidence;
    result.min_char_confidence = reading.min_char_confidence;
    result.character_confidences = std::move(reading.character_confidences);
    result.region = "kz";
    // Three gates: an empty reading, a weak character, a low mean.
    if (result.text.empty()) {
        result.rejection = OcrRejection::kNoText;
    } else if (result.min_char_confidence < config_.min_char_confidence) {
        result.rejection = OcrRejection::kWeakCharacter;
    } else if (result.confidence < config_.min_confidence) {
        result.rejection = OcrRejection::kLowConfidence;
    }
    return finish();
}

std::unique_ptr<IPlateOcr> makeNomeroffOnnx(const OcrConfig& ocr, const InferenceConfig& inference,
                                            PipelineMetrics* metrics, std::string& error) {
    SessionRequest request;
    request.model_path = ocr.model;
    request.inference = inference;
    // `strict_backend` guards the detector. Like the other OCR models this one may fall back to
    // ONNX Runtime; the model_loaded line and the backend name say where it runs.
    request.inference.strict_backend = false;
    // FP32: about 0.5 GFLOP per crop, so FP16 would save little and could change a marginal
    // character against the PyTorch model.
    request.inference.fp16 = false;
    request.tag = "ocr";
    request.input_size_hint = NomeroffOnnx::kInputWidth;
    std::unique_ptr<IInferenceSession> session = createInferenceSession(request, error);
    if (session == nullptr) return nullptr;
    try {
        return std::make_unique<NomeroffOnnx>(ocr, std::move(session), metrics);
    } catch (const std::exception& failure) {
        error = std::string("UNSUPPORTED_MODEL: ocr: ") + failure.what();
        return nullptr;
    }
}

}  // namespace anpr
