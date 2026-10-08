// Per-camera targets (discovery + overrides + environment credentials) and the RTSP half of the
// stream preflight: one DESCRIBE with credentials, classified into the field diagnostics before
// any decoder is involved.
#include "anpr/cameras/rtsp_preflight.hpp"

#include <algorithm>
#include <utility>

#include "anpr/common/logging.hpp"

namespace anpr::cameras {
namespace {

/// Problems discovery found that say something about the camera's configuration rather than
/// about this moment's network, carried into the target for the reports.
bool isConfigurationProblem(CameraError error) {
    return error == CameraError::kCameraOnOtherSubnet ||
           error == CameraError::kCameraNotActivated ||
           error == CameraError::kDuplicateIpDetected;
}

/// Problems that forbid contacting the camera at all: an address outside the camera LAN would
/// leave through the GSM modem, and an inactive Hikvision camera has no password to accept, so
/// any login attempt only counts towards its lockout.
bool blocksContact(CameraError error) {
    return error == CameraError::kCameraOnOtherSubnet || error == CameraError::kCameraNotActivated;
}

std::string hostPort(net::Ipv4 host, std::uint16_t port) {
    return net::toString(host) + ":" + std::to_string(port);
}

}  // namespace

std::string CameraTarget::redactedUrl() const {
    return net::redactedRtspUrl(ip, rtsp_port, rtsp_path, has_credentials);
}

std::vector<CameraTarget> buildTargets(const DiscoveryReport& report,
                                       const CameraModeConfig& config, const EnvLookup& env) {
    std::vector<const DiscoveredCamera*> cameras;
    cameras.reserve(report.cameras.size());
    for (const DiscoveredCamera& camera : report.cameras) {
        cameras.push_back(&camera);
    }
    std::stable_sort(cameras.begin(), cameras.end(),
                     [](const DiscoveredCamera* lhs, const DiscoveredCamera* rhs) {
                         return cameraIdLess(lhs->id, rhs->id);
                     });

    std::vector<CameraTarget> targets;
    targets.reserve(cameras.size());
    for (const DiscoveredCamera* camera : cameras) {
        const CameraOverride* camera_override = findOverride(
            config, camera->id, camera->mac, camera->serial, net::toString(camera->ip));

        CameraTarget target;
        target.id = camera->id;
        target.ip = camera->ip;
        target.mac = camera->mac;
        target.vendor = camera->vendor;
        target.model = camera->model;
        target.serial = camera->serial;
        target.rtsp_port = camera->rtsp_port;
        target.http_port = camera->http_port;
        target.stream = config.rtsp.stream;
        target.decoder = config.decode.decoder;
        target.native_anpr = config.native_anpr.enabled;

        if (camera_override != nullptr) {
            target.enabled = camera_override->enabled.value_or(true);
            if (camera_override->rtsp_port && *camera_override->rtsp_port > 0 &&
                *camera_override->rtsp_port <= 65535) {
                target.rtsp_port = static_cast<std::uint16_t>(*camera_override->rtsp_port);
            }
            if (camera_override->stream) {
                target.stream = *camera_override->stream;
            }
            if (camera_override->http_port && *camera_override->http_port > 0 &&
                *camera_override->http_port <= 65535) {
                target.http_port = static_cast<std::uint16_t>(*camera_override->http_port);
            }
            if (camera_override->decoder) {
                target.decoder = *camera_override->decoder;
            }
            if (camera_override->native_anpr) {
                target.native_anpr = *camera_override->native_anpr;
            }
            target.anpr_config = camera_override->anpr_config;
        }
        target.rtsp_path = camera_override != nullptr && !camera_override->rtsp_path.empty()
                               ? camera_override->rtsp_path
                               : (target.stream == StreamSelection::kSub ? config.rtsp.sub_path
                                                                         : config.rtsp.main_path);

        const ResolvedCredentials credentials =
            resolveCredentials(config, camera_override, camera->id, env);
        target.has_credentials = credentials.present;
        target.credential_source = credentials.source;
        if (credentials.present) {
            target.credentials = credentials.credentials;
        }

        for (const auto& problem : camera->problems) {
            if (isConfigurationProblem(problem.first)) {
                target.discovery_problems.push_back(problem);
            }
        }
        targets.push_back(std::move(target));
    }
    return targets;
}

CameraError classifyRtspProbe(const net::RtspProbeResult& probe, bool has_credentials) {
    switch (probe.status) {
        case net::RtspProbeStatus::kOk:
            return CameraError::kNone;
        case net::RtspProbeStatus::kUnreachable:
            return CameraError::kCameraUnreachable;
        case net::RtspProbeStatus::kPortClosed:
            return CameraError::kRtspPortClosed;
        case net::RtspProbeStatus::kTimeout:
            // Connected but silent is a broken service; never connected is a host that is gone.
            return probe.connect == net::ConnectOutcome::kConnected
                       ? CameraError::kRtspProtocolError
                       : CameraError::kCameraUnreachable;
        case net::RtspProbeStatus::kAuthRequired:
            return has_credentials ? CameraError::kRtspAuthFailed
                                   : CameraError::kRtspCredentialsMissing;
        case net::RtspProbeStatus::kAuthFailed:
            return CameraError::kRtspAuthFailed;
        case net::RtspProbeStatus::kForbidden:
            return has_credentials ? CameraError::kRtspAuthFailed
                                   : CameraError::kRtspCredentialsMissing;
        case net::RtspProbeStatus::kPathInvalid:
            return CameraError::kRtspStreamPathInvalid;
        case net::RtspProbeStatus::kServerError:
        case net::RtspProbeStatus::kProtocolError:
            return CameraError::kRtspProtocolError;
    }
    return CameraError::kRtspProtocolError;
}

RtspCheck checkRtsp(const CameraTarget& target, const net::NetworkSnapshot& snapshot,
                    const net::CameraLanSelection& lan, int timeout_ms) {
    RtspCheck check;
    const std::string url = target.redactedUrl();
    const std::string ip_text = net::toString(target.ip);

    for (const auto& problem : target.discovery_problems) {
        if (blocksContact(problem.first)) {
            check.error = problem.first;
            check.detail = problem.second.empty() ? "Not contacted"
                                                  : problem.second + "; not contacted";
            return check;
        }
    }

    // The kernel would pick this route for the connection. Anything but the camera LAN means
    // the packets leave through the GSM modem (or nowhere): refuse rather than leak a login.
    const net::RouteEntry* route = snapshot.routeFor(target.ip);
    if (lan.interface.empty() || route == nullptr || route->interface != lan.interface) {
        check.error = CameraError::kCameraOnOtherSubnet;
        if (lan.interface.empty()) {
            check.detail = "No camera LAN interface is selected; " + ip_text + " not contacted";
        } else if (route == nullptr) {
            check.detail = "No route to " + ip_text + " (camera LAN " + lan.interface +
                           "); not contacted";
        } else {
            check.detail = "The route to " + ip_text + " leaves through " + route->interface +
                           ", not the camera LAN " + lan.interface + "; not contacted";
        }
        return check;
    }

    net::RtspProbeOptions options;
    options.host = target.ip;
    options.port = target.rtsp_port;
    options.path = target.rtsp_path;
    options.timeout_ms = timeout_ms;
    options.method = "DESCRIBE";
    options.credentials =
        target.has_credentials && !target.credentials.empty() ? &target.credentials : nullptr;
    check.probe = net::rtspProbe(options);
    const net::RtspProbeResult& probe = check.probe;

    check.rtsp_port_open = probe.connect == net::ConnectOutcome::kConnected;
    check.network_reachable =
        check.rtsp_port_open || probe.connect == net::ConnectOutcome::kRefused;
    check.auth_ok = probe.status == net::RtspProbeStatus::kOk ||
                    probe.status == net::RtspProbeStatus::kPathInvalid;
    check.path_ok = probe.status == net::RtspProbeStatus::kOk;
    check.error = classifyRtspProbe(probe, options.credentials != nullptr);
    check.detail = "DESCRIBE " + url + ": " + probe.detail;

    if (!check.rtsp_port_open) {
        // Nothing on the RTSP port: ask the web and SDK ports whether the camera itself is up,
        // which separates "RTSP disabled or wrong port" from "camera powered off".
        std::vector<std::uint16_t> ports;
        for (const std::uint16_t port : {target.http_port, std::uint16_t{8000}}) {
            if (port != 0 && port != target.rtsp_port &&
                std::find(ports.begin(), ports.end(), port) == ports.end()) {
                ports.push_back(port);
            }
        }
        const int probe_timeout = std::max(1, std::min(timeout_ms, 1500));
        std::string answered;
        for (const net::PortProbe& result :
             net::probePorts({target.ip}, ports, probe_timeout, static_cast<int>(ports.size()))) {
            if (result.outcome == net::ConnectOutcome::kConnected ||
                result.outcome == net::ConnectOutcome::kRefused) {
                answered += (answered.empty() ? "" : ", ") + std::to_string(result.port) + " " +
                            (result.outcome == net::ConnectOutcome::kConnected ? "open"
                                                                               : "closed");
            }
        }
        if (!answered.empty()) {
            check.network_reachable = true;
        }
        if (check.network_reachable) {
            check.error = CameraError::kRtspPortClosed;
            check.detail = "Nothing answers RTSP on " + hostPort(target.ip, target.rtsp_port) +
                           " (" + net::toString(probe.connect) + ")" +
                           (answered.empty() ? std::string()
                                             : "; the camera itself answers (port " + answered +
                                                   ")");
        } else {
            check.error = CameraError::kCameraUnreachable;
            check.detail = "DESCRIBE " + url + ": " + probe.detail +
                           "; no answer on the HTTP/SDK ports either";
        }
    } else if (check.error == CameraError::kNone) {
        check.video = net::parseSdpVideo(probe.sdp);
        if (!check.video.present) {
            check.error = CameraError::kUnsupportedCodec;
            check.detail = "DESCRIBE " + url + " returned no video stream in its SDP";
        } else if (check.video.codec != net::VideoCodec::kH264 &&
                   check.video.codec != net::VideoCodec::kH265) {
            check.error = CameraError::kUnsupportedCodec;
            check.detail = "The stream at " + url + " is " + net::toString(check.video.codec) +
                           (check.video.encoding_name.empty()
                                ? std::string()
                                : " (rtpmap " + check.video.encoding_name + ")") +
                           "; only H.264 and H.265 are decoded";
        } else {
            check.detail = "DESCRIBE " + url + " ok: " + net::toString(check.video.codec) +
                           (check.video.width > 0
                                ? " " + std::to_string(check.video.width) + "x" +
                                      std::to_string(check.video.height)
                                : std::string());
        }
    }

    logEvent(LogLevel::kDebug, "rtsp_check",
             LogFields()
                 .addQuoted("camera_id", target.id)
                 .add("ip", ip_text)
                 .addQuoted("url", url)
                 .add("status", net::toString(probe.status))
                 .add("code", probe.code)
                 .add("error", toString(check.error))
                 .add("ms", static_cast<long long>(probe.total_ms)));
    return check;
}

}  // namespace anpr::cameras
