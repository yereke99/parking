#pragma once

#include <memory>
#include <string>

#include "anpr/common/config.hpp"
#include "anpr/common/metrics.hpp"
#include "anpr/ocr/plate_ocr.hpp"

namespace anpr {

class ResearchOcrWorker;

/// C++ adapter for the PaddleOCR and EasyOCR research baselines.
///
/// The Python process is persistent and shared by identical camera configurations. This keeps
/// model startup out of per-crop latency and prevents four simulated cameras from loading four
/// copies of the same large framework model.
class ResearchOcrRecognizer final : public IPlateOcr {
public:
    ResearchOcrRecognizer(OcrConfig config, std::shared_ptr<ResearchOcrWorker> worker,
                          PipelineMetrics* metrics);

    OcrResult recognize(const cv::Mat& plate) override;
    [[nodiscard]] std::string backendName() const override;
    [[nodiscard]] std::string modelDescription() const override;

private:
    OcrConfig config_;
    std::shared_ptr<ResearchOcrWorker> worker_;
    PipelineMetrics* metrics_;
};

std::unique_ptr<IPlateOcr> makeResearchOcrRecognizer(const OcrConfig& config,
                                                      PipelineMetrics* metrics,
                                                      std::string& error);

}  // namespace anpr
