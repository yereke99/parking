#include "anpr/cameras/camera_commands.hpp"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <future>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <system_error>
#include <thread>
#include <utility>

#include "anpr/camera/camera_source.hpp"
#include "anpr/cameras/camera_registry.hpp"
#include "anpr/cameras/multi_camera_runner.hpp"
#include "anpr/cameras/reconnect_policy.hpp"
#include "anpr/cameras/rtsp_camera_source.hpp"
#include "anpr/cameras/stream_check.hpp"
#include "anpr/cameras/system_stats.hpp"
#include "anpr/common/logging.hpp"
#include "anpr/hikvision/isapi.hpp"

namespace anpr::cameras {
namespace {

/// How often a missing camera LAN is looked for again, and how often the wait is logged.
constexpr std::int64_t kLanRetryMs = 10000;
constexpr std::int64_t kLanWaitLogMs = 60000;
/// How often the running process re-reads the camera LAN carrier.
constexpr std::int64_t kLinkCheckMs = 5000;
/// Rediscovery cadence while no camera runs, or during the first minutes while a registered camera
/// is still missing: PoE cameras boot for a minute or two, often after the Jetson. Later the
/// configured interval applies, so a camera removed for good does not cause a subnet sweep every
/// half minute.
constexpr std::int64_t kFastRescanMs = 30000;
constexpr std::int64_t kFastRescanWindowMs = 600000;

std::int64_t monotonicMs() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

std::int64_t unixMs() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

/// Sleeps up to `duration_ms`, waking within 100 ms of a stop request.
void sleepUnlessStopped(std::int64_t duration_ms, const std::atomic_bool& stop) {
    const std::int64_t deadline = monotonicMs() + duration_ms;
    while (!stop.load()) {
        const std::int64_t remaining = deadline - monotonicMs();
        if (remaining <= 0) {
            return;
        }
        const std::int64_t slice = std::min<std::int64_t>(100, remaining);
        std::this_thread::sleep_for(std::chrono::milliseconds(slice));
    }
}

std::string join(const std::vector<std::string>& items, const std::string& separator) {
    std::string text;
    for (const std::string& item : items) {
        if (!text.empty()) {
            text += separator;
        }
        text += item;
    }
    return text;
}

/// Prefixes every non-empty line, so a formatError block nests inside a camera block.
std::string indent(const std::string& text, const std::string& prefix) {
    std::string out;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t newline = text.find('\n', start);
        const std::size_t end = newline == std::string::npos ? text.size() : newline;
        if (end > start) {
            out += prefix;
        }
        out.append(text, start, end - start);
        if (newline == std::string::npos) {
            break;
        }
        out += '\n';
        start = newline + 1;
    }
    return out;
}

std::string orDash(const std::string& value) {
    return value.empty() ? "-" : value;
}

/// Runs `function` on its own thread, or deferred (at `get`) when no thread can be created, so a
/// starved system degrades to sequential probes instead of failing the command.
template <typename Function>
auto runAsync(Function function) -> std::future<decltype(function())> {
    try {
        return std::async(std::launch::async, function);
    } catch (const std::system_error&) {
        return std::async(std::launch::deferred, function);
    }
}

template <typename T>
bool futureReady(const std::future<T>& future) {
    return future.wait_for(std::chrono::seconds(0)) != std::future_status::timeout;
}

DiagnosticContext cameraContext(const CameraTarget& target, Stage stage) {
    DiagnosticContext context;
    context.camera_id = target.id;
    context.ip = net::toString(target.ip);
    context.stage = stage;
    return context;
}

CameraStatus baseStatus(const CameraTarget& target) {
    CameraStatus status;
    status.id = target.id;
    status.ip = net::toString(target.ip);
    status.mac = target.mac;
    status.model = target.model;
    return status;
}

void setError(CameraStatus& status, CameraError error, const std::string& detail) {
    status.error = toString(error);
    status.action = describe(error).action;
    status.detail = detail;
}

/// The detail discovery attached to `error` for this camera, empty when none.
std::string discoveryDetail(const CameraTarget& target, CameraError error) {
    for (const auto& problem : target.discovery_problems) {
        if (problem.first == error) {
            return problem.second;
        }
    }
    return {};
}

// ---- Configuration and environment --------------------------------------------------------------

/// OPENCV_FFMPEG_CAPTURE_OPTIONS replaces OpenCV's implicit RTSP-over-TCP default, so it is set
/// once, before any capture thread exists (setenv is not thread-safe), and never over a value
/// the operator chose.
void applyFfmpegCaptureOptions(std::int64_t read_timeout_ms) {
    static bool applied = false;
    if (applied) {
        return;
    }
    applied = true;
    if (std::getenv("OPENCV_FFMPEG_CAPTURE_OPTIONS") != nullptr) {
        logEvent(LogLevel::kDebug, "ffmpeg_capture_options",
                 LogFields().add("source", "environment"));
        return;
    }
    const std::string value = ffmpegCaptureOptions(read_timeout_ms);
    if (::setenv("OPENCV_FFMPEG_CAPTURE_OPTIONS", value.c_str(), 0) != 0) {
        logEvent(LogLevel::kWarn, "ffmpeg_capture_options_failed",
                 LogFields().addQuoted("reason", net::errnoText(errno)));
        return;
    }
    logEvent(LogLevel::kDebug, "ffmpeg_capture_options",
             LogFields().add("source", "camera_config").addQuoted("value", value));
}

bool loadCameraConfig(const CameraCommandOptions& options, CameraModeConfig& config) {
    CameraModeConfigResult loaded = loadCameraModeConfigFile(options.camera_config_path);
    if (!loaded.ok) {
        std::cerr << loaded.error;
        if (loaded.error.find(options.camera_config_path) == std::string::npos) {
            std::cerr << " (" << options.camera_config_path << ')';
        }
        std::cerr << '\n';
        return false;
    }
    for (const std::string& key : loaded.unknown_keys) {
        logEvent(LogLevel::kWarn, "unknown_config_key",
                 LogFields().addQuoted("config", options.camera_config_path).add("key", key));
    }
    config = std::move(loaded.config);
    applyFfmpegCaptureOptions(config.capture.read_timeout_ms);
    return true;
}

/// Every per-camera ANPR profile named in the camera config, loaded once up front: a broken
/// profile is a configuration error at startup, not a camera that silently never starts.
bool loadCameraProfiles(const CameraModeConfig& config,
                        std::map<std::string, AnprConfig>& profiles) {
    for (const CameraOverride& camera : config.cameras) {
        if (camera.anpr_config.empty() || profiles.count(camera.anpr_config) != 0) {
            continue;
        }
        ConfigLoadResult loaded = loadConfigFile(camera.anpr_config);
        if (!loaded.ok) {
            std::cerr << loaded.error << " (camera profile " << camera.anpr_config << ")\n";
            return false;
        }
        for (const std::string& key : loaded.unknown_keys) {
            logEvent(LogLevel::kWarn, "unknown_config_key",
                     LogFields().addQuoted("config", camera.anpr_config).add("key", key));
        }
        profiles.emplace(camera.anpr_config, std::move(loaded.config));
    }
    return true;
}

// ---- Network ------------------------------------------------------------------------------------

struct NetworkState {
    net::NetworkSnapshot snapshot;
    net::CameraLanSelection lan;
    net::InternetStatus internet;
    std::vector<std::string> overlaps;
};

NetworkState inspectNetwork(const CameraModeConfig& config) {
    NetworkState state;
    state.snapshot = net::readNetworkSnapshot();
    state.lan = net::selectCameraLan(state.snapshot, config.network.interface);
    state.internet =
        net::inspectInternet(state.snapshot, config.network.internet_check,
                             config.network.internet_probe_targets,
                             config.network.internet_timeout_ms);
    state.overlaps = net::overlappingSubnets(state.snapshot, state.lan);
    return state;
}

std::optional<NetworkProblem> lanProblem(const net::CameraLanSelection& lan) {
    NetworkProblem problem;
    problem.context.interface = lan.interface;
    problem.context.stage = Stage::kNetwork;
    problem.detail = lan.detail;
    switch (lan.status) {
        case net::CameraLanStatus::kReady:
            return std::nullopt;
        case net::CameraLanStatus::kNoIpv4:
            problem.error = CameraError::kCameraSubnetUnconfigured;
            if (problem.detail.empty()) {
                problem.detail = lan.interface + " has link but no usable IPv4 address";
            }
            break;
        case net::CameraLanStatus::kNoLink:
            problem.error = CameraError::kCameraLanNotFound;
            if (problem.detail.empty()) {
                problem.detail = lan.interface + " has no link";
            }
            break;
        case net::CameraLanStatus::kNoInterface:
            problem.error = CameraError::kCameraLanNotFound;
            if (problem.detail.empty()) {
                problem.detail = "no wired Ethernet interface found";
            }
            break;
        case net::CameraLanStatus::kOverrideMissing:
            problem.error = CameraError::kCameraLanNotFound;
            if (problem.detail.empty()) {
                problem.detail = "configured interface " + lan.interface + " does not exist";
            }
            break;
    }
    return problem;
}

void printProblems(const std::vector<NetworkProblem>& problems, std::ostream& out) {
    std::ostringstream text;
    for (const NetworkProblem& problem : problems) {
        text << formatError(problem.error, problem.context, problem.detail);
    }
    out << text.str() << std::flush;
    for (const NetworkProblem& problem : problems) {
        reportCameraError(problem.error, problem.context, problem.detail);
    }
}

/// The CAMERA NETWORK / INTERNET block and one block plus one log line per problem.
void reportNetwork(const NetworkState& state, const CameraModeConfig& config, std::ostream& out) {
    out << net::formatNetworkSummary(state.snapshot, state.lan, state.internet);
    const std::vector<NetworkProblem> problems =
        networkProblems(state.lan, state.internet, state.overlaps, config.network);
    if (!problems.empty()) {
        out << '\n';
    }
    printProblems(problems, out);
}

// ---- Registry and discovery ---------------------------------------------------------------------

std::shared_ptr<CameraRegistry> loadRegistry(const std::string& path) {
    std::string error;
    auto registry = std::make_shared<CameraRegistry>(CameraRegistry::load(path, error));
    if (!error.empty()) {
        logEvent(LogLevel::kWarn, "registry_unreadable",
                 LogFields().addQuoted("file", path).addQuoted("reason", error));
    }
    return registry;
}

/// A read-only filesystem or a missing var/ only costs id stability across restarts; the ids
/// assigned in memory still hold for this run.
void saveRegistry(const CameraRegistry& registry, const std::string& path) {
    std::string error;
    if (!registry.save(path, error)) {
        logEvent(LogLevel::kWarn, "registry_save_failed",
                 LogFields().addQuoted("file", path).addQuoted("reason", error));
    }
}

DiscoveryReport discoverAndSave(const net::NetworkSnapshot& snapshot,
                                const net::CameraLanSelection& lan,
                                const CameraModeConfig& config, CameraRegistry& registry) {
    DiscoveryReport report = discoverCameras(snapshot, lan, config, registry);
    saveRegistry(registry, config.discovery.registry_file);
    return report;
}

std::size_t camerasOnCameraSubnet(const DiscoveryReport& report) {
    return static_cast<std::size_t>(
        std::count_if(report.cameras.begin(), report.cameras.end(),
                      [](const DiscoveredCamera& camera) { return camera.on_camera_subnet; }));
}

// ---- Report pieces ------------------------------------------------------------------------------

std::string yesNo(bool value) {
    return value ? "yes" : "no";
}

/// "CAMERA camera-01" and the identity line, as formatStreamCheck starts its blocks.
std::string cameraHeader(const CameraTarget& target) {
    std::ostringstream out;
    out << "CAMERA " << target.id << '\n';
    out << "  IP: " << net::toString(target.ip) << "   MAC: " << orDash(target.mac)
        << "   Vendor: " << orDash(target.vendor) << "   Model: " << orDash(target.model) << '\n';
    return out.str();
}

std::string credentialsLine(const CameraTarget& target, const CameraModeConfig& config) {
    if (target.has_credentials) {
        return "  Credentials: " + target.credential_source + '\n';
    }
    return "  Credentials: none (set " + config.rtsp.username_env + " / " +
           config.rtsp.password_env + ")\n";
}

std::string nativeAnprWord(hikvision::NativeAnprSupport support) {
    switch (support) {
        case hikvision::NativeAnprSupport::kSupported:
            return "supported";
        case hikvision::NativeAnprSupport::kNotSupported:
            return "not supported";
        case hikvision::NativeAnprSupport::kUnknown:
            break;
    }
    return "unknown";
}

std::string formatIsapiLines(const CameraTarget& target, const hikvision::IsapiProbe& probe) {
    std::ostringstream out;
    const DiagnosticContext context = cameraContext(target, Stage::kIsapi);
    switch (probe.status) {
        case hikvision::IsapiStatus::kOk: {
            out << "  ISAPI: OK";
            const auto field = [&out](const char* label, const std::string& value) {
                if (!value.empty()) {
                    out << "   " << label << ": " << value;
                }
            };
            field("Model", probe.device.model);
            field("Serial", probe.device.serial);
            std::string firmware = probe.device.firmware;
            if (!probe.device.firmware_date.empty()) {
                firmware += (firmware.empty() ? "" : " ") + probe.device.firmware_date;
            }
            field("Firmware", firmware);
            out << '\n' << "  Native ANPR: " << nativeAnprWord(probe.native_anpr) << '\n';
            if (target.native_anpr &&
                probe.native_anpr != hikvision::NativeAnprSupport::kSupported) {
                out << indent(formatError(CameraError::kNativeAnprUnavailable, context,
                                          probe.native_anpr ==
                                                  hikvision::NativeAnprSupport::kNotSupported
                                              ? "the camera reports no ANPR support"
                                              : "the camera does not report ANPR support"),
                              "  ");
            }
            break;
        }
        case hikvision::IsapiStatus::kAuthFailed:
            out << "  ISAPI: AUTH_FAILED (the web service rejected the login RTSP accepted; "
                   "not retried)\n";
            if (target.native_anpr) {
                out << indent(formatError(CameraError::kNativeAnprUnavailable, context,
                                          "ISAPI rejected the login"),
                              "  ");
            }
            break;
        case hikvision::IsapiStatus::kUnavailable:
            out << indent(formatError(CameraError::kIsapiUnavailable, context, probe.detail), "  ");
            break;
        case hikvision::IsapiStatus::kError:
            out << "  ISAPI: ERROR";
            if (!probe.detail.empty()) {
                out << " (" << probe.detail << ')';
            }
            out << '\n';
            break;
        case hikvision::IsapiStatus::kSkipped:
            break;
    }
    return out.str();
}

/// Inserts `extra` before the block's final "  STATUS:" line, so STATUS stays the last line.
std::string insertBeforeStatus(const std::string& block, const std::string& extra) {
    if (extra.empty()) {
        return block;
    }
    const std::size_t status = block.rfind("\n  STATUS:");
    if (status == std::string::npos) {
        return block + extra;
    }
    return block.substr(0, status + 1) + extra + block.substr(status + 1);
}

void reportHardwareDecoder(const DecoderCapabilities& capabilities,
                           const CameraModeConfig& config, std::ostream& out) {
    if (config.decode.decoder == DecoderPreference::kSoftware ||
        hardwareDecodingAvailable(capabilities)) {
        return;
    }
    DiagnosticContext context;
    context.stage = Stage::kDecode;
    const std::string detail = join(missingHardwareDecoding(capabilities), "; ");
    out << '\n' << formatError(CameraError::kHardwareDecoderUnavailable, context, detail)
        << std::flush;
    reportCameraError(CameraError::kHardwareDecoderUnavailable, context, detail);
}

void warnIfLowMemory(const CameraModeConfig& config) {
    SystemStatsReader reader;
    const SystemStats stats = reader.read();
    if (stats.mem_total_kb <= 0) {
        return;  // no /proc/meminfo (not Linux): nothing to judge
    }
    const std::int64_t available_mb = stats.mem_available_kb / 1024;
    if (available_mb >= config.runtime.min_available_ram_mb) {
        return;
    }
    DiagnosticContext context;
    context.stage = Stage::kSystem;
    reportCameraError(CameraError::kOutOfMemoryRisk, context,
                      "available " + std::to_string(available_mb) + " MB of " +
                          std::to_string(stats.mem_total_kb / 1024) +
                          " MB before loading the models; runtime.min_available_ram_mb is " +
                          std::to_string(config.runtime.min_available_ram_mb));
}

void reportModelFailure(const std::string& error, std::ostream& out) {
    DiagnosticContext context;
    context.stage = Stage::kInference;
    out << '\n' << formatError(CameraError::kTensorrtNotAvailable, context, error) << std::flush;
    reportCameraError(CameraError::kTensorrtNotAvailable, context, error);
}

/// With link but no address the cameras still answer SADP (layer 2), which says which subnet
/// the Jetson has to join.
void suggestAddress(const NetworkState& state, const CameraModeConfig& config,
                    CameraRegistry& registry, std::ostream& out) {
    const DiscoveryReport report =
        discoverAndSave(state.snapshot, state.lan, config, registry);
    out << '\n' << formatAddressSuggestion(report.cameras, state.lan.interface) << std::flush;
}

/// Blocks until the camera LAN is ready (cable plugged in, static address configured) or `stop`.
/// An installer may plug the cable in after starting, and at boot the service may start before
/// NetworkManager has configured the port: each new problem is printed once, the wait is logged
/// once a minute.
bool waitForCameraLan(const CameraModeConfig& config, NetworkState& state,
                      CameraRegistry& registry, const std::atomic_bool& stop, std::ostream& out) {
    if (state.lan.status == net::CameraLanStatus::kNoIpv4) {
        suggestAddress(state, config, registry, out);
    }
    net::CameraLanStatus reported = state.lan.status;
    const std::int64_t started = monotonicMs();
    std::int64_t next_log = started + kLanWaitLogMs;
    for (;;) {
        sleepUnlessStopped(kLanRetryMs, stop);
        if (stop.load()) {
            return false;
        }
        state.snapshot = net::readNetworkSnapshot();
        state.lan = net::selectCameraLan(state.snapshot, config.network.interface);
        if (state.lan.ready()) {
            state = inspectNetwork(config);
            if (state.lan.ready()) {
                out << "\nCamera LAN is up after " << (monotonicMs() - started) / 1000 << " s.\n";
                reportNetwork(state, config, out);
                return true;
            }
        }
        if (state.lan.status != reported) {
            reported = state.lan.status;
            const std::optional<NetworkProblem> problem = lanProblem(state.lan);
            if (problem) {
                out << '\n';
                printProblems({*problem}, out);
            }
            if (state.lan.status == net::CameraLanStatus::kNoIpv4) {
                suggestAddress(state, config, registry, out);
            }
        }
        const std::int64_t now = monotonicMs();
        if (now >= next_log) {
            next_log += kLanWaitLogMs;
            const std::optional<NetworkProblem> problem = lanProblem(state.lan);
            logEvent(LogLevel::kWarn, "camera_lan_waiting",
                     LogFields()
                         .addQuoted("interface", state.lan.interface)
                         .add("error", problem ? toString(problem->error) : "NONE")
                         .add("waited_s", (now - started) / 1000));
        }
    }
}

/// The status row of a registered camera that discovery did not find now.
CameraStatus missingCameraStatus(const RegistryEntry& entry, const CameraModeConfig& config) {
    CameraStatus status;
    status.id = entry.id;
    status.ip = entry.last_ip;
    status.mac = entry.mac;
    status.model = entry.model;
    const CameraOverride* camera_override =
        findOverride(config, entry.id, entry.mac, entry.serial, entry.last_ip);
    if (camera_override != nullptr && camera_override->enabled && !*camera_override->enabled) {
        status.anpr = "DISABLED";
        setError(status, CameraError::kCameraDisabled, "");
        return status;
    }
    status.link = "DOWN";
    status.anpr = "OFFLINE";
    setError(status, CameraError::kCameraUnreachable,
             "not found by discovery" +
                 (entry.last_seen.empty() ? std::string() : "; last seen " + entry.last_seen));
    return status;
}

// ---- Camera mode run ----------------------------------------------------------------------------

struct RediscoveryOutcome {
    bool lan_ready{false};
    net::NetworkSnapshot snapshot;
    net::CameraLanSelection lan;
    DiscoveryReport report;
    std::vector<CameraTarget> targets;
    /// Preflights of the cameras that could be started, by id.
    std::map<std::string, RtspCheck> checks;
};

/// The cameras of one `run-cameras` process: which are processed, which wait, and why.
///
/// Every camera is contacted with credentials as rarely as the lockout allows: a rejected login
/// found by the startup preflight is not repeated by the camera's source a second later; the
/// camera is started only when its retry is due, with one retry fewer, so a wrong password costs
/// at most 1 + capture.auth_max_retries attempts per process. Rediscovery never preflights a
/// camera that already runs, waits for a login retry or gave up.
class CameraFleet {
public:
    CameraFleet(const CameraModeConfig& config, const AnprConfig& base,
                std::map<std::string, AnprConfig> profiles, DecoderCapabilities capabilities,
                EnvLookup env, MultiCameraRunner& runner,
                std::shared_ptr<CameraRegistry> registry, const NetworkState& network)
        : config_(config),
          base_(base),
          profiles_(std::move(profiles)),
          capabilities_(std::move(capabilities)),
          env_(std::move(env)),
          runner_(runner),
          registry_(std::move(registry)),
          snapshot_(network.snapshot),
          lan_(network.lan),
          internet_summary_(internetSummary(network.internet)) {
        const std::int64_t now = monotonicMs();
        started_ms_ = now;
        next_link_check_ms_ = now + kLinkCheckMs;
        next_rediscovery_ms_ = now + config_.discovery.rescan_interval_ms;
    }

    CameraFleet(const CameraFleet&) = delete;
    CameraFleet& operator=(const CameraFleet&) = delete;

    ~CameraFleet() { finish(); }

    /// Logs each discovery problem once per camera and code.
    void logDiscoveryProblems(const DiscoveryReport& report) {
        for (const auto& problem : report.problems) {
            if (logged_problems_.insert(toString(problem.first) + "|" + problem.second).second) {
                DiagnosticContext context;
                context.interface = lan_.interface;
                context.stage = Stage::kDiscovery;
                reportCameraError(problem.first, context, problem.second);
            }
        }
        for (const DiscoveredCamera& camera : report.cameras) {
            for (const auto& problem : camera.problems) {
                if (logged_problems_.insert(camera.id + "|" + toString(problem.first)).second) {
                    DiagnosticContext context;
                    context.camera_id = camera.id;
                    context.ip = net::toString(camera.ip);
                    context.stage = Stage::kDiscovery;
                    reportCameraError(problem.first, context, problem.second);
                }
            }
        }
    }

    /// Decides every discovered camera in id order; preflights run in parallel.
    std::vector<StartupCameraLine> admitAtStartup(const std::vector<CameraTarget>& targets) {
        enum class Plan { kDisabled, kBlocked, kStandby, kPreflight };
        std::vector<Plan> plans;
        plans.reserve(targets.size());
        int slots = 0;
        for (const CameraTarget& target : targets) {
            if (!target.enabled) {
                plans.push_back(Plan::kDisabled);
            } else if (blockingDiscoveryProblem(target) != CameraError::kNone) {
                plans.push_back(Plan::kBlocked);
            } else if (slots >= config_.runtime.max_active_cameras) {
                plans.push_back(Plan::kStandby);
            } else {
                plans.push_back(Plan::kPreflight);
                ++slots;
            }
        }

        std::vector<std::future<RtspCheck>> checks(targets.size());
        for (std::size_t index = 0; index < targets.size(); ++index) {
            if (plans[index] == Plan::kPreflight) {
                const CameraTarget& target = targets[index];
                const net::NetworkSnapshot& snapshot = snapshot_;
                const net::CameraLanSelection& lan = lan_;
                const int timeout_ms = config_.rtsp.timeout_ms;
                checks[index] = runAsync([&target, &snapshot, &lan, timeout_ms] {
                    return checkRtsp(target, snapshot, lan, timeout_ms);
                });
            }
        }

        std::vector<StartupCameraLine> lines;
        lines.reserve(targets.size());
        // Cameras to hand to the runner, by index into `lines`. Opening one takes seconds (the
        // source's own preflight, the decoder, the first frame), so they open in parallel below.
        std::vector<std::pair<std::size_t, std::unique_ptr<Prepared>>> to_start;
        for (std::size_t index = 0; index < targets.size(); ++index) {
            const CameraTarget& target = targets[index];
            StartupCameraLine line;
            line.id = target.id;
            line.model = target.model;
            line.ip = net::toString(target.ip);
            line.rtsp = "-";
            line.video = "-";
            switch (plans[index]) {
                case Plan::kDisabled:
                    registerDisabled(target);
                    line.anpr = "DISABLED";
                    break;
                case Plan::kBlocked: {
                    const CameraError problem = blockingDiscoveryProblem(target);
                    registerBlocked(target, problem);
                    line.rtsp = rtspStartupWord(problem);
                    line.anpr = config_.discovery.rescan_interval_ms > 0
                                    ? "WAITING (rechecked by discovery every " +
                                          formatRetryDelay(config_.discovery.rescan_interval_ms) +
                                          ")"
                                    : "ERROR";
                    break;
                }
                case Plan::kStandby:
                    registerStandby(target);
                    line.anpr = "STANDBY (camera limit " +
                                std::to_string(config_.runtime.max_active_cameras) + ")";
                    break;
                case Plan::kPreflight: {
                    const RtspCheck rtsp = checks[index].get();
                    line.rtsp = rtspStartupWord(rtsp.error);
                    line.anpr = anprStartupText(rtsp.error, config_.capture);
                    if (rtsp.error != CameraError::kNone &&
                        classifyFailure(rtsp.error) == FailureClass::kAuthentication) {
                        scheduleLogin(target, rtsp);
                        break;
                    }
                    // OK, transient or configuration: the camera's own source reconnects on its
                    // schedule (backoff for the network, configuration_retry_interval_ms for a
                    // wrong path) and logs its own errors.
                    std::unique_ptr<Prepared> prepared = prepare(target, rtsp.auth_ok, false);
                    if (prepared == nullptr) {
                        line.anpr = "ERROR (not started, see the log)";
                        break;
                    }
                    prepared->preflight_ok = rtsp.error == CameraError::kNone;
                    to_start.emplace_back(lines.size(), std::move(prepared));
                    break;
                }
            }
            lines.push_back(std::move(line));
        }

        std::vector<std::future<std::string>> opens;
        opens.reserve(to_start.size());
        for (auto& entry : to_start) {
            Prepared* prepared = entry.second.get();
            MultiCameraRunner* runner = &runner_;
            opens.push_back(
                runAsync([prepared, runner] { return addToRunner(*runner, *prepared); }));
        }
        for (std::size_t item = 0; item < to_start.size(); ++item) {
            Prepared& prepared = *to_start[item].second;
            const std::string error = opens[item].get();
            record(prepared, error);
            StartupCameraLine& line = lines[to_start[item].first];
            if (!error.empty()) {
                line.anpr = "ERROR (not started, see the log)";
            } else if (prepared.preflight_ok) {
                first_frame_watch_[prepared.target.id] = prepared.source;
            }
        }
        last_round_incomplete_ = registerMissing(targets) > 0;
        next_rediscovery_ms_ = monotonicMs() + rediscoveryDelay();
        return lines;
    }

    /// Waits up to capture.first_frame_timeout_ms (all cameras at once) for the first frame of
    /// every camera whose preflight passed, then fills the VIDEO lines.
    void waitForFirstFrames(std::vector<StartupCameraLine>& lines, const std::atomic_bool& stop) {
        if (first_frame_watch_.empty()) {
            return;
        }
        const std::int64_t deadline =
            monotonicMs() + std::max<std::int64_t>(0, config_.capture.first_frame_timeout_ms);
        while (!stop.load() && monotonicMs() < deadline) {
            const bool all = std::all_of(
                first_frame_watch_.begin(), first_frame_watch_.end(),
                [](const std::pair<const std::string, RtspCameraSource*>& entry) {
                    return entry.second->status().first_frame_ms > 0.0;
                });
            if (all) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        const std::vector<CameraStatus> statuses = runner_.statuses();
        for (StartupCameraLine& line : lines) {
            const auto watched = first_frame_watch_.find(line.id);
            if (watched == first_frame_watch_.end()) {
                continue;
            }
            const RtspCameraSource::Status status = watched->second->status();
            const auto row = std::find_if(statuses.begin(), statuses.end(),
                                          [&line](const CameraStatus& candidate) {
                                              return candidate.id == line.id;
                                          });
            const bool row_has_video =
                row != statuses.end() && !row->video.empty() && row->video != "-";
            if (status.first_frame_ms > 0.0 || row_has_video) {
                const int width = status.width > 0 ? status.width
                                                    : (row != statuses.end() ? row->width : 0);
                const int height = status.height > 0 ? status.height
                                                     : (row != statuses.end() ? row->height : 0);
                const double fps = status.stream_fps > 0.0
                                       ? status.stream_fps
                                       : (row != statuses.end() ? row->input_fps : 0.0);
                const std::string decoder =
                    !status.decoder.empty() ? status.decoder
                                            : (row != statuses.end() ? row->decoder : "");
                line.video = formatVideoSummary(status.codec, width, height, fps, decoder);
            } else {
                line.video = "pending";
                if (status.error != CameraError::kNone) {
                    line.video += " (" + toString(status.error) + ")";
                }
            }
        }
        // The pointers are not needed after startup; the runner owns the sources.
        first_frame_watch_.clear();
    }

    [[nodiscard]] bool hasWork() const {
        return runner_.activeCameraCount() > 0 || !pending_logins_.empty() ||
               config_.discovery.rescan_interval_ms > 0;
    }

    /// Called about once a second from the runner thread.
    void periodic(const std::atomic_bool& stop) {
        if (stop.load()) {
            return;
        }
        const std::int64_t now = monotonicMs();
        collectStarts(false);
        watchLink(now);
        retryLogins(now);
        rediscover(now);
    }

    /// Waits for a rediscovery and camera starts still running (bounded by their timeouts).
    void finish() {
        collectStarts(true);
        if (rediscovery_.valid()) {
            try {
                rediscovery_.get();
            } catch (const std::exception&) {
                // Shutting down; the failure no longer matters.
            }
        }
    }

private:
    struct PendingLogin {
        CameraTarget target;
        std::int64_t due_ms{0};
        bool login_spent{false};
    };

    /// A camera ready to hand to the runner. `source` stays owned by the runner after that.
    struct Prepared {
        CameraTarget target;
        CameraWorkerSpec spec;
        RtspCameraSource* source{nullptr};
        bool preflight_ok{false};
    };

    struct Starting {
        std::unique_ptr<Prepared> prepared;
        std::future<std::string> result;
        /// Found by rediscovery after startup.
        bool late{false};
    };

    /// `runner.addCamera`, run on a helper thread; empty when the camera was added.
    static std::string addToRunner(MultiCameraRunner& runner, Prepared& prepared) {
        std::string error;
        if (!runner.addCamera(std::move(prepared.spec), error) && error.empty()) {
            error = "not started";
        }
        return error;
    }

    const CameraModeConfig& config_;
    const AnprConfig& base_;
    std::map<std::string, AnprConfig> profiles_;
    DecoderCapabilities capabilities_;
    EnvLookup env_;
    MultiCameraRunner& runner_;
    std::shared_ptr<CameraRegistry> registry_;

    /// The newest view of the network with a ready camera LAN, for sources started from now on.
    net::NetworkSnapshot snapshot_;
    net::CameraLanSelection lan_;
    std::string internet_summary_;

    std::set<std::string> started_;
    std::set<std::string> disabled_;
    std::set<std::string> given_up_;
    std::map<std::string, PendingLogin> pending_logins_;
    /// Last code shown in the status table for a camera that is not processed.
    std::map<std::string, std::string> inactive_codes_;
    /// Last preflight error logged for a camera rediscovery could not start.
    std::map<std::string, CameraError> last_reported_;
    std::set<std::string> logged_problems_;
    std::map<std::string, RtspCameraSource*> first_frame_watch_;
    std::vector<Starting> starting_;

    std::int64_t started_ms_{0};
    bool link_up_{true};
    std::int64_t next_link_check_ms_{0};
    std::future<RediscoveryOutcome> rediscovery_;
    std::int64_t next_rediscovery_ms_{0};
    bool last_round_incomplete_{false};

    /// Started (or being opened) plus waiting for a login retry. Counted here rather than from
    /// the runner, which only lists a camera once its first open has finished.
    [[nodiscard]] std::size_t usedSlots() const {
        return started_.size() + pending_logins_.size();
    }

    [[nodiscard]] bool slotFree() const {
        const int limit = std::max(0, config_.runtime.max_active_cameras);
        return usedSlots() < static_cast<std::size_t>(limit);
    }

    /// Adds or updates the camera's row in the status table when its code changed.
    void showInactive(const CameraStatus& status) {
        const auto known = inactive_codes_.find(status.id);
        const std::string key = status.anpr + "|" + status.error;
        if (known != inactive_codes_.end() && known->second == key) {
            return;
        }
        inactive_codes_[status.id] = key;
        runner_.addInactiveCamera(status);
    }

    void registerDisabled(const CameraTarget& target) {
        disabled_.insert(target.id);
        CameraStatus status = baseStatus(target);
        status.anpr = "DISABLED";
        setError(status, CameraError::kCameraDisabled, "");
        showInactive(status);
    }

    void registerBlocked(const CameraTarget& target, CameraError problem) {
        CameraStatus status = baseStatus(target);
        status.link = "OK";
        status.anpr = "ERROR";
        setError(status, problem, discoveryDetail(target, problem));
        showInactive(status);
    }

    /// Registered cameras discovery did not find (powered off, unplugged, rebooting) stay in the
    /// status table as OFFLINE; rediscovery starts them once they answer again.
    /// Returns how many of them are expected (not disabled in the config).
    std::size_t registerMissing(const std::vector<CameraTarget>& targets) {
        std::set<std::string> found;
        for (const CameraTarget& target : targets) {
            found.insert(target.id);
        }
        std::size_t missing = 0;
        for (const RegistryEntry& entry : registry_->entries()) {
            if (found.count(entry.id) != 0 || started_.count(entry.id) != 0 ||
                pending_logins_.count(entry.id) != 0 || given_up_.count(entry.id) != 0) {
                continue;
            }
            const CameraStatus status = missingCameraStatus(entry, config_);
            if (status.anpr != "DISABLED") {
                ++missing;
            }
            showInactive(status);
        }
        return missing;
    }

    [[nodiscard]] std::int64_t rediscoveryDelay() const {
        const std::int64_t interval = config_.discovery.rescan_interval_ms;
        const bool fast = started_.empty() ||
                          (last_round_incomplete_ &&
                           monotonicMs() - started_ms_ < kFastRescanWindowMs);
        return fast ? std::min(interval, kFastRescanMs) : interval;
    }

    void registerStandby(const CameraTarget& target) {
        CameraStatus status = baseStatus(target);
        status.anpr = "STANDBY";
        const std::string detail = "runtime.max_active_cameras is " +
                                   std::to_string(config_.runtime.max_active_cameras);
        setError(status, CameraError::kCameraLimitReached, detail);
        if (inactive_codes_.count(target.id) == 0) {
            reportCameraError(CameraError::kCameraLimitReached,
                              cameraContext(target, Stage::kDiscovery), detail);
        }
        showInactive(status);
    }

    /// A camera whose preflight login was rejected (or that has no credentials): its source is
    /// created only when the retry is due, so the rejected login is not repeated a second later.
    void scheduleLogin(const CameraTarget& target, const RtspCheck& rtsp) {
        reportCameraError(rtsp.error, cameraContext(target, Stage::kRtsp), rtsp.detail);
        last_reported_[target.id] = rtsp.error;
        CameraStatus status = baseStatus(target);
        status.link = rtsp.network_reachable || rtsp.rtsp_port_open ? "OK" : "DOWN";
        status.rtsp = rtspStatusWord(rtsp.error);
        status.anpr = "ERROR";
        if (config_.capture.auth_max_retries <= 0) {
            given_up_.insert(target.id);
            setError(status, rtsp.error,
                     "login not retried (capture.auth_max_retries is 0); restart after fixing "
                     "the credentials");
        } else {
            PendingLogin pending;
            pending.target = target;
            pending.due_ms = monotonicMs() + config_.capture.auth_retry_interval_ms;
            pending.login_spent =
                rtsp.probe.auth_attempted || rtsp.error == CameraError::kRtspAuthFailed;
            pending_logins_[target.id] = std::move(pending);
            setError(status, rtsp.error,
                     "next login attempt in " +
                         formatRetryDelay(config_.capture.auth_retry_interval_ms));
        }
        showInactive(status);
    }

    bool profileFor(const CameraTarget& target, const AnprConfig*& profile) {
        profile = nullptr;
        if (target.anpr_config.empty()) {
            return true;
        }
        auto found = profiles_.find(target.anpr_config);
        if (found == profiles_.end()) {
            ConfigLoadResult loaded = loadConfigFile(target.anpr_config);
            if (!loaded.ok) {
                logEvent(LogLevel::kError, "camera_profile_invalid",
                         LogFields()
                             .add("camera_id", target.id)
                             .addQuoted("config", target.anpr_config)
                             .addQuoted("reason", loaded.error));
                return false;
            }
            found = profiles_.emplace(target.anpr_config, std::move(loaded.config)).first;
        }
        profile = &found->second;
        return true;
    }

    /// Builds the camera's worker: its source, its recognition settings and, when RTSP has
    /// proven the login, its native ANPR listener. nullptr when its ANPR profile cannot be loaded.
    std::unique_ptr<Prepared> prepare(const CameraTarget& target, bool auth_proven,
                                      bool login_spent) {
        const AnprConfig* profile = nullptr;
        if (!profileFor(target, profile)) {
            return nullptr;
        }
        auto prepared = std::make_unique<Prepared>();
        prepared->target = target;
        CameraWorkerSpec& spec = prepared->spec;
        std::vector<std::string> replaced;
        spec.anpr = cameraAnprConfig(base_, profile, target, config_, replaced);
        if (!replaced.empty()) {
            logEvent(LogLevel::kWarn, "camera_profile_models_ignored",
                     LogFields()
                         .add("camera_id", target.id)
                         .addQuoted("config", target.anpr_config)
                         .add("sections", join(replaced, ","))
                         .addQuoted("reason", "models and inference are shared by all cameras; "
                                              "the base profile's settings are used"));
        }

        CameraModeConfig source_config = config_;
        if (login_spent) {
            // The preflight already used one of this process's login attempts.
            source_config.capture.auth_max_retries =
                std::max(0, source_config.capture.auth_max_retries - 1);
        }
        auto source = std::make_unique<RtspCameraSource>(target, source_config, capabilities_,
                                                         snapshot_, lan_);
        prepared->source = source.get();
        spec.camera_id = target.id;
        spec.source = std::move(source);
        spec.live = true;
        spec.queue_capacity = static_cast<std::size_t>(std::max(1, config_.capture.queue_size));
        spec.max_frame_age_ms = config_.capture.max_frame_age_ms;
        spec.ip = net::toString(target.ip);
        spec.mac = target.mac;
        spec.model = target.model;
        if (target.native_anpr) {
            const DiagnosticContext context = cameraContext(target, Stage::kIsapi);
            if (!target.has_credentials) {
                reportCameraError(CameraError::kNativeAnprUnavailable, context,
                                  "no credentials for the camera's event stream");
            } else if (!auth_proven) {
                // The event stream logs in with the same account; it must not spend a login
                // attempt that RTSP has not proven yet.
                reportCameraError(CameraError::kNativeAnprUnavailable, context,
                                  "not started: RTSP has not accepted the credentials yet; "
                                  "restart once the camera is READY");
            } else {
                spec.native_anpr = true;
                spec.native_anpr_options.camera_id = target.id;
                spec.native_anpr_options.host = target.ip;
                spec.native_anpr_options.http_port = target.http_port;
                spec.native_anpr_options.credentials = target.credentials;
            }
        }
        return prepared;
    }

    /// Bookkeeping after `runner_.addCamera` (empty `error`: added).
    void record(const Prepared& prepared, const std::string& error) {
        const CameraTarget& target = prepared.target;
        if (!error.empty()) {
            started_.erase(target.id);
            logEvent(LogLevel::kError, "camera_start_failed",
                     LogFields()
                         .add("camera_id", target.id)
                         .add("ip", net::toString(target.ip))
                         .addQuoted("reason", error));
            return;
        }
        started_.insert(target.id);
        inactive_codes_.erase(target.id);
        logEvent(LogLevel::kInfo, "camera_added",
                 LogFields()
                     .add("camera_id", target.id)
                     .add("ip", net::toString(target.ip))
                     .addQuoted("url", target.redactedUrl())
                     .add("decoder", toString(target.decoder))
                     .addQuoted("credentials",
                                target.has_credentials ? target.credential_source : "none"));
    }

    /// Hands a prepared camera to the runner on its own thread: opening it (the source's
    /// preflight, the decoder, the first frame) takes seconds, and the runner thread keeps
    /// writing the status file meanwhile. The id counts as started (a slot, skipped by
    /// rediscovery) until the open fails.
    void startInBackground(std::unique_ptr<Prepared> prepared, bool late) {
        started_.insert(prepared->target.id);
        Prepared* raw = prepared.get();
        MultiCameraRunner* runner = &runner_;
        Starting starting;
        starting.result = runAsync([raw, runner] { return addToRunner(*runner, *raw); });
        starting.prepared = std::move(prepared);
        starting.late = late;
        starting_.push_back(std::move(starting));
    }

    /// Records the background starts that finished (all of them with `wait`).
    void collectStarts(bool wait) {
        for (auto item = starting_.begin(); item != starting_.end();) {
            if (!wait && !futureReady(item->result)) {
                ++item;
                continue;
            }
            std::string error;
            try {
                error = item->result.get();
            } catch (const std::exception& failure) {
                error = failure.what();
            }
            record(*item->prepared, error);
            if (error.empty() && item->late) {
                logEvent(LogLevel::kInfo, "camera_discovered_late",
                         LogFields()
                             .add("camera_id", item->prepared->target.id)
                             .add("ip", net::toString(item->prepared->target.ip)));
            }
            item = starting_.erase(item);
        }
    }

    void watchLink(std::int64_t now) {
        if (now < next_link_check_ms_ || lan_.interface.empty()) {
            return;
        }
        next_link_check_ms_ = now + kLinkCheckMs;
        const net::NetworkSnapshot snapshot = net::readNetworkSnapshot();
        const net::NetworkInterface* nic = snapshot.find(lan_.interface);
        // A carrier the kernel cannot report counts as up: no false alarm on odd drivers.
        const bool has_link = nic != nullptr && nic->admin_up && nic->carrier.value_or(true);
        if (!has_link && link_up_) {
            link_up_ = false;
            DiagnosticContext context;
            context.interface = lan_.interface;
            context.stage = Stage::kNetwork;
            const std::string detail =
                nic == nullptr ? lan_.interface + " disappeared"
                               : (nic->admin_up ? lan_.interface + " lost its carrier"
                                                : lan_.interface + " is administratively down");
            reportCameraError(CameraError::kCameraLanLinkDown, context, detail);
            runner_.setNetworkSummary(
                lan_.interface + " " + toString(CameraError::kCameraLanLinkDown),
                internet_summary_);
        } else if (has_link && !link_up_) {
            link_up_ = true;
            logEvent(LogLevel::kInfo, "camera_lan_link_restored",
                     LogFields().add("interface", lan_.interface));
            runner_.setNetworkSummary(cameraLanSummary(lan_), internet_summary_);
        }
    }

    void retryLogins(std::int64_t now) {
        for (auto entry = pending_logins_.begin(); entry != pending_logins_.end();) {
            // A login is only spent while the camera LAN is up.
            if (now < entry->second.due_ms || !link_up_) {
                ++entry;
                continue;
            }
            PendingLogin pending = std::move(entry->second);
            entry = pending_logins_.erase(entry);
            logEvent(LogLevel::kInfo, "camera_login_retry",
                     LogFields()
                         .add("camera_id", pending.target.id)
                         .add("ip", net::toString(pending.target.ip)));
            std::unique_ptr<Prepared> prepared =
                prepare(pending.target, false, pending.login_spent);
            if (prepared == nullptr) {
                given_up_.insert(pending.target.id);
                continue;
            }
            startInBackground(std::move(prepared), false);
        }
    }

    void rediscover(std::int64_t now) {
        const std::int64_t interval = config_.discovery.rescan_interval_ms;
        if (interval <= 0) {
            return;
        }
        if (rediscovery_.valid()) {
            if (!futureReady(rediscovery_)) {
                return;
            }
            try {
                RediscoveryOutcome outcome = rediscovery_.get();
                applyRediscovery(outcome);
            } catch (const std::exception& failure) {
                logEvent(LogLevel::kError, "rediscovery_failed",
                         LogFields().addQuoted("reason", failure.what()));
            }
            next_rediscovery_ms_ = monotonicMs() + rediscoveryDelay();
            return;
        }
        if (now < next_rediscovery_ms_ || !link_up_) {
            return;
        }

        std::set<std::string> skip = started_;
        skip.insert(disabled_.begin(), disabled_.end());
        skip.insert(given_up_.begin(), given_up_.end());
        for (const auto& pending : pending_logins_) {
            skip.insert(pending.first);
        }
        const std::size_t limit =
            static_cast<std::size_t>(std::max(0, config_.runtime.max_active_cameras));
        const std::size_t free_slots = limit > usedSlots() ? limit - usedSlots() : 0;
        const CameraModeConfig config = config_;
        const EnvLookup env = env_;
        const std::shared_ptr<CameraRegistry> registry = registry_;
        logEvent(LogLevel::kDebug, "rediscovery_started",
                 LogFields().add("free_slots", free_slots));
        rediscovery_ = runAsync([config, env, registry, skip, free_slots] {
            RediscoveryOutcome outcome;
            outcome.snapshot = net::readNetworkSnapshot();
            outcome.lan = net::selectCameraLan(outcome.snapshot, config.network.interface);
            if (!outcome.lan.ready()) {
                return outcome;
            }
            outcome.lan_ready = true;
            outcome.report = discoverAndSave(outcome.snapshot, outcome.lan, config, *registry);
            outcome.targets = buildTargets(outcome.report, config, env);
            std::size_t remaining = free_slots;
            for (const CameraTarget& target : outcome.targets) {
                if (remaining == 0) {
                    break;
                }
                if (!target.enabled || skip.count(target.id) != 0 ||
                    blockingDiscoveryProblem(target) != CameraError::kNone) {
                    continue;
                }
                RtspCheck check =
                    checkRtsp(target, outcome.snapshot, outcome.lan, config.rtsp.timeout_ms);
                // Only a camera that will take a slot (started, or waiting for its login retry)
                // uses up the budget; a camera still booting does not keep the next one out.
                if (check.error == CameraError::kNone ||
                    classifyFailure(check.error) == FailureClass::kAuthentication) {
                    --remaining;
                }
                outcome.checks.emplace(target.id, std::move(check));
            }
            return outcome;
        });
    }

    void applyRediscovery(const RediscoveryOutcome& outcome) {
        last_round_incomplete_ = false;
        if (!outcome.lan_ready) {
            logEvent(LogLevel::kDebug, "rediscovery_skipped",
                     LogFields().add("reason", "camera_lan_not_ready"));
            last_round_incomplete_ = true;
            return;
        }
        snapshot_ = outcome.snapshot;
        lan_ = outcome.lan;
        logDiscoveryProblems(outcome.report);

        for (const CameraTarget& target : outcome.targets) {
            const std::string& id = target.id;
            if (started_.count(id) != 0 || disabled_.count(id) != 0 || given_up_.count(id) != 0 ||
                pending_logins_.count(id) != 0) {
                continue;
            }
            if (!target.enabled) {
                registerDisabled(target);
                continue;
            }
            const CameraError problem = blockingDiscoveryProblem(target);
            if (problem != CameraError::kNone) {
                registerBlocked(target, problem);
                continue;
            }
            const auto check = outcome.checks.find(id);
            if (check == outcome.checks.end()) {
                if (!slotFree()) {
                    registerStandby(target);
                }
                continue;
            }
            const RtspCheck& rtsp = check->second;
            if (rtsp.error == CameraError::kNone) {
                if (!slotFree()) {
                    registerStandby(target);
                    continue;
                }
                std::unique_ptr<Prepared> prepared = prepare(target, rtsp.auth_ok, false);
                if (prepared != nullptr) {
                    startInBackground(std::move(prepared), true);
                }
                continue;
            }
            if (classifyFailure(rtsp.error) == FailureClass::kAuthentication) {
                scheduleLogin(target, rtsp);
                continue;
            }
            // Still booting, or misconfigured: shown in the status table, tried next round.
            last_round_incomplete_ = true;
            const auto reported = last_reported_.find(id);
            if (reported == last_reported_.end() || reported->second != rtsp.error) {
                reportCameraError(rtsp.error, cameraContext(target, Stage::kRtsp), rtsp.detail);
                last_reported_[id] = rtsp.error;
            }
            CameraStatus status = baseStatus(target);
            status.link = rtsp.network_reachable || rtsp.rtsp_port_open ? "OK" : "DOWN";
            status.rtsp = rtspStatusWord(rtsp.error);
            status.anpr = classifyFailure(rtsp.error) == FailureClass::kTransient ? "OFFLINE"
                                                                                  : "ERROR";
            setError(status, rtsp.error, rtsp.detail);
            showInactive(status);
        }
        if (registerMissing(outcome.targets) > 0) {
            last_round_incomplete_ = true;
        }
    }
};

}  // namespace

// ---- Building blocks ----------------------------------------------------------------------------

bool cameraLanMissing(const net::CameraLanSelection& lan) {
    return lan.status == net::CameraLanStatus::kNoInterface ||
           lan.status == net::CameraLanStatus::kNoLink ||
           lan.status == net::CameraLanStatus::kOverrideMissing;
}

std::vector<NetworkProblem> networkProblems(const net::CameraLanSelection& lan,
                                            const net::InternetStatus& internet,
                                            const std::vector<std::string>& overlaps,
                                            const NetworkSettings& settings) {
    std::vector<NetworkProblem> problems;
    const auto add = [&problems](CameraError error, Stage stage, const std::string& interface,
                                 const std::string& detail) {
        NetworkProblem problem;
        problem.error = error;
        problem.context.interface = interface;
        problem.context.stage = stage;
        problem.detail = detail;
        problems.push_back(std::move(problem));
    };
    if (const std::optional<NetworkProblem> lan_problem = lanProblem(lan)) {
        problems.push_back(*lan_problem);
    }
    if (lan.carries_default_route) {
        add(CameraError::kCameraLanHasDefaultRoute, Stage::kNetwork, lan.interface,
            "the active default route leaves through " + lan.interface);
    }
    if (!overlaps.empty()) {
        add(CameraError::kSubnetConflict, Stage::kNetwork, lan.interface,
            "overlapping: " + join(overlaps, ", "));
    }
    if (!internet.has_default_route) {
        add(CameraError::kNoInternetRoute, Stage::kInternet, "", "");
    } else if (internet.reach == net::InternetReach::kOffline) {
        add(CameraError::kInternetOffline, Stage::kInternet, internet.interface,
            "no answer from " + join(settings.internet_probe_targets, ", ") + " within " +
                std::to_string(settings.internet_timeout_ms) + " ms");
    }
    return problems;
}

std::string cameraLanSummary(const net::CameraLanSelection& lan) {
    if (lan.status == net::CameraLanStatus::kReady) {
        std::vector<std::string> addresses;
        for (const net::Ipv4Network& network : lan.networks) {
            addresses.push_back(network.addressWithPrefix());
        }
        if (addresses.empty() && lan.address) {
            addresses.push_back(lan.address->network().addressWithPrefix());
        }
        return addresses.empty() ? lan.interface : lan.interface + " " + join(addresses, ",");
    }
    const std::string code = lan.status == net::CameraLanStatus::kNoIpv4
                                 ? toString(CameraError::kCameraSubnetUnconfigured)
                                 : toString(CameraError::kCameraLanNotFound);
    return lan.interface.empty() ? code : lan.interface + " " + code;
}

std::string internetSummary(const net::InternetStatus& internet) {
    if (!internet.has_default_route) {
        return toString(CameraError::kNoInternetRoute);
    }
    std::string reach;
    switch (internet.reach) {
        case net::InternetReach::kOnline:
            reach = "ONLINE";
            break;
        case net::InternetReach::kOffline:
            reach = "OFFLINE";
            break;
        case net::InternetReach::kNotChecked:
            reach = "NOT CHECKED";
            break;
    }
    return internet.interface + " " + reach;
}

std::optional<net::Ipv4Network> suggestCameraLanAddress(
    const std::vector<DiscoveredCamera>& cameras) {
    const auto usable = [](const DiscoveredCamera& camera) {
        return !camera.ip.isZero() && !net::isLoopback(camera.ip) && !net::isLinkLocal(camera.ip) &&
               !net::isMulticast(camera.ip);
    };
    std::vector<const DiscoveredCamera*> pool;
    for (const DiscoveredCamera& camera : cameras) {
        if (usable(camera) && (camera.sources & kFoundBySadp) != 0U) {
            pool.push_back(&camera);
        }
    }
    if (pool.empty()) {
        for (const DiscoveredCamera& camera : cameras) {
            if (usable(camera)) {
                pool.push_back(&camera);
            }
        }
    }
    if (pool.empty()) {
        return std::nullopt;
    }

    // The subnet most cameras sit in, by the mask SADP reports (/24 when unknown or odd).
    const auto prefixOf = [](const DiscoveredCamera& camera) {
        const std::optional<net::Ipv4> mask = net::parseIpv4(camera.subnet_mask);
        const int prefix = mask ? net::prefixFromNetmask(*mask) : -1;
        return prefix >= 8 && prefix <= 30 ? prefix : 24;
    };
    std::map<std::pair<std::uint32_t, int>, int> counts;
    for (const DiscoveredCamera* camera : pool) {
        const int prefix = prefixOf(*camera);
        ++counts[{net::Ipv4Network{camera->ip, prefix}.network().value, prefix}];
    }
    auto best = counts.begin();
    for (auto candidate = counts.begin(); candidate != counts.end(); ++candidate) {
        if (candidate->second > best->second) {
            best = candidate;
        }
    }
    const int prefix = best->first.second;
    const net::Ipv4Network subnet{net::Ipv4{best->first.first}, prefix};

    std::set<std::uint32_t> used;
    for (const DiscoveredCamera& camera : cameras) {
        used.insert(camera.ip.value);
        if (const std::optional<net::Ipv4> gateway = net::parseIpv4(camera.gateway)) {
            used.insert(gateway->value);
        }
    }
    // 64-bit arithmetic: host .200 of a subnet near the top of the address space must not wrap.
    const std::uint64_t base = subnet.network().value;
    const std::uint64_t broadcast = subnet.broadcast().value;
    const auto unused = [&used](std::uint64_t value) {
        return used.count(static_cast<std::uint32_t>(value)) == 0;
    };
    const auto address = [prefix](std::uint64_t value) {
        return net::Ipv4Network{net::Ipv4{static_cast<std::uint32_t>(value)}, prefix};
    };
    // Host .200 and up first (clear of the cameras, which installers number from .21 or .64),
    // then downwards.
    const std::uint64_t preferred = base + 200U;
    for (std::uint64_t value = preferred; value < broadcast; ++value) {
        if (unused(value)) {
            return address(value);
        }
    }
    for (std::uint64_t value = std::min(preferred, broadcast) - 1U; value > base; --value) {
        if (unused(value)) {
            return address(value);
        }
    }
    return std::nullopt;
}

std::string formatAddressSuggestion(const std::vector<DiscoveredCamera>& cameras,
                                    const std::string& interface) {
    std::ostringstream out;
    const std::string interface_argument =
        interface.empty() ? std::string() : "--interface " + interface + " ";
    out << "SUGGESTED CAMERA LAN ADDRESS\n";
    const std::optional<net::Ipv4Network> suggestion = suggestCameraLanAddress(cameras);
    if (suggestion) {
        std::vector<std::string> members;
        for (const DiscoveredCamera& camera : cameras) {
            if (suggestion->contains(camera.ip)) {
                members.push_back(camera.id.empty() ? net::toString(camera.ip)
                                                    : camera.id + " " + net::toString(camera.ip));
            }
        }
        out << "  The cameras are in " << suggestion->cidr();
        if (!members.empty()) {
            out << " (" << join(members, ", ") << ")";
        }
        out << "; " << net::toString(suggestion->address)
            << " is not used by any of them. Give it to the Jetson:\n";
        out << "  make camera-lan-setup LAN_ARGS='" << interface_argument << "--address "
            << suggestion->addressWithPrefix() << "'\n";
    } else {
        out << (cameras.empty() ? "  No camera answered SADP. Give the Jetson the address "
                                  "planned for the camera subnet, for example:\n"
                                : "  No free address found next to the cameras. Give the Jetson "
                                  "an unused address in the cameras' subnet, for example:\n");
        out << "  make camera-lan-setup LAN_ARGS='" << interface_argument
            << "--address 192.168.10.5/24'\n";
    }
    out << "  Without --apply in LAN_ARGS this is a dry run that prints the nmcli commands.\n";
    return out.str();
}

bool hardwareDecodingAvailable(const DecoderCapabilities& capabilities) {
    return missingHardwareDecoding(capabilities).empty();
}

std::vector<std::string> missingHardwareDecoding(const DecoderCapabilities& capabilities) {
    std::vector<std::string> missing;
    if (!capabilities.nvidia_decoder) {
        missing.push_back("nvv4l2decoder/nvvidconv GStreamer plugins not found");
    }
    if (!capabilities.nvidia_device) {
        missing.push_back("/dev/nvhost-nvdec not present in this container");
    }
    if (!capabilities.gst_rtsp) {
        missing.push_back("GStreamer RTSP elements (rtspsrc, depayloaders, parsers) not found");
    }
    if (!capabilities.gst_native && !capabilities.opencv_gstreamer) {
        missing.push_back("no GStreamer capture path (native capture not built, OpenCV without "
                          "GStreamer)");
    }
    return missing;
}

std::string formatDecoderCapabilities(const DecoderCapabilities& capabilities) {
    std::ostringstream out;
    out << "DECODER CAPABILITIES\n";
    out << "  native GStreamer capture: " << yesNo(capabilities.gst_native) << '\n';
    out << "  GStreamer RTSP elements: " << yesNo(capabilities.gst_rtsp) << '\n';
    out << "  nvv4l2decoder (NVIDIA hardware): " << yesNo(capabilities.nvidia_decoder) << '\n';
    out << "  /dev/nvhost-nvdec: " << yesNo(capabilities.nvidia_device) << '\n';
    out << "  software decoders: avdec_h264 " << yesNo(capabilities.gst_avdec_h264)
        << ", avdec_h265 " << yesNo(capabilities.gst_avdec_h265) << '\n';
    out << "  OpenCV GStreamer: " << yesNo(capabilities.opencv_gstreamer) << '\n';
    out << "  OpenCV FFmpeg: " << yesNo(capabilities.opencv_ffmpeg) << '\n';
    out << "  hardware decoding: "
        << (hardwareDecodingAvailable(capabilities) ? "available" : "NOT AVAILABLE (CPU decoding)")
        << '\n';
    for (const std::string& note : capabilities.notes) {
        out << "  note: " << note << '\n';
    }
    return out.str();
}

bool statusIsFresh(const StatusSnapshot& snapshot, std::int64_t now_unix_ms,
                   std::int64_t status_interval_ms) {
    if (snapshot.updated_unix_ms <= 0) {
        return false;
    }
    const std::int64_t window = 3 * std::max<std::int64_t>(1, status_interval_ms);
    const std::int64_t age = now_unix_ms - snapshot.updated_unix_ms;
    // A small step of the wall clock either way (NTP over the modem) must not hide a running
    // instance, which rewrites the file every interval anyway.
    return age <= window && age >= -window;
}

int statusExitCode(const StatusSnapshot& snapshot) {
    bool any = false;
    for (const CameraStatus& camera : snapshot.cameras) {
        // Only a camera switched off in the config is fine; one that gave up (a rejected login)
        // carries its own error code and still counts as a failure.
        const bool disabled_by_config =
            camera.anpr == "DISABLED" &&
            (camera.error.empty() || camera.error == toString(CameraError::kCameraDisabled));
        if (disabled_by_config) {
            continue;
        }
        any = true;
        if (camera.anpr != "RUNNING") {
            return 3;
        }
    }
    return any ? 0 : 3;
}

std::string rtspStatusWord(CameraError error) {
    switch (error) {
        case CameraError::kNone:
            return "OK";
        case CameraError::kRtspAuthFailed:
            return "AUTH";
        case CameraError::kRtspCredentialsMissing:
            return "NOCRED";
        case CameraError::kRtspPortClosed:
            return "PORT";
        case CameraError::kRtspStreamPathInvalid:
            return "PATH";
        case CameraError::kCameraUnreachable:
            return "DOWN";
        default:
            return "ERROR";
    }
}

std::string rtspStartupWord(CameraError error) {
    if (error == CameraError::kNone) {
        return "OK";
    }
    const std::string code = toString(error);
    const std::string prefix = "RTSP_";
    return code.compare(0, prefix.size(), prefix) == 0 ? code.substr(prefix.size()) : code;
}

std::string anprStartupText(CameraError error, const CaptureSettings& capture) {
    if (error == CameraError::kNone) {
        return "RUNNING";
    }
    switch (classifyFailure(error)) {
        case FailureClass::kAuthentication:
            if (capture.auth_max_retries <= 0) {
                return "ERROR (login not retried: capture.auth_max_retries is 0)";
            }
            return "WAITING (retry in " + formatRetryDelay(capture.auth_retry_interval_ms) + ")";
        case FailureClass::kConfiguration:
            return "WAITING (retry in " +
                   formatRetryDelay(capture.configuration_retry_interval_ms) + ")";
        case FailureClass::kTransient:
            break;
    }
    return "RECONNECTING";
}

CameraError blockingDiscoveryProblem(const CameraTarget& target) {
    for (const CameraError error :
         {CameraError::kCameraOnOtherSubnet, CameraError::kDuplicateIpDetected,
          CameraError::kCameraNotActivated}) {
        for (const auto& problem : target.discovery_problems) {
            if (problem.first == error) {
                return error;
            }
        }
    }
    return CameraError::kNone;
}

std::string formatVideoSummary(net::VideoCodec codec, int width, int height, double fps,
                               const std::string& decoder) {
    std::vector<std::string> parts;
    if (codec != net::VideoCodec::kUnknown) {
        parts.push_back(net::toString(codec));
    }
    if (width > 0 && height > 0) {
        parts.push_back(std::to_string(width) + "x" + std::to_string(height));
    }
    if (fps > 0.0) {
        std::ostringstream rate;
        rate << std::fixed << std::setprecision(1) << fps << " fps";
        parts.push_back(rate.str());
    }
    std::string text = join(parts, " ");
    if (!decoder.empty()) {
        text += (text.empty() ? "decoder=" : ", decoder=") + decoder;
    }
    return text.empty() ? "pending" : text;
}

std::string formatRetryDelay(std::int64_t delay_ms) {
    const std::int64_t ms = std::max<std::int64_t>(0, delay_ms);
    constexpr std::int64_t kMinute = 60000;
    constexpr std::int64_t kHour = 60 * kMinute;
    if (ms >= kHour && ms % kHour == 0) {
        return std::to_string(ms / kHour) + " h";
    }
    if (ms >= kMinute && ms % kMinute == 0) {
        return std::to_string(ms / kMinute) + " min";
    }
    if (ms < 10 * kMinute) {
        return std::to_string((ms + 999) / 1000) + " s";
    }
    return std::to_string((ms + kMinute / 2) / kMinute) + " min";
}

std::string formatStartupReport(const std::vector<StartupCameraLine>& cameras,
                                std::size_t active) {
    std::ostringstream out;
    out << "DISCOVERED CAMERAS: " << cameras.size() << '\n';
    for (const StartupCameraLine& camera : cameras) {
        out << '\n'
            << camera.id << '\n'
            << "  Model: " << (camera.model.empty() ? "unknown" : camera.model) << '\n'
            << "  IP: " << orDash(camera.ip) << '\n'
            << "  RTSP: " << orDash(camera.rtsp) << '\n'
            << "  VIDEO: " << orDash(camera.video) << '\n'
            << "  ANPR: " << orDash(camera.anpr) << '\n';
    }
    out << '\n' << "ACTIVE CAMERAS: " << active << '/' << cameras.size() << '\n';
    return out.str();
}

namespace {

bool sameDetector(const DetectorConfig& lhs, const DetectorConfig& rhs) {
    return lhs.model == rhs.model && lhs.input_size == rhs.input_size &&
           lhs.confidence_threshold == rhs.confidence_threshold &&
           lhs.nms_threshold == rhs.nms_threshold && lhs.letterbox_pad == rhs.letterbox_pad &&
           lhs.swap_rb == rhs.swap_rb && lhs.interval_idle_ms == rhs.interval_idle_ms &&
           lhs.interval_approaching_ms == rhs.interval_approaching_ms &&
           lhs.interval_near_ms == rhs.interval_near_ms &&
           lhs.interval_recognition_ms == rhs.interval_recognition_ms &&
           lhs.interval_cooldown_ms == rhs.interval_cooldown_ms &&
           lhs.require_motion_in_idle == rhs.require_motion_in_idle;
}

bool sameOcr(const OcrConfig& lhs, const OcrConfig& rhs) {
    return lhs.model == rhs.model && lhs.min_confidence == rhs.min_confidence &&
           lhs.min_char_confidence == rhs.min_char_confidence &&
           lhs.max_attempts == rhs.max_attempts;
}

bool sameInference(const InferenceConfig& lhs, const InferenceConfig& rhs) {
    return lhs.backend == rhs.backend && lhs.device_id == rhs.device_id && lhs.fp16 == rhs.fp16 &&
           lhs.engine_cache_dir == rhs.engine_cache_dir &&
           lhs.strict_backend == rhs.strict_backend &&
           lhs.intra_op_threads == rhs.intra_op_threads &&
           lhs.inter_op_threads == rhs.inter_op_threads;
}

}  // namespace

AnprConfig cameraAnprConfig(const AnprConfig& base, const AnprConfig* profile,
                            const CameraTarget& target, const CameraModeConfig& config,
                            std::vector<std::string>& replaced_sections) {
    replaced_sections.clear();
    AnprConfig result = profile != nullptr ? *profile : base;
    if (profile != nullptr) {
        if (!sameDetector(profile->detector, base.detector)) {
            replaced_sections.emplace_back("detector");
        }
        if (!sameOcr(profile->ocr, base.ocr)) {
            replaced_sections.emplace_back("ocr");
        }
        if (!sameInference(profile->inference, base.inference)) {
            replaced_sections.emplace_back("inference");
        }
        result.detector = base.detector;
        result.ocr = base.ocr;
        result.inference = base.inference;
        // One process, one log level.
        result.logging = base.logging;
    }

    CameraConfig& camera = result.camera;
    camera.camera_id = target.id;
    camera.kind = CameraKind::kRtsp;
    // Only ever the redacted URL: the source itself holds the credentials.
    camera.source = target.redactedUrl();
    camera.rtsp_tcp = true;
    camera.capture_buffer_size = 1;
    camera.read_timeout_ms = config.capture.read_timeout_ms;
    camera.reconnect_initial_backoff_ms = config.capture.reconnect_initial_backoff_ms;
    camera.reconnect_max_backoff_ms = config.capture.reconnect_max_backoff_ms;
    camera.loop_file = false;
    camera.process_every_file_frame = false;
    return result;
}

std::string ffmpegCaptureOptions(std::int64_t read_timeout_ms) {
    const std::int64_t timeout_us = std::max<std::int64_t>(1, read_timeout_ms) * 1000;
    return "rtsp_transport;tcp|stimeout;" + std::to_string(timeout_us);
}

int isapiStreamingChannel(const CameraTarget& target) {
    const int fallback = target.stream == StreamSelection::kSub ? 102 : 101;
    std::string path = target.rtsp_path;
    while (!path.empty() && path.back() == '/') {
        path.pop_back();
    }
    std::size_t begin = path.size();
    while (begin > 0 && std::isdigit(static_cast<unsigned char>(path[begin - 1])) != 0) {
        --begin;
    }
    const std::size_t digits = path.size() - begin;
    if (digits == 0 || digits > 6) {
        return fallback;
    }
    const std::string marker = "channels/";
    if (begin < marker.size()) {
        return fallback;
    }
    std::string preceding = path.substr(begin - marker.size(), marker.size());
    std::transform(preceding.begin(), preceding.end(), preceding.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (preceding != marker) {
        return fallback;
    }
    return std::stoi(path.substr(begin));
}

// ---- Commands -----------------------------------------------------------------------------------

int runCameraScan(const CameraCommandOptions& options) {
    CameraModeConfig config;
    if (!loadCameraConfig(options, config)) {
        return 2;
    }
    std::ostream& out = std::cout;
    const NetworkState state = inspectNetwork(config);
    reportNetwork(state, config, out);
    if (cameraLanMissing(state.lan)) {
        return 3;
    }

    const std::shared_ptr<CameraRegistry> registry = loadRegistry(config.discovery.registry_file);
    const DiscoveryReport report = discoverAndSave(state.snapshot, state.lan, config, *registry);
    out << '\n' << formatDiscoveryReport(report);
    if (state.lan.status == net::CameraLanStatus::kNoIpv4) {
        out << '\n' << formatAddressSuggestion(report.cameras, state.lan.interface);
    }
    out << std::flush;
    return camerasOnCameraSubnet(report) > 0 ? 0 : 3;
}

int runCameraCheck(const CameraCommandOptions& options) {
    CameraModeConfig config;
    if (!loadCameraConfig(options, config)) {
        return 2;
    }
    std::ostream& out = std::cout;
    const NetworkState state = inspectNetwork(config);
    reportNetwork(state, config, out);
    if (cameraLanMissing(state.lan)) {
        return 3;
    }

    const DecoderCapabilities capabilities = detectDecoderCapabilities();
    out << '\n' << formatDecoderCapabilities(capabilities);
    reportHardwareDecoder(capabilities, config, out);

    const std::shared_ptr<CameraRegistry> registry = loadRegistry(config.discovery.registry_file);
    const DiscoveryReport report = discoverAndSave(state.snapshot, state.lan, config, *registry);
    out << '\n' << formatDiscoveryReport(report);
    if (state.lan.status == net::CameraLanStatus::kNoIpv4) {
        out << '\n' << formatAddressSuggestion(report.cameras, state.lan.interface);
    }
    out << std::flush;

    const std::vector<CameraTarget> targets =
        buildTargets(report, config, processEnvironment());
    std::size_t enabled = 0;
    std::size_t ready = 0;
    int slots = 0;
    for (const CameraTarget& target : targets) {
        out << '\n';
        if (!target.enabled) {
            out << cameraHeader(target)
                << indent(formatError(CameraError::kCameraDisabled,
                                      cameraContext(target, Stage::kDiscovery), ""),
                          "  ")
                << "  STATUS: DISABLED\n"
                << std::flush;
            continue;
        }
        ++enabled;
        const CameraError blocking = blockingDiscoveryProblem(target);
        if (blocking != CameraError::kNone) {
            // Never send credentials to a camera discovery already ruled out: behind a
            // duplicate IP they could reach the wrong device.
            out << cameraHeader(target)
                << indent(formatError(blocking, cameraContext(target, Stage::kDiscovery),
                                      discoveryDetail(target, blocking)),
                          "  ")
                << "  STATUS: NOT READY\n"
                << std::flush;
            continue;
        }

        const StreamCheckResult result =
            checkStream(target, config, capabilities, state.snapshot, state.lan,
                        config.capture.check_duration_ms);
        std::string extra = credentialsLine(target, config);
        if (result.rtsp.auth_ok && target.has_credentials) {
            // ISAPI only after RTSP accepted this very login, and once per camera.
            const hikvision::IsapiProbe probe =
                hikvision::isapiProbe(target.ip, target.http_port, target.credentials,
                                      isapiStreamingChannel(target), config.rtsp.timeout_ms);
            extra += formatIsapiLines(target, probe);
        }
        ++slots;
        if (slots > config.runtime.max_active_cameras) {
            extra += indent(formatError(CameraError::kCameraLimitReached,
                                        cameraContext(target, Stage::kDiscovery),
                                        "runtime.max_active_cameras is " +
                                            std::to_string(config.runtime.max_active_cameras) +
                                            "; run-cameras keeps this camera on standby"),
                            "  ");
        }
        out << insertBeforeStatus(formatStreamCheck(target, result), extra) << std::flush;
        if (result.ready()) {
            ++ready;
        }
    }
    out << "\nREADY CAMERAS: " << ready << '/' << enabled << '\n' << std::flush;
    return enabled >= 1 && ready == enabled ? 0 : 3;
}

int runCameraStatus(const CameraCommandOptions& options) {
    CameraModeConfig config;
    if (!loadCameraConfig(options, config)) {
        return 2;
    }
    std::ostream& out = std::cout;
    StatusSnapshot running;
    std::string error;
    if (readStatusFile(config.runtime.status_file, running, error) &&
        statusIsFresh(running, unixMs(), config.runtime.status_interval_ms)) {
        out << formatStatusTable(running) << std::flush;
        return statusExitCode(running);
    }
    if (!error.empty()) {
        logEvent(LogLevel::kDebug, "status_file_unusable",
                 LogFields()
                     .addQuoted("file", config.runtime.status_file)
                     .addQuoted("reason", error));
    }

    out << "No running camera-mode ANPR (status file missing or stale); probing cameras...\n\n"
        << std::flush;
    const NetworkState state = inspectNetwork(config);
    reportNetwork(state, config, out);

    StatusSnapshot probe;
    probe.updated_at = utcNowIso();
    probe.updated_unix_ms = unixMs();
    probe.pid = static_cast<int>(::getpid());
    probe.camera_lan = cameraLanSummary(state.lan);
    probe.internet = internetSummary(state.internet);

    const std::shared_ptr<CameraRegistry> registry = loadRegistry(config.discovery.registry_file);
    std::set<std::string> listed;
    if (!cameraLanMissing(state.lan)) {
        const DiscoveryReport report =
            discoverAndSave(state.snapshot, state.lan, config, *registry);
        if (state.lan.status == net::CameraLanStatus::kNoIpv4) {
            out << '\n' << formatAddressSuggestion(report.cameras, state.lan.interface);
        }
        const std::vector<CameraTarget> targets =
            buildTargets(report, config, processEnvironment());
        std::vector<std::future<RtspCheck>> checks(targets.size());
        for (std::size_t index = 0; index < targets.size(); ++index) {
            const CameraTarget& target = targets[index];
            if (target.enabled && blockingDiscoveryProblem(target) == CameraError::kNone) {
                const net::NetworkSnapshot& snapshot = state.snapshot;
                const net::CameraLanSelection& lan = state.lan;
                const int timeout_ms = config.rtsp.timeout_ms;
                checks[index] = runAsync([&target, &snapshot, &lan, timeout_ms] {
                    return checkRtsp(target, snapshot, lan, timeout_ms);
                });
            }
        }
        for (std::size_t index = 0; index < targets.size(); ++index) {
            const CameraTarget& target = targets[index];
            CameraStatus row = baseStatus(target);
            row.anpr = "STOPPED";
            const CameraError blocking = blockingDiscoveryProblem(target);
            if (!target.enabled) {
                row.anpr = "DISABLED";
                setError(row, CameraError::kCameraDisabled, "");
            } else if (blocking != CameraError::kNone) {
                row.link = "OK";
                setError(row, blocking, discoveryDetail(target, blocking));
            } else {
                const RtspCheck rtsp = checks[index].get();
                row.link = rtsp.network_reachable || rtsp.rtsp_port_open ||
                                   rtsp.error == CameraError::kNone
                               ? "OK"
                               : "DOWN";
                row.rtsp = rtspStatusWord(rtsp.error);
                if (rtsp.error != CameraError::kNone) {
                    setError(row, rtsp.error, rtsp.detail);
                }
            }
            listed.insert(row.id);
            probe.cameras.push_back(std::move(row));
        }
    }

    // Registered cameras that did not answer at all.
    for (const RegistryEntry& entry : registry->entries()) {
        if (listed.count(entry.id) != 0) {
            continue;
        }
        CameraStatus row = missingCameraStatus(entry, config);
        if (row.anpr != "DISABLED") {
            row.anpr = "STOPPED";
            if (cameraLanMissing(state.lan)) {
                row.link = "-";
                setError(row, CameraError::kCameraLanNotFound, row.detail);
            }
        }
        probe.cameras.push_back(std::move(row));
    }
    std::sort(probe.cameras.begin(), probe.cameras.end(),
              [](const CameraStatus& lhs, const CameraStatus& rhs) { return lhs.id < rhs.id; });

    out << '\n' << formatStatusTable(probe) << std::flush;
    return statusExitCode(probe);
}

int runCameras(const CameraCommandOptions& options, const AnprConfig& anpr_config,
               std::shared_ptr<PlateSink> sink, std::atomic_bool& stop, bool warmup_only) {
    CameraModeConfig config;
    if (!loadCameraConfig(options, config)) {
        return 2;
    }
    std::map<std::string, AnprConfig> profiles;
    if (!loadCameraProfiles(config, profiles)) {
        return 2;
    }
    std::ostream& out = std::cerr;

    MultiCameraRunner::Options runner_options;
    runner_options.status_file = config.runtime.status_file;
    runner_options.status_interval_ms = config.runtime.status_interval_ms;
    runner_options.metrics_interval_ms = config.runtime.metrics_interval_ms;
    runner_options.min_available_ram_mb = config.runtime.min_available_ram_mb;
    runner_options.exit_when_all_finished = false;

    if (warmup_only) {
        // Builds and caches the engines only: no camera, no network wait, and never the status
        // file of a service that may be running next to it.
        runner_options.status_file.clear();
        warnIfLowMemory(config);
        MultiCameraRunner runner(anpr_config, runner_options, sink);
        std::string error;
        if (!runner.loadModels(error)) {
            reportModelFailure(error, out);
            return 4;
        }
        logEvent(LogLevel::kInfo, "warmup_complete",
                 LogFields()
                     .add("detector_backend", runner.detectorBackend())
                     .add("ocr_backend", runner.ocrBackend()));
        return 0;
    }

    const std::shared_ptr<CameraRegistry> registry = loadRegistry(config.discovery.registry_file);
    NetworkState network = inspectNetwork(config);
    reportNetwork(network, config, out);
    if (!network.lan.ready() && !waitForCameraLan(config, network, *registry, stop, out)) {
        logEvent(LogLevel::kInfo, "shutdown",
                 LogFields().add("reason", "stopped_before_camera_lan"));
        return 0;
    }

    const DiscoveryReport report =
        discoverAndSave(network.snapshot, network.lan, config, *registry);
    const DecoderCapabilities capabilities = detectDecoderCapabilities();
    reportHardwareDecoder(capabilities, config, out);
    const EnvLookup env = processEnvironment();
    const std::vector<CameraTarget> targets = buildTargets(report, config, env);
    if (stop.load()) {
        return 0;
    }

    warnIfLowMemory(config);
    runner_options.camera_lan_summary = cameraLanSummary(network.lan);
    runner_options.internet_summary = internetSummary(network.internet);
    MultiCameraRunner runner(anpr_config, runner_options, sink);
    std::string error;
    if (!runner.loadModels(error)) {
        reportModelFailure(error, out);
        return 4;
    }
    if (stop.load()) {
        return 0;
    }

    CameraFleet fleet(config, anpr_config, std::move(profiles), capabilities, env, runner,
                      registry, network);
    fleet.logDiscoveryProblems(report);
    std::vector<StartupCameraLine> lines = fleet.admitAtStartup(targets);
    fleet.waitForFirstFrames(lines, stop);
    out << '\n' << formatStartupReport(lines, runner.activeCameraCount()) << std::flush;

    if (!fleet.hasWork()) {
        out << "\nNo camera can be processed and discovery.rescan_interval_ms is 0, so none can "
               "appear later.\n"
            << std::flush;
        runner.stopAll();
        return 3;
    }

    runner.run(stop, [&fleet, &stop] { fleet.periodic(stop); });
    runner.stopAll();
    fleet.finish();
    logEvent(LogLevel::kInfo, "shutdown",
             LogFields()
                 .add("cameras", runner.activeCameraCount())
                 .add("frames_processed", runner.framesProcessed())
                 .add("plates_confirmed", runner.platesConfirmed()));
    return 0;
}

int runSources(const std::vector<std::string>& sources, const AnprConfig& anpr_config,
               std::shared_ptr<PlateSink> sink, std::atomic_bool& stop, bool warmup_only) {
    MultiCameraRunner::Options options;
    options.metrics_interval_ms = anpr_config.performance.metrics_interval_ms;
    options.exit_when_all_finished = true;
    MultiCameraRunner runner(anpr_config, options, std::move(sink));

    // Models first, then the sources, as the separate-pipeline path did: a missing model is
    // reported (exit 4) before a mistyped source (exit 3).
    std::string error;
    if (!runner.loadModels(error)) {
        logEvent(LogLevel::kError, "model_load_failed", LogFields().add("reason", error));
        return 4;
    }

    std::vector<CameraWorkerSpec> specs;
    specs.reserve(sources.size());
    for (std::size_t index = 0; index < sources.size(); ++index) {
        CameraWorkerSpec spec;
        spec.anpr = anpr_config;
        spec.anpr.camera.source = sources[index];
        spec.anpr.camera.kind = CameraKind::kAuto;
        spec.anpr.camera.camera_id =
            anpr_config.camera.camera_id + "-" + std::to_string(index + 1);
        spec.camera_id = spec.anpr.camera.camera_id;
        spec.source = makeCameraSource(spec.anpr.camera, error);
        if (spec.source == nullptr) {
            logEvent(LogLevel::kError, "camera_unavailable",
                     LogFields().add("camera_id", spec.camera_id).add("reason", error));
            return 3;
        }
        spec.live = spec.source->reconnectable();
        spec.queue_capacity = 1;
        spec.max_frame_age_ms = 0;
        specs.push_back(std::move(spec));
    }
    if (warmup_only) {
        return 0;
    }

    for (CameraWorkerSpec& spec : specs) {
        const std::string camera_id = spec.camera_id;
        if (!runner.addCamera(std::move(spec), error)) {
            logEvent(LogLevel::kError, "camera_unavailable",
                     LogFields().add("camera_id", camera_id).add("reason", error));
            runner.stopAll();
            return 3;
        }
    }

    runner.run(stop);
    runner.stopAll();
    logEvent(LogLevel::kInfo, "shutdown",
             LogFields()
                 .add("streams", sources.size())
                 .add("frames_processed", runner.framesProcessed())
                 .add("plates_confirmed", runner.platesConfirmed()));
    return 0;
}

}  // namespace anpr::cameras
