#include "anpr/tracking/plate_tracker.hpp"
#include "anpr/tracking/stop_detector.hpp"

#include "test_framework.hpp"

namespace {

anpr::Detection detection(int x, int y, int w, int h, float confidence = 0.9F) {
    anpr::Detection det;
    det.box = anpr::BoundingBox{x, y, w, h};
    det.confidence = confidence;
    return det;
}

anpr::TrackingConfig trackingConfig() {
    anpr::TrackingConfig config;
    config.min_iou = 0.20;
    config.max_center_distance_ratio = 0.06;
    config.max_age_ms = 1200;
    config.min_hits = 2;
    config.box_smoothing = 0.55;
    return config;
}

anpr::StopDetectionConfig stopConfig() {
    anpr::StopDetectionConfig config;
    config.window_ms = 600;
    config.stop_duration_ms = 500;
    config.max_center_displacement_ratio = 0.18;
    config.max_size_change_ratio = 0.12;
    config.max_speed_px_per_s = 40.0;
    return config;
}

}  // namespace

TEST("tracker needs repeated hits before reporting a track") {
    anpr::PlateTracker tracker(trackingConfig());
    tracker.update({detection(100, 100, 120, 40)}, 0, 1920, 1080);
    CHECK(tracker.primary() == nullptr);
    tracker.update({detection(104, 101, 121, 40)}, 100, 1920, 1080);
    CHECK(tracker.primary() != nullptr);
    CHECK_EQ(tracker.primary()->hits, 2);
}

TEST("tracker keeps one identity across an approach") {
    anpr::PlateTracker tracker(trackingConfig());
    int x = 100;
    int width = 120;
    for (int frame = 0; frame < 8; ++frame) {
        tracker.update({detection(x, 300, width, width / 3)}, frame * 100, 1920, 1080);
        x += 14;
        width += 6;
    }
    const auto* track = tracker.primary();
    CHECK(track != nullptr);
    CHECK_EQ(track->id, 1);
    CHECK_EQ(track->hits, 8);
    CHECK(track->velocity_x > 0.0);
    CHECK(track->growth_px_per_s > 0.0);
}

TEST("tracker drops a track that stops being detected") {
    anpr::PlateTracker tracker(trackingConfig());
    tracker.update({detection(100, 100, 120, 40)}, 0, 1920, 1080);
    tracker.update({detection(102, 101, 120, 40)}, 100, 1920, 1080);
    CHECK(tracker.primary() != nullptr);
    tracker.age(1500);
    CHECK(tracker.primary() == nullptr);
    CHECK(tracker.tracks().empty());
}

TEST("tracker picks the largest plate when two are visible") {
    anpr::PlateTracker tracker(trackingConfig());
    const std::vector<anpr::Detection> frame = {detection(100, 100, 80, 26),
                                                detection(900, 500, 200, 66)};
    tracker.update(frame, 0, 1920, 1080);
    tracker.update(frame, 100, 1920, 1080);
    const auto* track = tracker.primary();
    CHECK(track != nullptr);
    CHECK(track->box.width > 150);
}

TEST("tracker associates a fast approach by centre distance") {
    // Consecutive boxes that do not overlap at all still belong to the same vehicle when the
    // centres are close relative to the frame.
    anpr::PlateTracker tracker(trackingConfig());
    tracker.update({detection(500, 500, 60, 20)}, 0, 1920, 1080);
    tracker.update({detection(575, 505, 60, 20)}, 100, 1920, 1080);
    CHECK_EQ(tracker.tracks().size(), std::size_t{1});
    CHECK_EQ(tracker.tracks()[0].hits, 2);
}

TEST("stop detector does not fire on a single quiet frame") {
    anpr::StopDetector detector(stopConfig());
    const auto state = detector.update(1, anpr::BoundingBox{100, 100, 200, 60}, 0);
    CHECK(!state.stopped);
    CHECK_EQ(state.stationary_ms, std::int64_t{0});
}

TEST("stop detector fires after the configured quiet duration") {
    anpr::StopDetector detector(stopConfig());
    bool fired = false;
    for (std::int64_t t = 0; t <= 700; t += 100) {
        const auto state = detector.update(1, anpr::BoundingBox{100, 100, 200, 60}, t);
        if (t < 500) {
            CHECK(!state.stopped);
        }
        fired = fired || state.stopped;
    }
    CHECK(fired);
}

TEST("stop detector stays clear while the vehicle rolls forward") {
    anpr::StopDetector detector(stopConfig());
    int x = 100;
    for (std::int64_t t = 0; t <= 1500; t += 100) {
        const auto state = detector.update(1, anpr::BoundingBox{x, 100, 200, 60}, t);
        CHECK(!state.stopped);
        x += 20;  // 200 px/s, well over the speed ceiling
    }
}

TEST("stop detector reacts to a growing plate even when the centre holds") {
    anpr::StopDetector detector(stopConfig());
    int width = 180;
    bool any_stop = false;
    for (std::int64_t t = 0; t <= 1200; t += 100) {
        const auto state = detector.update(1, anpr::BoundingBox{500, 100, width, width / 3}, t);
        any_stop = any_stop || state.stopped;
        width += 12;
    }
    CHECK(!any_stop);
}

TEST("stop detector restarts its window for a new vehicle") {
    anpr::StopDetector detector(stopConfig());
    for (std::int64_t t = 0; t <= 700; t += 100) {
        detector.update(1, anpr::BoundingBox{100, 100, 200, 60}, t);
    }
    CHECK(detector.state().stopped);
    const auto state = detector.update(2, anpr::BoundingBox{800, 400, 200, 60}, 800);
    CHECK(!state.stopped);
    CHECK_EQ(state.stationary_ms, std::int64_t{0});
}

TEST("motion inside the window resets the stationary timer") {
    anpr::StopDetector detector(stopConfig());
    for (std::int64_t t = 0; t <= 400; t += 100) {
        detector.update(1, anpr::BoundingBox{100, 100, 200, 60}, t);
    }
    CHECK(!detector.state().stopped);
    detector.update(1, anpr::BoundingBox{260, 100, 200, 60}, 500);
    CHECK_EQ(detector.state().stationary_ms, std::int64_t{0});
    detector.update(1, anpr::BoundingBox{260, 100, 200, 60}, 600);
    CHECK(!detector.state().stopped);
}
