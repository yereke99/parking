#include "anpr/pipeline/plate_consensus.hpp"

#include <algorithm>
#include <cmath>

namespace anpr {
namespace {

double clamp01(double value) {
    return std::clamp(value, 0.0, 1.0);
}

}  // namespace

PlateConsensus::PlateConsensus(PlateValidator validator, ConsensusConfig config)
    : validator_(std::move(validator)), config_(config) {
    candidates_.reserve(4);
}

void PlateConsensus::reset() {
    candidates_.clear();
    total_weight_ = 0.0;
    total_observations_ = 0;
    saw_ambiguous_ = false;
    saw_invalid_ = false;
}

PlateValidationStatus PlateConsensus::add(const PlateObservation& observation) {
    ++total_observations_;
    ValidatedPlate plate = validator_.validate(observation.raw_text, observation.ocr_confidence);
    if (plate.status == PlateValidationStatus::kAmbiguous) {
        saw_ambiguous_ = true;
        return plate.status;
    }
    if (!plate.valid()) {
        saw_invalid_ = true;
        return plate.status;
    }

    // The weakest character matters more than the mean: one 0.2 character in an otherwise strong
    // reading is exactly the case multi-frame voting exists to catch.
    const double weight = clamp01(observation.detector_confidence) *
                          clamp01(observation.ocr_confidence) *
                          clamp01(observation.image_quality) *
                          clamp01(observation.min_char_confidence) * plate.confidence_multiplier;
    if (weight <= 0.0) {
        return plate.status;
    }

    auto it = std::find_if(candidates_.begin(), candidates_.end(), [&plate](const Candidate& c) {
        return c.plate.normalized == plate.normalized;
    });
    if (it == candidates_.end()) {
        candidates_.push_back(Candidate{});
        it = std::prev(candidates_.end());
        it->plate = plate;
        it->raw_text = observation.raw_text;
    }

    it->weight += weight;
    it->detector_sum += clamp01(observation.detector_confidence);
    it->ocr_sum += clamp01(observation.ocr_confidence);
    it->min_char_sum += clamp01(observation.min_char_confidence);
    it->quality_sum += clamp01(observation.image_quality);
    ++it->count;
    if (weight > it->best_weight) {
        it->best_weight = weight;
        it->best_box = observation.plate_box;
        it->best_crop_path = observation.crop_path;
        it->raw_text = observation.raw_text;
        // Keep the status from the strongest reading so a later weak-but-valid frame cannot
        // downgrade an already clean result.
        it->plate.status = plate.status;
    }
    total_weight_ += weight;
    return plate.status;
}

int PlateConsensus::validObservations() const {
    int count = 0;
    for (const Candidate& candidate : candidates_) {
        count += candidate.count;
    }
    return count;
}

const PlateConsensus::Candidate* PlateConsensus::best() const {
    const Candidate* best = nullptr;
    for (const Candidate& candidate : candidates_) {
        if (best == nullptr || candidate.weight > best->weight) {
            best = &candidate;
        }
    }
    return best;
}

double PlateConsensus::combinedConfidence(const Candidate& candidate, double agreement) const {
    const double count = static_cast<double>(std::max(1, candidate.count));
    const double detector_avg = candidate.detector_sum / count;
    const double ocr_avg = candidate.ocr_sum / count;
    const double quality_avg = candidate.quality_sum / count;
    const double min_char_avg = candidate.min_char_sum / count;
    const double evidence =
        0.30 * detector_avg + 0.35 * ocr_avg + 0.20 * quality_avg + 0.15 * agreement;
    // Repeated agreement raises confidence, but only up to the point where extra frames stop
    // adding information.
    const double repetition =
        std::min(1.0, 0.70 + 0.10 * static_cast<double>(std::min(candidate.count, 3)));
    // The plate string is only as certain as its least certain character, so a single weak
    // character caps the result. A reading whose weakest character is already as confident as
    // the required mean is not penalised at all.
    const double char_knee = std::max(0.01, config_.min_avg_confidence);
    const double weakest_character_factor = std::min(1.0, min_char_avg / char_knee);
    return clamp01(evidence * repetition * candidate.plate.confidence_multiplier *
                   weakest_character_factor);
}

bool PlateConsensus::satisfied() const {
    const Candidate* candidate = best();
    if (candidate == nullptr) {
        return false;
    }
    const double agreement = total_weight_ > 0.0 ? candidate->weight / total_weight_ : 0.0;
    const double confidence = combinedConfidence(*candidate, agreement);
    const double ocr_avg = candidate->ocr_sum / static_cast<double>(std::max(1, candidate->count));

    if (candidate->count == 1) {
        return config_.allow_single_frame && ocr_avg >= config_.single_frame_confidence &&
               confidence >= config_.min_final_confidence;
    }

    return validObservations() >= config_.min_samples &&
           candidate->count >= config_.required_votes && agreement >= config_.min_agreement &&
           ocr_avg >= config_.min_avg_confidence && confidence >= config_.min_final_confidence;
}

ConsensusResult PlateConsensus::resolve() const {
    ConsensusResult result;
    result.total_observations = total_observations_;

    const Candidate* candidate = best();
    if (candidate == nullptr) {
        if (total_observations_ == 0) {
            result.status = RecognitionStatus::kNoPlate;
        } else if (saw_ambiguous_) {
            result.status = RecognitionStatus::kAmbiguous;
        } else if (saw_invalid_) {
            result.status = RecognitionStatus::kInvalidFormat;
        } else {
            result.status = RecognitionStatus::kLowConfidence;
        }
        return result;
    }

    const double agreement = total_weight_ > 0.0 ? candidate->weight / total_weight_ : 0.0;
    result.normalized_plate = candidate->plate.normalized;
    result.raw_plate = candidate->raw_text;
    result.format_name = candidate->plate.format_name;
    result.region_code = candidate->plate.region_code;
    result.region_name = candidate->plate.region_name;
    result.confidence = combinedConfidence(*candidate, agreement);
    result.agreement = agreement;
    result.agreeing_observations = candidate->count;
    result.plate_box = candidate->best_box;
    result.crop_path = candidate->best_crop_path;

    if (satisfied()) {
        result.status = toRecognitionStatus(candidate->plate.status);
    } else {
        result.status = RecognitionStatus::kLowConfidence;
    }
    return result;
}

}  // namespace anpr
