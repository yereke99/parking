#include "anpr/camera/camera_source.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <thread>

#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>

#include "anpr/camera/video_orientation.hpp"
#include "anpr/common/filesystem.hpp"
#include "anpr/common/logging.hpp"

namespace anpr {
namespace {

std::int64_t monotonicMs() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

/// cv::CAP_PROP_ORIENTATION_AUTO, by number: the enum value does not exist in older OpenCV
/// releases, and setting an unknown property is a harmless no-op there.
constexpr int kCapPropOrientationAuto = 49;

bool looksLikeDeviceIndex(const std::string& source) {
    return !source.empty() &&
           std::all_of(source.begin(), source.end(),
                       [](unsigned char ch) { return std::isdigit(ch) != 0; });
}

bool looksLikeRtsp(const std::string& source) {
    return source.rfind("rtsp://", 0) == 0 || source.rfind("rtsps://", 0) == 0;
}

bool looksLikeGStreamerPipeline(const std::string& source) {
    // A pipeline description always ends in a sink and contains the element separator.
    return source.find(" ! ") != std::string::npos;
}

/// One OpenCV VideoCapture, opened through whichever backend suits the source.
class OpenCvCameraSource final : public CameraSource {
public:
    OpenCvCameraSource(CameraConfig config, CameraKind kind)
        : config_(std::move(config)), kind_(kind) {}

    ~OpenCvCameraSource() override { close(); }

    bool open() override {
        close();
        bool opened = false;
        switch (kind_) {
            case CameraKind::kDevice:
                opened = capture_.open(std::atoi(config_.source.c_str()));
                break;
            case CameraKind::kGStreamer:
                opened = capture_.open(config_.source, cv::CAP_GSTREAMER);
                break;
            case CameraKind::kRtsp:
                opened = openRtsp();
                break;
            case CameraKind::kFile:
            case CameraKind::kAuto:
            default:
                opened = capture_.open(config_.source);
                break;
        }
        if (!opened) {
            return false;
        }

        if (kind_ == CameraKind::kDevice) {
            capture_.set(cv::CAP_PROP_FRAME_WIDTH, config_.width);
            capture_.set(cv::CAP_PROP_FRAME_HEIGHT, config_.height);
            capture_.set(cv::CAP_PROP_FPS, config_.fps);
        }
        if (kind_ != CameraKind::kFile) {
            // Keep the driver queue at one frame so a slow tick discards stale frames instead of
            // handing the pipeline an old view of the barrier.
            capture_.set(cv::CAP_PROP_BUFFERSIZE, std::max(1, config_.capture_buffer_size));
        }
        rotation_ = 0;
        if (kind_ == CameraKind::kFile) {
            // Phone clips are landscape pictures plus a display rotation. Whether OpenCV applies
            // it depends on its version, so it is switched off where it exists and applied here,
            // the same way on the Jetson and on a development machine.
            rotation_ = mp4DisplayRotation(config_.source);
            if (rotation_ != 0) {
                capture_.set(kCapPropOrientationAuto, 0);
                logEvent(LogLevel::kInfo, "video_rotation",
                         LogFields().add("degrees", rotation_).add("source", describe()));
            }
        }
        sequence_ = 0;
        const double reported_fps = capture_.get(cv::CAP_PROP_FPS);
        frame_interval_fps_ = reported_fps > 1.0 ? reported_fps : std::max(1.0, config_.fps);
        playback_started_ms_ = monotonicMs();
        return true;
    }

    void close() override {
        if (capture_.isOpened()) {
            capture_.release();
        }
    }

    [[nodiscard]] bool isOpen() const override { return capture_.isOpened(); }

    ReadStatus read(Frame& frame) override {
        if (!capture_.isOpened()) {
            return ReadStatus::kFailed;
        }
        // `read` writes into the existing buffer when the geometry is unchanged, so a steady
        // stream performs no per-frame allocation.
        const auto decode_started = std::chrono::steady_clock::now();
        // A rotated clip decodes into its own buffer and is turned into the frame's, so both keep
        // a fixed geometry and nothing is reallocated per frame.
        cv::Mat& decoded = rotation_ != 0 ? decoded_ : frame.image;
        if (!capture_.read(decoded) || decoded.empty()) {
            if (kind_ == CameraKind::kFile) {
                if (config_.loop_file && capture_.set(cv::CAP_PROP_POS_FRAMES, 0)) {
                    return ReadStatus::kEmpty;
                }
                return ReadStatus::kEndOfStream;
            }
            return ReadStatus::kFailed;
        }

        if (rotation_ == 90) {
            cv::rotate(decoded_, frame.image, cv::ROTATE_90_CLOCKWISE);
        } else if (rotation_ == 180) {
            cv::rotate(decoded_, frame.image, cv::ROTATE_180);
        } else if (rotation_ == 270) {
            cv::rotate(decoded_, frame.image, cv::ROTATE_90_COUNTERCLOCKWISE);
        }
        frame.decode_ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - decode_started)
                              .count();

        // VideoCapture always delivers BGR; the field travels with recycled frame buffers.
        frame.format = PixelFormat::kBgr;
        frame.capture_ms = monotonicMs();
        frame.sequence = ++sequence_;
        // A file gets a timeline derived from its own frame rate. It must never be mixed with
        // the wall clock: the state machine compares timestamps, and one wall-clock value among
        // stream-relative ones puts every later frame in the past. The index-based timeline is
        // also monotonic across a loop and reproducible between benchmark runs.
        frame.stream_ms =
            kind_ == CameraKind::kFile
                ? static_cast<std::int64_t>((frame.sequence - 1) * 1000.0 / frame_interval_fps_)
                : frame.capture_ms;

        if (kind_ == CameraKind::kFile && config_.realtime_file) {
            // Hold the frame until its place in the clip's own timeline. A file otherwise
            // decodes far faster than the pipeline processes, the latest-frame slot discards
            // almost everything, and the live path looks broken when it is working correctly.
            const std::int64_t due_ms = playback_started_ms_ + frame.stream_ms;
            const std::int64_t wait_ms = due_ms - frame.capture_ms;
            if (wait_ms > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(std::min<std::int64_t>(wait_ms, 1000)));
                frame.capture_ms = monotonicMs();
            }
        }
        return ReadStatus::kOk;
    }

    [[nodiscard]] std::string describe() const override {
        return maskCredentials(config_.source);
    }

    [[nodiscard]] bool reconnectable() const override { return kind_ != CameraKind::kFile; }

private:
    CameraConfig config_;
    CameraKind kind_;
    cv::VideoCapture capture_;
    /// Clockwise display rotation of a video file, applied to every frame; 0 for other sources.
    int rotation_{0};
    cv::Mat decoded_;
    std::int64_t sequence_{0};
    /// Frame rate used to build a file's timeline. Taken from the container, else from config.
    double frame_interval_fps_{25.0};
    /// Monotonic time at which replay of a file began, for real-time pacing.
    std::int64_t playback_started_ms_{0};

    bool openRtsp() {
        // FFmpeg reads the transport preference from this environment variable. UDP drops
        // packets under load and produces torn frames, which wastes detector work.
        if (config_.rtsp_tcp) {
#ifdef _WIN32
            _putenv_s("OPENCV_FFMPEG_CAPTURE_OPTIONS", "rtsp_transport;tcp");
#else
            setenv("OPENCV_FFMPEG_CAPTURE_OPTIONS", "rtsp_transport;tcp", 1);
#endif
        }
        if (capture_.open(config_.source, cv::CAP_FFMPEG)) {
            return true;
        }
        // Jetson images ship a GStreamer-backed OpenCV where FFmpeg may be absent.
        return capture_.open(config_.source, cv::CAP_GSTREAMER);
    }
};

}  // namespace

std::string maskCredentials(const std::string& source) {
    const std::size_t scheme = source.find("://");
    if (scheme == std::string::npos) {
        return source;
    }
    const std::size_t at = source.find('@', scheme);
    if (at == std::string::npos) {
        return source;
    }
    return source.substr(0, scheme + 3) + "***@" + source.substr(at + 1);
}

std::unique_ptr<CameraSource> makeCameraSource(const CameraConfig& config, std::string& error) {
    CameraKind kind = config.kind;
    if (kind == CameraKind::kAuto) {
        if (looksLikeDeviceIndex(config.source)) {
            kind = CameraKind::kDevice;
        } else if (looksLikeRtsp(config.source)) {
            kind = CameraKind::kRtsp;
        } else if (looksLikeGStreamerPipeline(config.source)) {
            kind = CameraKind::kGStreamer;
        } else {
            kind = CameraKind::kFile;
        }
    }

    if (kind == CameraKind::kFile && !filesystem::exists(config.source)) {
        error = "CAMERA_UNAVAILABLE: video file not found: " + config.source;
        return nullptr;
    }
    return std::make_unique<OpenCvCameraSource>(config, kind);
}

}  // namespace anpr
