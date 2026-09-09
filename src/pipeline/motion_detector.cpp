#include "anpr/pipeline/motion_detector.hpp"

#include <algorithm>

#include <opencv2/imgproc.hpp>

namespace anpr {

RoiMotionDetector::RoiMotionDetector(MotionConfig config, NormalizedRect roi)
    : config_(config), roi_(roi) {}

MotionSample RoiMotionDetector::update(const cv::Mat& frame, std::int64_t now_ms) {
    MotionSample sample;
    if (frame.empty()) {
        return sample;
    }

    const BoundingBox box = roi_.toPixels(frame.cols, frame.rows);
    const cv::Rect roi_rect(box.x, box.y, box.width, box.height);
    if (roi_rect.empty()) {
        return sample;
    }

    // A view, not a copy: nothing is duplicated to look at the ROI.
    const cv::Mat roi_view = frame(roi_rect);

    const int target_width = std::min(config_.frame_width, roi_view.cols);
    const double scale = static_cast<double>(target_width) / std::max(1, roi_view.cols);
    const int target_height = std::max(1, static_cast<int>(roi_view.rows * scale));
    cv::resize(roi_view, resized_, cv::Size(target_width, target_height), 0.0, 0.0, cv::INTER_AREA);

    if (resized_.channels() == 3) {
        cv::cvtColor(resized_, gray_, cv::COLOR_BGR2GRAY);
    } else {
        gray_ = resized_;
    }
    cv::GaussianBlur(gray_, blurred_, cv::Size(5, 5), 0.0);

    // Drop references older than twice the comparison interval, and any whose geometry no longer
    // matches after a stream restart at a different resolution.
    while (!history_.empty() &&
           (now_ms - history_.front().timestamp_ms > 2 * config_.reference_interval_ms ||
            history_.front().image.size() != blurred_.size())) {
        history_.pop_front();
    }

    // The reference is the newest frame that is still at least one interval old, so the measure
    // is displacement across a fixed span rather than speed between two adjacent frames.
    const Reference* reference = nullptr;
    for (const Reference& candidate : history_) {
        if (now_ms - candidate.timestamp_ms >= config_.reference_interval_ms) {
            reference = &candidate;
        }
    }

    if (reference != nullptr) {
        cv::absdiff(blurred_, reference->image, diff_);
        cv::threshold(diff_, diff_, config_.pixel_threshold, 255, cv::THRESH_BINARY);
        const double changed = static_cast<double>(cv::countNonZero(diff_));
        const double total = static_cast<double>(std::max(1, diff_.rows * diff_.cols));
        sample.raw_score = changed / total;
        smoothed_score_ = (1.0 - config_.smoothing) * smoothed_score_ +
                          config_.smoothing * sample.raw_score;
        sample.score = smoothed_score_;
        sample.initialized = true;
    }

    // Keep one sample per interval rather than one per frame: two references inside the same
    // interval add nothing, and this bounds the deque regardless of frame rate.
    const bool need_sample =
        history_.empty() ||
        now_ms - history_.back().timestamp_ms >= config_.reference_interval_ms / 2;
    if (need_sample) {
        history_.push_back(Reference{});
        blurred_.copyTo(history_.back().image);
        history_.back().timestamp_ms = now_ms;
    }
    return sample;
}

void RoiMotionDetector::reset() {
    history_.clear();
    smoothed_score_ = 0.0;
}

}  // namespace anpr
