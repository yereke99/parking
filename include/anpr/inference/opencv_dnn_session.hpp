#pragma once

#include <memory>
#include <string>

#include "anpr/inference/inference_session.hpp"

namespace anpr {

/// OpenCV DNN fallback, for machines without ONNX Runtime.
///
/// Only float32 NCHW inputs are supported. Whether a model loads depends on the OpenCV build's
/// ONNX importer, so it is a development last resort; production profiles never select it.
std::unique_ptr<IInferenceSession> createOpenCvDnnSession(const SessionRequest& request,
                                                          std::string& error);

}  // namespace anpr
