#pragma once

#include <string>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include "anpr/common/config.hpp"
#include "anpr/common/geometry.hpp"

namespace anpr {

enum class QualityRejection {
    kNone,
    kTooSmall,
    kTooBlurred,
    kTooDark,
    kTooBright,
    kClipped,
    kLowScore,
};

std::string toString(QualityRejection reason);

struct ImageQuality {
    double sharpness{0.0};
    double brightness{0.0};
    double contrast{0.0};
    double clipping_ratio{0.0};
    double size_score{0.0};
    /// Combined score in [0, 1], used as an observation weight in the consensus.
    double score{0.0};
    QualityRejection rejection{QualityRejection::kNone};

    [[nodiscard]] bool acceptable() const { return rejection == QualityRejection::kNone; }
};

/// Decides whether a crop is worth an OCR call.
///
/// Cheap measurements only, in this order: size, then a Laplacian variance for blur, then mean
/// and spread for exposure, then the clipped-pixel fraction. Every one of these costs orders of
/// magnitude less than an OCR inference, which is the whole point of running them first.
class QualityAssessor {
public:
    explicit QualityAssessor(QualityConfig config);

    [[nodiscard]] ImageQuality evaluate(const cv::Mat& crop, const BoundingBox& box);

    /// Optional CLAHE pass for a crop that passed the hard gates but scored poorly. Writes into
    /// `out` and returns false when nothing was done, so the caller keeps the original.
    bool enhance(const cv::Mat& crop, const ImageQuality& quality, cv::Mat& out);

private:
    QualityConfig config_;
    cv::Ptr<cv::CLAHE> clahe_;
    cv::Mat gray_;
    cv::Mat laplacian_;
    cv::Mat enhanced_gray_;
};

}  // namespace anpr
