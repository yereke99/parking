#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "anpr/common/geometry.hpp"
#include "anpr/common/logging.hpp"

namespace anpr {

enum class CameraKind { kAuto, kFile, kDevice, kRtsp, kGStreamer };

struct CameraConfig {
    std::string source{"video/parking.mp4"};
    CameraKind kind{CameraKind::kAuto};
    std::string camera_id{"gate-01"};
    int width{1920};
    int height{1080};
    double fps{25.0};
    /// RTSP transport hint applied through the GStreamer/FFmpeg backend.
    bool rtsp_tcp{true};
    /// Number of frames the driver buffers. 1 keeps latency low on USB and RTSP cameras.
    int capture_buffer_size{1};
    std::int64_t reconnect_initial_backoff_ms{500};
    std::int64_t reconnect_max_backoff_ms{15000};
    /// A read that produces nothing for this long is treated as a dead stream.
    std::int64_t read_timeout_ms{5000};
    /// Video files loop instead of ending. Useful for soak tests, off by default.
    bool loop_file{false};
    /// Replay video files at their own frame rate instead of as fast as they decode. Without
    /// this a file races ahead of the pipeline and almost every frame is dropped, which makes
    /// the live path behave nothing like it does on a camera. The benchmark tool bypasses the
    /// capture thread entirely and is unaffected.
    bool realtime_file{true};
    /// Video files only: the capture thread waits for the pipeline to take each frame instead
    /// of replacing it, so every frame of the clip is processed even when inference is slower
    /// than the clip. Live sources always keep only the newest frame.
    bool process_every_file_frame{false};
};

enum class InferenceBackend { kAuto, kTensorRT, kOnnxCuda, kOnnxCpu, kOpenCvDnn };

std::string toString(InferenceBackend backend);
InferenceBackend inferenceBackendFromString(const std::string& text, bool& ok);

struct InferenceConfig {
    InferenceBackend backend{InferenceBackend::kAuto};
    /// TensorRT and CUDA device ordinal.
    int device_id{0};
    /// FP16 for the TensorRT execution provider. Ignored by the CPU provider.
    bool fp16{true};
    /// Directory holding serialised TensorRT engines. Engines are built once by the
    /// model-build tool and reused, so normal startup does not pay the build cost.
    std::string engine_cache_dir{"var/trt_cache"};
    /// Refuse to run if the requested backend is unavailable, instead of falling back.
    bool strict_backend{false};
    int intra_op_threads{2};
    int inter_op_threads{1};
};

struct RoiConfig {
    /// Cheap frame-difference monitoring zone, evaluated on every frame while idle.
    NormalizedRect motion{0.0, 0.15, 1.0, 0.75};
    /// Region fed to the plate detector. Everything outside is never looked at.
    NormalizedRect detection{0.05, 0.15, 0.90, 0.70};
    /// A tracked plate whose centre lands here counts as near the barrier.
    NormalizedRect near_barrier{0.10, 0.25, 0.80, 0.60};
    /// Stop confirmation only counts inside this zone.
    NormalizedRect stop{0.10, 0.28, 0.80, 0.55};
    /// OCR only runs on plates whose centre is inside this zone.
    NormalizedRect recognition{0.05, 0.20, 0.90, 0.65};
};

struct MotionConfig {
    /// Working width for the differencing image. Smaller is cheaper and less noisy.
    int frame_width{320};
    /// Per-pixel absolute difference above which a pixel counts as changed.
    int pixel_threshold{24};
    /// Fraction of changed pixels that counts as vehicle-scale motion.
    double threshold{0.030};
    /// Fraction below which the scene counts as quiet.
    double quiet_threshold{0.006};
    /// How far back the comparison frame is taken. Differencing adjacent frames measures speed
    /// and misses a slow approach; comparing across an interval measures displacement.
    std::int64_t reference_interval_ms{200};
    /// Exponential smoothing weight for the new sample.
    double smoothing{0.35};
    /// Motion must persist this long before the state machine leaves idle.
    std::int64_t min_motion_ms{250};
};

struct DetectorConfig {
    std::string model{"models/license_plate_detector.onnx"};
    int input_size{640};
    double confidence_threshold{0.50};
    double nms_threshold{0.45};
    /// YOLO letterbox padding value. Matches the Ultralytics export convention.
    int letterbox_pad{114};
    bool swap_rb{true};
    /// Minimum detector interval per state. The detector never runs faster than this.
    std::int64_t interval_idle_ms{200};        // 5 FPS
    std::int64_t interval_approaching_ms{100};  // 10 FPS
    std::int64_t interval_near_ms{66};          // ~15 FPS
    std::int64_t interval_recognition_ms{40};   // 25 FPS
    std::int64_t interval_cooldown_ms{300};
    /// While idle, skip the detector entirely until the motion detector sees something.
    bool require_motion_in_idle{true};
};

struct OcrConfig {
    /// Nomeroff Net 4.0.1's Kazakhstan text reader as ONNX, made by tools/export_nomeroff_onnx.py.
    /// The Jetson image builds it at /opt/kz-anpr/models/nomeroff-onnx/kz.onnx.
    std::string model{"models/nomeroff-onnx/kz.onnx"};
    /// Reject an OCR reading whose mean character confidence is below this.
    double min_confidence{0.55};
    /// Reject a reading whose weakest character is below this.
    double min_char_confidence{0.30};
    /// Upper bound on OCR calls in one recognition session (one plate track). Caps the
    /// worst-case GPU cost.
    int max_attempts{12};
    /// Best-frame selection: within each window of this length the sharpest acceptable crop of a
    /// plate is kept and read once, so a vehicle or camera in motion spends its OCR calls on the
    /// sharpest views instead of every blurred one. 0 reads every acceptable crop at the
    /// detector cadence.
    std::int64_t min_interval_ms{0};
};

struct QualityConfig {
    /// Crops narrower or shorter than this never reach OCR.
    int min_plate_width_px{90};
    int min_plate_height_px{22};
    /// Laplacian variance below this counts as blurred.
    double min_sharpness{45.0};
    /// Mean intensity limits. Outside them the crop is too dark or too washed out.
    double min_brightness{35.0};
    double max_brightness{225.0};
    /// Fraction of fully black plus fully white pixels tolerated.
    double max_clipping_ratio{0.22};
    /// Combined score a crop must reach to be worth an OCR call.
    double min_score{0.30};
    /// Reference values used to normalise the individual scores into [0, 1].
    double sharpness_reference{160.0};
    double contrast_reference{48.0};
    double brightness_reference{135.0};
    /// Optional lightweight CLAHE pass, only applied to crops that pass the hard gates but
    /// score below `enhance_below_score`. Off by default; measure before enabling.
    bool enable_enhancement{false};
    double enhance_below_score{0.45};
    double clahe_clip_limit{2.0};
};

struct TrackingConfig {
    /// Minimum IoU for a detection to continue an existing track.
    double min_iou{0.20};
    /// Fallback association distance, as a fraction of the frame diagonal.
    double max_center_distance_ratio{0.06};
    /// A track with no detection for this long is dropped.
    std::int64_t max_age_ms{1200};
    /// Detections needed before a track is treated as real.
    int min_hits{2};
    /// Exponential smoothing weight applied to the box on each update.
    double box_smoothing{0.55};
};

struct StopDetectionConfig {
    /// Rolling window over which movement is measured.
    std::int64_t window_ms{600};
    /// Movement must stay under the thresholds for this long to count as stopped.
    std::int64_t stop_duration_ms{500};
    /// Centre displacement across the window, as a fraction of the plate box width.
    double max_center_displacement_ratio{0.18};
    /// Relative box width change across the window.
    double max_size_change_ratio{0.12};
    /// Absolute centre speed ceiling in pixels per second.
    double max_speed_px_per_s{40.0};
};

struct ConsensusConfig {
    /// Valid observations needed before any result can be accepted.
    int min_samples{3};
    /// Observations that must agree on the winning string.
    int required_votes{3};
    /// Winning string's share of the total valid weight.
    double min_agreement{0.60};
    /// Mean OCR confidence of the winning observations.
    double min_avg_confidence{0.65};
    /// Final combined confidence needed to accept.
    double min_final_confidence{0.68};
    /// Accept after a single observation only when explicitly enabled and very confident.
    bool allow_single_frame{false};
    double single_frame_confidence{0.95};
};

struct RecognitionConfig {
    /// false: a plate is read as soon as its tracked box is inside roi.recognition and large
    /// enough to read, whether the vehicle or the camera is moving or not. true: the strict
    /// barrier behaviour, where reading starts only after a confirmed stop inside roi.stop.
    bool require_stop{false};
    /// Plates read at the same time, each with its own vote. Bounds the OCR calls and memory of
    /// a scene with several vehicles.
    int max_concurrent_plates{3};
    /// A plate session ends early after this many reads in a row without a valid Kazakhstan
    /// plate (a sticker, a sign, a plate too oblique to read), so it stops costing OCR calls.
    /// 0 disables the limit; ocr.max_attempts still applies.
    int max_unreadable_reads{0};
    /// Budget for one plate's recognition session, from the moment it became readable.
    std::int64_t timeout_ms{3000};
    /// Quiet period after a result before the same vehicle can trigger again.
    std::int64_t cooldown_ms{4000};
    /// Motion must stay quiet this long after the vehicle leaves before returning to idle.
    std::int64_t leave_confirmation_ms{800};
    /// A track that disappears for this long during approach returns the machine to idle.
    std::int64_t track_lost_timeout_ms{1500};
    /// Suppress a repeated event for the same plate within this window.
    std::int64_t duplicate_suppression_ms{10000};
};

/// One plate layout. `pattern` uses single-character slot classes:
///   D  digit
///   L  letter from `letters`
///   R  region-code digit, validated against the region table
/// Example: Kazakhstan private cars are `DDDLLLRR` for `123ABC02`.
struct PlateFormat {
    std::string name;
    std::string pattern;
    /// Allow position-aware confusion corrections inside this format.
    bool allow_corrections{true};
    /// Confidence factor applied when this format matches. Lets a rarer legacy layout be
    /// ranked below the current one without rejecting it.
    double weight{1.0};
};

struct ValidationConfig {
    /// Letters permitted in `L` slots.
    std::string letters{"ABCDEFGHIJKLMNOPQRSTUVWXYZ"};
    std::vector<PlateFormat> formats;
    /// Region codes accepted in `R` slots, mapped to a human-readable name.
    std::map<std::string, std::string> regions;
    /// Character to substitute when a digit slot holds a letter.
    std::map<char, char> digit_confusions;
    /// Character to substitute when a letter slot holds a digit.
    std::map<char, char> letter_confusions;
    /// Corrections allowed in one plate before the reading is called ambiguous.
    int max_corrections{2};
    /// OCR confidence needed for a corrected reading to still count as high confidence.
    double high_confidence_threshold{0.80};
    /// Confidence multipliers by number of applied corrections, index 0 meaning none.
    std::vector<double> correction_multipliers{1.0, 0.92, 0.82};
};

struct DebugConfig {
    bool visualize{false};
    bool save_crops{false};
    std::string output_dir{"debug_frames"};
    int max_files{500};
    /// Emit one line per processed frame. Very noisy, benchmark use only.
    bool frame_timeline{false};
};

struct PerformanceConfig {
    /// OpenCV's internal thread pool. Small values keep CPU headroom on the edge device.
    int opencv_threads{2};
    /// Latest-frame slot. Fixed at 1 so latency never accumulates behind the pipeline.
    int frame_queue_size{1};
    /// Interval for the periodic metrics line. Zero disables it.
    std::int64_t metrics_interval_ms{60000};
};

struct LoggingConfig {
    LogLevel level{LogLevel::kInfo};
};

struct AnprConfig {
    CameraConfig camera;
    InferenceConfig inference;
    RoiConfig roi;
    MotionConfig motion;
    DetectorConfig detector;
    OcrConfig ocr;
    QualityConfig quality;
    TrackingConfig tracking;
    StopDetectionConfig stop_detection;
    ConsensusConfig consensus;
    RecognitionConfig recognition;
    ValidationConfig validation;
    DebugConfig debug;
    PerformanceConfig performance;
    LoggingConfig logging;
};

struct ConfigLoadResult {
    AnprConfig config;
    bool ok{true};
    std::string error;
    /// Keys present in the file that the loader does not know about. Reported so a typo in a
    /// threshold name is visible instead of silently keeping the default.
    std::vector<std::string> unknown_keys;
};

/// Returns the built-in Kazakhstan validation rules, used when the file omits `validation`.
ValidationConfig defaultKazakhstanValidation();

ConfigLoadResult loadConfigFile(const std::string& path);
ConfigLoadResult loadConfigText(const std::string& text);
bool validateConfig(const AnprConfig& config, std::string& error);

}  // namespace anpr
