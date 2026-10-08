// Neighbour table parsing and raw ARP probing for duplicate-IP detection on the camera LAN.
#include "anpr/net/arp.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <sstream>

#ifdef __linux__
#include <array>
#include <chrono>
#include <cstring>

#include <arpa/inet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <netpacket/packet.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#endif

#include "anpr/common/logging.hpp"

namespace anpr::net {
namespace {

constexpr unsigned long kAtfCom = 0x2UL;

bool parseHexFlags(const std::string& text, unsigned long& out) {
    if (text.empty()) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long value = std::strtoul(text.c_str(), &end, 16);
    if (errno != 0 || end == text.c_str() || *end != '\0') {
        return false;
    }
    out = value;
    return true;
}

#ifdef __linux__

constexpr std::size_t kArpPayloadSize = 28;
constexpr std::uint16_t kArpOpRequest = 1;
constexpr std::uint16_t kArpOpReply = 2;
/// Gap between requests so a /24 sweep does not burst 254 broadcasts into a small PoE switch.
constexpr auto kSendSpacing = std::chrono::milliseconds(2);

using MacBytes = std::array<std::uint8_t, 6>;

void putU16(std::uint8_t* out, std::uint16_t value) {
    out[0] = static_cast<std::uint8_t>(value >> 8U);
    out[1] = static_cast<std::uint8_t>(value & 0xFFU);
}

void putU32(std::uint8_t* out, std::uint32_t value) {
    out[0] = static_cast<std::uint8_t>(value >> 24U);
    out[1] = static_cast<std::uint8_t>((value >> 16U) & 0xFFU);
    out[2] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
    out[3] = static_cast<std::uint8_t>(value & 0xFFU);
}

std::uint16_t getU16(const std::uint8_t* in) {
    return static_cast<std::uint16_t>((in[0] << 8U) | in[1]);
}

std::uint32_t getU32(const std::uint8_t* in) {
    return (static_cast<std::uint32_t>(in[0]) << 24U) | (static_cast<std::uint32_t>(in[1]) << 16U) |
           (static_cast<std::uint32_t>(in[2]) << 8U) | static_cast<std::uint32_t>(in[3]);
}

/// Ethernet/IPv4 ARP request (RFC 826 layout). With a zero sender address it is an RFC 5227
/// probe, which a camera answers without adding the Jetson to its own neighbour table.
std::array<std::uint8_t, kArpPayloadSize> arpRequest(const MacBytes& own_mac, Ipv4 sender,
                                                     Ipv4 target) {
    std::array<std::uint8_t, kArpPayloadSize> packet{};
    putU16(&packet[0], ARPHRD_ETHER);
    putU16(&packet[2], ETH_P_IP);
    packet[4] = 6;
    packet[5] = 4;
    putU16(&packet[6], kArpOpRequest);
    std::copy(own_mac.begin(), own_mac.end(), packet.begin() + 8);
    putU32(&packet[14], sender.value);
    // Target hardware address stays zero.
    putU32(&packet[24], target.value);
    return packet;
}

std::string macText(const std::uint8_t* bytes) {
    static const char* kHex = "0123456789abcdef";
    std::string text;
    text.reserve(17);
    for (int i = 0; i < 6; ++i) {
        if (i > 0) {
            text.push_back(':');
        }
        text.push_back(kHex[bytes[i] >> 4U]);
        text.push_back(kHex[bytes[i] & 0xFU]);
    }
    return text;
}

std::int64_t millisecondsUntil(std::chrono::steady_clock::time_point when) {
    const auto left = when - std::chrono::steady_clock::now();
    if (left <= std::chrono::steady_clock::duration::zero()) {
        return 0;
    }
    // Round up so poll never spins on a sub-millisecond remainder.
    return std::chrono::duration_cast<std::chrono::milliseconds>(left).count() + 1;
}

#endif

}  // namespace

std::vector<ArpEntry> parseProcNetArp(const std::string& text) {
    std::vector<ArpEntry> entries;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream columns(line);
        std::string ip_text;
        std::string hw_type;
        std::string flags_text;
        std::string mac_text;
        std::string mask;
        std::string device;
        if (!(columns >> ip_text >> hw_type >> flags_text >> mac_text >> mask >> device)) {
            continue;
        }
        // The header line ("IP address  HW type ...") fails the address parse.
        const std::optional<Ipv4> ip = parseIpv4(ip_text);
        unsigned long flags = 0;
        if (!ip || !parseHexFlags(flags_text, flags)) {
            continue;
        }
        ArpEntry entry;
        entry.ip = *ip;
        entry.interface = device;
        // 0x0 is an incomplete or failed resolution shown with an all-zero MAC; 0x6 is a
        // permanent entry, which is complete as well.
        entry.complete = (flags & kAtfCom) != 0;
        entry.mac = entry.complete ? normalizeMac(mac_text) : std::string();
        if (entry.mac.empty()) {
            entry.complete = false;
        }
        entries.push_back(entry);
    }
    return entries;
}

std::vector<ArpEntry> readArpTable(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        return {};
    }
    std::ostringstream text;
    text << in.rdbuf();
    return parseProcNetArp(text.str());
}

ArpProbeResult arpProbe(const std::string& interface, Ipv4 sender_ip,
                        const std::vector<Ipv4>& targets, int listen_ms) {
    ArpProbeResult result;
#ifdef __linux__
    if (interface.empty() || interface.size() >= IFNAMSIZ) {
        result.error = "invalid interface name '" + interface + "'";
        return result;
    }
    const unsigned index = interfaceIndex(interface);
    if (index == 0) {
        result.error = "interface " + interface + " does not exist";
        return result;
    }
    Socket packet_socket(::socket(AF_PACKET, SOCK_DGRAM | SOCK_CLOEXEC, htons(ETH_P_ARP)));
    if (!packet_socket.valid()) {
        const int error_number = errno;
        result.error = error_number == EPERM || error_number == EACCES
                           ? "raw ARP sockets need CAP_NET_RAW: " + errnoText(error_number)
                           : "cannot open an ARP socket: " + errnoText(error_number);
        return result;
    }

    ifreq request{};
    std::memcpy(request.ifr_name, interface.c_str(), interface.size() + 1);
    if (::ioctl(packet_socket.fd(), SIOCGIFHWADDR, &request) != 0) {
        result.error = "cannot read the MAC address of " + interface + ": " + errnoText(errno);
        return result;
    }
    if (request.ifr_hwaddr.sa_family != ARPHRD_ETHER) {
        result.error = interface + " is not an Ethernet interface";
        return result;
    }
    MacBytes own_mac{};
    std::memcpy(own_mac.data(), request.ifr_hwaddr.sa_data, own_mac.size());

    sockaddr_ll local{};
    local.sll_family = AF_PACKET;
    local.sll_protocol = htons(ETH_P_ARP);
    local.sll_ifindex = static_cast<int>(index);
    if (::bind(packet_socket.fd(), reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
        result.error = "cannot bind an ARP socket to " + interface + ": " + errnoText(errno);
        return result;
    }
    result.supported = true;
    if (targets.empty()) {
        return result;
    }

    sockaddr_ll broadcast = local;
    broadcast.sll_halen = 6;
    std::memset(broadcast.sll_addr, 0xFF, 6);

    std::set<std::uint32_t> wanted;
    for (const Ipv4 target : targets) {
        wanted.insert(target.value);
    }

    using Clock = std::chrono::steady_clock;
    std::size_t next = 0;
    std::size_t send_failures = 0;
    std::string send_error;
    Clock::time_point next_send = Clock::now();
    Clock::time_point deadline = next_send + std::chrono::milliseconds(std::max(listen_ms, 0));
    for (;;) {
        const Clock::time_point now = Clock::now();
        if (next < targets.size() && now >= next_send) {
            const auto packet = arpRequest(own_mac, sender_ip, targets[next]);
            if (::sendto(packet_socket.fd(), packet.data(), packet.size(), 0,
                         reinterpret_cast<const sockaddr*>(&broadcast), sizeof(broadcast)) < 0) {
                ++send_failures;
                send_error = errnoText(errno);
            }
            ++next;
            next_send = now + kSendSpacing;
            if (next == targets.size()) {
                deadline = now + std::chrono::milliseconds(std::max(listen_ms, 0));
            }
        }
        if (next >= targets.size() && now >= deadline) {
            break;
        }
        const Clock::time_point wake = next < targets.size() ? next_send : deadline;
        pollfd waiting{};
        waiting.fd = packet_socket.fd();
        waiting.events = POLLIN;
        const int ready = ::poll(&waiting, 1, static_cast<int>(millisecondsUntil(wake)));
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            result.error = "poll failed: " + errnoText(errno);
            break;
        }
        if (ready == 0 || (waiting.revents & POLLIN) == 0) {
            continue;
        }
        for (;;) {
            std::array<std::uint8_t, 128> frame{};
            sockaddr_ll from{};
            socklen_t from_length = sizeof(from);
            const ssize_t length =
                ::recvfrom(packet_socket.fd(), frame.data(), frame.size(), MSG_DONTWAIT,
                           reinterpret_cast<sockaddr*>(&from), &from_length);
            if (length < 0) {
                break;  // EAGAIN: drained
            }
            if (static_cast<std::size_t>(length) < kArpPayloadSize ||
                from.sll_pkttype == PACKET_OUTGOING || getU16(&frame[0]) != ARPHRD_ETHER ||
                getU16(&frame[2]) != ETH_P_IP || frame[4] != 6 || frame[5] != 4) {
                continue;
            }
            const std::uint16_t operation = getU16(&frame[6]);
            if (operation != kArpOpReply && operation != kArpOpRequest) {
                continue;
            }
            // RFC 5227: any ARP packet whose sender address is the probed address, request or
            // reply, means a device owns it. Our own frames are ignored by MAC.
            const std::uint32_t sender = getU32(&frame[14]);
            if (sender == 0 || wanted.count(sender) == 0 ||
                std::equal(own_mac.begin(), own_mac.end(), &frame[8])) {
                continue;
            }
            const std::string mac = macText(&frame[8]);
            if (normalizeMac(mac).empty()) {
                continue;
            }
            result.replies[sender].insert(mac);
        }
    }
    if (send_failures == targets.size()) {
        result.error = "sending ARP requests on " + interface + " failed: " + send_error;
    }

    std::size_t duplicates = 0;
    for (const auto& item : result.replies) {
        duplicates += item.second.size() > 1 ? 1 : 0;
    }
    logEvent(LogLevel::kDebug, "arp_probe",
             LogFields()
                 .add("interface", interface)
                 .add("targets", targets.size())
                 .add("answered", result.replies.size())
                 .add("duplicates", duplicates)
                 .add("send_failures", send_failures));
#else
    (void)interface;
    (void)sender_ip;
    (void)targets;
    (void)listen_ms;
    result.error = "ARP probing needs Linux AF_PACKET sockets; not supported on this platform";
#endif
    return result;
}

}  // namespace anpr::net
