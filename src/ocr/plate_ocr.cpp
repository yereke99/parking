#include "anpr/ocr/plate_ocr.hpp"

#include "anpr/common/config.hpp"
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

}  // namespace anpr
