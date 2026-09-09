#include "anpr/common/config.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

#include "anpr/common/yaml.hpp"

namespace anpr {
namespace {

/// Reads typed values out of the YAML tree while recording which paths were understood, so
/// unrecognised keys can be reported instead of silently ignored.
class Reader {
public:
    Reader(const yaml::Node& root, std::string& error) : root_(root), error_(error) {}

    void get(const std::string& path, std::string& out) {
        const yaml::Node* node = consume(path);
        if (node == nullptr) {
            return;
        }
        if (const auto value = yaml::asString(node)) {
            out = *value;
        } else {
            fail(path, "expected a string");
        }
    }

    void get(const std::string& path, bool& out) {
        const yaml::Node* node = consume(path);
        if (node == nullptr) {
            return;
        }
        if (const auto value = yaml::asBool(node)) {
            out = *value;
        } else {
            fail(path, "expected true or false");
        }
    }

    void get(const std::string& path, int& out) {
        const yaml::Node* node = consume(path);
        if (node == nullptr) {
            return;
        }
        if (const auto value = yaml::asInt(node)) {
            out = static_cast<int>(*value);
        } else {
            fail(path, "expected an integer");
        }
    }

    void get(const std::string& path, std::int64_t& out) {
        const yaml::Node* node = consume(path);
        if (node == nullptr) {
            return;
        }
        if (const auto value = yaml::asInt(node)) {
            out = *value;
        } else {
            fail(path, "expected an integer");
        }
    }

    void get(const std::string& path, double& out) {
        const yaml::Node* node = consume(path);
        if (node == nullptr) {
            return;
        }
        if (const auto value = yaml::asDouble(node)) {
            out = *value;
        } else {
            fail(path, "expected a number");
        }
    }

    void get(const std::string& path, NormalizedRect& out) {
        const yaml::Node* node = consume(path);
        if (node == nullptr) {
            return;
        }
        if (!node->isSequence() || node->sequence().size() != 4) {
            fail(path, "expected [x, y, width, height]");
            return;
        }
        double values[4] = {0.0, 0.0, 0.0, 0.0};
        for (std::size_t i = 0; i < 4; ++i) {
            const auto value = yaml::asDouble(&node->sequence()[i]);
            if (!value) {
                fail(path, "expected four numbers");
                return;
            }
            values[i] = *value;
        }
        out = NormalizedRect{values[0], values[1], values[2], values[3]};
    }

    /// Marks a subtree as understood without reading it here.
    const yaml::Node* claimSubtree(const std::string& path) {
        claimed_subtrees_.insert(path);
        return root_.path(path);
    }

    [[nodiscard]] const std::set<std::string>& consumed() const { return consumed_; }
    [[nodiscard]] const std::set<std::string>& claimedSubtrees() const { return claimed_subtrees_; }
    [[nodiscard]] bool ok() const { return error_.empty(); }

private:
    const yaml::Node& root_;
    std::string& error_;
    std::set<std::string> consumed_;
    std::set<std::string> claimed_subtrees_;

    const yaml::Node* consume(const std::string& path) {
        consumed_.insert(path);
        const yaml::Node* node = root_.path(path);
        if (node == nullptr || node->isNull()) {
            return nullptr;
        }
        return node;
    }

    void fail(const std::string& path, const std::string& message) {
        if (error_.empty()) {
            error_ = "INVALID_CONFIG: " + path + ": " + message;
        }
    }
};

void collectLeafPaths(const yaml::Node& node, const std::string& prefix,
                      std::vector<std::string>& out) {
    if (node.isMap()) {
        for (const auto& [key, child] : node.map()) {
            const std::string path = prefix.empty() ? key : prefix + "." + key;
            if (child.isMap()) {
                collectLeafPaths(child, path, out);
            } else {
                out.push_back(path);
            }
        }
        return;
    }
    if (!prefix.empty()) {
        out.push_back(prefix);
    }
}

bool startsWithPath(const std::string& path, const std::string& prefix) {
    return path == prefix || (path.size() > prefix.size() && path.compare(0, prefix.size(), prefix) == 0 &&
                              path[prefix.size()] == '.');
}

CameraKind cameraKindFromString(const std::string& text, bool& ok) {
    ok = true;
    if (text == "auto") return CameraKind::kAuto;
    if (text == "file") return CameraKind::kFile;
    if (text == "device" || text == "usb") return CameraKind::kDevice;
    if (text == "rtsp") return CameraKind::kRtsp;
    if (text == "gstreamer") return CameraKind::kGStreamer;
    ok = false;
    return CameraKind::kAuto;
}

void readValidation(Reader& reader, ValidationConfig& validation, std::string& error) {
    reader.get("validation.letters", validation.letters);
    reader.get("validation.max_corrections", validation.max_corrections);
    reader.get("validation.high_confidence_threshold", validation.high_confidence_threshold);

    if (const yaml::Node* multipliers = reader.claimSubtree("validation.correction_multipliers");
        multipliers != nullptr && multipliers->isSequence()) {
        std::vector<double> parsed;
        for (const auto& item : multipliers->sequence()) {
            const auto value = yaml::asDouble(&item);
            if (!value) {
                error = "INVALID_CONFIG: validation.correction_multipliers must be numbers";
                return;
            }
            parsed.push_back(*value);
        }
        if (parsed.empty()) {
            error = "INVALID_CONFIG: validation.correction_multipliers must not be empty";
            return;
        }
        validation.correction_multipliers = std::move(parsed);
    }

    if (const yaml::Node* formats = reader.claimSubtree("validation.formats");
        formats != nullptr && formats->isSequence()) {
        std::vector<PlateFormat> parsed;
        for (const auto& item : formats->sequence()) {
            if (!item.isMap()) {
                error = "INVALID_CONFIG: validation.formats entries must be mappings";
                return;
            }
            PlateFormat format;
            const auto name = yaml::asString(item.find("name"));
            const auto pattern = yaml::asString(item.find("pattern"));
            if (!name || !pattern || pattern->empty()) {
                error = "INVALID_CONFIG: each validation.formats entry needs name and pattern";
                return;
            }
            format.name = *name;
            format.pattern = *pattern;
            if (const auto allow = yaml::asBool(item.find("allow_corrections"))) {
                format.allow_corrections = *allow;
            }
            if (const auto weight = yaml::asDouble(item.find("weight"))) {
                format.weight = *weight;
            }
            for (const char slot : format.pattern) {
                if (slot != 'D' && slot != 'L' && slot != 'R') {
                    error = "INVALID_CONFIG: validation.formats pattern '" + format.pattern +
                            "' uses an unknown slot class; allowed classes are D, L and R";
                    return;
                }
            }
            parsed.push_back(std::move(format));
        }
        if (parsed.empty()) {
            error = "INVALID_CONFIG: validation.formats must not be empty";
            return;
        }
        validation.formats = std::move(parsed);
    }

    if (const yaml::Node* regions = reader.claimSubtree("validation.regions");
        regions != nullptr && regions->isMap()) {
        std::map<std::string, std::string> parsed;
        for (const auto& [code, name] : regions->map()) {
            const auto label = yaml::asString(&name);
            if (!label) {
                error = "INVALID_CONFIG: validation.regions values must be strings";
                return;
            }
            parsed[code] = *label;
        }
        validation.regions = std::move(parsed);
    }

    auto readConfusions = [&](const std::string& path, std::map<char, char>& out) {
        const yaml::Node* node = reader.claimSubtree(path);
        if (node == nullptr || !node->isMap()) {
            return;
        }
        std::map<char, char> parsed;
        for (const auto& [from, to] : node->map()) {
            const auto target = yaml::asString(&to);
            if (from.size() != 1 || !target || target->size() != 1) {
                error = "INVALID_CONFIG: " + path + " entries must map one character to one character";
                return;
            }
            parsed[from[0]] = (*target)[0];
        }
        out = std::move(parsed);
    };
    readConfusions("validation.digit_confusions", validation.digit_confusions);
    readConfusions("validation.letter_confusions", validation.letter_confusions);
}

void readAll(Reader& reader, AnprConfig& config, std::string& error) {
    reader.get("camera.source", config.camera.source);
    std::string camera_kind = "auto";
    reader.get("camera.kind", camera_kind);
    bool kind_ok = true;
    config.camera.kind = cameraKindFromString(camera_kind, kind_ok);
    if (!kind_ok && error.empty()) {
        error = "INVALID_CONFIG: camera.kind must be auto, file, device, rtsp or gstreamer";
        return;
    }
    reader.get("camera.camera_id", config.camera.camera_id);
    reader.get("camera.width", config.camera.width);
    reader.get("camera.height", config.camera.height);
    reader.get("camera.fps", config.camera.fps);
    reader.get("camera.rtsp_tcp", config.camera.rtsp_tcp);
    reader.get("camera.capture_buffer_size", config.camera.capture_buffer_size);
    reader.get("camera.reconnect_initial_backoff_ms", config.camera.reconnect_initial_backoff_ms);
    reader.get("camera.reconnect_max_backoff_ms", config.camera.reconnect_max_backoff_ms);
    reader.get("camera.read_timeout_ms", config.camera.read_timeout_ms);
    reader.get("camera.loop_file", config.camera.loop_file);
    reader.get("camera.realtime_file", config.camera.realtime_file);

    std::string backend = toString(config.inference.backend);
    reader.get("inference.backend", backend);
    bool backend_ok = true;
    config.inference.backend = inferenceBackendFromString(backend, backend_ok);
    if (!backend_ok && error.empty()) {
        error = "INVALID_CONFIG: inference.backend must be auto, tensorrt, onnx_cuda, onnx_cpu or opencv_dnn";
        return;
    }
    reader.get("inference.device_id", config.inference.device_id);
    reader.get("inference.fp16", config.inference.fp16);
    reader.get("inference.engine_cache_dir", config.inference.engine_cache_dir);
    reader.get("inference.strict_backend", config.inference.strict_backend);
    reader.get("inference.intra_op_threads", config.inference.intra_op_threads);
    reader.get("inference.inter_op_threads", config.inference.inter_op_threads);

    reader.get("roi.motion", config.roi.motion);
    reader.get("roi.detection", config.roi.detection);
    reader.get("roi.near_barrier", config.roi.near_barrier);
    reader.get("roi.stop", config.roi.stop);
    reader.get("roi.recognition", config.roi.recognition);

    reader.get("motion.frame_width", config.motion.frame_width);
    reader.get("motion.pixel_threshold", config.motion.pixel_threshold);
    reader.get("motion.threshold", config.motion.threshold);
    reader.get("motion.quiet_threshold", config.motion.quiet_threshold);
    reader.get("motion.reference_interval_ms", config.motion.reference_interval_ms);
    reader.get("motion.smoothing", config.motion.smoothing);
    reader.get("motion.min_motion_ms", config.motion.min_motion_ms);

    reader.get("detector.model", config.detector.model);
    reader.get("detector.input_size", config.detector.input_size);
    reader.get("detector.confidence_threshold", config.detector.confidence_threshold);
    reader.get("detector.nms_threshold", config.detector.nms_threshold);
    reader.get("detector.letterbox_pad", config.detector.letterbox_pad);
    reader.get("detector.swap_rb", config.detector.swap_rb);
    reader.get("detector.interval_idle_ms", config.detector.interval_idle_ms);
    reader.get("detector.interval_approaching_ms", config.detector.interval_approaching_ms);
    reader.get("detector.interval_near_ms", config.detector.interval_near_ms);
    reader.get("detector.interval_recognition_ms", config.detector.interval_recognition_ms);
    reader.get("detector.interval_cooldown_ms", config.detector.interval_cooldown_ms);
    reader.get("detector.require_motion_in_idle", config.detector.require_motion_in_idle);

    reader.get("ocr.model", config.ocr.model);
    reader.get("ocr.plate_config", config.ocr.plate_config);
    reader.get("ocr.min_confidence", config.ocr.min_confidence);
    reader.get("ocr.min_char_confidence", config.ocr.min_char_confidence);
    reader.get("ocr.max_attempts", config.ocr.max_attempts);

    reader.get("quality.min_plate_width_px", config.quality.min_plate_width_px);
    reader.get("quality.min_plate_height_px", config.quality.min_plate_height_px);
    reader.get("quality.min_sharpness", config.quality.min_sharpness);
    reader.get("quality.min_brightness", config.quality.min_brightness);
    reader.get("quality.max_brightness", config.quality.max_brightness);
    reader.get("quality.max_clipping_ratio", config.quality.max_clipping_ratio);
    reader.get("quality.min_score", config.quality.min_score);
    reader.get("quality.sharpness_reference", config.quality.sharpness_reference);
    reader.get("quality.contrast_reference", config.quality.contrast_reference);
    reader.get("quality.brightness_reference", config.quality.brightness_reference);
    reader.get("quality.enable_enhancement", config.quality.enable_enhancement);
    reader.get("quality.enhance_below_score", config.quality.enhance_below_score);
    reader.get("quality.clahe_clip_limit", config.quality.clahe_clip_limit);

    reader.get("tracking.min_iou", config.tracking.min_iou);
    reader.get("tracking.max_center_distance_ratio", config.tracking.max_center_distance_ratio);
    reader.get("tracking.max_age_ms", config.tracking.max_age_ms);
    reader.get("tracking.min_hits", config.tracking.min_hits);
    reader.get("tracking.box_smoothing", config.tracking.box_smoothing);

    reader.get("stop_detection.window_ms", config.stop_detection.window_ms);
    reader.get("stop_detection.stop_duration_ms", config.stop_detection.stop_duration_ms);
    reader.get("stop_detection.max_center_displacement_ratio",
               config.stop_detection.max_center_displacement_ratio);
    reader.get("stop_detection.max_size_change_ratio", config.stop_detection.max_size_change_ratio);
    reader.get("stop_detection.max_speed_px_per_s", config.stop_detection.max_speed_px_per_s);

    reader.get("consensus.min_samples", config.consensus.min_samples);
    reader.get("consensus.required_votes", config.consensus.required_votes);
    reader.get("consensus.min_agreement", config.consensus.min_agreement);
    reader.get("consensus.min_avg_confidence", config.consensus.min_avg_confidence);
    reader.get("consensus.min_final_confidence", config.consensus.min_final_confidence);
    reader.get("consensus.allow_single_frame", config.consensus.allow_single_frame);
    reader.get("consensus.single_frame_confidence", config.consensus.single_frame_confidence);

    reader.get("recognition.timeout_ms", config.recognition.timeout_ms);
    reader.get("recognition.cooldown_ms", config.recognition.cooldown_ms);
    reader.get("recognition.leave_confirmation_ms", config.recognition.leave_confirmation_ms);
    reader.get("recognition.track_lost_timeout_ms", config.recognition.track_lost_timeout_ms);
    reader.get("recognition.duplicate_suppression_ms", config.recognition.duplicate_suppression_ms);

    readValidation(reader, config.validation, error);
    if (!error.empty()) {
        return;
    }

    reader.get("debug.visualize", config.debug.visualize);
    reader.get("debug.save_crops", config.debug.save_crops);
    reader.get("debug.output_dir", config.debug.output_dir);
    reader.get("debug.max_files", config.debug.max_files);
    reader.get("debug.frame_timeline", config.debug.frame_timeline);

    reader.get("performance.opencv_threads", config.performance.opencv_threads);
    reader.get("performance.frame_queue_size", config.performance.frame_queue_size);
    reader.get("performance.metrics_interval_ms", config.performance.metrics_interval_ms);

    std::string level = toString(config.logging.level);
    reader.get("logging.level", level);
    config.logging.level = logLevelFromString(level, config.logging.level);
}

}  // namespace

std::string toString(InferenceBackend backend) {
    switch (backend) {
        case InferenceBackend::kAuto:
            return "auto";
        case InferenceBackend::kTensorRT:
            return "tensorrt";
        case InferenceBackend::kOnnxCuda:
            return "onnx_cuda";
        case InferenceBackend::kOnnxCpu:
            return "onnx_cpu";
        case InferenceBackend::kOpenCvDnn:
            return "opencv_dnn";
    }
    return "auto";
}

InferenceBackend inferenceBackendFromString(const std::string& text, bool& ok) {
    ok = true;
    if (text == "auto") return InferenceBackend::kAuto;
    if (text == "tensorrt" || text == "trt") return InferenceBackend::kTensorRT;
    if (text == "onnx_cuda" || text == "cuda") return InferenceBackend::kOnnxCuda;
    if (text == "onnx_cpu" || text == "cpu") return InferenceBackend::kOnnxCpu;
    if (text == "opencv_dnn" || text == "opencv") return InferenceBackend::kOpenCvDnn;
    ok = false;
    return InferenceBackend::kAuto;
}

ValidationConfig defaultKazakhstanValidation() {
    ValidationConfig validation;
    validation.letters = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    validation.formats = {
        // 123 ABC 02, the current private-vehicle layout.
        PlateFormat{"current_individual", "DDDLLLRR", true, 1.0},
        // 123 AB 02, the current legal-entity layout.
        PlateFormat{"current_legal_entity", "DDDLLRR", true, 1.0},
        // A 123 ABC, the 1993 layout still seen on older vehicles.
        PlateFormat{"legacy_1993", "LDDDLLL", true, 0.9},
    };
    validation.regions = {
        {"01", "Astana"},
        {"02", "Almaty"},
        {"03", "Akmola region"},
        {"04", "Aktobe region"},
        {"05", "Almaty region"},
        {"06", "Atyrau region"},
        {"07", "West Kazakhstan region"},
        {"08", "Zhambyl region"},
        {"09", "Karaganda region"},
        {"10", "Kostanay region"},
        {"11", "Kyzylorda region"},
        {"12", "Mangystau region"},
        {"13", "Turkistan region"},
        {"14", "Pavlodar region"},
        {"15", "North Kazakhstan region"},
        {"16", "East Kazakhstan region"},
        {"17", "Shymkent"},
        {"18", "Abay region"},
        {"19", "Zhetysu region"},
        {"20", "Ulytau region"},
    };
    // Only glyph pairs that genuinely collide in plate fonts. Nothing else is ever substituted.
    validation.digit_confusions = {
        {'O', '0'}, {'Q', '0'}, {'D', '0'}, {'I', '1'}, {'L', '1'},
        {'Z', '2'}, {'S', '5'}, {'G', '6'}, {'B', '8'},
    };
    validation.letter_confusions = {
        {'0', 'O'}, {'1', 'I'}, {'2', 'Z'}, {'5', 'S'}, {'6', 'G'}, {'8', 'B'},
    };
    return validation;
}

ConfigLoadResult loadConfigText(const std::string& text) {
    ConfigLoadResult result;
    result.config.validation = defaultKazakhstanValidation();

    yaml::ParseResult parsed = yaml::parse(text);
    if (!parsed.ok) {
        result.ok = false;
        result.error = "INVALID_CONFIG: " + parsed.error;
        return result;
    }
    if (!parsed.root.isMap() && !parsed.root.isNull()) {
        result.ok = false;
        result.error = "INVALID_CONFIG: the document root must be a mapping";
        return result;
    }

    std::string error;
    Reader reader(parsed.root, error);
    readAll(reader, result.config, error);
    if (!error.empty()) {
        result.ok = false;
        result.error = error;
        return result;
    }

    std::vector<std::string> leaves;
    collectLeafPaths(parsed.root, {}, leaves);
    for (const std::string& leaf : leaves) {
        if (reader.consumed().count(leaf) > 0) {
            continue;
        }
        const bool claimed = std::any_of(
            reader.claimedSubtrees().begin(), reader.claimedSubtrees().end(),
            [&leaf](const std::string& prefix) { return startsWithPath(leaf, prefix); });
        if (!claimed) {
            result.unknown_keys.push_back(leaf);
        }
    }

    if (!validateConfig(result.config, result.error)) {
        result.ok = false;
    }
    return result;
}

ConfigLoadResult loadConfigFile(const std::string& path) {
    std::ifstream input(path);
    if (!input) {
        ConfigLoadResult result;
        result.ok = false;
        result.error = "INVALID_CONFIG: cannot open " + path;
        return result;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return loadConfigText(buffer.str());
}

bool validateConfig(const AnprConfig& config, std::string& error) {
    auto require = [&error](bool condition, const char* message) {
        if (!condition && error.empty()) {
            error = std::string("INVALID_CONFIG: ") + message;
        }
        return condition;
    };

    const NormalizedRect* rects[] = {&config.roi.motion, &config.roi.detection,
                                     &config.roi.near_barrier, &config.roi.stop,
                                     &config.roi.recognition};
    for (const NormalizedRect* rect : rects) {
        if (!require(rect->valid(),
                     "every roi entry must be normalized [x, y, width, height] inside the frame")) {
            return false;
        }
    }

    if (!require(config.motion.quiet_threshold < config.motion.threshold,
                 "motion.quiet_threshold must be below motion.threshold")) {
        return false;
    }
    if (!require(config.motion.frame_width >= 64, "motion.frame_width must be at least 64")) {
        return false;
    }
    if (!require(config.motion.smoothing > 0.0 && config.motion.smoothing <= 1.0,
                 "motion.smoothing must be in (0, 1]")) {
        return false;
    }
    if (!require(config.motion.reference_interval_ms > 0,
                 "motion.reference_interval_ms must be positive")) {
        return false;
    }
    if (!require(config.detector.input_size > 0 && config.detector.input_size % 32 == 0,
                 "detector.input_size must be a positive multiple of 32")) {
        return false;
    }
    if (!require(config.detector.confidence_threshold > 0.0 &&
                     config.detector.confidence_threshold < 1.0,
                 "detector.confidence_threshold must be in (0, 1)")) {
        return false;
    }
    if (!require(config.detector.interval_idle_ms >= config.detector.interval_recognition_ms,
                 "detector.interval_idle_ms must not be shorter than interval_recognition_ms")) {
        return false;
    }
    if (!require(config.ocr.max_attempts > 0, "ocr.max_attempts must be positive")) {
        return false;
    }
    if (!require(config.quality.min_plate_width_px > 0 && config.quality.min_plate_height_px > 0,
                 "quality plate size limits must be positive")) {
        return false;
    }
    if (!require(config.quality.min_brightness < config.quality.max_brightness,
                 "quality.min_brightness must be below quality.max_brightness")) {
        return false;
    }
    if (!require(config.tracking.min_hits >= 1, "tracking.min_hits must be at least 1")) {
        return false;
    }
    if (!require(config.tracking.box_smoothing > 0.0 && config.tracking.box_smoothing <= 1.0,
                 "tracking.box_smoothing must be in (0, 1]")) {
        return false;
    }
    if (!require(config.stop_detection.stop_duration_ms > 0 && config.stop_detection.window_ms > 0,
                 "stop_detection durations must be positive")) {
        return false;
    }
    if (!require(config.stop_detection.window_ms >= config.stop_detection.stop_duration_ms,
                 "stop_detection.window_ms must cover stop_duration_ms")) {
        return false;
    }
    if (!require(config.consensus.min_samples >= 1 && config.consensus.required_votes >= 1,
                 "consensus sample counts must be positive")) {
        return false;
    }
    if (!require(config.consensus.required_votes <= config.consensus.min_samples,
                 "consensus.required_votes cannot exceed consensus.min_samples")) {
        return false;
    }
    if (!require(config.recognition.timeout_ms > 0 && config.recognition.cooldown_ms > 0,
                 "recognition timing values must be positive")) {
        return false;
    }
    if (!require(!config.validation.formats.empty(), "validation.formats must not be empty")) {
        return false;
    }
    if (!require(!config.validation.letters.empty(), "validation.letters must not be empty")) {
        return false;
    }
    if (!require(config.validation.max_corrections >= 0,
                 "validation.max_corrections must not be negative")) {
        return false;
    }
    for (const PlateFormat& format : config.validation.formats) {
        if (format.pattern.find('R') != std::string::npos && config.validation.regions.empty()) {
            error = "INVALID_CONFIG: format '" + format.name +
                    "' uses region slots but validation.regions is empty";
            return false;
        }
    }
    if (!require(config.debug.max_files >= 0, "debug.max_files must not be negative")) {
        return false;
    }
    if (!require(!(config.debug.save_crops && config.debug.max_files == 0),
                 "debug.max_files must be positive when debug.save_crops is enabled")) {
        return false;
    }
    if (!require(config.performance.frame_queue_size == 1,
                 "performance.frame_queue_size is intentionally fixed at 1 so latency cannot "
                 "accumulate behind the pipeline")) {
        return false;
    }
    if (!require(config.performance.opencv_threads >= 1,
                 "performance.opencv_threads must be at least 1")) {
        return false;
    }
    if (!require(config.camera.reconnect_initial_backoff_ms > 0 &&
                     config.camera.reconnect_max_backoff_ms >=
                         config.camera.reconnect_initial_backoff_ms,
                 "camera reconnect backoff values are inconsistent")) {
        return false;
    }
    return true;
}

}  // namespace anpr
