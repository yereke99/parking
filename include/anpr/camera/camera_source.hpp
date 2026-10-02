#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <opencv2/core.hpp>

#include "anpr/common/config.hpp"

namespace anpr {

struct Frame {
    cv::Mat image;
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
};

/// Chooses the implementation from `config.kind`, or infers it from the source string.
/// Returns nullptr with `error` set when the source cannot be interpreted.
std::unique_ptr<CameraSource> makeCameraSource(const CameraConfig& config, std::string& error);

/// Replaces the credentials in an rtsp URL with asterisks, for logging.
std::string maskCredentials(const std::string& source);

}  // namespace anpr
