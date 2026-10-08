#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <opencv2/core.hpp>

#include "anpr/common/config.hpp"

namespace anpr {

/// Pixel layout of `Frame::image`.
enum class PixelFormat {
    /// CV_8UC3 BGR, what the pipeline processes. Every file, USB and OpenCV source produces it.
    kBgr,
    /// Planar YUV 4:2:0 as one CV_8UC1 matrix of height * 3 / 2 rows, straight from the hardware
    /// decoder. Camera mode converts to BGR only the frames it actually processes, which saves
    /// the CPU colour conversion of every frame that is dropped as stale anyway.
    kI420,
};

struct Frame {
    cv::Mat image;
    PixelFormat format{PixelFormat::kBgr};
    /// Monotonic milliseconds captured immediately after the read returned.
    std::int64_t capture_ms{0};
    /// Stream position for file sources, otherwise equal to `capture_ms`.
    std::int64_t stream_ms{0};
    std::int64_t sequence{0};
    /// Time spent in the source read/decode call. For live capture this may include waiting for
    /// the next frame; for files it is the decode latency.
    double decode_ms{0.0};
};

enum class ReadStatus {
    kOk,
    /// Nothing available yet. The caller retries.
    kEmpty,
    /// A file source reached its end.
    kEndOfStream,
    /// The stream broke. The caller reopens.
    kFailed,
};

/// A frame source. Implementations own their handle and reopen it on `open`, never per frame.
class CameraSource {
public:
    virtual ~CameraSource() = default;

    virtual bool open() = 0;
    virtual void close() = 0;
    [[nodiscard]] virtual bool isOpen() const = 0;
    /// Reads the next frame into `frame`, reusing its buffer when the geometry is unchanged.
    virtual ReadStatus read(Frame& frame) = 0;
    /// Human-readable source description for logs. Credentials in URLs are masked.
    [[nodiscard]] virtual std::string describe() const = 0;
    /// True when reconnecting makes sense. False for a finished video file.
    [[nodiscard]] virtual bool reconnectable() const = 0;

    /// After a failed `open`: how long the capture thread should wait before the next attempt,
    /// replacing its own exponential backoff. 0 keeps the backoff. A camera that rejected the
    /// password uses this to slow down to one attempt every few minutes.
    [[nodiscard]] virtual std::int64_t reconnectDelayOverrideMs() const { return 0; }
    /// True when the source must not be opened again (for example authentication attempts are
    /// exhausted). The capture thread then stops quietly; the source has already reported why.
    [[nodiscard]] virtual bool permanentlyFailed() const { return false; }
    /// True when the source logs its own failures with a diagnostic code (RtspCameraSource), so
    /// the capture thread's generic reconnect lines would only repeat them and drop to debug.
    [[nodiscard]] virtual bool reportsOwnErrors() const { return false; }
    /// Called from another thread while the capture thread stops: a blocking `open` or `read`
    /// should give up soon (an RTSP camera may otherwise sit in a first-frame wait for seconds).
    /// Thread-safe. The source is not opened again afterwards.
    virtual void interrupt() {}
};

/// Chooses the implementation from `config.kind`, or infers it from the source string.
/// Returns nullptr with `error` set when the source cannot be interpreted.
std::unique_ptr<CameraSource> makeCameraSource(const CameraConfig& config, std::string& error);

/// Replaces the credentials in an rtsp URL with asterisks, for logging.
std::string maskCredentials(const std::string& source);

}  // namespace anpr
