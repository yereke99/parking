#include "anpr/cameras/stream_check.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <sstream>

#include "anpr/cameras/rtsp_camera_source.hpp"

namespace anpr::cameras {
namespace {

std::int64_t monotonicMs() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

bool decodableCodec(net::VideoCodec codec) {
    return codec == net::VideoCodec::kH264 || codec == net::VideoCodec::kH265 ||
           codec == net::VideoCodec::kUnknown;
}

std::string join(const std::vector<std::string>& parts, const std::string& separator) {
    std::string out;
    for (const std::string& part : parts) {
        if (!out.empty()) {
            out += separator;
        }
        out += part;
    }
    return out;
}

std::string orDash(const std::string& value) {
    return value.empty() ? "-" : value;
}

std::string fixed(double value, int decimals) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.*f", decimals, value);
    return text;
}

/// Prefixes every line of `block` with `indent`.
std::string indentLines(const std::string& block, const std::string& indent) {
    std::istringstream lines(block);
    std::string line;
    std::string out;
    while (std::getline(lines, line)) {
        out += indent + line + "\n";
    }
    return out;
}

}  // namespace

std::string rtspStatusLabel(CameraError error, bool rtsp_ok) {
    switch (error) {
        case CameraError::kRtspAuthFailed:
            return "AUTH";
        case CameraError::kRtspCredentialsMissing:
            return "NOCRED";
        case CameraError::kRtspPortClosed:
            return "PORT";
        case CameraError::kRtspStreamPathInvalid:
            return "PATH";
        case CameraError::kCameraUnreachable:
        case CameraError::kCameraLanLinkDown:
            return "DOWN";
        case CameraError::kRtspProtocolError:
            return "ERROR";
        case CameraError::kCameraOnOtherSubnet:
        case CameraError::kDuplicateIpDetected:
        case CameraError::kCameraNotActivated:
        case CameraError::kCameraDisabled:
        case CameraError::kCameraLimitReached:
            return "-";
        default:
            return rtsp_ok ? "OK" : "-";
    }
}

StreamCheckResult checkStream(const CameraTarget& target, const CameraModeConfig& config,
                              const DecoderCapabilities& capabilities,
                              const net::NetworkSnapshot& snapshot,
                              const net::CameraLanSelection& lan, std::int64_t duration_ms) {
    StreamCheckResult result;
    result.rtsp = checkRtsp(target, snapshot, lan, config.rtsp.timeout_ms);
    if (result.rtsp.error != CameraError::kNone) {
        result.error = result.rtsp.error;
        result.detail = result.rtsp.detail;
        return result;
    }
    const net::SdpVideo& video = result.rtsp.video;
    result.codec = video.present ? video.codec : net::VideoCodec::kUnknown;
    result.nominal_fps = video.framerate;
    result.width = video.width;
    result.height = video.height;

    StreamEndpoint endpoint;
    endpoint.host = target.ip;
    endpoint.port = target.rtsp_port;
    endpoint.path = target.rtsp_path;
    endpoint.credentials = target.credentials;
    endpoint.has_credentials = target.has_credentials;
    endpoint.width = video.width;
    endpoint.height = video.height;
    DecodeSettings decode = config.decode;
    decode.decoder = target.decoder;
    std::string why_empty;
    const std::vector<DecoderAttempt> plan =
        planDecoders(endpoint, result.codec, capabilities, decode, config.rtsp, why_empty);
    if (plan.empty()) {
        result.error = decodableCodec(result.codec) ? CameraError::kStreamOpenFailed
                                                    : CameraError::kUnsupportedCodec;
        result.detail = why_empty.empty() ? "no decoder can open this stream" : why_empty;
        return result;
    }

    bool opened_without_frames = false;
    for (const DecoderAttempt& attempt : plan) {
        StreamDecoder decoder;
        const std::int64_t attempt_started = monotonicMs();
        std::string error;
        const StreamDecoder::Status opened =
            decoder.open(attempt, config.capture.first_frame_timeout_ms, error);
        if (opened != StreamDecoder::Status::kFrame) {
            if (opened == StreamDecoder::Status::kTimeout ||
                opened == StreamDecoder::Status::kEndOfStream) {
                opened_without_frames = true;
            }
            result.fallbacks.push_back(
                attempt.decoder + ": " + error +
                (attempt.note.empty() ? std::string() : " (" + attempt.note + ")"));
            continue;
        }

        // The first frame is waiting in the decoder: read it and then everything that arrives
        // within the window that starts with it.
        cv::Mat image;
        PixelFormat format = PixelFormat::kBgr;
        std::int64_t first_frame_at = 0;
        std::int64_t last_frame_at = 0;
        std::int64_t deadline = 0;
        while (true) {
            const std::int64_t now = monotonicMs();
            const std::int64_t timeout =
                result.frames == 0
                    ? config.capture.read_timeout_ms
                    : std::min<std::int64_t>(config.capture.read_timeout_ms,
                                             std::max<std::int64_t>(1, deadline - now));
            if (result.frames > 0 && now >= deadline) {
                break;
            }
            const StreamDecoder::Status status = decoder.read(image, format, timeout, error);
            if (status != StreamDecoder::Status::kFrame) {
                break;
            }
            const std::int64_t arrived = monotonicMs();
            if (result.frames == 0) {
                first_frame_at = arrived;
                deadline = arrived + std::max<std::int64_t>(0, duration_ms);
                result.first_frame_ms = static_cast<double>(arrived - attempt_started);
                pictureSize(image, format, result.width, result.height);
            }
            last_frame_at = arrived;
            ++result.frames;
        }
        if (result.frames >= 2 && last_frame_at > first_frame_at) {
            result.measured_fps = static_cast<double>(result.frames - 1) * 1000.0 /
                                  static_cast<double>(last_frame_at - first_frame_at);
        }
        if (decoder.streamFps() > 0.0) {
            result.nominal_fps = decoder.streamFps();
        }
        result.decoder = attempt.decoder;
        if (!attempt.note.empty()) {
            result.notes.push_back(attempt.note);
        }
        if (decode.max_width > 0 && endpoint.width == 0 && result.width > decode.max_width) {
            result.notes.push_back("the SDP has no picture size, so decode.max_width is applied "
                                   "only from the stream's first frame on (one extra RTSP "
                                   "session when the camera connects)");
        }
        decoder.close();
        break;
    }

    if (result.frames == 0) {
        result.error = opened_without_frames ? CameraError::kNoFramesReceived
                                             : CameraError::kStreamOpenFailed;
        result.detail = join(result.fallbacks, "; ");
    }
    return result;
}

std::string formatStreamCheck(const CameraTarget& target, const StreamCheckResult& result) {
    std::ostringstream out;
    out << "CAMERA " << target.id << '\n';
    out << "  IP: " << net::toString(target.ip) << "   MAC: " << orDash(target.mac)
        << "   Vendor: " << orDash(target.vendor) << "   Model: " << orDash(target.model) << '\n';
    out << "  RTSP: " << rtspStatusLabel(result.rtsp.error, result.rtsp.error == CameraError::kNone)
        << "   Stream: " << toString(target.stream) << "   URL: " << target.redactedUrl() << '\n';

    if (result.rtsp.error == CameraError::kNone) {
        const double fps = result.measured_fps > 0.0 ? result.measured_fps : result.nominal_fps;
        out << "  Codec: " << net::toString(result.codec) << "   Resolution: "
            << (result.width > 0 && result.height > 0
                    ? std::to_string(result.width) + "x" + std::to_string(result.height)
                    : std::string("-"))
            << "   FPS: " << (fps > 0.0 ? fixed(fps, 1) : std::string("-"))
            << "   First frame: "
            << (result.frames > 0
                    ? std::to_string(static_cast<long long>(result.first_frame_ms + 0.5)) + " ms"
                    : std::string("-"))
            << '\n';
        out << "  Decoder: " << (result.decoder.empty() ? std::string("none") : result.decoder)
            << '\n';
        if (!result.fallbacks.empty()) {
            out << "  Fallbacks: " << join(result.fallbacks, "; ") << '\n';
        }
        for (const std::string& note : result.notes) {
            out << "  Note: " << note << '\n';
        }
    }

    if (!result.ready()) {
        DiagnosticContext context;
        context.camera_id = target.id;
        context.ip = net::toString(target.ip);
        context.stage = result.rtsp.error != CameraError::kNone ? Stage::kRtsp : Stage::kDecode;
        const CameraError error =
            result.error != CameraError::kNone ? result.error : CameraError::kNoFramesReceived;
        // The fallback list already carries the decoder errors; repeating it would double the
        // block for a stream that no decoder could open.
        const std::string detail =
            result.rtsp.error == CameraError::kNone && !result.fallbacks.empty() ? std::string()
                                                                                 : result.detail;
        out << indentLines(formatError(error, context, detail), "  ");
    }
    out << "  STATUS: " << (result.ready() ? "READY" : "NOT READY") << '\n';
    return out.str();
}

}  // namespace anpr::cameras
