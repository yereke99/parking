#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "anpr/cameras/camera_mode_config.hpp"
#include "anpr/cameras/diagnostics.hpp"
#include "anpr/cameras/discovery.hpp"
#include "anpr/cameras/gst_pipeline.hpp"
#include "anpr/cameras/rtsp_preflight.hpp"
#include "anpr/cameras/status_store.hpp"
#include "anpr/common/config.hpp"
#include "anpr/net/network_topology.hpp"
#include "anpr/net/sdp.hpp"
#include "anpr/pipeline/plate_sink.hpp"

namespace anpr::cameras {

/// The installer-facing commands behind `make camera-scan`, `camera-check`, `camera-status` and
/// `run-cameras`. Reports go to stdout for the read-only commands and to stderr while running
/// (stdout then carries only JSON events).
///
/// Exit codes follow kz_anpr: 0 success, 2 configuration error, 3 camera problem (no camera LAN,
/// no cameras, a camera that is not ready), 4 model or inference backend unavailable.
struct CameraCommandOptions {
    std::string camera_config_path{"config/cameras.yaml"};
};

/// Network topology + discovery only. No credentials are used. Updates the registry.
int runCameraScan(const CameraCommandOptions& options);

/// Discovery, then per camera: RTSP authentication and path, codec, decoder opened in this
/// container, frames read, first-frame latency and FPS. Prints one block per camera and exit 0
/// only when every enabled camera is READY.
int runCameraCheck(const CameraCommandOptions& options);

/// The status table of a running `run-cameras` (from its status file) or, when none is running,
/// a quick reachability/RTSP/authentication probe of the registered cameras.
int runCameraStatus(const CameraCommandOptions& options);

/// Discover, preflight, start ANPR on every healthy camera with one shared detector and OCR,
/// keep failed cameras retrying in the background, rediscover periodically.
int runCameras(const CameraCommandOptions& options, const AnprConfig& anpr_config,
               std::shared_ptr<PlateSink> sink, std::atomic_bool& stop, bool warmup_only);

/// Several `--source` arguments (files, URLs, devices) on the same shared-model runner. Camera
/// ids are `<camera_id>-1`, `<camera_id>-2`, ... as before.
int runSources(const std::vector<std::string>& sources, const AnprConfig& anpr_config,
               std::shared_ptr<PlateSink> sink, std::atomic_bool& stop, bool warmup_only);

// ---- Building blocks of the commands, exposed for the unit tests --------------------------------

/// No wired interface with link, or the configured one is missing: CAMERA_LAN_NOT_FOUND.
[[nodiscard]] bool cameraLanMissing(const net::CameraLanSelection& lan);

/// One camera-LAN or Internet finding, reported as a terminal block and a log line.
struct NetworkProblem {
    CameraError error{CameraError::kNone};
    DiagnosticContext context;
    std::string detail;
};

/// The camera LAN problems first, then the Internet ones (informational: camera processing never
/// depends on the uplink).
std::vector<NetworkProblem> networkProblems(const net::CameraLanSelection& lan,
                                            const net::InternetStatus& internet,
                                            const std::vector<std::string>& overlaps,
                                            const NetworkSettings& settings);

/// "eth0 192.168.10.5/24", or the interface with the error code
/// ("eth0 CAMERA_SUBNET_UNCONFIGURED").
std::string cameraLanSummary(const net::CameraLanSelection& lan);
/// "wwan0 ONLINE", "wwan0 OFFLINE", "wwan0 NOT CHECKED" or "NO_INTERNET_ROUTE".
std::string internetSummary(const net::InternetStatus& internet);

/// An unused address for the Jetson in the subnet most cameras answered SADP from (any
/// discovered camera when none came from SADP), host .200 first: 192.168.1.200/24 for a
/// factory-default camera at 192.168.1.64. nullopt when no camera has an address or the subnet
/// is full.
std::optional<net::Ipv4Network> suggestCameraLanAddress(
    const std::vector<DiscoveredCamera>& cameras);
/// The SUGGESTED CAMERA LAN ADDRESS block with the `make camera-lan-setup` command line.
std::string formatAddressSuggestion(const std::vector<DiscoveredCamera>& cameras,
                                    const std::string& interface);

/// nvv4l2decoder usable here: plugin, decoder device node, rtspsrc and a GStreamer capture path.
[[nodiscard]] bool hardwareDecodingAvailable(const DecoderCapabilities& capabilities);
/// What is missing for hardware decoding, one item each; empty when available.
std::vector<std::string> missingHardwareDecoding(const DecoderCapabilities& capabilities);
/// The DECODER CAPABILITIES block of `camera-check`.
std::string formatDecoderCapabilities(const DecoderCapabilities& capabilities);

/// The status file of a running instance was written within 3 x the status interval of `now`.
[[nodiscard]] bool statusIsFresh(const StatusSnapshot& snapshot, std::int64_t now_unix_ms,
                                 std::int64_t status_interval_ms);
/// 0 when at least one camera is not disabled in the config and every such camera is RUNNING,
/// else 3. A DISABLED row with another error code (a camera that gave up) counts as a failure.
int statusExitCode(const StatusSnapshot& snapshot);

/// The RTSP column of the status table: OK, AUTH, NOCRED, PORT, PATH, DOWN or ERROR.
std::string rtspStatusWord(CameraError error);
/// The RTSP line of the run-cameras startup block: OK, AUTH_FAILED, PORT_CLOSED, ... (the
/// diagnostic code without its RTSP_ prefix).
std::string rtspStartupWord(CameraError error);
/// The ANPR line of the startup block for a camera whose preflight gave `error`: RUNNING,
/// RECONNECTING, "WAITING (retry in 15 min)", ...
std::string anprStartupText(CameraError error, const CaptureSettings& capture);

/// A problem found by discovery that rules out contacting the camera now (another subnet, not
/// activated, duplicate IP); kNone when there is none.
CameraError blockingDiscoveryProblem(const CameraTarget& target);

/// "H.264 2688x1520 25.0 fps, decoder=nvidia_hardware"; "pending" when nothing is known yet.
std::string formatVideoSummary(net::VideoCodec codec, int width, int height, double fps,
                               const std::string& decoder);
/// "15 min", "2 min", "90 s", "1 h".
std::string formatRetryDelay(std::int64_t delay_ms);

struct StartupCameraLine {
    std::string id;
    std::string model;
    std::string ip;
    std::string rtsp;
    std::string video;
    std::string anpr;
};

/// The run-cameras startup block:
///   DISCOVERED CAMERAS: 4
///
///   camera-01
///     Model: DS-TCG406-E
///     IP: 192.168.10.21
///     RTSP: OK
///     VIDEO: H.264 2688x1520 25.0 fps, decoder=nvidia_hardware
///     ANPR: RUNNING
///   ...
///   ACTIVE CAMERAS: 3/4
std::string formatStartupReport(const std::vector<StartupCameraLine>& cameras,
                                std::size_t active);

/// One camera's recognition settings: `profile` (the camera's own anpr_config) or else `base`,
/// always with the detector, OCR and inference of `base` (the models are shared), and the capture
/// settings of camera mode. `replaced_sections` names the profile sections that differed from
/// the base and were replaced ("detector", "ocr", "inference").
AnprConfig cameraAnprConfig(const AnprConfig& base, const AnprConfig* profile,
                            const CameraTarget& target, const CameraModeConfig& config,
                            std::vector<std::string>& replaced_sections);

/// OPENCV_FFMPEG_CAPTURE_OPTIONS for camera mode: RTSP over TCP and a socket timeout, in the
/// FFmpeg 3.4 option names ("rtsp_transport;tcp|stimeout;5000000").
std::string ffmpegCaptureOptions(std::int64_t read_timeout_ms);

/// The ISAPI streaming channel of the camera's RTSP path (/Streaming/Channels/102 -> 102), else
/// 101 for the main and 102 for the sub stream.
int isapiStreamingChannel(const CameraTarget& target);

}  // namespace anpr::cameras
