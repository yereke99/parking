#pragma once

#include <memory>
#include <string>

#include "anpr/inference/inference_session.hpp"

namespace anpr {

/// Creates a native TensorRT 8.x session. The ONNX network is compiled once on the Jetson and
/// the serialized engine is reused from inference.engine_cache_dir on later runs.
std::unique_ptr<IInferenceSession> createTensorRTSession(const SessionRequest& request,
                                                         std::string& error);

/// True only when this binary was linked against the native TensorRT and CUDA runtimes.
[[nodiscard]] bool tensorRTAvailable();

}  // namespace anpr
