#pragma once

#include <opencv2/core.hpp>

namespace anpr {

/// Finds a large, convex plate outline inside an axis-aligned detector crop and removes its
/// perspective before OCR. Returns false when no reliable outline is found or the crop is
/// already aligned; callers then keep the original. The input pixels are never changed.
bool rectifyPlate(const cv::Mat& crop, cv::Size output_size, cv::Mat& rectified);

}  // namespace anpr
