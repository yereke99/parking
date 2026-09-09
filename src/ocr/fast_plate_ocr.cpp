#include "anpr/ocr/fast_plate_ocr.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include <opencv2/imgproc.hpp>

#include "anpr/common/logging.hpp"
#include "anpr/common/yaml.hpp"

namespace anpr {
namespace {

/// Names used by the Fast Plate OCR config, mapped to the OpenCV flags its Python code uses.
int interpolationFromName(const std::string& name, bool& ok) {
    ok = true;
    if (name == "nearest") return cv::INTER_NEAREST;
    if (name == "linear") return cv::INTER_LINEAR;
    if (name == "cubic") return cv::INTER_CUBIC;
    if (name == "area") return cv::INTER_AREA;
    if (name == "lanczos4") return cv::INTER_LANCZOS4;
    ok = false;
    return cv::INTER_LINEAR;
}

}  // namespace

std::string toString(OcrRejection reason) {
    switch (reason) {
        case OcrRejection::kNone:
            return "none";
        case OcrRejection::kEmptyCrop:
            return "empty_crop";
        case OcrRejection::kInferenceFailed:
            return "inference_failed";
        case OcrRejection::kAllPadding:
            return "all_padding";
        case OcrRejection::kInteriorPadding:
            return "interior_padding";
        case OcrRejection::kWeakCharacter:
            return "weak_character";
        case OcrRejection::kLowConfidence:
            return "low_confidence";
    }
    return "none";
}

int FastPlateOcrModelConfig::padIndex() const {
    const std::size_t index = alphabet.find(pad_char);
    return index == std::string::npos ? -1 : static_cast<int>(index);
}

FastPlateOcrConfigLoad loadFastPlateOcrConfig(const std::string& path) {
    FastPlateOcrConfigLoad result;
    const yaml::ParseResult parsed = yaml::parseFile(path);
    if (!parsed.ok) {
        result.ok = false;
        result.error = "OCR model config: " + parsed.error;
        return result;
    }

    auto require = [&result](bool condition, const std::string& message) {
        if (!condition && result.ok) {
            result.ok = false;
            result.error = "OCR model config: " + message;
        }
        return condition;
    };

    const auto slots = yaml::asInt(parsed.root.find("max_plate_slots"));
    const auto alphabet = yaml::asString(parsed.root.find("alphabet"));
    const auto pad = yaml::asString(parsed.root.find("pad_char"));
    const auto height = yaml::asInt(parsed.root.find("img_height"));
    const auto width = yaml::asInt(parsed.root.find("img_width"));
    if (!require(slots.has_value(), "max_plate_slots is missing") ||
        !require(alphabet.has_value() && !alphabet->empty(), "alphabet is missing") ||
        !require(pad.has_value() && pad->size() == 1, "pad_char must be a single character") ||
        !require(height.has_value(), "img_height is missing") ||
        !require(width.has_value(), "img_width is missing")) {
        return result;
    }

    FastPlateOcrModelConfig& config = result.config;
    config.max_plate_slots = static_cast<int>(*slots);
    config.alphabet = *alphabet;
    config.pad_char = (*pad)[0];
    config.img_height = static_cast<int>(*height);
    config.img_width = static_cast<int>(*width);

    if (const auto keep = yaml::asBool(parsed.root.find("keep_aspect_ratio"))) {
        config.keep_aspect_ratio = *keep;
    }
    if (const auto interpolation = yaml::asString(parsed.root.find("interpolation"))) {
        bool ok = false;
        config.interpolation = interpolationFromName(*interpolation, ok);
        if (!require(ok, "unknown interpolation method '" + *interpolation + "'")) {
            return result;
        }
    }
    if (const auto color_mode = yaml::asString(parsed.root.find("image_color_mode"))) {
        if (!require(*color_mode == "rgb" || *color_mode == "grayscale",
                     "image_color_mode must be rgb or grayscale")) {
            return result;
        }
        config.grayscale = *color_mode == "grayscale";
    }
    if (const yaml::Node* padding = parsed.root.find("padding_color")) {
        if (padding->isSequence() && padding->sequence().size() == 3) {
            config.padding_color = cv::Scalar(yaml::asDouble(&padding->sequence()[0]).value_or(114),
                                              yaml::asDouble(&padding->sequence()[1]).value_or(114),
                                              yaml::asDouble(&padding->sequence()[2]).value_or(114));
        } else if (const auto single = yaml::asDouble(padding)) {
            config.padding_color = cv::Scalar::all(*single);
        }
    }
    if (const yaml::Node* regions = parsed.root.find("plate_regions");
        regions != nullptr && regions->isSequence()) {
        config.plate_regions.reserve(regions->sequence().size());
        for (const auto& item : regions->sequence()) {
            config.plate_regions.push_back(yaml::asString(&item).value_or(""));
        }
    }

    if (!require(config.padIndex() >= 0, "pad_char must be present in the alphabet") ||
        !require(config.max_plate_slots > 0, "max_plate_slots must be positive") ||
        !require(config.img_height > 0 && config.img_width > 0,
                 "img_height and img_width must be positive")) {
        return result;
    }
    return result;
}

FastPlateOcr::FastPlateOcr(FastPlateOcrModelConfig model, OcrConfig config,
                           std::unique_ptr<IInferenceSession> session, PipelineMetrics* metrics)
    : model_(std::move(model)),
      config_(std::move(config)),
      session_(std::move(session)),
      metrics_(metrics) {
    if (session_->inputs().size() != 1) {
        throw std::runtime_error("Fast Plate OCR model must have exactly one input");
    }
    const TensorSpec& input = session_->inputs().front();
    if (input.type != TensorType::kUInt8) {
        throw std::runtime_error(
            "Fast Plate OCR model input must be uint8; this model normalises pixels internally");
    }
    if (input.shape.size() != 4) {
        throw std::runtime_error("Fast Plate OCR model input must be NHWC with four dimensions");
    }
    // Cross-check the YAML against the model itself, so a config paired with the wrong ONNX file
    // fails at startup rather than producing quiet nonsense.
    if (input.shape[1] != model_.img_height || input.shape[2] != model_.img_width ||
        input.shape[3] != model_.channels()) {
        throw std::runtime_error(
            "Fast Plate OCR config does not match the model: config says " +
            std::to_string(model_.img_height) + "x" + std::to_string(model_.img_width) + "x" +
            std::to_string(model_.channels()) + ", model wants " +
            std::to_string(input.shape[1]) + "x" + std::to_string(input.shape[2]) + "x" +
            std::to_string(input.shape[3]));
    }

    plate_output_ = session_->findOutput("plate");
    if (plate_output_ == static_cast<std::size_t>(-1)) {
        plate_output_ = 0;
    }
    const std::size_t expected =
        static_cast<std::size_t>(model_.max_plate_slots) * model_.alphabet.size();
    const TensorSpec& plate_spec = session_->outputs().at(plate_output_);
    if (plate_spec.elementCount() != expected) {
        throw std::runtime_error(
            "Fast Plate OCR plate head has " + std::to_string(plate_spec.elementCount()) +
            " values but the config implies " + std::to_string(expected) +
            " (max_plate_slots times alphabet size)");
    }

    const std::size_t region_index = session_->findOutput("region");
    if (region_index != static_cast<std::size_t>(-1) && !model_.plate_regions.empty()) {
        if (session_->outputs().at(region_index).elementCount() == model_.plate_regions.size()) {
            region_output_ = region_index;
        } else {
            logEvent(LogLevel::kWarn, "ocr_region_head_ignored",
                     LogFields()
                         .add("model_outputs", session_->outputs().at(region_index).elementCount())
                         .add("config_regions", model_.plate_regions.size()));
        }
    }

    const int type = model_.grayscale ? CV_8UC1 : CV_8UC3;
    // A header directly over the session's input tensor: the resize writes the model input in
    // place, with no staging buffer and no copy.
    model_input_ = cv::Mat(model_.img_height, model_.img_width, type,
                           session_->inputBuffer(0));
    converted_.create(1, 1, type);
    scratch_.create(1, 1, type);
}

std::string FastPlateOcr::modelDescription() const {
    return std::to_string(model_.img_width) + "x" + std::to_string(model_.img_height) +
           (model_.grayscale ? " grayscale" : " rgb") + ", " +
           std::to_string(model_.max_plate_slots) + " slots, alphabet " +
           std::to_string(model_.alphabet.size()) +
           (model_.keep_aspect_ratio ? ", aspect preserved" : ", stretched");
}

void FastPlateOcr::preprocess(const cv::Mat& plate) {
    // OpenCV hands us BGR. The model was trained on the colour mode named in its config.
    if (model_.grayscale) {
        if (plate.channels() == 3) {
            cv::cvtColor(plate, converted_, cv::COLOR_BGR2GRAY);
        } else {
            converted_ = plate;
        }
    } else {
        if (plate.channels() == 3) {
            cv::cvtColor(plate, converted_, cv::COLOR_BGR2RGB);
        } else {
            cv::cvtColor(plate, converted_, cv::COLOR_GRAY2RGB);
        }
    }

    if (!model_.keep_aspect_ratio) {
        cv::resize(converted_, model_input_, cv::Size(model_.img_width, model_.img_height), 0.0,
                   0.0, model_.interpolation);
        return;
    }

    // Letterbox exactly as the reference implementation does, including its rounding.
    const double ratio = std::min(static_cast<double>(model_.img_height) / converted_.rows,
                                  static_cast<double>(model_.img_width) / converted_.cols);
    const int new_w = std::max(1, static_cast<int>(std::lround(converted_.cols * ratio)));
    const int new_h = std::max(1, static_cast<int>(std::lround(converted_.rows * ratio)));
    cv::resize(converted_, scratch_, cv::Size(new_w, new_h), 0.0, 0.0, model_.interpolation);

    model_input_.setTo(model_.grayscale ? cv::Scalar(model_.padding_color[0])
                                        : model_.padding_color);
    const int left = std::max(0, (model_.img_width - new_w) / 2);
    const int top = std::max(0, (model_.img_height - new_h) / 2);
    const int copy_w = std::min(new_w, model_.img_width - left);
    const int copy_h = std::min(new_h, model_.img_height - top);
    scratch_(cv::Rect(0, 0, copy_w, copy_h))
        .copyTo(model_input_(cv::Rect(left, top, copy_w, copy_h)));
}

OcrResult FastPlateOcr::decode(const FastPlateOcrModelConfig& model, const float* plate_head,
                               float min_char_confidence) {
    OcrResult result;
    const int vocabulary = static_cast<int>(model.alphabet.size());
    const int pad_index = model.padIndex();

    std::string text;
    std::vector<float> probabilities;
    text.reserve(static_cast<std::size_t>(model.max_plate_slots));
    probabilities.reserve(static_cast<std::size_t>(model.max_plate_slots));

    for (int slot = 0; slot < model.max_plate_slots; ++slot) {
        const float* row = plate_head + static_cast<std::size_t>(slot) * vocabulary;
        int best = 0;
        float best_probability = row[0];
        for (int index = 1; index < vocabulary; ++index) {
            if (row[index] > best_probability) {
                best_probability = row[index];
                best = index;
            }
        }
        text.push_back(model.alphabet[static_cast<std::size_t>(best)]);
        probabilities.push_back(best_probability);
        (void)pad_index;
    }

    // Trailing padding is normal for plates shorter than the slot count.
    std::size_t length = text.size();
    while (length > 0 && text[length - 1] == model.pad_char) {
        --length;
    }
    text.resize(length);
    probabilities.resize(length);

    if (text.empty()) {
        result.rejection = OcrRejection::kAllPadding;
        return result;
    }
    // Padding between characters means the model could not settle on a layout. Silently deleting
    // it would splice two halves into a plausible-looking plate, so the reading is dropped.
    if (text.find(model.pad_char) != std::string::npos) {
        result.text = text;
        result.rejection = OcrRejection::kInteriorPadding;
        return result;
    }

    const float weakest = *std::min_element(probabilities.begin(), probabilities.end());
    float sum = 0.0F;
    for (const float probability : probabilities) {
        sum += probability;
    }

    result.text = text;
    result.character_confidences = std::move(probabilities);
    result.confidence = sum / static_cast<float>(result.character_confidences.size());
    result.min_char_confidence = weakest;
    if (weakest < min_char_confidence) {
        result.rejection = OcrRejection::kWeakCharacter;
    }
    return result;
}

void FastPlateOcr::decodeRegion(OcrResult& result) const {
    if (region_output_ == static_cast<std::size_t>(-1)) {
        return;
    }
    const float* data = session_->outputData(region_output_);
    const std::size_t count = model_.plate_regions.size();
    std::size_t best = 0;
    for (std::size_t index = 1; index < count; ++index) {
        if (data[index] > data[best]) {
            best = index;
        }
    }
    result.region = model_.plate_regions[best];
    result.region_confidence = data[best];
}

OcrResult FastPlateOcr::recognize(const cv::Mat& plate) {
    OcrResult result;
    if (plate.empty() || plate.rows < 2 || plate.cols < 2) {
        result.rejection = OcrRejection::kEmptyCrop;
        return result;
    }

    const auto started = std::chrono::steady_clock::now();
    {
        ScopedTimer timer(metrics_->ocr_preprocess);
        preprocess(plate);
    }

    std::string error;
    {
        ScopedTimer timer(metrics_->ocr_inference);
        if (!session_->run(error)) {
            logEvent(LogLevel::kWarn, "ocr_inference_failed", LogFields().add("reason", error));
            result.rejection = OcrRejection::kInferenceFailed;
            return result;
        }
    }

    result = decode(model_, session_->outputData(plate_output_),
                    static_cast<float>(config_.min_char_confidence));
    if (result.rejection == OcrRejection::kNone &&
        result.confidence < static_cast<float>(config_.min_confidence)) {
        result.rejection = OcrRejection::kLowConfidence;
    }
    decodeRegion(result);

    const auto finished = std::chrono::steady_clock::now();
    metrics_->ocr_total.add(std::chrono::duration<double, std::milli>(finished - started).count());
    ++metrics_->ocr_calls;
    if (!result.ok()) {
        ++metrics_->ocr_empty;
    }
    return result;
}

std::unique_ptr<IPlateOcr> makeFastPlateOcr(const OcrConfig& ocr, const InferenceConfig& inference,
                                            PipelineMetrics* metrics, std::string& error) {
    const FastPlateOcrConfigLoad model = loadFastPlateOcrConfig(ocr.plate_config);
    if (!model.ok) {
        error = "INVALID_MODEL_CONFIG: " + model.error;
        return nullptr;
    }

    SessionRequest request;
    request.model_path = ocr.model;
    request.inference = inference;
    request.tag = "ocr";
    request.requires_uint8_input = true;

    std::unique_ptr<IInferenceSession> session = createInferenceSession(request, error);
    if (session == nullptr) {
        return nullptr;
    }
    try {
        return std::make_unique<FastPlateOcr>(model.config, ocr, std::move(session), metrics);
    } catch (const std::exception& failure) {
        error = std::string("UNSUPPORTED_MODEL: ocr: ") + failure.what();
        return nullptr;
    }
}

}  // namespace anpr
