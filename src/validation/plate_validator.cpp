#include "anpr/validation/plate_validator.hpp"

#include <algorithm>
#include <cctype>

namespace anpr {
namespace {

bool isDigit(char ch) {
    return ch >= '0' && ch <= '9';
}

}  // namespace

std::string toString(PlateValidationStatus status) {
    switch (status) {
        case PlateValidationStatus::kValidHighConfidence:
            return "VALID_HIGH_CONFIDENCE";
        case PlateValidationStatus::kValidLowConfidence:
            return "VALID_LOW_CONFIDENCE";
        case PlateValidationStatus::kInvalidFormat:
            return "INVALID_FORMAT";
        case PlateValidationStatus::kAmbiguous:
            return "AMBIGUOUS";
    }
    return "INVALID_FORMAT";
}

std::string sanitizePlateText(const std::string& raw_text) {
    std::string out;
    out.reserve(raw_text.size());
    for (const unsigned char byte : raw_text) {
        if (std::isalnum(byte) != 0) {
            out.push_back(static_cast<char>(std::toupper(byte)));
        }
    }
    return out;
}

PlateValidator::PlateValidator(ValidationConfig config) : config_(std::move(config)) {}

bool PlateValidator::isLetter(char ch) const {
    return config_.letters.find(ch) != std::string::npos;
}

double PlateValidator::correctionMultiplier(std::size_t corrections) const {
    if (config_.correction_multipliers.empty()) {
        return 1.0;
    }
    const std::size_t index =
        std::min(corrections, config_.correction_multipliers.size() - 1);
    return config_.correction_multipliers[index];
}

std::optional<std::string> PlateValidator::regionName(const std::string& code) const {
    const auto it = config_.regions.find(code);
    if (it == config_.regions.end()) {
        return std::nullopt;
    }
    return it->second;
}

ValidatedPlate PlateValidator::applyFormat(const PlateFormat& format, const std::string& text,
                                           double ocr_confidence) const {
    ValidatedPlate result;
    result.raw = text;
    result.format_name = format.name;
    if (text.size() != format.pattern.size()) {
        return result;
    }

    std::string normalized = text;
    std::string region_code;

    for (std::size_t index = 0; index < format.pattern.size(); ++index) {
        const char slot = format.pattern[index];
        char ch = normalized[index];
        const bool wants_digit = slot == 'D' || slot == 'R';

        if (wants_digit ? isDigit(ch) : isLetter(ch)) {
            if (slot == 'R') {
                region_code.push_back(ch);
            }
            continue;
        }

        if (!format.allow_corrections) {
            return result;
        }
        const auto& table = wants_digit ? config_.digit_confusions : config_.letter_confusions;
        const auto it = table.find(ch);
        if (it == table.end()) {
            return result;
        }
        const char replacement = it->second;
        // The substitution itself has to satisfy the slot, otherwise the table is misconfigured.
        if (wants_digit ? !isDigit(replacement) : !isLetter(replacement)) {
            return result;
        }
        result.corrections.push_back(PlateCorrection{static_cast<int>(index), ch, replacement});
        normalized[index] = replacement;
        ch = replacement;
        if (slot == 'R') {
            region_code.push_back(ch);
        }
    }

    if (!region_code.empty() && config_.regions.find(region_code) == config_.regions.end()) {
        return result;
    }

    if (result.corrections.size() > static_cast<std::size_t>(config_.max_corrections)) {
        result.status = PlateValidationStatus::kAmbiguous;
        result.normalized = normalized;
        return result;
    }

    result.normalized = normalized;
    if (region_code.empty()) {
        result.main = normalized;
    } else {
        const std::size_t region_start = format.pattern.find('R');
        result.main = normalized.substr(0, region_start);
        result.region_code = region_code;
        result.region_name = regionName(region_code);
    }

    result.confidence_multiplier = correctionMultiplier(result.corrections.size()) * format.weight;
    const bool clean_enough = result.corrections.size() <= 1;
    result.status = ocr_confidence >= config_.high_confidence_threshold && clean_enough
                        ? PlateValidationStatus::kValidHighConfidence
                        : PlateValidationStatus::kValidLowConfidence;
    return result;
}

ValidatedPlate PlateValidator::validate(const std::string& raw_text, double ocr_confidence) const {
    ValidatedPlate best;
    best.raw = sanitizePlateText(raw_text);
    if (best.raw.empty()) {
        return best;
    }

    bool saw_ambiguous = false;
    for (const PlateFormat& format : config_.formats) {
        ValidatedPlate candidate = applyFormat(format, best.raw, ocr_confidence);
        if (candidate.status == PlateValidationStatus::kAmbiguous) {
            saw_ambiguous = true;
            continue;
        }
        if (!candidate.valid()) {
            continue;
        }
        // Prefer the layout that needed the least repair, then the higher-weighted layout.
        const bool better =
            !best.valid() ||
            candidate.corrections.size() < best.corrections.size() ||
            (candidate.corrections.size() == best.corrections.size() &&
             candidate.confidence_multiplier > best.confidence_multiplier);
        if (better) {
            const std::string raw = best.raw;
            best = std::move(candidate);
            best.raw = raw;
        }
    }

    if (!best.valid()) {
        best.status = saw_ambiguous ? PlateValidationStatus::kAmbiguous
                                    : PlateValidationStatus::kInvalidFormat;
        best.confidence_multiplier = 0.0;
    }
    return best;
}

}  // namespace anpr
