#include "anpr/cameras/multi_camera_runner.hpp"

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <deque>
#include <exception>
#include <optional>
#include <thread>
#include <utility>

#include <opencv2/imgproc.hpp>

#include "anpr/camera/frame_pump.hpp"
#include "anpr/cameras/diagnostics.hpp"
#include "anpr/cameras/reconnect_policy.hpp"
#include "anpr/cameras/rtsp_camera_source.hpp"
#include "anpr/cameras/stream_check.hpp"
#include "anpr/common/logging.hpp"
#include "anpr/hikvision/isapi.hpp"
#include "anpr/pipeline/anpr_pipeline.hpp"

namespace anpr::cameras {
namespace {

std::int64_t monotonicMs() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

std::int64_t unixTimeMs() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

/// "2026-10-09T10:15:05Z".
std::string isoUtc(std::int64_t unix_ms) {
    const std::time_t seconds = static_cast<std::time_t>(unix_ms / 1000);
    std::tm utc{};
    gmtime_r(&seconds, &utc);
    char text[32];
    std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return text;
}

double rounded(double value, int decimals) {
    const double scale = std::pow(10.0, decimals);
    return std::round(value * scale) / scale;
}

/// How often a processing thread publishes its numbers, and how many of those samples the
/// rates in the status file span (about five seconds).
constexpr std::int64_t kPublishIntervalMs = 1000;
constexpr std::size_t kRateWindowSamples = 6;
/// System numbers (and the low-memory check) are refreshed this often.
constexpr std::int64_t kSystemCheckIntervalMs = 5000;
constexpr std::int64_t kMemoryWarningIntervalMs = 60000;
/// A camera whose frames keep throwing logs one line per this interval, with the count.
constexpr std::int64_t kFrameErrorLogIntervalMs = 10000;

/// Cumulative numbers of one camera, as published by its processing thread.
struct WorkerTotals {
    std::int64_t captured{0};
    std::int64_t picked{0};
    std::int64_t frames_processed{0};
    std::int64_t stale_dropped{0};
    std::int64_t frame_errors{0};
    std::int64_t detector_calls{0};
    double detector_ms{0.0};
    std::int64_t ocr_calls{0};
    double ocr_ms{0.0};
    std::int64_t plates_confirmed{0};
    /// Sum over picked frames of their age at pick-up.
    double age_ms{0.0};
};

struct WorkerRates {
    double input_fps{0.0};
    double processed_fps{0.0};
    double detector_fps{0.0};
    double detector_avg_ms{0.0};
    double ocr_avg_ms{0.0};
    double capture_to_process_ms{0.0};
};

/// Rates between two samples; averages fall back to the lifetime value when the window saw no
/// call, so an idle camera still shows what its last detector call cost.
WorkerRates ratesBetween(const WorkerTotals& before, std::int64_t before_ms,
                         const WorkerTotals& after, std::int64_t after_ms) {
    WorkerRates rates;
    const double seconds = static_cast<double>(after_ms - before_ms) / 1000.0;
    if (seconds > 0.0) {
        rates.input_fps = static_cast<double>(after.captured - before.captured) / seconds;
        rates.processed_fps =
            static_cast<double>(after.frames_processed - before.frames_processed) / seconds;
        rates.detector_fps =
            static_cast<double>(after.detector_calls - before.detector_calls) / seconds;
    }
    const std::int64_t detector_calls = after.detector_calls - before.detector_calls;
    if (detector_calls > 0) {
        rates.detector_avg_ms =
            (after.detector_ms - before.detector_ms) / static_cast<double>(detector_calls);
    } else if (after.detector_calls > 0) {
        rates.detector_avg_ms = after.detector_ms / static_cast<double>(after.detector_calls);
    }
    const std::int64_t ocr_calls = after.ocr_calls - before.ocr_calls;
    if (ocr_calls > 0) {
        rates.ocr_avg_ms = (after.ocr_ms - before.ocr_ms) / static_cast<double>(ocr_calls);
    } else if (after.ocr_calls > 0) {
        rates.ocr_avg_ms = after.ocr_ms / static_cast<double>(after.ocr_calls);
    }
    const std::int64_t picked = after.picked - before.picked;
    if (picked > 0) {
        rates.capture_to_process_ms = (after.age_ms - before.age_ms) / static_cast<double>(picked);
    }
    return rates;
}

std::string stripSharedPrefix(const std::string& backend) {
    const std::string prefix = "shared:";
    return backend.compare(0, prefix.size(), prefix) == 0 ? backend.substr(prefix.size()) : backend;
}

/// One camera's view of the shared sink: remembers the last accepted plate for the status file
/// and forwards everything.
class CameraSink final : public PlateSink {
public:
    explicit CameraSink(std::shared_ptr<PlateSink> shared) : shared_(std::move(shared)) {}

    void onRecognition(const PlateRecognitionEvent& event) override {
        if (isAccepted(event.status)) {
            last_plate_ = event.normalized_plate;
            last_plate_unix_ms_ = event.unix_time_ms > 0 ? event.unix_time_ms : unixTimeMs();
        }
        shared_->onRecognition(event);
    }

    void onRawEvent(const std::string& json_line) override { shared_->onRawEvent(json_line); }

    /// Processing thread only (the pipeline calls onRecognition from there).
    [[nodiscard]] const std::string& lastPlate() const { return last_plate_; }
    [[nodiscard]] std::int64_t lastPlateUnixMs() const { return last_plate_unix_ms_; }

private:
    std::shared_ptr<PlateSink> shared_;
    std::string last_plate_;
    std::int64_t last_plate_unix_ms_{0};
};

AnprConfig pipelineConfig(const CameraWorkerSpec& spec) {
    AnprConfig config = spec.anpr;
    config.camera.camera_id = spec.camera_id;
    return config;
}

CameraConfig captureConfig(const CameraWorkerSpec& spec) {
    // `camera.process_every_file_frame` (on in the shipped profiles) keeps deciding whether a
    // clip is processed frame by frame, exactly as for a single --source.
    CameraConfig config = spec.anpr.camera;
    config.camera_id = spec.camera_id;
    return config;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Worker: one camera's capture pump, pipeline and processing thread
// ---------------------------------------------------------------------------------------------

class MultiCameraRunner::Worker {
public:
    Worker(CameraWorkerSpec spec, const std::shared_ptr<SharedPlateDetectorCore>& detector,
           const std::shared_ptr<SharedPlateOcrCore>& ocr, std::shared_ptr<PlateSink> shared_sink)
        : id_(spec.camera_id),
          ip_(spec.ip),
          mac_(spec.mac),
          model_(spec.model),
          live_(spec.live),
          max_frame_age_ms_(spec.max_frame_age_ms),
          read_timeout_ms_(std::max<std::int64_t>(1, spec.anpr.camera.read_timeout_ms)),
          log_context_("camera_id=" + quoteLogValue(spec.camera_id)),
          native_enabled_(spec.native_anpr),
          native_options_(spec.native_anpr_options),
          shared_sink_(std::move(shared_sink)),
          sink_(shared_sink_),
          pipeline_(pipelineConfig(spec), sink_),
          pump_(captureConfig(spec), std::move(spec.source)) {
        pipeline_.setDetector(makeSharedPlateDetectorClient(detector, &pipeline_.metrics()));
        pipeline_.setOcr(makeSharedPlateOcrClient(ocr, &pipeline_.metrics()));
        rtsp_ = dynamic_cast<const RtspCameraSource*>(&pump_.source());
        pump_.setQueueCapacity(live_ ? spec.queue_capacity : 1);
        pump_.setLogContext(log_context_);
        // A camera's first connection (RTSP check, decoder attempts, first frame) may take many
        // seconds; it happens on the capture thread so adding a camera never blocks the caller.
        pump_.setOpenInBackground(live_);
        if (native_options_.camera_id.empty()) {
            native_options_.camera_id = id_;
        }
    }

    ~Worker() {
        requestStop();
        join();
    }

    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    bool start(std::string& error) {
        if (!pump_.start()) {
            error = "camera " + id_ + ": the source could not be opened (" +
                    pump_.source().describe() + ")";
            return false;
        }
        thread_ = std::thread(&Worker::processLoop, this);
        if (native_enabled_) {
            std::shared_ptr<PlateSink> sink = shared_sink_;
            const std::string id = id_;
            native_ = std::make_unique<hikvision::NativeAnprListener>(
                native_options_, [sink, id](const hikvision::NativePlateEvent& event) {
                    hikvision::NativePlateEvent tagged = event;
                    if (tagged.camera_id.empty()) {
                        tagged.camera_id = id;
                    }
                    if (tagged.unix_time_ms <= 0) {
                        tagged.unix_time_ms = unixTimeMs();
                    }
                    try {
                        sink->onRawEvent(hikvision::toJson(tagged));
                    } catch (const std::exception& exception) {
                        logEvent(LogLevel::kWarn, "native_anpr_event_dropped",
                                 LogFields()
                                     .addQuoted("camera_id", id)
                                     .addQuoted("what", exception.what()));
                    }
                });
            native_->start();
        }
        return true;
    }

    /// Stops the native listener and the capture pump (which closes the source). The processing
    /// thread sees the flag within one wait slice.
    void requestStop() {
        stop_.store(true);
        if (native_ != nullptr) {
            native_->stop();
        }
        pump_.stop();
    }

    void join() {
        if (thread_.joinable()) {
            thread_.join();
        }
        stopped_.store(true);
    }

    [[nodiscard]] const std::string& id() const { return id_; }
    [[nodiscard]] bool finished() const { return finished_.load(); }
    [[nodiscard]] std::string sourceDescription() const { return pump_.source().describe(); }

    [[nodiscard]] WorkerTotals totals() const {
        const std::lock_guard<std::mutex> guard(snapshot_mutex_);
        return published_totals_;
    }

    CameraStatus status(std::int64_t now_ms) const {
        CameraStatus status;
        status.id = id_;
        status.ip = ip_;
        status.mac = mac_;
        status.model = model_;

        const PumpStats pump = pump_.stats();
        WorkerTotals totals;
        WorkerRates rates;
        int frame_width = 0;
        int frame_height = 0;
        std::string last_plate;
        std::int64_t last_plate_unix_ms = 0;
        {
            const std::lock_guard<std::mutex> guard(snapshot_mutex_);
            totals = published_totals_;
            rates = published_rates_;
            frame_width = frame_width_;
            frame_height = frame_height_;
            last_plate = last_plate_;
            last_plate_unix_ms = last_plate_unix_ms_;
        }
        std::optional<RtspCameraSource::Status> source;
        if (rtsp_ != nullptr) {
            source = rtsp_->status();
        }
        const CameraError error = source ? source->error : CameraError::kNone;
        const bool given_up = pump.given_up || (rtsp_ != nullptr && rtsp_->permanentlyFailed());

        if (stopped_.load() || pump.ended) {
            status.anpr = "STOPPED";
        } else if (given_up) {
            status.anpr = "DISABLED";
        } else if (error != CameraError::kNone &&
                   classifyFailure(error) != FailureClass::kTransient) {
            status.anpr = "ERROR";
        } else if (!pump.connected) {
            // Never connected and nothing failed yet: the first connection is still being made.
            const bool first_attempt = pump.open_failures == 0 && pump.last_frame_ms == 0 &&
                                       error == CameraError::kNone;
            status.anpr = first_attempt ? "STARTING" : "RECONNECTING";
        } else if (pump.first_frame_ms == 0) {
            status.anpr = "STARTING";
        } else if (now_ms - pump.last_frame_ms > read_timeout_ms_) {
            status.anpr = "STALLED";
        } else {
            status.anpr = "RUNNING";
        }

        if (source) {
            status.rtsp = rtspStatusLabel(error, source->rtsp_ok);
            if (source->reachable) {
                status.link = "OK";
            } else if (error == CameraError::kCameraUnreachable ||
                       error == CameraError::kCameraLanLinkDown) {
                status.link = "DOWN";
            }
            if (status.anpr == "RUNNING" && source->codec != net::VideoCodec::kUnknown) {
                status.video = net::shortName(source->codec);
            }
            status.decoder = source->decoder;
            status.width = source->width;
            status.height = source->height;
            status.first_frame_ms = source->first_frame_ms;
            if (error != CameraError::kNone) {
                const ErrorInfo& info = describe(error);
                status.error = info.code;
                status.action = info.action;
            }
            status.detail = source->detail;
        } else {
            status.width = frame_width;
            status.height = frame_height;
        }

        status.input_fps = rounded(rates.input_fps, 2);
        status.processed_fps = rounded(rates.processed_fps, 2);
        status.detector_fps = rounded(rates.detector_fps, 2);
        status.detector_avg_ms = rounded(rates.detector_avg_ms, 2);
        status.ocr_calls = totals.ocr_calls;
        status.ocr_avg_ms = rounded(rates.ocr_avg_ms, 2);
        status.plates_confirmed = totals.plates_confirmed;
        status.live_dropped = pump.dropped;
        status.stale_dropped = totals.stale_dropped;
        status.reconnects = pump.reconnects;
        status.capture_to_process_ms = rounded(rates.capture_to_process_ms, 1);
        status.last_plate = last_plate;
        if (last_plate_unix_ms > 0) {
            status.last_plate_time = isoUtc(last_plate_unix_ms);
        }
        return status;
    }

    /// One `camera_metrics` line with rates over the time since the previous one. Called from
    /// the runner's thread only.
    void logMetrics(std::int64_t now_ms) {
        WorkerTotals now_totals;
        std::int64_t published_ms = 0;
        {
            const std::lock_guard<std::mutex> guard(snapshot_mutex_);
            now_totals = published_totals_;
            published_ms = published_ms_;
        }
        const PumpStats pump = pump_.stats();
        // Rates over the published snapshots only, so every counter covers the same window.
        const WorkerRates rates = ratesBetween(metrics_baseline_, metrics_baseline_ms_,
                                               now_totals, published_ms);
        const CameraStatus state = status(now_ms);
        LogFields fields;
        fields.addQuoted("camera_id", id_)
            .add("input_fps", rounded(rates.input_fps, 2))
            .add("processed_fps", rounded(rates.processed_fps, 2))
            .add("detector_fps", rounded(rates.detector_fps, 2))
            .add("detector_avg_ms", rounded(rates.detector_avg_ms, 2))
            .add("ocr_calls", now_totals.ocr_calls)
            .add("ocr_avg_ms", rounded(rates.ocr_avg_ms, 2))
            .add("plates_confirmed", now_totals.plates_confirmed)
            .add("live_dropped", pump.dropped)
            .add("stale_dropped", now_totals.stale_dropped)
            .add("reconnects", pump.reconnects)
            .add("capture_to_process_ms", rounded(rates.capture_to_process_ms, 1))
            .add("state", state.anpr)
            .addQuoted("decoder", state.decoder.empty() ? "-" : state.decoder);
        if (now_totals.frame_errors > 0) {
            fields.add("frame_errors", now_totals.frame_errors);
        }
        logEvent(LogLevel::kInfo, "camera_metrics", fields);
        metrics_baseline_ = now_totals;
        metrics_baseline_ms_ = published_ms;
    }

private:
    const std::string id_;
    const std::string ip_;
    const std::string mac_;
    const std::string model_;
    const bool live_;
    const std::int64_t max_frame_age_ms_;
    const std::int64_t read_timeout_ms_;
    const std::string log_context_;
    const bool native_enabled_;
    hikvision::NativeAnprListener::Options native_options_;
    std::shared_ptr<PlateSink> shared_sink_;
    CameraSink sink_;
    AnprPipeline pipeline_;
    FramePump pump_;
    const RtspCameraSource* rtsp_{nullptr};
    std::unique_ptr<hikvision::NativeAnprListener> native_;
    std::thread thread_;
    std::atomic_bool stop_{false};
    std::atomic_bool finished_{false};
    std::atomic_bool stopped_{false};

    // Processing thread only.
    WorkerTotals totals_;
    Frame converted_;
    int last_width_{0};
    int last_height_{0};
    std::int64_t last_publish_ms_{0};
    std::deque<std::pair<std::int64_t, WorkerTotals>> history_;
    std::int64_t last_error_log_ms_{0};
    std::int64_t errors_since_log_{0};

    // Published by the processing thread.
    mutable std::mutex snapshot_mutex_;
    WorkerTotals published_totals_;
    WorkerRates published_rates_;
    std::int64_t published_ms_{monotonicMs()};
    int frame_width_{0};
    int frame_height_{0};
    std::string last_plate_;
    std::int64_t last_plate_unix_ms_{0};

    // Runner thread only.
    WorkerTotals metrics_baseline_;
    std::int64_t metrics_baseline_ms_{monotonicMs()};

    void processLoop() {
        const LogContext context(log_context_);
        Frame frame;
        last_publish_ms_ = monotonicMs();
        history_.emplace_back(last_publish_ms_, totals_);
        bool ended = false;
        while (!stop_.load()) {
            if (!pump_.waitForFrame(frame, 200)) {
                if (pump_.finished()) {
                    ended = true;
                    break;
                }
                publish(monotonicMs(), false);
                continue;
            }
            processOne(frame);
            publish(monotonicMs(), false);
        }
        // A clip that ended (or a stop) must not drop the readings of an open session.
        try {
            pipeline_.finishPendingRecognition();
        } catch (const std::exception& exception) {
            frameFailed(exception.what());
        }
        publish(monotonicMs(), true);
        if (ended) {
            logEvent(LogLevel::kInfo, "camera_processing_finished",
                     LogFields().add("frames_processed", pipeline_.metrics().frames_processed));
            finished_.store(true);
        }
    }

    void processOne(const Frame& frame) {
        ++totals_.picked;
        const std::int64_t age_ms = std::max<std::int64_t>(0, monotonicMs() - frame.capture_ms);
        totals_.age_ms += static_cast<double>(age_ms);
        // A frame that waited too long describes a vehicle that has moved on; processing it
        // would only delay the fresh ones behind it.
        if (live_ && max_frame_age_ms_ > 0 && age_ms > max_frame_age_ms_) {
            ++totals_.stale_dropped;
            return;
        }
        try {
            if (frame.format == PixelFormat::kI420) {
                // Only frames that are processed pay for the colour conversion.
                cv::cvtColor(frame.image, converted_.image, cv::COLOR_YUV2BGR_I420);
                converted_.format = PixelFormat::kBgr;
                converted_.capture_ms = frame.capture_ms;
                converted_.stream_ms = frame.stream_ms;
                converted_.sequence = frame.sequence;
                converted_.decode_ms = frame.decode_ms;
                pipeline_.processFrame(converted_);
                last_width_ = converted_.image.cols;
                last_height_ = converted_.image.rows;
            } else {
                pipeline_.processFrame(frame);
                last_width_ = frame.image.cols;
                last_height_ = frame.image.rows;
            }
        } catch (const std::exception& exception) {
            frameFailed(exception.what());
        } catch (...) {
            frameFailed("unknown exception");
        }
    }

    /// One camera's bad frame must never stop it, let alone the others: count, log (rate
    /// limited) and carry on with the next frame.
    void frameFailed(const std::string& what) {
        ++totals_.frame_errors;
        ++errors_since_log_;
        const std::int64_t now = monotonicMs();
        if (last_error_log_ms_ != 0 && now - last_error_log_ms_ < kFrameErrorLogIntervalMs) {
            return;
        }
        logEvent(LogLevel::kError, "camera_frame_failed",
                 LogFields()
                     .addQuoted("what", what)
                     .add("errors", errors_since_log_)
                     .add("errors_total", totals_.frame_errors));
        last_error_log_ms_ = now;
        errors_since_log_ = 0;
    }

    void publish(std::int64_t now_ms, bool force) {
        if (!force && now_ms - last_publish_ms_ < kPublishIntervalMs) {
            return;
        }
        WorkerTotals totals = totals_;
        const PipelineMetrics& metrics = pipeline_.metrics();
        totals.frames_processed = metrics.frames_processed;
        totals.detector_calls = metrics.detector_calls;
        totals.detector_ms = metrics.detector_total.sumMs();
        totals.ocr_calls = metrics.ocr_calls;
        totals.ocr_ms = metrics.ocr_total.sumMs();
        totals.plates_confirmed = metrics.plates_confirmed;
        totals.captured = pump_.stats().captured;

        history_.emplace_back(now_ms, totals);
        while (history_.size() > kRateWindowSamples) {
            history_.pop_front();
        }
        const WorkerRates rates = ratesBetween(history_.front().second, history_.front().first,
                                               history_.back().second, history_.back().first);
        {
            const std::lock_guard<std::mutex> guard(snapshot_mutex_);
            published_totals_ = totals;
            published_rates_ = rates;
            published_ms_ = now_ms;
            frame_width_ = last_width_;
            frame_height_ = last_height_;
            last_plate_ = sink_.lastPlate();
            last_plate_unix_ms_ = sink_.lastPlateUnixMs();
        }
        last_publish_ms_ = now_ms;
    }
};

// ---------------------------------------------------------------------------------------------
// MultiCameraRunner
// ---------------------------------------------------------------------------------------------

MultiCameraRunner::MultiCameraRunner(AnprConfig shared_config, Options options,
                                     std::shared_ptr<PlateSink> sink)
    : shared_config_(std::move(shared_config)),
      options_(std::move(options)),
      // Every camera's processing thread writes events; the sink sees them one at a time.
      sink_(std::make_shared<SynchronizedSink>(sink != nullptr
                                                   ? std::move(sink)
                                                   : std::make_shared<JsonStdoutSink>())),
      camera_lan_summary_(options_.camera_lan_summary),
      internet_summary_(options_.internet_summary) {}

MultiCameraRunner::~MultiCameraRunner() {
    stopAll();
}

bool MultiCameraRunner::loadModels(std::string& error) {
    std::string load_error;
    std::shared_ptr<SharedPlateDetectorCore> detector =
        makeSharedPlateDetector(shared_config_.detector, shared_config_.inference, load_error);
    if (detector == nullptr) {
        error = "plate detector " + shared_config_.detector.model + " (backend " +
                toString(shared_config_.inference.backend) + "): " + load_error;
        return false;
    }
    std::shared_ptr<SharedPlateOcrCore> ocr =
        makeSharedPlateOcr(shared_config_.ocr, shared_config_.inference, load_error);
    if (ocr == nullptr) {
        error = "OCR " + shared_config_.ocr.model + " (backend " +
                toString(shared_config_.inference.backend) + "): " + load_error;
        return false;
    }

    // One inference on each model now: a TensorRT engine that cannot execute fails here, at
    // startup, instead of on the first vehicle.
    PipelineMetrics warmup_metrics;
    std::unique_ptr<IPlateDetector> detector_client =
        makeSharedPlateDetectorClient(detector, &warmup_metrics);
    std::unique_ptr<IPlateOcr> ocr_client = makeSharedPlateOcrClient(ocr, &warmup_metrics);
    double detector_ms = 0.0;
    double ocr_ms = 0.0;
    try {
        const int width = std::max(64, shared_config_.camera.width);
        const int height = std::max(64, shared_config_.camera.height);
        const cv::Mat blank_frame(height, width, CV_8UC3, cv::Scalar(114, 114, 114));
        const auto detector_started = std::chrono::steady_clock::now();
        detector_client->detect(blank_frame);
        detector_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                                detector_started)
                          .count();
        const cv::Mat blank_crop(64, 192, CV_8UC3, cv::Scalar(114, 114, 114));
        const auto ocr_started = std::chrono::steady_clock::now();
        ocr_client->recognize(blank_crop);
        ocr_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                           ocr_started)
                     .count();
    } catch (const std::exception& exception) {
        error = std::string("warm-up inference failed: ") + exception.what();
        return false;
    }

    const std::string detector_backend = stripSharedPrefix(detector_client->backendName());
    const std::string ocr_backend = stripSharedPrefix(ocr_client->backendName());
    logEvent(LogLevel::kInfo, "models_ready",
             LogFields()
                 .add("detector_backend", detector_backend)
                 .add("ocr_backend", ocr_backend)
                 .addQuoted("ocr_model", ocr_client->modelDescription())
                 .add("detector_first_call_ms", rounded(detector_ms, 1))
                 .add("ocr_first_call_ms", rounded(ocr_ms, 1)));
    setSharedModels(std::move(detector), std::move(ocr), detector_backend, ocr_backend);
    return true;
}

void MultiCameraRunner::setSharedModels(std::shared_ptr<SharedPlateDetectorCore> detector,
                                        std::shared_ptr<SharedPlateOcrCore> ocr,
                                        std::string detector_backend, std::string ocr_backend) {
    const std::lock_guard<std::mutex> guard(workers_mutex_);
    detector_core_ = std::move(detector);
    ocr_core_ = std::move(ocr);
    detector_backend_ = std::move(detector_backend);
    ocr_backend_ = std::move(ocr_backend);
}

std::string MultiCameraRunner::detectorBackend() const {
    const std::lock_guard<std::mutex> guard(workers_mutex_);
    return detector_backend_;
}

std::string MultiCameraRunner::ocrBackend() const {
    const std::lock_guard<std::mutex> guard(workers_mutex_);
    return ocr_backend_;
}

bool MultiCameraRunner::hasCameraLocked(const std::string& camera_id) const {
    return std::any_of(workers_.begin(), workers_.end(),
                       [&camera_id](const std::unique_ptr<Worker>& worker) {
                           return worker->id() == camera_id;
                       });
}

bool MultiCameraRunner::addCamera(CameraWorkerSpec spec, std::string& error) {
    if (spec.camera_id.empty()) {
        error = "a camera needs an id";
        return false;
    }
    if (spec.source == nullptr) {
        error = "camera " + spec.camera_id + " has no source";
        return false;
    }
    const std::string camera_id = spec.camera_id;
    std::shared_ptr<SharedPlateDetectorCore> detector;
    std::shared_ptr<SharedPlateOcrCore> ocr;
    {
        const std::lock_guard<std::mutex> guard(workers_mutex_);
        if (stopped_) {
            error = "camera " + camera_id + ": the runner is stopping";
            return false;
        }
        if (hasCameraLocked(camera_id)) {
            error = "camera " + camera_id + " is already running";
            return false;
        }
        detector = detector_core_;
        ocr = ocr_core_;
    }
    if (detector == nullptr || ocr == nullptr) {
        error = "camera " + camera_id + ": the shared models are not loaded";
        return false;
    }

    if (spec.anpr.debug.visualize) {
        // OpenCV's highgui windows belong to one GUI thread; pipelines here run on one thread
        // per camera, where imshow is unsafe.
        spec.anpr.debug.visualize = false;
        logEvent(LogLevel::kWarn, "visualization_disabled",
                 LogFields()
                     .addQuoted("camera_id", spec.camera_id)
                     .addQuoted("reason", "the debug window is not available with several "
                                          "processing threads"));
    }
    const bool live = spec.live;
    const std::size_t queue_capacity = spec.queue_capacity;
    std::unique_ptr<Worker> worker;
    try {
        worker = std::make_unique<Worker>(std::move(spec), detector, ocr, sink_);
    } catch (const std::exception& exception) {
        error = "camera " + camera_id + ": " + exception.what();
        return false;
    }
    // A file is opened here; the lock is not held meanwhile, so the status file and the other
    // cameras are not held up. Live cameras connect on their own capture thread.
    try {
        if (!worker->start(error)) {
            return false;
        }
    } catch (const std::exception& exception) {
        error = "camera " + camera_id + ": " + exception.what();
        return false;
    }

    bool rejected = false;
    {
        const std::lock_guard<std::mutex> guard(workers_mutex_);
        if (stopped_ || hasCameraLocked(camera_id)) {
            rejected = true;
        } else {
            inactive_.erase(std::remove_if(inactive_.begin(), inactive_.end(),
                                           [&camera_id](const CameraStatus& status) {
                                               return status.id == camera_id;
                                           }),
                            inactive_.end());
            logEvent(LogLevel::kInfo, "camera_started",
                     LogFields()
                         .addQuoted("camera_id", camera_id)
                         .addQuoted("source", worker->sourceDescription())
                         .add("live", live ? 1 : 0)
                         .add("queue_size", live ? queue_capacity : 1)
                         .add("cameras_active", workers_.size() + 1));
            workers_.push_back(std::move(worker));
        }
    }
    if (rejected) {
        worker.reset();
        error = "camera " + camera_id + " was added twice or the runner is stopping";
        return false;
    }
    return true;
}

void MultiCameraRunner::addInactiveCamera(const CameraStatus& status) {
    const std::lock_guard<std::mutex> guard(workers_mutex_);
    for (CameraStatus& existing : inactive_) {
        if (existing.id == status.id) {
            existing = status;
            return;
        }
    }
    inactive_.push_back(status);
}

bool MultiCameraRunner::hasCamera(const std::string& camera_id) const {
    const std::lock_guard<std::mutex> guard(workers_mutex_);
    return hasCameraLocked(camera_id);
}

std::size_t MultiCameraRunner::activeCameraCount() const {
    const std::lock_guard<std::mutex> guard(workers_mutex_);
    return workers_.size();
}

bool MultiCameraRunner::allFinished() const {
    const std::lock_guard<std::mutex> guard(workers_mutex_);
    return std::all_of(workers_.begin(), workers_.end(),
                       [](const std::unique_ptr<Worker>& worker) { return worker->finished(); });
}

void MultiCameraRunner::refreshSystemStats() {
    SystemStats stats = stats_reader_.read();
    const std::lock_guard<std::mutex> guard(workers_mutex_);
    last_stats_ = std::move(stats);
}

void MultiCameraRunner::run(std::atomic_bool& stop, const std::function<void()>& periodic) {
    std::int64_t now = monotonicMs();
    std::int64_t last_periodic = now;
    std::int64_t last_system = now;
    std::int64_t last_status = now;
    std::int64_t last_metrics = now;
    refreshSystemStats();
    checkMemory();
    writeStatus();

    while (!stop.load()) {
        if (options_.exit_when_all_finished && allFinished()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        now = monotonicMs();
        if (periodic && now - last_periodic >= 1000) {
            last_periodic = now;
            try {
                periodic();
            } catch (const std::exception& exception) {
                logEvent(LogLevel::kWarn, "periodic_task_failed",
                         LogFields().addQuoted("what", exception.what()));
            }
        }
        if (now - last_system >= kSystemCheckIntervalMs) {
            last_system = now;
            refreshSystemStats();
            checkMemory();
        }
        if (options_.status_interval_ms > 0 && now - last_status >= options_.status_interval_ms) {
            last_status = now;
            writeStatus();
        }
        if (options_.metrics_interval_ms > 0 &&
            now - last_metrics >= options_.metrics_interval_ms) {
            last_metrics = now;
            logSummaries();
        }
    }
    // A sample taken right after the previous one has no CPU interval to measure.
    if (monotonicMs() - last_system >= 1000) {
        refreshSystemStats();
    }
    writeStatus();
    logSummaries();
}

void MultiCameraRunner::stopAll() {
    std::vector<Worker*> workers;
    {
        const std::lock_guard<std::mutex> guard(workers_mutex_);
        if (stopped_) {
            return;
        }
        stopped_ = true;
        for (const std::unique_ptr<Worker>& worker : workers_) {
            workers.push_back(worker.get());
        }
    }
    // All cameras at once: a capture thread may be inside a read for up to the read timeout,
    // and stopping N cameras one after the other would add those waits up.
    std::vector<std::thread> stoppers;
    stoppers.reserve(workers.size());
    for (Worker* worker : workers) {
        stoppers.emplace_back([worker] { worker->requestStop(); });
    }
    for (std::thread& stopper : stoppers) {
        stopper.join();
    }
    for (Worker* worker : workers) {
        worker->join();
    }
    writeStatus();
}

std::vector<CameraStatus> MultiCameraRunner::statuses() const {
    const std::int64_t now = monotonicMs();
    std::vector<CameraStatus> result;
    {
        const std::lock_guard<std::mutex> guard(workers_mutex_);
        result.reserve(workers_.size() + inactive_.size());
        for (const std::unique_ptr<Worker>& worker : workers_) {
            result.push_back(worker->status(now));
        }
        result.insert(result.end(), inactive_.begin(), inactive_.end());
    }
    std::stable_sort(result.begin(), result.end(),
                     [](const CameraStatus& lhs, const CameraStatus& rhs) {
                         return lhs.id < rhs.id;
                     });
    return result;
}

std::int64_t MultiCameraRunner::platesConfirmed() const {
    const std::lock_guard<std::mutex> guard(workers_mutex_);
    std::int64_t total = 0;
    for (const std::unique_ptr<Worker>& worker : workers_) {
        total += worker->totals().plates_confirmed;
    }
    return total;
}

std::int64_t MultiCameraRunner::framesProcessed() const {
    const std::lock_guard<std::mutex> guard(workers_mutex_);
    std::int64_t total = 0;
    for (const std::unique_ptr<Worker>& worker : workers_) {
        total += worker->totals().frames_processed;
    }
    return total;
}

void MultiCameraRunner::setNetworkSummary(std::string camera_lan, std::string internet) {
    const std::lock_guard<std::mutex> guard(workers_mutex_);
    camera_lan_summary_ = std::move(camera_lan);
    internet_summary_ = std::move(internet);
}

SystemStatus MultiCameraRunner::systemStatus() const {
    const std::lock_guard<std::mutex> guard(workers_mutex_);
    SystemStatus status;
    status.mem_total_mb = last_stats_.mem_total_kb / 1024;
    status.mem_available_mb = last_stats_.mem_available_kb / 1024;
    status.process_rss_mb = last_stats_.process_rss_kb / 1024;
    status.container_mem_mb = last_stats_.cgroup_usage_bytes
                                  ? *last_stats_.cgroup_usage_bytes / (1024 * 1024)
                                  : -1;
    status.load1 = last_stats_.load1;
    status.cpu_percent = last_stats_.cpu_percent ? rounded(*last_stats_.cpu_percent, 1) : -1.0;
    status.gpu_percent = last_stats_.gpu_percent ? rounded(*last_stats_.gpu_percent, 1) : -1.0;
    status.detector_backend = detector_backend_;
    status.ocr_backend = ocr_backend_;
    return status;
}

void MultiCameraRunner::writeStatus() {
    if (options_.status_file.empty()) {
        return;
    }
    StatusSnapshot snapshot;
    snapshot.updated_unix_ms = unixTimeMs();
    snapshot.updated_at = isoUtc(snapshot.updated_unix_ms);
    snapshot.pid = static_cast<int>(::getpid());
    {
        const std::lock_guard<std::mutex> guard(workers_mutex_);
        snapshot.camera_lan = camera_lan_summary_;
        snapshot.internet = internet_summary_;
    }
    snapshot.cameras = statuses();
    snapshot.system = systemStatus();

    std::string error;
    const bool written = writeStatusFile(options_.status_file, snapshot, error);
    const std::lock_guard<std::mutex> guard(workers_mutex_);
    if (written) {
        last_status_error_.clear();
    } else if (error != last_status_error_) {
        // Logged when the problem starts or changes, not every few seconds.
        last_status_error_ = error;
        logEvent(LogLevel::kWarn, "status_write_failed",
                 LogFields().addQuoted("path", options_.status_file).addQuoted("error", error));
    }
}

void MultiCameraRunner::logSummaries() {
    const std::int64_t now = monotonicMs();
    std::size_t cameras = 0;
    {
        const std::lock_guard<std::mutex> guard(workers_mutex_);
        cameras = workers_.size();
        for (const std::unique_ptr<Worker>& worker : workers_) {
            worker->logMetrics(now);
        }
    }
    const SystemStatus system = systemStatus();
    logEvent(LogLevel::kInfo, "system_metrics",
             LogFields()
                 .add("mem_available_mb", system.mem_available_mb)
                 .add("process_rss_mb", system.process_rss_mb)
                 .add("container_mem_mb", system.container_mem_mb)
                 .add("cpu_percent", system.cpu_percent)
                 .add("gpu_percent", system.gpu_percent)
                 .add("load1", rounded(system.load1, 2))
                 .addQuoted("detector_backend",
                            system.detector_backend.empty() ? "-" : system.detector_backend)
                 .addQuoted("ocr_backend", system.ocr_backend.empty() ? "-" : system.ocr_backend)
                 .add("cameras_active", cameras));
}

void MultiCameraRunner::checkMemory() {
    SystemStats stats;
    std::size_t cameras = 0;
    {
        const std::lock_guard<std::mutex> guard(workers_mutex_);
        stats = last_stats_;
        cameras = workers_.size();
    }
    // Without /proc/meminfo (not Linux) there is nothing to judge.
    if (stats.mem_total_kb <= 0 || options_.min_available_ram_mb <= 0) {
        return;
    }
    const std::int64_t available_mb = stats.mem_available_kb / 1024;
    if (available_mb >= options_.min_available_ram_mb) {
        return;
    }
    const std::int64_t now = monotonicMs();
    if (last_memory_warning_ms_ != 0 && now - last_memory_warning_ms_ < kMemoryWarningIntervalMs) {
        return;
    }
    last_memory_warning_ms_ = now;
    DiagnosticContext context;
    context.stage = Stage::kSystem;
    reportCameraError(CameraError::kOutOfMemoryRisk, context,
                      "available " + std::to_string(available_mb) + " MB of " +
                          std::to_string(stats.mem_total_kb / 1024) + " MB, threshold " +
                          std::to_string(options_.min_available_ram_mb) + " MB; process RSS " +
                          std::to_string(stats.process_rss_kb / 1024) + " MB; " +
                          std::to_string(cameras) + " cameras active");
}

}  // namespace anpr::cameras
