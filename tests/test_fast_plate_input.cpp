#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <opencv2/core.hpp>

#include "anpr/common/metrics.hpp"
#include "anpr/inference/inference_session.hpp"
#include "anpr/ocr/fast_plate_ocr.hpp"
#include "test_framework.hpp"

namespace {

// A few lines of protobuf encoding, enough to build ONNX headers for the input-type probe.
std::string varint(std::uint64_t value) {
    std::string out;
    do {
        unsigned char byte = static_cast<unsigned char>(value & 0x7FU);
        value >>= 7U;
        if (value != 0U) byte |= 0x80U;
        out.push_back(static_cast<char>(byte));
    } while (value != 0U);
    return out;
}

std::string field(std::uint64_t number, const std::string& payload) {
    return varint(number << 3U | 2U) + varint(payload.size()) + payload;
}

std::string intField(std::uint64_t number, std::uint64_t value) {
    return varint(number << 3U) + varint(value);
}

std::string valueInfo(const std::string& name, int elem_type) {
    const std::string shape = field(2, field(1, intField(1, 1)));
    const std::string tensor = intField(1, static_cast<std::uint64_t>(elem_type)) + shape;
    return field(1, name) + field(2, field(1, tensor));
}

std::string onnxModel(const std::vector<std::string>& inputs,
                      const std::vector<std::string>& initializers = {}) {
    std::string graph = field(2, "graph");
    for (const std::string& name : initializers) graph += field(5, field(8, name));
    for (const std::string& input : inputs) graph += field(11, input);
    return intField(1, 8) + field(7, graph) + field(8, intField(2, 15));
}

/// Stands in for ONNX Runtime or TensorRT: records what the OCR writes into its input tensor.
class RecordingSession final : public anpr::IInferenceSession {
public:
    explicit RecordingSession(anpr::TensorType input_type) {
        inputs_.push_back(anpr::TensorSpec{"input", input_type, {1, 64, 128, 3}});
        outputs_.push_back(anpr::TensorSpec{"plate", anpr::TensorType::kFloat32, {1, 10, 37}});
        storage_.assign(inputs_.front().elementCount(), 0.0F);  // float-aligned for both types
        plate_.assign(outputs_.front().elementCount(), 0.0F);
    }

    [[nodiscard]] const std::vector<anpr::TensorSpec>& inputs() const override { return inputs_; }
    [[nodiscard]] const std::vector<anpr::TensorSpec>& outputs() const override {
        return outputs_;
    }
    [[nodiscard]] void* inputBuffer(std::size_t /*index*/) override { return storage_.data(); }
    bool run(std::string& /*error*/) override { return true; }
    [[nodiscard]] const float* outputData(std::size_t /*index*/) const override {
        return plate_.data();
    }
    [[nodiscard]] const std::vector<std::int64_t>& outputShape(std::size_t index) const override {
        return outputs_.at(index).shape;
    }
    [[nodiscard]] std::string backendName() const override { return "recording"; }

    /// Input element `index` as the model would read it.
    [[nodiscard]] float pixel(std::size_t index) const {
        if (inputs_.front().type == anpr::TensorType::kFloat32) return storage_[index];
        return static_cast<float>(reinterpret_cast<const unsigned char*>(storage_.data())[index]);
    }
    [[nodiscard]] std::size_t size() const { return inputs_.front().elementCount(); }

private:
    std::vector<anpr::TensorSpec> inputs_;
    std::vector<anpr::TensorSpec> outputs_;
    std::vector<float> storage_;
    std::vector<float> plate_;
};

anpr::FastPlateOcrModelConfig shippedModel(bool keep_aspect_ratio) {
    anpr::FastPlateOcrModelConfig config;
    config.max_plate_slots = 10;
    config.alphabet = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ_";
    config.pad_char = '_';
    config.img_height = 64;
    config.img_width = 128;
    config.keep_aspect_ratio = keep_aspect_ratio;
    return config;
}

}  // namespace

TEST("the ONNX input probe reads the image input's element type") {
    std::string error;
    CHECK(anpr::onnxInputElementTypeOf(onnxModel({valueInfo("input", anpr::kOnnxUint8)}), error) ==
          anpr::kOnnxUint8);
    CHECK(anpr::onnxInputElementTypeOf(onnxModel({valueInfo("input", anpr::kOnnxFloat)}), error) ==
          anpr::kOnnxFloat);
    // Older exporters list initializers among the inputs; they are weights, not the image.
    const std::string with_weights =
        onnxModel({valueInfo("weights", anpr::kOnnxFloat), valueInfo("input", anpr::kOnnxUint8)},
                  {"weights"});
    CHECK(anpr::onnxInputElementTypeOf(with_weights, error) == anpr::kOnnxUint8);
}

TEST("the ONNX input probe refuses what it cannot read") {
    std::string error;
    const std::string two_inputs =
        onnxModel({valueInfo("a", anpr::kOnnxUint8), valueInfo("b", anpr::kOnnxUint8)});
    CHECK(!anpr::onnxInputElementTypeOf(two_inputs, error).has_value());
    CHECK(!error.empty());
    const std::string model = onnxModel({valueInfo("input", anpr::kOnnxUint8)});
    CHECK(!anpr::onnxInputElementTypeOf(model.substr(0, model.size() - 5), error).has_value());
    CHECK(!anpr::onnxInputElementTypeOf("not an onnx model", error).has_value());
    CHECK(!anpr::onnxInputElementTypeOf(std::string(), error).has_value());
    CHECK(!anpr::onnxInputElementType("/nonexistent/model.onnx", error).has_value());
}

TEST("a float32-input model receives exactly the pixels a uint8 model receives") {
    cv::Mat plate(37, 151, CV_8UC3);
    cv::randu(plate, cv::Scalar::all(0), cv::Scalar::all(256));
    for (const bool keep_aspect_ratio : {false, true}) {
        anpr::PipelineMetrics metrics;
        auto uint8_session = std::make_unique<RecordingSession>(anpr::TensorType::kUInt8);
        auto float_session = std::make_unique<RecordingSession>(anpr::TensorType::kFloat32);
        const RecordingSession& uint8_input = *uint8_session;
        const RecordingSession& float_input = *float_session;
        anpr::FastPlateOcr uint8_ocr(shippedModel(keep_aspect_ratio), anpr::OcrConfig{},
                                     std::move(uint8_session), &metrics);
        anpr::FastPlateOcr float_ocr(shippedModel(keep_aspect_ratio), anpr::OcrConfig{},
                                     std::move(float_session), &metrics);
        (void)uint8_ocr.recognize(plate);
        (void)float_ocr.recognize(plate);

        CHECK_EQ(float_input.size(), uint8_input.size());
        double total = 0.0;
        for (std::size_t index = 0; index < uint8_input.size(); ++index) {
            CHECK_EQ(float_input.pixel(index), uint8_input.pixel(index));
            total += uint8_input.pixel(index);
        }
        CHECK(total > 0.0);  // the crop really was written, not two equal zero buffers
        CHECK(float_ocr.modelDescription().find("float32 input") != std::string::npos);
        CHECK(uint8_ocr.modelDescription().find("uint8 input") != std::string::npos);
    }
}
