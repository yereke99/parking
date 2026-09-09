#pragma once

#include <memory>
#include <string>

#include "anpr/inference/inference_session.hpp"

namespace anpr {

/// Builds an ONNX Runtime session for `backend`. Only compiled when ONNX Runtime is linked.
/// Returns nullptr and fills `error` when that provider is not usable on this machine.
std::unique_ptr<IInferenceSession> createOnnxRuntimeSession(const SessionRequest& request,
                                                            InferenceBackend backend,
                                                            std::string& error);

}  // namespace anpr
