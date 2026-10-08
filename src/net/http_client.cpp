// Minimal HTTP/1.1 client for ISAPI: one request per connection ("Connection: close"), every
// response framing (Content-Length, chunked, read-until-close), and authentication that sends
// credentials at most once per request, because Hikvision locks an address out after a few
// failed logins.
#include "anpr/net/http_client.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <climits>

namespace anpr::net {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kMaxHeadBytes = 64U * 1024U;
constexpr std::size_t kReadChunk = 64U * 1024U;
constexpr std::size_t kMaxChunkLine = 4096;
constexpr const char* kUserAgent = "kz-anpr";

std::string toLower(std::string text) {
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

bool equalsIgnoreCase(const std::string& lhs, const std::string& rhs) {
    return lhs.size() == rhs.size() && toLower(lhs) == toLower(rhs);
}

std::string trim(const std::string& text) {
    const std::size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const std::size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

bool isTokenChar(char ch) {
    if (std::isalnum(static_cast<unsigned char>(ch)) != 0) {
        return true;
    }
    static const std::string kExtra = "!#$%&'*+-.^_`|~";
    return kExtra.find(ch) != std::string::npos;
}

Clock::time_point deadlineAfter(int timeout_ms) {
    return Clock::now() + std::chrono::milliseconds(std::max(timeout_ms, 0));
}

int remainingMs(Clock::time_point deadline) {
    const auto left = deadline - Clock::now();
    if (left <= Clock::duration::zero()) {
        return 0;
    }
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(left).count();
    return static_cast<int>(std::min<long long>((us + 999) / 1000, INT_MAX));
}

/// "HTTP/1.1", "RTSP/1.0": an upper-case name, a slash and a dotted numeric version.
bool isProtocolVersion(const std::string& text) {
    const std::size_t slash = text.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= text.size()) {
        return false;
    }
    for (std::size_t i = 0; i < slash; ++i) {
        if (std::isupper(static_cast<unsigned char>(text[i])) == 0) {
            return false;
        }
    }
    bool digit_seen = false;
    for (std::size_t i = slash + 1; i < text.size(); ++i) {
        const char ch = text[i];
        if (std::isdigit(static_cast<unsigned char>(ch)) != 0) {
            digit_seen = true;
        } else if (ch != '.') {
            return false;
        }
    }
    return digit_seen;
}

bool parseContentLength(const std::string& text, std::size_t& length) {
    const std::string value = trim(text);
    if (value.empty() || value.size() > 18) {
        return false;
    }
    std::size_t parsed = 0;
    for (const char ch : value) {
        if (std::isdigit(static_cast<unsigned char>(ch)) == 0) {
            return false;
        }
        parsed = parsed * 10 + static_cast<std::size_t>(ch - '0');
    }
    length = parsed;
    return true;
}

bool isChunked(const ResponseHead& head) {
    const auto encoding = findHeader(head.headers, "Transfer-Encoding");
    return encoding && toLower(*encoding).find("chunked") != std::string::npos;
}

bool hasNoBody(const std::string& method, int status) {
    return equalsIgnoreCase(method, "HEAD") || (status >= 100 && status < 200) || status == 204 ||
           status == 304;
}

std::string normalizedPath(const std::string& path) {
    if (path.empty()) {
        return "/";
    }
    return path.front() == '/' ? path : "/" + path;
}

bool userHeaderPresent(const HeaderList& headers, const char* name) {
    return std::any_of(headers.begin(), headers.end(),
                       [name](const std::pair<std::string, std::string>& header) {
                           return equalsIgnoreCase(header.first, name);
                       });
}

std::string buildRequest(Ipv4 host, std::uint16_t port, const HttpRequestOptions& options,
                         const std::string& authorization) {
    std::string request = options.method + " " + normalizedPath(options.path) + " HTTP/1.1\r\n";
    if (!userHeaderPresent(options.headers, "Host")) {
        request += "Host: " + toString(host) + (port == 80 ? "" : ":" + std::to_string(port)) +
                   "\r\n";
    }
    if (!userHeaderPresent(options.headers, "User-Agent")) {
        request += std::string("User-Agent: ") + kUserAgent + "\r\n";
    }
    request += "Connection: close\r\n";
    if (!authorization.empty()) {
        request += "Authorization: " + authorization + "\r\n";
    }
    for (const auto& header : options.headers) {
        // Framing and authentication belong to this client; a caller's copy would contradict it.
        if (equalsIgnoreCase(header.first, "Connection") ||
            equalsIgnoreCase(header.first, "Content-Length") ||
            equalsIgnoreCase(header.first, "Authorization") ||
            equalsIgnoreCase(header.first, "Transfer-Encoding")) {
            continue;
        }
        request += header.first + ": " + header.second + "\r\n";
    }
    if (!options.content_type.empty() && !userHeaderPresent(options.headers, "Content-Type")) {
        request += "Content-Type: " + options.content_type + "\r\n";
    }
    const bool sends_body = !options.body.empty() || equalsIgnoreCase(options.method, "POST") ||
                            equalsIgnoreCase(options.method, "PUT") ||
                            equalsIgnoreCase(options.method, "PATCH");
    if (sends_body) {
        request += "Content-Length: " + std::to_string(options.body.size()) + "\r\n";
    }
    request += "\r\n";
    request += options.body;
    return request;
}

std::string describeHeadFailure(detail::HeadStatus status) {
    switch (status) {
        case detail::HeadStatus::kOk:
            return "ok";
        case detail::HeadStatus::kTimeout:
            return "no complete response before the timeout";
        case detail::HeadStatus::kClosed:
            return "connection closed before a complete response";
        case detail::HeadStatus::kReset:
            return "connection reset by the peer";
        case detail::HeadStatus::kError:
            return "receive failed";
        case detail::HeadStatus::kNotProtocol:
            return "the answer is not an HTTP response";
        case detail::HeadStatus::kMalformed:
            return "malformed response head";
        case detail::HeadStatus::kTooLarge:
            return "response head too large";
    }
    return "receive failed";
}

/// One connection: connect, send, read the final response head. The body is read separately so
/// that `HttpStream` can keep the connection.
struct Attempt {
    HttpOutcome outcome{HttpOutcome::kConnectFailed};
    ConnectOutcome connect{ConnectOutcome::kError};
    /// Bounds connect, request and the complete response of this connection.
    Clock::time_point deadline;
    Socket socket;
    ResponseHead head;
    /// Bytes received after the head.
    std::string buffer;
    std::string error;
};

Attempt startAttempt(Ipv4 host, std::uint16_t port, const HttpRequestOptions& options,
                     const std::string& authorization, Clock::time_point deadline) {
    Attempt attempt;
    attempt.deadline = deadline;
    ConnectResult connected = tcpConnect(host, port, remainingMs(deadline));
    attempt.connect = connected.outcome;
    if (connected.outcome != ConnectOutcome::kConnected) {
        attempt.outcome = HttpOutcome::kConnectFailed;
        attempt.error = "connect to " + toString(host) + ":" + std::to_string(port) + " " +
                        toString(connected.outcome);
        if (connected.error_number != 0) {
            attempt.error += " (" + errnoText(connected.error_number) + ")";
        }
        return attempt;
    }
    attempt.socket = std::move(connected.socket);

    std::string send_error;
    if (!sendAll(attempt.socket, buildRequest(host, port, options, authorization),
                 remainingMs(deadline), send_error)) {
        attempt.outcome = Clock::now() >= deadline ? HttpOutcome::kTimeout
                                                   : HttpOutcome::kProtocolError;
        attempt.error = send_error;
        return attempt;
    }

    for (;;) {
        const detail::HeadStatus status = detail::readResponseHead(
            attempt.socket, attempt.buffer, "HTTP", remainingMs(deadline), attempt.head);
        if (status != detail::HeadStatus::kOk) {
            attempt.outcome = status == detail::HeadStatus::kTimeout ? HttpOutcome::kTimeout
                                                                     : HttpOutcome::kProtocolError;
            attempt.error = describeHeadFailure(status);
            return attempt;
        }
        // Interim answers (100 Continue, 102 Processing) precede the real one.
        if (attempt.head.status >= 100 && attempt.head.status < 200 && attempt.head.status != 101) {
            continue;
        }
        attempt.outcome = HttpOutcome::kResponse;
        return attempt;
    }
}

HttpOutcome readBody(Attempt& attempt, const HttpRequestOptions& options, std::string& body,
                     std::string& error) {
    const Clock::time_point deadline = attempt.deadline;
    body.clear();
    if (hasNoBody(options.method, attempt.head.status)) {
        return HttpOutcome::kResponse;
    }
    const std::string too_large =
        "response body larger than " + std::to_string(options.max_body_bytes) + " bytes";

    if (isChunked(attempt.head)) {
        detail::ChunkedDecoder decoder;
        for (;;) {
            const auto status = decoder.feed(attempt.buffer, body);
            if (status == detail::ChunkedDecoder::Status::kError) {
                error = "malformed chunked body";
                return HttpOutcome::kProtocolError;
            }
            if (body.size() > options.max_body_bytes) {
                error = too_large;
                return HttpOutcome::kProtocolError;
            }
            if (status == detail::ChunkedDecoder::Status::kDone) {
                return HttpOutcome::kResponse;
            }
            const RecvStatus received =
                recvSome(attempt.socket, attempt.buffer, kReadChunk, remainingMs(deadline));
            if (received == RecvStatus::kTimeout) {
                error = "chunked body incomplete at the timeout";
                return HttpOutcome::kTimeout;
            }
            if (received != RecvStatus::kData) {
                error = "connection " + toString(received) + " inside a chunked body";
                return HttpOutcome::kProtocolError;
            }
        }
    }

    const auto length_header = findHeader(attempt.head.headers, "Content-Length");
    if (length_header) {
        std::size_t length = 0;
        if (!parseContentLength(*length_header, length)) {
            error = "invalid Content-Length";
            return HttpOutcome::kProtocolError;
        }
        if (length > options.max_body_bytes) {
            error = too_large;
            return HttpOutcome::kProtocolError;
        }
        while (attempt.buffer.size() < length) {
            const RecvStatus received =
                recvSome(attempt.socket, attempt.buffer, length - attempt.buffer.size(),
                         remainingMs(deadline));
            if (received == RecvStatus::kTimeout) {
                error = "body incomplete at the timeout (" + std::to_string(attempt.buffer.size()) +
                        " of " + std::to_string(length) + " bytes)";
                return HttpOutcome::kTimeout;
            }
            if (received != RecvStatus::kData) {
                error = "connection " + toString(received) + " after " +
                        std::to_string(attempt.buffer.size()) + " of " + std::to_string(length) +
                        " body bytes";
                return HttpOutcome::kProtocolError;
            }
        }
        body = attempt.buffer.substr(0, length);
        attempt.buffer.erase(0, length);
        return HttpOutcome::kResponse;
    }

    // No framing: the body ends when the server closes the connection.
    body.swap(attempt.buffer);
    for (;;) {
        if (body.size() > options.max_body_bytes) {
            error = too_large;
            return HttpOutcome::kProtocolError;
        }
        const RecvStatus received =
            recvSome(attempt.socket, body, kReadChunk, remainingMs(deadline));
        if (received == RecvStatus::kData) {
            continue;
        }
        if (received == RecvStatus::kTimeout) {
            error = "body did not end before the timeout";
            return HttpOutcome::kTimeout;
        }
        // Closed, or reset right after the last byte (common with "Connection: close" servers).
        return HttpOutcome::kResponse;
    }
}

/// The first attempt and, when it is a 401 with a usable challenge and credentials are set,
/// exactly one authenticated attempt on a new connection. Each connection gets the whole timeout
/// for connect, request and complete response, so a slow 401 cannot leave the authenticated
/// attempt without time to finish.
Attempt startWithLogin(Ipv4 host, std::uint16_t port, const HttpRequestOptions& options,
                       HttpResult& result) {
    Attempt attempt = startAttempt(host, port, options, {}, deadlineAfter(options.timeout_ms));
    if (attempt.outcome != HttpOutcome::kResponse || attempt.head.status != 401 ||
        options.credentials == nullptr || options.credentials->empty()) {
        return attempt;
    }
    const std::vector<AuthChallenge> challenges =
        parseAuthChallenges(findHeaders(attempt.head.headers, "WWW-Authenticate"));
    const AuthChallenge* challenge = preferredChallenge(challenges);
    if (challenge == nullptr) {
        return attempt;  // Nothing we can answer: the 401 is the result, credentials unsent.
    }
    const std::string authorization = buildAuthorization(
        *challenge, *options.credentials, options.method, normalizedPath(options.path), 1, {});
    attempt.socket.close();  // The 401 body is irrelevant.
    result.auth_attempted = true;
    return startAttempt(host, port, options, authorization, deadlineAfter(options.timeout_ms));
}

void copyHead(const ResponseHead& head, HttpResponse& response) {
    response.status = head.status;
    response.reason = head.reason;
    response.headers = head.headers;
}

}  // namespace

std::optional<std::string> findHeader(const HeaderList& headers, const std::string& name) {
    for (const auto& header : headers) {
        if (equalsIgnoreCase(header.first, name)) {
            return header.second;
        }
    }
    return std::nullopt;
}

std::vector<std::string> findHeaders(const HeaderList& headers, const std::string& name) {
    std::vector<std::string> values;
    for (const auto& header : headers) {
        if (equalsIgnoreCase(header.first, name)) {
            values.push_back(header.second);
        }
    }
    return values;
}

bool parseResponseHead(const std::string& head, ResponseHead& out) {
    std::vector<std::string> lines;
    std::size_t begin = 0;
    while (begin <= head.size()) {
        std::size_t end = head.find('\n', begin);
        if (end == std::string::npos) {
            end = head.size();
        }
        std::string line = head.substr(begin, end - begin);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(std::move(line));
        begin = end + 1;
    }
    while (!lines.empty() && lines.back().empty()) {
        lines.pop_back();
    }
    if (lines.empty()) {
        return false;
    }

    ResponseHead parsed;
    const std::string& status_line = lines.front();
    const std::size_t first_space = status_line.find(' ');
    if (first_space == std::string::npos) {
        return false;
    }
    parsed.protocol = status_line.substr(0, first_space);
    if (!isProtocolVersion(parsed.protocol)) {
        return false;
    }
    std::size_t code_begin = status_line.find_first_not_of(' ', first_space);
    if (code_begin == std::string::npos || code_begin + 3 > status_line.size()) {
        return false;
    }
    int code = 0;
    for (std::size_t i = code_begin; i < code_begin + 3; ++i) {
        if (std::isdigit(static_cast<unsigned char>(status_line[i])) == 0) {
            return false;
        }
        code = code * 10 + (status_line[i] - '0');
    }
    if (code_begin + 3 < status_line.size() && status_line[code_begin + 3] != ' ') {
        return false;  // "2000" or "200OK".
    }
    parsed.status = code;
    parsed.reason = code_begin + 3 < status_line.size() ? trim(status_line.substr(code_begin + 4))
                                                        : std::string();

    for (std::size_t i = 1; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        if (line.empty()) {
            return false;  // A blank line ends the head; the caller must not include one.
        }
        if (line.front() == ' ' || line.front() == '\t') {
            // Obsolete line folding continues the previous value.
            if (parsed.headers.empty()) {
                return false;
            }
            const std::string continuation = trim(line);
            if (!continuation.empty()) {
                std::string& value = parsed.headers.back().second;
                value += value.empty() ? continuation : " " + continuation;
            }
            continue;
        }
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos || colon == 0) {
            return false;
        }
        const std::string name = line.substr(0, colon);
        if (!std::all_of(name.begin(), name.end(), isTokenChar)) {
            return false;
        }
        parsed.headers.emplace_back(name, trim(line.substr(colon + 1)));
    }
    out = std::move(parsed);
    return true;
}

namespace detail {

ChunkedDecoder::Status ChunkedDecoder::feed(std::string& input, std::string& out) {
    std::size_t position = 0;
    Status status = Status::kNeedMore;
    bool progress = true;
    while (progress && status == Status::kNeedMore) {
        progress = false;
        switch (state_) {
            case State::kSize:
            case State::kTrailer: {
                const std::size_t end = input.find('\n', position);
                if (end == std::string::npos) {
                    if (input.size() - position > kMaxChunkLine) {
                        status = Status::kError;
                    }
                    break;
                }
                std::string line = input.substr(position, end - position);
                position = end + 1;
                progress = true;
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                if (state_ == State::kTrailer) {
                    // Trailer fields are ignored; an empty line ends the message.
                    if (line.empty()) {
                        state_ = State::kDone;
                        status = Status::kDone;
                    }
                    break;
                }
                const std::string size_text = trim(line.substr(0, line.find(';')));
                if (size_text.empty() || size_text.size() > 15) {
                    status = Status::kError;
                    break;
                }
                std::uint64_t size = 0;
                for (const char ch : size_text) {
                    if (std::isxdigit(static_cast<unsigned char>(ch)) == 0) {
                        status = Status::kError;
                        break;
                    }
                    const int digit = std::isdigit(static_cast<unsigned char>(ch)) != 0
                                          ? ch - '0'
                                          : std::tolower(static_cast<unsigned char>(ch)) - 'a' + 10;
                    size = size * 16 + static_cast<std::uint64_t>(digit);
                }
                if (status == Status::kError) {
                    break;
                }
                remaining_ = size;
                state_ = size == 0 ? State::kTrailer : State::kData;
                break;
            }
            case State::kData: {
                const std::size_t available = input.size() - position;
                const std::size_t take =
                    static_cast<std::size_t>(std::min<std::uint64_t>(remaining_, available));
                if (take == 0) {
                    break;
                }
                out.append(input, position, take);
                position += take;
                remaining_ -= take;
                progress = true;
                if (remaining_ == 0) {
                    state_ = State::kDataEnd;
                }
                break;
            }
            case State::kDataEnd: {
                if (position >= input.size()) {
                    break;
                }
                if (input[position] == '\n') {
                    position += 1;
                } else if (input[position] == '\r') {
                    if (position + 1 >= input.size()) {
                        break;
                    }
                    if (input[position + 1] != '\n') {
                        status = Status::kError;
                        break;
                    }
                    position += 2;
                } else {
                    status = Status::kError;
                    break;
                }
                state_ = State::kSize;
                progress = true;
                break;
            }
            case State::kDone:
                status = Status::kDone;
                break;
        }
    }
    input.erase(0, position);
    return status;
}

HeadStatus readResponseHead(const Socket& socket, std::string& buffer, const std::string& protocol,
                            int timeout_ms, ResponseHead& head) {
    const auto deadline = deadlineAfter(timeout_ms);
    const std::string prefix = protocol + "/";
    for (;;) {
        // Blank lines before a status line are tolerated (RFC 7230 section 3.5).
        const std::size_t start = buffer.find_first_not_of("\r\n");
        buffer.erase(0, start == std::string::npos ? buffer.size() : start);
        const std::size_t compared = std::min(buffer.size(), prefix.size());
        if (buffer.compare(0, compared, prefix, 0, compared) != 0) {
            return HeadStatus::kNotProtocol;
        }
        std::size_t end = buffer.find("\r\n\r\n");
        std::size_t separator = 4;
        const std::size_t bare = buffer.find("\n\n");
        if (bare != std::string::npos && (end == std::string::npos || bare < end)) {
            end = bare;
            separator = 2;
        }
        if (end != std::string::npos) {
            const std::string text = buffer.substr(0, end);
            buffer.erase(0, end + separator);
            if (!parseResponseHead(text, head)) {
                return HeadStatus::kMalformed;
            }
            return head.protocol.compare(0, prefix.size(), prefix) == 0 ? HeadStatus::kOk
                                                                       : HeadStatus::kNotProtocol;
        }
        if (buffer.size() > kMaxHeadBytes) {
            return HeadStatus::kTooLarge;
        }
        switch (recvSome(socket, buffer, 16U * 1024U, remainingMs(deadline))) {
            case RecvStatus::kData:
                break;
            case RecvStatus::kTimeout:
                return HeadStatus::kTimeout;
            case RecvStatus::kClosed:
                return HeadStatus::kClosed;
            case RecvStatus::kReset:
                return HeadStatus::kReset;
            case RecvStatus::kError:
                return HeadStatus::kError;
        }
    }
}

}  // namespace detail

std::string toString(HttpOutcome outcome) {
    switch (outcome) {
        case HttpOutcome::kResponse:
            return "response";
        case HttpOutcome::kConnectFailed:
            return "connect_failed";
        case HttpOutcome::kTimeout:
            return "timeout";
        case HttpOutcome::kProtocolError:
            return "protocol_error";
    }
    return "protocol_error";
}

HttpResult httpRequest(Ipv4 host, std::uint16_t port, const HttpRequestOptions& options) {
    HttpResult result;
    Attempt attempt = startWithLogin(host, port, options, result);
    result.outcome = attempt.outcome;
    result.connect = attempt.connect;
    result.error = attempt.error;
    if (attempt.outcome != HttpOutcome::kResponse) {
        return result;
    }
    copyHead(attempt.head, result.response);
    result.outcome = readBody(attempt, options, result.response.body, result.error);
    return result;
}

HttpStream::~HttpStream() {
    close();
}

void HttpStream::close() {
    socket_.close();
    pending_.clear();
}

HttpResult HttpStream::open(Ipv4 host, std::uint16_t port, const HttpRequestOptions& options) {
    close();
    head_ = ResponseHead();
    chunked_ = false;
    decoder_.reset();
    length_remaining_.reset();
    finished_ = false;

    HttpResult result;
    Attempt attempt = startWithLogin(host, port, options, result);
    result.outcome = attempt.outcome;
    result.connect = attempt.connect;
    result.error = attempt.error;
    if (attempt.outcome != HttpOutcome::kResponse) {
        return result;
    }
    copyHead(attempt.head, result.response);
    head_ = attempt.head;

    if (attempt.head.status < 200 || attempt.head.status >= 300) {
        // The error body (an ISAPI ResponseStatus) is informative only; a failure to read it
        // does not hide the status.
        std::string ignored;
        readBody(attempt, options, result.response.body, ignored);
        return result;
    }

    socket_ = std::move(attempt.socket);
    pending_ = std::move(attempt.buffer);
    if (hasNoBody(options.method, attempt.head.status)) {
        finished_ = true;
    } else if (isChunked(attempt.head)) {
        chunked_ = true;
    } else if (const auto length = findHeader(attempt.head.headers, "Content-Length")) {
        std::size_t parsed = 0;
        if (!parseContentLength(*length, parsed)) {
            close();
            result.outcome = HttpOutcome::kProtocolError;
            result.error = "invalid Content-Length";
            return result;
        }
        length_remaining_ = parsed;
        finished_ = parsed == 0;
    }
    return result;
}

bool HttpStream::deliver(std::string& out) {
    if (chunked_) {
        const auto status = decoder_.feed(pending_, out);
        if (status == detail::ChunkedDecoder::Status::kError) {
            return false;
        }
        finished_ = status == detail::ChunkedDecoder::Status::kDone;
        return true;
    }
    if (length_remaining_) {
        const std::size_t take = std::min(*length_remaining_, pending_.size());
        out.append(pending_, 0, take);
        pending_.erase(0, take);
        *length_remaining_ -= take;
        finished_ = *length_remaining_ == 0;
        return true;
    }
    out += pending_;
    pending_.clear();
    return true;
}

RecvStatus HttpStream::read(std::string& out, int timeout_ms) {
    const auto deadline = deadlineAfter(timeout_ms);
    for (;;) {
        if (!socket_.valid()) {
            return RecvStatus::kClosed;
        }
        const std::size_t before = out.size();
        if (!deliver(out)) {
            close();
            return RecvStatus::kError;
        }
        if (finished_) {
            // The message is complete; hand out what remains first, then report the end.
            close();
            return out.size() > before ? RecvStatus::kData : RecvStatus::kClosed;
        }
        if (out.size() > before) {
            return RecvStatus::kData;
        }
        const RecvStatus status = recvSome(socket_, pending_, kReadChunk, remainingMs(deadline));
        if (status == RecvStatus::kData) {
            continue;
        }
        if (status == RecvStatus::kTimeout) {
            return RecvStatus::kTimeout;
        }
        close();
        return status;
    }
}

}  // namespace anpr::net
