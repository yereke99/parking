#include <atomic>
#include <csignal>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include "anpr/camera/camera_source.hpp"
#include "anpr/camera/frame_pump.hpp"
#include "anpr/cameras/camera_commands.hpp"
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
                     or a GStreamer pipeline. Repeat for multiple streams.
  --camera-id ID     identifier written into every event
  --events-file PATH also append every recognition event to PATH, one JSON object per line
  --snapshots-dir PATH save full-frame JPEGs for confirmed plates (single source only)
  --backend B        auto | tensorrt | onnx_cuda | onnx_cpu | opencv_dnn
  --log-level L      error | warn | info | debug
  --timeline         one debug line per processed frame
  --visualize        open a debug window (needs an OpenCV build with highgui)
  --print-backends   list the inference providers available here, then exit
  --warmup           load both models, run one inference each, then exit. Use this after
                     deployment to build and cache the TensorRT engines. Every run warms up
                     the same way before opening the camera.
  --help

Hikvision camera mode (docs/CAMERAS.md):
  --camera-config PATH  camera-mode configuration (default config/cameras.yaml)
  --camera-scan      camera LAN, Internet uplink and every camera found on the PoE switch;
                     sends no password. Exit 0 when a camera answers on the camera subnet
  --camera-check     per camera: RTSP login, stream path, codec, decoder and frames. Exit 0
                     only when every enabled camera is READY
  --camera-status    status table of the running camera mode, or a quick probe when none runs
  --cameras          ANPR on every healthy camera with one shared detector and OCR; events
                     on stdout and in --events-file, reports and logs on stderr
  Camera logins come from HIKVISION_USERNAME / HIKVISION_PASSWORD (or per camera
  HIKVISION_USERNAME_CAMERA_02 / HIKVISION_PASSWORD_CAMERA_02), never from the command line.

Exit codes: 0 success, 1 unexpected error, 2 configuration error, 3 camera unavailable (in
camera mode: no camera LAN, no camera, or a camera that is not ready), 4 model or backend
unavailable.
)";
}

struct Cli {
    std::string config_path{"config/default.yaml"};
    std::vector<std::string> sources;
    std::string camera_id;
    std::string events_file;
    std::string snapshots_dir;
    std::string backend;
    std::string log_level;
    bool timeline{false};
    bool visualize{false};
    bool print_backends{false};
    bool warmup{false};
    bool help{false};
    std::string camera_config{"config/cameras.yaml"};
    bool camera_scan{false};
    bool camera_check{false};
    bool camera_status{false};
    bool cameras{false};

    [[nodiscard]] bool cameraReadOnlyCommand() const {
        return camera_scan || camera_check || camera_status;
    }
    [[nodiscard]] int cameraCommandCount() const {
        return static_cast<int>(camera_scan) + static_cast<int>(camera_check) +
               static_cast<int>(camera_status) + static_cast<int>(cameras);
    }
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
        } else if (arg == "--events-file") {
            if (!value(cli.events_file)) return false;
        } else if (arg == "--snapshots-dir") {
            if (!value(cli.snapshots_dir)) return false;
        } else if (arg == "--backend") {
            if (!value(cli.backend)) return false;
        } else if (arg == "--log-level") {
            if (!value(cli.log_level)) return false;
        } else if (arg == "--camera-config") {
            if (!value(cli.camera_config)) return false;
        } else if (arg == "--camera-scan") {
            cli.camera_scan = true;
        } else if (arg == "--camera-check") {
            cli.camera_check = true;
        } else if (arg == "--camera-status") {
            cli.camera_status = true;
        } else if (arg == "--cameras") {
            cli.cameras = true;
        } else {
            error = "unknown argument " + arg;
            return false;
        }
    }
    if (cli.cameraCommandCount() > 1) {
        error = "--camera-scan, --camera-check, --camera-status and --cameras are mutually "
                "exclusive";
        return false;
    }
    if (cli.cameraCommandCount() == 1 && !cli.sources.empty()) {
        error = "--source cannot be combined with camera mode: camera mode finds its cameras";
        return false;
    }
    if (!cli.snapshots_dir.empty() && (cli.cameraCommandCount() != 0 || cli.sources.size() > 1)) {
        error = "--snapshots-dir requires single-source mode";
        return false;
    }
    return true;
}

anpr::cameras::CameraCommandOptions cameraOptions(const Cli& cli) {
    anpr::cameras::CameraCommandOptions options;
    options.camera_config_path = cli.camera_config;
    return options;
}

/// --camera-scan, --camera-check and --camera-status need no ANPR profile and load no model, so
/// they run next to a camera service without touching the GPU.
int runCameraReadOnlyCommand(const Cli& cli) {
    // Nothing here needs an orderly stop (the registry and status files are replaced
    // atomically), so Ctrl+C ends a long camera check at once.
    std::signal(SIGINT, SIG_DFL);
    std::signal(SIGTERM, SIG_DFL);
    if (!cli.log_level.empty()) {
        anpr::Logger::instance().setLevel(
            anpr::logLevelFromString(cli.log_level, anpr::Logger::instance().level()));
    }
    const anpr::cameras::CameraCommandOptions options = cameraOptions(cli);
    try {
        if (cli.camera_scan) {
            return anpr::cameras::runCameraScan(options);
        }
        if (cli.camera_check) {
            return anpr::cameras::runCameraCheck(options);
        }
        return anpr::cameras::runCameraStatus(options);
    } catch (const std::exception& failure) {
        anpr::logEvent(anpr::LogLevel::kError, "fatal",
                       anpr::LogFields().add("reason", failure.what()));
        return 1;
    }
}

/// Every event goes to stdout as a JSON line; `--events-file` adds a persistent copy.
std::shared_ptr<anpr::PlateSink> makeEventSink(const std::string& events_file,
                                               std::string& error) {
    auto sink = std::make_shared<anpr::FanOutSink>();
    sink->add(std::make_shared<anpr::JsonStdoutSink>());
    if (!events_file.empty()) {
        auto file = std::make_shared<anpr::JsonLinesFileSink>(events_file);
        if (!file->ok()) {
            error = "INVALID_CONFIG: cannot open events file " + events_file;
            return nullptr;
        }
        sink->add(std::move(file));
    }
    return sink;
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
    if (cli.cameraReadOnlyCommand()) {
        return runCameraReadOnlyCommand(cli);
    }

    anpr::ConfigLoadResult loaded = anpr::loadConfigFile(cli.config_path);
    if (!loaded.ok) {
        std::cerr << loaded.error << '\n';
        return 2;
    }
    anpr::AnprConfig config = loaded.config;

    const std::shared_ptr<anpr::PlateSink> event_sink = makeEventSink(cli.events_file, error);
    if (event_sink == nullptr) {
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
    if (cli.cameras) {
        anpr::logEvent(anpr::LogLevel::kInfo, "startup",
                       anpr::LogFields()
                           .add("version", "1.0.0")
                           .add("mode", "cameras")
                           .add("config", cli.config_path)
                           .add("camera_config", cli.camera_config)
                           .add("requested_backend", anpr::toString(config.inference.backend))
                           .add("ocr_model", config.ocr.model)
                           .add("events_file", cli.events_file.empty() ? "none" : cli.events_file)
                           .add("opencv_threads", config.performance.opencv_threads));
        try {
            // Several processing threads deliver events: one JSON line at a time.
            return anpr::cameras::runCameras(cameraOptions(cli), config,
                                             std::make_shared<anpr::SynchronizedSink>(event_sink),
                                             g_stop_requested, cli.warmup);
        } catch (const std::exception& failure) {
            anpr::logEvent(anpr::LogLevel::kError, "fatal",
                           anpr::LogFields().add("reason", failure.what()));
            return 1;
        }
    }
    anpr::logEvent(anpr::LogLevel::kInfo, "startup",
                   anpr::LogFields()
                       .add("version", "1.0.0")
                       .add("config", cli.config_path)
                       .add("camera_id", config.camera.camera_id)
                       .add("source", anpr::maskCredentials(config.camera.source))
                       .add("streams", cli.sources.empty() ? 1 : cli.sources.size())
                       .add("requested_backend", anpr::toString(config.inference.backend))
                       .add("ocr_model", config.ocr.model)
                       .add("events_file", cli.events_file.empty() ? "none" : cli.events_file)
                       .add("opencv_threads", config.performance.opencv_threads));

    try {
        if (cli.sources.size() > 1) {
            // One detector and one OCR for every stream, one capture and one processing thread
            // per stream; the threads share the event sink.
            return anpr::cameras::runSources(cli.sources, config,
                                             std::make_shared<anpr::SynchronizedSink>(event_sink),
                                             g_stop_requested, cli.warmup);
        }

        anpr::AnprPipeline pipeline(config, *event_sink);
        pipeline.setSnapshotDirectory(cli.snapshots_dir);
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
