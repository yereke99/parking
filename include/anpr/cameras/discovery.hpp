#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "anpr/cameras/camera_mode_config.hpp"
#include "anpr/cameras/camera_registry.hpp"
#include "anpr/cameras/diagnostics.hpp"
#include "anpr/hikvision/onvif.hpp"
#include "anpr/hikvision/sadp.hpp"
#include "anpr/net/arp.hpp"
#include "anpr/net/network_topology.hpp"
#include "anpr/net/socket.hpp"

namespace anpr::cameras {

/// How a camera was found. A camera found several ways carries several bits.
enum DiscoverySource : unsigned {
    kFoundBySadp = 1U << 0U,
    kFoundByOnvif = 1U << 1U,
    kFoundByArp = 1U << 2U,
    kFoundByScan = 1U << 3U,
    kFoundByManual = 1U << 4U,
    kFoundByRegistry = 1U << 5U,
};

/// "sadp,onvif,scan"; "none" for 0.
std::string describeSources(unsigned sources);

/// Natural order of camera ids ("camera-2" before "camera-10", "camera-01" before "camera-02"),
/// used for every list in id order.
bool cameraIdLess(const std::string& lhs, const std::string& rhs);

/// A device on (or visible from) the camera LAN that looks like an IP camera.
struct DiscoveredCamera {
    /// Stable id from the registry ("camera-01"), assigned at the end of discovery.
    std::string id;
    net::Ipv4 ip;
    std::string mac;  ///< normalized, may be empty
    /// "Hikvision" when any evidence says so (SADP, ONVIF scopes, ISAPI, HTTP server, OUI).
    std::string vendor;
    std::string model;
    std::string serial;
    std::string device_id;
    std::string firmware;
    std::string device_type;
    unsigned sources{0};

    /// The address lies in a subnet configured on the camera LAN interface, so traffic to it
    /// stays on the PoE switch. False for, e.g., a factory-default 192.168.1.64 seen by SADP
    /// while the Jetson sits in 192.168.10.0/24: such a camera is reported, never contacted.
    bool on_camera_subnet{false};
    /// From SADP: nullopt when unknown.
    std::optional<bool> activated;
    bool dhcp{false};
    std::string subnet_mask;
    std::string gateway;

    /// TCP probe results by port.
    std::map<std::uint16_t, net::ConnectOutcome> ports;
    std::uint16_t rtsp_port{554};
    std::uint16_t http_port{80};
    std::uint16_t sdk_port{8000};
    /// RTSP answered OPTIONS (no credentials sent).
    bool rtsp_ok{false};
    std::string rtsp_server;
    std::string http_server;

    /// ONVIF WS-Discovery answer, if any.
    bool onvif_seen{false};
    std::string onvif_endpoint;
    std::string onvif_xaddr;
    std::vector<std::string> onvif_scopes;

    /// Problems specific to this device (CAMERA_ON_OTHER_SUBNET, DUPLICATE_IP_DETECTED,
    /// CAMERA_NOT_ACTIVATED, ONVIF_DISABLED_OR_UNAVAILABLE, ...) with a detail text each.
    std::vector<std::pair<CameraError, std::string>> problems;
    /// Other MACs seen answering for this IP.
    std::vector<std::string> conflicting_macs;
    /// Free-form remarks for the scan report.
    std::vector<std::string> notes;

    [[nodiscard]] bool isHikvision() const { return vendor == "Hikvision"; }
    [[nodiscard]] bool hasProblem(CameraError error) const;
};

struct DiscoveryMethodReport {
    std::string method;  ///< "sadp", "onvif", "arp", "scan", "manual", "registry"
    bool ran{false};
    int found{0};
    std::string detail;
};

struct DiscoveryReport {
    std::vector<DiscoveredCamera> cameras;
    std::vector<DiscoveryMethodReport> methods;
    /// Problems that are not about one device (NO_CAMERAS_DISCOVERED, CAMERA_LIMIT_REACHED, ...).
    std::vector<std::pair<CameraError, std::string>> problems;
};

/// What one address answered without credentials: RTSP OPTIONS, the realm of a credential-less
/// DESCRIBE's 401 (reading a challenge is not a login attempt) and the `Server` header of an
/// HTTP GET /. Vendor evidence and `rtsp_ok` come from here.
struct ServiceAnswers {
    /// The RTSP port that was asked, 0 when none was open.
    std::uint16_t rtsp_port{0};
    /// A valid RTSP response arrived (any status: a 401 still proves an RTSP server).
    bool rtsp_ok{false};
    std::string rtsp_server;
    std::string rtsp_realm;
    std::string http_server;
};

/// Raw inputs to the merge step, gathered from the network (or built by tests).
struct DiscoveryInputs {
    std::vector<hikvision::SadpDevice> sadp;
    std::vector<hikvision::WsDiscoveryMatch> onvif;
    std::vector<net::ArpEntry> arp;
    std::vector<net::PortProbe> ports;
    /// Manual hosts and registry addresses with their RTSP port, tagged with the source bit.
    std::vector<std::pair<std::pair<net::Ipv4, std::uint16_t>, unsigned>> listed;
    /// ARP probe replies: address value -> MACs that answered.
    std::map<std::uint32_t, std::vector<std::string>> arp_probe;
    /// Unauthenticated identification answers by address value.
    std::map<std::uint32_t, ServiceAnswers> services;
    /// Remarks for the report by address value, for example why an address was not probed.
    std::map<std::uint32_t, std::vector<std::string>> notes;
    /// WS-Discovery actually ran. ONVIF_DISABLED_OR_UNAVAILABLE is only reported after it did
    /// (and when discovery.onvif is on): without an address on the camera LAN it cannot run.
    bool onvif_ran{true};
};

/// Pure merge: one DiscoveredCamera per physical device (keyed by MAC when known, otherwise by
/// IP), vendor and identity filled from every source, devices classified as cameras when they
/// answer SADP or ONVIF or have an RTSP port open (hosts the operator listed, kFoundByManual, are
/// kept even while silent), problems attached (other subnet, duplicate IP, not activated, ONVIF
/// silent). Ports listed in `ports` are attached by IP. Order: by IP, then MAC.
std::vector<DiscoveredCamera> mergeDiscovery(const DiscoveryInputs& inputs,
                                             const net::CameraLanSelection& lan,
                                             const CameraModeConfig& config);

/// Assigns stable ids from `registry` (adding new devices), applies `max_cameras` and adds
/// CAMERA_LIMIT_REACHED / NO_CAMERAS_DISCOVERED problems. Cameras come back sorted by id.
void assignIds(DiscoveryReport& report, CameraRegistry& registry, const CameraModeConfig& config,
               const std::string& now_iso);

/// The whole discovery on the camera LAN: SADP, WS-Discovery, neighbour table, manual and
/// registry hosts, subnet scan when needed, TCP probes of the RTSP/HTTP/SDK ports, RTSP OPTIONS
/// and an unauthenticated HTTP request for identification, duplicate-IP ARP probes, then
/// `mergeDiscovery` and `assignIds`. Never sends credentials. Never contacts an address whose
/// route leaves through another interface (the GSM modem).
DiscoveryReport discoverCameras(const net::NetworkSnapshot& snapshot,
                                const net::CameraLanSelection& lan, const CameraModeConfig& config,
                                CameraRegistry& registry);

/// The scan report printed by `make camera-scan`.
std::string formatDiscoveryReport(const DiscoveryReport& report);

}  // namespace anpr::cameras
