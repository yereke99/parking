#pragma once

#include <optional>
#include <string>

namespace anpr::net {

/// Small, dependency-free hashes for HTTP/RTSP digest authentication (MD5, RFC 1321 / RFC 2617)
/// and ONVIF WS-Security password digests (SHA-1, RFC 3174). Not for anything security-critical
/// beyond talking to cameras that require exactly these algorithms.

/// Lower-case hex MD5 of `data`.
std::string md5Hex(const std::string& data);

/// Raw 20-byte SHA-1 of `data`.
std::string sha1Raw(const std::string& data);
/// Lower-case hex SHA-1 of `data`.
std::string sha1Hex(const std::string& data);

std::string base64Encode(const std::string& bytes);
/// Accepts padded and unpadded input; nullopt on invalid characters.
std::optional<std::string> base64Decode(const std::string& text);

/// `count` random bytes from /dev/urandom (std::random_device as a fallback).
std::string randomBytes(std::size_t count);
/// Lower-case hex of `count` random bytes.
std::string randomHex(std::size_t count);
/// A random RFC 4122 version 4 UUID, "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx".
std::string randomUuid();

}  // namespace anpr::net
