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

TEST("shipped jetson-nano.yaml loads without unknown keys") {
    const auto loaded = anpr::loadConfigFile("config/jetson-nano.yaml");
    CHECK(loaded.ok);
    CHECK(loaded.unknown_keys.empty());
    CHECK_EQ(loaded.config.inference.backend, anpr::InferenceBackend::kTensorRT);
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

TEST("a validation list left out of the file keeps the built-in one") {
    const auto loaded = anpr::loadConfigText(R"(
validation:
  formats:
    - name: kz_individual
      pattern: DDDLLLRR
)");
    CHECK(loaded.ok);
    CHECK_EQ(loaded.config.validation.formats.size(), std::size_t{1});
    CHECK_EQ(loaded.config.validation.regions.size(), std::size_t{20});
    CHECK_EQ(loaded.config.validation.regions.at("02"), std::string("Almaty"));
    CHECK_EQ(loaded.config.validation.digit_confusions.at('O'), '0');
    CHECK_EQ(loaded.config.validation.letter_confusions.at('1'), 'I');
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

TEST("the OCR reads Nomeroff's Kazakhstan model by default") {
    anpr::AnprConfig config;
    CHECK_EQ(config.ocr.model, std::string("models/nomeroff-onnx/kz.onnx"));
    CHECK(config.ocr.max_attempts > 0);
}

TEST("an empty OCR model or an out-of-range OCR threshold fails configuration") {
    CHECK(!anpr::loadConfigText("ocr:\n  model: \"\"\n").ok);
    CHECK(!anpr::loadConfigText("ocr:\n  min_confidence: 1.5\n").ok);
    CHECK(!anpr::loadConfigText("ocr:\n  min_char_confidence: -0.1\n").ok);
    CHECK(!anpr::loadConfigText("ocr:\n  max_attempts: 0\n").ok);
}

TEST("removed settings are reported, not fatal") {
    const auto loaded =
        anpr::loadConfigText("ocr:\n  backend: fast_plate_ocr\nvalidation:\n  profile: auto\n");
    CHECK(loaded.ok);
    CHECK_EQ(loaded.unknown_keys.size(), std::size_t{2});
    CHECK_EQ(loaded.unknown_keys.front(), std::string("ocr.backend"));
    CHECK_EQ(loaded.unknown_keys.back(), std::string("validation.profile"));
}
