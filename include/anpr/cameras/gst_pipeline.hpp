#pragma once

#include <string>
#include <vector>

#include "anpr/cameras/camera_mode_config.hpp"
#include "anpr/net/http_auth.hpp"
#include "anpr/net/sdp.hpp"
#include "anpr/net/socket.hpp"

namespace anpr::cameras {

/// What this container can decode with. Filled at runtime (OpenCV build information, GStreamer
/// plugin files, Jetson device nodes); a plain struct so the plan below is testable anywhere.
struct DecoderCapabilities {
    /// This binary drives GStreamer itself (GstCapture): bounded reads, bus errors, I420 output.
    bool gst_native{false};
    bool opencv_gstreamer{false};
    bool opencv_ffmpeg{false};
    /// nvv4l2decoder and nvvidconv plugins present (Jetson multimedia, mounted by the NVIDIA
    /// container runtime).
    bool nvidia_decoder{false};
    /// /dev/nvhost-nvdec exists in this container.
    bool nvidia_device{false};
    /// GStreamer software decoders (gst-libav) present.
    bool gst_avdec_h264{false};
    bool gst_avdec_h265{false};
    /// Base pieces needed by every GStreamer RTSP pipeline.
    bool gst_rtsp{false};
    /// One line per finding, for `camera-check`.
    std::vector<std::string> notes;
};

/// The decoder names reported as `decoder=`.
constexpr const char* kDecoderNvidiaHardware = "nvidia_hardware";
constexpr const char* kDecoderSoftwareGstreamer = "software_gstreamer";
constexpr const char* kDecoderSoftwareFfmpeg = "software_ffmpeg";

/// How an attempt is opened.
constexpr const char* kBackendGstNative = "gst_native";
constexpr const char* kBackendOpenCvGstreamer = "opencv_gstreamer";
constexpr const char* kBackendOpenCvFfmpeg = "opencv_ffmpeg";

/// One way to open a stream.
struct DecoderAttempt {
    /// kDecoderNvidiaHardware, kDecoderSoftwareGstreamer or kDecoderSoftwareFfmpeg.
    std::string decoder;
    /// kBackendGstNative (GstCapture), kBackendOpenCvGstreamer or kBackendOpenCvFfmpeg
    /// (cv::VideoCapture with CAP_GSTREAMER / CAP_FFMPEG).
    std::string backend;
    /// The pipeline description or URL handed to cv::VideoCapture. SECRET: contains the password.
    std::string source;
    /// The same with credentials removed. Safe to log.
    std::string description;
    /// Why this attempt may fail although it is planned (for example the NVIDIA plugins exist
    /// but /dev/nvhost-nvdec is missing), for `camera-check`. Usually empty.
    std::string note;
};

struct StreamEndpoint {
    net::Ipv4 host;
    std::uint16_t port{554};
    std::string path;
    net::Credentials credentials;
    bool has_credentials{false};
    /// Coded picture size from the SDP (SPS), 0 when unknown. `decode.max_width` scaling needs
    /// it: the caps after the converter must carry both dimensions to keep the aspect ratio.
    int width{0};
    int height{0};
};

/// Ordered attempts for one stream. NVIDIA hardware first when allowed and available, then the
/// software paths; native GStreamer before OpenCV's GStreamer backend before FFmpeg. Empty when
/// the preference cannot be met (nvidia_hardware requested but absent, or a codec no available
/// decoder handles); `why_empty` says why.
std::vector<DecoderAttempt> planDecoders(const StreamEndpoint& endpoint, net::VideoCodec codec,
                                         const DecoderCapabilities& capabilities,
                                         const DecodeSettings& decode, const RtspSettings& rtsp,
                                         std::string& why_empty);

/// Hardware pipeline. With `i420_output` (native capture), for H.264:
///   rtspsrc location="rtsp://ip:554/path" user-id="..." user-pw="..." protocols=tcp
///     latency=200 drop-on-latency=true tcp-timeout=5000000 ! rtph264depay
///     ! h264parse config-interval=-1 ! nvv4l2decoder enable-max-performance=true ! nvvidconv
///     ! video/x-raw,format=I420[,width=W,height=H] [! videorate max-rate=N]
///     ! appsink name=sink drop=true max-buffers=1 sync=false
/// Without it (OpenCV 4.5.0 accepts only BGR): ... ! nvvidconv
///     ! video/x-raw,format=BGRx[,width=W,height=H] [! videorate max-rate=N] ! videoconvert
///     ! video/x-raw,format=BGR ! appsink ...
/// rtspsrc also gets do-rtsp-keep-alive=true; tcp-timeout is rtsp.timeout_ms in microseconds.
/// H.265 uses rtph265depay ! h265parse. `redact` replaces the credentials with "***".
/// Width/height are added only when `decode.max_width` applies and `endpoint` knows the source
/// size. Empty for a codec other than H.264/H.265.
std::string buildNvidiaPipeline(const StreamEndpoint& endpoint, net::VideoCodec codec,
                                const DecodeSettings& decode, const RtspSettings& rtsp,
                                bool i420_output, bool redact);
/// The same with avdec_h264 / avdec_h265 instead of nvv4l2decoder, videoscale/videoconvert
/// instead of nvvidconv:
///   ... ! avdec_h264 [! videorate max-rate=N] [! videoscale] ! videoconvert
///     ! video/x-raw,format=I420|BGR[,width=W,height=H] ! appsink ...
std::string buildSoftwarePipeline(const StreamEndpoint& endpoint, net::VideoCodec codec,
                                  const DecodeSettings& decode, const RtspSettings& rtsp,
                                  bool i420_output, bool redact);

/// Width and height after the `max_width` down-scale, keeping the aspect ratio, both rounded
/// down to multiples of 8 so planes stay contiguous. Unchanged when no scaling applies or the
/// source size is unknown (0).
void scaledSize(int width, int height, int max_width, int& out_width, int& out_height);

/// Quotes a value for gst_parse_launch: "..." with \" and \\ escaped.
std::string gstQuote(const std::string& value);

}  // namespace anpr::cameras
