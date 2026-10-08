#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "anpr/cameras/camera_mode_config.hpp"
#include "anpr/cameras/camera_registry.hpp"
#include "anpr/cameras/diagnostics.hpp"
#include "anpr/cameras/reconnect_policy.hpp"
#include "anpr/cameras/status_store.hpp"
#include "anpr/common/filesystem.hpp"
#include "anpr/common/logging.hpp"
#include "test_framework.hpp"

using namespace anpr::cameras;

namespace {

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

/// A fake environment that also records every variable name looked up.
struct FakeEnv {
    std::map<std::string, std::string> values;
    std::vector<std::string> lookups;

    EnvLookup lookup() {
        return [this](const std::string& name) -> std::optional<std::string> {
            lookups.push_back(name);
            const auto found = values.find(name);
            if (found == values.end()) {
                return std::nullopt;
            }
            return found->second;
        };
    }
};

/// Field-by-field comparison; returns the first differing path, empty when equal.
std::string firstDifference(const CameraModeConfig& a, const CameraModeConfig& b) {
#define ANPR_COMPARE(field)          \
    if (!(a.field == b.field)) {     \
        return #field;               \
    }
    ANPR_COMPARE(network.interface)
    ANPR_COMPARE(network.internet_check)
    ANPR_COMPARE(network.internet_probe_targets)
    ANPR_COMPARE(network.internet_timeout_ms)
    ANPR_COMPARE(discovery.enabled)
    ANPR_COMPARE(discovery.sadp)
    ANPR_COMPARE(discovery.onvif)
    ANPR_COMPARE(discovery.arp_table)
    ANPR_COMPARE(discovery.subnet_scan)
    ANPR_COMPARE(discovery.scan_max_hosts)
    ANPR_COMPARE(discovery.scan_ports)
    ANPR_COMPARE(discovery.listen_ms)
    ANPR_COMPARE(discovery.connect_timeout_ms)
    ANPR_COMPARE(discovery.manual_hosts)
    ANPR_COMPARE(discovery.max_cameras)
    ANPR_COMPARE(discovery.registry_file)
    ANPR_COMPARE(discovery.duplicate_ip_check)
    ANPR_COMPARE(discovery.rescan_interval_ms)
    ANPR_COMPARE(rtsp.port)
    ANPR_COMPARE(rtsp.stream)
    ANPR_COMPARE(rtsp.main_path)
    ANPR_COMPARE(rtsp.sub_path)
    ANPR_COMPARE(rtsp.username_env)
    ANPR_COMPARE(rtsp.password_env)
    ANPR_COMPARE(rtsp.timeout_ms)
    ANPR_COMPARE(rtsp.latency_ms)
    ANPR_COMPARE(decode.decoder)
    ANPR_COMPARE(decode.max_width)
    ANPR_COMPARE(decode.max_fps)
    ANPR_COMPARE(capture.queue_size)
    ANPR_COMPARE(capture.first_frame_timeout_ms)
    ANPR_COMPARE(capture.read_timeout_ms)
    ANPR_COMPARE(capture.max_frame_age_ms)
    ANPR_COMPARE(capture.reconnect_initial_backoff_ms)
    ANPR_COMPARE(capture.reconnect_max_backoff_ms)
    ANPR_COMPARE(capture.auth_retry_interval_ms)
    ANPR_COMPARE(capture.auth_max_retries)
    ANPR_COMPARE(capture.configuration_retry_interval_ms)
    ANPR_COMPARE(capture.check_duration_ms)
    ANPR_COMPARE(runtime.max_active_cameras)
    ANPR_COMPARE(runtime.status_file)
    ANPR_COMPARE(runtime.status_interval_ms)
    ANPR_COMPARE(runtime.metrics_interval_ms)
    ANPR_COMPARE(runtime.min_available_ram_mb)
    ANPR_COMPARE(onvif.username_env)
    ANPR_COMPARE(onvif.password_env)
    ANPR_COMPARE(native_anpr.enabled)
    ANPR_COMPARE(cameras.size())
#undef ANPR_COMPARE
    return {};
}

/// A unique, empty scratch directory under the system temp directory.
anpr::filesystem::path freshTempDir(const std::string& name) {
    const anpr::filesystem::path dir =
        anpr::filesystem::temp_directory_path() / ("kz_anpr_test_" + name);
    std::error_code ec;
    anpr::filesystem::remove_all(dir, ec);
    return dir;
}

/// File names in `dir`, sorted: proves an atomic write left no temporary file behind.
std::vector<std::string> directoryEntries(const anpr::filesystem::path& dir) {
    std::vector<std::string> names;
    for (const auto& entry : anpr::filesystem::directory_iterator(dir)) {
        names.push_back(entry.path().filename().string());
    }
    std::sort(names.begin(), names.end());
    return names;
}

/// Captures what the structured logger writes to stderr while alive.
class StderrCapture {
public:
    StderrCapture() : previous_(std::cerr.rdbuf(buffer_.rdbuf())) {
        previous_level_ = anpr::Logger::instance().level();
        anpr::Logger::instance().setLevel(anpr::LogLevel::kDebug);
    }
    ~StderrCapture() {
        std::cerr.rdbuf(previous_);
        anpr::Logger::instance().setLevel(previous_level_);
    }
    StderrCapture(const StderrCapture&) = delete;
    StderrCapture& operator=(const StderrCapture&) = delete;

    [[nodiscard]] std::string text() const { return buffer_.str(); }

private:
    std::ostringstream buffer_;
    std::streambuf* previous_;
    anpr::LogLevel previous_level_{anpr::LogLevel::kInfo};
};

const char* const kFullConfig = R"(
network:
  interface: eth0
  internet_check: false
  internet_probe_targets:
    - 9.9.9.9:53
    - "1.0.0.1:443"
  internet_timeout_ms: 900
discovery:
  enabled: false
  sadp: false
  onvif: false
  arp_table: false
  subnet_scan: always
  scan_max_hosts: 254
  scan_ports: [554, 8554]
  listen_ms: 1200
  connect_timeout_ms: 250
  manual_hosts: ["192.168.10.21", 192.168.10.22:8554]
  max_cameras: 6
  registry_file: /data/registry.yaml
  duplicate_ip_check: false
  rescan_interval_ms: 0
rtsp:
  port: 8554
  stream: sub
  main_path: /Streaming/Channels/201
  sub_path: /Streaming/Channels/202
  username_env: CAM_USER
  password_env: CAM_PASS
  timeout_ms: 2500
  latency_ms: 0
decode:
  decoder: nvidia_hardware
  max_width: 1920
  max_fps: 12
capture:
  queue_size: 2
  first_frame_timeout_ms: 8000
  read_timeout_ms: 3000
  max_frame_age_ms: 900
  reconnect_initial_backoff_ms: 500
  reconnect_max_backoff_ms: 20000
  auth_retry_interval_ms: 600000
  auth_max_retries: 0
  configuration_retry_interval_ms: 60000
  check_duration_ms: 5000
runtime:
  max_active_cameras: 2
  status_file: /data/status.json
  status_interval_ms: 2000
  metrics_interval_ms: 30000
  min_available_ram_mb: 0
onvif:
  username_env: ONVIF_USER
  password_env: ONVIF_PASS
native_anpr:
  enabled: true
cameras:
  - id: gate-in
    mac: 44-19-B6-01-02-03
    serial: DS-TCG406-E0001
    ip: 192.168.10.21
    enabled: false
    rtsp_port: 8554
    stream: sub
    rtsp_path: /Streaming/Channels/103
    http_port: 8080
    username_env: GATE_IN_USER
    password_env: GATE_IN_PASS
    decoder: software
    anpr_config: config/gate-in.yaml
    native_anpr: true
  - ip: 192.168.10.22
)";

}  // namespace

// ---------------------------------------------------------------------------------------------
// Camera mode configuration
// ---------------------------------------------------------------------------------------------

TEST("camera config: defaults are valid and match the documented values") {
    const CameraModeConfig config;
    std::string error;
    CHECK(validateCameraModeConfig(config, error));
    CHECK_EQ(error, std::string{});
    CHECK_EQ(config.network.interface, std::string("auto"));
    CHECK_EQ(config.network.internet_probe_targets.size(), std::size_t{2});
    CHECK_EQ(config.discovery.subnet_scan, ScanMode::kAuto);
    CHECK_EQ(config.discovery.scan_ports.size(), std::size_t{3});
    CHECK_EQ(config.rtsp.port, 554);
    CHECK_EQ(config.rtsp.stream, StreamSelection::kMain);
    CHECK_EQ(config.rtsp.username_env, std::string("HIKVISION_USERNAME"));
    CHECK_EQ(config.decode.decoder, DecoderPreference::kAuto);
    CHECK_EQ(config.capture.queue_size, 1);
    CHECK_EQ(config.runtime.max_active_cameras, 4);
    CHECK(config.cameras.empty());
}

TEST("camera config: an empty document loads the defaults") {
    const auto loaded = loadCameraModeConfigText("");
    CHECK(loaded.ok);
    CHECK(loaded.unknown_keys.empty());
    CHECK_EQ(firstDifference(loaded.config, CameraModeConfig{}), std::string{});

    const auto comments_only = loadCameraModeConfigText("# nothing here\n\n");
    CHECK(comments_only.ok);
    CHECK_EQ(firstDifference(comments_only.config, CameraModeConfig{}), std::string{});
}

TEST("camera config: every key is read") {
    const auto loaded = loadCameraModeConfigText(kFullConfig);
    CHECK(loaded.ok);
    CHECK_EQ(loaded.error, std::string{});
    CHECK(loaded.unknown_keys.empty());
    const CameraModeConfig& c = loaded.config;

    CHECK_EQ(c.network.interface, std::string("eth0"));
    CHECK_EQ(c.network.internet_check, false);
    CHECK_EQ(c.network.internet_probe_targets.size(), std::size_t{2});
    CHECK_EQ(c.network.internet_probe_targets[0], std::string("9.9.9.9:53"));
    CHECK_EQ(c.network.internet_probe_targets[1], std::string("1.0.0.1:443"));
    CHECK_EQ(c.network.internet_timeout_ms, 900);

    CHECK_EQ(c.discovery.enabled, false);
    CHECK_EQ(c.discovery.sadp, false);
    CHECK_EQ(c.discovery.onvif, false);
    CHECK_EQ(c.discovery.arp_table, false);
    CHECK_EQ(c.discovery.subnet_scan, ScanMode::kAlways);
    CHECK_EQ(c.discovery.scan_max_hosts, 254);
    CHECK(c.discovery.scan_ports == std::vector<int>({554, 8554}));
    CHECK_EQ(c.discovery.listen_ms, 1200);
    CHECK_EQ(c.discovery.connect_timeout_ms, 250);
    CHECK(c.discovery.manual_hosts ==
          std::vector<std::string>({"192.168.10.21", "192.168.10.22:8554"}));
    CHECK_EQ(c.discovery.max_cameras, 6);
    CHECK_EQ(c.discovery.registry_file, std::string("/data/registry.yaml"));
    CHECK_EQ(c.discovery.duplicate_ip_check, false);
    CHECK_EQ(c.discovery.rescan_interval_ms, std::int64_t{0});

    CHECK_EQ(c.rtsp.port, 8554);
    CHECK_EQ(c.rtsp.stream, StreamSelection::kSub);
    CHECK_EQ(c.rtsp.main_path, std::string("/Streaming/Channels/201"));
    CHECK_EQ(c.rtsp.sub_path, std::string("/Streaming/Channels/202"));
    CHECK_EQ(c.rtsp.username_env, std::string("CAM_USER"));
    CHECK_EQ(c.rtsp.password_env, std::string("CAM_PASS"));
    CHECK_EQ(c.rtsp.timeout_ms, 2500);
    CHECK_EQ(c.rtsp.latency_ms, 0);

    CHECK_EQ(c.decode.decoder, DecoderPreference::kNvidiaHardware);
    CHECK_EQ(c.decode.max_width, 1920);
    CHECK_EQ(c.decode.max_fps, 12);

    CHECK_EQ(c.capture.queue_size, 2);
    CHECK_EQ(c.capture.first_frame_timeout_ms, std::int64_t{8000});
    CHECK_EQ(c.capture.read_timeout_ms, std::int64_t{3000});
    CHECK_EQ(c.capture.max_frame_age_ms, std::int64_t{900});
    CHECK_EQ(c.capture.reconnect_initial_backoff_ms, std::int64_t{500});
    CHECK_EQ(c.capture.reconnect_max_backoff_ms, std::int64_t{20000});
    CHECK_EQ(c.capture.auth_retry_interval_ms, std::int64_t{600000});
    CHECK_EQ(c.capture.auth_max_retries, 0);
    CHECK_EQ(c.capture.configuration_retry_interval_ms, std::int64_t{60000});
    CHECK_EQ(c.capture.check_duration_ms, std::int64_t{5000});

    CHECK_EQ(c.runtime.max_active_cameras, 2);
    CHECK_EQ(c.runtime.status_file, std::string("/data/status.json"));
    CHECK_EQ(c.runtime.status_interval_ms, std::int64_t{2000});
    CHECK_EQ(c.runtime.metrics_interval_ms, std::int64_t{30000});
    CHECK_EQ(c.runtime.min_available_ram_mb, 0);

    CHECK_EQ(c.onvif.username_env, std::string("ONVIF_USER"));
    CHECK_EQ(c.onvif.password_env, std::string("ONVIF_PASS"));
    CHECK_EQ(c.native_anpr.enabled, true);

    CHECK_EQ(c.cameras.size(), std::size_t{2});
    const CameraOverride& gate = c.cameras[0];
    CHECK_EQ(gate.id, std::string("gate-in"));
    CHECK_EQ(gate.mac, std::string("44:19:b6:01:02:03"));
    CHECK_EQ(gate.serial, std::string("DS-TCG406-E0001"));
    CHECK_EQ(gate.ip, std::string("192.168.10.21"));
    CHECK(gate.enabled.has_value() && !*gate.enabled);
    CHECK(gate.rtsp_port.has_value() && *gate.rtsp_port == 8554);
    CHECK(gate.stream.has_value() && *gate.stream == StreamSelection::kSub);
    CHECK_EQ(gate.rtsp_path, std::string("/Streaming/Channels/103"));
    CHECK(gate.http_port.has_value() && *gate.http_port == 8080);
    CHECK_EQ(gate.username_env, std::string("GATE_IN_USER"));
    CHECK_EQ(gate.password_env, std::string("GATE_IN_PASS"));
    CHECK(gate.decoder.has_value() && *gate.decoder == DecoderPreference::kSoftware);
    CHECK_EQ(gate.anpr_config, std::string("config/gate-in.yaml"));
    CHECK(gate.native_anpr.has_value() && *gate.native_anpr);

    const CameraOverride& second = c.cameras[1];
    CHECK_EQ(second.ip, std::string("192.168.10.22"));
    CHECK(second.id.empty());
    CHECK(!second.enabled.has_value());
    CHECK(!second.rtsp_port.has_value());
    CHECK(!second.stream.has_value());
    CHECK(!second.http_port.has_value());
    CHECK(!second.decoder.has_value());
    CHECK(!second.native_anpr.has_value());
}

TEST("camera config: unknown keys are reported at the top level and inside camera entries") {
    const auto loaded = loadCameraModeConfigText(R"(
network:
  interface: eth0
  interfase: eth1
discovery:
  sadp: true
  fancy:
    deep: 1
colour: blue
cameras:
  - id: camera-01
    strem: sub
  - ip: 192.168.10.22
    rtsp:
      port: 554
)");
    CHECK(loaded.ok);
    const std::vector<std::string> expected = {"network.interfase", "discovery.fancy.deep",
                                               "colour", "cameras[0].strem",
                                               "cameras[1].rtsp.port"};
    CHECK(loaded.unknown_keys == expected);
    CHECK_EQ(loaded.config.network.interface, std::string("eth0"));
    CHECK_EQ(loaded.config.cameras.size(), std::size_t{2});
}

TEST("camera config: type and enum errors name the path") {
    struct Case {
        const char* yaml;
        const char* expected;
    };
    const Case cases[] = {
        {"rtsp:\n  stream: third\n", "INVALID_CONFIG: rtsp.stream: must be one of main or sub"},
        {"decode:\n  decoder: cuda\n",
         "INVALID_CONFIG: decode.decoder: must be one of auto, nvidia_hardware or software"},
        {"discovery:\n  subnet_scan: sometimes\n",
         "INVALID_CONFIG: discovery.subnet_scan: must be one of auto, always or never"},
        {"rtsp:\n  port: fast\n", "INVALID_CONFIG: rtsp.port: expected an integer"},
        {"rtsp:\n  port: 99999999999\n", "INVALID_CONFIG: rtsp.port: integer out of range"},
        {"discovery:\n  sadp: maybe\n", "INVALID_CONFIG: discovery.sadp: expected true or false"},
        {"discovery:\n  scan_ports: 554\n",
         "INVALID_CONFIG: discovery.scan_ports: expected a list"},
        {"discovery:\n  scan_ports: [554, http]\n",
         "INVALID_CONFIG: discovery.scan_ports: expected a list of integers"},
        {"network:\n  internet_probe_targets: 1.1.1.1:53\n",
         "INVALID_CONFIG: network.internet_probe_targets: expected a list"},
        {"network:\n  interface:\n    name: eth0\n",
         "INVALID_CONFIG: network.interface: expected a string"},
        {"cameras:\n  id: camera-01\n", "INVALID_CONFIG: cameras: expected a list"},
        {"cameras:\n  - camera-01\n", "INVALID_CONFIG: cameras[0]: expected a mapping"},
        {"cameras:\n  - id: camera-01\n    mac: not-a-mac\n",
         "INVALID_CONFIG: cameras[0].mac: 'not-a-mac' is not a MAC address"},
        {"cameras:\n  - id: camera-01\n  - id: camera-02\n    stream: 4k\n",
         "INVALID_CONFIG: cameras[1].stream: must be one of main or sub"},
        {"cameras:\n  - id: camera-01\n    enabled: perhaps\n",
         "INVALID_CONFIG: cameras[0].enabled: expected true or false"},
        {"cameras:\n  - id: camera-01\n    rtsp_port: x\n",
         "INVALID_CONFIG: cameras[0].rtsp_port: expected an integer"},
        {"rtsp:\n\tport: 554\n", "INVALID_CONFIG: line 2: tab indentation is not supported"},
        {"- a\n- b\n", "INVALID_CONFIG: the document root must be a mapping"},
    };
    for (const Case& test : cases) {
        const auto loaded = loadCameraModeConfigText(test.yaml);
        if (loaded.ok || !contains(loaded.error, test.expected)) {
            anpr_test::recordFailure(std::string("for yaml '") + test.yaml + "' expected error '" +
                                     test.expected + "' but got '" + loaded.error + "'");
            return;
        }
    }
}

TEST("camera config: validation rejects out-of-range values") {
    struct Case {
        const char* yaml;
        const char* expected;
    };
    const Case cases[] = {
        {"network:\n  interface: \"\"\n",
         "INVALID_CONFIG: network.interface: '' must be auto or an interface name"},
        {"network:\n  interface: an-interface-name-too-long\n",
         "INVALID_CONFIG: network.interface: 'an-interface-name-too-long'"},
        {"network:\n  interface: \"eth 0\"\n", "INVALID_CONFIG: network.interface: 'eth 0'"},
        {"rtsp:\n  port: 0\n", "INVALID_CONFIG: rtsp.port: must be between 1 and 65535"},
        {"rtsp:\n  port: 65536\n", "INVALID_CONFIG: rtsp.port: must be between 1 and 65535"},
        {"discovery:\n  scan_ports: [554, 70000]\n",
         "INVALID_CONFIG: discovery.scan_ports: port 70000 is outside 1..65535"},
        {"discovery:\n  scan_ports: []\n", "INVALID_CONFIG: discovery.scan_ports: must list"},
        {"capture:\n  queue_size: 0\n",
         "INVALID_CONFIG: capture.queue_size: must be between 1 and 8"},
        {"capture:\n  queue_size: 9\n",
         "INVALID_CONFIG: capture.queue_size: must be between 1 and 8"},
        {"capture:\n  read_timeout_ms: 0\n",
         "INVALID_CONFIG: capture.read_timeout_ms: must be positive"},
        {"capture:\n  first_frame_timeout_ms: -5\n",
         "INVALID_CONFIG: capture.first_frame_timeout_ms: must be positive"},
        {"capture:\n  reconnect_initial_backoff_ms: 5000\n  reconnect_max_backoff_ms: 1000\n",
         "INVALID_CONFIG: capture.reconnect_max_backoff_ms: must not be shorter"},
        {"capture:\n  auth_max_retries: -1\n",
         "INVALID_CONFIG: capture.auth_max_retries: must not be negative"},
        {"rtsp:\n  timeout_ms: 0\n", "INVALID_CONFIG: rtsp.timeout_ms: must be positive"},
        {"network:\n  internet_timeout_ms: 0\n",
         "INVALID_CONFIG: network.internet_timeout_ms: must be positive"},
        {"network:\n  internet_probe_targets: [1.1.1.1]\n",
         "INVALID_CONFIG: network.internet_probe_targets: '1.1.1.1' must be an IPv4 address "
         "and port"},
        {"discovery:\n  connect_timeout_ms: 0\n",
         "INVALID_CONFIG: discovery.connect_timeout_ms: must be positive"},
        {"discovery:\n  max_cameras: 0\n",
         "INVALID_CONFIG: discovery.max_cameras: must be at least 1"},
        {"runtime:\n  max_active_cameras: 0\n",
         "INVALID_CONFIG: runtime.max_active_cameras: must be at least 1"},
        {"runtime:\n  min_available_ram_mb: -1\n",
         "INVALID_CONFIG: runtime.min_available_ram_mb: must not be negative"},
        {"runtime:\n  status_interval_ms: 0\n",
         "INVALID_CONFIG: runtime.status_interval_ms: must be positive"},
        {"discovery:\n  scan_max_hosts: 0\n",
         "INVALID_CONFIG: discovery.scan_max_hosts: must be between 1 and 65536"},
        {"discovery:\n  scan_max_hosts: 65537\n",
         "INVALID_CONFIG: discovery.scan_max_hosts: must be between 1 and 65536"},
        {"discovery:\n  listen_ms: 99\n",
         "INVALID_CONFIG: discovery.listen_ms: must be between 100 and 30000"},
        {"discovery:\n  listen_ms: 30001\n",
         "INVALID_CONFIG: discovery.listen_ms: must be between 100 and 30000"},
        {"discovery:\n  manual_hosts: [192.168.10.300]\n",
         "INVALID_CONFIG: discovery.manual_hosts: '192.168.10.300' must be an IPv4 address"},
        {"discovery:\n  manual_hosts: [\"192.168.10.21:0\"]\n",
         "INVALID_CONFIG: discovery.manual_hosts: '192.168.10.21:0'"},
        {"discovery:\n  manual_hosts: [camera.local]\n",
         "INVALID_CONFIG: discovery.manual_hosts: 'camera.local'"},
        {"discovery:\n  rescan_interval_ms: -1\n",
         "INVALID_CONFIG: discovery.rescan_interval_ms: must not be negative"},
        {"discovery:\n  registry_file: \"\"\n",
         "INVALID_CONFIG: discovery.registry_file: must name a file"},
        {"rtsp:\n  main_path: Streaming/Channels/101\n",
         "INVALID_CONFIG: rtsp.main_path: must start with '/'"},
        {"rtsp:\n  sub_path: \"\"\n", "INVALID_CONFIG: rtsp.sub_path: must start with '/'"},
        {"rtsp:\n  username_env: HIKVISION-USER\n",
         "INVALID_CONFIG: rtsp.username_env: 'HIKVISION-USER' is not a valid environment"},
        {"rtsp:\n  password_env: \"\"\n", "INVALID_CONFIG: rtsp.password_env:"},
        {"onvif:\n  username_env: 1USER\n", "INVALID_CONFIG: onvif.username_env:"},
        {"decode:\n  max_width: -1\n", "INVALID_CONFIG: decode.max_width: must not be negative"},
        {"cameras:\n  - stream: sub\n",
         "INVALID_CONFIG: cameras[0]: needs at least one of id, mac, serial or ip"},
        {"cameras:\n  - id: camera 01\n",
         "INVALID_CONFIG: cameras[0].id: 'camera 01' may only contain letters, digits"},
        {"cameras:\n  - id: cam.01\n", "INVALID_CONFIG: cameras[0].id: 'cam.01'"},
        {"cameras:\n  - id: camera-01\n  - id: camera-01\n",
         "INVALID_CONFIG: cameras[1].id: 'camera-01' is used by another camera entry"},
        {"cameras:\n  - mac: 44:19:b6:01:02:03\n  - mac: 44-19-B6-01-02-03\n",
         "INVALID_CONFIG: cameras[1].mac: '44:19:b6:01:02:03' is used by another camera entry"},
        {"cameras:\n  - serial: S1\n  - serial: S1\n",
         "INVALID_CONFIG: cameras[1].serial: 'S1' is used by another camera entry"},
        {"cameras:\n  - ip: 192.168.10.256\n",
         "INVALID_CONFIG: cameras[0].ip: '192.168.10.256' is not an IPv4 address"},
        {"cameras:\n  - ip: 192.168.10.21\n  - ip: 192.168.10.21\n",
         "INVALID_CONFIG: cameras[1].ip: '192.168.10.21' is used by another camera entry"},
        {"cameras:\n  - id: camera-01\n    rtsp_port: 0\n",
         "INVALID_CONFIG: cameras[0].rtsp_port: must be between 1 and 65535"},
        {"cameras:\n  - id: camera-01\n    http_port: 65536\n",
         "INVALID_CONFIG: cameras[0].http_port: must be between 1 and 65535"},
        {"cameras:\n  - id: camera-01\n    rtsp_path: Streaming/Channels/101\n",
         "INVALID_CONFIG: cameras[0].rtsp_path: must start with '/'"},
        {"cameras:\n  - id: camera-01\n    username_env: \"MY USER\"\n",
         "INVALID_CONFIG: cameras[0].username_env: 'MY USER' is not a valid environment"},
        {"cameras:\n  - id: camera-01\n    password_env: MY-PASS\n",
         "INVALID_CONFIG: cameras[0].password_env:"},
    };
    for (const Case& test : cases) {
        const auto loaded = loadCameraModeConfigText(test.yaml);
        if (loaded.ok || !contains(loaded.error, test.expected)) {
            anpr_test::recordFailure(std::string("for yaml '") + test.yaml + "' expected error '" +
                                     test.expected + "' but got '" + loaded.error + "'");
            return;
        }
    }
}

TEST("camera config: boundary values are accepted") {
    const auto loaded = loadCameraModeConfigText(R"(
rtsp:
  port: 65535
discovery:
  scan_max_hosts: 65536
  listen_ms: 100
  subnet_scan: never
  scan_ports: []
  manual_hosts: ["192.168.10.21:1", 192.168.10.22]
capture:
  queue_size: 8
  auth_max_retries: 0
runtime:
  min_available_ram_mb: 0
onvif:
  username_env: ""
  password_env: ""
cameras:
  - id: Gate_02-b
    rtsp_port: 1
)");
    CHECK(loaded.ok);
    CHECK_EQ(loaded.error, std::string{});
    CHECK_EQ(loaded.config.discovery.subnet_scan, ScanMode::kNever);
    CHECK(loaded.config.discovery.scan_ports.empty());
}

TEST("camera config: the shipped config/cameras.yaml equals the defaults") {
    const auto loaded = loadCameraModeConfigFile("config/cameras.yaml");
    CHECK(loaded.ok);
    CHECK_EQ(loaded.error, std::string{});
    if (!loaded.unknown_keys.empty()) {
        anpr_test::recordFailure("unknown key in config/cameras.yaml: " + loaded.unknown_keys[0]);
        return;
    }
    CHECK_EQ(firstDifference(loaded.config, CameraModeConfig{}), std::string{});
}

TEST("camera config: the commented camera examples in config/cameras.yaml are valid") {
    // Uncomment the example block and load it, so the documentation cannot drift from the parser.
    std::ifstream input("config/cameras.yaml");
    CHECK(static_cast<bool>(input));
    std::ostringstream text;
    std::string line;
    bool in_example = false;
    while (std::getline(input, line)) {
        if (line == "cameras: []") {
            continue;
        }
        if (line == "# cameras:") {
            in_example = true;
        }
        if (in_example && line.rfind("# ", 0) == 0) {
            line = line.substr(2);
        }
        text << line << '\n';
    }
    CHECK(in_example);
    const auto loaded = loadCameraModeConfigText(text.str());
    CHECK(loaded.ok);
    CHECK(loaded.unknown_keys.empty());
    CHECK_EQ(loaded.config.cameras.size(), std::size_t{3});
    CHECK_EQ(loaded.config.cameras[0].mac, std::string("44:19:b6:01:02:03"));
    CHECK(loaded.config.cameras[1].enabled.has_value() && !*loaded.config.cameras[1].enabled);
    CHECK_EQ(loaded.config.cameras[2].username_env, std::string("GATE_EXIT_USERNAME"));
}

TEST("camera config: a missing file is a configuration error") {
    const auto loaded = loadCameraModeConfigFile("config/definitely-not-cameras.yaml");
    CHECK(!loaded.ok);
    CHECK(contains(loaded.error, "INVALID_CONFIG: cannot open config/definitely-not-cameras.yaml"));
}

TEST("camera config: enum names round trip") {
    CHECK_EQ(toString(StreamSelection::kMain), std::string("main"));
    CHECK_EQ(toString(StreamSelection::kSub), std::string("sub"));
    CHECK_EQ(toString(DecoderPreference::kAuto), std::string("auto"));
    CHECK_EQ(toString(DecoderPreference::kNvidiaHardware), std::string("nvidia_hardware"));
    CHECK_EQ(toString(DecoderPreference::kSoftware), std::string("software"));
    CHECK_EQ(toString(ScanMode::kAuto), std::string("auto"));
    CHECK_EQ(toString(ScanMode::kAlways), std::string("always"));
    CHECK_EQ(toString(ScanMode::kNever), std::string("never"));
    for (const StreamSelection stream : {StreamSelection::kMain, StreamSelection::kSub}) {
        const auto loaded = loadCameraModeConfigText("rtsp:\n  stream: " + toString(stream) + "\n");
        CHECK(loaded.ok);
        CHECK_EQ(loaded.config.rtsp.stream, stream);
    }
    for (const DecoderPreference decoder : {DecoderPreference::kAuto,
                                            DecoderPreference::kNvidiaHardware,
                                            DecoderPreference::kSoftware}) {
        const auto loaded =
            loadCameraModeConfigText("decode:\n  decoder: " + toString(decoder) + "\n");
        CHECK(loaded.ok);
        CHECK_EQ(loaded.config.decode.decoder, decoder);
    }
    for (const ScanMode mode : {ScanMode::kAuto, ScanMode::kAlways, ScanMode::kNever}) {
        const auto loaded =
            loadCameraModeConfigText("discovery:\n  subnet_scan: " + toString(mode) + "\n");
        CHECK(loaded.ok);
        CHECK_EQ(loaded.config.discovery.subnet_scan, mode);
    }
}

TEST("camera config: envSuffix upper-cases and maps '-' to '_'") {
    CHECK_EQ(envSuffix("camera-02"), std::string("CAMERA_02"));
    CHECK_EQ(envSuffix("Gate_In-1"), std::string("GATE_IN_1"));
    CHECK_EQ(envSuffix(""), std::string(""));
}

TEST("camera config: credentials resolve override, then per-camera, then global variables") {
    CameraModeConfig config;
    CameraOverride override_entry;
    override_entry.id = "camera-02";
    override_entry.username_env = "GATE_USER";
    override_entry.password_env = "GATE_PASS";

    FakeEnv env;
    env.values = {
        {"GATE_USER", "gate-user"},
        {"GATE_PASS", "gate-secret"},
        {"HIKVISION_USERNAME_CAMERA_02", "cam2-user"},
        {"HIKVISION_PASSWORD_CAMERA_02", "cam2-secret"},
        {"HIKVISION_USERNAME", "admin"},
        {"HIKVISION_PASSWORD", "global-secret"},
    };

    // 1. The override's own variables.
    auto resolved = resolveCredentials(config, &override_entry, "camera-02", env.lookup());
    CHECK(resolved.present);
    CHECK_EQ(resolved.credentials.username, std::string("gate-user"));
    CHECK_EQ(resolved.credentials.password, std::string("gate-secret"));
    CHECK_EQ(resolved.source, std::string("GATE_USER/GATE_PASS"));

    // 2. Override variables unset: the per-camera variables.
    env.values.erase("GATE_USER");
    resolved = resolveCredentials(config, &override_entry, "camera-02", env.lookup());
    CHECK(resolved.present);
    CHECK_EQ(resolved.credentials.username, std::string("cam2-user"));
    CHECK_EQ(resolved.credentials.password, std::string("cam2-secret"));
    CHECK_EQ(resolved.source,
             std::string("HIKVISION_USERNAME_CAMERA_02/HIKVISION_PASSWORD_CAMERA_02"));

    // An empty username counts as unset.
    env.values["HIKVISION_USERNAME_CAMERA_02"] = "";
    resolved = resolveCredentials(config, nullptr, "camera-02", env.lookup());
    CHECK(resolved.present);
    CHECK_EQ(resolved.source, std::string("HIKVISION_USERNAME/HIKVISION_PASSWORD"));

    // 3. The global pair.
    env.values.erase("HIKVISION_USERNAME_CAMERA_02");
    resolved = resolveCredentials(config, &override_entry, "camera-02", env.lookup());
    CHECK(resolved.present);
    CHECK_EQ(resolved.credentials.username, std::string("admin"));
    CHECK_EQ(resolved.credentials.password, std::string("global-secret"));
    CHECK_EQ(resolved.source, std::string("HIKVISION_USERNAME/HIKVISION_PASSWORD"));

    // Another camera never sees camera-02's variables.
    env.values["HIKVISION_USERNAME_CAMERA_02"] = "cam2-user";
    resolved = resolveCredentials(config, nullptr, "camera-03", env.lookup());
    CHECK_EQ(resolved.credentials.username, std::string("admin"));

    // Nothing set: not present, nothing reported.
    FakeEnv empty;
    resolved = resolveCredentials(config, &override_entry, "camera-02", empty.lookup());
    CHECK(!resolved.present);
    CHECK(resolved.credentials.empty());
    CHECK(resolved.source.empty());
    const std::vector<std::string> order = {"GATE_USER", "HIKVISION_USERNAME_CAMERA_02",
                                            "HIKVISION_USERNAME"};
    CHECK(empty.lookups == order);
}

TEST("camera config: a username without a password resolves with an empty password") {
    CameraModeConfig config;
    FakeEnv env;
    env.values = {{"HIKVISION_USERNAME", "viewer"}};
    const auto resolved = resolveCredentials(config, nullptr, "camera-01", env.lookup());
    CHECK(resolved.present);
    CHECK_EQ(resolved.credentials.username, std::string("viewer"));
    CHECK_EQ(resolved.credentials.password, std::string(""));
    CHECK_EQ(resolved.source, std::string("HIKVISION_USERNAME/HIKVISION_PASSWORD"));

    // A password alone is not a login.
    FakeEnv password_only;
    password_only.values = {{"HIKVISION_PASSWORD", "secret"}};
    CHECK(!resolveCredentials(config, nullptr, "camera-01", password_only.lookup()).present);
}

TEST("camera config: renamed global variables keep their per-camera variants") {
    CameraModeConfig config;
    config.rtsp.username_env = "CAM_USER";
    config.rtsp.password_env = "CAM_PASS";
    FakeEnv env;
    env.values = {{"CAM_USER_GATE_1", "gate"}, {"CAM_PASS_GATE_1", "gate-pass"},
                  {"CAM_USER", "all"}, {"CAM_PASS", "all-pass"},
                  {"HIKVISION_USERNAME", "ignored"}};
    auto resolved = resolveCredentials(config, nullptr, "gate-1", env.lookup());
    CHECK_EQ(resolved.credentials.username, std::string("gate"));
    CHECK_EQ(resolved.source, std::string("CAM_USER_GATE_1/CAM_PASS_GATE_1"));
    resolved = resolveCredentials(config, nullptr, "gate-2", env.lookup());
    CHECK_EQ(resolved.credentials.username, std::string("all"));
    CHECK_EQ(resolved.source, std::string("CAM_USER/CAM_PASS"));
}

TEST("camera config: an override naming only a password variable reuses the global username") {
    CameraModeConfig config;
    CameraOverride override_entry;
    override_entry.id = "camera-04";
    override_entry.password_env = "CAMERA_FOUR_PASSWORD";
    FakeEnv env;
    env.values = {{"HIKVISION_USERNAME", "admin"},
                  {"HIKVISION_PASSWORD", "global"},
                  {"CAMERA_FOUR_PASSWORD", "four"}};
    const auto resolved = resolveCredentials(config, &override_entry, "camera-04", env.lookup());
    CHECK(resolved.present);
    CHECK_EQ(resolved.credentials.username, std::string("admin"));
    CHECK_EQ(resolved.credentials.password, std::string("four"));
    CHECK_EQ(resolved.source, std::string("HIKVISION_USERNAME/CAMERA_FOUR_PASSWORD"));
}

TEST("camera config: credential values never appear in the reported source") {
    CameraModeConfig config;
    CameraOverride override_entry;
    override_entry.username_env = "U1";
    override_entry.password_env = "P1";
    FakeEnv env;
    env.values = {{"U1", "operator-name"}, {"P1", "Sup3r-Secret!"},
                  {"HIKVISION_USERNAME_CAMERA_01", "per-camera-user"},
                  {"HIKVISION_PASSWORD_CAMERA_01", "per-camera-pass"},
                  {"HIKVISION_USERNAME", "global-user"}, {"HIKVISION_PASSWORD", "global-pass"}};
    const std::vector<std::string> secrets = {"operator-name", "Sup3r-Secret!", "per-camera-user",
                                              "per-camera-pass", "global-user", "global-pass"};
    for (int step = 0; step < 3; ++step) {
        const auto resolved = resolveCredentials(config, step == 0 ? &override_entry : nullptr,
                                                 step == 2 ? "camera-09" : "camera-01",
                                                 env.lookup());
        CHECK(resolved.present);
        CHECK(!resolved.source.empty());
        for (const std::string& secret : secrets) {
            CHECK(!contains(resolved.source, secret));
        }
    }
}

TEST("camera config: ONVIF credentials only when the ONVIF username is set") {
    CameraModeConfig config;
    FakeEnv env;
    CHECK(!resolveOnvifCredentials(config, env.lookup()).has_value());
    env.values = {{"HIKVISION_USERNAME", "admin"}, {"ONVIF_PASSWORD", "x"}};
    CHECK(!resolveOnvifCredentials(config, env.lookup()).has_value());
    env.values["ONVIF_USERNAME"] = "onvif";
    const auto credentials = resolveOnvifCredentials(config, env.lookup());
    CHECK(credentials.has_value());
    CHECK_EQ(credentials->username, std::string("onvif"));
    CHECK_EQ(credentials->password, std::string("x"));
    config.onvif.username_env.clear();
    CHECK(!resolveOnvifCredentials(config, env.lookup()).has_value());
}

TEST("camera config: processEnvironment reads the process environment") {
    const EnvLookup env = processEnvironment();
    ::setenv("KZ_ANPR_TEST_CAMERA_ENV", "value-1", 1);
    const auto present = env("KZ_ANPR_TEST_CAMERA_ENV");
    CHECK(present.has_value());
    CHECK_EQ(*present, std::string("value-1"));
    ::unsetenv("KZ_ANPR_TEST_CAMERA_ENV");
    CHECK(!env("KZ_ANPR_TEST_CAMERA_ENV").has_value());
}

TEST("camera config: findOverride matches id, then MAC, serial and IP") {
    CameraModeConfig config;
    CameraOverride by_id;
    by_id.id = "gate-in";
    CameraOverride by_mac;
    by_mac.mac = "44:19:b6:01:02:03";
    CameraOverride by_serial;
    by_serial.serial = "SERIAL-1";
    CameraOverride by_ip;
    by_ip.ip = "192.168.10.30";
    config.cameras = {by_id, by_mac, by_serial, by_ip};
    const CameraOverride* const id_entry = &config.cameras[0];
    const CameraOverride* const mac_entry = &config.cameras[1];
    const CameraOverride* const serial_entry = &config.cameras[2];
    const CameraOverride* const ip_entry = &config.cameras[3];

    CHECK(findOverride(config, "gate-in", "", "", "") == id_entry);
    // The id wins even when the other identifiers point elsewhere.
    CHECK(findOverride(config, "gate-in", "44:19:b6:01:02:03", "SERIAL-1", "192.168.10.30") ==
          id_entry);
    CHECK(findOverride(config, "camera-07", "44:19:b6:01:02:03", "SERIAL-1", "192.168.10.30") ==
          mac_entry);
    CHECK(findOverride(config, "camera-07", "44-19-B6-01-02-03", "", "") == mac_entry);
    CHECK(findOverride(config, "camera-07", "", "SERIAL-1", "192.168.10.30") == serial_entry);
    CHECK(findOverride(config, "camera-07", "", "", "192.168.10.30") == ip_entry);
    CHECK(findOverride(config, "camera-07", "aa:bb:cc:dd:ee:ff", "OTHER", "192.168.10.99") ==
          nullptr);
    // Empty fields never match entries that leave those fields empty.
    CHECK(findOverride(config, "", "", "", "") == nullptr);
}

TEST("camera config: findOverride never applies an IP override written for another device") {
    CameraModeConfig config;
    CameraOverride entry;
    entry.mac = "44:19:b6:01:02:03";
    entry.ip = "192.168.10.21";
    CameraOverride serial_entry;
    serial_entry.serial = "S-2";
    serial_entry.ip = "192.168.10.22";
    config.cameras = {entry, serial_entry};
    // Same IP, different MAC: another camera took the address.
    CHECK(findOverride(config, "camera-05", "44:19:b6:0a:0b:0c", "", "192.168.10.21") == nullptr);
    // Same IP, MAC unknown: still that camera.
    CHECK(findOverride(config, "camera-05", "", "", "192.168.10.21") == &config.cameras[0]);
    CHECK(findOverride(config, "camera-06", "", "S-3", "192.168.10.22") == nullptr);
    CHECK(findOverride(config, "camera-06", "", "", "192.168.10.22") == &config.cameras[1]);
}

// ---------------------------------------------------------------------------------------------
// Camera registry
// ---------------------------------------------------------------------------------------------

namespace {

DeviceIdentity device(const std::string& mac, const std::string& serial, const std::string& ip) {
    DeviceIdentity identity;
    identity.mac = mac;
    identity.serial = serial;
    identity.ip = ip;
    return identity;
}

}  // namespace

TEST("camera registry: new devices get camera-01, camera-02 in order") {
    CameraRegistry registry;
    CHECK_EQ(registry.assign(device("44:19:b6:00:00:01", "", "192.168.10.21"), "t1"),
             std::string("camera-01"));
    CHECK_EQ(registry.lastMatchKey(), std::string("new"));
    CHECK_EQ(registry.assign(device("44:19:b6:00:00:02", "", "192.168.10.22"), "t1"),
             std::string("camera-02"));
    CHECK_EQ(registry.assign(device("", "SERIAL-3", "192.168.10.23"), "t1"),
             std::string("camera-03"));
    CHECK_EQ(registry.entries().size(), std::size_t{3});
    const RegistryEntry* first = registry.find("camera-01");
    CHECK(first != nullptr);
    CHECK_EQ(first->mac, std::string("44:19:b6:00:00:01"));
    CHECK_EQ(first->last_ip, std::string("192.168.10.21"));
    CHECK_EQ(first->first_seen, std::string("t1"));
    CHECK_EQ(first->last_seen, std::string("t1"));
    CHECK(registry.find("camera-04") == nullptr);
}

TEST("camera registry: the id follows the MAC across an IP change") {
    CameraRegistry registry;
    registry.assign(device("44:19:b6:00:00:01", "", "192.168.10.21"), "t1");
    registry.assign(device("44:19:b6:00:00:02", "", "192.168.10.22"), "t1");
    CHECK_EQ(registry.assign(device("44-19-B6-00-00-02", "", "192.168.10.50"), "t2"),
             std::string("camera-02"));
    CHECK_EQ(registry.lastMatchKey(), std::string("mac"));
    const RegistryEntry* entry = registry.find("camera-02");
    CHECK_EQ(entry->last_ip, std::string("192.168.10.50"));
    CHECK_EQ(entry->first_seen, std::string("t1"));
    CHECK_EQ(entry->last_seen, std::string("t2"));
    CHECK_EQ(registry.entries().size(), std::size_t{2});
}

TEST("camera registry: the serial identifies a device without a MAC") {
    CameraRegistry registry;
    registry.assign(device("", "DS-TCG406-E0001", "192.168.10.21"), "t1");
    CHECK_EQ(registry.assign(device("", "DS-TCG406-E0001", "192.168.10.40"), "t2"),
             std::string("camera-01"));
    CHECK_EQ(registry.lastMatchKey(), std::string("serial"));
    // The MAC learned later is filled into the entry and wins from then on.
    CHECK_EQ(registry.assign(device("44:19:b6:00:00:09", "DS-TCG406-E0001", "192.168.10.41"), "t3"),
             std::string("camera-01"));
    CHECK_EQ(registry.lastMatchKey(), std::string("serial"));
    CHECK_EQ(registry.find("camera-01")->mac, std::string("44:19:b6:00:00:09"));
    CHECK_EQ(registry.assign(device("44:19:b6:00:00:09", "", "192.168.10.42"), "t4"),
             std::string("camera-01"));
    CHECK_EQ(registry.lastMatchKey(), std::string("mac"));
}

TEST("camera registry: device ID and ONVIF UUID identify a device without MAC or serial") {
    CameraRegistry registry;
    DeviceIdentity hik;
    hik.device_id = "6aff4000-0001";
    hik.ip = "192.168.10.21";
    DeviceIdentity onvif;
    onvif.onvif_uuid = "urn:uuid:0000-0002";
    onvif.ip = "192.168.10.22";
    CHECK_EQ(registry.assign(hik, "t1"), std::string("camera-01"));
    CHECK_EQ(registry.assign(onvif, "t1"), std::string("camera-02"));
    hik.ip = "192.168.10.31";
    onvif.ip = "192.168.10.32";
    CHECK_EQ(registry.assign(hik, "t2"), std::string("camera-01"));
    CHECK_EQ(registry.lastMatchKey(), std::string("device_id"));
    CHECK_EQ(registry.assign(onvif, "t2"), std::string("camera-02"));
    CHECK_EQ(registry.lastMatchKey(), std::string("onvif_uuid"));
}

TEST("camera registry: an IP-only device keeps its id while its IP is unchanged") {
    CameraRegistry registry;
    CHECK_EQ(registry.assign(device("", "", "192.168.10.21"), "t1"), std::string("camera-01"));
    CHECK_EQ(registry.assign(device("", "", "192.168.10.21"), "t2"), std::string("camera-01"));
    CHECK_EQ(registry.lastMatchKey(), std::string("ip"));
    CHECK_EQ(registry.assign(device("", "", "192.168.10.22"), "t2"), std::string("camera-02"));
    // The MAC seen later (ARP) is learned by the entry found through its IP.
    CHECK_EQ(registry.assign(device("44:19:b6:00:00:01", "", "192.168.10.21"), "t3"),
             std::string("camera-01"));
    CHECK_EQ(registry.lastMatchKey(), std::string("ip"));
    CHECK_EQ(registry.find("camera-01")->mac, std::string("44:19:b6:00:00:01"));
}

TEST("camera registry: a different MAC at an old IP gets a new id") {
    CameraRegistry registry;
    registry.assign(device("44:19:b6:00:00:01", "", "192.168.10.21"), "t1");
    CHECK_EQ(registry.assign(device("44:19:b6:00:00:99", "", "192.168.10.21"), "t2"),
             std::string("camera-02"));
    CHECK_EQ(registry.lastMatchKey(), std::string("new"));
    // The original camera keeps its MAC.
    CHECK_EQ(registry.find("camera-01")->mac, std::string("44:19:b6:00:00:01"));
    // Likewise a different serial at the address.
    CameraRegistry serials;
    serials.assign(device("", "SERIAL-A", "192.168.10.21"), "t1");
    CHECK_EQ(serials.assign(device("", "SERIAL-B", "192.168.10.21"), "t2"),
             std::string("camera-02"));
}

TEST("camera registry: a stored MAC or serial is never overwritten") {
    CameraRegistry registry;
    registry.assign(device("44:19:b6:00:00:01", "SERIAL-A", "192.168.10.21"), "t1");
    // Same MAC, different serial (should not happen, but must not corrupt the entry).
    CHECK_EQ(registry.assign(device("44:19:b6:00:00:01", "SERIAL-B", "192.168.10.21"), "t2"),
             std::string("camera-01"));
    CHECK_EQ(registry.find("camera-01")->serial, std::string("SERIAL-A"));
    // Same serial but a different MAC is a different device.
    CHECK_EQ(registry.assign(device("44:19:b6:00:00:02", "SERIAL-A", "192.168.10.22"), "t3"),
             std::string("camera-02"));
    CHECK_EQ(registry.find("camera-01")->mac, std::string("44:19:b6:00:00:01"));
}

TEST("camera registry: model, device ID and timestamps are updated") {
    CameraRegistry registry;
    DeviceIdentity identity = device("44:19:b6:00:00:01", "", "192.168.10.21");
    registry.assign(identity, "2026-10-09T10:00:00Z");
    identity.model = "DS-TCG406-E";
    identity.device_id = "dev-1";
    identity.onvif_uuid = "urn:uuid:1";
    identity.serial = "SERIAL-1";
    registry.assign(identity, "2026-10-09T11:00:00Z");
    const RegistryEntry* entry = registry.find("camera-01");
    CHECK_EQ(entry->model, std::string("DS-TCG406-E"));
    CHECK_EQ(entry->device_id, std::string("dev-1"));
    CHECK_EQ(entry->onvif_uuid, std::string("urn:uuid:1"));
    CHECK_EQ(entry->serial, std::string("SERIAL-1"));
    CHECK_EQ(entry->first_seen, std::string("2026-10-09T10:00:00Z"));
    CHECK_EQ(entry->last_seen, std::string("2026-10-09T11:00:00Z"));
    // An identity without a model keeps the stored one.
    registry.assign(device("44:19:b6:00:00:01", "", ""), "2026-10-09T12:00:00Z");
    CHECK_EQ(entry->model, std::string("DS-TCG406-E"));
    CHECK_EQ(entry->last_ip, std::string("192.168.10.21"));
}

TEST("camera registry: ids of offline cameras stay reserved") {
    CameraRegistry registry;
    registry.assign(device("44:19:b6:00:00:01", "", "192.168.10.21"), "t1");
    registry.assign(device("44:19:b6:00:00:02", "", "192.168.10.22"), "t1");
    registry.assign(device("44:19:b6:00:00:03", "", "192.168.10.23"), "t1");
    // camera-02 is unplugged; a new camera arrives, even at camera-02's old address.
    CHECK_EQ(registry.assign(device("44:19:b6:00:00:04", "", "192.168.10.22"), "t2"),
             std::string("camera-04"));
    // camera-02 comes back.
    CHECK_EQ(registry.assign(device("44:19:b6:00:00:02", "", "192.168.10.60"), "t3"),
             std::string("camera-02"));
    CHECK_EQ(registry.entries().size(), std::size_t{4});
}

TEST("camera registry: ties between equally good matches are resolved deterministically") {
    const std::string text = R"(cameras:
  - id: "old"
    last_ip: "192.168.10.21"
    mac: "44:19:b6:00:00:01"
    last_seen: "2026-01-01T00:00:00Z"
  - id: "ip-only"
    last_ip: "192.168.10.21"
    last_seen: "2025-01-01T00:00:00Z"
  - id: "dup-a"
    serial: "S"
    last_seen: "2026-01-01T00:00:00Z"
  - id: "dup-b"
    serial: "S"
    last_seen: "2026-05-01T00:00:00Z"
)";
    std::string error;
    CameraRegistry registry = CameraRegistry::parse(text, error);
    CHECK_EQ(error, std::string{});
    // An unknown MAC at the address: the entry known only by its IP is the closer fit.
    CHECK_EQ(registry.assign(device("", "", "192.168.10.21"), "t"), std::string("ip-only"));
    // Two entries with the same serial: the one seen most recently.
    CHECK_EQ(registry.assign(device("", "S", ""), "t"), std::string("dup-b"));
}

TEST("camera registry: nextCameraId takes the lowest free number") {
    CHECK_EQ(nextCameraId({}), std::string("camera-01"));
    CHECK_EQ(nextCameraId({"camera-01", "camera-03"}), std::string("camera-02"));
    CHECK_EQ(nextCameraId({"camera-02", "gate-in"}), std::string("camera-01"));
    std::vector<std::string> taken;
    for (int i = 1; i <= 99; ++i) {
        taken.push_back(nextCameraId(taken));
    }
    CHECK_EQ(taken.back(), std::string("camera-99"));
    CHECK_EQ(nextCameraId(taken), std::string("camera-100"));
}

TEST("camera registry: utcNowIso is an ISO 8601 UTC timestamp") {
    const std::string now = utcNowIso();
    CHECK_EQ(now.size(), std::size_t{20});
    CHECK_EQ(now[4], '-');
    CHECK_EQ(now[7], '-');
    CHECK_EQ(now[10], 'T');
    CHECK_EQ(now[13], ':');
    CHECK_EQ(now[16], ':');
    CHECK_EQ(now[19], 'Z');
    CHECK(now >= std::string("2024-01-01T00:00:00Z"));
}

TEST("camera registry: serialize and parse round trip every field") {
    CameraRegistry registry;
    DeviceIdentity tricky;
    tricky.mac = "44:19:b6:00:00:01";
    tricky.serial = "DS-TCG406-E \"quoted\" #1";
    tricky.device_id = "back\\slash: [x]";
    tricky.onvif_uuid = "urn:uuid:5a6b";
    tricky.model = "tab\there, it's 'odd'";
    tricky.ip = "192.168.10.21";
    registry.assign(tricky, "2026-10-09T10:15:00Z");
    registry.assign(device("", "", "192.168.10.22"), "2026-10-09T10:16:00Z");

    const std::string text = registry.serialize();
    CHECK(contains(text, "# Camera identities, generated"));
    CHECK(contains(text, "No secrets"));
    CHECK(contains(text, "Ids may be edited"));
    CHECK(!contains(text, "\t"));

    std::string error;
    const CameraRegistry parsed = CameraRegistry::parse(text, error);
    CHECK_EQ(error, std::string{});
    CHECK_EQ(parsed.entries().size(), std::size_t{2});
    const RegistryEntry& a = registry.entries()[0];
    const RegistryEntry& b = parsed.entries()[0];
    CHECK_EQ(b.id, a.id);
    CHECK_EQ(b.mac, a.mac);
    CHECK_EQ(b.serial, a.serial);
    CHECK_EQ(b.device_id, a.device_id);
    CHECK_EQ(b.onvif_uuid, a.onvif_uuid);
    CHECK_EQ(b.model, a.model);
    CHECK_EQ(b.last_ip, a.last_ip);
    CHECK_EQ(b.first_seen, a.first_seen);
    CHECK_EQ(b.last_seen, a.last_seen);
    CHECK_EQ(parsed.entries()[1].id, std::string("camera-02"));
    CHECK_EQ(parsed.entries()[1].last_ip, std::string("192.168.10.22"));
    CHECK_EQ(parsed.serialize(), text);

    std::string empty_error;
    const CameraRegistry empty = CameraRegistry::parse(CameraRegistry{}.serialize(), empty_error);
    CHECK_EQ(empty_error, std::string{});
    CHECK(empty.entries().empty());
}

TEST("camera registry: hand-edited ids are kept and new ids avoid them") {
    const std::string text = R"(# edited by the installer
cameras:
  - id: gate-in
    mac: 44-19-B6-00-00-01
    last_ip: 192.168.10.21
  - id: camera-01
    serial: SERIAL-2
)";
    std::string error;
    CameraRegistry registry = CameraRegistry::parse(text, error);
    CHECK_EQ(error, std::string{});
    CHECK_EQ(registry.entries()[0].mac, std::string("44:19:b6:00:00:01"));
    CHECK_EQ(registry.assign(device("44:19:b6:00:00:01", "", "192.168.10.21"), "t"),
             std::string("gate-in"));
    CHECK_EQ(registry.assign(device("44:19:b6:00:00:05", "", "192.168.10.25"), "t"),
             std::string("camera-02"));
}

TEST("camera registry: malformed files are reported, valid entries kept") {
    std::string error;
    CameraRegistry broken = CameraRegistry::parse("cameras:\n\t- id: x\n", error);
    CHECK(!error.empty());
    CHECK(broken.entries().empty());

    broken = CameraRegistry::parse("- just\n- a list\n", error);
    CHECK(contains(error, "root must be a mapping"));
    CHECK(broken.entries().empty());

    broken = CameraRegistry::parse("cameras: camera-01\n", error);
    CHECK(contains(error, "'cameras' must be a list"));

    const std::string text = R"(cameras:
  - id: camera-01
    mac: 44:19:b6:00:00:01
  - id: "bad id!"
    mac: 44:19:b6:00:00:02
  - id: camera-01
    mac: 44:19:b6:00:00:03
  - mac: 44:19:b6:00:00:04
  - plain scalar
  - id: camera-05
    mac: nonsense
    last_ip: 192.168.10.25
)";
    const CameraRegistry partial = CameraRegistry::parse(text, error);
    CHECK(contains(error, "cameras[1]: id 'bad id!'"));
    CHECK(contains(error, "cameras[2]: id 'camera-01' is used twice"));
    CHECK(contains(error, "cameras[3]: id ''"));
    CHECK(contains(error, "cameras[4] is not a mapping"));
    CHECK(contains(error, "cameras[5]: mac 'nonsense' is not a MAC address"));
    CHECK_EQ(partial.entries().size(), std::size_t{2});
    CHECK_EQ(partial.entries()[0].id, std::string("camera-01"));
    CHECK_EQ(partial.entries()[0].mac, std::string("44:19:b6:00:00:01"));
    CHECK_EQ(partial.entries()[1].id, std::string("camera-05"));
    CHECK(partial.entries()[1].mac.empty());

    // A blank file and a file without a cameras key are simply empty.
    CameraRegistry blank = CameraRegistry::parse("", error);
    CHECK_EQ(error, std::string{});
    CHECK(blank.entries().empty());
    blank = CameraRegistry::parse("version: 1\n", error);
    CHECK_EQ(error, std::string{});
}

TEST("camera registry: save and load through a temporary directory") {
    const anpr::filesystem::path dir = freshTempDir("registry");
    const std::string path = (dir / "nested" / "registry.yaml").string();

    std::string error;
    CameraRegistry missing = CameraRegistry::load(path, error);
    CHECK_EQ(error, std::string{});
    CHECK(missing.entries().empty());

    CameraRegistry registry;
    registry.assign(device("44:19:b6:00:00:01", "SERIAL-1", "192.168.10.21"), "t1");
    registry.assign(device("", "", "192.168.10.22"), "t1");
    CHECK(registry.save(path, error));
    CHECK_EQ(error, std::string{});
    CHECK(directoryEntries(dir / "nested") == std::vector<std::string>({"registry.yaml"}));

    CameraRegistry loaded = CameraRegistry::load(path, error);
    CHECK_EQ(error, std::string{});
    CHECK_EQ(loaded.serialize(), registry.serialize());
    CHECK_EQ(loaded.assign(device("44:19:b6:00:00:01", "", "192.168.10.99"), "t2"),
             std::string("camera-01"));
    CHECK(loaded.save(path, error));
    CameraRegistry reloaded = CameraRegistry::load(path, error);
    CHECK_EQ(reloaded.find("camera-01")->last_ip, std::string("192.168.10.99"));

    // A directory where the file should be is unreadable and cannot be replaced.
    const std::string blocked = (dir / "blocked").string();
    anpr::filesystem::create_directories(blocked);
    CameraRegistry::load(blocked, error);
    CHECK(!error.empty());
    CHECK(!registry.save(blocked, error));
    CHECK(!error.empty());

    // A corrupt file reports an error and yields an empty registry.
    {
        std::ofstream corrupt(path, std::ios::trunc);
        corrupt << "cameras:\n  - id: [unterminated\n";
    }
    CameraRegistry corrupt_loaded = CameraRegistry::load(path, error);
    CHECK(contains(error, path));
    CHECK(corrupt_loaded.entries().empty());

    std::error_code ec;
    anpr::filesystem::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------------------------

namespace {

std::vector<CameraError> allCameraErrors() {
    std::vector<CameraError> errors;
    for (int value = static_cast<int>(CameraError::kNone);
         value <= static_cast<int>(CameraError::kNativeAnprUnavailable); ++value) {
        errors.push_back(static_cast<CameraError>(value));
    }
    return errors;
}

}  // namespace

TEST("diagnostics: every error has a unique code, a meaning and an action") {
    std::set<std::string> codes;
    for (const CameraError error : allCameraErrors()) {
        const ErrorInfo& info = describe(error);
        CHECK(info.code != nullptr && info.meaning != nullptr && info.action != nullptr);
        CHECK(!std::string(info.code).empty());
        CHECK(!std::string(info.meaning).empty());
        CHECK(!std::string(info.action).empty());
        CHECK(codes.insert(info.code).second);
        CHECK_EQ(toString(error), std::string(info.code));
        CHECK_EQ(cameraErrorFromString(info.code), error);
        // Codes are SCREAMING_SNAKE_CASE tokens, safe as unquoted log values.
        for (const char ch : std::string(info.code)) {
            CHECK((ch >= 'A' && ch <= 'Z') || ch == '_');
        }
    }
    CHECK_EQ(codes.size(), std::size_t{31});
    CHECK_EQ(toString(CameraError::kNone), std::string("NONE"));
    CHECK_EQ(cameraErrorFromString("NOT_A_CODE"), CameraError::kNone);
    CHECK_EQ(cameraErrorFromString(""), CameraError::kNone);
    CHECK_EQ(cameraErrorFromString("rtsp_auth_failed"), CameraError::kNone);
}

TEST("diagnostics: catalog texts and severities match the installer brief") {
    const ErrorInfo& auth = describe(CameraError::kRtspAuthFailed);
    CHECK_EQ(std::string(auth.code), std::string("RTSP_AUTH_FAILED"));
    CHECK_EQ(std::string(auth.meaning),
             std::string("The camera rejected the configured username/password."));
    CHECK_EQ(std::string(auth.action),
             std::string("Verify HIKVISION_USERNAME / HIKVISION_PASSWORD (or the per-camera "
                         "variables); repeated failures lock the camera for 30 minutes."));
    CHECK_EQ(auth.severity, Severity::kError);

    const ErrorInfo& subnet = describe(CameraError::kCameraSubnetUnconfigured);
    CHECK_EQ(std::string(subnet.action),
             std::string("Give the Jetson a static address in the cameras' subnet: make "
                         "camera-lan-setup ADDRESS=<ip>/24."));

    const std::map<CameraError, Severity> expected = {
        {CameraError::kCameraLanNotFound, Severity::kError},
        {CameraError::kCameraLanHasDefaultRoute, Severity::kWarning},
        {CameraError::kSubnetConflict, Severity::kWarning},
        {CameraError::kNoInternetRoute, Severity::kInfo},
        {CameraError::kInternetOffline, Severity::kWarning},
        {CameraError::kOnvifDisabledOrUnavailable, Severity::kInfo},
        {CameraError::kCameraLimitReached, Severity::kWarning},
        {CameraError::kCameraDisabled, Severity::kInfo},
        {CameraError::kIsapiUnavailable, Severity::kInfo},
        {CameraError::kStreamTimeout, Severity::kWarning},
        {CameraError::kStreamEnded, Severity::kWarning},
        {CameraError::kHardwareDecoderUnavailable, Severity::kWarning},
        {CameraError::kTensorrtNotAvailable, Severity::kError},
        {CameraError::kOutOfMemoryRisk, Severity::kWarning},
        {CameraError::kNativeAnprUnavailable, Severity::kInfo},
        {CameraError::kDuplicateIpDetected, Severity::kError},
    };
    for (const auto& [error, severity] : expected) {
        CHECK_EQ(describe(error).severity, severity);
    }
}

TEST("diagnostics: stage names") {
    CHECK_EQ(toString(Stage::kNetwork), std::string("network"));
    CHECK_EQ(toString(Stage::kInternet), std::string("internet"));
    CHECK_EQ(toString(Stage::kDiscovery), std::string("discovery"));
    CHECK_EQ(toString(Stage::kOnvif), std::string("onvif"));
    CHECK_EQ(toString(Stage::kIsapi), std::string("isapi"));
    CHECK_EQ(toString(Stage::kRtsp), std::string("rtsp"));
    CHECK_EQ(toString(Stage::kDecode), std::string("decode"));
    CHECK_EQ(toString(Stage::kFrames), std::string("frames"));
    CHECK_EQ(toString(Stage::kInference), std::string("inference"));
    CHECK_EQ(toString(Stage::kSystem), std::string("system"));
}

TEST("diagnostics: reportCameraError writes one structured line") {
    std::string text;
    {
        StderrCapture capture;
        DiagnosticContext context;
        context.camera_id = "camera-03";
        context.ip = "192.168.10.23";
        context.stage = Stage::kRtsp;
        reportCameraError(
            CameraError::kRtspAuthFailed, context,
            "DESCRIBE rtsp://<redacted>@192.168.10.23:554/Streaming/Channels/101 -> 401");
        text = capture.text();
    }
    CHECK_EQ(std::count(text.begin(), text.end(), '\n'), 1);
    CHECK(contains(text, " level=error event=camera_error camera_id=camera-03 ip=192.168.10.23 "
                         "stage=rtsp error=RTSP_AUTH_FAILED detail=\"DESCRIBE "
                         "rtsp://<redacted>@192.168.10.23:554/Streaming/Channels/101 -> 401\" "
                         "action=\"Verify HIKVISION_USERNAME / HIKVISION_PASSWORD"));
}

TEST("diagnostics: network problems are network_error lines at the severity's level") {
    std::string text;
    {
        StderrCapture capture;
        DiagnosticContext context;
        context.interface = "eth0";
        context.stage = Stage::kNetwork;
        reportCameraError(CameraError::kCameraLanHasDefaultRoute, context, "");
        context.interface = "wwan0";
        context.stage = Stage::kInternet;
        reportCameraError(CameraError::kNoInternetRoute, context, "");
        text = capture.text();
    }
    CHECK(contains(text, " level=warn event=network_error interface=eth0 stage=network "
                         "error=CAMERA_LAN_HAS_DEFAULT_ROUTE action=\""));
    CHECK(contains(text, " level=info event=network_error interface=wwan0 stage=internet "
                         "error=NO_INTERNET_ROUTE action=\""));
    CHECK(!contains(text, "detail="));
    CHECK(!contains(text, "camera_id="));
    CHECK(!contains(text, " ip="));
}

TEST("diagnostics: a camera thread's log context is not repeated") {
    std::string text;
    {
        StderrCapture capture;
        const anpr::LogContext log_context("camera_id=camera-02");
        DiagnosticContext context;
        context.camera_id = "camera-02";
        context.stage = Stage::kFrames;
        reportCameraError(CameraError::kStreamTimeout, context, "no frame for 5000 ms");
        context.camera_id = "camera-020";
        reportCameraError(CameraError::kStreamTimeout, context, "");
        text = capture.text();
    }
    const std::size_t first_line_end = text.find('\n');
    const std::string first = text.substr(0, first_line_end);
    const std::string second = text.substr(first_line_end + 1);
    CHECK(contains(first, "event=camera_error camera_id=camera-02 stage=frames "
                          "error=STREAM_TIMEOUT detail=\"no frame for 5000 ms\""));
    CHECK_EQ(first.find("camera_id="), first.rfind("camera_id="));
    CHECK(contains(second, "camera_id=camera-02 camera_id=camera-020 stage=frames"));
}

TEST("diagnostics: formatError prints the human-readable block") {
    DiagnosticContext context;
    context.camera_id = "camera-03";
    context.ip = "192.168.10.23";
    context.stage = Stage::kRtsp;
    CHECK_EQ(formatError(CameraError::kRtspAuthFailed, context, "(401 after Digest login)"),
             std::string("ERROR RTSP_AUTH_FAILED  camera-03 192.168.10.23\n"
                         "  The camera rejected the configured username/password.\n"
                         "  detail: (401 after Digest login)\n"
                         "  action: Verify HIKVISION_USERNAME / HIKVISION_PASSWORD (or the "
                         "per-camera variables); repeated failures lock the camera for 30 "
                         "minutes.\n"));

    DiagnosticContext lan;
    lan.interface = "eth0";
    lan.stage = Stage::kNetwork;
    CHECK_EQ(formatError(CameraError::kInternetOffline, lan, ""),
             std::string("WARNING INTERNET_OFFLINE  eth0\n"
                         "  A default route exists but the Internet probe failed. Camera "
                         "processing continues.\n"
                         "  action: Check the GSM modem signal, SIM card and APN.\n"));
    CHECK_EQ(formatError(CameraError::kCameraDisabled, DiagnosticContext{}, "").substr(0, 21),
             std::string("INFO CAMERA_DISABLED\n"));
}

// ---------------------------------------------------------------------------------------------
// Reconnect policy
// ---------------------------------------------------------------------------------------------

namespace {

/// The delay as a plain value (-1 when the policy gives up), so CHECK_EQ never binds a reference
/// into a temporary optional.
std::int64_t delayAfter(ReconnectPolicy& policy, CameraError error) {
    return policy.onFailure(error).value_or(-1);
}

}  // namespace

TEST("reconnect: failures are classified") {
    CHECK_EQ(classifyFailure(CameraError::kRtspAuthFailed), FailureClass::kAuthentication);
    CHECK_EQ(classifyFailure(CameraError::kRtspCredentialsMissing), FailureClass::kAuthentication);
    for (const CameraError error :
         {CameraError::kRtspStreamPathInvalid, CameraError::kUnsupportedCodec,
          CameraError::kCameraNotActivated, CameraError::kCameraOnOtherSubnet,
          CameraError::kDuplicateIpDetected}) {
        CHECK_EQ(classifyFailure(error), FailureClass::kConfiguration);
    }
    for (const CameraError error :
         {CameraError::kNone, CameraError::kCameraUnreachable, CameraError::kRtspPortClosed,
          CameraError::kRtspProtocolError, CameraError::kStreamOpenFailed,
          CameraError::kNoFramesReceived, CameraError::kStreamTimeout, CameraError::kStreamEnded,
          CameraError::kCameraLanLinkDown, CameraError::kHardwareDecoderUnavailable}) {
        CHECK_EQ(classifyFailure(error), FailureClass::kTransient);
    }
}

TEST("reconnect: transient failures back off exponentially up to the cap") {
    ReconnectPolicy policy(ReconnectSettings{});
    const std::int64_t expected[] = {1000, 2000, 4000, 8000, 16000, 30000, 30000, 30000};
    for (const std::int64_t delay : expected) {
        const auto next = policy.onFailure(CameraError::kStreamTimeout);
        CHECK(next.has_value());
        CHECK_EQ(*next, delay);
    }
    CHECK_EQ(policy.consecutiveFailures(), 8);
    CHECK_EQ(policy.lastError(), CameraError::kStreamTimeout);
    CHECK(!policy.exhausted());

    policy.onSuccess();
    CHECK_EQ(policy.consecutiveFailures(), 0);
    CHECK_EQ(policy.lastError(), CameraError::kNone);
    CHECK_EQ(delayAfter(policy, CameraError::kStreamEnded), std::int64_t{1000});
    CHECK_EQ(delayAfter(policy, CameraError::kCameraUnreachable), std::int64_t{2000});
}

TEST("reconnect: an odd cap and a huge cap never overflow") {
    ReconnectSettings settings;
    settings.initial_backoff_ms = 300;
    settings.max_backoff_ms = 1000;
    ReconnectPolicy policy(settings);
    const std::int64_t expected[] = {300, 600, 1000, 1000};
    for (const std::int64_t delay : expected) {
        CHECK_EQ(delayAfter(policy, CameraError::kStreamTimeout), delay);
    }

    settings.initial_backoff_ms = 1;
    settings.max_backoff_ms = std::numeric_limits<std::int64_t>::max();
    ReconnectPolicy unbounded(settings);
    std::int64_t previous = 0;
    for (int i = 0; i < 200; ++i) {
        const std::int64_t delay = delayAfter(unbounded, CameraError::kStreamTimeout);
        CHECK(delay >= previous);
        CHECK(delay > 0);
        previous = delay;
    }
    CHECK_EQ(previous, std::numeric_limits<std::int64_t>::max());
}

TEST("reconnect: authentication failures retry slowly, then stop") {
    ReconnectSettings settings;
    settings.auth_retry_interval_ms = 900000;
    settings.auth_max_retries = 2;
    ReconnectPolicy policy(settings);
    CHECK_EQ(delayAfter(policy, CameraError::kRtspAuthFailed), std::int64_t{900000});
    CHECK(!policy.exhausted());
    // A link blip in between neither resets nor consumes the authentication budget.
    CHECK_EQ(delayAfter(policy, CameraError::kCameraUnreachable), std::int64_t{1000});
    CHECK_EQ(delayAfter(policy, CameraError::kRtspAuthFailed), std::int64_t{900000});
    CHECK(!policy.exhausted());
    CHECK(!policy.onFailure(CameraError::kRtspAuthFailed).has_value());
    CHECK(policy.exhausted());
    CHECK_EQ(policy.lastError(), CameraError::kRtspAuthFailed);
    // Once exhausted, nothing is retried any more.
    CHECK(!policy.onFailure(CameraError::kStreamTimeout).has_value());
    CHECK_EQ(policy.consecutiveFailures(), 5);

    policy.onSuccess();
    CHECK(!policy.exhausted());
    CHECK_EQ(delayAfter(policy, CameraError::kRtspCredentialsMissing), std::int64_t{900000});
}

TEST("reconnect: zero authentication retries stop at the first rejection") {
    ReconnectSettings settings;
    settings.auth_max_retries = 0;
    ReconnectPolicy policy(settings);
    CHECK(!policy.onFailure(CameraError::kRtspAuthFailed).has_value());
    CHECK(policy.exhausted());
}

TEST("reconnect: configuration failures retry at their own slow interval, forever") {
    ReconnectSettings settings;
    settings.configuration_retry_interval_ms = 120000;
    ReconnectPolicy policy(settings);
    for (int i = 0; i < 20; ++i) {
        const auto delay = policy.onFailure(i % 2 == 0 ? CameraError::kRtspStreamPathInvalid
                                                       : CameraError::kUnsupportedCodec);
        CHECK(delay.has_value());
        CHECK_EQ(*delay, std::int64_t{120000});
    }
    CHECK(!policy.exhausted());
    // The transient schedule was not advanced by configuration failures.
    CHECK_EQ(delayAfter(policy, CameraError::kStreamTimeout), std::int64_t{1000});
}

// ---------------------------------------------------------------------------------------------
// Status store
// ---------------------------------------------------------------------------------------------

namespace {

StatusSnapshot sampleSnapshot() {
    StatusSnapshot snapshot;
    snapshot.updated_at = "2026-10-09T10:15:00Z";
    snapshot.updated_unix_ms = 1791540900123;
    snapshot.pid = 4242;
    snapshot.camera_lan = "eth0 192.168.10.5/24";
    snapshot.internet = "wwan0 ONLINE";
    snapshot.system.mem_total_mb = 3964;
    snapshot.system.mem_available_mb = 1210;
    snapshot.system.process_rss_mb = 812;
    snapshot.system.container_mem_mb = 1500;
    snapshot.system.load1 = 2.75;
    snapshot.system.cpu_percent = 63.5;
    snapshot.system.gpu_percent = 41.25;
    snapshot.system.detector_backend = "tensorrt";
    snapshot.system.ocr_backend = "tensorrt";

    CameraStatus running;
    running.id = "camera-01";
    running.ip = "192.168.10.21";
    running.mac = "44:19:b6:00:00:01";
    running.model = "DS-TCG406-E";
    running.link = "OK";
    running.rtsp = "OK";
    running.video = "H264";
    running.decoder = "nvv4l2decoder";
    running.width = 2688;
    running.height = 1520;
    running.anpr = "RUNNING";
    running.input_fps = 25.0;
    running.processed_fps = 12.345;
    running.detector_fps = 9.5;
    running.detector_avg_ms = 38.125;
    running.ocr_calls = 1234;
    running.ocr_avg_ms = 7.75;
    running.plates_confirmed = 17;
    running.live_dropped = 3301;
    running.stale_dropped = 12;
    running.reconnects = 2;
    running.capture_to_process_ms = 41.5;
    running.first_frame_ms = 812.25;
    running.last_plate = "123ABC02";
    running.last_plate_time = "2026-10-09T10:14:58Z";

    CameraStatus failing;
    failing.id = "camera-02";
    failing.ip = "192.168.10.22";
    failing.link = "OK";
    failing.rtsp = "AUTH";
    failing.anpr = "ERROR";
    failing.error = "RTSP_AUTH_FAILED";
    failing.action = "Verify \"HIKVISION_USERNAME\" \\ password\nnext line\ttab";
    failing.detail = "Казахстан \x01 control";

    snapshot.cameras = {running, failing};
    return snapshot;
}

bool sameCamera(const CameraStatus& a, const CameraStatus& b) {
    return a.id == b.id && a.ip == b.ip && a.mac == b.mac && a.model == b.model &&
           a.link == b.link && a.rtsp == b.rtsp && a.video == b.video && a.decoder == b.decoder &&
           a.width == b.width && a.height == b.height && a.anpr == b.anpr && a.error == b.error &&
           a.action == b.action && a.detail == b.detail && a.input_fps == b.input_fps &&
           a.processed_fps == b.processed_fps && a.detector_fps == b.detector_fps &&
           a.detector_avg_ms == b.detector_avg_ms && a.ocr_calls == b.ocr_calls &&
           a.ocr_avg_ms == b.ocr_avg_ms && a.plates_confirmed == b.plates_confirmed &&
           a.live_dropped == b.live_dropped && a.stale_dropped == b.stale_dropped &&
           a.reconnects == b.reconnects && a.capture_to_process_ms == b.capture_to_process_ms &&
           a.first_frame_ms == b.first_frame_ms && a.last_plate == b.last_plate &&
           a.last_plate_time == b.last_plate_time;
}

bool sameSnapshot(const StatusSnapshot& a, const StatusSnapshot& b) {
    if (a.updated_at != b.updated_at || a.updated_unix_ms != b.updated_unix_ms ||
        a.pid != b.pid || a.camera_lan != b.camera_lan || a.internet != b.internet ||
        a.cameras.size() != b.cameras.size()) {
        return false;
    }
    const SystemStatus& x = a.system;
    const SystemStatus& y = b.system;
    if (x.mem_total_mb != y.mem_total_mb || x.mem_available_mb != y.mem_available_mb ||
        x.process_rss_mb != y.process_rss_mb || x.container_mem_mb != y.container_mem_mb ||
        x.load1 != y.load1 || x.cpu_percent != y.cpu_percent || x.gpu_percent != y.gpu_percent ||
        x.detector_backend != y.detector_backend || x.ocr_backend != y.ocr_backend) {
        return false;
    }
    for (std::size_t i = 0; i < a.cameras.size(); ++i) {
        if (!sameCamera(a.cameras[i], b.cameras[i])) {
            return false;
        }
    }
    return true;
}

}  // namespace

TEST("status: JSON round trip keeps every field") {
    const StatusSnapshot original = sampleSnapshot();
    const std::string text = toJson(original);
    StatusSnapshot parsed;
    std::string error;
    CHECK(fromJson(text, parsed, error));
    CHECK_EQ(error, std::string{});
    CHECK(sameSnapshot(parsed, original));
    // Serialising the parsed snapshot again gives the same document.
    CHECK_EQ(toJson(parsed), text);
}

TEST("status: JSON is readable, escaped and free of NaN") {
    StatusSnapshot snapshot = sampleSnapshot();
    snapshot.cameras[0].input_fps = std::numeric_limits<double>::quiet_NaN();
    snapshot.system.gpu_percent = std::numeric_limits<double>::infinity();
    const std::string text = toJson(snapshot);
    CHECK(contains(text, "\n  \"updated_at\": \"2026-10-09T10:15:00Z\",\n"));
    CHECK(contains(text, "\"processed_fps\": 12.345"));
    CHECK(contains(text, "\"input_fps\": null"));
    CHECK(contains(text, "\"gpu_percent\": null"));
    CHECK(contains(text, "\"load1\": 2.75,"));
    CHECK(contains(text, "\"width\": 2688,"));
    CHECK(contains(text, "\\\"HIKVISION_USERNAME\\\" \\\\ password\\nnext line\\ttab"));
    CHECK(contains(text, "\\u0001"));
    CHECK(!contains(text, "nan"));
    CHECK(!contains(text, "inf"));

    StatusSnapshot parsed;
    std::string error;
    CHECK(fromJson(text, parsed, error));
    // Non-finite values fall back to the field defaults.
    CHECK_EQ(parsed.cameras[0].input_fps, 0.0);
    CHECK_EQ(parsed.system.gpu_percent, -1.0);
}

TEST("status: missing fields keep their defaults") {
    StatusSnapshot snapshot;
    std::string error;
    CHECK(fromJson("{}", snapshot, error));
    CHECK(snapshot.cameras.empty());
    CHECK_EQ(snapshot.system.container_mem_mb, std::int64_t{-1});
    CHECK_EQ(snapshot.system.cpu_percent, -1.0);

    CHECK(fromJson(R"({"updated_at": "x", "future_field": [1, 2],
                       "cameras": [{"id": "camera-07", "rtsp": "PORT"}]})",
                   snapshot, error));
    CHECK_EQ(snapshot.updated_at, std::string("x"));
    CHECK_EQ(snapshot.cameras.size(), std::size_t{1});
    const CameraStatus& camera = snapshot.cameras[0];
    CHECK_EQ(camera.id, std::string("camera-07"));
    CHECK_EQ(camera.rtsp, std::string("PORT"));
    CHECK_EQ(camera.link, std::string("-"));
    CHECK_EQ(camera.video, std::string("-"));
    CHECK_EQ(camera.anpr, std::string("-"));
    CHECK_EQ(camera.width, 0);
    CHECK_EQ(camera.reconnects, std::int64_t{0});
}

TEST("status: malformed documents are rejected without touching the snapshot") {
    StatusSnapshot snapshot = sampleSnapshot();
    std::string error;
    CHECK(!fromJson("{\"cameras\": [", snapshot, error));
    CHECK(contains(error, "malformed status file"));
    CHECK(!fromJson("[1, 2]", snapshot, error));
    CHECK(contains(error, "must be a JSON object"));
    CHECK(!fromJson("{\"cameras\": {}}", snapshot, error));
    CHECK(contains(error, "'cameras' must be an array"));
    CHECK(!fromJson("{\"cameras\": [1]}", snapshot, error));
    CHECK(!fromJson("{\"system\": 5}", snapshot, error));
    CHECK(contains(error, "'system' must be an object"));
    CHECK(!fromJson("", snapshot, error));
    CHECK_EQ(snapshot.cameras.size(), std::size_t{2});
    CHECK_EQ(snapshot.pid, 4242);
}

TEST("status: the status file is written atomically and read back") {
    const anpr::filesystem::path dir = freshTempDir("status");
    const std::string path = (dir / "cameras" / "status.json").string();
    StatusSnapshot read_back;
    std::string error;
    CHECK(!readStatusFile(path, read_back, error));
    CHECK(contains(error, "cannot open"));

    const StatusSnapshot snapshot = sampleSnapshot();
    CHECK(writeStatusFile(path, snapshot, error));
    CHECK_EQ(error, std::string{});
    CHECK(directoryEntries(dir / "cameras") == std::vector<std::string>({"status.json"}));
    CHECK(readStatusFile(path, read_back, error));
    CHECK(sameSnapshot(read_back, snapshot));

    StatusSnapshot second = snapshot;
    second.cameras.pop_back();
    second.pid = 1;
    CHECK(writeStatusFile(path, second, error));
    CHECK(readStatusFile(path, read_back, error));
    CHECK_EQ(read_back.cameras.size(), std::size_t{1});
    CHECK_EQ(read_back.pid, 1);

    {
        std::ofstream corrupt(path, std::ios::trunc);
        corrupt << "{\"pid\": ";
    }
    CHECK(!readStatusFile(path, read_back, error));
    CHECK(contains(error, path));

    const std::string blocked = (dir / "blocked").string();
    anpr::filesystem::create_directories(blocked);
    CHECK(!writeStatusFile(blocked, snapshot, error));
    CHECK(!error.empty());
    CHECK(!readStatusFile(blocked, read_back, error));

    std::error_code ec;
    anpr::filesystem::remove_all(dir, ec);
}

TEST("status: the table fits its columns to the content") {
    const StatusSnapshot snapshot = sampleSnapshot();
    const std::string expected =
        "updated 2026-10-09T10:15:00Z  camera LAN: eth0 192.168.10.5/24  Internet: wwan0 ONLINE\n"
        "CAMERA     IP             LINK  RTSP  VIDEO  ANPR     ERROR\n"
        "camera-01  192.168.10.21  OK    OK    H264   RUNNING\n"
        "camera-02  192.168.10.22  OK    AUTH  -      ERROR    RTSP_AUTH_FAILED\n";
    CHECK_EQ(formatStatusTable(snapshot), expected);

    StatusSnapshot wide = snapshot;
    wide.cameras[0].id = "gate-entrance-north";
    wide.cameras[0].anpr = "RECONNECTING";
    const std::string table = formatStatusTable(wide);
    CHECK(contains(table, "\nCAMERA               IP             LINK  RTSP  VIDEO  ANPR          "
                          "ERROR\n"));
    CHECK(contains(table,
                   "\ngate-entrance-north  192.168.10.21  OK    OK    H264   RECONNECTING\n"));
    CHECK(contains(table, "\ncamera-02            192.168.10.22  OK    AUTH  -      ERROR         "
                          "RTSP_AUTH_FAILED\n"));
}

TEST("status: an empty camera list prints no cameras") {
    StatusSnapshot snapshot;
    CHECK_EQ(formatStatusTable(snapshot),
             std::string("updated -  camera LAN: -  Internet: -\nno cameras\n"));
    snapshot.updated_at = "2026-10-09T10:15:00Z";
    snapshot.camera_lan = "CAMERA_LAN_NOT_FOUND";
    snapshot.internet = "wwan0 OFFLINE";
    CHECK_EQ(formatStatusTable(snapshot),
             std::string("updated 2026-10-09T10:15:00Z  camera LAN: CAMERA_LAN_NOT_FOUND  "
                         "Internet: wwan0 OFFLINE\nno cameras\n"));
}
