#include <atomic>
#include <csignal>
#include <iostream>
#include <memory>
#include <string>

#include <opencv2/core.hpp>

#include "anpr/camera/camera_source.hpp"
#include "anpr/camera/frame_pump.hpp"
#include "anpr/common/config.hpp"
#include "anpr/common/logging.hpp"
#include "anpr/inference/inference_session.hpp"
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
                     or a GStreamer pipeline
  --camera-id ID     identifier written into every event
  --backend B        auto | tensorrt | onnx_cuda | onnx_cpu | opencv_dnn
  --log-level L      error | warn | info | debug
  --timeline         one debug line per processed frame
  --visualize        open a debug window (needs an OpenCV build with highgui)
  --print-backends   list the inference providers available here, then exit
  --warmup           load both models, run one inference each, then exit. Use this after
                     deployment to build and cache the TensorRT engines.
  --help

Exit codes: 0 success, 1 unexpected error, 2 configuration error, 3 camera unavailable,
4 model or backend unavailable.
)";
}

struct Cli {
    std::string config_path{"config/default.yaml"};
    std::string source;
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
            if (!value(cli.source)) return false;
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

    if (!cli.source.empty()) {
        config.camera.source = cli.source;
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
                       .add("requested_backend", anpr::toString(config.inference.backend))
                       .add("opencv_threads", config.performance.opencv_threads));

    try {
        anpr::JsonStdoutSink sink;
        anpr::AnprPipeline pipeline(config, sink);
        if (!pipeline.loadModels(error)) {
            anpr::logEvent(anpr::LogLevel::kError, "model_load_failed",
                           anpr::LogFields().add("reason", error));
            return 4;
        }

        if (cli.warmup) {
            if (!pipeline.warmup(error)) {
                anpr::logEvent(anpr::LogLevel::kError, "warmup_failed",
                               anpr::LogFields().add("reason", error));
                return 4;
            }
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
