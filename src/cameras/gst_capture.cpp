#include "anpr/cameras/gst_capture.hpp"

#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <utility>

#include "anpr/cameras/rtsp_camera_source.hpp"

namespace anpr::cameras {
namespace {

/// Bus and appsink are polled in slices this long, so an error posted by a streaming thread
/// (which does not always end the stream) is seen promptly instead of after the whole timeout.
constexpr std::int64_t kPollSliceMs = 100;

struct GstInitState {
    bool ok{false};
    std::string error;
};

/// gst_init exactly once per process, from whichever thread gets here first. OpenCV's GStreamer
/// backend initialises the library too; GStreamer treats the second call as a no-op.
const GstInitState& gstInitState() {
    static std::once_flag once;
    static GstInitState state;
    std::call_once(once, [] {
        GError* error = nullptr;
        state.ok = gst_init_check(nullptr, nullptr, &error) == TRUE;
        if (error != nullptr) {
            state.error = error->message != nullptr ? error->message : "unknown error";
            g_error_free(error);
        }
        if (!state.ok && state.error.empty()) {
            state.error = "gst_init_check failed";
        }
    });
    return state;
}

std::string lastLine(const std::string& text) {
    std::size_t end = text.find_last_not_of(" \t\r\n");
    if (end == std::string::npos) {
        return {};
    }
    const std::size_t newline = text.rfind('\n', end);
    const std::size_t begin = newline == std::string::npos ? 0 : newline + 1;
    return text.substr(begin, end - begin + 1);
}

/// "rtspsrc0: Unauthorized (gst_rtspsrc_send (): ...)" style text for an ERROR or WARNING.
std::string messageText(GstMessage* message) {
    GError* error = nullptr;
    gchar* debug = nullptr;
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
        gst_message_parse_error(message, &error, &debug);
    } else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_WARNING) {
        gst_message_parse_warning(message, &error, &debug);
    }
    std::string text;
    if (GST_MESSAGE_SRC(message) != nullptr) {
        const gchar* name = GST_OBJECT_NAME(GST_MESSAGE_SRC(message));
        if (name != nullptr) {
            text = std::string(name) + ": ";
        }
    }
    const std::string summary =
        error != nullptr && error->message != nullptr ? error->message : "unknown error";
    text += summary;
    if (debug != nullptr) {
        // The last debug line holds the specific reason ("streaming stopped, reason
        // not-negotiated (-4)", "Unauthorized") when there is one; a line that is only the
        // source location ("gstidentity.c(723): gst_identity_transform_ip (): /GstPipeline:...")
        // says nothing to an installer.
        const std::string detail = lastLine(debug);
        const bool location_only =
            detail.find(".c(") != std::string::npos && detail.find("():") != std::string::npos;
        if (!detail.empty() && detail != summary && !location_only) {
            text += " (" + detail + ")";
        }
    }
    if (error != nullptr) {
        g_error_free(error);
    }
    g_free(debug);
    return redactSecrets(text);
}

/// The appsink named "sink", else the only appsink anywhere in the bin (a description written for
/// OpenCV names it "appsink0" or "opencvsink"). A new reference, or nullptr.
GstElement* findAppSink(GstBin* bin) {
    GstElement* named = gst_bin_get_by_name(bin, "sink");
    if (named != nullptr && GST_IS_APP_SINK(named)) {
        return named;
    }
    if (named != nullptr) {
        gst_object_unref(named);
    }
    GstElement* found = nullptr;
    int count = 0;
    GstIterator* iterator = gst_bin_iterate_recurse(bin);
    GValue item{};
    bool done = false;
    while (!done) {
        switch (gst_iterator_next(iterator, &item)) {
            case GST_ITERATOR_OK: {
                auto* element = static_cast<GstElement*>(g_value_get_object(&item));
                if (GST_IS_APP_SINK(element)) {
                    ++count;
                    if (found == nullptr) {
                        found = GST_ELEMENT(gst_object_ref(element));
                    }
                }
                g_value_reset(&item);
                break;
            }
            case GST_ITERATOR_RESYNC:
                gst_iterator_resync(iterator);
                if (found != nullptr) {
                    gst_object_unref(found);
                    found = nullptr;
                }
                count = 0;
                break;
            case GST_ITERATOR_ERROR:
            case GST_ITERATOR_DONE:
                done = true;
                break;
        }
    }
    if (G_IS_VALUE(&item)) {
        g_value_unset(&item);
    }
    gst_iterator_free(iterator);
    // Two unnamed appsinks are ambiguous: take neither.
    if (count > 1 && found != nullptr) {
        gst_object_unref(found);
        found = nullptr;
    }
    return found;
}

std::int64_t elapsedMs(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                 since)
        .count();
}

}  // namespace

struct GstCapture::Impl {
    GstElement* pipeline{nullptr};
    GstAppSink* sink{nullptr};
    GstBus* bus{nullptr};
    /// The first sample, pulled by `open` and handed out by the next `pull`.
    GstSample* pending{nullptr};
    double caps_fps{0.0};
    /// rtspsrc announces a clean server close as a WARNING followed by EOS; the warning is the
    /// useful part of an end-of-stream report.
    std::string last_warning;
    const std::atomic_bool* interrupt{nullptr};

    /// Drains ERROR, WARNING and EOS messages queued on the bus (other messages are discarded by
    /// the filtered pop, so the bus never accumulates them). kError on an error, kEndOfStream on
    /// EOS, kFrame when nothing fatal is pending.
    Status pollBus(std::string& error) {
        if (bus == nullptr) {
            return Status::kFrame;
        }
        bool end_of_stream = false;
        while (GstMessage* message = gst_bus_timed_pop_filtered(
                   bus, 0,
                   static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_WARNING |
                                               GST_MESSAGE_EOS))) {
            const GstMessageType type = GST_MESSAGE_TYPE(message);
            if (type == GST_MESSAGE_ERROR) {
                error = messageText(message);
                gst_message_unref(message);
                return Status::kError;
            }
            if (type == GST_MESSAGE_WARNING) {
                last_warning = messageText(message);
            } else if (type == GST_MESSAGE_EOS) {
                end_of_stream = true;
            }
            gst_message_unref(message);
        }
        if (end_of_stream) {
            error = last_warning.empty() ? "end of stream" : last_warning;
            return Status::kEndOfStream;
        }
        return Status::kFrame;
    }

    /// Waits up to `timeout_ms` for the next sample while watching the bus.
    Status waitForSample(std::int64_t timeout_ms, GstSample*& sample, std::string& error) {
        sample = nullptr;
        const auto started = std::chrono::steady_clock::now();
        while (true) {
            // A sample already queued wins over an EOS behind it.
            sample = gst_app_sink_try_pull_sample(sink, 0);
            if (sample != nullptr) {
                return Status::kFrame;
            }
            if (interrupt != nullptr && interrupt->load()) {
                error = "interrupted";
                return Status::kError;
            }
            const Status bus_status = pollBus(error);
            if (bus_status == Status::kError) {
                return bus_status;
            }
            const std::int64_t remaining =
                std::max<std::int64_t>(0, timeout_ms - elapsedMs(started));
            const std::int64_t slice = std::min(kPollSliceMs, remaining);
            sample = gst_app_sink_try_pull_sample(sink,
                                                  static_cast<GstClockTime>(slice) * GST_MSECOND);
            if (sample != nullptr) {
                return Status::kFrame;
            }
            if (gst_app_sink_is_eos(sink) == TRUE) {
                // An error usually precedes the EOS it causes; report the error then.
                if (pollBus(error) == Status::kError) {
                    return Status::kError;
                }
                error = last_warning.empty() ? "end of stream" : last_warning;
                return Status::kEndOfStream;
            }
            if (elapsedMs(started) >= timeout_ms) {
                if (pollBus(error) == Status::kError) {
                    return Status::kError;
                }
                error = "no frame within " + std::to_string(timeout_ms) + " ms";
                return Status::kTimeout;
            }
        }
    }

    /// Copies the sample's picture into `image`, honouring the plane strides and offsets of the
    /// buffer (GstVideoMeta), which need not match the default layout for the caps.
    Status convert(GstSample* sample, cv::Mat& image, bool& format_i420, std::string& error) {
        GstCaps* caps = gst_sample_get_caps(sample);
        GstBuffer* buffer = gst_sample_get_buffer(sample);
        if (caps == nullptr || buffer == nullptr) {
            error = "appsink delivered a sample without caps or buffer";
            return Status::kError;
        }
        GstCapsFeatures* features = gst_caps_get_features(caps, 0);
        if (features != nullptr && gst_caps_features_contains(features, "memory:NVMM") == TRUE) {
            error = "appsink receives NVMM buffers; convert to system memory with nvvidconv ! "
                    "video/x-raw,format=I420 before the appsink";
            return Status::kError;
        }
        GstVideoInfo info;
        gst_video_info_init(&info);
        if (gst_video_info_from_caps(&info, caps) == FALSE) {
            gchar* text = gst_caps_to_string(caps);
            error = std::string("unsupported appsink caps: ") + (text != nullptr ? text : "?");
            g_free(text);
            return Status::kError;
        }
        if (GST_VIDEO_INFO_FPS_N(&info) > 0 && GST_VIDEO_INFO_FPS_D(&info) > 0) {
            caps_fps = static_cast<double>(GST_VIDEO_INFO_FPS_N(&info)) /
                       static_cast<double>(GST_VIDEO_INFO_FPS_D(&info));
        } else {
            caps_fps = 0.0;
        }

        const GstVideoFormat format = GST_VIDEO_INFO_FORMAT(&info);
        if (format != GST_VIDEO_FORMAT_I420 && format != GST_VIDEO_FORMAT_BGR) {
            error = std::string("unsupported frame format ") + gst_video_format_to_string(format) +
                    " (the appsink must receive I420 or BGR)";
            return Status::kError;
        }

        GstVideoFrame frame;
        if (gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ) == FALSE) {
            error = "could not map the decoded buffer";
            return Status::kError;
        }
        const int width = GST_VIDEO_FRAME_WIDTH(&frame);
        const int height = GST_VIDEO_FRAME_HEIGHT(&frame);
        Status status = Status::kFrame;

        // A recycled frame buffer that is a view into a larger matrix would not be contiguous.
        if (!image.empty() && !image.isContinuous()) {
            image.release();
        }
        if (format == GST_VIDEO_FORMAT_I420) {
            // OpenCV's I420 layout (COLOR_YUV2BGR_I420) is one contiguous matrix: the Y plane,
            // then U and V packed at half width and half height. It needs even sizes, so an odd
            // last row or column is dropped.
            const int luma_width = width & ~1;
            const int luma_height = height & ~1;
            if (luma_width <= 0 || luma_height <= 0) {
                error = "decoded I420 frame is too small";
                status = Status::kError;
            } else {
                image.create(luma_height * 3 / 2, luma_width, CV_8UC1);
                std::uint8_t* out = image.data;
                const auto* y_plane =
                    static_cast<const std::uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
                const int y_stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
                for (int row = 0; row < luma_height; ++row) {
                    std::memcpy(out + static_cast<std::size_t>(row) * luma_width,
                                y_plane + static_cast<std::ptrdiff_t>(row) * y_stride,
                                static_cast<std::size_t>(luma_width));
                }
                const int chroma_width = luma_width / 2;
                const int chroma_height = luma_height / 2;
                std::uint8_t* chroma_out = out + static_cast<std::size_t>(luma_width) *
                                                     static_cast<std::size_t>(luma_height);
                for (int plane = 1; plane <= 2; ++plane) {
                    const auto* source =
                        static_cast<const std::uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, plane));
                    const int stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, plane);
                    for (int row = 0; row < chroma_height; ++row) {
                        std::memcpy(chroma_out + static_cast<std::size_t>(row) * chroma_width,
                                    source + static_cast<std::ptrdiff_t>(row) * stride,
                                    static_cast<std::size_t>(chroma_width));
                    }
                    chroma_out += static_cast<std::size_t>(chroma_width) *
                                  static_cast<std::size_t>(chroma_height);
                }
                format_i420 = true;
            }
        } else {
            image.create(height, width, CV_8UC3);
            const auto* source =
                static_cast<const std::uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
            const int stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
            const std::size_t row_bytes = static_cast<std::size_t>(width) * 3;
            for (int row = 0; row < height; ++row) {
                std::memcpy(image.ptr(row), source + static_cast<std::ptrdiff_t>(row) * stride,
                            row_bytes);
            }
            format_i420 = false;
        }
        gst_video_frame_unmap(&frame);
        return status;
    }

    void release() {
        if (pending != nullptr) {
            gst_sample_unref(pending);
            pending = nullptr;
        }
        if (pipeline != nullptr) {
            // NULL tears rtspsrc down (TEARDOWN, sockets closed) and stops every streaming thread.
            gst_element_set_state(pipeline, GST_STATE_NULL);
        }
        if (sink != nullptr) {
            gst_object_unref(sink);
            sink = nullptr;
        }
        if (bus != nullptr) {
            gst_object_unref(bus);
            bus = nullptr;
        }
        if (pipeline != nullptr) {
            gst_object_unref(pipeline);
            pipeline = nullptr;
        }
        caps_fps = 0.0;
        last_warning.clear();
    }
};

GstCapture::GstCapture() : impl_(std::make_unique<Impl>()) {}

GstCapture::~GstCapture() {
    close();
}

GstCapture::Status GstCapture::open(const std::string& pipeline_description,
                                    int first_frame_timeout_ms, std::string& error) {
    close();
    error.clear();
    const GstInitState& init = gstInitState();
    if (!init.ok) {
        error = "GStreamer could not be initialised: " + init.error;
        return Status::kError;
    }

    GError* parse_error = nullptr;
    GstElement* pipeline = gst_parse_launch_full(pipeline_description.c_str(), nullptr,
                                                 GST_PARSE_FLAG_FATAL_ERRORS, &parse_error);
    if (parse_error != nullptr || pipeline == nullptr) {
        // Like gst-launch, a "recoverable" parse error (an unknown property) is still fatal:
        // the pipeline would not be the one that was asked for.
        error = std::string("pipeline: ") +
                (parse_error != nullptr && parse_error->message != nullptr ? parse_error->message
                                                                           : "could not be built");
        error = redactSecrets(error);
        if (parse_error != nullptr) {
            g_error_free(parse_error);
        }
        if (pipeline != nullptr) {
            gst_object_unref(gst_object_ref_sink(pipeline));
        }
        return Status::kError;
    }
    impl_->pipeline = GST_ELEMENT(gst_object_ref_sink(pipeline));

    GstElement* sink =
        GST_IS_BIN(impl_->pipeline) ? findAppSink(GST_BIN(impl_->pipeline)) : nullptr;
    if (sink == nullptr) {
        error = "pipeline: no appsink named \"sink\" (and not exactly one unnamed appsink)";
        close();
        return Status::kError;
    }
    impl_->sink = GST_APP_SINK(sink);
    impl_->bus = gst_element_get_bus(impl_->pipeline);

    if (gst_element_set_state(impl_->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        std::string bus_error;
        error = impl_->pollBus(bus_error) == Status::kError ? bus_error
                                                            : "the pipeline refused to start";
        close();
        return Status::kError;
    }

    GstSample* sample = nullptr;
    const Status status =
        impl_->waitForSample(std::max(0, first_frame_timeout_ms), sample, error);
    if (status != Status::kFrame) {
        close();
        return status;
    }
    impl_->pending = sample;
    // Learn the frame rate from the first sample's caps right away.
    if (GstCaps* caps = gst_sample_get_caps(sample)) {
        GstVideoInfo info;
        gst_video_info_init(&info);
        if (gst_video_info_from_caps(&info, caps) == TRUE && GST_VIDEO_INFO_FPS_N(&info) > 0 &&
            GST_VIDEO_INFO_FPS_D(&info) > 0) {
            impl_->caps_fps = static_cast<double>(GST_VIDEO_INFO_FPS_N(&info)) /
                              static_cast<double>(GST_VIDEO_INFO_FPS_D(&info));
        }
    }
    return Status::kFrame;
}

GstCapture::Status GstCapture::pull(cv::Mat& image, bool& format_i420, int timeout_ms,
                                    std::string& error) {
    error.clear();
    if (impl_->pipeline == nullptr) {
        error = "capture is not open";
        return Status::kError;
    }
    GstSample* sample = std::exchange(impl_->pending, nullptr);
    if (sample == nullptr) {
        const Status status = impl_->waitForSample(std::max(0, timeout_ms), sample, error);
        if (status != Status::kFrame) {
            return status;
        }
    }
    const Status status = impl_->convert(sample, image, format_i420, error);
    gst_sample_unref(sample);
    return status;
}

void GstCapture::close() {
    if (impl_ != nullptr) {
        impl_->release();
    }
}

void GstCapture::setInterruptFlag(const std::atomic_bool* flag) {
    impl_->interrupt = flag;
}

bool GstCapture::isOpen() const {
    return impl_ != nullptr && impl_->pipeline != nullptr;
}

double GstCapture::capsFps() const {
    return impl_ != nullptr ? impl_->caps_fps : 0.0;
}

bool gstreamerCompiledIn() {
    return gstInitState().ok;
}

bool gstElementAvailable(const std::string& factory_name) {
    if (!gstInitState().ok || factory_name.empty()) {
        return false;
    }
    GstElementFactory* factory = gst_element_factory_find(factory_name.c_str());
    if (factory == nullptr) {
        return false;
    }
    gst_object_unref(factory);
    return true;
}

}  // namespace anpr::cameras
