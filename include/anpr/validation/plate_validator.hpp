#pragma once

#include <optional>
#include <string>
#include <vector>

#include "anpr/common/config.hpp"

namespace anpr {

enum class PlateValidationStatus {
    kValidHighConfidence,
    kValidLowConfidence,
    kInvalidFormat,
    kAmbiguous,
};

std::string toString(PlateValidationStatus status);

struct PlateCorrection {
    int position{0};
    char from{'\0'};
    char to{'\0'};
};

struct ValidatedPlate {
    std::string raw;         ///< OCR text after character-set sanitisation only
    std::string normalized;  ///< text after format-aware corrections
    std::string main;        ///< serial part, without the region code
    std::optional<std::string> region_code;
    std::optional<std::string> region_name;
    std::string format_name;
    PlateValidationStatus status{PlateValidationStatus::kInvalidFormat};
    /// Multiplier in [0, 1] folded into the consensus weight. Corrections lower it.
    double confidence_multiplier{0.0};
    std::vector<PlateCorrection> corrections;

    [[nodiscard]] bool valid() const {
        return status == PlateValidationStatus::kValidHighConfidence ||
               status == PlateValidationStatus::kValidLowConfidence;
    }
};

/// Uppercases and drops every character outside [A-Z0-9].
std::string sanitizePlateText(const std::string& raw_text);

/// Applies the configured plate layouts to OCR output.
///
/// Nothing about Kazakhstan is compiled in beyond the defaults in `defaultKazakhstanValidation`.
/// Layouts, the letter alphabet, region codes and the confusion maps all come from configuration,
/// so a new layout is a config change rather than a code change.
///
/// Corrections are position-aware: a digit slot may only take a digit-shaped substitution and a
/// letter slot only a letter-shaped one. A character with no configured confusion partner is
/// never rewritten, and a reading needing more than `max_corrections` is reported as ambiguous
/// rather than silently repaired.
class PlateValidator {
public:
    explicit PlateValidator(ValidationConfig config = defaultKazakhstanValidation());

    [[nodiscard]] ValidatedPlate validate(const std::string& raw_text,
                                          double ocr_confidence = 1.0) const;

    [[nodiscard]] const ValidationConfig& config() const { return config_; }
    [[nodiscard]] std::optional<std::string> regionName(const std::string& code) const;

private:
    ValidationConfig config_;

    [[nodiscard]] ValidatedPlate applyFormat(const PlateFormat& format, const std::string& text,
                                             double ocr_confidence) const;
    [[nodiscard]] bool isLetter(char ch) const;
    [[nodiscard]] double correctionMultiplier(std::size_t corrections) const;
};

}  // namespace anpr
