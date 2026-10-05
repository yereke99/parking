#include "anpr/common/config.hpp"
#include "anpr/validation/plate_validator.hpp"

#include "test_framework.hpp"

TEST("default configuration validates") {
    anpr::AnprConfig config;
    config.validation = anpr::defaultKazakhstanValidation();
    std::string error;
    CHECK(anpr::validateConfig(config, error));
    CHECK(error.empty());
}

TEST("shipped default.yaml loads") {
    const auto loaded = anpr::loadConfigFile("config/default.yaml");
    CHECK(loaded.ok);
    CHECK_EQ(loaded.error, std::string());
    CHECK_EQ(loaded.config.camera.camera_id, std::string("gate-01"));
}

TEST("shipped default.yaml has no unknown keys") {
    const auto loaded = anpr::loadConfigFile("config/default.yaml");
    CHECK(loaded.ok);
    if (!loaded.unknown_keys.empty()) {
        anpr_test::recordFailure("unknown config key: " + loaded.unknown_keys.front());
    }
}

TEST("shipped research.yaml loads without unknown keys") {
    const auto loaded = anpr::loadConfigFile("config/research.yaml");
    CHECK(loaded.ok);
    if (!loaded.unknown_keys.empty()) {
        anpr_test::recordFailure("unknown research config key: " + loaded.unknown_keys.front());
    }
}

TEST("unknown keys are reported instead of silently ignored") {
    const auto loaded = anpr::loadConfigText("detector:\n  confidence_treshold: 0.6\n");
    CHECK(loaded.ok);
    CHECK_EQ(loaded.unknown_keys.size(), std::size_t{1});
    CHECK_EQ(loaded.unknown_keys.front(), std::string("detector.confidence_treshold"));
}

TEST("validation profiles come from the config file") {
    const auto loaded = anpr::loadConfigText(R"(
validation:
  letters: "ABC"
  formats:
    - name: short
      pattern: LLDD
      weight: 0.8
  regions:
    "01": Astana
)");
    CHECK(loaded.ok);
    CHECK_EQ(loaded.config.validation.formats.size(), std::size_t{1});
    CHECK_EQ(loaded.config.validation.formats.front().name, std::string("short"));
    CHECK_EQ(loaded.config.validation.formats.front().pattern, std::string("LLDD"));
    CHECK_NEAR(loaded.config.validation.formats.front().weight, 0.8, 1e-9);
    CHECK_EQ(loaded.config.validation.letters, std::string("ABC"));
    CHECK_EQ(loaded.config.validation.regions.size(), std::size_t{1});
}

TEST("confusion maps come from the config file") {
    const auto loaded = anpr::loadConfigText(R"(
validation:
  digit_confusions:
    O: "0"
    I: "1"
  letter_confusions:
    "0": O
)");
    CHECK(loaded.ok);
    CHECK_EQ(loaded.config.validation.digit_confusions.size(), std::size_t{2});
    CHECK_EQ(loaded.config.validation.digit_confusions.at('O'), '0');
    CHECK_EQ(loaded.config.validation.letter_confusions.at('0'), 'O');
}

TEST("an unknown slot class in a pattern is rejected") {
    const auto loaded = anpr::loadConfigText(R"(
validation:
  formats:
    - name: bad
      pattern: DDXX
)");
    CHECK(!loaded.ok);
    CHECK(loaded.error.find("slot class") != std::string::npos);
}

TEST("inconsistent motion thresholds are rejected") {
    const auto loaded = anpr::loadConfigText("motion:\n  threshold: 0.005\n  quiet_threshold: 0.02\n");
    CHECK(!loaded.ok);
    CHECK(loaded.error.find("quiet_threshold") != std::string::npos);
}

TEST("an out-of-frame roi is rejected") {
    const auto loaded = anpr::loadConfigText("roi:\n  stop: [0.5, 0.5, 0.8, 0.8]\n");
    CHECK(!loaded.ok);
    CHECK(loaded.error.find("roi") != std::string::npos);
}

TEST("a frame queue larger than one is rejected") {
    const auto loaded = anpr::loadConfigText("performance:\n  frame_queue_size: 8\n");
    CHECK(!loaded.ok);
    CHECK(loaded.error.find("frame_queue_size") != std::string::npos);
}

TEST("required votes cannot exceed the sample minimum") {
    const auto loaded =
        anpr::loadConfigText("consensus:\n  min_samples: 2\n  required_votes: 5\n");
    CHECK(!loaded.ok);
    CHECK(loaded.error.find("required_votes") != std::string::npos);
}

TEST("a stop window shorter than the stop duration is rejected") {
    const auto loaded =
        anpr::loadConfigText("stop_detection:\n  window_ms: 200\n  stop_duration_ms: 800\n");
    CHECK(!loaded.ok);
}

TEST("a wrongly typed value is reported with its key") {
    const auto loaded = anpr::loadConfigText("detector:\n  input_size: wide\n");
    CHECK(!loaded.ok);
    CHECK(loaded.error.find("detector.input_size") != std::string::npos);
}

TEST("a missing config file fails cleanly") {
    const auto loaded = anpr::loadConfigFile("config/definitely-not-here.yaml");
    CHECK(!loaded.ok);
    CHECK(loaded.error.find("cannot open") != std::string::npos);
}

TEST("the inference backend name is parsed and rejected when unknown") {
    const auto ok = anpr::loadConfigText("inference:\n  backend: tensorrt\n");
    CHECK(ok.ok);
    CHECK_EQ(ok.config.inference.backend, anpr::InferenceBackend::kTensorRT);
    const auto bad = anpr::loadConfigText("inference:\n  backend: magic\n");
    CHECK(!bad.ok);
}

TEST("Nomeroff is the default OCR backend with explicit Kazakhstan routing") {
    anpr::AnprConfig config;
    config.validation = anpr::defaultKazakhstanValidation();
    CHECK_EQ(config.ocr.backend, std::string("nomeroff"));
    CHECK_EQ(config.ocr.region_mode, std::string("kz"));
    CHECK_EQ(config.ocr.device, std::string("auto"));
}

TEST("invalid Nomeroff device region and line settings fail configuration") {
    CHECK(!anpr::loadConfigText("ocr:\n  device: quantum\n").ok);
    CHECK(!anpr::loadConfigText("ocr:\n  region_mode: auto\n").ok);
    CHECK(!anpr::loadConfigText("ocr:\n  lines_count: 3\n").ok);
}

TEST("legacy OCR remains selectable only by explicit configuration") {
    const auto loaded = anpr::loadConfigText("ocr:\n  backend: fast_plate_ocr\n");
    CHECK(loaded.ok);
    CHECK_EQ(loaded.config.ocr.backend, std::string("fast_plate_ocr"));
}

TEST("all four research OCR backends are selectable") {
    for (const std::string backend : {"fast_plate_ocr", "nomeroff", "paddleocr", "easyocr"}) {
        const auto loaded = anpr::loadConfigText("ocr:\n  backend: " + backend + "\n");
        CHECK(loaded.ok);
        CHECK_EQ(loaded.config.ocr.backend, backend);
    }
    CHECK(!anpr::loadConfigText("ocr:\n  backend: unknown\n").ok);
}

TEST("Russian OCR mode selects Russian position-aware validation") {
    const auto loaded = anpr::loadConfigText("ocr:\n  region_mode: ru\n");
    CHECK(loaded.ok);
    CHECK_EQ(loaded.config.validation.letters, std::string("ABCEHKMOPTXY"));
    const anpr::PlateValidator validator(loaded.config.validation);
    CHECK(validator.validate("A123BC77", 0.95).valid());
    CHECK(validator.validate("A123BC777", 0.95).valid());
    CHECK(!validator.validate("123ABC02", 0.95).valid());
}

TEST("other CIS OCR modes require an explicit validation profile") {
    const auto automatic = anpr::loadConfigText("ocr:\n  region_mode: by\n");
    CHECK(!automatic.ok);
    const auto custom = anpr::loadConfigText(R"(
ocr:
  region_mode: by
validation:
  profile: custom
  letters: "ABCEHIKMOPT"
  formats:
    - name: by_private
      pattern: DDDDLLD
)");
    CHECK(custom.ok);
}
