#pragma once

#include <cstdint>
#include <vector>

#include "anpr/common/config.hpp"
#include "anpr/common/geometry.hpp"
#include "anpr/detection/detection.hpp"

namespace anpr {

struct TrackedPlate {
    int id{0};
    BoundingBox box;      ///< smoothed box, used for zone tests and cropping decisions
    BoundingBox last_box; ///< most recent raw detection
    double confidence{0.0};
    std::int64_t first_seen_ms{0};
    std::int64_t last_seen_ms{0};
    int hits{0};
    /// Smoothed centre velocity in pixels per second.
    double velocity_x{0.0};
    double velocity_y{0.0};
    /// Smoothed width growth in pixels per second. Positive means the plate is getting closer.
    double growth_px_per_s{0.0};

    [[nodiscard]] double speed() const;
};

/// Greedy single-camera tracker for plate boxes.
///
/// The parking scenario is deliberately simple: one fixed camera, one vehicle that matters, and a
/// known approach direction. Association is IoU first, then centre distance for the frames where
/// a plate shrinks or grows quickly. No Kalman filter and no external tracking framework, because
/// neither would change any decision the state machine makes here.
class PlateTracker {
public:
    explicit PlateTracker(TrackingConfig config);

    /// Associates a fresh set of detections. Call only on ticks where the detector actually ran.
    void update(const std::vector<Detection>& detections, std::int64_t now_ms, int frame_width,
                int frame_height);

    /// Drops stale tracks on ticks where the detector was skipped.
    void age(std::int64_t now_ms);

    /// Largest confirmed track, or nullptr. One vehicle matters, so the biggest plate wins.
    [[nodiscard]] const TrackedPlate* primary() const;

    [[nodiscard]] const std::vector<TrackedPlate>& tracks() const { return tracks_; }
    void reset();

private:
    TrackingConfig config_;
    std::vector<TrackedPlate> tracks_;
    std::vector<int> assignment_;   ///< detection index per track, reused across calls
    std::vector<char> detection_used_;
    int next_id_{1};

    void applyDetection(TrackedPlate& track, const Detection& detection, std::int64_t now_ms);
};

}  // namespace anpr
