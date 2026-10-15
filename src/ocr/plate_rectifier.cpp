#include "anpr/ocr/plate_rectifier.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include <opencv2/imgproc.hpp>
#include <opencv2/core/version.hpp>
#if CV_VERSION_MAJOR >= 5
#include <opencv2/geometry.hpp>
#endif

namespace anpr {
namespace {

std::array<cv::Point2f, 4> orderedCorners(const std::vector<cv::Point>& points) {
    const auto sum = [](const cv::Point& point) { return point.x + point.y; };
    const auto difference = [](const cv::Point& point) { return point.x - point.y; };
    return {*std::min_element(points.begin(), points.end(), [&](const auto& a, const auto& b) {
                return sum(a) < sum(b);
            }),
            *std::max_element(points.begin(), points.end(), [&](const auto& a, const auto& b) {
                return difference(a) < difference(b);
            }),
            *std::max_element(points.begin(), points.end(), [&](const auto& a, const auto& b) {
                return sum(a) < sum(b);
            }),
            *std::min_element(points.begin(), points.end(), [&](const auto& a, const auto& b) {
                return difference(a) < difference(b);
            })};
}

}  // namespace

bool rectifyPlate(const cv::Mat& crop, cv::Size output_size, cv::Mat& rectified) {
    if (crop.empty() || crop.depth() != CV_8U || (crop.channels() != 1 && crop.channels() != 3) ||
        crop.cols < 2 || crop.rows < 2 || output_size.width < 2 || output_size.height < 2) {
        return false;
    }
    cv::Mat gray;
    if (crop.channels() == 3) {
        cv::cvtColor(crop, gray, cv::COLOR_BGR2GRAY);
    } else {
        gray = crop;
    }
    std::array<cv::Mat, 2> masks;
    cv::threshold(gray, masks[0], 0, 255, cv::THRESH_BINARY | cv::THRESH_OTSU);
    cv::Canny(gray, masks[1], 40, 140);
    const cv::Mat kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3));
    const std::array<cv::Point2f, 4> crop_corners = {
        cv::Point2f(0, 0), cv::Point2f(static_cast<float>(crop.cols - 1), 0),
        cv::Point2f(static_cast<float>(crop.cols - 1), static_cast<float>(crop.rows - 1)),
        cv::Point2f(0, static_cast<float>(crop.rows - 1))};
    std::array<cv::Point2f, 4> best{};
    // Small bright shapes are letters and reflections, not the plate outline. The horizontal
    // aspect check matches the single-line plates this OCR model reads.
    double best_area = static_cast<double>(crop.total()) * 0.25;
    bool found = false;
    for (cv::Mat& mask : masks) {
        cv::morphologyEx(mask, mask, cv::MORPH_CLOSE, kernel);
        std::vector<std::vector<cv::Point>> contours;
        cv::findContours(mask, contours, cv::RETR_LIST, cv::CHAIN_APPROX_SIMPLE);
        for (const auto& contour : contours) {
            const double area = cv::contourArea(contour);
            if (area <= best_area) {
                continue;
            }
            std::vector<cv::Point> polygon;
            cv::approxPolyDP(contour, polygon, cv::arcLength(contour, true) * 0.03, true);
            if (polygon.size() != 4 || !cv::isContourConvex(polygon)) {
                continue;
            }
            const auto corners = orderedCorners(polygon);
            const double width = std::max(cv::norm(corners[1] - corners[0]),
                                          cv::norm(corners[2] - corners[3]));
            const double height = std::max(cv::norm(corners[3] - corners[0]),
                                           cv::norm(corners[2] - corners[1]));
            if (height < 1.0 || width < height * 1.8) {
                continue;
            }
            bool distinct = true;
            bool already_aligned = true;
            for (std::size_t index = 0; index < corners.size(); ++index) {
                already_aligned = already_aligned &&
                                  cv::norm(corners[index] - crop_corners[index]) <= 2.0;
                for (std::size_t other = 0; other < index; ++other) {
                    distinct = distinct && cv::norm(corners[index] - corners[other]) > 1.0;
                }
            }
            if (!distinct || already_aligned) {
                continue;
            }
            best = corners;
            best_area = area;
            found = true;
        }
    }
    if (!found) {
        return false;
    }
    const std::array<cv::Point2f, 4> target = {
        cv::Point2f(0, 0), cv::Point2f(static_cast<float>(output_size.width - 1), 0),
        cv::Point2f(static_cast<float>(output_size.width - 1),
                    static_cast<float>(output_size.height - 1)),
        cv::Point2f(0, static_cast<float>(output_size.height - 1))};
    const cv::Mat transform = cv::getPerspectiveTransform(best.data(), target.data());
    cv::warpPerspective(crop, rectified, transform, output_size, cv::INTER_LINEAR,
                        cv::BORDER_REPLICATE);
    return true;
}

}  // namespace anpr
