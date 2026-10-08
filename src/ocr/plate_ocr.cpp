#include "anpr/ocr/plate_ocr.hpp"

#include <chrono>
#include <mutex>
#include <utility>

#include "anpr/common/config.hpp"
#include "anpr/common/metrics.hpp"
#include "anpr/ocr/nomeroff_onnx.hpp"

namespace anpr {

std::string toString(OcrRejection reason) {
    switch (reason) {
        case OcrRejection::kNone:
            return "none";
        case OcrRejection::kEmptyCrop:
            return "empty_crop";
        case OcrRejection::kInferenceFailed:
            return "inference_failed";
        case OcrRejection::kNoText:
            return "no_text";
        case OcrRejection::kWeakCharacter:
            return "weak_character";
        case OcrRejection::kLowConfidence:
            return "low_confidence";
    }
    return "none";
}

std::unique_ptr<IPlateOcr> makePlateOcr(const OcrConfig& ocr, const InferenceConfig& inference,
                                        PipelineMetrics* metrics, std::string& error) {
    return makeNomeroffOnnx(ocr, inference, metrics, error);
}

class SharedPlateOcrCore {
public:
    SharedPlateOcrCore(std::unique_ptr<IPlateOcr> shared_ocr,
                       std::shared_ptr<PipelineMetrics> shared_metrics)
        : ocr(std::move(shared_ocr)), aggregate_metrics(std::move(shared_metrics)) {}

    std::mutex mutex;
    std::unique_ptr<IPlateOcr> ocr;
    /// Written by the shared reader itself, under `mutex`.
    std::shared_ptr<PipelineMetrics> aggregate_metrics;
};

namespace {

class SharedPlateOcrClient final : public IPlateOcr {
public:
    SharedPlateOcrClient(std::shared_ptr<SharedPlateOcrCore> core, PipelineMetrics* metrics)
        : core_(std::move(core)), metrics_(metrics) {}

    OcrResult recognize(const cv::Mat& plate) override {
        const auto started = std::chrono::steady_clock::now();
        OcrResult result;
        {
            const std::lock_guard<std::mutex> guard(core_->mutex);
            result = core_->ocr->recognize(plate);
        }
        if (metrics_ != nullptr) {
            metrics_->ocr_total.add(std::chrono::duration<double, std::milli>(
                                        std::chrono::steady_clock::now() - started)
                                        .count());
            ++metrics_->ocr_calls;
            if (!result.ok()) {
                ++metrics_->ocr_empty;
            }
        }
        return result;
    }

    [[nodiscard]] std::string backendName() const override {
        return "shared:" + core_->ocr->backendName();
    }

    [[nodiscard]] std::string modelDescription() const override {
        return core_->ocr->modelDescription();
    }

private:
    std::shared_ptr<SharedPlateOcrCore> core_;
    PipelineMetrics* metrics_;
};

}  // namespace

std::shared_ptr<SharedPlateOcrCore> makeSharedPlateOcr(const OcrConfig& ocr,
                                                       const InferenceConfig& inference,
                                                       std::string& error) {
    auto aggregate_metrics = std::make_shared<PipelineMetrics>();
    auto implementation = makePlateOcr(ocr, inference, aggregate_metrics.get(), error);
    if (implementation == nullptr) {
        return nullptr;
    }
    return std::make_shared<SharedPlateOcrCore>(std::move(implementation),
                                                std::move(aggregate_metrics));
}

std::shared_ptr<SharedPlateOcrCore> makeSharedPlateOcr(std::unique_ptr<IPlateOcr> ocr) {
    if (ocr == nullptr) {
        return nullptr;
    }
    return std::make_shared<SharedPlateOcrCore>(std::move(ocr),
                                                std::make_shared<PipelineMetrics>());
}

std::unique_ptr<IPlateOcr> makeSharedPlateOcrClient(const std::shared_ptr<SharedPlateOcrCore>& core,
                                                    PipelineMetrics* metrics) {
    if (core == nullptr) {
        return nullptr;
    }
    return std::make_unique<SharedPlateOcrClient>(core, metrics);
}

}  // namespace anpr
