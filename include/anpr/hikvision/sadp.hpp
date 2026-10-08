#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "anpr/net/socket.hpp"

namespace anpr::hikvision {

/// SADP, Hikvision's "Search Active Devices Protocol": an XML inquiry multicast to
/// 239.255.255.250:37020 that every Hikvision device answers with its model, serial number, MAC,
/// IPv4 configuration, ports and activation state. It needs no credentials, changes nothing on
/// the device, and works before the Jetson has an address on the cameras' subnet, which makes it
/// the most informative discovery method for Hikvision cameras.
constexpr std::uint16_t kSadpPort = 37020;
constexpr const char* kSadpGroup = "239.255.255.250";

struct SadpDevice {
    /// Where the answer came from (may differ from `ipv4` while the device reconfigures).
    net::Ipv4 from;
    std::string uuid;
    std::string device_type;
    /// DeviceDescription: the model, for example "DS-TCG406-E".
    std::string model;
    /// DeviceSN: the full serial number.
    std::string serial;
    std::string mac;  ///< normalized
    std::string ipv4;
    std::string subnet_mask;
    std::string gateway;
    bool dhcp{false};
    std::uint16_t http_port{0};
    /// The Hikvision SDK / "server" port, 8000 by default.
    std::uint16_t command_port{0};
    std::string software_version;
    std::string boot_time;
    /// nullopt when the device does not report it (old firmware).
    std::optional<bool> activated;
    /// Every element of the answer by name, for diagnostics.
    std::map<std::string, std::string> fields;
};

/// The inquiry datagram. `uuid` identifies this request (any random UUID, upper case).
std::string buildSadpProbe(const std::string& uuid);

/// Parses one datagram. nullopt for anything that is not a device answer (our own looped-back
/// inquiry, other multicast traffic, malformed XML).
std::optional<SadpDevice> parseSadpResponse(const std::string& payload, net::Ipv4 from);

struct SadpResult {
    bool ran{false};
    std::string error;
    std::vector<SadpDevice> devices;
};

/// Multicasts the inquiry on `interface` and collects answers for `listen_ms`. Duplicate answers
/// from one device (same MAC and IP) are merged; two devices claiming the same IP are kept apart,
/// which is how duplicate addresses are noticed.
SadpResult sadpDiscover(const std::string& interface, net::Ipv4 interface_address, int listen_ms);

/// The merge step of `sadpDiscover`: one entry per (MAC, IPv4), later answers filling fields
/// earlier ones lacked, two MACs answering for one address kept apart. Sorted by address, then
/// MAC; answers without a valid address last.
std::vector<SadpDevice> mergeSadpAnswers(std::vector<SadpDevice> answers);

namespace detail {

/// Extracts SADP answers (UDP from port 37020) from IPv4 packets as a Linux packet socket
/// delivers them (starting at the IP header), reassembling fragmented ones. sadpDiscover feeds it
/// on Linux only; it is platform independent so that it is tested everywhere.
class SadpPacketCollector {
public:
    static constexpr std::size_t kMaxDatagrams = 512;
    /// Fragments of other traffic (their first piece filtered out) never complete; the oldest
    /// incomplete group makes room for a new one.
    static constexpr std::size_t kMaxFragmentGroups = 64;

    void addIpv4Packet(const unsigned char* packet, std::size_t length);
    /// The answers extracted so far, in arrival order.
    std::vector<net::Datagram> take();
    [[nodiscard]] std::size_t datagramCount() const { return datagrams_.size(); }
    [[nodiscard]] std::size_t pendingFragmentGroups() const { return fragments_.size(); }

private:
    struct Fragments {
        /// Pieces of the UDP datagram by byte offset.
        std::map<std::size_t, std::string> pieces;
        std::optional<std::size_t> total;
        std::size_t bytes{0};
        std::uint64_t sequence{0};
    };

    std::vector<net::Datagram> datagrams_;
    std::map<std::pair<std::uint32_t, std::uint16_t>, Fragments> fragments_;
    std::uint64_t sequence_{0};

    void addUdp(net::Ipv4 source, const std::string& udp);
    void addFragment(net::Ipv4 source, std::uint16_t identification, std::size_t offset,
                     bool more_fragments, std::string piece);
};

}  // namespace detail

}  // namespace anpr::hikvision
