#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

#include "anpr/camera/camera_source.hpp"
#include "anpr/common/config.hpp"

namespace anpr {

struct PumpStats {
    std::int64_t captured{0};
    std::int64_t dropped{0};
    std::int64_t reconnects{0};
    bool connected{false};
    bool ended{false};
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

    void captureLoop();
    void publish(Frame& frame);
};

}  // namespace anpr
