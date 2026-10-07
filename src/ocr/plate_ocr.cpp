#include "anpr/ocr/plate_ocr.hpp"

#include "anpr/common/config.hpp"
#include "anpr/ocr/easyocr_onnx.hpp"
#include "anpr/ocr/fast_plate_ocr.hpp"
#include "anpr/ocr/nomeroff_ocr.hpp"
#include "anpr/ocr/research_ocr.hpp"

namespace anpr {

std::unique_ptr<IPlateOcr> makePlateOcr(const OcrConfig& ocr,
                                        const InferenceConfig& inference,
                                        PipelineMetrics* metrics, std::string& error) {
    if (ocr.backend == "nomeroff") {
        return makeNomeroffRecognizer(ocr, metrics, error);
    }
    if (ocr.backend == "fast_plate_ocr") {
        return makeFastPlateOcr(ocr, inference, metrics, error);
    }
    if (ocr.backend == "easyocr_onnx") {
        return makeEasyOcrOnnx(ocr, inference, metrics, error);
    }
    if (ocr.backend == "paddleocr" || ocr.backend == "easyocr") {
        return makeResearchOcrRecognizer(ocr, metrics, error);
    }
    error = "INVALID_CONFIG: unsupported OCR backend '" + ocr.backend + "'";
    return nullptr;
}

}  // namespace anpr
