#include "anpr/detection/plate_detector.hpp"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <stdexcept>

#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>

#include "anpr/common/logging.hpp"

namespace anpr {

PlateDetector::PlateDetector(DetectorConfig config, std::unique_ptr<IInferenceSession> session,
                             PipelineMetrics* metrics)
    : config_(std::move(config)), session_(std::move(session)), metrics_(metrics) {
    if (session_->inputs().empty() || session_->outputs().empty()) {
        throw std::runtime_error("detector model exposes no input or output tensor");
    }

    const TensorSpec& input = session_->inputs().front();
    if (input.shape.size() != 4 || input.shape[1] != 3) {
        throw std::runtime_error(
            "detector model input must be NCHW with three channels; got a different signature");
    }
    if (input.type != TensorType::kFloat32) {
        throw std::runtime_error("detector model input must be float32");
    }
    if (input.shape[2] != input.shape[3]) {
        throw std::runtime_error("detector model input must be square");
    }
    input_size_ = static_cast<int>(input.shape[2]);
    if (input_size_ != config_.input_size) {
        logEvent(LogLevel::kWarn, "detector_input_size_overridden",
                 LogFields()
                     .add("configured", config_.input_size)
                     .add("model", input_size_));
        config_.input_size = input_size_;
    }

    letterboxed_.create(input_size_, input_size_, CV_8UC3);
    rgb_.create(input_size_, input_size_, CV_8UC3);
    float_image_.create(input_size_, input_size_, CV_32FC3);

    // Three single-channel views straight onto the session's input tensor, so cv::split writes
    // the planar NCHW layout with no intermediate buffer and no copy afterwards.
    auto* tensor = static_cast<float*>(session_->inputBuffer(0));
    const std::size_t plane = static_cast<std::size_t>(input_size_) * input_size_;
    channel_views_.reserve(3);
    for (int channel = 0; channel < 3; ++channel) {
        channel_views_.emplace_back(input_size_, input_size_, CV_32F,
                                    tensor + static_cast<std::size_t>(channel) * plane);
    }

    detections_.reserve(16);
    boxes_.reserve(64);
    scores_.reserve(64);
    keep_.reserve(16);
}

void PlateDetector::preprocess(const cv::Mat& frame) {
    // Letterbox: scale to fit, centre, pad. Matches the Ultralytics export convention, so the
    // box decoding below is the exact inverse.
    scale_ = std::min(static_cast<double>(input_size_) / frame.cols,
                      static_cast<double>(input_size_) / frame.rows);
    const int resized_w = std::max(1, static_cast<int>(std::round(frame.cols * scale_)));
    const int resized_h = std::max(1, static_cast<int>(std::round(frame.rows * scale_)));
    pad_x_ = (input_size_ - resized_w) / 2;
    pad_y_ = (input_size_ - resized_h) / 2;

    letterboxed_.setTo(cv::Scalar::all(config_.letterbox_pad));
    cv::resize(frame, resized_, cv::Size(resized_w, resized_h), 0.0, 0.0, cv::INTER_LINEAR);
    resized_.copyTo(letterboxed_(cv::Rect(pad_x_, pad_y_, resized_w, resized_h)));

    if (config_.swap_rb) {
        cv::cvtColor(letterboxed_, rgb_, cv::COLOR_BGR2RGB);
        rgb_.convertTo(float_image_, CV_32FC3, 1.0 / 255.0);
    } else {
        letterboxed_.convertTo(float_image_, CV_32FC3, 1.0 / 255.0);
    }
    cv::split(float_image_, channel_views_);
}

void PlateDetector::decode(int frame_width, int frame_height) {
    const std::vector<std::int64_t>& shape = session_->outputShape(output_index_);
    if (shape.size() != 3) {
        return;
    }
    // Ultralytics emits (1, 4 + classes, anchors).
    const int channels = static_cast<int>(shape[1]);
    const int anchors = static_cast<int>(shape[2]);
    if (channels < 5 || anchors <= 0) {
        return;
    }

    const float* data = session_->outputData(output_index_);
    const float threshold = static_cast<float>(config_.confidence_threshold);
    boxes_.clear();
    scores_.clear();

    for (int anchor = 0; anchor < anchors; ++anchor) {
        float best_score = 0.0F;
        int best_class = 0;
        for (int channel = 4; channel < channels; ++channel) {
            const float score = data[static_cast<std::size_t>(channel) * anchors + anchor];
            if (score > best_score) {
                best_score = score;
                best_class = channel - 4;
            }
        }
        if (best_score < threshold) {
            continue;
        }

        const double cx = data[anchor];
        const double cy = data[static_cast<std::size_t>(anchors) + anchor];
        const double width = data[static_cast<std::size_t>(2) * anchors + anchor];
        const double height = data[static_cast<std::size_t>(3) * anchors + anchor];

        BoundingBox box{
            static_cast<int>(std::lround((cx - width * 0.5 - pad_x_) / scale_)),
            static_cast<int>(std::lround((cy - height * 0.5 - pad_y_) / scale_)),
            static_cast<int>(std::lround(width / scale_)),
            static_cast<int>(std::lround(height / scale_)),
        };
        box = clampBox(box, frame_width, frame_height);
        if (box.empty()) {
            continue;
        }
        boxes_.emplace_back(box.x, box.y, box.width, box.height);
        scores_.push_back(best_score);
        // Single-class detector in practice; the class index is carried through for models that
        // are not.
        (void)best_class;
    }

    keep_.clear();
    if (!boxes_.empty()) {
        cv::dnn::NMSBoxes(boxes_, scores_, threshold, static_cast<float>(config_.nms_threshold),
                          keep_);
    }
    for (const int index : keep_) {
        const cv::Rect& rect = boxes_[static_cast<std::size_t>(index)];
        detections_.push_back(Detection{BoundingBox{rect.x, rect.y, rect.width, rect.height},
                                        scores_[static_cast<std::size_t>(index)], 0});
    }
}

const std::vector<Detection>& PlateDetector::detect(const cv::Mat& frame) {
    detections_.clear();
    if (frame.empty() || frame.channels() != 3) {
        return detections_;
    }

    const auto started = std::chrono::steady_clock::now();
    {
        ScopedTimer timer(metrics_->detector_preprocess);
        preprocess(frame);
    }

    std::string error;
    {
        ScopedTimer timer(metrics_->detector_inference);
        if (!session_->run(error)) {
            logEvent(LogLevel::kWarn, "detector_inference_failed",
                     LogFields().add("reason", error));
            return detections_;
        }
    }

    decode(frame.cols, frame.rows);

    const auto finished = std::chrono::steady_clock::now();
    metrics_->detector_total.add(
        std::chrono::duration<double, std::milli>(finished - started).count());
    ++metrics_->detector_calls;
    metrics_->detections += static_cast<std::int64_t>(detections_.size());
    return detections_;
}

std::unique_ptr<IPlateDetector> makePlateDetector(const DetectorConfig& detector,
                                                  const InferenceConfig& inference,
                                                  PipelineMetrics* metrics, std::string& error) {
    SessionRequest request;
    request.model_path = detector.model;
    request.inference = inference;
    request.tag = "detector";
    request.input_size_hint = detector.input_size;

    std::unique_ptr<IInferenceSession> session = createInferenceSession(request, error);
    if (session == nullptr) {
        return nullptr;
    }
    try {
        return std::make_unique<PlateDetector>(detector, std::move(session), metrics);
    } catch (const std::exception& failure) {
        error = std::string("UNSUPPORTED_MODEL: detector: ") + failure.what();
        return nullptr;
    }
}

class SharedPlateDetectorCore {
public:
    SharedPlateDetectorCore(std::unique_ptr<IPlateDetector> shared_detector,
                            std::shared_ptr<PipelineMetrics> shared_metrics)
        : detector(std::move(shared_detector)), aggregate_metrics(std::move(shared_metrics)) {}

    std::mutex mutex;
    std::unique_ptr<IPlateDetector> detector;
    std::shared_ptr<PipelineMetrics> aggregate_metrics;
};

namespace {

class SharedPlateDetectorClient final : public IPlateDetector {
public:
    SharedPlateDetectorClient(std::shared_ptr<SharedPlateDetectorCore> core,
                              PipelineMetrics* metrics)
        : core_(std::move(core)), metrics_(metrics) {}

    const std::vector<Detection>& detect(const cv::Mat& frame) override {
        const auto started = std::chrono::steady_clock::now();
        {
            const std::lock_guard<std::mutex> guard(core_->mutex);
            detections_ = core_->detector->detect(frame);
        }
        if (metrics_ != nullptr) {
            metrics_->detector_total.add(std::chrono::duration<double, std::milli>(
                                             std::chrono::steady_clock::now() - started)
                                             .count());
            ++metrics_->detector_calls;
            metrics_->detections += static_cast<std::int64_t>(detections_.size());
        }
        return detections_;
    }

    [[nodiscard]] std::string backendName() const override {
        return "shared:" + core_->detector->backendName();
    }

private:
    std::shared_ptr<SharedPlateDetectorCore> core_;
    PipelineMetrics* metrics_;
    std::vector<Detection> detections_;
};

}  // namespace

std::shared_ptr<SharedPlateDetectorCore> makeSharedPlateDetector(
    const DetectorConfig& detector, const InferenceConfig& inference, std::string& error) {
    auto aggregate_metrics = std::make_shared<PipelineMetrics>();
    auto implementation = makePlateDetector(detector, inference, aggregate_metrics.get(), error);
    if (implementation == nullptr) {
        return nullptr;
    }
    return std::make_shared<SharedPlateDetectorCore>(std::move(implementation),
                                                      std::move(aggregate_metrics));
}

std::unique_ptr<IPlateDetector> makeSharedPlateDetectorClient(
    const std::shared_ptr<SharedPlateDetectorCore>& core, PipelineMetrics* metrics) {
    if (core == nullptr) {
        return nullptr;
    }
    return std::make_unique<SharedPlateDetectorClient>(core, metrics);
}

}  // namespace anpr
