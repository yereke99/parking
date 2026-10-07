#include "anpr/inference/inference_session.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <numeric>
#include <set>

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
std::vector<InferenceBackend> backendOrder(InferenceBackend requested, bool requires_uint8) {
    std::vector<InferenceBackend> order;
    switch (requested) {
        case InferenceBackend::kAuto:
            order = {InferenceBackend::kTensorRT, InferenceBackend::kOnnxCuda,
                     InferenceBackend::kOnnxCpu, InferenceBackend::kOpenCvDnn};
            break;
        case InferenceBackend::kTensorRT:
            // TensorRT 8.2 does not accept the published OCR model's uint8 input. This is a known,
            // explicit per-model CPU fallback; the detector remains strict native TensorRT, and
            // the float32-input OCR copy built into the Jetson image runs on TensorRT too.
            order = requires_uint8
                        ? std::vector<InferenceBackend>{InferenceBackend::kOnnxCpu}
                        : std::vector<InferenceBackend>{InferenceBackend::kTensorRT,
                                                        InferenceBackend::kOnnxCuda,
                                                        InferenceBackend::kOnnxCpu};
            break;
        case InferenceBackend::kOnnxCuda:
            order = {InferenceBackend::kOnnxCuda, InferenceBackend::kOnnxCpu};
            break;
        case InferenceBackend::kOnnxCpu:
            order = {InferenceBackend::kOnnxCpu, InferenceBackend::kOpenCvDnn};
            break;
        case InferenceBackend::kOpenCvDnn:
            order = {InferenceBackend::kOpenCvDnn};
            break;
    }
    if (requires_uint8) {
        order.erase(std::remove(order.begin(), order.end(), InferenceBackend::kOpenCvDnn),
                    order.end());
    }
    return order;
}

// A minimal protobuf reader, enough to find an ONNX graph's input types without linking a
// runtime or the protobuf library. Field numbers are from onnx.proto (stable since IR version 3).
struct ProtoField {
    std::uint64_t number{0};
    std::uint64_t wire{0};
    std::size_t begin{0};  ///< value bytes; for length-delimited fields, the payload
    std::size_t end{0};
    std::uint64_t varint{0};
};

bool readVarint(const std::string& data, std::size_t& pos, std::size_t end, std::uint64_t& value) {
    value = 0;
    for (unsigned shift = 0; shift < 64U; shift += 7U) {
        if (pos >= end) return false;
        const auto byte = static_cast<unsigned char>(data[pos++]);
        value |= static_cast<std::uint64_t>(byte & 0x7FU) << shift;
        if ((byte & 0x80U) == 0U) return true;
    }
    return false;
}

/// Splits one message into its fields. False for anything that is not well-formed protobuf.
bool protoFields(const std::string& data, std::size_t begin, std::size_t end,
                 std::vector<ProtoField>& fields) {
    fields.clear();
    std::size_t pos = begin;
    while (pos < end) {
        std::uint64_t key = 0;
        if (!readVarint(data, pos, end, key)) return false;
        ProtoField field;
        field.number = key >> 3U;
        field.wire = key & 7U;
        field.begin = pos;
        if (field.wire == 0U) {
            if (!readVarint(data, pos, end, field.varint)) return false;
        } else if (field.wire == 2U) {
            std::uint64_t length = 0;
            if (!readVarint(data, pos, end, length) || length > end - pos) return false;
            field.begin = pos;
            pos += static_cast<std::size_t>(length);
        } else if (field.wire == 1U || field.wire == 5U) {
            const std::size_t width = field.wire == 1U ? 8U : 4U;
            if (end - pos < width) return false;
            pos += width;
        } else {
            return false;  // groups are not used by ONNX
        }
        field.end = pos;
        fields.push_back(field);
    }
    return true;
}

/// Payload of the last length-delimited occurrence of `number`, as protobuf merging defines.
const ProtoField* lastMessage(const std::vector<ProtoField>& fields, std::uint64_t number) {
    const ProtoField* found = nullptr;
    for (const ProtoField& field : fields) {
        if (field.number == number && field.wire == 2U) found = &field;
    }
    return found;
}

bool stringField(const std::string& data, const ProtoField& message, std::uint64_t number,
                 std::string& value) {
    std::vector<ProtoField> fields;
    if (!protoFields(data, message.begin, message.end, fields)) return false;
    const ProtoField* found = lastMessage(fields, number);
    value = found == nullptr ? std::string() : data.substr(found->begin, found->end - found->begin);
    return true;
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

    const std::vector<InferenceBackend> order =
        backendOrder(request.inference.backend, request.requires_uint8_input);
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

std::optional<int> onnxInputElementTypeOf(const std::string& data, std::string& error) {
    constexpr std::uint64_t kModelGraph = 7;
    constexpr std::uint64_t kGraphInitializer = 5;
    constexpr std::uint64_t kGraphInput = 11;
    constexpr std::uint64_t kTensorName = 8;
    constexpr std::uint64_t kValueInfoName = 1;
    constexpr std::uint64_t kValueInfoType = 2;
    constexpr std::uint64_t kTypeTensor = 1;
    constexpr std::uint64_t kTensorElemType = 1;
    const std::string malformed = "not a well-formed ONNX protobuf";

    std::vector<ProtoField> model;
    if (!protoFields(data, 0, data.size(), model)) {
        error = malformed;
        return std::nullopt;
    }
    const ProtoField* graph_field = lastMessage(model, kModelGraph);
    if (graph_field == nullptr) {
        error = "the ONNX model has no graph";
        return std::nullopt;
    }
    std::vector<ProtoField> graph;
    if (!protoFields(data, graph_field->begin, graph_field->end, graph)) {
        error = malformed;
        return std::nullopt;
    }

    std::set<std::string> initializers;
    for (const ProtoField& field : graph) {
        std::string name;
        if (field.number != kGraphInitializer || field.wire != 2U) continue;
        if (!stringField(data, field, kTensorName, name)) {
            error = malformed;
            return std::nullopt;
        }
        initializers.insert(name);
    }

    std::optional<int> element_type;
    int image_inputs = 0;
    for (const ProtoField& field : graph) {
        std::string name;
        if (field.number != kGraphInput || field.wire != 2U) continue;
        if (!stringField(data, field, kValueInfoName, name)) {
            error = malformed;
            return std::nullopt;
        }
        if (initializers.count(name) != 0U) continue;
        ++image_inputs;

        // ValueInfoProto.type -> TypeProto.tensor_type -> TypeProto.Tensor.elem_type
        std::vector<ProtoField> value_info;
        std::vector<ProtoField> type_proto;
        std::vector<ProtoField> tensor_type;
        if (!protoFields(data, field.begin, field.end, value_info)) {
            error = malformed;
            return std::nullopt;
        }
        const ProtoField* type = lastMessage(value_info, kValueInfoType);
        if (type != nullptr && !protoFields(data, type->begin, type->end, type_proto)) {
            error = malformed;
            return std::nullopt;
        }
        const ProtoField* tensor = lastMessage(type_proto, kTypeTensor);
        if (tensor != nullptr && !protoFields(data, tensor->begin, tensor->end, tensor_type)) {
            error = malformed;
            return std::nullopt;
        }
        for (const ProtoField& item : tensor_type) {
            if (item.number == kTensorElemType && item.wire == 0U) {
                element_type = static_cast<int>(item.varint);
            }
        }
    }
    if (image_inputs != 1) {
        error = "expected one image input in the ONNX graph, found " + std::to_string(image_inputs);
        return std::nullopt;
    }
    if (!element_type) {
        error = "the ONNX image input declares no tensor element type";
    }
    return element_type;
}

std::optional<int> onnxInputElementType(const std::string& model_path, std::string& error) {
    std::ifstream input(model_path, std::ios::binary);
    if (!input) {
        error = "cannot read " + model_path;
        return std::nullopt;
    }
    const std::string data((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
    return onnxInputElementTypeOf(data, error);
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
