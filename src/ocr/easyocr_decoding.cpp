#include "anpr/ocr/easyocr_decoding.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace anpr {
namespace easyocr {
namespace {

/// EasyOCR 1.6.2 english_g2 characters; model class i is entry i - 1. tools/export_easyocr_onnx.py
/// checks EasyOCR's own copy against the same string before exporting.
constexpr char32_t kEnglishG2Characters[] =
    U"0123456789!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~ €"
    U"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
static_assert(sizeof(kEnglishG2Characters) / sizeof(char32_t) - 1 == kEnglishG2Classes - 1,
              "english_g2 has 96 characters");

/// NumPy's default (linear) percentile of an 8-bit image given its histogram.
double percentile(const std::array<std::size_t, 256>& histogram, std::size_t count, double q) {
    const double position = q / 100.0 * static_cast<double>(count - 1);
    const auto lower_rank = static_cast<std::size_t>(std::floor(position));
    const std::size_t upper_rank = std::min(lower_rank + 1, count - 1);
    auto value_at = [&histogram](std::size_t rank) {
        std::size_t seen = 0;
        for (std::size_t value = 0; value < histogram.size(); ++value) {
            seen += histogram[value];
            if (seen > rank) return static_cast<double>(value);
        }
        return 255.0;
    };
    const double lower = value_at(lower_rank);
    const double upper = value_at(upper_rank);
    return lower + (upper - lower) * (position - static_cast<double>(lower_rank));
}

}  // namespace

AllowedClasses englishG2Allowed(const std::string& allowlist) {
    AllowedClasses allowed;
    allowed.classes.push_back(0);
    allowed.characters.push_back('\0');
    for (int index = 0; index < kEnglishG2Classes - 1; ++index) {
        const char32_t character = kEnglishG2Characters[index];
        if (character < 0x80 && allowlist.find(static_cast<char>(character)) != std::string::npos) {
            allowed.classes.push_back(index + 1);
            allowed.characters.push_back(static_cast<char>(character));
        }
    }
    return allowed;
}

CtcReading decodeGreedy(const float* logits, int steps, int classes,
                        const AllowedClasses& allowed) {
    CtcReading reading;
    double log_product = 0.0;
    int counted = 0;
    std::size_t previous = static_cast<std::size_t>(-1);
    for (int step = 0; step < steps; ++step) {
        const float* row = logits + static_cast<std::size_t>(step) * classes;
        std::size_t best = 0;
        for (std::size_t index = 1; index < allowed.classes.size(); ++index) {
            if (row[allowed.classes[index]] > row[allowed.classes[best]]) best = index;
        }
        if (best != 0) {
            const double top = row[allowed.classes[best]];
            double sum = 0.0;
            for (const int cls : allowed.classes) sum += std::exp(row[cls] - top);
            // The winning probability is exp(0) / sum.
            log_product -= std::log(sum);
            ++counted;
            if (best != previous) reading.text.push_back(allowed.characters[best]);
        }
        previous = best;
    }
    if (counted > 0) {
        reading.confidence = std::exp(log_product * 2.0 / std::sqrt(static_cast<double>(counted)));
    }
    return reading;
}

void adjustContrastGrey(std::uint8_t* pixels, std::size_t count, double target) {
    if (count == 0) return;
    std::array<std::size_t, 256> histogram{};
    for (std::size_t index = 0; index < count; ++index) ++histogram[pixels[index]];
    const double high = percentile(histogram, count, 90.0);
    const double low = percentile(histogram, count, 10.0);
    const double contrast = (high - low) / std::max(10.0, high + low);
    if (contrast >= target) return;
    const double ratio = 200.0 / std::max(10.0, high - low);
    for (std::size_t index = 0; index < count; ++index) {
        const double value = (static_cast<double>(pixels[index]) - low + 25.0) * ratio;
        // NumPy clips to [0, 255] and then truncates with astype(uint8).
        pixels[index] = static_cast<std::uint8_t>(std::min(255.0, std::max(0.0, value)));
    }
}

}  // namespace easyocr
}  // namespace anpr
