#pragma once

#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
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
/// state deserves. OCR runs only inside recognition sessions, one per readable plate track, each
/// with its own vote, and each session ends as soon as its consensus has enough evidence. A
/// plate is readable once its box is inside roi.recognition and large enough; the vehicle does
/// not have to stop unless `recognition.require_stop` asks for it.
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
    /// Opt-in full-frame JPEGs for accepted events. Disabled for normal camera runs.
    void setSnapshotDirectory(std::string directory) { snapshot_directory_ = std::move(directory); }

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
    /// Ends every open recognition session with what it has (a clip ended or the run stops), so
    /// readings already made are reported instead of silently dropped. `run` calls it itself.
    void finishPendingRecognition();

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
    /// An empty vote, copied into every new plate session.
    PlateConsensus consensus_;

    /// One plate track being read: its own vote, its OCR budget, and the sharpest crop seen in
    /// the current best-frame window (a copy, since the frame buffer is reused).
    struct PlateSession {
        PlateSession(int id, std::int64_t now_ms, PlateConsensus vote)
            : track_id(id), started_ms(now_ms), last_seen_ms(now_ms), consensus(std::move(vote)) {}

        int track_id;
        std::int64_t started_ms;
        std::int64_t last_seen_ms;
        std::int64_t last_ocr_ms{std::numeric_limits<std::int64_t>::min() / 4};
        int ocr_attempts{0};
        /// Reads in a row that gave no valid plate, for recognition.max_unreadable_reads.
        int unreadable_streak{0};
        PlateConsensus consensus;
        bool has_candidate{false};
        cv::Mat candidate;
        /// Original frame behind the selected OCR crop; retained only when snapshots are enabled.
        cv::Mat candidate_frame;
        double candidate_score{0.0};
        Detection candidate_detection;
        ImageQuality candidate_quality;
        std::int64_t candidate_ms{0};
    };

    std::vector<Detection> mapped_detections_;
    cv::Mat enhanced_crop_;
    cv::Mat visualization_;
    std::string snapshot_directory_;

    std::int64_t last_detector_ms_{std::numeric_limits<std::int64_t>::min() / 4};
    std::int64_t last_metrics_ms_{0};
    std::int64_t last_frame_ms_{0};
    int saved_crop_count_{0};
    std::vector<PlateSession> sessions_;
    /// Tracks whose session has ended (read, given up or timed out). They are never read again
    /// while the tracker keeps them, which is the per-vehicle duplicate guard.
    std::vector<int> finished_tracks_;
    int last_finished_track_id_{-1};
    bool confirmed_in_phase_{false};
    /// Plate text -> time it was last emitted, for duplicate_suppression_ms.
    std::vector<std::pair<std::string, std::int64_t>> recent_plates_;

    [[nodiscard]] std::int64_t detectorIntervalMs(VehicleState state) const;
    [[nodiscard]] bool shouldRunDetector(VehicleState state, std::int64_t now_ms,
                                         const MotionSample& motion) const;
    void runDetector(const cv::Mat& frame, std::int64_t now_ms);
    [[nodiscard]] TrackObservation observeTrack(const cv::Mat& frame, std::int64_t now_ms);
    [[nodiscard]] bool isFinishedTrack(int track_id) const;
    [[nodiscard]] PlateSession* findSession(int track_id);
    /// A confirmed, not yet read track inside roi.recognition whose box is large enough to read.
    [[nodiscard]] bool isReadable(const TrackedPlate& track, int frame_width, int frame_height) const;
    void openSessions(int frame_width, int frame_height, std::int64_t now_ms);
    void collectCandidates(const cv::Mat& frame, std::int64_t now_ms);
    void readCandidates(std::int64_t now_ms);
    void finishSessions(std::int64_t now_ms, bool flush);
    void finishSession(PlateSession& session, std::int64_t now_ms, bool timed_out);
    void saveSnapshot(const PlateSession& session, PlateRecognitionEvent& event);
    std::optional<std::string> saveDebugCrop(const cv::Mat& crop, std::int64_t now_ms, int index);
    void reportMetrics(std::int64_t now_ms, bool force);
    void drawOverlay(const cv::Mat& frame, const TrackObservation& track);
};

}  // namespace anpr
