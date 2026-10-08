// GStreamer pipeline descriptions and the ordered decoder plan for one RTSP stream. Pure string
// building, so every combination is unit tested anywhere; the runtime opens the result with
// GstCapture or cv::VideoCapture.
#include "anpr/cameras/gst_pipeline.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>

#include "anpr/net/rtsp_client.hpp"

namespace anpr::cameras {
namespace {

constexpr const char* kRedacted = "***";
constexpr const char* kAppSink = "appsink name=sink drop=true max-buffers=1 sync=false";

bool supportedCodec(net::VideoCodec codec) {
    return codec == net::VideoCodec::kH264 || codec == net::VideoCodec::kH265;
}

std::string normalizedPath(const std::string& path) {
    if (path.empty()) {
        return "/";
    }
    return path.front() == '/' ? path : "/" + path;
}

bool sendsCredentials(const StreamEndpoint& endpoint) {
    return endpoint.has_credentials && !endpoint.credentials.empty();
}

/// rtspsrc with the credentials as properties rather than in the URL: no percent-encoding
/// pitfalls, and the location stays loggable. TCP interleaving because UDP from a camera is
/// dropped by rp_filter-strict hosts and by any NAT in between; `tcp-timeout` bounds the connect
/// and each read so an unplugged camera fails instead of hanging the open.
std::string rtspSource(const StreamEndpoint& endpoint, const RtspSettings& rtsp, bool redact) {
    std::string out = "rtspsrc location=" +
                      gstQuote("rtsp://" + net::toString(endpoint.host) + ":" +
                               std::to_string(endpoint.port) + normalizedPath(endpoint.path));
    if (sendsCredentials(endpoint)) {
        out += " user-id=" + gstQuote(redact ? kRedacted : endpoint.credentials.username);
        out += " user-pw=" + gstQuote(redact ? kRedacted : endpoint.credentials.password);
    }
    const std::int64_t latency_ms = rtsp.latency_ms > 0 ? rtsp.latency_ms : 0;
    const std::int64_t tcp_timeout_us =
        rtsp.timeout_ms > 0 ? static_cast<std::int64_t>(rtsp.timeout_ms) * 1000 : 0;
    out += " protocols=tcp latency=" + std::to_string(latency_ms) +
           " drop-on-latency=true tcp-timeout=" + std::to_string(tcp_timeout_us) +
           " do-rtsp-keep-alive=true";
    return out;
}

/// config-interval=-1 re-sends SPS/PPS with every IDR: without them in-band nvv4l2decoder
/// loops in VIDIOC_DQEVENT and never outputs a frame.
std::string depayAndParse(net::VideoCodec codec) {
    return codec == net::VideoCodec::kH265 ? "rtph265depay ! h265parse config-interval=-1"
                                           : "rtph264depay ! h264parse config-interval=-1";
}

/// ",width=W,height=H" when `decode.max_width` shrinks a stream of known size, else "".
std::string sizeCaps(const StreamEndpoint& endpoint, const DecodeSettings& decode) {
    int width = 0;
    int height = 0;
    scaledSize(endpoint.width, endpoint.height, decode.max_width, width, height);
    if (width <= 0 || height <= 0 || (width == endpoint.width && height == endpoint.height)) {
        return {};
    }
    return ",width=" + std::to_string(width) + ",height=" + std::to_string(height);
}

/// Placed in system memory: videorate on NVMM buffers is untested on R32.7.
std::string rateLimit(const DecodeSettings& decode) {
    return decode.max_fps > 0 ? " ! videorate max-rate=" + std::to_string(decode.max_fps)
                              : std::string();
}

DecoderAttempt gstAttempt(const char* decoder, const char* backend, const StreamEndpoint& endpoint,
                          net::VideoCodec codec, const DecodeSettings& decode,
                          const RtspSettings& rtsp, bool nvidia) {
    const bool i420 = std::string(backend) == kBackendGstNative;
    DecoderAttempt attempt;
    attempt.decoder = decoder;
    attempt.backend = backend;
    if (nvidia) {
        attempt.source = buildNvidiaPipeline(endpoint, codec, decode, rtsp, i420, false);
        attempt.description = buildNvidiaPipeline(endpoint, codec, decode, rtsp, i420, true);
    } else {
        attempt.source = buildSoftwarePipeline(endpoint, codec, decode, rtsp, i420, false);
        attempt.description = buildSoftwarePipeline(endpoint, codec, decode, rtsp, i420, true);
    }
    return attempt;
}

std::string joinReasons(const std::vector<std::string>& reasons) {
    std::string out;
    for (const std::string& reason : reasons) {
        if (!out.empty()) {
            out += "; ";
        }
        out += reason;
    }
    return out;
}

}  // namespace

std::string gstQuote(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');
    for (const char ch : value) {
        if (ch == '"' || ch == '\\') {
            out.push_back('\\');
        }
        out.push_back(ch);
    }
    out.push_back('"');
    return out;
}

void scaledSize(int width, int height, int max_width, int& out_width, int& out_height) {
    out_width = width;
    out_height = height;
    if (max_width <= 0 || width <= 0 || height <= 0 || width <= max_width) {
        return;
    }
    const int scaled_width = max_width / 8 * 8;
    if (scaled_width < 8) {
        return;
    }
    const std::int64_t exact_height =
        static_cast<std::int64_t>(height) * scaled_width / static_cast<std::int64_t>(width);
    const std::int64_t scaled_height = exact_height / 8 * 8;
    out_width = scaled_width;
    out_height = static_cast<int>(scaled_height < 8 ? 8 : scaled_height);
}

std::string buildNvidiaPipeline(const StreamEndpoint& endpoint, net::VideoCodec codec,
                                const DecodeSettings& decode, const RtspSettings& rtsp,
                                bool i420_output, bool redact) {
    if (!supportedCodec(codec)) {
        return {};
    }
    // nvvidconv scales in the VIC engine and writes system memory. It has no BGR output, so the
    // OpenCV path takes BGRx and pays one CPU conversion; the native path takes I420 and converts
    // only the frames that are actually processed.
    std::string out = rtspSource(endpoint, rtsp, redact) + " ! " + depayAndParse(codec) +
                      " ! nvv4l2decoder enable-max-performance=true ! nvvidconv ! ";
    const std::string size = sizeCaps(endpoint, decode);
    if (i420_output) {
        out += "video/x-raw,format=I420" + size + rateLimit(decode);
    } else {
        out += "video/x-raw,format=BGRx" + size + rateLimit(decode) +
               " ! videoconvert ! video/x-raw,format=BGR";
    }
    return out + " ! " + kAppSink;
}

std::string buildSoftwarePipeline(const StreamEndpoint& endpoint, net::VideoCodec codec,
                                  const DecodeSettings& decode, const RtspSettings& rtsp,
                                  bool i420_output, bool redact) {
    if (!supportedCodec(codec)) {
        return {};
    }
    // Drop and shrink frames before the colour conversion: on four Cortex-A57 cores the
    // conversion of full-size frames costs more than the decode itself.
    const std::string size = sizeCaps(endpoint, decode);
    std::string out = rtspSource(endpoint, rtsp, redact) + " ! " + depayAndParse(codec) + " ! " +
                      (codec == net::VideoCodec::kH265 ? "avdec_h265" : "avdec_h264") +
                      rateLimit(decode);
    if (!size.empty()) {
        out += " ! videoscale";
    }
    out += std::string(" ! videoconvert ! video/x-raw,format=") + (i420_output ? "I420" : "BGR") +
           size;
    return out + " ! " + kAppSink;
}

std::vector<DecoderAttempt> planDecoders(const StreamEndpoint& endpoint, net::VideoCodec codec,
                                         const DecoderCapabilities& capabilities,
                                         const DecodeSettings& decode, const RtspSettings& rtsp,
                                         std::string& why_empty) {
    why_empty.clear();
    std::vector<DecoderAttempt> plan;
    if (!supportedCodec(codec)) {
        why_empty = "the stream codec is " + net::toString(codec) +
                    "; only H.264 and H.265 can be decoded";
        return plan;
    }

    std::vector<std::string> reasons;
    const auto add_reason = [&reasons](const std::string& reason) {
        if (std::find(reasons.begin(), reasons.end(), reason) == reasons.end()) {
            reasons.push_back(reason);
        }
    };
    const std::string no_gst_rtsp =
        "the GStreamer RTSP elements (rtspsrc, depayloaders, parsers) are missing";
    const std::string no_gst_backend =
        "native GStreamer capture is not compiled in and OpenCV has no GStreamer backend";

    if (decode.decoder != DecoderPreference::kSoftware) {
        if (!capabilities.nvidia_decoder) {
            add_reason("nvv4l2decoder is not available in this container");
        } else if (!capabilities.gst_rtsp) {
            add_reason(no_gst_rtsp);
        } else if (!capabilities.gst_native && !capabilities.opencv_gstreamer) {
            add_reason(no_gst_backend);
        } else {
            // One hardware attempt: when nvv4l2decoder fails through one backend it fails
            // through the other as well, and every attempt costs a full open timeout.
            DecoderAttempt attempt = gstAttempt(
                kDecoderNvidiaHardware,
                capabilities.gst_native ? kBackendGstNative : kBackendOpenCvGstreamer, endpoint,
                codec, decode, rtsp, true);
            if (!capabilities.nvidia_device) {
                attempt.note =
                    "the NVIDIA GStreamer plugins are present but /dev/nvhost-nvdec is missing "
                    "(container not started with --runtime nvidia?); trying the hardware decoder "
                    "anyway";
            }
            plan.push_back(std::move(attempt));
        }
    }
    if (decode.decoder == DecoderPreference::kNvidiaHardware) {
        if (plan.empty()) {
            why_empty = joinReasons(reasons) +
                        "; decode.decoder is nvidia_hardware, so no software decoder is tried";
        }
        return plan;
    }

    const bool h265 = codec == net::VideoCodec::kH265;
    const bool avdec = h265 ? capabilities.gst_avdec_h265 : capabilities.gst_avdec_h264;
    if (!capabilities.gst_rtsp) {
        add_reason(no_gst_rtsp);
    } else if (!avdec) {
        add_reason(std::string("the GStreamer software decoder ") +
                   (h265 ? "avdec_h265" : "avdec_h264") + " (gst-libav) is missing");
    } else if (!capabilities.gst_native && !capabilities.opencv_gstreamer) {
        add_reason(no_gst_backend);
    } else {
        if (capabilities.gst_native) {
            plan.push_back(gstAttempt(kDecoderSoftwareGstreamer, kBackendGstNative, endpoint,
                                      codec, decode, rtsp, false));
        }
        if (capabilities.opencv_gstreamer) {
            plan.push_back(gstAttempt(kDecoderSoftwareGstreamer, kBackendOpenCvGstreamer,
                                      endpoint, codec, decode, rtsp, false));
        }
    }

    if (capabilities.opencv_ffmpeg) {
        DecoderAttempt attempt;
        attempt.decoder = kDecoderSoftwareFfmpeg;
        attempt.backend = kBackendOpenCvFfmpeg;
        const net::Credentials none;
        attempt.source = net::rtspUrlWithCredentials(
            endpoint.host, endpoint.port, endpoint.path,
            sendsCredentials(endpoint) ? endpoint.credentials : none);
        attempt.description = net::redactedRtspUrl(endpoint.host, endpoint.port, endpoint.path,
                                                   sendsCredentials(endpoint));
        if (decode.max_width > 0 || decode.max_fps > 0) {
            attempt.note = "decode.max_width and decode.max_fps are not applied by the FFmpeg "
                           "backend; every frame is decoded at full size on the CPU";
        }
        plan.push_back(std::move(attempt));
    } else {
        add_reason("OpenCV has no FFmpeg backend");
    }

    if (plan.empty()) {
        why_empty = "no decoder for " + net::toString(codec) + ": " + joinReasons(reasons);
    }
    return plan;
}

}  // namespace anpr::cameras
