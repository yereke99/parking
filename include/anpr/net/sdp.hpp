#pragma once

#include <map>
#include <string>

namespace anpr::net {

enum class VideoCodec { kUnknown, kH264, kH265, kMjpeg, kMpeg4, kOther };

/// "H.264", "H.265", "MJPEG", "MPEG-4", "other", "unknown".
std::string toString(VideoCodec codec);
/// "H264", "H265", ...: compact form for status tables.
std::string shortName(VideoCodec codec);

/// The first video media section of an SDP description, as needed to pick a decoder before
/// opening the stream.
struct SdpVideo {
    bool present{false};
    VideoCodec codec{VideoCodec::kUnknown};
    /// Encoding name exactly as in a=rtpmap ("H264", "H265", "HEVC", ...).
    std::string encoding_name;
    int payload_type{-1};
    int clock_rate{0};
    /// a=control of the video media (relative or absolute URL), empty when absent.
    std::string control;
    /// a=fmtp parameters for the video payload type, keys lower-cased.
    std::map<std::string, std::string> fmtp;
    /// a=framerate when present, otherwise 0.
    double framerate{0.0};
    /// From the SPS in sprop-parameter-sets / sprop-sps when present and parseable, else 0.
    int width{0};
    int height{0};
};

/// Parses `sdp` (CRLF or LF line endings). `present` is false when there is no m=video section.
SdpVideo parseSdpVideo(const std::string& sdp);

struct SpsInfo {
    bool ok{false};
    int width{0};
    int height{0};
};

/// Width and height from an H.264 sequence parameter set NAL unit (no start code, emulation
/// prevention bytes still present), honouring frame cropping.
SpsInfo parseH264Sps(const std::string& nal);
/// The same for an H.265 SPS NAL unit (two-byte NAL header included).
SpsInfo parseH265Sps(const std::string& nal);

}  // namespace anpr::net
