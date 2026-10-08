// The camera's own ANPR results from the ISAPI alert stream: one long-lived authenticated GET,
// multipart parts parsed as they arrive, plates de-duplicated and handed to the callback.
#include "anpr/hikvision/native_anpr.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <exception>
#include <iterator>
#include <map>
#include <optional>
#include <utility>

#include "anpr/cameras/diagnostics.hpp"
#include "anpr/common/logging.hpp"
#include "anpr/hikvision/xml_lite.hpp"
#include "anpr/net/http_client.hpp"

namespace anpr::hikvision {
namespace {

using Clock = std::chrono::steady_clock;

constexpr const char* kAlertStreamPath = "/ISAPI/Event/notification/alertStream";
/// stop() and the idle check are served at least this often.
constexpr int kPollSliceMs = 200;
/// A part (picture) this large without its end means the stream is not what we expect.
constexpr std::size_t kMaxBufferedBytes = 8U * 1024U * 1024U;
/// Cameras re-send an alert they consider unacknowledged.
constexpr std::int64_t kDuplicateWindowMs = 10000;
/// Bytes read while looking for the boundary of a stream whose Content-Type names none.
constexpr std::size_t kMaxBoundarySearch = 4096;

std::string lowerCase(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return text;
}

std::int64_t unixTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::int64_t msSince(Clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}

/// Alert parts are XML; pictures (image/jpeg) are skipped without looking at them. A part
/// without a usable Content-Type counts as XML when its body starts with markup.
bool isXmlPart(const MultipartParser::Part& part) {
    const auto type = part.headers.find("content-type");
    if (type != part.headers.end()) {
        const std::string value = lowerCase(type->second);
        if (value.find("xml") != std::string::npos) {
            return true;
        }
        if (value.find("image") != std::string::npos || value.find("video") != std::string::npos) {
            return false;
        }
    }
    const std::size_t first = part.body.find_first_not_of(" \t\r\n");
    return first != std::string::npos && part.body[first] == '<';
}

/// The boundary of a stream whose Content-Type does not name it, from its first delimiter line
/// ("--boundary"). Empty while the first line is incomplete; `failed` once it cannot be one.
std::string boundaryFromFirstLine(const std::string& bytes, bool& failed) {
    const std::size_t begin = bytes.find_first_not_of("\r\n");
    if (begin == std::string::npos) {
        failed = bytes.size() > kMaxBoundarySearch;
        return {};
    }
    const std::size_t end = bytes.find('\n', begin);
    if (end == std::string::npos) {
        failed = bytes.size() > kMaxBoundarySearch;
        return {};
    }
    const std::string line = xml::trim(bytes.substr(begin, end - begin));
    if (line.size() <= 2 || line.compare(0, 2, "--") != 0) {
        failed = true;
        return {};
    }
    return line.substr(2);
}

void logDisconnect(LogLevel level, const NativeAnprListener::Options& options,
                   const std::string& reason, std::int64_t retry_ms) {
    logEvent(level, "native_anpr_disconnected",
             LogFields()
                 .add("camera_id", options.camera_id)
                 .add("ip", net::toString(options.host))
                 .addQuoted("reason", reason)
                 .add("retry_ms", retry_ms));
}

}  // namespace

std::string toString(NativeAnprListener::State state) {
    switch (state) {
        case NativeAnprListener::State::kStopped:
            return "stopped";
        case NativeAnprListener::State::kConnecting:
            return "connecting";
        case NativeAnprListener::State::kStreaming:
            return "streaming";
        case NativeAnprListener::State::kBackoff:
            return "backoff";
        case NativeAnprListener::State::kAuthFailed:
            return "auth_failed";
        case NativeAnprListener::State::kUnsupported:
            return "unsupported";
    }
    return "stopped";
}

NativeAnprListener::NativeAnprListener(Options options, Callback callback)
    : options_(std::move(options)), callback_(std::move(callback)) {}

NativeAnprListener::~NativeAnprListener() {
    stop();
}

void NativeAnprListener::start() {
    if (thread_.joinable()) {
        if (running_.load()) {
            return;
        }
        // The previous run ended by itself (credentials rejected, no event stream): start over.
        thread_.join();
    }
    running_ = true;
    setState(State::kConnecting);
    thread_ = std::thread([this]() { loop(); });
}

void NativeAnprListener::stop() {
    running_ = false;
    if (thread_.joinable()) {
        if (thread_.get_id() == std::this_thread::get_id()) {
            return;  // Called from the callback: the loop ends on its own.
        }
        thread_.join();
    }
    const std::lock_guard<std::mutex> lock(mutex_);
    // A final verdict stays visible after stop(): it tells the status table why.
    if (state_ != State::kAuthFailed && state_ != State::kUnsupported) {
        state_ = State::kStopped;
    }
}

NativeAnprListener::State NativeAnprListener::state() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

void NativeAnprListener::setState(State state) {
    const std::lock_guard<std::mutex> lock(mutex_);
    state_ = state;
}

void NativeAnprListener::loop() {
    const std::int64_t initial_ms = std::max<std::int64_t>(options_.reconnect_initial_ms, 100);
    const std::int64_t max_ms = std::max(options_.reconnect_max_ms, initial_ms);
    const std::int64_t idle_ms = std::max(options_.idle_timeout_ms, kPollSliceMs);
    std::int64_t backoff_ms = initial_ms;
    // Consecutive failures without a working stream; only the first one is logged loudly.
    int failures = 0;
    // plate + camera time -> when it was delivered, for the re-send window.
    std::map<std::string, Clock::time_point> delivered;

    const auto finish = [this](State state, const std::string& detail) {
        setState(state);
        cameras::DiagnosticContext context;
        context.camera_id = options_.camera_id;
        context.ip = net::toString(options_.host);
        context.stage = cameras::Stage::kIsapi;
        cameras::reportCameraError(cameras::CameraError::kNativeAnprUnavailable, context, detail);
        running_ = false;
    };

    const auto handlePart = [this, &delivered](const MultipartParser::Part& part) {
        if (!isXmlPart(part)) {
            return;
        }
        std::optional<NativePlateEvent> event = parseAnprAlert(part.body);
        if (!event) {
            return;  // heartbeat or another event type
        }
        const auto now = Clock::now();
        for (auto entry = delivered.begin(); entry != delivered.end();) {
            entry = now - entry->second > std::chrono::milliseconds(kDuplicateWindowMs)
                        ? delivered.erase(entry)
                        : std::next(entry);
        }
        const std::string key = event->plate + '\n' + event->camera_time;
        if (!delivered.emplace(key, now).second) {
            logEvent(LogLevel::kDebug, "native_anpr_duplicate",
                     LogFields()
                         .add("camera_id", options_.camera_id)
                         .addQuoted("plate", event->plate)
                         .addQuoted("camera_time", event->camera_time));
            return;
        }
        event->camera_id = options_.camera_id;
        event->unix_time_ms = unixTimeMs();
        ++events_;
        if (!callback_) {
            return;
        }
        try {
            callback_(*event);
        } catch (const std::exception& exception) {
            logEvent(LogLevel::kError, "native_anpr_callback_failed",
                     LogFields()
                         .add("camera_id", options_.camera_id)
                         .addQuoted("error", exception.what()));
        }
    };

    while (running_.load()) {
        setState(State::kConnecting);
        net::HttpStream stream;
        net::HttpRequestOptions request;
        request.path = kAlertStreamPath;
        request.credentials = &options_.credentials;
        request.timeout_ms = options_.connect_timeout_ms;
        const net::HttpResult opened = stream.open(options_.host, options_.http_port, request);
        if (!running_.load()) {
            break;
        }

        std::string reason;
        bool connected = false;
        bool streamed = false;
        if (opened.outcome != net::HttpOutcome::kResponse) {
            reason = toString(opened.outcome) + (opened.error.empty() ? "" : ": " + opened.error);
        } else if (opened.response.status == 401) {
            // Never retried: Hikvision locks the address out after a few failed logins.
            finish(State::kAuthFailed,
                   opened.auth_attempted
                       ? "the camera rejected the credentials for the ISAPI event stream (HTTP "
                         "401 after one attempt); not retried to avoid the login lockout"
                       : "the ISAPI event stream needs a login, but no usable credentials or "
                         "challenge were available; credentials were not sent");
            return;
        } else if (opened.response.status == 403 || opened.response.status == 404) {
            finish(State::kUnsupported,
                   "the camera has no ISAPI event stream for this account (HTTP " +
                       std::to_string(opened.response.status) + " for " + kAlertStreamPath + ")");
            return;
        } else if (opened.response.status < 200 || opened.response.status >= 300) {
            reason = "HTTP " + std::to_string(opened.response.status);
        } else {
            connected = true;
            setState(State::kStreaming);
            logEvent(LogLevel::kInfo, "native_anpr_connected",
                     LogFields()
                         .add("camera_id", options_.camera_id)
                         .add("ip", net::toString(options_.host))
                         .add("port", options_.http_port));
            std::string boundary = boundaryFromContentType(
                net::findHeader(stream.head().headers, "Content-Type").value_or(""));
            std::optional<MultipartParser> parser;
            if (!boundary.empty()) {
                parser.emplace(boundary);
            }
            std::string unframed;  // body bytes before the boundary is known
            auto last_byte = Clock::now();
            while (running_.load()) {
                std::string bytes;
                const net::RecvStatus received = stream.read(bytes, kPollSliceMs);
                if (received == net::RecvStatus::kTimeout) {
                    if (msSince(last_byte) >= idle_ms) {
                        reason = "no data for " + std::to_string(idle_ms) + " ms";
                        break;
                    }
                    continue;
                }
                if (received != net::RecvStatus::kData) {
                    reason = "stream " + net::toString(received);
                    break;
                }
                last_byte = Clock::now();
                if (!parser) {
                    unframed += bytes;
                    bool failed = false;
                    boundary = boundaryFromFirstLine(unframed, failed);
                    if (failed) {
                        reason = "the event stream is not multipart (no boundary)";
                        break;
                    }
                    if (boundary.empty()) {
                        continue;
                    }
                    parser.emplace(boundary);
                    bytes.swap(unframed);
                    unframed.clear();
                }
                parser->feed(bytes);
                MultipartParser::Part part;
                while (parser->next(part)) {
                    streamed = true;
                    handlePart(part);
                }
                if (parser->buffered() > kMaxBufferedBytes) {
                    reason = "more than " + std::to_string(kMaxBufferedBytes) +
                             " bytes without a complete part";
                    break;
                }
            }
            if (!running_.load()) {
                break;
            }
        }

        // A stream that delivered parts was healthy: the next outage starts the backoff over.
        if (streamed) {
            backoff_ms = initial_ms;
            failures = 0;
        }
        // Losing an established stream is always worth a line; repeated failed connects are not.
        logDisconnect(connected || failures == 0 ? LogLevel::kWarn : LogLevel::kDebug, options_,
                      reason, backoff_ms);
        ++failures;
        setState(State::kBackoff);
        const auto wait_start = Clock::now();
        while (running_.load() && msSince(wait_start) < backoff_ms) {
            const std::int64_t left = backoff_ms - msSince(wait_start);
            std::this_thread::sleep_for(
                std::chrono::milliseconds(std::min<std::int64_t>(left, kPollSliceMs)));
        }
        backoff_ms = std::min(backoff_ms * 2, max_ms);
    }
}

}  // namespace anpr::hikvision
