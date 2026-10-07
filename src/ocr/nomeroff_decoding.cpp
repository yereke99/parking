#include "anpr/ocr/nomeroff_decoding.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace anpr {
namespace nomeroff {

CtcReading decodeGreedy(const float* logits, int steps, int classes, const std::string& letters) {
    CtcReading reading;
    int previous = -1;
    for (int step = 0; step < steps; ++step) {
        const float* row =
            logits + static_cast<std::size_t>(step) * static_cast<std::size_t>(classes);
        int best = 0;
        for (int index = 1; index < classes; ++index) {
            if (row[index] > row[best]) best = index;
        }
        // Softmax probability of the winning class: 1 / sum(exp(logit - best logit)).
        double sum = 0.0;
        for (int index = 0; index < classes; ++index) {
            sum += std::exp(static_cast<double>(row[index]) - static_cast<double>(row[best]));
        }
        const float probability = static_cast<float>(1.0 / sum);
        const bool known = best > 0 && static_cast<std::size_t>(best) <= letters.size();
        if (known && best != previous) {
            reading.text.push_back(letters[static_cast<std::size_t>(best - 1)]);
            reading.character_confidences.push_back(probability);
        } else if (known && !reading.character_confidences.empty()) {
            float& current = reading.character_confidences.back();
            current = std::max(current, probability);
        }
        previous = best;
    }
    if (!reading.character_confidences.empty()) {
        float sum = 0.0F;
        for (const float value : reading.character_confidences) sum += value;
        reading.confidence = sum / static_cast<float>(reading.character_confidences.size());
        reading.min_char_confidence = *std::min_element(reading.character_confidences.begin(),
                                                        reading.character_confidences.end());
    }
    return reading;
}

}  // namespace nomeroff
}  // namespace anpr
