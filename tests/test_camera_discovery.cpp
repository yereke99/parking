#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "anpr/cameras/camera_mode_config.hpp"
#include "anpr/cameras/camera_registry.hpp"
#include "anpr/cameras/diagnostics.hpp"
#include "anpr/cameras/discovery.hpp"
#include "anpr/cameras/gst_pipeline.hpp"
#include "anpr/cameras/rtsp_preflight.hpp"
#include "anpr/common/logging.hpp"
#include "anpr/net/network_topology.hpp"
#include "anpr/net/rtsp_client.hpp"
#include "test_framework.hpp"

using namespace anpr::cameras;
using anpr::net::ConnectOutcome;
using anpr::net::Ipv4;
using anpr::net::RtspProbeStatus;
using anpr::net::VideoCodec;

namespace {

#ifdef MSG_NOSIGNAL
constexpr int kNoSignal = MSG_NOSIGNAL;
#else
constexpr int kNoSignal = 0;
#endif

const char* const kPassword = "s3cr3t-P@ss \"q\"";

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

Ipv4 ip(const std::string& text) {
    return anpr::net::parseIpv4(text).value_or(Ipv4{});
}

/// The camera LAN of the field setup: eth0 with 192.168.10.5/24.
anpr::net::CameraLanSelection fieldLan() {
    anpr::net::CameraLanSelection lan;
    lan.status = anpr::net::CameraLanStatus::kReady;
    lan.interface = "eth0";
    lan.address = anpr::net::InterfaceAddress{ip("192.168.10.5"), 24};
    lan.networks.push_back(anpr::net::Ipv4Network{ip("192.168.10.5"), 24});
    return lan;
}

/// Loopback as the "camera LAN" so a fake camera on 127.0.0.1 passes the route guard, and a
/// GSM-like default route on wwan0 for everything else.
anpr::net::NetworkSnapshot loopbackSnapshot() {
    anpr::net::NetworkSnapshot snapshot;
    anpr::net::RouteEntry loopback;
    loopback.interface = "lo";
    loopback.destination = ip("127.0.0.0");
    loopback.prefix = 8;
    loopback.up = true;
    snapshot.routes.push_back(loopback);
    anpr::net::RouteEntry modem;
    modem.interface = "wwan0";
    modem.prefix = 0;
    modem.gateway = ip("10.64.64.64");
    modem.via_gateway = true;
    modem.up = true;
    snapshot.routes.push_back(modem);
    return snapshot;
}

anpr::net::CameraLanSelection loopbackLan() {
    anpr::net::CameraLanSelection lan;
    lan.status = anpr::net::CameraLanStatus::kReady;
    lan.interface = "lo";
    // The Jetson's "own" address must not be the fake camera's 127.0.0.1: own addresses are
    // never probed.
    lan.address = anpr::net::InterfaceAddress{ip("127.0.0.254"), 8};
    lan.networks.push_back(anpr::net::Ipv4Network{ip("127.0.0.254"), 8});
    return lan;
}

anpr::hikvision::SadpDevice sadpCamera(const std::string& address, const std::string& mac,
                                       const std::string& serial) {
    anpr::hikvision::SadpDevice device;
    device.from = ip(address);
    device.ipv4 = address;
    device.mac = mac;
    device.serial = serial;
    device.model = "DS-TCG406-E";
    device.device_type = "IPC";
    device.subnet_mask = "255.255.255.0";
    device.gateway = "0.0.0.0";
    device.http_port = 80;
    device.command_port = 8000;
    device.software_version = "V5.5.110";
    device.activated = true;
    return device;
}

void addPorts(anpr::cameras::DiscoveryInputs& inputs, const std::string& address,
              const std::map<std::uint16_t, ConnectOutcome>& ports) {
    for (const auto& port : ports) {
        anpr::net::PortProbe probe;
        probe.host = ip(address);
        probe.port = port.first;
        probe.outcome = port.second;
        inputs.ports.push_back(probe);
    }
}

const DiscoveredCamera* byIp(const std::vector<DiscoveredCamera>& cameras,
                             const std::string& address) {
    for (const DiscoveredCamera& camera : cameras) {
        if (camera.ip == ip(address)) {
            return &camera;
        }
    }
    return nullptr;
}

const DiscoveredCamera* byMac(const std::vector<DiscoveredCamera>& cameras,
                              const std::string& mac) {
    for (const DiscoveredCamera& camera : cameras) {
        if (camera.mac == mac) {
            return &camera;
        }
    }
    return nullptr;
}

std::string problemDetail(const DiscoveredCamera& camera, CameraError error) {
    for (const auto& problem : camera.problems) {
        if (problem.first == error) {
            return problem.second;
        }
    }
    return {};
}

bool anyNoteContains(const DiscoveredCamera& camera, const std::string& part) {
    for (const std::string& note : camera.notes) {
        if (contains(note, part)) {
            return true;
        }
    }
    return false;
}

struct FakeEnv {
    std::map<std::string, std::string> values;

    EnvLookup lookup() const {
        return [this](const std::string& name) -> std::optional<std::string> {
            const auto found = values.find(name);
            if (found == values.end()) {
                return std::nullopt;
            }
            return found->second;
        };
    }
};

/// Quiets the info-level discovery log lines for the duration of a test.
class LogLevelGuard {
public:
    explicit LogLevelGuard(anpr::LogLevel level) : previous_(anpr::Logger::instance().level()) {
        anpr::Logger::instance().setLevel(level);
    }
    ~LogLevelGuard() { anpr::Logger::instance().setLevel(previous_); }
    LogLevelGuard(const LogLevelGuard&) = delete;
    LogLevelGuard& operator=(const LogLevelGuard&) = delete;

private:
    anpr::LogLevel previous_;
};

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

/// A minimal Hikvision-like RTSP server on 127.0.0.1: OPTIONS without authentication, DESCRIBE
/// behind a Digest challenge with the realm "IP Camera(G1234)".
class FakeRtspServer {
public:
    enum class Mode { kDigestOk, kAlwaysUnauthorized, kNotFound, kMjpeg };

    explicit FakeRtspServer(Mode mode) : mode_(mode) {
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
        thread_ = std::thread([this]() { serve(); });
    }
    ~FakeRtspServer() {
        stop_ = true;
        thread_.join();
        ::close(fd_);
    }
    FakeRtspServer(const FakeRtspServer&) = delete;
    FakeRtspServer& operator=(const FakeRtspServer&) = delete;

    [[nodiscard]] std::uint16_t port() const { return port_; }
    [[nodiscard]] int connections() const { return connections_; }
    [[nodiscard]] bool sawAuthorization() const { return saw_authorization_; }
    [[nodiscard]] std::vector<std::string> methods() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return methods_;
    }

private:
    Mode mode_;
    int fd_{-1};
    std::uint16_t port_{0};
    std::atomic_bool stop_{false};
    std::atomic<int> connections_{0};
    std::atomic_bool saw_authorization_{false};
    mutable std::mutex mutex_;
    std::vector<std::string> methods_;
    std::thread thread_;

    void serve() {
        while (!stop_) {
            pollfd entry{};
            entry.fd = fd_;
            entry.events = POLLIN;
            if (::poll(&entry, 1, 20) <= 0) {
                continue;
            }
            const int client = ::accept(fd_, nullptr, nullptr);
            if (client < 0) {
                continue;
            }
#ifdef SO_NOSIGPIPE
            const int one = 1;
            ::setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
            ++connections_;
            handle(client);
            ::close(client);
        }
    }

    void handle(int client) {
        std::string buffer;
        int idle_ms = 0;
        while (!stop_ && idle_ms < 3000) {
            const std::size_t end = buffer.find("\r\n\r\n");
            if (end != std::string::npos) {
                const std::string request = buffer.substr(0, end + 2);
                buffer.erase(0, end + 4);
                respond(client, request);
                idle_ms = 0;
                continue;
            }
            pollfd entry{};
            entry.fd = client;
            entry.events = POLLIN;
            if (::poll(&entry, 1, 20) <= 0) {
                idle_ms += 20;
                continue;
            }
            char chunk[2048];
            const ssize_t received = ::recv(client, chunk, sizeof(chunk), 0);
            if (received <= 0) {
                return;
            }
            buffer.append(chunk, static_cast<std::size_t>(received));
        }
    }

    static std::string header(const std::string& request, const std::string& name) {
        const std::size_t start = request.find("\r\n" + name + ": ");
        if (start == std::string::npos) {
            return {};
        }
        const std::size_t value = start + name.size() + 4;
        return request.substr(value, request.find("\r\n", value) - value);
    }

    void respond(int client, const std::string& request) {
        const std::string method = request.substr(0, request.find(' '));
        const std::string authorization = header(request, "Authorization");
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            methods_.push_back(method);
        }
        if (!authorization.empty()) {
            saw_authorization_ = true;
        }
        const std::string cseq = "CSeq: " + header(request, "CSeq") + "\r\n";
        std::string reply;
        const std::string challenge =
            "RTSP/1.0 401 Unauthorized\r\n" + cseq +
            "WWW-Authenticate: Digest realm=\"IP Camera(G1234)\", "
            "nonce=\"0123456789abcdef0123456789abcdef\", stale=\"FALSE\"\r\n\r\n";
        if (method == "OPTIONS") {
            reply = "RTSP/1.0 200 OK\r\n" + cseq +
                    "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN\r\n\r\n";
        } else if (method != "DESCRIBE") {
            reply = "RTSP/1.0 405 Method Not Allowed\r\n" + cseq + "\r\n";
        } else if (authorization.empty() || mode_ == Mode::kAlwaysUnauthorized ||
                   !contains(authorization, "username=\"admin\"")) {
            reply = challenge;
        } else if (mode_ == Mode::kNotFound) {
            reply = "RTSP/1.0 404 Not Found\r\n" + cseq + "\r\n";
        } else {
            const std::string media = mode_ == Mode::kMjpeg
                                          ? "m=video 0 RTP/AVP 26\r\na=rtpmap:26 JPEG/90000\r\n"
                                          : "m=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\n"
                                            "a=fmtp:96 profile-level-id=420029; "
                                            "packetization-mode=1\r\n";
            const std::string sdp = "v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=Media Presentation\r\n"
                                    "c=IN IP4 0.0.0.0\r\nt=0 0\r\n" +
                                    media + "a=control:trackID=1\r\n";
            reply = "RTSP/1.0 200 OK\r\n" + cseq + "Content-Type: application/sdp\r\n" +
                    "Content-Length: " + std::to_string(sdp.size()) + "\r\n\r\n" + sdp;
        }
        std::size_t sent = 0;
        while (sent < reply.size()) {
            const ssize_t written =
                ::send(client, reply.data() + sent, reply.size() - sent, kNoSignal);
            if (written <= 0) {
                return;
            }
            sent += static_cast<std::size_t>(written);
        }
    }
};

CameraTarget loopbackTarget(std::uint16_t rtsp_port, bool with_credentials) {
    CameraTarget target;
    target.id = "camera-01";
    target.ip = ip("127.0.0.1");
    target.rtsp_port = rtsp_port;
    target.http_port = closedPort();
    if (with_credentials) {
        target.credentials.username = "admin";
        target.credentials.password = kPassword;
        target.has_credentials = true;
        target.credential_source = "HIKVISION_USERNAME/HIKVISION_PASSWORD";
    }
    return target;
}

StreamEndpoint fieldEndpoint(bool with_credentials) {
    StreamEndpoint endpoint;
    endpoint.host = ip("192.168.10.21");
    endpoint.port = 554;
    endpoint.path = "/Streaming/Channels/101";
    if (with_credentials) {
        endpoint.credentials.username = "admin";
        endpoint.credentials.password = kPassword;
        endpoint.has_credentials = true;
    }
    return endpoint;
}

DecoderCapabilities everything() {
    DecoderCapabilities capabilities;
    capabilities.gst_native = true;
    capabilities.opencv_gstreamer = true;
    capabilities.opencv_ffmpeg = true;
    capabilities.nvidia_decoder = true;
    capabilities.nvidia_device = true;
    capabilities.gst_avdec_h264 = true;
    capabilities.gst_avdec_h265 = true;
    capabilities.gst_rtsp = true;
    return capabilities;
}

std::string planSummary(const std::vector<DecoderAttempt>& plan) {
    std::string out;
    for (const DecoderAttempt& attempt : plan) {
        if (!out.empty()) {
            out += " ";
        }
        out += attempt.decoder + "/" + attempt.backend;
    }
    return out;
}

bool leaksPassword(const std::vector<DecoderAttempt>& plan) {
    for (const DecoderAttempt& attempt : plan) {
        if (contains(attempt.description, "s3cr3t") || contains(attempt.note, "s3cr3t")) {
            return true;
        }
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// describeSources / ids
// ---------------------------------------------------------------------------------------------

TEST("discovery: sources are described in a fixed order") {
    CHECK_EQ(describeSources(0), std::string("none"));
    CHECK_EQ(describeSources(kFoundByScan | kFoundBySadp | kFoundByOnvif),
             std::string("sadp,onvif,scan"));
    CHECK_EQ(describeSources(kFoundByRegistry | kFoundByManual | kFoundByArp),
             std::string("arp,manual,registry"));
}

TEST("discovery: camera ids sort naturally") {
    CHECK(cameraIdLess("camera-01", "camera-02"));
    CHECK(cameraIdLess("camera-2", "camera-10"));
    CHECK(!cameraIdLess("camera-10", "camera-2"));
    CHECK(cameraIdLess("camera-09", "camera-10"));
    CHECK(cameraIdLess("camera-1", "camera-01"));
    CHECK(!cameraIdLess("camera-01", "camera-01"));
    CHECK(cameraIdLess("camera-99", "camera-100"));
    CHECK(cameraIdLess("camera", "camera-01"));
    CHECK(cameraIdLess("entrance", "exit"));
}

// ---------------------------------------------------------------------------------------------
// mergeDiscovery
// ---------------------------------------------------------------------------------------------

TEST("discovery: four SADP cameras on the camera subnet") {
    DiscoveryInputs inputs;
    for (int i = 1; i <= 4; ++i) {
        const std::string address = "192.168.10.2" + std::to_string(i);
        // Out of order on purpose: the result is sorted by address.
        inputs.sadp.insert(inputs.sadp.begin(),
                           sadpCamera(address, "44:19:b6:00:00:0" + std::to_string(i),
                                      "DS-TCG406-E2025000" + std::to_string(i)));
        addPorts(inputs, address,
                 {{554, ConnectOutcome::kConnected},
                  {80, ConnectOutcome::kConnected},
                  {8000, ConnectOutcome::kConnected}});
        ServiceAnswers answers;
        answers.rtsp_port = 554;
        answers.rtsp_ok = true;
        inputs.services[ip(address).value] = answers;
    }
    const CameraModeConfig config;
    const std::vector<DiscoveredCamera> cameras = mergeDiscovery(inputs, fieldLan(), config);
    CHECK_EQ(cameras.size(), std::size_t{4});
    for (int i = 0; i < 4; ++i) {
        const DiscoveredCamera& camera = cameras[static_cast<std::size_t>(i)];
        CHECK_EQ(anpr::net::toString(camera.ip), "192.168.10.2" + std::to_string(i + 1));
        CHECK_EQ(camera.mac, "44:19:b6:00:00:0" + std::to_string(i + 1));
        CHECK_EQ(camera.vendor, std::string("Hikvision"));
        CHECK(camera.isHikvision());
        CHECK_EQ(camera.model, std::string("DS-TCG406-E"));
        CHECK_EQ(camera.firmware, std::string("V5.5.110"));
        CHECK_EQ(camera.sources, static_cast<unsigned>(kFoundBySadp));
        CHECK(camera.on_camera_subnet);
        CHECK(camera.activated.has_value() && *camera.activated);
        CHECK(camera.rtsp_ok);
        CHECK_EQ(camera.rtsp_port, std::uint16_t{554});
        CHECK_EQ(camera.http_port, std::uint16_t{80});
        CHECK_EQ(camera.sdk_port, std::uint16_t{8000});
        CHECK_EQ(camera.ports.size(), std::size_t{3});
        CHECK(anyNoteContains(camera, "SADP answer"));
        // ONVIF ran and was silent: informational only, nothing else is wrong.
        CHECK_EQ(camera.problems.size(), std::size_t{1});
        CHECK(camera.hasProblem(CameraError::kOnvifDisabledOrUnavailable));
        CHECK(!camera.hasProblem(CameraError::kDuplicateIpDetected));
    }
}

TEST("discovery: ONVIF info is only reported when WS-Discovery ran") {
    DiscoveryInputs inputs;
    inputs.sadp.push_back(sadpCamera("192.168.10.21", "44:19:b6:00:00:01", "S1"));
    inputs.onvif_ran = false;
    CameraModeConfig config;
    std::vector<DiscoveredCamera> cameras = mergeDiscovery(inputs, fieldLan(), config);
    CHECK_EQ(cameras.size(), std::size_t{1});
    CHECK(cameras[0].problems.empty());

    inputs.onvif_ran = true;
    config.discovery.onvif = false;
    cameras = mergeDiscovery(inputs, fieldLan(), config);
    CHECK(cameras[0].problems.empty());
}

TEST("discovery: a SADP camera outside the camera subnet is reported, not contacted") {
    DiscoveryInputs inputs;
    anpr::hikvision::SadpDevice device = sadpCamera("192.168.1.64", "44:19:b6:00:00:40", "S64");
    inputs.sadp.push_back(device);
    inputs.notes[ip("192.168.1.64").value].push_back("not contacted: outside the camera subnet");
    const std::vector<DiscoveredCamera> cameras =
        mergeDiscovery(inputs, fieldLan(), CameraModeConfig{});
    CHECK_EQ(cameras.size(), std::size_t{1});
    const DiscoveredCamera& camera = cameras[0];
    CHECK(!camera.on_camera_subnet);
    CHECK(camera.hasProblem(CameraError::kCameraOnOtherSubnet));
    CHECK_EQ(problemDetail(camera, CameraError::kCameraOnOtherSubnet),
             std::string("SADP reports 192.168.1.64/255.255.255.0; the Jetson's camera LAN is "
                         "192.168.10.0/24"));
    // ONVIF would not reach it anyway: no misleading "ONVIF disabled" for it.
    CHECK(!camera.hasProblem(CameraError::kOnvifDisabledOrUnavailable));
    CHECK(camera.ports.empty());
    CHECK(anyNoteContains(camera, "not contacted"));
    CHECK_EQ(camera.rtsp_port, std::uint16_t{554});
}

TEST("discovery: two SADP answers for one address are a duplicate on both devices") {
    DiscoveryInputs inputs;
    inputs.sadp.push_back(sadpCamera("192.168.1.64", "44:19:b6:00:00:01", "SA"));
    inputs.sadp.push_back(sadpCamera("192.168.1.64", "44:19:b6:00:00:02", "SB"));
    inputs.sadp.push_back(sadpCamera("192.168.10.21", "44:19:b6:00:00:03", "SC"));
    const std::vector<DiscoveredCamera> cameras =
        mergeDiscovery(inputs, fieldLan(), CameraModeConfig{});
    CHECK_EQ(cameras.size(), std::size_t{3});
    const DiscoveredCamera* first = byMac(cameras, "44:19:b6:00:00:01");
    const DiscoveredCamera* second = byMac(cameras, "44:19:b6:00:00:02");
    const DiscoveredCamera* other = byMac(cameras, "44:19:b6:00:00:03");
    CHECK(first != nullptr && second != nullptr && other != nullptr);
    CHECK(first->hasProblem(CameraError::kDuplicateIpDetected));
    CHECK(second->hasProblem(CameraError::kDuplicateIpDetected));
    CHECK(!other->hasProblem(CameraError::kDuplicateIpDetected));
    CHECK_EQ(first->conflicting_macs.size(), std::size_t{1});
    CHECK_EQ(first->conflicting_macs[0], std::string("44:19:b6:00:00:02"));
    CHECK_EQ(second->conflicting_macs[0], std::string("44:19:b6:00:00:01"));
    const std::string detail = problemDetail(*first, CameraError::kDuplicateIpDetected);
    CHECK(contains(detail, "192.168.1.64"));
    CHECK(contains(detail, "44:19:b6:00:00:01 (SADP)"));
    CHECK(contains(detail, "44:19:b6:00:00:02 (SADP)"));
    // Both are factory-default cameras outside the subnet as well.
    CHECK(first->hasProblem(CameraError::kCameraOnOtherSubnet));
    CHECK(other->conflicting_macs.empty());
    // Sorted by address, then MAC.
    CHECK_EQ(cameras[0].mac, std::string("44:19:b6:00:00:01"));
    CHECK_EQ(cameras[1].mac, std::string("44:19:b6:00:00:02"));
    CHECK_EQ(cameras[2].mac, std::string("44:19:b6:00:00:03"));
}

TEST("discovery: ARP probe replies from a second MAC are a duplicate address") {
    DiscoveryInputs inputs;
    inputs.sadp.push_back(sadpCamera("192.168.10.21", "44:19:b6:00:00:01", "SA"));
    inputs.arp_probe[ip("192.168.10.21").value] = {"44:19:b6:00:00:01", "00-11-22-33-44-55"};
    // A single reply from the camera itself is not a conflict.
    inputs.sadp.push_back(sadpCamera("192.168.10.22", "44:19:b6:00:00:02", "SB"));
    inputs.arp_probe[ip("192.168.10.22").value] = {"44:19:b6:00:00:02"};
    const std::vector<DiscoveredCamera> cameras =
        mergeDiscovery(inputs, fieldLan(), CameraModeConfig{});
    CHECK_EQ(cameras.size(), std::size_t{2});
    CHECK(cameras[0].hasProblem(CameraError::kDuplicateIpDetected));
    CHECK_EQ(cameras[0].conflicting_macs.size(), std::size_t{1});
    CHECK_EQ(cameras[0].conflicting_macs[0], std::string("00:11:22:33:44:55"));
    CHECK(contains(problemDetail(cameras[0], CameraError::kDuplicateIpDetected), "ARP probe"));
    CHECK(!cameras[1].hasProblem(CameraError::kDuplicateIpDetected));
}

TEST("discovery: an inactive camera is reported as not activated") {
    DiscoveryInputs inputs;
    anpr::hikvision::SadpDevice device = sadpCamera("192.168.10.30", "44:19:b6:00:00:30", "SX");
    device.activated = false;
    inputs.sadp.push_back(device);
    anpr::hikvision::SadpDevice old_firmware =
        sadpCamera("192.168.10.31", "44:19:b6:00:00:31", "SY");
    old_firmware.activated.reset();
    inputs.sadp.push_back(old_firmware);
    const std::vector<DiscoveredCamera> cameras =
        mergeDiscovery(inputs, fieldLan(), CameraModeConfig{});
    CHECK_EQ(cameras.size(), std::size_t{2});
    CHECK(cameras[0].hasProblem(CameraError::kCameraNotActivated));
    CHECK(contains(problemDetail(cameras[0], CameraError::kCameraNotActivated),
                   "Activated=false"));
    CHECK(cameras[0].activated.has_value() && !*cameras[0].activated);
    CHECK(!cameras[1].hasProblem(CameraError::kCameraNotActivated));
    CHECK(!cameras[1].activated.has_value());
}

TEST("discovery: an ONVIF-only RTSP device of another vendor is a camera") {
    DiscoveryInputs inputs;
    anpr::hikvision::WsDiscoveryMatch match;
    match.from = ip("192.168.10.40");
    match.xaddr_host = ip("192.168.10.40");
    match.endpoint = "urn:uuid:5f5a69c2-e0ae-504f-829b-00aabbccdd40";
    match.xaddrs.push_back("http://192.168.10.40/onvif/device_service");
    match.scopes.push_back("onvif://www.onvif.org/hardware/IPC-HDW2431T");
    match.scopes.push_back("onvif://www.onvif.org/name/Dahua");
    match.hardware = "IPC-HDW2431T";
    match.name = "Dahua";
    inputs.onvif.push_back(match);
    addPorts(inputs, "192.168.10.40",
             {{554, ConnectOutcome::kConnected}, {80, ConnectOutcome::kConnected}});
    ServiceAnswers answers;
    answers.rtsp_port = 554;
    answers.rtsp_ok = true;
    answers.rtsp_server = "Rtsp Server/3.0";
    answers.http_server = "nginx";
    inputs.services[ip("192.168.10.40").value] = answers;

    const std::vector<DiscoveredCamera> cameras =
        mergeDiscovery(inputs, fieldLan(), CameraModeConfig{});
    CHECK_EQ(cameras.size(), std::size_t{1});
    const DiscoveredCamera& camera = cameras[0];
    CHECK(camera.vendor.empty());
    CHECK(!camera.isHikvision());
    CHECK(camera.onvif_seen);
    CHECK_EQ(camera.sources, static_cast<unsigned>(kFoundByOnvif));
    CHECK_EQ(camera.onvif_endpoint, match.endpoint);
    CHECK_EQ(camera.onvif_xaddr, match.xaddrs[0]);
    CHECK_EQ(camera.onvif_scopes.size(), std::size_t{2});
    CHECK_EQ(camera.model, std::string("IPC-HDW2431T"));
    CHECK(camera.rtsp_ok);
    CHECK_EQ(camera.rtsp_server, std::string("Rtsp Server/3.0"));
    CHECK_EQ(camera.http_server, std::string("nginx"));
    CHECK(camera.problems.empty());
}

TEST("discovery: ARP and scan devices count only with an RTSP port open") {
    DiscoveryInputs inputs;
    anpr::net::ArpEntry with_rtsp;
    with_rtsp.ip = ip("192.168.10.50");
    with_rtsp.mac = "00:aa:bb:cc:dd:50";
    with_rtsp.interface = "eth0";
    with_rtsp.complete = true;
    inputs.arp.push_back(with_rtsp);
    addPorts(inputs, "192.168.10.50",
             {{554, ConnectOutcome::kConnected}, {80, ConnectOutcome::kRefused}});

    anpr::net::ArpEntry web_only = with_rtsp;
    web_only.ip = ip("192.168.10.51");
    web_only.mac = "00:aa:bb:cc:dd:51";
    inputs.arp.push_back(web_only);
    addPorts(inputs, "192.168.10.51",
             {{554, ConnectOutcome::kRefused}, {80, ConnectOutcome::kConnected}});

    anpr::net::ArpEntry incomplete = with_rtsp;
    incomplete.ip = ip("192.168.10.52");
    incomplete.mac.clear();
    incomplete.complete = false;
    inputs.arp.push_back(incomplete);

    // Found only by the sweep; the scan saw RTSP on it twice (sweep, then candidate probe).
    addPorts(inputs, "192.168.10.60", {{554, ConnectOutcome::kTimeout}});
    addPorts(inputs, "192.168.10.60", {{554, ConnectOutcome::kConnected}});
    // A sweep hit without RTSP: a printer.
    addPorts(inputs, "192.168.10.61", {{80, ConnectOutcome::kConnected}});

    const std::vector<DiscoveredCamera> cameras =
        mergeDiscovery(inputs, fieldLan(), CameraModeConfig{});
    CHECK_EQ(cameras.size(), std::size_t{2});
    const DiscoveredCamera* arp_camera = byIp(cameras, "192.168.10.50");
    CHECK(arp_camera != nullptr);
    CHECK_EQ(arp_camera->mac, std::string("00:aa:bb:cc:dd:50"));
    CHECK((arp_camera->sources & kFoundByArp) != 0U);
    CHECK(arp_camera->vendor.empty());
    CHECK(arp_camera->ports.at(554) == ConnectOutcome::kConnected);
    CHECK(arp_camera->ports.at(80) == ConnectOutcome::kRefused);
    CHECK(arp_camera->problems.empty());

    const DiscoveredCamera* scan_camera = byIp(cameras, "192.168.10.60");
    CHECK(scan_camera != nullptr);
    CHECK_EQ(scan_camera->sources, static_cast<unsigned>(kFoundByScan));
    CHECK(scan_camera->mac.empty());
    CHECK(scan_camera->ports.at(554) == ConnectOutcome::kConnected);
    CHECK(byIp(cameras, "192.168.10.51") == nullptr);
    CHECK(byIp(cameras, "192.168.10.61") == nullptr);
}

TEST("discovery: a scan device takes its MAC from the neighbour table") {
    DiscoveryInputs inputs;
    addPorts(inputs, "192.168.10.70", {{554, ConnectOutcome::kConnected}});
    anpr::net::ArpEntry entry;
    entry.ip = ip("192.168.10.70");
    entry.mac = "44:19:b6:00:00:70";
    entry.interface = "eth0";
    entry.complete = true;
    inputs.arp.push_back(entry);
    const std::vector<DiscoveredCamera> cameras =
        mergeDiscovery(inputs, fieldLan(), CameraModeConfig{});
    CHECK_EQ(cameras.size(), std::size_t{1});
    CHECK_EQ(cameras[0].mac, std::string("44:19:b6:00:00:70"));
    // Hikvision OUI and nothing else: the weakest evidence, and it says so.
    CHECK_EQ(cameras[0].vendor, std::string("Hikvision"));
    CHECK(anyNoteContains(cameras[0], "OUI"));
    CHECK(anyNoteContains(cameras[0], "weakest"));
}

TEST("discovery: an ONVIF answer is merged with the SADP answer of the same MAC") {
    DiscoveryInputs inputs;
    inputs.sadp.push_back(sadpCamera("192.168.10.21", "44:19:b6:00:00:21", "S21"));
    anpr::hikvision::WsDiscoveryMatch match;
    match.from = ip("192.168.10.21");
    match.xaddr_host = ip("192.168.10.21");
    match.endpoint = "urn:uuid:a9b3c4d5-e6f7-11e9-8000-4419b6000021";
    match.mac = "44:19:b6:00:00:21";
    match.hardware = "DS-TCG406-E";
    match.xaddrs.push_back("http://192.168.10.21/onvif/device_service");
    inputs.onvif.push_back(match);
    // The neighbour table agrees with both.
    anpr::net::ArpEntry entry;
    entry.ip = ip("192.168.10.21");
    entry.mac = "44:19:B6:00:00:21";
    entry.interface = "eth0";
    entry.complete = true;
    inputs.arp.push_back(entry);
    // Another MAC for the same address in the neighbour table (stale, or a second device the
    // ARP probe would reveal) is a remark, not a second camera.
    anpr::net::ArpEntry stale = entry;
    stale.mac = "00:11:22:33:44:99";
    inputs.arp.push_back(stale);

    const std::vector<DiscoveredCamera> cameras =
        mergeDiscovery(inputs, fieldLan(), CameraModeConfig{});
    CHECK_EQ(cameras.size(), std::size_t{1});
    const DiscoveredCamera& camera = cameras[0];
    CHECK_EQ(camera.sources, static_cast<unsigned>(kFoundBySadp | kFoundByOnvif | kFoundByArp));
    CHECK(camera.onvif_seen);
    CHECK_EQ(camera.serial, std::string("S21"));
    CHECK_EQ(camera.onvif_endpoint, match.endpoint);
    CHECK(!camera.hasProblem(CameraError::kOnvifDisabledOrUnavailable));
    CHECK(!camera.hasProblem(CameraError::kDuplicateIpDetected));
    CHECK(anyNoteContains(camera, "neighbour table shows 00:11:22:33:44:99"));
}

TEST("discovery: an ONVIF MAC that SADP does not know is not a second device") {
    DiscoveryInputs inputs;
    inputs.sadp.push_back(sadpCamera("192.168.10.21", "44:19:b6:00:00:21", "S21"));
    anpr::hikvision::WsDiscoveryMatch match;
    match.from = ip("192.168.10.21");
    match.endpoint = "urn:uuid:a9b3c4d5-e6f7-11e9-8000-0123456789ab";
    match.mac = "01:23:45:67:89:ab";
    inputs.onvif.push_back(match);
    const std::vector<DiscoveredCamera> cameras =
        mergeDiscovery(inputs, fieldLan(), CameraModeConfig{});
    CHECK_EQ(cameras.size(), std::size_t{1});
    CHECK_EQ(cameras[0].mac, std::string("44:19:b6:00:00:21"));
    CHECK(cameras[0].onvif_seen);
    CHECK(!cameras[0].hasProblem(CameraError::kDuplicateIpDetected));
    CHECK(anyNoteContains(cameras[0], "ONVIF reports MAC 01:23:45:67:89:ab"));
}

TEST("discovery: an ONVIF answer without MAC joins the device known at its address") {
    DiscoveryInputs inputs;
    anpr::net::ArpEntry entry;
    entry.ip = ip("192.168.10.33");
    entry.mac = "00:11:22:33:44:33";
    entry.interface = "eth0";
    entry.complete = true;
    inputs.arp.push_back(entry);
    anpr::hikvision::WsDiscoveryMatch match;
    match.from = ip("192.168.10.33");
    match.endpoint = "urn:uuid:11111111-2222-3333-4444-555555555555";
    inputs.onvif.push_back(match);
    const std::vector<DiscoveredCamera> cameras =
        mergeDiscovery(inputs, fieldLan(), CameraModeConfig{});
    CHECK_EQ(cameras.size(), std::size_t{1});
    CHECK_EQ(cameras[0].mac, std::string("00:11:22:33:44:33"));
    CHECK_EQ(cameras[0].sources, static_cast<unsigned>(kFoundByOnvif | kFoundByArp));
}

TEST("discovery: vendor evidence from ONVIF, HTTP, the RTSP realm and the OUI") {
    DiscoveryInputs inputs;
    const auto rtsp_device = [&inputs](const std::string& address) {
        addPorts(inputs, address, {{554, ConnectOutcome::kConnected}});
    };
    // ONVIF hardware scope with a DS- model, no MAC.
    anpr::hikvision::WsDiscoveryMatch onvif_model;
    onvif_model.from = ip("192.168.10.11");
    onvif_model.hardware = "DS-2CD2386G2-IU";
    inputs.onvif.push_back(onvif_model);
    // ONVIF scope naming the maker.
    anpr::hikvision::WsDiscoveryMatch onvif_scope;
    onvif_scope.from = ip("192.168.10.12");
    onvif_scope.scopes.push_back("onvif://www.onvif.org/name/HIKVISION%20Camera");
    inputs.onvif.push_back(onvif_scope);
    // HTTP Server headers of the Hikvision web stacks.
    const std::vector<std::pair<std::string, std::string>> servers = {
        {"192.168.10.13", "webserver"},
        {"192.168.10.14", "App-webs/"},
        {"192.168.10.15", "DNVRS-Webs"},
        {"192.168.10.16", "Hikvision-Webs"},
    };
    for (const auto& server : servers) {
        rtsp_device(server.first);
        inputs.services[ip(server.first).value].http_server = server.second;
    }
    // RTSP realms.
    const std::vector<std::pair<std::string, std::string>> realms = {
        {"192.168.10.17", "IP Camera(G6822)"},
        {"192.168.10.18", "Hikvision"},
        {"192.168.10.19", "DS-TCG406-E"},
    };
    for (const auto& realm : realms) {
        rtsp_device(realm.first);
        inputs.services[ip(realm.first).value].rtsp_realm = realm.second;
    }
    // Nothing that says Hikvision.
    rtsp_device("192.168.10.20");
    inputs.services[ip("192.168.10.20").value].http_server = "lighttpd";
    inputs.services[ip("192.168.10.20").value].rtsp_realm = "Streaming Server";

    const std::vector<DiscoveredCamera> cameras =
        mergeDiscovery(inputs, fieldLan(), CameraModeConfig{});
    CHECK_EQ(cameras.size(), std::size_t{10});
    const DiscoveredCamera* model = byIp(cameras, "192.168.10.11");
    CHECK(model != nullptr && model->isHikvision());
    CHECK_EQ(model->model, std::string("DS-2CD2386G2-IU"));
    CHECK(anyNoteContains(*model, "ONVIF"));
    CHECK(byIp(cameras, "192.168.10.12")->isHikvision());
    for (const auto& server : servers) {
        const DiscoveredCamera* camera = byIp(cameras, server.first);
        CHECK(camera != nullptr && camera->isHikvision());
        CHECK(anyNoteContains(*camera, "HTTP Server header"));
    }
    for (const auto& realm : realms) {
        const DiscoveredCamera* camera = byIp(cameras, realm.first);
        CHECK(camera != nullptr && camera->isHikvision());
        CHECK(anyNoteContains(*camera, "RTSP realm"));
    }
    // A realm that is a model number also names the model.
    CHECK_EQ(byIp(cameras, "192.168.10.19")->model, std::string("DS-TCG406-E"));
    const DiscoveredCamera* unknown = byIp(cameras, "192.168.10.20");
    CHECK(unknown != nullptr && unknown->vendor.empty());
    CHECK(!anyNoteContains(*unknown, "vendor"));
    // A non-Hikvision camera never gets the Hikvision-only ONVIF remark.
    CHECK(!unknown->hasProblem(CameraError::kOnvifDisabledOrUnavailable));
    CHECK(byIp(cameras, "192.168.10.13")->hasProblem(CameraError::kOnvifDisabledOrUnavailable));
}

TEST("discovery: the RTSP port follows overrides, listed ports and what is open") {
    CameraModeConfig config;
    CameraOverride by_ip;
    by_ip.ip = "192.168.10.24";
    by_ip.rtsp_port = 10554;
    config.cameras.push_back(by_ip);
    config.rtsp.port = 8554;

    DiscoveryInputs inputs;
    // Configured port open.
    inputs.sadp.push_back(sadpCamera("192.168.10.21", "44:19:b6:00:00:21", "S21"));
    addPorts(inputs, "192.168.10.21",
             {{8554, ConnectOutcome::kConnected}, {554, ConnectOutcome::kConnected}});
    // Only the default port open.
    inputs.sadp.push_back(sadpCamera("192.168.10.22", "44:19:b6:00:00:22", "S22"));
    addPorts(inputs, "192.168.10.22",
             {{8554, ConnectOutcome::kRefused}, {554, ConnectOutcome::kConnected}});
    // Listed with its own port, which is closed now: still that port.
    inputs.listed.push_back({{ip("192.168.10.23"), std::uint16_t{9554}}, kFoundByManual});
    addPorts(inputs, "192.168.10.23", {{9554, ConnectOutcome::kRefused}});
    // Override by address.
    inputs.sadp.push_back(sadpCamera("192.168.10.24", "44:19:b6:00:00:24", "S24"));
    addPorts(inputs, "192.168.10.24", {{554, ConnectOutcome::kConnected}});
    // Nothing open: the configured port.
    inputs.sadp.push_back(sadpCamera("192.168.10.25", "44:19:b6:00:00:25", "S25"));

    const std::vector<DiscoveredCamera> cameras = mergeDiscovery(inputs, fieldLan(), config);
    CHECK_EQ(cameras.size(), std::size_t{5});
    CHECK_EQ(byIp(cameras, "192.168.10.21")->rtsp_port, std::uint16_t{8554});
    CHECK_EQ(byIp(cameras, "192.168.10.22")->rtsp_port, std::uint16_t{554});
    CHECK_EQ(byIp(cameras, "192.168.10.23")->rtsp_port, std::uint16_t{9554});
    CHECK_EQ(byIp(cameras, "192.168.10.24")->rtsp_port, std::uint16_t{10554});
    CHECK_EQ(byIp(cameras, "192.168.10.25")->rtsp_port, std::uint16_t{8554});
}

TEST("discovery: listed hosts stay in the report while silent") {
    DiscoveryInputs inputs;
    inputs.listed.push_back({{ip("192.168.10.90"), std::uint16_t{554}}, kFoundByManual});
    addPorts(inputs, "192.168.10.90",
             {{554, ConnectOutcome::kTimeout}, {80, ConnectOutcome::kTimeout}});
    // A registry address that does not answer is not kept: the camera may have moved.
    inputs.listed.push_back({{ip("192.168.10.91"), std::uint16_t{554}}, kFoundByRegistry});
    addPorts(inputs, "192.168.10.91", {{554, ConnectOutcome::kTimeout}});
    // A registry address that answers RTSP is.
    inputs.listed.push_back({{ip("192.168.10.92"), std::uint16_t{554}}, kFoundByRegistry});
    addPorts(inputs, "192.168.10.92", {{554, ConnectOutcome::kConnected}});
    // A listed host that SADP also found carries both bits.
    inputs.sadp.push_back(sadpCamera("192.168.10.93", "44:19:b6:00:00:93", "S93"));
    inputs.listed.push_back({{ip("192.168.10.93"), std::uint16_t{554}}, kFoundByRegistry});

    const std::vector<DiscoveredCamera> cameras =
        mergeDiscovery(inputs, fieldLan(), CameraModeConfig{});
    CHECK_EQ(cameras.size(), std::size_t{3});
    const DiscoveredCamera* manual = byIp(cameras, "192.168.10.90");
    CHECK(manual != nullptr);
    CHECK_EQ(manual->sources, static_cast<unsigned>(kFoundByManual));
    CHECK(anyNoteContains(*manual, "does not answer"));
    CHECK(byIp(cameras, "192.168.10.91") == nullptr);
    CHECK_EQ(byIp(cameras, "192.168.10.92")->sources, static_cast<unsigned>(kFoundByRegistry));
    CHECK_EQ(byIp(cameras, "192.168.10.93")->sources,
             static_cast<unsigned>(kFoundBySadp | kFoundByRegistry));
}

// ---------------------------------------------------------------------------------------------
// assignIds / report
// ---------------------------------------------------------------------------------------------

TEST("discovery: ids stay with the device when addresses change") {
    const CameraModeConfig config;
    CameraRegistry registry;

    DiscoveryInputs first_inputs;
    first_inputs.sadp.push_back(sadpCamera("192.168.10.21", "44:19:b6:00:00:01", "SA"));
    first_inputs.sadp.push_back(sadpCamera("192.168.10.22", "44:19:b6:00:00:02", "SB"));
    first_inputs.sadp.push_back(sadpCamera("192.168.10.23", "44:19:b6:00:00:03", "SC"));
    DiscoveryReport first;
    first.cameras = mergeDiscovery(first_inputs, fieldLan(), config);
    assignIds(first, registry, config, "2026-10-09T10:00:00Z");
    CHECK_EQ(first.cameras.size(), std::size_t{3});
    CHECK_EQ(first.cameras[0].id, std::string("camera-01"));
    CHECK_EQ(first.cameras[0].mac, std::string("44:19:b6:00:00:01"));
    CHECK_EQ(first.cameras[2].id, std::string("camera-03"));
    CHECK(first.problems.empty());

    // The registry survives a save/load.
    std::string error;
    CameraRegistry reloaded = CameraRegistry::parse(registry.serialize(), error);
    CHECK(error.empty());

    // Addresses rotated and a new camera appeared at the lowest address.
    DiscoveryInputs second_inputs;
    second_inputs.sadp.push_back(sadpCamera("192.168.10.40", "44:19:b6:00:00:01", "SA"));
    second_inputs.sadp.push_back(sadpCamera("192.168.10.21", "44:19:b6:00:00:02", "SB"));
    second_inputs.sadp.push_back(sadpCamera("192.168.10.22", "44:19:b6:00:00:03", "SC"));
    second_inputs.sadp.push_back(sadpCamera("192.168.10.10", "44:19:b6:00:00:04", "SD"));
    DiscoveryReport second;
    second.cameras = mergeDiscovery(second_inputs, fieldLan(), config);
    assignIds(second, reloaded, config, "2026-10-09T11:00:00Z");
    CHECK_EQ(second.cameras.size(), std::size_t{4});
    CHECK_EQ(second.cameras[0].id, std::string("camera-01"));
    CHECK_EQ(second.cameras[0].mac, std::string("44:19:b6:00:00:01"));
    CHECK_EQ(anpr::net::toString(second.cameras[0].ip), std::string("192.168.10.40"));
    CHECK_EQ(second.cameras[1].id, std::string("camera-02"));
    CHECK_EQ(second.cameras[1].mac, std::string("44:19:b6:00:00:02"));
    CHECK_EQ(second.cameras[2].id, std::string("camera-03"));
    CHECK_EQ(second.cameras[3].id, std::string("camera-04"));
    CHECK_EQ(second.cameras[3].mac, std::string("44:19:b6:00:00:04"));
    const RegistryEntry* entry = reloaded.find("camera-01");
    CHECK(entry != nullptr);
    CHECK_EQ(entry->last_ip, std::string("192.168.10.40"));
    CHECK_EQ(entry->serial, std::string("SA"));
}

TEST("discovery: ONVIF endpoint UUIDs identify MAC-less devices") {
    const CameraModeConfig config;
    CameraRegistry registry;
    const auto run = [&](const std::string& address) {
        DiscoveryInputs inputs;
        anpr::hikvision::WsDiscoveryMatch match;
        match.from = ip(address);
        match.endpoint = "urn:uuid:ABCDEF01-2222-3333-4444-555555555555";
        inputs.onvif.push_back(match);
        DiscoveryReport report;
        report.cameras = mergeDiscovery(inputs, fieldLan(), config);
        assignIds(report, registry, config, "2026-10-09T10:00:00Z");
        return report;
    };
    const DiscoveryReport first = run("192.168.10.21");
    const DiscoveryReport second = run("192.168.10.77");
    CHECK_EQ(first.cameras.size(), std::size_t{1});
    CHECK_EQ(second.cameras.size(), std::size_t{1});
    CHECK_EQ(first.cameras[0].id, second.cameras[0].id);
    CHECK_EQ(registry.entries().size(), std::size_t{1});
    CHECK_EQ(registry.entries()[0].onvif_uuid,
             std::string("abcdef01-2222-3333-4444-555555555555"));
    CHECK_EQ(registry.lastMatchKey(), std::string("onvif_uuid"));
}

TEST("discovery: max_cameras caps the report") {
    CameraModeConfig config;
    config.discovery.max_cameras = 2;
    CameraRegistry registry;
    DiscoveryInputs inputs;
    for (int i = 1; i <= 3; ++i) {
        inputs.sadp.push_back(sadpCamera("192.168.10.2" + std::to_string(i),
                                         "44:19:b6:00:00:0" + std::to_string(i),
                                         "S" + std::to_string(i)));
    }
    DiscoveryReport report;
    report.cameras = mergeDiscovery(inputs, fieldLan(), config);
    assignIds(report, registry, config, "2026-10-09T10:00:00Z");
    CHECK_EQ(report.cameras.size(), std::size_t{2});
    CHECK_EQ(report.cameras[1].id, std::string("camera-02"));
    CHECK_EQ(report.problems.size(), std::size_t{1});
    CHECK(report.problems[0].first == CameraError::kCameraLimitReached);
    CHECK(contains(report.problems[0].second, "camera-03 (192.168.10.23)"));
    CHECK(contains(report.problems[0].second, "max_cameras is 2"));
    // The ignored camera still has its reserved id.
    CHECK_EQ(registry.entries().size(), std::size_t{3});
}

TEST("discovery: an empty result is NO_CAMERAS_DISCOVERED") {
    CameraRegistry registry;
    DiscoveryReport report;
    DiscoveryMethodReport sadp;
    sadp.method = "sadp";
    sadp.ran = true;
    report.methods.push_back(sadp);
    DiscoveryMethodReport scan;
    scan.method = "scan";
    scan.ran = false;
    report.methods.push_back(scan);
    assignIds(report, registry, CameraModeConfig{}, "2026-10-09T10:00:00Z");
    CHECK(report.cameras.empty());
    CHECK_EQ(report.problems.size(), std::size_t{1});
    CHECK(report.problems[0].first == CameraError::kNoCamerasDiscovered);
    CHECK(contains(report.problems[0].second, "sadp"));
    CHECK(!contains(report.problems[0].second, "scan"));
}

TEST("discovery: the scan report lists cameras, problems and methods") {
    CameraModeConfig config;
    CameraRegistry registry;
    DiscoveryInputs inputs;
    inputs.sadp.push_back(sadpCamera("192.168.10.21", "44:19:b6:00:00:21", "DS-TCG406-E2025"));
    addPorts(inputs, "192.168.10.21",
             {{554, ConnectOutcome::kConnected},
              {80, ConnectOutcome::kConnected},
              {8000, ConnectOutcome::kRefused}});
    inputs.services[ip("192.168.10.21").value].rtsp_ok = true;
    inputs.services[ip("192.168.10.21").value].rtsp_port = 554;
    inputs.sadp.push_back(sadpCamera("192.168.1.64", "44:19:b6:00:00:64", "DS-FACTORY"));

    DiscoveryReport report;
    report.cameras = mergeDiscovery(inputs, fieldLan(), config);
    DiscoveryMethodReport sadp;
    sadp.method = "sadp";
    sadp.ran = true;
    sadp.found = 2;
    report.methods.push_back(sadp);
    DiscoveryMethodReport scan;
    scan.method = "scan";
    scan.detail = "SADP and ONVIF found every known camera";
    report.methods.push_back(scan);
    assignIds(report, registry, config, "2026-10-09T10:00:00Z");

    const std::string text = formatDiscoveryReport(report);
    CHECK(contains(text, "DISCOVERED CAMERAS: 2\n"));
    CHECK(contains(text, "\ncamera-01\n"));
    CHECK(contains(text, "Model: DS-TCG406-E   Vendor: Hikvision"));
    CHECK(contains(text, "IP: 192.168.1.64   MAC: 44:19:b6:00:00:64   Serial: DS-FACTORY"));
    CHECK(contains(text, "Found by: sadp"));
    CHECK(contains(text, "Ports: 554 open, 80 open, 8000 closed"));
    CHECK(contains(text, "RTSP: OPTIONS ok on port 554"));
    CHECK(contains(text, "ONVIF: not answering"));
    CHECK(contains(text, "Ports: not probed (outside the camera subnet)"));
    CHECK(contains(text, "  ERROR CAMERA_ON_OTHER_SUBNET  camera-01 192.168.1.64\n"));
    CHECK(contains(text, "SADP reports 192.168.1.64/255.255.255.0"));
    CHECK(contains(text, "  INFO ONVIF_DISABLED_OR_UNAVAILABLE"));
    CHECK(contains(text, "DISCOVERY METHODS\n"));
    CHECK(contains(text, "  sadp: 2 found\n"));
    CHECK(contains(text, "  scan: skipped (SADP and ONVIF found every known camera)\n"));
    CHECK(contains(text, "SADP: activated, DHCP off, mask 255.255.255.0"));
}

// ---------------------------------------------------------------------------------------------
// buildTargets / classifyRtspProbe / checkRtsp
// ---------------------------------------------------------------------------------------------

TEST("preflight: targets apply overrides and resolve credentials from the environment") {
    CameraModeConfig config;
    config.decode.decoder = DecoderPreference::kAuto;
    config.native_anpr.enabled = false;
    CameraOverride by_ip;
    by_ip.ip = "192.168.10.22";
    by_ip.enabled = false;
    by_ip.rtsp_port = 8554;
    by_ip.stream = StreamSelection::kSub;
    by_ip.http_port = 8080;
    by_ip.decoder = DecoderPreference::kSoftware;
    by_ip.native_anpr = true;
    by_ip.anpr_config = "config/gate-exit.yaml";
    by_ip.username_env = "GATE_USER";
    by_ip.password_env = "GATE_PASS";
    config.cameras.push_back(by_ip);
    CameraOverride by_mac;
    by_mac.mac = "44-19-B6-00-00-23";
    by_mac.rtsp_path = "/ISAPI/Streaming/channels/103";
    config.cameras.push_back(by_mac);

    DiscoveryReport report;
    const auto add = [&report](const std::string& id, const std::string& address,
                               const std::string& mac) {
        DiscoveredCamera camera;
        camera.id = id;
        camera.ip = ip(address);
        camera.mac = mac;
        camera.vendor = "Hikvision";
        camera.model = "DS-TCG406-E";
        camera.serial = "S-" + id;
        camera.rtsp_port = 554;
        camera.http_port = 80;
        report.cameras.push_back(camera);
    };
    add("camera-10", "192.168.10.30", "44:19:b6:00:00:30");
    add("camera-03", "192.168.10.23", "44:19:b6:00:00:23");
    add("camera-02", "192.168.10.22", "44:19:b6:00:00:22");
    add("camera-01", "192.168.10.21", "44:19:b6:00:00:21");
    report.cameras[0].problems.emplace_back(CameraError::kCameraNotActivated, "SADP reports ...");
    report.cameras[0].problems.emplace_back(CameraError::kOnvifDisabledOrUnavailable, "no answer");

    FakeEnv env;
    env.values["HIKVISION_USERNAME"] = "admin";
    env.values["HIKVISION_PASSWORD"] = kPassword;
    env.values["GATE_USER"] = "gate";
    env.values["GATE_PASS"] = "gate-secret";
    env.values["HIKVISION_USERNAME_CAMERA_03"] = "operator";
    env.values["HIKVISION_PASSWORD_CAMERA_03"] = "op-secret";

    const std::vector<CameraTarget> targets = buildTargets(report, config, env.lookup());
    CHECK_EQ(targets.size(), std::size_t{4});
    CHECK_EQ(targets[0].id, std::string("camera-01"));
    CHECK_EQ(targets[1].id, std::string("camera-02"));
    CHECK_EQ(targets[2].id, std::string("camera-03"));
    CHECK_EQ(targets[3].id, std::string("camera-10"));

    const CameraTarget& plain = targets[0];
    CHECK(plain.enabled);
    CHECK_EQ(plain.rtsp_port, std::uint16_t{554});
    CHECK_EQ(plain.rtsp_path, std::string("/Streaming/Channels/101"));
    CHECK(plain.stream == StreamSelection::kMain);
    CHECK(plain.decoder == DecoderPreference::kAuto);
    CHECK(!plain.native_anpr);
    CHECK(plain.has_credentials);
    CHECK_EQ(plain.credentials.username, std::string("admin"));
    CHECK_EQ(plain.credentials.password, std::string(kPassword));
    CHECK_EQ(plain.credential_source, std::string("HIKVISION_USERNAME/HIKVISION_PASSWORD"));
    CHECK_EQ(plain.redactedUrl(),
             std::string("rtsp://<redacted>@192.168.10.21:554/Streaming/Channels/101"));
    CHECK(!contains(plain.redactedUrl(), "s3cr3t"));
    CHECK_EQ(plain.vendor, std::string("Hikvision"));
    CHECK_EQ(plain.serial, std::string("S-camera-01"));

    const CameraTarget& gate = targets[1];
    CHECK(!gate.enabled);
    CHECK_EQ(gate.rtsp_port, std::uint16_t{8554});
    CHECK(gate.stream == StreamSelection::kSub);
    CHECK_EQ(gate.rtsp_path, std::string("/Streaming/Channels/102"));
    CHECK_EQ(gate.http_port, std::uint16_t{8080});
    CHECK(gate.decoder == DecoderPreference::kSoftware);
    CHECK(gate.native_anpr);
    CHECK_EQ(gate.anpr_config, std::string("config/gate-exit.yaml"));
    CHECK_EQ(gate.credentials.username, std::string("gate"));
    CHECK_EQ(gate.credential_source, std::string("GATE_USER/GATE_PASS"));

    const CameraTarget& by_mac_target = targets[2];
    CHECK_EQ(by_mac_target.rtsp_path, std::string("/ISAPI/Streaming/channels/103"));
    CHECK_EQ(by_mac_target.credentials.username, std::string("operator"));
    CHECK_EQ(by_mac_target.credential_source,
             std::string("HIKVISION_USERNAME_CAMERA_03/HIKVISION_PASSWORD_CAMERA_03"));

    // Inactive: still enabled, the problem travels with the target; the info does not.
    const CameraTarget& inactive = targets[3];
    CHECK(inactive.enabled);
    CHECK_EQ(inactive.discovery_problems.size(), std::size_t{1});
    CHECK(inactive.discovery_problems[0].first == CameraError::kCameraNotActivated);

    // Without any variable set: no credentials, URL without the redaction marker.
    const std::vector<CameraTarget> bare = buildTargets(report, config, FakeEnv{}.lookup());
    CHECK(!bare[0].has_credentials);
    CHECK(bare[0].credentials.empty());
    CHECK(bare[0].credential_source.empty());
    CHECK_EQ(bare[0].redactedUrl(),
             std::string("rtsp://192.168.10.21:554/Streaming/Channels/101"));
}

TEST("preflight: RTSP probe results map to the diagnostic codes") {
    struct Row {
        RtspProbeStatus status;
        ConnectOutcome connect;
        bool credentials;
        CameraError expected;
    };
    const std::vector<Row> rows = {
        {RtspProbeStatus::kOk, ConnectOutcome::kConnected, true, CameraError::kNone},
        {RtspProbeStatus::kUnreachable, ConnectOutcome::kTimeout, true,
         CameraError::kCameraUnreachable},
        {RtspProbeStatus::kUnreachable, ConnectOutcome::kUnreachable, false,
         CameraError::kCameraUnreachable},
        {RtspProbeStatus::kPortClosed, ConnectOutcome::kRefused, true,
         CameraError::kRtspPortClosed},
        {RtspProbeStatus::kTimeout, ConnectOutcome::kTimeout, true,
         CameraError::kCameraUnreachable},
        {RtspProbeStatus::kTimeout, ConnectOutcome::kConnected, true,
         CameraError::kRtspProtocolError},
        {RtspProbeStatus::kAuthRequired, ConnectOutcome::kConnected, false,
         CameraError::kRtspCredentialsMissing},
        {RtspProbeStatus::kAuthRequired, ConnectOutcome::kConnected, true,
         CameraError::kRtspAuthFailed},
        {RtspProbeStatus::kAuthFailed, ConnectOutcome::kConnected, true,
         CameraError::kRtspAuthFailed},
        {RtspProbeStatus::kForbidden, ConnectOutcome::kConnected, true,
         CameraError::kRtspAuthFailed},
        {RtspProbeStatus::kForbidden, ConnectOutcome::kConnected, false,
         CameraError::kRtspCredentialsMissing},
        {RtspProbeStatus::kPathInvalid, ConnectOutcome::kConnected, true,
         CameraError::kRtspStreamPathInvalid},
        {RtspProbeStatus::kServerError, ConnectOutcome::kConnected, true,
         CameraError::kRtspProtocolError},
        {RtspProbeStatus::kProtocolError, ConnectOutcome::kConnected, false,
         CameraError::kRtspProtocolError},
    };
    for (const Row& row : rows) {
        anpr::net::RtspProbeResult probe;
        probe.status = row.status;
        probe.connect = row.connect;
        CHECK_EQ(toString(classifyRtspProbe(probe, row.credentials)), toString(row.expected));
    }
}

TEST("preflight: checkRtsp against a local camera: OK with credentials, H.264") {
    const FakeRtspServer server(FakeRtspServer::Mode::kDigestOk);
    const CameraTarget target = loopbackTarget(server.port(), true);
    const RtspCheck check = checkRtsp(target, loopbackSnapshot(), loopbackLan(), 2000);
    CHECK_EQ(toString(check.error), std::string("NONE"));
    CHECK(check.network_reachable);
    CHECK(check.rtsp_port_open);
    CHECK(check.auth_ok);
    CHECK(check.path_ok);
    CHECK(check.probe.auth_attempted);
    CHECK_EQ(check.probe.realm, std::string("IP Camera(G1234)"));
    CHECK(check.video.present);
    CHECK(check.video.codec == VideoCodec::kH264);
    CHECK(contains(check.detail, "rtsp://<redacted>@127.0.0.1:"));
    CHECK(!contains(check.detail, "s3cr3t"));
    CHECK(server.sawAuthorization());
}

TEST("preflight: checkRtsp classifies a missing login, a rejected login and a wrong path") {
    {
        const FakeRtspServer server(FakeRtspServer::Mode::kDigestOk);
        const RtspCheck check = checkRtsp(loopbackTarget(server.port(), false),
                                          loopbackSnapshot(), loopbackLan(), 2000);
        CHECK_EQ(toString(check.error), std::string("RTSP_CREDENTIALS_MISSING"));
        CHECK(check.rtsp_port_open);
        CHECK(!check.auth_ok);
        CHECK(!server.sawAuthorization());
    }
    {
        const FakeRtspServer server(FakeRtspServer::Mode::kAlwaysUnauthorized);
        const RtspCheck check = checkRtsp(loopbackTarget(server.port(), true),
                                          loopbackSnapshot(), loopbackLan(), 2000);
        CHECK_EQ(toString(check.error), std::string("RTSP_AUTH_FAILED"));
        CHECK(!check.auth_ok);
        CHECK(!contains(check.detail, "s3cr3t"));
        // Exactly one DESCRIBE without and one with credentials: never retried.
        const std::vector<std::string> methods = server.methods();
        CHECK_EQ(methods.size(), std::size_t{2});
    }
    {
        const FakeRtspServer server(FakeRtspServer::Mode::kNotFound);
        const RtspCheck check = checkRtsp(loopbackTarget(server.port(), true),
                                          loopbackSnapshot(), loopbackLan(), 2000);
        CHECK_EQ(toString(check.error), std::string("RTSP_STREAM_PATH_INVALID"));
        CHECK(check.auth_ok);
        CHECK(!check.path_ok);
    }
    {
        const FakeRtspServer server(FakeRtspServer::Mode::kMjpeg);
        const RtspCheck check = checkRtsp(loopbackTarget(server.port(), true),
                                          loopbackSnapshot(), loopbackLan(), 2000);
        CHECK_EQ(toString(check.error), std::string("UNSUPPORTED_CODEC"));
        CHECK(check.video.codec == VideoCodec::kMjpeg);
        CHECK(contains(check.detail, "MJPEG"));
    }
}

TEST("preflight: checkRtsp tells a closed RTSP port from an unreachable camera") {
    const CameraTarget target = loopbackTarget(closedPort(), true);
    const RtspCheck check = checkRtsp(target, loopbackSnapshot(), loopbackLan(), 2000);
    CHECK_EQ(toString(check.error), std::string("RTSP_PORT_CLOSED"));
    CHECK(check.network_reachable);
    CHECK(!check.rtsp_port_open);
    CHECK(check.probe.status == RtspProbeStatus::kPortClosed);
    CHECK(contains(check.detail, "127.0.0.1"));
}

TEST("preflight: checkRtsp never contacts an address routed through another interface") {
    const FakeRtspServer server(FakeRtspServer::Mode::kDigestOk);
    CameraTarget target = loopbackTarget(server.port(), true);
    target.ip = ip("10.255.255.1");
    const auto start = std::chrono::steady_clock::now();
    const RtspCheck routed = checkRtsp(target, loopbackSnapshot(), loopbackLan(), 2000);
    CHECK_EQ(toString(routed.error), std::string("CAMERA_ON_OTHER_SUBNET"));
    CHECK(contains(routed.detail, "wwan0"));
    CHECK(!routed.network_reachable);
    CHECK(routed.probe.connect == ConnectOutcome::kError);
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(500));

    // No camera LAN at all.
    anpr::net::CameraLanSelection none;
    const RtspCheck no_lan = checkRtsp(loopbackTarget(server.port(), true), loopbackSnapshot(),
                                       none, 2000);
    CHECK_EQ(toString(no_lan.error), std::string("CAMERA_ON_OTHER_SUBNET"));

    // Discovery already found the camera inactive or in another subnet: not contacted either.
    CameraTarget inactive = loopbackTarget(server.port(), true);
    inactive.discovery_problems.emplace_back(CameraError::kCameraNotActivated,
                                             "SADP reports Activated=false");
    const RtspCheck blocked = checkRtsp(inactive, loopbackSnapshot(), loopbackLan(), 2000);
    CHECK_EQ(toString(blocked.error), std::string("CAMERA_NOT_ACTIVATED"));
    CHECK(contains(blocked.detail, "Activated=false"));

    // A duplicate address does not block the check.
    CameraTarget duplicate = loopbackTarget(server.port(), true);
    duplicate.discovery_problems.emplace_back(CameraError::kDuplicateIpDetected, "two MACs");
    const RtspCheck allowed = checkRtsp(duplicate, loopbackSnapshot(), loopbackLan(), 2000);
    CHECK_EQ(toString(allowed.error), std::string("NONE"));
    CHECK_EQ(server.connections(), 1);
}

// ---------------------------------------------------------------------------------------------
// discoverCameras end to end on loopback (discovery disabled: listed hosts only)
// ---------------------------------------------------------------------------------------------

TEST("discovery: listed hosts are identified without credentials and get stable ids") {
    const LogLevelGuard quiet(anpr::LogLevel::kError);
    const FakeRtspServer server(FakeRtspServer::Mode::kDigestOk);
    CameraModeConfig config;
    config.discovery.enabled = false;
    config.discovery.duplicate_ip_check = false;
    config.discovery.connect_timeout_ms = 300;
    config.rtsp.timeout_ms = 1500;
    config.discovery.manual_hosts = {"127.0.0.1:" + std::to_string(server.port()), "10.1.2.3"};
    CameraRegistry registry;

    const DiscoveryReport report =
        discoverCameras(loopbackSnapshot(), loopbackLan(), config, registry);
    CHECK_EQ(report.cameras.size(), std::size_t{2});
    const DiscoveredCamera* local = byIp(report.cameras, "127.0.0.1");
    CHECK(local != nullptr);
    CHECK(local->rtsp_ok);
    CHECK_EQ(local->rtsp_port, server.port());
    CHECK_EQ(local->sources, static_cast<unsigned>(kFoundByManual));
    CHECK(local->on_camera_subnet);
    CHECK_EQ(local->vendor, std::string("Hikvision"));
    CHECK(anyNoteContains(*local, "RTSP realm"));
    CHECK(local->ports.at(server.port()) == ConnectOutcome::kConnected);
    CHECK(!local->hasProblem(CameraError::kCameraOnOtherSubnet));
    // Discovery is disabled, so ONVIF never ran: no ONVIF remark.
    CHECK(!local->hasProblem(CameraError::kOnvifDisabledOrUnavailable));

    const DiscoveredCamera* remote = byIp(report.cameras, "10.1.2.3");
    CHECK(remote != nullptr);
    CHECK(remote->hasProblem(CameraError::kCameraOnOtherSubnet));
    CHECK(remote->ports.empty());
    CHECK(anyNoteContains(*remote, "not contacted"));

    CHECK(!server.sawAuthorization());
    const std::vector<std::string> methods = server.methods();
    CHECK(std::find(methods.begin(), methods.end(), "OPTIONS") != methods.end());

    bool sadp_skipped = false;
    bool manual_found = false;
    for (const DiscoveryMethodReport& method : report.methods) {
        if (method.method == "sadp") {
            sadp_skipped = !method.ran && method.detail == "discovery.enabled is false";
        }
        if (method.method == "manual") {
            manual_found = method.ran && method.found == 2;
        }
    }
    CHECK(sadp_skipped);
    CHECK(manual_found);
    CHECK_EQ(registry.entries().size(), std::size_t{2});

    // The same hosts again: the same ids.
    const DiscoveryReport again =
        discoverCameras(loopbackSnapshot(), loopbackLan(), config, registry);
    CHECK_EQ(again.cameras.size(), std::size_t{2});
    CHECK_EQ(byIp(again.cameras, "127.0.0.1")->id, local->id);
    CHECK_EQ(byIp(again.cameras, "10.1.2.3")->id, remote->id);
    CHECK_EQ(registry.entries().size(), std::size_t{2});
    CHECK(contains(formatDiscoveryReport(again), "DISCOVERED CAMERAS: 2"));
}

// ---------------------------------------------------------------------------------------------
// GStreamer pipelines and the decoder plan
// ---------------------------------------------------------------------------------------------

TEST("gst: quoting escapes quotes and backslashes") {
    CHECK_EQ(gstQuote(""), std::string("\"\""));
    CHECK_EQ(gstQuote("admin"), std::string("\"admin\""));
    CHECK_EQ(gstQuote("a\"b\\c d!"), std::string("\"a\\\"b\\\\c d!\""));
}

TEST("gst: scaled size keeps the aspect ratio in multiples of 8") {
    int width = 0;
    int height = 0;
    scaledSize(2688, 1520, 1280, width, height);
    CHECK_EQ(width, 1280);
    CHECK_EQ(height, 720);
    scaledSize(1920, 1080, 1280, width, height);
    CHECK_EQ(width, 1280);
    CHECK_EQ(height, 720);
    scaledSize(2560, 1440, 1000, width, height);
    CHECK_EQ(width, 1000);
    CHECK_EQ(height, 560);
    scaledSize(1920, 1080, 1283, width, height);
    CHECK_EQ(width, 1280);
    CHECK_EQ(height, 720);
    scaledSize(1280, 720, 1920, width, height);
    CHECK_EQ(width, 1280);
    CHECK_EQ(height, 720);
    scaledSize(1921, 1081, 0, width, height);
    CHECK_EQ(width, 1921);
    CHECK_EQ(height, 1081);
    scaledSize(0, 0, 1280, width, height);
    CHECK_EQ(width, 0);
    CHECK_EQ(height, 0);
}

TEST("gst: NVIDIA H.264 pipeline for native I420 capture") {
    const RtspSettings rtsp;
    const DecodeSettings decode;
    const std::string expected =
        "rtspsrc location=\"rtsp://192.168.10.21:554/Streaming/Channels/101\" user-id=\"admin\" "
        "user-pw=\"s3cr3t-P@ss \\\"q\\\"\" protocols=tcp latency=200 drop-on-latency=true "
        "tcp-timeout=4000000 do-rtsp-keep-alive=true ! rtph264depay ! h264parse "
        "config-interval=-1 ! nvv4l2decoder enable-max-performance=true ! nvvidconv ! "
        "video/x-raw,format=I420 ! appsink name=sink drop=true max-buffers=1 sync=false";
    CHECK_EQ(buildNvidiaPipeline(fieldEndpoint(true), VideoCodec::kH264, decode, rtsp, true, false),
             expected);
    const std::string redacted =
        buildNvidiaPipeline(fieldEndpoint(true), VideoCodec::kH264, decode, rtsp, true, true);
    CHECK(contains(redacted, "user-id=\"***\" user-pw=\"***\""));
    CHECK(!contains(redacted, "s3cr3t"));
    CHECK(!contains(redacted, "admin"));
}

TEST("gst: NVIDIA H.265 pipeline for OpenCV with scaling and a frame rate cap") {
    RtspSettings rtsp;
    rtsp.latency_ms = 300;
    rtsp.timeout_ms = 5000;
    DecodeSettings decode;
    decode.max_width = 1280;
    decode.max_fps = 8;
    StreamEndpoint endpoint = fieldEndpoint(false);
    endpoint.width = 2688;
    endpoint.height = 1520;
    const std::string expected =
        "rtspsrc location=\"rtsp://192.168.10.21:554/Streaming/Channels/101\" protocols=tcp "
        "latency=300 drop-on-latency=true tcp-timeout=5000000 do-rtsp-keep-alive=true ! "
        "rtph265depay ! h265parse config-interval=-1 ! nvv4l2decoder "
        "enable-max-performance=true ! nvvidconv ! video/x-raw,format=BGRx,width=1280,height=720 "
        "! videorate max-rate=8 ! videoconvert ! video/x-raw,format=BGR ! appsink name=sink "
        "drop=true max-buffers=1 sync=false";
    CHECK_EQ(buildNvidiaPipeline(endpoint, VideoCodec::kH265, decode, rtsp, false, false),
             expected);
    // I420 with the same settings.
    const std::string i420 =
        buildNvidiaPipeline(endpoint, VideoCodec::kH265, decode, rtsp, true, false);
    CHECK(contains(i420, "nvvidconv ! video/x-raw,format=I420,width=1280,height=720 ! "
                         "videorate max-rate=8 ! appsink name=sink"));
    // Scaling asked for, source size unknown: no size in the caps.
    endpoint.width = 0;
    endpoint.height = 0;
    const std::string unknown =
        buildNvidiaPipeline(endpoint, VideoCodec::kH265, decode, rtsp, true, false);
    CHECK(contains(unknown, "video/x-raw,format=I420 ! videorate max-rate=8 ! appsink"));
    // A stream already narrow enough is not touched.
    endpoint.width = 1280;
    endpoint.height = 720;
    CHECK(!contains(buildNvidiaPipeline(endpoint, VideoCodec::kH264, decode, rtsp, true, false),
                    "width="));
}

TEST("gst: software pipelines") {
    const RtspSettings rtsp;
    DecodeSettings decode;
    decode.max_width = 1280;
    StreamEndpoint endpoint = fieldEndpoint(true);
    endpoint.width = 1920;
    endpoint.height = 1080;
    const std::string h264 =
        buildSoftwarePipeline(endpoint, VideoCodec::kH264, decode, rtsp, true, true);
    CHECK_EQ(h264,
             std::string("rtspsrc location=\"rtsp://192.168.10.21:554/Streaming/Channels/101\" "
                         "user-id=\"***\" user-pw=\"***\" protocols=tcp latency=200 "
                         "drop-on-latency=true tcp-timeout=4000000 do-rtsp-keep-alive=true ! "
                         "rtph264depay ! h264parse config-interval=-1 ! avdec_h264 ! videoscale ! "
                         "videoconvert ! video/x-raw,format=I420,width=1280,height=720 ! appsink "
                         "name=sink drop=true max-buffers=1 sync=false"));
    decode.max_width = 0;
    decode.max_fps = 10;
    const std::string h265 =
        buildSoftwarePipeline(endpoint, VideoCodec::kH265, decode, rtsp, false, false);
    CHECK(contains(h265, "user-pw=\"s3cr3t-P@ss \\\"q\\\"\""));
    CHECK(contains(h265, "! rtph265depay ! h265parse config-interval=-1 ! avdec_h265 ! "
                         "videorate max-rate=10 ! videoconvert ! video/x-raw,format=BGR ! "
                         "appsink name=sink drop=true max-buffers=1 sync=false"));
    CHECK(!contains(h265, "videoscale"));
    CHECK(!contains(h265, "nvv4l2decoder"));
    CHECK(buildSoftwarePipeline(endpoint, VideoCodec::kMjpeg, decode, rtsp, true, false).empty());
    CHECK(buildNvidiaPipeline(endpoint, VideoCodec::kUnknown, decode, rtsp, true, false).empty());
}

TEST("gst: the decoder plan puts NVIDIA first, then native GStreamer, OpenCV, FFmpeg") {
    const RtspSettings rtsp;
    const DecodeSettings decode;
    std::string why;
    const std::vector<DecoderAttempt> plan =
        planDecoders(fieldEndpoint(true), VideoCodec::kH264, everything(), decode, rtsp, why);
    CHECK(why.empty());
    CHECK_EQ(planSummary(plan),
             std::string("nvidia_hardware/gst_native software_gstreamer/gst_native "
                         "software_gstreamer/opencv_gstreamer software_ffmpeg/opencv_ffmpeg"));
    CHECK(contains(plan[0].source, "nvv4l2decoder"));
    CHECK(contains(plan[0].source, "format=I420"));
    CHECK(contains(plan[0].source, "s3cr3t"));
    CHECK(contains(plan[1].description, "avdec_h264"));
    CHECK(contains(plan[1].description, "format=I420"));
    CHECK(contains(plan[2].description, "format=BGR"));
    CHECK(plan[0].note.empty());
    CHECK_EQ(plan[3].source,
             std::string("rtsp://admin:s3cr3t-P%40ss%20%22q%22@192.168.10.21:554/Streaming/"
                         "Channels/101"));
    CHECK_EQ(plan[3].description,
             std::string("rtsp://<redacted>@192.168.10.21:554/Streaming/Channels/101"));
    CHECK(!leaksPassword(plan));
}

TEST("gst: decoder plans for capability combinations") {
    const RtspSettings rtsp;
    std::string why;

    // OpenCV's GStreamer backend only: hardware through OpenCV (BGR), then software.
    DecoderCapabilities opencv = everything();
    opencv.gst_native = false;
    std::vector<DecoderAttempt> plan =
        planDecoders(fieldEndpoint(true), VideoCodec::kH265, opencv, DecodeSettings{}, rtsp, why);
    CHECK_EQ(planSummary(plan),
             std::string("nvidia_hardware/opencv_gstreamer software_gstreamer/opencv_gstreamer "
                         "software_ffmpeg/opencv_ffmpeg"));
    CHECK(contains(plan[0].description, "rtph265depay"));
    CHECK(contains(plan[0].description, "format=BGRx"));
    CHECK(contains(plan[1].description, "avdec_h265"));
    CHECK(!leaksPassword(plan));

    // Plugins present but the device node missing: still tried, with a note.
    DecoderCapabilities no_device = everything();
    no_device.nvidia_device = false;
    plan = planDecoders(fieldEndpoint(true), VideoCodec::kH264, no_device, DecodeSettings{}, rtsp,
                        why);
    CHECK_EQ(plan[0].decoder, std::string(kDecoderNvidiaHardware));
    CHECK(contains(plan[0].note, "/dev/nvhost-nvdec"));

    // Software preferred: no hardware attempt.
    DecodeSettings software;
    software.decoder = DecoderPreference::kSoftware;
    plan = planDecoders(fieldEndpoint(true), VideoCodec::kH264, everything(), software, rtsp, why);
    CHECK_EQ(planSummary(plan),
             std::string("software_gstreamer/gst_native software_gstreamer/opencv_gstreamer "
                         "software_ffmpeg/opencv_ffmpeg"));

    // Hardware only: exactly the hardware attempt.
    DecodeSettings hardware;
    hardware.decoder = DecoderPreference::kNvidiaHardware;
    plan = planDecoders(fieldEndpoint(true), VideoCodec::kH264, everything(), hardware, rtsp, why);
    CHECK_EQ(planSummary(plan), std::string("nvidia_hardware/gst_native"));

    // Hardware only without the NVIDIA plugins: nothing, and why.
    DecoderCapabilities cpu_only = everything();
    cpu_only.nvidia_decoder = false;
    cpu_only.nvidia_device = false;
    plan = planDecoders(fieldEndpoint(true), VideoCodec::kH264, cpu_only, hardware, rtsp, why);
    CHECK(plan.empty());
    CHECK(contains(why, "nvv4l2decoder is not available in this container"));

    // Auto without the NVIDIA plugins: software only.
    plan = planDecoders(fieldEndpoint(true), VideoCodec::kH264, cpu_only, DecodeSettings{}, rtsp,
                        why);
    CHECK_EQ(plan.front().decoder, std::string(kDecoderSoftwareGstreamer));
    CHECK(why.empty());

    // H.265 without avdec_h265: GStreamer software is skipped, FFmpeg remains.
    DecoderCapabilities no_h265 = cpu_only;
    no_h265.gst_avdec_h265 = false;
    plan = planDecoders(fieldEndpoint(false), VideoCodec::kH265, no_h265, DecodeSettings{}, rtsp,
                        why);
    CHECK_EQ(planSummary(plan), std::string("software_ffmpeg/opencv_ffmpeg"));
    CHECK_EQ(plan[0].source, std::string("rtsp://192.168.10.21:554/Streaming/Channels/101"));

    // The FFmpeg path cannot honour scaling or a rate cap, and says so.
    DecodeSettings capped;
    capped.max_fps = 5;
    plan = planDecoders(fieldEndpoint(false), VideoCodec::kH265, no_h265, capped, rtsp, why);
    CHECK(contains(plan[0].note, "max_fps"));

    // Nothing at all.
    plan = planDecoders(fieldEndpoint(true), VideoCodec::kH265, DecoderCapabilities{},
                        DecodeSettings{}, rtsp, why);
    CHECK(plan.empty());
    CHECK(contains(why, "nvv4l2decoder is not available"));
    CHECK(contains(why, "OpenCV has no FFmpeg backend"));
    CHECK(!contains(why, "s3cr3t"));

    // No GStreamer RTSP elements: every GStreamer path is out, once in the reason.
    DecoderCapabilities no_rtsp = everything();
    no_rtsp.gst_rtsp = false;
    no_rtsp.opencv_ffmpeg = false;
    plan = planDecoders(fieldEndpoint(true), VideoCodec::kH264, no_rtsp, DecodeSettings{}, rtsp,
                        why);
    CHECK(plan.empty());
    const std::string marker = "RTSP elements";
    CHECK(contains(why, marker));
    CHECK_EQ(why.find(marker), why.rfind(marker));

    // Codecs other than H.264/H.265.
    plan = planDecoders(fieldEndpoint(true), VideoCodec::kMjpeg, everything(), DecodeSettings{},
                        rtsp, why);
    CHECK(plan.empty());
    CHECK(contains(why, "MJPEG"));
    CHECK(contains(why, "only H.264 and H.265"));
}
