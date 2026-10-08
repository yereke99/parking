#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "anpr/net/socket.hpp"

namespace anpr::net {

/// What a network interface is, decided from sysfs only (never from its name alone: `usb0` on a
/// Jetson is the USB device-mode gadget, not a modem, and a GSM modem may appear as `eth1`).
enum class InterfaceKind {
    kLoopback,
    /// Physical wired Ethernet: onboard (PCI/platform) or a USB Ethernet adapter. The camera LAN
    /// is one of these.
    kEthernet,
    kWireless,
    /// A cellular modem: ppp, wwan (qmi_wwan, cdc_mbim) or a USB modem presenting Ethernet
    /// (cdc_ether / rndis_host / cdc_ncm / huawei_cdc_ncm from a known modem vendor).
    kCellular,
    /// The Jetson's own USB device-mode interfaces (rndis0, usb0) and their bridge l4tbr0.
    kUsbGadget,
    /// A Linux bridge (docker0, br-*, l4tbr0 when not recognised as the gadget bridge).
    kBridge,
    /// veth, tun, tap, dummy, vxlan and other software interfaces.
    kVirtual,
    kOther,
};

std::string toString(InterfaceKind kind);

struct InterfaceAddress {
    Ipv4 address;
    int prefix{0};

    [[nodiscard]] Ipv4Network network() const { return Ipv4Network{address, prefix}; }
};

struct NetworkInterface {
    std::string name;
    InterfaceKind kind{InterfaceKind::kOther};
    /// Kernel driver module ("r8168", "cdc_ether", "qmi_wwan"), empty for software interfaces.
    std::string driver;
    /// "pci", "usb", "platform", "virtual" or empty.
    std::string bus;
    /// USB idVendor of the device behind a USB interface ("12d1" for Huawei), else empty.
    std::string usb_vendor_id;
    std::string mac;
    /// ARPHRD type from sysfs: 1 Ethernet, 512 PPP, 772 loopback, 65534 none.
    int type{0};
    /// IFF_UP: administratively enabled.
    bool admin_up{false};
    /// Physical link (cable plugged into a powered switch). nullopt when the kernel cannot say,
    /// for example for an interface that is administratively down.
    std::optional<bool> carrier;
    std::string operstate;
    /// Bridge this interface is enslaved to, if any.
    std::string master;
    std::vector<InterfaceAddress> ipv4;
    /// One line explaining the classification, for the startup report.
    std::string reason;

    /// The first address that is neither link-local nor loopback.
    [[nodiscard]] std::optional<InterfaceAddress> usableIpv4() const;
};

struct RouteEntry {
    std::string interface;
    Ipv4 destination;
    int prefix{0};
    Ipv4 gateway;
    int metric{0};
    bool up{false};
    bool via_gateway{false};

    [[nodiscard]] bool isDefault() const { return prefix == 0; }
};

/// Parses /proc/net/route (hex little-endian fields, the first line is the header).
std::vector<RouteEntry> parseProcNetRoute(const std::string& text);

struct NetworkSnapshot {
    std::vector<NetworkInterface> interfaces;
    std::vector<RouteEntry> routes;

    [[nodiscard]] const NetworkInterface* find(const std::string& name) const;
    /// Default routes that are up, lowest metric first. The first one carries Internet traffic.
    [[nodiscard]] std::vector<RouteEntry> defaultRoutes() const;
    /// The route the kernel would use for `destination`: longest prefix, then lowest metric.
    /// nullptr when nothing matches.
    [[nodiscard]] const RouteEntry* routeFor(Ipv4 destination) const;
    /// Name of the interface whose own subnet contains `destination` (a directly connected
    /// host), or empty.
    [[nodiscard]] std::string connectedInterfaceFor(Ipv4 destination) const;
};

/// Where the snapshot reads from. Tests point these at a fake tree.
struct SystemPaths {
    std::string sys_class_net{"/sys/class/net"};
    std::string proc_net_route{"/proc/net/route"};
};

/// IPv4 addresses per interface name. The default implementation uses getifaddrs(), which also
/// returns secondary addresses.
using AddressProvider = std::function<std::map<std::string, std::vector<InterfaceAddress>>()>;
AddressProvider systemAddressProvider();

/// Reads every interface from `paths.sys_class_net`, classifies it, attaches its IPv4 addresses
/// and reads the routing table. Never fails: missing files leave fields empty.
NetworkSnapshot readNetworkSnapshot(const SystemPaths& paths, const AddressProvider& addresses);
NetworkSnapshot readNetworkSnapshot();

/// USB vendor ids of cellular modem makers (Huawei, ZTE, Quectel, Sierra, Telit, Fibocom,
/// SIMCom, u-blox, Alcatel/TCL, ...). Used to tell a HiLink modem from a USB Ethernet adapter
/// when both bind cdc_ether.
[[nodiscard]] bool isCellularModemVendor(const std::string& usb_vendor_id);

enum class CameraLanStatus {
    /// A wired interface with link and a usable IPv4 address.
    kReady,
    /// A wired interface has link but no usable IPv4 address (no DHCP on the PoE switch).
    kNoIpv4,
    /// Wired interfaces exist but none has link.
    kNoLink,
    /// No wired Ethernet interface at all.
    kNoInterface,
    /// The configured interface does not exist.
    kOverrideMissing,
};

std::string toString(CameraLanStatus status);

struct CameraLanSelection {
    CameraLanStatus status{CameraLanStatus::kNoInterface};
    /// The chosen interface (also set for kNoIpv4 / kNoLink so the report can name it).
    std::string interface;
    /// True when `interface` came from the configuration rather than auto-detection.
    bool from_override{false};
    std::optional<InterfaceAddress> address;
    /// Every IPv4 network on the interface (secondary addresses included, link-local excluded).
    std::vector<Ipv4Network> networks;
    /// The interface also carries the active default route: Internet traffic would go to the
    /// PoE switch instead of the GSM modem.
    bool carries_default_route{false};
    /// Other wired interfaces with link, for the report.
    std::vector<std::string> other_candidates;
    std::string detail;

    [[nodiscard]] bool ready() const { return status == CameraLanStatus::kReady; }
    /// True when `ip` lies in one of `networks`.
    [[nodiscard]] bool inCameraSubnet(Ipv4 ip) const;
};

/// Picks the camera LAN. With `override_name` ("" or "auto" means automatic) that interface is
/// used whatever its kind. Automatically: wired Ethernet with carrier, preferring one with a
/// usable address and one that does not carry the default route.
CameraLanSelection selectCameraLan(const NetworkSnapshot& snapshot,
                                   const std::string& override_name);

enum class InternetReach { kNotChecked, kOnline, kOffline };
std::string toString(InternetReach reach);

struct InternetStatus {
    bool has_default_route{false};
    std::string interface;
    InterfaceKind kind{InterfaceKind::kOther};
    Ipv4 gateway;
    int metric{0};
    InternetReach reach{InternetReach::kNotChecked};
    std::string probe_target;
    double probe_ms{0.0};
    /// Further default routes with higher metrics.
    std::vector<std::string> backup_interfaces;
};

/// Reports the default route and, when `probe` is set, whether a TCP connect to any of
/// `probe_targets` ("1.1.1.1:53") succeeds within `timeout_ms`. Read-only: never touches routes.
InternetStatus inspectInternet(const NetworkSnapshot& snapshot, bool probe,
                               const std::vector<std::string>& probe_targets, int timeout_ms);

/// Subnets of other interfaces (Docker bridges, the USB gadget, the modem) that overlap the
/// camera LAN. Each entry is "docker0 172.17.0.0/16". Overlaps make camera traffic leave through
/// the wrong interface.
std::vector<std::string> overlappingSubnets(const NetworkSnapshot& snapshot,
                                            const CameraLanSelection& lan);

/// The multi-line CAMERA NETWORK / INTERNET block printed at startup.
std::string formatNetworkSummary(const NetworkSnapshot& snapshot, const CameraLanSelection& lan,
                                 const InternetStatus& internet);

}  // namespace anpr::net
