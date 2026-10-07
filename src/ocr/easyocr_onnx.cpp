#include "anpr/ocr/easyocr_onnx.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <utility>

#include <opencv2/imgproc.hpp>

#include "anpr/common/logging.hpp"

namespace anpr {
namespace {

constexpr int kModelHeight = 64;
/// The allowlist the `easyocr` worker passes to Reader.recognize.
constexpr const char* kPlateAllowlist = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
/// Reader.recognize defaults: a reading scored below contrast_ths is retried on a
/// contrast-stretched crop with target adjust_contrast, and the better score wins.
constexpr double kContrastThreshold = 0.1;
constexpr double kAdjustContrast = 0.5;

}  // namespace

const std::vector<int>& EasyOcrOnnx::widths() {
    static const std::vector<int> values{64, 128, 192, 256, 320, 384};
    return values;
}

EasyOcrOnnx::EasyOcrOnnx(OcrConfig config, std::vector<WidthSession> sessions,
                         PipelineMetrics* metrics)
    : config_(std::move(config)),
      metrics_(metrics),
      allowed_(easyocr::englishG2Allowed(kPlateAllowlist)) {
    for (WidthSession& item : sessions) {
        IInferenceSession& session = *item.session;
        if (session.inputs().size() != 1 || session.outputs().size() != 1) {
            throw std::runtime_error("EasyOCR model must have one input and one output");
        }
        const TensorSpec& input = session.inputs().front();
        const std::vector<std::int64_t> expected{1, 1, kModelHeight, item.width};
        if (input.type != TensorType::kFloat32 || input.shape != expected) {
            throw std::runtime_error("EasyOCR model for width " + std::to_string(item.width) +
                                     " must take float32 1x1x64x" + std::to_string(item.width));
        }
        const std::vector<std::int64_t>& output = session.outputs().front().shape;
        if (output.size() != 3 || output[0] != 1 || output[2] != easyocr::kEnglishG2Classes) {
            throw std::runtime_error("EasyOCR model output must be 1 x steps x 97 logits");
        }
        Width width;
        width.width = item.width;
        width.steps = static_cast<int>(output[1]);
        width.input = cv::Mat(kModelHeight, item.width, CV_32FC1, session.inputBuffer(0));
        width.session = std::move(item.session);
        sessions_.push_back(std::move(width));
    }
    if (sessions_.empty()) throw std::runtime_error("no EasyOCR models were loaded");
    std::sort(sessions_.begin(), sessions_.end(),
              [](const Width& a, const Width& b) { return a.width < b.width; });
}

std::string EasyOcrOnnx::backendName() const {
    return "easyocr_" + sessions_.front().session->backendName();
}

std::string EasyOcrOnnx::modelDescription() const {
    return "easyocr 1.6.2 english_g2 onnx widths " + std::to_string(sessions_.front().width) +
           "-" + std::to_string(sessions_.back().width);
}

bool EasyOcrOnnx::read(const cv::Mat& resized, Width& width, easyocr::CtcReading& reading,
                       std::string& error) {
    // AlignCollate: keep the ratio at height 64, never wider than the model; for the usual wide
    // crop this is already the size of `resized`.
    const double ratio = static_cast<double>(resized.cols) / resized.rows;
    const int aligned_width =
        std::min(width.width, static_cast<int>(std::ceil(kModelHeight * ratio)));
    const cv::Mat* source = &resized;
    if (resized.rows != kModelHeight || resized.cols != aligned_width) {
        cv::resize(resized, aligned_, cv::Size(aligned_width, kModelHeight), 0.0, 0.0,
                   cv::INTER_CUBIC);
        source = &aligned_;
    }
    // NormalizePAD: (x / 255 - 0.5) / 0.5, then repeat the last column up to the model width.
    cv::Mat left = width.input(cv::Rect(0, 0, aligned_width, kModelHeight));
    source->convertTo(left, CV_32F, 1.0 / 127.5, -1.0);
    for (int column = aligned_width; column < width.width; ++column) {
        left.col(aligned_width - 1).copyTo(width.input.col(column));
    }
    {
        ScopedTimer timer(metrics_->ocr_inference);
        if (!width.session->run(error)) return false;
    }
    reading = easyocr::decodeGreedy(width.session->outputData(0), width.steps,
                                    easyocr::kEnglishG2Classes, allowed_);
    return true;
}

OcrResult EasyOcrOnnx::recognize(const cv::Mat& plate) {
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

    Width* width = nullptr;
    {
        ScopedTimer timer(metrics_->ocr_preprocess);
        if (plate.channels() == 3) {
            cv::cvtColor(plate, grey_, cv::COLOR_BGR2GRAY);
        } else {
            grey_ = plate;
        }
        // get_image_list: height 64 for a wide crop, width 64 for a tall one, with Python's
        // int() truncation, and a model width of ceil(aspect) * 64.
        const int w = grey_.cols;
        const int h = grey_.rows;
        const double aspect = w >= h ? static_cast<double>(w) / h : static_cast<double>(h) / w;
        const int scaled = static_cast<int>(kModelHeight * aspect);
        if (scaled == 0) {
            result.rejection = OcrRejection::kEmptyCrop;
            return finish();
        }
        const cv::Size size = w >= h ? cv::Size(scaled, kModelHeight) : cv::Size(kModelHeight, scaled);
        cv::resize(grey_, resized_, size, 0.0, 0.0, cv::INTER_LINEAR);
        const int wanted = static_cast<int>(std::ceil(aspect)) * kModelHeight;
        width = &sessions_.back();
        for (Width& candidate : sessions_) {
            if (candidate.width >= wanted) {
                width = &candidate;
                break;
            }
        }
    }

    std::string error;
    easyocr::CtcReading reading;
    if (!read(resized_, *width, reading, error)) {
        logEvent(LogLevel::kWarn, "ocr_inference_failed", LogFields().add("reason", error));
        result.rejection = OcrRejection::kInferenceFailed;
        return finish();
    }
    if (reading.confidence < kContrastThreshold) {
        resized_.copyTo(contrast_);
        easyocr::adjustContrastGrey(contrast_.ptr(), contrast_.total(), kAdjustContrast);
        easyocr::CtcReading retry;
        if (read(contrast_, *width, retry, error) && !(reading.confidence > retry.confidence)) {
            reading = std::move(retry);
        }
    }

    result.text = reading.text;
    result.confidence = static_cast<float>(reading.confidence);
    // EasyOCR scores the whole string; the worker reports that score for both fields too.
    result.min_char_confidence = result.confidence;
    if (result.text.empty()) {
        result.rejection = OcrRejection::kAllPadding;
    } else if (result.min_char_confidence < config_.min_char_confidence) {
        result.rejection = OcrRejection::kWeakCharacter;
    } else if (result.confidence < config_.min_confidence) {
        result.rejection = OcrRejection::kLowConfidence;
    }
    return finish();
}

std::unique_ptr<IPlateOcr> makeEasyOcrOnnx(const OcrConfig& ocr, const InferenceConfig& inference,
                                           PipelineMetrics* metrics, std::string& error) {
    std::vector<EasyOcrOnnx::WidthSession> sessions;
    for (const int width : EasyOcrOnnx::widths()) {
        SessionRequest request;
        request.model_path =
            ocr.easyocr_onnx_dir + "/english_g2_" + std::to_string(width) + ".onnx";
        request.inference = inference;
        // `strict_backend` guards the detector. Like the uint8 Fast Plate OCR model, this OCR may
        // fall back to ONNX Runtime; the model_loaded line and backend name say where it runs.
        request.inference.strict_backend = false;
        request.tag = "ocr_w" + std::to_string(width);
        request.input_size_hint = width;
        std::unique_ptr<IInferenceSession> session = createInferenceSession(request, error);
        if (session == nullptr) return nullptr;
        sessions.push_back({width, std::move(session)});
    }
    try {
        return std::make_unique<EasyOcrOnnx>(ocr, std::move(sessions), metrics);
    } catch (const std::exception& failure) {
        error = std::string("UNSUPPORTED_MODEL: ocr: ") + failure.what();
        return nullptr;
    }
}

}  // namespace anpr
