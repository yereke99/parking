#include "anpr/ocr/fast_plate_ocr.hpp"

#include <vector>

#include "test_framework.hpp"

namespace {

/// The contract of the shipped cct-s-v2-global model: 10 slots, 37 classes, '_' padding.
anpr::FastPlateOcrModelConfig modelConfig() {
    anpr::FastPlateOcrModelConfig config;
    config.max_plate_slots = 10;
    config.alphabet = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ_";
    config.pad_char = '_';
    config.img_height = 64;
    config.img_width = 128;
    config.grayscale = false;
    return config;
}

/// Builds a plate-head tensor where each slot puts `confidence` on the wanted character and
/// spreads the remainder across the rest, mirroring the softmax the model already applies.
std::vector<float> makeHead(const anpr::FastPlateOcrModelConfig& config, const std::string& text,
                            const std::vector<float>& confidences) {
    const std::size_t vocabulary = config.alphabet.size();
    std::vector<float> head(static_cast<std::size_t>(config.max_plate_slots) * vocabulary, 0.0F);
    for (int slot = 0; slot < config.max_plate_slots; ++slot) {
        const char wanted = slot < static_cast<int>(text.size())
                                ? text[static_cast<std::size_t>(slot)]
                                : config.pad_char;
        const float confidence = slot < static_cast<int>(confidences.size())
                                     ? confidences[static_cast<std::size_t>(slot)]
                                     : 0.99F;
        const std::size_t index = config.alphabet.find(wanted);
        const float rest = (1.0F - confidence) / static_cast<float>(vocabulary - 1);
        for (std::size_t i = 0; i < vocabulary; ++i) {
            head[static_cast<std::size_t>(slot) * vocabulary + i] = rest;
        }
        head[static_cast<std::size_t>(slot) * vocabulary + index] = confidence;
    }
    return head;
}

}  // namespace

TEST("decoding maps argmax through the alphabet and drops trailing padding") {
    const auto config = modelConfig();
    const auto head = makeHead(config, "123ABC02", {});
    const auto result = anpr::FastPlateOcr::decode(config, head.data(), 0.30F);
    CHECK(result.ok());
    CHECK_EQ(result.text, std::string("123ABC02"));
    CHECK_EQ(result.character_confidences.size(), std::size_t{8});
    CHECK_NEAR(result.confidence, 0.99, 0.01);
}

TEST("decoding reports the weakest character") {
    const auto config = modelConfig();
    const auto head = makeHead(config, "123ABC02", {0.99F, 0.99F, 0.42F, 0.99F, 0.99F, 0.99F,
                                                    0.99F, 0.99F});
    const auto result = anpr::FastPlateOcr::decode(config, head.data(), 0.30F);
    CHECK(result.ok());
    CHECK_NEAR(result.min_char_confidence, 0.42, 0.01);
    // The mean stays healthy even though one character is weak, which is why the minimum is
    // carried separately into the consensus.
    CHECK(result.confidence > 0.90);
}

TEST("decoding rejects a character below the floor") {
    const auto config = modelConfig();
    const auto head = makeHead(config, "123ABC02", {0.99F, 0.99F, 0.10F, 0.99F, 0.99F, 0.99F,
                                                    0.99F, 0.99F});
    const auto result = anpr::FastPlateOcr::decode(config, head.data(), 0.30F);
    CHECK(!result.ok());
    CHECK_EQ(result.rejection, anpr::OcrRejection::kWeakCharacter);
}

TEST("decoding rejects an all-padding prediction") {
    const auto config = modelConfig();
    const auto head = makeHead(config, "", {});
    const auto result = anpr::FastPlateOcr::decode(config, head.data(), 0.30F);
    CHECK(!result.ok());
    CHECK_EQ(result.rejection, anpr::OcrRejection::kAllPadding);
}

TEST("decoding refuses to splice around interior padding") {
    // Deleting the interior pad would turn "12_ABC02" into a plausible seven-character plate.
    const auto config = modelConfig();
    const auto head = makeHead(config, "12_ABC02", {});
    const auto result = anpr::FastPlateOcr::decode(config, head.data(), 0.30F);
    CHECK(!result.ok());
    CHECK_EQ(result.rejection, anpr::OcrRejection::kInteriorPadding);
}

TEST("decoding handles a full-length plate with no padding") {
    const auto config = modelConfig();
    const auto head = makeHead(config, "1234567890", {});
    const auto result = anpr::FastPlateOcr::decode(config, head.data(), 0.30F);
    CHECK(result.ok());
    CHECK_EQ(result.text, std::string("1234567890"));
    CHECK_EQ(result.character_confidences.size(), std::size_t{10});
}

TEST("confidence excludes padding slots") {
    // Padding slots are predicted with near certainty. Averaging them in would inflate a short
    // plate's confidence, so only the returned characters count.
    const auto config = modelConfig();
    const auto head = makeHead(config, "123AB01", {0.60F, 0.60F, 0.60F, 0.60F, 0.60F, 0.60F, 0.60F});
    const auto result = anpr::FastPlateOcr::decode(config, head.data(), 0.30F);
    CHECK(result.ok());
    CHECK_NEAR(result.confidence, 0.60, 0.01);
}

TEST("the shipped model config parses") {
    const auto loaded = anpr::loadFastPlateOcrConfig("models/plate_ocr_config.yaml");
    CHECK(loaded.ok);
    CHECK_EQ(loaded.config.max_plate_slots, 10);
    CHECK_EQ(loaded.config.img_height, 64);
    CHECK_EQ(loaded.config.img_width, 128);
    CHECK_EQ(loaded.config.pad_char, '_');
    CHECK_EQ(loaded.config.grayscale, false);
    CHECK_EQ(loaded.config.keep_aspect_ratio, false);
    CHECK_EQ(loaded.config.alphabet, std::string("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ_"));
    CHECK_EQ(loaded.config.padIndex(), 36);
    // The multi-line region list has to survive the YAML reader.
    CHECK_EQ(loaded.config.plate_regions.size(), std::size_t{66});
    CHECK_EQ(loaded.config.plate_regions.front(), std::string("Albania"));
    CHECK_EQ(loaded.config.plate_regions.back(), std::string("Unknown"));
}

TEST("a config missing required fields is rejected") {
    const auto loaded = anpr::loadFastPlateOcrConfig("config/default.yaml");
    CHECK(!loaded.ok);
}
