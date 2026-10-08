// Camera discovery on the camera LAN: SADP, ONVIF WS-Discovery, the neighbour table, listed
// hosts and an optional TCP sweep, then unauthenticated identification (RTSP OPTIONS, the realm
// of a 401, the HTTP Server header) and duplicate-IP ARP probes, merged into one record per
// physical device with a stable registry id. Nothing here sends credentials, and no address is
// contacted whose route leaves through another interface than the camera LAN.
#include "anpr/cameras/discovery.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <fstream>
#include <set>
#include <sstream>
#include <thread>
#include <utility>

#include "anpr/common/logging.hpp"
#include "anpr/net/http_client.hpp"
#include "anpr/net/rtsp_client.hpp"

namespace anpr::cameras {
namespace {

constexpr const char* kProcNetArp = "/proc/net/arp";
constexpr std::uint16_t kDefaultRtspPort = 554;
constexpr std::uint16_t kDefaultHttpPort = 80;
constexpr std::uint16_t kDefaultSdkPort = 8000;
/// Sockets in flight during the subnet sweep: a /24 on three ports in about two connect
/// timeouts, far below the descriptor limit.
constexpr int kScanParallel = 64;
/// Hosts identified at the same time (RTSP OPTIONS, DESCRIBE for the realm, HTTP GET /).
constexpr std::size_t kIdentifyParallel = 8;

std::string lowerCase(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return text;
}

bool startsWith(const std::string& text, const std::string& prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

/// "DS-TCG406-E", "ds-2cd2386g2-iu": every Hikvision model number starts with "DS-".
bool isDsModel(const std::string& text) {
    return text.size() > 3 && startsWith(lowerCase(text), "ds-") &&
           std::isalnum(static_cast<unsigned char>(text[3])) != 0;
}

std::string join(const std::vector<std::string>& parts, const char* separator) {
    std::string out;
    for (const std::string& part : parts) {
        if (!out.empty()) {
            out += separator;
        }
        out += part;
    }
    return out;
}

/// The address a device reports for itself; the datagram source when the field is unusable.
net::Ipv4 sadpAddress(const hikvision::SadpDevice& device) {
    const auto parsed = net::parseIpv4(device.ipv4);
    return parsed && !parsed->isZero() ? *parsed : device.from;
}

net::Ipv4 onvifAddress(const hikvision::WsDiscoveryMatch& match) {
    return match.xaddr_host.isZero() ? match.from : match.xaddr_host;
}

std::string cameraLanText(const net::CameraLanSelection& lan) {
    if (lan.networks.empty()) {
        return lan.interface.empty() ? "no camera LAN interface is selected"
                                     : "the camera LAN " + lan.interface +
                                           " has no IPv4 address";
    }
    std::vector<std::string> networks;
    for (const net::Ipv4Network& network : lan.networks) {
        networks.push_back(network.cidr());
    }
    return "the Jetson's camera LAN is " + join(networks, ", ");
}

/// Lower is better: one probe that connected beats any number that did not.
int outcomeRank(net::ConnectOutcome outcome) {
    switch (outcome) {
        case net::ConnectOutcome::kConnected:
            return 0;
        case net::ConnectOutcome::kRefused:
            return 1;
        case net::ConnectOutcome::kTimeout:
            return 2;
        case net::ConnectOutcome::kUnreachable:
            return 3;
        case net::ConnectOutcome::kError:
            break;
    }
    return 4;
}

std::string outcomeWord(net::ConnectOutcome outcome) {
    switch (outcome) {
        case net::ConnectOutcome::kConnected:
            return "open";
        case net::ConnectOutcome::kRefused:
            return "closed";
        case net::ConnectOutcome::kTimeout:
            return "no answer";
        case net::ConnectOutcome::kUnreachable:
            return "unreachable";
        case net::ConnectOutcome::kError:
            break;
    }
    return "error";
}

std::uint16_t toPort(int value) {
    return value > 0 && value <= 65535 ? static_cast<std::uint16_t>(value) : 0;
}

/// Every port that may carry RTSP on some camera: the default, the configured one, per-camera
/// overrides and ports given with listed hosts.
std::set<std::uint16_t> rtspPortSet(const DiscoveryInputs& inputs, const CameraModeConfig& config) {
    std::set<std::uint16_t> ports{kDefaultRtspPort};
    if (const std::uint16_t port = toPort(config.rtsp.port)) {
        ports.insert(port);
    }
    for (const CameraOverride& camera : config.cameras) {
        if (camera.rtsp_port) {
            if (const std::uint16_t port = toPort(*camera.rtsp_port)) {
                ports.insert(port);
            }
        }
    }
    for (const auto& item : inputs.listed) {
        if (item.first.second != 0) {
            ports.insert(item.first.second);
        }
    }
    return ports;
}

/// One physical device while the inputs are merged.
struct DeviceRecord {
    DiscoveredCamera camera;
    bool sadp{false};
    std::string sadp_ipv4;
    /// ONVIF hardware and name scopes, for the vendor evidence.
    std::vector<std::string> onvif_names;
    /// Ports given with the listed entries for this address, in input order.
    std::vector<std::uint16_t> listed_ports;
    std::string rtsp_realm;
};

/// MACs that answered for one address, with the protocols that said so.
using Claims = std::map<std::string, std::set<std::string>>;

class Merger {
public:
    Merger(const DiscoveryInputs& inputs, const net::CameraLanSelection& lan,
           const CameraModeConfig& config)
        : inputs_(inputs), lan_(lan), config_(config) {}

    std::vector<DiscoveredCamera> run() {
        collectClaims();
        addSadp();
        addOnvif();
        addArpTable();
        addListed();
        addPortsAndServices();
        return finish();
    }

private:
    const DiscoveryInputs& inputs_;
    const net::CameraLanSelection& lan_;
    const CameraModeConfig& config_;
    std::vector<DeviceRecord> devices_;
    /// SADP and ARP-probe answers per address: two MACs here are a duplicate address. ONVIF is
    /// left out: its MAC is often derived from the endpoint UUID, which is a convention, not a
    /// measurement, and a false DUPLICATE_IP_DETECTED would block a healthy camera.
    std::map<std::uint32_t, Claims> claims_;
    /// `claims_` plus ONVIF and the neighbour table: gives MAC-less observations their device.
    std::map<std::uint32_t, std::set<std::string>> known_macs_;

    void claim(net::Ipv4 ip, const std::string& mac, const char* protocol) {
        if (mac.empty() || ip.isZero()) {
            return;
        }
        claims_[ip.value][mac].insert(protocol);
        known_macs_[ip.value].insert(mac);
    }

    void collectClaims() {
        for (const hikvision::SadpDevice& device : inputs_.sadp) {
            claim(sadpAddress(device), net::normalizeMac(device.mac), "SADP");
        }
        for (const hikvision::WsDiscoveryMatch& match : inputs_.onvif) {
            const std::string mac = net::normalizeMac(match.mac);
            if (!mac.empty() && !onvifAddress(match).isZero()) {
                known_macs_[onvifAddress(match).value].insert(mac);
            }
        }
        for (const auto& entry : inputs_.arp_probe) {
            for (const std::string& mac : entry.second) {
                claim(net::Ipv4{entry.first}, net::normalizeMac(mac), "ARP probe");
            }
        }
        for (const net::ArpEntry& entry : inputs_.arp) {
            const std::string mac = net::normalizeMac(entry.mac);
            if (entry.complete && !mac.empty() && !entry.ip.isZero()) {
                known_macs_[entry.ip.value].insert(mac);
            }
        }
    }

    /// The only MAC seen for `ip`, or empty when none or several are.
    std::string uniqueMac(net::Ipv4 ip) const {
        const auto found = known_macs_.find(ip.value);
        return found != known_macs_.end() && found->second.size() == 1 ? *found->second.begin()
                                                                       : std::string();
    }

    bool hasDeviceWithMac(const std::string& mac) const {
        return std::any_of(devices_.begin(), devices_.end(),
                           [&mac](const DeviceRecord& device) { return device.camera.mac == mac; });
    }

    std::vector<std::size_t> devicesAt(net::Ipv4 ip) const {
        std::vector<std::size_t> indexes;
        for (std::size_t i = 0; i < devices_.size(); ++i) {
            if (devices_[i].camera.ip == ip) {
                indexes.push_back(i);
            }
        }
        return indexes;
    }

    /// The device with `mac` (or, without one, the first device at `ip`), created when new.
    /// A MAC-less record at `ip` that was seen before its MAC was known becomes that device.
    std::size_t obtain(const std::string& mac, net::Ipv4 ip) {
        if (!mac.empty()) {
            for (std::size_t i = 0; i < devices_.size(); ++i) {
                if (devices_[i].camera.mac == mac) {
                    return i;
                }
            }
            for (std::size_t i = 0; i < devices_.size(); ++i) {
                if (devices_[i].camera.mac.empty() && devices_[i].camera.ip == ip) {
                    devices_[i].camera.mac = mac;
                    return i;
                }
            }
        } else {
            const std::vector<std::size_t> at = devicesAt(ip);
            if (!at.empty()) {
                return at.front();
            }
        }
        devices_.emplace_back();
        devices_.back().camera.ip = ip;
        devices_.back().camera.mac = mac;
        return devices_.size() - 1;
    }

    /// The first device at `ip`, or a new one identified by the address's only known MAC.
    std::size_t obtainAt(net::Ipv4 ip) {
        const std::vector<std::size_t> at = devicesAt(ip);
        if (!at.empty()) {
            return at.front();
        }
        const std::string mac = uniqueMac(ip);
        return obtain(hasDeviceWithMac(mac) ? std::string() : mac, ip);
    }

    void addSadp() {
        for (const hikvision::SadpDevice& answer : inputs_.sadp) {
            const net::Ipv4 ip = sadpAddress(answer);
            DeviceRecord& device = devices_[obtain(net::normalizeMac(answer.mac), ip)];
            DiscoveredCamera& camera = device.camera;
            if (device.sadp && camera.ip != ip) {
                camera.notes.push_back("SADP also reported this device at " + net::toString(ip));
            }
            device.sadp = true;
            camera.sources |= kFoundBySadp;
            if (device.sadp_ipv4.empty()) {
                device.sadp_ipv4 = answer.ipv4.empty() ? net::toString(ip) : answer.ipv4;
            }
            const auto fill = [](std::string& field, const std::string& value) {
                if (field.empty()) {
                    field = value;
                }
            };
            fill(camera.model, answer.model);
            fill(camera.serial, answer.serial);
            fill(camera.firmware, answer.software_version);
            fill(camera.device_type, answer.device_type);
            fill(camera.subnet_mask, answer.subnet_mask);
            fill(camera.gateway, answer.gateway);
            if (answer.activated) {
                camera.activated = answer.activated;
            }
            camera.dhcp = camera.dhcp || answer.dhcp;
            if (answer.http_port != 0) {
                camera.http_port = answer.http_port;
            }
            if (answer.command_port != 0) {
                camera.sdk_port = answer.command_port;
            }
        }
    }

    void addOnvif() {
        for (const hikvision::WsDiscoveryMatch& match : inputs_.onvif) {
            const net::Ipv4 ip = onvifAddress(match);
            std::string mac = net::normalizeMac(match.mac);
            std::string mac_note;
            if (mac.empty()) {
                mac = uniqueMac(ip);
            } else if (!hasDeviceWithMac(mac)) {
                // A MAC no SADP answer reported, at an address a SADP device holds: the same
                // camera with a UUID that does not encode its MAC, not a second device.
                for (const std::size_t index : devicesAt(ip)) {
                    if (devices_[index].sadp) {
                        mac_note = "ONVIF reports MAC " + mac + " for this address";
                        mac = devices_[index].camera.mac;
                        break;
                    }
                }
            }
            DeviceRecord& device = devices_[obtain(mac, ip)];
            DiscoveredCamera& camera = device.camera;
            if (!mac_note.empty()) {
                camera.notes.push_back(mac_note);
            }
            camera.sources |= kFoundByOnvif;
            camera.onvif_seen = true;
            if (camera.onvif_endpoint.empty()) {
                camera.onvif_endpoint = match.endpoint;
            }
            if (camera.onvif_xaddr.empty() && !match.xaddrs.empty()) {
                camera.onvif_xaddr = match.xaddrs.front();
            }
            for (const std::string& scope : match.scopes) {
                if (std::find(camera.onvif_scopes.begin(), camera.onvif_scopes.end(), scope) ==
                    camera.onvif_scopes.end()) {
                    camera.onvif_scopes.push_back(scope);
                }
            }
            if (camera.model.empty()) {
                camera.model = match.hardware;
            }
            for (const std::string* text : {&match.hardware, &match.name}) {
                if (!text->empty()) {
                    device.onvif_names.push_back(*text);
                }
            }
        }
    }

    void addArpTable() {
        for (const net::ArpEntry& entry : inputs_.arp) {
            const std::string mac = net::normalizeMac(entry.mac);
            if (!entry.complete || mac.empty() || entry.ip.isZero()) {
                continue;
            }
            // A neighbour entry that contradicts what the address itself answered is either a
            // second device or a stale entry; the ARP probe, not the cache, decides which.
            const auto claimed = claims_.find(entry.ip.value);
            if (claimed != claims_.end() && claimed->second.count(mac) == 0) {
                for (const std::size_t index : devicesAt(entry.ip)) {
                    devices_[index].camera.notes.push_back("the neighbour table shows " + mac +
                                                           " for this address");
                }
                continue;
            }
            devices_[obtain(mac, entry.ip)].camera.sources |= kFoundByArp;
        }
    }

    void addListed() {
        for (const auto& item : inputs_.listed) {
            const net::Ipv4 ip = item.first.first;
            if (ip.isZero()) {
                continue;
            }
            std::vector<std::size_t> at = devicesAt(ip);
            if (at.empty()) {
                at.push_back(obtainAt(ip));
            }
            for (const std::size_t index : at) {
                devices_[index].camera.sources |= item.second;
                devices_[index].listed_ports.push_back(item.first.second);
            }
        }
    }

    void addPortsAndServices() {
        std::map<std::uint32_t, std::map<std::uint16_t, net::ConnectOutcome>> by_host;
        for (const net::PortProbe& probe : inputs_.ports) {
            auto& ports = by_host[probe.host.value];
            const auto existing = ports.find(probe.port);
            if (existing == ports.end() ||
                outcomeRank(probe.outcome) < outcomeRank(existing->second)) {
                ports[probe.port] = probe.outcome;
            }
        }
        // A host found only by the sweep is a device once anything on it answered.
        for (const auto& host : by_host) {
            const net::Ipv4 ip{host.first};
            const bool answered = std::any_of(
                host.second.begin(), host.second.end(),
                [](const auto& port) { return port.second == net::ConnectOutcome::kConnected; });
            if (answered && devicesAt(ip).empty()) {
                devices_[obtainAt(ip)].camera.sources |= kFoundByScan;
            }
        }
        for (DeviceRecord& device : devices_) {
            const std::uint32_t key = device.camera.ip.value;
            const auto ports = by_host.find(key);
            if (ports != by_host.end()) {
                device.camera.ports = ports->second;
            }
            const auto services = inputs_.services.find(key);
            if (services != inputs_.services.end()) {
                const ServiceAnswers& answers = services->second;
                device.camera.rtsp_ok = answers.rtsp_ok;
                device.camera.rtsp_server = answers.rtsp_server;
                device.camera.http_server = answers.http_server;
                device.rtsp_realm = answers.rtsp_realm;
                if (answers.rtsp_ok && answers.rtsp_port != 0) {
                    device.camera.ports[answers.rtsp_port] = net::ConnectOutcome::kConnected;
                }
            }
            const auto notes = inputs_.notes.find(key);
            if (notes != inputs_.notes.end()) {
                device.camera.notes.insert(device.camera.notes.end(), notes->second.begin(),
                                           notes->second.end());
            }
        }
    }

    /// Which evidence says Hikvision, strongest first; empty when nothing does.
    static std::string hikvisionEvidence(const DeviceRecord& device) {
        const DiscoveredCamera& camera = device.camera;
        if (device.sadp) {
            return "SADP answer";
        }
        const auto mentions = [](const std::string& text) {
            return lowerCase(text).find("hikvision") != std::string::npos;
        };
        for (const std::string& name : device.onvif_names) {
            if (mentions(name) || isDsModel(name)) {
                return "ONVIF hardware/name scope";
            }
        }
        for (const std::string& scope : camera.onvif_scopes) {
            if (mentions(scope)) {
                return "ONVIF scopes";
            }
        }
        const std::string server = lowerCase(camera.http_server);
        if (server == "webserver" || startsWith(server, "app-webs") ||
            startsWith(server, "dnvrs-webs") || startsWith(server, "hikvision-webs")) {
            return "HTTP Server header \"" + camera.http_server + "\"";
        }
        if (startsWith(device.rtsp_realm, "IP Camera(") || device.rtsp_realm == "Hikvision" ||
            isDsModel(device.rtsp_realm)) {
            return "RTSP realm \"" + device.rtsp_realm + "\"";
        }
        if (mentions(camera.rtsp_server)) {
            return "RTSP Server header \"" + camera.rtsp_server + "\"";
        }
        if (net::isHikvisionOui(camera.mac)) {
            return "MAC OUI only (weakest evidence)";
        }
        return {};
    }

    std::uint16_t chooseRtspPort(const DeviceRecord& device) const {
        const DiscoveredCamera& camera = device.camera;
        const CameraOverride* camera_override =
            findOverride(config_, "", camera.mac, camera.serial, net::toString(camera.ip));
        if (camera_override != nullptr && camera_override->rtsp_port) {
            if (const std::uint16_t port = toPort(*camera_override->rtsp_port)) {
                return port;
            }
        }
        const std::uint16_t configured = toPort(config_.rtsp.port);
        for (const std::uint16_t port : device.listed_ports) {
            if (port != 0 && port != configured) {
                return port;
            }
        }
        const auto open = [&camera](std::uint16_t port) {
            const auto found = camera.ports.find(port);
            return found != camera.ports.end() && found->second == net::ConnectOutcome::kConnected;
        };
        if (configured != 0 && open(configured)) {
            return configured;
        }
        if (open(kDefaultRtspPort)) {
            return kDefaultRtspPort;
        }
        return configured != 0 ? configured : kDefaultRtspPort;
    }

    void addProblems(DeviceRecord& device, const std::set<std::uint16_t>& rtsp_ports) const {
        DiscoveredCamera& camera = device.camera;
        const std::string ip_text = net::toString(camera.ip);

        const auto claimed = claims_.find(camera.ip.value);
        if (claimed != claims_.end()) {
            std::set<std::string> macs;
            for (const auto& entry : claimed->second) {
                macs.insert(entry.first);
            }
            if (!camera.mac.empty()) {
                macs.insert(camera.mac);
            }
            if (macs.size() >= 2) {
                std::vector<std::string> parts;
                for (const std::string& mac : macs) {
                    const auto protocols = claimed->second.find(mac);
                    std::vector<std::string> names;
                    if (protocols != claimed->second.end()) {
                        names.assign(protocols->second.begin(), protocols->second.end());
                    }
                    parts.push_back(mac + (names.empty() ? "" : " (" + join(names, ", ") + ")"));
                    if (mac != camera.mac) {
                        camera.conflicting_macs.push_back(mac);
                    }
                }
                camera.problems.emplace_back(CameraError::kDuplicateIpDetected,
                                             ip_text + " is answered by " + join(parts, " and "));
            }
        }

        if (!camera.on_camera_subnet) {
            std::string seen;
            if (device.sadp) {
                seen = "SADP reports " + device.sadp_ipv4 +
                       (camera.subnet_mask.empty() ? "" : "/" + camera.subnet_mask);
            } else {
                seen = ip_text + " (found by " + describeSources(camera.sources) + ")";
            }
            camera.problems.emplace_back(CameraError::kCameraOnOtherSubnet,
                                         seen + "; " + cameraLanText(lan_));
        }

        if (camera.activated && !*camera.activated) {
            camera.problems.emplace_back(
                CameraError::kCameraNotActivated,
                "SADP reports Activated=false" +
                    (camera.model.empty() ? std::string() : " for " + camera.model) +
                    (camera.serial.empty() ? std::string() : " serial " + camera.serial));
        }

        const bool onvif_probed =
            config_.discovery.enabled && config_.discovery.onvif && inputs_.onvif_ran;
        const bool answered_otherwise =
            device.sadp || camera.rtsp_ok || hasOpenRtspPort(camera, rtsp_ports);
        if (onvif_probed && camera.isHikvision() && !camera.onvif_seen && answered_otherwise &&
            camera.on_camera_subnet) {
            camera.problems.emplace_back(CameraError::kOnvifDisabledOrUnavailable,
                                         "No WS-Discovery answer from " + ip_text);
        }
    }

    static bool hasOpenRtspPort(const DiscoveredCamera& camera,
                                const std::set<std::uint16_t>& rtsp_ports) {
        return std::any_of(rtsp_ports.begin(), rtsp_ports.end(), [&camera](std::uint16_t port) {
            const auto found = camera.ports.find(port);
            return found != camera.ports.end() && found->second == net::ConnectOutcome::kConnected;
        });
    }

    std::vector<DiscoveredCamera> finish() {
        const std::set<std::uint16_t> rtsp_ports = rtspPortSet(inputs_, config_);
        std::vector<DiscoveredCamera> cameras;
        for (DeviceRecord& device : devices_) {
            DiscoveredCamera& camera = device.camera;
            const bool listed_by_operator = (camera.sources & kFoundByManual) != 0U;
            // Listed hosts are kept even while silent: the operator says a camera is there, and
            // the preflight then reports it as unreachable instead of it vanishing.
            const bool is_camera = device.sadp || camera.onvif_seen || camera.rtsp_ok ||
                                   hasOpenRtspPort(camera, rtsp_ports) || listed_by_operator;
            if (!is_camera) {
                continue;
            }
            camera.on_camera_subnet = lan_.inCameraSubnet(camera.ip);

            const std::string evidence = hikvisionEvidence(device);
            if (!evidence.empty()) {
                camera.vendor = "Hikvision";
                camera.notes.push_back("vendor Hikvision from the " + evidence);
            }
            if (camera.model.empty() && isDsModel(device.rtsp_realm)) {
                camera.model = device.rtsp_realm;
            }
            camera.rtsp_port = chooseRtspPort(device);
            if (listed_by_operator && camera.ports.empty() && !device.sadp &&
                !camera.onvif_seen) {
                camera.notes.push_back("listed in the configuration; not probed");
            } else if (listed_by_operator && !device.sadp && !camera.onvif_seen &&
                       !camera.rtsp_ok && !hasOpenRtspPort(camera, rtsp_ports)) {
                camera.notes.push_back("listed in the configuration; RTSP does not answer now");
            }
            addProblems(device, rtsp_ports);
            cameras.push_back(std::move(camera));
        }
        std::sort(cameras.begin(), cameras.end(),
                  [](const DiscoveredCamera& lhs, const DiscoveredCamera& rhs) {
                      if (lhs.ip != rhs.ip) {
                          return lhs.ip < rhs.ip;
                      }
                      return lhs.mac < rhs.mac;
                  });
        return cameras;
    }
};

DiscoveryMethodReport methodReport(const char* name, bool ran, int found, std::string detail) {
    DiscoveryMethodReport report;
    report.method = name;
    report.ran = ran;
    report.found = found;
    report.detail = std::move(detail);
    return report;
}

void logMethod(const DiscoveryMethodReport& method) {
    logEvent(LogLevel::kInfo, "camera_discovery_method",
             LogFields()
                 .add("method", method.method)
                 .add("ran", method.ran ? "true" : "false")
                 .add("found", method.found)
                 .addQuoted("detail", method.detail));
}

std::string portsText(const DiscoveredCamera& camera) {
    if (camera.ports.empty()) {
        return camera.on_camera_subnet ? "not probed" : "not probed (outside the camera subnet)";
    }
    std::vector<std::string> parts;
    const auto rtsp = camera.ports.find(camera.rtsp_port);
    if (rtsp != camera.ports.end()) {
        parts.push_back(std::to_string(rtsp->first) + " " + outcomeWord(rtsp->second));
    }
    for (const auto& port : camera.ports) {
        if (port.first != camera.rtsp_port) {
            parts.push_back(std::to_string(port.first) + " " + outcomeWord(port.second));
        }
    }
    return join(parts, ", ");
}

std::string rtspText(const DiscoveredCamera& camera) {
    const std::string port = std::to_string(camera.rtsp_port);
    if (camera.rtsp_ok) {
        return "OPTIONS ok on port " + port +
               (camera.rtsp_server.empty() ? std::string()
                                           : ", server \"" + camera.rtsp_server + "\"");
    }
    const auto found = camera.ports.find(camera.rtsp_port);
    if (found == camera.ports.end()) {
        return "not probed";
    }
    if (found->second == net::ConnectOutcome::kConnected) {
        return "port " + port + " open, no RTSP answer to OPTIONS";
    }
    return found->second == net::ConnectOutcome::kRefused
               ? "closed (port " + port + " refused)"
               : "port " + port + " " + outcomeWord(found->second);
}

/// Indents every line of `text` by `prefix`.
std::string indent(const std::string& text, const std::string& prefix) {
    std::istringstream lines(text);
    std::string line;
    std::string out;
    while (std::getline(lines, line)) {
        out += prefix + line + "\n";
    }
    return out;
}

}  // namespace

std::string describeSources(unsigned sources) {
    static const std::pair<DiscoverySource, const char*> kNames[] = {
        {kFoundBySadp, "sadp"}, {kFoundByOnvif, "onvif"},   {kFoundByArp, "arp"},
        {kFoundByScan, "scan"}, {kFoundByManual, "manual"}, {kFoundByRegistry, "registry"},
    };
    std::vector<std::string> names;
    for (const auto& entry : kNames) {
        if ((sources & static_cast<unsigned>(entry.first)) != 0U) {
            names.emplace_back(entry.second);
        }
    }
    return names.empty() ? "none" : join(names, ",");
}

bool cameraIdLess(const std::string& lhs, const std::string& rhs) {
    const auto digit = [](char ch) { return std::isdigit(static_cast<unsigned char>(ch)) != 0; };
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < lhs.size() && j < rhs.size()) {
        if (digit(lhs[i]) && digit(rhs[j])) {
            std::size_t end_i = i;
            std::size_t end_j = j;
            while (end_i < lhs.size() && digit(lhs[end_i])) {
                ++end_i;
            }
            while (end_j < rhs.size() && digit(rhs[end_j])) {
                ++end_j;
            }
            // Numbers compare by value; leading zeros only break ties ("camera-01" == 1).
            std::size_t start_i = i;
            std::size_t start_j = j;
            while (start_i + 1 < end_i && lhs[start_i] == '0') {
                ++start_i;
            }
            while (start_j + 1 < end_j && rhs[start_j] == '0') {
                ++start_j;
            }
            const std::string left = lhs.substr(start_i, end_i - start_i);
            const std::string right = rhs.substr(start_j, end_j - start_j);
            if (left.size() != right.size()) {
                return left.size() < right.size();
            }
            if (left != right) {
                return left < right;
            }
            if (end_i - i != end_j - j) {
                return end_i - i < end_j - j;
            }
            i = end_i;
            j = end_j;
            continue;
        }
        if (lhs[i] != rhs[j]) {
            return static_cast<unsigned char>(lhs[i]) < static_cast<unsigned char>(rhs[j]);
        }
        ++i;
        ++j;
    }
    return lhs.size() - i < rhs.size() - j;
}

bool DiscoveredCamera::hasProblem(CameraError error) const {
    return std::any_of(problems.begin(), problems.end(),
                       [error](const auto& problem) { return problem.first == error; });
}

std::vector<DiscoveredCamera> mergeDiscovery(const DiscoveryInputs& inputs,
                                             const net::CameraLanSelection& lan,
                                             const CameraModeConfig& config) {
    return Merger(inputs, lan, config).run();
}

void assignIds(DiscoveryReport& report, CameraRegistry& registry, const CameraModeConfig& config,
               const std::string& now_iso) {
    std::set<std::string> used;
    for (DiscoveredCamera& camera : report.cameras) {
        DeviceIdentity identity;
        identity.mac = camera.mac;
        identity.serial = camera.serial;
        identity.device_id = camera.device_id;
        std::string uuid = camera.onvif_endpoint;
        if (startsWith(lowerCase(uuid), "urn:uuid:")) {
            uuid = uuid.substr(9);
        }
        identity.onvif_uuid = lowerCase(uuid);
        identity.ip = camera.ip.isZero() ? std::string() : net::toString(camera.ip);
        identity.model = camera.model;

        std::string id = registry.assign(identity, now_iso);
        if (used.count(id) != 0U) {
            // Two devices of one scan matched the same entry (only possible with identifiers
            // that contradict each other); the second gets a free id for this run only.
            std::vector<std::string> taken(used.begin(), used.end());
            for (const RegistryEntry& entry : registry.entries()) {
                taken.push_back(entry.id);
            }
            const std::string first = id;
            id = nextCameraId(taken);
            camera.notes.push_back("matched the registry entry of " + first +
                                   " as well; temporary id " + id + " for this run");
        }
        used.insert(id);
        camera.id = id;
    }
    std::stable_sort(report.cameras.begin(), report.cameras.end(),
                     [](const DiscoveredCamera& lhs, const DiscoveredCamera& rhs) {
                         return cameraIdLess(lhs.id, rhs.id);
                     });

    const int limit = config.discovery.max_cameras;
    if (limit > 0 && report.cameras.size() > static_cast<std::size_t>(limit)) {
        std::vector<std::string> ignored;
        for (std::size_t i = static_cast<std::size_t>(limit); i < report.cameras.size(); ++i) {
            ignored.push_back(report.cameras[i].id + " (" + net::toString(report.cameras[i].ip) +
                              ")");
        }
        report.problems.emplace_back(
            CameraError::kCameraLimitReached,
            "Found " + std::to_string(report.cameras.size()) +
                " cameras, discovery.max_cameras is " + std::to_string(limit) +
                "; ignored: " + join(ignored, ", "));
        report.cameras.resize(static_cast<std::size_t>(limit));
    }

    if (report.cameras.empty()) {
        std::vector<std::string> ran;
        for (const DiscoveryMethodReport& method : report.methods) {
            if (method.ran) {
                ran.push_back(method.method);
            }
        }
        report.problems.emplace_back(
            CameraError::kNoCamerasDiscovered,
            ran.empty() ? "No discovery method could run"
                        : "Nothing answered: " + join(ran, ", "));
    }
}

DiscoveryReport discoverCameras(const net::NetworkSnapshot& snapshot,
                                const net::CameraLanSelection& lan, const CameraModeConfig& config,
                                CameraRegistry& registry) {
    const DiscoverySettings& settings = config.discovery;
    const std::string& lan_interface = lan.interface;
    const net::Ipv4 own_address = lan.address ? lan.address->address : net::Ipv4{};
    const int listen_ms = std::max(settings.listen_ms, 0);
    const int connect_ms = std::max(settings.connect_timeout_ms, 1);
    const std::uint16_t configured_rtsp = toPort(config.rtsp.port);

    DiscoveryReport report;
    DiscoveryInputs inputs;

    std::set<std::uint32_t> own_addresses;
    for (const net::Ipv4Network& network : lan.networks) {
        own_addresses.insert(network.address.value);
    }
    if (!own_address.isZero()) {
        own_addresses.insert(own_address.value);
    }

    // The one gate in front of every unicast packet: inside a camera LAN subnet and routed by
    // the kernel through the camera LAN interface, never the GSM modem's default route.
    const auto contactable = [&](net::Ipv4 ip, std::string& why) {
        if (lan_interface.empty()) {
            why = "no camera LAN interface is selected";
            return false;
        }
        if (!lan.inCameraSubnet(ip)) {
            why = "outside the camera subnet (" + cameraLanText(lan) + ")";
            return false;
        }
        const net::RouteEntry* route = snapshot.routeFor(ip);
        if (route == nullptr) {
            why = "no route to it";
            return false;
        }
        if (route->interface != lan_interface) {
            why = "its route leaves through " + route->interface + ", not the camera LAN " +
                  lan_interface;
            return false;
        }
        return true;
    };

    // --- SADP and WS-Discovery, listening at the same time -------------------------------
    const bool want_sadp = settings.enabled && settings.sadp && !lan_interface.empty();
    const bool want_onvif =
        settings.enabled && settings.onvif && !lan_interface.empty() && !own_address.isZero();
    hikvision::SadpResult sadp;
    hikvision::WsDiscoveryResult onvif;
    {
        std::thread onvif_thread;
        if (want_onvif) {
            onvif_thread = std::thread(
                [&]() { onvif = hikvision::wsDiscover(lan_interface, own_address, listen_ms); });
        }
        if (want_sadp) {
            sadp = hikvision::sadpDiscover(lan_interface, own_address, listen_ms);
        }
        if (onvif_thread.joinable()) {
            onvif_thread.join();
        }
    }
    const auto skipped_because = [&](bool method_enabled) -> std::string {
        if (!settings.enabled) {
            return "discovery.enabled is false";
        }
        if (!method_enabled) {
            return "disabled in config";
        }
        return lan_interface.empty() ? "no camera LAN interface is selected" : std::string();
    };
    if (want_sadp) {
        report.methods.push_back(methodReport(
            "sadp", sadp.ran, static_cast<int>(sadp.devices.size()),
            sadp.ran ? "multicast on " + lan_interface : sadp.error));
        inputs.sadp = std::move(sadp.devices);
    } else {
        report.methods.push_back(methodReport("sadp", false, 0, skipped_because(settings.sadp)));
    }
    if (want_onvif) {
        report.methods.push_back(methodReport(
            "onvif", onvif.ran, static_cast<int>(onvif.matches.size()),
            onvif.ran ? "WS-Discovery on " + lan_interface : onvif.error));
        inputs.onvif = std::move(onvif.matches);
    } else {
        std::string why = skipped_because(settings.onvif);
        report.methods.push_back(methodReport(
            "onvif", false, 0,
            why.empty() ? "the camera LAN " + lan_interface + " has no IPv4 address" : why));
    }
    inputs.onvif_ran = want_onvif && onvif.ran;

    // --- Neighbour table -----------------------------------------------------------------
    const bool arp_readable = static_cast<bool>(std::ifstream(kProcNetArp));
    const auto read_lan_arp = [&]() {
        std::vector<net::ArpEntry> entries;
        for (const net::ArpEntry& entry : net::readArpTable(kProcNetArp)) {
            if (entry.interface == lan_interface && entry.complete && !entry.mac.empty()) {
                entries.push_back(entry);
            }
        }
        return entries;
    };
    const bool want_arp = settings.enabled && settings.arp_table && !lan_interface.empty();
    if (want_arp && arp_readable) {
        inputs.arp = read_lan_arp();
        report.methods.push_back(methodReport("arp", true, static_cast<int>(inputs.arp.size()),
                                              "neighbour table of " + lan_interface));
    } else {
        report.methods.push_back(methodReport(
            "arp", false, 0,
            want_arp ? std::string(kProcNetArp) + " is not readable on this system"
                     : skipped_because(settings.arp_table)));
    }

    // --- Listed hosts: configuration and registry ----------------------------------------
    int manual_count = 0;
    for (const std::string& host : settings.manual_hosts) {
        net::Ipv4 ip;
        std::uint16_t port = configured_rtsp != 0 ? configured_rtsp : kDefaultRtspPort;
        if (!net::parseHostPort(host, ip, port)) {
            logEvent(LogLevel::kWarn, "camera_manual_host_invalid",
                     LogFields().addQuoted("host", host));
            continue;
        }
        inputs.listed.push_back({{ip, port}, kFoundByManual});
        ++manual_count;
    }
    for (const CameraOverride& camera : config.cameras) {
        const std::optional<net::Ipv4> ip =
            camera.ip.empty() ? std::optional<net::Ipv4>() : net::parseIpv4(camera.ip);
        if (!ip) {
            continue;
        }
        std::uint16_t port = configured_rtsp != 0 ? configured_rtsp : kDefaultRtspPort;
        if (camera.rtsp_port && toPort(*camera.rtsp_port) != 0) {
            port = toPort(*camera.rtsp_port);
        }
        inputs.listed.push_back({{*ip, port}, kFoundByManual});
        ++manual_count;
    }
    report.methods.push_back(methodReport("manual", manual_count > 0, manual_count,
                                          manual_count > 0
                                              ? "discovery.manual_hosts and cameras[].ip"
                                              : "no hosts listed in the configuration"));
    if (settings.enabled) {
        int registry_count = 0;
        for (const RegistryEntry& entry : registry.entries()) {
            const auto ip = net::parseIpv4(entry.last_ip);
            if (!ip || ip->isZero()) {
                continue;
            }
            const CameraOverride* camera_override =
                findOverride(config, entry.id, entry.mac, entry.serial, entry.last_ip);
            std::uint16_t port = configured_rtsp != 0 ? configured_rtsp : kDefaultRtspPort;
            if (camera_override != nullptr && camera_override->rtsp_port &&
                toPort(*camera_override->rtsp_port) != 0) {
                port = toPort(*camera_override->rtsp_port);
            }
            inputs.listed.push_back({{*ip, port}, kFoundByRegistry});
            ++registry_count;
        }
        report.methods.push_back(
            methodReport("registry", true, registry_count, "last known addresses"));
    } else {
        report.methods.push_back(methodReport("registry", false, 0, "discovery.enabled is false"));
    }

    // --- Subnet sweep when the multicast methods may have missed cameras -----------------
    std::set<std::uint32_t> multicast_found;
    for (const hikvision::SadpDevice& device : inputs.sadp) {
        if (lan.inCameraSubnet(sadpAddress(device))) {
            multicast_found.insert(sadpAddress(device).value);
        }
    }
    for (const hikvision::WsDiscoveryMatch& match : inputs.onvif) {
        if (lan.inCameraSubnet(onvifAddress(match))) {
            multicast_found.insert(onvifAddress(match).value);
        }
    }
    bool want_scan = false;
    std::string scan_reason;
    if (!settings.enabled) {
        scan_reason = "discovery.enabled is false";
    } else if (settings.subnet_scan == ScanMode::kNever) {
        scan_reason = "subnet_scan is never";
    } else if (settings.subnet_scan == ScanMode::kAlways) {
        want_scan = true;
        scan_reason = "subnet_scan is always";
    } else if (multicast_found.empty()) {
        want_scan = true;
        scan_reason = "SADP and ONVIF found no camera on the camera subnet";
    } else if (multicast_found.size() < registry.entries().size()) {
        want_scan = true;
        scan_reason = "SADP and ONVIF found " + std::to_string(multicast_found.size()) +
                      " cameras, the registry knows " + std::to_string(registry.entries().size());
    } else {
        scan_reason = "SADP and ONVIF found every known camera";
    }

    std::vector<std::uint16_t> scan_ports;
    for (const int value : settings.scan_ports) {
        const std::uint16_t port = toPort(value);
        if (port != 0 &&
            std::find(scan_ports.begin(), scan_ports.end(), port) == scan_ports.end()) {
            scan_ports.push_back(port);
        }
    }
    std::set<std::uint32_t> scanned;
    if (want_scan && lan.networks.empty()) {
        report.methods.push_back(methodReport(
            "scan", false, 0,
            lan_interface.empty() ? "no camera LAN interface is selected"
                                  : "the camera LAN " + lan_interface + " has no IPv4 address"));
    } else if (want_scan) {
        std::vector<std::string> swept;
        std::vector<std::string> too_large;
        std::vector<net::Ipv4> hosts;
        const std::uint32_t limit =
            static_cast<std::uint32_t>(std::max(settings.scan_max_hosts, 0));
        int refused_by_route = 0;
        for (const net::Ipv4Network& network : lan.networks) {
            const std::uint32_t count = network.hostCount();
            if (count > limit) {
                too_large.push_back(network.cidr() + " has " + std::to_string(count) +
                                    " hosts, more than scan_max_hosts " + std::to_string(limit));
                continue;
            }
            swept.push_back(network.cidr());
            for (const net::Ipv4 ip : network.hosts(limit)) {
                std::string why;
                if (own_addresses.count(ip.value) != 0U || scanned.count(ip.value) != 0U) {
                    continue;
                }
                if (!contactable(ip, why)) {
                    ++refused_by_route;
                    continue;
                }
                scanned.insert(ip.value);
                hosts.push_back(ip);
            }
        }
        std::string detail = scan_reason;
        if (!too_large.empty()) {
            detail += "; skipped " + join(too_large, ", ");
        }
        if (refused_by_route > 0) {
            detail += "; " + std::to_string(refused_by_route) +
                      " addresses not routed through the camera LAN were left out";
        }
        if (hosts.empty() || scan_ports.empty()) {
            report.methods.push_back(methodReport(
                "scan", false, 0, scan_ports.empty() ? "no scan_ports configured" : detail));
        } else {
            std::set<std::uint32_t> answered;
            for (const net::PortProbe& probe :
                 net::probePorts(hosts, scan_ports, connect_ms, kScanParallel)) {
                if (probe.outcome == net::ConnectOutcome::kConnected) {
                    answered.insert(probe.host.value);
                }
                inputs.ports.push_back(probe);
            }
            report.methods.push_back(methodReport(
                "scan", true, static_cast<int>(answered.size()),
                join(swept, ", ") + ": " + std::to_string(hosts.size()) + " hosts x " +
                    std::to_string(scan_ports.size()) + " ports; " + detail));
        }
    } else {
        report.methods.push_back(methodReport("scan", false, 0, scan_reason));
    }

    // --- TCP probe of every candidate on the camera LAN ----------------------------------
    std::set<std::uint32_t> candidates;
    for (const hikvision::SadpDevice& device : inputs.sadp) {
        candidates.insert(sadpAddress(device).value);
    }
    for (const hikvision::WsDiscoveryMatch& match : inputs.onvif) {
        candidates.insert(onvifAddress(match).value);
    }
    for (const net::ArpEntry& entry : inputs.arp) {
        candidates.insert(entry.ip.value);
    }
    for (const auto& item : inputs.listed) {
        candidates.insert(item.first.first.value);
    }
    for (const net::PortProbe& probe : inputs.ports) {
        if (probe.outcome == net::ConnectOutcome::kConnected) {
            candidates.insert(probe.host.value);
        }
    }

    std::set<std::uint16_t> rtsp_ports = rtspPortSet(inputs, config);
    std::set<std::uint16_t> candidate_ports = rtsp_ports;
    candidate_ports.insert(kDefaultHttpPort);
    candidate_ports.insert(kDefaultSdkPort);
    for (const hikvision::SadpDevice& device : inputs.sadp) {
        if (device.http_port != 0) {
            candidate_ports.insert(device.http_port);
        }
        if (device.command_port != 0) {
            candidate_ports.insert(device.command_port);
        }
    }
    std::vector<net::Ipv4> fresh_hosts;
    std::vector<net::Ipv4> scanned_hosts;
    std::vector<net::Ipv4> probed;
    for (const std::uint32_t value : candidates) {
        const net::Ipv4 ip{value};
        if (ip.isZero() || own_addresses.count(value) != 0U) {
            continue;
        }
        std::string why;
        if (!contactable(ip, why)) {
            inputs.notes[value].push_back("not contacted: " + why);
            logEvent(LogLevel::kInfo, "camera_probe_skipped",
                     LogFields().add("ip", net::toString(ip)).addQuoted("reason", why));
            continue;
        }
        probed.push_back(ip);
        (scanned.count(value) != 0U ? scanned_hosts : fresh_hosts).push_back(ip);
    }
    const std::vector<std::uint16_t> all_ports(candidate_ports.begin(), candidate_ports.end());
    std::vector<std::uint16_t> extra_ports;
    for (const std::uint16_t port : candidate_ports) {
        if (std::find(scan_ports.begin(), scan_ports.end(), port) == scan_ports.end()) {
            extra_ports.push_back(port);
        }
    }
    if (!fresh_hosts.empty()) {
        for (const net::PortProbe& probe :
             net::probePorts(fresh_hosts, all_ports, connect_ms, kScanParallel)) {
            inputs.ports.push_back(probe);
        }
    }
    if (!scanned_hosts.empty() && !extra_ports.empty()) {
        for (const net::PortProbe& probe :
             net::probePorts(scanned_hosts, extra_ports, connect_ms, kScanParallel)) {
            inputs.ports.push_back(probe);
        }
    }

    // The probes made the kernel resolve every candidate: the neighbour table now has MACs for
    // listed and swept hosts, which gives them a stable identity in the registry.
    if (arp_readable && !lan_interface.empty() && !probed.empty()) {
        const std::set<std::uint32_t> probed_set = [&probed]() {
            std::set<std::uint32_t> values;
            for (const net::Ipv4 ip : probed) {
                values.insert(ip.value);
            }
            return values;
        }();
        for (const net::ArpEntry& entry : read_lan_arp()) {
            const bool wanted = want_arp || probed_set.count(entry.ip.value) != 0U;
            const bool known = std::any_of(
                inputs.arp.begin(), inputs.arp.end(), [&entry](const net::ArpEntry& existing) {
                    return existing.ip == entry.ip && existing.mac == entry.mac;
                });
            if (wanted && !known) {
                inputs.arp.push_back(entry);
            }
        }
    }

    // --- Unauthenticated identification ----------------------------------------------------
    std::map<std::uint32_t, std::map<std::uint16_t, bool>> open_ports;
    for (const net::PortProbe& probe : inputs.ports) {
        if (probe.outcome == net::ConnectOutcome::kConnected) {
            open_ports[probe.host.value][probe.port] = true;
        }
    }
    struct Job {
        net::Ipv4 ip;
        std::uint16_t rtsp_port{0};
        std::uint16_t http_port{0};
    };
    std::vector<Job> jobs;
    for (const net::Ipv4 ip : probed) {
        const auto open = open_ports.find(ip.value);
        if (open == open_ports.end()) {
            continue;
        }
        const auto is_open = [&open](std::uint16_t port) { return open->second.count(port) != 0U; };
        Job job;
        job.ip = ip;
        std::vector<std::uint16_t> rtsp_order;
        for (const auto& item : inputs.listed) {
            if (item.first.first == ip) {
                rtsp_order.push_back(item.first.second);
            }
        }
        rtsp_order.push_back(configured_rtsp);
        rtsp_order.push_back(kDefaultRtspPort);
        rtsp_order.insert(rtsp_order.end(), rtsp_ports.begin(), rtsp_ports.end());
        for (const std::uint16_t port : rtsp_order) {
            if (port != 0 && is_open(port)) {
                job.rtsp_port = port;
                break;
            }
        }
        std::vector<std::uint16_t> http_order;
        bool announced = false;
        for (const hikvision::SadpDevice& device : inputs.sadp) {
            if (sadpAddress(device) == ip && device.http_port != 0) {
                http_order.push_back(device.http_port);
                announced = true;
            }
        }
        for (const hikvision::WsDiscoveryMatch& match : inputs.onvif) {
            announced = announced || onvifAddress(match) == ip;
        }
        http_order.push_back(kDefaultHttpPort);
        for (const std::uint16_t port : http_order) {
            if (is_open(port)) {
                job.http_port = port;
                break;
            }
        }
        // The web page of a printer or a switch says nothing useful: only devices that already
        // look like cameras are asked for their Server header.
        if (job.rtsp_port == 0 && !announced) {
            job.http_port = 0;
        }
        if (job.rtsp_port != 0 || job.http_port != 0) {
            jobs.push_back(job);
        }
    }

    // Identification answers come within milliseconds on a LAN; a camera that holds the
    // connection open without answering must not stall the scan for the full RTSP timeout.
    const int exchange_ms =
        std::max(connect_ms, std::min(std::max(config.rtsp.timeout_ms, 1), 3000));
    std::vector<ServiceAnswers> answers(jobs.size());
    {
        std::atomic<std::size_t> next{0};
        const auto worker = [&]() {
            for (std::size_t index = next++; index < jobs.size(); index = next++) {
                const Job& job = jobs[index];
                ServiceAnswers& answer = answers[index];
                if (job.rtsp_port != 0) {
                    net::RtspProbeOptions options;
                    options.host = job.ip;
                    options.port = job.rtsp_port;
                    options.path = config.rtsp.main_path;
                    options.timeout_ms = exchange_ms;
                    options.method = "OPTIONS";
                    options.credentials = nullptr;
                    const net::RtspProbeResult reply = net::rtspProbe(options);
                    answer.rtsp_port = job.rtsp_port;
                    answer.rtsp_ok = reply.code > 0;
                    answer.rtsp_server = reply.server;
                    answer.rtsp_realm = reply.realm;
                    if (answer.rtsp_ok && answer.rtsp_realm.empty()) {
                        // No credentials: the 401 challenge is read, nothing is attempted, so it
                        // cannot count towards the camera's login lockout.
                        options.method = "DESCRIBE";
                        const net::RtspProbeResult challenge = net::rtspProbe(options);
                        answer.rtsp_realm = challenge.realm;
                        if (answer.rtsp_server.empty()) {
                            answer.rtsp_server = challenge.server;
                        }
                    }
                }
                if (job.http_port != 0) {
                    net::HttpRequestOptions request;
                    request.method = "GET";
                    request.path = "/";
                    request.credentials = nullptr;
                    request.timeout_ms = exchange_ms;
                    request.max_body_bytes = 64U * 1024U;
                    const net::HttpResult reply = net::httpRequest(job.ip, job.http_port, request);
                    if (reply.outcome == net::HttpOutcome::kResponse) {
                        answer.http_server = reply.response.header("Server").value_or("");
                    }
                }
            }
        };
        std::vector<std::thread> workers;
        const std::size_t count = std::min(kIdentifyParallel, jobs.size());
        for (std::size_t i = 1; i < count; ++i) {
            workers.emplace_back(worker);
        }
        if (count > 0) {
            worker();
        }
        for (std::thread& thread : workers) {
            thread.join();
        }
    }
    for (std::size_t i = 0; i < jobs.size(); ++i) {
        inputs.services[jobs[i].ip.value] = answers[i];
    }

    // --- Duplicate addresses ---------------------------------------------------------------
    if (settings.duplicate_ip_check && !lan_interface.empty()) {
        std::set<std::uint32_t> targets;
        for (const hikvision::SadpDevice& device : inputs.sadp) {
            targets.insert(sadpAddress(device).value);
        }
        for (const hikvision::WsDiscoveryMatch& match : inputs.onvif) {
            targets.insert(onvifAddress(match).value);
        }
        for (const auto& item : inputs.listed) {
            targets.insert(item.first.first.value);
        }
        for (const auto& host : open_ports) {
            if (std::any_of(rtsp_ports.begin(), rtsp_ports.end(), [&host](std::uint16_t port) {
                    return host.second.count(port) != 0U;
                })) {
                targets.insert(host.first);
            }
        }
        // ARP stays on the camera LAN wire (AF_PACKET on its interface), so addresses outside
        // the subnet (two factory-default cameras on 192.168.1.64) are probed as well, RFC 5227
        // style with sender 0.0.0.0 so no camera caches an address it cannot reach.
        std::vector<net::Ipv4> local;
        std::vector<net::Ipv4> foreign;
        for (const std::uint32_t value : targets) {
            const net::Ipv4 ip{value};
            if (ip.isZero() || own_addresses.count(value) != 0U || net::isLoopback(ip) ||
                net::isMulticast(ip)) {
                continue;
            }
            (lan.inCameraSubnet(ip) && !own_address.isZero() ? local : foreign).push_back(ip);
        }
        const int arp_ms = std::max(1, std::min(listen_ms, 1000));
        bool supported = false;
        std::string error;
        std::vector<std::string> duplicates;
        for (int pass = 0; pass < 2; ++pass) {
            const std::vector<net::Ipv4>& list = pass == 0 ? local : foreign;
            if (list.empty()) {
                continue;
            }
            const net::ArpProbeResult result =
                net::arpProbe(lan_interface, pass == 0 ? own_address : net::Ipv4{}, list, arp_ms);
            if (!result.supported) {
                error = result.error;
                break;
            }
            supported = true;
            for (const auto& reply : result.replies) {
                inputs.arp_probe[reply.first].assign(reply.second.begin(), reply.second.end());
                if (reply.second.size() >= 2) {
                    duplicates.push_back(net::toString(net::Ipv4{reply.first}));
                }
            }
        }
        const std::size_t checked = local.size() + foreign.size();
        if (supported) {
            report.methods.push_back(methodReport(
                "arp_probe", true, static_cast<int>(duplicates.size()),
                std::to_string(checked) + " addresses checked for duplicates" +
                    (duplicates.empty() ? std::string() : ": " + join(duplicates, ", "))));
        } else {
            report.methods.push_back(methodReport(
                "arp_probe", false, 0,
                checked == 0 ? "nothing to check"
                             : "not available (" + error +
                                   "); duplicates are detected from SADP answers only"));
        }
    } else {
        report.methods.push_back(methodReport(
            "arp_probe", false, 0,
            settings.duplicate_ip_check ? "no camera LAN interface is selected"
                                        : "duplicate_ip_check is false"));
    }

    for (const DiscoveryMethodReport& method : report.methods) {
        logMethod(method);
    }

    report.cameras = mergeDiscovery(inputs, lan, config);
    assignIds(report, registry, config, utcNowIso());

    for (const DiscoveredCamera& camera : report.cameras) {
        logEvent(LogLevel::kInfo, "camera_discovered",
                 LogFields()
                     .addQuoted("camera_id", camera.id)
                     .add("ip", net::toString(camera.ip))
                     .addQuoted("mac", camera.mac)
                     .addQuoted("vendor", camera.vendor)
                     .addQuoted("model", camera.model)
                     .add("found_by", describeSources(camera.sources))
                     .add("rtsp_ok", camera.rtsp_ok ? "true" : "false")
                     .add("problems", camera.problems.size()));
    }
    logEvent(LogLevel::kInfo, "camera_discovery_done",
             LogFields()
                 .add("cameras", report.cameras.size())
                 .addQuoted("interface", lan_interface)
                 .add("problems", report.problems.size()));
    return report;
}

std::string formatDiscoveryReport(const DiscoveryReport& report) {
    std::ostringstream out;
    out << "DISCOVERED CAMERAS: " << report.cameras.size() << '\n';
    for (const DiscoveredCamera& camera : report.cameras) {
        const std::string ip = net::toString(camera.ip);
        const auto or_unknown = [](const std::string& value) {
            return value.empty() ? std::string("unknown") : value;
        };
        out << '\n' << (camera.id.empty() ? std::string("(no id)") : camera.id) << '\n';
        out << "  Model: " << or_unknown(camera.model) << "   Vendor: " << or_unknown(camera.vendor)
            << '\n';
        out << "  IP: " << ip << "   MAC: " << or_unknown(camera.mac)
            << "   Serial: " << or_unknown(camera.serial) << '\n';
        if ((camera.sources & kFoundBySadp) != 0U) {
            out << "  SADP: "
                << (!camera.activated ? "activation unknown"
                                      : (*camera.activated ? "activated" : "NOT activated"))
                << ", DHCP " << (camera.dhcp ? "on" : "off");
            if (!camera.subnet_mask.empty()) {
                out << ", mask " << camera.subnet_mask;
            }
            if (!camera.gateway.empty()) {
                out << ", gateway " << camera.gateway;
            }
            if (!camera.firmware.empty()) {
                out << ", firmware " << camera.firmware;
            }
            out << '\n';
        }
        out << "  Found by: " << describeSources(camera.sources) << '\n';
        out << "  Ports: " << portsText(camera) << '\n';
        out << "  RTSP: " << rtspText(camera) << '\n';
        out << "  ONVIF: "
            << (camera.onvif_seen
                    ? "seen" + (camera.onvif_xaddr.empty() ? std::string()
                                                           : " (" + camera.onvif_xaddr + ")")
                    : std::string("not answering"))
            << '\n';
        DiagnosticContext context;
        context.camera_id = camera.id;
        context.ip = ip;
        context.stage = Stage::kDiscovery;
        for (const auto& problem : camera.problems) {
            out << indent(formatError(problem.first, context, problem.second), "  ");
        }
        for (const std::string& note : camera.notes) {
            out << "  note: " << note << '\n';
        }
    }
    if (!report.problems.empty()) {
        out << '\n';
        DiagnosticContext context;
        context.stage = Stage::kDiscovery;
        for (const auto& problem : report.problems) {
            out << formatError(problem.first, context, problem.second);
        }
    }
    out << "\nDISCOVERY METHODS\n";
    for (const DiscoveryMethodReport& method : report.methods) {
        out << "  " << method.method << ": "
            << (method.ran ? std::to_string(method.found) + " found" : std::string("skipped"));
        if (!method.detail.empty()) {
            out << " (" << method.detail << ")";
        }
        out << '\n';
    }
    return out.str();
}

}  // namespace anpr::cameras
