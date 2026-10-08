#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "anpr/net/http_auth.hpp"
#include "anpr/net/socket.hpp"

namespace anpr::hikvision {

/// Hikvision ISAPI over HTTP with digest authentication. Used after RTSP has proven the
/// credentials, so a wrong password never costs more than the one RTSP attempt.

struct IsapiDeviceInfo {
    bool ok{false};
    std::string device_name;
    /// deviceID: Hikvision's own device identifier (stable across IP changes).
    std::string device_id;
    std::string model;
    std::string serial;
    std::string mac;  ///< normalized
    std::string firmware;
    std::string firmware_date;
    std::string device_type;
    std::string hardware_version;
};

IsapiDeviceInfo parseDeviceInfo(const std::string& xml);

/// One streaming channel's configured video (GET /ISAPI/Streaming/channels/<id>).
struct IsapiStreamInfo {
    bool ok{false};
    /// "H.264", "H.265", "MJPEG", ... as the camera names it.
    std::string codec;
    int width{0};
    int height{0};
    /// Frames per second (the ISAPI field is in hundredths: 2500 means 25).
    double max_fps{0.0};
    std::string bitrate_control;
};

IsapiStreamInfo parseStreamingChannel(const std::string& xml);

/// What the camera says about on-board licence plate recognition.
enum class NativeAnprSupport { kUnknown, kSupported, kNotSupported };
std::string toString(NativeAnprSupport support);

/// Looks for ANPR / vehicle detection support in capability documents
/// (/ISAPI/Traffic/capabilities, /ISAPI/Event/capabilities, /ISAPI/System/capabilities).
NativeAnprSupport parseNativeAnprSupport(const std::vector<std::string>& capability_documents);

enum class IsapiStatus { kOk, kAuthFailed, kUnavailable, kError, kSkipped };
std::string toString(IsapiStatus status);

struct IsapiProbe {
    IsapiStatus status{IsapiStatus::kSkipped};
    IsapiDeviceInfo device;
    IsapiStreamInfo stream;
    NativeAnprSupport native_anpr{NativeAnprSupport::kUnknown};
    std::string server_header;
    std::string detail;
};

/// deviceInfo, the streaming channel and the ANPR capability, each one authenticated request.
/// Stops at the first 401 so that a rejected password is tried once only.
IsapiProbe isapiProbe(net::Ipv4 host, std::uint16_t http_port, const net::Credentials& credentials,
                      int streaming_channel, int timeout_ms);

/// One licence plate read by the camera's own ANPR, from an ISAPI event.
struct NativePlateEvent {
    std::string camera_id;
    std::string plate;
    std::string country;
    std::string direction;
    /// 0..100 as reported by the camera, negative when absent.
    double confidence{-1.0};
    std::string plate_color;
    std::string vehicle_type;
    std::string lane;
    /// The camera's own timestamp text (dateTime), unchanged.
    std::string camera_time;
    std::int64_t unix_time_ms{0};
    std::string channel;
};

/// Parses an EventNotificationAlert carrying an ANPR result. nullopt for other event types
/// (heartbeats "videoloss" with inactive state, motion, ...) or when no plate is present.
/// `camera_id` and `unix_time_ms` are left for the receiver to fill: the camera clock on an
/// isolated LAN without NTP is not trusted.
std::optional<NativePlateEvent> parseAnprAlert(const std::string& xml);

/// ISO 3166 alpha-2 code for the numeric <country> of an ANPR event ("30" -> "KZ"), from the
/// camera's region table, for the countries of the Russian-speaking region and its neighbours.
/// A two-letter code is returned upper-cased; anything else gives an empty string.
std::string anprCountryIso(const std::string& hikvision_country);

/// `{"event":"hikvision_anpr",...}` for the events stream. A different event name than the
/// pipeline's own `plate_recognition`, so barrier logic never mistakes one for the other.
std::string toJson(const NativePlateEvent& event);

/// Incremental multipart/mixed parser for the alertStream body.
class MultipartParser {
public:
    struct Part {
        std::map<std::string, std::string> headers;  ///< lower-cased names
        std::string body;
    };

    explicit MultipartParser(std::string boundary);
    void feed(const std::string& bytes);
    /// Moves the next complete part into `part`. False when none is complete yet.
    bool next(Part& part);
    /// Bytes buffered without a complete part; the caller resets the stream when this grows
    /// beyond a limit (a camera sending pictures we do not want). Every byte the parser holds
    /// is counted here: garbage before a delimiter is dropped, a part is kept whole until done.
    [[nodiscard]] std::size_t buffered() const { return buffer_.size() - pos_; }

private:
    enum class State { kPreamble, kHeaders, kBody };
    enum class Scan { kNone, kNeedMore, kDelimiter, kClose };
    struct Delimiter {
        std::size_t begin{0};
        /// Past the delimiter line (kDelimiter) or past the closing "--" (kClose).
        std::size_t end{0};
        /// Where to search again after kNeedMore or kNone.
        std::size_t resume{0};
    };

    std::string boundary_;
    std::string buffer_;
    /// Start of the bytes not consumed yet; the bytes before it are erased by compact().
    std::size_t pos_{0};
    /// The byte before pos_ ended a line (or pos_ is the start of the stream).
    bool line_start_{true};
    State state_{State::kPreamble};
    std::size_t scan_{0};
    std::size_t headers_begin_{0};
    std::size_t line_scan_{0};
    std::size_t body_begin_{0};
    std::optional<std::size_t> body_length_;
    std::map<std::string, std::string> headers_;

    [[nodiscard]] bool lineStartAt(std::size_t index) const;
    [[nodiscard]] bool delimiterLineAt(std::size_t index) const;
    Scan findDelimiter(std::size_t from, std::size_t min_begin, Delimiter& found) const;
    bool seekDelimiter();
    bool readHeaders();
    bool readBody(Part& part);
    void compact();
};

/// The boundary parameter of a multipart Content-Type, without quotes. Empty when absent.
std::string boundaryFromContentType(const std::string& content_type);

}  // namespace anpr::hikvision
