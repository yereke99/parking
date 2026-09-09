#include "anpr/pipeline/quality_assessor.hpp"

#include <algorithm>
#include <cmath>

namespace anpr {
namespace {

double clamp01(double value) {
    return std::clamp(value, 0.0, 1.0);
}

}  // namespace

std::string toString(QualityRejection reason) {
    switch (reason) {
        case QualityRejection::kNone:
            return "none";
        case QualityRejection::kTooSmall:
            return "too_small";
        case QualityRejection::kTooBlurred:
            return "too_blurred";
        case QualityRejection::kTooDark:
            return "too_dark";
        case QualityRejection::kTooBright:
            return "too_bright";
        case QualityRejection::kClipped:
            return "clipped";
        case QualityRejection::kLowScore:
            return "low_score";
    }
    return "none";
}

QualityAssessor::QualityAssessor(QualityConfig config) : config_(config) {
    if (config_.enable_enhancement) {
        clahe_ = cv::createCLAHE(config_.clahe_clip_limit, cv::Size(8, 8));
    }
}

ImageQuality QualityAssessor::evaluate(const cv::Mat& crop, const BoundingBox& box) {
    ImageQuality quality;
    if (crop.empty() || box.empty()) {
        quality.rejection = QualityRejection::kTooSmall;
        return quality;
    }

    // Size first: it costs nothing and rejects the majority of useless crops.
    if (box.width < config_.min_plate_width_px || box.height < config_.min_plate_height_px) {
        quality.rejection = QualityRejection::kTooSmall;
        return quality;
    }

    if (crop.channels() == 3) {
        cv::cvtColor(crop, gray_, cv::COLOR_BGR2GRAY);
    } else {
        gray_ = crop;
    }

    cv::Scalar mean;
    cv::Scalar stddev;
    cv::meanStdDev(gray_, mean, stddev);
    quality.brightness = mean[0];
    quality.contrast = stddev[0];

    cv::Laplacian(gray_, laplacian_, CV_64F);
    cv::Scalar laplacian_mean;
    cv::Scalar laplacian_stddev;
    cv::meanStdDev(laplacian_, laplacian_mean, laplacian_stddev);
    quality.sharpness = laplacian_stddev[0] * laplacian_stddev[0];

    const int total = std::max(1, gray_.rows * gray_.cols);
    const int dark = cv::countNonZero(gray_ < 8);
    const int bright = cv::countNonZero(gray_ > 247);
    quality.clipping_ratio = static_cast<double>(dark + bright) / total;

    const double sharpness_score = clamp01(quality.sharpness / config_.sharpness_reference);
    const double contrast_score = clamp01(quality.contrast / config_.contrast_reference);
    const double brightness_score = clamp01(
        1.0 - std::abs(quality.brightness - config_.brightness_reference) /
                  std::max(1.0, config_.brightness_reference));
    const double clipping_score = clamp01(1.0 - quality.clipping_ratio * 4.0);
    quality.size_score = clamp01(static_cast<double>(box.width) /
                                 std::max(1, config_.min_plate_width_px * 2));
    quality.score = clamp01(0.30 * sharpness_score + 0.20 * contrast_score +
                            0.20 * brightness_score + 0.20 * clipping_score +
                            0.10 * quality.size_score);

    if (quality.sharpness < config_.min_sharpness) {
        quality.rejection = QualityRejection::kTooBlurred;
    } else if (quality.brightness < config_.min_brightness) {
        quality.rejection = QualityRejection::kTooDark;
    } else if (quality.brightness > config_.max_brightness) {
        quality.rejection = QualityRejection::kTooBright;
    } else if (quality.clipping_ratio > config_.max_clipping_ratio) {
        quality.rejection = QualityRejection::kClipped;
    } else if (quality.score < config_.min_score) {
        quality.rejection = QualityRejection::kLowScore;
    }
    return quality;
}

bool QualityAssessor::enhance(const cv::Mat& crop, const ImageQuality& quality, cv::Mat& out) {
    if (!config_.enable_enhancement || clahe_.empty() || crop.empty()) {
        return false;
    }
    if (quality.score >= config_.enhance_below_score) {
        return false;
    }
    // Contrast only, on the luminance channel. No sharpening and no denoising: those cost more
    // than they return on plate crops, and they invent edges the OCR model then reads.
    if (crop.channels() == 3) {
        cv::cvtColor(crop, gray_, cv::COLOR_BGR2GRAY);
    } else {
        gray_ = crop;
    }
    clahe_->apply(gray_, enhanced_gray_);
    cv::cvtColor(enhanced_gray_, out, cv::COLOR_GRAY2BGR);
    return true;
}

}  // namespace anpr
