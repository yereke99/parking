#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "anpr/camera/camera_source.hpp"
#include "anpr/cameras/status_store.hpp"
#include "anpr/cameras/system_stats.hpp"
#include "anpr/common/config.hpp"
#include "anpr/detection/plate_detector.hpp"
#include "anpr/hikvision/native_anpr.hpp"
#include "anpr/ocr/plate_ocr.hpp"
#include "anpr/pipeline/plate_sink.hpp"

namespace anpr::cameras {

/// One camera to process.
struct CameraWorkerSpec {
    std::string camera_id;
    std::unique_ptr<CameraSource> source;
    /// Recognition settings for this camera; `camera.camera_id` is set to `camera_id`.
    AnprConfig anpr;
    /// Live sources keep only the newest frames; files are processed frame by frame.
    bool live{true};
    std::size_t queue_capacity{1};
    /// Live only: frames older than this at pick-up are dropped as stale (0 disables).
    std::int64_t max_frame_age_ms{0};
    /// Static facts for the status table.
    std::string ip;
    std::string mac;
    std::string model;
    /// Start the camera's own ANPR listener next to the pipeline.
    bool native_anpr{false};
    hikvision::NativeAnprListener::Options native_anpr_options;
};

/// N cameras, one process, one copy of each model.
///
///   capture thread per camera (decode, latest-frames queue, reconnect)
///     -> processing thread per camera (motion, tracking, state machine, consensus: all
///        per camera, nothing shared)
///       -> shared TensorRT detector and shared Nomeroff OCR, each one engine, calls serialized
///     -> events tagged with camera_id
///
/// A camera that disconnects, stalls or fails only affects its own threads; the others keep
/// their GPU turns. Cameras can be added while running (a camera powered up later).
class MultiCameraRunner {
public:
    struct Options {
        std::string status_file;
        std::int64_t status_interval_ms{5000};
        std::int64_t metrics_interval_ms{60000};
        int min_available_ram_mb{300};
        /// Stop when every source has ended (video files). Live cameras never end.
        bool exit_when_all_finished{false};
        std::string camera_lan_summary;
        std::string internet_summary;
    };

    MultiCameraRunner(AnprConfig shared_config, Options options, std::shared_ptr<PlateSink> sink);
    ~MultiCameraRunner();
    MultiCameraRunner(const MultiCameraRunner&) = delete;
    MultiCameraRunner& operator=(const MultiCameraRunner&) = delete;

    /// Loads the detector and the OCR once and runs one warm-up inference on each. On failure
    /// `error` names the model and the backend; the caller reports TENSORRT_NOT_AVAILABLE.
    bool loadModels(std::string& error);
    /// Uses already built shared models instead of `loadModels` (tests with fake models).
    void setSharedModels(std::shared_ptr<SharedPlateDetectorCore> detector,
                         std::shared_ptr<SharedPlateOcrCore> ocr, std::string detector_backend,
                         std::string ocr_backend);
    [[nodiscard]] std::string detectorBackend() const;
    [[nodiscard]] std::string ocrBackend() const;

    /// Creates the camera's pipeline on the shared models and starts its threads.
    bool addCamera(CameraWorkerSpec spec, std::string& error);
    /// A discovered camera that is not processed (failed preflight, disabled, over the limit),
    /// listed in the status file so the operator sees it.
    void addInactiveCamera(const CameraStatus& status);
    /// True when a camera with this id is being processed.
    [[nodiscard]] bool hasCamera(const std::string& camera_id) const;
    [[nodiscard]] std::size_t activeCameraCount() const;

    /// Runs until `stop` is set or, with exit_when_all_finished, every source ended: writes the
    /// status file, logs per-camera and system summaries, warns on low memory. `periodic` is
    /// called about once a second from this thread (rediscovery, LAN link watch).
    void run(std::atomic_bool& stop, const std::function<void()>& periodic = {});
    void stopAll();

    [[nodiscard]] std::vector<CameraStatus> statuses() const;
    [[nodiscard]] std::int64_t platesConfirmed() const;
    [[nodiscard]] std::int64_t framesProcessed() const;

    /// Overrides the status of an inactive camera or the LAN summary after startup.
    void setNetworkSummary(std::string camera_lan, std::string internet);

private:
    class Worker;

    AnprConfig shared_config_;
    Options options_;
    std::shared_ptr<PlateSink> sink_;
    std::shared_ptr<SharedPlateDetectorCore> detector_core_;
    std::shared_ptr<SharedPlateOcrCore> ocr_core_;
    std::string detector_backend_;
    std::string ocr_backend_;

    mutable std::mutex workers_mutex_;
    std::vector<std::unique_ptr<Worker>> workers_;
    std::vector<CameraStatus> inactive_;
    std::string camera_lan_summary_;
    std::string internet_summary_;
    /// Read and written by the `run` thread; copied under `workers_mutex_` for the status file.
    SystemStatsReader stats_reader_;
    SystemStats last_stats_;
    /// After `stopAll` no camera is added and the status file says STOPPED.
    bool stopped_{false};
    /// `run` thread only.
    std::int64_t last_memory_warning_ms_{0};
    std::string last_status_error_;

    void writeStatus();
    void logSummaries();
    void checkMemory();
    void refreshSystemStats();
    [[nodiscard]] bool allFinished() const;
    [[nodiscard]] bool hasCameraLocked(const std::string& camera_id) const;
    [[nodiscard]] SystemStatus systemStatus() const;
};

}  // namespace anpr::cameras
