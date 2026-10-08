#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "anpr/net/http_auth.hpp"
#include "anpr/net/socket.hpp"

namespace anpr::net {

using HeaderList = std::vector<std::pair<std::string, std::string>>;

/// Case-insensitive header lookups shared by the HTTP and RTSP clients.
std::optional<std::string> findHeader(const HeaderList& headers, const std::string& name);
std::vector<std::string> findHeaders(const HeaderList& headers, const std::string& name);

/// A parsed response head ("HTTP/1.1 200 OK" or "RTSP/1.0 401 Unauthorized") and its headers.
struct ResponseHead {
    std::string protocol;
    int status{0};
    std::string reason;
    HeaderList headers;
};

/// Parses a status line and header block (everything before the blank line, which must not be
/// included). False on malformed input.
bool parseResponseHead(const std::string& head, ResponseHead& out);

namespace detail {

/// Incremental "Transfer-Encoding: chunked" decoder shared by `httpRequest` and `HttpStream`.
class ChunkedDecoder {
public:
    enum class Status { kNeedMore, kDone, kError };

    /// Consumes what it can from the front of `input` and appends the payload bytes to `out`.
    /// kDone once the last chunk and its trailer are consumed; bytes after it stay in `input`.
    Status feed(std::string& input, std::string& out);
    [[nodiscard]] bool done() const { return state_ == State::kDone; }
    void reset() { *this = ChunkedDecoder(); }

private:
    enum class State { kSize, kData, kDataEnd, kTrailer, kDone };
    State state_{State::kSize};
    std::uint64_t remaining_{0};
};

enum class HeadStatus {
    kOk,
    kTimeout,
    kClosed,
    kReset,
    kError,
    /// The first bytes cannot start a response of the expected protocol.
    kNotProtocol,
    kMalformed,
    kTooLarge,
};

/// Reads from `socket` into `buffer` until a complete response head of `protocol` ("HTTP" or
/// "RTSP") is buffered, then parses it and removes it from `buffer`; body bytes that arrived with
/// it stay there. Gives up as soon as the first bytes cannot start such a response, so a wrong
/// port fails fast instead of waiting for the timeout.
HeadStatus readResponseHead(const Socket& socket, std::string& buffer, const std::string& protocol,
                            int timeout_ms, ResponseHead& head);

}  // namespace detail

struct HttpResponse {
    int status{0};
    std::string reason;
    HeaderList headers;
    std::string body;

    [[nodiscard]] std::optional<std::string> header(const std::string& name) const {
        return findHeader(headers, name);
    }
};

enum class HttpOutcome {
    /// A response arrived; look at `response.status`.
    kResponse,
    /// TCP connect failed; `connect` says how.
    kConnectFailed,
    /// Connected but the response did not complete in time.
    kTimeout,
    /// Connected but the bytes were not HTTP.
    kProtocolError,
};

std::string toString(HttpOutcome outcome);

struct HttpRequestOptions {
    std::string method{"GET"};
    std::string path{"/"};
    HeaderList headers;
    std::string body;
    std::string content_type;
    /// When set and the server answers 401 with a usable challenge, the request is repeated
    /// exactly ONCE with credentials. Never more: Hikvision locks an address out after a few
    /// failed logins, so a wrong password must cost one attempt, not a retry loop.
    const Credentials* credentials{nullptr};
    int timeout_ms{3000};
    std::size_t max_body_bytes{1U << 20U};
};

struct HttpResult {
    HttpOutcome outcome{HttpOutcome::kConnectFailed};
    ConnectOutcome connect{ConnectOutcome::kError};
    HttpResponse response;
    /// True when credentials were actually sent. A 401 with this set means they were rejected.
    bool auth_attempted{false};
    std::string error;
};

/// One HTTP/1.1 request with "Connection: close". Handles Content-Length, chunked and
/// read-until-close bodies.
HttpResult httpRequest(Ipv4 host, std::uint16_t port, const HttpRequestOptions& options);

/// A long-lived streaming GET (Hikvision's ISAPI alertStream): connects, authenticates the same
/// way as `httpRequest` (one attempt), then hands out body bytes as they arrive, de-chunked.
class HttpStream {
public:
    HttpStream() = default;
    ~HttpStream();
    HttpStream(const HttpStream&) = delete;
    HttpStream& operator=(const HttpStream&) = delete;

    /// Opens the stream. On anything but a 2xx response the stream is closed and the result
    /// carries the response head (status 401 after `auth_attempted` means bad credentials).
    HttpResult open(Ipv4 host, std::uint16_t port, const HttpRequestOptions& options);

    /// Appends newly received body bytes to `out`. kTimeout means nothing arrived within
    /// `timeout_ms`; the stream is still usable.
    RecvStatus read(std::string& out, int timeout_ms);

    [[nodiscard]] bool isOpen() const { return socket_.valid(); }
    [[nodiscard]] const ResponseHead& head() const { return head_; }
    void close();

private:
    Socket socket_;
    ResponseHead head_;
    bool chunked_{false};
    detail::ChunkedDecoder decoder_;
    /// Body bytes still expected with Content-Length; unset when the body ends at close.
    std::optional<std::size_t> length_remaining_;
    bool finished_{false};
    /// Received bytes not yet handed out (still chunk-encoded when `chunked_`).
    std::string pending_;

    /// Moves deliverable bytes from `pending_` to `out`. False on a malformed chunk.
    bool deliver(std::string& out);
};

}  // namespace anpr::net
