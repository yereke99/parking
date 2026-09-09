#pragma once

#include <memory>
#include <string>

#include "anpr/inference/inference_session.hpp"

namespace anpr {

/// OpenCV DNN fallback, for machines without ONNX Runtime.
///
/// Only float32 NCHW inputs are supported, which is enough for the YOLO detector and not enough
/// for Fast Plate OCR. It exists so a development box with nothing but OpenCV can still run the
/// detection half of the pipeline.
std::unique_ptr<IInferenceSession> createOpenCvDnnSession(const SessionRequest& request,
                                                          std::string& error);

}  // namespace anpr
