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
    std::int64_t timestamp_ms{0};
    BoundingBox plate_box;
    std::string camera_id;
    std::optional<std::string> region_code;
    std::optional<std::string> region_name;
    std::string format_name;
    std::int64_t recognition_latency_ms{0};
    int observation_count{0};
    int agreeing_observations{0};
    std::optional<std::string> best_crop_path;
    RecognitionStatus status{RecognitionStatus::kNoPlate};
};

std::string toJson(const PlateRecognitionEvent& event);

}  // namespace anpr
