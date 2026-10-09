#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "anpr/common/geometry.hpp"
#include "anpr/validation/plate_validator.hpp"

namespace anpr {

enum class RecognitionStatus {
    kValidHighConfidence,
    kValidLowConfidence,
    kInvalidFormat,
    kInsufficientImageQuality,
    kNoPlate,
    kAmbiguous,
    kLowConfidence,
    kTimeout,
};

std::string toString(RecognitionStatus status);
RecognitionStatus toRecognitionStatus(PlateValidationStatus status);

/// True when the barrier may act on the event.
[[nodiscard]] bool isAccepted(RecognitionStatus status);

/// The result of one recognition session. This is the whole public surface of the subsystem:
/// a barrier controller, an access-control service, an HTTP client or a database writer consumes
/// this and nothing else.
struct PlateRecognitionEvent {
    std::string normalized_plate;
    std::string raw_plate;
    double confidence{0.0};
    /// Pipeline time in milliseconds: the position in the clip for a video file, a monotonic
    /// clock for a live camera. It orders the events of one run.
    std::int64_t timestamp_ms{0};
    /// Wall-clock time the event was produced, in milliseconds since the Unix epoch. The JSON
    /// carries it as `time`, ISO 8601 in UTC.
    std::int64_t unix_time_ms{0};
    BoundingBox plate_box;
    std::string camera_id;
    std::optional<std::string> region_code;
    std::optional<std::string> region_name;
    std::string format_name;
    std::int64_t recognition_latency_ms{0};
    int observation_count{0};
    int agreeing_observations{0};
    std::optional<std::string> best_crop_path;
    /// Full original frame used for the read that confirmed the plate, when requested.
    std::optional<std::string> snapshot_path;
    std::optional<std::int64_t> snapshot_timestamp_ms;
    RecognitionStatus status{RecognitionStatus::kNoPlate};
};

std::string toJson(const PlateRecognitionEvent& event);

}  // namespace anpr
