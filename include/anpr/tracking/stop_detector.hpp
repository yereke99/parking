#pragma once

#include <cstdint>
#include <deque>

#include "anpr/common/config.hpp"
#include "anpr/common/geometry.hpp"

namespace anpr {

struct StopState {
    /// True while the movement signals are all under their thresholds.
    bool below_threshold{false};
    /// How long `below_threshold` has held without interruption.
    std::int64_t stationary_ms{0};
    /// True once `stationary_ms` reaches the configured stop duration.
    bool stopped{false};
    double displacement_px{0.0};
    double size_change_ratio{0.0};
    double speed_px_per_s{0.0};
};

/// Decides whether a tracked plate has stopped, from temporal evidence only.
///
/// A single frame never decides anything. Three signals are measured over a rolling window:
/// centre displacement relative to the plate width, relative width change, and centre speed. All
/// three must stay under their thresholds continuously for `stop_duration_ms`.
class StopDetector {
public:
    explicit StopDetector(StopDetectionConfig config);

    /// Feeds one observation of a tracked plate. Changing `track_id` restarts the window.
    StopState update(int track_id, const BoundingBox& box, std::int64_t now_ms);

    /// Call when no track is present, so a stop cannot carry across vehicles.
    void reset();

    [[nodiscard]] const StopState& state() const { return state_; }

private:
    struct Sample {
        std::int64_t timestamp_ms{0};
        double center_x{0.0};
        double center_y{0.0};
        double width{0.0};
    };

    StopDetectionConfig config_;
    std::deque<Sample> samples_;
    int track_id_{-1};
    std::int64_t stationary_since_ms_{0};
    bool stationary_active_{false};
    StopState state_;
};

}  // namespace anpr
