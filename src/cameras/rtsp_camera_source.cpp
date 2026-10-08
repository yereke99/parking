#include "anpr/cameras/rtsp_camera_source.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <numeric>
#include <sstream>
#include <system_error>
#include <utility>

#include <opencv2/core/utility.hpp>

#include "anpr/cameras/gst_capture.hpp"
#include "anpr/common/filesystem.hpp"
#include "anpr/common/logging.hpp"

namespace anpr::cameras {
namespace {

std::int64_t monotonicMs() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

/// The same error is logged again after this long while it persists.
constexpr std::int64_t kRepeatReportMs = 60000;

/// Where GStreamer plugins live in the Jetson image (and in x86 development containers). Used
/// when this binary cannot ask GStreamer itself.
const char* const kPluginDirectories[] = {
    "/usr/lib/aarch64-linux-gnu/gstreamer-1.0",
    "/usr/lib/x86_64-linux-gnu/gstreamer-1.0",
    "/usr/lib/gstreamer-1.0",
    "/usr/local/lib/gstreamer-1.0",
};

bool pathExists(const std::string& path) {
    std::error_code error;
    return filesystem::exists(path, error);
}

bool pluginFileExists(const std::string& file_name) {
    for (const char* directory : kPluginDirectories) {
        if (pathExists(std::string(directory) + "/" + file_name)) {
            return true;
        }
    }
    return false;
}

/// True when a "Video I/O" line of cv::getBuildInformation() such as
/// "      GStreamer:                   YES (1.14.5)" says YES for `label` ("GStreamer:").
bool buildInfoSaysYes(const std::string& build_information, const std::string& label) {
    std::istringstream lines(build_information);
    std::string line;
    while (std::getline(lines, line)) {
        const std::size_t begin = line.find_first_not_of(" \t");
        if (begin == std::string::npos || line.compare(begin, label.size(), label) != 0) {
            continue;
        }
        const std::size_t value = line.find_first_not_of(" \t", begin + label.size());
        return value != std::string::npos && line.compare(value, 3, "YES") == 0;
    }
    return false;
}

std::string yesNo(bool value) {
    return value ? "yes" : "no";
}

/// OpenCV's GStreamer backend (4.2 to 4.5) only finds an appsink whose name contains "appsink"
/// or "opencvsink" and otherwise refuses the pipeline ("cannot find appsink in manual
/// pipeline"). The planned descriptions name it "sink" for GstCapture.
std::string forOpenCvGstreamer(std::string description) {
    const std::string native_sink = "appsink name=sink";
    const std::size_t position = description.rfind(native_sink);
    if (position != std::string::npos) {
        const std::size_t end = position + native_sink.size();
        if (end == description.size() || description[end] == ' ') {
            description.replace(position, native_sink.size(), "appsink name=opencvsink");
        }
    }
    return description;
}

bool startsWith(const std::string& text, const std::string& prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
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

/// Adds `camera_id` unless the thread's LogContext already carries it.
void addCameraId(LogFields& fields, const std::string& camera_id) {
    const std::string& context = LogContext::current();
    if (context.find("camera_id=") == std::string::npos) {
        fields.addQuoted("camera_id", camera_id);
    }
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Decoder capabilities
// ---------------------------------------------------------------------------------------------

DecoderCapabilities detectDecoderCapabilities() {
    DecoderCapabilities capabilities;
    capabilities.gst_native = gstreamerCompiledIn();

    const std::string build_information = cv::getBuildInformation();
    capabilities.opencv_gstreamer = buildInfoSaysYes(build_information, "GStreamer:");
    capabilities.opencv_ffmpeg = buildInfoSaysYes(build_information, "FFMPEG:");

    // Ask GStreamer's registry when this binary drives GStreamer itself (a plugin file that
    // failed to load, for example with a mismatched container toolkit, is not in the registry);
    // otherwise the plugin files are the best evidence available.
    const bool ask_registry = capabilities.gst_native;
    const auto element = [ask_registry](const char* factory, const char* plugin_file) {
        return ask_registry ? gstElementAvailable(factory) : pluginFileExists(plugin_file);
    };
    const bool nvv4l2decoder = element("nvv4l2decoder", "libgstnvvideo4linux2.so");
    const bool nvvidconv = element("nvvidconv", "libgstnvvidconv.so");
    capabilities.nvidia_decoder = nvv4l2decoder && nvvidconv;
    capabilities.nvidia_device = pathExists("/dev/nvhost-nvdec");
    capabilities.gst_avdec_h264 = element("avdec_h264", "libgstlibav.so");
    capabilities.gst_avdec_h265 = element("avdec_h265", "libgstlibav.so");

    std::vector<std::string> missing_rtsp;
    const std::pair<const char*, const char*> rtsp_elements[] = {
        {"rtspsrc", "libgstrtsp.so"},
        {"rtph264depay", "libgstrtp.so"},
        {"rtph265depay", "libgstrtp.so"},
        {"h264parse", "libgstvideoparsersbad.so"},
        {"h265parse", "libgstvideoparsersbad.so"},
        {"videoconvert", "libgstvideoconvert.so"},
        {"appsink", "libgstapp.so"},
    };
    for (const auto& entry : rtsp_elements) {
        if (!element(entry.first, entry.second)) {
            missing_rtsp.emplace_back(entry.first);
        }
    }
    capabilities.gst_rtsp = missing_rtsp.empty();

    std::vector<std::string>& notes = capabilities.notes;
    notes.push_back(std::string("native GStreamer capture: ") +
                    (capabilities.gst_native
                         ? "yes"
                         : "no (built without the GStreamer development files, or gst_init "
                           "failed)"));
    notes.push_back("OpenCV videoio backends: GStreamer " + yesNo(capabilities.opencv_gstreamer) +
                    ", FFmpeg " + yesNo(capabilities.opencv_ffmpeg));
    std::string nvidia = "NVIDIA decoder plugins: nvv4l2decoder " + yesNo(nvv4l2decoder) +
                         ", nvvidconv " + yesNo(nvvidconv);
    if (!capabilities.nvidia_decoder) {
        nvidia += " (run the image with --runtime nvidia; if the plugin files exist, GStreamer "
                  "blacklisted them: check gst-inspect-1.0 nvv4l2decoder)";
    }
    notes.push_back(nvidia);
    notes.push_back(std::string("/dev/nvhost-nvdec: ") +
                    (capabilities.nvidia_device
                         ? "present"
                         : "missing (the NVIDIA container runtime did not mount the decoder)"));
    notes.push_back("GStreamer software decoders: avdec_h264 " +
                    yesNo(capabilities.gst_avdec_h264) + ", avdec_h265 " +
                    yesNo(capabilities.gst_avdec_h265));
    notes.push_back("GStreamer RTSP elements: " +
                    (capabilities.gst_rtsp ? std::string("complete")
                                           : "missing " + join(missing_rtsp, ", ")));
    return capabilities;
}

// ---------------------------------------------------------------------------------------------
// StreamDecoder
// ---------------------------------------------------------------------------------------------

void GstCaptureDeleter::operator()(GstCapture* capture) const {
#ifdef KZ_ANPR_WITH_GSTREAMER
    delete capture;
#else
    // Never constructed without GStreamer support; nothing to release.
    (void)capture;
#endif
}

#ifndef KZ_ANPR_WITH_GSTREAMER
bool gstreamerCompiledIn() {
    return false;
}

bool gstElementAvailable(const std::string& factory_name) {
    (void)factory_name;
    return false;
}
#endif

std::string redactSecrets(std::string text) {
    for (const char* key : {"user-pw=", "user-id=", "proxy-pw=", "proxy-id="}) {
        const std::size_t key_length = std::strlen(key);
        std::size_t position = text.find(key);
        while (position != std::string::npos) {
            const std::size_t value_begin = position + key_length;
            std::size_t value_end = value_begin;
            const bool quoted = value_begin < text.size() &&
                                (text[value_begin] == '"' || text[value_begin] == '\'');
            if (quoted) {
                const char quote = text[value_begin];
                std::size_t i = value_begin + 1;
                while (i < text.size() && text[i] != quote) {
                    i += text[i] == '\\' && i + 1 < text.size() ? 2 : 1;
                }
                value_end = std::min(text.size(), i + 1);
            } else {
                value_end = text.find_first_of(" \t\r\n!,;)", value_begin);
                if (value_end == std::string::npos) {
                    value_end = text.size();
                }
            }
            text.replace(value_begin, value_end - value_begin, "***");
            position = text.find(key, value_begin + 3);
        }
    }

    const std::string redacted = "<redacted>";
    std::size_t scheme = text.find("://");
    while (scheme != std::string::npos) {
        const std::size_t authority = scheme + 3;
        std::size_t token_end = text.find_first_of(" \t\r\n\"'<>", authority);
        if (token_end == std::string::npos) {
            token_end = text.size();
        }
        // The last '@' of the token: an unencoded '@' in a password must not leak its tail.
        const std::size_t at =
            token_end > authority ? text.rfind('@', token_end - 1) : std::string::npos;
        if (at != std::string::npos && at >= authority) {
            text.replace(authority, at - authority, redacted);
            token_end = authority + redacted.size() + (token_end - at);
        }
        scheme = text.find("://", std::min(text.size(), token_end));
    }
    return text;
}

void pictureSize(const cv::Mat& image, PixelFormat format, int& width, int& height) {
    width = image.cols;
    height = format == PixelFormat::kI420 ? image.rows * 2 / 3 : image.rows;
}

StreamDecoder::StreamDecoder() = default;

StreamDecoder::~StreamDecoder() {
    close();
}

StreamDecoder::Status StreamDecoder::open(const DecoderAttempt& attempt,
                                          std::int64_t first_frame_timeout_ms,
                                          std::string& error) {
    close();
    error.clear();
    if (interrupted_.load()) {
        error = "interrupted";
        return Status::kError;
    }
    backend_ = attempt.backend;

    if (attempt.backend == kBackendGstNative) {
#ifdef KZ_ANPR_WITH_GSTREAMER
        gst_.reset(new GstCapture());
        gst_->setInterruptFlag(&interrupted_);
        const int timeout_ms = static_cast<int>(
            std::min<std::int64_t>(std::max<std::int64_t>(0, first_frame_timeout_ms), 3600000));
        const GstCapture::Status status = gst_->open(attempt.source, timeout_ms, error);
        if (status != GstCapture::Status::kFrame) {
            close();
            switch (status) {
                case GstCapture::Status::kTimeout:
                    return Status::kTimeout;
                case GstCapture::Status::kEndOfStream:
                    return Status::kEndOfStream;
                default:
                    return Status::kError;
            }
        }
        // Take the first frame now: its size is part of the open result.
        bool i420 = false;
        if (gst_->pull(pending_, i420, 0, error) != GstCapture::Status::kFrame) {
            close();
            return Status::kError;
        }
        pending_format_ = i420 ? PixelFormat::kI420 : PixelFormat::kBgr;
#else
        (void)first_frame_timeout_ms;
        error = "native GStreamer capture is not built into this binary";
        close();
        return Status::kError;
#endif
    } else if (attempt.backend == kBackendOpenCvGstreamer ||
               attempt.backend == kBackendOpenCvFfmpeg) {
        const bool gstreamer = attempt.backend == kBackendOpenCvGstreamer;
        const char* backend_name = gstreamer ? "GStreamer" : "FFmpeg";
        bool opened = false;
        try {
            opened = gstreamer
                         ? capture_.open(forOpenCvGstreamer(attempt.source), cv::CAP_GSTREAMER)
                         : capture_.open(attempt.source, cv::CAP_FFMPEG);
        } catch (const cv::Exception& exception) {
            error = redactSecrets(std::string("OpenCV ") + backend_name + " backend: " +
                                  exception.what());
            close();
            return Status::kError;
        }
        if (!opened || !capture_.isOpened()) {
            error = std::string("OpenCV's ") + backend_name +
                    " backend could not open the stream (its own warning on stderr has the "
                    "reason)";
            close();
            return Status::kError;
        }
        // OpenCV 4.5 cannot bound this wait; the native capture exists for that reason.
        bool read_ok = false;
        try {
            read_ok = capture_.read(pending_) && !pending_.empty();
        } catch (const cv::Exception& exception) {
            error = redactSecrets(std::string("OpenCV ") + backend_name + " backend: " +
                                  exception.what());
            close();
            return Status::kError;
        }
        if (!read_ok) {
            error = std::string("the stream opened with OpenCV's ") + backend_name +
                    " backend but delivered no frame";
            close();
            return Status::kEndOfStream;
        }
        pending_format_ = PixelFormat::kBgr;
    } else {
        error = "unknown decoder backend " + attempt.backend;
        close();
        return Status::kError;
    }

    has_pending_ = true;
    pictureSize(pending_, pending_format_, width_, height_);
    return Status::kFrame;
}

StreamDecoder::Status StreamDecoder::read(cv::Mat& image, PixelFormat& format,
                                          std::int64_t timeout_ms, std::string& error) {
    error.clear();
    if (has_pending_) {
        std::swap(image, pending_);
        pending_.release();
        format = pending_format_;
        has_pending_ = false;
        return Status::kFrame;
    }
#ifdef KZ_ANPR_WITH_GSTREAMER
    if (gst_ != nullptr) {
        bool i420 = false;
        const int bounded_ms = static_cast<int>(
            std::min<std::int64_t>(std::max<std::int64_t>(0, timeout_ms), 3600000));
        switch (gst_->pull(image, i420, bounded_ms, error)) {
            case GstCapture::Status::kFrame:
                format = i420 ? PixelFormat::kI420 : PixelFormat::kBgr;
                return Status::kFrame;
            case GstCapture::Status::kTimeout:
                return Status::kTimeout;
            case GstCapture::Status::kEndOfStream:
                return Status::kEndOfStream;
            case GstCapture::Status::kError:
                return Status::kError;
        }
        return Status::kError;
    }
#else
    (void)timeout_ms;
#endif
    if (!capture_.isOpened()) {
        error = "decoder is not open";
        return Status::kError;
    }
    bool read_ok = false;
    try {
        read_ok = capture_.read(image) && !image.empty();
    } catch (const cv::Exception& exception) {
        error = redactSecrets(std::string("OpenCV: ") + exception.what());
        return Status::kError;
    }
    if (!read_ok) {
        error = "read() returned no frame (the stream closed or broke)";
        return Status::kEndOfStream;
    }
    format = PixelFormat::kBgr;
    return Status::kFrame;
}

void StreamDecoder::close() {
#ifdef KZ_ANPR_WITH_GSTREAMER
    if (gst_ != nullptr) {
        gst_->close();
    }
#endif
    gst_.reset();
    if (capture_.isOpened()) {
        capture_.release();
    }
    pending_.release();
    has_pending_ = false;
    backend_.clear();
}

void StreamDecoder::interrupt() {
    interrupted_.store(true);
}

bool StreamDecoder::isOpen() const {
#ifdef KZ_ANPR_WITH_GSTREAMER
    if (gst_ != nullptr) {
        return gst_->isOpen();
    }
#endif
    return capture_.isOpened();
}

double StreamDecoder::streamFps() const {
#ifdef KZ_ANPR_WITH_GSTREAMER
    if (gst_ != nullptr) {
        return gst_->capsFps();
    }
#endif
    if (capture_.isOpened()) {
        const double fps = capture_.get(cv::CAP_PROP_FPS);
        // Containers report 0, 1000 or 90000 when they do not know.
        return fps > 0.0 && fps < 500.0 ? fps : 0.0;
    }
    return 0.0;
}

// ---------------------------------------------------------------------------------------------
// RtspCameraSource
// ---------------------------------------------------------------------------------------------

namespace {

ReconnectSettings reconnectSettings(const CaptureSettings& capture) {
    ReconnectSettings settings;
    settings.initial_backoff_ms = capture.reconnect_initial_backoff_ms;
    settings.max_backoff_ms = capture.reconnect_max_backoff_ms;
    settings.auth_retry_interval_ms = capture.auth_retry_interval_ms;
    settings.auth_max_retries = capture.auth_max_retries;
    settings.configuration_retry_interval_ms = capture.configuration_retry_interval_ms;
    return settings;
}

bool decodableCodec(net::VideoCodec codec) {
    return codec == net::VideoCodec::kH264 || codec == net::VideoCodec::kH265 ||
           codec == net::VideoCodec::kUnknown;
}

}  // namespace

RtspCameraSource::RtspCameraSource(CameraTarget target, CameraModeConfig config,
                                   DecoderCapabilities capabilities, net::NetworkSnapshot snapshot,
                                   net::CameraLanSelection lan)
    : target_(std::move(target)),
      config_(std::move(config)),
      capabilities_(std::move(capabilities)),
      snapshot_(std::move(snapshot)),
      lan_(std::move(lan)),
      policy_(reconnectSettings(config_.capture)) {}

RtspCameraSource::~RtspCameraSource() {
    close();
}

bool RtspCameraSource::open() {
    close();
    if (given_up_.load() || interrupted_.load()) {
        return false;
    }
    open_started_ms_ = monotonicMs();
    {
        const std::lock_guard<std::mutex> guard(status_mutex_);
        ++status_.opens;
    }

    // One DESCRIBE with the credentials first: it classifies an unreachable camera, a closed
    // port, a rejected login or a wrong path before any decoder is involved, and it is the only
    // login a failed open costs (the decoder's own RTSP session is never tried after a 401).
    const RtspCheck check = checkRtsp(target_, snapshot_, lan_, config_.rtsp.timeout_ms);
    if (interrupted_.load()) {
        return false;
    }
    {
        const std::lock_guard<std::mutex> guard(status_mutex_);
        status_.reachable = check.network_reachable;
        status_.rtsp_ok = check.error == CameraError::kNone;
        if (check.video.present) {
            status_.codec = check.video.codec;
        }
    }
    if (check.error != CameraError::kNone) {
        fail(check.error, check.detail, Stage::kRtsp);
        return false;
    }

    const net::VideoCodec codec =
        check.video.present ? check.video.codec : net::VideoCodec::kUnknown;
    StreamEndpoint endpoint;
    endpoint.host = target_.ip;
    endpoint.port = target_.rtsp_port;
    endpoint.path = target_.rtsp_path;
    endpoint.credentials = target_.credentials;
    endpoint.has_credentials = target_.has_credentials;
    // decode.max_width needs the coded size: from the SDP, else from an earlier decoded frame.
    endpoint.width = check.video.width > 0 ? check.video.width : coded_width_;
    endpoint.height = check.video.width > 0 ? check.video.height : coded_height_;
    DecodeSettings decode = config_.decode;
    decode.decoder = target_.decoder;

    std::vector<std::string> failures;
    const DecoderAttempt* used = nullptr;
    for (int pass = 0; pass < 2; ++pass) {
        std::string why_empty;
        plan_ = planDecoders(endpoint, codec, capabilities_, decode, config_.rtsp, why_empty);
        if (plan_.empty()) {
            fail(decodableCodec(codec) ? CameraError::kStreamOpenFailed
                                       : CameraError::kUnsupportedCodec,
                 why_empty.empty() ? "no decoder can open this stream" : why_empty,
                 Stage::kDecode);
            return false;
        }
        bool opened_without_frames = false;
        used = openFirstWorking(failures, opened_without_frames);
        if (interrupted_.load()) {
            decoder_.close();
            return false;
        }
        if (used == nullptr) {
            fail(opened_without_frames ? CameraError::kNoFramesReceived
                                       : CameraError::kStreamOpenFailed,
                 join(failures, "; "), opened_without_frames ? Stage::kFrames : Stage::kDecode);
            return false;
        }
        if (endpoint.width > 0 || decoder_.width() <= 0) {
            break;
        }
        // The SDP had no picture size, so nothing was scaled. Now the size is known; a stream
        // wider than decode.max_width is reopened once so the converter scales it from the
        // first processed frame on, not only after the next reconnect.
        coded_width_ = decoder_.width();
        coded_height_ = decoder_.height();
        if (pass > 0 || config_.decode.max_width <= 0 || coded_width_ <= config_.decode.max_width) {
            break;
        }
        endpoint.width = coded_width_;
        endpoint.height = coded_height_;
        decoder_.close();
        failures.clear();
    }

    working_decoder_ = used->decoder;
    working_backend_ = used->backend;
    policy_.onSuccess();
    delay_override_ms_.store(0);
    sequence_ = 0;
    const double first_frame_ms = static_cast<double>(monotonicMs() - open_started_ms_);
    const double stream_fps =
        decoder_.streamFps() > 0.0 ? decoder_.streamFps() : check.video.framerate;
    {
        const std::lock_guard<std::mutex> guard(status_mutex_);
        status_.error = CameraError::kNone;
        status_.detail =
            failures.empty() ? std::string() : "decoder fallback: " + join(failures, "; ");
        status_.decoder = used->decoder;
        status_.codec = codec;
        status_.width = decoder_.width();
        status_.height = decoder_.height();
        status_.stream_fps = stream_fps;
        status_.open = true;
        status_.rtsp_ok = true;
        status_.reachable = true;
        status_.first_frame_ms = first_frame_ms;
    }
    // The next failure is news again, whatever it is.
    last_reported_ = CameraError::kNone;

    LogFields fields;
    addCameraId(fields, target_.id);
    fields.add("ip", net::toString(target_.ip))
        .add("decoder", used->decoder)
        .add("backend", used->backend)
        .add("codec", net::shortName(codec))
        .add("width", decoder_.width())
        .add("height", decoder_.height())
        .add("first_frame_ms", static_cast<std::int64_t>(first_frame_ms));
    if (stream_fps > 0.0) {
        fields.add("stream_fps", stream_fps);
    }
    if (!used->note.empty()) {
        fields.addQuoted("note", used->note);
    }
    logEvent(LogLevel::kInfo, "camera_stream_opened", fields);
    reportHardwareFallback(failures);
    return true;
}

const DecoderAttempt* RtspCameraSource::openFirstWorking(std::vector<std::string>& failures,
                                                         bool& opened_without_frames) {
    // The attempt that worked last goes first: a reconnect does not pay again for the
    // hardware attempt that failed in this container.
    std::vector<std::size_t> order(plan_.size());
    std::iota(order.begin(), order.end(), 0);
    if (!working_decoder_.empty()) {
        std::stable_partition(order.begin(), order.end(), [this](std::size_t index) {
            return plan_[index].decoder == working_decoder_ &&
                   plan_[index].backend == working_backend_;
        });
    }
    for (const std::size_t index : order) {
        const DecoderAttempt& attempt = plan_[index];
        std::string error;
        const StreamDecoder::Status result =
            decoder_.open(attempt, config_.capture.first_frame_timeout_ms, error);
        if (result == StreamDecoder::Status::kFrame) {
            return &attempt;
        }
        if (result == StreamDecoder::Status::kTimeout ||
            result == StreamDecoder::Status::kEndOfStream) {
            opened_without_frames = true;
        }
        failures.push_back(attempt.decoder + ": " + error +
                           (attempt.note.empty() ? std::string() : " (" + attempt.note + ")"));
        LogFields fields;
        addCameraId(fields, target_.id);
        logEvent(LogLevel::kDebug, "camera_decoder_failed",
                 fields.add("decoder", attempt.decoder)
                     .add("backend", attempt.backend)
                     .addQuoted("pipeline", attempt.description)
                     .addQuoted("error", error));
    }
    return nullptr;
}

void RtspCameraSource::reportHardwareFallback(const std::vector<std::string>& failures) {
    if (hardware_fallback_reported_ || target_.decoder != DecoderPreference::kAuto) {
        return;
    }
    const std::string& decoder = working_decoder_;
    if (decoder == kDecoderNvidiaHardware) {
        return;
    }
    std::string reason;
    for (const std::string& failure : failures) {
        if (startsWith(failure, std::string(kDecoderNvidiaHardware) + ":")) {
            reason = failure;
            break;
        }
    }
    if (reason.empty()) {
        if (!capabilities_.nvidia_decoder) {
            reason = "nvv4l2decoder/nvvidconv plugins are not available in this container";
        } else if (!capabilities_.nvidia_device) {
            reason = "/dev/nvhost-nvdec is missing in this container";
        } else {
            return;
        }
    }
    hardware_fallback_reported_ = true;
    DiagnosticContext context;
    context.camera_id = target_.id;
    context.ip = net::toString(target_.ip);
    context.stage = Stage::kDecode;
    reportCameraError(CameraError::kHardwareDecoderUnavailable, context,
                      reason + "; decoding with " + decoder);
}

void RtspCameraSource::close() {
    decoder_.close();
    const std::lock_guard<std::mutex> guard(status_mutex_);
    status_.open = false;
    status_.decoder.clear();
}

bool RtspCameraSource::isOpen() const {
    return decoder_.isOpen();
}

ReadStatus RtspCameraSource::read(Frame& frame) {
    if (!decoder_.isOpen()) {
        return ReadStatus::kFailed;
    }
    const auto started = std::chrono::steady_clock::now();
    std::string error;
    PixelFormat format = PixelFormat::kBgr;
    const StreamDecoder::Status result =
        decoder_.read(frame.image, format, config_.capture.read_timeout_ms, error);
    if (result == StreamDecoder::Status::kFrame) {
        frame.format = format;
        frame.capture_ms = monotonicMs();
        frame.stream_ms = frame.capture_ms;
        frame.sequence = ++sequence_;
        frame.decode_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
                .count();
        return ReadStatus::kOk;
    }

    if (interrupted_.load()) {
        decoder_.close();
        return ReadStatus::kFailed;
    }
    // A camera that went silent (PoE cable pulled: no FIN, no RST) surfaces here as a timeout
    // instead of a read that blocks for minutes; the capture thread reconnects.
    const bool timeout = result == StreamDecoder::Status::kTimeout;
    const CameraError code = timeout ? CameraError::kStreamTimeout : CameraError::kStreamEnded;
    const std::string detail =
        timeout ? "no frame for " + std::to_string(config_.capture.read_timeout_ms) + " ms"
                : (error.empty() ? std::string("end of stream") : error);
    decoder_.close();
    policy_.onFailure(code);
    {
        const std::lock_guard<std::mutex> guard(status_mutex_);
        status_.error = code;
        status_.detail = detail;
        status_.open = false;
        status_.decoder.clear();
        ++status_.failures;
    }
    report(code, detail, Stage::kFrames);
    return ReadStatus::kFailed;
}

void RtspCameraSource::interrupt() {
    interrupted_.store(true);
    decoder_.interrupt();
}

std::string RtspCameraSource::describe() const {
    return target_.redactedUrl();
}

std::int64_t RtspCameraSource::reconnectDelayOverrideMs() const {
    return delay_override_ms_.load();
}

bool RtspCameraSource::permanentlyFailed() const {
    return given_up_.load();
}

RtspCameraSource::Status RtspCameraSource::status() const {
    const std::lock_guard<std::mutex> guard(status_mutex_);
    return status_;
}

void RtspCameraSource::fail(CameraError error, const std::string& detail, Stage stage) {
    decoder_.close();
    const std::optional<std::int64_t> delay = policy_.onFailure(error);
    if (!delay) {
        given_up_.store(true);
        delay_override_ms_.store(0);
    } else if (classifyFailure(error) == FailureClass::kTransient) {
        // Network and stream failures use the capture thread's own exponential backoff.
        delay_override_ms_.store(0);
    } else {
        delay_override_ms_.store(*delay);
    }
    {
        const std::lock_guard<std::mutex> guard(status_mutex_);
        status_.error = error;
        status_.detail = detail;
        status_.open = false;
        status_.decoder.clear();
        ++status_.failures;
    }
    report(error, detail, stage);

    if (given_up_.load()) {
        LogFields fields;
        addCameraId(fields, target_.id);
        logEvent(LogLevel::kError, "camera_given_up",
                 fields.add("ip", net::toString(target_.ip))
                     .add("error", toString(error))
                     .add("attempts", policy_.consecutiveFailures())
                     .addQuoted("action",
                                "fix the credentials, then restart the service; the camera is "
                                "not contacted again until then"));
    } else if (delay_override_ms_.load() > 0) {
        LogFields fields;
        addCameraId(fields, target_.id);
        logEvent(LogLevel::kInfo, "camera_retry_scheduled",
                 fields.add("error", toString(error))
                     .add("retry_in_ms", delay_override_ms_.load()));
    }
}

void RtspCameraSource::report(CameraError error, const std::string& detail, Stage stage) {
    const std::int64_t now = monotonicMs();
    if (error == last_reported_ && now - last_reported_ms_ < kRepeatReportMs) {
        return;
    }
    last_reported_ = error;
    last_reported_ms_ = now;
    DiagnosticContext context;
    context.camera_id = target_.id;
    context.ip = net::toString(target_.ip);
    context.stage = stage;
    reportCameraError(error, context, detail);
}

}  // namespace anpr::cameras
