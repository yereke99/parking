#include "anpr/ocr/nomeroff_decoding.hpp"

#include <cmath>
#include <string>
#include <vector>

#include "test_framework.hpp"

namespace {

constexpr int kClasses = 37;  // blank + 0-9A-Z

int classOf(char letter) {
    return static_cast<int>(std::string(anpr::nomeroff::kKzLetters).find(letter)) + 1;
}

/// One step per entry: '-' is the blank, any other character its class. The winning class gets
/// logit `strength`, the rest 0, so its probability is e^s / (e^s + 36).
std::vector<float> steps(const std::string& path, const std::vector<float>& strengths) {
    std::vector<float> logits(path.size() * kClasses, 0.0F);
    for (std::size_t step = 0; step < path.size(); ++step) {
        const int cls = path[step] == '-' ? 0 : classOf(path[step]);
        logits[step * kClasses + static_cast<std::size_t>(cls)] = strengths[step];
    }
    return logits;
}

float probability(float strength) {
    return static_cast<float>(std::exp(strength) / (std::exp(strength) + (kClasses - 1)));
}

}  // namespace

TEST("nomeroff decoding collapses repeats and drops blanks") {
    const std::string path = "-115--22J-TTA0-2-";
    const std::vector<float> strengths(path.size(), 8.0F);
    const auto logits = steps(path, strengths);
    const auto reading = anpr::nomeroff::decodeGreedy(logits.data(), static_cast<int>(path.size()),
                                                      kClasses, anpr::nomeroff::kKzLetters);
    CHECK_EQ(reading.text, std::string("152JTA02"));
    CHECK_EQ(reading.character_confidences.size(), std::size_t{8});
}

TEST("a blank between equal classes keeps both characters") {
    const std::string path = "2-2";
    const auto logits = steps(path, {6.0F, 6.0F, 6.0F});
    const auto reading =
        anpr::nomeroff::decodeGreedy(logits.data(), 3, kClasses, anpr::nomeroff::kKzLetters);
    CHECK_EQ(reading.text, std::string("22"));
}

TEST("a character's confidence is its strongest step, and mean and minimum follow") {
    const std::string path = "11-A";
    const auto logits = steps(path, {2.0F, 7.0F, 5.0F, 4.0F});
    const auto reading =
        anpr::nomeroff::decodeGreedy(logits.data(), 4, kClasses, anpr::nomeroff::kKzLetters);
    CHECK_EQ(reading.text, std::string("1A"));
    CHECK_NEAR(reading.character_confidences[0], probability(7.0F), 1e-6);
    CHECK_NEAR(reading.character_confidences[1], probability(4.0F), 1e-6);
    CHECK_NEAR(reading.confidence, (probability(7.0F) + probability(4.0F)) / 2.0F, 1e-6);
    CHECK_NEAR(reading.min_char_confidence, probability(4.0F), 1e-6);
}

TEST("an all-blank reading is empty with zero confidence") {
    const auto logits = steps("----", {9.0F, 9.0F, 9.0F, 9.0F});
    const auto reading =
        anpr::nomeroff::decodeGreedy(logits.data(), 4, kClasses, anpr::nomeroff::kKzLetters);
    CHECK(reading.text.empty());
    CHECK(reading.character_confidences.empty());
    CHECK_EQ(reading.confidence, 0.0F);
    CHECK_EQ(reading.min_char_confidence, 0.0F);
}
