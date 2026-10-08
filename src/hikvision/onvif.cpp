// WS-Discovery probe building and ProbeMatch parsing. The multicast exchange and the ONVIF
// device calls (wsDiscover, onvifProbeDevice, onvifPasswordDigest) are in the "Network" section
// at the end.
#include "anpr/hikvision/onvif.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <utility>

#include "anpr/common/logging.hpp"
#include "anpr/hikvision/xml_lite.hpp"
#include "anpr/net/crypto.hpp"
#include "anpr/net/http_client.hpp"

namespace anpr::hikvision {
namespace {

constexpr const char* kOnvifScopePrefix = "onvif://www.onvif.org/";

bool startsWithNoCase(const std::string& text, const std::string& prefix) {
    if (text.size() < prefix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(text[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

int hexValue(char ch) {
    const auto byte = static_cast<unsigned char>(ch);
    if (std::isdigit(byte) != 0) {
        return ch - '0';
    }
    if (std::isxdigit(byte) != 0) {
        return std::tolower(byte) - 'a' + 10;
    }
    return -1;
}

/// RFC 3986 percent-decoding ("HIKVISION%20DS-TCG406-E"). A '%' not followed by two hex digits
/// is kept as written; '+' is not a space outside form encoding.
std::string percentDecode(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size()) {
            const int high = hexValue(text[i + 1]);
            const int low = hexValue(text[i + 2]);
            if (high >= 0 && low >= 0) {
                out.push_back(static_cast<char>(high * 16 + low));
                i += 2;
                continue;
            }
        }
        out.push_back(text[i]);
    }
    return out;
}

std::vector<std::string> splitWhitespace(const std::string& text) {
    std::vector<std::string> items;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])) != 0) {
            ++i;
        }
        const std::size_t begin = i;
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])) == 0) {
            ++i;
        }
        if (i > begin) {
            items.push_back(text.substr(begin, i - begin));
        }
    }
    return items;
}

/// Decoded value of the first scope "onvif://www.onvif.org/<category>/<value>". Categories are
/// compared without case: firmware writes both "MAC" and "mac", "Hardware" and "hardware".
std::string scopeValue(const std::vector<std::string>& scopes, const std::string& category) {
    const std::string prefix = std::string(kOnvifScopePrefix) + category + "/";
    for (const std::string& scope : scopes) {
        if (startsWithNoCase(scope, prefix) && scope.size() > prefix.size()) {
            return percentDecode(scope.substr(prefix.size()));
        }
    }
    return {};
}

/// Hikvision firmware builds its WS-Discovery endpoint UUID with the device MAC as the last 12
/// hex digits ("urn:uuid:3fa1fe68-b915-4053-a3e1-c42f90a7b5d1"). Other vendors use random or
/// time-based UUIDs whose last group is not necessarily a MAC of this device, so the digits are
/// taken only from a canonical 8-4-4-4-12 UUID whose last group starts with a Hikvision OUI. A
/// random UUID passes that test by chance with a probability of about 1 in 400 000.
std::string macFromEndpoint(const std::string& endpoint) {
    std::string uuid = endpoint;
    if (startsWithNoCase(uuid, "urn:uuid:")) {
        uuid = uuid.substr(9);
    } else if (startsWithNoCase(uuid, "uuid:")) {
        uuid = uuid.substr(5);
    }
    if (uuid.size() != 36) {
        return {};
    }
    for (std::size_t i = 0; i < uuid.size(); ++i) {
        const bool dash_position = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash_position ? uuid[i] != '-' : hexValue(uuid[i]) < 0) {
            return {};
        }
    }
    const std::string mac = net::normalizeMac(uuid.substr(24));
    return net::isHikvisionOui(mac) ? mac : std::string();
}

/// "http://192.168.1.64[:port]/onvif/device_service". Host names and IPv6 literals are refused:
/// the camera is contacted by IPv4 on the camera LAN only.
bool parseHttpXaddr(const std::string& xaddr, net::Ipv4& host, std::uint16_t& port,
                    std::string& path) {
    const std::string scheme = "http://";
    if (!startsWithNoCase(xaddr, scheme)) {
        return false;
    }
    const std::string rest = xaddr.substr(scheme.size());
    const std::size_t slash = rest.find('/');
    std::string authority = rest.substr(0, slash);
    const std::size_t at = authority.rfind('@');
    if (at != std::string::npos) {
        authority = authority.substr(at + 1);
    }
    if (authority.empty() || authority.front() == '[') {
        return false;
    }
    net::Ipv4 parsed_host;
    std::uint16_t parsed_port = 80;
    if (!net::parseHostPort(authority, parsed_host, parsed_port) || parsed_host.isZero()) {
        return false;
    }
    host = parsed_host;
    port = parsed_port;
    path = slash == std::string::npos ? std::string("/") : rest.substr(slash);
    return true;
}

}  // namespace

std::string buildWsDiscoveryProbe(const std::string& message_uuid) {
    std::string id = message_uuid;
    if (startsWithNoCase(id, "urn:uuid:")) {
        id = id.substr(9);
    } else if (startsWithNoCase(id, "uuid:")) {
        id = id.substr(5);
    }
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
           "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\""
           " xmlns:a=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\""
           " xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\""
           " xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\">"
           "<s:Header>"
           "<a:Action s:mustUnderstand=\"1\">"
           "http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</a:Action>"
           "<a:MessageID>uuid:" +
           xml::escape(id) +
           "</a:MessageID>"
           "<a:ReplyTo><a:Address>"
           "http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous"
           "</a:Address></a:ReplyTo>"
           "<a:To s:mustUnderstand=\"1\">urn:schemas-xmlsoap-org:ws:2005:04:discovery</a:To>"
           "</s:Header>"
           "<s:Body><d:Probe><d:Types>dn:NetworkVideoTransmitter</d:Types></d:Probe></s:Body>"
           "</s:Envelope>";
}

std::vector<WsDiscoveryMatch> parseProbeMatches(const std::string& payload, net::Ipv4 from) {
    std::vector<WsDiscoveryMatch> matches;
    // A looped-back Probe (ours or another client's) has no ProbeMatch and yields nothing.
    for (const std::string& block : xml::allElements(payload, "ProbeMatch")) {
        WsDiscoveryMatch match;
        match.from = from;
        if (const auto reference = xml::firstElement(block, "EndpointReference")) {
            match.endpoint = xml::trim(xml::firstText(*reference, "Address").value_or(""));
        }
        match.types = xml::trim(xml::firstText(block, "Types").value_or(""));
        match.scopes = splitWhitespace(xml::firstText(block, "Scopes").value_or(""));
        match.xaddrs = splitWhitespace(xml::firstText(block, "XAddrs").value_or(""));
        if (match.endpoint.empty() && match.xaddrs.empty()) {
            continue;
        }
        match.hardware = scopeValue(match.scopes, "hardware");
        match.name = scopeValue(match.scopes, "name");
        match.location = scopeValue(match.scopes, "location");
        match.mac = net::normalizeMac(scopeValue(match.scopes, "MAC"));
        if (match.mac.empty()) {
            match.mac = macFromEndpoint(match.endpoint);
        }
        for (const std::string& xaddr : match.xaddrs) {
            if (parseHttpXaddr(xaddr, match.xaddr_host, match.xaddr_port, match.xaddr_path)) {
                break;
            }
        }
        matches.push_back(std::move(match));
    }
    return matches;
}

std::string toString(OnvifState state) {
    switch (state) {
        case OnvifState::kAvailable:
            return "available";
        case OnvifState::kAuthRequired:
            return "auth_required";
        case OnvifState::kUnavailable:
            return "unavailable";
        case OnvifState::kError:
            return "error";
    }
    return "error";
}

// ---------------------------------------------------------------------------------------------
// Network
// ---------------------------------------------------------------------------------------------

namespace {

constexpr const char* kDeviceServicePath = "/onvif/device_service";
constexpr const char* kDeviceNamespace = "http://www.onvif.org/ver10/device/wsdl";
constexpr std::size_t kMaxSoapBytes = 256U * 1024U;
/// Cameras reject a UsernameToken whose Created is too far from their own clock, and a camera
/// on an isolated LAN without NTP is often minutes (or years) off.
constexpr std::int64_t kMaxClockSkewSeconds = 5;
constexpr std::size_t kMaxFaultText = 200;

/// Merges what a repeated answer adds (the probe is sent twice, so every device answers twice).
void mergeMatch(WsDiscoveryMatch& into, const WsDiscoveryMatch& from) {
    const auto fill = [](std::string& target, const std::string& source) {
        if (target.empty()) {
            target = source;
        }
    };
    fill(into.types, from.types);
    fill(into.hardware, from.hardware);
    fill(into.name, from.name);
    fill(into.location, from.location);
    fill(into.mac, from.mac);
    for (const std::string& xaddr : from.xaddrs) {
        if (std::find(into.xaddrs.begin(), into.xaddrs.end(), xaddr) == into.xaddrs.end()) {
            into.xaddrs.push_back(xaddr);
        }
    }
    for (const std::string& scope : from.scopes) {
        if (std::find(into.scopes.begin(), into.scopes.end(), scope) == into.scopes.end()) {
            into.scopes.push_back(scope);
        }
    }
    if (into.xaddr_host.isZero() && !from.xaddr_host.isZero()) {
        into.xaddr_host = from.xaddr_host;
        into.xaddr_port = from.xaddr_port;
        into.xaddr_path = from.xaddr_path;
    }
}

/// The endpoint reference identifies a device across answers; a device without one is
/// identified by its service addresses.
std::string matchKey(const WsDiscoveryMatch& match) {
    if (!match.endpoint.empty()) {
        return "endpoint:" + match.endpoint;
    }
    std::string key = "xaddrs:" + net::toString(match.from);
    for (const std::string& xaddr : match.xaddrs) {
        key += " " + xaddr;
    }
    return key;
}

std::string soapEnvelope(const std::string& header, const std::string& body) {
    return std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                       "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\""
                       " xmlns:tds=\"") +
           kDeviceNamespace + "\">" +
           (header.empty() ? std::string() : "<s:Header>" + header + "</s:Header>") +
           "<s:Body>" + body + "</s:Body></s:Envelope>";
}

/// WS-Security UsernameToken with a PasswordDigest: the password itself never goes on the wire.
std::string usernameToken(const net::Credentials& credentials, const std::string& nonce_raw,
                          const std::string& created) {
    return "<wsse:Security s:mustUnderstand=\"1\""
           " xmlns:wsse=\"http://docs.oasis-open.org/wss/2004/01/"
           "oasis-200401-wss-wssecurity-secext-1.0.xsd\""
           " xmlns:wsu=\"http://docs.oasis-open.org/wss/2004/01/"
           "oasis-200401-wss-wssecurity-utility-1.0.xsd\">"
           "<wsse:UsernameToken><wsse:Username>" +
           xml::escape(credentials.username) +
           "</wsse:Username><wsse:Password Type=\"http://docs.oasis-open.org/wss/2004/01/"
           "oasis-200401-wss-username-token-profile-1.0#PasswordDigest\">" +
           onvifPasswordDigest(nonce_raw, created, credentials.password) +
           "</wsse:Password><wsse:Nonce EncodingType=\"http://docs.oasis-open.org/wss/2004/01/"
           "oasis-200401-wss-soap-message-security-1.0#Base64Binary\">" +
           net::base64Encode(nonce_raw) + "</wsse:Nonce><wsu:Created>" + created +
           "</wsu:Created></wsse:UsernameToken></wsse:Security>";
}

/// One SOAP 1.2 call. Never HTTP-authenticated: ONVIF credentials travel in the WS-Security
/// header only, so a rejected login costs exactly one request.
net::HttpResult soapCall(net::Ipv4 host, std::uint16_t port, const std::string& path,
                         const std::string& action, const std::string& envelope, int timeout_ms) {
    net::HttpRequestOptions options;
    options.method = "POST";
    options.path = path;
    options.content_type = std::string("application/soap+xml; charset=utf-8; action=\"") +
                           kDeviceNamespace + "/" + action + "\"";
    options.body = envelope;
    options.timeout_ms = timeout_ms;
    options.max_body_bytes = kMaxSoapBytes;
    return net::httpRequest(host, port, options);
}

/// Collapses whitespace and bounds the length of camera-supplied text for a detail line.
std::string oneLine(const std::string& text) {
    std::string out;
    bool space = false;
    for (const char ch : text) {
        if (std::isspace(static_cast<unsigned char>(ch)) != 0 ||
            std::iscntrl(static_cast<unsigned char>(ch)) != 0) {
            space = !out.empty();
            continue;
        }
        if (space) {
            out.push_back(' ');
            space = false;
        }
        out.push_back(ch);
        if (out.size() >= kMaxFaultText) {
            out += "...";
            break;
        }
    }
    return out;
}

enum class SoapReply { kOk, kAuthFault, kFault, kNotSoap };

struct SoapAnswer {
    SoapReply reply{SoapReply::kNotSoap};
    /// The fault reason (or code) as one line.
    std::string fault;
};

bool containsNoCase(const std::string& text, const std::string& needle) {
    const auto lower = [](std::string value) {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return value;
    };
    return lower(text).find(lower(needle)) != std::string::npos;
}

/// SOAP 1.2 faults carry Code/Subcode and Reason; SOAP 1.1 faults faultcode and faultstring.
/// ONVIF signals a rejected UsernameToken with ter:NotAuthorized, WS-Security stacks with
/// wsse:FailedAuthentication or wsse:InvalidSecurity(Token); some firmware answers HTTP 401.
SoapAnswer classify(const net::HttpResponse& response) {
    SoapAnswer answer;
    const bool envelope = xml::rootName(response.body) == "Envelope";
    const auto fault = envelope ? xml::firstElement(response.body, "Fault") : std::nullopt;
    if (fault) {
        std::string code = xml::firstText(*fault, "Code").value_or("");
        if (code.empty()) {
            code = xml::firstText(*fault, "faultcode").value_or("");
        }
        std::string reason = xml::firstText(*fault, "Reason").value_or("");
        if (reason.empty()) {
            reason = xml::firstText(*fault, "faultstring").value_or("");
        }
        answer.fault = oneLine(reason.empty() ? code : reason);
        const std::string evidence = code + " " + reason;
        const bool rejected = containsNoCase(evidence, "NotAuthorized") ||
                              containsNoCase(evidence, "FailedAuthentication") ||
                              containsNoCase(evidence, "InvalidSecurity");
        answer.reply =
            rejected || response.status == 401 ? SoapReply::kAuthFault : SoapReply::kFault;
        return answer;
    }
    if (response.status == 401) {
        answer.reply = SoapReply::kAuthFault;
    } else if (envelope && response.status >= 200 && response.status < 300) {
        answer.reply = SoapReply::kOk;
    } else if (envelope) {
        answer.reply = SoapReply::kFault;
        answer.fault = "HTTP " + std::to_string(response.status);
    }
    return answer;
}

std::string describeFailure(const net::HttpResult& result, int timeout_ms) {
    switch (result.outcome) {
        case net::HttpOutcome::kConnectFailed:
            return result.error.empty() ? "connection " + net::toString(result.connect)
                                        : result.error;
        case net::HttpOutcome::kTimeout:
            return "no complete answer within " + std::to_string(timeout_ms) + " ms";
        case net::HttpOutcome::kProtocolError:
            return "not an HTTP answer" + (result.error.empty() ? "" : " (" + result.error + ")");
        case net::HttpOutcome::kResponse:
            break;
    }
    return "HTTP " + std::to_string(result.response.status);
}

std::int64_t daysFromCivil(std::int64_t year, std::int64_t month, std::int64_t day) {
    year -= month <= 2 ? 1 : 0;
    const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
    const std::int64_t year_of_era = year - era * 400;
    const std::int64_t day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const std::int64_t day_of_era =
        year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return era * 146097 + day_of_era - 719468;
}

/// "2026-10-09T08:15:30Z" for a Unix time, without gmtime and its time zone state.
std::string formatUtcSeconds(std::int64_t unix_seconds) {
    std::int64_t days = unix_seconds / 86400;
    std::int64_t seconds = unix_seconds % 86400;
    if (seconds < 0) {
        seconds += 86400;
        --days;
    }
    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const std::int64_t day_of_era = days - era * 146097;
    const std::int64_t year_of_era =
        (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
    const std::int64_t day_of_year =
        day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    const std::int64_t shifted_month = (5 * day_of_year + 2) / 153;
    const std::int64_t day = day_of_year - (153 * shifted_month + 2) / 5 + 1;
    const std::int64_t month = shifted_month < 10 ? shifted_month + 3 : shifted_month - 9;
    const std::int64_t year = year_of_era + era * 400 + (month <= 2 ? 1 : 0);
    char text[64];
    std::snprintf(text, sizeof(text), "%04lld-%02lld-%02lldT%02lld:%02lld:%02lldZ",
                  static_cast<long long>(year), static_cast<long long>(month),
                  static_cast<long long>(day), static_cast<long long>(seconds / 3600),
                  static_cast<long long>(seconds / 60 % 60), static_cast<long long>(seconds % 60));
    return text;
}

std::int64_t unixSecondsNow() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::optional<std::int64_t> parseSmallNumber(const std::string& text) {
    const std::string value = xml::trim(text);
    if (value.empty() || value.size() > 4) {
        return std::nullopt;
    }
    std::int64_t number = 0;
    for (const char ch : value) {
        if (std::isdigit(static_cast<unsigned char>(ch)) == 0) {
            return std::nullopt;
        }
        number = number * 10 + (ch - '0');
    }
    return number;
}

/// The camera's UTC clock from a GetSystemDateAndTime answer, as Unix seconds.
std::optional<std::int64_t> cameraUtcSeconds(const std::string& body) {
    const auto utc = xml::firstElement(body, "UTCDateTime");
    if (!utc) {
        return std::nullopt;
    }
    const auto date = xml::firstElement(*utc, "Date");
    const auto time = xml::firstElement(*utc, "Time");
    if (!date || !time) {
        return std::nullopt;
    }
    const auto field = [](const std::string& block, const char* name) {
        return parseSmallNumber(xml::firstText(block, name).value_or(""));
    };
    const auto year = field(*date, "Year");
    const auto month = field(*date, "Month");
    const auto day = field(*date, "Day");
    const auto hour = field(*time, "Hour");
    const auto minute = field(*time, "Minute");
    const auto second = field(*time, "Second");
    if (!year || !month || !day || !hour || !minute || !second || *year < 1970 || *month < 1 ||
        *month > 12 || *day < 1 || *day > 31 || *hour > 23 || *minute > 59 || *second > 60) {
        return std::nullopt;
    }
    return daysFromCivil(*year, *month, *day) * 86400 + *hour * 3600 + *minute * 60 + *second;
}

}  // namespace

WsDiscoveryResult wsDiscover(const std::string& interface, net::Ipv4 interface_address,
                             int listen_ms) {
    net::MulticastExchange exchange;
    exchange.interface_name = interface;
    exchange.interface_address = interface_address;
    exchange.group = net::parseIpv4(kWsDiscoveryGroup).value_or(net::Ipv4{});
    exchange.port = kWsDiscoveryPort;
    // ProbeMatches are unicast to the probe's source; an ephemeral port also keeps us clear of
    // the Hello/Bye multicast traffic on 3702.
    exchange.bind_port = 0;
    exchange.listen_ms = listen_ms;
    const net::MulticastResult exchanged =
        net::multicastExchange(exchange, buildWsDiscoveryProbe(net::randomUuid()));

    WsDiscoveryResult result;
    result.ran = exchanged.ran;
    result.error = exchanged.error;
    std::vector<std::string> keys;
    for (const net::Datagram& datagram : exchanged.datagrams) {
        for (WsDiscoveryMatch& match : parseProbeMatches(datagram.payload, datagram.from)) {
            const std::string key = matchKey(match);
            const auto known = std::find(keys.begin(), keys.end(), key);
            if (known != keys.end()) {
                mergeMatch(result.matches[static_cast<std::size_t>(known - keys.begin())], match);
                continue;
            }
            keys.push_back(key);
            result.matches.push_back(std::move(match));
        }
    }
    return result;
}

std::string onvifPasswordDigest(const std::string& nonce_raw, const std::string& created,
                                const std::string& password) {
    return net::base64Encode(net::sha1Raw(nonce_raw + created + password));
}

OnvifDeviceInfo onvifProbeDevice(net::Ipv4 host, std::uint16_t port, const std::string& path,
                                 const net::Credentials* onvif_credentials, int timeout_ms) {
    OnvifDeviceInfo info;
    const std::string service_path = path.empty() ? std::string(kDeviceServicePath) : path;
    const std::string where = net::toString(host) + ":" + std::to_string(port) + service_path;

    const net::HttpResult clock =
        soapCall(host, port, service_path, "GetSystemDateAndTime",
                 soapEnvelope({}, "<tds:GetSystemDateAndTime/>"), timeout_ms);
    const std::int64_t local_seconds = unixSecondsNow();
    if (clock.outcome != net::HttpOutcome::kResponse) {
        info.state = OnvifState::kUnavailable;
        info.detail = "no ONVIF answer from " + where + ": " + describeFailure(clock, timeout_ms);
        return info;
    }
    if (clock.response.status == 404) {
        info.state = OnvifState::kUnavailable;
        info.detail = "HTTP 404 for the ONVIF device service at " + where +
                      " (ONVIF disabled or not supported)";
        return info;
    }
    const SoapAnswer answer = classify(clock.response);
    std::int64_t skew = 0;
    switch (answer.reply) {
        case SoapReply::kNotSoap:
            info.state = OnvifState::kUnavailable;
            info.detail = "HTTP " + std::to_string(clock.response.status) +
                          " without a SOAP answer at " + where +
                          " (ONVIF disabled or not supported)";
            return info;
        case SoapReply::kOk:
            info.state = OnvifState::kAvailable;
            if (const auto camera = cameraUtcSeconds(clock.response.body)) {
                skew = *camera - local_seconds;
            }
            break;
        case SoapReply::kAuthFault:
            info.state = OnvifState::kAuthRequired;
            info.detail = "ONVIF at " + where + " requires a login even for GetSystemDateAndTime";
            break;
        case SoapReply::kFault:
            info.state = OnvifState::kError;
            info.detail = "GetSystemDateAndTime failed at " + where + ": " + answer.fault;
            break;
    }

    if (onvif_credentials == nullptr || onvif_credentials->empty()) {
        if (info.state == OnvifState::kAvailable) {
            info.detail = "ONVIF device service answers at " + where +
                          "; identification needs an ONVIF user (ONVIF_USERNAME / "
                          "ONVIF_PASSWORD)";
        } else if (info.state == OnvifState::kAuthRequired) {
            info.detail += "; set ONVIF_USERNAME / ONVIF_PASSWORD";
        }
        return info;
    }

    if (skew > kMaxClockSkewSeconds || skew < -kMaxClockSkewSeconds) {
        logEvent(LogLevel::kDebug, "onvif_clock_skew",
                 LogFields().add("ip", net::toString(host)).add("skew_s", skew));
    } else {
        skew = 0;
    }
    const std::string nonce = net::randomBytes(16);
    const std::string created = formatUtcSeconds(unixSecondsNow() + skew);
    const net::HttpResult device =
        soapCall(host, port, service_path, "GetDeviceInformation",
                 soapEnvelope(usernameToken(*onvif_credentials, nonce, created),
                              "<tds:GetDeviceInformation/>"),
                 timeout_ms);
    const std::string clock_note =
        skew != 0 ? " (camera clock " + std::to_string(skew) + " s off, compensated)" : "";
    if (device.outcome != net::HttpOutcome::kResponse) {
        info.state = OnvifState::kError;
        info.detail = "GetDeviceInformation at " + where + ": " +
                      describeFailure(device, timeout_ms);
        return info;
    }
    const SoapAnswer identified = classify(device.response);
    switch (identified.reply) {
        case SoapReply::kAuthFault:
            info.state = OnvifState::kAuthRequired;
            info.detail = "the camera rejected the ONVIF credentials (HTTP " +
                          std::to_string(device.response.status) +
                          ", one attempt, not retried)" + clock_note;
            return info;
        case SoapReply::kFault:
            info.state = OnvifState::kError;
            info.detail = "GetDeviceInformation failed at " + where + ": " + identified.fault;
            return info;
        case SoapReply::kNotSoap:
            info.state = OnvifState::kError;
            info.detail = "GetDeviceInformation answered HTTP " +
                          std::to_string(device.response.status) + " without SOAP at " + where;
            return info;
        case SoapReply::kOk:
            break;
    }
    const auto response = xml::firstElement(device.response.body, "GetDeviceInformationResponse");
    if (!response) {
        info.state = OnvifState::kError;
        info.detail = "GetDeviceInformation at " + where + " answered without device information";
        return info;
    }
    const auto text = [&response](const char* name) {
        return xml::trim(xml::firstText(*response, name).value_or(""));
    };
    info.manufacturer = text("Manufacturer");
    info.model = text("Model");
    info.firmware = text("FirmwareVersion");
    info.serial = text("SerialNumber");
    info.hardware_id = text("HardwareId");
    info.state = OnvifState::kAvailable;
    info.detail = "ONVIF identification: " + oneLine(info.manufacturer + " " + info.model) +
                  (info.firmware.empty() ? std::string() : ", firmware " + oneLine(info.firmware));
    return info;
}

}  // namespace anpr::hikvision
