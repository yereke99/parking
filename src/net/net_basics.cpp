// Address, MAC and socket-handle helpers shared by every camera-network module. The I/O itself
// (connect, send, receive, multicast) lives in socket.cpp.
#include "anpr/net/socket.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <sstream>

#include <net/if.h>
#include <unistd.h>

namespace anpr::net {
namespace {

bool parseDecimal(const std::string& text, std::uint32_t max_value, std::uint32_t& out) {
    if (text.empty() || text.size() > 5) {
        return false;
    }
    std::uint32_t value = 0;
    for (const char ch : text) {
        if (std::isdigit(static_cast<unsigned char>(ch)) == 0) {
            return false;
        }
        value = value * 10 + static_cast<std::uint32_t>(ch - '0');
    }
    if (value > max_value) {
        return false;
    }
    out = value;
    return true;
}

std::uint32_t maskFor(int prefix) {
    if (prefix <= 0) {
        return 0;
    }
    if (prefix >= 32) {
        return 0xFFFFFFFFU;
    }
    return 0xFFFFFFFFU << static_cast<unsigned>(32 - prefix);
}

}  // namespace

std::optional<Ipv4> parseIpv4(const std::string& text) {
    std::uint32_t value = 0;
    std::size_t start = 0;
    for (int octet = 0; octet < 4; ++octet) {
        const std::size_t dot = text.find('.', start);
        const bool last = octet == 3;
        if (last != (dot == std::string::npos)) {
            return std::nullopt;
        }
        const std::string part = text.substr(start, last ? std::string::npos : dot - start);
        // "010" is octal to inet_aton; refuse it rather than guess.
        if (part.size() > 1 && part[0] == '0') {
            return std::nullopt;
        }
        std::uint32_t number = 0;
        if (part.size() > 3 || !parseDecimal(part, 255, number)) {
            return std::nullopt;
        }
        value = (value << 8U) | number;
        start = dot + 1;
    }
    return Ipv4{value};
}

std::string toString(Ipv4 address) {
    std::ostringstream out;
    out << ((address.value >> 24U) & 0xFFU) << '.' << ((address.value >> 16U) & 0xFFU) << '.'
        << ((address.value >> 8U) & 0xFFU) << '.' << (address.value & 0xFFU);
    return out.str();
}

bool parseHostPort(const std::string& text, Ipv4& host, std::uint16_t& port) {
    const std::size_t colon = text.find(':');
    const auto parsed = parseIpv4(text.substr(0, colon));
    if (!parsed) {
        return false;
    }
    if (colon != std::string::npos) {
        std::uint32_t number = 0;
        if (!parseDecimal(text.substr(colon + 1), 65535, number) || number == 0) {
            return false;
        }
        port = static_cast<std::uint16_t>(number);
    }
    host = *parsed;
    return true;
}

bool isLoopback(Ipv4 address) {
    return (address.value >> 24U) == 127U;
}

bool isLinkLocal(Ipv4 address) {
    return (address.value >> 16U) == 0xA9FEU;  // 169.254.0.0/16
}

bool isMulticast(Ipv4 address) {
    return (address.value >> 28U) == 0xEU;  // 224.0.0.0/4
}

Ipv4 Ipv4Network::netmask() const {
    return Ipv4{maskFor(prefix)};
}

Ipv4 Ipv4Network::network() const {
    return Ipv4{address.value & maskFor(prefix)};
}

Ipv4 Ipv4Network::broadcast() const {
    return Ipv4{(address.value & maskFor(prefix)) | ~maskFor(prefix)};
}

bool Ipv4Network::contains(Ipv4 ip) const {
    return (ip.value & maskFor(prefix)) == (address.value & maskFor(prefix));
}

bool Ipv4Network::overlaps(const Ipv4Network& other) const {
    const int shorter = std::min(prefix, other.prefix);
    return (address.value & maskFor(shorter)) == (other.address.value & maskFor(shorter));
}

std::uint32_t Ipv4Network::hostCount() const {
    if (prefix >= 32) {
        return 1;
    }
    if (prefix == 31) {
        return 2;
    }
    const std::uint64_t size = 1ULL << static_cast<unsigned>(32 - std::max(prefix, 0));
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(size - 2, 0xFFFFFFFFULL));
}

std::vector<Ipv4> Ipv4Network::hosts(std::uint32_t limit) const {
    std::vector<Ipv4> result;
    const std::uint32_t count = hostCount();
    if (count > limit) {
        return result;
    }
    result.reserve(count);
    if (prefix >= 31) {
        const std::uint32_t base = network().value;
        for (std::uint32_t i = 0; i < count; ++i) {
            result.push_back(Ipv4{base + i});
        }
        return result;
    }
    const std::uint32_t first = network().value + 1;
    for (std::uint32_t i = 0; i < count; ++i) {
        result.push_back(Ipv4{first + i});
    }
    return result;
}

std::string Ipv4Network::cidr() const {
    return toString(network()) + "/" + std::to_string(prefix);
}

std::string Ipv4Network::addressWithPrefix() const {
    return toString(address) + "/" + std::to_string(prefix);
}

int prefixFromNetmask(Ipv4 netmask) {
    int prefix = 0;
    std::uint32_t mask = netmask.value;
    while ((mask & 0x80000000U) != 0U) {
        ++prefix;
        mask <<= 1U;
    }
    return mask == 0U ? prefix : -1;
}

std::string normalizeMac(const std::string& text) {
    std::string hex;
    hex.reserve(12);
    for (const char ch : text) {
        if (std::isxdigit(static_cast<unsigned char>(ch)) != 0) {
            hex.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        } else if (ch != ':' && ch != '-' && ch != '.' && ch != ' ') {
            return {};
        }
    }
    if (hex.size() != 12 || hex == "000000000000") {
        return {};
    }
    std::string mac;
    mac.reserve(17);
    for (std::size_t i = 0; i < 12; i += 2) {
        if (!mac.empty()) {
            mac.push_back(':');
        }
        mac.append(hex, i, 2);
    }
    return mac;
}

bool isHikvisionOui(const std::string& normalized_mac) {
    // Blocks registered to Hangzhou Hikvision Digital Technology. A hint for reports only.
    static const std::array<const char*, 43> kOuis = {
        "08:54:11", "08:a1:89", "0c:75:d2", "10:12:fb", "18:68:cb", "20:bb:bc", "24:28:fd",
        "24:32:ae", "28:57:be", "2c:a5:9c", "3c:1b:f8", "40:ac:bf", "44:19:b6", "44:47:cc",
        "4c:bd:8f", "4c:f5:dc", "54:8c:81", "54:c4:15", "58:03:fb", "5c:34:5b", "64:db:8b",
        "68:6d:bc", "80:7c:62", "84:9a:40", "8c:e7:48", "94:e1:ac", "98:df:82", "a4:14:37",
        "a4:29:02", "ac:cb:51", "b4:a3:82", "bc:5e:33", "bc:ad:28", "bc:ba:c2", "c0:56:e3",
        "c4:2f:90", "c8:a7:02", "d4:e8:53", "e0:ba:ad", "e0:ca:3c", "e8:a0:ed", "ec:c8:9c",
        "f8:4d:fc",
    };
    if (normalized_mac.size() < 8) {
        return false;
    }
    const std::string oui = normalized_mac.substr(0, 8);
    return std::any_of(kOuis.begin(), kOuis.end(),
                       [&oui](const char* known) { return oui == known; });
}

std::string toString(ConnectOutcome outcome) {
    switch (outcome) {
        case ConnectOutcome::kConnected:
            return "connected";
        case ConnectOutcome::kRefused:
            return "refused";
        case ConnectOutcome::kTimeout:
            return "timeout";
        case ConnectOutcome::kUnreachable:
            return "unreachable";
        case ConnectOutcome::kError:
            return "error";
    }
    return "error";
}

std::string toString(RecvStatus status) {
    switch (status) {
        case RecvStatus::kData:
            return "data";
        case RecvStatus::kClosed:
            return "closed";
        case RecvStatus::kTimeout:
            return "timeout";
        case RecvStatus::kReset:
            return "reset";
        case RecvStatus::kError:
            return "error";
    }
    return "error";
}

Socket::~Socket() {
    close();
}

Socket::Socket(Socket&& other) noexcept : fd_(other.fd_) {
    other.fd_ = -1;
}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

void Socket::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

unsigned interfaceIndex(const std::string& name) {
    return name.empty() ? 0U : if_nametoindex(name.c_str());
}

std::string errnoText(int error_number) {
    return std::strerror(error_number);
}

}  // namespace anpr::net
