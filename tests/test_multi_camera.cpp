#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include "anpr/cameras/multi_camera_runner.hpp"
#include "anpr/cameras/rtsp_camera_source.hpp"
#include "anpr/cameras/status_store.hpp"
#include "anpr/cameras/stream_check.hpp"
#include "anpr/common/filesystem.hpp"
#include "anpr/common/logging.hpp"
#include "anpr/pipeline/plate_sink.hpp"
#include "test_framework.hpp"

namespace {

using anpr::CameraSource;
using anpr::Frame;
using anpr::PixelFormat;
using anpr::ReadStatus;
using anpr::cameras::CameraStatus;
using anpr::cameras::CameraWorkerSpec;
using anpr::cameras::MultiCameraRunner;

std::int64_t nowMs() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

template <typename Condition>
bool waitUntil(Condition condition, std::int64_t timeout_ms) {
    const std::int64_t deadline = nowMs() + timeout_ms;
    while (nowMs() < deadline) {
        if (condition()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return condition();
}

/// Synthetic camera: a bright block moving over a dark background, BGR or I420.
class SyntheticSource final : public CameraSource {
public:
    struct Options {
        int width{400};
        int height{300};
        PixelFormat format{PixelFormat::kBgr};
        int interval_ms{10};
        bool live{true};
        /// Frames before the end (files) or before going quiet (live); -1: endless.
        int frames{-1};
        bool fail_opens{false};
        bool fail_reads{false};
        /// Frames claim to have been captured this long ago.
        std::int64_t age_ms{0};
    };

    explicit SyntheticSource(Options options) : options_(options) {}

    bool open() override {
        ++opens_;
        if (options_.fail_opens) {
            return false;
        }
        open_ = true;
        return true;
    }
    void close() override { open_ = false; }
    [[nodiscard]] bool isOpen() const override { return open_; }

    ReadStatus read(Frame& frame) override {
        if (options_.fail_reads) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            return ReadStatus::kFailed;
        }
        if (options_.frames >= 0 && produced_ >= options_.frames) {
            if (!options_.live) {
                return ReadStatus::kEndOfStream;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            return ReadStatus::kEmpty;
        }
        if (options_.interval_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(options_.interval_ms));
        }
        ++produced_;
        const int block_x = (produced_ * 7) % std::max(1, options_.width - 40);
        if (options_.format == PixelFormat::kI420) {
            frame.image.create(options_.height * 3 / 2, options_.width, CV_8UC1);
            frame.image.setTo(cv::Scalar(128));
            frame.image(cv::Rect(0, 0, options_.width, options_.height)).setTo(cv::Scalar(30));
            frame.image(cv::Rect(block_x, options_.height / 3, 40, 30)).setTo(cv::Scalar(220));
        } else {
            frame.image.create(options_.height, options_.width, CV_8UC3);
            frame.image.setTo(cv::Scalar(30, 30, 30));
            cv::rectangle(frame.image, cv::Rect(block_x, options_.height / 3, 40, 30),
                          cv::Scalar(220, 220, 220), cv::FILLED);
        }
        frame.format = options_.format;
        frame.sequence = produced_;
        frame.capture_ms = nowMs() - options_.age_ms;
        frame.stream_ms = options_.live ? frame.capture_ms : (produced_ - 1) * 40;
        frame.decode_ms = 0.1;
        return ReadStatus::kOk;
    }

    [[nodiscard]] std::string describe() const override { return "synthetic"; }
    [[nodiscard]] bool reconnectable() const override { return options_.live; }

private:
    Options options_;
    std::atomic_bool open_{false};
    std::atomic_int opens_{0};
    int produced_{0};
};

/// What the shared fake detector saw, by input width (the ROI of each camera's frame).
struct DetectorLog {
    std::mutex mutex;
    std::map<int, int> calls_by_width;
    int non_bgr_inputs{0};
    /// Inputs this wide throw, to test that one camera's failures stay its own.
    int throw_for_width{-1};

    int calls(int width) {
        const std::lock_guard<std::mutex> guard(mutex);
        const auto found = calls_by_width.find(width);
        return found == calls_by_width.end() ? 0 : found->second;
    }
};

class FakeDetector final : public anpr::IPlateDetector {
public:
    explicit FakeDetector(std::shared_ptr<DetectorLog> log) : log_(std::move(log)) {}

    const std::vector<anpr::Detection>& detect(const cv::Mat& frame) override {
        int throw_for = -1;
        {
            const std::lock_guard<std::mutex> guard(log_->mutex);
            ++log_->calls_by_width[frame.cols];
            if (frame.type() != CV_8UC3) {
                ++log_->non_bgr_inputs;
            }
            throw_for = log_->throw_for_width;
        }
        if (frame.cols == throw_for) {
            throw std::runtime_error("synthetic detector failure");
        }
        return detections_;
    }
    [[nodiscard]] std::string backendName() const override { return "fake"; }

private:
    std::shared_ptr<DetectorLog> log_;
    std::vector<anpr::Detection> detections_;
};

class FakeOcr final : public anpr::IPlateOcr {
public:
    anpr::OcrResult recognize(const cv::Mat& plate) override {
        anpr::OcrResult result;
        if (plate.empty()) {
            result.rejection = anpr::OcrRejection::kEmptyCrop;
            return result;
        }
        result.text = "123ABC02";
        result.confidence = 0.9F;
        result.min_char_confidence = 0.8F;
        return result;
    }
    [[nodiscard]] std::string backendName() const override { return "fake-ocr"; }
    [[nodiscard]] std::string modelDescription() const override { return "fake"; }
};

anpr::AnprConfig testConfig() {
    anpr::AnprConfig config;
    // Run the detector on every frame interval, motion or not, so every camera calls it.
    config.detector.require_motion_in_idle = false;
    config.detector.interval_idle_ms = 30;
    config.performance.metrics_interval_ms = 0;
    config.camera.read_timeout_ms = 2000;
    config.camera.reconnect_initial_backoff_ms = 100;
    config.camera.reconnect_max_backoff_ms = 400;
    return config;
}

void useFakeModels(MultiCameraRunner& runner, const std::shared_ptr<DetectorLog>& log) {
    runner.setSharedModels(
        anpr::makeSharedPlateDetector(std::make_unique<FakeDetector>(log)),
        anpr::makeSharedPlateOcr(std::make_unique<FakeOcr>()), "fake", "fake-ocr");
}

CameraWorkerSpec syntheticSpec(const std::string& id, SyntheticSource::Options options) {
    CameraWorkerSpec spec;
    spec.camera_id = id;
    spec.anpr = testConfig();
    spec.live = options.live;
    spec.ip = "127.0.0.1";
    spec.source = std::make_unique<SyntheticSource>(options);
    return spec;
}

const CameraStatus* findStatus(const std::vector<CameraStatus>& statuses, const std::string& id) {
    for (const CameraStatus& status : statuses) {
        if (status.id == id) {
            return &status;
        }
    }
    return nullptr;
}

std::string statusOf(const MultiCameraRunner& runner, const std::string& id) {
    const std::vector<CameraStatus> statuses = runner.statuses();
    const CameraStatus* status = findStatus(statuses, id);
    return status != nullptr ? status->anpr : std::string("missing");
}

std::string scratchDirectory(const std::string& name) {
    const anpr::filesystem::path directory =
        anpr::filesystem::temp_directory_path() /
        ("kz_anpr_" + name + "_" + std::to_string(static_cast<long long>(::getpid())));
    std::error_code ignored;
    anpr::filesystem::remove_all(directory, ignored);
    return directory.string();
}

class CerrCapture {
public:
    CerrCapture() : previous_(std::cerr.rdbuf(buffer_.rdbuf())) {}
    ~CerrCapture() { std::cerr.rdbuf(previous_); }
    CerrCapture(const CerrCapture&) = delete;
    CerrCapture& operator=(const CerrCapture&) = delete;
    [[nodiscard]] std::string text() const { return buffer_.str(); }

private:
    std::ostringstream buffer_;
    std::streambuf* previous_;
};

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

/// A minimal RTSP server on 127.0.0.1 that answers every request the same way.
class FakeRtspServer {
public:
    enum class Mode { kUnauthorized, kNotFound };

    explicit FakeRtspServer(Mode mode) : mode_(mode) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        const int enable = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        ::bind(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        ::listen(fd_, 8);
        socklen_t length = sizeof(address);
        ::getsockname(fd_, reinterpret_cast<sockaddr*>(&address), &length);
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this] { serve(); });
    }

    ~FakeRtspServer() {
        stop_.store(true);
        thread_.join();
        ::close(fd_);
    }

    FakeRtspServer(const FakeRtspServer&) = delete;
    FakeRtspServer& operator=(const FakeRtspServer&) = delete;

    [[nodiscard]] std::uint16_t port() const { return port_; }
    [[nodiscard]] int requests() const { return requests_.load(); }
    [[nodiscard]] int authorizedRequests() const { return authorized_.load(); }

private:
    Mode mode_;
    int fd_{-1};
    std::uint16_t port_{0};
    std::thread thread_;
    std::atomic_bool stop_{false};
    std::atomic_int requests_{0};
    std::atomic_int authorized_{0};

    void serve() {
        while (!stop_.load()) {
            pollfd listening{fd_, POLLIN, 0};
            if (::poll(&listening, 1, 50) <= 0) {
                continue;
            }
            const int client = ::accept(fd_, nullptr, nullptr);
            if (client < 0) {
                continue;
            }
            handle(client);
            ::close(client);
        }
    }

    void handle(int client) {
        std::string buffer;
        char chunk[2048];
        while (!stop_.load()) {
            pollfd readable{client, POLLIN, 0};
            const int ready = ::poll(&readable, 1, 50);
            if (ready == 0) {
                continue;
            }
            if (ready < 0) {
                return;
            }
            const ssize_t received = ::recv(client, chunk, sizeof(chunk), 0);
            if (received <= 0) {
                return;
            }
            buffer.append(chunk, static_cast<std::size_t>(received));
            std::size_t end = buffer.find("\r\n\r\n");
            while (end != std::string::npos) {
                const std::string request = buffer.substr(0, end);
                buffer.erase(0, end + 4);
                respond(client, request);
                end = buffer.find("\r\n\r\n");
            }
        }
    }

    void respond(int client, const std::string& request) {
        ++requests_;
        if (request.find("\r\nAuthorization:") != std::string::npos) {
            ++authorized_;
        }
        std::string cseq = "1";
        const std::size_t header = request.find("CSeq:");
        if (header != std::string::npos) {
            const std::size_t begin = request.find_first_not_of(' ', header + 5);
            const std::size_t end = request.find("\r\n", begin);
            cseq = request.substr(begin,
                                  end == std::string::npos ? std::string::npos : end - begin);
        }
        std::string response;
        if (mode_ == Mode::kUnauthorized) {
            response = "RTSP/1.0 401 Unauthorized\r\nCSeq: " + cseq +
                       "\r\nWWW-Authenticate: Digest realm=\"IP Camera(C1234)\", "
                       "nonce=\"0123456789abcdef\"\r\nContent-Length: 0\r\n\r\n";
        } else {
            response = "RTSP/1.0 404 Not Found\r\nCSeq: " + cseq + "\r\nContent-Length: 0\r\n\r\n";
        }
        ::send(client, response.data(), response.size(), kSendFlags);
    }
};

/// A camera's ISAPI event stream on 127.0.0.1: one ANPR alert, then the connection stays open.
class FakeAlertStreamServer {
public:
    FakeAlertStreamServer() {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        const int enable = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        ::listen(fd_, 4);
        socklen_t length = sizeof(address);
        ::getsockname(fd_, reinterpret_cast<sockaddr*>(&address), &length);
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this] { serve(); });
    }
    ~FakeAlertStreamServer() {
        stop_.store(true);
        thread_.join();
        ::close(fd_);
    }
    FakeAlertStreamServer(const FakeAlertStreamServer&) = delete;
    FakeAlertStreamServer& operator=(const FakeAlertStreamServer&) = delete;

    [[nodiscard]] std::uint16_t port() const { return port_; }

private:
    int fd_{-1};
    std::uint16_t port_{0};
    std::thread thread_;
    std::atomic_bool stop_{false};

    void serve() {
        while (!stop_.load()) {
            pollfd listening{fd_, POLLIN, 0};
            if (::poll(&listening, 1, 50) <= 0) {
                continue;
            }
            const int client = ::accept(fd_, nullptr, nullptr);
            if (client < 0) {
                continue;
            }
            std::string request;
            char chunk[1024];
            while (request.find("\r\n\r\n") == std::string::npos) {
                const ssize_t received = ::recv(client, chunk, sizeof(chunk), 0);
                if (received <= 0) {
                    break;
                }
                request.append(chunk, static_cast<std::size_t>(received));
            }
            const std::string alert =
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                "<EventNotificationAlert version=\"2.0\" "
                "xmlns=\"http://www.hikvision.com/ver20/XMLSchema\">\n"
                "<channelID>1</channelID>\n<dateTime>2026-10-09T10:15:05+05:00</dateTime>\n"
                "<eventType>ANPR</eventType>\n<eventState>active</eventState>\n"
                "<ANPR>\n<country>30</country>\n<licensePlate>777ABC02</licensePlate>\n"
                "<direction>forward</direction>\n<confidenceLevel>97</confidenceLevel>\n"
                "</ANPR>\n</EventNotificationAlert>\n";
            const std::string part = "--boundary\r\nContent-Type: application/xml\r\n"
                                     "Content-Length: " + std::to_string(alert.size()) +
                                     "\r\n\r\n" + alert + "\r\n";
            std::ostringstream chunked;
            chunked << std::hex << part.size() << "\r\n" << part << "\r\n";
            const std::string response =
                "HTTP/1.1 200 OK\r\nContent-Type: multipart/mixed; boundary=boundary\r\n"
                "Transfer-Encoding: chunked\r\nConnection: keep-alive\r\n\r\n" +
                chunked.str();
            ::send(client, response.data(), response.size(), kSendFlags);
            // Hold the stream open, as a camera does between events.
            while (!stop_.load()) {
                pollfd peer{client, POLLIN, 0};
                if (::poll(&peer, 1, 50) > 0 && ::recv(client, chunk, sizeof(chunk), 0) <= 0) {
                    break;
                }
            }
            ::close(client);
        }
    }
};

/// Keeps the raw JSON lines (the camera's own ANPR events).
class RawEventSink final : public anpr::PlateSink {
public:
    void onRecognition(const anpr::PlateRecognitionEvent& event) override { (void)event; }
    void onRawEvent(const std::string& json_line) override {
        const std::lock_guard<std::mutex> guard(mutex_);
        lines_.push_back(json_line);
    }
    std::vector<std::string> lines() {
        const std::lock_guard<std::mutex> guard(mutex_);
        return lines_;
    }

private:
    std::mutex mutex_;
    std::vector<std::string> lines_;
};

/// A port on 127.0.0.1 where nothing listens.
std::uint16_t closedPort() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    socklen_t length = sizeof(address);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length);
    ::close(fd);
    return ntohs(address.sin_port);
}

/// The loopback interface as the camera LAN, so the preflight's route check passes for
/// 127.0.0.1 and the camera is contacted.
void loopbackLan(anpr::net::NetworkSnapshot& snapshot, anpr::net::CameraLanSelection& lan) {
    anpr::net::RouteEntry route;
    route.interface = "lo";
    route.destination = *anpr::net::parseIpv4("127.0.0.0");
    route.prefix = 8;
    route.up = true;
    snapshot.routes.push_back(route);
    lan.status = anpr::net::CameraLanStatus::kReady;
    lan.interface = "lo";
    anpr::net::Ipv4Network network;
    network.address = *anpr::net::parseIpv4("127.0.0.1");
    network.prefix = 8;
    lan.networks.push_back(network);
}

std::unique_ptr<anpr::cameras::RtspCameraSource> rtspSource(std::uint16_t port, int auth_retries,
                                                           anpr::cameras::RtspCameraSource** raw) {
    anpr::cameras::CameraTarget target;
    target.id = "cam-rtsp";
    target.ip = *anpr::net::parseIpv4("127.0.0.1");
    target.rtsp_port = port;
    target.http_port = closedPort();
    target.credentials.username = "admin";
    target.credentials.password = "wrong-password";
    target.has_credentials = true;
    anpr::cameras::CameraModeConfig config;
    config.rtsp.timeout_ms = 1000;
    config.capture.auth_max_retries = auth_retries;
    config.capture.auth_retry_interval_ms = 600000;
    config.capture.configuration_retry_interval_ms = 600000;
    anpr::net::NetworkSnapshot snapshot;
    anpr::net::CameraLanSelection lan;
    loopbackLan(snapshot, lan);
    auto source = std::make_unique<anpr::cameras::RtspCameraSource>(
        target, config, anpr::cameras::DecoderCapabilities{}, snapshot, lan);
    *raw = source.get();
    return source;
}

}  // namespace

TEST("MultiCameraRunner keeps healthy cameras processing while others fail") {
    const std::string directory = scratchDirectory("multi_camera");
    const std::string status_path = directory + "/status.json";
    MultiCameraRunner::Options options;
    options.status_file = status_path;
    options.status_interval_ms = 200;
    options.metrics_interval_ms = 0;
    options.min_available_ram_mb = 0;
    options.camera_lan_summary = "lo 127.0.0.1/8";
    options.internet_summary = "none";
    auto events = std::make_shared<anpr::CollectingSink>();
    auto log = std::make_shared<DetectorLog>();
    MultiCameraRunner runner(testConfig(), options, events);
    useFakeModels(runner, log);

    std::string error;
    SyntheticSource::Options bgr;
    CHECK(runner.addCamera(syntheticSpec("cam-01", bgr), error));
    SyntheticSource::Options i420;
    i420.width = 320;
    i420.height = 240;
    i420.format = PixelFormat::kI420;
    CameraWorkerSpec i420_spec = syntheticSpec("cam-02", i420);
    i420_spec.queue_capacity = 3;
    CHECK(runner.addCamera(std::move(i420_spec), error));
    SyntheticSource::Options broken_reads;
    broken_reads.fail_reads = true;
    CHECK(runner.addCamera(syntheticSpec("cam-03", broken_reads), error));
    SyntheticSource::Options broken_opens;
    broken_opens.fail_opens = true;
    CHECK(runner.addCamera(syntheticSpec("cam-04", broken_opens), error));
    CameraStatus standby;
    standby.id = "cam-09";
    standby.ip = "192.168.10.29";
    standby.anpr = "STANDBY";
    standby.error = "CAMERA_LIMIT_REACHED";
    runner.addInactiveCamera(standby);

    CHECK_EQ(runner.activeCameraCount(), static_cast<std::size_t>(4));
    CHECK(runner.hasCamera("cam-02"));
    CHECK(!runner.hasCamera("cam-09"));
    CHECK_EQ(runner.detectorBackend(), std::string("fake"));

    std::atomic_bool stop{false};
    std::atomic_int periodic_calls{0};
    std::thread run_thread([&runner, &stop, &periodic_calls] {
        runner.run(stop, [&periodic_calls] { ++periodic_calls; });
    });

    const bool both_running = waitUntil(
        [&runner] {
            const std::vector<CameraStatus> statuses = runner.statuses();
            const CameraStatus* first = findStatus(statuses, "cam-01");
            const CameraStatus* second = findStatus(statuses, "cam-02");
            return first != nullptr && second != nullptr && first->anpr == "RUNNING" &&
                   second->anpr == "RUNNING" && first->processed_fps > 0.0 &&
                   second->processed_fps > 0.0;
        },
        6000);
    // Give the status file one more interval to catch up.
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    const std::vector<CameraStatus> statuses = runner.statuses();
    anpr::cameras::StatusSnapshot snapshot;
    std::string read_error;
    const bool file_ok = anpr::cameras::readStatusFile(status_path, snapshot, read_error);
    stop.store(true);
    run_thread.join();

    CHECK(both_running);
    CHECK_EQ(statuses.size(), static_cast<std::size_t>(5));
    const CameraStatus* first = findStatus(statuses, "cam-01");
    const CameraStatus* second = findStatus(statuses, "cam-02");
    const CameraStatus* reads = findStatus(statuses, "cam-03");
    const CameraStatus* opens = findStatus(statuses, "cam-04");
    const CameraStatus* inactive = findStatus(statuses, "cam-09");
    CHECK(first != nullptr && second != nullptr && reads != nullptr && opens != nullptr &&
          inactive != nullptr);
    CHECK_EQ(first->anpr, std::string("RUNNING"));
    CHECK(first->input_fps > 0.0);
    CHECK_EQ(first->width, 400);
    CHECK_EQ(first->height, 300);
    CHECK_EQ(first->ip, std::string("127.0.0.1"));
    // Not an RTSP camera: no RTSP or link verdict.
    CHECK_EQ(first->rtsp, std::string("-"));
    CHECK_EQ(second->anpr, std::string("RUNNING"));
    // The I420 camera's frames reached the pipeline converted to BGR.
    CHECK_EQ(second->width, 320);
    CHECK_EQ(second->height, 240);
    CHECK(reads->anpr == "RECONNECTING" || reads->anpr == "STARTING");
    CHECK_EQ(reads->processed_fps, 0.0);
    CHECK_EQ(opens->anpr, std::string("RECONNECTING"));
    CHECK_EQ(opens->processed_fps, 0.0);
    CHECK_EQ(inactive->anpr, std::string("STANDBY"));
    CHECK(periodic_calls.load() >= 1);

    // The shared detector served both healthy cameras (detection ROI is 90% of the width) and
    // only ever saw BGR.
    CHECK(log->calls(360) > 0);
    CHECK(log->calls(288) > 0);
    CHECK_EQ(log->non_bgr_inputs, 0);

    CHECK(file_ok);
    CHECK_EQ(snapshot.cameras.size(), static_cast<std::size_t>(5));
    CHECK_EQ(snapshot.pid, static_cast<int>(::getpid()));
    CHECK_EQ(snapshot.camera_lan, std::string("lo 127.0.0.1/8"));
    CHECK_EQ(snapshot.system.detector_backend, std::string("fake"));
    CHECK_EQ(snapshot.system.ocr_backend, std::string("fake-ocr"));
    const CameraStatus* saved = findStatus(snapshot.cameras, "cam-01");
    CHECK(saved != nullptr);
    CHECK_EQ(saved->anpr, std::string("RUNNING"));
    CHECK(!snapshot.updated_at.empty());

    runner.stopAll();
    CHECK(runner.framesProcessed() > 0);
    CHECK_EQ(statusOf(runner, "cam-01"), std::string("STOPPED"));
    CHECK_EQ(statusOf(runner, "cam-04"), std::string("STOPPED"));
    // The last status file written says so too.
    CHECK(anpr::cameras::readStatusFile(status_path, snapshot, read_error));
    const CameraStatus* stopped = findStatus(snapshot.cameras, "cam-02");
    CHECK(stopped != nullptr);
    CHECK_EQ(stopped->anpr, std::string("STOPPED"));
    // Adding after stopAll is refused.
    CHECK(!runner.addCamera(syntheticSpec("cam-05", bgr), error));
    std::error_code ignored;
    anpr::filesystem::remove_all(directory, ignored);
}

TEST("MultiCameraRunner drops frames older than max_frame_age_ms") {
    MultiCameraRunner::Options options;
    auto log = std::make_shared<DetectorLog>();
    MultiCameraRunner runner(testConfig(), options, std::make_shared<anpr::CollectingSink>());
    useFakeModels(runner, log);
    SyntheticSource::Options old_frames;
    old_frames.age_ms = 400;
    CameraWorkerSpec spec = syntheticSpec("cam-stale", old_frames);
    spec.max_frame_age_ms = 100;
    std::string error;
    CHECK(runner.addCamera(std::move(spec), error));

    const bool dropped = waitUntil(
        [&runner] {
            const std::vector<CameraStatus> statuses = runner.statuses();
            const CameraStatus* status = findStatus(statuses, "cam-stale");
            return status != nullptr && status->stale_dropped > 5;
        },
        4000);
    CHECK(dropped);
    const std::vector<CameraStatus> statuses = runner.statuses();
    const CameraStatus* status = findStatus(statuses, "cam-stale");
    CHECK(status != nullptr);
    CHECK(status->capture_to_process_ms >= 400.0);
    runner.stopAll();
    CHECK_EQ(runner.framesProcessed(), 0);
    CHECK_EQ(log->calls(360), 0);
}

TEST("MultiCameraRunner processes every frame of file sources and returns when they end") {
    MultiCameraRunner::Options options;
    options.exit_when_all_finished = true;
    MultiCameraRunner runner(testConfig(), options, std::make_shared<anpr::CollectingSink>());
    auto log = std::make_shared<DetectorLog>();
    useFakeModels(runner, log);
    std::string error;
    for (const char* id : {"clip-1", "clip-2"}) {
        SyntheticSource::Options clip;
        clip.live = false;
        clip.frames = 15;
        clip.interval_ms = 0;
        CameraWorkerSpec spec = syntheticSpec(id, clip);
        spec.anpr.camera.process_every_file_frame = true;
        CHECK(runner.addCamera(std::move(spec), error));
    }

    std::atomic_bool stop{false};
    std::thread watchdog([&stop] {
        const std::int64_t deadline = nowMs() + 15000;
        while (!stop.load() && nowMs() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        stop.store(true);
    });
    const std::int64_t started = nowMs();
    runner.run(stop);
    const std::int64_t elapsed = nowMs() - started;
    stop.store(true);
    watchdog.join();

    CHECK(elapsed < 10000);
    CHECK_EQ(runner.framesProcessed(), 30);
    CHECK_EQ(statusOf(runner, "clip-1"), std::string("STOPPED"));
    CHECK_EQ(statusOf(runner, "clip-2"), std::string("STOPPED"));
    runner.stopAll();
}

TEST("MultiCameraRunner contains one camera's exceptions") {
    const anpr::LogLevel previous_level = anpr::Logger::instance().level();
    anpr::Logger::instance().setLevel(anpr::LogLevel::kInfo);
    std::string logged;
    int healthy_calls = 0;
    int failing_calls = 0;
    std::string failing_state;
    std::string healthy_state;
    {
        CerrCapture capture;
        auto log = std::make_shared<DetectorLog>();
        log->throw_for_width = 360;
        MultiCameraRunner runner(testConfig(), MultiCameraRunner::Options{},
                                 std::make_shared<anpr::CollectingSink>());
        useFakeModels(runner, log);
        std::string error;
        CHECK(runner.addCamera(syntheticSpec("cam-throws", SyntheticSource::Options{}), error));
        SyntheticSource::Options small;
        small.width = 320;
        small.height = 240;
        CHECK(runner.addCamera(syntheticSpec("cam-fine", small), error));
        CHECK(waitUntil([&log] { return log->calls(360) >= 5 && log->calls(288) >= 5; }, 5000));
        const int before = log->calls(288);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        healthy_calls = log->calls(288) - before;
        failing_calls = log->calls(360);
        failing_state = statusOf(runner, "cam-throws");
        healthy_state = statusOf(runner, "cam-fine");
        runner.stopAll();
        logged = capture.text();
    }
    anpr::Logger::instance().setLevel(previous_level);
    CHECK(healthy_calls > 0);
    CHECK(failing_calls >= 5);
    CHECK_EQ(failing_state, std::string("RUNNING"));
    CHECK_EQ(healthy_state, std::string("RUNNING"));
    CHECK(logged.find("event=camera_frame_failed camera_id=cam-throws") != std::string::npos);
    CHECK(logged.find("synthetic detector failure") != std::string::npos);
}

TEST("MultiCameraRunner validates cameras and accepts new ones while running") {
    MultiCameraRunner runner(testConfig(), MultiCameraRunner::Options{},
                             std::make_shared<anpr::CollectingSink>());
    std::string error;
    CHECK(!runner.addCamera(syntheticSpec("cam-a", SyntheticSource::Options{}), error));
    CHECK(error.find("models") != std::string::npos);

    auto log = std::make_shared<DetectorLog>();
    useFakeModels(runner, log);
    CameraWorkerSpec no_source;
    no_source.camera_id = "cam-x";
    CHECK(!runner.addCamera(std::move(no_source), error));
    CHECK(!runner.addCamera(syntheticSpec("", SyntheticSource::Options{}), error));

    CHECK(runner.addCamera(syntheticSpec("cam-a", SyntheticSource::Options{}), error));
    CHECK(!runner.addCamera(syntheticSpec("cam-a", SyntheticSource::Options{}), error));
    CHECK(error.find("already running") != std::string::npos);

    // A file that cannot be opened is refused at once.
    SyntheticSource::Options missing_file;
    missing_file.live = false;
    missing_file.fail_opens = true;
    CHECK(!runner.addCamera(syntheticSpec("cam-file", missing_file), error));
    CHECK(!runner.hasCamera("cam-file"));

    CameraStatus waiting;
    waiting.id = "cam-b";
    waiting.anpr = "OFFLINE";
    runner.addInactiveCamera(waiting);

    std::atomic_bool stop{false};
    std::thread run_thread([&runner, &stop] { runner.run(stop); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    // A camera powered up later joins the running process and replaces its inactive entry.
    CHECK(runner.addCamera(syntheticSpec("cam-b", SyntheticSource::Options{}), error));
    CHECK_EQ(runner.activeCameraCount(), static_cast<std::size_t>(2));
    const bool running =
        waitUntil([&runner] { return statusOf(runner, "cam-b") == "RUNNING"; }, 4000);
    const std::size_t entries = runner.statuses().size();
    stop.store(true);
    run_thread.join();
    runner.stopAll();
    CHECK(running);
    CHECK_EQ(entries, static_cast<std::size_t>(2));
}

TEST("MultiCameraRunner logs camera and system metrics") {
    const anpr::LogLevel previous_level = anpr::Logger::instance().level();
    anpr::Logger::instance().setLevel(anpr::LogLevel::kInfo);
    std::string logged;
    {
        CerrCapture capture;
        MultiCameraRunner::Options options;
        options.metrics_interval_ms = 300;
        // Far above any machine's RAM: the low-memory warning must fire (where meminfo exists).
        options.min_available_ram_mb = 1 << 30;
        MultiCameraRunner runner(testConfig(), options, std::make_shared<anpr::CollectingSink>());
        useFakeModels(runner, std::make_shared<DetectorLog>());
        std::string error;
        CHECK(runner.addCamera(syntheticSpec("cam-m", SyntheticSource::Options{}), error));
        std::atomic_bool stop{false};
        std::thread run_thread([&runner, &stop] { runner.run(stop); });
        std::this_thread::sleep_for(std::chrono::milliseconds(900));
        stop.store(true);
        run_thread.join();
        runner.stopAll();
        logged = capture.text();
    }
    anpr::Logger::instance().setLevel(previous_level);
    CHECK(logged.find("event=camera_metrics camera_id=cam-m input_fps=") != std::string::npos);
    CHECK(logged.find("state=RUNNING") != std::string::npos);
    CHECK(logged.find("event=system_metrics") != std::string::npos);
    CHECK(logged.find("detector_backend=fake") != std::string::npos);
    CHECK(logged.find("cameras_active=1") != std::string::npos);
#ifdef __linux__
    CHECK(logged.find("error=OUT_OF_MEMORY_RISK") != std::string::npos);
    // At most once per minute.
    const std::size_t first = logged.find("error=OUT_OF_MEMORY_RISK");
    CHECK(logged.find("error=OUT_OF_MEMORY_RISK", first + 1) == std::string::npos);
#endif
}

TEST("RtspCameraSource gives up after a rejected login and the runner shows it DISABLED") {
    FakeRtspServer server(FakeRtspServer::Mode::kUnauthorized);
    MultiCameraRunner runner(testConfig(), MultiCameraRunner::Options{},
                             std::make_shared<anpr::CollectingSink>());
    useFakeModels(runner, std::make_shared<DetectorLog>());
    anpr::cameras::RtspCameraSource* raw = nullptr;
    CameraWorkerSpec spec;
    spec.camera_id = "cam-rtsp";
    spec.anpr = testConfig();
    spec.source = rtspSource(server.port(), 0, &raw);
    std::string error;
    CHECK(runner.addCamera(std::move(spec), error));
    CHECK(waitUntil([&runner] { return statusOf(runner, "cam-rtsp") == "DISABLED"; }, 4000));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // The password went to the camera exactly once: no retry can lock the camera out.
    CHECK_EQ(server.authorizedRequests(), 1);
    CHECK(raw->permanentlyFailed());
    const anpr::cameras::RtspCameraSource::Status source = raw->status();
    CHECK(source.error == anpr::cameras::CameraError::kRtspAuthFailed);
    CHECK(source.reachable);
    CHECK(!source.open);
    const std::vector<CameraStatus> statuses = runner.statuses();
    const CameraStatus* status = findStatus(statuses, "cam-rtsp");
    CHECK(status != nullptr);
    CHECK_EQ(status->rtsp, std::string("AUTH"));
    CHECK_EQ(status->link, std::string("OK"));
    CHECK_EQ(status->error, std::string("RTSP_AUTH_FAILED"));
    CHECK(status->detail.find("wrong-password") == std::string::npos);
    CHECK_EQ(raw->describe().find("wrong-password"), std::string::npos);
    runner.stopAll();
}

TEST("RtspCameraSource schedules the slow retry for login and configuration errors") {
    {
        FakeRtspServer server(FakeRtspServer::Mode::kUnauthorized);
        anpr::cameras::RtspCameraSource* raw = nullptr;
        auto source = rtspSource(server.port(), 2, &raw);
        CHECK(!source->open());
        CHECK(!source->permanentlyFailed());
        CHECK_EQ(source->reconnectDelayOverrideMs(), 600000);
        CHECK(source->status().error == anpr::cameras::CameraError::kRtspAuthFailed);
        CHECK_EQ(server.authorizedRequests(), 1);
        CHECK(source->reportsOwnErrors());
    }
    {
        FakeRtspServer server(FakeRtspServer::Mode::kNotFound);
        MultiCameraRunner runner(testConfig(), MultiCameraRunner::Options{},
                                 std::make_shared<anpr::CollectingSink>());
        useFakeModels(runner, std::make_shared<DetectorLog>());
        anpr::cameras::RtspCameraSource* raw = nullptr;
        CameraWorkerSpec spec;
        spec.camera_id = "cam-rtsp";
        spec.anpr = testConfig();
        spec.source = rtspSource(server.port(), 2, &raw);
        std::string error;
        CHECK(runner.addCamera(std::move(spec), error));
        CHECK(waitUntil([&runner] { return statusOf(runner, "cam-rtsp") == "ERROR"; }, 4000));
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const std::vector<CameraStatus> statuses = runner.statuses();
        const CameraStatus* status = findStatus(statuses, "cam-rtsp");
        CHECK(status != nullptr);
        CHECK_EQ(status->rtsp, std::string("PATH"));
        CHECK_EQ(status->error, std::string("RTSP_STREAM_PATH_INVALID"));
        // configuration_retry_interval_ms (10 min) holds the next attempt back.
        CHECK_EQ(raw->reconnectDelayOverrideMs(), 600000);
        CHECK_EQ(server.requests(), 1);
        const std::int64_t stop_started = nowMs();
        runner.stopAll();
        CHECK(nowMs() - stop_started < 2000);
    }
    {
        // Nothing listens on the RTSP port: a transient failure on the pump's own backoff.
        anpr::cameras::RtspCameraSource* raw = nullptr;
        auto source = rtspSource(closedPort(), 2, &raw);
        CHECK(!source->open());
        CHECK_EQ(source->reconnectDelayOverrideMs(), 0);
        CHECK(!source->permanentlyFailed());
        const anpr::cameras::RtspCameraSource::Status status = source->status();
        CHECK(status.error == anpr::cameras::CameraError::kRtspPortClosed);
        CHECK_EQ(anpr::cameras::rtspStatusLabel(status.error, status.rtsp_ok), std::string("PORT"));
    }
}

TEST("MultiCameraRunner forwards the camera's own ANPR events tagged with its id") {
    FakeAlertStreamServer camera;
    auto sink = std::make_shared<RawEventSink>();
    MultiCameraRunner runner(testConfig(), MultiCameraRunner::Options{}, sink);
    useFakeModels(runner, std::make_shared<DetectorLog>());
    CameraWorkerSpec spec = syntheticSpec("cam-native", SyntheticSource::Options{});
    spec.native_anpr = true;
    spec.native_anpr_options.host = *anpr::net::parseIpv4("127.0.0.1");
    spec.native_anpr_options.http_port = camera.port();
    spec.native_anpr_options.credentials.username = "admin";
    spec.native_anpr_options.credentials.password = "unused";
    std::string error;
    CHECK(runner.addCamera(std::move(spec), error));
    const bool delivered = waitUntil([&sink] { return !sink->lines().empty(); }, 5000);
    const std::int64_t stop_started = nowMs();
    runner.stopAll();
    CHECK(nowMs() - stop_started < 3000);
    CHECK(delivered);
    const std::vector<std::string> lines = sink->lines();
    CHECK_EQ(lines.size(), static_cast<std::size_t>(1));
    CHECK(lines[0].find("\"event\":\"hikvision_anpr\"") != std::string::npos);
    CHECK(lines[0].find("\"camera_id\":\"cam-native\"") != std::string::npos);
    CHECK(lines[0].find("777ABC02") != std::string::npos);
}

TEST("rtspStatusLabel maps diagnostics to the RTSP column") {
    using anpr::cameras::CameraError;
    using anpr::cameras::rtspStatusLabel;
    CHECK_EQ(rtspStatusLabel(CameraError::kNone, true), std::string("OK"));
    CHECK_EQ(rtspStatusLabel(CameraError::kNone, false), std::string("-"));
    CHECK_EQ(rtspStatusLabel(CameraError::kRtspAuthFailed, false), std::string("AUTH"));
    CHECK_EQ(rtspStatusLabel(CameraError::kRtspCredentialsMissing, false), std::string("NOCRED"));
    CHECK_EQ(rtspStatusLabel(CameraError::kRtspStreamPathInvalid, false), std::string("PATH"));
    CHECK_EQ(rtspStatusLabel(CameraError::kCameraUnreachable, false), std::string("DOWN"));
    CHECK_EQ(rtspStatusLabel(CameraError::kRtspProtocolError, false), std::string("ERROR"));
    CHECK_EQ(rtspStatusLabel(CameraError::kCameraOnOtherSubnet, false), std::string("-"));
    CHECK_EQ(rtspStatusLabel(CameraError::kStreamTimeout, true), std::string("OK"));
    CHECK_EQ(rtspStatusLabel(CameraError::kStreamOpenFailed, true), std::string("OK"));
}

TEST("formatStreamCheck prints the READY and NOT READY blocks") {
    anpr::cameras::CameraTarget target;
    target.id = "camera-01";
    target.ip = *anpr::net::parseIpv4("192.168.10.21");
    target.mac = "44:19:b6:3a:10:21";
    target.vendor = "Hikvision";
    target.model = "DS-TCG406-E";
    target.has_credentials = true;
    target.credentials.username = "admin";
    target.credentials.password = "secret-password";

    anpr::cameras::StreamCheckResult ready;
    ready.codec = anpr::net::VideoCodec::kH264;
    ready.width = 2688;
    ready.height = 1520;
    ready.measured_fps = 20.0;
    ready.first_frame_ms = 430.0;
    ready.frames = 60;
    ready.decoder = "nvidia_hardware";
    const std::string block = anpr::cameras::formatStreamCheck(target, ready);
    CHECK_EQ(block,
             std::string("CAMERA camera-01\n"
                         "  IP: 192.168.10.21   MAC: 44:19:b6:3a:10:21   Vendor: Hikvision   "
                         "Model: DS-TCG406-E\n"
                         "  RTSP: OK   Stream: main   URL: "
                         "rtsp://<redacted>@192.168.10.21:554/Streaming/Channels/101\n"
                         "  Codec: H.264   Resolution: 2688x1520   FPS: 20.0   "
                         "First frame: 430 ms\n"
                         "  Decoder: nvidia_hardware\n"
                         "  STATUS: READY\n"));

    anpr::cameras::StreamCheckResult rejected;
    rejected.error = anpr::cameras::CameraError::kRtspAuthFailed;
    rejected.rtsp.error = anpr::cameras::CameraError::kRtspAuthFailed;
    const std::string failed = anpr::cameras::formatStreamCheck(target, rejected);
    CHECK(failed.find("  RTSP: AUTH   Stream: main") != std::string::npos);
    CHECK(failed.find("  ERROR RTSP_AUTH_FAILED  camera-01 192.168.10.21\n") != std::string::npos);
    CHECK(failed.find("    action: ") != std::string::npos);
    CHECK(failed.find("Codec:") == std::string::npos);
    CHECK(failed.find("secret-password") == std::string::npos);
    CHECK(failed.size() >= 18);
    CHECK_EQ(failed.substr(failed.size() - 18), std::string("STATUS: NOT READY\n"));

    anpr::cameras::StreamCheckResult no_decoder;
    no_decoder.error = anpr::cameras::CameraError::kStreamOpenFailed;
    no_decoder.codec = anpr::net::VideoCodec::kH265;
    no_decoder.fallbacks = {"nvidia_hardware: no element \"nvv4l2decoder\"",
                            "software_gstreamer: rtspsrc0: Unauthorized"};
    no_decoder.detail = "nvidia_hardware: ...";
    const std::string open_failed = anpr::cameras::formatStreamCheck(target, no_decoder);
    CHECK(open_failed.find("  Codec: H.265   Resolution: -   FPS: -   First frame: -\n") !=
          std::string::npos);
    CHECK(open_failed.find("  Decoder: none\n") != std::string::npos);
    CHECK(open_failed.find("  Fallbacks: nvidia_hardware: no element \"nvv4l2decoder\"; "
                           "software_gstreamer: rtspsrc0: Unauthorized\n") != std::string::npos);
    CHECK(open_failed.find("  ERROR STREAM_OPEN_FAILED  camera-01 192.168.10.21\n") !=
          std::string::npos);
}

TEST("detectDecoderCapabilities describes every finding") {
    const anpr::cameras::DecoderCapabilities capabilities =
        anpr::cameras::detectDecoderCapabilities();
    CHECK(capabilities.notes.size() >= 6);
#ifdef KZ_ANPR_WITH_GSTREAMER
    CHECK(capabilities.gst_native);
#else
    CHECK(!capabilities.gst_native);
#endif
    // The development image has neither the Jetson plugins nor the decoder device.
    if (!anpr::filesystem::exists("/dev/nvhost-nvdec")) {
        CHECK(!capabilities.nvidia_device);
    }
}
