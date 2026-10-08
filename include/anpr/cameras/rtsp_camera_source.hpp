#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <opencv2/videoio.hpp>

#include "anpr/camera/camera_source.hpp"
#include "anpr/cameras/camera_mode_config.hpp"
#include "anpr/cameras/diagnostics.hpp"
#include "anpr/cameras/gst_pipeline.hpp"
#include "anpr/cameras/reconnect_policy.hpp"
#include "anpr/cameras/rtsp_preflight.hpp"
#include "anpr/net/network_topology.hpp"
#include "anpr/net/sdp.hpp"

namespace anpr::cameras {

/// Detects what this container can decode with: OpenCV's videoio backends
/// (cv::getBuildInformation), the NVIDIA GStreamer plugins and the Jetson decoder device node.
DecoderCapabilities detectDecoderCapabilities();

class GstCapture;

/// Deletes a GstCapture. GstCapture is only compiled with the GStreamer development files; the
/// deleter keeps this header (and StreamDecoder's destructor) the same in builds without them.
struct GstCaptureDeleter {
    void operator()(GstCapture* capture) const;
};

/// One decoder attempt opened for reading: the native GStreamer capture (bounded reads, I420
/// frames) or cv::VideoCapture with its GStreamer or FFmpeg backend (BGR frames). Shared by
/// RtspCameraSource and the camera check, so `camera-check` opens a stream exactly the way the
/// running service will.
class StreamDecoder {
public:
    enum class Status {
        kFrame,
        /// Opened, but no frame within the timeout (native capture only).
        kTimeout,
        /// The stream ended or broke (EOF, connection reset, read() returned nothing).
        kEndOfStream,
        /// Could not be opened, or the decoder reported an error.
        kError,
    };

    StreamDecoder();
    ~StreamDecoder();
    StreamDecoder(const StreamDecoder&) = delete;
    StreamDecoder& operator=(const StreamDecoder&) = delete;

    /// Opens `attempt` and waits for its first frame, which the next `read` returns. On anything
    /// but kFrame the decoder is closed and `error` says why; it never contains credentials.
    /// OpenCV's backends cannot bound the wait (OpenCV 4.5 has no open/read timeouts for
    /// GStreamer; FFmpeg's is fixed at 30 s), so `first_frame_timeout_ms` applies to the native
    /// capture only.
    Status open(const DecoderAttempt& attempt, std::int64_t first_frame_timeout_ms,
                std::string& error);
    /// The next frame into `image` (its buffer is reused when the geometry is unchanged).
    /// `timeout_ms` bounds the wait on the native capture only.
    Status read(cv::Mat& image, PixelFormat& format, std::int64_t timeout_ms, std::string& error);
    void close();
    [[nodiscard]] bool isOpen() const;
    /// Thread-safe: a wait in `open` or `read` on the native capture ends within one poll slice
    /// (kError "interrupted"), and later opens fail at once. OpenCV's reads cannot be cut short.
    void interrupt();
    /// Frame rate announced by the decoder (caps or container), 0 when unknown.
    [[nodiscard]] double streamFps() const;
    /// Picture size of the first frame after `open`.
    [[nodiscard]] int width() const { return width_; }
    [[nodiscard]] int height() const { return height_; }

private:
    std::string backend_;
    std::unique_ptr<GstCapture, GstCaptureDeleter> gst_;
    cv::VideoCapture capture_;
    cv::Mat pending_;
    PixelFormat pending_format_{PixelFormat::kBgr};
    bool has_pending_{false};
    int width_{0};
    int height_{0};
    std::atomic_bool interrupted_{false};
};

/// Picture width and height of a decoded image: an I420 matrix has height * 3 / 2 rows.
void pictureSize(const cv::Mat& image, PixelFormat format, int& width, int& height);

/// Masks anything in a decoder message that looks like a credential: the values of user-id,
/// user-pw, proxy-id and proxy-pw properties and the user-info of URLs
/// ("rtsp://<redacted>@host/..."). GStreamer and OpenCV texts reach logs and the status file.
std::string redactSecrets(std::string text);

/// A Hikvision (or any RTSP) camera as a CameraSource.
///
/// Every `open` first runs the cheap RTSP DESCRIBE probe (checkRtsp), so an unreachable camera,
/// a closed port, a wrong password or a wrong path is classified precisely and never reaches the
/// decoder; then it tries the planned decoders in order (NVIDIA hardware first) and remembers the
/// one that worked for the next reconnect. Authentication failures switch the capture thread to
/// the slow ReconnectPolicy schedule through `reconnectDelayOverrideMs`.
///
/// `open`, `read` and `close` run on the capture thread; `status()` may be called from any thread.
class RtspCameraSource final : public CameraSource {
public:
    struct Status {
        CameraError error{CameraError::kNone};
        std::string detail;
        std::string decoder;  ///< decoder in use, empty when not open
        net::VideoCodec codec{net::VideoCodec::kUnknown};
        int width{0};
        int height{0};
        /// The stream's own frame rate as reported by the decoder, 0 when unknown.
        double stream_fps{0.0};
        bool open{false};
        bool rtsp_ok{false};
        bool reachable{false};
        std::int64_t opens{0};
        std::int64_t failures{0};
        /// open() start to first decoded frame of the latest successful open, ms.
        double first_frame_ms{0.0};
    };

    RtspCameraSource(CameraTarget target, CameraModeConfig config, DecoderCapabilities capabilities,
                     net::NetworkSnapshot snapshot, net::CameraLanSelection lan);
    ~RtspCameraSource() override;

    bool open() override;
    void close() override;
    [[nodiscard]] bool isOpen() const override;
    ReadStatus read(Frame& frame) override;
    [[nodiscard]] std::string describe() const override;
    [[nodiscard]] bool reconnectable() const override { return true; }
    [[nodiscard]] std::int64_t reconnectDelayOverrideMs() const override;
    [[nodiscard]] bool permanentlyFailed() const override;
    /// Every failure is logged here with its diagnostic code (rate limited).
    [[nodiscard]] bool reportsOwnErrors() const override { return true; }
    /// Ends a first-frame wait or a read early when the capture thread stops; nothing is
    /// reported for what an interruption cuts short.
    void interrupt() override;

    [[nodiscard]] Status status() const;
    [[nodiscard]] const CameraTarget& target() const { return target_; }

private:
    CameraTarget target_;
    CameraModeConfig config_;
    DecoderCapabilities capabilities_;
    net::NetworkSnapshot snapshot_;
    net::CameraLanSelection lan_;
    ReconnectPolicy policy_;

    StreamDecoder decoder_;
    /// SECRET: the attempts carry the password in their `source`.
    std::vector<DecoderAttempt> plan_;
    /// Decoder and backend of the attempt that worked last, tried first on the next open.
    std::string working_decoder_;
    std::string working_backend_;
    std::int64_t sequence_{0};
    std::int64_t open_started_ms_{0};
    /// HARDWARE_DECODER_UNAVAILABLE is logged once per camera, not on every reconnect.
    bool hardware_fallback_reported_{false};
    /// Coded picture size learnt from a decoded frame when the SDP does not carry it.
    int coded_width_{0};
    int coded_height_{0};
    std::atomic<std::int64_t> delay_override_ms_{0};
    std::atomic_bool given_up_{false};
    std::atomic_bool interrupted_{false};

    mutable std::mutex status_mutex_;
    Status status_;
    /// The error last written to the log, so a camera that stays down is not reported on every
    /// retry.
    CameraError last_reported_{CameraError::kNone};
    std::int64_t last_reported_ms_{0};

    void fail(CameraError error, const std::string& detail, Stage stage);
    void report(CameraError error, const std::string& detail, Stage stage);
    void reportHardwareFallback(const std::vector<std::string>& failures);
    /// Opens the attempts of `plan_` in order until one delivers a frame; nullptr when none
    /// does, with every attempt's error in `failures`.
    const DecoderAttempt* openFirstWorking(std::vector<std::string>& failures,
                                           bool& opened_without_frames);
};

}  // namespace anpr::cameras
