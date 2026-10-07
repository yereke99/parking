#pragma once

#include <string>
#include <vector>

namespace anpr {
namespace nomeroff {

/// Characters of Nomeroff Net's Kazakhstan text reader ("kz", model card
/// nomeroff-net-ocr-kz/model-4.json). Class 0 is the CTC blank; class i is kKzLetters[i - 1].
constexpr const char* kKzLetters = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";

struct CtcReading {
    std::string text;
    /// One probability per returned character.
    std::vector<float> character_confidences;
    /// Mean and minimum of `character_confidences`; 0 when nothing was read.
    float confidence{0.0F};
    float min_char_confidence{0.0F};
};

/// Greedy CTC decoding of `steps` x `classes` logits for one crop: softmax per step, argmax,
/// repeated classes collapse, blanks are dropped (a blank between two equal classes keeps both
/// characters). A character's confidence is the highest probability among the steps that
/// emitted it, so a long run does not inflate the mean.
CtcReading decodeGreedy(const float* logits, int steps, int classes, const std::string& letters);

}  // namespace nomeroff
}  // namespace anpr
