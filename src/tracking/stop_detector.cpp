#include "anpr/tracking/stop_detector.hpp"

#include <algorithm>
#include <cmath>

namespace anpr {

StopDetector::StopDetector(StopDetectionConfig config) : config_(config) {}

void StopDetector::reset() {
    samples_.clear();
    track_id_ = -1;
    stationary_since_ms_ = 0;
    stationary_active_ = false;
    state_ = StopState{};
}

StopState StopDetector::update(int track_id, const BoundingBox& box, std::int64_t now_ms) {
    if (track_id != track_id_) {
        reset();
        track_id_ = track_id;
    }
    if (box.empty()) {
        return state_;
    }

    samples_.push_back(Sample{now_ms, box.centerX(), box.centerY(),
                              static_cast<double>(box.width)});
    while (samples_.size() > 1 && now_ms - samples_.front().timestamp_ms > config_.window_ms) {
        samples_.pop_front();
    }

    const Sample& newest = samples_.back();
    double max_displacement = 0.0;
    double min_width = newest.width;
    double max_width = newest.width;
    for (const Sample& sample : samples_) {
        const double dx = sample.center_x - newest.center_x;
        const double dy = sample.center_y - newest.center_y;
        max_displacement = std::max(max_displacement, std::sqrt(dx * dx + dy * dy));
        min_width = std::min(min_width, sample.width);
        max_width = std::max(max_width, sample.width);
    }

    const double reference_width = std::max(1.0, newest.width);
    const double mean_width = std::max(1.0, 0.5 * (min_width + max_width));
    const Sample& oldest = samples_.front();
    const double span_s =
        static_cast<double>(newest.timestamp_ms - oldest.timestamp_ms) / 1000.0;
    const double travelled = std::sqrt(std::pow(newest.center_x - oldest.center_x, 2.0) +
                                       std::pow(newest.center_y - oldest.center_y, 2.0));

    state_.displacement_px = max_displacement;
    state_.size_change_ratio = (max_width - min_width) / mean_width;
    state_.speed_px_per_s = span_s > 0.0 ? travelled / span_s : 0.0;

    const bool displacement_ok =
        max_displacement <= config_.max_center_displacement_ratio * reference_width;
    const bool size_ok = state_.size_change_ratio <= config_.max_size_change_ratio;
    const bool speed_ok = state_.speed_px_per_s <= config_.max_speed_px_per_s;
    state_.below_threshold = displacement_ok && size_ok && speed_ok;

    if (state_.below_threshold) {
        if (!stationary_active_) {
            stationary_active_ = true;
            stationary_since_ms_ = now_ms;
        }
        state_.stationary_ms = now_ms - stationary_since_ms_;
    } else {
        stationary_active_ = false;
        state_.stationary_ms = 0;
    }

    state_.stopped = state_.stationary_ms >= config_.stop_duration_ms;
    return state_;
}

}  // namespace anpr
