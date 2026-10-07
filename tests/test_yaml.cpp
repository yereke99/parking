#include "anpr/common/yaml.hpp"

#include "test_framework.hpp"

using anpr::yaml::asBool;
using anpr::yaml::asDouble;
using anpr::yaml::asInt;
using anpr::yaml::asString;

TEST("yaml parses nested mappings") {
    const auto result = anpr::yaml::parse(R"(
camera:
  source: video/parking.mp4
  fps: 25
detector:
  intervals:
    idle_ms: 200
)");
    CHECK(result.ok);
    CHECK_EQ(asString(result.root.path("camera.source")).value_or(""), std::string("video/parking.mp4"));
    CHECK_EQ(asInt(result.root.path("camera.fps")).value_or(0), 25);
    CHECK_EQ(asInt(result.root.path("detector.intervals.idle_ms")).value_or(0), 200);
    CHECK(result.root.path("camera.missing") == nullptr);
}

TEST("yaml parses inline flow sequences") {
    const auto result = anpr::yaml::parse("roi:\n  stop: [0.1, 0.28, 0.8, 0.55]\n");
    CHECK(result.ok);
    const auto* node = result.root.path("roi.stop");
    CHECK(node != nullptr);
    CHECK(node->isSequence());
    CHECK_EQ(node->sequence().size(), std::size_t{4});
    CHECK_NEAR(asDouble(&node->sequence()[1]).value_or(0.0), 0.28, 1e-9);
}

TEST("yaml parses a sequence of mappings") {
    const auto result = anpr::yaml::parse(R"(
validation:
  formats:
    - name: current_individual
      pattern: DDDLLLRR
      weight: 1.0
    - name: legacy
      pattern: LDDDLLL
      allow_corrections: false
)");
    CHECK(result.ok);
    const auto* formats = result.root.path("validation.formats");
    CHECK(formats != nullptr);
    CHECK(formats->isSequence());
    CHECK_EQ(formats->sequence().size(), std::size_t{2});
    CHECK_EQ(asString(formats->sequence()[0].find("name")).value_or(""),
             std::string("current_individual"));
    CHECK_EQ(asString(formats->sequence()[0].find("pattern")).value_or(""),
             std::string("DDDLLLRR"));
    CHECK_NEAR(asDouble(formats->sequence()[0].find("weight")).value_or(0.0), 1.0, 1e-9);
    CHECK_EQ(asString(formats->sequence()[1].find("pattern")).value_or(""), std::string("LDDDLLL"));
    CHECK_EQ(asBool(formats->sequence()[1].find("allow_corrections")).value_or(true), false);
}

TEST("yaml keeps quoted values and strips comments") {
    const auto result = anpr::yaml::parse(R"(
ocr:
  alphabet: "0123456789ABC_"   # trailing comment
  note: 'value # not a comment'
)");
    CHECK(result.ok);
    CHECK_EQ(asString(result.root.path("ocr.alphabet")).value_or(""),
             std::string("0123456789ABC_"));
    CHECK_EQ(asString(result.root.path("ocr.note")).value_or(""),
             std::string("value # not a comment"));
}

TEST("yaml keeps colons inside values") {
    const auto result = anpr::yaml::parse("camera:\n  source: rtsp://user:pass@10.0.0.5/stream1\n");
    CHECK(result.ok);
    CHECK_EQ(asString(result.root.path("camera.source")).value_or(""),
             std::string("rtsp://user:pass@10.0.0.5/stream1"));
}

TEST("yaml rejects tabs and malformed lines") {
    const auto tabbed = anpr::yaml::parse("camera:\n\tsource: a\n");
    CHECK(!tabbed.ok);
    const auto malformed = anpr::yaml::parse("camera:\n  just_a_word\n");
    CHECK(!malformed.ok);
}

TEST("yaml scalar conversions reject junk") {
    const auto result = anpr::yaml::parse("a:\n  n: 12x\n  b: maybe\n  f: 1.5\n");
    CHECK(result.ok);
    CHECK(!asInt(result.root.path("a.n")).has_value());
    CHECK(!asBool(result.root.path("a.b")).has_value());
    CHECK_NEAR(asDouble(result.root.path("a.f")).value_or(0.0), 1.5, 1e-9);
}
