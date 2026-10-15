#include "anpr/ocr/plate_rectifier.hpp"

#include <array>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/core/version.hpp>
#if CV_VERSION_MAJOR >= 5
#include <opencv2/geometry.hpp>
#endif

#include "test_framework.hpp"

namespace {

cv::Mat skewedPlate(bool grayscale) {
    cv::Mat plate(50, 200, CV_8UC3, cv::Scalar::all(220));
    for (int x = 35; x < 170; x += 20) {
        cv::rectangle(plate, cv::Rect(x, 15, 7, 20), cv::Scalar::all(20), cv::FILLED);
    }
    cv::rectangle(plate, cv::Rect(8, 7, 12, 12), cv::Scalar(0, 0, 220), cv::FILLED);
    cv::rectangle(plate, cv::Rect(180, 7, 12, 12), cv::Scalar(0, 220, 0), cv::FILLED);
    cv::rectangle(plate, cv::Rect(180, 31, 12, 12), cv::Scalar(220, 0, 0), cv::FILLED);
    const std::array<cv::Point2f, 4> source = {
        cv::Point2f(0, 0), cv::Point2f(199, 0), cv::Point2f(199, 49), cv::Point2f(0, 49)};
    const std::array<cv::Point2f, 4> destination = {
        cv::Point2f(15, 40), cv::Point2f(270, 10), cv::Point2f(285, 100), cv::Point2f(5, 130)};
    cv::Mat image;
    cv::warpPerspective(plate, image,
                        cv::getPerspectiveTransform(source.data(), destination.data()),
                        cv::Size(300, 140));
    if (grayscale) {
        cv::cvtColor(image, image, cv::COLOR_BGR2GRAY);
    }
    return image;
}

}  // namespace

TEST("plate rectification removes perspective without flipping the plate or changing its source") {
    const cv::Mat crop = skewedPlate(false);
    const cv::Mat original = crop.clone();
    cv::Mat aligned;
    CHECK(anpr::rectifyPlate(crop, cv::Size(200, 50), aligned));
    CHECK_EQ(aligned.cols, 200);
    CHECK_EQ(aligned.rows, 50);
    CHECK_EQ(aligned.channels(), 3);
    CHECK_NEAR(cv::norm(crop, original, cv::NORM_INF), 0.0, 0.0);
    const auto red = cv::mean(aligned(cv::Rect(11, 10, 5, 5)));
    const auto green = cv::mean(aligned(cv::Rect(183, 10, 5, 5)));
    const auto blue = cv::mean(aligned(cv::Rect(183, 34, 5, 5)));
    CHECK(red[2] > red[0] + 40 && red[2] > red[1] + 40);
    CHECK(green[1] > green[0] + 40 && green[1] > green[2] + 40);
    CHECK(blue[0] > blue[1] + 40 && blue[0] > blue[2] + 40);
}

TEST("plate rectification handles a grayscale crop") {
    const cv::Mat crop = skewedPlate(true);
    cv::Mat aligned;
    CHECK(anpr::rectifyPlate(crop, cv::Size(200, 50), aligned));
    CHECK_EQ(aligned.channels(), 1);
    CHECK_EQ(aligned.size(), cv::Size(200, 50));
}

TEST("plate rectification leaves aligned and outline-free crops alone") {
    cv::Mat crop(50, 200, CV_8UC3, cv::Scalar::all(180));
    cv::putText(crop, "123ABC02", cv::Point(10, 35), cv::FONT_HERSHEY_SIMPLEX, 0.65,
                cv::Scalar::all(30), 2);
    cv::Mat aligned;
    CHECK(!anpr::rectifyPlate(crop, cv::Size(200, 50), aligned));
    CHECK(aligned.empty());
    const cv::Mat blank(50, 200, CV_8UC3, cv::Scalar::all(0));
    CHECK(!anpr::rectifyPlate(blank, cv::Size(200, 50), aligned));
}

TEST("plate rectification rejects empty or unsupported buffers safely") {
    cv::Mat aligned;
    CHECK(!anpr::rectifyPlate(cv::Mat{}, cv::Size(200, 50), aligned));
    CHECK(!anpr::rectifyPlate(cv::Mat(50, 200, CV_16UC3), cv::Size(200, 50), aligned));
    CHECK(!anpr::rectifyPlate(cv::Mat(50, 200, CV_8UC4), cv::Size(200, 50), aligned));
    CHECK(!anpr::rectifyPlate(skewedPlate(false), cv::Size(0, 50), aligned));
}

TEST("the oblique plate from IMG_5666 is rectified before OCR") {
    // First frame of the repository's IMG_5666.mp4, extracted by the existing detector. Raw
    // resizing read 702BA681/702BAC03 across neighbouring frames; rectification reads 702BAC01.
    const cv::Mat crop = cv::imread("tests/fixtures/plate_skewed_702BAC01.png");
    CHECK(!crop.empty());
    const cv::Mat original = crop.clone();
    cv::Mat aligned;
    CHECK(anpr::rectifyPlate(crop, cv::Size(200, 50), aligned));
    CHECK_EQ(aligned.size(), cv::Size(200, 50));
    CHECK_NEAR(cv::norm(crop, original, cv::NORM_INF), 0.0, 0.0);
    cv::Scalar mean;
    cv::Scalar deviation;
    cv::meanStdDev(aligned, mean, deviation);
    CHECK(deviation[0] > 40.0);
}
