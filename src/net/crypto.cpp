// MD5 (RFC 1321), SHA-1 (RFC 3174), base64 (RFC 4648) and random tokens. Small and portable on
// purpose: the Jetson image has no OpenSSL development files, and digest authentication and ONVIF
// password digests need nothing more.
#include "anpr/net/crypto.hpp"

#include <array>
#include <cstdint>
#include <fstream>
#include <random>

namespace anpr::net {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

std::string toHex(const std::uint8_t* bytes, std::size_t count) {
    std::string hex;
    hex.reserve(count * 2);
    for (std::size_t i = 0; i < count; ++i) {
        hex.push_back(kHexDigits[bytes[i] >> 4U]);
        hex.push_back(kHexDigits[bytes[i] & 0x0FU]);
    }
    return hex;
}

std::uint32_t rotateLeft(std::uint32_t value, unsigned bits) {
    return (value << bits) | (value >> (32U - bits));
}

/// Both hashes pad the same way (0x80, zeros, 64-bit bit length); only the byte order of the
/// length differs: little-endian for MD5, big-endian for SHA-1.
std::string padMessage(const std::string& data, bool big_endian_length) {
    const std::uint64_t bit_length = static_cast<std::uint64_t>(data.size()) * 8U;
    std::string padded = data;
    padded.push_back(static_cast<char>(0x80));
    while (padded.size() % 64 != 56) {
        padded.push_back('\0');
    }
    for (unsigned i = 0; i < 8; ++i) {
        const unsigned shift = big_endian_length ? (56U - 8U * i) : (8U * i);
        padded.push_back(static_cast<char>((bit_length >> shift) & 0xFFU));
    }
    return padded;
}

std::array<std::uint8_t, 16> md5Raw(const std::string& data) {
    static const std::uint32_t kSine[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613,
        0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193,
        0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d,
        0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
        0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122,
        0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
        0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244,
        0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb,
        0xeb86d391,
    };
    static const unsigned kShift[64] = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
    };

    std::uint32_t state[4] = {0x67452301U, 0xefcdab89U, 0x98badcfeU, 0x10325476U};
    const std::string padded = padMessage(data, false);
    for (std::size_t block = 0; block < padded.size(); block += 64) {
        std::uint32_t words[16];
        for (std::size_t i = 0; i < 16; ++i) {
            const auto* bytes =
                reinterpret_cast<const unsigned char*>(padded.data() + block + i * 4);
            words[i] = static_cast<std::uint32_t>(bytes[0]) |
                       (static_cast<std::uint32_t>(bytes[1]) << 8U) |
                       (static_cast<std::uint32_t>(bytes[2]) << 16U) |
                       (static_cast<std::uint32_t>(bytes[3]) << 24U);
        }
        std::uint32_t a = state[0];
        std::uint32_t b = state[1];
        std::uint32_t c = state[2];
        std::uint32_t d = state[3];
        for (unsigned i = 0; i < 64; ++i) {
            std::uint32_t mixed = 0;
            unsigned word = 0;
            if (i < 16) {
                mixed = (b & c) | (~b & d);
                word = i;
            } else if (i < 32) {
                mixed = (d & b) | (~d & c);
                word = (5 * i + 1) % 16;
            } else if (i < 48) {
                mixed = b ^ c ^ d;
                word = (3 * i + 5) % 16;
            } else {
                mixed = c ^ (b | ~d);
                word = (7 * i) % 16;
            }
            const std::uint32_t next_d = d;
            d = c;
            c = b;
            b = b + rotateLeft(a + mixed + kSine[i] + words[word], kShift[i]);
            a = next_d;
        }
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
    }

    std::array<std::uint8_t, 16> digest{};
    for (std::size_t i = 0; i < 4; ++i) {
        for (std::size_t j = 0; j < 4; ++j) {
            digest[i * 4 + j] = static_cast<std::uint8_t>((state[i] >> (8U * j)) & 0xFFU);
        }
    }
    return digest;
}

std::array<std::uint8_t, 20> sha1Digest(const std::string& data) {
    std::uint32_t state[5] = {0x67452301U, 0xEFCDAB89U, 0x98BADCFEU, 0x10325476U, 0xC3D2E1F0U};
    const std::string padded = padMessage(data, true);
    for (std::size_t block = 0; block < padded.size(); block += 64) {
        std::uint32_t words[80];
        for (std::size_t i = 0; i < 16; ++i) {
            const auto* bytes =
                reinterpret_cast<const unsigned char*>(padded.data() + block + i * 4);
            words[i] = (static_cast<std::uint32_t>(bytes[0]) << 24U) |
                       (static_cast<std::uint32_t>(bytes[1]) << 16U) |
                       (static_cast<std::uint32_t>(bytes[2]) << 8U) |
                       static_cast<std::uint32_t>(bytes[3]);
        }
        for (std::size_t i = 16; i < 80; ++i) {
            words[i] = rotateLeft(words[i - 3] ^ words[i - 8] ^ words[i - 14] ^ words[i - 16], 1);
        }
        std::uint32_t a = state[0];
        std::uint32_t b = state[1];
        std::uint32_t c = state[2];
        std::uint32_t d = state[3];
        std::uint32_t e = state[4];
        for (std::size_t i = 0; i < 80; ++i) {
            std::uint32_t mixed = 0;
            std::uint32_t constant = 0;
            if (i < 20) {
                mixed = (b & c) | (~b & d);
                constant = 0x5A827999U;
            } else if (i < 40) {
                mixed = b ^ c ^ d;
                constant = 0x6ED9EBA1U;
            } else if (i < 60) {
                mixed = (b & c) | (b & d) | (c & d);
                constant = 0x8F1BBCDCU;
            } else {
                mixed = b ^ c ^ d;
                constant = 0xCA62C1D6U;
            }
            const std::uint32_t temp = rotateLeft(a, 5) + mixed + e + constant + words[i];
            e = d;
            d = c;
            c = rotateLeft(b, 30);
            b = a;
            a = temp;
        }
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
    }

    std::array<std::uint8_t, 20> digest{};
    for (std::size_t i = 0; i < 5; ++i) {
        for (std::size_t j = 0; j < 4; ++j) {
            digest[i * 4 + j] = static_cast<std::uint8_t>((state[i] >> (24U - 8U * j)) & 0xFFU);
        }
    }
    return digest;
}

constexpr char kBase64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int base64Value(char ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return ch - 'A';
    }
    if (ch >= 'a' && ch <= 'z') {
        return ch - 'a' + 26;
    }
    if (ch >= '0' && ch <= '9') {
        return ch - '0' + 52;
    }
    if (ch == '+') {
        return 62;
    }
    if (ch == '/') {
        return 63;
    }
    return -1;
}

}  // namespace

std::string md5Hex(const std::string& data) {
    const auto digest = md5Raw(data);
    return toHex(digest.data(), digest.size());
}

std::string sha1Raw(const std::string& data) {
    const auto digest = sha1Digest(data);
    return std::string(reinterpret_cast<const char*>(digest.data()), digest.size());
}

std::string sha1Hex(const std::string& data) {
    const auto digest = sha1Digest(data);
    return toHex(digest.data(), digest.size());
}

std::string base64Encode(const std::string& bytes) {
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t i = 0;
    const auto byteAt = [&bytes](std::size_t index) {
        return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[index]));
    };
    for (; i + 3 <= bytes.size(); i += 3) {
        const std::uint32_t group = (byteAt(i) << 16U) | (byteAt(i + 1) << 8U) | byteAt(i + 2);
        out.push_back(kBase64Alphabet[(group >> 18U) & 0x3FU]);
        out.push_back(kBase64Alphabet[(group >> 12U) & 0x3FU]);
        out.push_back(kBase64Alphabet[(group >> 6U) & 0x3FU]);
        out.push_back(kBase64Alphabet[group & 0x3FU]);
    }
    const std::size_t rest = bytes.size() - i;
    if (rest == 1) {
        const std::uint32_t group = byteAt(i) << 16U;
        out.push_back(kBase64Alphabet[(group >> 18U) & 0x3FU]);
        out.push_back(kBase64Alphabet[(group >> 12U) & 0x3FU]);
        out.append("==");
    } else if (rest == 2) {
        const std::uint32_t group = (byteAt(i) << 16U) | (byteAt(i + 1) << 8U);
        out.push_back(kBase64Alphabet[(group >> 18U) & 0x3FU]);
        out.push_back(kBase64Alphabet[(group >> 12U) & 0x3FU]);
        out.push_back(kBase64Alphabet[(group >> 6U) & 0x3FU]);
        out.push_back('=');
    }
    return out;
}

std::optional<std::string> base64Decode(const std::string& text) {
    // Whitespace is skipped: XML and MIME producers wrap long base64 values.
    std::string out;
    out.reserve(text.size() / 4 * 3 + 3);
    std::uint32_t group = 0;
    int group_size = 0;
    int padding = 0;
    for (const char ch : text) {
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
            continue;
        }
        if (ch == '=') {
            ++padding;
            continue;
        }
        const int value = base64Value(ch);
        if (value < 0 || padding > 0) {
            return std::nullopt;  // Invalid character, or data after padding.
        }
        group = (group << 6U) | static_cast<std::uint32_t>(value);
        if (++group_size == 4) {
            out.push_back(static_cast<char>((group >> 16U) & 0xFFU));
            out.push_back(static_cast<char>((group >> 8U) & 0xFFU));
            out.push_back(static_cast<char>(group & 0xFFU));
            group = 0;
            group_size = 0;
        }
    }
    // A lone sixth-bit group cannot encode a byte; padding must complete the last quantum.
    if (group_size == 1 || padding > 2 || (padding > 0 && group_size + padding != 4)) {
        return std::nullopt;
    }
    if (group_size == 2) {
        out.push_back(static_cast<char>((group >> 4U) & 0xFFU));
    } else if (group_size == 3) {
        out.push_back(static_cast<char>((group >> 10U) & 0xFFU));
        out.push_back(static_cast<char>((group >> 2U) & 0xFFU));
    }
    return out;
}

std::string randomBytes(std::size_t count) {
    std::string bytes(count, '\0');
    if (count == 0) {
        return bytes;
    }
    std::ifstream urandom("/dev/urandom", std::ios::binary);
    if (urandom && urandom.read(&bytes[0], static_cast<std::streamsize>(count)) &&
        static_cast<std::size_t>(urandom.gcount()) == count) {
        return bytes;
    }
    std::random_device device;
    for (std::size_t i = 0; i < count; ++i) {
        bytes[i] = static_cast<char>(device() & 0xFFU);
    }
    return bytes;
}

std::string randomHex(std::size_t count) {
    const std::string bytes = randomBytes(count);
    return toHex(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
}

std::string randomUuid() {
    std::string bytes = randomBytes(16);
    bytes[6] = static_cast<char>((static_cast<unsigned char>(bytes[6]) & 0x0FU) | 0x40U);
    bytes[8] = static_cast<char>((static_cast<unsigned char>(bytes[8]) & 0x3FU) | 0x80U);
    const std::string hex = toHex(reinterpret_cast<const std::uint8_t*>(bytes.data()), 16);
    return hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) + "-" +
           hex.substr(16, 4) + "-" + hex.substr(20, 12);
}

}  // namespace anpr::net
