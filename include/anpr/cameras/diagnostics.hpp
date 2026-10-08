#pragma once

#include <string>

namespace anpr::cameras {

/// Every condition the camera subsystem reports to an installer. Each has a stable code (the
/// identifier in logs and the status file), a one-line meaning and one suggested action, so a
/// terminal line always says WHAT failed, WHERE, and what to do next.
enum class CameraError {
    kNone,
    // Camera LAN (Jetson side)
    kCameraLanNotFound,
    kCameraSubnetUnconfigured,
    kCameraLanLinkDown,
    kCameraLanHasDefaultRoute,
    kSubnetConflict,
    // Internet (GSM side), informational: camera processing never depends on it
    kNoInternetRoute,
    kInternetOffline,
    // Discovery
    kNoCamerasDiscovered,
    kDuplicateIpDetected,
    kCameraOnOtherSubnet,
    kCameraNotActivated,
    kOnvifDisabledOrUnavailable,
    kCameraLimitReached,
    kCameraDisabled,
    // Network / RTSP
    kCameraUnreachable,
    kRtspPortClosed,
    kRtspCredentialsMissing,
    kRtspAuthFailed,
    kRtspStreamPathInvalid,
    kRtspProtocolError,
    kIsapiUnavailable,
    // Stream / decoder
    kUnsupportedCodec,
    kStreamOpenFailed,
    kNoFramesReceived,
    kStreamTimeout,
    kStreamEnded,
    kHardwareDecoderUnavailable,
    // Inference / system
    kTensorrtNotAvailable,
    kOutOfMemoryRisk,
    kNativeAnprUnavailable,
};

enum class Severity { kInfo, kWarning, kError };

struct ErrorInfo {
    /// Stable identifier, for example "RTSP_AUTH_FAILED".
    const char* code;
    /// What the condition means, one sentence.
    const char* meaning;
    /// One short suggested action for the installer.
    const char* action;
    Severity severity;
};

const ErrorInfo& describe(CameraError error);
/// The stable code, "NONE" for kNone.
std::string toString(CameraError error);
/// Inverse of toString; kNone for unknown text.
CameraError cameraErrorFromString(const std::string& code);

/// Where in the chain a problem was found. Logged as `stage=`.
enum class Stage { kNetwork, kInternet, kDiscovery, kOnvif, kIsapi, kRtsp, kDecode, kFrames, kInference, kSystem };
std::string toString(Stage stage);

/// Who the problem is about. Empty fields are left out of the log line.
struct DiagnosticContext {
    std::string camera_id;
    std::string ip;
    std::string interface;
    Stage stage{Stage::kSystem};
};

/// One structured log line on stderr, level from the severity:
///   level=error event=camera_error camera_id=camera-03 ip=192.168.10.23 stage=rtsp
///   error=RTSP_AUTH_FAILED detail="..." action="..."
/// `detail` must never contain a password; callers pass redacted URLs only.
void reportCameraError(CameraError error, const DiagnosticContext& context,
                       const std::string& detail);

/// The same as human-readable lines for the terminal reports:
///   ERROR RTSP_AUTH_FAILED  camera-03 192.168.10.23
///     Camera rejected the configured username/password. <detail>
///     action: verify HIKVISION_USERNAME / HIKVISION_PASSWORD ...
std::string formatError(CameraError error, const DiagnosticContext& context,
                        const std::string& detail);

}  // namespace anpr::cameras
