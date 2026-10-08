#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "anpr/hikvision/isapi.hpp"
#include "anpr/net/http_auth.hpp"
#include "anpr/net/socket.hpp"

namespace anpr::hikvision {

/// Optional second plate source: the camera's own ANPR results, read from the ISAPI event stream
/// (GET /ISAPI/Event/notification/alertStream). It never replaces the Nomeroff pipeline. Its
/// events are written as `hikvision_anpr` next to the pipeline's `plate_recognition` events so
/// the two can be compared, or one used to validate the other later.
class NativeAnprListener {
public:
    struct Options {
        std::string camera_id;
        net::Ipv4 host;
        std::uint16_t http_port{80};
        net::Credentials credentials;
        int connect_timeout_ms{3000};
        /// No byte (not even a heartbeat) for this long reconnects the stream.
        int idle_timeout_ms{60000};
        std::int64_t reconnect_initial_ms{2000};
        std::int64_t reconnect_max_ms{60000};
    };

    enum class State { kStopped, kConnecting, kStreaming, kBackoff, kAuthFailed, kUnsupported };

    using Callback = std::function<void(const NativePlateEvent&)>;

    NativeAnprListener(Options options, Callback callback);
    ~NativeAnprListener();
    NativeAnprListener(const NativeAnprListener&) = delete;
    NativeAnprListener& operator=(const NativeAnprListener&) = delete;

    void start();
    void stop();

    [[nodiscard]] State state() const;
    [[nodiscard]] std::int64_t eventsReceived() const { return events_.load(); }

private:
    Options options_;
    Callback callback_;
    std::thread thread_;
    std::atomic_bool running_{false};
    std::atomic<std::int64_t> events_{0};
    mutable std::mutex mutex_;
    State state_{State::kStopped};

    void loop();
    void setState(State state);
};

std::string toString(NativeAnprListener::State state);

}  // namespace anpr::hikvision
