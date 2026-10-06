#include "anpr/pipeline/anpr_pipeline.hpp"

#include <algorithm>
#include <chrono>
#include <sstream>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#ifdef KZ_ANPR_WITH_HIGHGUI
#include <opencv2/highgui.hpp>
#endif

#include "anpr/common/filesystem.hpp"
#include "anpr/common/logging.hpp"
#include "anpr/ocr/plate_ocr.hpp"

namespace anpr {
namespace {

std::int64_t monotonicMs() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

cv::Rect toRect(const BoundingBox& box) {
    return cv::Rect(box.x, box.y, box.width, box.height);
}

}  // namespace

AnprPipeline::AnprPipeline(AnprConfig config, PlateSink& sink)
    : config_(std::move(config)),
      sink_(sink),
      motion_detector_(config_.motion, config_.roi.motion),
      tracker_(config_.tracking),
      stop_detector_(config_.stop_detection),
      state_machine_(config_.motion, config_.recognition),
      quality_assessor_(config_.quality),
      consensus_(PlateValidator(config_.validation), config_.consensus) {
    mapped_detections_.reserve(16);
#ifndef KZ_ANPR_WITH_HIGHGUI
    if (config_.debug.visualize) {
        logEvent(LogLevel::kWarn, "visualization_unavailable",
                 LogFields().add("reason", "this build has no OpenCV highgui"));
        config_.debug.visualize = false;
    }
#endif
}

AnprPipeline::~AnprPipeline() {
#ifdef KZ_ANPR_WITH_HIGHGUI
    if (config_.debug.visualize) {
        cv::destroyAllWindows();
    }
#endif
}

bool AnprPipeline::loadModels(std::string& error) {
    if (detector_ == nullptr) {
        detector_ = makePlateDetector(config_.detector, config_.inference, &metrics_, error);
        if (detector_ == nullptr) {
            return false;
        }
    }
    if (ocr_ == nullptr) {
        ocr_ = makePlateOcr(config_.ocr, config_.inference, &metrics_, error);
        if (ocr_ == nullptr) {
            return false;
        }
    }
    logEvent(LogLevel::kInfo, "models_ready",
             LogFields()
                 .add("detector_backend", detector_->backendName())
                 .add("ocr_backend", ocr_->backendName())
                 .add("ocr_model", ocr_->modelDescription()));
    return true;
}

void AnprPipeline::setDetector(std::unique_ptr<IPlateDetector> detector) {
    detector_ = std::move(detector);
}

void AnprPipeline::setOcr(std::unique_ptr<IPlateOcr> ocr) {
    ocr_ = std::move(ocr);
}

bool AnprPipeline::warmup(std::string& error) {
    if (detector_ == nullptr || ocr_ == nullptr) {
        error = "warmup requires both models to be loaded";
        return false;
    }

    const int width = std::max(64, config_.camera.width);
    const int height = std::max(64, config_.camera.height);
    const cv::Mat blank_frame(height, width, CV_8UC3, cv::Scalar(114, 114, 114));

    const auto detector_started = std::chrono::steady_clock::now();
    detector_->detect(blank_frame);
    const double detector_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - detector_started)
            .count();

    const cv::Mat blank_crop(64, 192, CV_8UC3, cv::Scalar(114, 114, 114));
    const auto ocr_started = std::chrono::steady_clock::now();
    ocr_->recognize(blank_crop);
    const double ocr_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - ocr_started)
            .count();

    logEvent(LogLevel::kInfo, "warmup_complete",
             LogFields()
                 .add("detector_backend", detector_->backendName())
                 .add("detector_first_call_ms", detector_ms)
                 .add("ocr_backend", ocr_->backendName())
                 .add("ocr_first_call_ms", ocr_ms));
    // The warm-up call is not a real measurement: it includes the engine build. Clearing keeps
    // it out of the running metrics.
    metrics_ = PipelineMetrics{};
    return true;
}

std::string AnprPipeline::backendSummary() const {
    std::ostringstream out;
    out << "detector=" << (detector_ ? detector_->backendName() : "none")
        << " ocr=" << (ocr_ ? ocr_->backendName() : "none");
    return out.str();
}

std::int64_t AnprPipeline::detectorIntervalMs(VehicleState state) const {
    switch (state) {
        case VehicleState::kIdle:
            return config_.detector.interval_idle_ms;
        case VehicleState::kApproaching:
            return config_.detector.interval_approaching_ms;
        case VehicleState::kNear:
        case VehicleState::kStopped:
            return config_.detector.interval_near_ms;
        case VehicleState::kRecognizing:
            return config_.detector.interval_recognition_ms;
        case VehicleState::kConfirmed:
        case VehicleState::kCooldown:
            return config_.detector.interval_cooldown_ms;
    }
    return config_.detector.interval_idle_ms;
}

bool AnprPipeline::shouldRunDetector(VehicleState state, std::int64_t now_ms,
                                     const MotionSample& motion) const {
    if (detector_ == nullptr) {
        return false;
    }
    if (now_ms - last_detector_ms_ < detectorIntervalMs(state)) {
        return false;
    }
    // The cheapest possible idle: with nothing moving in front of the barrier, the detector is
    // never invoked at all and the frame costs one downscaled difference.
    //
    // The gate is the quiet floor, not the vehicle-scale threshold. A vehicle far from the
    // camera moves few pixels, so waiting for full vehicle-scale motion would miss the approach
    // entirely. Anything above the quiet floor is worth one detector call at the idle cadence;
    // `motion.threshold` remains the stronger signal the state machine uses to leave idle on
    // motion alone.
    if (state == VehicleState::kIdle && config_.detector.require_motion_in_idle &&
        motion.initialized && motion.score < config_.motion.quiet_threshold) {
        return false;
    }
    return true;
}

void AnprPipeline::runDetector(const cv::Mat& frame, std::int64_t now_ms) {
    mapped_detections_.clear();

    const BoundingBox roi = config_.roi.detection.toPixels(frame.cols, frame.rows);
    const cv::Rect roi_rect = toRect(roi);
    if (roi_rect.empty()) {
        return;
    }

    // A view onto the frame, not a copy. Only the detection zone is ever fed to the network.
    const cv::Mat view = frame(roi_rect);
    const std::vector<Detection>& detections = detector_->detect(view);
    for (const Detection& detection : detections) {
        Detection mapped = detection;
        mapped.box.x += roi.x;
        mapped.box.y += roi.y;
        mapped.box = clampBox(mapped.box, frame.cols, frame.rows);
        if (!mapped.box.empty()) {
            mapped_detections_.push_back(mapped);
        }
    }
    last_detector_ms_ = now_ms;
}

TrackObservation AnprPipeline::observeTrack(const cv::Mat& frame, std::int64_t now_ms) {
    TrackObservation observation;
    const TrackedPlate* track = tracker_.primary();
    if (track == nullptr) {
        stop_detector_.reset();
        return observation;
    }

    observation.present = true;
    observation.id = track->id;
    observation.in_detection_zone =
        centerInside(track->box, config_.roi.detection, frame.cols, frame.rows);
    observation.in_near_zone =
        centerInside(track->box, config_.roi.near_barrier, frame.cols, frame.rows);
    observation.in_stop_zone =
        centerInside(track->box, config_.roi.stop, frame.cols, frame.rows);
    observation.in_recognition_zone =
        centerInside(track->box, config_.roi.recognition, frame.cols, frame.rows);

    const StopState stop = stop_detector_.update(track->id, track->box, now_ms);
    observation.stopped = stop.stopped;
    observation.stationary_ms = stop.stationary_ms;
    observation.speed_px_per_s = stop.speed_px_per_s;
    return observation;
}

void AnprPipeline::beginRecognition(std::int64_t now_ms, int track_id) {
    consensus_.reset();
    recognition_started_ms_ = now_ms;
    recognition_track_id_ = track_id;
    ocr_attempts_ = 0;
    ++metrics_.recognition_sessions;
    state_machine_.markRecognitionActive();
    logEvent(LogLevel::kInfo, "recognition_started",
             LogFields().add("track_id", track_id).add("timestamp_ms", now_ms));
}

void AnprPipeline::runRecognitionTick(const cv::Mat& frame, std::int64_t now_ms) {
    if (ocr_ == nullptr) {
        return;
    }

    // Recognition belongs to the track that opened this session. Never mix observations from a
    // second visible plate into the same temporal vote. `last_box` is the raw current detection
    // after tracker.update, so the matching detection has effectively perfect overlap.
    const TrackedPlate* recognition_track = nullptr;
    for (const TrackedPlate& track : tracker_.tracks()) {
        if (track.id == recognition_track_id_) {
            recognition_track = &track;
            break;
        }
    }
    if (recognition_track == nullptr) {
        return;
    }

    int index = 0;
    for (const Detection& detection : mapped_detections_) {
        if (iou(detection.box, recognition_track->last_box) < 0.90) {
            continue;
        }
        if (ocr_attempts_ >= config_.ocr.max_attempts || consensus_.satisfied()) {
            return;
        }
        if (!centerInside(detection.box, config_.roi.recognition, frame.cols, frame.rows)) {
            ++metrics_.crops_rejected_roi;
            return;
        }

        const cv::Rect crop_rect = toRect(detection.box);
        if (crop_rect.empty()) {
            return;
        }
        const cv::Mat crop = frame(crop_rect);

        ImageQuality quality;
        {
            ScopedTimer timer(metrics_.quality_latency);
            quality = quality_assessor_.evaluate(crop, detection.box);
        }
        if (!quality.acceptable()) {
            if (quality.rejection == QualityRejection::kTooSmall) {
                ++metrics_.crops_rejected_size;
            } else {
                ++metrics_.crops_rejected_quality;
            }
            logEvent(LogLevel::kDebug, "ocr_crop_rejected",
                     LogFields()
                         .add("reason", toString(quality.rejection))
                         .add("width", detection.box.width)
                         .add("sharpness", quality.sharpness)
                         .add("brightness", quality.brightness));
            return;
        }

        const cv::Mat& ocr_input =
            quality_assessor_.enhance(crop, quality, enhanced_crop_) ? enhanced_crop_ : crop;

        // When benchmark/debug crop capture is enabled, retain rejected OCR attempts too. They
        // are the most useful samples for the failure buckets; production keeps this disabled.
        const std::optional<std::string> crop_path = saveDebugCrop(ocr_input, now_ms, index++);
        const OcrResult reading = ocr_->recognize(ocr_input);
        ++ocr_attempts_;
        if (!reading.ok()) {
            logEvent(LogLevel::kDebug, "ocr_rejected",
                     LogFields()
                         .add("reason", toString(reading.rejection))
                         .add("text", reading.text)
                         .add("confidence", reading.confidence));
            return;
        }

        PlateObservation observation;
        observation.raw_text = reading.text;
        observation.detector_confidence = detection.confidence;
        observation.ocr_confidence = reading.confidence;
        observation.min_char_confidence = reading.min_char_confidence;
        observation.image_quality = quality.score;
        observation.plate_box = detection.box;
        observation.timestamp_ms = now_ms;
        observation.crop_path = crop_path;

        PlateValidationStatus status;
        {
            ScopedTimer timer(metrics_.postprocess_latency);
            status = consensus_.add(observation);
        }
        if (status == PlateValidationStatus::kInvalidFormat ||
            status == PlateValidationStatus::kAmbiguous) {
            ++metrics_.observations_invalid_format;
        }
        const ConsensusResult current = consensus_.resolve();
        logEvent(LogLevel::kDebug, "ocr_candidate",
                 LogFields()
                     .add("raw", reading.text)
                     .add("normalized", current.normalized_plate)
                     .add("ocr_confidence", reading.confidence)
                     .add("min_char_confidence", reading.min_char_confidence)
                     .add("detector_confidence", detection.confidence)
                     .add("quality", quality.score)
                     .add("validation", toString(status))
                     .add("model_region", reading.region.empty() ? "none" : reading.region));
        return;
    }
}

void AnprPipeline::finishRecognition(std::int64_t now_ms, bool timed_out) {
    const ConsensusResult result = consensus_.resolve();
    const std::int64_t latency = now_ms - recognition_started_ms_;

    PlateRecognitionEvent event;
    event.normalized_plate = result.normalized_plate;
    event.raw_plate = result.raw_plate;
    event.confidence = result.confidence;
    event.timestamp_ms = now_ms;
    event.plate_box = result.plate_box;
    event.camera_id = config_.camera.camera_id;
    event.region_code = result.region_code;
    event.region_name = result.region_name;
    event.format_name = result.format_name;
    event.recognition_latency_ms = latency;
    event.observation_count = result.total_observations;
    event.agreeing_observations = result.agreeing_observations;
    event.best_crop_path = result.crop_path;
    event.status = result.status;
    if (timed_out && !isAccepted(result.status) && result.total_observations == 0) {
        event.status = RecognitionStatus::kTimeout;
    }

    const bool accepted = isAccepted(event.status);
    if (accepted) {
        ++metrics_.plates_confirmed;
        metrics_.recognition_latency.add(static_cast<double>(latency));
    }
    if (timed_out) {
        ++metrics_.recognition_timeouts;
    }

    // Second guard against duplicates. The state machine already refuses to retrigger for the
    // same tracked vehicle; this also covers a vehicle that leaves and returns immediately.
    const bool duplicate = accepted && !last_emitted_plate_.empty() &&
                           last_emitted_plate_ == event.normalized_plate &&
                           now_ms - last_emitted_ms_ < config_.recognition.duplicate_suppression_ms;

    logEvent(accepted ? LogLevel::kInfo : LogLevel::kWarn,
             accepted ? "plate_confirmed" : "recognition_failed",
             LogFields()
                 .add("status", toString(event.status))
                 .add("plate", event.normalized_plate)
                 .add("confidence", event.confidence)
                 .add("agreement", result.agreement)
                 .add("observations", result.total_observations)
                 .add("agreeing", result.agreeing_observations)
                 .add("ocr_calls", ocr_attempts_)
                 .add("latency_ms", latency)
                 .add("duplicate_suppressed", duplicate ? 1 : 0));

    if (!duplicate) {
        sink_.onRecognition(event);
    }
    if (accepted) {
        last_emitted_plate_ = event.normalized_plate;
        last_emitted_ms_ = now_ms;
    }

    state_machine_.markRecognitionFinished(now_ms, accepted, recognition_track_id_);
    recognition_track_id_ = -1;
}

std::optional<std::string> AnprPipeline::saveDebugCrop(const cv::Mat& crop, std::int64_t now_ms,
                                                       int index) {
    if (!config_.debug.save_crops || crop.empty()) {
        return std::nullopt;
    }
    std::error_code ignored;
    const filesystem::path directory = filesystem::path(config_.debug.output_dir) / "crops";
    filesystem::create_directories(directory, ignored);

    std::ostringstream name;
    name << config_.camera.camera_id << '_' << now_ms << '_' << index << ".jpg";
    const filesystem::path path = directory / name.str();
    if (!cv::imwrite(path.string(), crop)) {
        return std::nullopt;
    }

    // Bounded storage: prune the oldest files once the cap is exceeded, so a camera left running
    // for weeks cannot fill the device.
    if (++saved_crop_count_ > config_.debug.max_files) {
        std::vector<filesystem::directory_entry> files;
        for (const auto& entry : filesystem::directory_iterator(directory, ignored)) {
            if (filesystem::is_regular_file(entry.path(), ignored)) {
                files.push_back(entry);
            }
        }
        if (files.size() > static_cast<std::size_t>(config_.debug.max_files)) {
            std::sort(files.begin(), files.end(), [](const auto& lhs, const auto& rhs) {
                std::error_code error;
                return filesystem::last_write_time(lhs.path(), error) <
                       filesystem::last_write_time(rhs.path(), error);
            });
            const std::size_t remove_count =
                files.size() - static_cast<std::size_t>(config_.debug.max_files);
            for (std::size_t i = 0; i < remove_count; ++i) {
                filesystem::remove(files[i].path(), ignored);
            }
        }
        saved_crop_count_ = 0;
    }
    return path.string();
}

void AnprPipeline::reportMetrics(std::int64_t now_ms, bool force) {
    if (config_.performance.metrics_interval_ms <= 0 && !force) {
        return;
    }
    if (!force && now_ms - last_metrics_ms_ < config_.performance.metrics_interval_ms) {
        return;
    }
    last_metrics_ms_ = now_ms;
    Logger::instance().log(LogLevel::kInfo, "metrics", metrics_.summary());
}

void AnprPipeline::drawOverlay(const cv::Mat& frame, const TrackObservation& track) {
#ifdef KZ_ANPR_WITH_HIGHGUI
    frame.copyTo(visualization_);
    auto draw = [this](const NormalizedRect& roi, const cv::Scalar& color, const char* label) {
        const BoundingBox box = roi.toPixels(visualization_.cols, visualization_.rows);
        cv::rectangle(visualization_, toRect(box), color, 2);
        cv::putText(visualization_, label, cv::Point(box.x + 6, box.y + 24),
                    cv::FONT_HERSHEY_SIMPLEX, 0.6, color, 2);
    };
    draw(config_.roi.detection, cv::Scalar(120, 120, 120), "detection");
    draw(config_.roi.near_barrier, cv::Scalar(0, 200, 255), "near");
    draw(config_.roi.stop, cv::Scalar(0, 0, 255), "stop");

    for (const Detection& detection : mapped_detections_) {
        cv::rectangle(visualization_, toRect(detection.box), cv::Scalar(0, 255, 0), 2);
    }
    std::ostringstream label;
    label << toString(state_machine_.state());
    if (track.present) {
        label << " track=" << track.id << " stationary_ms=" << track.stationary_ms;
    }
    cv::putText(visualization_, label.str(), cv::Point(20, 40), cv::FONT_HERSHEY_SIMPLEX, 0.9,
                cv::Scalar(0, 255, 255), 2);
    cv::imshow("kz_anpr", visualization_);
    cv::waitKey(1);
#else
    (void)frame;
    (void)track;
#endif
}

void AnprPipeline::processFrame(const Frame& frame) {
    if (frame.image.empty()) {
        return;
    }
    const auto tick_started = std::chrono::steady_clock::now();
    ++metrics_.frames_processed;
    metrics_.capture_latency.add(static_cast<double>(monotonicMs() - frame.capture_ms));
    metrics_.decode_latency.add(frame.decode_ms);

    // Video files carry their own timeline so a benchmark run is reproducible; live sources use
    // the capture clock.
    const std::int64_t now_ms = frame.stream_ms;

    MotionSample motion;
    {
        ScopedTimer timer(metrics_.motion_latency);
        motion = motion_detector_.update(frame.image, now_ms);
    }

    const VehicleState state_before = state_machine_.state();
    const bool ran_detector = shouldRunDetector(state_before, now_ms, motion);
    if (ran_detector) {
        runDetector(frame.image, now_ms);
    } else {
        mapped_detections_.clear();
    }

    StateInput input;
    input.timestamp_ms = now_ms;
    input.motion_score = motion.score;
    {
        ScopedTimer timer(metrics_.tracking_latency);
        if (ran_detector) {
            tracker_.update(mapped_detections_, now_ms, frame.image.cols, frame.image.rows);
        } else {
            tracker_.age(now_ms);
        }
        input.track = observeTrack(frame.image, now_ms);
    }

    const StateUpdate update = state_machine_.update(input);
    if (update.changed) {
        logEvent(LogLevel::kInfo, "state_changed",
                 LogFields()
                     .add("from", toString(update.previous))
                     .add("to", toString(update.state))
                     .add("motion", motion.score)
                     .add("track", input.track.present ? input.track.id : 0)
                     .add("stationary_ms", input.track.stationary_ms));
    }
    if (config_.debug.frame_timeline) {
        logEvent(LogLevel::kDebug, "frame",
                 LogFields()
                     .add("ts_ms", now_ms)
                     .add("state", toString(state_machine_.state()))
                     .add("motion", motion.score)
                     .add("detector_ran", ran_detector ? 1 : 0)
                     .add("detections", mapped_detections_.size())
                     .add("speed_px_s", input.track.speed_px_per_s));
    }

    if (update.recognition_ready) {
        beginRecognition(now_ms, input.track.present ? input.track.id : -1);
    }

    if (state_machine_.state() == VehicleState::kRecognizing) {
        if (ran_detector) {
            runRecognitionTick(frame.image, now_ms);
        }
        const auto deadline = state_machine_.recognitionDeadlineMs();
        const bool timed_out = deadline && now_ms >= *deadline;
        const bool exhausted = ocr_attempts_ >= config_.ocr.max_attempts;
        if (consensus_.satisfied() || timed_out || exhausted) {
            finishRecognition(now_ms, timed_out || (exhausted && !consensus_.satisfied()));
        }
    }

    if (config_.debug.visualize) {
        drawOverlay(frame.image, input.track);
    }

    metrics_.frame_total.add(
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tick_started)
            .count());
    reportMetrics(monotonicMs(), false);
}

void AnprPipeline::run(FramePump& pump, std::atomic_bool& stop_requested) {
    Frame frame;
    last_metrics_ms_ = monotonicMs();

    while (!stop_requested.load()) {
        if (!pump.waitForFrame(frame, 200)) {
            if (pump.finished()) {
                break;
            }
            continue;
        }
        processFrame(frame);
    }

    const PumpStats pump_stats = pump.stats();
    metrics_.frames_captured = pump_stats.captured;
    metrics_.frames_dropped = pump_stats.dropped;
    metrics_.camera_reconnects = pump_stats.reconnects;
    reportMetrics(monotonicMs(), true);
}

}  // namespace anpr
