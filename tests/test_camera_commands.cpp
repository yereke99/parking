#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>

#include "anpr/cameras/camera_commands.hpp"
#include "anpr/cameras/camera_registry.hpp"
#include "anpr/cameras/status_store.hpp"
#include "anpr/common/filesystem.hpp"
#include "anpr/common/logging.hpp"
#include "anpr/pipeline/plate_sink.hpp"
#include "test_framework.hpp"

namespace {

using anpr::cameras::CameraError;
using anpr::cameras::CameraStatus;
using anpr::cameras::CameraTarget;
using anpr::cameras::DecoderCapabilities;
using anpr::cameras::DiscoveredCamera;
using anpr::cameras::NetworkProblem;
using anpr::cameras::StartupCameraLine;
using anpr::cameras::StatusSnapshot;
using anpr::net::CameraLanSelection;
using anpr::net::CameraLanStatus;
using anpr::net::InternetReach;
using anpr::net::InternetStatus;
using anpr::net::Ipv4;
using anpr::net::Ipv4Network;

Ipv4 ip(const std::string& text) {
    return anpr::net::parseIpv4(text).value();
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

CameraLanSelection readyLan() {
    CameraLanSelection lan;
    lan.status = CameraLanStatus::kReady;
    lan.interface = "eth0";
    lan.address = anpr::net::InterfaceAddress{ip("192.168.10.5"), 24};
    lan.networks.push_back(Ipv4Network{ip("192.168.10.5"), 24});
    lan.detail = "eth0 (r8168, pci) has link and 192.168.10.5/24";
    return lan;
}

InternetStatus onlineInternet() {
    InternetStatus internet;
    internet.has_default_route = true;
    internet.interface = "wwan0";
    internet.kind = anpr::net::InterfaceKind::kCellular;
    internet.reach = InternetReach::kOnline;
    return internet;
}

DiscoveredCamera sadpCamera(const std::string& id, const std::string& address,
                            const std::string& mask = "255.255.255.0") {
    DiscoveredCamera camera;
    camera.id = id;
    camera.ip = ip(address);
    camera.subnet_mask = mask;
    camera.sources = anpr::cameras::kFoundBySadp;
    camera.vendor = "Hikvision";
    return camera;
}

CameraTarget target(const std::string& id, const std::string& address) {
    CameraTarget result;
    result.id = id;
    result.ip = ip(address);
    result.model = "DS-TCG406-E";
    return result;
}

/// A unique, empty scratch directory under the system temp directory.
anpr::filesystem::path freshTempDir(const std::string& name) {
    const anpr::filesystem::path dir =
        anpr::filesystem::temp_directory_path() / ("kz_anpr_commands_" + name);
    std::error_code ec;
    anpr::filesystem::remove_all(dir, ec);
    anpr::filesystem::create_directories(dir, ec);
    return dir;
}

void writeFile(const anpr::filesystem::path& path, const std::string& text) {
    std::ofstream out(path.string());
    out << text;
}

/// Redirects a standard stream into a string while alive.
class StreamCapture {
public:
    explicit StreamCapture(std::ostream& stream) : stream_(stream), saved_(stream.rdbuf()) {
        stream_.rdbuf(buffer_.rdbuf());
    }
    ~StreamCapture() { stream_.rdbuf(saved_); }
    StreamCapture(const StreamCapture&) = delete;
    StreamCapture& operator=(const StreamCapture&) = delete;

    [[nodiscard]] std::string text() const { return buffer_.str(); }

private:
    std::ostream& stream_;
    std::streambuf* saved_;
    std::ostringstream buffer_;
};

/// A camera config that keeps every command off the real network: an interface that does not
/// exist (CAMERA_LAN_NOT_FOUND before any discovery), no Internet probe, files in `dir`.
std::string isolatedCameraConfig(const anpr::filesystem::path& dir) {
    return "network:\n"
           "  interface: kztest9\n"
           "  internet_check: false\n"
           "discovery:\n"
           "  registry_file: " + (dir / "registry.yaml").string() + "\n"
           "runtime:\n"
           "  status_file: " + (dir / "status.json").string() + "\n"
           "  status_interval_ms: 5000\n";
}

std::int64_t unixNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool modelsPresent() {
    return anpr::filesystem::exists("models/license_plate_detector.onnx") &&
           anpr::filesystem::exists("models/nomeroff-onnx/kz.onnx");
}

}  // namespace

// ---- Camera LAN and Internet --------------------------------------------------------------------

TEST("camera commands: only a missing interface or link counts as CAMERA_LAN_NOT_FOUND") {
    CameraLanSelection lan;
    lan.status = CameraLanStatus::kNoInterface;
    CHECK(anpr::cameras::cameraLanMissing(lan));
    lan.status = CameraLanStatus::kNoLink;
    CHECK(anpr::cameras::cameraLanMissing(lan));
    lan.status = CameraLanStatus::kOverrideMissing;
    CHECK(anpr::cameras::cameraLanMissing(lan));
    lan.status = CameraLanStatus::kNoIpv4;
    CHECK(!anpr::cameras::cameraLanMissing(lan));
    lan.status = CameraLanStatus::kReady;
    CHECK(!anpr::cameras::cameraLanMissing(lan));
}

TEST("camera commands: a ready camera LAN with GSM online reports nothing") {
    const anpr::cameras::NetworkSettings settings;
    CHECK(anpr::cameras::networkProblems(readyLan(), onlineInternet(), {}, settings).empty());
}

TEST("camera commands: link without an address is CAMERA_SUBNET_UNCONFIGURED on that port") {
    CameraLanSelection lan;
    lan.status = CameraLanStatus::kNoIpv4;
    lan.interface = "eth0";
    lan.detail = "eth0 (r8168, pci) has link but no usable IPv4 address";
    const auto problems =
        anpr::cameras::networkProblems(lan, onlineInternet(), {}, anpr::cameras::NetworkSettings{});
    CHECK_EQ(problems.size(), std::size_t{1});
    CHECK(problems[0].error == CameraError::kCameraSubnetUnconfigured);
    CHECK_EQ(problems[0].context.interface, std::string("eth0"));
    CHECK(problems[0].context.stage == anpr::cameras::Stage::kNetwork);
    CHECK_EQ(problems[0].detail, lan.detail);
}

TEST("camera commands: no link, no interface and a missing override are CAMERA_LAN_NOT_FOUND") {
    const anpr::cameras::NetworkSettings settings;
    for (const CameraLanStatus status :
         {CameraLanStatus::kNoLink, CameraLanStatus::kNoInterface,
          CameraLanStatus::kOverrideMissing}) {
        CameraLanSelection lan;
        lan.status = status;
        lan.interface = status == CameraLanStatus::kNoInterface ? "" : "eth1";
        const auto problems = anpr::cameras::networkProblems(lan, onlineInternet(), {}, settings);
        CHECK_EQ(problems.size(), std::size_t{1});
        CHECK(problems[0].error == CameraError::kCameraLanNotFound);
        CHECK(!problems[0].detail.empty());
    }
    CameraLanSelection missing;
    missing.status = CameraLanStatus::kOverrideMissing;
    missing.interface = "eth9";
    const auto problems = anpr::cameras::networkProblems(missing, onlineInternet(), {}, settings);
    CHECK(contains(problems[0].detail, "eth9"));
}

TEST("camera commands: default route, overlaps and the uplink are reported in order") {
    CameraLanSelection lan = readyLan();
    lan.carries_default_route = true;
    InternetStatus internet = onlineInternet();
    internet.interface = "eth0";
    internet.reach = InternetReach::kOffline;
    anpr::cameras::NetworkSettings settings;
    settings.internet_probe_targets = {"1.1.1.1:53", "8.8.8.8:53"};
    settings.internet_timeout_ms = 1500;
    const auto problems = anpr::cameras::networkProblems(
        lan, internet, {"docker0 192.168.0.0/20", "wwan0 192.168.10.0/24"}, settings);
    CHECK_EQ(problems.size(), std::size_t{3});
    CHECK(problems[0].error == CameraError::kCameraLanHasDefaultRoute);
    CHECK(problems[1].error == CameraError::kSubnetConflict);
    CHECK(contains(problems[1].detail, "docker0 192.168.0.0/20, wwan0 192.168.10.0/24"));
    CHECK(problems[2].error == CameraError::kInternetOffline);
    CHECK(problems[2].context.stage == anpr::cameras::Stage::kInternet);
    CHECK(contains(problems[2].detail, "1.1.1.1:53, 8.8.8.8:53"));
    CHECK(contains(problems[2].detail, "1500 ms"));

    InternetStatus none;
    const auto offline = anpr::cameras::networkProblems(readyLan(), none, {}, settings);
    CHECK_EQ(offline.size(), std::size_t{1});
    CHECK(offline[0].error == CameraError::kNoInternetRoute);
    CHECK(anpr::cameras::describe(offline[0].error).severity == anpr::cameras::Severity::kInfo);
}

TEST("camera commands: LAN and Internet summaries for the status file") {
    CHECK_EQ(anpr::cameras::cameraLanSummary(readyLan()), std::string("eth0 192.168.10.5/24"));
    CameraLanSelection two = readyLan();
    two.networks.push_back(Ipv4Network{ip("192.168.1.10"), 24});
    CHECK_EQ(anpr::cameras::cameraLanSummary(two),
             std::string("eth0 192.168.10.5/24,192.168.1.10/24"));
    CameraLanSelection no_ip;
    no_ip.status = CameraLanStatus::kNoIpv4;
    no_ip.interface = "eth0";
    CHECK_EQ(anpr::cameras::cameraLanSummary(no_ip),
             std::string("eth0 CAMERA_SUBNET_UNCONFIGURED"));
    CameraLanSelection none;
    CHECK_EQ(anpr::cameras::cameraLanSummary(none), std::string("CAMERA_LAN_NOT_FOUND"));

    CHECK_EQ(anpr::cameras::internetSummary(onlineInternet()), std::string("wwan0 ONLINE"));
    InternetStatus offline = onlineInternet();
    offline.reach = InternetReach::kOffline;
    CHECK_EQ(anpr::cameras::internetSummary(offline), std::string("wwan0 OFFLINE"));
    offline.reach = InternetReach::kNotChecked;
    CHECK_EQ(anpr::cameras::internetSummary(offline), std::string("wwan0 NOT CHECKED"));
    CHECK_EQ(anpr::cameras::internetSummary(InternetStatus{}), std::string("NO_INTERNET_ROUTE"));
}

// ---- Suggested camera LAN address ---------------------------------------------------------------

TEST("camera commands: a factory-default camera suggests host .200 of its subnet") {
    const auto suggestion =
        anpr::cameras::suggestCameraLanAddress({sadpCamera("camera-01", "192.168.1.64")});
    CHECK(suggestion.has_value());
    CHECK_EQ(suggestion->addressWithPrefix(), std::string("192.168.1.200/24"));
}

TEST("camera commands: the suggestion skips addresses cameras or their gateway use") {
    DiscoveredCamera first = sadpCamera("camera-01", "192.168.10.200");
    first.gateway = "192.168.10.201";
    const DiscoveredCamera second = sadpCamera("camera-02", "192.168.10.202");
    const auto suggestion = anpr::cameras::suggestCameraLanAddress({first, second});
    CHECK(suggestion.has_value());
    CHECK_EQ(anpr::net::toString(suggestion->address), std::string("192.168.10.203"));
}

TEST("camera commands: the suggestion follows most cameras and prefers SADP answers") {
    std::vector<DiscoveredCamera> cameras = {sadpCamera("camera-01", "192.168.1.64"),
                                             sadpCamera("camera-02", "192.168.10.21"),
                                             sadpCamera("camera-03", "192.168.10.22")};
    DiscoveredCamera arp_only;
    arp_only.id = "camera-04";
    arp_only.ip = ip("10.0.0.7");
    arp_only.sources = anpr::cameras::kFoundByArp;
    cameras.push_back(arp_only);
    cameras.push_back(arp_only);
    cameras.back().id = "camera-05";
    cameras.back().ip = ip("10.0.0.8");
    cameras.push_back(arp_only);
    cameras.back().id = "camera-06";
    cameras.back().ip = ip("10.0.0.9");
    const auto suggestion = anpr::cameras::suggestCameraLanAddress(cameras);
    CHECK(suggestion.has_value());
    CHECK_EQ(suggestion->addressWithPrefix(), std::string("192.168.10.200/24"));

    // Without SADP answers any discovered camera counts.
    const auto fallback = anpr::cameras::suggestCameraLanAddress({arp_only});
    CHECK(fallback.has_value());
    CHECK_EQ(fallback->addressWithPrefix(), std::string("10.0.0.200/24"));
}

TEST("camera commands: the suggestion uses the camera's mask and stays inside small subnets") {
    const auto narrow = anpr::cameras::suggestCameraLanAddress(
        {sadpCamera("camera-01", "192.168.10.130", "255.255.255.128")});
    CHECK(narrow.has_value());
    CHECK_EQ(narrow->addressWithPrefix(), std::string("192.168.10.254/25"));

    // A mask SADP got wrong (non-contiguous) falls back to /24.
    const auto odd = anpr::cameras::suggestCameraLanAddress(
        {sadpCamera("camera-01", "172.20.5.64", "255.0.255.0")});
    CHECK(odd.has_value());
    CHECK_EQ(odd->addressWithPrefix(), std::string("172.20.5.200/24"));

    // A /30 with both hosts taken has no room.
    const auto full = anpr::cameras::suggestCameraLanAddress(
        {sadpCamera("camera-01", "192.168.50.1", "255.255.255.252"),
         sadpCamera("camera-02", "192.168.50.2", "255.255.255.252")});
    CHECK(!full.has_value());
    CHECK(!anpr::cameras::suggestCameraLanAddress({}).has_value());
}

TEST("camera commands: the suggestion block carries the exact camera-lan-setup command") {
    const std::string block = anpr::cameras::formatAddressSuggestion(
        {sadpCamera("camera-01", "192.168.1.64")}, "eth0");
    CHECK(contains(block, "SUGGESTED CAMERA LAN ADDRESS\n"));
    CHECK(contains(block, "make camera-lan-setup LAN_ARGS='--interface eth0 --address "
                          "192.168.1.200/24'\n"));
    CHECK(contains(block, "camera-01 192.168.1.64"));
    CHECK(contains(block, "192.168.1.0/24"));

    const std::string empty = anpr::cameras::formatAddressSuggestion({}, "eth0");
    CHECK(contains(empty, "No camera answered SADP"));
    CHECK(contains(empty, "LAN_ARGS='--interface eth0 --address 192.168.10.5/24'"));
    const std::string no_interface =
        anpr::cameras::formatAddressSuggestion({sadpCamera("camera-01", "192.168.1.64")}, "");
    CHECK(contains(no_interface, "LAN_ARGS='--address 192.168.1.200/24'"));
}

// ---- Decoder capabilities -----------------------------------------------------------------------

TEST("camera commands: hardware decoding needs plugin, device node, rtspsrc and a GStreamer path") {
    DecoderCapabilities jetson;
    jetson.gst_native = true;
    jetson.gst_rtsp = true;
    jetson.nvidia_decoder = true;
    jetson.nvidia_device = true;
    CHECK(anpr::cameras::hardwareDecodingAvailable(jetson));
    CHECK(anpr::cameras::missingHardwareDecoding(jetson).empty());

    DecoderCapabilities opencv_only = jetson;
    opencv_only.gst_native = false;
    opencv_only.opencv_gstreamer = true;
    CHECK(anpr::cameras::hardwareDecodingAvailable(opencv_only));

    DecoderCapabilities no_device = jetson;
    no_device.nvidia_device = false;
    CHECK(!anpr::cameras::hardwareDecodingAvailable(no_device));
    const auto missing = anpr::cameras::missingHardwareDecoding(no_device);
    CHECK_EQ(missing.size(), std::size_t{1});
    CHECK(contains(missing[0], "/dev/nvhost-nvdec"));
    CHECK_EQ(anpr::cameras::missingHardwareDecoding(DecoderCapabilities{}).size(), std::size_t{4});
}

TEST("camera commands: the DECODER CAPABILITIES block lists every finding") {
    DecoderCapabilities capabilities;
    capabilities.gst_native = true;
    capabilities.gst_rtsp = true;
    capabilities.gst_avdec_h264 = true;
    capabilities.opencv_ffmpeg = true;
    capabilities.notes.push_back("nvv4l2decoder plugin not found");
    const std::string block = anpr::cameras::formatDecoderCapabilities(capabilities);
    CHECK(contains(block, "DECODER CAPABILITIES\n"));
    CHECK(contains(block, "  native GStreamer capture: yes\n"));
    CHECK(contains(block, "  nvv4l2decoder (NVIDIA hardware): no\n"));
    CHECK(contains(block, "  /dev/nvhost-nvdec: no\n"));
    CHECK(contains(block, "  software decoders: avdec_h264 yes, avdec_h265 no\n"));
    CHECK(contains(block, "  OpenCV GStreamer: no\n"));
    CHECK(contains(block, "  OpenCV FFmpeg: yes\n"));
    CHECK(contains(block, "  hardware decoding: NOT AVAILABLE"));
    CHECK(contains(block, "  note: nvv4l2decoder plugin not found\n"));
}

// ---- Status -------------------------------------------------------------------------------------

TEST("camera commands: a status file is trusted within three status intervals") {
    StatusSnapshot snapshot;
    const std::int64_t now = 1'760'000'000'000;
    snapshot.updated_unix_ms = now - 14'000;
    CHECK(anpr::cameras::statusIsFresh(snapshot, now, 5000));
    snapshot.updated_unix_ms = now - 16'000;
    CHECK(!anpr::cameras::statusIsFresh(snapshot, now, 5000));
    // A wall clock that stepped back a little does not hide a running instance.
    snapshot.updated_unix_ms = now + 3'000;
    CHECK(anpr::cameras::statusIsFresh(snapshot, now, 5000));
    snapshot.updated_unix_ms = now + 60'000;
    CHECK(!anpr::cameras::statusIsFresh(snapshot, now, 5000));
    snapshot.updated_unix_ms = 0;
    CHECK(!anpr::cameras::statusIsFresh(snapshot, now, 5000));
}

TEST("camera commands: status exit code is 0 only when every enabled camera runs") {
    StatusSnapshot snapshot;
    CHECK_EQ(anpr::cameras::statusExitCode(snapshot), 3);
    CameraStatus running;
    running.id = "camera-01";
    running.anpr = "RUNNING";
    snapshot.cameras.push_back(running);
    CHECK_EQ(anpr::cameras::statusExitCode(snapshot), 0);
    CameraStatus disabled;
    disabled.id = "camera-02";
    disabled.anpr = "DISABLED";
    snapshot.cameras.push_back(disabled);
    CHECK_EQ(anpr::cameras::statusExitCode(snapshot), 0);
    CameraStatus reconnecting;
    reconnecting.id = "camera-03";
    reconnecting.anpr = "RECONNECTING";
    snapshot.cameras.push_back(reconnecting);
    CHECK_EQ(anpr::cameras::statusExitCode(snapshot), 3);

    StatusSnapshot only_disabled;
    only_disabled.cameras.push_back(disabled);
    CHECK_EQ(anpr::cameras::statusExitCode(only_disabled), 3);

    // Disabled in the config (with its code) is fine; given up after a rejected login is not.
    StatusSnapshot config_disabled;
    config_disabled.cameras.push_back(running);
    disabled.error = "CAMERA_DISABLED";
    config_disabled.cameras.push_back(disabled);
    CHECK_EQ(anpr::cameras::statusExitCode(config_disabled), 0);
    CameraStatus given_up = disabled;
    given_up.id = "camera-04";
    given_up.error = "RTSP_AUTH_FAILED";
    config_disabled.cameras.push_back(given_up);
    CHECK_EQ(anpr::cameras::statusExitCode(config_disabled), 3);
}

// ---- Startup report pieces ----------------------------------------------------------------------

TEST("camera commands: RTSP words for the status table and the startup block") {
    CHECK_EQ(anpr::cameras::rtspStatusWord(CameraError::kNone), std::string("OK"));
    CHECK_EQ(anpr::cameras::rtspStatusWord(CameraError::kRtspAuthFailed), std::string("AUTH"));
    CHECK_EQ(anpr::cameras::rtspStatusWord(CameraError::kRtspCredentialsMissing),
             std::string("NOCRED"));
    CHECK_EQ(anpr::cameras::rtspStatusWord(CameraError::kRtspPortClosed), std::string("PORT"));
    CHECK_EQ(anpr::cameras::rtspStatusWord(CameraError::kRtspStreamPathInvalid),
             std::string("PATH"));
    CHECK_EQ(anpr::cameras::rtspStatusWord(CameraError::kCameraUnreachable), std::string("DOWN"));
    CHECK_EQ(anpr::cameras::rtspStatusWord(CameraError::kRtspProtocolError), std::string("ERROR"));

    CHECK_EQ(anpr::cameras::rtspStartupWord(CameraError::kNone), std::string("OK"));
    CHECK_EQ(anpr::cameras::rtspStartupWord(CameraError::kRtspAuthFailed),
             std::string("AUTH_FAILED"));
    CHECK_EQ(anpr::cameras::rtspStartupWord(CameraError::kRtspStreamPathInvalid),
             std::string("STREAM_PATH_INVALID"));
    CHECK_EQ(anpr::cameras::rtspStartupWord(CameraError::kCameraUnreachable),
             std::string("CAMERA_UNREACHABLE"));
}

TEST("camera commands: the ANPR startup line follows the retry policy") {
    anpr::cameras::CaptureSettings capture;
    CHECK_EQ(anpr::cameras::anprStartupText(CameraError::kNone, capture), std::string("RUNNING"));
    CHECK_EQ(anpr::cameras::anprStartupText(CameraError::kRtspAuthFailed, capture),
             std::string("WAITING (retry in 15 min)"));
    CHECK_EQ(anpr::cameras::anprStartupText(CameraError::kRtspCredentialsMissing, capture),
             std::string("WAITING (retry in 15 min)"));
    CHECK_EQ(anpr::cameras::anprStartupText(CameraError::kRtspStreamPathInvalid, capture),
             std::string("WAITING (retry in 2 min)"));
    CHECK_EQ(anpr::cameras::anprStartupText(CameraError::kCameraUnreachable, capture),
             std::string("RECONNECTING"));
    CHECK_EQ(anpr::cameras::anprStartupText(CameraError::kRtspPortClosed, capture),
             std::string("RECONNECTING"));
    capture.auth_max_retries = 0;
    CHECK(contains(anpr::cameras::anprStartupText(CameraError::kRtspAuthFailed, capture),
                   "not retried"));
}

TEST("camera commands: discovery problems that forbid contacting a camera, worst first") {
    CameraTarget camera = target("camera-01", "192.168.1.64");
    CHECK(anpr::cameras::blockingDiscoveryProblem(camera) == CameraError::kNone);
    camera.discovery_problems.emplace_back(CameraError::kOnvifDisabledOrUnavailable, "");
    CHECK(anpr::cameras::blockingDiscoveryProblem(camera) == CameraError::kNone);
    camera.discovery_problems.emplace_back(CameraError::kCameraNotActivated, "factory state");
    CHECK(anpr::cameras::blockingDiscoveryProblem(camera) == CameraError::kCameraNotActivated);
    camera.discovery_problems.emplace_back(CameraError::kDuplicateIpDetected, "two MACs");
    CHECK(anpr::cameras::blockingDiscoveryProblem(camera) == CameraError::kDuplicateIpDetected);
    camera.discovery_problems.emplace_back(CameraError::kCameraOnOtherSubnet, "192.168.1.64");
    CHECK(anpr::cameras::blockingDiscoveryProblem(camera) == CameraError::kCameraOnOtherSubnet);
}

TEST("camera commands: VIDEO line from what the decoder knows") {
    CHECK_EQ(anpr::cameras::formatVideoSummary(anpr::net::VideoCodec::kH264, 2688, 1520, 25.0,
                                               "nvidia_hardware"),
             std::string("H.264 2688x1520 25.0 fps, decoder=nvidia_hardware"));
    CHECK_EQ(anpr::cameras::formatVideoSummary(anpr::net::VideoCodec::kH265, 1920, 1080, 0.0,
                                               "software_gstreamer"),
             std::string("H.265 1920x1080, decoder=software_gstreamer"));
    CHECK_EQ(anpr::cameras::formatVideoSummary(anpr::net::VideoCodec::kUnknown, 0, 0, 0.0,
                                               "software_ffmpeg"),
             std::string("decoder=software_ffmpeg"));
    CHECK_EQ(anpr::cameras::formatVideoSummary(anpr::net::VideoCodec::kUnknown, 0, 0, 0.0, ""),
             std::string("pending"));
}

TEST("camera commands: retry delays read naturally") {
    CHECK_EQ(anpr::cameras::formatRetryDelay(900000), std::string("15 min"));
    CHECK_EQ(anpr::cameras::formatRetryDelay(120000), std::string("2 min"));
    CHECK_EQ(anpr::cameras::formatRetryDelay(90000), std::string("90 s"));
    CHECK_EQ(anpr::cameras::formatRetryDelay(30000), std::string("30 s"));
    CHECK_EQ(anpr::cameras::formatRetryDelay(1500), std::string("2 s"));
    CHECK_EQ(anpr::cameras::formatRetryDelay(3600000), std::string("1 h"));
    CHECK_EQ(anpr::cameras::formatRetryDelay(630000), std::string("11 min"));
    CHECK_EQ(anpr::cameras::formatRetryDelay(-5), std::string("0 s"));
}

TEST("camera commands: the startup block has the documented layout") {
    StartupCameraLine running;
    running.id = "camera-01";
    running.model = "DS-TCG406-E";
    running.ip = "192.168.10.21";
    running.rtsp = "OK";
    running.video = "H.264 2688x1520 25.0 fps, decoder=nvidia_hardware";
    running.anpr = "RUNNING";
    StartupCameraLine waiting;
    waiting.id = "camera-02";
    waiting.ip = "192.168.10.22";
    waiting.rtsp = "AUTH_FAILED";
    waiting.video = "-";
    waiting.anpr = "WAITING (retry in 15 min)";
    const std::string block = anpr::cameras::formatStartupReport({running, waiting}, 1);
    CHECK_EQ(block, std::string("DISCOVERED CAMERAS: 2\n"
                                "\n"
                                "camera-01\n"
                                "  Model: DS-TCG406-E\n"
                                "  IP: 192.168.10.21\n"
                                "  RTSP: OK\n"
                                "  VIDEO: H.264 2688x1520 25.0 fps, decoder=nvidia_hardware\n"
                                "  ANPR: RUNNING\n"
                                "\n"
                                "camera-02\n"
                                "  Model: unknown\n"
                                "  IP: 192.168.10.22\n"
                                "  RTSP: AUTH_FAILED\n"
                                "  VIDEO: -\n"
                                "  ANPR: WAITING (retry in 15 min)\n"
                                "\n"
                                "ACTIVE CAMERAS: 1/2\n"));
    CHECK_EQ(anpr::cameras::formatStartupReport({}, 0),
             std::string("DISCOVERED CAMERAS: 0\n\nACTIVE CAMERAS: 0/0\n"));
}

// ---- Per-camera settings ------------------------------------------------------------------------

TEST("camera commands: per-camera settings take the capture policy and never the password") {
    anpr::AnprConfig base;
    base.camera.camera_id = "gate-01";
    base.camera.source = "video/parking.mp4";
    anpr::cameras::CameraModeConfig config;
    config.capture.read_timeout_ms = 4321;
    config.capture.reconnect_initial_backoff_ms = 1111;
    config.capture.reconnect_max_backoff_ms = 22222;
    CameraTarget camera = target("camera-02", "192.168.10.22");
    camera.credentials.username = "admin";
    camera.credentials.password = "s3cret-Pa55";
    camera.has_credentials = true;
    std::vector<std::string> replaced{"stale"};
    const anpr::AnprConfig result =
        anpr::cameras::cameraAnprConfig(base, nullptr, camera, config, replaced);
    CHECK(replaced.empty());
    CHECK_EQ(result.camera.camera_id, std::string("camera-02"));
    CHECK(result.camera.kind == anpr::CameraKind::kRtsp);
    CHECK_EQ(result.camera.read_timeout_ms, std::int64_t{4321});
    CHECK_EQ(result.camera.reconnect_initial_backoff_ms, std::int64_t{1111});
    CHECK_EQ(result.camera.reconnect_max_backoff_ms, std::int64_t{22222});
    CHECK(!result.camera.loop_file);
    CHECK(!result.camera.process_every_file_frame);
    CHECK(!contains(result.camera.source, "s3cret"));
    CHECK(!contains(result.camera.source, "admin"));
    CHECK(contains(result.camera.source, "192.168.10.22"));
    // Everything else is the base profile.
    CHECK_EQ(result.detector.model, base.detector.model);
    CHECK_EQ(result.recognition.cooldown_ms, base.recognition.cooldown_ms);
}

TEST("camera commands: a camera profile keeps the shared models and says what it ignored") {
    anpr::AnprConfig base;
    base.detector.model = "models/license_plate_detector.onnx";
    base.ocr.model = "models/nomeroff-onnx/kz.onnx";
    base.inference.backend = anpr::InferenceBackend::kTensorRT;
    base.logging.level = anpr::LogLevel::kWarn;
    anpr::AnprConfig profile = base;
    profile.roi.detection = anpr::NormalizedRect{0.2, 0.3, 0.5, 0.4};
    profile.recognition.cooldown_ms = 9999;
    profile.ocr.model = "models/other.onnx";
    profile.inference.backend = anpr::InferenceBackend::kOnnxCpu;
    profile.logging.level = anpr::LogLevel::kDebug;
    std::vector<std::string> replaced;
    const anpr::AnprConfig result = anpr::cameras::cameraAnprConfig(
        base, &profile, target("camera-03", "192.168.10.23"), anpr::cameras::CameraModeConfig{},
        replaced);
    CHECK_EQ(replaced.size(), std::size_t{2});
    CHECK_EQ(replaced[0], std::string("ocr"));
    CHECK_EQ(replaced[1], std::string("inference"));
    CHECK_EQ(result.ocr.model, base.ocr.model);
    CHECK(result.inference.backend == anpr::InferenceBackend::kTensorRT);
    CHECK(result.logging.level == anpr::LogLevel::kWarn);
    CHECK_NEAR(result.roi.detection.x, 0.2, 1e-9);
    CHECK_EQ(result.recognition.cooldown_ms, std::int64_t{9999});
    CHECK_EQ(result.camera.camera_id, std::string("camera-03"));

    anpr::AnprConfig same_models = base;
    same_models.recognition.cooldown_ms = 1;
    anpr::cameras::cameraAnprConfig(base, &same_models, target("camera-04", "192.168.10.24"),
                                    anpr::cameras::CameraModeConfig{}, replaced);
    CHECK(replaced.empty());
}

TEST("camera commands: FFmpeg capture options use TCP and the read timeout in microseconds") {
    CHECK_EQ(anpr::cameras::ffmpegCaptureOptions(5000),
             std::string("rtsp_transport;tcp|stimeout;5000000"));
    CHECK_EQ(anpr::cameras::ffmpegCaptureOptions(0),
             std::string("rtsp_transport;tcp|stimeout;1000"));
}

TEST("camera commands: the ISAPI channel comes from the RTSP path") {
    CameraTarget camera = target("camera-01", "192.168.10.21");
    camera.rtsp_path = "/Streaming/Channels/102";
    CHECK_EQ(anpr::cameras::isapiStreamingChannel(camera), 102);
    camera.rtsp_path = "/ISAPI/streaming/channels/201/";
    CHECK_EQ(anpr::cameras::isapiStreamingChannel(camera), 201);
    camera.rtsp_path = "/live/ch1";
    CHECK_EQ(anpr::cameras::isapiStreamingChannel(camera), 101);
    camera.stream = anpr::cameras::StreamSelection::kSub;
    CHECK_EQ(anpr::cameras::isapiStreamingChannel(camera), 102);
    camera.rtsp_path = "/Streaming/Channels/";
    CHECK_EQ(anpr::cameras::isapiStreamingChannel(camera), 102);
    camera.rtsp_path = "/Streaming/Channels/12345678";
    CHECK_EQ(anpr::cameras::isapiStreamingChannel(camera), 102);
}

// ---- Commands, kept off the network -------------------------------------------------------------

TEST("camera commands: a broken camera config is exit code 2 for every command") {
    const anpr::filesystem::path dir = freshTempDir("broken");
    anpr::cameras::CameraCommandOptions options;
    options.camera_config_path = (dir / "missing.yaml").string();
    std::atomic_bool stop{true};
    StreamCapture err(std::cerr);
    CHECK_EQ(anpr::cameras::runCameraScan(options), 2);
    CHECK_EQ(anpr::cameras::runCameraCheck(options), 2);
    CHECK_EQ(anpr::cameras::runCameraStatus(options), 2);
    CHECK_EQ(anpr::cameras::runCameras(options, anpr::AnprConfig{},
                                       std::make_shared<anpr::CollectingSink>(), stop, false),
             2);
    CHECK(contains(err.text(), "INVALID_CONFIG"));

    writeFile(dir / "bad.yaml", "network:\n  interface: \"eth 0\"\n");
    options.camera_config_path = (dir / "bad.yaml").string();
    CHECK_EQ(anpr::cameras::runCameraScan(options), 2);
}

TEST("camera commands: scan and check stop at a missing camera LAN with exit code 3") {
    const anpr::filesystem::path dir = freshTempDir("nolan");
    writeFile(dir / "cameras.yaml", isolatedCameraConfig(dir));
    anpr::cameras::CameraCommandOptions options;
    options.camera_config_path = (dir / "cameras.yaml").string();
    std::string report;
    std::string log;
    {
        StreamCapture out(std::cout);
        StreamCapture err(std::cerr);
        CHECK_EQ(anpr::cameras::runCameraScan(options), 3);
        CHECK_EQ(anpr::cameras::runCameraCheck(options), 3);
        report = out.text();
        log = err.text();
    }
    CHECK(contains(report, "CAMERA NETWORK\n"));
    CHECK(contains(report, "ERROR CAMERA_LAN_NOT_FOUND  kztest9\n"));
    CHECK(contains(report, "configured interface kztest9 does not exist"));
    CHECK(contains(log, "error=CAMERA_LAN_NOT_FOUND"));
    CHECK(contains(log, "interface=kztest9"));
    // Neither command got as far as discovery, so nothing was written.
    CHECK(!anpr::filesystem::exists(dir / "registry.yaml"));
}

TEST("camera commands: camera-status prints a fresh status file of a running instance") {
    const anpr::filesystem::path dir = freshTempDir("status_fresh");
    writeFile(dir / "cameras.yaml", isolatedCameraConfig(dir));
    StatusSnapshot snapshot;
    snapshot.updated_at = "2026-10-09T10:15:05Z";
    snapshot.updated_unix_ms = unixNowMs();
    snapshot.camera_lan = "eth0 192.168.10.5/24";
    snapshot.internet = "wwan0 ONLINE";
    CameraStatus first;
    first.id = "camera-01";
    first.ip = "192.168.10.21";
    first.link = "OK";
    first.rtsp = "OK";
    first.video = "H264";
    first.anpr = "RUNNING";
    snapshot.cameras.push_back(first);
    std::string error;
    CHECK(anpr::cameras::writeStatusFile((dir / "status.json").string(), snapshot, error));

    anpr::cameras::CameraCommandOptions options;
    options.camera_config_path = (dir / "cameras.yaml").string();
    std::string report;
    int code = -1;
    {
        StreamCapture out(std::cout);
        code = anpr::cameras::runCameraStatus(options);
        report = out.text();
    }
    CHECK_EQ(code, 0);
    CHECK_EQ(report, anpr::cameras::formatStatusTable(snapshot));
    CHECK(!contains(report, "probing"));

    CameraStatus second = first;
    second.id = "camera-02";
    second.anpr = "ERROR";
    second.rtsp = "AUTH";
    second.error = "RTSP_AUTH_FAILED";
    snapshot.cameras.push_back(second);
    CHECK(anpr::cameras::writeStatusFile((dir / "status.json").string(), snapshot, error));
    {
        StreamCapture out(std::cout);
        code = anpr::cameras::runCameraStatus(options);
        report = out.text();
    }
    CHECK_EQ(code, 3);
    CHECK(contains(report, "RTSP_AUTH_FAILED"));
}

TEST("camera commands: camera-status probes when the status file is stale") {
    const anpr::filesystem::path dir = freshTempDir("status_stale");
    writeFile(dir / "cameras.yaml", isolatedCameraConfig(dir));
    StatusSnapshot old;
    old.updated_unix_ms = unixNowMs() - 3'600'000;
    CameraStatus ghost;
    ghost.id = "camera-09";
    ghost.anpr = "RUNNING";
    old.cameras.push_back(ghost);
    std::string error;
    CHECK(anpr::cameras::writeStatusFile((dir / "status.json").string(), old, error));
    writeFile(dir / "registry.yaml",
              "cameras:\n"
              "  - id: \"camera-01\"\n"
              "    mac: \"44:19:b6:3a:10:21\"\n"
              "    model: \"DS-TCG406-E\"\n"
              "    last_ip: \"192.168.10.21\"\n"
              "    last_seen: \"2026-10-09T09:58:12Z\"\n");

    anpr::cameras::CameraCommandOptions options;
    options.camera_config_path = (dir / "cameras.yaml").string();
    std::string report;
    int code = -1;
    {
        StreamCapture out(std::cout);
        StreamCapture err(std::cerr);
        code = anpr::cameras::runCameraStatus(options);
        report = out.text();
    }
    CHECK_EQ(code, 3);
    CHECK(contains(report, "No running camera-mode ANPR (status file missing or stale); probing "
                           "cameras...\n"));
    CHECK(contains(report, "CAMERA NETWORK\n"));
    CHECK(contains(report, "camera LAN: kztest9 CAMERA_LAN_NOT_FOUND"));
    CHECK(contains(report, "Internet: "));
    CHECK(contains(report, "camera-01"));
    CHECK(contains(report, "192.168.10.21"));
    CHECK(contains(report, "STOPPED"));
    CHECK(!contains(report, "camera-09"));
}

TEST("camera commands: run-cameras waits for the camera LAN and stops on request") {
    const anpr::filesystem::path dir = freshTempDir("run_wait");
    writeFile(dir / "cameras.yaml", isolatedCameraConfig(dir));
    anpr::cameras::CameraCommandOptions options;
    options.camera_config_path = (dir / "cameras.yaml").string();
    std::atomic_bool stop{true};
    std::string report;
    int code = -1;
    {
        StreamCapture err(std::cerr);
        code = anpr::cameras::runCameras(options, anpr::AnprConfig{},
                                         std::make_shared<anpr::CollectingSink>(), stop, false);
        report = err.text();
    }
    CHECK_EQ(code, 0);
    CHECK(contains(report, "ERROR CAMERA_LAN_NOT_FOUND  kztest9"));
    CHECK(contains(report, "reason=stopped_before_camera_lan"));
    // No model was loaded and no status file written while waiting.
    CHECK(!contains(report, "models_ready"));
    CHECK(!anpr::filesystem::exists(dir / "status.json"));
}

TEST("camera commands: a model that cannot load is TENSORRT_NOT_AVAILABLE with exit code 4") {
    const anpr::filesystem::path dir = freshTempDir("run_models");
    writeFile(dir / "cameras.yaml", isolatedCameraConfig(dir));
    anpr::cameras::CameraCommandOptions options;
    options.camera_config_path = (dir / "cameras.yaml").string();
    anpr::AnprConfig config;
    config.detector.model = (dir / "missing_detector.onnx").string();
    config.ocr.model = (dir / "missing_ocr.onnx").string();
    config.inference.backend = anpr::InferenceBackend::kOnnxCpu;
    std::atomic_bool stop{false};
    std::string log;
    {
        StreamCapture err(std::cerr);
        CHECK_EQ(anpr::cameras::runCameras(options, config,
                                           std::make_shared<anpr::CollectingSink>(), stop, true),
                 4);
        CHECK_EQ(anpr::cameras::runSources({"a.mp4", "b.mp4"}, config,
                                           std::make_shared<anpr::CollectingSink>(), stop, false),
                 4);
        log = err.text();
    }
    CHECK(contains(log, "error=TENSORRT_NOT_AVAILABLE"));
    CHECK(contains(log, "stage=inference"));
    CHECK(contains(log, "event=model_load_failed"));
    // Warm-up never touches the network or the status file of a running service.
    CHECK(!contains(log, "CAMERA_LAN_NOT_FOUND"));
    CHECK(!anpr::filesystem::exists(dir / "status.json"));
}

TEST("camera commands: several sources share one model pair and end with the files") {
    if (!modelsPresent()) {
        std::cout << "  (skipped: models/ not present)\n";
        return;
    }
    const anpr::filesystem::path dir = freshTempDir("sources");
    const std::string clip = (dir / "clip.avi").string();
    {
        cv::VideoWriter writer(clip, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'), 10.0,
                               cv::Size(160, 120));
        CHECK(writer.isOpened());
        for (int index = 0; index < 12; ++index) {
            writer.write(cv::Mat(120, 160, CV_8UC3, cv::Scalar(40 + index, 90, 160)));
        }
    }
    anpr::AnprConfig config;
    config.inference.backend = anpr::InferenceBackend::kAuto;
    config.camera.camera_id = "bench";
    config.camera.realtime_file = false;
    config.camera.process_every_file_frame = true;
    config.performance.metrics_interval_ms = 0;
    std::atomic_bool stop{false};
    std::string log;
    int code = -1;
    int warmup_code = -1;
    int missing_code = -1;
    {
        StreamCapture err(std::cerr);
        warmup_code = anpr::cameras::runSources({clip, clip}, config,
                                                std::make_shared<anpr::CollectingSink>(), stop,
                                                true);
        missing_code = anpr::cameras::runSources({clip, (dir / "missing.mp4").string()}, config,
                                                 std::make_shared<anpr::CollectingSink>(), stop,
                                                 false);
        code = anpr::cameras::runSources({clip, clip}, config,
                                         std::make_shared<anpr::CollectingSink>(), stop, false);
        log = err.text();
    }
    CHECK_EQ(warmup_code, 0);
    CHECK_EQ(missing_code, 3);
    CHECK(contains(log, "event=camera_unavailable camera_id=bench-2"));
    CHECK_EQ(code, 0);
    CHECK(contains(log, "bench-1"));
    CHECK(contains(log, "bench-2"));
    CHECK(contains(log, "event=shutdown streams=2 frames_processed="));
    // One detector and one OCR for every run above: three loads, never one per stream.
    std::size_t loads = 0;
    for (std::size_t at = log.find("event=models_ready"); at != std::string::npos;
         at = log.find("event=models_ready", at + 1)) {
        ++loads;
    }
    CHECK_EQ(loads, std::size_t{3});
}
