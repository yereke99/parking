// SDP video section parsing and H.264 / H.265 SPS decoding, enough to pick a decoder and to know
// the coded picture size before the stream is opened.
#include "anpr/net/sdp.hpp"

#include <cctype>
#include <cstdint>
#include <locale>
#include <sstream>
#include <vector>

#include "anpr/net/crypto.hpp"

namespace anpr::net {
namespace {

/// Exp-Golomb bit reader over an RBSP. Reading past the end sets `failed` instead of throwing,
/// so a truncated SPS yields `ok == false` rather than garbage sizes.
class BitReader {
public:
    explicit BitReader(std::string data) : data_(std::move(data)) {}

    [[nodiscard]] bool failed() const { return failed_; }
    void fail() { failed_ = true; }

    std::uint32_t bits(unsigned count) {
        std::uint32_t value = 0;
        for (unsigned i = 0; i < count; ++i) {
            value = (value << 1U) | bit();
        }
        return value;
    }

    std::uint32_t bit() {
        if (position_ >= data_.size() * 8) {
            failed_ = true;
            return 0;
        }
        const auto byte = static_cast<unsigned char>(data_[position_ / 8]);
        const unsigned shift = 7U - static_cast<unsigned>(position_ % 8);
        ++position_;
        return (byte >> shift) & 1U;
    }

    void skip(std::size_t count) {
        position_ += count;
        if (position_ > data_.size() * 8) {
            failed_ = true;
        }
    }

    /// ue(v). Values above 2^32 - 2 cannot occur in a valid SPS and mark the read as failed.
    std::uint32_t ue() {
        unsigned leading_zeros = 0;
        while (!failed_ && bit() == 0) {
            if (++leading_zeros > 31) {
                failed_ = true;
                return 0;
            }
        }
        if (failed_) {
            return 0;
        }
        if (leading_zeros == 0) {
            return 0;
        }
        const std::uint64_t value =
            (std::uint64_t{1} << leading_zeros) - 1 + bits(leading_zeros);
        if (value > 0xFFFFFFFEULL) {
            failed_ = true;
            return 0;
        }
        return static_cast<std::uint32_t>(value);
    }

    /// se(v).
    std::int64_t se() {
        const std::uint32_t code = ue();
        return (code & 1U) != 0 ? static_cast<std::int64_t>((code + 1ULL) / 2)
                                : -static_cast<std::int64_t>(code / 2);
    }

private:
    std::string data_;
    std::size_t position_{0};
    bool failed_{false};
};

/// Removes emulation prevention bytes (00 00 03 -> 00 00) after the NAL header.
std::string unescapeRbsp(const std::string& nal, std::size_t header_bytes) {
    std::string rbsp;
    rbsp.reserve(nal.size());
    int zeros = 0;
    for (std::size_t i = header_bytes; i < nal.size(); ++i) {
        const auto byte = static_cast<unsigned char>(nal[i]);
        if (zeros >= 2 && byte == 0x03) {
            zeros = 0;
            continue;
        }
        rbsp.push_back(static_cast<char>(byte));
        zeros = byte == 0 ? zeros + 1 : 0;
    }
    return rbsp;
}

/// Largest picture dimension accepted from an SPS (H.265 level 6.2 allows 8192 x 4320).
constexpr std::uint32_t kMaxDimension = 16384;

void skipScalingList(BitReader& reader, int size) {
    int last_scale = 8;
    int next_scale = 8;
    for (int j = 0; j < size && !reader.failed(); ++j) {
        if (next_scale != 0) {
            const std::int64_t delta = reader.se();
            if (delta < -128 || delta > 127) {
                reader.fail();
                return;
            }
            next_scale = static_cast<int>((last_scale + delta + 256) % 256);
        }
        last_scale = next_scale == 0 ? last_scale : next_scale;
    }
}

bool isHighProfile(std::uint32_t profile_idc) {
    switch (profile_idc) {
        case 100:
        case 110:
        case 122:
        case 244:
        case 44:
        case 83:
        case 86:
        case 118:
        case 128:
        case 138:
        case 139:
        case 134:
        case 135:
            return true;
        default:
            return false;
    }
}

SpsInfo finishSize(std::uint64_t width, std::uint64_t height, std::uint64_t crop_x,
                   std::uint64_t crop_y) {
    SpsInfo info;
    if (width == 0 || height == 0 || width > kMaxDimension || height > kMaxDimension ||
        crop_x >= width || crop_y >= height) {
        return info;
    }
    info.ok = true;
    info.width = static_cast<int>(width - crop_x);
    info.height = static_cast<int>(height - crop_y);
    return info;
}

std::string toLower(std::string text) {
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

std::string toUpper(std::string text) {
    for (char& ch : text) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    return text;
}

std::string trim(const std::string& text) {
    const std::size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const std::size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

bool parseInt(const std::string& text, int& value) {
    if (text.empty() || text.size() > 9) {
        return false;
    }
    int parsed = 0;
    for (const char ch : text) {
        if (std::isdigit(static_cast<unsigned char>(ch)) == 0) {
            return false;
        }
        parsed = parsed * 10 + (ch - '0');
    }
    value = parsed;
    return true;
}

/// Locale-independent: a process that called setlocale() must still read "25.000" as 25.
bool parseDouble(const std::string& text, double& value) {
    std::istringstream in(text);
    in.imbue(std::locale::classic());
    double parsed = 0.0;
    in >> parsed;
    if (in.fail()) {
        return false;
    }
    value = parsed;
    return true;
}

VideoCodec codecForEncoding(const std::string& encoding_name) {
    const std::string name = toUpper(encoding_name);
    if (name.empty()) {
        return VideoCodec::kUnknown;
    }
    if (name == "H264") {
        return VideoCodec::kH264;
    }
    if (name == "H265" || name == "HEVC") {
        return VideoCodec::kH265;
    }
    if (name == "JPEG" || name == "MJPEG" || name == "MJPG") {
        return VideoCodec::kMjpeg;
    }
    if (name == "MP4V-ES") {
        return VideoCodec::kMpeg4;
    }
    return VideoCodec::kOther;
}

/// Static RTP payload types (RFC 3551) used for video without an a=rtpmap line.
std::string staticEncodingName(int payload_type) {
    switch (payload_type) {
        case 26:
            return "JPEG";
        case 31:
            return "H261";
        case 32:
            return "MPV";
        case 34:
            return "H263";
        default:
            return {};
    }
}

std::vector<std::string> split(const std::string& text, char separator) {
    std::vector<std::string> parts;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        std::size_t end = text.find(separator, begin);
        if (end == std::string::npos) {
            end = text.size();
        }
        parts.push_back(text.substr(begin, end - begin));
        begin = end + 1;
    }
    return parts;
}

/// "96 key=value; key2=value2" -> payload type and the remainder.
bool splitPayloadAttribute(const std::string& value, int& payload_type, std::string& rest) {
    const std::size_t space = value.find_first_of(" \t");
    if (!parseInt(trim(value.substr(0, space)), payload_type)) {
        return false;
    }
    rest = space == std::string::npos ? std::string() : trim(value.substr(space + 1));
    return true;
}

/// Width and height from the parameter sets in the fmtp line, when one parses.
void sizeFromParameterSets(SdpVideo& video) {
    const auto tryEntries = [&video](const std::string& list, bool h265) {
        for (const std::string& entry : split(list, ',')) {
            const auto nal = base64Decode(trim(entry));
            if (!nal || nal->empty()) {
                continue;
            }
            const auto header = static_cast<unsigned char>((*nal)[0]);
            const bool is_sps = h265 ? ((header >> 1U) & 0x3FU) == 33 : (header & 0x1FU) == 7;
            if (!is_sps) {
                continue;
            }
            const SpsInfo sps = h265 ? parseH265Sps(*nal) : parseH264Sps(*nal);
            if (sps.ok) {
                video.width = sps.width;
                video.height = sps.height;
                return true;
            }
        }
        return false;
    };
    const auto find = [&video](const char* key) {
        const auto it = video.fmtp.find(key);
        return it == video.fmtp.end() ? std::string() : it->second;
    };
    if (video.codec == VideoCodec::kH264) {
        tryEntries(find("sprop-parameter-sets"), false);
    } else if (video.codec == VideoCodec::kH265) {
        // RFC 7798 uses sprop-sps; a few older servers put all sets in sprop-parameter-sets.
        if (!tryEntries(find("sprop-sps"), true)) {
            tryEntries(find("sprop-parameter-sets"), true);
        }
    }
}

}  // namespace

std::string toString(VideoCodec codec) {
    switch (codec) {
        case VideoCodec::kUnknown:
            return "unknown";
        case VideoCodec::kH264:
            return "H.264";
        case VideoCodec::kH265:
            return "H.265";
        case VideoCodec::kMjpeg:
            return "MJPEG";
        case VideoCodec::kMpeg4:
            return "MPEG-4";
        case VideoCodec::kOther:
            return "other";
    }
    return "unknown";
}

std::string shortName(VideoCodec codec) {
    switch (codec) {
        case VideoCodec::kUnknown:
            return "unknown";
        case VideoCodec::kH264:
            return "H264";
        case VideoCodec::kH265:
            return "H265";
        case VideoCodec::kMjpeg:
            return "MJPEG";
        case VideoCodec::kMpeg4:
            return "MPEG4";
        case VideoCodec::kOther:
            return "other";
    }
    return "unknown";
}

SdpVideo parseSdpVideo(const std::string& sdp) {
    SdpVideo video;
    bool in_video = false;
    std::string rtpmap;
    std::string fmtp;
    for (const std::string& raw_line : split(sdp, '\n')) {
        const std::string line = trim(raw_line);
        if (line.size() < 2 || line[1] != '=') {
            continue;
        }
        const char type = line[0];
        const std::string value = line.substr(2);
        if (type == 'm') {
            if (in_video) {
                break;  // Only the first video section matters.
            }
            if (value.compare(0, 6, "video ") != 0) {
                continue;
            }
            in_video = true;
            video.present = true;
            // m=video <port> <proto> <fmt> ...
            std::istringstream fields(value);
            std::string media;
            std::string port;
            std::string proto;
            std::string format;
            fields >> media >> port >> proto >> format;
            int payload_type = -1;
            if (parseInt(format, payload_type)) {
                video.payload_type = payload_type;
            }
            continue;
        }
        if (!in_video || type != 'a') {
            continue;
        }
        const std::size_t colon = value.find(':');
        const std::string name = toLower(value.substr(0, colon));
        const std::string argument =
            colon == std::string::npos ? std::string() : trim(value.substr(colon + 1));
        int payload_type = -1;
        std::string rest;
        if (name == "rtpmap" && splitPayloadAttribute(argument, payload_type, rest) &&
            payload_type == video.payload_type) {
            rtpmap = rest;
        } else if (name == "fmtp" && splitPayloadAttribute(argument, payload_type, rest) &&
                   payload_type == video.payload_type) {
            fmtp = rest;
        } else if (name == "control") {
            video.control = argument;
        } else if (name == "framerate") {
            double framerate = 0.0;
            if (parseDouble(argument, framerate) && framerate > 0.0) {
                video.framerate = framerate;
            }
        }
    }
    if (!video.present) {
        return video;
    }

    if (!rtpmap.empty()) {
        // "H264/90000" or "H265/90000/1".
        const std::vector<std::string> parts = split(rtpmap, '/');
        video.encoding_name = trim(parts[0]);
        int clock_rate = 0;
        if (parts.size() > 1 && parseInt(trim(parts[1]), clock_rate)) {
            video.clock_rate = clock_rate;
        }
    } else if (video.payload_type >= 0 && video.payload_type < 96) {
        video.encoding_name = staticEncodingName(video.payload_type);
        if (!video.encoding_name.empty()) {
            video.clock_rate = 90000;
        }
    }
    video.codec = codecForEncoding(video.encoding_name);

    for (const std::string& parameter : split(fmtp, ';')) {
        const std::string item = trim(parameter);
        if (item.empty()) {
            continue;
        }
        // Split at the first '=' only: base64 values end in '=' padding.
        const std::size_t equals = item.find('=');
        const std::string key = toLower(trim(item.substr(0, equals)));
        if (key.empty()) {
            continue;
        }
        video.fmtp[key] =
            equals == std::string::npos ? std::string() : trim(item.substr(equals + 1));
    }
    sizeFromParameterSets(video);
    return video;
}

SpsInfo parseH264Sps(const std::string& nal) {
    if (nal.size() < 4 || (static_cast<unsigned char>(nal[0]) & 0x1FU) != 7) {
        return {};
    }
    BitReader reader(unescapeRbsp(nal, 1));
    const std::uint32_t profile_idc = reader.bits(8);
    reader.skip(8);  // constraint_set flags and reserved bits
    reader.skip(8);  // level_idc
    if (reader.ue() > 31) {
        return {};  // seq_parameter_set_id
    }

    std::uint32_t chroma_format_idc = 1;
    bool separate_colour_plane = false;
    if (isHighProfile(profile_idc)) {
        chroma_format_idc = reader.ue();
        if (chroma_format_idc > 3) {
            return {};
        }
        if (chroma_format_idc == 3) {
            separate_colour_plane = reader.bit() != 0;
        }
        if (reader.ue() > 6 || reader.ue() > 6) {
            return {};  // bit_depth_luma_minus8, bit_depth_chroma_minus8
        }
        reader.skip(1);  // qpprime_y_zero_transform_bypass_flag
        if (reader.bit() != 0) {  // seq_scaling_matrix_present_flag
            const int lists = chroma_format_idc != 3 ? 8 : 12;
            for (int i = 0; i < lists && !reader.failed(); ++i) {
                if (reader.bit() != 0) {
                    skipScalingList(reader, i < 6 ? 16 : 64);
                }
            }
        }
    }

    if (reader.ue() > 12) {
        return {};  // log2_max_frame_num_minus4
    }
    const std::uint32_t pic_order_cnt_type = reader.ue();
    if (pic_order_cnt_type == 0) {
        if (reader.ue() > 12) {
            return {};  // log2_max_pic_order_cnt_lsb_minus4
        }
    } else if (pic_order_cnt_type == 1) {
        reader.skip(1);  // delta_pic_order_always_zero_flag
        reader.se();     // offset_for_non_ref_pic
        reader.se();     // offset_for_top_to_bottom_field
        const std::uint32_t cycle = reader.ue();
        if (cycle > 255) {
            return {};
        }
        for (std::uint32_t i = 0; i < cycle && !reader.failed(); ++i) {
            reader.se();  // offset_for_ref_frame
        }
    } else if (pic_order_cnt_type != 2) {
        return {};
    }
    reader.ue();     // max_num_ref_frames
    reader.skip(1);  // gaps_in_frame_num_value_allowed_flag
    const std::uint64_t width_in_mbs = std::uint64_t{reader.ue()} + 1;
    const std::uint64_t height_in_map_units = std::uint64_t{reader.ue()} + 1;
    const bool frame_mbs_only = reader.bit() != 0;
    if (!frame_mbs_only) {
        reader.skip(1);  // mb_adaptive_frame_field_flag
    }
    reader.skip(1);  // direct_8x8_inference_flag
    std::uint64_t crop_left = 0;
    std::uint64_t crop_right = 0;
    std::uint64_t crop_top = 0;
    std::uint64_t crop_bottom = 0;
    if (reader.bit() != 0) {  // frame_cropping_flag
        crop_left = reader.ue();
        crop_right = reader.ue();
        crop_top = reader.ue();
        crop_bottom = reader.ue();
    }
    if (reader.failed()) {
        return {};
    }

    // Crop offsets count chroma samples (H.264 7.4.2.1.1, equations 7-19 to 7-22).
    const std::uint64_t field_factor = frame_mbs_only ? 1 : 2;
    const std::uint32_t chroma_array_type = separate_colour_plane ? 0 : chroma_format_idc;
    std::uint64_t crop_unit_x = 1;
    std::uint64_t crop_unit_y = field_factor;
    if (chroma_array_type != 0) {
        const std::uint64_t sub_width_c = chroma_format_idc == 3 ? 1 : 2;
        const std::uint64_t sub_height_c = chroma_format_idc == 1 ? 2 : 1;
        crop_unit_x = sub_width_c;
        crop_unit_y = sub_height_c * field_factor;
    }
    return finishSize(width_in_mbs * 16, field_factor * height_in_map_units * 16,
                      crop_unit_x * (crop_left + crop_right),
                      crop_unit_y * (crop_top + crop_bottom));
}

SpsInfo parseH265Sps(const std::string& nal) {
    if (nal.size() < 5 || ((static_cast<unsigned char>(nal[0]) >> 1U) & 0x3FU) != 33) {
        return {};
    }
    BitReader reader(unescapeRbsp(nal, 2));
    reader.skip(4);  // sps_video_parameter_set_id
    const std::uint32_t max_sub_layers_minus1 = reader.bits(3);
    if (max_sub_layers_minus1 > 6) {
        return {};
    }
    reader.skip(1);  // sps_temporal_id_nesting_flag

    // profile_tier_level(1, sps_max_sub_layers_minus1), H.265 7.3.3.
    reader.skip(2 + 1 + 5 + 32 + 4 + 43 + 1 + 8);  // general profile, flags and level_idc
    bool profile_present[8] = {};
    bool level_present[8] = {};
    for (std::uint32_t i = 0; i < max_sub_layers_minus1; ++i) {
        profile_present[i] = reader.bit() != 0;
        level_present[i] = reader.bit() != 0;
    }
    if (max_sub_layers_minus1 > 0) {
        reader.skip(2 * (8 - max_sub_layers_minus1));  // reserved_zero_2bits
    }
    for (std::uint32_t i = 0; i < max_sub_layers_minus1; ++i) {
        if (profile_present[i]) {
            reader.skip(2 + 1 + 5 + 32 + 4 + 43 + 1);
        }
        if (level_present[i]) {
            reader.skip(8);
        }
    }

    if (reader.ue() > 15) {
        return {};  // sps_seq_parameter_set_id
    }
    const std::uint32_t chroma_format_idc = reader.ue();
    if (chroma_format_idc > 3) {
        return {};
    }
    bool separate_colour_plane = false;
    if (chroma_format_idc == 3) {
        separate_colour_plane = reader.bit() != 0;
    }
    const std::uint64_t width = reader.ue();
    const std::uint64_t height = reader.ue();
    std::uint64_t left = 0;
    std::uint64_t right = 0;
    std::uint64_t top = 0;
    std::uint64_t bottom = 0;
    if (reader.bit() != 0) {  // conformance_window_flag
        left = reader.ue();
        right = reader.ue();
        top = reader.ue();
        bottom = reader.ue();
    }
    if (reader.failed()) {
        return {};
    }

    // Offsets are in chroma sample units (H.265 table 6-1); monochrome and separate planes use 1.
    const std::uint32_t chroma_array_type = separate_colour_plane ? 0 : chroma_format_idc;
    const std::uint64_t sub_width_c = (chroma_array_type == 1 || chroma_array_type == 2) ? 2 : 1;
    const std::uint64_t sub_height_c = chroma_array_type == 1 ? 2 : 1;
    return finishSize(width, height, sub_width_c * (left + right), sub_height_c * (top + bottom));
}

}  // namespace anpr::net
