#include "anpr/ocr/nomeroff_onnx.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <opencv2/imgproc.hpp>

#include "anpr/common/metrics.hpp"
#include "anpr/ocr/nomeroff_decoding.hpp"
#include "test_framework.hpp"

namespace {

constexpr int kSteps = 13;
constexpr int kClasses = 37;

/// Stands in for TensorRT or ONNX Runtime: keeps the input tensor and returns fixed logits.
class FakeNomeroffSession final : public anpr::IInferenceSession {
public:
    FakeNomeroffSession() {
        inputs_.push_back(anpr::TensorSpec{"image", anpr::TensorType::kFloat32, {1, 3, 50, 200}});
        outputs_.push_back(
            anpr::TensorSpec{"logits", anpr::TensorType::kFloat32, {1, kSteps, kClasses}});
        input_.assign(inputs_.front().elementCount(), -1.0F);
        logits_.assign(outputs_.front().elementCount(), 0.0F);
    }

    [[nodiscard]] const std::vector<anpr::TensorSpec>& inputs() const override { return inputs_; }
    [[nodiscard]] const std::vector<anpr::TensorSpec>& outputs() const override {
        return outputs_;
    }
    [[nodiscard]] void* inputBuffer(std::size_t /*index*/) override { return input_.data(); }
    bool run(std::string& /*error*/) override { return true; }
    [[nodiscard]] const float* outputData(std::size_t /*index*/) const override {
        return logits_.data();
    }
    [[nodiscard]] const std::vector<std::int64_t>& outputShape(std::size_t index) const override {
        return outputs_.at(index).shape;
    }
    [[nodiscard]] std::string backendName() const override { return "fake"; }

    /// Logits that spell `path` ('-' is the blank), one step per character.
    void spell(const std::string& path, float strength) {
        std::fill(logits_.begin(), logits_.end(), 0.0F);
        const std::string letters = anpr::nomeroff::kKzLetters;
        for (std::size_t step = 0; step < path.size(); ++step) {
            const std::size_t cls = path[step] == '-' ? 0 : letters.find(path[step]) + 1;
            logits_[step * kClasses + cls] = strength;
        }
    }

    std::vector<float> input_;

private:
    std::vector<anpr::TensorSpec> inputs_;
    std::vector<anpr::TensorSpec> outputs_;
    std::vector<float> logits_;
};

anpr::OcrConfig kzConfig() {
    anpr::OcrConfig config;
    config.min_confidence = 0.40;
    config.min_char_confidence = 0.20;
    return config;
}

}  // namespace

TEST("nomeroff_onnx fills the CHW tensor exactly as Nomeroff Net preprocesses a BGR crop") {
    cv::Mat crop(37, 141, CV_8UC3);
    cv::randu(crop, cv::Scalar::all(20), cv::Scalar::all(230));
    anpr::PipelineMetrics metrics;
    auto session = std::make_unique<FakeNomeroffSession>();
    FakeNomeroffSession& fake = *session;
    anpr::NomeroffOnnx ocr(kzConfig(), std::move(session), &metrics);
    (void)ocr.recognize(crop);

    // Reference: BGR -> RGB, bilinear resize to 200x50, min-max over all channels to 0..1.
    cv::Mat rgb;
    cv::Mat resized;
    cv::cvtColor(crop, rgb, cv::COLOR_BGR2RGB);
    cv::resize(rgb, resized, cv::Size(200, 50), 0.0, 0.0, cv::INTER_LINEAR);
    double low = 0.0;
    double high = 0.0;
    cv::minMaxLoc(resized.reshape(1), &low, &high);
    for (int channel = 0; channel < 3; ++channel) {
        for (int y = 0; y < 50; ++y) {
            for (int x = 0; x < 200; ++x) {
                const double expected =
                    (resized.at<cv::Vec3b>(y, x)[channel] - low) / (high - low);
                const auto index = static_cast<std::size_t>((channel * 50 + y) * 200 + x);
                const float actual = fake.input_[index];
                CHECK_NEAR(actual, expected, 1e-5);
            }
        }
    }
}

TEST("nomeroff_onnx returns the decoded plate and applies the confidence gates") {
    cv::Mat crop(40, 160, CV_8UC3, cv::Scalar(30, 120, 200));
    anpr::PipelineMetrics metrics;
    auto session = std::make_unique<FakeNomeroffSession>();
    FakeNomeroffSession& fake = *session;
    anpr::NomeroffOnnx ocr(kzConfig(), std::move(session), &metrics);

    fake.spell("1-52JTA0-2", 9.0F);
    anpr::OcrResult result = ocr.recognize(crop);
    CHECK_EQ(result.text, std::string("152JTA02"));
    CHECK(result.ok());
    CHECK_EQ(result.region, std::string("kz"));
    CHECK(result.confidence > 0.99F);

    fake.spell("1-52JTA0-2", 0.5F);  // every character near 1/37: too weak to accept
    result = ocr.recognize(crop);
    CHECK_EQ(result.text, std::string("152JTA02"));
    CHECK(result.rejection == anpr::OcrRejection::kWeakCharacter);

    fake.spell("-------------", 9.0F);
    result = ocr.recognize(crop);
    CHECK(result.rejection == anpr::OcrRejection::kNoText);
    CHECK(ocr.recognize(cv::Mat()).rejection == anpr::OcrRejection::kEmptyCrop);
    CHECK(ocr.backendName() == "nomeroff_fake");
}
