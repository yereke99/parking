#include <atomic>
#include <cctype>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "anpr/net/crypto.hpp"
#include "anpr/net/http_auth.hpp"
#include "anpr/net/http_client.hpp"
#include "anpr/net/rtsp_client.hpp"
#include "test_framework.hpp"

using anpr::net::AuthChallenge;
using anpr::net::Credentials;
using anpr::net::HttpOutcome;
using anpr::net::Ipv4;
using anpr::net::RecvStatus;
using anpr::net::RtspProbeStatus;

namespace {

const Ipv4 kLoopback{0x7F000001U};

#ifdef MSG_NOSIGNAL
constexpr int kNoSignal = MSG_NOSIGNAL;
#else
constexpr int kNoSignal = 0;
#endif

using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

void sleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

/// A listening TCP socket on 127.0.0.1 with an ephemeral port.
class Listener {
public:
    Listener() {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        const int one = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        ::listen(fd_, 16);
        socklen_t length = sizeof(address);
        ::getsockname(fd_, reinterpret_cast<sockaddr*>(&address), &length);
        port_ = ntohs(address.sin_port);
    }
    ~Listener() { ::close(fd_); }
    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;

    [[nodiscard]] std::uint16_t port() const { return port_; }

    int accept(int timeout_ms) const {
        pollfd entry{};
        entry.fd = fd_;
        entry.events = POLLIN;
        if (::poll(&entry, 1, timeout_ms) <= 0) {
            return -1;
        }
        const int fd = ::accept(fd_, nullptr, nullptr);
#ifdef SO_NOSIGPIPE
        const int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
        return fd;
    }

private:
    int fd_{-1};
    std::uint16_t port_{0};
};

std::uint16_t closedPort() {
    const Listener listener;
    return listener.port();
}

void writeAll(int fd, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t written = ::send(fd, data.data() + sent, data.size() - sent, kNoSignal);
        if (written <= 0) {
            return;
        }
        sent += static_cast<std::size_t>(written);
    }
}

/// Writes `data` and the FIN in one segment, so the client cannot see the data without also
/// seeing the end of the stream (a camera that hangs up right after its answer).
void writeAndHangUp(int fd, const std::string& data) {
    const int one = 1;
#if defined(TCP_CORK)
    ::setsockopt(fd, IPPROTO_TCP, TCP_CORK, &one, sizeof(one));
#elif defined(TCP_NOPUSH)
    ::setsockopt(fd, IPPROTO_TCP, TCP_NOPUSH, &one, sizeof(one));
#endif
    writeAll(fd, data);
    ::shutdown(fd, SHUT_WR);
}

std::string toLower(std::string text) {
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

/// Value of the first `name` header of an HTTP/RTSP message, empty when absent.
std::string headerValue(const std::string& message, const std::string& name) {
    std::size_t begin = message.find("\r\n");
    while (begin != std::string::npos) {
        begin += 2;
        const std::size_t end = message.find("\r\n", begin);
        if (end == std::string::npos || end == begin) {
            return {};
        }
        const std::string line = message.substr(begin, end - begin);
        const std::size_t colon = line.find(':');
        if (colon != std::string::npos && toLower(line.substr(0, colon)) == toLower(name)) {
            const std::size_t value = line.find_first_not_of(' ', colon + 1);
            return value == std::string::npos ? std::string() : line.substr(value);
        }
        begin = end;
    }
    return {};
}

std::size_t countOccurrences(const std::string& text, const std::string& needle) {
    std::size_t count = 0;
    for (std::size_t at = text.find(needle); at != std::string::npos;
         at = text.find(needle, at + needle.size())) {
        ++count;
    }
    return count;
}

std::string requestLine(const std::string& message) {
    return message.substr(0, message.find("\r\n"));
}

/// Reads one request (head plus Content-Length body). `carry` keeps the bytes of a following
/// request. Empty when the client closed or nothing arrived in time.
std::string readRequest(int fd, std::string& carry, int timeout_ms = 2000) {
    const auto start = Clock::now();
    for (;;) {
        const std::size_t end = carry.find("\r\n\r\n");
        if (end != std::string::npos) {
            const std::string length_text = headerValue(carry.substr(0, end + 4), "Content-Length");
            const std::size_t length = length_text.empty() ? 0 : std::stoul(length_text);
            if (carry.size() >= end + 4 + length) {
                std::string request = carry.substr(0, end + 4 + length);
                carry.erase(0, end + 4 + length);
                return request;
            }
        }
        const int left = timeout_ms - static_cast<int>(msSince(start));
        pollfd entry{};
        entry.fd = fd;
        entry.events = POLLIN;
        if (left <= 0 || ::poll(&entry, 1, left) <= 0) {
            return {};
        }
        char buffer[4096];
        const ssize_t received = ::recv(fd, buffer, sizeof(buffer), 0);
        if (received <= 0) {
            return {};
        }
        carry.append(buffer, static_cast<std::size_t>(received));
    }
}

/// Holds the connection open without answering until the client gives up and closes it.
void waitForClose(int fd, int timeout_ms) {
    const auto start = Clock::now();
    char buffer[4096];
    while (msSince(start) < timeout_ms) {
        pollfd entry{};
        entry.fd = fd;
        entry.events = POLLIN;
        if (::poll(&entry, 1, 50) > 0 && ::recv(fd, buffer, sizeof(buffer), 0) <= 0) {
            return;
        }
    }
}

/// A single-threaded scripted server on 127.0.0.1: accepts connections one after another and
/// runs `handler` for each.
class FakeServer {
public:
    using Handler = std::function<void(FakeServer& server, int fd, int index)>;

    explicit FakeServer(Handler handler)
        : handler_(std::move(handler)), thread_([this]() { run(); }) {}
    ~FakeServer() { stop(); }
    FakeServer(const FakeServer&) = delete;
    FakeServer& operator=(const FakeServer&) = delete;

    [[nodiscard]] std::uint16_t port() const { return listener_.port(); }

    /// Stops accepting; returns how many connections were served.
    int stop() {
        stop_ = true;
        if (thread_.joinable()) {
            thread_.join();
        }
        return connections_;
    }

    void record(const std::string& request) {
        const std::lock_guard<std::mutex> lock(mutex_);
        requests_.push_back(request);
    }

    std::vector<std::string> requests() {
        const std::lock_guard<std::mutex> lock(mutex_);
        return requests_;
    }

private:
    void run() {
        while (!stop_) {
            const int fd = listener_.accept(20);
            if (fd < 0) {
                continue;
            }
            const int index = connections_++;
            handler_(*this, fd, index);
            ::close(fd);
        }
    }

    Listener listener_;
    Handler handler_;
    std::atomic<bool> stop_{false};
    std::atomic<int> connections_{0};
    std::mutex mutex_;
    std::vector<std::string> requests_;
    std::thread thread_;
};

/// A parameter of an Authorization header, quoted or bare.
std::string authParam(const std::string& header, const std::string& key) {
    std::size_t at = 0;
    while ((at = header.find(key + "=", at)) != std::string::npos) {
        const bool boundary = at == 0 || header[at - 1] == ' ' || header[at - 1] == ',';
        at += key.size() + 1;
        if (!boundary) {
            continue;
        }
        if (at < header.size() && header[at] == '"') {
            std::string value;
            for (std::size_t i = at + 1; i < header.size() && header[i] != '"'; ++i) {
                if (header[i] == '\\' && i + 1 < header.size()) {
                    ++i;
                }
                value.push_back(header[i]);
            }
            return value;
        }
        const std::size_t end = header.find(',', at);
        return header.substr(at, end == std::string::npos ? std::string::npos : end - at);
    }
    return {};
}

/// Checks a digest Authorization the way a camera does, from the RFC 2617 formulas.
bool digestValid(const std::string& request, const std::string& method, const std::string& uri,
                 const Credentials& expected, const std::string& nonce) {
    const std::string header = headerValue(request, "Authorization");
    if (header.compare(0, 7, "Digest ") != 0 ||
        authParam(header, "username") != expected.username || authParam(header, "uri") != uri ||
        authParam(header, "nonce") != nonce) {
        return false;
    }
    const std::string ha1 = anpr::net::md5Hex(expected.username + ":" + authParam(header, "realm") +
                                              ":" + expected.password);
    const std::string ha2 = anpr::net::md5Hex(method + ":" + uri);
    const std::string qop = authParam(header, "qop");
    const std::string response =
        qop.empty() ? anpr::net::md5Hex(ha1 + ":" + nonce + ":" + ha2)
                    : anpr::net::md5Hex(ha1 + ":" + nonce + ":" + authParam(header, "nc") + ":" +
                                        authParam(header, "cnonce") + ":" + qop + ":" + ha2);
    return authParam(header, "response") == response;
}

std::string rtspResponse(const std::string& request, const std::string& status_line,
                         const std::string& headers, const std::string& body = {}) {
    std::string response = "RTSP/1.0 " + status_line + "\r\nCSeq: " +
                           headerValue(request, "CSeq") + "\r\n" + headers;
    if (!body.empty()) {
        response += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    return response + "\r\n" + body;
}

const Credentials kCameraLogin{"admin", "Kz#Secret:9"};

const char* const kSdp =
    "v=0\r\n"
    "o=- 1109162014219182 1109162014219192 IN IP4 127.0.0.1\r\n"
    "s=Media Presentation\r\n"
    "t=0 0\r\n"
    "m=video 0 RTP/AVP 96\r\n"
    "a=rtpmap:96 H264/90000\r\n"
    "a=control:rtsp://127.0.0.1/Streaming/Channels/101/trackID=1\r\n";

std::string streamUrl(std::uint16_t port) {
    return "rtsp://127.0.0.1:" + std::to_string(port) + "/Streaming/Channels/101";
}

anpr::net::RtspProbeOptions probeOptions(std::uint16_t port) {
    anpr::net::RtspProbeOptions options;
    options.host = kLoopback;
    options.port = port;
    options.timeout_ms = 2000;
    return options;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// WWW-Authenticate parsing and Authorization building.

TEST("auth challenges parse Hikvision's RFC 2069 digest") {
    const auto challenges = anpr::net::parseAuthChallenges(
        {"Digest realm=\"IP Camera(G6822)\", nonce=\"3d2a9ee3ec2c\", stale=\"FALSE\""});
    CHECK_EQ(challenges.size(), std::size_t{1});
    CHECK_EQ(challenges[0].scheme, std::string("digest"));
    CHECK_EQ(challenges[0].realm, std::string("IP Camera(G6822)"));
    CHECK_EQ(challenges[0].nonce, std::string("3d2a9ee3ec2c"));
    CHECK(challenges[0].qop.empty());
    CHECK(challenges[0].algorithm.empty());
    CHECK(!challenges[0].stale);
}

TEST("auth challenges split several challenges and honour quoting") {
    auto challenges = anpr::net::parseAuthChallenges(
        {"Digest realm=\"a, b\", qop=\"auth,auth-int\", nonce=\"n\\\"q\", algorithm=MD5-sess, "
         "opaque=\"x=y\", Basic realm=\"basic realm\""});
    CHECK_EQ(challenges.size(), std::size_t{2});
    CHECK_EQ(challenges[0].scheme, std::string("digest"));
    CHECK_EQ(challenges[0].realm, std::string("a, b"));
    CHECK_EQ(challenges[0].qop, std::string("auth,auth-int"));
    CHECK_EQ(challenges[0].nonce, std::string("n\"q"));
    CHECK_EQ(challenges[0].algorithm, std::string("MD5-sess"));
    CHECK_EQ(challenges[0].opaque, std::string("x=y"));
    CHECK_EQ(challenges[1].scheme, std::string("basic"));
    CHECK_EQ(challenges[1].realm, std::string("basic realm"));

    challenges = anpr::net::parseAuthChallenges({"DIGEST REALM=\"x\", NONCE=y , STALE=TRUE"});
    CHECK_EQ(challenges.size(), std::size_t{1});
    CHECK_EQ(challenges[0].scheme, std::string("digest"));
    CHECK_EQ(challenges[0].realm, std::string("x"));
    CHECK_EQ(challenges[0].nonce, std::string("y"));
    CHECK(challenges[0].stale);

    challenges = anpr::net::parseAuthChallenges(
        {"Negotiate YIIGhgYGKwYBBQUCoIIGejCC==", "Basic realm=\"r\"", "",
         "Negotiate, Digest realm=\"d\", nonce=\"n\""});
    CHECK_EQ(challenges.size(), std::size_t{4});
    CHECK_EQ(challenges[0].scheme, std::string("negotiate"));
    CHECK_EQ(challenges[1].scheme, std::string("basic"));
    CHECK_EQ(challenges[1].realm, std::string("r"));
    CHECK_EQ(challenges[2].scheme, std::string("negotiate"));
    CHECK_EQ(challenges[3].scheme, std::string("digest"));
    CHECK_EQ(challenges[3].nonce, std::string("n"));
    CHECK(anpr::net::parseAuthChallenges({}).empty());
    CHECK(anpr::net::parseAuthChallenges({"  ,  "}).empty());
}

TEST("preferred challenge is MD5 digest, then Basic, never unsupported digests") {
    AuthChallenge basic;
    basic.scheme = "basic";
    AuthChallenge digest;
    digest.scheme = "digest";
    digest.nonce = "n";
    AuthChallenge sha256 = digest;
    sha256.algorithm = "SHA-256";
    AuthChallenge auth_int = digest;
    auth_int.qop = "auth-int";
    AuthChallenge sess = digest;
    sess.algorithm = "md5-sess";
    sess.qop = "auth";
    AuthChallenge sess_without_qop = digest;
    sess_without_qop.algorithm = "MD5-sess";
    AuthChallenge no_nonce = digest;
    no_nonce.nonce.clear();

    std::vector<AuthChallenge> list = {basic, digest};
    CHECK(anpr::net::preferredChallenge(list) == &list[1]);
    list = {sha256, basic};
    CHECK(anpr::net::preferredChallenge(list) == &list[1]);
    list = {sha256, auth_int, sess_without_qop, no_nonce};
    CHECK(anpr::net::preferredChallenge(list) == nullptr);
    list = {sha256, sess};
    CHECK(anpr::net::preferredChallenge(list) == &list[1]);
    list.clear();
    CHECK(anpr::net::preferredChallenge(list) == nullptr);
}

TEST("digest authorization reproduces the RFC 2617 section 3.5 example") {
    AuthChallenge challenge;
    challenge.scheme = "digest";
    challenge.realm = "testrealm@host.com";
    challenge.qop = "auth,auth-int";
    challenge.nonce = "dcd98b7102dd2f0e8b11d0f600bfb0c093";
    challenge.opaque = "5ccc069c403ebaf9f0171e9517f40e41";
    const std::string header =
        anpr::net::buildAuthorization(challenge, Credentials{"Mufasa", "Circle Of Life"}, "GET",
                                      "/dir/index.html", 1, "0a4f113b");
    CHECK_EQ(header.compare(0, 7, "Digest "), 0);
    CHECK_EQ(authParam(header, "response"), std::string("6629fae49393a05397450978507c4ef1"));
    CHECK_EQ(authParam(header, "username"), std::string("Mufasa"));
    CHECK_EQ(authParam(header, "realm"), std::string("testrealm@host.com"));
    CHECK_EQ(authParam(header, "nonce"), std::string("dcd98b7102dd2f0e8b11d0f600bfb0c093"));
    CHECK_EQ(authParam(header, "uri"), std::string("/dir/index.html"));
    CHECK_EQ(authParam(header, "opaque"), std::string("5ccc069c403ebaf9f0171e9517f40e41"));
    CHECK(header.find("qop=auth,") != std::string::npos);
    CHECK(header.find("nc=00000001") != std::string::npos);
    CHECK_EQ(authParam(header, "cnonce"), std::string("0a4f113b"));
    CHECK(header.find("algorithm") == std::string::npos);
    CHECK(header.find("Circle Of Life") == std::string::npos);
}

TEST("digest authorization without qop follows RFC 2069") {
    AuthChallenge challenge;
    challenge.scheme = "digest";
    challenge.realm = "IP Camera(C1234)";
    challenge.nonce = "a5b1f7c2e8d94b03";
    const std::string uri = "rtsp://192.168.1.64:554/Streaming/Channels/101";
    const Credentials login{"admin", "pa\"ss"};
    const std::string header =
        anpr::net::buildAuthorization(challenge, login, "DESCRIBE", uri, 7, "unused");
    const std::string ha1 = anpr::net::md5Hex("admin:IP Camera(C1234):pa\"ss");
    const std::string ha2 = anpr::net::md5Hex("DESCRIBE:" + uri);
    CHECK_EQ(authParam(header, "response"),
             anpr::net::md5Hex(ha1 + ":a5b1f7c2e8d94b03:" + ha2));
    CHECK(header.find("qop") == std::string::npos);
    CHECK(header.find("nc=") == std::string::npos);
    CHECK(header.find("cnonce") == std::string::npos);
    CHECK(header.find("opaque") == std::string::npos);

    challenge.algorithm = "MD5";
    const std::string with_algorithm =
        anpr::net::buildAuthorization(challenge, login, "DESCRIBE", uri, 1, {});
    CHECK(with_algorithm.find("algorithm=MD5") != std::string::npos);
    CHECK_EQ(authParam(with_algorithm, "response"), authParam(header, "response"));

    // Quotes in the username are escaped inside the quoted-string.
    const std::string quoted = anpr::net::buildAuthorization(
        challenge, Credentials{"ad\"min", "x"}, "DESCRIBE", uri, 1, {});
    CHECK(quoted.find("username=\"ad\\\"min\"") != std::string::npos);
}

TEST("digest authorization supports MD5-sess and generates a cnonce") {
    AuthChallenge challenge;
    challenge.scheme = "digest";
    challenge.realm = "r";
    challenge.nonce = "n0";
    challenge.qop = "auth";
    challenge.algorithm = "MD5-sess";
    const std::string header =
        anpr::net::buildAuthorization(challenge, Credentials{"u", "p"}, "GET", "/a", 2, "c0");
    const std::string ha1 = anpr::net::md5Hex(anpr::net::md5Hex("u:r:p") + ":n0:c0");
    const std::string ha2 = anpr::net::md5Hex("GET:/a");
    CHECK_EQ(authParam(header, "response"),
             anpr::net::md5Hex(ha1 + ":n0:00000002:c0:auth:" + ha2));
    CHECK(header.find("algorithm=MD5-sess") != std::string::npos);
    CHECK(header.find("nc=00000002") != std::string::npos);

    challenge.algorithm.clear();
    const std::string generated =
        anpr::net::buildAuthorization(challenge, Credentials{"u", "p"}, "GET", "/a", 1, {});
    CHECK_EQ(authParam(generated, "cnonce").size(), std::size_t{16});
}

TEST("basic authorization and unknown schemes") {
    AuthChallenge basic;
    basic.scheme = "basic";
    CHECK_EQ(anpr::net::buildAuthorization(basic, Credentials{"Aladdin", "open sesame"}, "GET",
                                           "/", 1, {}),
             std::string("Basic QWxhZGRpbjpvcGVuIHNlc2FtZQ=="));
    AuthChallenge negotiate;
    negotiate.scheme = "negotiate";
    CHECK(anpr::net::buildAuthorization(negotiate, kCameraLogin, "GET", "/", 1, {}).empty());
}

// ---------------------------------------------------------------------------------------------
// Response heads and chunked decoding.

TEST("response heads parse HTTP and RTSP status lines and headers") {
    anpr::net::ResponseHead head;
    CHECK(anpr::net::parseResponseHead(
        "HTTP/1.1 200 OK\r\nContent-Type: text/xml\r\nX-Folded: first\r\n  second\r\n"
        "Empty:\r\nX-Folded: again",
        head));
    CHECK_EQ(head.protocol, std::string("HTTP/1.1"));
    CHECK_EQ(head.status, 200);
    CHECK_EQ(head.reason, std::string("OK"));
    CHECK_EQ(head.headers.size(), std::size_t{4});
    CHECK_EQ(anpr::net::findHeader(head.headers, "content-TYPE").value_or("-"),
             std::string("text/xml"));
    CHECK_EQ(anpr::net::findHeader(head.headers, "x-folded").value_or("-"),
             std::string("first second"));
    CHECK_EQ(anpr::net::findHeader(head.headers, "empty").value_or("-"), std::string());
    CHECK(!anpr::net::findHeader(head.headers, "missing").has_value());
    const auto folded = anpr::net::findHeaders(head.headers, "X-FOLDED");
    CHECK_EQ(folded.size(), std::size_t{2});
    CHECK_EQ(folded[1], std::string("again"));

    CHECK(anpr::net::parseResponseHead("RTSP/1.0 401 Unauthorized\nCSeq: 1\n", head));
    CHECK_EQ(head.protocol, std::string("RTSP/1.0"));
    CHECK_EQ(head.status, 401);
    CHECK_EQ(head.reason, std::string("Unauthorized"));
    CHECK_EQ(anpr::net::findHeader(head.headers, "cseq").value_or("-"), std::string("1"));

    CHECK(anpr::net::parseResponseHead("HTTP/1.0 204", head));
    CHECK_EQ(head.status, 204);
    CHECK(head.reason.empty());
    CHECK(head.headers.empty());
}

TEST("response heads reject malformed input and leave the output untouched") {
    anpr::net::ResponseHead head;
    head.status = 999;
    const std::vector<std::string> malformed = {
        "",
        "HTTP/1.1",
        "HTTP/1.1 20 OK",
        "HTTP/1.1 2000 OK",
        "HTTP/1.1 2x0 OK",
        "http/1.1 200 OK",
        "HTTP/ 200 OK",
        "SSH-2.0-OpenSSH_8.2p1",
        "HTTP/1.1 200 OK\r\nNo colon here",
        "HTTP/1.1 200 OK\r\n continuation first",
        "HTTP/1.1 200 OK\r\nBad Name: x",
        "HTTP/1.1 200 OK\r\n: no name",
        "HTTP/1.1 200 OK\r\n\r\nX-After-Blank: 1",
    };
    for (const std::string& text : malformed) {
        CHECK(!anpr::net::parseResponseHead(text, head));
        CHECK_EQ(head.status, 999);
    }
}

TEST("chunked decoder handles every split of a message") {
    const std::string message =
        "4\r\nWiki\r\n5;name=value\r\npedia\r\nE\r\n in\r\n\r\nchunks.\r\n0\r\nTrailer: x\r\n\r\n";
    const std::string expected = "Wikipedia in\r\n\r\nchunks.";
    for (std::size_t split = 0; split <= message.size(); ++split) {
        anpr::net::detail::ChunkedDecoder decoder;
        std::string input = message.substr(0, split);
        std::string out;
        const auto first = decoder.feed(input, out);
        CHECK(first != anpr::net::detail::ChunkedDecoder::Status::kError);
        input += message.substr(split) + "NEXT";
        CHECK_EQ(decoder.feed(input, out), anpr::net::detail::ChunkedDecoder::Status::kDone);
        CHECK_EQ(out, expected);
        CHECK_EQ(input, std::string("NEXT"));  // Bytes after the message are left alone.
        CHECK(decoder.done());
    }

    anpr::net::detail::ChunkedDecoder byte_by_byte;
    std::string pending;
    std::string out;
    auto status = anpr::net::detail::ChunkedDecoder::Status::kNeedMore;
    for (const char ch : std::string("3\nabc\n0\n\n")) {  // Bare LF line endings are tolerated.
        pending.push_back(ch);
        status = byte_by_byte.feed(pending, out);
        CHECK(status != anpr::net::detail::ChunkedDecoder::Status::kError);
    }
    CHECK_EQ(status, anpr::net::detail::ChunkedDecoder::Status::kDone);
    CHECK_EQ(out, std::string("abc"));
}

TEST("chunked decoder rejects malformed framing") {
    const std::vector<std::string> malformed = {
        "zz\r\n",
        "\r\n",
        "1234567890abcdef0\r\n",
        "3\r\nabcX\r\n",
        "3\r\nabc\rX",
        std::string(5000, '1'),
    };
    for (const std::string& text : malformed) {
        anpr::net::detail::ChunkedDecoder decoder;
        std::string input = text;
        std::string out;
        CHECK_EQ(decoder.feed(input, out), anpr::net::detail::ChunkedDecoder::Status::kError);
    }
}

// ---------------------------------------------------------------------------------------------
// httpRequest against scripted servers.

TEST("http request sends a well-formed request and reads a Content-Length body") {
    FakeServer server([](FakeServer& self, int fd, int) {
        std::string carry;
        self.record(readRequest(fd, carry));
        writeAll(fd, "HTTP/1.1 200 OK\r\nContent-Type: application/xml\r\nContent-Length: 11\r\n"
                     "\r\n<ok>hi</ok>");
    });
    anpr::net::HttpRequestOptions options;
    options.method = "PUT";
    options.path = "/ISAPI/System/deviceInfo";
    options.body = "<x/>";
    options.content_type = "application/xml";
    options.headers = {{"X-Test", "yes"}, {"Content-Length", "999"}, {"Connection", "keep-alive"}};
    options.timeout_ms = 2000;
    const auto result = anpr::net::httpRequest(kLoopback, server.port(), options);
    const int connections = server.stop();
    CHECK_EQ(result.outcome, HttpOutcome::kResponse);
    CHECK_EQ(result.connect, anpr::net::ConnectOutcome::kConnected);
    CHECK_EQ(result.response.status, 200);
    CHECK_EQ(result.response.reason, std::string("OK"));
    CHECK_EQ(result.response.body, std::string("<ok>hi</ok>"));
    CHECK_EQ(result.response.header("content-type").value_or("-"), std::string("application/xml"));
    CHECK(!result.auth_attempted);
    CHECK_EQ(connections, 1);

    const auto requests = server.requests();
    CHECK_EQ(requests.size(), std::size_t{1});
    const std::string& request = requests[0];
    CHECK_EQ(requestLine(request), std::string("PUT /ISAPI/System/deviceInfo HTTP/1.1"));
    CHECK_EQ(headerValue(request, "Host"), "127.0.0.1:" + std::to_string(server.port()));
    CHECK_EQ(headerValue(request, "Connection"), std::string("close"));
    CHECK_EQ(headerValue(request, "User-Agent"), std::string("kz-anpr"));
    CHECK_EQ(headerValue(request, "Content-Length"), std::string("4"));
    CHECK_EQ(headerValue(request, "Content-Type"), std::string("application/xml"));
    CHECK_EQ(headerValue(request, "X-Test"), std::string("yes"));
    CHECK_EQ(countOccurrences(request, "Content-Length:"), std::size_t{1});
    CHECK_EQ(countOccurrences(request, "Connection:"), std::size_t{1});
    CHECK(headerValue(request, "Authorization").empty());
    CHECK_EQ(request.substr(request.size() - 4), std::string("<x/>"));
}

TEST("http request decodes a chunked body that arrives in pieces") {
    FakeServer server([](FakeServer&, int fd, int) {
        std::string carry;
        readRequest(fd, carry);
        writeAll(fd, "HTTP/1.1 100 Continue\r\n\r\n"
                     "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5;e");
        sleepMs(20);
        writeAll(fd, "xt=1\r\nhel");
        sleepMs(20);
        writeAll(fd, "lo\r\n7\r\n, world\r\n0\r\nX-Trailer: 1\r\n\r\n");
        waitForClose(fd, 2000);  // Ends the message by framing, not by closing.
    });
    anpr::net::HttpRequestOptions options;
    options.path = "ISAPI/Streaming/channels/101";
    options.timeout_ms = 2000;
    const auto start = Clock::now();
    const auto result = anpr::net::httpRequest(kLoopback, server.port(), options);
    CHECK(msSince(start) < 1500.0);
    CHECK_EQ(result.outcome, HttpOutcome::kResponse);
    CHECK_EQ(result.response.status, 200);
    CHECK_EQ(result.response.body, std::string("hello, world"));
}

TEST("http request reads a body that ends when the server closes") {
    FakeServer server([](FakeServer&, int fd, int) {
        std::string carry;
        readRequest(fd, carry);
        writeAll(fd, "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\nstreamed");
        sleepMs(20);
        writeAll(fd, "-body");
    });
    anpr::net::HttpRequestOptions options;
    options.timeout_ms = 2000;
    const auto result = anpr::net::httpRequest(kLoopback, server.port(), options);
    CHECK_EQ(result.outcome, HttpOutcome::kResponse);
    CHECK_EQ(result.response.body, std::string("streamed-body"));
}

TEST("http request skips the body of HEAD and 204 responses") {
    FakeServer server([](FakeServer&, int fd, int index) {
        std::string carry;
        readRequest(fd, carry);
        writeAll(fd, index == 0 ? "HTTP/1.1 200 OK\r\nContent-Length: 1234\r\n\r\n"
                                : "HTTP/1.1 204 No Content\r\n\r\n");
        waitForClose(fd, 2000);
    });
    anpr::net::HttpRequestOptions options;
    options.method = "HEAD";
    options.timeout_ms = 2000;
    auto result = anpr::net::httpRequest(kLoopback, server.port(), options);
    CHECK_EQ(result.outcome, HttpOutcome::kResponse);
    CHECK_EQ(result.response.status, 200);
    CHECK(result.response.body.empty());
    options.method = "GET";
    const auto start = Clock::now();
    result = anpr::net::httpRequest(kLoopback, server.port(), options);
    CHECK_EQ(result.outcome, HttpOutcome::kResponse);
    CHECK_EQ(result.response.status, 204);
    CHECK(msSince(start) < 1500.0);
}

TEST("http digest login succeeds with exactly one authenticated attempt") {
    const std::string nonce = "4e6a6b5c3d2e1f00";
    FakeServer server([&nonce](FakeServer& self, int fd, int) {
        std::string carry;
        const std::string request = readRequest(fd, carry);
        self.record(request);
        if (headerValue(request, "Authorization").empty()) {
            writeAll(fd, "HTTP/1.1 401 Unauthorized\r\n"
                         "WWW-Authenticate: Basic realm=\"IP Camera(C1234)\"\r\n"
                         "WWW-Authenticate: Digest qop=\"auth\", realm=\"IP Camera(C1234)\", "
                         "nonce=\"" + nonce + "\", opaque=\"op1\", stale=\"FALSE\"\r\n"
                         "Content-Length: 12\r\n\r\nUnauthorized");
            return;
        }
        const bool valid = digestValid(request, "GET", "/ISAPI/System/deviceInfo", kCameraLogin,
                                       nonce);
        writeAll(fd, valid ? "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"
                           : "HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\n\r\n");
    });
    anpr::net::HttpRequestOptions options;
    options.path = "/ISAPI/System/deviceInfo";
    options.credentials = &kCameraLogin;
    options.timeout_ms = 2000;
    const auto result = anpr::net::httpRequest(kLoopback, server.port(), options);
    const int connections = server.stop();
    CHECK_EQ(result.outcome, HttpOutcome::kResponse);
    CHECK_EQ(result.response.status, 200);
    CHECK_EQ(result.response.body, std::string("ok"));
    CHECK(result.auth_attempted);
    CHECK_EQ(connections, 2);
    const auto requests = server.requests();
    CHECK_EQ(requests.size(), std::size_t{2});
    const std::string authorization = headerValue(requests[1], "Authorization");
    CHECK_EQ(authParam(authorization, "opaque"), std::string("op1"));
    CHECK(authorization.find("qop=auth") != std::string::npos);
    CHECK(authorization.find("nc=00000001") != std::string::npos);
    CHECK(authorization.find(kCameraLogin.password) == std::string::npos);
}

TEST("http wrong password costs exactly one authenticated attempt") {
    FakeServer server([](FakeServer& self, int fd, int) {
        std::string carry;
        self.record(readRequest(fd, carry));
        writeAll(fd, "HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Digest realm=\"r\", "
                     "nonce=\"n\"\r\nContent-Length: 0\r\n\r\n");
    });
    const Credentials wrong{"admin", "wrong"};
    anpr::net::HttpRequestOptions options;
    options.credentials = &wrong;
    options.timeout_ms = 2000;
    const auto result = anpr::net::httpRequest(kLoopback, server.port(), options);
    const int connections = server.stop();
    CHECK_EQ(result.outcome, HttpOutcome::kResponse);
    CHECK_EQ(result.response.status, 401);
    CHECK(result.auth_attempted);
    CHECK_EQ(connections, 2);
    const auto requests = server.requests();
    CHECK(headerValue(requests[0], "Authorization").empty());
    CHECK(!headerValue(requests[1], "Authorization").empty());
}

TEST("http 401 is returned unanswered without credentials or a usable challenge") {
    FakeServer server([](FakeServer&, int fd, int index) {
        std::string carry;
        readRequest(fd, carry);
        writeAll(fd, index == 0 ? "HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Digest "
                                  "realm=\"r\", nonce=\"n\"\r\nContent-Length: 0\r\n\r\n"
                                : "HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Digest "
                                  "realm=\"r\", nonce=\"n\", algorithm=SHA-256\r\n"
                                  "Content-Length: 0\r\n\r\n");
    });
    anpr::net::HttpRequestOptions options;
    options.timeout_ms = 2000;
    auto result = anpr::net::httpRequest(kLoopback, server.port(), options);
    CHECK_EQ(result.response.status, 401);
    CHECK(!result.auth_attempted);
    const Credentials empty_login;
    options.credentials = &empty_login;
    result = anpr::net::httpRequest(kLoopback, server.port(), options);
    CHECK_EQ(result.response.status, 401);
    CHECK(!result.auth_attempted);
    const int after_unanswered = server.stop();
    CHECK_EQ(after_unanswered, 2);
}

TEST("http unsupported challenge is not answered even with credentials") {
    FakeServer server([](FakeServer&, int fd, int) {
        std::string carry;
        readRequest(fd, carry);
        writeAll(fd, "HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Digest realm=\"r\", "
                     "nonce=\"n\", algorithm=SHA-256\r\nContent-Length: 0\r\n\r\n");
    });
    anpr::net::HttpRequestOptions options;
    options.credentials = &kCameraLogin;
    options.timeout_ms = 2000;
    const auto result = anpr::net::httpRequest(kLoopback, server.port(), options);
    CHECK_EQ(result.response.status, 401);
    CHECK(!result.auth_attempted);
    CHECK_EQ(server.stop(), 1);
}

TEST("http basic login is answered when it is the only scheme") {
    FakeServer server([](FakeServer& self, int fd, int) {
        std::string carry;
        const std::string request = readRequest(fd, carry);
        self.record(request);
        const std::string expected =
            "Basic " + anpr::net::base64Encode(kCameraLogin.username + ":" + kCameraLogin.password);
        if (headerValue(request, "Authorization") == expected) {
            writeAll(fd, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
        } else {
            writeAll(fd, "HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Basic realm=\"r\"\r\n"
                         "Content-Length: 0\r\n\r\n");
        }
    });
    anpr::net::HttpRequestOptions options;
    options.credentials = &kCameraLogin;
    options.timeout_ms = 2000;
    const auto result = anpr::net::httpRequest(kLoopback, server.port(), options);
    CHECK_EQ(result.response.status, 200);
    CHECK(result.auth_attempted);
    CHECK_EQ(server.stop(), 2);
}

TEST("http request reports refused connections, timeouts and non-HTTP answers") {
    anpr::net::HttpRequestOptions options;
    options.timeout_ms = 2000;
    auto result = anpr::net::httpRequest(kLoopback, closedPort(), options);
    CHECK_EQ(result.outcome, HttpOutcome::kConnectFailed);
    CHECK_EQ(result.connect, anpr::net::ConnectOutcome::kRefused);
    CHECK(!result.error.empty());

    {
        FakeServer silent([](FakeServer&, int fd, int) { waitForClose(fd, 3000); });
        options.timeout_ms = 300;
        const auto start = Clock::now();
        result = anpr::net::httpRequest(kLoopback, silent.port(), options);
        CHECK_EQ(result.outcome, HttpOutcome::kTimeout);
        CHECK_EQ(result.connect, anpr::net::ConnectOutcome::kConnected);
        CHECK(msSince(start) >= 250.0);
        CHECK(msSince(start) < 2000.0);
    }
    {
        FakeServer ssh([](FakeServer&, int fd, int) {
            writeAll(fd, "SSH-2.0-OpenSSH_8.2p1\r\n");
            waitForClose(fd, 3000);
        });
        options.timeout_ms = 3000;
        const auto start = Clock::now();
        result = anpr::net::httpRequest(kLoopback, ssh.port(), options);
        CHECK_EQ(result.outcome, HttpOutcome::kProtocolError);
        CHECK(msSince(start) < 1500.0);  // Recognised at once, not at the timeout.
    }
    {
        FakeServer hangup([](FakeServer&, int fd, int) {
            std::string carry;
            readRequest(fd, carry);
        });
        result = anpr::net::httpRequest(kLoopback, hangup.port(), options);
        CHECK_EQ(result.outcome, HttpOutcome::kProtocolError);
    }
    CHECK_EQ(anpr::net::toString(HttpOutcome::kConnectFailed), std::string("connect_failed"));
}

TEST("http request enforces body limits and complete bodies") {
    FakeServer server([](FakeServer&, int fd, int index) {
        std::string carry;
        readRequest(fd, carry);
        switch (index) {
            case 0:
                writeAll(fd, "HTTP/1.1 200 OK\r\nContent-Length: 1000\r\n\r\n" +
                                 std::string(1000, 'a'));
                break;
            case 1:
                writeAll(fd, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                             "80\r\n" + std::string(128, 'b') + "\r\n0\r\n\r\n");
                break;
            case 2:
                writeAll(fd, "HTTP/1.1 200 OK\r\n\r\n" + std::string(500, 'c'));
                break;
            case 3:
                writeAll(fd, "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc");
                break;
            default:
                writeAll(fd, "HTTP/1.1 200 OK\r\nContent-Length: ten\r\n\r\n");
                break;
        }
    });
    anpr::net::HttpRequestOptions options;
    options.max_body_bytes = 100;
    options.timeout_ms = 2000;
    for (int i = 0; i < 5; ++i) {
        const auto result = anpr::net::httpRequest(kLoopback, server.port(), options);
        CHECK_EQ(result.outcome, HttpOutcome::kProtocolError);
        CHECK_EQ(result.response.status, 200);
        CHECK(!result.error.empty());
    }
}

// ---------------------------------------------------------------------------------------------
// HttpStream.

TEST("http stream hands out chunked body bytes as they arrive") {
    std::atomic<bool> first_read{false};
    FakeServer server([&first_read](FakeServer& self, int fd, int) {
        std::string carry;
        self.record(readRequest(fd, carry));
        writeAll(fd, "HTTP/1.1 200 OK\r\nContent-Type: multipart/mixed; boundary=b\r\n"
                     "Transfer-Encoding: chunked\r\n\r\n4\r\nabc");
        const auto start = Clock::now();
        while (!first_read && msSince(start) < 2000.0) {
            sleepMs(5);
        }
        writeAll(fd, "d\r\n");
        sleepMs(20);
        writeAll(fd, "3\r\nefg\r\n0\r\n\r\n");
        waitForClose(fd, 2000);
    });
    anpr::net::HttpStream stream;
    anpr::net::HttpRequestOptions options;
    options.path = "/ISAPI/Event/notification/alertStream";
    options.timeout_ms = 2000;
    const auto result = stream.open(kLoopback, server.port(), options);
    CHECK_EQ(result.outcome, HttpOutcome::kResponse);
    CHECK_EQ(result.response.status, 200);
    CHECK(stream.isOpen());
    CHECK_EQ(stream.head().status, 200);

    std::string out;
    CHECK_EQ(stream.read(out, 2000), RecvStatus::kData);
    CHECK_EQ(out, std::string("abc"));  // Part of a chunk is handed out immediately.
    CHECK_EQ(stream.read(out, 50), RecvStatus::kTimeout);
    CHECK(stream.isOpen());
    first_read = true;
    while (out.size() < 7) {
        CHECK_EQ(stream.read(out, 2000), RecvStatus::kData);
    }
    CHECK_EQ(out, std::string("abcdefg"));
    CHECK_EQ(stream.read(out, 2000), RecvStatus::kClosed);
    CHECK(!stream.isOpen());
    CHECK_EQ(stream.read(out, 10), RecvStatus::kClosed);
    CHECK_EQ(requestLine(server.requests().at(0)),
             std::string("GET /ISAPI/Event/notification/alertStream HTTP/1.1"));
}

TEST("http stream authenticates once and closes on a rejected login") {
    FakeServer server([](FakeServer& self, int fd, int) {
        std::string carry;
        self.record(readRequest(fd, carry));
        writeAll(fd, "HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Digest realm=\"r\", "
                     "nonce=\"n\", qop=\"auth\"\r\nContent-Length: 5\r\n\r\ndenied");
    });
    anpr::net::HttpStream stream;
    anpr::net::HttpRequestOptions options;
    options.credentials = &kCameraLogin;
    options.timeout_ms = 2000;
    const auto result = stream.open(kLoopback, server.port(), options);
    CHECK_EQ(result.outcome, HttpOutcome::kResponse);
    CHECK_EQ(result.response.status, 401);
    CHECK(result.auth_attempted);
    CHECK(!stream.isOpen());
    CHECK_EQ(stream.head().status, 401);
    CHECK_EQ(server.stop(), 2);
}

TEST("http stream with digest login delivers a Content-Length body and ends") {
    FakeServer server([](FakeServer&, int fd, int) {
        std::string carry;
        const std::string request = readRequest(fd, carry);
        if (headerValue(request, "Authorization").empty()) {
            writeAll(fd, "HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Digest realm=\"r\", "
                         "nonce=\"abc\"\r\nContent-Length: 0\r\n\r\n");
            return;
        }
        if (!digestValid(request, "GET", "/stream", kCameraLogin, "abc")) {
            writeAll(fd, "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n");
            return;
        }
        writeAll(fd, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhel");
        sleepMs(20);
        writeAll(fd, "loEXTRA");
        waitForClose(fd, 2000);
    });
    anpr::net::HttpStream stream;
    anpr::net::HttpRequestOptions options;
    options.path = "/stream";
    options.credentials = &kCameraLogin;
    options.timeout_ms = 2000;
    const auto result = stream.open(kLoopback, server.port(), options);
    CHECK_EQ(result.response.status, 200);
    CHECK(result.auth_attempted);
    CHECK(stream.isOpen());
    std::string out;
    RecvStatus status = RecvStatus::kData;
    for (int i = 0; i < 10 && status == RecvStatus::kData; ++i) {
        status = stream.read(out, 2000);
    }
    CHECK_EQ(status, RecvStatus::kClosed);
    CHECK_EQ(out, std::string("hello"));
}

TEST("http stream reads until close and reports error statuses") {
    FakeServer server([](FakeServer&, int fd, int index) {
        std::string carry;
        readRequest(fd, carry);
        if (index == 0) {
            writeAll(fd, "HTTP/1.1 200 OK\r\n\r\nevent-1;");
            sleepMs(20);
            writeAll(fd, "event-2;");
        } else {
            writeAll(fd, "HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\n\r\nnot found");
        }
    });
    anpr::net::HttpStream stream;
    anpr::net::HttpRequestOptions options;
    options.timeout_ms = 2000;
    auto result = stream.open(kLoopback, server.port(), options);
    CHECK_EQ(result.response.status, 200);
    std::string out;
    RecvStatus status = RecvStatus::kData;
    for (int i = 0; i < 10 && status == RecvStatus::kData; ++i) {
        status = stream.read(out, 2000);
    }
    CHECK_EQ(status, RecvStatus::kClosed);
    CHECK_EQ(out, std::string("event-1;event-2;"));

    result = stream.open(kLoopback, server.port(), options);
    CHECK_EQ(result.outcome, HttpOutcome::kResponse);
    CHECK_EQ(result.response.status, 404);
    CHECK_EQ(result.response.body, std::string("not found"));
    CHECK(!stream.isOpen());
    CHECK_EQ(stream.head().status, 404);

    result = stream.open(kLoopback, closedPort(), options);
    CHECK_EQ(result.outcome, HttpOutcome::kConnectFailed);
    CHECK(!stream.isOpen());
}

// ---------------------------------------------------------------------------------------------
// RTSP probes.

TEST("rtsp DESCRIBE returns the SDP and the server details") {
    FakeServer server([](FakeServer& self, int fd, int) {
        std::string carry;
        const std::string request = readRequest(fd, carry);
        self.record(request);
        const std::string sdp = kSdp;
        const std::string response = rtspResponse(
            request, "200 OK",
            "Content-Type: application/sdp\r\nContent-Base: rtsp://127.0.0.1/Streaming/"
            "Channels/101/\r\nServer: Hikvision RTSP Server\r\n",
            sdp);
        writeAll(fd, response.substr(0, response.size() - 40));
        sleepMs(20);
        writeAll(fd, response.substr(response.size() - 40));
        waitForClose(fd, 2000);
    });
    const auto result = anpr::net::rtspProbe(probeOptions(server.port()));
    CHECK_EQ(result.status, RtspProbeStatus::kOk);
    CHECK_EQ(result.connect, anpr::net::ConnectOutcome::kConnected);
    CHECK_EQ(result.code, 200);
    CHECK_EQ(result.reason, std::string("OK"));
    CHECK_EQ(result.sdp, std::string(kSdp));
    CHECK_EQ(result.server, std::string("Hikvision RTSP Server"));
    CHECK_EQ(result.content_base, std::string("rtsp://127.0.0.1/Streaming/Channels/101/"));
    CHECK(!result.auth_attempted);
    CHECK(result.realm.empty());
    CHECK(result.connect_ms >= 0.0);
    CHECK(result.total_ms >= result.connect_ms);
    CHECK(!result.detail.empty());

    const auto requests = server.requests();
    CHECK_EQ(requests.size(), std::size_t{1});
    CHECK_EQ(requestLine(requests[0]), "DESCRIBE " + streamUrl(server.port()) + " RTSP/1.0");
    CHECK_EQ(headerValue(requests[0], "CSeq"), std::string("1"));
    CHECK_EQ(headerValue(requests[0], "Accept"), std::string("application/sdp"));
    CHECK_EQ(headerValue(requests[0], "User-Agent"), std::string("kz-anpr"));
    CHECK(headerValue(requests[0], "Authorization").empty());
}

TEST("rtsp OPTIONS reports the public methods without credentials") {
    FakeServer server([](FakeServer& self, int fd, int) {
        std::string carry;
        const std::string request = readRequest(fd, carry);
        self.record(request);
        writeAll(fd, rtspResponse(request, "200 OK",
                                  "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN\r\n"));
        waitForClose(fd, 2000);
    });
    auto options = probeOptions(server.port());
    options.method = "options";
    options.path = "/";
    const auto result = anpr::net::rtspProbe(options);
    CHECK_EQ(result.status, RtspProbeStatus::kOk);
    CHECK_EQ(result.public_methods, std::string("OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN"));
    CHECK(result.sdp.empty());
    const auto requests = server.requests();
    CHECK_EQ(requestLine(requests.at(0)),
             "OPTIONS rtsp://127.0.0.1:" + std::to_string(server.port()) + "/ RTSP/1.0");
    CHECK(headerValue(requests.at(0), "Accept").empty());
}

TEST("rtsp answers a digest challenge once on the same connection") {
    FakeServer server([](FakeServer& self, int fd, int) {
        std::string carry;
        const std::string first = readRequest(fd, carry);
        self.record(first);
        writeAll(fd, rtspResponse(first, "401 Unauthorized",
                                  "WWW-Authenticate: Digest realm=\"IP Camera(C1234)\", "
                                  "nonce=\"b1c2d3e4\", stale=\"FALSE\"\r\n"));
        const std::string second = readRequest(fd, carry);
        self.record(second);
        const std::string url =
            "rtsp://127.0.0.1:" + std::to_string(self.port()) + "/Streaming/Channels/101";
        if (digestValid(second, "DESCRIBE", url, kCameraLogin, "b1c2d3e4")) {
            writeAll(fd, rtspResponse(second, "200 OK", "Content-Type: application/sdp\r\n", kSdp));
        } else {
            writeAll(fd, rtspResponse(second, "401 Unauthorized", ""));
        }
        waitForClose(fd, 2000);
    });
    auto options = probeOptions(server.port());
    options.credentials = &kCameraLogin;
    const auto result = anpr::net::rtspProbe(options);
    const int connections = server.stop();
    CHECK_EQ(result.status, RtspProbeStatus::kOk);
    CHECK(result.auth_attempted);
    CHECK_EQ(result.realm, std::string("IP Camera(C1234)"));
    CHECK_EQ(result.auth_scheme, std::string("digest"));
    CHECK_EQ(result.sdp, std::string(kSdp));
    CHECK_EQ(connections, 1);
    const auto requests = server.requests();
    CHECK_EQ(requests.size(), std::size_t{2});
    CHECK_EQ(headerValue(requests[1], "CSeq"), std::string("2"));
    CHECK(requests[1].find(kCameraLogin.password) == std::string::npos);
    CHECK(result.detail.find(kCameraLogin.password) == std::string::npos);
}

TEST("rtsp reconnects for the authenticated request when the camera closed the connection") {
    for (const bool announce_close : {true, false}) {
        FakeServer server([announce_close](FakeServer& self, int fd, int index) {
            std::string carry;
            const std::string request = readRequest(fd, carry);
            self.record(request);
            if (index == 0) {
                const std::string connection = announce_close ? "Connection: close\r\n" : "";
                writeAndHangUp(fd, rtspResponse(request, "401 Unauthorized",
                                                connection + "WWW-Authenticate: Digest "
                                                             "realm=\"r\", nonce=\"n1\"\r\n"));
                return;
            }
            const std::string url =
                "rtsp://127.0.0.1:" + std::to_string(self.port()) + "/Streaming/Channels/101";
            writeAll(fd, digestValid(request, "DESCRIBE", url, kCameraLogin, "n1")
                             ? rtspResponse(request, "200 OK", "", kSdp)
                             : rtspResponse(request, "401 Unauthorized", ""));
            waitForClose(fd, 2000);
        });
        auto options = probeOptions(server.port());
        options.credentials = &kCameraLogin;
        const auto result = anpr::net::rtspProbe(options);
        const int connections = server.stop();
        CHECK_EQ(result.status, RtspProbeStatus::kOk);
        CHECK(result.auth_attempted);
        CHECK_EQ(connections, 2);
        const auto requests = server.requests();
        CHECK_EQ(requests.size(), std::size_t{2});
        CHECK_EQ(headerValue(requests[1], "CSeq"), std::string("2"));
    }
}

TEST("rtsp wrong password is sent exactly once and reported as auth failed") {
    std::atomic<int> requests_seen{0};
    FakeServer server([&requests_seen](FakeServer&, int fd, int) {
        std::string carry;
        for (;;) {
            const std::string request = readRequest(fd, carry);
            if (request.empty()) {
                return;
            }
            ++requests_seen;
            writeAll(fd, rtspResponse(request, "401 Unauthorized",
                                      "WWW-Authenticate: Digest realm=\"IP Camera(C1234)\", "
                                      "nonce=\"n\"\r\n"));
        }
    });
    const Credentials wrong{"admin", "Wrong:Pass@1"};
    auto options = probeOptions(server.port());
    options.credentials = &wrong;
    const auto result = anpr::net::rtspProbe(options);
    const int connections = server.stop();
    CHECK_EQ(result.status, RtspProbeStatus::kAuthFailed);
    CHECK_EQ(result.code, 401);
    CHECK(result.auth_attempted);
    CHECK_EQ(requests_seen.load(), 2);
    CHECK_EQ(connections, 1);
    CHECK(result.detail.find(wrong.password) == std::string::npos);
    CHECK(result.detail.find("admin") == std::string::npos);
    CHECK_EQ(anpr::net::toString(result.status), std::string("auth_failed"));
}

TEST("rtsp 401 without credentials reports the realm and sends nothing more") {
    std::atomic<int> requests_seen{0};
    FakeServer server([&requests_seen](FakeServer&, int fd, int) {
        std::string carry;
        for (;;) {
            const std::string request = readRequest(fd, carry);
            if (request.empty()) {
                return;
            }
            ++requests_seen;
            writeAll(fd, rtspResponse(request, "401 Unauthorized",
                                      "WWW-Authenticate: Basic realm=\"IP Camera(C9876)\"\r\n"));
        }
    });
    const auto result = anpr::net::rtspProbe(probeOptions(server.port()));
    server.stop();
    CHECK_EQ(result.status, RtspProbeStatus::kAuthRequired);
    CHECK(!result.auth_attempted);
    CHECK_EQ(result.realm, std::string("IP Camera(C9876)"));
    CHECK_EQ(result.auth_scheme, std::string("basic"));
    CHECK_EQ(requests_seen.load(), 1);
}

TEST("rtsp unsupported challenge keeps the credentials unsent") {
    std::atomic<int> requests_seen{0};
    FakeServer server([&requests_seen](FakeServer&, int fd, int) {
        std::string carry;
        for (;;) {
            const std::string request = readRequest(fd, carry);
            if (request.empty()) {
                return;
            }
            ++requests_seen;
            writeAll(fd, rtspResponse(request, "401 Unauthorized",
                                      "WWW-Authenticate: Digest realm=\"r\", nonce=\"n\", "
                                      "algorithm=SHA-256\r\n"));
        }
    });
    auto options = probeOptions(server.port());
    options.credentials = &kCameraLogin;
    const auto result = anpr::net::rtspProbe(options);
    server.stop();
    CHECK_EQ(result.status, RtspProbeStatus::kAuthFailed);
    CHECK(!result.auth_attempted);
    CHECK_EQ(result.auth_scheme, std::string("digest"));
    CHECK_EQ(requests_seen.load(), 1);
}

TEST("rtsp status codes map to the field diagnostics") {
    const std::vector<std::pair<std::string, RtspProbeStatus>> cases = {
        {"404 Stream Not Found", RtspProbeStatus::kPathInvalid},
        {"454 Session Not Found", RtspProbeStatus::kPathInvalid},
        {"400 Bad Request", RtspProbeStatus::kPathInvalid},
        {"403 Forbidden", RtspProbeStatus::kForbidden},
        {"500 Internal Server Error", RtspProbeStatus::kServerError},
        {"503 Service Unavailable", RtspProbeStatus::kServerError},
        {"302 Moved Temporarily", RtspProbeStatus::kProtocolError},
        {"405 Method Not Allowed", RtspProbeStatus::kProtocolError},
    };
    for (const auto& entry : cases) {
        const std::string status_line = entry.first;
        FakeServer server([status_line](FakeServer&, int fd, int) {
            std::string carry;
            const std::string request = readRequest(fd, carry);
            writeAll(fd, rtspResponse(request, status_line, ""));
            waitForClose(fd, 2000);
        });
        auto options = probeOptions(server.port());
        options.credentials = &kCameraLogin;
        const auto result = anpr::net::rtspProbe(options);
        CHECK_EQ(result.status, entry.second);
        CHECK_EQ(result.code, std::stoi(status_line.substr(0, 3)));
        CHECK(!result.auth_attempted);
        CHECK(result.sdp.empty());
        CHECK(result.detail.find(status_line.substr(0, 3)) != std::string::npos);
    }
}

TEST("rtsp probe classifies closed ports, silence, garbage and hang-ups") {
    auto result = anpr::net::rtspProbe(probeOptions(closedPort()));
    CHECK_EQ(result.status, RtspProbeStatus::kPortClosed);
    CHECK_EQ(result.connect, anpr::net::ConnectOutcome::kRefused);
    CHECK_EQ(result.code, 0);
    CHECK(result.detail.find("refused") != std::string::npos);

    {
        FakeServer silent([](FakeServer&, int fd, int) { waitForClose(fd, 3000); });
        auto options = probeOptions(silent.port());
        options.timeout_ms = 300;
        const auto start = Clock::now();
        result = anpr::net::rtspProbe(options);
        CHECK_EQ(result.status, RtspProbeStatus::kTimeout);
        CHECK_EQ(result.connect, anpr::net::ConnectOutcome::kConnected);
        CHECK(msSince(start) < 2000.0);
        CHECK(result.total_ms >= 250.0);
    }
    for (const std::string& garbage :
         {std::string("HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n"),
          std::string("\x16\x03\x01\x02\x00\x01\x00\x01\xfc\x03\x03", 11),
          std::string("RTSP/1.0 OK\r\n\r\n")}) {
        FakeServer server([garbage](FakeServer&, int fd, int) {
            std::string carry;
            readRequest(fd, carry);
            writeAll(fd, garbage);
            waitForClose(fd, 3000);
        });
        auto options = probeOptions(server.port());
        options.timeout_ms = 3000;
        const auto start = Clock::now();
        result = anpr::net::rtspProbe(options);
        CHECK_EQ(result.status, RtspProbeStatus::kProtocolError);
        CHECK_EQ(result.code, 0);
        CHECK(msSince(start) < 1500.0);
    }
    {
        FakeServer hangup([](FakeServer&, int fd, int) {
            std::string carry;
            readRequest(fd, carry);
        });
        result = anpr::net::rtspProbe(probeOptions(hangup.port()));
        CHECK_EQ(result.status, RtspProbeStatus::kProtocolError);
        CHECK(result.detail.find("closed") != std::string::npos);
    }
    {
        FakeServer reset([](FakeServer&, int fd, int) {
            std::string carry;
            readRequest(fd, carry);
            linger abortive{};
            abortive.l_onoff = 1;
            abortive.l_linger = 0;
            ::setsockopt(fd, SOL_SOCKET, SO_LINGER, &abortive, sizeof(abortive));
        });
        result = anpr::net::rtspProbe(probeOptions(reset.port()));
        CHECK_EQ(result.status, RtspProbeStatus::kProtocolError);
        CHECK(result.detail.find("reset") != std::string::npos);
    }
}

TEST("rtsp URLs percent-encode credentials and redact them for logs") {
    CHECK_EQ(anpr::net::percentEncodeUserInfo("admin"), std::string("admin"));
    CHECK_EQ(anpr::net::percentEncodeUserInfo("A-z_0.9~"), std::string("A-z_0.9~"));
    CHECK_EQ(anpr::net::percentEncodeUserInfo("p@ss:w/rd %#?"),
             std::string("p%40ss%3Aw%2Frd%20%25%23%3F"));
    CHECK_EQ(anpr::net::percentEncodeUserInfo("\xc3\xa4"), std::string("%C3%A4"));
    const Ipv4 camera = *anpr::net::parseIpv4("192.168.1.64");
    CHECK_EQ(anpr::net::rtspUrlWithCredentials(camera, 554, "/Streaming/Channels/101",
                                               Credentials{"admin", "p@ss:1"}),
             std::string("rtsp://admin:p%40ss%3A1@192.168.1.64:554/Streaming/Channels/101"));
    CHECK_EQ(anpr::net::rtspUrlWithCredentials(camera, 8554, "Streaming/Channels/102",
                                               Credentials{}),
             std::string("rtsp://192.168.1.64:8554/Streaming/Channels/102"));
    CHECK_EQ(anpr::net::redactedRtspUrl(camera, 554, "/Streaming/Channels/101", true),
             std::string("rtsp://<redacted>@192.168.1.64:554/Streaming/Channels/101"));
    CHECK_EQ(anpr::net::redactedRtspUrl(camera, 554, "/Streaming/Channels/101", false),
             std::string("rtsp://192.168.1.64:554/Streaming/Channels/101"));
    CHECK_EQ(anpr::net::toString(RtspProbeStatus::kPortClosed), std::string("port_closed"));
    CHECK_EQ(anpr::net::toString(RtspProbeStatus::kPathInvalid), std::string("path_invalid"));
}
