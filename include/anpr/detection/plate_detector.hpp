#pragma once

#include <memory>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include "anpr/common/config.hpp"
#include "anpr/common/metrics.hpp"
#include "anpr/detection/detection.hpp"
#include "anpr/inference/inference_session.hpp"

namespace anpr {

class IPlateDetector {
public:
    virtual ~IPlateDetector() = default;

    /// Detects plates in `frame`. Boxes are returned in `frame` coordinates; the caller adds the
    /// ROI offset. The returned reference is owned by the detector and stays valid until the next
    /// call, so no vector is allocated per frame.
    virtual const std::vector<Detection>& detect(const cv::Mat& frame) = 0;
    [[nodiscard]] virtual std::string backendName() const = 0;
    /// How urgently the next detect calls need the shared model: higher goes first when several
    /// cameras wait for one detector (see PriorityGate). A detector of its own ignores it.
    virtual void setSchedulingPriority(int priority) { (void)priority; }
};

/// YOLOv8-style single-class plate detector.
///
/// Every buffer the frame loop touches is allocated once in the constructor: the letterbox
/// canvas, the RGB and float staging images, the channel views over the session's input tensor,
/// and the decode scratch vectors. A detect call performs one resize, one colour conversion, one
/// scale-and-split, the session run, and the decode.
class PlateDetector final : public IPlateDetector {
public:
    PlateDetector(DetectorConfig config, std::unique_ptr<IInferenceSession> session,
                  PipelineMetrics* metrics);

    const std::vector<Detection>& detect(const cv::Mat& frame) override;
    [[nodiscard]] std::string backendName() const override { return session_->backendName(); }

private:
    DetectorConfig config_;
    std::unique_ptr<IInferenceSession> session_;
    PipelineMetrics* metrics_;

    int input_size_{640};
    std::size_t output_index_{0};

    // Preallocated staging buffers.
    cv::Mat resized_;
    cv::Mat letterboxed_;
    cv::Mat rgb_;
    cv::Mat float_image_;
    std::vector<cv::Mat> channel_views_;

    // Preallocated decode scratch.
    std::vector<Detection> detections_;
    std::vector<cv::Rect> boxes_;
    std::vector<float> scores_;
    std::vector<int> keep_;

    double scale_{1.0};
    int pad_x_{0};
    int pad_y_{0};

    void preprocess(const cv::Mat& frame);
    void decode(int frame_width, int frame_height);
};

/// Builds a detector, or returns nullptr with `error` set. Never throws at call sites.
std::unique_ptr<IPlateDetector> makePlateDetector(const DetectorConfig& detector,
                                                  const InferenceConfig& inference,
                                                  PipelineMetrics* metrics, std::string& error);

class SharedPlateDetectorCore;

/// Loads one detector model for a group of streams. Clients take turns on the non-thread-safe
/// inference session through a PriorityGate (a camera recognising a plate before idle ones)
/// while retaining per-stream total latency/counters.
std::shared_ptr<SharedPlateDetectorCore> makeSharedPlateDetector(
    const DetectorConfig& detector, const InferenceConfig& inference, std::string& error);
/// Wraps an already built detector (tests, or a caller that loaded it itself).
std::shared_ptr<SharedPlateDetectorCore> makeSharedPlateDetector(
    std::unique_ptr<IPlateDetector> detector);
std::unique_ptr<IPlateDetector> makeSharedPlateDetectorClient(
    const std::shared_ptr<SharedPlateDetectorCore>& core, PipelineMetrics* metrics);

}  // namespace anpr
