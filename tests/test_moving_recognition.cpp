// Recognition while the vehicle (or the camera) keeps moving: the pipeline reads a plate as soon
// as it is readable, keeps one vote per plate track, reads the sharpest crops and reports open
// sessions when a clip ends. Synthetic frames, a scripted detector and a scripted OCR stand in for
// the models.
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include "anpr/camera/camera_source.hpp"
#include "anpr/common/config.hpp"
#include "anpr/detection/plate_detector.hpp"
#include "anpr/ocr/plate_ocr.hpp"
#include "anpr/pipeline/anpr_pipeline.hpp"
#include "anpr/pipeline/plate_sink.hpp"

#include "test_framework.hpp"

namespace {

constexpr int kWidth = 1280;
constexpr int kHeight = 720;
constexpr int kFrameMs = 40;

/// One plate in one frame: where it is, which text it carries and whether it is sharp.
struct PlateSpec {
    cv::Rect box;
    bool second_plate{false};
    bool sharp{true};
};

/// Paints the plates: a high-contrast checkerboard reads as sharp, the same pattern smeared by a
/// blur as a motion-blurred view (it still passes the quality gate, just with a lower score). The
/// two plates differ in brightness so the scripted OCR can tell them apart.
cv::Mat drawFrame(const std::vector<PlateSpec>& plates) {
    cv::Mat frame(kHeight, kWidth, CV_8UC3, cv::Scalar(110, 110, 110));
    for (const PlateSpec& plate : plates) {
        const int low = plate.second_plate ? 120 : 40;
        const int high = plate.second_plate ? 230 : 210;
        cv::Mat region = frame(plate.box);
        for (int y = 0; y < region.rows; ++y) {
            for (int x = 0; x < region.cols; ++x) {
                const int level = ((x / 4 + y / 4) % 2 == 0) ? low : high;
                region.at<cv::Vec3b>(y, x) = cv::Vec3b(static_cast<uchar>(level),
                                                       static_cast<uchar>(level),
                                                       static_cast<uchar>(level));
            }
        }
        if (!plate.sharp) {
            cv::GaussianBlur(region, region, cv::Size(0, 0), 2.0);
        }
    }
    return frame;
}

/// Returns whatever the test put in `next` for the coming frame.
class ScriptedDetector final : public anpr::IPlateDetector {
public:
    std::vector<anpr::Detection> next;

    const std::vector<anpr::Detection>& detect(const cv::Mat&) override {
        detections_ = next;
        return detections_;
    }
    [[nodiscard]] std::string backendName() const override { return "scripted"; }

private:
    std::vector<anpr::Detection> detections_;
};

/// Reads plate 1 as 152JTA02 and the brighter plate 2 as 808LUV02; records what it was given.
/// `override_text` makes every read return that text instead (a misread or garbage).
class ScriptedOcr final : public anpr::IPlateOcr {
public:
    int calls{0};
    int blurred_calls{0};
    std::string override_text;
    /// When set, reads cycle through these texts (disagreeing misreads).
    std::vector<std::string> cycle;

    anpr::OcrResult recognize(const cv::Mat& plate) override {
        ++calls;
        cv::Mat gray;
        cv::cvtColor(plate, gray, cv::COLOR_BGR2GRAY);
        cv::Scalar mean;
        cv::Scalar stddev;
        cv::meanStdDev(gray, mean, stddev);
        // The sharp checkerboard has a deviation of about 85; the blurred one far less.
        if (stddev[0] < 50.0) {
            ++blurred_calls;
        }
        anpr::OcrResult result;
        result.text = !cycle.empty()          ? cycle[static_cast<std::size_t>(calls) % cycle.size()]
                      : !override_text.empty() ? override_text
                      : mean[0] > 150.0     ? "808LUV02"
                                            : "152JTA02";
        result.confidence = 0.92F;
        result.min_char_confidence = 0.85F;
        result.region = "kz";
        return result;
    }
    [[nodiscard]] std::string backendName() const override { return "scripted"; }
    [[nodiscard]] std::string modelDescription() const override { return "scripted"; }
};

anpr::AnprConfig movingConfig() {
    anpr::AnprConfig config = anpr::loadConfigText("").config;
    // The whole frame is every zone, so only movement and readability matter here.
    const anpr::NormalizedRect everything{0.0, 0.0, 1.0, 1.0};
    config.roi.motion = everything;
    config.roi.detection = everything;
    config.roi.near_barrier = everything;
    config.roi.stop = everything;
    config.roi.recognition = everything;
    // As in the Jetson profile: the detector keeps its idle cadence without waiting for motion
    // (a small synthetic plate barely moves the frame difference).
    config.detector.require_motion_in_idle = false;
    config.quality.min_sharpness = 1.0;
    config.quality.min_score = 0.0;
    config.quality.max_clipping_ratio = 1.0;
    config.ocr.max_attempts = 20;
    config.recognition.timeout_ms = 30000;
    config.debug.save_crops = false;
    return config;
}

struct Harness {
    explicit Harness(const anpr::AnprConfig& config) : pipeline(config, sink) {
        auto detector_owned = std::make_unique<ScriptedDetector>();
        auto ocr_owned = std::make_unique<ScriptedOcr>();
        detector = detector_owned.get();
        ocr = ocr_owned.get();
        pipeline.setDetector(std::move(detector_owned));
        pipeline.setOcr(std::move(ocr_owned));
    }

    void step(const std::vector<PlateSpec>& plates) {
        detector->next.clear();
        for (const PlateSpec& plate : plates) {
            detector->next.push_back(
                anpr::Detection{anpr::BoundingBox{plate.box.x, plate.box.y, plate.box.width,
                                                  plate.box.height},
                                0.9F, 0});
        }
        anpr::Frame frame;
        frame.image = drawFrame(plates);
        frame.stream_ms = time_ms;
        frame.capture_ms = time_ms;
        frame.sequence = ++sequence;
        pipeline.processFrame(frame);
        time_ms += kFrameMs;
    }

    anpr::CollectingSink sink;
    anpr::AnprPipeline pipeline;
    ScriptedDetector* detector{nullptr};
    ScriptedOcr* ocr{nullptr};
    std::int64_t time_ms{0};
    std::int64_t sequence{0};
};

/// A plate crossing the frame at 15 px per 40 ms frame (375 px/s): never stationary.
cv::Rect movingBox(int frame_index, int y = 300) {
    return cv::Rect(80 + 15 * frame_index, y, 180, 44);
}

std::vector<std::string> plates(const anpr::CollectingSink& sink) {
    std::vector<std::string> out;
    for (const auto& event : sink.events()) {
        if (anpr::isAccepted(event.status)) {
            out.push_back(event.normalized_plate);
        }
    }
    return out;
}

}  // namespace

TEST("a plate is read while the vehicle keeps moving, without a stop") {
    Harness harness(movingConfig());
    for (int index = 0; index < 60; ++index) {
        harness.step({PlateSpec{movingBox(index)}});
    }
    CHECK(plates(harness.sink) == std::vector<std::string>{"152JTA02"});
    CHECK(harness.pipeline.state() != anpr::VehicleState::kStopped);
}

TEST("strict mode still waits for a stop and reads nothing from a moving plate") {
    anpr::AnprConfig config = movingConfig();
    config.recognition.require_stop = true;
    Harness harness(config);
    for (int index = 0; index < 60; ++index) {
        harness.step({PlateSpec{movingBox(index)}});
    }
    CHECK(harness.sink.events().empty());
    CHECK_EQ(harness.ocr->calls, 0);
}

TEST("a confirmed plate costs no further OCR calls while it stays in view") {
    Harness harness(movingConfig());
    for (int index = 0; index < 60; ++index) {
        harness.step({PlateSpec{movingBox(index)}});
    }
    // The vote needs three agreeing readings; after them the track is done.
    CHECK_EQ(harness.ocr->calls, 3);
    CHECK_EQ(harness.sink.events().size(), std::size_t{1});
}

TEST("two plates in view are both read, each with its own vote") {
    Harness harness(movingConfig());
    for (int index = 0; index < 60; ++index) {
        harness.step({PlateSpec{movingBox(index, 200)}, PlateSpec{movingBox(index, 500), true}});
    }
    std::vector<std::string> read = plates(harness.sink);
    std::sort(read.begin(), read.end());
    CHECK(read == (std::vector<std::string>{"152JTA02", "808LUV02"}));
}

TEST("a session still open when the clip ends reports what it read") {
    anpr::AnprConfig config = movingConfig();
    config.consensus.min_samples = 10;
    config.consensus.required_votes = 10;
    Harness harness(config);
    for (int index = 0; index < 12; ++index) {
        harness.step({PlateSpec{movingBox(index)}});
    }
    CHECK(harness.sink.events().empty());
    CHECK(harness.ocr->calls > 0);
    harness.pipeline.finishPendingRecognition();
    CHECK_EQ(harness.sink.events().size(), std::size_t{1});
    CHECK_EQ(harness.sink.events().front().normalized_plate, std::string("152JTA02"));
    CHECK(!anpr::isAccepted(harness.sink.events().front().status));
}

TEST("best-frame selection reads the sharp views and skips the blurred ones") {
    anpr::AnprConfig config = movingConfig();
    config.ocr.min_interval_ms = 200;
    config.consensus.min_samples = 50;  // keep the session open for the whole run
    config.consensus.required_votes = 50;
    config.ocr.max_attempts = 100;
    Harness harness(config);
    // Mostly motion-blurred views with an occasional sharp one, as from a moving camera.
    for (int index = 0; index < 100; ++index) {
        harness.step({PlateSpec{movingBox(index % 40), false, index % 4 == 1}});
    }
    // 4 s of video at one read per 200 ms window: about 20 calls instead of one per detector
    // tick. Three in four views are blurred, yet the reads go to the sharp ones; only a window
    // that saw no sharp view at all (the slower detector cadence before recognition starts) falls
    // back to the best blurred one.
    CHECK(harness.ocr->calls >= 15);
    CHECK(harness.ocr->calls <= 21);
    CHECK(harness.ocr->blurred_calls * 10 <= harness.ocr->calls);
}

TEST("an unreadable plate stops costing OCR calls after max_unreadable_reads") {
    anpr::AnprConfig config = movingConfig();
    config.ocr.max_attempts = 60;
    config.recognition.max_unreadable_reads = 6;
    Harness harness(config);
    harness.ocr->override_text = "XQ7";  // never a Kazakhstan plate
    for (int index = 0; index < 60; ++index) {
        harness.step({PlateSpec{movingBox(index)}});
    }
    CHECK_EQ(harness.ocr->calls, 6);
    CHECK_EQ(harness.sink.events().size(), std::size_t{1});
    CHECK(!anpr::isAccepted(harness.sink.events().front().status));
}

TEST("the same vehicle misread under a new track is not reported next to its plate") {
    anpr::AnprConfig config = movingConfig();
    config.ocr.max_attempts = 4;
    config.recognition.cooldown_ms = 500;
    Harness harness(config);
    for (int index = 0; index < 20; ++index) {
        harness.step({PlateSpec{movingBox(index)}});
    }
    CHECK(plates(harness.sink) == std::vector<std::string>{"152JTA02"});
    // The camera loses the plate long enough for the tracker to drop it...
    for (int index = 0; index < 50; ++index) {
        harness.step({});
    }
    // ...and finds it again as a new track, misread one character off, never three alike.
    harness.ocr->cycle = {"052JTA02", "152JTA05", "052JTA02", "152ITA02"};
    for (int index = 0; index < 20; ++index) {
        harness.step({PlateSpec{movingBox(index)}});
    }
    CHECK(harness.ocr->calls > 3);
    CHECK_EQ(harness.sink.events().size(), std::size_t{1});

    // A different plate that is accepted is always reported, however similar.
    harness.ocr->cycle.clear();
    harness.ocr->override_text = "153JTA02";
    for (int index = 0; index < 50; ++index) {
        harness.step({});
    }
    for (int index = 0; index < 20; ++index) {
        harness.step({PlateSpec{movingBox(index)}});
    }
    CHECK((plates(harness.sink) == std::vector<std::string>{"152JTA02", "153JTA02"}));
}
