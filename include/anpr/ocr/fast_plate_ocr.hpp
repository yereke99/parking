#pragma once

#include <memory>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include "anpr/common/config.hpp"
#include "anpr/common/metrics.hpp"
#include "anpr/inference/inference_session.hpp"
#include "anpr/ocr/plate_ocr.hpp"

namespace anpr {

/// The model contract shipped alongside a Fast Plate OCR ONNX file.
///
/// Every field is read from the model's own YAML rather than assumed, so swapping the global
/// model for a regional or a retrained one is a configuration change. The values below are only
/// the struct defaults; a real model always overrides them.
struct FastPlateOcrModelConfig {
    int max_plate_slots{10};
    std::string alphabet{"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ_"};
    char pad_char{'_'};
    int img_height{64};
    int img_width{128};
    bool keep_aspect_ratio{false};
    /// OpenCV interpolation flag, translated from the config's method name.
    int interpolation{1};  // cv::INTER_LINEAR
    bool grayscale{false};
    cv::Scalar padding_color{114, 114, 114};
    std::vector<std::string> plate_regions;

    [[nodiscard]] int channels() const { return grayscale ? 1 : 3; }
    [[nodiscard]] int padIndex() const;
};

struct FastPlateOcrConfigLoad {
    FastPlateOcrModelConfig config;
    bool ok{true};
    std::string error;
};

/// Reads the YAML that Fast Plate OCR distributes with each model.
FastPlateOcrConfigLoad loadFastPlateOcrConfig(const std::string& path);

/// Native Fast Plate OCR inference.
///
/// The model takes a uint8 NHWC tensor and normalises pixel values internally, so preprocessing
/// is a colour conversion plus a resize written straight into the session's input tensor. There
/// is no Python, no subprocess and no per-call allocation.
///
/// Decoding mirrors the reference implementation: reshape to (slots, vocabulary), take the
/// argmax per slot, map through the alphabet, then drop trailing padding. It deviates in one
/// deliberate way, documented on `OcrResult::confidence`: confidence averages only the returned
/// characters, where the reference averages all slots including padding.
class FastPlateOcr final : public IPlateOcr {
public:
    FastPlateOcr(FastPlateOcrModelConfig model, OcrConfig config,
                 std::unique_ptr<IInferenceSession> session, PipelineMetrics* metrics);

    OcrResult recognize(const cv::Mat& plate) override;
    [[nodiscard]] std::string backendName() const override { return session_->backendName(); }
    [[nodiscard]] std::string modelDescription() const override;

    [[nodiscard]] const FastPlateOcrModelConfig& modelConfig() const { return model_; }

    /// Decodes a raw plate-head tensor. Exposed so decoding is unit tested without a model.
    static OcrResult decode(const FastPlateOcrModelConfig& model, const float* plate_head,
                            float min_char_confidence);

private:
    FastPlateOcrModelConfig model_;
    OcrConfig config_;
    std::unique_ptr<IInferenceSession> session_;
    PipelineMetrics* metrics_;

    std::size_t plate_output_{0};
    std::size_t region_output_{static_cast<std::size_t>(-1)};

    cv::Mat converted_;   ///< crop in the model's colour mode
    cv::Mat model_input_; ///< header over the session input tensor, written in place
    cv::Mat scratch_;     ///< aspect-ratio-preserving resize target

    void preprocess(const cv::Mat& plate);
    void decodeRegion(OcrResult& result) const;
};

/// Builds the OCR stage, or returns nullptr with `error` set.
std::unique_ptr<IPlateOcr> makeFastPlateOcr(const OcrConfig& ocr, const InferenceConfig& inference,
                                            PipelineMetrics* metrics, std::string& error);

}  // namespace anpr
