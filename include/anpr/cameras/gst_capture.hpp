#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include <opencv2/core.hpp>

namespace anpr::cameras {

/// Native GStreamer capture: a pipeline description ending in `appsink name=sink` (or one with a
/// single appsink of any name), pulled with a timeout and with the pipeline bus watched for
/// errors.
///
/// OpenCV 4.5.0's GStreamer backend (the one in the Jetson image) waits forever in read() when a
/// camera goes silent (PoE cable pulled: no FIN, no RST), has no open/read timeouts, and accepts
/// only BGR, so every frame pays a CPU colour conversion. Driving the appsink directly gives a
/// bounded read (the stall becomes STREAM_TIMEOUT within `read_timeout_ms`), the real error text
/// from the bus ("no element nvv4l2decoder", "Unauthorized", "not-negotiated"), a clean teardown
/// from the capture thread, and I420 frames straight from the hardware converter.
///
/// Only built when the GStreamer development files are present (KZ_ANPR_WITH_GSTREAMER); without
/// them camera mode falls back to cv::VideoCapture.
class GstCapture {
public:
    GstCapture();
    ~GstCapture();
    GstCapture(const GstCapture&) = delete;
    GstCapture& operator=(const GstCapture&) = delete;

    enum class Status { kFrame, kTimeout, kEndOfStream, kError };

    /// Builds and starts the pipeline and waits up to `first_frame_timeout_ms` for the first
    /// frame, which is kept for the next `pull`. kError carries the bus message in `error`.
    Status open(const std::string& pipeline_description, int first_frame_timeout_ms,
                std::string& error);

    /// The next frame within `timeout_ms`. BGR caps give CV_8UC3; I420 caps give one CV_8UC1
    /// matrix of height * 3 / 2 rows and `format_i420` true. The matrix owns its memory.
    Status pull(cv::Mat& image, bool& format_i420, int timeout_ms, std::string& error);

    /// Stops the pipeline (state NULL) and releases it. Safe to call twice.
    void close();
    /// While `*flag` is true (set from any thread), waits in `open` and `pull` end within one
    /// poll slice with kError "interrupted". The flag must outlive this object; nullptr clears.
    void setInterruptFlag(const std::atomic_bool* flag);
    [[nodiscard]] bool isOpen() const;

    /// Frame rate announced in the negotiated caps, 0 when variable or unknown.
    [[nodiscard]] double capsFps() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// True when this binary was built with native GStreamer support and gst_init succeeded.
bool gstreamerCompiledIn();
/// True when an element factory with this name can be created ("nvv4l2decoder", "avdec_h265").
bool gstElementAvailable(const std::string& factory_name);

}  // namespace anpr::cameras
