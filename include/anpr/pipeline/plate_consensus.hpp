#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "anpr/common/config.hpp"
#include "anpr/common/geometry.hpp"
#include "anpr/pipeline/recognition_event.hpp"
#include "anpr/validation/plate_validator.hpp"

namespace anpr {

/// One OCR reading of one crop, before validation.
struct PlateObservation {
    std::string raw_text;
    double detector_confidence{0.0};
    double ocr_confidence{0.0};
    /// Lowest per-character probability in the reading. A single weak character drags the whole
    /// observation's weight down even when the mean looks healthy.
    double min_char_confidence{1.0};
    double image_quality{0.0};
    BoundingBox plate_box;
    std::int64_t timestamp_ms{0};
    std::optional<std::string> crop_path;
};

struct ConsensusResult {
    std::string normalized_plate;
    std::string raw_plate;
    std::string format_name;
    std::optional<std::string> region_code;
    std::optional<std::string> region_name;
    double confidence{0.0};
    double agreement{0.0};
    int agreeing_observations{0};
    int total_observations{0};
    BoundingBox plate_box;
    std::optional<std::string> crop_path;
    RecognitionStatus status{RecognitionStatus::kNoPlate};
};

/// Accumulates OCR readings across frames and decides when there is enough evidence.
///
/// Observations are added as they arrive rather than batched at the end, so the pipeline can stop
/// spending GPU time the moment `satisfied()` turns true. A single reading is never enough unless
/// `consensus.allow_single_frame` is explicitly turned on.
///
/// Candidate weight combines detector confidence, mean OCR confidence, the weakest character in
/// the reading, crop quality and the validator's correction penalty. Frequency then decides
/// between candidates, which is what removes a one-off character flip.
class PlateConsensus {
public:
    PlateConsensus(PlateValidator validator, ConsensusConfig config);

    void reset();

    /// Validates and records one reading. Returns the validation status for logging.
    PlateValidationStatus add(const PlateObservation& observation);

    /// True once the acceptance criteria are met and OCR can stop.
    [[nodiscard]] bool satisfied() const;

    /// Best answer available right now. Used both on early stop and on timeout.
    [[nodiscard]] ConsensusResult resolve() const;

    [[nodiscard]] int totalObservations() const { return total_observations_; }
    [[nodiscard]] int validObservations() const;

private:
    struct Candidate {
        ValidatedPlate plate;
        std::string raw_text;
        double weight{0.0};
        double detector_sum{0.0};
        double ocr_sum{0.0};
        double min_char_sum{0.0};
        double quality_sum{0.0};
        int count{0};
        double best_weight{0.0};
        BoundingBox best_box;
        std::optional<std::string> best_crop_path;
    };

    PlateValidator validator_;
    ConsensusConfig config_;
    std::vector<Candidate> candidates_;
    double total_weight_{0.0};
    int total_observations_{0};
    bool saw_ambiguous_{false};
    bool saw_invalid_{false};

    [[nodiscard]] const Candidate* best() const;
    [[nodiscard]] double combinedConfidence(const Candidate& candidate, double agreement) const;
};

}  // namespace anpr
