#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "anpr/net/http_auth.hpp"

namespace anpr::cameras {

/// Configuration of camera mode (config/cameras.yaml): how the camera LAN is found, how cameras
/// are discovered and identified, which stream is read and how, and how failures are retried.
/// The recognition thresholds stay in the ANPR profile (config/jetson-nano.yaml).
///
/// Secrets are never part of this file. Credentials come from the environment: by default
/// HIKVISION_USERNAME / HIKVISION_PASSWORD for every camera, overridden per camera by
/// HIKVISION_USERNAME_CAMERA_02 / HIKVISION_PASSWORD_CAMERA_02 (camera id upper-cased, '-' as '_')
/// or by the env variable names given in a camera's override.

enum class StreamSelection { kMain, kSub };
std::string toString(StreamSelection stream);

enum class DecoderPreference {
    /// NVIDIA hardware decoding when this container can do it, otherwise software.
    kAuto,
    /// Only nvv4l2decoder. A camera whose stream it cannot open is reported, not degraded.
    kNvidiaHardware,
    /// Only CPU decoding (FFmpeg or GStreamer software decoders).
    kSoftware,
};
std::string toString(DecoderPreference decoder);

enum class ScanMode {
    /// Scan the subnet only when SADP and ONVIF found nothing, or found fewer cameras than the
    /// registry remembers.
    kAuto,
    kAlways,
    kNever,
};
std::string toString(ScanMode mode);

struct NetworkSettings {
    /// Interface wired to the PoE switch, or "auto".
    std::string interface{"auto"};
    bool internet_check{true};
    std::vector<std::string> internet_probe_targets{"1.1.1.1:53", "8.8.8.8:53"};
    int internet_timeout_ms{1500};
};

struct DiscoverySettings {
    /// Off: only `manual_hosts` and cameras listed under `cameras` with an `ip` are used.
    bool enabled{true};
    bool sadp{true};
    bool onvif{true};
    bool arp_table{true};
    ScanMode subnet_scan{ScanMode::kAuto};
    /// Subnets larger than this many hosts are not swept.
    int scan_max_hosts{1024};
    std::vector<int> scan_ports{554, 80, 8000};
    int listen_ms{2500};
    int connect_timeout_ms{400};
    /// Always probed, discovered or not: "192.168.10.21" or "192.168.10.21:8554" (RTSP port).
    std::vector<std::string> manual_hosts;
    /// More cameras than this are listed but ignored.
    int max_cameras{16};
    /// Non-secret stable identities (id <-> MAC / serial), rewritten by every scan.
    std::string registry_file{"var/cameras/registry.yaml"};
    bool duplicate_ip_check{true};
    /// While running, look for cameras that appeared later (0 disables).
    std::int64_t rescan_interval_ms{300000};
};

struct RtspSettings {
    /// RTSP port used when a camera override does not set one.
    int port{554};
    StreamSelection stream{StreamSelection::kMain};
    std::string main_path{"/Streaming/Channels/101"};
    std::string sub_path{"/Streaming/Channels/102"};
    std::string username_env{"HIKVISION_USERNAME"};
    std::string password_env{"HIKVISION_PASSWORD"};
    int timeout_ms{4000};
    /// rtspsrc jitter-buffer latency.
    int latency_ms{200};
};

struct DecodeSettings {
    DecoderPreference decoder{DecoderPreference::kAuto};
    /// Hardware down-scaling of wider streams before conversion to BGR (0 keeps the size).
    int max_width{0};
    /// Frame rate cap applied before colour conversion (0 keeps the camera's rate).
    int max_fps{0};
};

struct CaptureSettings {
    /// Bounded latest-frames queue between capture and processing; the oldest frame is dropped.
    int queue_size{1};
    std::int64_t first_frame_timeout_ms{10000};
    std::int64_t read_timeout_ms{5000};
    /// A frame older than this when processing picks it up is discarded as stale.
    std::int64_t max_frame_age_ms{1500};
    std::int64_t reconnect_initial_backoff_ms{1000};
    std::int64_t reconnect_max_backoff_ms{30000};
    std::int64_t auth_retry_interval_ms{900000};
    int auth_max_retries{2};
    std::int64_t configuration_retry_interval_ms{120000};
    /// How long `camera-check` reads frames from each camera.
    std::int64_t check_duration_ms{3000};
};

struct RuntimeSettings {
    /// Cameras beyond this are discovered and reported but not processed (CAMERA_LIMIT_REACHED).
    int max_active_cameras{4};
    std::string status_file{"var/cameras/status.json"};
    std::int64_t status_interval_ms{5000};
    /// Per-camera and system summary lines.
    std::int64_t metrics_interval_ms{60000};
    /// Below this much available RAM the run logs OUT_OF_MEMORY_RISK.
    int min_available_ram_mb{300};
};

struct OnvifSettings {
    /// ONVIF accounts are separate on Hikvision. GetDeviceInformation is only called when these
    /// variables are set; otherwise ONVIF is probed without credentials.
    std::string username_env{"ONVIF_USERNAME"};
    std::string password_env{"ONVIF_PASSWORD"};
};

struct NativeAnprSettings {
    /// Read the camera's own ANPR results (ISAPI alertStream) as an extra `hikvision_anpr` event.
    bool enabled{false};
};

/// Per-camera settings. Matched to a discovered camera by `id`, then `mac`, `serial`, `ip`.
struct CameraOverride {
    std::string id;
    std::string mac;
    std::string serial;
    std::string ip;
    std::optional<bool> enabled;
    std::optional<int> rtsp_port;
    std::optional<StreamSelection> stream;
    std::string rtsp_path;
    std::optional<int> http_port;
    std::string username_env;
    std::string password_env;
    std::optional<DecoderPreference> decoder;
    /// Another ANPR profile for this camera's recognition settings (ROIs, thresholds). Model and
    /// inference settings always come from the shared profile.
    std::string anpr_config;
    std::optional<bool> native_anpr;
};

struct CameraModeConfig {
    NetworkSettings network;
    DiscoverySettings discovery;
    RtspSettings rtsp;
    DecodeSettings decode;
    CaptureSettings capture;
    RuntimeSettings runtime;
    OnvifSettings onvif;
    NativeAnprSettings native_anpr;
    std::vector<CameraOverride> cameras;
};

struct CameraModeConfigResult {
    CameraModeConfig config;
    bool ok{true};
    std::string error;
    std::vector<std::string> unknown_keys;
};

CameraModeConfigResult loadCameraModeConfigText(const std::string& text);
CameraModeConfigResult loadCameraModeConfigFile(const std::string& path);
bool validateCameraModeConfig(const CameraModeConfig& config, std::string& error);

/// Reads one environment variable. Injected so tests never depend on the real environment.
using EnvLookup = std::function<std::optional<std::string>(const std::string& name)>;
EnvLookup processEnvironment();

/// "camera-02" -> "CAMERA_02".
std::string envSuffix(const std::string& camera_id);

struct ResolvedCredentials {
    net::Credentials credentials;
    bool present{false};
    /// Names of the variables used ("HIKVISION_USERNAME_CAMERA_02/HIKVISION_PASSWORD_CAMERA_02").
    /// Names only: values are never reported.
    std::string source;
};

/// Per-camera override env names, then HIKVISION_*_<ID>, then the global names.
ResolvedCredentials resolveCredentials(const CameraModeConfig& config,
                                       const CameraOverride* camera_override,
                                       const std::string& camera_id, const EnvLookup& env);

/// ONVIF credentials, or nullopt when not configured.
std::optional<net::Credentials> resolveOnvifCredentials(const CameraModeConfig& config,
                                                        const EnvLookup& env);

/// The override for a camera, or nullptr. `mac` must be normalized.
const CameraOverride* findOverride(const CameraModeConfig& config, const std::string& id,
                                   const std::string& mac, const std::string& serial,
                                   const std::string& ip);

}  // namespace anpr::cameras
