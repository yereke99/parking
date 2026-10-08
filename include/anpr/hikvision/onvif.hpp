#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "anpr/net/http_auth.hpp"
#include "anpr/net/socket.hpp"

namespace anpr::hikvision {

/// ONVIF WS-Discovery (multicast 239.255.255.250:3702) and the two ONVIF device calls that are
/// useful for identification. Hikvision ships ONVIF disabled on current firmware and requires a
/// separate ONVIF user once enabled, so "no ONVIF answer" never means "no camera": the caller
/// reports ONVIF_DISABLED_OR_UNAVAILABLE and carries on with SADP/RTSP.
constexpr std::uint16_t kWsDiscoveryPort = 3702;
constexpr const char* kWsDiscoveryGroup = "239.255.255.250";

struct WsDiscoveryMatch {
    net::Ipv4 from;
    /// wsa:EndpointReference/Address, usually "urn:uuid:...".
    std::string endpoint;
    std::vector<std::string> xaddrs;
    std::vector<std::string> scopes;
    std::string types;
    /// Decoded from onvif://www.onvif.org/hardware/<model>, /name/<name>, /location/...
    std::string hardware;
    std::string name;
    /// "city/hangzhou": everything after /location/, percent-decoded.
    std::string location;
    /// MAC (normalized) from an onvif://www.onvif.org/MAC/ scope, or from a Hikvision endpoint
    /// UUID, whose last 12 hex digits are the MAC. Empty when neither applies.
    std::string mac;
    /// Host of the first device-service XAddr.
    net::Ipv4 xaddr_host;
    std::uint16_t xaddr_port{80};
    std::string xaddr_path{"/onvif/device_service"};
};

/// A WS-Discovery Probe for dn:NetworkVideoTransmitter with the given MessageID UUID.
std::string buildWsDiscoveryProbe(const std::string& message_uuid);

/// Every ProbeMatch in one datagram.
std::vector<WsDiscoveryMatch> parseProbeMatches(const std::string& payload, net::Ipv4 from);

struct WsDiscoveryResult {
    bool ran{false};
    std::string error;
    std::vector<WsDiscoveryMatch> matches;
};

WsDiscoveryResult wsDiscover(const std::string& interface, net::Ipv4 interface_address,
                             int listen_ms);

enum class OnvifState {
    /// The device service answered GetSystemDateAndTime (and GetDeviceInformation when asked).
    kAvailable,
    /// The device service exists but GetDeviceInformation needs ONVIF credentials that were
    /// either not configured or rejected.
    kAuthRequired,
    /// Nothing answers ONVIF SOAP at that address: ONVIF disabled or not supported.
    kUnavailable,
    kError,
};

std::string toString(OnvifState state);

struct OnvifDeviceInfo {
    OnvifState state{OnvifState::kUnavailable};
    std::string manufacturer;
    std::string model;
    std::string firmware;
    std::string serial;
    std::string hardware_id;
    std::string detail;
};

/// GetSystemDateAndTime (never authenticated), then GetDeviceInformation with a WS-Security
/// UsernameToken when `onvif_credentials` is non-null (one attempt). Hikvision ONVIF users are
/// separate accounts; never pass the main camera password here unless the operator configured it
/// as the ONVIF account.
OnvifDeviceInfo onvifProbeDevice(net::Ipv4 host, std::uint16_t port, const std::string& path,
                                 const net::Credentials* onvif_credentials, int timeout_ms);

/// Base64(SHA1(nonce + created + password)), the WS-Security PasswordDigest.
std::string onvifPasswordDigest(const std::string& nonce_raw, const std::string& created,
                                const std::string& password);

}  // namespace anpr::hikvision
