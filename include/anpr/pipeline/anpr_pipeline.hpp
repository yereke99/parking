#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include "anpr/camera/frame_pump.hpp"
#include "anpr/common/config.hpp"
#include "anpr/common/metrics.hpp"
#include "anpr/detection/plate_detector.hpp"
#include "anpr/ocr/plate_ocr.hpp"
#include "anpr/pipeline/motion_detector.hpp"
#include "anpr/pipeline/plate_consensus.hpp"
#include "anpr/pipeline/plate_sink.hpp"
#include "anpr/pipeline/quality_assessor.hpp"
#include "anpr/pipeline/vehicle_state_machine.hpp"
#include "anpr/tracking/plate_tracker.hpp"
#include "anpr/tracking/stop_detector.hpp"

namespace anpr {

/// Event-driven recognition pipeline.
///
/// One processing thread. Every frame pays for a downscaled ROI frame difference and nothing
/// else until the state machine says otherwise. The detector runs at the cadence the current
/// state deserves, OCR runs only inside a recognition session, and the session ends as soon as
/// the consensus has enough evidence.
class AnprPipeline {
public:
    AnprPipeline(AnprConfig config, PlateSink& sink);
    ~AnprPipeline();

    AnprPipeline(const AnprPipeline&) = delete;
    AnprPipeline& operator=(const AnprPipeline&) = delete;

    /// Loads the detector and the OCR model. Call once before `run`.
    bool loadModels(std::string& error);
    /// Injects a detector, for the benchmark's detector-only mode and for tests.
    void setDetector(std::unique_ptr<IPlateDetector> detector);
    void setOcr(std::unique_ptr<IPlateOcr> ocr);

    /// Runs one detector and one OCR inference over blank buffers.
    ///
    /// On Jetson the TensorRT execution provider builds its engine on the first inference, which
    /// takes minutes. Doing that here, from the deployment tool, means the serialised engine is
    /// already in the cache and a real start does not pay for it. It also proves at deploy time
    /// that both models actually execute on the selected backend.
    bool warmup(std::string& error);

    /// Processes frames until `stop_requested` or the source ends.
    void run(FramePump& pump, std::atomic_bool& stop_requested);
    /// Processes one frame. Exposed so the benchmark can drive the pipeline itself.
    void processFrame(const Frame& frame);

    [[nodiscard]] const PipelineMetrics& metrics() const { return metrics_; }
    [[nodiscard]] PipelineMetrics& metrics() { return metrics_; }
    [[nodiscard]] VehicleState state() const { return state_machine_.state(); }
    [[nodiscard]] std::string backendSummary() const;

private:
    AnprConfig config_;
    PlateSink& sink_;
    PipelineMetrics metrics_;

    std::unique_ptr<IPlateDetector> detector_;
    std::unique_ptr<IPlateOcr> ocr_;

    RoiMotionDetector motion_detector_;
    PlateTracker tracker_;
    StopDetector stop_detector_;
    VehicleStateMachine state_machine_;
    QualityAssessor quality_assessor_;
    PlateConsensus consensus_;

    std::vector<Detection> mapped_detections_;
    cv::Mat enhanced_crop_;
    cv::Mat visualization_;

    std::int64_t last_detector_ms_{std::numeric_limits<std::int64_t>::min() / 4};
    std::int64_t last_metrics_ms_{0};
    std::int64_t recognition_started_ms_{0};
    int recognition_track_id_{-1};
    int ocr_attempts_{0};
    int saved_crop_count_{0};
    std::string last_emitted_plate_;
    std::int64_t last_emitted_ms_{0};

    [[nodiscard]] std::int64_t detectorIntervalMs(VehicleState state) const;
    [[nodiscard]] bool shouldRunDetector(VehicleState state, std::int64_t now_ms,
                                         const MotionSample& motion) const;
    void runDetector(const cv::Mat& frame, std::int64_t now_ms);
    [[nodiscard]] TrackObservation observeTrack(const cv::Mat& frame, std::int64_t now_ms);
    void beginRecognition(std::int64_t now_ms, int track_id);
    void runRecognitionTick(const cv::Mat& frame, std::int64_t now_ms);
    void finishRecognition(std::int64_t now_ms, bool timed_out);
    std::optional<std::string> saveDebugCrop(const cv::Mat& crop, std::int64_t now_ms, int index);
    void reportMetrics(std::int64_t now_ms, bool force);
    void drawOverlay(const cv::Mat& frame, const TrackObservation& track);
};

}  // namespace anpr
