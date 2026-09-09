#include "anpr/pipeline/quality_assessor.hpp"

#include <opencv2/imgproc.hpp>

#include "test_framework.hpp"

namespace {

anpr::QualityConfig qualityConfig() {
    anpr::QualityConfig config;
    config.min_plate_width_px = 90;
    config.min_plate_height_px = 22;
    config.min_sharpness = 45.0;
    config.min_brightness = 35.0;
    config.max_brightness = 225.0;
    config.max_clipping_ratio = 0.22;
    config.min_score = 0.30;
    return config;
}

/// A synthetic plate: light background with dark bars, which is what a Laplacian sees on a real
/// plate's characters.
cv::Mat syntheticPlate(int width, int height, int background, int foreground) {
    cv::Mat plate(height, width, CV_8UC3, cv::Scalar::all(background));
    for (int x = 10; x + 8 < width; x += 20) {
        cv::rectangle(plate, cv::Rect(x, height / 5, 8, height * 3 / 5),
                      cv::Scalar::all(foreground), cv::FILLED);
    }
    return plate;
}

}  // namespace

TEST("a clean crop passes the quality gate") {
    anpr::QualityAssessor assessor(qualityConfig());
    const cv::Mat plate = syntheticPlate(200, 60, 210, 30);
    const auto quality = assessor.evaluate(plate, anpr::BoundingBox{100, 100, 200, 60});
    CHECK(quality.acceptable());
    CHECK(quality.score > 0.30);
    CHECK(quality.sharpness > 45.0);
}

TEST("a crop below the minimum size is rejected before anything is measured") {
    anpr::QualityAssessor assessor(qualityConfig());
    const cv::Mat plate = syntheticPlate(60, 18, 210, 30);
    const auto quality = assessor.evaluate(plate, anpr::BoundingBox{100, 100, 60, 18});
    CHECK(!quality.acceptable());
    CHECK_EQ(quality.rejection, anpr::QualityRejection::kTooSmall);
}

TEST("a blurred crop is rejected") {
    anpr::QualityAssessor assessor(qualityConfig());
    cv::Mat plate = syntheticPlate(200, 60, 210, 30);
    cv::GaussianBlur(plate, plate, cv::Size(31, 31), 0.0);
    const auto quality = assessor.evaluate(plate, anpr::BoundingBox{100, 100, 200, 60});
    CHECK(!quality.acceptable());
    CHECK_EQ(quality.rejection, anpr::QualityRejection::kTooBlurred);
}

TEST("a crop that is nearly black is rejected") {
    anpr::QualityAssessor assessor(qualityConfig());
    const cv::Mat plate(60, 200, CV_8UC3, cv::Scalar::all(4));
    const auto quality = assessor.evaluate(plate, anpr::BoundingBox{100, 100, 200, 60});
    CHECK(!quality.acceptable());
    // A flat dark crop trips the blur gate first, which is measured before exposure.
    CHECK(quality.rejection == anpr::QualityRejection::kTooBlurred ||
          quality.rejection == anpr::QualityRejection::kTooDark);
}

TEST("an overexposed crop is rejected") {
    anpr::QualityAssessor assessor(qualityConfig());
    cv::Mat plate = syntheticPlate(200, 60, 254, 250);
    const auto quality = assessor.evaluate(plate, anpr::BoundingBox{100, 100, 200, 60});
    CHECK(!quality.acceptable());
}

TEST("an empty crop is rejected without crashing") {
    anpr::QualityAssessor assessor(qualityConfig());
    const auto quality = assessor.evaluate(cv::Mat{}, anpr::BoundingBox{0, 0, 0, 0});
    CHECK(!quality.acceptable());
    CHECK_EQ(quality.rejection, anpr::QualityRejection::kTooSmall);
}

TEST("enhancement stays off unless it is enabled") {
    anpr::QualityAssessor assessor(qualityConfig());
    const cv::Mat plate = syntheticPlate(200, 60, 120, 90);
    const auto quality = assessor.evaluate(plate, anpr::BoundingBox{100, 100, 200, 60});
    cv::Mat enhanced;
    CHECK(!assessor.enhance(plate, quality, enhanced));
    CHECK(enhanced.empty());
}

TEST("enhancement runs only for a low-scoring crop when enabled") {
    auto config = qualityConfig();
    config.enable_enhancement = true;
    config.enhance_below_score = 0.45;
    anpr::QualityAssessor assessor(config);

    const cv::Mat good = syntheticPlate(200, 60, 210, 30);
    const auto good_quality = assessor.evaluate(good, anpr::BoundingBox{100, 100, 200, 60});
    cv::Mat enhanced;
    CHECK(!assessor.enhance(good, good_quality, enhanced));

    anpr::ImageQuality poor = good_quality;
    poor.score = 0.20;
    CHECK(assessor.enhance(good, poor, enhanced));
    CHECK_EQ(enhanced.size(), good.size());
    CHECK_EQ(enhanced.channels(), 3);
}
