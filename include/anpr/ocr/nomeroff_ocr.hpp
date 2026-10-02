#pragma once

#include <memory>
#include <string>

#include "anpr/common/config.hpp"
#include "anpr/common/metrics.hpp"
#include "anpr/ocr/plate_ocr.hpp"

namespace anpr {

class NomeroffWorker;

/// Native adapter around one persistent Nomeroff Net Python worker. Worker instances are shared
/// by identical configurations, so adding cameras does not duplicate model weights or CUDA RAM.
class NomeroffRecognizer final : public IPlateOcr {
public:
    NomeroffRecognizer(OcrConfig config, std::shared_ptr<NomeroffWorker> worker,
                       PipelineMetrics* metrics);

    OcrResult recognize(const cv::Mat& plate) override;
    [[nodiscard]] std::string backendName() const override;
    [[nodiscard]] std::string modelDescription() const override;

private:
    OcrConfig config_;
    std::shared_ptr<NomeroffWorker> worker_;
    PipelineMetrics* metrics_;
};

std::unique_ptr<IPlateOcr> makeNomeroffRecognizer(const OcrConfig& config,
                                                   PipelineMetrics* metrics,
                                                   std::string& error);

}  // namespace anpr
