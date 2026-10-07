#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/core.hpp>

#include "anpr/camera/camera_source.hpp"
#include "anpr/camera/frame_pump.hpp"
#include "anpr/common/config.hpp"
#include "anpr/common/logging.hpp"
#include "anpr/inference/inference_session.hpp"
#include "anpr/detection/plate_detector.hpp"
#include "anpr/pipeline/anpr_pipeline.hpp"
#include "anpr/pipeline/plate_sink.hpp"

namespace {

std::atomic_bool g_stop_requested{false};

void handleSignal(int) {
    g_stop_requested.store(true);
}

void usage() {
    std::cout <<
        R"(Usage: kz_anpr [options]

  --config PATH      configuration file (default config/default.yaml)
  --source S         camera source override: file path, device index, rtsp:// URL,
                     or a GStreamer pipeline. Repeat for multiple streams.
  --camera-id ID     identifier written into every event
  --backend B        auto | tensorrt | onnx_cuda | onnx_cpu | opencv_dnn
  --log-level L      error | warn | info | debug
  --timeline         one debug line per processed frame
  --visualize        open a debug window (needs an OpenCV build with highgui)
  --print-backends   list the inference providers available here, then exit
  --warmup           load both models, run one inference each, then exit. Use this after
                     deployment to build and cache the TensorRT engines. Every run warms up
                     the same way before opening the camera.
  --help

Exit codes: 0 success, 1 unexpected error, 2 configuration error, 3 camera unavailable,
4 model or backend unavailable.
)";
}

struct Cli {
    std::string config_path{"config/default.yaml"};
    std::vector<std::string> sources;
    std::string camera_id;
    std::string backend;
    std::string log_level;
    bool timeline{false};
    bool visualize{false};
    bool print_backends{false};
    bool warmup{false};
    bool help{false};
};

bool parseArgs(int argc, char** argv, Cli& cli, std::string& error) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&](std::string& target) {
            if (i + 1 >= argc) {
                error = "missing value for " + arg;
                return false;
            }
            target = argv[++i];
            return true;
        };

        if (arg == "--help" || arg == "-h") {
            cli.help = true;
            return true;
        }
        if (arg == "--timeline") {
            cli.timeline = true;
        } else if (arg == "--visualize") {
            cli.visualize = true;
        } else if (arg == "--print-backends") {
            cli.print_backends = true;
        } else if (arg == "--warmup") {
            cli.warmup = true;
        } else if (arg == "--config") {
            if (!value(cli.config_path)) return false;
        } else if (arg == "--source" || arg == "--video" || arg == "--rtsp" || arg == "--camera") {
            std::string source;
            if (!value(source)) return false;
            cli.sources.push_back(std::move(source));
        } else if (arg == "--camera-id") {
            if (!value(cli.camera_id)) return false;
        } else if (arg == "--backend") {
            if (!value(cli.backend)) return false;
        } else if (arg == "--log-level") {
            if (!value(cli.log_level)) return false;
        } else {
            error = "unknown argument " + arg;
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    Cli cli;
    std::string error;
    if (!parseArgs(argc, argv, cli, error)) {
        std::cerr << error << "\n\n";
        usage();
        return 2;
    }
    if (cli.help) {
        usage();
        return 0;
    }
    if (cli.print_backends) {
        std::cout << "native_tensorrt_linked=" << (anpr::tensorRTAvailable() ? "yes" : "no")
                  << '\n';
        if (anpr::tensorRTAvailable()) {
            std::cout << "provider=NativeTensorRTExecutionProvider\n";
        }
        std::cout << "onnxruntime_linked=" << (anpr::onnxRuntimeAvailable() ? "yes" : "no") << '\n';
        for (const std::string& provider : anpr::availableProviders()) {
            std::cout << "provider=" << provider << '\n';
        }
        std::cout << "provider=opencv_dnn\n";
        return 0;
    }

    anpr::ConfigLoadResult loaded = anpr::loadConfigFile(cli.config_path);
    if (!loaded.ok) {
        std::cerr << loaded.error << '\n';
        return 2;
    }
    anpr::AnprConfig config = loaded.config;

    if (!anpr::applyEnvironmentOverrides(config, error)) {
        std::cerr << error << '\n';
        return 2;
    }

    if (cli.sources.size() == 1) {
        config.camera.source = cli.sources.front();
        config.camera.kind = anpr::CameraKind::kAuto;
    }
    if (!cli.camera_id.empty()) {
        config.camera.camera_id = cli.camera_id;
    }
    if (!cli.backend.empty()) {
        bool ok = false;
        config.inference.backend = anpr::inferenceBackendFromString(cli.backend, ok);
        if (!ok) {
            std::cerr << "INVALID_CONFIG: unknown backend '" << cli.backend << "'\n";
            return 2;
        }
    }
    if (!cli.log_level.empty()) {
        config.logging.level = anpr::logLevelFromString(cli.log_level, config.logging.level);
    }
    config.debug.frame_timeline = config.debug.frame_timeline || cli.timeline;
    config.debug.visualize = config.debug.visualize || cli.visualize;

    anpr::Logger::instance().setLevel(config.logging.level);
    for (const std::string& key : loaded.unknown_keys) {
        anpr::logEvent(anpr::LogLevel::kWarn, "unknown_config_key",
                       anpr::LogFields().add("key", key));
    }

    cv::setNumThreads(std::max(1, config.performance.opencv_threads));
    anpr::logEvent(anpr::LogLevel::kInfo, "startup",
                   anpr::LogFields()
                       .add("version", "1.0.0")
                       .add("config", cli.config_path)
                       .add("camera_id", config.camera.camera_id)
                       .add("source", anpr::maskCredentials(config.camera.source))
                       .add("streams", cli.sources.empty() ? 1 : cli.sources.size())
                       .add("requested_backend", anpr::toString(config.inference.backend))
                       .add("ocr_backend", config.ocr.backend)
                       .add("ocr_device", config.ocr.device)
                       .add("plate_region_mode", config.ocr.region_mode)
                       .add("opencv_threads", config.performance.opencv_threads));

    try {
        if (cli.sources.size() > 1) {
            struct StreamRuntime {
                anpr::JsonStdoutSink sink;
                std::unique_ptr<anpr::AnprPipeline> pipeline;
                std::unique_ptr<anpr::FramePump> pump;
                std::string camera_id;
                bool finished{false};
            };

            auto shared_detector =
                anpr::makeSharedPlateDetector(config.detector, config.inference, error);
            if (shared_detector == nullptr) {
                anpr::logEvent(anpr::LogLevel::kError, "model_load_failed",
                               anpr::LogFields().add("reason", error));
                return 4;
            }

            std::vector<std::unique_ptr<StreamRuntime>> streams;
            streams.reserve(cli.sources.size());
            for (std::size_t index = 0; index < cli.sources.size(); ++index) {
                auto stream = std::make_unique<StreamRuntime>();
                anpr::AnprConfig stream_config = config;
                stream_config.camera.source = cli.sources[index];
                stream_config.camera.kind = anpr::CameraKind::kAuto;
                stream_config.camera.camera_id =
                    (cli.camera_id.empty() ? config.camera.camera_id : cli.camera_id) + "-" +
                    std::to_string(index + 1);
                stream->camera_id = stream_config.camera.camera_id;
                stream->pipeline =
                    std::make_unique<anpr::AnprPipeline>(stream_config, stream->sink);
                stream->pipeline->setDetector(anpr::makeSharedPlateDetectorClient(
                    shared_detector, &stream->pipeline->metrics()));
                if (!stream->pipeline->loadModels(error)) {
                    anpr::logEvent(anpr::LogLevel::kError, "model_load_failed",
                                   anpr::LogFields()
                                       .add("camera_id", stream->camera_id)
                                       .add("reason", error));
                    return 4;
                }
                auto source = anpr::makeCameraSource(stream_config.camera, error);
                if (source == nullptr) {
                    anpr::logEvent(anpr::LogLevel::kError, "camera_unavailable",
                                   anpr::LogFields()
                                       .add("camera_id", stream->camera_id)
                                       .add("reason", error));
                    return 3;
                }
                stream->pump = std::make_unique<anpr::FramePump>(stream_config.camera,
                                                                 std::move(source));
                streams.push_back(std::move(stream));
            }

            // Warm every pipeline before the first frame. TensorRT finishes its lazy setup on the
            // first inference; on a stream that stall would drop every frame behind it.
            for (auto& stream : streams) {
                if (!stream->pipeline->warmup(error)) {
                    anpr::logEvent(anpr::LogLevel::kError, "warmup_failed",
                                   anpr::LogFields()
                                       .add("camera_id", stream->camera_id)
                                       .add("reason", error));
                    return 4;
                }
            }
            if (cli.warmup) {
                return 0;
            }

            for (auto& stream : streams) {
                if (!stream->pump->start()) {
                    return 3;
                }
                anpr::logEvent(anpr::LogLevel::kInfo, "stream_started",
                               anpr::LogFields().add("camera_id", stream->camera_id));
            }

            while (!g_stop_requested.load()) {
                bool active = false;
                bool processed = false;
                for (auto& stream : streams) {
                    if (stream->finished) {
                        continue;
                    }
                    active = true;
                    anpr::Frame frame;
                    if (stream->pump->waitForFrame(frame, 2)) {
                        stream->pipeline->processFrame(frame);
                        processed = true;
                    } else if (stream->pump->finished()) {
                        stream->finished = true;
                        anpr::logEvent(anpr::LogLevel::kInfo, "stream_stopped",
                                       anpr::LogFields().add("camera_id", stream->camera_id));
                    }
                }
                if (!active) {
                    break;
                }
                if (!processed) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }

            std::int64_t total_frames = 0;
            std::int64_t total_confirmed = 0;
            for (auto& stream : streams) {
                stream->pump->stop();
                const anpr::PumpStats stats = stream->pump->stats();
                stream->pipeline->metrics().frames_captured = stats.captured;
                stream->pipeline->metrics().frames_dropped = stats.dropped;
                stream->pipeline->metrics().camera_reconnects = stats.reconnects;
                total_frames += stream->pipeline->metrics().frames_processed;
                total_confirmed += stream->pipeline->metrics().plates_confirmed;
            }
            anpr::logEvent(anpr::LogLevel::kInfo, "shutdown",
                           anpr::LogFields()
                               .add("streams", streams.size())
                               .add("frames_processed", total_frames)
                               .add("plates_confirmed", total_confirmed));
            return 0;
        }

        anpr::JsonStdoutSink sink;
        anpr::AnprPipeline pipeline(config, sink);
        if (!pipeline.loadModels(error)) {
            anpr::logEvent(anpr::LogLevel::kError, "model_load_failed",
                           anpr::LogFields().add("reason", error));
            return 4;
        }

        // Warm up before the first frame; see the multi-stream path above.
        if (!pipeline.warmup(error)) {
            anpr::logEvent(anpr::LogLevel::kError, "warmup_failed",
                           anpr::LogFields().add("reason", error));
            return 4;
        }
        if (cli.warmup) {
            return 0;
        }

        std::unique_ptr<anpr::CameraSource> source = anpr::makeCameraSource(config.camera, error);
        if (source == nullptr) {
            anpr::logEvent(anpr::LogLevel::kError, "camera_unavailable",
                           anpr::LogFields().add("reason", error));
            return 3;
        }

        anpr::FramePump pump(config.camera, std::move(source));
        if (!pump.start()) {
            return 3;
        }

        anpr::logEvent(anpr::LogLevel::kInfo, "pipeline_running",
                       anpr::LogFields().add("backends", pipeline.backendSummary()));
        pipeline.run(pump, g_stop_requested);
        pump.stop();

        anpr::logEvent(anpr::LogLevel::kInfo, "shutdown",
                       anpr::LogFields()
                           .add("frames_processed", pipeline.metrics().frames_processed)
                           .add("plates_confirmed", pipeline.metrics().plates_confirmed));
    } catch (const std::exception& failure) {
        anpr::logEvent(anpr::LogLevel::kError, "fatal",
                       anpr::LogFields().add("reason", failure.what()));
        return 1;
    }
    return 0;
}
