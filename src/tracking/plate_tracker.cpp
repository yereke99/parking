#include "anpr/tracking/plate_tracker.hpp"

#include <algorithm>
#include <cmath>

namespace anpr {
namespace {

int blend(int previous, int current, double weight) {
    return static_cast<int>(std::lround(previous * (1.0 - weight) + current * weight));
}

}  // namespace

double TrackedPlate::speed() const {
    return std::sqrt(velocity_x * velocity_x + velocity_y * velocity_y);
}

PlateTracker::PlateTracker(TrackingConfig config) : config_(config) {
    tracks_.reserve(8);
    assignment_.reserve(8);
    detection_used_.reserve(16);
}

void PlateTracker::applyDetection(TrackedPlate& track, const Detection& detection,
                                  std::int64_t now_ms) {
    const double dt_s = static_cast<double>(now_ms - track.last_seen_ms) / 1000.0;
    if (dt_s > 0.0) {
        const double vx = (detection.box.centerX() - track.last_box.centerX()) / dt_s;
        const double vy = (detection.box.centerY() - track.last_box.centerY()) / dt_s;
        const double growth =
            (static_cast<double>(detection.box.width) - track.last_box.width) / dt_s;
        // Same smoothing weight as the box, so velocity and position react together.
        const double weight = config_.box_smoothing;
        track.velocity_x = track.velocity_x * (1.0 - weight) + vx * weight;
        track.velocity_y = track.velocity_y * (1.0 - weight) + vy * weight;
        track.growth_px_per_s = track.growth_px_per_s * (1.0 - weight) + growth * weight;
    }

    track.box.x = blend(track.box.x, detection.box.x, config_.box_smoothing);
    track.box.y = blend(track.box.y, detection.box.y, config_.box_smoothing);
    track.box.width = blend(track.box.width, detection.box.width, config_.box_smoothing);
    track.box.height = blend(track.box.height, detection.box.height, config_.box_smoothing);
    track.last_box = detection.box;
    track.confidence = detection.confidence;
    track.last_seen_ms = now_ms;
    ++track.hits;
}

void PlateTracker::update(const std::vector<Detection>& detections, std::int64_t now_ms,
                          int frame_width, int frame_height) {
    const double diagonal =
        std::sqrt(static_cast<double>(frame_width) * frame_width +
                  static_cast<double>(frame_height) * frame_height);
    const double max_center_distance = diagonal * config_.max_center_distance_ratio;

    detection_used_.assign(detections.size(), 0);
    assignment_.assign(tracks_.size(), -1);

    // Greedy association, best overlap first. With one relevant vehicle this is equivalent to a
    // full assignment solve and costs nothing.
    for (std::size_t track_index = 0; track_index < tracks_.size(); ++track_index) {
        double best_score = 0.0;
        int best_detection = -1;
        for (std::size_t det_index = 0; det_index < detections.size(); ++det_index) {
            if (detection_used_[det_index] != 0) {
                continue;
            }
            const double overlap = iou(tracks_[track_index].box, detections[det_index].box);
            if (overlap >= config_.min_iou && overlap > best_score) {
                best_score = overlap;
                best_detection = static_cast<int>(det_index);
            }
        }
        if (best_detection < 0) {
            // Fall back to proximity for fast approaches where consecutive boxes barely overlap.
            double best_distance = max_center_distance;
            for (std::size_t det_index = 0; det_index < detections.size(); ++det_index) {
                if (detection_used_[det_index] != 0) {
                    continue;
                }
                const double distance =
                    centerDistance(tracks_[track_index].box, detections[det_index].box);
                if (distance < best_distance) {
                    best_distance = distance;
                    best_detection = static_cast<int>(det_index);
                }
            }
        }
        if (best_detection >= 0) {
            detection_used_[static_cast<std::size_t>(best_detection)] = 1;
            assignment_[track_index] = best_detection;
        }
    }

    for (std::size_t track_index = 0; track_index < tracks_.size(); ++track_index) {
        if (assignment_[track_index] >= 0) {
            applyDetection(tracks_[track_index],
                           detections[static_cast<std::size_t>(assignment_[track_index])], now_ms);
        }
    }

    for (std::size_t det_index = 0; det_index < detections.size(); ++det_index) {
        if (detection_used_[det_index] != 0) {
            continue;
        }
        TrackedPlate track;
        track.id = next_id_++;
        track.box = detections[det_index].box;
        track.last_box = detections[det_index].box;
        track.confidence = detections[det_index].confidence;
        track.first_seen_ms = now_ms;
        track.last_seen_ms = now_ms;
        track.hits = 1;
        tracks_.push_back(track);
    }

    age(now_ms);
}

void PlateTracker::age(std::int64_t now_ms) {
    tracks_.erase(std::remove_if(tracks_.begin(), tracks_.end(),
                                 [&](const TrackedPlate& track) {
                                     return now_ms - track.last_seen_ms > config_.max_age_ms;
                                 }),
                  tracks_.end());
}

const TrackedPlate* PlateTracker::primary() const {
    const TrackedPlate* best = nullptr;
    for (const TrackedPlate& track : tracks_) {
        if (track.hits < config_.min_hits) {
            continue;
        }
        if (best == nullptr || track.box.area() > best->box.area()) {
            best = &track;
        }
    }
    return best;
}

void PlateTracker::reset() {
    tracks_.clear();
    assignment_.clear();
    detection_used_.clear();
}

}  // namespace anpr
