#include "anpr/camera/frame_pump.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <utility>

#include "anpr/common/logging.hpp"

namespace anpr {
namespace {

std::int64_t monotonicMs() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

constexpr std::size_t kMaxQueueCapacity = 8;

}  // namespace

FramePump::FramePump(CameraConfig config, std::unique_ptr<CameraSource> source)
    : config_(std::move(config)), source_(std::move(source)) {}

FramePump::~FramePump() {
    stop();
}

void FramePump::setQueueCapacity(std::size_t capacity) {
    capacity_ = std::min(kMaxQueueCapacity, std::max<std::size_t>(1, capacity));
}

void FramePump::setLogContext(std::string fields) {
    log_context_ = std::move(fields);
}

void FramePump::setOpenInBackground(bool enabled) {
    open_in_background_ = enabled;
}

bool FramePump::start() {
    if (running_.load()) {
        return true;
    }
    // The first open runs on the caller's thread; its lines carry the camera's context too.
    const LogContext context(log_context_.empty() ? LogContext::current() : log_context_);
    first_open_pending_ = open_in_background_ && source_->reconnectable();
    bool opened = false;
    if (!first_open_pending_) {
        try {
            opened = source_->open();
        } catch (const std::exception& error) {
            logEvent(LogLevel::kWarn, "camera_open_exception",
                     LogFields()
                         .add("source", source_->describe())
                         .addQuoted("what", error.what()));
        }
    }
    if (!opened && !first_open_pending_) {
        const std::lock_guard<std::mutex> guard(mutex_);
        ++stats_.open_failures;
    }
    if (!opened && !source_->reconnectable()) {
        logEvent(LogLevel::kError, "camera_unavailable",
                 LogFields().add("source", source_->describe()));
        return false;
    }
    if (opened) {
        logEvent(source_->reportsOwnErrors() ? LogLevel::kDebug : LogLevel::kInfo,
                 "camera_connected", LogFields().add("source", source_->describe()));
        const std::lock_guard<std::mutex> guard(mutex_);
        stats_.connected = true;
    }

    // Only file sources cannot reconnect; nothing else may make the capture thread wait. A file
    // also keeps the single slot whatever capacity was asked for.
    wait_for_slot_ = config_.process_every_file_frame && !source_->reconnectable();
    if (!source_->reconnectable()) {
        capacity_ = 1;
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
    {
        // Taking the lock orders the stop flag before a capture thread that is about to wait.
        const std::lock_guard<std::mutex> guard(mutex_);
    }
    frame_available_.notify_all();
    slot_free_.notify_all();
    // A capture thread inside a long open or read returns early instead of holding up shutdown.
    source_->interrupt();
    if (thread_.joinable()) {
        thread_.join();
    }
    source_->close();
}

void FramePump::sleepInterruptible(std::int64_t delay_ms) const {
    const std::int64_t deadline = monotonicMs() + delay_ms;
    while (running_.load() && monotonicMs() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

void FramePump::publish(Frame& frame, bool first_after_connect) {
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (wait_for_slot_) {
            slot_free_.wait(lock, [this] { return !slot_filled_ || !running_.load(); });
            if (!running_.load()) {
                return;
            }
        }
        stats_.last_frame_ms = frame.capture_ms;
        if (first_after_connect) {
            stats_.first_frame_ms = frame.capture_ms;
        }
        ++stats_.captured;

        if (capacity_ <= 1) {
            if (slot_filled_) {
                // The processing thread is still busy. The older frame is worth less than the
                // new one at a barrier, so it goes.
                ++stats_.dropped;
            }
            std::swap(slot_, frame);
            slot_filled_ = true;
        } else {
            // The newest frame always sits in `slot_`; older ones wait in `queue_` until the
            // queue is full, then the oldest goes and its buffer comes back to the capture thread.
            if (slot_filled_) {
                queue_.emplace_back();
                std::swap(queue_.back(), slot_);
            }
            std::swap(slot_, frame);
            slot_filled_ = true;
            if (queue_.size() + 1 > capacity_) {
                ++stats_.dropped;
                std::swap(frame, queue_.front());
                queue_.pop_front();
            } else if (frame.image.empty() && !spare_.empty()) {
                std::swap(frame, spare_.back());
                spare_.pop_back();
            }
        }
    }
    frame_available_.notify_one();
}

void FramePump::captureLoop() {
    const LogContext context(log_context_.empty() ? LogContext::current() : log_context_);
    // A source that reports its own failures with a diagnostic code (RTSP cameras) would only be
    // repeated by the generic lines below, so they drop to debug for it.
    const bool quiet = source_->reportsOwnErrors();
    const LogLevel failure_level = quiet ? LogLevel::kDebug : LogLevel::kWarn;
    const LogLevel lifecycle_level = quiet ? LogLevel::kDebug : LogLevel::kInfo;

    Frame frame;
    std::int64_t backoff_ms = config_.reconnect_initial_backoff_ms;
    std::int64_t last_success_ms = monotonicMs();
    // start() made the first attempt unless it left it to this thread. When it failed, the next
    // one waits like after any failed attempt: a camera that rejected the password must not be
    // asked again at once.
    bool open_failed = !first_open_pending_ && !source_->isOpen();
    bool first_after_connect = source_->isOpen();
    bool ever_connected = source_->isOpen();

    while (running_.load()) {
        if (!source_->isOpen()) {
            if (open_failed) {
                if (source_->permanentlyFailed()) {
                    {
                        const std::lock_guard<std::mutex> guard(mutex_);
                        stats_.given_up = true;
                        stats_.connected = false;
                    }
                    frame_available_.notify_all();
                    logEvent(LogLevel::kDebug, "camera_capture_stopped",
                             LogFields()
                                 .add("source", source_->describe())
                                 .add("reason", "source_gave_up"));
                    return;
                }
                // The source may ask for its own schedule (minutes after a rejected password);
                // otherwise the exponential backoff applies.
                const std::int64_t override_ms = source_->reconnectDelayOverrideMs();
                const std::int64_t delay_ms = override_ms > 0 ? override_ms : backoff_ms;
                logEvent(failure_level, "camera_reconnect_failed",
                         LogFields()
                             .add("source", source_->describe())
                             .add("retry_in_ms", delay_ms));
                sleepInterruptible(delay_ms);
                if (override_ms <= 0) {
                    backoff_ms = std::min(config_.reconnect_max_backoff_ms, backoff_ms * 2);
                }
                open_failed = false;
                continue;
            }

            bool opened = false;
            try {
                opened = source_->open();
            } catch (const std::exception& error) {
                logEvent(LogLevel::kWarn, "camera_open_exception",
                         LogFields()
                             .add("source", source_->describe())
                             .addQuoted("what", error.what()));
            }
            if (!opened) {
                const std::lock_guard<std::mutex> guard(mutex_);
                ++stats_.open_failures;
                open_failed = true;
                continue;
            }
            {
                const std::lock_guard<std::mutex> guard(mutex_);
                if (ever_connected) {
                    ++stats_.reconnects;
                }
                stats_.connected = true;
                stats_.first_frame_ms = 0;
            }
            logEvent(lifecycle_level, ever_connected ? "camera_reconnected" : "camera_connected",
                     LogFields().add("source", source_->describe()));
            ever_connected = true;
            // The backoff resets with the first frame, not here: a stream that opens and breaks
            // at once keeps backing off instead of reconnecting in a busy loop.
            last_success_ms = monotonicMs();
            first_after_connect = true;
        }

        ReadStatus status = ReadStatus::kFailed;
        try {
            status = source_->read(frame);
        } catch (const std::exception& error) {
            // A decoder that throws is treated like a broken stream: reopen, never terminate.
            logEvent(LogLevel::kWarn, "camera_read_exception",
                     LogFields()
                         .add("source", source_->describe())
                         .addQuoted("what", error.what()));
            status = ReadStatus::kFailed;
        }
        if (status == ReadStatus::kOk) {
            backoff_ms = config_.reconnect_initial_backoff_ms;
            last_success_ms = frame.capture_ms;
            publish(frame, first_after_connect);
            first_after_connect = false;
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
            logEvent(failure_level, "camera_stream_lost",
                     LogFields()
                         .add("source", source_->describe())
                         .add("reason", timed_out ? "read_timeout" : "read_failed"));
            source_->close();
            {
                const std::lock_guard<std::mutex> guard(mutex_);
                stats_.connected = false;
                if (first_after_connect) {
                    ++stats_.open_failures;
                }
            }
            // Lost before delivering a single frame: that connection attempt failed, so the
            // next one waits like after a failed open.
            open_failed = first_after_connect;
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
    if (!queue_.empty()) {
        // Newest wins: the frames that queued up behind it are counted as dropped and their
        // buffers kept for the capture thread.
        stats_.dropped += static_cast<std::int64_t>(queue_.size());
        for (Frame& stale : queue_) {
            if (spare_.size() < capacity_) {
                spare_.push_back(std::move(stale));
            }
        }
        queue_.clear();
    }
    lock.unlock();
    slot_free_.notify_one();
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
