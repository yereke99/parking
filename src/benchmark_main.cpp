#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include "anpr/camera/camera_source.hpp"
#include "anpr/camera/frame_pump.hpp"
#include "anpr/common/config.hpp"
#include "anpr/common/filesystem.hpp"
#include "anpr/common/logging.hpp"
#include "anpr/common/metrics.hpp"
#include "anpr/detection/plate_detector.hpp"
#include "anpr/ocr/plate_ocr.hpp"
#include "anpr/pipeline/anpr_pipeline.hpp"
#include "anpr/pipeline/plate_sink.hpp"
#include "anpr/pipeline/quality_assessor.hpp"

namespace {

std::atomic_bool g_stop_requested{false};

void handleSignal(int) {
    g_stop_requested.store(true);
}

void usage() {
    std::cout <<
        R"(Usage: kz_anpr_benchmark --video PATH [options]

  --video PATH        clip to replay
  --source SOURCE     independent file/camera/RTSP source; repeat up to four times
  --streams N         simulate N cameras by repeating --video (supported: 1, 2, 4)
  --config PATH       configuration file (default config/default.yaml)
  --backend B         auto | tensorrt | onnx_cuda | onnx_cpu | opencv_dnn
  --detector-only     time the detector on every frame, skipping the state machine and OCR
  --extract-crops DIR run the detector on every frame and save each crop that passes the
                      pipeline's ROI, size and quality gates (as OCR would receive it) to
                      DIR/<video>/, appending DIR/index.csv
  --ocr-crops INDEX   load only the OCR model, warm it up, then read every crop listed in an
                      --extract-crops index and report each reading and its latency
  --max-frames N      stop after N frames
  --repeat N          replay the clip N times, for a longer sample
  --warmup-frames N   model warm-up calls before measurement (default 3)
  --save-crops DIR    save bounded OCR crops for accuracy-failure inspection
  --json              emit the report as one JSON object
  --help
)";
}

struct Cli {
    std::string video;
    std::string config_path{"config/default.yaml"};
    std::string backend;
    std::vector<std::string> sources;
    int streams{0};
    int warmup_frames{3};
    std::string crop_directory;
    bool detector_only{false};
    std::string extract_crops;
    std::string ocr_crops;
    bool json{false};
    int max_frames{0};
    int repeat{1};
};

std::string jsonEscape(const std::string& value) {
    std::ostringstream out;
    for (const unsigned char ch : value) {
        switch (ch) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (ch < 0x20) {
                    out << "\\u00" << std::hex << std::setw(2) << std::setfill('0')
                        << static_cast<int>(ch) << std::dec << std::setfill(' ');
                } else {
                    out << ch;
                }
        }
    }
    return out.str();
}

/// Replays a clip through the detector on every frame: the detector's raw throughput, without the
/// state machine that normally skips most frames.
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

double elapsedMs(std::chrono::steady_clock::time_point started) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
        .count();
}

void printLatencyJson(const char* name, const anpr::LatencyStat& stat);

std::vector<std::string> splitCsv(const std::string& line) {
    std::vector<std::string> fields;
    std::stringstream stream(line);
    std::string field;
    while (std::getline(stream, field, ',')) fields.push_back(field);
    return fields;
}

/// Saves every plate crop the pipeline would hand to OCR: the detector runs on every frame inside
/// the detection zone, then each detection goes through the pipeline's size and quality gates and
/// its optional enhancement. --ocr-crops later reads exactly this set with the OCR model.
int runExtractCrops(const Cli& cli, anpr::AnprConfig config) {
    config.camera.realtime_file = false;
    anpr::PipelineMetrics metrics;
    std::string error;
    const auto load_started = std::chrono::steady_clock::now();
    auto detector = anpr::makePlateDetector(config.detector, config.inference, &metrics, error);
    if (detector == nullptr) {
        std::cerr << error << '\n';
        return 4;
    }
    const cv::Mat blank(std::max(64, config.camera.height), std::max(64, config.camera.width),
                        CV_8UC3, cv::Scalar(114, 114, 114));
    for (int index = 0; index < std::max(1, cli.warmup_frames); ++index) detector->detect(blank);
    const double startup_ms = elapsedMs(load_started);
    metrics = anpr::PipelineMetrics{};

    auto source = anpr::makeCameraSource(config.camera, error);
    if (source == nullptr || !source->open()) {
        std::cerr << "CAMERA_UNAVAILABLE: " << (error.empty() ? config.camera.source : error)
                  << '\n';
        return 3;
    }
    const anpr::filesystem::path root(cli.extract_crops);
    const std::string stem = anpr::filesystem::path(config.camera.source).stem().string();
    std::error_code ignored;
    anpr::filesystem::create_directories(root / stem, ignored);
    const anpr::filesystem::path index_path = root / "index.csv";
    const bool new_index = !anpr::filesystem::exists(index_path, ignored);
    std::ofstream index(index_path.string(), std::ios::app);
    if (!index) {
        std::cerr << "cannot write " << index_path.string() << '\n';
        return 2;
    }
    if (new_index) {
        index << "file,video,frame,stream_ms,x,y,width,height,detector_confidence,quality_score\n";
    }

    anpr::QualityAssessor assessor(config.quality);
    cv::Mat enhanced;
    anpr::Frame frame;
    std::int64_t frames = 0;
    std::int64_t detections = 0;
    std::int64_t saved = 0;
    std::int64_t rejected_size = 0;
    std::int64_t rejected_quality = 0;
    const auto started = std::chrono::steady_clock::now();
    while (!g_stop_requested.load()) {
        const anpr::ReadStatus status = source->read(frame);
        if (status == anpr::ReadStatus::kEndOfStream || status == anpr::ReadStatus::kFailed) {
            break;
        }
        if (status != anpr::ReadStatus::kOk) continue;
        ++frames;
        const anpr::BoundingBox roi =
            config.roi.detection.toPixels(frame.image.cols, frame.image.rows);
        const cv::Rect roi_rect(roi.x, roi.y, roi.width, roi.height);
        if (!roi_rect.empty()) {
            int number = 0;
            for (const anpr::Detection& detection : detector->detect(frame.image(roi_rect))) {
                ++detections;
                anpr::BoundingBox box = detection.box;
                box.x += roi.x;
                box.y += roi.y;
                box = anpr::clampBox(box, frame.image.cols, frame.image.rows);
                if (box.empty()) continue;
                const cv::Mat crop = frame.image(cv::Rect(box.x, box.y, box.width, box.height));
                const anpr::ImageQuality quality = assessor.evaluate(crop, box);
                if (!quality.acceptable()) {
                    ++(quality.rejection == anpr::QualityRejection::kTooSmall ? rejected_size
                                                                              : rejected_quality);
                    continue;
                }
                const cv::Mat& ocr_input = assessor.enhance(crop, quality, enhanced) ? enhanced : crop;
                std::ostringstream name;
                name << stem << '/' << stem << '_' << std::setw(6) << std::setfill('0') << frames
                     << '_' << number++ << ".png";
                if (!cv::imwrite((root / name.str()).string(), ocr_input)) {
                    std::cerr << "cannot write crop " << name.str() << '\n';
                    return 5;
                }
                index << name.str() << ',' << config.camera.source << ',' << frames << ','
                      << frame.stream_ms << ',' << box.x << ',' << box.y << ',' << box.width << ','
                      << box.height << ',' << detection.confidence << ',' << quality.score << '\n';
                ++saved;
            }
        }
        if (cli.max_frames > 0 && frames >= cli.max_frames) break;
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

    std::cout << std::fixed << std::setprecision(3);
    std::cout << "{\"mode\":\"extract_crops\",\"video\":\"" << jsonEscape(config.camera.source)
              << "\",\"detector_backend\":\"" << jsonEscape(detector->backendName())
              << "\",\"startup_ms\":" << startup_ms << ",\"seconds\":" << seconds
              << ",\"frames\":" << frames
              << ",\"detector_fps\":" << (seconds > 0.0 ? frames / seconds : 0.0)
              << ",\"detections\":" << detections << ",\"crops\":" << saved
              << ",\"rejected_size\":" << rejected_size
              << ",\"rejected_quality\":" << rejected_quality << ",\"latency\":{";
    printLatencyJson("detection", metrics.detector_total);
    std::cout << "}}\n";
    return 0;
}

/// Times the OCR model alone on a fixed crop set. Loading and warm-up are reported separately
/// and excluded from the latency figures.
int runOcrCrops(const Cli& cli, const anpr::AnprConfig& config) {
    struct Crop {
        std::string file;
        std::string video;
    };
    const anpr::filesystem::path index_path(cli.ocr_crops);
    std::ifstream index(index_path.string());
    std::string line;
    if (!index || !std::getline(index, line)) {
        std::cerr << "cannot read crop index " << cli.ocr_crops << '\n';
        return 2;
    }
    std::vector<Crop> crops;
    while (std::getline(index, line)) {
        const std::vector<std::string> fields = splitCsv(line);
        if (fields.size() >= 2) crops.push_back({fields[0], fields[1]});
    }

    anpr::PipelineMetrics metrics;
    std::string error;
    const auto load_started = std::chrono::steady_clock::now();
    auto ocr = anpr::makePlateOcr(config.ocr, config.inference, &metrics, error);
    if (ocr == nullptr) {
        std::cerr << error << '\n';
        return 4;
    }
    const double load_ms = elapsedMs(load_started);
    const auto warmup_started = std::chrono::steady_clock::now();
    const cv::Mat blank(64, 192, CV_8UC3, cv::Scalar(114, 114, 114));
    for (int index_number = 0; index_number < std::max(1, cli.warmup_frames); ++index_number) {
        ocr->recognize(blank);
    }
    const double warmup_ms = elapsedMs(warmup_started);

    anpr::LatencyStat latency;
    std::ostringstream results;
    results << std::fixed << std::setprecision(4);
    std::int64_t unreadable = 0;
    std::int64_t accepted = 0;
    std::int64_t empty = 0;
    const auto started = std::chrono::steady_clock::now();
    for (std::size_t number = 0; number < crops.size() && !g_stop_requested.load(); ++number) {
        const cv::Mat image = cv::imread((index_path.parent_path() / crops[number].file).string(),
                                         cv::IMREAD_COLOR);
        if (image.empty()) {
            ++unreadable;
            continue;
        }
        const auto call_started = std::chrono::steady_clock::now();
        const anpr::OcrResult result = ocr->recognize(image);
        const double ms = elapsedMs(call_started);
        latency.add(ms);
        accepted += result.ok() ? 1 : 0;
        empty += result.text.empty() ? 1 : 0;
        results << (number == 0 ? "" : ",") << "{\"file\":\"" << jsonEscape(crops[number].file)
                << "\",\"video\":\"" << jsonEscape(crops[number].video) << "\",\"text\":\""
                << jsonEscape(result.text) << "\",\"confidence\":" << result.confidence
                << ",\"rejection\":\"" << anpr::toString(result.rejection) << "\",\"ms\":" << ms
                << '}';
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

    std::cout << std::fixed << std::setprecision(3);
    std::cout << "{\"mode\":\"ocr_crops\",\"backend\":\"" << jsonEscape(ocr->backendName()) << "\",\"model\":\""
              << jsonEscape(ocr->modelDescription()) << "\",\"load_ms\":" << load_ms
              << ",\"warmup_ms\":" << warmup_ms << ",\"seconds\":" << seconds
              << ",\"crops\":" << latency.count() << ",\"unreadable_files\":" << unreadable
              << ",\"accepted\":" << accepted << ",\"empty\":" << empty
              << ",\"ocr_fps\":" << (latency.sumMs() > 0.0 ? latency.count() * 1000.0 / latency.sumMs() : 0.0)
              << ",\"latency\":{";
    printLatencyJson("ocr", latency);
    std::cout << "},\"results\":[" << results.str() << "]}\n";
    return 0;
}

void printMetricsJson(const anpr::PipelineMetrics& metrics, double seconds);

void printReport(const Cli& cli, const anpr::AnprPipeline& pipeline,
                 const anpr::CollectingSink& sink, double seconds, double load_ms,
                 double warmup_ms) {
    const anpr::PipelineMetrics& m = pipeline.metrics();
    std::cout << std::fixed << std::setprecision(3);

    if (cli.json) {
        std::cout << "{\"mode\":\"pipeline\",\"video\":\"" << jsonEscape(cli.video)
                  << "\",\"backend\":\"" << jsonEscape(pipeline.backendSummary())
                  << "\",\"load_ms\":" << load_ms << ",\"warmup_ms\":" << warmup_ms
                  << ",\"seconds\":"
                  << seconds << ",\"frames\":" << m.frames_processed
                  << ",\"effective_fps\":" << (seconds > 0.0 ? m.frames_processed / seconds : 0.0)
                  << ",\"detector_calls\":" << m.detector_total.count()
                  << ",\"detector_avg_ms\":" << m.detector_total.avgMs()
                  << ",\"ocr_calls\":" << m.ocr_total.count()
                  << ",\"ocr_avg_ms\":" << m.ocr_total.avgMs()
                  << ",\"plates_confirmed\":" << m.plates_confirmed
                  << ",\"recognition_sessions\":" << m.recognition_sessions << ",\"metrics\":";
        printMetricsJson(m, seconds);
        std::cout << ",\"events\":[";
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

void printLatencyJson(const char* name, const anpr::LatencyStat& stat) {
    std::cout << '"' << name << "\":{\"count\":" << stat.count()
              << ",\"avg_ms\":" << stat.avgMs()
              << ",\"p50_ms\":" << stat.percentileMs(0.50)
              << ",\"p95_ms\":" << stat.percentileMs(0.95)
              << ",\"p99_ms\":" << stat.percentileMs(0.99)
              << ",\"max_ms\":" << stat.maxMs() << '}';
}

void printMetricsJson(const anpr::PipelineMetrics& metrics, double seconds) {
    std::cout << "{\"frames_captured\":" << metrics.frames_captured
              << ",\"frames_processed\":" << metrics.frames_processed
              << ",\"frames_dropped\":" << metrics.frames_dropped
              << ",\"input_fps\":"
              << (seconds > 0.0 ? metrics.frames_captured / seconds : 0.0)
              << ",\"processed_fps\":"
              << (seconds > 0.0 ? metrics.frames_processed / seconds : 0.0)
              << ",\"detection_fps\":"
              << (seconds > 0.0 ? metrics.detector_calls / seconds : 0.0)
              << ",\"ocr_throughput\":"
              << (seconds > 0.0 ? metrics.ocr_calls / seconds : 0.0)
              << ",\"detector_calls\":" << metrics.detector_calls
              << ",\"detections\":" << metrics.detections
              << ",\"ocr_calls\":" << metrics.ocr_calls
              << ",\"ocr_rejected\":" << metrics.ocr_empty
              << ",\"recognition_sessions\":" << metrics.recognition_sessions
              << ",\"plates_confirmed\":" << metrics.plates_confirmed
              << ",\"recognition_timeouts\":" << metrics.recognition_timeouts
              << ",\"latency\":{";
    printLatencyJson("decode", metrics.decode_latency);
    std::cout << ',';
    printLatencyJson("capture", metrics.capture_latency);
    std::cout << ',';
    printLatencyJson("detection", metrics.detector_total);
    std::cout << ',';
    printLatencyJson("tracking", metrics.tracking_latency);
    std::cout << ',';
    printLatencyJson("ocr", metrics.ocr_total);
    std::cout << ',';
    printLatencyJson("postprocess", metrics.postprocess_latency);
    std::cout << ',';
    printLatencyJson("total", metrics.frame_total);
    std::cout << "}}";
}

int runMultiStream(const Cli& cli, anpr::AnprConfig config) {
    std::vector<std::string> sources = cli.sources;
    if (sources.empty()) {
        const int count = cli.streams > 0 ? cli.streams : 1;
        sources.assign(static_cast<std::size_t>(count), cli.video);
    } else if (sources.size() == 1 && cli.streams > 1) {
        sources.assign(static_cast<std::size_t>(cli.streams), sources.front());
    }
    if (sources.empty() || (sources.size() != 1 && sources.size() != 2 && sources.size() != 4)) {
        std::cerr << "multi-stream benchmark requires 1, 2 or 4 sources\n";
        return 2;
    }

    struct Stream {
        anpr::CollectingSink sink;
        std::unique_ptr<anpr::AnprPipeline> pipeline;
        std::unique_ptr<anpr::FramePump> pump;
        std::string id;
        bool finished{false};
        int input_width{0};
        int input_height{0};
    };

    std::string error;
    const auto load_started = std::chrono::steady_clock::now();
    auto shared_detector =
        anpr::makeSharedPlateDetector(config.detector, config.inference, error);
    if (shared_detector == nullptr) {
        std::cerr << error << '\n';
        return 4;
    }
    // One OCR engine for every stream, like the detector and like camera mode.
    auto shared_ocr = anpr::makeSharedPlateOcr(config.ocr, config.inference, error);
    if (shared_ocr == nullptr) {
        std::cerr << error << '\n';
        return 4;
    }

    std::vector<std::unique_ptr<Stream>> runtimes;
    runtimes.reserve(sources.size());
    for (std::size_t index = 0; index < sources.size(); ++index) {
        auto runtime = std::make_unique<Stream>();
        anpr::AnprConfig stream_config = config;
        stream_config.camera.source = sources[index];
        stream_config.camera.kind = anpr::CameraKind::kAuto;
        stream_config.camera.camera_id = "benchmark-" + std::to_string(index + 1);
        stream_config.camera.loop_file = false;
        stream_config.camera.realtime_file = true;
        // Simulated cameras: each pump keeps only its newest frame, as a live camera does.
        stream_config.camera.process_every_file_frame = false;
        runtime->id = stream_config.camera.camera_id;
        runtime->pipeline =
            std::make_unique<anpr::AnprPipeline>(stream_config, runtime->sink);
        runtime->pipeline->setDetector(anpr::makeSharedPlateDetectorClient(
            shared_detector, &runtime->pipeline->metrics()));
        auto ocr_client =
            anpr::makeSharedPlateOcrClient(shared_ocr, &runtime->pipeline->metrics());
        if (runtimes.empty()) {
            anpr::logEvent(anpr::LogLevel::kInfo, "shared_ocr_ready",
                           anpr::LogFields()
                               .add("ocr_backend", ocr_client->backendName())
                               .addQuoted("ocr_model", ocr_client->modelDescription())
                               .add("streams", sources.size()));
        }
        runtime->pipeline->setOcr(std::move(ocr_client));
        if (!runtime->pipeline->loadModels(error)) {
            std::cerr << error << '\n';
            return 4;
        }
        auto source = anpr::makeCameraSource(stream_config.camera, error);
        if (source == nullptr) {
            std::cerr << "CAMERA_UNAVAILABLE: " << error << '\n';
            return 3;
        }
        runtime->pump =
            std::make_unique<anpr::FramePump>(stream_config.camera, std::move(source));
        runtimes.push_back(std::move(runtime));
    }
    const double model_load_ms = std::chrono::duration<double, std::milli>(
                                     std::chrono::steady_clock::now() - load_started)
                                     .count();

    const auto warmup_started = std::chrono::steady_clock::now();
    for (auto& runtime : runtimes) {
        for (int index = 0; index < cli.warmup_frames; ++index) {
            if (!runtime->pipeline->warmup(error)) {
                std::cerr << error << '\n';
                return 4;
            }
        }
    }
    const double warmup_ms = std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - warmup_started)
                                 .count();

    for (auto& runtime : runtimes) {
        if (!runtime->pump->start()) {
            return 3;
        }
    }

    const auto started = std::chrono::steady_clock::now();
    std::atomic<std::int64_t> total_processed{0};
    std::vector<std::thread> processing_threads;
    processing_threads.reserve(runtimes.size());
    for (auto& runtime : runtimes) {
        processing_threads.emplace_back([&cli, runtime = runtime.get(), &total_processed] {
            anpr::Frame frame;
            while (!g_stop_requested.load()) {
                if (runtime->pump->waitForFrame(frame, 20)) {
                    runtime->input_width = frame.image.cols;
                    runtime->input_height = frame.image.rows;
                    runtime->pipeline->processFrame(frame);
                    const std::int64_t processed = total_processed.fetch_add(1) + 1;
                    if (cli.max_frames > 0 && processed >= cli.max_frames) {
                        g_stop_requested.store(true);
                        break;
                    }
                } else if (runtime->pump->finished()) {
                    break;
                }
            }
            runtime->finished = true;
        });
    }
    for (std::thread& processing_thread : processing_threads) {
        processing_thread.join();
    }
    const double seconds = std::chrono::duration<double>(
                               std::chrono::steady_clock::now() - started)
                               .count();

    anpr::PipelineMetrics aggregate;
    for (auto& runtime : runtimes) {
        runtime->pump->stop();
        const anpr::PumpStats stats = runtime->pump->stats();
        runtime->pipeline->metrics().frames_captured = stats.captured;
        runtime->pipeline->metrics().frames_dropped = stats.dropped;
        runtime->pipeline->metrics().camera_reconnects = stats.reconnects;
        aggregate.mergeFrom(runtime->pipeline->metrics());
    }

    std::cout << std::fixed << std::setprecision(3);
    if (cli.json) {
        std::cout << "{\"mode\":\"multi_stream\",\"streams\":" << runtimes.size()
                  << ",\"processing_threads\":" << processing_threads.size()
                  << ",\"seconds\":" << seconds << ",\"model_load_ms\":" << model_load_ms
                  << ",\"startup_ms\":" << model_load_ms + warmup_ms
                  << ",\"warmup_ms\":" << warmup_ms << ",\"backend\":\""
                  << jsonEscape(runtimes.front()->pipeline->backendSummary())
                  << "\",\"models\":{\"detector\":\""
                  << jsonEscape(config.detector.model) << "\",\"detector_input_size\":"
                  << config.detector.input_size << ",\"ocr\":\""
                  << jsonEscape(config.ocr.model) << "\"},\"aggregate\":";
        printMetricsJson(aggregate, seconds);
        std::cout << ",\"per_stream\":[";
        for (std::size_t index = 0; index < runtimes.size(); ++index) {
            if (index > 0) std::cout << ',';
            std::cout << "{\"camera_id\":\"" << jsonEscape(runtimes[index]->id)
                      << "\",\"input_width\":" << runtimes[index]->input_width
                      << ",\"input_height\":" << runtimes[index]->input_height
                      << ",\"metrics\":";
            printMetricsJson(runtimes[index]->pipeline->metrics(), seconds);
            std::cout << ",\"events\":[";
            const auto& events = runtimes[index]->sink.events();
            for (std::size_t event_index = 0; event_index < events.size(); ++event_index) {
                if (event_index > 0) std::cout << ',';
                std::cout << anpr::toJson(events[event_index]);
            }
            std::cout << "]}";
        }
        std::cout << "]}\n";
    } else {
        std::cout << "mode=multi_stream\nstreams=" << runtimes.size()
                  << "\nseconds=" << seconds << "\nmodel_load_ms=" << model_load_ms
                  << "\nwarmup_ms=" << warmup_ms
                  << "\naggregate_fps="
                  << (seconds > 0.0 ? aggregate.frames_processed / seconds : 0.0)
                  << "\nfps_per_stream="
                  << (seconds > 0.0 ? aggregate.frames_processed / seconds / runtimes.size() : 0.0)
                  << "\nocr_p50_ms=" << aggregate.ocr_total.percentileMs(0.50)
                  << "\nocr_p95_ms=" << aggregate.ocr_total.percentileMs(0.95)
                  << "\nframes_dropped=" << aggregate.frames_dropped << '\n';
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    Cli cli;
    auto parseInteger = [](const std::string& text, const char* option, int& result) {
        try {
            std::size_t used = 0;
            const int parsed = std::stoi(text, &used);
            if (used != text.size()) {
                throw std::invalid_argument("trailing characters");
            }
            result = parsed;
            return true;
        } catch (const std::exception&) {
            std::cerr << option << " requires an integer, got '" << text << "'\n";
            return false;
        }
    };
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
        } else if (arg == "--source") {
            cli.sources.push_back(value);
        } else if (arg == "--streams") {
            if (!parseInteger(value, "--streams", cli.streams)) return 2;
        } else if (arg == "--warmup-frames") {
            int parsed = 0;
            if (!parseInteger(value, "--warmup-frames", parsed)) return 2;
            cli.warmup_frames = std::max(0, parsed);
        } else if (arg == "--save-crops") {
            cli.crop_directory = value;
        } else if (arg == "--extract-crops") {
            cli.extract_crops = value;
        } else if (arg == "--ocr-crops") {
            cli.ocr_crops = value;
        } else if (arg == "--config") {
            cli.config_path = value;
        } else if (arg == "--backend") {
            cli.backend = value;
        } else if (arg == "--max-frames") {
            if (!parseInteger(value, "--max-frames", cli.max_frames)) return 2;
        } else if (arg == "--repeat") {
            int parsed = 0;
            if (!parseInteger(value, "--repeat", parsed)) return 2;
            cli.repeat = std::max(1, parsed);
        } else {
            std::cerr << "unknown argument " << arg << '\n';
            return 2;
        }
    }

    if (cli.video.empty() && cli.sources.empty() && cli.ocr_crops.empty()) {
        usage();
        return 2;
    }
    if (cli.streams != 0 && cli.streams != 1 && cli.streams != 2 && cli.streams != 4) {
        std::cerr << "--streams must be 1, 2 or 4\n";
        return 2;
    }

    anpr::ConfigLoadResult loaded = anpr::loadConfigFile(cli.config_path);
    if (!loaded.ok) {
        std::cerr << loaded.error << '\n';
        return 2;
    }
    anpr::AnprConfig config = loaded.config;
    if (!cli.video.empty() || !cli.sources.empty()) {
        config.camera.source = cli.video.empty() ? cli.sources.front() : cli.video;
    }
    config.camera.kind = anpr::CameraKind::kAuto;
    config.debug.visualize = false;
    if (!cli.crop_directory.empty()) {
        config.debug.save_crops = true;
        config.debug.output_dir = cli.crop_directory;
    }
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
    if (!cli.extract_crops.empty()) {
        return runExtractCrops(cli, config);
    }
    if (!cli.ocr_crops.empty()) {
        return runOcrCrops(cli, config);
    }
    if (cli.streams > 0 || !cli.sources.empty()) {
        try {
            return runMultiStream(cli, config);
        } catch (const std::exception& exception) {
            std::cerr << "benchmark failed: " << exception.what() << '\n';
            return 5;
        }
    }

    try {
        // Frames are read directly below, so a real-time file would only add sleeps to the
        // timings; every frame is processed either way.
        config.camera.realtime_file = false;
        anpr::CollectingSink sink;
        anpr::AnprPipeline pipeline(config, sink);
        std::string error;
        const auto load_started = std::chrono::steady_clock::now();
        if (!pipeline.loadModels(error)) {
            std::cerr << error << '\n';
            return 4;
        }
        const double load_ms = elapsedMs(load_started);
        const auto warmup_started = std::chrono::steady_clock::now();
        for (int index = 0; index < cli.warmup_frames; ++index) {
            if (!pipeline.warmup(error)) {
                std::cerr << error << '\n';
                return 4;
            }
        }
        const double warmup_ms = elapsedMs(warmup_started);

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
        printReport(cli, pipeline, sink, seconds, load_ms, warmup_ms);
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
    return 0;
}
