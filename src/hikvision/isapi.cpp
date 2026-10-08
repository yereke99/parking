// ISAPI document parsing (deviceInfo, streaming channel, capabilities, ANPR alerts) and the
// incremental multipart parser for the alert stream. The authenticated HTTP requests (isapiProbe)
// and the event JSON (toJson) are in the "Network" section at the end.
#include "anpr/hikvision/isapi.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <locale>
#include <sstream>
#include <utility>

#include "anpr/common/json.hpp"
#include "anpr/hikvision/xml_lite.hpp"
#include "anpr/net/http_client.hpp"

namespace anpr::hikvision {
namespace {

std::string lowerCase(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return text;
}

std::string upperCase(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    return text;
}

/// Trimmed text of the first element `name`, empty when absent.
std::string textOf(const std::string& xml, const char* name) {
    return xml::trim(xml::firstText(xml, name).value_or(""));
}

/// Non-negative decimal integer of at most nine digits.
std::optional<int> parseCount(const std::string& text) {
    if (text.empty() || text.size() > 9) {
        return std::nullopt;
    }
    int value = 0;
    for (const char ch : text) {
        if (std::isdigit(static_cast<unsigned char>(ch)) == 0) {
            return std::nullopt;
        }
        value = value * 10 + (ch - '0');
    }
    return value;
}

/// "[-+]digits[.digits]", parsed without the C locale's say on the decimal separator.
std::optional<double> parseNumber(const std::string& text) {
    std::size_t i = 0;
    bool negative = false;
    if (i < text.size() && (text[i] == '-' || text[i] == '+')) {
        negative = text[i] == '-';
        ++i;
    }
    double value = 0.0;
    std::size_t digits = 0;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])) != 0) {
        value = value * 10.0 + (text[i] - '0');
        ++digits;
        ++i;
    }
    if (i < text.size() && text[i] == '.') {
        ++i;
        double scale = 0.1;
        while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])) != 0) {
            value += (text[i] - '0') * scale;
            scale /= 10.0;
            ++digits;
            ++i;
        }
    }
    if (digits == 0 || digits > 18 || i != text.size()) {
        return std::nullopt;
    }
    return negative ? -value : value;
}

bool containsNoCase(const std::string& text, const std::string& needle) {
    return lowerCase(text).find(lowerCase(needle)) != std::string::npos;
}

/// Element names that announce on-board plate recognition, lower case, with a leading
/// "isSupport" removed (isSupportANPR, isSupportVehicleDetection, <ANPR>, <vehicleDetect>, ...).
bool isAnprCapabilityName(const std::string& lower_name) {
    static const std::array<const char*, 10> kNames = {
        "anpr",         "lpr",          "mnpr",
        "itc",          "licenseplate", "licenseplaterecognition",
        "vehicledetect", "vehicledetection", "platerecognition",
        "vehicleplaterecognition",
    };
    std::string name = lower_name;
    const std::string prefix = "issupport";
    if (name.compare(0, prefix.size(), prefix) == 0) {
        name = name.substr(prefix.size());
    }
    return std::any_of(kNames.begin(), kNames.end(),
                       [&name](const char* known) { return name == known; });
}

/// A value listing event types or functions ("VMD,ANPR,vehicleDetection").
bool listsAnpr(const std::string& lower_value) {
    std::size_t i = 0;
    while (i < lower_value.size()) {
        const std::size_t begin = i;
        while (i < lower_value.size() &&
               (std::isalnum(static_cast<unsigned char>(lower_value[i])) != 0 ||
                lower_value[i] == '_')) {
            ++i;
        }
        const std::string token = lower_value.substr(begin, i - begin);
        if (token == "anpr" || token == "vehicledetection" || token == "vehicledetect" ||
            token == "lpr") {
            return true;
        }
        if (i == begin) {
            ++i;
        }
    }
    return false;
}

enum class Evidence { kNone, kSupported, kLacking };

Evidence inspectCapabilities(const std::string& document) {
    const std::string root = xml::rootName(document);
    if (root.empty() || lowerCase(root) == "html") {
        // Not XML, or a web server error page: says nothing about the camera.
        return Evidence::kNone;
    }
    if (root == "ResponseStatus") {
        // The answer of an ISAPI URL the device does not implement, for example
        // /ISAPI/Traffic/capabilities on a camera without traffic functions.
        const std::string sub_status = lowerCase(textOf(document, "subStatusCode"));
        const std::string status = textOf(document, "statusString");
        if (sub_status == "notsupport" || containsNoCase(status, "not support")) {
            return Evidence::kLacking;
        }
        return Evidence::kNone;
    }
    // Only traffic (ITC) devices such as the DS-TCG406-E serve a traffic capability document;
    // a 200 with one is itself the most reliable "this is an LPR camera" signal.
    if (containsNoCase(root, "traffic") || root.find("ITC") != std::string::npos) {
        return Evidence::kSupported;
    }
    for (const xml::ElementInfo& element : xml::elements(document)) {
        const std::string value = lowerCase(xml::trim(element.text));
        if (isAnprCapabilityName(lowerCase(element.name))) {
            if (element.has_children || value == "true" || value == "1" || value == "yes") {
                return Evidence::kSupported;
            }
            continue;
        }
        if (!element.has_children && listsAnpr(value)) {
            return Evidence::kSupported;
        }
    }
    return Evidence::kLacking;
}

bool isNoPlate(const std::string& plate) {
    const std::string value = lowerCase(plate);
    // "无车牌" ("no plate") is what Chinese-locale firmware writes for a vehicle without a plate.
    return value.empty() || value == "unknown" || value == "noplate" || value == "no plate" ||
           value == "\xE6\x97\xA0\xE8\xBD\xA6\xE7\x89\x8C";
}

constexpr std::size_t kMaxContentLength = 64U * 1024U * 1024U;

std::optional<std::size_t> parseContentLength(const std::string& text) {
    const std::string value = xml::trim(text);
    if (value.empty() || value.size() > 12) {
        return std::nullopt;
    }
    std::size_t length = 0;
    for (const char ch : value) {
        if (std::isdigit(static_cast<unsigned char>(ch)) == 0) {
            return std::nullopt;
        }
        length = length * 10 + static_cast<std::size_t>(ch - '0');
    }
    // An absurd length would make the parser wait forever; the delimiter search copes instead.
    if (length > kMaxContentLength) {
        return std::nullopt;
    }
    return length;
}

}  // namespace

IsapiDeviceInfo parseDeviceInfo(const std::string& xml) {
    IsapiDeviceInfo info;
    info.device_name = textOf(xml, "deviceName");
    info.device_id = textOf(xml, "deviceID");
    info.model = textOf(xml, "model");
    info.serial = textOf(xml, "serialNumber");
    info.mac = net::normalizeMac(textOf(xml, "macAddress"));
    info.firmware = textOf(xml, "firmwareVersion");
    info.firmware_date = textOf(xml, "firmwareReleasedDate");
    info.device_type = textOf(xml, "deviceType");
    info.hardware_version = textOf(xml, "hardwareVersion");
    info.ok = !info.model.empty() || !info.serial.empty() || !info.mac.empty();
    return info;
}

IsapiStreamInfo parseStreamingChannel(const std::string& xml) {
    IsapiStreamInfo info;
    const auto video = xml::firstElement(xml, "Video");
    if (!video) {
        return info;
    }
    info.codec = textOf(*video, "videoCodecType");
    info.width = parseCount(textOf(*video, "videoResolutionWidth")).value_or(0);
    info.height = parseCount(textOf(*video, "videoResolutionHeight")).value_or(0);
    // ISAPI frame rates are in hundredths of a frame per second: 2500 is 25 fps.
    const auto frame_rate = parseNumber(textOf(*video, "maxFrameRate"));
    info.max_fps = frame_rate && *frame_rate > 0.0 ? *frame_rate / 100.0 : 0.0;
    info.bitrate_control = textOf(*video, "videoQualityControlType");
    info.ok = !info.codec.empty() || (info.width > 0 && info.height > 0);
    return info;
}

std::string toString(NativeAnprSupport support) {
    switch (support) {
        case NativeAnprSupport::kUnknown:
            return "unknown";
        case NativeAnprSupport::kSupported:
            return "supported";
        case NativeAnprSupport::kNotSupported:
            return "not_supported";
    }
    return "unknown";
}

NativeAnprSupport parseNativeAnprSupport(const std::vector<std::string>& capability_documents) {
    bool lacking = false;
    for (const std::string& document : capability_documents) {
        switch (inspectCapabilities(document)) {
            case Evidence::kSupported:
                return NativeAnprSupport::kSupported;
            case Evidence::kLacking:
                lacking = true;
                break;
            case Evidence::kNone:
                break;
        }
    }
    // "Not supported" needs a document that clearly lacks ANPR; errors and garbage prove nothing.
    return lacking ? NativeAnprSupport::kNotSupported : NativeAnprSupport::kUnknown;
}

std::string toString(IsapiStatus status) {
    switch (status) {
        case IsapiStatus::kOk:
            return "ok";
        case IsapiStatus::kAuthFailed:
            return "auth_failed";
        case IsapiStatus::kUnavailable:
            return "unavailable";
        case IsapiStatus::kError:
            return "error";
        case IsapiStatus::kSkipped:
            return "skipped";
    }
    return "error";
}

std::optional<NativePlateEvent> parseAnprAlert(const std::string& xml) {
    // A cut-off document is a framing error (wrong Content-Length): its fields cannot be trusted.
    if (xml::rootName(xml) != "EventNotificationAlert" ||
        !xml::firstElement(xml, "EventNotificationAlert")) {
        return std::nullopt;
    }
    const std::string event_type = lowerCase(textOf(xml, "eventType"));
    const auto anpr = xml::firstElement(xml, "ANPR");
    const bool plate_event = event_type == "anpr" || event_type == "vehicledetection" ||
                             event_type == "vehicledetect";
    if (!anpr && !plate_event) {
        return std::nullopt;
    }
    const std::string& scope = anpr ? *anpr : xml;

    NativePlateEvent event;
    event.plate = textOf(scope, "licensePlate");
    if (isNoPlate(event.plate)) {
        event.plate = textOf(scope, "originalLicensePlate");
    }
    if (isNoPlate(event.plate)) {
        // The traffic polling API and some firmware name the field plateNumber.
        event.plate = textOf(scope, "plateNumber");
    }
    if (isNoPlate(event.plate)) {
        return std::nullopt;
    }
    event.country = textOf(scope, "country");
    event.direction = textOf(scope, "direction");
    if (const auto confidence = parseNumber(textOf(scope, "confidenceLevel"))) {
        event.confidence = *confidence;
    }
    event.plate_color = textOf(scope, "plateColor");
    event.vehicle_type = textOf(scope, "vehicleType");
    event.lane = textOf(scope, "line");
    if (event.lane.empty()) {
        event.lane = textOf(scope, "laneNo");
    }
    if (event.lane.empty()) {
        event.lane = textOf(xml, "laneNo");
    }
    event.camera_time = textOf(xml, "dateTime");
    event.channel = textOf(xml, "channelID");
    if (event.channel.empty()) {
        event.channel = textOf(xml, "dynChannelID");
    }
    return event;
}

std::string anprCountryIso(const std::string& hikvision_country) {
    const std::string value = xml::trim(hikvision_country);
    if (value.size() == 2 && std::isalpha(static_cast<unsigned char>(value[0])) != 0 &&
        std::isalpha(static_cast<unsigned char>(value[1])) != 0) {
        return upperCase(value);
    }
    const auto code = parseCount(value);
    if (!code) {
        return {};
    }
    // Hikvision's ANPR country table (the region list the DS-TCG406-E offers for Kazakhstan).
    static const std::array<std::pair<int, const char*>, 15> kCountries = {{
        {9, "BY"}, {10, "MD"}, {11, "RU"}, {12, "UA"}, {19, "HR"},
        {28, "AZ"}, {29, "GE"}, {30, "KZ"}, {32, "TM"}, {33, "UZ"},
        {51, "AM"}, {59, "CN"}, {73, "KG"}, {76, "MN"}, {83, "TJ"},
    }};
    for (const auto& country : kCountries) {
        if (country.first == *code) {
            return country.second;
        }
    }
    return {};
}

MultipartParser::MultipartParser(std::string boundary) : boundary_(std::move(boundary)) {}

void MultipartParser::feed(const std::string& bytes) {
    buffer_.append(bytes);
    // Drop bytes before the first delimiter right away, so garbage never accumulates even when
    // the caller looks at buffered() before calling next().
    if (state_ == State::kPreamble && !boundary_.empty()) {
        while (state_ == State::kPreamble && seekDelimiter()) {
        }
        compact();
    }
}

bool MultipartParser::next(Part& part) {
    if (boundary_.empty()) {
        return false;
    }
    bool produced = false;
    while (!produced) {
        bool progressed = false;
        switch (state_) {
            case State::kPreamble:
                progressed = seekDelimiter();
                break;
            case State::kHeaders:
                progressed = readHeaders();
                break;
            case State::kBody:
                progressed = readBody(part);
                produced = progressed;
                break;
        }
        if (!progressed) {
            break;
        }
    }
    compact();
    return produced;
}

bool MultipartParser::lineStartAt(std::size_t index) const {
    if (index == pos_) {
        return line_start_;
    }
    return index > pos_ && buffer_[index - 1] == '\n';
}

bool MultipartParser::delimiterLineAt(std::size_t index) const {
    const auto has = [this, index](const std::string& text) {
        return buffer_.size() - index >= text.size() &&
               buffer_.compare(index, text.size(), text) == 0;
    };
    return has("--" + boundary_) || (boundary_.compare(0, 2, "--") == 0 && has(boundary_));
}

/// The boundary text is searched for once; a match is a delimiter when "--" precedes it at the
/// start of a line (RFC 2046) or, for a boundary that already begins with "--", when the match
/// itself starts a line: some cameras declare the dashes as part of the boundary.
MultipartParser::Scan MultipartParser::findDelimiter(std::size_t from, std::size_t min_begin,
                                                     Delimiter& found) const {
    const bool dashed = boundary_.compare(0, 2, "--") == 0;
    for (std::size_t match = buffer_.find(boundary_, from); match != std::string::npos;
         match = buffer_.find(boundary_, match + 1)) {
        std::size_t begin = std::string::npos;
        if (match >= min_begin + 2 && buffer_[match - 2] == '-' && buffer_[match - 1] == '-' &&
            lineStartAt(match - 2)) {
            begin = match - 2;
        } else if (dashed && match >= min_begin && lineStartAt(match)) {
            begin = match;
        } else {
            continue;
        }
        found.begin = begin;
        found.resume = match;
        std::size_t i = match + boundary_.size();
        if (i < buffer_.size() && buffer_[i] == '-') {
            if (i + 1 >= buffer_.size()) {
                return Scan::kNeedMore;
            }
            if (buffer_[i + 1] == '-') {
                found.end = i + 2;
                return Scan::kClose;
            }
            continue;
        }
        // Transport padding, then the end of the line.
        while (i < buffer_.size() && (buffer_[i] == ' ' || buffer_[i] == '\t')) {
            ++i;
        }
        if (i < buffer_.size() && buffer_[i] == '\r') {
            ++i;
        }
        if (i >= buffer_.size()) {
            return Scan::kNeedMore;
        }
        if (buffer_[i] == '\n') {
            found.end = i + 1;
            return Scan::kDelimiter;
        }
    }
    // A delimiter can still begin within the last boundary-length bytes.
    const std::size_t tail =
        boundary_.size() > buffer_.size() ? 0 : buffer_.size() - boundary_.size() + 1;
    found.resume = std::max(from, tail);
    return Scan::kNone;
}

bool MultipartParser::seekDelimiter() {
    Delimiter delimiter;
    switch (findDelimiter(std::max(scan_, pos_), pos_, delimiter)) {
        case Scan::kNone: {
            // Keep only a tail that may hold the start of a delimiter ("--" + boundary).
            const std::size_t keep = boundary_.size() + 2;
            if (buffer_.size() - pos_ > keep) {
                pos_ = buffer_.size() - keep;
                line_start_ = buffer_[pos_ - 1] == '\n';
            }
            scan_ = pos_;
            return false;
        }
        case Scan::kNeedMore:
            line_start_ = lineStartAt(delimiter.begin);
            pos_ = delimiter.begin;
            scan_ = delimiter.resume;
            return false;
        case Scan::kClose:
            // "--boundary--" ends the body; anything after it is epilogue until a new delimiter.
            pos_ = delimiter.end;
            line_start_ = false;
            scan_ = pos_;
            return true;
        case Scan::kDelimiter:
            line_start_ = true;
            pos_ = delimiter.begin;
            headers_begin_ = delimiter.end;
            line_scan_ = delimiter.end;
            headers_.clear();
            body_length_.reset();
            state_ = State::kHeaders;
            return true;
    }
    return false;
}

bool MultipartParser::readHeaders() {
    while (true) {
        const std::size_t newline = buffer_.find('\n', line_scan_);
        if (newline == std::string::npos) {
            return false;
        }
        std::size_t line_end = newline;
        if (line_end > line_scan_ && buffer_[line_end - 1] == '\r') {
            --line_end;
        }
        if (line_end == line_scan_) {
            break;
        }
        if (delimiterLineAt(line_scan_)) {
            // A delimiter where a header was expected: the part had no blank line. Drop it and
            // start over at this delimiter.
            pos_ = line_scan_;
            line_start_ = true;
            scan_ = pos_;
            state_ = State::kPreamble;
            return true;
        }
        line_scan_ = newline + 1;
    }

    // The block is complete: parse it in one pass. Continuation lines (leading blank) extend the
    // previous header.
    const std::size_t blank_line = line_scan_;
    std::string last_name;
    std::size_t line_begin = headers_begin_;
    while (line_begin < blank_line) {
        const std::size_t newline = buffer_.find('\n', line_begin);
        std::string line = buffer_.substr(line_begin, newline - line_begin);
        line_begin = newline + 1;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty() && (line[0] == ' ' || line[0] == '\t')) {
            if (!last_name.empty()) {
                std::string& value = headers_[last_name];
                value += (value.empty() ? "" : " ") + xml::trim(line);
            }
            continue;
        }
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) {
            last_name.clear();
            continue;
        }
        const std::string name = lowerCase(xml::trim(line.substr(0, colon)));
        if (name.empty()) {
            last_name.clear();
            continue;
        }
        headers_.emplace(name, xml::trim(line.substr(colon + 1)));
        last_name = name;
    }
    const auto length = headers_.find("content-length");
    body_length_ = length == headers_.end() ? std::nullopt : parseContentLength(length->second);
    body_begin_ = buffer_.find('\n', blank_line) + 1;
    scan_ = body_begin_;
    state_ = State::kBody;
    return true;
}

bool MultipartParser::readBody(Part& part) {
    if (body_length_) {
        if (buffer_.size() - body_begin_ < *body_length_) {
            return false;
        }
        part.headers = std::move(headers_);
        part.body = buffer_.substr(body_begin_, *body_length_);
        pos_ = body_begin_ + *body_length_;
        // The CRLF before the next delimiter follows; a sender that omits it is tolerated.
        line_start_ = true;
    } else {
        // Without Content-Length the body ends at the line break before the next delimiter.
        Delimiter delimiter;
        switch (findDelimiter(scan_, body_begin_, delimiter)) {
            case Scan::kNone:
            case Scan::kNeedMore:
                scan_ = delimiter.resume;
                return false;
            case Scan::kClose:
            case Scan::kDelimiter:
                break;
        }
        std::size_t end = delimiter.begin;
        if (end > body_begin_ && buffer_[end - 1] == '\n') {
            --end;
            if (end > body_begin_ && buffer_[end - 1] == '\r') {
                --end;
            }
        }
        part.headers = std::move(headers_);
        part.body = buffer_.substr(body_begin_, end - body_begin_);
        pos_ = delimiter.begin;
        line_start_ = true;
    }
    headers_.clear();
    body_length_.reset();
    state_ = State::kPreamble;
    scan_ = pos_;
    return true;
}

void MultipartParser::compact() {
    // Erasing only once the consumed prefix outweighs the rest keeps the cost linear overall.
    if (pos_ == 0 || pos_ < buffer_.size() - pos_) {
        return;
    }
    buffer_.erase(0, pos_);
    const auto shift = [this](std::size_t& index) { index = index >= pos_ ? index - pos_ : 0; };
    shift(scan_);
    shift(headers_begin_);
    shift(line_scan_);
    shift(body_begin_);
    pos_ = 0;
}

std::string boundaryFromContentType(const std::string& content_type) {
    std::size_t i = content_type.find(';');
    while (i != std::string::npos && i < content_type.size()) {
        ++i;  // past ';'
        while (i < content_type.size() && (content_type[i] == ' ' || content_type[i] == '\t')) {
            ++i;
        }
        const std::size_t name_begin = i;
        while (i < content_type.size() && content_type[i] != '=' && content_type[i] != ';') {
            ++i;
        }
        const std::string name =
            lowerCase(xml::trim(content_type.substr(name_begin, i - name_begin)));
        if (i >= content_type.size() || content_type[i] == ';') {
            continue;
        }
        ++i;  // past '='
        while (i < content_type.size() && (content_type[i] == ' ' || content_type[i] == '\t')) {
            ++i;
        }
        std::string value;
        if (i < content_type.size() && content_type[i] == '"') {
            ++i;
            while (i < content_type.size() && content_type[i] != '"') {
                if (content_type[i] == '\\' && i + 1 < content_type.size()) {
                    ++i;
                }
                value.push_back(content_type[i]);
                ++i;
            }
            if (i < content_type.size()) {
                ++i;  // past the closing quote
            }
            i = content_type.find(';', i);
        } else {
            const std::size_t value_begin = i;
            i = content_type.find(';', i);
            value = xml::trim(content_type.substr(
                value_begin, i == std::string::npos ? std::string::npos : i - value_begin));
        }
        if (name == "boundary") {
            return value;
        }
    }
    return {};
}

// ---------------------------------------------------------------------------------------------
// Network
// ---------------------------------------------------------------------------------------------

namespace {

constexpr std::size_t kMaxIsapiBody = 1U << 20U;

/// English names for the codes `anprCountryIso` produces.
const char* countryName(const std::string& iso) {
    static const std::array<std::pair<const char*, const char*>, 15> kNames = {{
        {"AM", "Armenia"},    {"AZ", "Azerbaijan"}, {"BY", "Belarus"},      {"CN", "China"},
        {"GE", "Georgia"},    {"HR", "Croatia"},    {"KG", "Kyrgyzstan"},   {"KZ", "Kazakhstan"},
        {"MD", "Moldova"},    {"MN", "Mongolia"},   {"RU", "Russia"},       {"TJ", "Tajikistan"},
        {"TM", "Turkmenistan"}, {"UA", "Ukraine"},  {"UZ", "Uzbekistan"},
    }};
    for (const auto& name : kNames) {
        if (iso == name.first) {
            return name.second;
        }
    }
    return nullptr;
}

std::string jsonString(const std::string& value) {
    return "\"" + json::escape(value) + "\"";
}

/// A camera-supplied field: null when the camera did not send it.
std::string jsonStringOrNull(const std::string& value) {
    return value.empty() ? std::string("null") : jsonString(value);
}

/// ISO 8601 UTC with milliseconds ("2026-10-09T08:15:30.120Z").
std::string formatUtcMs(std::int64_t unix_time_ms) {
    std::int64_t seconds = unix_time_ms / 1000;
    std::int64_t millis = unix_time_ms % 1000;
    if (millis < 0) {
        millis += 1000;
        --seconds;
    }
    const auto time = static_cast<std::time_t>(seconds);
    std::tm utc{};
    if (gmtime_r(&time, &utc) == nullptr) {
        return {};
    }
    char text[64];
    std::snprintf(text, sizeof(text), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", utc.tm_year + 1900,
                  utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec,
                  static_cast<int>(millis));
    return text;
}

/// Shortest round-trip-safe text for a confidence value, independent of the process locale
/// (GStreamer or OpenCV may have changed LC_NUMERIC).
std::string formatNumber(double value) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out.precision(6);
    out << value;
    return out.str();
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

std::string statusText(const net::HttpResponse& response) {
    std::string text = "HTTP " + std::to_string(response.status);
    if (!response.reason.empty()) {
        text += " " + response.reason;
    }
    return text;
}

/// A 401 that answered credentials: they were rejected, and must not be sent again.
bool credentialsRejected(const net::HttpResult& result) {
    return result.outcome == net::HttpOutcome::kResponse && result.response.status == 401 &&
           result.auth_attempted;
}

std::string join(const std::vector<std::string>& parts, const char* separator) {
    std::string out;
    for (const std::string& part : parts) {
        if (part.empty()) {
            continue;
        }
        if (!out.empty()) {
            out += separator;
        }
        out += part;
    }
    return out;
}

std::string describeDevice(const IsapiDeviceInfo& device) {
    std::vector<std::string> parts;
    if (!device.model.empty()) {
        parts.push_back("model " + device.model);
    }
    if (!device.serial.empty()) {
        parts.push_back("serial " + device.serial);
    }
    if (!device.firmware.empty()) {
        parts.push_back("firmware " + device.firmware +
                        (device.firmware_date.empty() ? "" : " " + device.firmware_date));
    }
    return join(parts, ", ");
}

std::string describeStream(int channel, const IsapiStreamInfo& stream) {
    std::string text = "channel " + std::to_string(channel) + ":";
    if (!stream.codec.empty()) {
        text += " " + stream.codec;
    }
    if (stream.width > 0 && stream.height > 0) {
        text += " " + std::to_string(stream.width) + "x" + std::to_string(stream.height);
    }
    if (stream.max_fps > 0.0) {
        text += " " + formatNumber(stream.max_fps) + " fps";
    }
    return text;
}

}  // namespace

IsapiProbe isapiProbe(net::Ipv4 host, std::uint16_t http_port, const net::Credentials& credentials,
                      int streaming_channel, int timeout_ms) {
    IsapiProbe probe;
    if (credentials.empty()) {
        probe.status = IsapiStatus::kSkipped;
        probe.detail = "no credentials configured; ISAPI not queried";
        return probe;
    }
    const std::string where = net::toString(host) + ":" + std::to_string(http_port);
    // Every request answers a digest challenge with the credentials at most once.
    const auto get = [&](const std::string& path) {
        net::HttpRequestOptions options;
        options.path = path;
        options.credentials = &credentials;
        options.timeout_ms = timeout_ms;
        options.max_body_bytes = kMaxIsapiBody;
        return net::httpRequest(host, http_port, options);
    };

    const net::HttpResult info = get("/ISAPI/System/deviceInfo");
    if (info.outcome != net::HttpOutcome::kResponse) {
        probe.status = IsapiStatus::kUnavailable;
        probe.detail = "no ISAPI answer from " + where + ": " + describeFailure(info, timeout_ms);
        return probe;
    }
    probe.server_header = info.response.header("Server").value_or("");
    const int status = info.response.status;
    if (credentialsRejected(info)) {
        probe.status = IsapiStatus::kAuthFailed;
        probe.detail = "the camera rejected the credentials for ISAPI at " + where +
                       " (HTTP 401 after one attempt; not retried to avoid the login lockout)";
        return probe;
    }
    if (status == 401) {
        probe.status = IsapiStatus::kError;
        probe.detail = "ISAPI at " + where +
                       " asks for a login this client cannot answer (no supported challenge); "
                       "credentials were not sent";
        return probe;
    }
    if (status == 404) {
        probe.status = IsapiStatus::kUnavailable;
        probe.detail = "no ISAPI at " + where + " (HTTP 404 for /ISAPI/System/deviceInfo)";
        return probe;
    }
    if (status != 200) {
        probe.status = IsapiStatus::kError;
        probe.detail = "/ISAPI/System/deviceInfo at " + where + " answered " +
                       statusText(info.response);
        return probe;
    }
    probe.device = parseDeviceInfo(info.response.body);
    if (!probe.device.ok) {
        probe.status = IsapiStatus::kUnavailable;
        probe.detail = "the web server at " + where +
                       " answered /ISAPI/System/deviceInfo without a DeviceInfo document";
        return probe;
    }
    probe.status = IsapiStatus::kOk;

    std::vector<std::string> notes{describeDevice(probe.device)};
    const auto finish = [&probe, &notes]() {
        probe.detail = join(notes, "; ");
        return probe;
    };
    const auto stopAfterRejection = [&notes](const std::string& path) {
        notes.push_back(path + " rejected the credentials (HTTP 401); further ISAPI requests "
                               "skipped");
    };

    if (streaming_channel > 0) {
        const std::string path = "/ISAPI/Streaming/channels/" + std::to_string(streaming_channel);
        const net::HttpResult channel = get(path);
        if (credentialsRejected(channel)) {
            stopAfterRejection(path);
            return finish();
        }
        if (channel.outcome == net::HttpOutcome::kResponse && channel.response.status == 200) {
            probe.stream = parseStreamingChannel(channel.response.body);
        }
        if (probe.stream.ok) {
            notes.push_back(describeStream(streaming_channel, probe.stream));
        } else {
            notes.push_back("channel " + std::to_string(streaming_channel) + ": " +
                            (channel.outcome == net::HttpOutcome::kResponse &&
                                     channel.response.status == 200
                                 ? std::string("no video settings in the answer")
                                 : describeFailure(channel, timeout_ms)));
        }
    }

    // Only traffic (ITC) cameras implement /ISAPI/Traffic; the others answer 403 notSupport or
    // 404 there, and then the event capabilities tell whether an ANPR event exists.
    std::vector<std::string> documents;
    const net::HttpResult traffic = get("/ISAPI/Traffic/capabilities");
    if (credentialsRejected(traffic)) {
        stopAfterRejection("/ISAPI/Traffic/capabilities");
        return finish();
    }
    bool ask_events = false;
    if (traffic.outcome == net::HttpOutcome::kResponse) {
        documents.push_back(traffic.response.body);
        ask_events = traffic.response.status != 200 && traffic.response.status != 401;
    }
    if (ask_events) {
        const net::HttpResult events = get("/ISAPI/Event/capabilities");
        if (credentialsRejected(events)) {
            stopAfterRejection("/ISAPI/Event/capabilities");
            return finish();
        }
        if (events.outcome == net::HttpOutcome::kResponse) {
            documents.push_back(events.response.body);
        }
    }
    probe.native_anpr = parseNativeAnprSupport(documents);
    notes.push_back("native ANPR " + toString(probe.native_anpr));
    return finish();
}

std::string toJson(const NativePlateEvent& event) {
    const std::string iso = anprCountryIso(event.country);
    const char* name = countryName(iso);
    const bool has_confidence = event.confidence >= 0.0 && std::isfinite(event.confidence);
    std::string out = "{\"event\":\"hikvision_anpr\",\"source\":\"hikvision_isapi\"";
    out += ",\"camera_id\":" + jsonString(event.camera_id);
    out += ",\"plate\":" + jsonString(event.plate);
    out += ",\"country\":" + jsonStringOrNull(event.country);
    out += ",\"country_iso\":" + jsonStringOrNull(iso);
    out += ",\"country_name\":" + (name != nullptr ? jsonString(name) : std::string("null"));
    out += ",\"direction\":" + jsonStringOrNull(event.direction);
    out += ",\"confidence\":" + (has_confidence ? formatNumber(event.confidence) : "null");
    out += ",\"plate_color\":" + jsonStringOrNull(event.plate_color);
    out += ",\"vehicle_type\":" + jsonStringOrNull(event.vehicle_type);
    out += ",\"lane\":" + jsonStringOrNull(event.lane);
    out += ",\"camera_time\":" + jsonStringOrNull(event.camera_time);
    const std::string time = event.unix_time_ms > 0 ? formatUtcMs(event.unix_time_ms) : "";
    out += ",\"time\":" + jsonStringOrNull(time);
    out += ",\"channel\":" + jsonStringOrNull(event.channel);
    out += "}";
    return out;
}

}  // namespace anpr::hikvision
