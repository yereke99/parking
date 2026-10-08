#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "anpr/camera/camera_source.hpp"
#include "anpr/common/config.hpp"

namespace anpr {

struct PumpStats {
    std::int64_t captured{0};
    std::int64_t dropped{0};
    std::int64_t reconnects{0};
    bool connected{false};
    bool ended{false};
    /// The source reported it must not be reopened; the capture thread has stopped.
    bool given_up{false};
    /// Monotonic ms of the last frame read, 0 before the first.
    std::int64_t last_frame_ms{0};
    /// Monotonic ms of the first frame after the latest (re)connect, 0 before it.
    std::int64_t first_frame_ms{0};
    /// Failed connection attempts: opens that failed and streams lost before their first frame.
    std::int64_t open_failures{0};
};

/// Capture thread with a single-frame handover slot.
///
/// The capture thread must never wait for inference: an RTSP stream that is not drained backs up
/// in the driver and every frame the pipeline eventually sees is stale. So capture runs on its
/// own thread and writes into one slot, overwriting whatever is there. A frame the processing
/// thread did not collect in time is counted as dropped and discarded.
///
/// For a real-time barrier this is the right trade: the newest view of the vehicle matters, the
/// one from 400 ms ago does not. Reconnection with exponential backoff also lives here, so a
/// camera outage never reaches the pipeline as anything worse than a gap in frames.
///
/// A video file has nothing to go stale. With `camera.process_every_file_frame` the capture
/// thread waits for the slot instead, so a clip is processed frame by frame.
class FramePump {
public:
    FramePump(CameraConfig config, std::unique_ptr<CameraSource> source);
    ~FramePump();

    /// Live sources only: keep up to `capacity` (1..8) newest frames instead of one; the oldest is
    /// dropped and counted when the queue is full. Call before `start`. Files keep their
    /// one-slot, wait-for-the-pipeline behaviour.
    ///
    /// `waitForFrame` still returns the NEWEST queued frame and counts the older ones as dropped.
    /// Handing them out oldest first would put one processing tick of latency behind every queued
    /// frame, and at a barrier a late view is worse than a gap. So the capacity only bounds how
    /// many decoded frames a burst can leave in memory at once, and their buffers are recycled.
    void setQueueCapacity(std::size_t capacity);
    /// Fields prepended to every log line of the capture thread ("camera_id=camera-02").
    void setLogContext(std::string fields);
    /// Reconnectable sources only: `start` returns at once and the capture thread makes the first
    /// open. An RTSP camera's first open (login check, decoder attempts, first frame) can take
    /// many seconds; it must not hold up the caller, other cameras or the status loop. Call
    /// before `start`. Files always open synchronously, so a missing file still fails `start`.
    void setOpenInBackground(bool enabled);
    /// Access to the source, for status reporting. The source is driven by the capture thread;
    /// only call its thread-safe accessors.
    [[nodiscard]] const CameraSource& source() const { return *source_; }

    FramePump(const FramePump&) = delete;
    FramePump& operator=(const FramePump&) = delete;
    FramePump(FramePump&&) = delete;
    FramePump& operator=(FramePump&&) = delete;

    /// Starts the capture thread. Returns false if the first open fails on a source that cannot
    /// be reconnected, such as a missing file.
    bool start();
    void stop();

    /// Moves the newest unread frame into `frame`. Returns false on timeout or end of stream.
    bool waitForFrame(Frame& frame, std::int64_t timeout_ms);

    [[nodiscard]] PumpStats stats() const;
    /// True once a file source is exhausted and no frames remain.
    [[nodiscard]] bool finished() const;

private:
    CameraConfig config_;
    std::unique_ptr<CameraSource> source_;
    std::thread thread_;
    std::atomic_bool running_{false};

    mutable std::mutex mutex_;
    std::condition_variable frame_available_;
    std::condition_variable slot_free_;
    Frame slot_;
    bool slot_filled_{false};
    bool wait_for_slot_{false};
    PumpStats stats_;
    bool ended_{false};
    /// Extra queued frames beyond `slot_` for live sources with a capacity above one, oldest
    /// first. Bounded by `capacity_ - 1`.
    std::deque<Frame> queue_;
    std::size_t capacity_{1};
    std::string log_context_;
    bool open_in_background_{false};
    /// Set by `start` when the capture thread makes the first open.
    bool first_open_pending_{false};
    /// Buffers of frames dropped from `queue_`, handed back to the capture thread so a full
    /// queue does not allocate a new frame per read. At most `capacity_` of them.
    std::vector<Frame> spare_;

    void captureLoop();
    void publish(Frame& frame, bool first_after_connect);
    /// Waits `delay_ms` in short slices so a stop request is not stuck behind a long backoff.
    void sleepInterruptible(std::int64_t delay_ms) const;
};

}  // namespace anpr
