#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "anpr/common/config.hpp"

namespace anpr {

enum class VehicleState {
    kIdle,
    kApproaching,
    kNear,
    kStopped,
    kRecognizing,
    kConfirmed,
    kCooldown,
};

std::string toString(VehicleState state);

/// What the tracker and the zone tests say about the plate that matters this tick.
struct TrackObservation {
    bool present{false};
    int id{0};
    bool in_detection_zone{false};
    bool in_near_zone{false};
    bool in_stop_zone{false};
    bool in_recognition_zone{false};
    bool stopped{false};
    std::int64_t stationary_ms{0};
    double speed_px_per_s{0.0};
};

struct StateInput {
    std::int64_t timestamp_ms{0};
    double motion_score{0.0};
    TrackObservation track;
    /// A confirmed plate track that has not been read yet is inside the recognition zone and
    /// large enough to read. With `recognition.require_stop` off this alone opens recognition,
    /// moving or not.
    bool readable_plate{false};
};

struct StateUpdate {
    VehicleState previous{VehicleState::kIdle};
    VehicleState state{VehicleState::kIdle};
    bool changed{false};
    /// Set on the tick where recognition may start: a readable plate (or, with
    /// `recognition.require_stop`, a confirmed stop). The pipeline opens its recognition sessions
    /// and calls `markRecognitionActive`.
    bool recognition_ready{false};
};

/// Drives the event-driven inference budget.
///
/// The machine owns the answer to "how much compute does this frame deserve". It never touches
/// images: it consumes a cheap motion score plus the tracker's verdict, so it is fully
/// deterministic and unit tested without a GPU or OpenCV.
class VehicleStateMachine {
public:
    VehicleStateMachine(MotionConfig motion, RecognitionConfig recognition);

    StateUpdate update(const StateInput& input);

    /// Called once the pipeline has opened a recognition session.
    void markRecognitionActive();
    /// Called when the session ends. `confirmed` records whether consensus produced a plate.
    void markRecognitionFinished(std::int64_t timestamp_ms, bool confirmed, int track_id);

    [[nodiscard]] VehicleState state() const { return state_; }
    [[nodiscard]] std::int64_t stateEnteredMs() const { return state_entered_ms_; }
    /// Wall-clock deadline for the active recognition session, if one is open.
    [[nodiscard]] std::optional<std::int64_t> recognitionDeadlineMs() const;
    void reset();

private:
    MotionConfig motion_;
    RecognitionConfig recognition_;
    VehicleState state_{VehicleState::kIdle};
    std::int64_t state_entered_ms_{0};
    std::optional<std::int64_t> motion_start_ms_;
    std::optional<std::int64_t> quiet_since_ms_;
    std::optional<std::int64_t> track_lost_since_ms_;
    std::optional<std::int64_t> recognition_started_ms_;
    int recognized_track_id_{-1};
    /// Timestamp of the latest update, for a recognition that starts without a stop.
    std::int64_t last_update_ms_{0};
    /// recognition_ready was raised without a stop and markRecognitionActive is still due.
    bool ready_without_stop_{false};

    /// recognition_ready for a readable plate, when stops are not required.
    bool readyWithoutStop(const StateInput& input);

    void enter(VehicleState next, std::int64_t timestamp_ms);
};

}  // namespace anpr
