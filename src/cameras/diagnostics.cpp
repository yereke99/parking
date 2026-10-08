#include "anpr/cameras/diagnostics.hpp"

#include <array>
#include <cstddef>
#include <sstream>

#include "anpr/common/logging.hpp"

namespace anpr::cameras {
namespace {

struct CatalogEntry {
    CameraError error;
    ErrorInfo info;
};

/// The installer-facing texts. Codes are stable identifiers (logs, status file, scripts); the
/// meaning and action are what an operator reads at the barrier, so they name the cable, the
/// variable or the make target to touch.
const std::array<CatalogEntry, 31> kCatalog = {{
    {CameraError::kNone,
     {"NONE", "No problem was detected.", "Nothing to do.", Severity::kInfo}},
    {CameraError::kCameraLanNotFound,
     {"CAMERA_LAN_NOT_FOUND",
      "No wired Ethernet interface has link: the Jetson is not connected to the PoE switch.",
      "Check the cable from the Jetson Ethernet port to the switch uplink port and that the "
      "switch is powered.",
      Severity::kError}},
    {CameraError::kCameraSubnetUnconfigured,
     {"CAMERA_SUBNET_UNCONFIGURED",
      "The Ethernet link to the PoE switch is up but the Jetson has no usable IPv4 address on it "
      "(the switch has no DHCP server).",
      "Give the Jetson a static address in the cameras' subnet: make camera-lan-setup "
      "ADDRESS=<ip>/24.",
      Severity::kError}},
    {CameraError::kCameraLanLinkDown,
     {"CAMERA_LAN_LINK_DOWN", "The camera LAN interface lost its link while running.",
      "Check the Jetson-to-switch cable and the switch power; cameras reconnect automatically.",
      Severity::kError}},
    {CameraError::kCameraLanHasDefaultRoute,
     {"CAMERA_LAN_HAS_DEFAULT_ROUTE",
      "The camera LAN interface carries the default route, so Internet traffic goes to the PoE "
      "switch instead of the GSM modem.",
      "Set ipv4.never-default on the camera LAN connection (make camera-lan-setup) and remove "
      "its gateway.",
      Severity::kWarning}},
    {CameraError::kSubnetConflict,
     {"SUBNET_CONFLICT",
      "Another interface (a Docker bridge, the USB gadget or the modem) uses a subnet that "
      "overlaps the camera LAN.",
      "Move the cameras to a non-overlapping subnet or set Docker's default-address-pools.",
      Severity::kWarning}},
    {CameraError::kNoInternetRoute,
     {"NO_INTERNET_ROUTE",
      "There is no default route, so no Internet uplink is configured. Camera processing does "
      "not need it.",
      "Connect the USB GSM modem if events must be sent upstream.", Severity::kInfo}},
    {CameraError::kInternetOffline,
     {"INTERNET_OFFLINE",
      "A default route exists but the Internet probe failed. Camera processing continues.",
      "Check the GSM modem signal, SIM card and APN.", Severity::kWarning}},
    {CameraError::kNoCamerasDiscovered,
     {"NO_CAMERAS_DISCOVERED",
      "The camera LAN is up, but no Hikvision/ONVIF/RTSP device answered.",
      "Check camera PoE power and cabling; list known camera IPs under discovery.manual_hosts.",
      Severity::kError}},
    {CameraError::kDuplicateIpDetected,
     {"DUPLICATE_IP_DETECTED", "More than one device answers for the same IP address.",
      "Give each camera a unique IP (Hikvision SADP tool or web UI); connect factory-reset "
      "cameras one at a time.",
      Severity::kError}},
    {CameraError::kCameraOnOtherSubnet,
     {"CAMERA_ON_OTHER_SUBNET",
      "The camera answered discovery but its IP is outside the Jetson's camera subnet, so it "
      "cannot be reached over the camera LAN.",
      "Change the camera IP into the camera subnet, or add an address in its subnet to the "
      "Jetson (make camera-lan-setup).",
      Severity::kError}},
    {CameraError::kCameraNotActivated,
     {"CAMERA_NOT_ACTIVATED",
      "The Hikvision camera is not activated (factory state, no admin password) and will not "
      "stream.",
      "Activate it with Hikvision SADP or its web page and set a strong admin password.",
      Severity::kError}},
    {CameraError::kOnvifDisabledOrUnavailable,
     {"ONVIF_DISABLED_OR_UNAVAILABLE",
      "The camera is reachable but did not answer ONVIF (disabled by default on Hikvision). Not "
      "needed: RTSP is used.",
      "Optional: enable ONVIF and create an ONVIF user in the camera web UI (Network > Advanced "
      "> Integration Protocol).",
      Severity::kInfo}},
    {CameraError::kCameraLimitReached,
     {"CAMERA_LIMIT_REACHED",
      "More cameras were found than runtime.max_active_cameras; the extra cameras are not "
      "processed.",
      "Raise runtime.max_active_cameras if the Jetson has headroom, or disable cameras in "
      "config/cameras.yaml.",
      Severity::kWarning}},
    {CameraError::kCameraDisabled,
     {"CAMERA_DISABLED", "The camera is disabled in config/cameras.yaml.",
      "Set enabled: true for this camera to process it.", Severity::kInfo}},
    {CameraError::kCameraUnreachable,
     {"CAMERA_UNREACHABLE", "The camera was discovered but does not answer on the network now.",
      "Check its PoE port, cable and power; it may be rebooting.", Severity::kError}},
    {CameraError::kRtspPortClosed,
     {"RTSP_PORT_CLOSED",
      "The camera responds on the network, but nothing listens on its RTSP port.",
      "Enable RTSP or check the RTSP port in the camera (Network > Advanced > Port), or set "
      "rtsp_port.",
      Severity::kError}},
    {CameraError::kRtspCredentialsMissing,
     {"RTSP_CREDENTIALS_MISSING",
      "The camera requires a login but no credentials are configured.",
      "Set HIKVISION_USERNAME and HIKVISION_PASSWORD (config/cameras.env).", Severity::kError}},
    {CameraError::kRtspAuthFailed,
     {"RTSP_AUTH_FAILED", "The camera rejected the configured username/password.",
      "Verify HIKVISION_USERNAME / HIKVISION_PASSWORD (or the per-camera variables); repeated "
      "failures lock the camera for 30 minutes.",
      Severity::kError}},
    {CameraError::kRtspStreamPathInvalid,
     {"RTSP_STREAM_PATH_INVALID",
      "Authentication works but the requested stream does not exist.",
      "Check rtsp.stream / rtsp_path (Hikvision main stream 101, sub stream 102).",
      Severity::kError}},
    {CameraError::kRtspProtocolError,
     {"RTSP_PROTOCOL_ERROR",
      "The RTSP port answered with something that is not a valid RTSP response.",
      "Check that the configured port is the camera's RTSP port.", Severity::kError}},
    {CameraError::kIsapiUnavailable,
     {"ISAPI_UNAVAILABLE",
      "The camera's ISAPI web service did not answer, so model/serial details may be missing.",
      "Optional: check the HTTP port; streaming does not need ISAPI.", Severity::kInfo}},
    {CameraError::kUnsupportedCodec,
     {"UNSUPPORTED_CODEC", "The stream codec cannot be decoded with this Jetson setup.",
      "Set the camera stream to H.264 or H.265 in its Video settings.", Severity::kError}},
    {CameraError::kStreamOpenFailed,
     {"STREAM_OPEN_FAILED", "RTSP works, but the decoder could not open the video.",
      "Run make camera-check to see each decoder's error; try decode.decoder: software.",
      Severity::kError}},
    {CameraError::kNoFramesReceived,
     {"NO_FRAMES_RECEIVED",
      "The stream opened but no video frames arrived within the timeout.",
      "Check the camera's stream settings and bitrate, or reboot the camera.",
      Severity::kError}},
    {CameraError::kStreamTimeout,
     {"STREAM_TIMEOUT",
      "The stream stopped delivering frames (camera reboot, PoE loss or network stall); "
      "reconnecting.",
      "Nothing if it recovers; otherwise check the camera power and cable.",
      Severity::kWarning}},
    {CameraError::kStreamEnded,
     {"STREAM_ENDED",
      "The camera closed the stream (EOF or connection reset); reconnecting.",
      "Nothing if it recovers; check the camera log if it repeats.", Severity::kWarning}},
    {CameraError::kHardwareDecoderUnavailable,
     {"HARDWARE_DECODER_UNAVAILABLE",
      "NVIDIA hardware decoding is not available in this container; decoding falls back to the "
      "CPU.",
      "Run the image with --runtime nvidia on JetPack 4.6 (keep nvidia-container-toolkit "
      "1.7.0).",
      Severity::kWarning}},
    {CameraError::kTensorrtNotAvailable,
     {"TENSORRT_NOT_AVAILABLE",
      "Video works but the AI inference backend (TensorRT) could not start.",
      "Run make check; rebuild the image with make docker-build if TensorRT is missing.",
      Severity::kError}},
    {CameraError::kOutOfMemoryRisk,
     {"OUT_OF_MEMORY_RISK", "Available Jetson RAM is below the safe threshold.",
      "Stop other processes, use the sub-stream, or reduce the number of active cameras.",
      Severity::kWarning}},
    {CameraError::kNativeAnprUnavailable,
     {"NATIVE_ANPR_UNAVAILABLE",
      "The camera's own ANPR events are not available (not supported, not enabled, or "
      "rejected).",
      "Optional: enable ANPR on the camera, or set native_anpr.enabled: false.",
      Severity::kInfo}},
}};

const char* severityLabel(Severity severity) {
    switch (severity) {
        case Severity::kInfo:
            return "INFO";
        case Severity::kWarning:
            return "WARNING";
        case Severity::kError:
            return "ERROR";
    }
    return "ERROR";
}

LogLevel logLevelFor(Severity severity) {
    switch (severity) {
        case Severity::kInfo:
            return LogLevel::kInfo;
        case Severity::kWarning:
            return LogLevel::kWarn;
        case Severity::kError:
            return LogLevel::kError;
    }
    return LogLevel::kError;
}

/// True when the calling thread's LogContext already carries `key=value`, so a camera thread
/// that set `camera_id=camera-02` does not get the field twice on one line.
bool contextHasField(const char* key, const std::string& value) {
    const std::string& context = LogContext::current();
    if (context.empty()) {
        return false;
    }
    const std::string field = std::string(key) + "=" + quoteLogValue(value);
    std::size_t position = context.find(field);
    while (position != std::string::npos) {
        const bool starts_token = position == 0 || context[position - 1] == ' ';
        const std::size_t end = position + field.size();
        const bool ends_token = end == context.size() || context[end] == ' ';
        if (starts_token && ends_token) {
            return true;
        }
        position = context.find(field, position + 1);
    }
    return false;
}

}  // namespace

const ErrorInfo& describe(CameraError error) {
    for (const CatalogEntry& entry : kCatalog) {
        if (entry.error == error) {
            return entry.info;
        }
    }
    // Unreachable for valid enum values (the catalog test covers every one).
    return kCatalog.front().info;
}

std::string toString(CameraError error) {
    return describe(error).code;
}

CameraError cameraErrorFromString(const std::string& code) {
    for (const CatalogEntry& entry : kCatalog) {
        if (code == entry.info.code) {
            return entry.error;
        }
    }
    return CameraError::kNone;
}

std::string toString(Stage stage) {
    switch (stage) {
        case Stage::kNetwork:
            return "network";
        case Stage::kInternet:
            return "internet";
        case Stage::kDiscovery:
            return "discovery";
        case Stage::kOnvif:
            return "onvif";
        case Stage::kIsapi:
            return "isapi";
        case Stage::kRtsp:
            return "rtsp";
        case Stage::kDecode:
            return "decode";
        case Stage::kFrames:
            return "frames";
        case Stage::kInference:
            return "inference";
        case Stage::kSystem:
            return "system";
    }
    return "system";
}

void reportCameraError(CameraError error, const DiagnosticContext& context,
                       const std::string& detail) {
    const ErrorInfo& info = describe(error);
    const bool network = context.stage == Stage::kNetwork || context.stage == Stage::kInternet;

    LogFields fields;
    if (!context.camera_id.empty() && !contextHasField("camera_id", context.camera_id)) {
        fields.addQuoted("camera_id", context.camera_id);
    }
    if (!context.ip.empty()) {
        fields.addQuoted("ip", context.ip);
    }
    if (!context.interface.empty()) {
        fields.addQuoted("interface", context.interface);
    }
    fields.add("stage", toString(context.stage));
    fields.add("error", info.code);
    if (!detail.empty()) {
        fields.addQuoted("detail", detail);
    }
    fields.addQuoted("action", info.action);
    logEvent(logLevelFor(info.severity), network ? "network_error" : "camera_error", fields);
}

std::string formatError(CameraError error, const DiagnosticContext& context,
                        const std::string& detail) {
    const ErrorInfo& info = describe(error);
    std::ostringstream out;
    out << severityLabel(info.severity) << ' ' << info.code;
    const char* separator = "  ";
    for (const std::string* part : {&context.camera_id, &context.ip, &context.interface}) {
        if (!part->empty()) {
            out << separator << *part;
            separator = " ";
        }
    }
    out << "\n  " << info.meaning;
    if (!detail.empty()) {
        // On its own line: the detail is a free-form fragment ("the active default route leaves
        // through eth0"), not a continuation of the catalog sentence.
        out << "\n  detail: " << detail;
    }
    out << "\n  action: " << info.action << '\n';
    return out.str();
}

}  // namespace anpr::cameras
