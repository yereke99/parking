#pragma once

#include <string>
#include <vector>

#include <opencv2/core.hpp>

namespace anpr {

/// Why a reading was not usable. Reported so rejections are visible in metrics and logs instead
/// of disappearing as an empty string.
enum class OcrRejection {
    kNone,
    kEmptyCrop,
    kInferenceFailed,
    kAllPadding,
    kInteriorPadding,
    kWeakCharacter,
    kLowConfidence,
};

std::string toString(OcrRejection reason);

struct OcrResult {
    std::string text;
    /// Mean probability over the characters actually returned. Padding slots are excluded, so a
    /// short plate is not flattered by the model's near-certain padding predictions.
    float confidence{0.0F};
    /// Lowest per-character probability among the returned characters.
    float min_char_confidence{0.0F};
    std::vector<float> character_confidences;
    /// Region head output, when the model has one. Diagnostic only; nothing depends on it.
    std::string region;
    float region_confidence{0.0F};
    OcrRejection rejection{OcrRejection::kNone};

    [[nodiscard]] bool ok() const { return rejection == OcrRejection::kNone && !text.empty(); }
};

class IPlateOcr {
public:
    virtual ~IPlateOcr() = default;

    /// Reads a plate from a BGR crop, the layout OpenCV produces. Never throws.
    virtual OcrResult recognize(const cv::Mat& plate) = 0;
    [[nodiscard]] virtual std::string backendName() const = 0;
    [[nodiscard]] virtual std::string modelDescription() const = 0;
};

}  // namespace anpr
