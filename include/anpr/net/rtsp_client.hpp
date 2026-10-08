#pragma once

#include <cstdint>
#include <string>

#include "anpr/net/http_auth.hpp"
#include "anpr/net/http_client.hpp"
#include "anpr/net/socket.hpp"

namespace anpr::net {

/// What one RTSP DESCRIBE (or OPTIONS) told us about a camera's stream. This is the probe the
/// field diagnostics are built on: it separates "nothing at that address" from "RTSP is off",
/// "wrong password", "wrong path" and "works", before any decoder is involved.
enum class RtspProbeStatus {
    kOk,
    /// TCP connect timed out or the host is unreachable.
    kUnreachable,
    /// The host answered with RST on the RTSP port.
    kPortClosed,
    /// Connected, but no complete answer in time.
    kTimeout,
    /// 401 and no credentials were configured.
    kAuthRequired,
    /// 401 after credentials were sent once.
    kAuthFailed,
    /// 403.
    kForbidden,
    /// 404 / 454 Session Not Found / 400 for an unknown channel.
    kPathInvalid,
    /// 5xx.
    kServerError,
    /// Not RTSP, or a status we do not understand.
    kProtocolError,
};

std::string toString(RtspProbeStatus status);

struct RtspProbeOptions {
    Ipv4 host;
    std::uint16_t port{554};
    /// Absolute path starting with '/', for example "/Streaming/Channels/101".
    std::string path{"/Streaming/Channels/101"};
    /// When set, a 401 is answered exactly once (digest preferred). Never retried.
    const Credentials* credentials{nullptr};
    int timeout_ms{3000};
    /// "DESCRIBE" (default; returns the SDP) or "OPTIONS" (no authentication on most cameras,
    /// used by discovery so that it never sends a password).
    std::string method{"DESCRIBE"};
};

struct RtspProbeResult {
    RtspProbeStatus status{RtspProbeStatus::kProtocolError};
    ConnectOutcome connect{ConnectOutcome::kError};
    /// Final RTSP status code, 0 when none arrived.
    int code{0};
    std::string reason;
    /// `Server` header, if any.
    std::string server;
    /// Realm of the authentication challenge, if any. Hikvision uses values like
    /// "IP Camera(C1234)" or the model name, which helps identify the vendor without logging in.
    std::string realm;
    /// "digest" or "basic" when a challenge was offered.
    std::string auth_scheme;
    bool auth_attempted{false};
    /// The SDP body of a successful DESCRIBE.
    std::string sdp;
    std::string content_base;
    /// The `Public` header of OPTIONS.
    std::string public_methods;
    double connect_ms{0.0};
    double total_ms{0.0};
    std::string detail;
};

/// Sends DESCRIBE (or OPTIONS) to rtsp://host:port/path over TCP and classifies the answer.
RtspProbeResult rtspProbe(const RtspProbeOptions& options);

/// Percent-encodes a URL user-info component (RFC 3986): everything except unreserved characters.
std::string percentEncodeUserInfo(const std::string& value);

/// rtsp://user:pass@host:port/path with user and password percent-encoded. The result is a
/// SECRET: pass it to the decoder only, never to a log or a file.
std::string rtspUrlWithCredentials(Ipv4 host, std::uint16_t port, const std::string& path,
                                   const Credentials& credentials);

/// rtsp://host:port/path, or rtsp://<redacted>@host:port/path when credentials exist. Safe to log.
std::string redactedRtspUrl(Ipv4 host, std::uint16_t port, const std::string& path,
                            bool has_credentials);

}  // namespace anpr::net
