#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "anpr/common/config.hpp"

namespace anpr {

enum class TensorType { kFloat32, kUInt8 };

struct TensorSpec {
    std::string name;
    TensorType type{TensorType::kFloat32};
    /// Shape with the batch dimension already resolved to 1.
    std::vector<std::int64_t> shape;

    [[nodiscard]] std::size_t elementCount() const;
    [[nodiscard]] std::size_t byteSize() const;
};

/// One loaded model, ready to run, with its host buffers allocated once.
///
/// The contract is deliberately buffer-oriented rather than value-oriented: the caller writes
/// straight into `inputBuffer` and reads straight out of `outputData`. No tensors, matrices or
/// vectors are constructed per frame, which is what keeps the frame loop allocation-free during
/// continuous operation.
///
/// Sessions are not thread-safe. The pipeline owns one detector session and one OCR session and
/// runs both from the processing thread.
class IInferenceSession {
public:
    virtual ~IInferenceSession() = default;

    [[nodiscard]] virtual const std::vector<TensorSpec>& inputs() const = 0;
    [[nodiscard]] virtual const std::vector<TensorSpec>& outputs() const = 0;

    /// Writable host buffer for one input. Valid for the lifetime of the session.
    [[nodiscard]] virtual void* inputBuffer(std::size_t index) = 0;

    /// Executes the model over the current contents of the input buffers.
    /// Returns false and fills `error` on a recoverable failure; the caller drops the frame.
    virtual bool run(std::string& error) = 0;

    /// Float output data. Valid until the next `run`.
    [[nodiscard]] virtual const float* outputData(std::size_t index) const = 0;
    [[nodiscard]] virtual const std::vector<std::int64_t>& outputShape(std::size_t index) const = 0;

    /// The provider actually in use, for logging. May differ from the requested backend.
    [[nodiscard]] virtual std::string backendName() const = 0;

    /// Index of a named input or output, or SIZE_MAX.
    [[nodiscard]] std::size_t findInput(const std::string& name) const;
    [[nodiscard]] std::size_t findOutput(const std::string& name) const;
};

struct SessionRequest {
    std::string model_path;
    InferenceConfig inference;
    /// Names this session in log lines, for example "detector" or "ocr".
    std::string tag;
    /// Square input edge, used only by the OpenCV DNN fallback, which cannot read the ONNX
    /// input signature itself. ONNX Runtime takes the shape from the model.
    int input_size_hint{640};
};

/// Builds a session for the requested backend, falling back through the available providers
/// unless `inference.strict_backend` forbids it. Returns nullptr and fills `error` on failure.
std::unique_ptr<IInferenceSession> createInferenceSession(const SessionRequest& request,
                                                          std::string& error);

/// True when this build links ONNX Runtime.
[[nodiscard]] bool onnxRuntimeAvailable();

/// Providers ONNX Runtime reports on this machine. Empty when ONNX Runtime is not linked.
[[nodiscard]] std::vector<std::string> availableProviders();

/// True when this binary links the native JetPack TensorRT/CUDA implementation.
[[nodiscard]] bool tensorRTAvailable();

}  // namespace anpr
