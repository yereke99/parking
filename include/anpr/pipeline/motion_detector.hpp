#pragma once

#include <cstdint>
#include <deque>

#include <opencv2/core.hpp>

#include "anpr/common/config.hpp"
#include "anpr/common/geometry.hpp"

namespace anpr {

struct MotionSample {
    /// Smoothed fraction of changed pixels in the ROI.
    double score{0.0};
    double raw_score{0.0};
    /// False until a reference frame old enough to compare against exists.
    bool initialized{false};
};

/// The cheapest stage in the pipeline, and the only one that runs on every single frame.
///
/// A downscaled grayscale difference over one ROI. This is what makes an empty barrier nearly
/// free: while nothing moves, the detector never runs and the GPU stays idle.
///
/// The comparison is against a frame from `reference_interval_ms` ago, not the immediately
/// previous one. Differencing consecutive frames measures speed, and a vehicle creeping toward a
/// barrier moves too few pixels between two frames to clear any threshold that also rejects
/// sensor noise. Comparing across a fixed interval measures displacement instead, which is the
/// quantity that actually distinguishes an approaching vehicle from an empty scene.
class RoiMotionDetector {
public:
    RoiMotionDetector(MotionConfig config, NormalizedRect roi);

    MotionSample update(const cv::Mat& frame, std::int64_t now_ms);
    void reset();

private:
    struct Reference {
        cv::Mat image;
        std::int64_t timestamp_ms{0};
    };

    MotionConfig config_;
    NormalizedRect roi_;
    /// Bounded history of downscaled ROI frames. At 320 px wide these are tens of kilobytes
    /// each and the deque is trimmed every update, so memory stays flat.
    std::deque<Reference> history_;
    cv::Mat gray_;
    cv::Mat resized_;
    cv::Mat blurred_;
    cv::Mat diff_;
    double smoothed_score_{0.0};
};

}  // namespace anpr
