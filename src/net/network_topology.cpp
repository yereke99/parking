// Camera LAN and uplink detection from sysfs and procfs. Strictly read-only: routes and
// addresses belong to NetworkManager on the host, and the container must never change them.
#include "anpr/net/network_topology.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <tuple>

#include <arpa/inet.h>
#include <dirent.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "anpr/common/logging.hpp"

namespace anpr::net {
namespace {

constexpr unsigned long kRtfUp = 0x1UL;
constexpr unsigned long kRtfGateway = 0x2UL;
constexpr unsigned long kIffUp = 0x1UL;
constexpr int kArphrdEther = 1;
constexpr int kArphrdPpp = 512;
constexpr int kArphrdLoopback = 772;
constexpr const char* kDash = "\xE2\x80\x94";  // em dash, UTF-8

std::string trim(const std::string& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }
    return text.substr(begin, end - begin);
}

std::string toLower(std::string text) {
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

/// First line of a sysfs/procfs attribute, trimmed. nullopt when the file is missing, empty or
/// the read fails: sysfs `carrier` answers EINVAL while the interface is administratively down.
std::optional<std::string> readAttribute(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        return std::nullopt;
    }
    std::string line;
    if (!std::getline(in, line)) {
        return std::nullopt;
    }
    return trim(line);
}

std::string readWholeFile(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        return {};
    }
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

bool parseLong(const std::string& text, int base, long& out) {
    if (text.empty()) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const long value = std::strtol(text.c_str(), &end, base);
    if (errno != 0 || end == text.c_str() || *end != '\0') {
        return false;
    }
    out = value;
    return true;
}

bool parseUnsigned(const std::string& text, int base, unsigned long& out) {
    if (text.empty() || text[0] == '-') {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long value = std::strtoul(text.c_str(), &end, base);
    if (errno != 0 || end == text.c_str() || *end != '\0') {
        return false;
    }
    out = value;
    return true;
}

/// A /proc/net/route address column: the kernel prints the raw network-order word with %08X, so
/// the hex value read back as a native integer already has the in-memory layout of s_addr.
bool parseRouteWord(const std::string& text, Ipv4& out) {
    unsigned long raw = 0;
    if (text.size() > 8 || !parseUnsigned(text, 16, raw)) {
        return false;
    }
    out = Ipv4{ntohl(static_cast<std::uint32_t>(raw))};
    return true;
}

bool pathExists(const std::string& path) {
    struct stat info {};
    return ::stat(path.c_str(), &info) == 0;
}

bool isDirectory(const std::string& path) {
    struct stat info {};
    return ::stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

std::string linkTarget(const std::string& path) {
    char buffer[PATH_MAX];
    const ssize_t length = ::readlink(path.c_str(), buffer, sizeof(buffer) - 1);
    if (length <= 0) {
        return {};
    }
    return std::string(buffer, static_cast<std::size_t>(length));
}

std::string baseName(std::string path) {
    while (path.size() > 1 && path.back() == '/') {
        path.pop_back();
    }
    const std::size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string parentPath(const std::string& path) {
    const std::size_t slash = path.rfind('/');
    if (slash == std::string::npos) {
        return {};
    }
    return slash == 0 ? std::string("/") : path.substr(0, slash);
}

std::string realPath(const std::string& path) {
    char* resolved = ::realpath(path.c_str(), nullptr);
    if (resolved == nullptr) {
        return {};
    }
    std::string result(resolved);
    std::free(resolved);
    return result;
}

std::vector<std::string> listDirectory(const std::string& path) {
    std::vector<std::string> names;
    DIR* dir = ::opendir(path.c_str());
    if (dir == nullptr) {
        return names;
    }
    while (const dirent* entry = ::readdir(dir)) {
        const std::string name(entry->d_name);
        if (name != "." && name != "..") {
            names.push_back(name);
        }
    }
    ::closedir(dir);
    std::sort(names.begin(), names.end());
    return names;
}

std::map<std::string, std::string> readUevent(const std::string& path) {
    std::map<std::string, std::string> values;
    std::istringstream lines(readWholeFile(path));
    std::string line;
    while (std::getline(lines, line)) {
        const std::size_t equals = line.find('=');
        if (equals != std::string::npos) {
            values[trim(line.substr(0, equals))] = trim(line.substr(equals + 1));
        }
    }
    return values;
}

std::vector<std::string> pathComponents(const std::string& path) {
    std::vector<std::string> parts;
    std::istringstream in(path);
    std::string part;
    while (std::getline(in, part, '/')) {
        if (!part.empty()) {
            parts.push_back(part);
        }
    }
    return parts;
}

/// `path` relative to the sysfs root, so that only kernel device names are inspected and never
/// the directory a test tree happens to live in.
std::string relativeToRoot(const std::string& root, const std::string& path) {
    if (!root.empty() && path.size() > root.size() && path.compare(0, root.size(), root) == 0 &&
        path[root.size()] == '/') {
        return path.substr(root.size() + 1);
    }
    return path;
}

/// The Jetson's device-mode interfaces hang off the USB device controller ("700d0000.xudc")
/// through its "gadget" device, never off a USB host bus.
bool underUsbDeviceController(const std::string& relative_device_path) {
    for (const std::string& part : pathComponents(relative_device_path)) {
        if (part == "gadget" || part.find("udc") != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool isUsbRootHub(const std::string& part) {
    return part.size() > 3 && part.compare(0, 3, "usb") == 0 &&
           std::isdigit(static_cast<unsigned char>(part[3])) != 0;
}

/// Bus from the device path when the device has no `subsystem` link: a USB root hub component
/// ("usb1") wins over the PCI host bridge that may sit above it.
std::string busFromPath(const std::string& relative_device_path) {
    const std::vector<std::string> parts = pathComponents(relative_device_path);
    if (std::any_of(parts.begin(), parts.end(), isUsbRootHub)) {
        return "usb";
    }
    const bool pci = std::any_of(parts.begin(), parts.end(), [](const std::string& part) {
        return part.compare(0, 3, "pci") == 0;
    });
    return pci ? "pci" : "platform";
}

/// Drivers that USB modems use to present a network interface. cdc_ether, rndis_host and cdc_ncm
/// also drive plain USB Ethernet adapters, so those need the vendor check.
bool isModemCapableDriver(const std::string& driver) {
    static const std::set<std::string> kDrivers = {
        "cdc_ether", "rndis_host", "cdc_ncm", "huawei_cdc_ncm", "qmi_wwan",
        "cdc_mbim",  "option",     "sierra_net",
    };
    return kDrivers.count(driver) != 0;
}

/// Drivers that exist only for cellular modems.
bool isModemOnlyDriver(const std::string& driver) {
    return driver == "qmi_wwan" || driver == "cdc_mbim" || driver == "huawei_cdc_ncm";
}

std::string normalizeVendorId(const std::string& text) {
    std::string vendor = toLower(trim(text));
    if (vendor.size() > 2 && vendor.compare(0, 2, "0x") == 0) {
        vendor = vendor.substr(2);
    }
    return vendor;
}

const char* modemVendorName(const std::string& vendor) {
    static const std::map<std::string, const char*> kVendors = {
        {"12d1", "Huawei"},       {"19d2", "ZTE"},       {"1bbb", "Alcatel/TCL"},
        {"2c7c", "Quectel"},      {"1e0e", "SIMCom"},    {"1199", "Sierra Wireless"},
        {"2cb7", "Fibocom"},      {"1508", "Fibocom"},   {"05c6", "Qualcomm"},
        {"1bc7", "Telit"},        {"1546", "u-blox"},    {"0bdb", "Ericsson"},
        {"413c", "Dell Wireless"}, {"1410", "Novatel"},  {"0af0", "Option"},
        {"1e2d", "Cinterion"},
    };
    const auto found = kVendors.find(normalizeVendorId(vendor));
    return found == kVendors.end() ? nullptr : found->second;
}

/// Everything sysfs says about one interface, gathered before classification because a bridge is
/// classified by its ports.
struct SysfsFacts {
    std::string devtype;
    bool has_device{false};
    std::string device_path;
    bool gadget{false};
    bool bridge{false};
    bool wireless{false};
    std::vector<std::string> ports;
    long ifindex{LONG_MAX};
};

std::string linkStateText(const NetworkInterface& nic) {
    if (!nic.admin_up) {
        return "admin down";
    }
    if (!nic.carrier) {
        return "carrier unknown";
    }
    return *nic.carrier ? "carrier up" : "no carrier";
}

std::string hardwareText(const NetworkInterface& nic) {
    std::string text = nic.driver.empty() ? std::string("unknown driver") : nic.driver;
    if (!nic.bus.empty()) {
        text += " on " + nic.bus;
    }
    return text;
}

std::string joinNames(const std::vector<std::string>& names) {
    std::string text;
    for (const std::string& name : names) {
        if (!text.empty()) {
            text += ", ";
        }
        text += name;
    }
    return text;
}

/// Decides the kind from sysfs signals only. The order matters: a bridge or a gadget is never a
/// camera LAN even though its type is Ethernet, and a wwan/ppp link is cellular whatever it is
/// called.
void classify(NetworkInterface& nic, const SysfsFacts& facts,
              const std::map<std::string, SysfsFacts>& all) {
    std::string why;
    if (nic.type == kArphrdLoopback) {
        nic.kind = InterfaceKind::kLoopback;
        why = "loopback (type 772)";
    } else if (facts.gadget) {
        nic.kind = InterfaceKind::kUsbGadget;
        why = facts.devtype == "gadget" ? "USB device-mode gadget (DEVTYPE=gadget)"
                                        : "USB device-mode gadget (device under the UDC)";
    } else if (facts.bridge) {
        const bool gadget_ports =
            !facts.ports.empty() &&
            std::all_of(facts.ports.begin(), facts.ports.end(), [&all](const std::string& port) {
                const auto found = all.find(port);
                return found != all.end() && found->second.gadget;
            });
        if (gadget_ports) {
            nic.kind = InterfaceKind::kUsbGadget;
            why = "bridge of USB gadget ports " + joinNames(facts.ports);
        } else {
            nic.kind = InterfaceKind::kBridge;
            why = facts.ports.empty() ? std::string("Linux bridge without ports")
                                      : "Linux bridge with ports " + joinNames(facts.ports);
        }
    } else if (nic.type == kArphrdPpp) {
        nic.kind = InterfaceKind::kCellular;
        why = "PPP link (type 512)";
    } else if (facts.devtype == "wwan") {
        nic.kind = InterfaceKind::kCellular;
        why = hardwareText(nic) + ", DEVTYPE=wwan";
    } else if (!facts.has_device) {
        nic.kind = InterfaceKind::kVirtual;
        why = "software interface (no device link)";
    } else if (facts.wireless) {
        nic.kind = InterfaceKind::kWireless;
        why = hardwareText(nic) + ", wireless";
    } else if (isModemCapableDriver(nic.driver) &&
               (isCellularModemVendor(nic.usb_vendor_id) || isModemOnlyDriver(nic.driver))) {
        nic.kind = InterfaceKind::kCellular;
        if (isCellularModemVendor(nic.usb_vendor_id)) {
            why = hardwareText(nic) + ", modem vendor " + nic.usb_vendor_id + " (" +
                  modemVendorName(nic.usb_vendor_id) + ")";
        } else {
            why = hardwareText(nic) + ", WWAN-only driver";
        }
    } else if (nic.type == kArphrdEther) {
        nic.kind = InterfaceKind::kEthernet;
        why = hardwareText(nic);
        if (!nic.usb_vendor_id.empty()) {
            why += ", vendor " + nic.usb_vendor_id + " (not a modem maker)";
        }
    } else {
        nic.kind = InterfaceKind::kOther;
        why = hardwareText(nic) + ", type " + std::to_string(nic.type);
    }
    why += ", " + linkStateText(nic);
    if (!nic.master.empty()) {
        why += ", port of " + nic.master;
    }
    nic.reason = why;
}

std::uint32_t metricKey(int metric) {
    // The kernel prints the unsigned priority with %d, so very large metrics come back negative.
    return static_cast<std::uint32_t>(metric);
}

bool isPrimaryBus(const std::string& bus) {
    return bus == "pci" || bus == "platform";
}

std::optional<RouteEntry> activeDefaultRoute(const NetworkSnapshot& snapshot) {
    const std::vector<RouteEntry> defaults = snapshot.defaultRoutes();
    if (defaults.empty()) {
        return std::nullopt;
    }
    return defaults.front();
}

std::vector<Ipv4Network> lanNetworks(const NetworkInterface& nic) {
    std::vector<Ipv4Network> result;
    for (const InterfaceAddress& entry : nic.ipv4) {
        if (entry.address.isZero() || isLinkLocal(entry.address) || isLoopback(entry.address)) {
            continue;
        }
        const Ipv4Network candidate = entry.network();
        const bool duplicate =
            std::any_of(result.begin(), result.end(), [&candidate](const Ipv4Network& known) {
                return known.prefix == candidate.prefix &&
                       known.network() == candidate.network();
            });
        if (!duplicate) {
            result.push_back(candidate);
        }
    }
    return result;
}

/// Fills everything except `status` and `detail` from the chosen interface.
void describeChoice(CameraLanSelection& selection, const NetworkInterface& nic,
                    const NetworkSnapshot& snapshot) {
    selection.interface = nic.name;
    selection.address = nic.usableIpv4();
    selection.networks = lanNetworks(nic);
    const std::optional<RouteEntry> active = activeDefaultRoute(snapshot);
    selection.carries_default_route = active && active->interface == nic.name;
    for (const NetworkInterface& other : snapshot.interfaces) {
        if (other.name != nic.name && other.kind == InterfaceKind::kEthernet &&
            other.carrier.value_or(false)) {
            selection.other_candidates.push_back(other.name);
        }
    }
}

void judgeChoice(CameraLanSelection& selection, const NetworkInterface& nic) {
    const std::string label =
        nic.name + " (" + (nic.driver.empty() ? toString(nic.kind) : hardwareText(nic)) + ")";
    if (!nic.carrier.value_or(false)) {
        selection.status = CameraLanStatus::kNoLink;
        selection.detail = nic.admin_up ? label + " has no link: no cable or the switch is off"
                                        : label + " is administratively down";
    } else if (!selection.address) {
        selection.status = CameraLanStatus::kNoIpv4;
        selection.detail = label + " has link but no usable IPv4 address";
        if (!nic.master.empty()) {
            selection.detail += "; it is a port of " + nic.master +
                                ", whose addresses apply (set network.interface: " + nic.master +
                                ")";
        }
    } else {
        selection.status = CameraLanStatus::kReady;
        selection.detail = label + " has link and " +
                           selection.address->network().addressWithPrefix();
    }
    if (selection.carries_default_route) {
        selection.detail += "; it also carries the default route";
    }
}

std::string metricText(const RouteEntry& route) {
    std::string text = "(";
    if (route.via_gateway && !route.gateway.isZero()) {
        text += "gateway " + toString(route.gateway) + ", ";
    }
    return text + "metric " + std::to_string(route.metric) + ")";
}

std::string kindLabel(InterfaceKind kind) {
    switch (kind) {
        case InterfaceKind::kCellular:
            return "GSM";
        case InterfaceKind::kEthernet:
            return "wired";
        case InterfaceKind::kWireless:
            return "Wi-Fi";
        case InterfaceKind::kUsbGadget:
            return "USB gadget";
        case InterfaceKind::kLoopback:
            return "loopback";
        case InterfaceKind::kBridge:
            return "bridge";
        case InterfaceKind::kVirtual:
            return "virtual";
        case InterfaceKind::kOther:
            return "other";
    }
    return "other";
}

}  // namespace

std::string toString(InterfaceKind kind) {
    switch (kind) {
        case InterfaceKind::kLoopback:
            return "loopback";
        case InterfaceKind::kEthernet:
            return "ethernet";
        case InterfaceKind::kWireless:
            return "wireless";
        case InterfaceKind::kCellular:
            return "cellular";
        case InterfaceKind::kUsbGadget:
            return "usb_gadget";
        case InterfaceKind::kBridge:
            return "bridge";
        case InterfaceKind::kVirtual:
            return "virtual";
        case InterfaceKind::kOther:
            return "other";
    }
    return "other";
}

std::optional<InterfaceAddress> NetworkInterface::usableIpv4() const {
    for (const InterfaceAddress& entry : ipv4) {
        if (!entry.address.isZero() && !isLinkLocal(entry.address) &&
            !isLoopback(entry.address)) {
            return entry;
        }
    }
    return std::nullopt;
}

std::vector<RouteEntry> parseProcNetRoute(const std::string& text) {
    std::vector<RouteEntry> result;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        // Rows are tab separated and padded with spaces to 127 columns; whitespace splitting
        // handles both.
        std::istringstream columns(line);
        std::string name;
        std::string destination_text;
        std::string gateway_text;
        std::string flags_text;
        std::string refcnt_text;
        std::string use_text;
        std::string metric_text;
        std::string mask_text;
        if (!(columns >> name >> destination_text >> gateway_text >> flags_text >> refcnt_text >>
              use_text >> metric_text >> mask_text) ||
            name == "Iface") {
            continue;
        }
        RouteEntry route;
        Ipv4 mask;
        unsigned long flags = 0;
        long metric = 0;
        if (!parseRouteWord(destination_text, route.destination) ||
            !parseRouteWord(gateway_text, route.gateway) || !parseRouteWord(mask_text, mask) ||
            !parseUnsigned(flags_text, 16, flags) || !parseLong(metric_text, 10, metric)) {
            continue;
        }
        route.prefix = prefixFromNetmask(mask);
        if (route.prefix < 0) {
            continue;
        }
        route.interface = name;
        route.metric = static_cast<int>(metric);
        route.up = (flags & kRtfUp) != 0;
        route.via_gateway = (flags & kRtfGateway) != 0;
        result.push_back(route);
    }
    return result;
}

const NetworkInterface* NetworkSnapshot::find(const std::string& name) const {
    for (const NetworkInterface& nic : interfaces) {
        if (nic.name == name) {
            return &nic;
        }
    }
    return nullptr;
}

std::vector<RouteEntry> NetworkSnapshot::defaultRoutes() const {
    std::vector<RouteEntry> result;
    for (const RouteEntry& route : routes) {
        // "*" is a reject (unreachable/prohibit) route: it has no device and carries nothing.
        if (route.up && route.isDefault() && route.interface != "*") {
            result.push_back(route);
        }
    }
    std::stable_sort(result.begin(), result.end(), [](const RouteEntry& lhs, const RouteEntry& rhs) {
        return metricKey(lhs.metric) < metricKey(rhs.metric);
    });
    return result;
}

const RouteEntry* NetworkSnapshot::routeFor(Ipv4 destination) const {
    const RouteEntry* best = nullptr;
    for (const RouteEntry& route : routes) {
        if (!route.up || !Ipv4Network{route.destination, route.prefix}.contains(destination)) {
            continue;
        }
        if (best == nullptr || route.prefix > best->prefix ||
            (route.prefix == best->prefix && metricKey(route.metric) < metricKey(best->metric))) {
            best = &route;
        }
    }
    return best;
}

std::string NetworkSnapshot::connectedInterfaceFor(Ipv4 destination) const {
    const NetworkInterface* best = nullptr;
    int best_prefix = 0;
    for (const NetworkInterface& nic : interfaces) {
        for (const InterfaceAddress& entry : nic.ipv4) {
            if (entry.prefix > best_prefix && entry.network().contains(destination)) {
                best = &nic;
                best_prefix = entry.prefix;
            }
        }
    }
    return best == nullptr ? std::string() : best->name;
}

AddressProvider systemAddressProvider() {
    return []() {
        std::map<std::string, std::vector<InterfaceAddress>> result;
        ifaddrs* list = nullptr;
        if (::getifaddrs(&list) != 0) {
            return result;
        }
        for (const ifaddrs* entry = list; entry != nullptr; entry = entry->ifa_next) {
            if (entry->ifa_name == nullptr || entry->ifa_addr == nullptr ||
                entry->ifa_addr->sa_family != AF_INET) {
                continue;
            }
            // glibc reports a labelled secondary address as "eth0:1".
            std::string name(entry->ifa_name);
            name = name.substr(0, name.find(':'));
            sockaddr_in address{};
            std::memcpy(&address, entry->ifa_addr, sizeof(address));
            InterfaceAddress item;
            item.address = Ipv4{ntohl(address.sin_addr.s_addr)};
            item.prefix = 32;
            if (entry->ifa_netmask != nullptr) {
                sockaddr_in mask{};
                std::size_t length = sizeof(mask);
#if defined(__APPLE__) || defined(__FreeBSD__)
                // BSD routing sockets may hand back a netmask shorter than sockaddr_in.
                length = std::min<std::size_t>(length, entry->ifa_netmask->sa_len);
#endif
                std::memcpy(&mask, entry->ifa_netmask, length);
                const int prefix = prefixFromNetmask(Ipv4{ntohl(mask.sin_addr.s_addr)});
                item.prefix = prefix < 0 ? 32 : prefix;
            }
            std::vector<InterfaceAddress>& known = result[name];
            const bool duplicate = std::any_of(
                known.begin(), known.end(), [&item](const InterfaceAddress& existing) {
                    return existing.address == item.address && existing.prefix == item.prefix;
                });
            if (!duplicate) {
                known.push_back(item);
            }
        }
        ::freeifaddrs(list);
        return result;
    };
}

NetworkSnapshot readNetworkSnapshot(const SystemPaths& paths, const AddressProvider& addresses) {
    NetworkSnapshot snapshot;
    const std::string sysfs_root = realPath(paths.sys_class_net + "/../..");

    std::map<std::string, SysfsFacts> facts;
    std::vector<NetworkInterface> found;
    for (const std::string& name : listDirectory(paths.sys_class_net)) {
        const std::string dir = paths.sys_class_net + "/" + name;
        if (!isDirectory(dir)) {
            continue;  // bonding_masters and other plain files
        }
        NetworkInterface nic;
        nic.name = name;
        SysfsFacts fact;
        fact.devtype = readUevent(dir + "/uevent")["DEVTYPE"];

        long number = 0;
        if (parseLong(readAttribute(dir + "/type").value_or(""), 10, number)) {
            nic.type = static_cast<int>(number);
        }
        if (parseLong(readAttribute(dir + "/ifindex").value_or(""), 10, number)) {
            fact.ifindex = number;
        }
        unsigned long flags = 0;
        if (parseUnsigned(readAttribute(dir + "/flags").value_or(""), 16, flags)) {
            nic.admin_up = (flags & kIffUp) != 0;
        }
        const std::optional<std::string> carrier = readAttribute(dir + "/carrier");
        if (carrier && (*carrier == "1" || *carrier == "0")) {
            nic.carrier = *carrier == "1";
        }
        nic.operstate = readAttribute(dir + "/operstate").value_or("");
        nic.mac = normalizeMac(readAttribute(dir + "/address").value_or(""));
        nic.master = baseName(linkTarget(dir + "/master"));
        if (nic.master.empty()) {
            nic.master = baseName(linkTarget(dir + "/brport/bridge"));
        }

        fact.has_device = pathExists(dir + "/device");
        if (fact.has_device) {
            fact.device_path = relativeToRoot(sysfs_root, realPath(dir + "/device"));
            nic.driver = baseName(linkTarget(dir + "/device/driver"));
            nic.bus = baseName(linkTarget(dir + "/device/subsystem"));
            if (nic.bus.empty()) {
                nic.bus = busFromPath(fact.device_path);
            }
            if (nic.bus == "usb") {
                // The netdev's parent is the USB interface; idVendor lives on its parent device.
                const std::string device = realPath(dir + "/device");
                std::string vendor = readAttribute(device + "/idVendor").value_or("");
                if (vendor.empty()) {
                    vendor = readAttribute(parentPath(device) + "/idVendor").value_or("");
                }
                nic.usb_vendor_id = normalizeVendorId(vendor);
            }
        } else {
            nic.bus = "virtual";
        }
        fact.gadget = fact.devtype == "gadget" ||
                      (fact.has_device && underUsbDeviceController(fact.device_path)) ||
                      underUsbDeviceController(relativeToRoot(sysfs_root, realPath(dir)));
        fact.bridge = fact.devtype == "bridge" || isDirectory(dir + "/bridge");
        fact.wireless = fact.devtype == "wlan" || pathExists(dir + "/phy80211") ||
                        isDirectory(dir + "/wireless");
        if (fact.bridge) {
            fact.ports = listDirectory(dir + "/brif");
        }
        facts[name] = fact;
        found.push_back(nic);
    }

    // Ports whose `master` names a bridge count even when the bridge's brif/ is unreadable.
    for (const NetworkInterface& nic : found) {
        const auto bridge = facts.find(nic.master);
        if (!nic.master.empty() && bridge != facts.end() && bridge->second.bridge) {
            std::vector<std::string>& ports = bridge->second.ports;
            if (std::find(ports.begin(), ports.end(), nic.name) == ports.end()) {
                ports.push_back(nic.name);
                std::sort(ports.begin(), ports.end());
            }
        }
    }

    std::map<std::string, std::vector<InterfaceAddress>> by_name;
    if (addresses) {
        for (const auto& item : addresses()) {
            std::vector<InterfaceAddress>& known = by_name[item.first.substr(0, item.first.find(':'))];
            known.insert(known.end(), item.second.begin(), item.second.end());
        }
    }
    for (NetworkInterface& nic : found) {
        classify(nic, facts[nic.name], facts);
        const auto assigned = by_name.find(nic.name);
        if (assigned != by_name.end()) {
            nic.ipv4 = assigned->second;
        }
    }
    std::stable_sort(found.begin(), found.end(),
                     [&facts](const NetworkInterface& lhs, const NetworkInterface& rhs) {
                         const long left = facts.at(lhs.name).ifindex;
                         const long right = facts.at(rhs.name).ifindex;
                         return left != right ? left < right : lhs.name < rhs.name;
                     });
    snapshot.interfaces = std::move(found);
    snapshot.routes = parseProcNetRoute(readWholeFile(paths.proc_net_route));
    return snapshot;
}

NetworkSnapshot readNetworkSnapshot() {
    return readNetworkSnapshot(SystemPaths{}, systemAddressProvider());
}

bool isCellularModemVendor(const std::string& usb_vendor_id) {
    return modemVendorName(usb_vendor_id) != nullptr;
}

std::string toString(CameraLanStatus status) {
    switch (status) {
        case CameraLanStatus::kReady:
            return "ready";
        case CameraLanStatus::kNoIpv4:
            return "no_ipv4";
        case CameraLanStatus::kNoLink:
            return "no_link";
        case CameraLanStatus::kNoInterface:
            return "no_interface";
        case CameraLanStatus::kOverrideMissing:
            return "override_missing";
    }
    return "no_interface";
}

bool CameraLanSelection::inCameraSubnet(Ipv4 ip) const {
    return std::any_of(networks.begin(), networks.end(),
                       [ip](const Ipv4Network& network) { return network.contains(ip); });
}

CameraLanSelection selectCameraLan(const NetworkSnapshot& snapshot,
                                   const std::string& override_name) {
    CameraLanSelection selection;
    const std::string wanted = trim(override_name);
    if (!wanted.empty() && wanted != "auto") {
        selection.from_override = true;
        const NetworkInterface* nic = snapshot.find(wanted);
        if (nic == nullptr) {
            selection.status = CameraLanStatus::kOverrideMissing;
            selection.interface = wanted;
            selection.detail = "configured interface " + wanted + " does not exist";
            return selection;
        }
        describeChoice(selection, *nic, snapshot);
        judgeChoice(selection, *nic);
        if (nic->kind != InterfaceKind::kEthernet) {
            selection.detail += " (configured; detected as " + toString(nic->kind) + ")";
        }
        return selection;
    }

    std::vector<const NetworkInterface*> wired;
    for (const NetworkInterface& nic : snapshot.interfaces) {
        if (nic.kind == InterfaceKind::kEthernet) {
            wired.push_back(&nic);
        }
    }
    if (wired.empty()) {
        selection.status = CameraLanStatus::kNoInterface;
        selection.detail = "no wired Ethernet interface found";
        return selection;
    }

    const std::optional<RouteEntry> active = activeDefaultRoute(snapshot);
    const auto carriesDefault = [&active](const NetworkInterface* nic) {
        return active && active->interface == nic->name;
    };
    // Lower rank wins: usable address, then not the Internet path, then onboard over USB.
    const auto rank = [&carriesDefault](const NetworkInterface* nic) {
        return std::make_tuple(nic->usableIpv4() ? 0 : 1, carriesDefault(nic) ? 1 : 0,
                               isPrimaryBus(nic->bus) ? 0 : 1, nic->name);
    };
    std::vector<const NetworkInterface*> linked;
    for (const NetworkInterface* nic : wired) {
        if (nic->carrier.value_or(false)) {
            linked.push_back(nic);
        }
    }
    std::vector<const NetworkInterface*>& pool = linked.empty() ? wired : linked;
    std::sort(pool.begin(), pool.end(),
              [&rank](const NetworkInterface* lhs, const NetworkInterface* rhs) {
                  return rank(lhs) < rank(rhs);
              });
    describeChoice(selection, *pool.front(), snapshot);
    judgeChoice(selection, *pool.front());
    return selection;
}

std::string toString(InternetReach reach) {
    switch (reach) {
        case InternetReach::kNotChecked:
            return "not_checked";
        case InternetReach::kOnline:
            return "online";
        case InternetReach::kOffline:
            return "offline";
    }
    return "not_checked";
}

InternetStatus inspectInternet(const NetworkSnapshot& snapshot, bool probe,
                               const std::vector<std::string>& probe_targets, int timeout_ms) {
    InternetStatus status;
    const std::vector<RouteEntry> defaults = snapshot.defaultRoutes();
    if (defaults.empty()) {
        // Nothing to probe through: the kernel would answer ENETUNREACH. NO_INTERNET_ROUTE
        // covers this case, so the reach stays "not checked" rather than "offline".
        return status;
    }
    const RouteEntry& active = defaults.front();
    status.has_default_route = true;
    status.interface = active.interface;
    status.gateway = active.gateway;
    status.metric = active.metric;
    if (const NetworkInterface* nic = snapshot.find(active.interface)) {
        status.kind = nic->kind;
    }
    for (std::size_t i = 1; i < defaults.size(); ++i) {
        const std::string& name = defaults[i].interface;
        if (name != active.interface && std::find(status.backup_interfaces.begin(),
                                                  status.backup_interfaces.end(),
                                                  name) == status.backup_interfaces.end()) {
            status.backup_interfaces.push_back(name);
        }
    }
    if (!probe || probe_targets.empty()) {
        return status;
    }

    std::vector<std::string> tried;
    for (const std::string& target : probe_targets) {
        Ipv4 host;
        std::uint16_t port = 53;
        if (!parseHostPort(trim(target), host, port)) {
            logEvent(LogLevel::kWarn, "internet_probe_target_invalid",
                     LogFields().addQuoted("target", target));
            continue;
        }
        const std::string label = toString(host) + ":" + std::to_string(port);
        const ConnectResult result = tcpConnect(host, port, timeout_ms);
        logEvent(LogLevel::kDebug, "internet_probe",
                 LogFields()
                     .add("target", label)
                     .add("outcome", toString(result.outcome))
                     .add("elapsed_ms", static_cast<long long>(std::lround(result.elapsed_ms))));
        // A refusal is an answer too: a RST from the far end (or the modem's NAT) proves that
        // packets leave and come back, which is all this check is about.
        if (result.outcome == ConnectOutcome::kConnected ||
            result.outcome == ConnectOutcome::kRefused) {
            status.reach = InternetReach::kOnline;
            status.probe_target = label;
            status.probe_ms = result.elapsed_ms;
            return status;
        }
        tried.push_back(label);
    }
    status.reach = InternetReach::kOffline;
    // When offline the field lists every target that stayed silent, for the report.
    status.probe_target = joinNames(tried);
    status.probe_ms = 0.0;
    return status;
}

std::vector<std::string> overlappingSubnets(const NetworkSnapshot& snapshot,
                                            const CameraLanSelection& lan) {
    std::vector<std::string> result;
    if (lan.networks.empty()) {
        return result;
    }
    for (const NetworkInterface& nic : snapshot.interfaces) {
        if (nic.name == lan.interface || nic.kind == InterfaceKind::kLoopback) {
            continue;
        }
        for (const InterfaceAddress& entry : nic.ipv4) {
            if (isLoopback(entry.address) || isLinkLocal(entry.address)) {
                continue;
            }
            const Ipv4Network network = entry.network();
            const bool overlaps =
                std::any_of(lan.networks.begin(), lan.networks.end(),
                            [&network](const Ipv4Network& camera) {
                                return camera.overlaps(network);
                            });
            const std::string text = nic.name + " " + network.cidr();
            if (overlaps && std::find(result.begin(), result.end(), text) == result.end()) {
                result.push_back(text);
            }
        }
    }
    return result;
}

std::string formatNetworkSummary(const NetworkSnapshot& snapshot, const CameraLanSelection& lan,
                                 const InternetStatus& internet) {
    std::ostringstream out;
    out << "CAMERA NETWORK\n";
    const NetworkInterface* camera = lan.interface.empty() ? nullptr : snapshot.find(lan.interface);
    if (lan.status == CameraLanStatus::kNoInterface) {
        out << "  interface: NONE (no wired Ethernet interface found)\n";
    } else if (lan.status == CameraLanStatus::kOverrideMissing) {
        out << "  interface: " << lan.interface << " (configured, NOT FOUND)\n";
    } else {
        std::vector<std::string> labels;
        if (camera != nullptr) {
            if (!camera->driver.empty()) {
                labels.push_back(camera->driver);
            }
            if (!camera->bus.empty() && camera->bus != "virtual") {
                labels.push_back(camera->bus);
            }
            if (labels.empty()) {
                labels.push_back(toString(camera->kind));
            }
        }
        if (lan.from_override) {
            labels.push_back("configured");
        }
        out << "  interface: " << lan.interface;
        if (!labels.empty()) {
            out << " (" << joinNames(labels) << ")";
        }
        out << '\n';
        if (lan.status == CameraLanStatus::kNoLink) {
            const bool admin_down = camera != nullptr && !camera->admin_up;
            out << "  link: DOWN ("
                << (admin_down ? "interface is administratively down"
                               : "no carrier: check the cable to the PoE switch")
                << ")\n";
        } else {
            out << "  link: UP\n";
            if (lan.status == CameraLanStatus::kNoIpv4) {
                out << "  ip: NONE (no static IPv4 address; the PoE switch has no DHCP)\n";
            } else {
                std::vector<std::string> ips;
                std::vector<std::string> subnets;
                for (const Ipv4Network& network : lan.networks) {
                    ips.push_back(network.addressWithPrefix());
                    subnets.push_back(network.cidr());
                }
                out << "  ip: " << joinNames(ips) << '\n';
                out << "  subnet: " << joinNames(subnets) << '\n';
            }
        }
        if (!lan.other_candidates.empty()) {
            out << "  other wired links: " << joinNames(lan.other_candidates) << '\n';
        }
        for (const std::string& overlap : overlappingSubnets(snapshot, lan)) {
            out << "  subnet conflict: " << overlap << ' ' << kDash << " WARNING\n";
        }
    }

    out << "INTERNET\n";
    if (!internet.has_default_route) {
        out << "  interface: none\n";
        out << "  default route: NONE (camera processing does not need it)\n";
        return out.str();
    }
    const NetworkInterface* uplink = snapshot.find(internet.interface);
    out << "  interface: " << internet.interface << " (" << toString(internet.kind);
    if (uplink != nullptr && !uplink->driver.empty()) {
        out << ", " << uplink->driver;
    }
    out << ")\n";

    RouteEntry active;
    active.interface = internet.interface;
    active.gateway = internet.gateway;
    active.via_gateway = !internet.gateway.isZero();
    active.metric = internet.metric;
    out << "  default route: ";
    if (!lan.interface.empty() && internet.interface == lan.interface) {
        out << "camera LAN " << internet.interface << ' ' << metricText(active)
            << ' ' << kDash << " WARNING: Internet traffic goes to the PoE switch, not GSM\n";
    } else {
        const std::string via = internet.kind == InterfaceKind::kCellular ? " via " : " ";
        out << kindLabel(internet.kind) << via << internet.interface << ' ' << metricText(active)
            << '\n';
    }
    if (!internet.backup_interfaces.empty()) {
        std::vector<std::string> backups;
        for (const RouteEntry& route : snapshot.defaultRoutes()) {
            if (std::find(internet.backup_interfaces.begin(), internet.backup_interfaces.end(),
                          route.interface) != internet.backup_interfaces.end() &&
                route.interface != internet.interface) {
                backups.push_back(route.interface + " (metric " + std::to_string(route.metric) +
                                  ")");
            }
        }
        if (backups.empty()) {
            backups = internet.backup_interfaces;
        }
        out << "  backup routes: " << joinNames(backups) << '\n';
    }
    out << "  status: ";
    switch (internet.reach) {
        case InternetReach::kOnline:
            out << "ONLINE (" << internet.probe_target << " in "
                << static_cast<long long>(std::lround(internet.probe_ms)) << " ms)";
            break;
        case InternetReach::kOffline:
            out << "OFFLINE (no answer";
            if (!internet.probe_target.empty()) {
                out << " from " << internet.probe_target;
            }
            out << "; camera processing continues)";
            break;
        case InternetReach::kNotChecked:
            out << "not checked";
            break;
    }
    out << '\n';
    return out.str();
}

}  // namespace anpr::net
