#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace anpr::net {

/// An IPv4 address in host byte order.
struct Ipv4 {
    std::uint32_t value{0};

    [[nodiscard]] bool isZero() const { return value == 0; }
    friend bool operator==(Ipv4 lhs, Ipv4 rhs) { return lhs.value == rhs.value; }
    friend bool operator!=(Ipv4 lhs, Ipv4 rhs) { return lhs.value != rhs.value; }
    friend bool operator<(Ipv4 lhs, Ipv4 rhs) { return lhs.value < rhs.value; }
};

/// Parses dotted-quad text ("192.168.10.21"). Rejects anything else, including leading zeros
/// that could be read as octal and trailing junk.
std::optional<Ipv4> parseIpv4(const std::string& text);
std::string toString(Ipv4 address);

/// "host" or "host:port". The port is left unchanged when the text has none.
bool parseHostPort(const std::string& text, Ipv4& host, std::uint16_t& port);

[[nodiscard]] bool isLoopback(Ipv4 address);
/// 169.254.0.0/16: the address an interface invents when no DHCP server answers. Not usable for
/// cameras with static addresses.
[[nodiscard]] bool isLinkLocal(Ipv4 address);
[[nodiscard]] bool isMulticast(Ipv4 address);

/// An interface address with its prefix, or a network.
struct Ipv4Network {
    Ipv4 address;
    int prefix{0};

    [[nodiscard]] Ipv4 netmask() const;
    [[nodiscard]] Ipv4 network() const;
    [[nodiscard]] Ipv4 broadcast() const;
    [[nodiscard]] bool contains(Ipv4 ip) const;
    [[nodiscard]] bool overlaps(const Ipv4Network& other) const;
    /// Usable host addresses (network and broadcast excluded for prefixes below 31).
    [[nodiscard]] std::uint32_t hostCount() const;
    /// Every usable host address, ascending. Empty when there are more than `limit`.
    [[nodiscard]] std::vector<Ipv4> hosts(std::uint32_t limit) const;
    /// "192.168.10.0/24".
    [[nodiscard]] std::string cidr() const;
    /// "192.168.10.5/24": the address itself with its prefix.
    [[nodiscard]] std::string addressWithPrefix() const;
};

/// Prefix length of a contiguous netmask, -1 when the mask is not contiguous.
int prefixFromNetmask(Ipv4 netmask);

/// Lower-case colon form "44:19:b6:01:02:03" from any of "44-19-B6-01-02-03",
/// "4419.b601.0203", "4419b6010203". Empty when the text is not a MAC address or is all zeros.
std::string normalizeMac(const std::string& text);

/// True for organisationally unique identifiers registered to Hikvision. A hint for naming a
/// device in reports, never a requirement: cameras are identified by what they answer.
[[nodiscard]] bool isHikvisionOui(const std::string& normalized_mac);

/// Why a TCP connect did or did not succeed. The distinction drives the field diagnostics:
/// a refused connection proves the host is up but the service is not listening.
enum class ConnectOutcome {
    kConnected,
    /// RST: host reachable, nothing listening on the port.
    kRefused,
    /// No answer within the timeout: host down, filtered or unplugged.
    kTimeout,
    /// EHOSTUNREACH / ENETUNREACH: no ARP answer or no route.
    kUnreachable,
    kError,
};

std::string toString(ConnectOutcome outcome);

/// RAII file descriptor. Move-only.
class Socket {
public:
    Socket() = default;
    explicit Socket(int fd) : fd_(fd) {}
    ~Socket();
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;

    [[nodiscard]] int fd() const { return fd_; }
    [[nodiscard]] bool valid() const { return fd_ >= 0; }
    void close();

private:
    int fd_{-1};
};

struct ConnectResult {
    Socket socket;
    ConnectOutcome outcome{ConnectOutcome::kError};
    int error_number{0};
    double elapsed_ms{0.0};
};

/// Non-blocking connect bounded by `timeout_ms`. The returned socket is blocking again and has
/// SO_RCVTIMEO/SO_SNDTIMEO unset; use the helpers below with their own timeouts.
ConnectResult tcpConnect(Ipv4 host, std::uint16_t port, int timeout_ms);

/// Writes everything, waiting at most `timeout_ms` overall. False with `error` set otherwise.
bool sendAll(const Socket& socket, const std::string& data, int timeout_ms, std::string& error);

enum class RecvStatus { kData, kClosed, kTimeout, kReset, kError };
std::string toString(RecvStatus status);

/// Appends up to `max_bytes` to `out`, waiting at most `timeout_ms` for the first byte.
RecvStatus recvSome(const Socket& socket, std::string& out, std::size_t max_bytes, int timeout_ms);

/// Result of probing one TCP port on one host.
struct PortProbe {
    Ipv4 host;
    std::uint16_t port{0};
    ConnectOutcome outcome{ConnectOutcome::kError};
    double elapsed_ms{0.0};
};

/// Connects to every host x port pair with at most `max_parallel` sockets in flight, each bounded
/// by `timeout_ms`, and closes each socket immediately. A whole /24 on three ports takes about
/// two seconds. Results come back in host-then-port order.
std::vector<PortProbe> probePorts(const std::vector<Ipv4>& hosts,
                                  const std::vector<std::uint16_t>& ports, int timeout_ms,
                                  int max_parallel);

/// One UDP multicast request/response exchange on one interface (SADP, WS-Discovery).
struct MulticastExchange {
    /// Interface to send on and listen on. Required: discovery must stay on the camera LAN.
    std::string interface_name;
    /// The interface's IPv4 address, or zero when it has none. With zero the request is still sent
    /// through the interface by index, which lets SADP find cameras before the Jetson has an
    /// address on their subnet.
    Ipv4 interface_address;
    Ipv4 group;
    std::uint16_t port{0};
    /// 0: send from an ephemeral port and read unicast replies (WS-Discovery). Otherwise bind this
    /// port and join `group` on the interface, for protocols that answer by multicast (SADP).
    std::uint16_t bind_port{0};
    int listen_ms{2000};
    std::size_t max_datagrams{256};
    /// Datagrams from this host's own addresses (our own request looped back) are dropped.
    bool ignore_own_datagrams{true};
    /// Also send the payload to 255.255.255.255:`port` through the interface, for devices that
    /// ignore multicast (SADP tools do both). Limited broadcast never leaves the camera LAN.
    bool also_broadcast{false};
};

struct Datagram {
    Ipv4 from;
    std::uint16_t from_port{0};
    std::string payload;
};

struct MulticastResult {
    /// False when the exchange could not run at all (no such interface, socket refused).
    bool ran{false};
    std::string error;
    std::vector<Datagram> datagrams;
};

/// Sends `payload` once (twice for robustness against a dropped first datagram, spaced
/// `listen_ms / 4` apart) and collects every reply until `listen_ms` elapses.
MulticastResult multicastExchange(const MulticastExchange& exchange, const std::string& payload);

/// Interface index for a name, 0 when it does not exist.
unsigned interfaceIndex(const std::string& name);

/// Human readable errno text.
std::string errnoText(int error_number);

}  // namespace anpr::net
