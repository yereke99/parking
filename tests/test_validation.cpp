#include "anpr/validation/plate_validator.hpp"

#include "test_framework.hpp"

namespace {

anpr::PlateValidator makeValidator() {
    return anpr::PlateValidator(anpr::defaultKazakhstanValidation());
}

}  // namespace

TEST("validator accepts the current private-car layout") {
    const auto plate = makeValidator().validate("123ABC02", 0.95);
    CHECK(plate.valid());
    CHECK_EQ(plate.normalized, std::string("123ABC02"));
    CHECK_EQ(plate.format_name, std::string("current_individual"));
    CHECK_EQ(plate.main, std::string("123ABC"));
    CHECK(plate.region_code.has_value());
    CHECK_EQ(*plate.region_code, std::string("02"));
    CHECK_EQ(*plate.region_name, std::string("Almaty"));
    CHECK_EQ(plate.status, anpr::PlateValidationStatus::kValidHighConfidence);
}

TEST("validator accepts the legal-entity layout") {
    const auto plate = makeValidator().validate("123AB01", 0.91);
    CHECK(plate.valid());
    CHECK_EQ(plate.format_name, std::string("current_legal_entity"));
    CHECK_EQ(plate.main, std::string("123AB"));
    CHECK_EQ(*plate.region_name, std::string("Astana"));
}

TEST("validator accepts the 1993 legacy layout") {
    const auto plate = makeValidator().validate("A123BCD", 0.93);
    CHECK(plate.valid());
    CHECK_EQ(plate.format_name, std::string("legacy_1993"));
    CHECK(!plate.region_code.has_value());
}

TEST("validator repairs 8 read as B in a letter slot") {
    const auto plate = makeValidator().validate("123A8C02", 0.78);
    CHECK(plate.valid());
    CHECK_EQ(plate.normalized, std::string("123ABC02"));
    CHECK_EQ(plate.corrections.size(), std::size_t{1});
    CHECK_EQ(plate.corrections[0].position, 4);
    CHECK_EQ(plate.corrections[0].from, '8');
    CHECK_EQ(plate.corrections[0].to, 'B');
}

TEST("validator repairs O read in a digit slot") {
    const auto plate = makeValidator().validate("1O3ABC02", 0.88);
    CHECK(plate.valid());
    CHECK_EQ(plate.normalized, std::string("103ABC02"));
    CHECK_EQ(plate.corrections.size(), std::size_t{1});
}

TEST("validator repairs 0 read in a letter slot") {
    const auto plate = makeValidator().validate("1230BC02", 0.88);
    CHECK(plate.valid());
    CHECK_EQ(plate.normalized, std::string("123OBC02"));
}

TEST("validator repairs 1 read in a letter slot") {
    const auto plate = makeValidator().validate("1231BC02", 0.88);
    CHECK(plate.valid());
    CHECK_EQ(plate.normalized, std::string("123IBC02"));
}

TEST("validator refuses to invent characters with no confusion partner") {
    // 'A' in a digit slot has no plausible digit twin, so the reading is rejected outright
    // instead of being repaired into something that looks valid.
    const auto plate = makeValidator().validate("12AABC02", 0.99);
    CHECK(!plate.valid());
    CHECK_EQ(plate.status, anpr::PlateValidationStatus::kInvalidFormat);
}

TEST("validator rejects an unknown region code") {
    const auto plate = makeValidator().validate("123ABC77", 0.99);
    CHECK(!plate.valid());
}

TEST("validator reports too many corrections as ambiguous") {
    auto config = anpr::defaultKazakhstanValidation();
    config.max_corrections = 1;
    const anpr::PlateValidator validator(config);
    // Two letter slots hold digits here, which exceeds the correction budget of one.
    const auto plate = validator.validate("12380C02", 0.90);
    CHECK(!plate.valid());
    CHECK_EQ(plate.status, anpr::PlateValidationStatus::kAmbiguous);
}

TEST("validator rejects partial and empty readings") {
    const auto validator = makeValidator();
    CHECK(!validator.validate("123AB", 0.99).valid());
    CHECK(!validator.validate("", 0.99).valid());
    CHECK(!validator.validate("!!!", 0.99).valid());
    CHECK(!validator.validate("123ABC020", 0.99).valid());
}

TEST("validator sanitises separators and case") {
    const auto plate = makeValidator().validate(" 123 abc 02 ", 0.95);
    CHECK(plate.valid());
    CHECK_EQ(plate.normalized, std::string("123ABC02"));
    CHECK_EQ(plate.raw, std::string("123ABC02"));
}

TEST("validator lowers the status when confidence is low") {
    const auto plate = makeValidator().validate("123ABC02", 0.50);
    CHECK(plate.valid());
    CHECK_EQ(plate.status, anpr::PlateValidationStatus::kValidLowConfidence);
}

TEST("validator formats come from configuration") {
    anpr::ValidationConfig config = anpr::defaultKazakhstanValidation();
    config.formats = {anpr::PlateFormat{"custom_four", "LLDD", false, 1.0}};
    const anpr::PlateValidator validator(config);
    CHECK(validator.validate("AB12", 0.95).valid());
    CHECK(!validator.validate("123ABC02", 0.95).valid());
    // allow_corrections is off for this layout, so no repair is attempted.
    CHECK(!validator.validate("A812", 0.95).valid());
}

TEST("validator prefers the layout that needs fewer repairs") {
    // Seven characters match both the legal-entity layout and the legacy layout. The reading is
    // clean under the legal-entity layout and would need a repair under the legacy one.
    const auto plate = makeValidator().validate("123AB02", 0.95);
    CHECK(plate.valid());
    CHECK_EQ(plate.format_name, std::string("current_legal_entity"));
}
