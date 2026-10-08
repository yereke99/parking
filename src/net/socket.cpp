// TCP connect/send/receive with explicit timeouts, a parallel port prober and a one-interface
// UDP multicast exchange. Everything is poll()-driven so no call can block past its deadline,
// and every socket is close-on-exec and immune to SIGPIPE: a camera that resets a connection
// must never kill the process.
#include "anpr/net/socket.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstring>
#include <set>

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include "anpr/common/logging.hpp"

#ifdef __linux__
// Older libc headers lack these; the values are the kernel ABI (include/uapi/linux/in.h).
#ifndef IP_MULTICAST_ALL
#define IP_MULTICAST_ALL 49
#endif
#ifndef IP_UNICAST_IF
#define IP_UNICAST_IF 50
#endif
#endif

namespace anpr::net {
namespace {

using Clock = std::chrono::steady_clock;

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;  // SO_NOSIGPIPE is set on the socket instead.
#endif

double msSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

Clock::time_point deadlineAfter(int timeout_ms) {
    return Clock::now() + std::chrono::milliseconds(std::max(timeout_ms, 0));
}

/// Milliseconds left until `deadline`, rounded up so poll() never wakes just before it.
int remainingMs(Clock::time_point deadline) {
    const auto left = deadline - Clock::now();
    if (left <= Clock::duration::zero()) {
        return 0;
    }
    const auto ms = std::chrono::duration_cast<std::chrono::microseconds>(left).count();
    return static_cast<int>(std::min<long long>((ms + 999) / 1000, INT_MAX));
}

/// poll() on one descriptor until `deadline`, restarting after signals. >0 ready, 0 timeout,
/// -1 error with errno set.
int pollUntil(int fd, short events, Clock::time_point deadline) {
    for (;;) {
        pollfd entry{};
        entry.fd = fd;
        entry.events = events;
        const int ready = ::poll(&entry, 1, remainingMs(deadline));
        if (ready >= 0) {
            return ready;
        }
        if (errno != EINTR) {
            return -1;
        }
    }
}

int openSocket(int type, int& error_number) {
#ifdef SOCK_CLOEXEC
    const int fd = ::socket(AF_INET, type | SOCK_CLOEXEC, 0);
#else
    const int fd = ::socket(AF_INET, type, 0);
    if (fd >= 0) {
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
#endif
    if (fd < 0) {
        error_number = errno;
        return -1;
    }
#ifdef SO_NOSIGPIPE
    const int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    return fd;
}

bool setNonBlocking(int fd, bool enabled) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    const int wanted = enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return wanted == flags || ::fcntl(fd, F_SETFL, wanted) == 0;
}

/// Makes a descriptor non-blocking for one call and restores its flags afterwards. MSG_DONTWAIT
/// alone is not enough: Darwin's send() still blocks on a stream whose buffer fills mid-copy.
class NonBlockingScope {
public:
    explicit NonBlockingScope(int fd) : fd_(fd), flags_(::fcntl(fd, F_GETFL, 0)) {
        if (flags_ >= 0 && (flags_ & O_NONBLOCK) == 0) {
            changed_ = ::fcntl(fd_, F_SETFL, flags_ | O_NONBLOCK) == 0;
        }
    }
    ~NonBlockingScope() {
        if (changed_) {
            ::fcntl(fd_, F_SETFL, flags_);
        }
    }
    NonBlockingScope(const NonBlockingScope&) = delete;
    NonBlockingScope& operator=(const NonBlockingScope&) = delete;

private:
    int fd_;
    int flags_;
    bool changed_{false};
};

sockaddr_in makeAddress(Ipv4 host, std::uint16_t port) {
    sockaddr_in address{};
#if defined(__APPLE__) || defined(__FreeBSD__)
    address.sin_len = sizeof(address);  // MCAST_JOIN_GROUP rejects a zero length on BSDs.
#endif
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(host.value);
    return address;
}

int pendingSocketError(int fd) {
    int error_number = 0;
    socklen_t length = sizeof(error_number);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error_number, &length) != 0) {
        return errno;
    }
    return error_number;
}

ConnectOutcome classifyConnectError(int error_number) {
    switch (error_number) {
        case 0:
            return ConnectOutcome::kConnected;
        case ECONNREFUSED:
            return ConnectOutcome::kRefused;
        case ETIMEDOUT:
            return ConnectOutcome::kTimeout;
        case EHOSTUNREACH:
        case ENETUNREACH:
        case EHOSTDOWN:
        case ENETDOWN:
            return ConnectOutcome::kUnreachable;
        default:
            return ConnectOutcome::kError;
    }
}

/// Starts a non-blocking connect. Returns the descriptor (connect in progress or done) and sets
/// `error_number` to 0 on immediate success, EINPROGRESS while pending, or the failure.
int startConnect(Ipv4 host, std::uint16_t port, int& error_number) {
    const int fd = openSocket(SOCK_STREAM, error_number);
    if (fd < 0) {
        return -1;
    }
    if (!setNonBlocking(fd, true)) {
        error_number = errno;
        ::close(fd);
        return -1;
    }
    const sockaddr_in address = makeAddress(host, port);
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0) {
        error_number = 0;
    } else {
        // EINTR leaves a non-blocking connect running asynchronously, exactly like EINPROGRESS.
        error_number = (errno == EINTR) ? EINPROGRESS : errno;
    }
    return fd;
}

/// Leaves room for the rest of the process: probing a /24 must not starve the capture threads
/// of descriptors (macOS defaults to a soft limit of 256).
int descriptorBudget(int wanted) {
    rlimit limit{};
    int budget = std::max(wanted, 1);
    if (::getrlimit(RLIMIT_NOFILE, &limit) == 0 && limit.rlim_cur != RLIM_INFINITY) {
        const auto half = static_cast<long long>(limit.rlim_cur / 2);
        budget = static_cast<int>(std::min<long long>(budget, std::max(half - 16, 1LL)));
    }
    return budget;
}

std::set<std::uint32_t> ownIpv4Addresses() {
    std::set<std::uint32_t> addresses;
    ifaddrs* list = nullptr;
    if (::getifaddrs(&list) != 0) {
        return addresses;
    }
    for (const ifaddrs* entry = list; entry != nullptr; entry = entry->ifa_next) {
        if (entry->ifa_addr != nullptr && entry->ifa_addr->sa_family == AF_INET) {
            const auto* address = reinterpret_cast<const sockaddr_in*>(entry->ifa_addr);
            addresses.insert(ntohl(address->sin_addr.s_addr));
        }
    }
    ::freeifaddrs(list);
    return addresses;
}

bool setIntOption(int fd, int level, int name, int value) {
    return ::setsockopt(fd, level, name, &value, sizeof(value)) == 0;
}

/// Points multicast egress at the interface. Fatal when it fails: without it the request would
/// follow the default route, which on the Jetson is the GSM modem.
bool setMulticastInterface(int fd, unsigned index, Ipv4 interface_address, std::string& error) {
#ifdef __linux__
    ip_mreqn request{};
    request.imr_address.s_addr = htonl(interface_address.value);
    request.imr_ifindex = static_cast<int>(index);
    if (::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &request, sizeof(request)) == 0) {
        return true;
    }
#else
    if (!interface_address.isZero()) {
        in_addr address{};
        address.s_addr = htonl(interface_address.value);
        if (::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &address, sizeof(address)) == 0) {
            return true;
        }
    } else {
#ifdef IP_MULTICAST_IFINDEX
        const unsigned int interface_index = index;
        if (::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IFINDEX, &interface_index,
                         sizeof(interface_index)) == 0) {
            return true;
        }
#else
        (void)index;
        error = "the interface has no IPv4 address and multicast by interface index is not "
                "supported on this platform";
        return false;
#endif
    }
#endif
    error = "cannot select the multicast interface: " + errnoText(errno);
    return false;
}

bool joinGroup(int fd, unsigned index, Ipv4 interface_address, Ipv4 group) {
#ifdef __linux__
    (void)interface_address;
    ip_mreqn request{};
    request.imr_multiaddr.s_addr = htonl(group.value);
    request.imr_ifindex = static_cast<int>(index);
    return ::setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &request, sizeof(request)) == 0;
#elif defined(MCAST_JOIN_GROUP)
    (void)interface_address;
    group_req request{};
    request.gr_interface = index;
    sockaddr_in address = makeAddress(group, 0);
    std::memcpy(&request.gr_group, &address, sizeof(address));
    return ::setsockopt(fd, IPPROTO_IP, MCAST_JOIN_GROUP, &request, sizeof(request)) == 0;
#else
    (void)index;
    ip_mreq request{};
    request.imr_multiaddr.s_addr = htonl(group.value);
    request.imr_interface.s_addr = htonl(interface_address.value);
    return ::setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &request, sizeof(request)) == 0;
#endif
}

/// Keeps unicast and broadcast traffic of the socket on the interface. True when that worked.
bool pinToInterface(int fd, const std::string& name, unsigned index) {
#ifdef __linux__
    // Needs CAP_NET_RAW on kernel 4.9 (Docker grants it by default). It also restricts
    // reception to the interface, so replies from a camera on another LAN cannot sneak in.
    if (::setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, name.c_str(),
                     static_cast<socklen_t>(name.size() + 1)) == 0) {
        return true;
    }
    logEvent(LogLevel::kDebug, "multicast_bind_to_device_unavailable",
             LogFields().add("interface", name).addQuoted("error", errnoText(errno)));
    // IP_UNICAST_IF needs no capability and covers unicast and broadcast sends.
    const int network_order_index = static_cast<int>(htonl(index));
    return setIntOption(fd, IPPROTO_IP, IP_UNICAST_IF, network_order_index);
#elif defined(IP_BOUND_IF)
    // The BSD equivalent of SO_BINDTODEVICE; no privilege needed.
    (void)name;
    return setIntOption(fd, IPPROTO_IP, IP_BOUND_IF, static_cast<int>(index));
#else
    (void)fd;
    (void)name;
    (void)index;
    return false;
#endif
}

bool sendDatagram(int fd, Ipv4 to, std::uint16_t port, const std::string& payload,
                  std::string& error) {
    const sockaddr_in address = makeAddress(to, port);
    for (;;) {
        const ssize_t sent = ::sendto(fd, payload.data(), payload.size(), kSendFlags,
                                      reinterpret_cast<const sockaddr*>(&address), sizeof(address));
        if (sent >= 0) {
            return true;
        }
        if (errno != EINTR) {
            error = "send to " + toString(to) + ":" + std::to_string(port) + " failed: " +
                    errnoText(errno);
            return false;
        }
    }
}

}  // namespace

ConnectResult tcpConnect(Ipv4 host, std::uint16_t port, int timeout_ms) {
    ConnectResult result;
    const auto start = Clock::now();
    const auto deadline = deadlineAfter(timeout_ms);
    int error_number = 0;
    Socket socket(startConnect(host, port, error_number));
    if (socket.valid() && error_number == EINPROGRESS) {
        const int ready = pollUntil(socket.fd(), POLLOUT, deadline);
        if (ready == 0) {
            error_number = ETIMEDOUT;
        } else if (ready < 0) {
            error_number = errno;
        } else {
            error_number = pendingSocketError(socket.fd());
        }
    }
    result.error_number = error_number;
    result.outcome = socket.valid() ? classifyConnectError(error_number) : ConnectOutcome::kError;
    if (result.outcome == ConnectOutcome::kConnected) {
        if (setNonBlocking(socket.fd(), false)) {
            result.socket = std::move(socket);
        } else {
            result.error_number = errno;
            result.outcome = ConnectOutcome::kError;
        }
    }
    result.elapsed_ms = msSince(start);
    return result;
}

bool sendAll(const Socket& socket, const std::string& data, int timeout_ms, std::string& error) {
    if (!socket.valid()) {
        error = "socket is not open";
        return false;
    }
    const auto deadline = deadlineAfter(timeout_ms);
    const NonBlockingScope non_blocking(socket.fd());
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t written =
            ::send(socket.fd(), data.data() + sent, data.size() - sent, kSendFlags | MSG_DONTWAIT);
        if (written > 0) {
            sent += static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            const int ready = pollUntil(socket.fd(), POLLOUT, deadline);
            if (ready == 0) {
                error = "send timed out after " + std::to_string(sent) + " of " +
                        std::to_string(data.size()) + " bytes";
                return false;
            }
            if (ready < 0) {
                error = "poll failed: " + errnoText(errno);
                return false;
            }
            continue;
        }
        error = "send failed: " + errnoText(written < 0 ? errno : EIO);
        return false;
    }
    return true;
}

RecvStatus recvSome(const Socket& socket, std::string& out, std::size_t max_bytes, int timeout_ms) {
    if (!socket.valid() || max_bytes == 0) {
        return RecvStatus::kError;
    }
    // One read never takes more than 64 KiB, so a large limit does not inflate `out` needlessly.
    const std::size_t chunk = std::min<std::size_t>(max_bytes, 64U * 1024U);
    const auto deadline = deadlineAfter(timeout_ms);
    for (;;) {
        const int ready = pollUntil(socket.fd(), POLLIN, deadline);
        if (ready == 0) {
            return RecvStatus::kTimeout;
        }
        if (ready < 0) {
            return RecvStatus::kError;
        }
        const std::size_t before = out.size();
        out.resize(before + chunk);
        const ssize_t received = ::recv(socket.fd(), &out[before], chunk, MSG_DONTWAIT);
        const int error_number = errno;
        out.resize(before + static_cast<std::size_t>(std::max<ssize_t>(received, 0)));
        if (received > 0) {
            return RecvStatus::kData;
        }
        if (received == 0) {
            return RecvStatus::kClosed;
        }
        if (error_number == EINTR || error_number == EAGAIN || error_number == EWOULDBLOCK) {
            continue;
        }
        if (error_number == ECONNRESET || error_number == ECONNABORTED || error_number == EPIPE) {
            return RecvStatus::kReset;
        }
        return RecvStatus::kError;
    }
}

std::vector<PortProbe> probePorts(const std::vector<Ipv4>& hosts,
                                  const std::vector<std::uint16_t>& ports, int timeout_ms,
                                  int max_parallel) {
    std::vector<PortProbe> results;
    results.reserve(hosts.size() * ports.size());
    for (const Ipv4 host : hosts) {
        for (const std::uint16_t port : ports) {
            PortProbe probe;
            probe.host = host;
            probe.port = port;
            results.push_back(probe);
        }
    }

    struct InFlight {
        std::size_t index;
        int fd;
        Clock::time_point start;
        Clock::time_point deadline;
    };
    const std::size_t budget = static_cast<std::size_t>(descriptorBudget(max_parallel));
    std::vector<InFlight> in_flight;
    in_flight.reserve(std::min(budget, results.size()));
    std::vector<pollfd> poll_set;
    std::size_t next = 0;

    while (next < results.size() || !in_flight.empty()) {
        while (in_flight.size() < budget && next < results.size()) {
            const std::size_t index = next;
            PortProbe& probe = results[index];
            const auto start = Clock::now();
            int error_number = 0;
            const int fd = startConnect(probe.host, probe.port, error_number);
            if (fd < 0 && (error_number == EMFILE || error_number == ENFILE) &&
                !in_flight.empty()) {
                break;  // Out of descriptors: retry once a probe in flight completes.
            }
            ++next;
            if (fd >= 0 && error_number == EINPROGRESS) {
                const auto deadline = start + std::chrono::milliseconds(std::max(timeout_ms, 0));
                in_flight.push_back(InFlight{index, fd, start, deadline});
                continue;
            }
            probe.outcome = fd >= 0 ? classifyConnectError(error_number) : ConnectOutcome::kError;
            probe.elapsed_ms = msSince(start);
            if (fd >= 0) {
                ::close(fd);
            }
        }
        if (in_flight.empty()) {
            continue;
        }

        poll_set.clear();
        auto earliest = in_flight.front().deadline;
        for (const InFlight& entry : in_flight) {
            pollfd item{};
            item.fd = entry.fd;
            item.events = POLLOUT;
            poll_set.push_back(item);
            earliest = std::min(earliest, entry.deadline);
        }
        const int ready = ::poll(poll_set.data(), static_cast<nfds_t>(poll_set.size()),
                                 remainingMs(earliest));
        if (ready < 0 && errno != EINTR) {
            for (const InFlight& entry : in_flight) {
                results[entry.index].outcome = ConnectOutcome::kError;
                results[entry.index].elapsed_ms = msSince(entry.start);
                ::close(entry.fd);
            }
            in_flight.clear();
            continue;
        }

        const auto now = Clock::now();
        std::vector<InFlight> still_waiting;
        still_waiting.reserve(in_flight.size());
        for (std::size_t i = 0; i < in_flight.size(); ++i) {
            const InFlight& entry = in_flight[i];
            PortProbe& probe = results[entry.index];
            if (ready > 0 && poll_set[i].revents != 0) {
                probe.outcome = classifyConnectError(pendingSocketError(entry.fd));
            } else if (now >= entry.deadline) {
                probe.outcome = ConnectOutcome::kTimeout;
            } else {
                still_waiting.push_back(entry);
                continue;
            }
            probe.elapsed_ms = msSince(entry.start);
            ::close(entry.fd);
        }
        in_flight.swap(still_waiting);
    }
    return results;
}

MulticastResult multicastExchange(const MulticastExchange& exchange, const std::string& payload) {
    MulticastResult result;
    if (exchange.interface_name.empty()) {
        result.error = "no interface given for the multicast exchange";
        return result;
    }
    const unsigned index = interfaceIndex(exchange.interface_name);
    if (index == 0) {
        result.error = "no such network interface: " + exchange.interface_name;
        return result;
    }
    if (!isMulticast(exchange.group)) {
        result.error = toString(exchange.group) + " is not a multicast group";
        return result;
    }
    if (exchange.port == 0) {
        result.error = "no destination port for the multicast exchange";
        return result;
    }

    int error_number = 0;
    Socket socket(openSocket(SOCK_DGRAM, error_number));
    if (!socket.valid()) {
        result.error = "cannot create a UDP socket: " + errnoText(error_number);
        return result;
    }
    const int fd = socket.fd();
    // Several discovery runs (scan, check, a running service) may share the SADP port.
    setIntOption(fd, SOL_SOCKET, SO_REUSEADDR, 1);
#ifdef SO_REUSEPORT
    setIntOption(fd, SOL_SOCKET, SO_REUSEPORT, 1);
#endif
    setIntOption(fd, SOL_SOCKET, SO_RCVBUF, 1 << 20);
#ifdef __linux__
    // Otherwise a host-network socket also receives groups joined by unrelated sockets.
    setIntOption(fd, IPPROTO_IP, IP_MULTICAST_ALL, 0);
#endif
    const bool pinned = pinToInterface(fd, exchange.interface_name, index);

    const sockaddr_in local = makeAddress(Ipv4{}, exchange.bind_port);
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
        result.error = "cannot bind UDP port " + std::to_string(exchange.bind_port) + ": " +
                       errnoText(errno);
        return result;
    }
    if (!setMulticastInterface(fd, index, exchange.interface_address, result.error)) {
        return result;
    }
    const unsigned char ttl = 1;  // Never routed beyond the camera LAN.
    ::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    if (exchange.bind_port != 0 &&
        !joinGroup(fd, index, exchange.interface_address, exchange.group)) {
        // Unicast answers still arrive; only multicast answers would be missed.
        logEvent(LogLevel::kDebug, "multicast_join_failed",
                 LogFields()
                     .add("interface", exchange.interface_name)
                     .add("group", toString(exchange.group))
                     .addQuoted("error", errnoText(errno)));
    }
    bool broadcast = exchange.also_broadcast;
    if (broadcast && (!pinned || !setIntOption(fd, SOL_SOCKET, SO_BROADCAST, 1))) {
        // An unpinned limited broadcast would leave through the default route's interface.
        logEvent(LogLevel::kDebug, "multicast_broadcast_skipped",
                 LogFields().add("interface", exchange.interface_name));
        broadcast = false;
    }

    const auto sendRequest = [&](std::string& error) {
        std::string multicast_error;
        std::string broadcast_error;
        const bool multicast_sent =
            sendDatagram(fd, exchange.group, exchange.port, payload, multicast_error);
        const bool broadcast_sent =
            broadcast && sendDatagram(fd, Ipv4{0xFFFFFFFFU}, exchange.port, payload,
                                      broadcast_error);
        if (!multicast_sent && !broadcast_sent) {
            error = multicast_error;
            return false;
        }
        return true;
    };

    const auto start = Clock::now();
    const int listen_ms = std::max(exchange.listen_ms, 0);
    const auto deadline = start + std::chrono::milliseconds(listen_ms);
    const auto resend_at = start + std::chrono::milliseconds(listen_ms / 4);
    if (!sendRequest(result.error)) {
        return result;
    }
    result.ran = true;

    const std::set<std::uint32_t> own =
        exchange.ignore_own_datagrams ? ownIpv4Addresses() : std::set<std::uint32_t>{};
    bool resent = false;
    std::string buffer(64U * 1024U, '\0');
    while (result.datagrams.size() < exchange.max_datagrams) {
        const auto now = Clock::now();
        if (now >= deadline) {
            break;
        }
        if (!resent && now >= resend_at) {
            std::string ignored;
            sendRequest(ignored);  // The first send worked; a lost repeat changes nothing.
            resent = true;
            continue;
        }
        const int ready = pollUntil(fd, POLLIN, resent ? deadline : std::min(deadline, resend_at));
        if (ready < 0) {
            break;
        }
        while (ready > 0 && result.datagrams.size() < exchange.max_datagrams) {
            sockaddr_in from{};
            socklen_t from_length = sizeof(from);
            const ssize_t received =
                ::recvfrom(fd, &buffer[0], buffer.size(), MSG_DONTWAIT,
                           reinterpret_cast<sockaddr*>(&from), &from_length);
            if (received < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;  // EAGAIN: drained. Errors such as ICMP-induced ECONNREFUSED are ignored.
            }
            const std::uint32_t source = ntohl(from.sin_addr.s_addr);
            if (exchange.ignore_own_datagrams &&
                (own.count(source) != 0 || Ipv4{source} == exchange.interface_address)) {
                continue;
            }
            Datagram datagram;
            datagram.from = Ipv4{source};
            datagram.from_port = ntohs(from.sin_port);
            datagram.payload.assign(buffer.data(), static_cast<std::size_t>(received));
            result.datagrams.push_back(std::move(datagram));
        }
    }
    return result;
}

}  // namespace anpr::net
