// SADP inquiry building and answer parsing. The multicast exchange itself (sadpDiscover) is in
// the "Network" section at the end.
#include "anpr/hikvision/sadp.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <exception>
#include <thread>
#include <utility>

#ifdef __linux__
#include <arpa/inet.h>
#include <linux/filter.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <poll.h>
#include <sys/socket.h>
#endif

#include "anpr/common/logging.hpp"
#include "anpr/hikvision/xml_lite.hpp"
#include "anpr/net/crypto.hpp"

namespace anpr::hikvision {
namespace {

std::string upperCase(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    return text;
}

std::string lowerCase(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return text;
}

std::optional<bool> parseFlag(const std::string& text) {
    const std::string value = lowerCase(xml::trim(text));
    if (value == "true" || value == "1" || value == "yes") {
        return true;
    }
    if (value == "false" || value == "0" || value == "no") {
        return false;
    }
    return std::nullopt;
}

/// 0 for anything that is not a port number, so a garbled field never becomes a wrong port.
std::uint16_t parsePort(const std::string& text) {
    const std::string value = xml::trim(text);
    if (value.empty() || value.size() > 5) {
        return 0;
    }
    std::uint32_t number = 0;
    for (const char ch : value) {
        if (std::isdigit(static_cast<unsigned char>(ch)) == 0) {
            return 0;
        }
        number = number * 10 + static_cast<std::uint32_t>(ch - '0');
    }
    return number <= 65535 ? static_cast<std::uint16_t>(number) : 0;
}

}  // namespace

std::string buildSadpProbe(const std::string& uuid) {
    return "<?xml version=\"1.0\" encoding=\"utf-8\"?><Probe><Uuid>" +
           xml::escape(upperCase(uuid)) + "</Uuid><Types>inquiry</Types></Probe>";
}

std::optional<SadpDevice> parseSadpResponse(const std::string& payload, net::Ipv4 from) {
    // Some firmware puts a short binary header before the XML; the document starts at its
    // declaration or, without one, at the root element.
    const std::size_t start = std::min(payload.find("<?xml"), payload.find("<ProbeMatch"));
    if (start == std::string::npos) {
        return std::nullopt;
    }
    const std::string document = payload.substr(start);
    // Our own inquiry comes back through multicast loopback with the root <Probe>.
    if (xml::rootName(document) != "ProbeMatch") {
        return std::nullopt;
    }
    // A datagram cut short would report a device with fields silently missing.
    if (!xml::firstElement(document, "ProbeMatch")) {
        return std::nullopt;
    }

    SadpDevice device;
    device.from = from;
    for (const xml::ElementInfo& element : xml::elements(document)) {
        if (element.depth == 1 && element.closed) {
            device.fields.emplace(element.name, xml::trim(element.text));
        }
    }
    const auto field = [&device](const char* name) -> std::string {
        const auto found = device.fields.find(name);
        return found == device.fields.end() ? std::string() : found->second;
    };

    device.uuid = field("Uuid");
    device.device_type = field("DeviceType");
    device.model = field("DeviceDescription");
    device.serial = field("DeviceSN");
    device.mac = net::normalizeMac(field("MAC"));
    device.ipv4 = field("IPv4Address");
    device.subnet_mask = field("IPv4SubnetMask");
    device.gateway = field("IPv4Gateway");
    device.dhcp = parseFlag(field("DHCP")).value_or(false);
    device.http_port = parsePort(field("HttpPort"));
    device.command_port = parsePort(field("CommandPort"));
    device.software_version = field("SoftwareVersion");
    device.boot_time = field("BootTime");
    device.activated = parseFlag(field("Activated"));

    if (device.mac.empty() && device.ipv4.empty()) {
        return std::nullopt;
    }
    return device;
}

// ---------------------------------------------------------------------------------------------
// Network
// ---------------------------------------------------------------------------------------------

namespace {

constexpr std::size_t kMaxIpPacket = 65535;

/// Newer firmware answers this variant; older firmware ignores it. Both inquiries carry the same
/// Uuid, so every answer belongs to one search.
std::string buildSadpProbeV32(const std::string& uuid) {
    std::string probe = buildSadpProbe(uuid);
    const std::string types = "<Types>inquiry</Types>";
    const std::size_t at = probe.find(types);
    if (at != std::string::npos) {
        probe.replace(at, types.size(), "<Types>inquiry_v32</Types>");
    }
    return probe;
}

void fillMissing(std::string& into, const std::string& from) {
    if (into.empty()) {
        into = from;
    }
}

/// Completes `into` with what another answer of the same device adds: the inquiry_v32 answer
/// carries fields the plain one lacks on some firmware.
void mergeAnswer(SadpDevice& into, const SadpDevice& from) {
    fillMissing(into.uuid, from.uuid);
    fillMissing(into.device_type, from.device_type);
    fillMissing(into.model, from.model);
    fillMissing(into.serial, from.serial);
    fillMissing(into.subnet_mask, from.subnet_mask);
    fillMissing(into.gateway, from.gateway);
    fillMissing(into.software_version, from.software_version);
    fillMissing(into.boot_time, from.boot_time);
    if (into.fields.count("DHCP") == 0 && from.fields.count("DHCP") != 0) {
        into.dhcp = from.dhcp;
    }
    if (into.http_port == 0) {
        into.http_port = from.http_port;
    }
    if (into.command_port == 0) {
        into.command_port = from.command_port;
    }
    if (!into.activated) {
        into.activated = from.activated;
    }
    into.fields.insert(from.fields.begin(), from.fields.end());
}

/// One entry per (MAC, IPv4). Repeated answers (two inquiries, each sent twice, by multicast
/// and broadcast, seen by the UDP and the packet socket) merge.
class AnswerSet {
public:
    /// True when the answer is from a device not seen before.
    bool add(SadpDevice device) {
        for (SadpDevice& known : devices_) {
            if (known.mac == device.mac && known.ipv4 == device.ipv4) {
                mergeAnswer(known, device);
                return false;
            }
        }
        devices_.push_back(std::move(device));
        return true;
    }

    std::vector<SadpDevice> take() {
        const auto order = [](const SadpDevice& device) {
            const auto address = net::parseIpv4(device.ipv4);
            return std::make_pair(address ? 0 : 1, address ? address->value : 0U);
        };
        std::stable_sort(devices_.begin(), devices_.end(),
                         [&order](const SadpDevice& lhs, const SadpDevice& rhs) {
                             const auto left = order(lhs);
                             const auto right = order(rhs);
                             return left != right ? left < right : lhs.mac < rhs.mac;
                         });
        return std::move(devices_);
    }

private:
    std::vector<SadpDevice> devices_;
};

#ifdef __linux__

constexpr int kCapturePollMs = 50;

/// Reads SADP answers below the IP stack for the duration of the exchange. Ubuntu 18.04 (the
/// L4T root file system) ships rp_filter=1, which silently drops the answer of a camera in a
/// foreign subnet (a factory-default 192.168.1.64) and every answer while the camera LAN
/// interface has no IPv4 address at all; a packet socket sees the frames before that check.
/// Needs CAP_NET_RAW, which Docker grants by default.
class RawSadpCapture {
public:
    RawSadpCapture() = default;
    ~RawSadpCapture() { finish(); }
    RawSadpCapture(const RawSadpCapture&) = delete;
    RawSadpCapture& operator=(const RawSadpCapture&) = delete;

    /// Opens the socket on `interface` and starts reading. False with `error` set when packet
    /// sockets are not available (no CAP_NET_RAW).
    bool start(const std::string& interface, std::string& error) {
        const unsigned index = net::interfaceIndex(interface);
        if (index == 0) {
            error = "no such network interface: " + interface;
            return false;
        }
        // Protocol 0 until bind: nothing is queued before the filter is attached.
        net::Socket opened(::socket(AF_PACKET, SOCK_DGRAM | SOCK_CLOEXEC, 0));
        if (!opened.valid()) {
            error = "packet socket unavailable (needs CAP_NET_RAW): " + net::errnoText(errno);
            return false;
        }
        const int fd = opened.fd();
        // Cooked packets start at the IP header. Keeps UDP from the SADP port and non-first
        // fragments (they have no UDP header to look at); the video of running cameras never
        // reaches user space. Without the filter the collector applies the same checks.
        sock_filter code[] = {
            {BPF_LD | BPF_B | BPF_ABS, 0, 0, 9},             // protocol
            {BPF_JMP | BPF_JEQ | BPF_K, 0, 6, IPPROTO_UDP},  //   not UDP: drop
            {BPF_LD | BPF_H | BPF_ABS, 0, 0, 6},             // flags and fragment offset
            {BPF_JMP | BPF_JSET | BPF_K, 3, 0, 0x1FFF},      //   later fragment: keep
            {BPF_LDX | BPF_B | BPF_MSH, 0, 0, 0},            // X = IP header length
            {BPF_LD | BPF_H | BPF_IND, 0, 0, 0},             // UDP source port
            {BPF_JMP | BPF_JEQ | BPF_K, 0, 1, kSadpPort},    //   SADP: keep
            {BPF_RET | BPF_K, 0, 0, static_cast<unsigned>(kMaxIpPacket)},
            {BPF_RET | BPF_K, 0, 0, 0},
        };
        sock_fprog program{};
        program.len = static_cast<unsigned short>(sizeof(code) / sizeof(code[0]));
        program.filter = code;
        if (::setsockopt(fd, SOL_SOCKET, SO_ATTACH_FILTER, &program, sizeof(program)) != 0) {
            logEvent(LogLevel::kDebug, "sadp_capture_filter_unavailable",
                     LogFields().add("interface", interface).addQuoted("error",
                                                                       net::errnoText(errno)));
        }
        const int receive_buffer = 1 << 20;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof(receive_buffer));
        sockaddr_ll address{};
        address.sll_family = AF_PACKET;
        address.sll_protocol = htons(ETH_P_IP);
        address.sll_ifindex = static_cast<int>(index);
        if (::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
            error = "cannot bind a packet socket to " + interface + ": " + net::errnoText(errno);
            return false;
        }
        // Answers multicast to 239.255.255.250 pass the NIC's filter even if the UDP socket
        // could not join the group.
        packet_mreq membership{};
        membership.mr_ifindex = static_cast<int>(index);
        membership.mr_type = PACKET_MR_MULTICAST;
        membership.mr_alen = 6;
        const unsigned char group_mac[6] = {0x01, 0x00, 0x5E, 0x7F, 0xFF, 0xFA};
        std::memcpy(membership.mr_address, group_mac, sizeof(group_mac));
        ::setsockopt(fd, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &membership, sizeof(membership));

        socket_ = std::move(opened);
        buffer_.assign(kMaxIpPacket + 1, '\0');
        thread_ = std::thread([this]() {
            try {
                run();
            } catch (const std::exception& exception) {
                logEvent(LogLevel::kWarn, "sadp_capture_failed",
                         LogFields().addQuoted("error", exception.what()));
            }
        });
        return true;
    }

    /// Stops reading after draining what is queued, and hands out the captured answers.
    std::vector<net::Datagram> finish() {
        done_ = true;
        if (thread_.joinable()) {
            thread_.join();
        }
        return collector_.take();
    }

private:
    net::Socket socket_;
    std::string buffer_;
    std::atomic_bool done_{false};
    std::thread thread_;
    detail::SadpPacketCollector collector_;

    void run() {
        while (!done_.load()) {
            pollfd entry{};
            entry.fd = socket_.fd();
            entry.events = POLLIN;
            const int ready = ::poll(&entry, 1, kCapturePollMs);
            if (ready < 0 && errno != EINTR) {
                return;
            }
            if (ready > 0) {
                drain();
            }
        }
        drain();
    }

    void drain() {
        for (;;) {
            sockaddr_ll from{};
            socklen_t from_length = sizeof(from);
            const ssize_t received =
                ::recvfrom(socket_.fd(), &buffer_[0], buffer_.size(), MSG_DONTWAIT,
                           reinterpret_cast<sockaddr*>(&from), &from_length);
            if (received < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return;
            }
            // Our own inquiries leave through this interface too.
            if (from.sll_pkttype == PACKET_OUTGOING) {
                continue;
            }
            collector_.addIpv4Packet(reinterpret_cast<const unsigned char*>(buffer_.data()),
                                     static_cast<std::size_t>(received));
        }
    }
};

#endif  // __linux__

/// Runs the plain and the v32 inquiry at the same time on two sockets sharing the port, so that
/// neither shortens the other's listening window.
std::pair<net::MulticastResult, net::MulticastResult> exchangeInquiries(
    const net::MulticastExchange& exchange, const std::string& inquiry,
    const std::string& inquiry_v32) {
    net::MulticastResult v32;
    std::thread worker([&exchange, &inquiry_v32, &v32]() {
        try {
            v32 = net::multicastExchange(exchange, inquiry_v32);
        } catch (const std::exception& exception) {
            v32 = net::MulticastResult();
            v32.error = exception.what();
        }
    });
    net::MulticastResult plain;
    try {
        plain = net::multicastExchange(exchange, inquiry);
    } catch (const std::exception& exception) {
        plain = net::MulticastResult();
        plain.error = exception.what();
    }
    worker.join();
    return {std::move(plain), std::move(v32)};
}

}  // namespace

namespace detail {

void SadpPacketCollector::addIpv4Packet(const unsigned char* packet, std::size_t length) {
    if (packet == nullptr || length < 20 || (packet[0] >> 4U) != 4U ||
        datagrams_.size() >= kMaxDatagrams) {
        return;
    }
    const std::size_t header = static_cast<std::size_t>(packet[0] & 0x0FU) * 4U;
    // Ethernet pads short frames: the IP total length says where the packet ends.
    const std::size_t total = (static_cast<std::size_t>(packet[2]) << 8U) | packet[3];
    if (header < 20 || total < header || total > length || packet[9] != 17) {  // 17: UDP
        return;
    }
    const unsigned flags = (static_cast<unsigned>(packet[6]) << 8U) | packet[7];
    const std::size_t offset = static_cast<std::size_t>(flags & 0x1FFFU) * 8U;
    const bool more_fragments = (flags & 0x2000U) != 0;
    const auto identification =
        static_cast<std::uint16_t>((static_cast<unsigned>(packet[4]) << 8U) | packet[5]);
    const net::Ipv4 source{(static_cast<std::uint32_t>(packet[12]) << 24U) |
                           (static_cast<std::uint32_t>(packet[13]) << 16U) |
                           (static_cast<std::uint32_t>(packet[14]) << 8U) | packet[15]};
    std::string piece(reinterpret_cast<const char*>(packet + header), total - header);
    if (offset == 0 && !more_fragments) {
        addUdp(source, piece);
    } else {
        addFragment(source, identification, offset, more_fragments, std::move(piece));
    }
}

std::vector<net::Datagram> SadpPacketCollector::take() {
    std::vector<net::Datagram> out;
    out.swap(datagrams_);
    return out;
}

void SadpPacketCollector::addUdp(net::Ipv4 source, const std::string& udp) {
    if (udp.size() < 8) {
        return;
    }
    const auto byte = [&udp](std::size_t index) {
        return static_cast<std::size_t>(static_cast<unsigned char>(udp[index]));
    };
    const std::size_t source_port = (byte(0) << 8U) | byte(1);
    const std::size_t length = (byte(4) << 8U) | byte(5);
    if (source_port != kSadpPort || length < 8 || length > udp.size()) {
        return;
    }
    net::Datagram datagram;
    datagram.from = source;
    datagram.from_port = kSadpPort;
    datagram.payload = udp.substr(8, length - 8);
    datagrams_.push_back(std::move(datagram));
}

/// A long answer may be fragmented: the kernel would reassemble it, the packet socket sees the
/// pieces. Overlapping and repeated pieces are tolerated; a datagram is delivered once.
void SadpPacketCollector::addFragment(net::Ipv4 source, std::uint16_t identification,
                                      std::size_t offset, bool more_fragments,
                                      std::string piece) {
    const auto key = std::make_pair(source.value, identification);
    auto group = fragments_.find(key);
    if (group == fragments_.end()) {
        if (fragments_.size() >= kMaxFragmentGroups) {
            const auto oldest = std::min_element(
                fragments_.begin(), fragments_.end(), [](const auto& lhs, const auto& rhs) {
                    return lhs.second.sequence < rhs.second.sequence;
                });
            fragments_.erase(oldest);
        }
        group = fragments_.emplace(key, Fragments()).first;
        group->second.sequence = sequence_++;
    }
    Fragments& fragments = group->second;
    const auto previous = fragments.pieces.find(offset);
    const std::size_t replaced = previous == fragments.pieces.end() ? 0 : previous->second.size();
    if (offset + piece.size() > kMaxIpPacket ||
        fragments.bytes - replaced + piece.size() > kMaxIpPacket) {
        fragments_.erase(group);
        return;
    }
    if (!more_fragments) {
        fragments.total = offset + piece.size();
    }
    fragments.bytes = fragments.bytes - replaced + piece.size();
    fragments.pieces[offset] = std::move(piece);
    if (!fragments.total) {
        return;
    }
    const std::size_t total = *fragments.total;
    std::size_t covered = 0;
    for (const auto& entry : fragments.pieces) {
        if (entry.first > covered) {
            return;  // a hole: more pieces to come
        }
        covered = std::max(covered, entry.first + entry.second.size());
    }
    if (covered < total) {
        return;
    }
    std::string udp(total, '\0');
    for (const auto& entry : fragments.pieces) {
        if (entry.first < total) {
            const std::size_t count = std::min(entry.second.size(), total - entry.first);
            std::memcpy(&udp[entry.first], entry.second.data(), count);
        }
    }
    fragments_.erase(group);
    addUdp(source, udp);
}

}  // namespace detail

std::vector<SadpDevice> mergeSadpAnswers(std::vector<SadpDevice> answers) {
    AnswerSet set;
    for (SadpDevice& answer : answers) {
        set.add(std::move(answer));
    }
    return set.take();
}

SadpResult sadpDiscover(const std::string& interface, net::Ipv4 interface_address, int listen_ms) {
    const std::string uuid = upperCase(net::randomUuid());
    const std::string inquiry = buildSadpProbe(uuid);
    const std::string inquiry_v32 = buildSadpProbeV32(uuid);

    std::string capture_error;
#ifdef __linux__
    RawSadpCapture capture;
    capture.start(interface, capture_error);
#else
    capture_error = "raw packet capture is not supported on this platform";
#endif

    net::MulticastExchange exchange;
    exchange.interface_name = interface;
    exchange.interface_address = interface_address;
    exchange.group = net::parseIpv4(kSadpGroup).value_or(net::Ipv4{});
    exchange.port = kSadpPort;
    // Devices answer to the inquiry's source port, and some firmware multicasts the answer to
    // the group: binding the SADP port itself receives both.
    exchange.bind_port = kSadpPort;
    exchange.listen_ms = listen_ms;
    exchange.also_broadcast = true;

    auto exchanged = exchangeInquiries(exchange, inquiry, inquiry_v32);
    if (!exchanged.first.ran && !exchanged.second.ran) {
        // Usually the SADP port is held by a program without SO_REUSEPORT (Hikvision's SADP
        // tool, another scanner): unicast answers still come back to an ephemeral port.
        logEvent(LogLevel::kDebug, "sadp_retry_ephemeral_port",
                 LogFields().add("interface", interface).addQuoted("error", exchanged.first.error));
        exchange.bind_port = 0;
        auto retry = exchangeInquiries(exchange, inquiry, inquiry_v32);
        if (retry.first.ran || retry.second.ran) {
            exchanged = std::move(retry);
        }
    }

#ifdef __linux__
    const std::vector<net::Datagram> captured = capture.finish();
#else
    const std::vector<net::Datagram> captured;
#endif

    AnswerSet answers;
    for (const net::MulticastResult* part : {&exchanged.first, &exchanged.second}) {
        for (const net::Datagram& datagram : part->datagrams) {
            if (auto device = parseSadpResponse(datagram.payload, datagram.from)) {
                answers.add(std::move(*device));
            }
        }
    }
    int captured_only = 0;
    for (const net::Datagram& datagram : captured) {
        if (auto device = parseSadpResponse(datagram.payload, datagram.from)) {
            captured_only += answers.add(std::move(*device)) ? 1 : 0;
        }
    }
    if (captured_only > 0) {
        logEvent(LogLevel::kInfo, "sadp_answers_captured_only",
                 LogFields()
                     .add("interface", interface)
                     .add("devices", captured_only)
                     .addQuoted("detail",
                                "answers the UDP socket never received (reverse-path filter or "
                                "a foreign subnet) were read from a packet socket"));
    }

    SadpResult result;
    result.ran = exchanged.first.ran || exchanged.second.ran;
    if (!result.ran) {
        result.error = exchanged.first.error.empty() ? exchanged.second.error
                                                     : exchanged.first.error;
        if (!capture_error.empty()) {
            result.error += "; " + capture_error;
        }
    } else {
        if (!exchanged.first.ran || !exchanged.second.ran) {
            logEvent(LogLevel::kDebug, "sadp_inquiry_failed",
                     LogFields()
                         .add("interface", interface)
                         .add("variant", exchanged.first.ran ? "inquiry_v32" : "inquiry")
                         .addQuoted("error", exchanged.first.ran ? exchanged.second.error
                                                                 : exchanged.first.error));
        }
        if (!capture_error.empty()) {
            logEvent(LogLevel::kDebug, "sadp_capture_unavailable",
                     LogFields().add("interface", interface).addQuoted("error", capture_error));
        }
    }
    result.devices = answers.take();
    return result;
}

}  // namespace anpr::hikvision
