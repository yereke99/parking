#pragma once

#include <memory>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

namespace anpr {

struct InferenceConfig;
struct OcrConfig;
struct PipelineMetrics;

/// Why a reading was not usable. Reported so rejections are visible in metrics and logs instead
/// of disappearing as an empty string.
enum class OcrRejection {
    kNone,
    kEmptyCrop,
    kInferenceFailed,
    kNoText,
    kWeakCharacter,
    kLowConfidence,
};

std::string toString(OcrRejection reason);

struct OcrResult {
    std::string text;
    /// Mean probability over the characters actually returned.
    float confidence{0.0F};
    /// Lowest per-character probability among the returned characters.
    float min_char_confidence{0.0F};
    std::vector<float> character_confidences;
    /// Plate region the model reads, "kz". Diagnostic only; nothing depends on it.
    std::string region;
    OcrRejection rejection{OcrRejection::kNone};

    [[nodiscard]] bool ok() const { return rejection == OcrRejection::kNone && !text.empty(); }
};

class IPlateOcr {
public:
    virtual ~IPlateOcr() = default;

    /// Reads a plate from a BGR crop, the layout OpenCV produces. Never throws.
    virtual OcrResult recognize(const cv::Mat& plate) = 0;
    /// Batch-capable abstraction. Backends may override this with true batch inference; the
    /// default preserves compatibility and executes in order through the same model instance.
    virtual std::vector<OcrResult> recognizeBatch(const std::vector<cv::Mat>& plates) {
        std::vector<OcrResult> results;
        results.reserve(plates.size());
        for (const cv::Mat& plate : plates) {
            results.push_back(recognize(plate));
        }
        return results;
    }
    [[nodiscard]] virtual std::string backendName() const = 0;
    [[nodiscard]] virtual std::string modelDescription() const = 0;
};

/// Builds the OCR stage: Nomeroff Net's Kazakhstan reader (`NomeroffOnnx`) from `ocr.model`.
std::unique_ptr<IPlateOcr> makePlateOcr(const OcrConfig& ocr,
                                        const InferenceConfig& inference,
                                        PipelineMetrics* metrics, std::string& error);

}  // namespace anpr
