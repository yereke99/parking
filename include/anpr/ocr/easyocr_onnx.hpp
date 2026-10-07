#pragma once

#include <memory>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include "anpr/common/config.hpp"
#include "anpr/common/metrics.hpp"
#include "anpr/inference/inference_session.hpp"
#include "anpr/ocr/easyocr_decoding.hpp"
#include "anpr/ocr/plate_ocr.hpp"

namespace anpr {

/// EasyOCR 1.6.2's english_g2 recognizer, run in-process by the detector's inference backend.
///
/// tools/export_easyocr_onnx.py exports the recognizer once per input width EasyOCR can use
/// (64 to 384 px, one per whole aspect ratio), because EasyOCR pads each crop to
/// ceil(width / height) * 64 and a fixed width changes its readings. A crop runs on the session
/// of its own width, so preprocessing, the 0-9A-Z allowlist, the low-confidence contrast retry and
/// the score reproduce `Reader.recognize` for one crop. Unlike the `easyocr` worker this needs no
/// PyTorch process, which on the Jetson Nano saves about 2 GB of shared CPU/GPU memory.
class EasyOcrOnnx final : public IPlateOcr {
public:
    struct WidthSession {
        int width{0};
        std::unique_ptr<IInferenceSession> session;
    };

    EasyOcrOnnx(OcrConfig config, std::vector<WidthSession> sessions, PipelineMetrics* metrics);

    OcrResult recognize(const cv::Mat& plate) override;
    [[nodiscard]] std::string backendName() const override;
    [[nodiscard]] std::string modelDescription() const override;

    /// Widths the exporter writes and this backend loads.
    static const std::vector<int>& widths();

private:
    struct Width {
        int width{0};
        int steps{0};
        std::unique_ptr<IInferenceSession> session;
        cv::Mat input;  ///< 64 x width float header over the session input tensor
    };

    OcrConfig config_;
    std::vector<Width> sessions_;
    PipelineMetrics* metrics_;
    easyocr::AllowedClasses allowed_;
    cv::Mat grey_;
    cv::Mat resized_;
    cv::Mat contrast_;
    cv::Mat aligned_;

    bool read(const cv::Mat& resized, Width& width, easyocr::CtcReading& reading,
              std::string& error);
};

/// Builds the backend from `ocr.easyocr_onnx_dir`, or returns nullptr with `error` set.
std::unique_ptr<IPlateOcr> makeEasyOcrOnnx(const OcrConfig& ocr, const InferenceConfig& inference,
                                           PipelineMetrics* metrics, std::string& error);

}  // namespace anpr
