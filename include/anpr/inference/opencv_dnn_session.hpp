#pragma once

#include <memory>
#include <string>

#include "anpr/inference/inference_session.hpp"

namespace anpr {

/// OpenCV DNN fallback, for machines without ONNX Runtime.
///
/// Only float32 NCHW inputs are supported, which is enough for the YOLO detector and not enough
/// for the legacy Fast Plate OCR backend. It exists so a development box with nothing but OpenCV
/// can run the detector; the default Nomeroff worker is independent of this session type.
std::unique_ptr<IInferenceSession> createOpenCvDnnSession(const SessionRequest& request,
                                                          std::string& error);

}  // namespace anpr
