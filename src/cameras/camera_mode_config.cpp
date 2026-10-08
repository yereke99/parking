#include "anpr/cameras/camera_mode_config.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <utility>

#include "anpr/common/yaml.hpp"
#include "anpr/net/socket.hpp"

namespace anpr::cameras {
namespace {

bool parseStream(const std::string& text, StreamSelection& out) {
    if (text == "main") {
        out = StreamSelection::kMain;
        return true;
    }
    if (text == "sub") {
        out = StreamSelection::kSub;
        return true;
    }
    return false;
}

bool parseDecoder(const std::string& text, DecoderPreference& out) {
    if (text == "auto") {
        out = DecoderPreference::kAuto;
        return true;
    }
    if (text == "nvidia_hardware") {
        out = DecoderPreference::kNvidiaHardware;
        return true;
    }
    if (text == "software") {
        out = DecoderPreference::kSoftware;
        return true;
    }
    return false;
}

bool parseScanMode(const std::string& text, ScanMode& out) {
    if (text == "auto") {
        out = ScanMode::kAuto;
        return true;
    }
    if (text == "always") {
        out = ScanMode::kAlways;
        return true;
    }
    if (text == "never") {
        out = ScanMode::kNever;
        return true;
    }
    return false;
}

/// Reads typed values out of a YAML mapping while recording which keys were understood, so
/// unrecognised keys are reported instead of silently ignored. The same reader serves the
/// document root (dotted paths) and each `cameras` entry (plain keys, reported as
/// `cameras[i].key`). The first problem wins and is reported as "INVALID_CONFIG: path: message".
class Reader {
public:
    Reader(const yaml::Node& root, std::string prefix, std::string& error)
        : root_(root), prefix_(std::move(prefix)), error_(error) {}

    void get(const std::string& path, std::string& out) {
        const yaml::Node* node = consume(path);
        if (node == nullptr) {
            return;
        }
        if (const auto value = yaml::asString(node)) {
            out = *value;
        } else {
            fail(path, "expected a string");
        }
    }

    void get(const std::string& path, bool& out) {
        const yaml::Node* node = consume(path);
        if (node == nullptr) {
            return;
        }
        if (const auto value = yaml::asBool(node)) {
            out = *value;
        } else {
            fail(path, "expected true or false");
        }
    }

    void get(const std::string& path, int& out) {
        std::int64_t value = out;
        if (readInteger(path, value, std::numeric_limits<int>::min(),
                        std::numeric_limits<int>::max())) {
            out = static_cast<int>(value);
        }
    }

    void get(const std::string& path, std::int64_t& out) {
        readInteger(path, out, std::numeric_limits<std::int64_t>::min(),
                    std::numeric_limits<std::int64_t>::max());
    }

    void get(const std::string& path, std::optional<bool>& out) {
        bool value = false;
        if (present(path)) {
            get(path, value);
            if (ok()) {
                out = value;
            }
        } else {
            consume(path);
        }
    }

    void get(const std::string& path, std::optional<int>& out) {
        int value = 0;
        if (present(path)) {
            get(path, value);
            if (ok()) {
                out = value;
            }
        } else {
            consume(path);
        }
    }

    /// A list of strings, written either as `[a, b]` or as a block of `- a` lines.
    void get(const std::string& path, std::vector<std::string>& out) {
        const yaml::Node* node = consume(path);
        if (node == nullptr) {
            return;
        }
        if (!node->isSequence()) {
            fail(path, "expected a list such as [a, b]");
            return;
        }
        std::vector<std::string> parsed;
        for (const yaml::Node& item : node->sequence()) {
            const auto value = yaml::asString(&item);
            if (!value) {
                fail(path, "expected a list of strings");
                return;
            }
            parsed.push_back(*value);
        }
        out = std::move(parsed);
    }

    void get(const std::string& path, std::vector<int>& out) {
        const yaml::Node* node = consume(path);
        if (node == nullptr) {
            return;
        }
        if (!node->isSequence()) {
            fail(path, "expected a list such as [554, 80]");
            return;
        }
        std::vector<int> parsed;
        for (const yaml::Node& item : node->sequence()) {
            const auto value = yaml::asInt(&item);
            if (!value || *value < std::numeric_limits<int>::min() ||
                *value > std::numeric_limits<int>::max()) {
                fail(path, "expected a list of integers");
                return;
            }
            parsed.push_back(static_cast<int>(*value));
        }
        out = std::move(parsed);
    }

    /// An enum written as a string; `parse` maps the text and `allowed` names the choices.
    template <typename Enum, typename Parse>
    void getEnum(const std::string& path, Enum& out, Parse parse, const char* allowed) {
        const yaml::Node* node = consume(path);
        if (node == nullptr) {
            return;
        }
        const auto text = yaml::asString(node);
        Enum value = out;
        if (!text || !parse(*text, value)) {
            fail(path, std::string("must be one of ") + allowed);
            return;
        }
        out = value;
    }

    template <typename Enum, typename Parse>
    void getEnum(const std::string& path, std::optional<Enum>& out, Parse parse,
                 const char* allowed) {
        if (!present(path)) {
            consume(path);
            return;
        }
        Enum value{};
        getEnum(path, value, parse, allowed);
        if (ok()) {
            out = value;
        }
    }

    /// Marks a subtree as understood without reading it here.
    const yaml::Node* claimSubtree(const std::string& path) {
        claimed_subtrees_.insert(path);
        return root_.path(path);
    }

    void fail(const std::string& path, const std::string& message) {
        if (error_.empty()) {
            error_ = "INVALID_CONFIG: " + prefix_ + path + ": " + message;
        }
    }

    [[nodiscard]] bool ok() const { return error_.empty(); }

    /// Every leaf of the mapping that no getter and no claimed subtree accounted for.
    [[nodiscard]] std::vector<std::string> unknownKeys() const {
        std::vector<std::string> leaves;
        collectLeafPaths(root_, {}, leaves);
        std::vector<std::string> unknown;
        for (const std::string& leaf : leaves) {
            if (consumed_.count(leaf) > 0) {
                continue;
            }
            const bool claimed = std::any_of(
                claimed_subtrees_.begin(), claimed_subtrees_.end(),
                [&leaf](const std::string& prefix) { return startsWithPath(leaf, prefix); });
            if (!claimed) {
                unknown.push_back(prefix_ + leaf);
            }
        }
        return unknown;
    }

private:
    const yaml::Node& root_;
    std::string prefix_;
    std::string& error_;
    std::set<std::string> consumed_;
    std::set<std::string> claimed_subtrees_;

    const yaml::Node* consume(const std::string& path) {
        consumed_.insert(path);
        const yaml::Node* node = root_.path(path);
        if (node == nullptr || node->isNull()) {
            return nullptr;
        }
        return node;
    }

    [[nodiscard]] bool present(const std::string& path) const {
        const yaml::Node* node = root_.path(path);
        return node != nullptr && !node->isNull();
    }

    bool readInteger(const std::string& path, std::int64_t& out, std::int64_t min,
                     std::int64_t max) {
        const yaml::Node* node = consume(path);
        if (node == nullptr) {
            return false;
        }
        const auto value = yaml::asInt(node);
        if (!value) {
            fail(path, "expected an integer");
            return false;
        }
        if (*value < min || *value > max) {
            fail(path, "integer out of range");
            return false;
        }
        out = static_cast<std::int64_t>(*value);
        return true;
    }

    static void collectLeafPaths(const yaml::Node& node, const std::string& prefix,
                                 std::vector<std::string>& out) {
        if (node.isMap()) {
            for (const auto& [key, child] : node.map()) {
                const std::string path = prefix.empty() ? key : prefix + "." + key;
                if (child.isMap()) {
                    collectLeafPaths(child, path, out);
                } else {
                    out.push_back(path);
                }
            }
            return;
        }
        if (!prefix.empty()) {
            out.push_back(prefix);
        }
    }

    static bool startsWithPath(const std::string& path, const std::string& prefix) {
        return path == prefix || (path.size() > prefix.size() &&
                                  path.compare(0, prefix.size(), prefix) == 0 &&
                                  path[prefix.size()] == '.');
    }
};

constexpr const char* kStreamChoices = "main or sub";
constexpr const char* kDecoderChoices = "auto, nvidia_hardware or software";
constexpr const char* kScanChoices = "auto, always or never";

void readCameraEntry(const yaml::Node& node, std::size_t index, CameraOverride& out,
                     std::vector<std::string>& unknown_keys, std::string& error) {
    const std::string prefix = "cameras[" + std::to_string(index) + "].";
    if (!node.isMap()) {
        error = "INVALID_CONFIG: cameras[" + std::to_string(index) +
                "]: expected a mapping with id, mac, serial or ip";
        return;
    }
    Reader reader(node, prefix, error);
    reader.get("id", out.id);
    reader.get("mac", out.mac);
    if (reader.ok() && !out.mac.empty()) {
        const std::string normalized = net::normalizeMac(out.mac);
        if (normalized.empty()) {
            reader.fail("mac", "'" + out.mac + "' is not a MAC address");
            return;
        }
        out.mac = normalized;
    }
    reader.get("serial", out.serial);
    reader.get("ip", out.ip);
    reader.get("enabled", out.enabled);
    reader.get("rtsp_port", out.rtsp_port);
    reader.getEnum("stream", out.stream, parseStream, kStreamChoices);
    reader.get("rtsp_path", out.rtsp_path);
    reader.get("http_port", out.http_port);
    reader.get("username_env", out.username_env);
    reader.get("password_env", out.password_env);
    reader.getEnum("decoder", out.decoder, parseDecoder, kDecoderChoices);
    reader.get("anpr_config", out.anpr_config);
    reader.get("native_anpr", out.native_anpr);
    if (!reader.ok()) {
        return;
    }
    for (std::string& key : reader.unknownKeys()) {
        unknown_keys.push_back(std::move(key));
    }
}

void readAll(Reader& reader, const yaml::Node& root, CameraModeConfig& config,
             std::vector<std::string>& camera_unknown_keys, std::string& error) {
    NetworkSettings& network = config.network;
    reader.get("network.interface", network.interface);
    reader.get("network.internet_check", network.internet_check);
    reader.get("network.internet_probe_targets", network.internet_probe_targets);
    reader.get("network.internet_timeout_ms", network.internet_timeout_ms);

    DiscoverySettings& discovery = config.discovery;
    reader.get("discovery.enabled", discovery.enabled);
    reader.get("discovery.sadp", discovery.sadp);
    reader.get("discovery.onvif", discovery.onvif);
    reader.get("discovery.arp_table", discovery.arp_table);
    reader.getEnum("discovery.subnet_scan", discovery.subnet_scan, parseScanMode, kScanChoices);
    reader.get("discovery.scan_max_hosts", discovery.scan_max_hosts);
    reader.get("discovery.scan_ports", discovery.scan_ports);
    reader.get("discovery.listen_ms", discovery.listen_ms);
    reader.get("discovery.connect_timeout_ms", discovery.connect_timeout_ms);
    reader.get("discovery.manual_hosts", discovery.manual_hosts);
    reader.get("discovery.max_cameras", discovery.max_cameras);
    reader.get("discovery.registry_file", discovery.registry_file);
    reader.get("discovery.duplicate_ip_check", discovery.duplicate_ip_check);
    reader.get("discovery.rescan_interval_ms", discovery.rescan_interval_ms);

    RtspSettings& rtsp = config.rtsp;
    reader.get("rtsp.port", rtsp.port);
    reader.getEnum("rtsp.stream", rtsp.stream, parseStream, kStreamChoices);
    reader.get("rtsp.main_path", rtsp.main_path);
    reader.get("rtsp.sub_path", rtsp.sub_path);
    reader.get("rtsp.username_env", rtsp.username_env);
    reader.get("rtsp.password_env", rtsp.password_env);
    reader.get("rtsp.timeout_ms", rtsp.timeout_ms);
    reader.get("rtsp.latency_ms", rtsp.latency_ms);

    reader.getEnum("decode.decoder", config.decode.decoder, parseDecoder, kDecoderChoices);
    reader.get("decode.max_width", config.decode.max_width);
    reader.get("decode.max_fps", config.decode.max_fps);

    CaptureSettings& capture = config.capture;
    reader.get("capture.queue_size", capture.queue_size);
    reader.get("capture.first_frame_timeout_ms", capture.first_frame_timeout_ms);
    reader.get("capture.read_timeout_ms", capture.read_timeout_ms);
    reader.get("capture.max_frame_age_ms", capture.max_frame_age_ms);
    reader.get("capture.reconnect_initial_backoff_ms", capture.reconnect_initial_backoff_ms);
    reader.get("capture.reconnect_max_backoff_ms", capture.reconnect_max_backoff_ms);
    reader.get("capture.auth_retry_interval_ms", capture.auth_retry_interval_ms);
    reader.get("capture.auth_max_retries", capture.auth_max_retries);
    reader.get("capture.configuration_retry_interval_ms",
               capture.configuration_retry_interval_ms);
    reader.get("capture.check_duration_ms", capture.check_duration_ms);

    RuntimeSettings& runtime = config.runtime;
    reader.get("runtime.max_active_cameras", runtime.max_active_cameras);
    reader.get("runtime.status_file", runtime.status_file);
    reader.get("runtime.status_interval_ms", runtime.status_interval_ms);
    reader.get("runtime.metrics_interval_ms", runtime.metrics_interval_ms);
    reader.get("runtime.min_available_ram_mb", runtime.min_available_ram_mb);

    reader.get("onvif.username_env", config.onvif.username_env);
    reader.get("onvif.password_env", config.onvif.password_env);

    reader.get("native_anpr.enabled", config.native_anpr.enabled);

    if (!reader.ok()) {
        return;
    }

    // `cameras` is a list of mappings, which the leaf walk would see as one opaque value: each
    // entry is read (and checked for unknown keys) on its own.
    reader.claimSubtree("cameras");
    const yaml::Node* cameras = root.find("cameras");
    if (cameras == nullptr || cameras->isNull()) {
        return;
    }
    if (!cameras->isSequence()) {
        error = "INVALID_CONFIG: cameras: expected a list of camera entries (- id: ...)";
        return;
    }
    std::vector<CameraOverride> parsed;
    for (std::size_t i = 0; i < cameras->sequence().size(); ++i) {
        CameraOverride entry;
        readCameraEntry(cameras->sequence()[i], i, entry, camera_unknown_keys, error);
        if (!error.empty()) {
            return;
        }
        parsed.push_back(std::move(entry));
    }
    config.cameras = std::move(parsed);
}

bool isCameraId(const std::string& id) {
    return !id.empty() && std::all_of(id.begin(), id.end(), [](unsigned char ch) {
        return std::isalnum(ch) != 0 || ch == '-' || ch == '_';
    });
}

/// Names an `--env-file` line can set: a letter or '_' first, then letters, digits and '_'.
bool isEnvName(const std::string& name) {
    if (name.empty() || std::isdigit(static_cast<unsigned char>(name.front())) != 0) {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](unsigned char ch) {
        return std::isalnum(ch) != 0 || ch == '_';
    });
}

/// "auto" or a Linux interface name: at most 15 bytes (IFNAMSIZ - 1), no whitespace or '/'.
bool isInterfaceSetting(const std::string& name) {
    if (name == "auto") {
        return true;
    }
    return !name.empty() && name.size() <= 15 &&
           std::none_of(name.begin(), name.end(), [](unsigned char ch) {
               return std::isspace(ch) != 0 || ch == '/' || std::iscntrl(ch) != 0;
           });
}

bool validPort(long long port) {
    return port >= 1 && port <= 65535;
}

/// Looks up the username variable; a set, non-empty username makes the pair usable.
bool tryCredentials(const EnvLookup& env, const std::string& username_var,
                    const std::string& password_var, ResolvedCredentials& out) {
    if (username_var.empty() || !env) {
        return false;
    }
    const auto username = env(username_var);
    if (!username || username->empty()) {
        return false;
    }
    out.present = true;
    out.credentials.username = *username;
    out.credentials.password.clear();
    if (!password_var.empty()) {
        if (const auto password = env(password_var)) {
            out.credentials.password = *password;
        }
    }
    out.source = username_var + "/" + password_var;
    return true;
}

}  // namespace

std::string toString(StreamSelection stream) {
    switch (stream) {
        case StreamSelection::kMain:
            return "main";
        case StreamSelection::kSub:
            return "sub";
    }
    return "main";
}

std::string toString(DecoderPreference decoder) {
    switch (decoder) {
        case DecoderPreference::kAuto:
            return "auto";
        case DecoderPreference::kNvidiaHardware:
            return "nvidia_hardware";
        case DecoderPreference::kSoftware:
            return "software";
    }
    return "auto";
}

std::string toString(ScanMode mode) {
    switch (mode) {
        case ScanMode::kAuto:
            return "auto";
        case ScanMode::kAlways:
            return "always";
        case ScanMode::kNever:
            return "never";
    }
    return "auto";
}

CameraModeConfigResult loadCameraModeConfigText(const std::string& text) {
    CameraModeConfigResult result;

    yaml::ParseResult parsed = yaml::parse(text);
    if (!parsed.ok) {
        result.ok = false;
        result.error = "INVALID_CONFIG: " + parsed.error;
        return result;
    }
    if (!parsed.root.isMap() && !parsed.root.isNull()) {
        result.ok = false;
        result.error = "INVALID_CONFIG: the document root must be a mapping";
        return result;
    }

    std::string error;
    Reader reader(parsed.root, {}, error);
    std::vector<std::string> camera_unknown_keys;
    readAll(reader, parsed.root, result.config, camera_unknown_keys, error);
    if (!error.empty()) {
        result.ok = false;
        result.error = error;
        return result;
    }

    result.unknown_keys = reader.unknownKeys();
    result.unknown_keys.insert(result.unknown_keys.end(), camera_unknown_keys.begin(),
                               camera_unknown_keys.end());

    if (!validateCameraModeConfig(result.config, result.error)) {
        result.ok = false;
    }
    return result;
}

CameraModeConfigResult loadCameraModeConfigFile(const std::string& path) {
    std::ifstream input(path);
    if (!input) {
        CameraModeConfigResult result;
        result.ok = false;
        result.error = "INVALID_CONFIG: cannot open " + path;
        return result;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return loadCameraModeConfigText(buffer.str());
}

bool validateCameraModeConfig(const CameraModeConfig& config, std::string& error) {
    auto require = [&error](bool condition, const std::string& path, const std::string& message) {
        if (!condition && error.empty()) {
            error = "INVALID_CONFIG: " + path + ": " + message;
        }
        return condition;
    };
    auto requireEnvName = [&require](const std::string& name, const std::string& path) {
        return require(isEnvName(name), path,
                       "'" + name + "' is not a valid environment variable name");
    };

    const NetworkSettings& network = config.network;
    if (!require(isInterfaceSetting(network.interface), "network.interface",
                 "'" + network.interface + "' must be auto or an interface name such as eth0")) {
        return false;
    }
    if (!require(network.internet_timeout_ms > 0, "network.internet_timeout_ms",
                 "must be positive")) {
        return false;
    }
    for (const std::string& target : network.internet_probe_targets) {
        net::Ipv4 host;
        std::uint16_t port = 0;
        if (!require(target.find(':') != std::string::npos &&
                         net::parseHostPort(target, host, port),
                     "network.internet_probe_targets",
                     "'" + target + "' must be an IPv4 address and port such as 1.1.1.1:53")) {
            return false;
        }
    }

    const DiscoverySettings& discovery = config.discovery;
    if (!require(discovery.scan_max_hosts >= 1 && discovery.scan_max_hosts <= 65536,
                 "discovery.scan_max_hosts", "must be between 1 and 65536")) {
        return false;
    }
    if (!require(discovery.subnet_scan == ScanMode::kNever || !discovery.scan_ports.empty(),
                 "discovery.scan_ports",
                 "must list at least one port unless subnet_scan is never")) {
        return false;
    }
    for (const int port : discovery.scan_ports) {
        if (!require(validPort(port), "discovery.scan_ports",
                     "port " + std::to_string(port) + " is outside 1..65535")) {
            return false;
        }
    }
    if (!require(discovery.listen_ms >= 100 && discovery.listen_ms <= 30000,
                 "discovery.listen_ms", "must be between 100 and 30000")) {
        return false;
    }
    if (!require(discovery.connect_timeout_ms > 0, "discovery.connect_timeout_ms",
                 "must be positive")) {
        return false;
    }
    for (const std::string& host : discovery.manual_hosts) {
        net::Ipv4 address;
        std::uint16_t port = 0;
        if (!require(net::parseHostPort(host, address, port), "discovery.manual_hosts",
                     "'" + host + "' must be an IPv4 address, optionally with :port")) {
            return false;
        }
    }
    if (!require(discovery.max_cameras >= 1, "discovery.max_cameras", "must be at least 1")) {
        return false;
    }
    if (!require(!discovery.registry_file.empty(), "discovery.registry_file",
                 "must name a file")) {
        return false;
    }
    if (!require(discovery.rescan_interval_ms >= 0, "discovery.rescan_interval_ms",
                 "must not be negative (0 disables rescans)")) {
        return false;
    }

    const RtspSettings& rtsp = config.rtsp;
    if (!require(validPort(rtsp.port), "rtsp.port", "must be between 1 and 65535")) {
        return false;
    }
    if (!require(!rtsp.main_path.empty() && rtsp.main_path.front() == '/', "rtsp.main_path",
                 "must start with '/'")) {
        return false;
    }
    if (!require(!rtsp.sub_path.empty() && rtsp.sub_path.front() == '/', "rtsp.sub_path",
                 "must start with '/'")) {
        return false;
    }
    if (!requireEnvName(rtsp.username_env, "rtsp.username_env") ||
        !requireEnvName(rtsp.password_env, "rtsp.password_env")) {
        return false;
    }
    if (!require(rtsp.timeout_ms > 0, "rtsp.timeout_ms", "must be positive")) {
        return false;
    }
    if (!require(rtsp.latency_ms >= 0, "rtsp.latency_ms", "must not be negative")) {
        return false;
    }

    if (!require(config.decode.max_width >= 0, "decode.max_width",
                 "must not be negative (0 keeps the size)")) {
        return false;
    }
    if (!require(config.decode.max_fps >= 0, "decode.max_fps",
                 "must not be negative (0 keeps the camera's rate)")) {
        return false;
    }

    const CaptureSettings& capture = config.capture;
    if (!require(capture.queue_size >= 1 && capture.queue_size <= 8, "capture.queue_size",
                 "must be between 1 and 8")) {
        return false;
    }
    const std::pair<std::int64_t, const char*> positive[] = {
        {capture.first_frame_timeout_ms, "capture.first_frame_timeout_ms"},
        {capture.read_timeout_ms, "capture.read_timeout_ms"},
        {capture.max_frame_age_ms, "capture.max_frame_age_ms"},
        {capture.reconnect_initial_backoff_ms, "capture.reconnect_initial_backoff_ms"},
        {capture.reconnect_max_backoff_ms, "capture.reconnect_max_backoff_ms"},
        {capture.auth_retry_interval_ms, "capture.auth_retry_interval_ms"},
        {capture.configuration_retry_interval_ms, "capture.configuration_retry_interval_ms"},
        {capture.check_duration_ms, "capture.check_duration_ms"},
        {config.runtime.status_interval_ms, "runtime.status_interval_ms"},
        {config.runtime.metrics_interval_ms, "runtime.metrics_interval_ms"},
    };
    for (const auto& [value, path] : positive) {
        if (!require(value > 0, path, "must be positive")) {
            return false;
        }
    }
    if (!require(capture.reconnect_max_backoff_ms >= capture.reconnect_initial_backoff_ms,
                 "capture.reconnect_max_backoff_ms",
                 "must not be shorter than reconnect_initial_backoff_ms")) {
        return false;
    }
    if (!require(capture.auth_max_retries >= 0, "capture.auth_max_retries",
                 "must not be negative")) {
        return false;
    }

    const RuntimeSettings& runtime = config.runtime;
    if (!require(runtime.max_active_cameras >= 1, "runtime.max_active_cameras",
                 "must be at least 1")) {
        return false;
    }
    if (!require(!runtime.status_file.empty(), "runtime.status_file", "must name a file")) {
        return false;
    }
    if (!require(runtime.min_available_ram_mb >= 0, "runtime.min_available_ram_mb",
                 "must not be negative")) {
        return false;
    }

    // ONVIF variable names may be emptied to switch ONVIF credentials off entirely.
    if (!config.onvif.username_env.empty() &&
        !requireEnvName(config.onvif.username_env, "onvif.username_env")) {
        return false;
    }
    if (!config.onvif.password_env.empty() &&
        !requireEnvName(config.onvif.password_env, "onvif.password_env")) {
        return false;
    }

    std::set<std::string> ids;
    std::set<std::string> macs;
    std::set<std::string> serials;
    std::set<std::string> ips;
    for (std::size_t i = 0; i < config.cameras.size(); ++i) {
        const CameraOverride& camera = config.cameras[i];
        const std::string path = "cameras[" + std::to_string(i) + "]";
        if (!require(!camera.id.empty() || !camera.mac.empty() || !camera.serial.empty() ||
                         !camera.ip.empty(),
                     path, "needs at least one of id, mac, serial or ip to match a camera")) {
            return false;
        }
        if (!camera.id.empty()) {
            if (!require(isCameraId(camera.id), path + ".id",
                         "'" + camera.id + "' may only contain letters, digits, '-' and '_'") ||
                !require(ids.insert(camera.id).second, path + ".id",
                         "'" + camera.id + "' is used by another camera entry")) {
                return false;
            }
        }
        if (!camera.mac.empty()) {
            if (!require(net::normalizeMac(camera.mac) == camera.mac, path + ".mac",
                         "'" + camera.mac + "' is not a normalized MAC address") ||
                !require(macs.insert(camera.mac).second, path + ".mac",
                         "'" + camera.mac + "' is used by another camera entry")) {
                return false;
            }
        }
        if (!camera.serial.empty() &&
            !require(serials.insert(camera.serial).second, path + ".serial",
                     "'" + camera.serial + "' is used by another camera entry")) {
            return false;
        }
        if (!camera.ip.empty()) {
            if (!require(net::parseIpv4(camera.ip).has_value(), path + ".ip",
                         "'" + camera.ip + "' is not an IPv4 address") ||
                !require(ips.insert(camera.ip).second, path + ".ip",
                         "'" + camera.ip + "' is used by another camera entry")) {
                return false;
            }
        }
        if (camera.rtsp_port && !require(validPort(*camera.rtsp_port), path + ".rtsp_port",
                                         "must be between 1 and 65535")) {
            return false;
        }
        if (camera.http_port && !require(validPort(*camera.http_port), path + ".http_port",
                                         "must be between 1 and 65535")) {
            return false;
        }
        if (!camera.rtsp_path.empty() &&
            !require(camera.rtsp_path.front() == '/', path + ".rtsp_path",
                     "must start with '/'")) {
            return false;
        }
        if (!camera.username_env.empty() &&
            !requireEnvName(camera.username_env, path + ".username_env")) {
            return false;
        }
        if (!camera.password_env.empty() &&
            !requireEnvName(camera.password_env, path + ".password_env")) {
            return false;
        }
    }
    return true;
}

EnvLookup processEnvironment() {
    return [](const std::string& name) -> std::optional<std::string> {
        const char* value = std::getenv(name.c_str());
        if (value == nullptr) {
            return std::nullopt;
        }
        return std::string(value);
    };
}

std::string envSuffix(const std::string& camera_id) {
    std::string suffix;
    suffix.reserve(camera_id.size());
    for (const char ch : camera_id) {
        const auto byte = static_cast<unsigned char>(ch);
        suffix.push_back(std::isalnum(byte) != 0 ? static_cast<char>(std::toupper(byte)) : '_');
    }
    return suffix;
}

ResolvedCredentials resolveCredentials(const CameraModeConfig& config,
                                       const CameraOverride* camera_override,
                                       const std::string& camera_id, const EnvLookup& env) {
    ResolvedCredentials resolved;
    const RtspSettings& rtsp = config.rtsp;

    // 1. Names given in this camera's override. A half left out falls back to the global name
    //    of the same role, so "same user, own password" needs only password_env.
    if (camera_override != nullptr &&
        (!camera_override->username_env.empty() || !camera_override->password_env.empty())) {
        const std::string username_var = camera_override->username_env.empty()
                                             ? rtsp.username_env
                                             : camera_override->username_env;
        const std::string password_var = camera_override->password_env.empty()
                                             ? rtsp.password_env
                                             : camera_override->password_env;
        if (tryCredentials(env, username_var, password_var, resolved)) {
            return resolved;
        }
    }

    // 2. HIKVISION_USERNAME_CAMERA_02 style, derived from the configured global names so a
    //    renamed global variable keeps its per-camera variants.
    const std::string suffix = envSuffix(camera_id);
    if (!suffix.empty() && !rtsp.username_env.empty() &&
        tryCredentials(env, rtsp.username_env + "_" + suffix,
                       rtsp.password_env.empty() ? std::string{} : rtsp.password_env + "_" + suffix,
                       resolved)) {
        return resolved;
    }

    // 3. The global pair shared by every camera.
    if (tryCredentials(env, rtsp.username_env, rtsp.password_env, resolved)) {
        return resolved;
    }
    return ResolvedCredentials{};
}

std::optional<net::Credentials> resolveOnvifCredentials(const CameraModeConfig& config,
                                                        const EnvLookup& env) {
    ResolvedCredentials resolved;
    if (!tryCredentials(env, config.onvif.username_env, config.onvif.password_env, resolved)) {
        return std::nullopt;
    }
    return resolved.credentials;
}

const CameraOverride* findOverride(const CameraModeConfig& config, const std::string& id,
                                   const std::string& mac, const std::string& serial,
                                   const std::string& ip) {
    const std::string device_mac = net::normalizeMac(mac);
    auto conflicts = [](const std::string& configured, const std::string& reported) {
        return !configured.empty() && !reported.empty() && configured != reported;
    };

    if (!id.empty()) {
        for (const CameraOverride& camera : config.cameras) {
            if (camera.id == id) {
                return &camera;
            }
        }
    }
    if (!device_mac.empty()) {
        for (const CameraOverride& camera : config.cameras) {
            if (!camera.mac.empty() && net::normalizeMac(camera.mac) == device_mac) {
                return &camera;
            }
        }
    }
    // Weaker identifiers never select an override written for a device that reports a
    // different MAC (or, for the IP, a different serial): a camera that took over another one's
    // address must not inherit its settings.
    if (!serial.empty()) {
        for (const CameraOverride& camera : config.cameras) {
            if (camera.serial == serial && !conflicts(net::normalizeMac(camera.mac), device_mac)) {
                return &camera;
            }
        }
    }
    if (!ip.empty()) {
        for (const CameraOverride& camera : config.cameras) {
            if (camera.ip == ip && !conflicts(net::normalizeMac(camera.mac), device_mac) &&
                !conflicts(camera.serial, serial)) {
                return &camera;
            }
        }
    }
    return nullptr;
}

}  // namespace anpr::cameras
