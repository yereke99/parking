#pragma once

#include <memory>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include "anpr/common/config.hpp"
#include "anpr/common/metrics.hpp"
#include "anpr/inference/inference_session.hpp"
#include "anpr/ocr/plate_ocr.hpp"

namespace anpr {

/// Nomeroff Net 4.0.1's Kazakhstan text reader, run in-process by the inference backend
/// (TensorRT on the Jetson Nano) instead of a PyTorch worker.
///
/// tools/export_nomeroff_onnx.py rebuilds the network with the image's PyTorch 1.10 and exports
/// it, because Nomeroff itself needs Python >= 3.9 and PyTorch >= 1.12, which JetPack 4 lacks.
/// Preprocessing and decoding reproduce the `nomeroff` worker for one crop: the worker passes
/// BGR crops and Nomeroff swaps their channels before normalize_img, so the network sees RGB,
/// resized to 200x50 (bilinear) and min-max scaled to 0..1 over the whole crop, in CHW order.
/// Greedy CTC decoding gives the text and per-character confidences.
class NomeroffOnnx final : public IPlateOcr {
public:
    static constexpr int kInputWidth = 200;
    static constexpr int kInputHeight = 50;

    NomeroffOnnx(OcrConfig config, std::unique_ptr<IInferenceSession> session,
                 PipelineMetrics* metrics);

    OcrResult recognize(const cv::Mat& plate) override;
    [[nodiscard]] std::string backendName() const override;
    [[nodiscard]] std::string modelDescription() const override;

private:
    OcrConfig config_;
    std::unique_ptr<IInferenceSession> session_;
    PipelineMetrics* metrics_;
    int steps_{0};
    int classes_{0};
    cv::Mat rgb_;
    cv::Mat resized_;
    cv::Mat scaled_;
    std::vector<cv::Mat> planes_;  ///< three 50 x 200 float headers over the CHW input tensor
};

/// Builds the backend from `ocr.nomeroff_onnx_model`, or returns nullptr with `error` set.
std::unique_ptr<IPlateOcr> makeNomeroffOnnx(const OcrConfig& ocr, const InferenceConfig& inference,
                                            PipelineMetrics* metrics, std::string& error);

}  // namespace anpr
