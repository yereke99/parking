#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "anpr/net/crypto.hpp"
#include "anpr/net/socket.hpp"
#include "test_framework.hpp"

using anpr::net::ConnectOutcome;
using anpr::net::Ipv4;
using anpr::net::RecvStatus;

namespace {

const Ipv4 kLoopback{0x7F000001U};

/// A listening TCP socket on 127.0.0.1 with an ephemeral port.
class Listener {
public:
    explicit Listener(int backlog = 16) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        const int one = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        ::listen(fd_, backlog);
        socklen_t length = sizeof(address);
        ::getsockname(fd_, reinterpret_cast<sockaddr*>(&address), &length);
        port_ = ntohs(address.sin_port);
    }
    ~Listener() { ::close(fd_); }
    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;

    [[nodiscard]] std::uint16_t port() const { return port_; }

    /// Accepted descriptor, or -1 when nobody connected within `timeout_ms`.
    int accept(int timeout_ms) const {
        pollfd entry{};
        entry.fd = fd_;
        entry.events = POLLIN;
        if (::poll(&entry, 1, timeout_ms) <= 0) {
            return -1;
        }
        return ::accept(fd_, nullptr, nullptr);
    }

private:
    int fd_{-1};
    std::uint16_t port_{0};
};

/// A port on 127.0.0.1 that nothing listens on: bound, read back, closed.
std::uint16_t closedPort() {
    const Listener listener;
    return listener.port();
}

double msSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
        .count();
}

std::string loopbackInterface() {
    return anpr::net::interfaceIndex("lo0") != 0 ? "lo0" : "lo";
}

std::uint16_t freeUdpPort() {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    socklen_t length = sizeof(address);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length);
    ::close(fd);
    return ntohs(address.sin_port);
}

}  // namespace

TEST("md5 matches the RFC 1321 test suite") {
    CHECK_EQ(anpr::net::md5Hex(""), std::string("d41d8cd98f00b204e9800998ecf8427e"));
    CHECK_EQ(anpr::net::md5Hex("a"), std::string("0cc175b9c0f1b6a831c399e269772661"));
    CHECK_EQ(anpr::net::md5Hex("abc"), std::string("900150983cd24fb0d6963f7d28e17f72"));
    CHECK_EQ(anpr::net::md5Hex("message digest"),
             std::string("f96b697d7cb7938d525a2f31aaf161d0"));
    CHECK_EQ(anpr::net::md5Hex("abcdefghijklmnopqrstuvwxyz"),
             std::string("c3fcd3d76192e4007dfb496cca67e13b"));
    CHECK_EQ(anpr::net::md5Hex("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"),
             std::string("d174ab98d277d9f5a5611c2c9f419d9f"));
    CHECK_EQ(anpr::net::md5Hex("1234567890123456789012345678901234567890123456789012345678901234"
                               "5678901234567890"),
             std::string("57edf4a22be3c955ac49da2e2107b67a"));
    // Lengths around the 56-byte padding boundary and the 64-byte block size (Python hashlib).
    const std::vector<std::pair<std::size_t, std::string>> boundaries = {
        {55, "04364420e25c512fd958a70738aa8f72"}, {56, "668a72d5ba17f08e62dabcafad6db14b"},
        {63, "7dc2ca208106a2f703567bdff99d8981"}, {64, "c1bb4f81d892b2d57947682aeb252456"},
        {65, "1bc932052302d074bdec39795fe00cf6"},
    };
    for (const auto& boundary : boundaries) {
        CHECK_EQ(anpr::net::md5Hex(std::string(boundary.first, 'x')), boundary.second);
    }
}

TEST("sha1 matches the RFC 3174 test vectors") {
    CHECK_EQ(anpr::net::sha1Hex("abc"), std::string("a9993e364706816aba3e25717850c26c9cd0d89d"));
    CHECK_EQ(anpr::net::sha1Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
             std::string("84983e441c3bd26ebaae4aa1f95129e5e54670f1"));
    CHECK_EQ(anpr::net::sha1Hex(""), std::string("da39a3ee5e6b4b0d3255bfef95601890afd80709"));
    CHECK_EQ(anpr::net::sha1Hex(std::string(1000000, 'a')),
             std::string("34aa973cd4c4daa4f61eeb2bdbad27316534016f"));
    std::string repeated;
    for (int i = 0; i < 10; ++i) {
        repeated += "0123456701234567012345670123456701234567012345670123456701234567";
    }
    CHECK_EQ(anpr::net::sha1Hex(repeated), std::string("dea356a2cddd90c7a7ecedc5ebb563934f460452"));
    const std::string raw = anpr::net::sha1Raw("abc");
    CHECK_EQ(raw.size(), std::size_t{20});
    CHECK_EQ(static_cast<unsigned char>(raw[0]), static_cast<unsigned char>(0xa9));
    CHECK_EQ(static_cast<unsigned char>(raw[19]), static_cast<unsigned char>(0x9d));
    const std::vector<std::pair<std::size_t, std::string>> boundaries = {
        {55, "cef734ba81a024479e09eb5a75b6ddae62e6abf1"},
        {56, "901305367c259952f4e7af8323f480d59f81335b"},
        {64, "bb2fa3ee7afb9f54c6dfb5d021f14b1ffe40c163"},
        {65, "78c741ddc482e4cdf8c474a0876347a0905b6233"},
    };
    for (const auto& boundary : boundaries) {
        CHECK_EQ(anpr::net::sha1Hex(std::string(boundary.first, 'x')), boundary.second);
    }
}

TEST("base64 encodes and decodes the RFC 4648 vectors") {
    const std::vector<std::pair<std::string, std::string>> vectors = {
        {"", ""},          {"f", "Zg=="},         {"fo", "Zm8="},        {"foo", "Zm9v"},
        {"foob", "Zm9vYg=="}, {"fooba", "Zm9vYmE="}, {"foobar", "Zm9vYmFy"},
    };
    for (const auto& vector : vectors) {
        CHECK_EQ(anpr::net::base64Encode(vector.first), vector.second);
        const auto decoded = anpr::net::base64Decode(vector.second);
        CHECK(decoded.has_value());
        CHECK_EQ(*decoded, vector.first);
    }
    // Unpadded input (as found in some SDP sprop values) and wrapped input decode too.
    CHECK_EQ(anpr::net::base64Decode("Zm9vYg").value_or("<invalid>"), std::string("foob"));
    CHECK_EQ(anpr::net::base64Decode("Zm9vYmE").value_or("<invalid>"), std::string("fooba"));
    CHECK_EQ(anpr::net::base64Decode("Zm9v\r\nYmFy").value_or("<invalid>"), std::string("foobar"));
    std::string all_bytes;
    for (int i = 0; i < 256; ++i) {
        all_bytes.push_back(static_cast<char>(i));
    }
    CHECK_EQ(anpr::net::base64Decode(anpr::net::base64Encode(all_bytes)).value_or("<invalid>"),
             all_bytes);
    CHECK_EQ(anpr::net::base64Encode("Aladdin:open sesame"),
             std::string("QWxhZGRpbjpvcGVuIHNlc2FtZQ=="));
}

TEST("base64 decode rejects malformed input") {
    CHECK(!anpr::net::base64Decode("Zm9v!").has_value());
    CHECK(!anpr::net::base64Decode("Zm-v").has_value());  // URL-safe alphabet is not accepted.
    CHECK(!anpr::net::base64Decode("Z").has_value());     // Six bits cannot make a byte.
    CHECK(!anpr::net::base64Decode("Zm9vY").has_value());
    CHECK(!anpr::net::base64Decode("Zg==Zg==").has_value());  // Data after padding.
    CHECK(!anpr::net::base64Decode("Zg===").has_value());
    CHECK(!anpr::net::base64Decode("Zm9v=").has_value());
    CHECK(!anpr::net::base64Decode("====").has_value());
}

TEST("random tokens have the requested shape and differ") {
    CHECK_EQ(anpr::net::randomBytes(0).size(), std::size_t{0});
    CHECK_EQ(anpr::net::randomBytes(32).size(), std::size_t{32});
    CHECK(anpr::net::randomBytes(32) != anpr::net::randomBytes(32));
    const std::string hex = anpr::net::randomHex(8);
    CHECK_EQ(hex.size(), std::size_t{16});
    CHECK(std::all_of(hex.begin(), hex.end(), [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    }));
    const std::string uuid = anpr::net::randomUuid();
    CHECK_EQ(uuid.size(), std::size_t{36});
    CHECK_EQ(uuid[8], '-');
    CHECK_EQ(uuid[13], '-');
    CHECK_EQ(uuid[18], '-');
    CHECK_EQ(uuid[23], '-');
    CHECK_EQ(uuid[14], '4');  // Version 4.
    CHECK(uuid[19] == '8' || uuid[19] == '9' || uuid[19] == 'a' || uuid[19] == 'b');
    CHECK(uuid != anpr::net::randomUuid());
}

TEST("tcpConnect reaches a listening port with a blocking close-on-exec socket") {
    const Listener listener;
    auto result = anpr::net::tcpConnect(kLoopback, listener.port(), 1000);
    CHECK_EQ(result.outcome, ConnectOutcome::kConnected);
    CHECK(result.socket.valid());
    CHECK_EQ(result.error_number, 0);
    CHECK(result.elapsed_ms >= 0.0);
    CHECK_EQ(::fcntl(result.socket.fd(), F_GETFL, 0) & O_NONBLOCK, 0);
    CHECK((::fcntl(result.socket.fd(), F_GETFD, 0) & FD_CLOEXEC) != 0);
    const int peer = listener.accept(1000);
    CHECK(peer >= 0);
    ::close(peer);
}

TEST("tcpConnect reports a refused connection on a closed port") {
    const std::uint16_t port = closedPort();
    const auto result = anpr::net::tcpConnect(kLoopback, port, 1000);
    CHECK_EQ(result.outcome, ConnectOutcome::kRefused);
    CHECK_EQ(result.error_number, ECONNREFUSED);
    CHECK(!result.socket.valid());
    CHECK_EQ(anpr::net::toString(result.outcome), std::string("refused"));
}

#ifdef __linux__
TEST("tcpConnect times out when the handshake never completes") {
    // Linux drops SYNs while a listener's accept queue is full, which is exactly what a
    // switched-off camera looks like to the client.
    const Listener listener(0);
    auto filler = anpr::net::tcpConnect(kLoopback, listener.port(), 500);  // Fills the queue.
    CHECK_EQ(filler.outcome, ConnectOutcome::kConnected);
    const auto start = std::chrono::steady_clock::now();
    const auto blocked = anpr::net::tcpConnect(kLoopback, listener.port(), 250);
    CHECK_EQ(blocked.outcome, ConnectOutcome::kTimeout);
    CHECK_EQ(blocked.error_number, ETIMEDOUT);
    CHECK(!blocked.socket.valid());
    CHECK(msSince(start) >= 200.0);
    CHECK(msSince(start) < 1500.0);
}
#endif

TEST("sendAll and recvSome exchange data and honour max_bytes") {
    const Listener listener;
    auto client = anpr::net::tcpConnect(kLoopback, listener.port(), 1000);
    CHECK_EQ(client.outcome, ConnectOutcome::kConnected);
    const anpr::net::Socket peer(listener.accept(1000));
    CHECK(peer.valid());

    std::string error;
    CHECK(anpr::net::sendAll(client.socket, "hello camera", 1000, error));
    std::string received;
    while (received.size() < 12) {
        CHECK_EQ(anpr::net::recvSome(peer, received, 64, 1000), RecvStatus::kData);
    }
    CHECK_EQ(received, std::string("hello camera"));

    CHECK(anpr::net::sendAll(peer, std::string(100, 'x'), 1000, error));
    std::string limited = "prefix:";
    CHECK_EQ(anpr::net::recvSome(client.socket, limited, 10, 1000), RecvStatus::kData);
    CHECK(limited.size() <= 17);
    CHECK(limited.size() > 7);
    CHECK_EQ(limited.substr(0, 7), std::string("prefix:"));

    std::string rest = limited.substr(7);
    while (rest.size() < 100) {
        CHECK_EQ(anpr::net::recvSome(client.socket, rest, 1000, 1000), RecvStatus::kData);
    }
    CHECK_EQ(rest, std::string(100, 'x'));

    // A payload far larger than the socket buffers goes through while the peer drains it.
    const std::string big(4U * 1024U * 1024U, 'z');
    std::size_t drained = 0;
    std::thread reader([&peer, &drained, &big]() {
        std::string sink;
        while (drained < big.size()) {
            sink.clear();
            if (anpr::net::recvSome(peer, sink, 1U << 20U, 2000) != RecvStatus::kData) {
                return;
            }
            drained += sink.size();
        }
    });
    const bool sent = anpr::net::sendAll(client.socket, big, 5000, error);
    reader.join();
    CHECK(sent);
    CHECK_EQ(drained, big.size());
}

TEST("recvSome reports timeout, orderly close and reset") {
    const Listener listener;
    auto client = anpr::net::tcpConnect(kLoopback, listener.port(), 1000);
    CHECK_EQ(client.outcome, ConnectOutcome::kConnected);
    anpr::net::Socket peer(listener.accept(1000));
    CHECK(peer.valid());

    std::string out;
    const auto start = std::chrono::steady_clock::now();
    CHECK_EQ(anpr::net::recvSome(client.socket, out, 100, 150), RecvStatus::kTimeout);
    CHECK(msSince(start) >= 100.0);
    CHECK(out.empty());

    peer.close();
    CHECK_EQ(anpr::net::recvSome(client.socket, out, 100, 1000), RecvStatus::kClosed);

    auto second = anpr::net::tcpConnect(kLoopback, listener.port(), 1000);
    CHECK_EQ(second.outcome, ConnectOutcome::kConnected);
    const int reset_fd = listener.accept(1000);
    CHECK(reset_fd >= 0);
    linger abortive{};
    abortive.l_onoff = 1;
    abortive.l_linger = 0;
    ::setsockopt(reset_fd, SOL_SOCKET, SO_LINGER, &abortive, sizeof(abortive));
    ::close(reset_fd);  // RST instead of FIN.
    CHECK_EQ(anpr::net::recvSome(second.socket, out, 100, 1000), RecvStatus::kReset);
}

TEST("sendAll gives up at its deadline when the peer stops reading") {
    const Listener listener;
    auto client = anpr::net::tcpConnect(kLoopback, listener.port(), 1000);
    CHECK_EQ(client.outcome, ConnectOutcome::kConnected);
    const anpr::net::Socket peer(listener.accept(1000));  // Accepted, never read.
    const std::string huge(64U * 1024U * 1024U, 'q');
    std::string error;
    const auto start = std::chrono::steady_clock::now();
    CHECK(!anpr::net::sendAll(client.socket, huge, 200, error));
    CHECK(msSince(start) < 3000.0);
    CHECK(error.find("timed out") != std::string::npos);
}

TEST("socket helpers refuse a closed socket") {
    const anpr::net::Socket closed;
    std::string error;
    CHECK(!anpr::net::sendAll(closed, "x", 100, error));
    CHECK(!error.empty());
    std::string out;
    CHECK_EQ(anpr::net::recvSome(closed, out, 10, 10), RecvStatus::kError);
    CHECK_EQ(anpr::net::toString(RecvStatus::kReset), std::string("reset"));
}

TEST("probePorts reports open and closed ports in host-then-port order") {
    const Listener first;
    const Listener second;
    const std::uint16_t closed = closedPort();
    const std::vector<Ipv4> hosts = {kLoopback, kLoopback};
    const std::vector<std::uint16_t> ports = {first.port(), closed, second.port()};
    for (const int parallel : {1, 2, 64}) {
        const auto probes = anpr::net::probePorts(hosts, ports, 1000, parallel);
        CHECK_EQ(probes.size(), std::size_t{6});
        for (std::size_t i = 0; i < probes.size(); ++i) {
            CHECK(probes[i].host == kLoopback);
            CHECK_EQ(probes[i].port, ports[i % 3]);
            CHECK(probes[i].elapsed_ms >= 0.0);
            CHECK(probes[i].elapsed_ms < 1000.0);
        }
        CHECK_EQ(probes[0].outcome, ConnectOutcome::kConnected);
        CHECK_EQ(probes[1].outcome, ConnectOutcome::kRefused);
        CHECK_EQ(probes[2].outcome, ConnectOutcome::kConnected);
        CHECK_EQ(probes[3].outcome, ConnectOutcome::kConnected);
        CHECK_EQ(probes[4].outcome, ConnectOutcome::kRefused);
        CHECK_EQ(probes[5].outcome, ConnectOutcome::kConnected);
    }
    CHECK(anpr::net::probePorts({}, ports, 100, 4).empty());
    CHECK(anpr::net::probePorts(hosts, {}, 100, 4).empty());
    // A non-positive parallelism still probes everything, one at a time.
    CHECK_EQ(anpr::net::probePorts(hosts, ports, 1000, 0).size(), std::size_t{6});
}

TEST("probePorts handles more probes than descriptors in flight") {
    const Listener listener(128);
    const std::vector<Ipv4> hosts(40, kLoopback);
    const auto probes = anpr::net::probePorts(hosts, {listener.port(), closedPort()}, 1000, 7);
    CHECK_EQ(probes.size(), std::size_t{80});
    for (std::size_t i = 0; i < probes.size(); ++i) {
        CHECK_EQ(probes[i].outcome, i % 2 == 0 ? ConnectOutcome::kConnected
                                               : ConnectOutcome::kRefused);
    }
}

#ifdef __linux__
TEST("probePorts times out connects that never complete") {
    const Listener listener(0);
    auto filler = anpr::net::tcpConnect(kLoopback, listener.port(), 500);
    CHECK_EQ(filler.outcome, ConnectOutcome::kConnected);
    const auto start = std::chrono::steady_clock::now();
    const auto probes = anpr::net::probePorts({kLoopback}, {listener.port(), closedPort()}, 250, 4);
    CHECK_EQ(probes.size(), std::size_t{2});
    CHECK_EQ(probes[0].outcome, ConnectOutcome::kTimeout);
    CHECK(probes[0].elapsed_ms >= 200.0);
    CHECK_EQ(probes[1].outcome, ConnectOutcome::kRefused);
    CHECK(msSince(start) < 1500.0);
}
#endif

TEST("multicastExchange refuses an unknown interface and bad parameters") {
    anpr::net::MulticastExchange exchange;
    exchange.interface_name = "anprnosuch0";
    exchange.group = *anpr::net::parseIpv4("239.255.255.250");
    exchange.port = 37020;
    exchange.listen_ms = 100;
    auto result = anpr::net::multicastExchange(exchange, "probe");
    CHECK(!result.ran);
    CHECK(result.error.find("anprnosuch0") != std::string::npos);
    CHECK(result.datagrams.empty());

    exchange.interface_name.clear();
    result = anpr::net::multicastExchange(exchange, "probe");
    CHECK(!result.ran);
    CHECK(!result.error.empty());

    exchange.interface_name = loopbackInterface();
    exchange.group = kLoopback;
    result = anpr::net::multicastExchange(exchange, "probe");
    CHECK(!result.ran);
    CHECK(result.error.find("multicast") != std::string::npos);
}

TEST("multicastExchange sends twice and drops its own looped-back datagrams") {
    anpr::net::MulticastExchange exchange;
    exchange.interface_name = loopbackInterface();
    exchange.interface_address = kLoopback;
    exchange.group = *anpr::net::parseIpv4("239.255.255.250");
    exchange.port = freeUdpPort();
    exchange.bind_port = exchange.port;  // Joined: our own request loops back to us.
    exchange.listen_ms = 300;
    exchange.ignore_own_datagrams = false;

    const auto start = std::chrono::steady_clock::now();
    auto result = anpr::net::multicastExchange(exchange, "sadp-probe");
    CHECK(result.ran);
    CHECK(msSince(start) >= 250.0);
    CHECK(msSince(start) < 2000.0);
    CHECK_EQ(result.datagrams.size(), std::size_t{2});  // The request and its repeat.
    for (const auto& datagram : result.datagrams) {
        CHECK_EQ(datagram.payload, std::string("sadp-probe"));
        CHECK_EQ(datagram.from_port, exchange.port);
    }

    exchange.max_datagrams = 1;
    result = anpr::net::multicastExchange(exchange, "sadp-probe");
    CHECK(result.ran);
    CHECK_EQ(result.datagrams.size(), std::size_t{1});

    exchange.max_datagrams = 256;
    exchange.ignore_own_datagrams = true;
    result = anpr::net::multicastExchange(exchange, "sadp-probe");
    CHECK(result.ran);
    CHECK(result.datagrams.empty());
}

TEST("multicastExchange collects unicast replies to an ephemeral port") {
    // A WS-Discovery style responder: joined to the group, answers each probe by unicast.
    const Ipv4 group = *anpr::net::parseIpv4("239.255.255.250");
    const std::uint16_t port = freeUdpPort();
    const int responder = ::socket(AF_INET, SOCK_DGRAM, 0);
    const int one = 1;
    ::setsockopt(responder, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    CHECK(::bind(responder, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0);
    ip_mreq membership{};
    membership.imr_multiaddr.s_addr = htonl(group.value);
    membership.imr_interface.s_addr = htonl(kLoopback.value);
    if (::setsockopt(responder, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership, sizeof(membership)) !=
        0) {
        ::close(responder);
        CHECK(false);
    }
    std::atomic<int> answered{0};
    std::thread thread([responder, &answered]() {
        const auto start = std::chrono::steady_clock::now();
        while (msSince(start) < 1500.0 && answered < 2) {
            pollfd entry{};
            entry.fd = responder;
            entry.events = POLLIN;
            if (::poll(&entry, 1, 100) <= 0) {
                continue;
            }
            char buffer[256];
            sockaddr_in from{};
            socklen_t length = sizeof(from);
            const ssize_t size = ::recvfrom(responder, buffer, sizeof(buffer), 0,
                                            reinterpret_cast<sockaddr*>(&from), &length);
            if (size > 0 && std::string(buffer, static_cast<std::size_t>(size)) == "probe") {
                ::sendto(responder, "match", 5, 0, reinterpret_cast<sockaddr*>(&from), length);
                ++answered;
            }
        }
    });

    anpr::net::MulticastExchange exchange;
    exchange.interface_name = loopbackInterface();
    exchange.interface_address = kLoopback;
    exchange.group = group;
    exchange.port = port;
    exchange.bind_port = 0;
    exchange.listen_ms = 400;
    exchange.ignore_own_datagrams = false;
    const auto result = anpr::net::multicastExchange(exchange, "probe");
    thread.join();
    ::close(responder);
    CHECK(result.ran);
    CHECK_EQ(answered.load(), 2);
    CHECK_EQ(result.datagrams.size(), std::size_t{2});
    for (const auto& datagram : result.datagrams) {
        CHECK_EQ(datagram.payload, std::string("match"));
        CHECK(datagram.from == kLoopback);
        CHECK_EQ(datagram.from_port, port);
    }
}
