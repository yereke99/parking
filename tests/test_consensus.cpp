#include "anpr/pipeline/plate_consensus.hpp"

#include "test_framework.hpp"

namespace {

anpr::ConsensusConfig strictConfig() {
    anpr::ConsensusConfig config;
    config.min_samples = 3;
    config.required_votes = 3;
    config.min_agreement = 0.60;
    config.min_avg_confidence = 0.65;
    config.min_final_confidence = 0.68;
    config.allow_single_frame = false;
    return config;
}

anpr::PlateObservation observation(const std::string& text, double ocr_confidence,
                                   std::int64_t timestamp_ms) {
    anpr::PlateObservation obs;
    obs.raw_text = text;
    obs.detector_confidence = 0.93;
    obs.ocr_confidence = ocr_confidence;
    obs.min_char_confidence = ocr_confidence;
    obs.image_quality = 0.85;
    obs.plate_box = anpr::BoundingBox{100, 200, 180, 50};
    obs.timestamp_ms = timestamp_ms;
    return obs;
}

anpr::PlateConsensus makeConsensus(anpr::ConsensusConfig config) {
    return anpr::PlateConsensus(anpr::PlateValidator(anpr::defaultKazakhstanValidation()),
                                config);
}

}  // namespace

TEST("consensus rejects a single observation by default") {
    auto consensus = makeConsensus(strictConfig());
    consensus.add(observation("123ABC02", 0.97, 1000));
    CHECK(!consensus.satisfied());
    CHECK_EQ(consensus.resolve().status, anpr::RecognitionStatus::kLowConfidence);
}

TEST("consensus accepts a single observation only when configured to") {
    auto config = strictConfig();
    config.allow_single_frame = true;
    config.single_frame_confidence = 0.95;
    auto consensus = makeConsensus(config);
    consensus.add(observation("123ABC02", 0.97, 1000));
    CHECK(consensus.satisfied());
    CHECK(anpr::isAccepted(consensus.resolve().status));
}

TEST("consensus outvotes one wrong reading among correct ones") {
    auto consensus = makeConsensus(strictConfig());
    consensus.add(observation("123ABC02", 0.93, 1000));
    consensus.add(observation("123ABC02", 0.97, 1120));
    consensus.add(observation("173ABC02", 0.70, 1240));  // one digit misread
    consensus.add(observation("123ABC02", 0.95, 1360));
    consensus.add(observation("123ABC02", 0.96, 1480));

    CHECK(consensus.satisfied());
    const auto result = consensus.resolve();
    CHECK_EQ(result.normalized_plate, std::string("123ABC02"));
    CHECK_EQ(result.agreeing_observations, 4);
    CHECK_EQ(result.total_observations, 5);
    CHECK(result.agreement > 0.60);
    CHECK(anpr::isAccepted(result.status));
}

TEST("consensus stays unsatisfied while readings disagree") {
    auto consensus = makeConsensus(strictConfig());
    consensus.add(observation("123ABC02", 0.90, 1000));
    consensus.add(observation("124ABC02", 0.90, 1100));
    consensus.add(observation("125ABC02", 0.90, 1200));
    CHECK(!consensus.satisfied());
    CHECK_EQ(consensus.resolve().status, anpr::RecognitionStatus::kLowConfidence);
}

TEST("consensus ignores readings that fail validation") {
    auto consensus = makeConsensus(strictConfig());
    consensus.add(observation("XYZ", 0.99, 1000));
    consensus.add(observation("123ABC99", 0.99, 1100));  // unknown region
    CHECK_EQ(consensus.validObservations(), 0);
    CHECK(!consensus.satisfied());
    CHECK_EQ(consensus.resolve().status, anpr::RecognitionStatus::kInvalidFormat);
}

TEST("consensus reports no plate when nothing was observed") {
    auto consensus = makeConsensus(strictConfig());
    CHECK_EQ(consensus.resolve().status, anpr::RecognitionStatus::kNoPlate);
}

TEST("a weak character drags an otherwise confident reading down") {
    auto config = strictConfig();
    config.min_final_confidence = 0.72;
    auto consensus = makeConsensus(config);
    for (int i = 0; i < 3; ++i) {
        auto obs = observation("123ABC02", 0.90, 1000 + i * 100);
        obs.min_char_confidence = 0.15;
        consensus.add(obs);
    }
    CHECK(!consensus.satisfied());
}

TEST("consensus becomes satisfied as soon as the votes arrive") {
    auto consensus = makeConsensus(strictConfig());
    consensus.add(observation("123ABC02", 0.94, 1000));
    CHECK(!consensus.satisfied());
    consensus.add(observation("123ABC02", 0.94, 1100));
    CHECK(!consensus.satisfied());
    consensus.add(observation("123ABC02", 0.94, 1200));
    CHECK(consensus.satisfied());
}

TEST("consensus reset clears accumulated evidence") {
    auto consensus = makeConsensus(strictConfig());
    for (int i = 0; i < 3; ++i) {
        consensus.add(observation("123ABC02", 0.94, 1000 + i * 100));
    }
    CHECK(consensus.satisfied());
    consensus.reset();
    CHECK(!consensus.satisfied());
    CHECK_EQ(consensus.totalObservations(), 0);
}

TEST("corrected and clean readings of the same plate reinforce each other") {
    auto consensus = makeConsensus(strictConfig());
    consensus.add(observation("123ABC02", 0.93, 1000));
    consensus.add(observation("123A8C02", 0.85, 1100));  // repaired to 123ABC02
    consensus.add(observation("123ABC02", 0.95, 1200));
    const auto result = consensus.resolve();
    CHECK_EQ(result.normalized_plate, std::string("123ABC02"));
    CHECK_EQ(result.agreeing_observations, 3);
}
