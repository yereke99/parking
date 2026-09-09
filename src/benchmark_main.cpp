#include <atomic>
#include <chrono>
#include <csignal>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>

#include <opencv2/core.hpp>

#include "anpr/camera/camera_source.hpp"
#include "anpr/camera/frame_pump.hpp"
#include "anpr/common/config.hpp"
#include "anpr/common/logging.hpp"
#include "anpr/pipeline/anpr_pipeline.hpp"
#include "anpr/pipeline/plate_sink.hpp"

namespace {

std::atomic_bool g_stop_requested{false};

void handleSignal(int) {
    g_stop_requested.store(true);
}

void usage() {
    std::cout <<
        R"(Usage: kz_anpr_benchmark --video PATH [options]

  --video PATH        clip to replay
  --config PATH       configuration file (default config/default.yaml)
  --backend B         auto | tensorrt | onnx_cuda | onnx_cpu | opencv_dnn
  --detector-only     time the detector on every frame, skipping the state machine and OCR
  --max-frames N      stop after N frames
  --repeat N          replay the clip N times, for a longer sample
  --json              emit the report as one JSON object
  --help
)";
}

struct Cli {
    std::string video;
    std::string config_path{"config/default.yaml"};
    std::string backend;
    bool detector_only{false};
    bool json{false};
    int max_frames{0};
    int repeat{1};
};

/// Replays a clip through the detector on every frame. This is the number to compare against the
/// Python prototype, which also ran the detector on every frame.
int runDetectorOnly(const Cli& cli, anpr::AnprConfig config) {
    anpr::PipelineMetrics metrics;
    std::string error;
    auto detector =
        anpr::makePlateDetector(config.detector, config.inference, &metrics, error);
    if (detector == nullptr) {
        std::cerr << error << '\n';
        return 4;
    }

    auto source = anpr::makeCameraSource(config.camera, error);
    if (source == nullptr || !source->open()) {
        std::cerr << "CAMERA_UNAVAILABLE: " << (error.empty() ? cli.video : error) << '\n';
        return 3;
    }

    anpr::Frame frame;
    std::int64_t frames = 0;
    std::int64_t frames_with_detections = 0;
    const auto started = std::chrono::steady_clock::now();

    while (!g_stop_requested.load()) {
        const anpr::ReadStatus status = source->read(frame);
        if (status == anpr::ReadStatus::kEndOfStream || status == anpr::ReadStatus::kFailed) {
            break;
        }
        if (status != anpr::ReadStatus::kOk) {
            continue;
        }
        ++frames;
        const anpr::BoundingBox roi =
            config.roi.detection.toPixels(frame.image.cols, frame.image.rows);
        const cv::Rect rect(roi.x, roi.y, roi.width, roi.height);
        if (rect.empty()) {
            continue;
        }
        if (!detector->detect(frame.image(rect)).empty()) {
            ++frames_with_detections;
        }
        if (cli.max_frames > 0 && frames >= cli.max_frames) {
            break;
        }
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

    std::cout << std::fixed << std::setprecision(3);
    std::cout << "mode=detector_only\n";
    std::cout << "backend=" << detector->backendName() << '\n';
    std::cout << "frames=" << frames << '\n';
    std::cout << "seconds=" << seconds << '\n';
    std::cout << "effective_fps=" << (seconds > 0.0 ? frames / seconds : 0.0) << '\n';
    std::cout << "frames_with_detections=" << frames_with_detections << '\n';
    std::cout << "detections=" << metrics.detections << '\n';
    std::cout << "detector_calls=" << metrics.detector_total.count() << '\n';
    std::cout << "detector_avg_ms=" << metrics.detector_total.avgMs() << '\n';
    std::cout << "detector_p95_ms=" << metrics.detector_total.percentileMs(0.95) << '\n';
    std::cout << "detector_max_ms=" << metrics.detector_total.maxMs() << '\n';
    std::cout << "detector_preprocess_avg_ms=" << metrics.detector_preprocess.avgMs() << '\n';
    std::cout << "detector_inference_avg_ms=" << metrics.detector_inference.avgMs() << '\n';
    return 0;
}

void printReport(const Cli& cli, const anpr::AnprPipeline& pipeline,
                 const anpr::CollectingSink& sink, double seconds) {
    const anpr::PipelineMetrics& m = pipeline.metrics();
    std::cout << std::fixed << std::setprecision(3);

    if (cli.json) {
        std::cout << "{\"mode\":\"pipeline\",\"video\":\"" << cli.video << "\",\"seconds\":"
                  << seconds << ",\"frames\":" << m.frames_processed
                  << ",\"effective_fps\":" << (seconds > 0.0 ? m.frames_processed / seconds : 0.0)
                  << ",\"detector_calls\":" << m.detector_total.count()
                  << ",\"detector_avg_ms\":" << m.detector_total.avgMs()
                  << ",\"ocr_calls\":" << m.ocr_total.count()
                  << ",\"ocr_avg_ms\":" << m.ocr_total.avgMs()
                  << ",\"plates_confirmed\":" << m.plates_confirmed
                  << ",\"recognition_sessions\":" << m.recognition_sessions << ",\"events\":[";
        for (std::size_t i = 0; i < sink.events().size(); ++i) {
            std::cout << (i == 0 ? "" : ",") << anpr::toJson(sink.events()[i]);
        }
        std::cout << "]}\n";
        return;
    }

    std::cout << "mode=pipeline\n";
    std::cout << "video=" << cli.video << '\n';
    std::cout << "backend=" << pipeline.backendSummary() << '\n';
    std::cout << "seconds=" << seconds << '\n';
    std::cout << "frames_captured=" << m.frames_captured << '\n';
    std::cout << "frames_processed=" << m.frames_processed << '\n';
    std::cout << "frames_dropped=" << m.frames_dropped << '\n';
    std::cout << "effective_fps=" << (seconds > 0.0 ? m.frames_processed / seconds : 0.0) << '\n';
    std::cout << "frame_avg_ms=" << m.frame_total.avgMs() << '\n';
    std::cout << "frame_p95_ms=" << m.frame_total.percentileMs(0.95) << '\n';
    std::cout << "motion_calls=" << m.motion_latency.count() << '\n';
    std::cout << "motion_avg_ms=" << m.motion_latency.avgMs() << '\n';
    std::cout << "detector_calls=" << m.detector_total.count() << '\n';
    std::cout << "detector_avg_ms=" << m.detector_total.avgMs() << '\n';
    std::cout << "detector_p95_ms=" << m.detector_total.percentileMs(0.95) << '\n';
    std::cout << "detector_preprocess_avg_ms=" << m.detector_preprocess.avgMs() << '\n';
    std::cout << "detector_inference_avg_ms=" << m.detector_inference.avgMs() << '\n';
    std::cout << "detections=" << m.detections << '\n';
    std::cout << "ocr_calls=" << m.ocr_total.count() << '\n';
    std::cout << "ocr_avg_ms=" << m.ocr_total.avgMs() << '\n';
    std::cout << "ocr_p95_ms=" << m.ocr_total.percentileMs(0.95) << '\n';
    std::cout << "ocr_preprocess_avg_ms=" << m.ocr_preprocess.avgMs() << '\n';
    std::cout << "ocr_inference_avg_ms=" << m.ocr_inference.avgMs() << '\n';
    std::cout << "ocr_rejected=" << m.ocr_empty << '\n';
    std::cout << "crops_rejected_quality=" << m.crops_rejected_quality << '\n';
    std::cout << "crops_rejected_size=" << m.crops_rejected_size << '\n';
    std::cout << "crops_rejected_roi=" << m.crops_rejected_roi << '\n';
    std::cout << "observations_invalid_format=" << m.observations_invalid_format << '\n';
    std::cout << "recognition_sessions=" << m.recognition_sessions << '\n';
    std::cout << "plates_confirmed=" << m.plates_confirmed << '\n';
    std::cout << "recognition_timeouts=" << m.recognition_timeouts << '\n';
    std::cout << "recognition_latency_avg_ms=" << m.recognition_latency.avgMs() << '\n';
    std::cout << "recognition_latency_max_ms=" << m.recognition_latency.maxMs() << '\n';

    for (const anpr::PlateRecognitionEvent& event : sink.events()) {
        std::cout << "event " << anpr::toJson(event) << '\n';
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    Cli cli;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            usage();
            return 0;
        }
        if (arg == "--detector-only") {
            cli.detector_only = true;
            continue;
        }
        if (arg == "--json") {
            cli.json = true;
            continue;
        }
        if (i + 1 >= argc) {
            std::cerr << "missing value for " << arg << '\n';
            return 2;
        }
        const std::string value = argv[++i];
        if (arg == "--video") {
            cli.video = value;
        } else if (arg == "--config") {
            cli.config_path = value;
        } else if (arg == "--backend") {
            cli.backend = value;
        } else if (arg == "--max-frames") {
            cli.max_frames = std::stoi(value);
        } else if (arg == "--repeat") {
            cli.repeat = std::max(1, std::stoi(value));
        } else {
            std::cerr << "unknown argument " << arg << '\n';
            return 2;
        }
    }

    if (cli.video.empty()) {
        usage();
        return 2;
    }

    anpr::ConfigLoadResult loaded = anpr::loadConfigFile(cli.config_path);
    if (!loaded.ok) {
        std::cerr << loaded.error << '\n';
        return 2;
    }
    anpr::AnprConfig config = loaded.config;
    config.camera.source = cli.video;
    config.camera.kind = anpr::CameraKind::kFile;
    config.debug.visualize = false;
    if (!cli.backend.empty()) {
        bool ok = false;
        config.inference.backend = anpr::inferenceBackendFromString(cli.backend, ok);
        if (!ok) {
            std::cerr << "unknown backend '" << cli.backend << "'\n";
            return 2;
        }
    }
    // The benchmark reports its own numbers; the pipeline's periodic metrics line only adds noise.
    config.performance.metrics_interval_ms = 0;
    anpr::Logger::instance().setLevel(config.logging.level);
    cv::setNumThreads(std::max(1, config.performance.opencv_threads));

    if (cli.detector_only) {
        return runDetectorOnly(cli, config);
    }

    try {
        anpr::CollectingSink sink;
        anpr::AnprPipeline pipeline(config, sink);
        std::string error;
        if (!pipeline.loadModels(error)) {
            std::cerr << error << '\n';
            return 4;
        }

        const auto started = std::chrono::steady_clock::now();
        std::int64_t processed = 0;
        // Each pass restarts the clip's timeline at zero, so passes are offset to keep the
        // pipeline's view of time monotonic.
        std::int64_t timeline_offset_ms = 0;
        std::int64_t last_stream_ms = 0;
        for (int pass = 0; pass < cli.repeat && !g_stop_requested.load(); ++pass) {
            auto source = anpr::makeCameraSource(config.camera, error);
            if (source == nullptr || !source->open()) {
                std::cerr << "CAMERA_UNAVAILABLE: " << cli.video << '\n';
                return 3;
            }
            // Frames are pulled directly rather than through the capture thread, so every frame
            // of the clip is processed and the timings are not distorted by frame dropping.
            anpr::Frame frame;
            while (!g_stop_requested.load()) {
                const anpr::ReadStatus status = source->read(frame);
                if (status == anpr::ReadStatus::kEndOfStream ||
                    status == anpr::ReadStatus::kFailed) {
                    break;
                }
                if (status != anpr::ReadStatus::kOk) {
                    continue;
                }
                last_stream_ms = frame.stream_ms;
                frame.stream_ms += timeline_offset_ms;
                pipeline.processFrame(frame);
                ++processed;
                if (cli.max_frames > 0 && processed >= cli.max_frames) {
                    break;
                }
            }
            timeline_offset_ms += last_stream_ms + 1000;
            if (cli.max_frames > 0 && processed >= cli.max_frames) {
                break;
            }
        }
        const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

        pipeline.metrics().frames_captured = processed;
        printReport(cli, pipeline, sink, seconds);
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
    return 0;
}
