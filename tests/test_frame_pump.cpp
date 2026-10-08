#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/core.hpp>

#include "anpr/camera/frame_pump.hpp"
#include "anpr/common/logging.hpp"
#include "test_framework.hpp"

namespace {

using anpr::CameraConfig;
using anpr::CameraSource;
using anpr::Frame;
using anpr::FramePump;
using anpr::PixelFormat;
using anpr::PumpStats;
using anpr::ReadStatus;

std::int64_t nowMs() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

/// Polls `condition` every 2 ms for up to `timeout_ms`.
template <typename Condition>
bool waitUntil(Condition condition, std::int64_t timeout_ms) {
    const std::int64_t deadline = nowMs() + timeout_ms;
    while (nowMs() < deadline) {
        if (condition()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return condition();
}

/// Scripted source: `frames` frames (sequence 1..frames), then nothing (live) or end of stream
/// (file). Optional failures for the reconnect tests.
class ScriptedSource final : public CameraSource {
public:
    struct Script {
        bool live{true};
        int frames{0};
        int interval_ms{0};
        PixelFormat format{PixelFormat::kBgr};
        /// open() fails for the first N attempts (-1: always).
        int failing_opens{0};
        /// Returned by reconnectDelayOverrideMs() after a failed open.
        std::int64_t override_ms{0};
        /// permanentlyFailed() turns true after this many failed opens (0: never).
        int give_up_after{0};
        /// read() fails right after every open, before any frame.
        bool fail_every_read{false};
        /// read() fails once after this many frames (0: never).
        int fail_after_frames{0};
        bool reports_own_errors{false};
        /// open() takes this long.
        int open_delay_ms{0};
        /// open() blocks this long unless interrupted, then fails.
        int blocking_open_ms{0};
    };

    explicit ScriptedSource(Script script) : script_(script) {}

    bool open() override {
        if (script_.open_delay_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(script_.open_delay_ms));
        }
        if (script_.blocking_open_ms > 0) {
            ++opens_;
            const std::int64_t deadline = nowMs() + script_.blocking_open_ms;
            while (!interrupted_.load() && nowMs() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            return false;
        }
        const int attempt = ++opens_;
        if (script_.failing_opens < 0 || attempt <= script_.failing_opens) {
            ++failed_opens_;
            return false;
        }
        open_ = true;
        ++connection_;
        return true;
    }
    void close() override { open_ = false; }
    [[nodiscard]] bool isOpen() const override { return open_; }

    ReadStatus read(Frame& frame) override {
        if (script_.fail_every_read) {
            return ReadStatus::kFailed;
        }
        if (script_.fail_after_frames > 0 && produced_ == script_.fail_after_frames &&
            !failed_once_) {
            failed_once_ = true;
            return ReadStatus::kFailed;
        }
        if (produced_ >= script_.frames) {
            if (!script_.live) {
                return ReadStatus::kEndOfStream;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return ReadStatus::kEmpty;
        }
        if (script_.interval_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(script_.interval_ms));
        }
        ++produced_;
        if (script_.format == PixelFormat::kI420) {
            frame.image.create(12, 8, CV_8UC1);
        } else {
            frame.image.create(8, 8, CV_8UC3);
        }
        frame.image.setTo(cv::Scalar::all(static_cast<double>(produced_ % 251)));
        frame.format = script_.format;
        frame.sequence = produced_;
        frame.capture_ms = nowMs();
        frame.stream_ms = frame.capture_ms;
        {
            const std::lock_guard<std::mutex> guard(mutex_);
            const auto connection = static_cast<std::size_t>(connection_.load());
            first_capture_by_connection_.resize(connection, 0);
            std::int64_t& first = first_capture_by_connection_[connection - 1];
            if (first == 0) {
                first = frame.capture_ms;
            }
        }
        return ReadStatus::kOk;
    }

    [[nodiscard]] std::string describe() const override { return "scripted"; }
    [[nodiscard]] bool reconnectable() const override { return script_.live; }
    [[nodiscard]] std::int64_t reconnectDelayOverrideMs() const override {
        return script_.override_ms;
    }
    [[nodiscard]] bool permanentlyFailed() const override {
        return script_.give_up_after > 0 && failed_opens_.load() >= script_.give_up_after;
    }
    [[nodiscard]] bool reportsOwnErrors() const override { return script_.reports_own_errors; }
    void interrupt() override { interrupted_.store(true); }

    [[nodiscard]] int opens() const { return opens_.load(); }
    [[nodiscard]] std::int64_t firstCaptureOfConnection(int connection) const {
        const std::lock_guard<std::mutex> guard(mutex_);
        const auto index = static_cast<std::size_t>(connection - 1);
        return index < first_capture_by_connection_.size() ? first_capture_by_connection_[index]
                                                           : 0;
    }

private:
    Script script_;
    std::atomic_bool open_{false};
    std::atomic_bool interrupted_{false};
    std::atomic_int opens_{0};
    std::atomic_int failed_opens_{0};
    std::atomic_int connection_{0};
    int produced_{0};
    bool failed_once_{false};
    mutable std::mutex mutex_;
    std::vector<std::int64_t> first_capture_by_connection_;
};

CameraConfig liveConfig() {
    CameraConfig config;
    config.reconnect_initial_backoff_ms = 100;
    config.reconnect_max_backoff_ms = 1000;
    // The scripted source idles with kEmpty; that must not count as a dead stream here.
    config.read_timeout_ms = 60000;
    return config;
}

/// Redirects std::cerr (where the structured logger writes) for the lifetime of the object.
class CerrCapture {
public:
    CerrCapture() : previous_(std::cerr.rdbuf(buffer_.rdbuf())) {}
    ~CerrCapture() { std::cerr.rdbuf(previous_); }
    CerrCapture(const CerrCapture&) = delete;
    CerrCapture& operator=(const CerrCapture&) = delete;
    [[nodiscard]] std::string text() const { return buffer_.str(); }

private:
    std::ostringstream buffer_;
    std::streambuf* previous_;
};

}  // namespace

TEST("FramePump live capacity 1: the newest frame wins and older ones count as dropped") {
    ScriptedSource::Script script;
    script.frames = 20;
    auto source = std::make_unique<ScriptedSource>(script);
    FramePump pump(liveConfig(), std::move(source));
    CHECK(pump.start());
    CHECK(waitUntil([&pump] { return pump.stats().captured == 20; }, 3000));

    Frame frame;
    CHECK(pump.waitForFrame(frame, 200));
    CHECK_EQ(frame.sequence, 20);
    const PumpStats stats = pump.stats();
    CHECK_EQ(stats.dropped, 19);
    CHECK(stats.connected);
    CHECK(stats.last_frame_ms > 0);
    CHECK(stats.first_frame_ms > 0);
    CHECK(stats.first_frame_ms <= stats.last_frame_ms);
    // Nothing new arrived: the slot is empty.
    CHECK(!pump.waitForFrame(frame, 30));
    pump.stop();
}

TEST("FramePump file with process_every_file_frame hands over every frame in order") {
    ScriptedSource::Script script;
    script.live = false;
    script.frames = 30;
    CameraConfig config = liveConfig();
    config.process_every_file_frame = true;
    FramePump pump(config, std::make_unique<ScriptedSource>(script));
    // A file keeps its single slot whatever capacity is asked for.
    pump.setQueueCapacity(4);
    CHECK(pump.start());

    std::vector<std::int64_t> sequences;
    Frame frame;
    const std::int64_t deadline = nowMs() + 5000;
    while (!pump.finished() && nowMs() < deadline) {
        if (pump.waitForFrame(frame, 100)) {
            sequences.push_back(frame.sequence);
            // A slow consumer: the capture thread must wait, not overwrite.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    CHECK(pump.finished());
    CHECK_EQ(sequences.size(), static_cast<std::size_t>(30));
    for (std::size_t i = 0; i < sequences.size(); ++i) {
        CHECK_EQ(sequences[i], static_cast<std::int64_t>(i + 1));
    }
    const PumpStats stats = pump.stats();
    CHECK_EQ(stats.dropped, 0);
    CHECK_EQ(stats.captured, 30);
    CHECK(stats.ended);
    pump.stop();
}

TEST("FramePump file without process_every_file_frame keeps the single overwritten slot") {
    ScriptedSource::Script script;
    script.live = false;
    script.frames = 10;
    CameraConfig config = liveConfig();
    config.process_every_file_frame = false;
    FramePump pump(config, std::make_unique<ScriptedSource>(script));
    CHECK(pump.start());
    CHECK(waitUntil([&pump] { return pump.stats().ended; }, 3000));
    Frame frame;
    CHECK(pump.waitForFrame(frame, 100));
    CHECK_EQ(frame.sequence, 10);
    CHECK_EQ(pump.stats().dropped, 9);
    CHECK(pump.finished());
    CHECK(!pump.waitForFrame(frame, 10));
    pump.stop();
}

TEST("FramePump capacity 3 never buffers more than 3 frames and returns the newest") {
    ScriptedSource::Script script;
    script.frames = 60;
    script.interval_ms = 1;
    FramePump pump(liveConfig(), std::make_unique<ScriptedSource>(script));
    pump.setQueueCapacity(3);
    CHECK(pump.start());

    std::int64_t most_buffered = 0;
    const std::int64_t deadline = nowMs() + 5000;
    while (nowMs() < deadline) {
        const PumpStats stats = pump.stats();
        // Without a consumer, every frame not dropped is still held by the pump.
        most_buffered = std::max(most_buffered, stats.captured - stats.dropped);
        if (stats.captured == 60) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(300));
    }
    PumpStats stats = pump.stats();
    CHECK_EQ(stats.captured, 60);
    CHECK(most_buffered <= 3);
    CHECK_EQ(stats.captured - stats.dropped, 3);

    Frame frame;
    CHECK(pump.waitForFrame(frame, 200));
    CHECK_EQ(frame.sequence, 60);
    stats = pump.stats();
    // The two older queued frames are dropped at pick-up: latency never accumulates.
    CHECK_EQ(stats.dropped, 59);
    CHECK(!pump.waitForFrame(frame, 20));
    pump.stop();
}

TEST("FramePump capacity is clamped to 1..8") {
    for (const std::size_t requested : {std::size_t{0}, std::size_t{100}}) {
        ScriptedSource::Script script;
        script.frames = 40;
        FramePump pump(liveConfig(), std::make_unique<ScriptedSource>(script));
        pump.setQueueCapacity(requested);
        CHECK(pump.start());
        CHECK(waitUntil([&pump] { return pump.stats().captured == 40; }, 3000));
        const PumpStats stats = pump.stats();
        CHECK_EQ(stats.captured - stats.dropped, requested == 0 ? 1 : 8);
        pump.stop();
    }
}

TEST("FramePump with a queue keeps delivering the newest frame under a slow consumer") {
    ScriptedSource::Script script;
    script.frames = 200;
    script.interval_ms = 1;
    FramePump pump(liveConfig(), std::make_unique<ScriptedSource>(script));
    pump.setQueueCapacity(4);
    CHECK(pump.start());

    std::int64_t previous = 0;
    std::int64_t taken = 0;
    Frame frame;
    const std::int64_t deadline = nowMs() + 5000;
    while (nowMs() < deadline && previous < 200) {
        if (pump.waitForFrame(frame, 50)) {
            CHECK(frame.sequence > previous);
            CHECK(!frame.image.empty());
            previous = frame.sequence;
            ++taken;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    CHECK_EQ(previous, 200);
    const PumpStats stats = pump.stats();
    // Every captured frame was either handed over or counted as dropped.
    CHECK_EQ(stats.captured, 200);
    CHECK_EQ(stats.dropped + taken, 200);
    pump.stop();
}

TEST("FramePump can leave the first open of a live source to the capture thread") {
    ScriptedSource::Script script;
    script.frames = 5;
    script.open_delay_ms = 300;
    auto source = std::make_unique<ScriptedSource>(script);
    ScriptedSource* raw = source.get();
    FramePump pump(liveConfig(), std::move(source));
    pump.setOpenInBackground(true);
    const std::int64_t started = nowMs();
    CHECK(pump.start());
    CHECK(nowMs() - started < 150);
    PumpStats stats = pump.stats();
    CHECK(!stats.connected);
    CHECK_EQ(stats.open_failures, 0);
    CHECK(waitUntil([&pump] { return pump.stats().captured == 5; }, 3000));
    stats = pump.stats();
    CHECK(stats.connected);
    // The first connection is not a reconnect.
    CHECK_EQ(stats.reconnects, 0);
    CHECK_EQ(raw->opens(), 1);
    pump.stop();

    // A file still opens on the caller's thread, so a missing one fails start().
    ScriptedSource::Script missing;
    missing.live = false;
    missing.failing_opens = -1;
    FramePump file_pump(liveConfig(), std::make_unique<ScriptedSource>(missing));
    file_pump.setOpenInBackground(true);
    CHECK(!file_pump.start());
}

TEST("FramePump stop interrupts a source blocked in its first open") {
    ScriptedSource::Script script;
    script.blocking_open_ms = 5000;
    auto source = std::make_unique<ScriptedSource>(script);
    ScriptedSource* raw = source.get();
    FramePump pump(liveConfig(), std::move(source));
    pump.setOpenInBackground(true);
    CHECK(pump.start());
    CHECK(waitUntil([raw] { return raw->opens() == 1; }, 1000));
    const std::int64_t stop_started = nowMs();
    pump.stop();
    CHECK(nowMs() - stop_started < 500);
    CHECK_EQ(raw->opens(), 1);
}

TEST("FramePump counts failed connection attempts") {
    ScriptedSource::Script script;
    script.failing_opens = 2;
    script.override_ms = 50;
    script.frames = 3;
    FramePump pump(liveConfig(), std::make_unique<ScriptedSource>(script));
    CHECK(pump.start());
    CHECK(waitUntil([&pump] { return pump.stats().captured == 3; }, 3000));
    const PumpStats stats = pump.stats();
    CHECK_EQ(stats.open_failures, 2);
    CHECK_EQ(stats.reconnects, 0);
    CHECK(stats.connected);
    pump.stop();
}

TEST("FramePump carries the pixel format with the frame") {
    ScriptedSource::Script script;
    script.frames = 3;
    script.format = PixelFormat::kI420;
    FramePump pump(liveConfig(), std::make_unique<ScriptedSource>(script));
    CHECK(pump.start());
    Frame frame;
    CHECK(pump.waitForFrame(frame, 1000));
    CHECK(frame.format == PixelFormat::kI420);
    CHECK_EQ(frame.image.type(), CV_8UC1);
    pump.stop();
}

TEST("FramePump honours the source's reconnect delay override") {
    ScriptedSource::Script script;
    script.failing_opens = -1;
    script.override_ms = 300;
    CameraConfig config = liveConfig();
    // Without the override a single attempt would fit into the second below.
    config.reconnect_initial_backoff_ms = 5000;
    config.reconnect_max_backoff_ms = 5000;
    auto source = std::make_unique<ScriptedSource>(script);
    ScriptedSource* raw = source.get();
    FramePump pump(config, std::move(source));
    CHECK(pump.start());
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    const int opens = raw->opens();
    pump.stop();
    // t = 0 (start), 300, 600, 900.
    CHECK(opens >= 3);
    CHECK(opens <= 5);
    CHECK(!pump.stats().connected);
}

TEST("FramePump uses its own backoff when the source has no override") {
    ScriptedSource::Script script;
    script.failing_opens = -1;
    CameraConfig config = liveConfig();
    config.reconnect_initial_backoff_ms = 5000;
    config.reconnect_max_backoff_ms = 5000;
    auto source = std::make_unique<ScriptedSource>(script);
    ScriptedSource* raw = source.get();
    FramePump pump(config, std::move(source));
    CHECK(pump.start());
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    // The failed first open waits the backoff before the next attempt.
    CHECK_EQ(raw->opens(), 1);
    const std::int64_t stop_started = nowMs();
    pump.stop();
    // Stopping is not stuck behind the remaining backoff.
    CHECK(nowMs() - stop_started < 500);
}

TEST("FramePump stops quietly when the source gives up") {
    ScriptedSource::Script script;
    script.failing_opens = -1;
    script.override_ms = 50;
    script.give_up_after = 2;
    auto source = std::make_unique<ScriptedSource>(script);
    ScriptedSource* raw = source.get();
    FramePump pump(liveConfig(), std::move(source));
    CHECK(pump.start());
    CHECK(waitUntil([&pump] { return pump.stats().given_up; }, 2000));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    // No attempt after the source said it must not be opened again.
    CHECK_EQ(raw->opens(), 2);
    const PumpStats stats = pump.stats();
    CHECK(stats.given_up);
    CHECK(!stats.connected);
    CHECK(!stats.ended);
    Frame frame;
    CHECK(!pump.waitForFrame(frame, 20));
    CHECK(!pump.finished());
    pump.stop();
}

TEST("FramePump tracks the first frame of the latest connection") {
    ScriptedSource::Script script;
    script.frames = 10;
    script.interval_ms = 3;
    script.fail_after_frames = 5;
    auto source = std::make_unique<ScriptedSource>(script);
    ScriptedSource* raw = source.get();
    FramePump pump(liveConfig(), std::move(source));
    CHECK(pump.start());
    CHECK(waitUntil([&pump] { return pump.stats().captured == 10; }, 3000));
    const PumpStats stats = pump.stats();
    CHECK_EQ(stats.reconnects, 1);
    CHECK(stats.connected);
    CHECK(raw->firstCaptureOfConnection(2) > 0);
    CHECK_EQ(stats.first_frame_ms, raw->firstCaptureOfConnection(2));
    CHECK(stats.first_frame_ms > raw->firstCaptureOfConnection(1));
    CHECK(stats.last_frame_ms >= stats.first_frame_ms);
    pump.stop();
}

TEST("FramePump backs off when a stream breaks before delivering any frame") {
    ScriptedSource::Script script;
    script.fail_every_read = true;
    CameraConfig config = liveConfig();
    config.reconnect_initial_backoff_ms = 200;
    config.reconnect_max_backoff_ms = 5000;
    auto source = std::make_unique<ScriptedSource>(script);
    ScriptedSource* raw = source.get();
    FramePump pump(config, std::move(source));
    CHECK(pump.start());
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    const int opens = raw->opens();
    pump.stop();
    // Opens at about 0, 200 and 600 ms instead of a busy reconnect loop.
    CHECK(opens >= 2);
    CHECK(opens <= 4);
}

TEST("FramePump applies the log context to its capture thread") {
    const anpr::LogLevel previous_level = anpr::Logger::instance().level();
    anpr::Logger::instance().setLevel(anpr::LogLevel::kInfo);
    std::string generic;
    std::string quiet;
    {
        CerrCapture capture;
        ScriptedSource::Script script;
        script.failing_opens = -1;
        script.override_ms = 50;
        FramePump pump(liveConfig(), std::make_unique<ScriptedSource>(script));
        pump.setLogContext("camera_id=cam-ctx");
        CHECK(pump.start());
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        pump.stop();
        generic = capture.text();
    }
    {
        CerrCapture capture;
        ScriptedSource::Script script;
        script.failing_opens = -1;
        script.override_ms = 50;
        script.reports_own_errors = true;
        FramePump pump(liveConfig(), std::make_unique<ScriptedSource>(script));
        pump.setLogContext("camera_id=cam-quiet");
        CHECK(pump.start());
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        pump.stop();
        quiet = capture.text();
    }
    anpr::Logger::instance().setLevel(previous_level);

    CHECK(generic.find("event=camera_reconnect_failed camera_id=cam-ctx") != std::string::npos);
    // A source that reports its own errors does not get the generic line at info level.
    CHECK(quiet.find("camera_reconnect_failed") == std::string::npos);
}
