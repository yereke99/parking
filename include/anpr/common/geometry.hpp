#pragma once

#include <algorithm>
#include <cmath>

namespace anpr {

struct BoundingBox {
    int x{0};
    int y{0};
    int width{0};
    int height{0};

    [[nodiscard]] int area() const { return width * height; }
    [[nodiscard]] bool empty() const { return width <= 0 || height <= 0; }
    [[nodiscard]] double centerX() const { return x + width * 0.5; }
    [[nodiscard]] double centerY() const { return y + height * 0.5; }
    [[nodiscard]] int right() const { return x + width; }
    [[nodiscard]] int bottom() const { return y + height; }
};

/// Intersection over union of two boxes. Zero when either box is degenerate.
[[nodiscard]] inline double iou(const BoundingBox& lhs, const BoundingBox& rhs) {
    if (lhs.empty() || rhs.empty()) {
        return 0.0;
    }
    const int x1 = std::max(lhs.x, rhs.x);
    const int y1 = std::max(lhs.y, rhs.y);
    const int x2 = std::min(lhs.right(), rhs.right());
    const int y2 = std::min(lhs.bottom(), rhs.bottom());
    const int w = x2 - x1;
    const int h = y2 - y1;
    if (w <= 0 || h <= 0) {
        return 0.0;
    }
    const double intersection = static_cast<double>(w) * static_cast<double>(h);
    const double union_area =
        static_cast<double>(lhs.area()) + static_cast<double>(rhs.area()) - intersection;
    return union_area > 0.0 ? intersection / union_area : 0.0;
}

/// Euclidean distance between box centres, in pixels.
[[nodiscard]] inline double centerDistance(const BoundingBox& lhs, const BoundingBox& rhs) {
    const double dx = lhs.centerX() - rhs.centerX();
    const double dy = lhs.centerY() - rhs.centerY();
    return std::sqrt(dx * dx + dy * dy);
}

struct NormalizedRect {
    double x{0.0};
    double y{0.0};
    double width{1.0};
    double height{1.0};

    [[nodiscard]] bool valid() const {
        return x >= 0.0 && y >= 0.0 && width > 0.0 && height > 0.0 && x + width <= 1.000001 &&
               y + height <= 1.000001;
    }

    [[nodiscard]] BoundingBox toPixels(int image_width, int image_height) const {
        BoundingBox box{
            static_cast<int>(x * image_width),
            static_cast<int>(y * image_height),
            static_cast<int>(width * image_width),
            static_cast<int>(height * image_height),
        };
        box.x = std::clamp(box.x, 0, std::max(0, image_width - 1));
        box.y = std::clamp(box.y, 0, std::max(0, image_height - 1));
        box.width = std::clamp(box.width, 0, image_width - box.x);
        box.height = std::clamp(box.height, 0, image_height - box.y);
        return box;
    }

    /// True when the normalized point falls inside the rectangle.
    [[nodiscard]] bool containsNormalized(double px, double py) const {
        return px >= x && px <= x + width && py >= y && py <= y + height;
    }
};

inline BoundingBox clampBox(BoundingBox box, int image_width, int image_height) {
    box.x = std::clamp(box.x, 0, std::max(0, image_width - 1));
    box.y = std::clamp(box.y, 0, std::max(0, image_height - 1));
    box.width = std::clamp(box.width, 0, image_width - box.x);
    box.height = std::clamp(box.height, 0, image_height - box.y);
    return box;
}

/// True when the centre of `box` lies inside `roi`, both expressed for the same image size.
[[nodiscard]] inline bool centerInside(const BoundingBox& box, const NormalizedRect& roi,
                                       int image_width, int image_height) {
    if (box.empty() || image_width <= 0 || image_height <= 0) {
        return false;
    }
    return roi.containsNormalized(box.centerX() / image_width, box.centerY() / image_height);
}

}  // namespace anpr
