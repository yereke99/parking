#include "anpr/ocr/easyocr_decoding.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

#include "test_framework.hpp"

namespace {

using anpr::easyocr::kEnglishG2Classes;

const anpr::easyocr::AllowedClasses& plateClasses() {
    static const auto allowed =
        anpr::easyocr::englishG2Allowed("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ");
    return allowed;
}

/// One time step whose logits are 0 except `cls`, which gets `logit`.
void step(std::vector<float>& logits, int cls, float logit) {
    std::vector<float> row(kEnglishG2Classes, 0.0F);
    row[static_cast<std::size_t>(cls)] = logit;
    logits.insert(logits.end(), row.begin(), row.end());
}

anpr::easyocr::CtcReading decode(const std::vector<float>& logits) {
    return anpr::easyocr::decodeGreedy(logits.data(),
                                       static_cast<int>(logits.size() / kEnglishG2Classes),
                                       kEnglishG2Classes, plateClasses());
}

}  // namespace

TEST("easyocr allowlist keeps the blank plus digits and capitals") {
    const auto& allowed = plateClasses();
    CHECK(allowed.classes.size() == 37);
    CHECK(allowed.classes[0] == 0);
    CHECK(allowed.classes[1] == 1 && allowed.characters[1] == '0');
    CHECK(allowed.classes[11] == 45 && allowed.characters[11] == 'A');
    CHECK(allowed.classes[36] == 70 && allowed.characters[36] == 'Z');
}

TEST("easyocr greedy decoding collapses repeats and drops blanks") {
    // Classes: 0 blank, 2 = '1', 6 = '5'.
    std::vector<float> logits;
    for (const int cls : {0, 2, 2, 0, 2, 6, 6, 0}) step(logits, cls, 10.0F);
    CHECK(decode(logits).text == "115");
}

TEST("easyocr decoding ignores characters outside the allowlist") {
    // Lower-case 'a' (class 71) wins overall, but only 'A' (class 45) is allowed.
    std::vector<float> logits;
    step(logits, 71, 9.0F);
    logits[45] = 5.0F;
    CHECK(decode(logits).text == "A");
}

TEST("easyocr confidence is EasyOCR's custom mean of non-blank step maxima") {
    std::vector<float> logits;
    step(logits, 0, 10.0F);
    step(logits, 2, 2.0F);
    step(logits, 6, 3.0F);
    // Softmax over the 37 allowed classes; the 36 others have logit 0.
    const double p1 = std::exp(2.0) / (std::exp(2.0) + 36.0);
    const double p2 = std::exp(3.0) / (std::exp(3.0) + 36.0);
    const double expected = std::pow(p1 * p2, 2.0 / std::sqrt(2.0));
    const auto reading = decode(logits);
    CHECK(reading.text == "15");
    CHECK(std::fabs(reading.confidence - expected) < 1e-9);
}

TEST("easyocr all-blank reading has zero confidence") {
    std::vector<float> logits;
    step(logits, 0, 4.0F);
    step(logits, 0, 4.0F);
    const auto reading = decode(logits);
    CHECK(reading.text.empty());
    CHECK(reading.confidence == 0.0);
}

TEST("easyocr contrast stretch matches adjust_contrast_grey") {
    // NumPy: p10 = 104.5, p90 = 140.5, so the image is stretched and clipped.
    std::vector<std::uint8_t> low{100, 105, 110, 115, 120, 125, 130, 135, 140, 145};
    anpr::easyocr::adjustContrastGrey(low.data(), low.size(), 0.5);
    const std::vector<std::uint8_t> expected{113, 141, 169, 197, 225, 252, 255, 255, 255, 255};
    CHECK(low == expected);

    std::vector<std::uint8_t> high{0, 30, 60, 90, 120, 150, 180, 210, 240, 250};
    const std::vector<std::uint8_t> unchanged = high;
    anpr::easyocr::adjustContrastGrey(high.data(), high.size(), 0.5);
    CHECK(high == unchanged);
}
