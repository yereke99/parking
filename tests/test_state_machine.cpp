#include "anpr/pipeline/vehicle_state_machine.hpp"

#include "test_framework.hpp"

namespace {

anpr::MotionConfig motionConfig() {
    anpr::MotionConfig config;
    config.threshold = 0.030;
    config.quiet_threshold = 0.006;
    config.min_motion_ms = 250;
    return config;
}

anpr::RecognitionConfig recognitionConfig() {
    anpr::RecognitionConfig config;
    config.timeout_ms = 3000;
    config.cooldown_ms = 2000;
    config.leave_confirmation_ms = 500;
    config.track_lost_timeout_ms = 800;
    return config;
}

anpr::TrackObservation movingTrack(int id) {
    anpr::TrackObservation track;
    track.present = true;
    track.id = id;
    track.in_detection_zone = true;
    track.in_near_zone = true;
    track.in_stop_zone = true;
    track.in_recognition_zone = true;
    track.stopped = false;
    track.speed_px_per_s = 180.0;
    return track;
}

anpr::TrackObservation stoppedTrack(int id) {
    anpr::TrackObservation track = movingTrack(id);
    track.stopped = true;
    track.stationary_ms = 600;
    track.speed_px_per_s = 5.0;
    return track;
}

anpr::StateInput input(std::int64_t timestamp_ms, double motion,
                       anpr::TrackObservation track = {}) {
    anpr::StateInput in;
    in.timestamp_ms = timestamp_ms;
    in.motion_score = motion;
    in.track = track;
    return in;
}

}  // namespace

TEST("machine stays idle without motion or tracks") {
    anpr::VehicleStateMachine machine(motionConfig(), recognitionConfig());
    for (std::int64_t t = 0; t < 2000; t += 100) {
        const auto update = machine.update(input(t, 0.001));
        CHECK_EQ(update.state, anpr::VehicleState::kIdle);
    }
}

TEST("brief motion alone does not leave idle") {
    anpr::VehicleStateMachine machine(motionConfig(), recognitionConfig());
    CHECK_EQ(machine.update(input(0, 0.05)).state, anpr::VehicleState::kIdle);
    CHECK_EQ(machine.update(input(100, 0.05)).state, anpr::VehicleState::kIdle);
    CHECK_EQ(machine.update(input(200, 0.001)).state, anpr::VehicleState::kIdle);
}

TEST("sustained motion promotes to approaching") {
    anpr::VehicleStateMachine machine(motionConfig(), recognitionConfig());
    machine.update(input(0, 0.05));
    machine.update(input(150, 0.05));
    CHECK_EQ(machine.update(input(300, 0.05)).state, anpr::VehicleState::kApproaching);
}

TEST("a tracked plate promotes immediately without waiting on motion") {
    anpr::VehicleStateMachine machine(motionConfig(), recognitionConfig());
    anpr::TrackObservation track = movingTrack(1);
    track.in_near_zone = false;
    CHECK_EQ(machine.update(input(0, 0.001, track)).state, anpr::VehicleState::kApproaching);
}

TEST("full approach, stop, recognise, cooldown cycle") {
    anpr::VehicleStateMachine machine(motionConfig(), recognitionConfig());

    anpr::TrackObservation far = movingTrack(7);
    far.in_near_zone = false;
    far.in_stop_zone = false;
    CHECK_EQ(machine.update(input(0, 0.08, far)).state, anpr::VehicleState::kApproaching);

    CHECK_EQ(machine.update(input(400, 0.06, movingTrack(7))).state, anpr::VehicleState::kNear);

    // Still rolling: no trigger.
    const auto rolling = machine.update(input(700, 0.04, movingTrack(7)));
    CHECK_EQ(rolling.state, anpr::VehicleState::kNear);
    CHECK(!rolling.recognition_ready);

    const auto stopped = machine.update(input(1300, 0.004, stoppedTrack(7)));
    CHECK_EQ(stopped.state, anpr::VehicleState::kStopped);
    CHECK(stopped.recognition_ready);

    machine.markRecognitionActive();
    CHECK_EQ(machine.state(), anpr::VehicleState::kRecognizing);
    CHECK(machine.recognitionDeadlineMs().has_value());

    // The machine does not leave recognition on its own.
    CHECK_EQ(machine.update(input(1500, 0.004, stoppedTrack(7))).state,
             anpr::VehicleState::kRecognizing);

    machine.markRecognitionFinished(1800, true, 7);
    CHECK_EQ(machine.state(), anpr::VehicleState::kConfirmed);
    CHECK_EQ(machine.update(input(1850, 0.004, stoppedTrack(7))).state,
             anpr::VehicleState::kCooldown);
}

TEST("a stopped vehicle that stays put does not retrigger") {
    anpr::VehicleStateMachine machine(motionConfig(), recognitionConfig());
    machine.update(input(0, 0.08, movingTrack(7)));
    machine.update(input(1300, 0.004, stoppedTrack(7)));
    machine.markRecognitionActive();
    machine.markRecognitionFinished(1800, true, 7);
    machine.update(input(1850, 0.004, stoppedTrack(7)));

    for (std::int64_t t = 2000; t < 12000; t += 250) {
        const auto update = machine.update(input(t, 0.002, stoppedTrack(7)));
        CHECK_EQ(update.state, anpr::VehicleState::kCooldown);
        CHECK(!update.recognition_ready);
    }
}

TEST("cooldown ends once the vehicle leaves and the scene goes quiet") {
    anpr::VehicleStateMachine machine(motionConfig(), recognitionConfig());
    machine.update(input(0, 0.08, movingTrack(7)));
    machine.update(input(1300, 0.004, stoppedTrack(7)));
    machine.markRecognitionActive();
    machine.markRecognitionFinished(1800, true, 7);
    machine.update(input(1850, 0.004, stoppedTrack(7)));

    // Driving away: motion returns, the plate track disappears.
    machine.update(input(2200, 0.09));
    machine.update(input(3900, 0.09));
    CHECK_EQ(machine.state(), anpr::VehicleState::kCooldown);

    machine.update(input(4200, 0.002));
    CHECK_EQ(machine.update(input(4800, 0.002)).state, anpr::VehicleState::kIdle);
}

TEST("a different vehicle is not blocked by the previous cooldown") {
    anpr::VehicleStateMachine machine(motionConfig(), recognitionConfig());
    machine.update(input(0, 0.08, movingTrack(7)));
    machine.update(input(1300, 0.004, stoppedTrack(7)));
    machine.markRecognitionActive();
    machine.markRecognitionFinished(1800, true, 7);
    machine.update(input(1850, 0.004, stoppedTrack(7)));

    // Same vehicle, still there: held.
    CHECK_EQ(machine.update(input(3900, 0.05, movingTrack(7))).state,
             anpr::VehicleState::kCooldown);
    // Next car in the queue, new track id: released.
    CHECK_EQ(machine.update(input(4000, 0.05, movingTrack(9))).state,
             anpr::VehicleState::kApproaching);
}

TEST("a lost track during approach returns to idle") {
    anpr::VehicleStateMachine machine(motionConfig(), recognitionConfig());
    anpr::TrackObservation far = movingTrack(3);
    far.in_near_zone = false;
    machine.update(input(0, 0.08, far));
    CHECK_EQ(machine.state(), anpr::VehicleState::kApproaching);
    machine.update(input(500, 0.001));
    CHECK_EQ(machine.update(input(1400, 0.001)).state, anpr::VehicleState::kIdle);
}

TEST("leaving the near zone falls back to approaching") {
    anpr::VehicleStateMachine machine(motionConfig(), recognitionConfig());
    // One transition per tick: idle to approaching, then approaching to near.
    machine.update(input(0, 0.08, movingTrack(4)));
    machine.update(input(100, 0.08, movingTrack(4)));
    CHECK_EQ(machine.state(), anpr::VehicleState::kNear);
    anpr::TrackObservation away = movingTrack(4);
    away.in_near_zone = false;
    CHECK_EQ(machine.update(input(300, 0.08, away)).state, anpr::VehicleState::kApproaching);
}

TEST("an unconfirmed recognition still enters cooldown") {
    anpr::VehicleStateMachine machine(motionConfig(), recognitionConfig());
    machine.update(input(0, 0.08, movingTrack(7)));
    machine.update(input(1300, 0.004, stoppedTrack(7)));
    machine.markRecognitionActive();
    machine.markRecognitionFinished(4800, false, 7);
    CHECK_EQ(machine.state(), anpr::VehicleState::kCooldown);
}
