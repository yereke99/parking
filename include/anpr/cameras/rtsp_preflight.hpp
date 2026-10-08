#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "anpr/cameras/camera_mode_config.hpp"
#include "anpr/cameras/diagnostics.hpp"
#include "anpr/cameras/discovery.hpp"
#include "anpr/net/http_auth.hpp"
#include "anpr/net/network_topology.hpp"
#include "anpr/net/rtsp_client.hpp"
#include "anpr/net/sdp.hpp"
#include "anpr/net/socket.hpp"

namespace anpr::cameras {

/// Everything needed to open one camera, resolved from discovery, the camera-mode config and the
/// environment. Holds the credentials in memory; `describe*` helpers never print them.
struct CameraTarget {
    std::string id;
    net::Ipv4 ip;
    std::string mac;
    std::string vendor;
    std::string model;
    std::string serial;
    std::uint16_t rtsp_port{554};
    std::string rtsp_path{"/Streaming/Channels/101"};
    StreamSelection stream{StreamSelection::kMain};
    std::uint16_t http_port{80};
    net::Credentials credentials;
    bool has_credentials{false};
    /// Env variable names the credentials came from (names only).
    std::string credential_source;
    DecoderPreference decoder{DecoderPreference::kAuto};
    bool enabled{true};
    bool native_anpr{false};
    std::string anpr_config;
    /// Problems discovery already found (other subnet, inactive, duplicate IP).
    std::vector<std::pair<CameraError, std::string>> discovery_problems;

    /// rtsp://<redacted>@ip:port/path or rtsp://ip:port/path.
    [[nodiscard]] std::string redactedUrl() const;
};

/// One target per discovered camera, in id order: overrides applied (enabled, port, stream,
/// path, decoder, credentials), and discovery-time defaults for the rest.
std::vector<CameraTarget> buildTargets(const DiscoveryReport& report, const CameraModeConfig& config,
                                       const EnvLookup& env);

/// The network/RTSP half of the stream preflight (no decoder involved).
struct RtspCheck {
    CameraError error{CameraError::kNone};
    std::string detail;
    /// Something answered at the address on any probed port.
    bool network_reachable{false};
    bool rtsp_port_open{false};
    bool auth_ok{false};
    bool path_ok{false};
    net::RtspProbeResult probe;
    net::SdpVideo video;
};

/// Maps a DESCRIBE result to the diagnostic code. `has_credentials` separates
/// RTSP_CREDENTIALS_MISSING from RTSP_AUTH_FAILED.
CameraError classifyRtspProbe(const net::RtspProbeResult& probe, bool has_credentials);

/// Refuses (CAMERA_ON_OTHER_SUBNET) when the camera's address would be routed through anything
/// but the camera LAN, then DESCRIBEs the stream once with credentials and, when the RTSP port
/// does not answer, checks the HTTP and SDK ports to tell RTSP_PORT_CLOSED from
/// CAMERA_UNREACHABLE. Never retries authentication.
RtspCheck checkRtsp(const CameraTarget& target, const net::NetworkSnapshot& snapshot,
                    const net::CameraLanSelection& lan, int timeout_ms);

}  // namespace anpr::cameras
