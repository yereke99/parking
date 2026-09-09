#pragma once

#include "anpr/common/geometry.hpp"

namespace anpr {

/// One plate detection in full-frame pixel coordinates.
struct Detection {
    BoundingBox box;
    float confidence{0.0F};
    int class_id{0};
};

}  // namespace anpr
