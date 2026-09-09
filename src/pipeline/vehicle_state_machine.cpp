#include "anpr/pipeline/vehicle_state_machine.hpp"

namespace anpr {

std::string toString(VehicleState state) {
    switch (state) {
        case VehicleState::kIdle:
            return "IDLE";
        case VehicleState::kApproaching:
            return "VEHICLE_APPROACHING";
        case VehicleState::kNear:
            return "VEHICLE_NEAR";
        case VehicleState::kStopped:
            return "VEHICLE_STOPPED";
        case VehicleState::kRecognizing:
            return "PLATE_RECOGNITION";
        case VehicleState::kConfirmed:
            return "PLATE_CONFIRMED";
        case VehicleState::kCooldown:
            return "COOLDOWN";
    }
    return "IDLE";
}

VehicleStateMachine::VehicleStateMachine(MotionConfig motion, RecognitionConfig recognition)
    : motion_(motion), recognition_(recognition) {}

void VehicleStateMachine::enter(VehicleState next, std::int64_t timestamp_ms) {
    state_ = next;
    state_entered_ms_ = timestamp_ms;
}

std::optional<std::int64_t> VehicleStateMachine::recognitionDeadlineMs() const {
    if (!recognition_started_ms_) {
        return std::nullopt;
    }
    return *recognition_started_ms_ + recognition_.timeout_ms;
}

StateUpdate VehicleStateMachine::update(const StateInput& input) {
    const VehicleState previous = state_;
    const std::int64_t now = input.timestamp_ms;
    const TrackObservation& track = input.track;
    bool recognition_ready = false;

    // Track how long the scene has been quiet and how long the plate has been missing. Both are
    // needed by several states, so they are maintained once per tick rather than per branch.
    if (input.motion_score < motion_.quiet_threshold) {
        if (!quiet_since_ms_) {
            quiet_since_ms_ = now;
        }
    } else {
        quiet_since_ms_.reset();
    }
    if (track.present) {
        track_lost_since_ms_.reset();
    } else if (!track_lost_since_ms_) {
        track_lost_since_ms_ = now;
    }

    const bool track_lost_long_enough =
        track_lost_since_ms_ && now - *track_lost_since_ms_ >= recognition_.track_lost_timeout_ms;

    switch (state_) {
        case VehicleState::kIdle: {
            // A tracked plate inside the detection zone is stronger evidence than raw motion, so
            // it promotes immediately. Otherwise motion has to persist before anything wakes up.
            if (track.present && track.in_detection_zone) {
                enter(VehicleState::kApproaching, now);
                motion_start_ms_.reset();
                break;
            }
            if (input.motion_score >= motion_.threshold) {
                if (!motion_start_ms_) {
                    motion_start_ms_ = now;
                }
                if (now - *motion_start_ms_ >= motion_.min_motion_ms) {
                    enter(VehicleState::kApproaching, now);
                }
            } else {
                motion_start_ms_.reset();
            }
            break;
        }

        case VehicleState::kApproaching: {
            if (track.present && track.in_near_zone) {
                enter(VehicleState::kNear, now);
                break;
            }
            if (!track.present && track_lost_long_enough &&
                input.motion_score < motion_.threshold) {
                enter(VehicleState::kIdle, now);
                motion_start_ms_.reset();
            }
            break;
        }

        case VehicleState::kNear: {
            if (track.present && track.stopped && track.in_stop_zone) {
                enter(VehicleState::kStopped, now);
                recognition_ready = true;
                break;
            }
            if (track.present && !track.in_near_zone) {
                enter(VehicleState::kApproaching, now);
                break;
            }
            if (!track.present && track_lost_long_enough) {
                enter(VehicleState::kIdle, now);
                motion_start_ms_.reset();
            }
            break;
        }

        case VehicleState::kStopped:
            // Held until the pipeline confirms it opened a recognition session. If the vehicle
            // pulls away first, fall back rather than recognising an empty stop zone.
            if (!track.present && track_lost_long_enough) {
                enter(VehicleState::kIdle, now);
            }
            break;

        case VehicleState::kRecognizing:
            // Left through markRecognitionFinished, which the pipeline calls on consensus or on
            // the recognition timeout.
            break;

        case VehicleState::kConfirmed:
            enter(VehicleState::kCooldown, now);
            break;

        case VehicleState::kCooldown: {
            const bool cooldown_elapsed = now - state_entered_ms_ >= recognition_.cooldown_ms;
            if (!cooldown_elapsed) {
                break;
            }
            // A different vehicle already at the barrier must not be blocked by the previous
            // vehicle's cooldown, which is what a queue at a parking entrance looks like.
            if (track.present && track.id != recognized_track_id_) {
                enter(VehicleState::kApproaching, now);
                break;
            }
            const bool vehicle_gone = !track.present;
            const bool quiet_long_enough =
                quiet_since_ms_ && now - *quiet_since_ms_ >= recognition_.leave_confirmation_ms;
            if (vehicle_gone && quiet_long_enough) {
                enter(VehicleState::kIdle, now);
                motion_start_ms_.reset();
                recognized_track_id_ = -1;
            }
            break;
        }
    }

    return StateUpdate{previous, state_, previous != state_, recognition_ready};
}

void VehicleStateMachine::markRecognitionActive() {
    if (state_ == VehicleState::kStopped) {
        state_ = VehicleState::kRecognizing;
        recognition_started_ms_ = state_entered_ms_;
    }
}

void VehicleStateMachine::markRecognitionFinished(std::int64_t timestamp_ms, bool confirmed,
                                                  int track_id) {
    recognition_started_ms_.reset();
    recognized_track_id_ = track_id;
    enter(confirmed ? VehicleState::kConfirmed : VehicleState::kCooldown, timestamp_ms);
}

void VehicleStateMachine::reset() {
    state_ = VehicleState::kIdle;
    state_entered_ms_ = 0;
    motion_start_ms_.reset();
    quiet_since_ms_.reset();
    track_lost_since_ms_.reset();
    recognition_started_ms_.reset();
    recognized_track_id_ = -1;
}

}  // namespace anpr
