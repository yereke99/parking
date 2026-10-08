#include "anpr/cameras/system_stats.hpp"
#include "anpr/common/filesystem.hpp"
#include "anpr/net/arp.hpp"
#include "anpr/net/network_topology.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <system_error>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "test_framework.hpp"

using anpr::net::AddressProvider;
using anpr::net::CameraLanSelection;
using anpr::net::CameraLanStatus;
using anpr::net::InterfaceAddress;
using anpr::net::InterfaceKind;
using anpr::net::InternetReach;
using anpr::net::InternetStatus;
using anpr::net::Ipv4;
using anpr::net::NetworkInterface;
using anpr::net::NetworkSnapshot;
using anpr::net::RouteEntry;
using anpr::net::SystemPaths;

namespace {

namespace fs = anpr::filesystem;

Ipv4 ip(const std::string& text) {
    return anpr::net::parseIpv4(text).value_or(Ipv4{});
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

void writeFile(const std::string& path, const std::string& text) {
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream out(path, std::ios::trunc);
    out << text;
}

std::vector<std::string> components(const std::string& path) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t slash = path.find('/', start);
        const std::string part =
            path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (!part.empty()) {
            parts.push_back(part);
        }
        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }
    return parts;
}

/// A relative symlink target, the way sysfs links look ("../../devices/pci0000:00/...").
std::string relativePath(const std::string& from_dir, const std::string& to) {
    const std::vector<std::string> from = components(from_dir);
    const std::vector<std::string> target = components(to);
    std::size_t common = 0;
    while (common < from.size() && common < target.size() && from[common] == target[common]) {
        ++common;
    }
    std::string result;
    for (std::size_t i = common; i < from.size(); ++i) {
        result += "../";
    }
    for (std::size_t i = common; i < target.size(); ++i) {
        result += target[i];
        result += '/';
    }
    if (result.empty()) {
        return ".";
    }
    result.pop_back();
    return result;
}

void makeLink(const std::string& target, const std::string& link) {
    const std::string parent = fs::path(link).parent_path().string();
    fs::create_directories(parent);
    const int rc = ::symlink(relativePath(parent, target).c_str(), link.c_str());
    if (rc != 0) {
        anpr_test::recordFailure("symlink failed: " + link);
    }
}

class TempDir {
public:
    TempDir() {
        const std::string pattern = (fs::temp_directory_path() / "kz-anpr-net-XXXXXX").string();
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        if (::mkdtemp(buffer.data()) != nullptr) {
            path_ = buffer.data();
        }
    }
    ~TempDir() {
        if (!path_.empty()) {
            std::error_code ignored;
            fs::remove_all(path_, ignored);
        }
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] const std::string& path() const { return path_; }

private:
    std::string path_;
};

/// One /sys/class/net entry for the fake tree.
struct Netdev {
    std::string name;
    /// Absolute device directory, empty for a software interface.
    std::string device;
    int type{1};
    std::string flags{"0x1003"};
    /// "-" leaves the file out; "" writes an empty file, which reads like EINVAL.
    std::string carrier{"1"};
    std::string operstate{"up"};
    std::string mac{"00:00:00:00:00:00"};
    std::string devtype;
    std::string master;
    bool bridge{false};
    bool phy80211{false};
};

/// A temporary sysfs tree laid out like the kernel's: netdev directories under their device,
/// /sys/class/net entries and device/driver/subsystem links as relative symlinks.
class FakeSystem {
public:
    FakeSystem() { fs::create_directories(classNet()); }

    [[nodiscard]] std::string root() const { return dir_.path(); }
    [[nodiscard]] std::string sys() const { return dir_.path() + "/sys"; }
    [[nodiscard]] std::string classNet() const { return sys() + "/class/net"; }

    [[nodiscard]] SystemPaths paths() const {
        SystemPaths result;
        result.sys_class_net = classNet();
        result.proc_net_route = dir_.path() + "/proc/net/route";
        return result;
    }

    std::string device(const std::string& path, const std::string& bus,
                       const std::string& driver) {
        const std::string dir = sys() + "/devices/" + path;
        fs::create_directories(dir);
        const std::string bus_dir = sys() + "/bus/" + (bus.empty() ? "platform" : bus);
        fs::create_directories(bus_dir);
        if (!bus.empty()) {
            makeLink(bus_dir, dir + "/subsystem");
        }
        if (!driver.empty()) {
            fs::create_directories(bus_dir + "/drivers/" + driver);
            makeLink(bus_dir + "/drivers/" + driver, dir + "/driver");
        }
        return dir;
    }

    /// USB device "1-<port>" with `vendor`, and its interface 1-<port>:1.0 bound to `driver`.
    std::string usbInterface(const std::string& port, const std::string& vendor,
                             const std::string& driver) {
        const std::string usb_device = "70090000.xusb/usb1/1-" + port;
        writeFile(sys() + "/devices/" + usb_device + "/idVendor", vendor + "\n");
        return device(usb_device + "/1-" + port + ":1.0", "usb", driver);
    }

    void add(const Netdev& netdev) {
        const std::string base = netdev.device.empty()
                                     ? sys() + "/devices/virtual/net/" + netdev.name
                                     : netdev.device + "/net/" + netdev.name;
        bases_[netdev.name] = base;
        fs::create_directories(base);
        makeLink(base, classNet() + "/" + netdev.name);
        writeFile(base + "/type", std::to_string(netdev.type) + "\n");
        writeFile(base + "/flags", netdev.flags + "\n");
        if (netdev.carrier != "-") {
            writeFile(base + "/carrier", netdev.carrier.empty() ? "" : netdev.carrier + "\n");
        }
        writeFile(base + "/operstate", netdev.operstate + "\n");
        writeFile(base + "/address", netdev.mac + "\n");
        std::string uevent;
        if (!netdev.devtype.empty()) {
            uevent += "DEVTYPE=" + netdev.devtype + "\n";
        }
        uevent += "INTERFACE=" + netdev.name + "\n";
        writeFile(base + "/uevent", uevent);
        if (!netdev.device.empty()) {
            makeLink(netdev.device, base + "/device");
        }
        if (!netdev.master.empty()) {
            makeLink(sys() + "/devices/virtual/net/" + netdev.master, base + "/master");
            fs::create_directories(base + "/brport");
        }
        if (netdev.bridge) {
            fs::create_directories(base + "/bridge");
            fs::create_directories(base + "/brif");
        }
        if (netdev.phy80211) {
            const std::string phy = netdev.device + "/ieee80211/phy0";
            fs::create_directories(phy);
            makeLink(phy, base + "/phy80211");
            fs::create_directories(base + "/wireless");
        }
    }

    void bridgePort(const std::string& bridge, const std::string& port) {
        makeLink(bases_[port] + "/brport", bases_[bridge] + "/brif/" + port);
    }

    void routes(const std::vector<std::string>& rows) {
        std::string text =
            "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT"
            "                                                       \n";
        for (const std::string& row : rows) {
            text += row + "\n";
        }
        writeFile(paths().proc_net_route, text);
    }

private:
    TempDir dir_;
    std::map<std::string, std::string> bases_;
};

/// The raw word /proc/net/route prints for an address: the network-order value read as a
/// native integer.
unsigned routeWord(const std::string& address) {
    return static_cast<unsigned>(htonl(ip(address).value));
}

std::string routeRow(const std::string& name, const std::string& destination,
                     const std::string& gateway, int prefix, int metric) {
    const unsigned mask = prefix == 0 ? 0U : htonl(0xFFFFFFFFU << static_cast<unsigned>(32 - prefix));
    const unsigned flags = 0x1U | (gateway == "0.0.0.0" ? 0U : 0x2U);
    char line[256];
    std::snprintf(line, sizeof(line), "%s\t%08X\t%08X\t%04X\t%d\t%u\t%d\t%08X\t%d\t%u\t%u",
                  name.c_str(), routeWord(destination), routeWord(gateway), flags, 0, 0U, metric,
                  mask, 0, 0U, 0U);
    std::string row(line);
    row.resize(127, ' ');
    return row;
}

AddressProvider fixedAddresses(const std::map<std::string, std::vector<std::string>>& cidrs) {
    std::map<std::string, std::vector<InterfaceAddress>> result;
    for (const auto& item : cidrs) {
        for (const std::string& cidr : item.second) {
            const std::size_t slash = cidr.find('/');
            InterfaceAddress address;
            address.address = ip(cidr.substr(0, slash));
            address.prefix = std::atoi(cidr.substr(slash + 1).c_str());
            result[item.first].push_back(address);
        }
    }
    return [result]() { return result; };
}

void addLoopback(FakeSystem& fake) {
    Netdev lo;
    lo.name = "lo";
    lo.type = 772;
    lo.flags = "0x9";
    lo.operstate = "unknown";
    fake.add(lo);
}

/// The Nano's onboard RTL8111 on PCIe, driven by the built-in r8168.
void addEth0(FakeSystem& fake, const std::string& carrier = "1",
             const std::string& flags = "0x1003") {
    Netdev eth0;
    eth0.name = "eth0";
    eth0.device = fake.device("1003000.pcie/pci0000:00/0000:00:02.0/0000:01:00.0", "pci", "r8168");
    eth0.carrier = carrier;
    eth0.flags = flags;
    eth0.operstate = carrier == "1" ? "up" : "down";
    eth0.mac = "00:04:4B:AA:BB:CC";
    fake.add(eth0);
}

void addWwan0(FakeSystem& fake) {
    Netdev wwan0;
    wwan0.name = "wwan0";
    wwan0.device = fake.usbInterface("2", "2c7c", "qmi_wwan");
    wwan0.type = 65534;
    wwan0.flags = "0x1091";
    wwan0.devtype = "wwan";
    wwan0.operstate = "unknown";
    fake.add(wwan0);
}

/// Huawei HiLink stick: cdc_ether with a universally administered MAC, so it is called eth1.
void addHilinkEth1(FakeSystem& fake) {
    Netdev eth1;
    eth1.name = "eth1";
    eth1.device = fake.usbInterface("3", "12d1", "cdc_ether");
    eth1.mac = "0c:5b:8f:27:9a:64";
    fake.add(eth1);
}

void addUsbEthernet(FakeSystem& fake, const std::string& name, const std::string& port,
                    const std::string& vendor, const std::string& driver,
                    const std::string& carrier = "1") {
    Netdev nic;
    nic.name = name;
    nic.device = fake.usbInterface(port, vendor, driver);
    nic.carrier = carrier;
    nic.operstate = carrier == "1" ? "up" : "down";
    nic.mac = "00:0e:c6:00:00:0" + port;
    fake.add(nic);
}

/// rndis0 (DEVTYPE=gadget) and usb0 (no DEVTYPE, recognised by its path under the UDC), both
/// ports of l4tbr0.
void addGadgets(FakeSystem& fake) {
    const std::string gadget = fake.device("700d0000.xudc/gadget", "", "");
    Netdev rndis0;
    rndis0.name = "rndis0";
    rndis0.device = gadget;
    rndis0.devtype = "gadget";
    rndis0.master = "l4tbr0";
    rndis0.carrier = "0";
    rndis0.operstate = "down";
    fake.add(rndis0);
    Netdev usb0 = rndis0;
    usb0.name = "usb0";
    usb0.devtype.clear();
    fake.add(usb0);
    Netdev l4tbr0;
    l4tbr0.name = "l4tbr0";
    l4tbr0.devtype = "bridge";
    l4tbr0.bridge = true;
    l4tbr0.carrier = "0";
    l4tbr0.operstate = "down";
    fake.add(l4tbr0);
    fake.bridgePort("l4tbr0", "rndis0");
    fake.bridgePort("l4tbr0", "usb0");
}

void addDocker(FakeSystem& fake) {
    Netdev docker0;
    docker0.name = "docker0";
    docker0.devtype = "bridge";
    docker0.bridge = true;
    fake.add(docker0);
    Netdev veth;
    veth.name = "veth1a2b3c";
    veth.master = "docker0";
    fake.add(veth);
    fake.bridgePort("docker0", "veth1a2b3c");
    Netdev user_bridge;
    user_bridge.name = "br-5f2a";
    user_bridge.bridge = true;  // no DEVTYPE: recognised by its bridge/ directory
    user_bridge.carrier = "0";
    user_bridge.operstate = "down";
    fake.add(user_bridge);
}

void addSoftware(FakeSystem& fake) {
    Netdev dummy0;
    dummy0.name = "dummy0";
    dummy0.flags = "0xc3";
    dummy0.operstate = "unknown";
    fake.add(dummy0);
    Netdev ppp0;
    ppp0.name = "ppp0";
    ppp0.type = 512;
    ppp0.flags = "0x10d1";
    ppp0.operstate = "unknown";
    fake.add(ppp0);
}

void addWlan0(FakeSystem& fake) {
    Netdev wlan0;
    wlan0.name = "wlan0";
    wlan0.device = fake.usbInterface("5", "0bda", "rtl8821cu");
    wlan0.devtype = "wlan";
    wlan0.phy80211 = true;
    wlan0.carrier = "0";
    wlan0.operstate = "down";
    fake.add(wlan0);
}

const NetworkInterface& mustFind(const NetworkSnapshot& snapshot, const std::string& name) {
    static const NetworkInterface kMissing;
    const NetworkInterface* nic = snapshot.find(name);
    if (nic == nullptr) {
        anpr_test::recordFailure("interface not in snapshot: " + name);
        return kMissing;
    }
    return *nic;
}

/// A listening socket on 127.0.0.1 with a kernel-chosen port. The kernel completes handshakes
/// into the backlog, so no accept thread is needed for a connect probe.
class LocalListener {
public:
    explicit LocalListener(bool listen_now) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        ::bind(fd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
        socklen_t length = sizeof(address);
        ::getsockname(fd_, reinterpret_cast<sockaddr*>(&address), &length);
        port_ = ntohs(address.sin_port);
        if (listen_now) {
            ::listen(fd_, 4);
        } else {
            ::close(fd_);  // the port is now closed: a connect gets RST
            fd_ = -1;
        }
    }
    ~LocalListener() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }
    LocalListener(const LocalListener&) = delete;
    LocalListener& operator=(const LocalListener&) = delete;

    [[nodiscard]] std::string target() const { return "127.0.0.1:" + std::to_string(port_); }

private:
    int fd_{-1};
    unsigned port_{0};
};

NetworkSnapshot snapshotWithDefault(const std::string& name) {
    NetworkSnapshot snapshot;
    RouteEntry route;
    route.interface = name;
    route.up = true;
    route.metric = 700;
    snapshot.routes.push_back(route);
    return snapshot;
}

}  // namespace

// ----------------------------------------------------------------------------------------------
// Classification

TEST("network topology classifies every Jetson interface from sysfs, never by name") {
    FakeSystem fake;
    addLoopback(fake);
    addEth0(fake);
    addWwan0(fake);
    addHilinkEth1(fake);
    addUsbEthernet(fake, "eth2", "4", "0b95", "cdc_ether");
    addUsbEthernet(fake, "eth3", "6", "0bda", "r8152");
    addGadgets(fake);
    addDocker(fake);
    addSoftware(fake);
    addWlan0(fake);
    writeFile(fake.classNet() + "/bonding_masters", "\n");

    const NetworkSnapshot snapshot = anpr::net::readNetworkSnapshot(fake.paths(), nullptr);
    CHECK_EQ(snapshot.interfaces.size(), std::size_t{15});
    CHECK(snapshot.find("bonding_masters") == nullptr);

    const NetworkInterface& lo = mustFind(snapshot, "lo");
    CHECK_EQ(lo.kind, InterfaceKind::kLoopback);
    CHECK(lo.mac.empty());

    const NetworkInterface& eth0 = mustFind(snapshot, "eth0");
    CHECK_EQ(eth0.kind, InterfaceKind::kEthernet);
    CHECK_EQ(eth0.driver, std::string("r8168"));
    CHECK_EQ(eth0.bus, std::string("pci"));
    CHECK(eth0.usb_vendor_id.empty());
    CHECK_EQ(eth0.mac, std::string("00:04:4b:aa:bb:cc"));
    CHECK_EQ(eth0.type, 1);
    CHECK(eth0.admin_up);
    CHECK(eth0.carrier.has_value());
    CHECK(*eth0.carrier);
    CHECK_EQ(eth0.operstate, std::string("up"));
    CHECK_EQ(eth0.reason, std::string("r8168 on pci, carrier up"));

    const NetworkInterface& wwan0 = mustFind(snapshot, "wwan0");
    CHECK_EQ(wwan0.kind, InterfaceKind::kCellular);
    CHECK_EQ(wwan0.driver, std::string("qmi_wwan"));
    CHECK_EQ(wwan0.bus, std::string("usb"));
    CHECK_EQ(wwan0.usb_vendor_id, std::string("2c7c"));
    CHECK_EQ(wwan0.type, 65534);
    CHECK(contains(wwan0.reason, "DEVTYPE=wwan"));

    const NetworkInterface& eth1 = mustFind(snapshot, "eth1");
    CHECK_EQ(eth1.kind, InterfaceKind::kCellular);
    CHECK_EQ(eth1.driver, std::string("cdc_ether"));
    CHECK_EQ(eth1.usb_vendor_id, std::string("12d1"));
    CHECK(contains(eth1.reason, "Huawei"));

    const NetworkInterface& eth2 = mustFind(snapshot, "eth2");
    CHECK_EQ(eth2.kind, InterfaceKind::kEthernet);
    CHECK_EQ(eth2.bus, std::string("usb"));
    CHECK_EQ(eth2.usb_vendor_id, std::string("0b95"));
    CHECK(contains(eth2.reason, "not a modem maker"));

    const NetworkInterface& eth3 = mustFind(snapshot, "eth3");
    CHECK_EQ(eth3.kind, InterfaceKind::kEthernet);
    CHECK_EQ(eth3.driver, std::string("r8152"));

    const NetworkInterface& rndis0 = mustFind(snapshot, "rndis0");
    CHECK_EQ(rndis0.kind, InterfaceKind::kUsbGadget);
    CHECK_EQ(rndis0.master, std::string("l4tbr0"));
    CHECK(contains(rndis0.reason, "DEVTYPE=gadget"));
    CHECK(rndis0.carrier.has_value());
    CHECK(!*rndis0.carrier);

    const NetworkInterface& usb0 = mustFind(snapshot, "usb0");
    CHECK_EQ(usb0.kind, InterfaceKind::kUsbGadget);
    CHECK(contains(usb0.reason, "under the UDC"));

    const NetworkInterface& l4tbr0 = mustFind(snapshot, "l4tbr0");
    CHECK_EQ(l4tbr0.kind, InterfaceKind::kUsbGadget);
    CHECK_EQ(l4tbr0.bus, std::string("virtual"));
    CHECK(contains(l4tbr0.reason, "rndis0, usb0"));

    CHECK_EQ(mustFind(snapshot, "docker0").kind, InterfaceKind::kBridge);
    CHECK(contains(mustFind(snapshot, "docker0").reason, "veth1a2b3c"));
    CHECK_EQ(mustFind(snapshot, "br-5f2a").kind, InterfaceKind::kBridge);

    const NetworkInterface& veth = mustFind(snapshot, "veth1a2b3c");
    CHECK_EQ(veth.kind, InterfaceKind::kVirtual);
    CHECK_EQ(veth.master, std::string("docker0"));
    CHECK(veth.driver.empty());

    CHECK_EQ(mustFind(snapshot, "dummy0").kind, InterfaceKind::kVirtual);
    CHECK_EQ(mustFind(snapshot, "ppp0").kind, InterfaceKind::kCellular);
    CHECK(contains(mustFind(snapshot, "ppp0").reason, "type 512"));

    const NetworkInterface& wlan0 = mustFind(snapshot, "wlan0");
    CHECK_EQ(wlan0.kind, InterfaceKind::kWireless);
    CHECK_EQ(wlan0.driver, std::string("rtl8821cu"));

    for (const NetworkInterface& nic : snapshot.interfaces) {
        CHECK(!nic.reason.empty());
    }
}

TEST("a GSM modem named eth1 and a gadget named usb0 are classified by sysfs, not name") {
    FakeSystem fake;
    // A Quectel in ECM mode binds cdc_ether and is named usb1 by usbnet; a plain adapter named
    // usb5 is still Ethernet.
    Netdev quectel;
    quectel.name = "usb1";
    quectel.device = fake.usbInterface("2", "2C7C", "cdc_ether");
    fake.add(quectel);
    addUsbEthernet(fake, "usb5", "3", "0b95", "ax88179_178a");
    const NetworkSnapshot snapshot = anpr::net::readNetworkSnapshot(fake.paths(), nullptr);
    CHECK_EQ(mustFind(snapshot, "usb1").kind, InterfaceKind::kCellular);
    CHECK_EQ(mustFind(snapshot, "usb1").usb_vendor_id, std::string("2c7c"));
    CHECK_EQ(mustFind(snapshot, "usb5").kind, InterfaceKind::kEthernet);
}

TEST("an administratively down eth0 reports an unknown carrier") {
    FakeSystem fake;
    addEth0(fake, "", "0x1002");
    const NetworkSnapshot snapshot = anpr::net::readNetworkSnapshot(fake.paths(), nullptr);
    const NetworkInterface& eth0 = mustFind(snapshot, "eth0");
    CHECK(!eth0.admin_up);
    CHECK(!eth0.carrier.has_value());
    CHECK_EQ(eth0.operstate, std::string("down"));
    CHECK(contains(eth0.reason, "admin down"));

    FakeSystem missing_file;
    addEth0(missing_file, "-");
    const NetworkSnapshot other = anpr::net::readNetworkSnapshot(missing_file.paths(), nullptr);
    CHECK(!mustFind(other, "eth0").carrier.has_value());
}

TEST("isCellularModemVendor knows modem makers and rejects adapter vendors") {
    CHECK(anpr::net::isCellularModemVendor("12d1"));
    CHECK(anpr::net::isCellularModemVendor("12D1"));
    CHECK(anpr::net::isCellularModemVendor("0x2c7c"));
    CHECK(anpr::net::isCellularModemVendor(" 1199\n"));
    CHECK(anpr::net::isCellularModemVendor("19d2"));
    CHECK(anpr::net::isCellularModemVendor("1bbb"));
    CHECK(anpr::net::isCellularModemVendor("1e0e"));
    CHECK(anpr::net::isCellularModemVendor("2cb7"));
    CHECK(anpr::net::isCellularModemVendor("05c6"));
    CHECK(!anpr::net::isCellularModemVendor("0bda"));  // Realtek
    CHECK(!anpr::net::isCellularModemVendor("0b95"));  // ASIX
    CHECK(!anpr::net::isCellularModemVendor("03f0"));  // HP
    CHECK(!anpr::net::isCellularModemVendor(""));
}

TEST("usableIpv4 skips link-local and loopback addresses") {
    NetworkInterface nic;
    InterfaceAddress link_local;
    link_local.address = ip("169.254.3.4");
    link_local.prefix = 16;
    InterfaceAddress real;
    real.address = ip("192.168.1.10");
    real.prefix = 24;
    nic.ipv4 = {link_local, real};
    CHECK(nic.usableIpv4().has_value());
    CHECK(nic.usableIpv4()->address == real.address);
    nic.ipv4 = {link_local};
    CHECK(!nic.usableIpv4().has_value());
    InterfaceAddress loopback;
    loopback.address = ip("127.0.0.1");
    loopback.prefix = 8;
    nic.ipv4 = {loopback};
    CHECK(!nic.usableIpv4().has_value());
}

// ----------------------------------------------------------------------------------------------
// Routes

TEST("parseProcNetRoute decodes the kernel's raw network-order hex columns") {
    const std::string text =
        "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT   \n"
        "eth1\t00000000\t0108A8C0\t0003\t0\t0\t100\t00000000\t0\t0\t0                    \n"
        "eth0\t000AA8C0\t00000000\t0001\t0\t0\t100\t00FFFFFF\t0\t0\t0                    \n"
        "wwan0\t00000000\t00000000\t0001\t0\t0\t700\t00000000\t0\t0\t0\n"
        "eth0\t4001A8C0\t0101A8C0\t0007\t0\t0\t0\tFFFFFFFF\t0\t0\t0\n"
        "*\t0000000A\t00000000\t0201\t0\t0\t0\t000000FF\t0\t0\t0\n"
        "broken line\n"
        "eth9\tZZZZ\t00000000\t0001\t0\t0\t0\t00000000\t0\t0\t0\n"
        "eth9\t00000000\t00000000\t0001\t0\t0\t0\t00FF00FF\t0\t0\t0\n";
    const std::vector<RouteEntry> routes = anpr::net::parseProcNetRoute(text);
    CHECK_EQ(routes.size(), std::size_t{5});

    CHECK_EQ(routes[0].interface, std::string("eth1"));
    CHECK(routes[0].isDefault());
    CHECK(routes[0].up);
    CHECK(routes[0].via_gateway);
    CHECK_EQ(anpr::net::toString(routes[0].gateway), std::string("192.168.8.1"));
    CHECK_EQ(routes[0].metric, 100);

    CHECK_EQ(anpr::net::toString(routes[1].destination), std::string("192.168.10.0"));
    CHECK_EQ(routes[1].prefix, 24);
    CHECK(!routes[1].via_gateway);

    CHECK_EQ(routes[2].metric, 700);
    CHECK(routes[2].gateway.isZero());

    // "0101A8C0" is 192.168.1.1 on a little-endian Jetson.
    CHECK_EQ(anpr::net::toString(routes[3].gateway), std::string("192.168.1.1"));
    CHECK_EQ(anpr::net::toString(routes[3].destination), std::string("192.168.1.64"));
    CHECK_EQ(routes[3].prefix, 32);

    CHECK_EQ(routes[4].interface, std::string("*"));
    CHECK_EQ(routes[4].prefix, 8);
    CHECK(anpr::net::parseProcNetRoute("").empty());
}

TEST("routeFor picks the longest prefix, then the lowest metric") {
    NetworkSnapshot snapshot;
    snapshot.routes = anpr::net::parseProcNetRoute(
        routeRow("wwan0", "0.0.0.0", "0.0.0.0", 0, 700) + "\n" +
        routeRow("eth1", "0.0.0.0", "192.168.8.1", 0, 100) + "\n" +
        routeRow("*", "0.0.0.0", "0.0.0.0", 0, 50) + "\n" +
        routeRow("eth0", "192.168.0.0", "0.0.0.0", 16, 50) + "\n" +
        routeRow("eth2", "192.168.10.0", "0.0.0.0", 24, 200) + "\n" +
        routeRow("eth3", "192.168.10.0", "0.0.0.0", 24, 100) + "\n");
    CHECK_EQ(snapshot.routes.size(), std::size_t{6});

    const std::vector<RouteEntry> defaults = snapshot.defaultRoutes();
    CHECK_EQ(defaults.size(), std::size_t{2});
    CHECK_EQ(defaults[0].interface, std::string("eth1"));
    CHECK_EQ(defaults[1].interface, std::string("wwan0"));

    const RouteEntry* camera = snapshot.routeFor(ip("192.168.10.7"));
    CHECK(camera != nullptr);
    CHECK_EQ(camera->interface, std::string("eth3"));
    const RouteEntry* wide = snapshot.routeFor(ip("192.168.20.1"));
    CHECK(wide != nullptr);
    CHECK_EQ(wide->interface, std::string("eth0"));
    // The reject default has the lowest metric, so it is what the kernel would pick.
    const RouteEntry* internet = snapshot.routeFor(ip("8.8.8.8"));
    CHECK(internet != nullptr);
    CHECK_EQ(internet->interface, std::string("*"));

    NetworkSnapshot empty;
    CHECK(empty.routeFor(ip("8.8.8.8")) == nullptr);
    CHECK(empty.defaultRoutes().empty());
}

TEST("connectedInterfaceFor uses the interfaces' own subnets") {
    NetworkSnapshot snapshot;
    NetworkInterface eth0;
    eth0.name = "eth0";
    InterfaceAddress primary;
    primary.address = ip("192.168.10.5");
    primary.prefix = 24;
    InterfaceAddress secondary;
    secondary.address = ip("192.168.1.10");
    secondary.prefix = 24;
    eth0.ipv4 = {primary, secondary};
    NetworkInterface docker0;
    docker0.name = "docker0";
    InterfaceAddress pool;
    pool.address = ip("192.168.0.1");
    pool.prefix = 20;
    docker0.ipv4 = {pool};
    snapshot.interfaces = {docker0, eth0};
    CHECK_EQ(snapshot.connectedInterfaceFor(ip("192.168.10.64")), std::string("eth0"));
    // The more specific /24 wins over the overlapping /20.
    CHECK_EQ(snapshot.connectedInterfaceFor(ip("192.168.1.64")), std::string("eth0"));
    CHECK_EQ(snapshot.connectedInterfaceFor(ip("192.168.2.1")), std::string("docker0"));
    CHECK(snapshot.connectedInterfaceFor(ip("8.8.8.8")).empty());
}

// ----------------------------------------------------------------------------------------------
// Camera LAN selection and the startup summary

TEST("camera LAN: linked r8168 eth0 with an address, GSM keeps the default route") {
    FakeSystem fake;
    addLoopback(fake);
    addEth0(fake);
    addWwan0(fake);
    addDocker(fake);
    fake.routes({routeRow("wwan0", "0.0.0.0", "0.0.0.0", 0, 700),
                 routeRow("wwan0", "10.64.12.4", "0.0.0.0", 30, 700),
                 routeRow("eth0", "192.168.10.0", "0.0.0.0", 24, 100),
                 routeRow("docker0", "172.17.0.0", "0.0.0.0", 16, 0)});
    const NetworkSnapshot snapshot = anpr::net::readNetworkSnapshot(
        fake.paths(), fixedAddresses({{"lo", {"127.0.0.1/8"}},
                                      {"eth0", {"192.168.10.5/24"}},
                                      {"wwan0", {"10.64.12.5/30"}},
                                      {"docker0", {"172.17.0.1/16"}}}));
    CHECK_EQ(snapshot.routes.size(), std::size_t{4});

    const CameraLanSelection lan = anpr::net::selectCameraLan(snapshot, "auto");
    CHECK_EQ(lan.status, CameraLanStatus::kReady);
    CHECK(lan.ready());
    CHECK_EQ(lan.interface, std::string("eth0"));
    CHECK(!lan.from_override);
    CHECK(lan.address.has_value());
    CHECK_EQ(lan.address->network().addressWithPrefix(), std::string("192.168.10.5/24"));
    CHECK_EQ(lan.networks.size(), std::size_t{1});
    CHECK_EQ(lan.networks[0].cidr(), std::string("192.168.10.0/24"));
    CHECK(!lan.carries_default_route);
    CHECK(lan.other_candidates.empty());
    CHECK(lan.inCameraSubnet(ip("192.168.10.64")));
    CHECK(!lan.inCameraSubnet(ip("192.168.1.64")));
    CHECK(contains(lan.detail, "eth0"));

    const RouteEntry* camera_route = snapshot.routeFor(ip("192.168.10.64"));
    CHECK(camera_route != nullptr);
    CHECK_EQ(camera_route->interface, std::string("eth0"));
    const RouteEntry* internet_route = snapshot.routeFor(ip("1.1.1.1"));
    CHECK(internet_route != nullptr);
    CHECK_EQ(internet_route->interface, std::string("wwan0"));
    CHECK_EQ(snapshot.connectedInterfaceFor(ip("192.168.10.64")), std::string("eth0"));

    InternetStatus internet = anpr::net::inspectInternet(snapshot, false, {"1.1.1.1:53"}, 100);
    CHECK(internet.has_default_route);
    CHECK_EQ(internet.interface, std::string("wwan0"));
    CHECK_EQ(internet.kind, InterfaceKind::kCellular);
    CHECK_EQ(internet.metric, 700);
    CHECK_EQ(internet.reach, InternetReach::kNotChecked);
    CHECK(internet.backup_interfaces.empty());
    CHECK(anpr::net::overlappingSubnets(snapshot, lan).empty());
    CHECK(contains(anpr::net::formatNetworkSummary(snapshot, lan, internet),
                   "status: not checked"));

    internet.reach = InternetReach::kOnline;
    internet.probe_target = "1.1.1.1:53";
    internet.probe_ms = 48.2;
    CHECK_EQ(anpr::net::formatNetworkSummary(snapshot, lan, internet),
             std::string("CAMERA NETWORK\n"
                         "  interface: eth0 (r8168, pci)\n"
                         "  link: UP\n"
                         "  ip: 192.168.10.5/24\n"
                         "  subnet: 192.168.10.0/24\n"
                         "INTERNET\n"
                         "  interface: wwan0 (cellular, qmi_wwan)\n"
                         "  default route: GSM via wwan0 (metric 700)\n"
                         "  status: ONLINE (1.1.1.1:53 in 48 ms)\n"));
}

TEST("camera LAN: link but no IPv4 address is kNoIpv4") {
    FakeSystem fake;
    addEth0(fake);
    addWwan0(fake);
    fake.routes({routeRow("wwan0", "0.0.0.0", "0.0.0.0", 0, 700)});
    const NetworkSnapshot snapshot =
        anpr::net::readNetworkSnapshot(fake.paths(), fixedAddresses({{"wwan0", {"10.64.12.5/30"}},
                                                                     {"eth0", {"169.254.7.9/16"}}}));
    const CameraLanSelection lan = anpr::net::selectCameraLan(snapshot, "");
    CHECK_EQ(lan.status, CameraLanStatus::kNoIpv4);
    CHECK_EQ(lan.interface, std::string("eth0"));
    CHECK(!lan.address.has_value());
    CHECK(lan.networks.empty());
    CHECK(contains(lan.detail, "no usable IPv4"));
    const std::string summary = anpr::net::formatNetworkSummary(
        snapshot, lan, anpr::net::inspectInternet(snapshot, false, {}, 100));
    CHECK(contains(summary, "interface: eth0 (r8168, pci)"));
    CHECK(contains(summary, "link: UP"));
    CHECK(contains(summary, "ip: NONE"));
    CHECK(!contains(summary, "subnet:"));
}

TEST("camera LAN: no carrier is kNoLink and names the interface") {
    FakeSystem fake;
    addEth0(fake, "0");
    const NetworkSnapshot snapshot = anpr::net::readNetworkSnapshot(fake.paths(), nullptr);
    CHECK(mustFind(snapshot, "eth0").carrier.has_value());
    CHECK(!*mustFind(snapshot, "eth0").carrier);
    const CameraLanSelection lan = anpr::net::selectCameraLan(snapshot, "auto");
    CHECK_EQ(lan.status, CameraLanStatus::kNoLink);
    CHECK_EQ(lan.interface, std::string("eth0"));
    CHECK(contains(lan.detail, "no link"));
    const InternetStatus internet = anpr::net::inspectInternet(snapshot, true, {"1.1.1.1:53"}, 50);
    CHECK(!internet.has_default_route);
    CHECK_EQ(internet.reach, InternetReach::kNotChecked);
    const std::string summary = anpr::net::formatNetworkSummary(snapshot, lan, internet);
    CHECK(contains(summary, "interface: eth0 (r8168, pci)"));
    CHECK(contains(summary, "link: DOWN (no carrier"));
    CHECK(!contains(summary, "ip:"));
    CHECK(contains(summary, "default route: NONE"));
}

TEST("camera LAN: an administratively down eth0 is kNoLink") {
    FakeSystem fake;
    addEth0(fake, "", "0x1002");
    const NetworkSnapshot snapshot = anpr::net::readNetworkSnapshot(fake.paths(), nullptr);
    const CameraLanSelection lan = anpr::net::selectCameraLan(snapshot, "auto");
    CHECK_EQ(lan.status, CameraLanStatus::kNoLink);
    CHECK_EQ(lan.interface, std::string("eth0"));
    CHECK(contains(lan.detail, "administratively down"));
    CHECK(contains(anpr::net::formatNetworkSummary(snapshot, lan, InternetStatus{}),
                   "link: DOWN (interface is administratively down)"));
}

TEST("camera LAN: no wired Ethernet at all is kNoInterface") {
    FakeSystem fake;
    addLoopback(fake);
    addWwan0(fake);
    addHilinkEth1(fake);
    addGadgets(fake);
    addDocker(fake);
    addSoftware(fake);
    addWlan0(fake);
    const NetworkSnapshot snapshot = anpr::net::readNetworkSnapshot(fake.paths(), nullptr);
    const CameraLanSelection lan = anpr::net::selectCameraLan(snapshot, "auto");
    CHECK_EQ(lan.status, CameraLanStatus::kNoInterface);
    CHECK(lan.interface.empty());
    CHECK(!lan.inCameraSubnet(ip("192.168.1.64")));
    CHECK(contains(anpr::net::formatNetworkSummary(snapshot, lan, InternetStatus{}),
                   "interface: NONE (no wired Ethernet interface found)"));
}

TEST("camera LAN: a HiLink modem on eth1 is the uplink, never the camera LAN") {
    FakeSystem fake;
    addEth0(fake, "0");
    addHilinkEth1(fake);
    fake.routes({routeRow("eth1", "0.0.0.0", "192.168.8.1", 0, 100),
                 routeRow("eth1", "192.168.8.0", "0.0.0.0", 24, 100)});
    const NetworkSnapshot snapshot = anpr::net::readNetworkSnapshot(
        fake.paths(), fixedAddresses({{"eth1", {"192.168.8.100/24"}}}));
    const CameraLanSelection lan = anpr::net::selectCameraLan(snapshot, "auto");
    CHECK_EQ(lan.status, CameraLanStatus::kNoLink);
    CHECK_EQ(lan.interface, std::string("eth0"));
    CHECK(lan.other_candidates.empty());
    const InternetStatus internet = anpr::net::inspectInternet(snapshot, false, {}, 100);
    CHECK_EQ(internet.interface, std::string("eth1"));
    CHECK_EQ(internet.kind, InterfaceKind::kCellular);
    CHECK_EQ(anpr::net::toString(internet.gateway), std::string("192.168.8.1"));
    const std::string summary = anpr::net::formatNetworkSummary(snapshot, lan, internet);
    CHECK(contains(summary, "interface: eth1 (cellular, cdc_ether)"));
    CHECK(contains(summary, "default route: GSM via eth1 (gateway 192.168.8.1, metric 100)"));
}

TEST("camera LAN: a USB Ethernet adapter from a non-modem vendor qualifies") {
    FakeSystem fake;
    addUsbEthernet(fake, "eth2", "4", "0b95", "cdc_ether");
    const NetworkSnapshot snapshot = anpr::net::readNetworkSnapshot(
        fake.paths(), fixedAddresses({{"eth2", {"192.168.1.10/24"}}}));
    const CameraLanSelection lan = anpr::net::selectCameraLan(snapshot, "auto");
    CHECK_EQ(lan.status, CameraLanStatus::kReady);
    CHECK_EQ(lan.interface, std::string("eth2"));
    CHECK(contains(anpr::net::formatNetworkSummary(snapshot, lan, InternetStatus{}),
                   "interface: eth2 (cdc_ether, usb)"));
}

TEST("camera LAN preference: usable address, then not the default route, then onboard") {
    {
        FakeSystem fake;
        addEth0(fake);
        addUsbEthernet(fake, "eth2", "4", "0bda", "r8152");
        const NetworkSnapshot snapshot = anpr::net::readNetworkSnapshot(
            fake.paths(), fixedAddresses({{"eth2", {"192.168.1.10/24"}}}));
        const CameraLanSelection lan = anpr::net::selectCameraLan(snapshot, "auto");
        CHECK_EQ(lan.interface, std::string("eth2"));
        CHECK_EQ(lan.status, CameraLanStatus::kReady);
        CHECK_EQ(lan.other_candidates.size(), std::size_t{1});
        CHECK_EQ(lan.other_candidates[0], std::string("eth0"));
        CHECK(contains(anpr::net::formatNetworkSummary(snapshot, lan, InternetStatus{}),
                       "other wired links: eth0"));
    }
    {
        FakeSystem fake;
        addEth0(fake);
        addUsbEthernet(fake, "eth2", "4", "0bda", "r8152");
        const NetworkSnapshot snapshot = anpr::net::readNetworkSnapshot(
            fake.paths(),
            fixedAddresses({{"eth0", {"192.168.10.5/24"}}, {"eth2", {"192.168.1.10/24"}}}));
        CHECK_EQ(anpr::net::selectCameraLan(snapshot, "auto").interface, std::string("eth0"));
    }
    {
        FakeSystem fake;
        addEth0(fake);
        addUsbEthernet(fake, "eth2", "4", "0bda", "r8152");
        fake.routes({routeRow("eth0", "0.0.0.0", "10.0.0.1", 0, 100)});
        const NetworkSnapshot snapshot = anpr::net::readNetworkSnapshot(
            fake.paths(),
            fixedAddresses({{"eth0", {"10.0.0.5/24"}}, {"eth2", {"192.168.1.10/24"}}}));
        const CameraLanSelection lan = anpr::net::selectCameraLan(snapshot, "auto");
        CHECK_EQ(lan.interface, std::string("eth2"));
        CHECK(!lan.carries_default_route);
    }
    {
        FakeSystem fake;
        addUsbEthernet(fake, "eth3", "5", "0bda", "r8152");
        addUsbEthernet(fake, "eth2", "4", "0bda", "r8152");
        const NetworkSnapshot snapshot = anpr::net::readNetworkSnapshot(
            fake.paths(),
            fixedAddresses({{"eth2", {"192.168.1.10/24"}}, {"eth3", {"192.168.2.10/24"}}}));
        CHECK_EQ(anpr::net::selectCameraLan(snapshot, "auto").interface, std::string("eth2"));
    }
    {
        // Nothing linked: the onboard port is named so the operator checks that cable.
        FakeSystem fake;
        addUsbEthernet(fake, "eth1", "4", "0bda", "r8152", "0");
        addEth0(fake, "0");
        const CameraLanSelection lan = anpr::net::selectCameraLan(
            anpr::net::readNetworkSnapshot(fake.paths(), nullptr), "auto");
        CHECK_EQ(lan.status, CameraLanStatus::kNoLink);
        CHECK_EQ(lan.interface, std::string("eth0"));
    }
}

TEST("camera LAN: a static eth0 gateway with metric 100 steals the default route") {
    FakeSystem fake;
    addEth0(fake);
    addWwan0(fake);
    fake.routes({routeRow("eth0", "0.0.0.0", "192.168.10.1", 0, 100),
                 routeRow("wwan0", "0.0.0.0", "0.0.0.0", 0, 700),
                 routeRow("eth0", "192.168.10.0", "0.0.0.0", 24, 100)});
    const NetworkSnapshot snapshot = anpr::net::readNetworkSnapshot(
        fake.paths(),
        fixedAddresses({{"eth0", {"192.168.10.5/24"}}, {"wwan0", {"10.64.12.5/30"}}}));
    const CameraLanSelection lan = anpr::net::selectCameraLan(snapshot, "auto");
    CHECK_EQ(lan.status, CameraLanStatus::kReady);
    CHECK_EQ(lan.interface, std::string("eth0"));
    CHECK(lan.carries_default_route);
    CHECK(contains(lan.detail, "default route"));

    const InternetStatus internet = anpr::net::inspectInternet(snapshot, false, {}, 100);
    CHECK_EQ(internet.interface, std::string("eth0"));
    CHECK_EQ(internet.kind, InterfaceKind::kEthernet);
    CHECK_EQ(anpr::net::toString(internet.gateway), std::string("192.168.10.1"));
    CHECK_EQ(internet.metric, 100);
    CHECK_EQ(internet.backup_interfaces.size(), std::size_t{1});
    CHECK_EQ(internet.backup_interfaces[0], std::string("wwan0"));
    const std::string summary = anpr::net::formatNetworkSummary(snapshot, lan, internet);
    CHECK(contains(summary,
                   "default route: camera LAN eth0 (gateway 192.168.10.1, metric 100) \xE2\x80\x94 "
                   "WARNING"));
    CHECK(contains(summary, "backup routes: wwan0 (metric 700)"));
}

TEST("summary: a wired uplink that is not the GSM modem") {
    FakeSystem fake;
    addEth0(fake);
    addUsbEthernet(fake, "eth1", "4", "0bda", "r8152");
    fake.routes({routeRow("eth1", "0.0.0.0", "10.0.0.1", 0, 100)});
    const NetworkSnapshot snapshot = anpr::net::readNetworkSnapshot(
        fake.paths(),
        fixedAddresses({{"eth0", {"192.168.10.5/24"}}, {"eth1", {"10.0.0.7/24"}}}));
    const CameraLanSelection lan = anpr::net::selectCameraLan(snapshot, "auto");
    CHECK_EQ(lan.interface, std::string("eth0"));
    const InternetStatus internet = anpr::net::inspectInternet(snapshot, false, {}, 100);
    CHECK_EQ(internet.kind, InterfaceKind::kEthernet);
    const std::string summary = anpr::net::formatNetworkSummary(snapshot, lan, internet);
    CHECK(contains(summary, "interface: eth1 (ethernet, r8152)"));
    CHECK(contains(summary, "default route: wired eth1 (gateway 10.0.0.1, metric 100)"));
    CHECK(contains(summary, "other wired links: eth1"));
    CHECK(!contains(summary, "WARNING"));
}

TEST("camera LAN override: any existing interface, or kOverrideMissing") {
    FakeSystem fake;
    addEth0(fake);
    addSoftware(fake);
    const NetworkSnapshot snapshot = anpr::net::readNetworkSnapshot(
        fake.paths(),
        fixedAddresses({{"eth0", {"192.168.10.5/24"}}, {"dummy0", {"10.99.0.1/24"}}}));

    const CameraLanSelection dummy = anpr::net::selectCameraLan(snapshot, "dummy0");
    CHECK_EQ(dummy.status, CameraLanStatus::kReady);
    CHECK_EQ(dummy.interface, std::string("dummy0"));
    CHECK(dummy.from_override);
    CHECK(dummy.inCameraSubnet(ip("10.99.0.20")));
    CHECK(contains(dummy.detail, "dummy0 (virtual) has link"));
    CHECK(contains(dummy.detail, "detected as virtual"));
    CHECK(contains(anpr::net::formatNetworkSummary(snapshot, dummy, InternetStatus{}),
                   "interface: dummy0 (virtual, configured)"));

    const CameraLanSelection missing = anpr::net::selectCameraLan(snapshot, "eth5");
    CHECK_EQ(missing.status, CameraLanStatus::kOverrideMissing);
    CHECK_EQ(missing.interface, std::string("eth5"));
    CHECK(missing.from_override);
    CHECK(missing.networks.empty());
    CHECK(contains(anpr::net::formatNetworkSummary(snapshot, missing, InternetStatus{}),
                   "interface: eth5 (configured, NOT FOUND)"));

    const CameraLanSelection automatic = anpr::net::selectCameraLan(snapshot, " auto ");
    CHECK(!automatic.from_override);
    CHECK_EQ(automatic.interface, std::string("eth0"));
}

TEST("overlappingSubnets reports a Docker pool covering the camera subnet") {
    FakeSystem fake;
    addEth0(fake);
    addDocker(fake);
    addGadgets(fake);
    const NetworkSnapshot snapshot = anpr::net::readNetworkSnapshot(
        fake.paths(), fixedAddresses({{"eth0", {"192.168.1.10/24"}},
                                      {"br-5f2a", {"192.168.0.1/20"}},
                                      {"docker0", {"172.17.0.1/16"}},
                                      {"l4tbr0", {"192.168.55.1/24"}}}));
    const CameraLanSelection lan = anpr::net::selectCameraLan(snapshot, "auto");
    CHECK_EQ(lan.interface, std::string("eth0"));
    const std::vector<std::string> overlaps = anpr::net::overlappingSubnets(snapshot, lan);
    CHECK_EQ(overlaps.size(), std::size_t{1});
    CHECK_EQ(overlaps[0], std::string("br-5f2a 192.168.0.0/20"));
    CHECK(contains(anpr::net::formatNetworkSummary(snapshot, lan, InternetStatus{}),
                   "subnet conflict: br-5f2a 192.168.0.0/20"));
    CHECK(anpr::net::overlappingSubnets(snapshot, CameraLanSelection{}).empty());
}

TEST("secondary camera addresses become extra networks, link-local ones do not") {
    FakeSystem fake;
    addEth0(fake);
    const NetworkSnapshot snapshot = anpr::net::readNetworkSnapshot(
        fake.paths(),
        fixedAddresses({{"eth0", {"192.168.10.5/24", "169.254.1.2/16", "192.168.1.10/24"}},
                        {"eth0:1", {"192.168.0.10/24"}}}));
    const CameraLanSelection lan = anpr::net::selectCameraLan(snapshot, "auto");
    CHECK_EQ(lan.networks.size(), std::size_t{3});
    CHECK(lan.inCameraSubnet(ip("192.168.1.64")));
    CHECK(lan.inCameraSubnet(ip("192.168.0.64")));
    CHECK(!lan.inCameraSubnet(ip("169.254.1.3")));
    const std::string summary = anpr::net::formatNetworkSummary(snapshot, lan, InternetStatus{});
    CHECK(contains(summary, "ip: 192.168.10.5/24, 192.168.1.10/24, 192.168.0.10/24"));
    CHECK(contains(summary, "subnet: 192.168.10.0/24, 192.168.1.0/24, 192.168.0.0/24"));
}

// ----------------------------------------------------------------------------------------------
// Internet probe (127.0.0.1 only)

TEST("inspectInternet: a listening local socket is ONLINE") {
    const LocalListener listener(true);
    const NetworkSnapshot snapshot = snapshotWithDefault("wwan0");
    const InternetStatus status =
        anpr::net::inspectInternet(snapshot, true, {"not-an-address", listener.target()}, 1000);
    CHECK(status.has_default_route);
    CHECK_EQ(status.reach, InternetReach::kOnline);
    CHECK_EQ(status.probe_target, listener.target());
    CHECK(status.probe_ms >= 0.0);
    CHECK(contains(anpr::net::formatNetworkSummary(snapshot, CameraLanSelection{}, status),
                   "status: ONLINE (" + listener.target() + " in "));
}

TEST("inspectInternet: a refused connection also proves reachability") {
    const LocalListener closed(false);
    const InternetStatus status =
        anpr::net::inspectInternet(snapshotWithDefault("wwan0"), true, {closed.target()}, 1000);
    CHECK_EQ(status.reach, InternetReach::kOnline);
    CHECK_EQ(status.probe_target, closed.target());
}

TEST("inspectInternet: an unroutable target is OFFLINE") {
    const NetworkSnapshot snapshot = snapshotWithDefault("wwan0");
    // TCP to the limited broadcast address never connects: the kernel refuses to route it.
    const InternetStatus status =
        anpr::net::inspectInternet(snapshot, true, {"255.255.255.255:53"}, 100);
    CHECK_EQ(status.reach, InternetReach::kOffline);
    CHECK(contains(status.probe_target, "255.255.255.255:53"));
    const std::string summary =
        anpr::net::formatNetworkSummary(snapshot, CameraLanSelection{}, status);
    CHECK(contains(summary, "status: OFFLINE (no answer from 255.255.255.255:53"));

    const InternetStatus skipped = anpr::net::inspectInternet(snapshot, true, {}, 100);
    CHECK_EQ(skipped.reach, InternetReach::kNotChecked);
}

// ----------------------------------------------------------------------------------------------
// Real system (read-only)

TEST("systemAddressProvider reports the loopback address without alias labels") {
    const auto addresses = anpr::net::systemAddressProvider()();
    bool loopback = false;
    for (const auto& item : addresses) {
        CHECK(item.first.find(':') == std::string::npos);
        for (const InterfaceAddress& address : item.second) {
            loopback = loopback || (address.address == ip("127.0.0.1") && address.prefix == 8);
        }
    }
    CHECK(loopback);
}

TEST("readNetworkSnapshot on this host never fails") {
    const NetworkSnapshot snapshot = anpr::net::readNetworkSnapshot();
    if (fs::exists("/sys/class/net/lo")) {
        const NetworkInterface* lo = snapshot.find("lo");
        CHECK(lo != nullptr);
        CHECK_EQ(lo->kind, InterfaceKind::kLoopback);
    } else {
        CHECK(snapshot.interfaces.empty());
    }
    SystemPaths nowhere;
    nowhere.sys_class_net = "/nonexistent/class/net";
    nowhere.proc_net_route = "/nonexistent/route";
    const NetworkSnapshot empty = anpr::net::readNetworkSnapshot(nowhere, nullptr);
    CHECK(empty.interfaces.empty());
    CHECK(empty.routes.empty());
    CHECK_EQ(anpr::net::selectCameraLan(empty, "auto").status, CameraLanStatus::kNoInterface);
}

TEST("enum names are stable for reports") {
    CHECK_EQ(anpr::net::toString(InterfaceKind::kUsbGadget), std::string("usb_gadget"));
    CHECK_EQ(anpr::net::toString(InterfaceKind::kCellular), std::string("cellular"));
    CHECK_EQ(anpr::net::toString(CameraLanStatus::kNoIpv4), std::string("no_ipv4"));
    CHECK_EQ(anpr::net::toString(CameraLanStatus::kOverrideMissing),
             std::string("override_missing"));
    CHECK_EQ(anpr::net::toString(InternetReach::kOffline), std::string("offline"));
}

// ----------------------------------------------------------------------------------------------
// ARP

TEST("parseProcNetArp keeps complete, permanent and incomplete entries") {
    const std::string text =
        "IP address       HW type     Flags       HW address            Mask     Device\n"
        "192.168.10.64    0x1         0x2         44:19:B6:01:02:03     *        eth0\n"
        "192.168.10.65    0x1         0x0         00:00:00:00:00:00     *        eth0\n"
        "192.168.10.1     0x1         0x6         00:11:22:33:44:55     *        eth0\n"
        "192.168.8.1      0x1         0x2         0c:5b:8f:27:9a:64     *        eth1\n"
        "not an entry\n";
    const std::vector<anpr::net::ArpEntry> entries = anpr::net::parseProcNetArp(text);
    CHECK_EQ(entries.size(), std::size_t{4});
    CHECK_EQ(anpr::net::toString(entries[0].ip), std::string("192.168.10.64"));
    CHECK(entries[0].complete);
    CHECK_EQ(entries[0].mac, std::string("44:19:b6:01:02:03"));
    CHECK_EQ(entries[0].interface, std::string("eth0"));
    CHECK(!entries[1].complete);
    CHECK(entries[1].mac.empty());
    CHECK(entries[2].complete);
    CHECK_EQ(entries[2].mac, std::string("00:11:22:33:44:55"));
    CHECK_EQ(entries[3].interface, std::string("eth1"));
    CHECK(anpr::net::parseProcNetArp("").empty());
}

TEST("readArpTable reads a file and tolerates a missing one") {
    TempDir dir;
    const std::string path = dir.path() + "/arp";
    writeFile(path,
              "IP address       HW type     Flags       HW address            Mask     Device\n"
              "192.168.1.64     0x1         0x2         bc:ad:28:00:00:01     *        eth0\n");
    const std::vector<anpr::net::ArpEntry> entries = anpr::net::readArpTable(path);
    CHECK_EQ(entries.size(), std::size_t{1});
    CHECK_EQ(entries[0].mac, std::string("bc:ad:28:00:00:01"));
    CHECK(anpr::net::readArpTable(dir.path() + "/missing").empty());
}

TEST("arpProbe reports a clear error instead of probing when it cannot") {
    const anpr::net::ArpProbeResult missing =
        anpr::net::arpProbe("nosuchif0", Ipv4{}, {ip("192.168.1.64")}, 50);
    CHECK(!missing.supported);
    CHECK(!missing.error.empty());
    CHECK(missing.replies.empty());

    // Loopback is not Ethernet; without CAP_NET_RAW or off Linux the socket is refused first.
    const std::string loopback = fs::exists("/sys/class/net/lo") ? "lo" : "lo0";
    const anpr::net::ArpProbeResult result =
        anpr::net::arpProbe(loopback, Ipv4{}, {ip("127.0.0.2")}, 50);
    CHECK(!result.supported);
    CHECK(!result.error.empty());
    CHECK(result.replies.empty());
#ifndef __linux__
    CHECK(contains(result.error, "not supported on this platform"));
#endif
}

// ----------------------------------------------------------------------------------------------
// System stats

TEST("parseMeminfo, parseVmRssKb and parseProcStatCpu read procfs text") {
    std::int64_t total = 0;
    std::int64_t available = 0;
    CHECK(anpr::cameras::parseMeminfo("MemTotal:        4051032 kB\n"
                                      "MemFree:          201244 kB\n"
                                      "MemAvailable:    1234567 kB\n",
                                      total, available));
    CHECK_EQ(total, std::int64_t{4051032});
    CHECK_EQ(available, std::int64_t{1234567});
    std::int64_t untouched = 7;
    CHECK(!anpr::cameras::parseMeminfo("MemTotal: 100 kB\n", untouched, untouched));
    CHECK_EQ(untouched, std::int64_t{7});

    CHECK_EQ(anpr::cameras::parseVmRssKb("Name:\tkz_anpr\nVmPeak:\t 999 kB\nVmRSS:\t  345678 kB\n"),
             std::int64_t{345678});
    CHECK_EQ(anpr::cameras::parseVmRssKb("Name:\tkz_anpr\n"), std::int64_t{-1});

    anpr::cameras::CpuTimes times;
    CHECK(anpr::cameras::parseProcStatCpu(
        "cpu  100 0 50 800 50 0 0 0 30 0\ncpu0 50 0 25 400 25 0 0 0 0 0\nintr 1 2 3\n", times));
    CHECK_EQ(times.idle, std::uint64_t{850});
    CHECK_EQ(times.total, std::uint64_t{1000});
    CHECK(!anpr::cameras::parseProcStatCpu("cpu0 1 2 3 4\n", times));
    CHECK(!anpr::cameras::parseProcStatCpu("cpu 1 2\n", times));
}

TEST("SystemStatsReader: memory, load, CPU delta, cgroup v1 and Jetson GPU load") {
    TempDir dir;
    const std::string proc = dir.path() + "/proc";
    const std::string sys = dir.path() + "/sys";
    writeFile(proc + "/meminfo", "MemTotal: 4051032 kB\nMemAvailable: 812000 kB\n");
    writeFile(proc + "/self/status", "VmRSS:\t  612000 kB\n");
    writeFile(proc + "/loadavg", "1.52 0.98 0.59 2/987 4321\n");
    writeFile(proc + "/stat", "cpu  100 0 50 800 50 0 0 0 0 0\n");
    writeFile(sys + "/fs/cgroup/memory/memory.usage_in_bytes", "1048576\n");
    writeFile(sys + "/fs/cgroup/memory/memory.limit_in_bytes", "9223372036854771712\n");
    writeFile(sys + "/devices/gpu.0/load", "345\n");

    anpr::cameras::SystemStatsReader reader(proc, sys);
    const anpr::cameras::SystemStats first = reader.read();
    CHECK_EQ(first.mem_total_kb, std::int64_t{4051032});
    CHECK_EQ(first.mem_available_kb, std::int64_t{812000});
    CHECK_EQ(first.process_rss_kb, std::int64_t{612000});
    CHECK_NEAR(first.load1, 1.52, 1e-9);
    CHECK_NEAR(first.load5, 0.98, 1e-9);
    CHECK_NEAR(first.load15, 0.59, 1e-9);
    CHECK(!first.cpu_percent.has_value());
    CHECK(first.cgroup_usage_bytes.has_value());
    CHECK_EQ(*first.cgroup_usage_bytes, std::int64_t{1048576});
    CHECK(!first.cgroup_limit_bytes.has_value());
    CHECK(first.gpu_percent.has_value());
    CHECK_NEAR(*first.gpu_percent, 34.5, 1e-9);

    // 200 more jiffies, 50 of them idle: 75 % busy.
    writeFile(proc + "/stat", "cpu  200 0 100 840 60 0 0 0 0 0\n");
    writeFile(sys + "/fs/cgroup/memory/memory.limit_in_bytes", "2147483648\n");
    const anpr::cameras::SystemStats second = reader.read();
    CHECK(second.cpu_percent.has_value());
    CHECK_NEAR(*second.cpu_percent, 75.0, 1e-9);
    CHECK(second.cgroup_limit_bytes.has_value());
    CHECK_EQ(*second.cgroup_limit_bytes, std::int64_t{2147483648LL});

    // iowait stepping backwards (tickless kernels) must not drop or invert the sample.
    writeFile(proc + "/stat", "cpu  300 0 150 840 50 0 0 0 0 0\n");
    const anpr::cameras::SystemStats third = reader.read();
    CHECK(third.cpu_percent.has_value());
    CHECK_NEAR(*third.cpu_percent, 100.0, 1e-9);
}

TEST("SystemStatsReader: cgroup v2, the Nano's platform GPU path and missing files") {
    TempDir dir;
    const std::string sys = dir.path() + "/sys";
    writeFile(sys + "/fs/cgroup/memory.current", "2097152\n");
    writeFile(sys + "/fs/cgroup/memory.max", "max\n");
    writeFile(sys + "/devices/platform/host1x/57000000.gpu/load", "1000\n");
    anpr::cameras::SystemStatsReader reader(dir.path() + "/proc", sys);
    const anpr::cameras::SystemStats stats = reader.read();
    CHECK(stats.cgroup_usage_bytes.has_value());
    CHECK_EQ(*stats.cgroup_usage_bytes, std::int64_t{2097152});
    CHECK(!stats.cgroup_limit_bytes.has_value());
    CHECK(stats.gpu_percent.has_value());
    CHECK_NEAR(*stats.gpu_percent, 100.0, 1e-9);
    CHECK_EQ(stats.mem_total_kb, std::int64_t{0});
    CHECK_EQ(stats.process_rss_kb, std::int64_t{0});
    CHECK(!stats.cpu_percent.has_value());

    writeFile(sys + "/fs/cgroup/memory.max", "536870912\n");
    const anpr::cameras::SystemStats limited = reader.read();
    CHECK(limited.cgroup_limit_bytes.has_value());
    CHECK_EQ(*limited.cgroup_limit_bytes, std::int64_t{536870912});

    anpr::cameras::SystemStatsReader nothing(dir.path() + "/none", dir.path() + "/none");
    const anpr::cameras::SystemStats empty = nothing.read();
    CHECK(!empty.cgroup_usage_bytes.has_value());
    CHECK(!empty.gpu_percent.has_value());
    CHECK_EQ(empty.mem_available_kb, std::int64_t{0});
}
