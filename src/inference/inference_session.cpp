#include "anpr/inference/inference_session.hpp"

#include <algorithm>
#include <numeric>

#include "anpr/common/filesystem.hpp"
#include "anpr/common/logging.hpp"
#include "anpr/inference/opencv_dnn_session.hpp"

#ifdef KZ_ANPR_WITH_ONNXRUNTIME
#include "anpr/inference/onnxruntime_session.hpp"
#endif
#ifdef KZ_ANPR_WITH_TENSORRT
#include "anpr/inference/tensorrt_session.hpp"
#endif

namespace anpr {
namespace {

/// Order in which backends are attempted for a given request. Hardware selection lives here and
/// only here, so no business-logic file needs a preprocessor branch.
std::vector<InferenceBackend> backendOrder(InferenceBackend requested) {
    switch (requested) {
        case InferenceBackend::kAuto:
            return {InferenceBackend::kTensorRT, InferenceBackend::kOnnxCuda,
                    InferenceBackend::kOnnxCpu, InferenceBackend::kOpenCvDnn};
        case InferenceBackend::kTensorRT:
            // The detector stays strict native TensorRT (`strict_backend`); the OCR allows the
            // ONNX Runtime fallback so a refused engine never stops the barrier.
            return {InferenceBackend::kTensorRT, InferenceBackend::kOnnxCuda,
                    InferenceBackend::kOnnxCpu};
        case InferenceBackend::kOnnxCuda:
            return {InferenceBackend::kOnnxCuda, InferenceBackend::kOnnxCpu};
        case InferenceBackend::kOnnxCpu:
            return {InferenceBackend::kOnnxCpu, InferenceBackend::kOpenCvDnn};
        case InferenceBackend::kOpenCvDnn:
            return {InferenceBackend::kOpenCvDnn};
    }
    return {};
}

}  // namespace

std::size_t TensorSpec::elementCount() const {
    if (shape.empty()) {
        return 0;
    }
    return static_cast<std::size_t>(std::accumulate(shape.begin(), shape.end(),
                                                    static_cast<std::int64_t>(1),
                                                    std::multiplies<std::int64_t>()));
}

std::size_t TensorSpec::byteSize() const {
    const std::size_t element_size = type == TensorType::kUInt8 ? 1 : sizeof(float);
    return elementCount() * element_size;
}

std::size_t IInferenceSession::findInput(const std::string& name) const {
    const auto& specs = inputs();
    for (std::size_t i = 0; i < specs.size(); ++i) {
        if (specs[i].name == name) {
            return i;
        }
    }
    return static_cast<std::size_t>(-1);
}

std::size_t IInferenceSession::findOutput(const std::string& name) const {
    const auto& specs = outputs();
    for (std::size_t i = 0; i < specs.size(); ++i) {
        if (specs[i].name == name) {
            return i;
        }
    }
    return static_cast<std::size_t>(-1);
}

std::unique_ptr<IInferenceSession> createInferenceSession(const SessionRequest& request,
                                                          std::string& error) {
    if (!filesystem::exists(request.model_path)) {
        error = "MODEL_NOT_FOUND: " + request.model_path;
        return nullptr;
    }

    const std::vector<InferenceBackend> order = backendOrder(request.inference.backend);
    std::string last_error;

    for (const InferenceBackend backend : order) {
        std::string attempt_error;
        std::unique_ptr<IInferenceSession> session;

        if (backend == InferenceBackend::kOpenCvDnn) {
            session = createOpenCvDnnSession(request, attempt_error);
        } else if (backend == InferenceBackend::kTensorRT) {
#ifdef KZ_ANPR_WITH_TENSORRT
            session = createTensorRTSession(request, attempt_error);
#else
            attempt_error = "this build does not link native TensorRT";
#endif
        } else {
#ifdef KZ_ANPR_WITH_ONNXRUNTIME
            session = createOnnxRuntimeSession(request, backend, attempt_error);
#else
            attempt_error = "this build does not link ONNX Runtime";
#endif
        }

        if (session != nullptr) {
            logEvent(LogLevel::kInfo, "model_loaded",
                     LogFields()
                         .add("tag", request.tag)
                         .add("model", request.model_path)
                         .add("backend", session->backendName())
                         .add("inputs", session->inputs().size())
                         .add("outputs", session->outputs().size()));
            return session;
        }

        last_error = attempt_error;
        logEvent(LogLevel::kWarn, "backend_unavailable",
                 LogFields()
                     .add("tag", request.tag)
                     .add("backend", toString(backend))
                     .add("reason", attempt_error));

        if (request.inference.strict_backend) {
            error = "BACKEND_UNAVAILABLE: " + toString(request.inference.backend) + ": " +
                    attempt_error;
            return nullptr;
        }
    }

    error = "BACKEND_UNAVAILABLE: no usable inference backend for " + request.model_path + ": " +
            last_error;
    return nullptr;
}

#ifndef KZ_ANPR_WITH_ONNXRUNTIME
bool onnxRuntimeAvailable() {
    return false;
}

std::vector<std::string> availableProviders() {
    return {};
}
#endif

#ifndef KZ_ANPR_WITH_TENSORRT
bool tensorRTAvailable() {
    return false;
}
#endif

}  // namespace anpr
