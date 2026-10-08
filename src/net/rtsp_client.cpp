// One RTSP DESCRIBE/OPTIONS probe over TCP and its classification. Credentials are sent at most
// once per probe: a Hikvision camera locks the client address out for 30 minutes after a few
// failed logins, so a 401 after the authenticated request is final.
#include "anpr/net/rtsp_client.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>

#include <poll.h>
#include <sys/socket.h>

#include "anpr/net/crypto.hpp"

namespace anpr::net {
namespace {

using Clock = std::chrono::steady_clock;

constexpr const char* kUserAgent = "kz-anpr";
/// An SDP is a few hundred bytes; anything near this is not a camera answering DESCRIBE.
constexpr std::size_t kMaxBodyBytes = 256U * 1024U;

double msSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

std::string toUpper(std::string text) {
    for (char& ch : text) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    return text;
}

std::string toLower(std::string text) {
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

std::string normalizedPath(const std::string& path) {
    if (path.empty()) {
        return "/";
    }
    return path.front() == '/' ? path : "/" + path;
}

std::string hostPort(Ipv4 host, std::uint16_t port) {
    return toString(host) + ":" + std::to_string(port);
}

/// The outcome of one request/response on the connection.
struct Exchange {
    bool ok{false};
    /// Set when `ok` is false.
    RtspProbeStatus failure{RtspProbeStatus::kProtocolError};
    std::string error;
    ResponseHead head;
    std::string body;
};

Exchange exchange(const Socket& socket, std::string& buffer, const std::string& method,
                  const std::string& url, int cseq, const std::string& authorization,
                  int timeout_ms) {
    Exchange result;
    std::string request = method + " " + url + " RTSP/1.0\r\n";
    request += "CSeq: " + std::to_string(cseq) + "\r\n";
    request += std::string("User-Agent: ") + kUserAgent + "\r\n";
    if (method == "DESCRIBE") {
        request += "Accept: application/sdp\r\n";
    }
    if (!authorization.empty()) {
        request += "Authorization: " + authorization + "\r\n";
    }
    request += "\r\n";

    const auto deadline = Clock::now() + std::chrono::milliseconds(std::max(timeout_ms, 0));
    const auto remaining = [&deadline]() {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline -
                                                                                Clock::now());
        return static_cast<int>(std::max<long long>(left.count(), 0));
    };

    std::string send_error;
    if (!sendAll(socket, request, remaining(), send_error)) {
        result.failure = Clock::now() >= deadline ? RtspProbeStatus::kTimeout
                                                  : RtspProbeStatus::kProtocolError;
        result.error = "sending " + method + " failed: " + send_error;
        return result;
    }

    switch (detail::readResponseHead(socket, buffer, "RTSP", remaining(), result.head)) {
        case detail::HeadStatus::kOk:
            break;
        case detail::HeadStatus::kTimeout:
            result.failure = RtspProbeStatus::kTimeout;
            result.error = "no complete RTSP response within " + std::to_string(timeout_ms) + " ms";
            return result;
        case detail::HeadStatus::kClosed:
            result.error = "the server closed the connection without an RTSP response";
            return result;
        case detail::HeadStatus::kReset:
            result.error = "the server reset the connection without an RTSP response";
            return result;
        case detail::HeadStatus::kError:
            result.error = "receiving the RTSP response failed";
            return result;
        case detail::HeadStatus::kNotProtocol:
            result.error = "the port answered with something that is not RTSP";
            return result;
        case detail::HeadStatus::kMalformed:
            result.error = "malformed RTSP response head";
            return result;
        case detail::HeadStatus::kTooLarge:
            result.error = "RTSP response head too large";
            return result;
    }

    // RTSP bodies are always framed by Content-Length; without it there is no body.
    const auto length_text = findHeader(result.head.headers, "Content-Length");
    std::size_t length = 0;
    if (length_text) {
        const std::string& text = *length_text;
        const auto is_digit = [](char ch) {
            return std::isdigit(static_cast<unsigned char>(ch)) != 0;
        };
        if (text.empty() || text.size() > 9 || !std::all_of(text.begin(), text.end(), is_digit)) {
            result.error = "invalid Content-Length in the RTSP response";
            return result;
        }
        length = static_cast<std::size_t>(std::stoul(text));
        if (length > kMaxBodyBytes) {
            result.error = "RTSP response body too large (" + text + " bytes)";
            return result;
        }
    }
    while (buffer.size() < length) {
        const RecvStatus status = recvSome(socket, buffer, length - buffer.size(), remaining());
        if (status == RecvStatus::kTimeout) {
            result.failure = RtspProbeStatus::kTimeout;
            result.error = "RTSP response body incomplete within " + std::to_string(timeout_ms) +
                           " ms";
            return result;
        }
        if (status != RecvStatus::kData) {
            result.error = "connection " + toString(status) + " inside the RTSP response body";
            return result;
        }
    }
    result.body = buffer.substr(0, length);
    buffer.erase(0, length);
    result.ok = true;
    return result;
}

RtspProbeStatus statusForConnect(ConnectOutcome outcome) {
    return outcome == ConnectOutcome::kRefused ? RtspProbeStatus::kPortClosed
                                               : RtspProbeStatus::kUnreachable;
}

std::string describeConnect(const ConnectResult& connected, Ipv4 host, std::uint16_t port,
                            int timeout_ms) {
    switch (connected.outcome) {
        case ConnectOutcome::kConnected:
            return "connected to " + hostPort(host, port);
        case ConnectOutcome::kRefused:
            return "connection to " + hostPort(host, port) +
                   " refused: the host is up but nothing listens on the RTSP port";
        case ConnectOutcome::kTimeout:
            return "no answer from " + hostPort(host, port) + " within " +
                   std::to_string(timeout_ms) + " ms";
        case ConnectOutcome::kUnreachable:
        case ConnectOutcome::kError:
            break;
    }
    return "cannot connect to " + hostPort(host, port) + ": " +
           errnoText(connected.error_number != 0 ? connected.error_number : EIO);
}

/// True when the server has not closed the connection after its 401. A pending EOF means the
/// authenticated request must go over a new connection, or it would be lost unanswered.
bool peerStillOpen(const Socket& socket) {
    pollfd entry{};
    entry.fd = socket.fd();
    entry.events = POLLIN;
    if (::poll(&entry, 1, 0) <= 0) {
        return true;
    }
    if ((entry.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 && (entry.revents & POLLIN) == 0) {
        return false;
    }
    char byte = 0;
    return ::recv(socket.fd(), &byte, 1, MSG_PEEK | MSG_DONTWAIT) > 0;
}

RtspProbeStatus classifyCode(int code, bool auth_attempted) {
    if (code >= 200 && code < 300) {
        return RtspProbeStatus::kOk;
    }
    if (code == 401) {
        return auth_attempted ? RtspProbeStatus::kAuthFailed : RtspProbeStatus::kAuthRequired;
    }
    if (code == 403) {
        return RtspProbeStatus::kForbidden;
    }
    if (code == 404 || code == 454 || code == 400) {
        return RtspProbeStatus::kPathInvalid;
    }
    if (code >= 500 && code < 600) {
        return RtspProbeStatus::kServerError;
    }
    return RtspProbeStatus::kProtocolError;
}

void recordResponse(const ResponseHead& head, RtspProbeResult& result) {
    result.code = head.status;
    result.reason = head.reason;
    if (const auto server = findHeader(head.headers, "Server")) {
        result.server = *server;
    }
    if (const auto base = findHeader(head.headers, "Content-Base")) {
        result.content_base = *base;
    } else if (const auto location = findHeader(head.headers, "Content-Location")) {
        result.content_base = *location;
    }
    if (const auto methods = findHeader(head.headers, "Public")) {
        result.public_methods = *methods;
    }
}

}  // namespace

std::string toString(RtspProbeStatus status) {
    switch (status) {
        case RtspProbeStatus::kOk:
            return "ok";
        case RtspProbeStatus::kUnreachable:
            return "unreachable";
        case RtspProbeStatus::kPortClosed:
            return "port_closed";
        case RtspProbeStatus::kTimeout:
            return "timeout";
        case RtspProbeStatus::kAuthRequired:
            return "auth_required";
        case RtspProbeStatus::kAuthFailed:
            return "auth_failed";
        case RtspProbeStatus::kForbidden:
            return "forbidden";
        case RtspProbeStatus::kPathInvalid:
            return "path_invalid";
        case RtspProbeStatus::kServerError:
            return "server_error";
        case RtspProbeStatus::kProtocolError:
            return "protocol_error";
    }
    return "protocol_error";
}

RtspProbeResult rtspProbe(const RtspProbeOptions& options) {
    // Connect and each request/response get `timeout_ms` of their own, so the authenticated
    // request is never cut short by a slow first answer after the credentials were spent.
    RtspProbeResult result;
    const auto start = Clock::now();
    const std::string method = options.method.empty() ? "DESCRIBE" : toUpper(options.method);
    const std::string url =
        "rtsp://" + hostPort(options.host, options.port) + normalizedPath(options.path);
    const auto finish = [&](RtspProbeStatus status, std::string detail_text) {
        result.status = status;
        result.detail = std::move(detail_text);
        result.total_ms = msSince(start);
        return result;
    };

    ConnectResult connected = tcpConnect(options.host, options.port, options.timeout_ms);
    result.connect = connected.outcome;
    result.connect_ms = connected.elapsed_ms;
    if (connected.outcome != ConnectOutcome::kConnected) {
        return finish(statusForConnect(connected.outcome),
                      describeConnect(connected, options.host, options.port, options.timeout_ms));
    }
    Socket socket = std::move(connected.socket);
    std::string buffer;
    int cseq = 1;

    Exchange answer = exchange(socket, buffer, method, url, cseq, {}, options.timeout_ms);
    if (!answer.ok) {
        return finish(answer.failure, answer.error);
    }
    recordResponse(answer.head, result);

    if (answer.head.status == 401) {
        const std::vector<AuthChallenge> challenges =
            parseAuthChallenges(findHeaders(answer.head.headers, "WWW-Authenticate"));
        const AuthChallenge* challenge = preferredChallenge(challenges);
        const AuthChallenge* shown = challenge != nullptr
                                         ? challenge
                                         : (challenges.empty() ? nullptr : &challenges.front());
        if (shown != nullptr) {
            result.realm = shown->realm;
            result.auth_scheme = shown->scheme;
        }
        const std::string realm_text =
            result.realm.empty() ? std::string() : " (realm \"" + result.realm + "\")";
        if (options.credentials == nullptr || options.credentials->empty()) {
            return finish(RtspProbeStatus::kAuthRequired,
                          method + " answered 401 " + answer.head.reason +
                              ": the camera requires a login and no credentials are configured" +
                              realm_text);
        }
        if (challenge == nullptr) {
            return finish(RtspProbeStatus::kAuthFailed,
                          "the camera offers no supported authentication scheme" +
                              (result.auth_scheme.empty() ? std::string()
                                                          : " (" + result.auth_scheme + ")") +
                              "; credentials were not sent");
        }

        const auto connection = findHeader(answer.head.headers, "Connection");
        const bool closing = connection && toLower(*connection).find("close") != std::string::npos;
        if (closing || !peerStillOpen(socket)) {
            socket.close();
            buffer.clear();
            ConnectResult reconnected =
                tcpConnect(options.host, options.port, options.timeout_ms);
            result.connect = reconnected.outcome;
            if (reconnected.outcome != ConnectOutcome::kConnected) {
                return finish(statusForConnect(reconnected.outcome),
                              "reconnecting for the authenticated request failed: " +
                                  describeConnect(reconnected, options.host, options.port,
                                                  options.timeout_ms));
            }
            socket = std::move(reconnected.socket);
        }

        const std::string authorization = buildAuthorization(
            *challenge, *options.credentials, method, url, 1, randomHex(8));
        result.auth_attempted = true;
        ++cseq;
        answer = exchange(socket, buffer, method, url, cseq, authorization, options.timeout_ms);
        if (!answer.ok) {
            return finish(answer.failure, answer.error + " (after sending credentials once)");
        }
        recordResponse(answer.head, result);
    }

    const RtspProbeStatus status = classifyCode(answer.head.status, result.auth_attempted);
    std::string detail_text =
        method + " answered " + std::to_string(answer.head.status) +
        (answer.head.reason.empty() ? std::string() : " " + answer.head.reason);
    switch (status) {
        case RtspProbeStatus::kOk:
            if (method == "DESCRIBE") {
                result.sdp = answer.body;
            }
            break;
        case RtspProbeStatus::kAuthFailed:
            detail_text += ": the camera rejected the configured credentials (sent once, not "
                           "retried)";
            break;
        case RtspProbeStatus::kAuthRequired:
            detail_text += ": the camera requires a login";
            break;
        case RtspProbeStatus::kPathInvalid:
            detail_text += ": no stream at " + normalizedPath(options.path);
            break;
        case RtspProbeStatus::kForbidden:
            detail_text += ": access to the stream is forbidden for this user";
            break;
        case RtspProbeStatus::kServerError:
            detail_text += ": the camera reported an internal error";
            break;
        case RtspProbeStatus::kProtocolError:
            detail_text += ": unexpected RTSP status";
            break;
        case RtspProbeStatus::kUnreachable:
        case RtspProbeStatus::kPortClosed:
        case RtspProbeStatus::kTimeout:
            break;
    }
    return finish(status, detail_text);
}

std::string percentEncodeUserInfo(const std::string& value) {
    static const char kHex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size() * 3);
    for (const char ch : value) {
        const auto byte = static_cast<unsigned char>(ch);
        if (std::isalnum(byte) != 0 || ch == '-' || ch == '.' || ch == '_' || ch == '~') {
            encoded.push_back(ch);
        } else {
            encoded.push_back('%');
            encoded.push_back(kHex[byte >> 4U]);
            encoded.push_back(kHex[byte & 0x0FU]);
        }
    }
    return encoded;
}

std::string rtspUrlWithCredentials(Ipv4 host, std::uint16_t port, const std::string& path,
                                   const Credentials& credentials) {
    std::string url = "rtsp://";
    if (!credentials.empty()) {
        url += percentEncodeUserInfo(credentials.username) + ":" +
               percentEncodeUserInfo(credentials.password) + "@";
    }
    return url + hostPort(host, port) + normalizedPath(path);
}

std::string redactedRtspUrl(Ipv4 host, std::uint16_t port, const std::string& path,
                            bool has_credentials) {
    return std::string("rtsp://") + (has_credentials ? "<redacted>@" : "") + hostPort(host, port) +
           normalizedPath(path);
}

}  // namespace anpr::net
