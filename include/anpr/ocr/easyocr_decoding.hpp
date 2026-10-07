#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace anpr {
namespace easyocr {

/// Number of classes of the english_g2 recognizer: the CTC blank plus 96 characters.
constexpr int kEnglishG2Classes = 97;

/// Model classes a reading may use: index 0 is always the blank, the rest map to `characters`.
struct AllowedClasses {
    std::vector<int> classes;
    std::vector<char> characters;
};

/// The english_g2 classes whose character is in `allowlist` (ASCII), plus the blank.
AllowedClasses englishG2Allowed(const std::string& allowlist);

struct CtcReading {
    std::string text;
    /// EasyOCR's score: the product of the per-step maximum probabilities of the non-blank steps,
    /// raised to 2 / sqrt(step count). 0 when every step is blank.
    double confidence{0.0};
};

/// EasyOCR 1.6.2 greedy decoding of `steps` x `classes` logits for one crop.
///
/// Probabilities are a softmax over the allowed classes only, which equals EasyOCR's softmax,
/// zeroing of the ignored characters and renormalisation. Repeated classes collapse and blanks
/// are dropped, as in CTCLabelConverter.decode_greedy.
CtcReading decodeGreedy(const float* logits, int steps, int classes,
                        const AllowedClasses& allowed);

/// EasyOCR's adjust_contrast_grey, in place: stretches a low-contrast 8-bit greyscale image
/// whose (p90 - p10) / (p90 + p10) is below `target`. Percentiles use NumPy's linear rule.
void adjustContrastGrey(std::uint8_t* pixels, std::size_t count, double target);

}  // namespace easyocr
}  // namespace anpr
