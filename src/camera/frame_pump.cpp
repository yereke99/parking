#include "anpr/camera/frame_pump.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

#include "anpr/common/logging.hpp"

namespace anpr {
namespace {

std::int64_t monotonicMs() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

}  // namespace

FramePump::FramePump(CameraConfig config, std::unique_ptr<CameraSource> source)
    : config_(std::move(config)), source_(std::move(source)) {}

FramePump::~FramePump() {
    stop();
}

bool FramePump::start() {
    if (running_.load()) {
        return true;
    }
    const bool opened = source_->open();
    if (!opened && !source_->reconnectable()) {
        logEvent(LogLevel::kError, "camera_unavailable",
                 LogFields().add("source", source_->describe()));
        return false;
    }
    if (opened) {
        logEvent(LogLevel::kInfo, "camera_connected",
                 LogFields().add("source", source_->describe()));
        const std::lock_guard<std::mutex> guard(mutex_);
        stats_.connected = true;
    }

    running_.store(true);
    thread_ = std::thread(&FramePump::captureLoop, this);
    return true;
}

void FramePump::stop() {
    if (!running_.exchange(false)) {
        if (thread_.joinable()) {
            thread_.join();
        }
        return;
    }
    frame_available_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
    source_->close();
}

void FramePump::publish(Frame& frame) {
    {
        const std::lock_guard<std::mutex> guard(mutex_);
        if (slot_filled_) {
            // The processing thread is still busy. The older frame is worth less than the new
            // one at a barrier, so it goes.
            ++stats_.dropped;
        }
        std::swap(slot_, frame);
        slot_filled_ = true;
        ++stats_.captured;
    }
    frame_available_.notify_one();
}

void FramePump::captureLoop() {
    Frame frame;
    std::int64_t backoff_ms = config_.reconnect_initial_backoff_ms;
    std::int64_t last_success_ms = monotonicMs();

    while (running_.load()) {
        if (!source_->isOpen()) {
            if (!source_->open()) {
                logEvent(LogLevel::kWarn, "camera_reconnect_failed",
                         LogFields()
                             .add("source", source_->describe())
                             .add("retry_in_ms", backoff_ms));
                // Sleep in slices so a stop request is not stuck behind a long backoff.
                const std::int64_t deadline = monotonicMs() + backoff_ms;
                while (running_.load() && monotonicMs() < deadline) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                backoff_ms = std::min(config_.reconnect_max_backoff_ms, backoff_ms * 2);
                continue;
            }
            {
                const std::lock_guard<std::mutex> guard(mutex_);
                ++stats_.reconnects;
                stats_.connected = true;
            }
            logEvent(LogLevel::kInfo, "camera_reconnected",
                     LogFields().add("source", source_->describe()));
            backoff_ms = config_.reconnect_initial_backoff_ms;
            last_success_ms = monotonicMs();
        }

        const ReadStatus status = source_->read(frame);
        if (status == ReadStatus::kOk) {
            backoff_ms = config_.reconnect_initial_backoff_ms;
            last_success_ms = frame.capture_ms;
            publish(frame);
            continue;
        }

        if (status == ReadStatus::kEndOfStream) {
            logEvent(LogLevel::kInfo, "camera_end_of_stream",
                     LogFields().add("source", source_->describe()));
            {
                const std::lock_guard<std::mutex> guard(mutex_);
                ended_ = true;
                stats_.ended = true;
            }
            frame_available_.notify_all();
            return;
        }

        // Empty or failed. A stream that produces nothing for the read timeout is dead even if
        // the handle still claims to be open, which is the usual RTSP failure mode.
        const bool timed_out = monotonicMs() - last_success_ms > config_.read_timeout_ms;
        if (status == ReadStatus::kFailed || timed_out) {
            logEvent(LogLevel::kWarn, "camera_stream_lost",
                     LogFields()
                         .add("source", source_->describe())
                         .add("reason", timed_out ? "read_timeout" : "read_failed"));
            source_->close();
            {
                const std::lock_guard<std::mutex> guard(mutex_);
                stats_.connected = false;
            }
            continue;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

bool FramePump::waitForFrame(Frame& frame, std::int64_t timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    const bool ready = frame_available_.wait_for(
        lock, std::chrono::milliseconds(timeout_ms), [this] {
            return slot_filled_ || ended_ || !running_.load();
        });
    if (!ready || !slot_filled_) {
        return false;
    }
    std::swap(frame, slot_);
    slot_filled_ = false;
    return !frame.image.empty();
}

PumpStats FramePump::stats() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    return stats_;
}

bool FramePump::finished() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    return ended_ && !slot_filled_;
}

}  // namespace anpr
