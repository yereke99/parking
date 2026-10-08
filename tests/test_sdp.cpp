#include <cstdint>
#include <string>
#include <vector>

#include "anpr/net/crypto.hpp"
#include "anpr/net/sdp.hpp"
#include "test_framework.hpp"

using anpr::net::SpsInfo;
using anpr::net::VideoCodec;

namespace {

std::string fromHex(const std::string& hex) {
    std::string bytes;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        bytes.push_back(static_cast<char>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    }
    return bytes;
}

std::string toHex(const std::string& bytes) {
    static const char kDigits[] = "0123456789abcdef";
    std::string hex;
    for (const char ch : bytes) {
        hex.push_back(kDigits[static_cast<unsigned char>(ch) >> 4U]);
        hex.push_back(kDigits[static_cast<unsigned char>(ch) & 0x0FU]);
    }
    return hex;
}

struct SpsVector {
    const char* name;
    const char* hex;
    int width;
    int height;
};

// Real SPS NAL units (no start code) from ffmpeg 4.2 in the dev image: testsrc frames encoded
// with libx264 / libx265 at the camera's stream sizes, extracted from the Annex-B output.
const std::vector<SpsVector> kH264Vectors = {
    // High profile 1080p: coded 1088 lines, cropped by 8 (frame_crop_bottom_offset 4).
    {"1080p high", "67640028acd940780227e5c044000003000400000300c83c60c658", 1920, 1080},
    {"1520p high", "67640032acd9402a00bfb011000003000100000300320f183196", 2688, 1520},
    {"720p high", "6764001facd9405005bb011000000300100000030320f1831960", 1280, 720},
    {"720p main", "674d401feca02802dd80880000030008000003019078c18cb0", 1280, 720},
    {"360p baseline", "6742c01ed900a02ff97011000003000100000300320f162e48", 640, 360},
    {"1080p high 4:2:2", "677a0028bcd940780227e27011000003000100000300320f183196", 1920, 1080},
    {"1080i high (field coding)", "67640028acd94078044fde0220000003002000000643e2c5b2c0", 1920,
     1080},
};

const std::vector<SpsVector> kH265Vectors = {
    {"1080p main",
     "420101016000000300900000030000030078a003c08010e596566924caf0168080000003008000000c84",
     1920, 1080},
    {"1520p main",
     "420101016000000300900000030000030096a001502005f165959a4932bc05a02000000300200000030321",
     2688, 1520},
    {"720p main",
     "42010101600000030090000003000003005da00280802d165959a4932bc05a020000030002000003003210",
     1280, 720},
    // 1916x1076 is padded to 1920x1080 and cropped by a conformance window in chroma units.
    {"1916x1076 conformance window",
     "420101016000000300900000030000030078a003c08010e77796566924caf0168080000003008000000c84",
     1916, 1076},
    // Two temporal sub-layers (sps_max_sub_layers_minus1 = 1).
    {"720p temporal layers",
     "42010201600000030090000003000003005d0000a00280802d1659598acd24995e02d01000000300100000"
     "03019080",
     1280, 720},
    {"1080p 4:2:2 RExt",
     "4201010408000003009d08000003000078b003c08010e596566924caf0168080000003008000000c84", 1920,
     1080},
    // 4:4:4 crops in luma units (SubWidthC = SubHeightC = 1).
    {"1918x1078 4:4:4 RExt",
     "4201010408000003009e0800000300007890007810021ceef2cacd24995e02d0100000030010000003019080",
     1918, 1078},
};

/// Writes an RBSP bit by bit and wraps it as a NAL unit with emulation prevention bytes, to build
/// SPS variants x264/x265 never emit. Every built unit was checked with ffmpeg's trace_headers.
class BitWriter {
public:
    void bits(std::uint64_t value, unsigned count) {
        for (unsigned i = count; i-- > 0;) {
            bit(static_cast<unsigned>((value >> i) & 1U));
        }
    }
    void bit(unsigned value) {
        current_ = static_cast<unsigned>((current_ << 1U) | (value & 1U));
        if (++filled_ == 8) {
            bytes_.push_back(static_cast<char>(current_));
            current_ = 0;
            filled_ = 0;
        }
    }
    void ue(std::uint32_t value) {
        const std::uint64_t coded = std::uint64_t{value} + 1;
        unsigned length = 0;
        while ((coded >> (length + 1)) != 0) {
            ++length;
        }
        bits(0, length);
        bits(coded, length + 1);
    }
    void se(std::int32_t value) {
        ue(value > 0 ? static_cast<std::uint32_t>(2 * value - 1)
                     : static_cast<std::uint32_t>(-2 * static_cast<std::int64_t>(value)));
    }
    std::string nal(const std::string& header) {
        bit(1);  // rbsp_stop_one_bit
        while (filled_ != 0) {
            bit(0);
        }
        std::string out = header;
        int zeros = 0;
        for (const char ch : bytes_) {
            const auto byte = static_cast<unsigned char>(ch);
            if (zeros >= 2 && byte <= 3) {
                out.push_back(3);
                zeros = 0;
            }
            out.push_back(ch);
            zeros = byte == 0 ? zeros + 1 : 0;
        }
        return out;
    }

private:
    std::string bytes_;
    unsigned current_{0};
    unsigned filled_{0};
};

/// The H.264 SPS from log2_max_frame_num_minus4 on, with x264's values for the 1080p stream.
void h264Tail(BitWriter& w, std::uint32_t width_mbs, std::uint32_t height_units,
              bool frame_mbs_only, std::uint32_t left, std::uint32_t right, std::uint32_t top,
              std::uint32_t bottom) {
    w.ue(0);   // log2_max_frame_num_minus4
    w.ue(0);   // pic_order_cnt_type
    w.ue(2);   // log2_max_pic_order_cnt_lsb_minus4
    w.ue(4);   // max_num_ref_frames
    w.bit(0);  // gaps_in_frame_num_value_allowed_flag
    w.ue(width_mbs - 1);
    w.ue(height_units - 1);
    w.bit(frame_mbs_only ? 1 : 0);
    if (!frame_mbs_only) {
        w.bit(1);  // mb_adaptive_frame_field_flag
    }
    w.bit(1);  // direct_8x8_inference_flag
    const bool crop = left + right + top + bottom != 0;
    w.bit(crop ? 1 : 0);
    if (crop) {
        w.ue(left);
        w.ue(right);
        w.ue(top);
        w.ue(bottom);
    }
    w.bit(0);  // vui_parameters_present_flag
}

/// High profile 1080p with explicit, default-flagged and early-terminated scaling lists. It
/// replaces x264's SPS in the real 1080p stream and ffmpeg still decodes 1920x1080 frames.
std::string h264ScalingSps() {
    BitWriter w;
    w.bits(100, 8);  // profile_idc: High
    w.bits(0, 8);    // constraint flags
    w.bits(40, 8);   // level_idc
    w.ue(0);         // seq_parameter_set_id
    w.ue(1);         // chroma_format_idc 4:2:0
    w.ue(0);         // bit_depth_luma_minus8
    w.ue(0);         // bit_depth_chroma_minus8
    w.bit(0);        // qpprime_y_zero_transform_bypass_flag
    w.bit(1);        // seq_scaling_matrix_present_flag
    w.bit(1);        // list 0: 16 explicit deltas
    for (const int delta : {2, 1, 1, 0, 3, -1, 2, 1, 0, 1, 1, 2, -2, 1, 1, 0}) {
        w.se(delta);
    }
    w.bit(0);  // list 1 absent
    w.bit(1);  // list 2: the first delta makes nextScale 0 (useDefaultScalingMatrixFlag)
    w.se(-8);
    w.bit(0);  // lists 3 to 5 absent
    w.bit(0);
    w.bit(0);
    w.bit(1);  // list 6 (8x8): ten deltas, then one that repeats the last scale to the end
    for (const int delta : {8, 4, -2, 6, 1, 1, 3, -5, 2, 2, -28}) {
        w.se(delta);
    }
    w.bit(0);  // list 7 absent
    h264Tail(w, 120, 68, true, 0, 0, 0, 4);
    return w.nal(std::string(1, '\x67'));
}

/// Main profile (no chroma fields), pic_order_cnt_type 1, cropping on three sides.
std::string h264PocType1Sps() {
    BitWriter w;
    w.bits(77, 8);
    w.bits(0, 8);
    w.bits(30, 8);
    w.ue(1);   // seq_parameter_set_id
    w.ue(1);   // log2_max_frame_num_minus4
    w.ue(1);   // pic_order_cnt_type
    w.bit(0);  // delta_pic_order_always_zero_flag
    w.se(-2);  // offset_for_non_ref_pic
    w.se(1);   // offset_for_top_to_bottom_field
    w.ue(3);   // num_ref_frames_in_pic_order_cnt_cycle
    w.se(2);
    w.se(-1);
    w.se(4);
    w.ue(3);   // max_num_ref_frames
    w.bit(0);  // gaps_in_frame_num_value_allowed_flag
    w.ue(45 - 1);
    w.ue(30 - 1);
    w.bit(1);  // frame_mbs_only_flag
    w.bit(1);  // direct_8x8_inference_flag
    w.bit(1);  // frame_cropping_flag
    w.ue(2);
    w.ue(2);
    w.ue(0);
    w.ue(3);
    w.bit(0);  // vui_parameters_present_flag
    return w.nal(std::string(1, '\x67'));
}

/// High 4:4:4 Predictive: twelve scaling lists, crop units of one sample.
std::string h264Yuv444Sps() {
    BitWriter w;
    w.bits(244, 8);
    w.bits(0, 8);
    w.bits(31, 8);
    w.ue(0);
    w.ue(3);   // chroma_format_idc 4:4:4
    w.bit(0);  // separate_colour_plane_flag
    w.ue(2);   // bit_depth_luma_minus8
    w.ue(2);   // bit_depth_chroma_minus8
    w.bit(0);
    w.bit(1);  // seq_scaling_matrix_present_flag
    for (int i = 0; i < 12; ++i) {
        const bool present = i == 1 || i == 8 || i == 11;
        w.bit(present ? 1 : 0);
        if (present) {
            const int size = i < 6 ? 16 : 64;
            for (int j = 0; j < size; ++j) {
                w.se(j % 3 == 0 ? 1 : (j % 3 == 1 ? -1 : 0));
            }
        }
    }
    h264Tail(w, 80, 45, true, 0, 3, 0, 5);
    return w.nal(std::string(1, '\x67'));
}

/// Monochrome field coding: crop units are 1 x 2 luma samples.
std::string h264MonochromeFieldSps() {
    BitWriter w;
    w.bits(100, 8);
    w.bits(0, 8);
    w.bits(40, 8);
    w.ue(0);
    w.ue(0);  // chroma_format_idc 4:0:0
    w.ue(0);
    w.ue(0);
    w.bit(0);
    w.bit(0);
    h264Tail(w, 120, 34, false, 3, 0, 0, 4);
    return w.nal(std::string(1, '\x67'));
}

void h265ProfileTier(BitWriter& w, bool with_level, unsigned level) {
    w.bits(0, 2);             // profile_space
    w.bit(0);                 // tier_flag
    w.bits(1, 5);             // profile_idc: Main
    w.bits(0x60000000U, 32);  // compatibility flags: Main and Main 10
    w.bit(1);                 // progressive_source_flag
    w.bit(0);                 // interlaced_source_flag
    w.bit(0);                 // non_packed_constraint_flag
    w.bit(1);                 // frame_only_constraint_flag
    w.bits(0, 43);
    w.bit(0);
    if (with_level) {
        w.bits(level, 8);
    }
}

/// Three temporal sub-layers with sub-layer profile and level information, 1080p by cropping.
std::string h265SubLayerSps() {
    BitWriter w;
    w.bits(0, 4);  // sps_video_parameter_set_id
    w.bits(2, 3);  // sps_max_sub_layers_minus1
    w.bit(1);      // sps_temporal_id_nesting_flag
    h265ProfileTier(w, true, 123);
    w.bit(1);  // sub-layer 0: profile present
    w.bit(1);  //              level present
    w.bit(0);  // sub-layer 1: profile absent
    w.bit(1);  //              level present
    for (int i = 2; i < 8; ++i) {
        w.bits(0, 2);  // reserved_zero_2bits
    }
    h265ProfileTier(w, false, 0);
    w.bits(90, 8);  // sub-layer 0 level
    w.bits(93, 8);  // sub-layer 1 level
    w.ue(0);        // sps_seq_parameter_set_id
    w.ue(1);        // chroma_format_idc
    w.ue(1920);
    w.ue(1088);
    w.bit(1);  // conformance_window_flag
    w.ue(0);
    w.ue(0);
    w.ue(0);
    w.ue(4);   // 4 chroma rows = 8 luma rows
    w.ue(0);   // bit_depth_luma_minus8
    w.ue(0);   // bit_depth_chroma_minus8
    w.ue(4);   // log2_max_pic_order_cnt_lsb_minus4
    w.bit(1);  // sps_sub_layer_ordering_info_present_flag
    for (int i = 0; i < 3; ++i) {
        w.ue(4);  // sps_max_dec_pic_buffering_minus1
        w.ue(2);  // sps_max_num_reorder_pics
        w.ue(0);  // sps_max_latency_increase_plus1
    }
    w.ue(0);   // log2_min_luma_coding_block_size_minus3
    w.ue(3);   // log2_diff_max_min_luma_coding_block_size
    w.ue(0);   // log2_min_luma_transform_block_size_minus2
    w.ue(3);   // log2_diff_max_min_luma_transform_block_size
    w.ue(0);   // max_transform_hierarchy_depth_inter
    w.ue(0);   // max_transform_hierarchy_depth_intra
    w.bit(0);  // scaling_list_enabled_flag
    w.bit(1);  // amp_enabled_flag
    w.bit(1);  // sample_adaptive_offset_enabled_flag
    w.bit(0);  // pcm_enabled_flag
    w.ue(0);   // num_short_term_ref_pic_sets
    w.bit(0);  // long_term_ref_pics_present_flag
    w.bit(1);  // sps_temporal_mvp_enabled_flag
    w.bit(1);  // strong_intra_smoothing_enabled_flag
    w.bit(0);  // vui_parameters_present_flag
    w.bit(0);  // sps_extension_present_flag
    return w.nal(std::string("\x42\x01", 2));
}

// Hikvision-style SDPs (layout as served by the cameras' RTSP server) with the parameter sets of
// the 1080p H.264 and 2688x1520 H.265 vectors above.
const char* const kHikvisionH264Sdp =
    "v=0\r\n"
    "o=- 1109162014219182 1109162014219192 IN IP4 192.168.1.64\r\n"
    "s=Media Presentation\r\n"
    "e=NONE\r\n"
    "b=AS:5050\r\n"
    "t=0 0\r\n"
    "a=control:rtsp://192.168.1.64:554/Streaming/Channels/101/?transportmode=unicast\r\n"
    "m=video 0 RTP/AVP 96\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "b=AS:5000\r\n"
    "a=recvonly\r\n"
    "a=x-dimensions:1920,1080\r\n"
    "a=control:rtsp://192.168.1.64:554/Streaming/Channels/101/trackID=1?transportmode=unicast\r\n"
    "a=rtpmap:96 H264/90000\r\n"
    "a=fmtp:96 profile-level-id=640028; packetization-mode=1; "
    "sprop-parameter-sets=Z2QAKKzZQHgCJ+XARAAAAwAEAAADAMg8YMZY,aOvjyyLA\r\n"
    "a=framerate:25.000000\r\n"
    "a=Media_header:MEDIAINFO=494D4B48010200000400000100000000000000000000000000000000000000000000"
    "000000000000;\r\n"
    "a=appversion:1.0\r\n";

const char* const kHikvisionH265Sdp =
    "v=0\n"
    "o=- 1109162014219182 1109162014219192 IN IP4 192.168.1.64\n"
    "s=Media Presentation\n"
    "t=0 0\n"
    "m=audio 0 RTP/AVP 0\n"
    "a=rtpmap:0 PCMU/8000\n"
    "a=control:trackID=2\n"
    "m=video 0 RTP/AVP 96\n"
    "a=rtpmap:96 H265/90000\n"
    "a=fmtp:96 sprop-vps=QAEMAf//AWAAAAMAkAAAAwAAAwCWlZgJ; "
    "sprop-sps=QgEBAWAAAAMAkAAAAwAAAwCWoAFQIAXxZZWaSTK8BaAgAAADACAAAAMDIQ==; "
    "sprop-pps=RAHBcrQiQA==\n"
    "a=control:trackID=1\n"
    "m=application 0 RTP/AVP 107\n"
    "a=rtpmap:107 isapi.metadata/90000\n"
    "a=control:trackID=3\n";

}  // namespace

TEST("H.264 SPS from x264 give the coded picture size after cropping") {
    for (const SpsVector& vector : kH264Vectors) {
        const SpsInfo info = anpr::net::parseH264Sps(fromHex(vector.hex));
        CHECK(info.ok);
        CHECK_EQ(info.width, vector.width);
        CHECK_EQ(info.height, vector.height);
    }
}

TEST("H.265 SPS from x265 give the picture size after the conformance window") {
    for (const SpsVector& vector : kH265Vectors) {
        const SpsInfo info = anpr::net::parseH265Sps(fromHex(vector.hex));
        CHECK(info.ok);
        CHECK_EQ(info.width, vector.width);
        CHECK_EQ(info.height, vector.height);
    }
}

TEST("H.264 SPS with scaling lists, POC type 1, 4:4:4 and field coding parse") {
    // Pin the builders to the exact units that ffmpeg's bitstream tracer accepted.
    CHECK_EQ(toHex(h264ScalingSps()),
             std::string("67640028ad9129991522152a111080828c48c2c84072d940780227e540"));
    CHECK_EQ(toHex(h264PocType1Sps()), std::string("674d001e490a884620805a1eedc880"));
    CHECK_EQ(toHex(h264MonochromeFieldSps()), std::string("67640028f36501e0113932a0"));
    CHECK_EQ(toHex(h264Yuv444Sps()).substr(0, 24), std::string("67f4001f90daa74e9d3a7405"));

    SpsInfo info = anpr::net::parseH264Sps(h264ScalingSps());
    CHECK(info.ok);
    CHECK_EQ(info.width, 1920);
    CHECK_EQ(info.height, 1080);

    info = anpr::net::parseH264Sps(h264PocType1Sps());
    CHECK(info.ok);
    CHECK_EQ(info.width, 720 - 2 * (2 + 2));
    CHECK_EQ(info.height, 480 - 2 * 3);

    info = anpr::net::parseH264Sps(h264Yuv444Sps());
    CHECK(info.ok);
    CHECK_EQ(info.width, 1280 - 3);
    CHECK_EQ(info.height, 720 - 5);

    info = anpr::net::parseH264Sps(h264MonochromeFieldSps());
    CHECK(info.ok);
    CHECK_EQ(info.width, 1920 - 3);
    CHECK_EQ(info.height, 2 * 34 * 16 - 2 * 4);
}

TEST("H.265 SPS with sub-layer profile and level information parses") {
    const std::string nal = h265SubLayerSps();
    CHECK_EQ(toHex(nal), std::string("42010501600000030090000003000003007bd000016000000300900000"
                                     "03000003005a5da003c0801107cb96572b95e4936b20"));
    const SpsInfo info = anpr::net::parseH265Sps(nal);
    CHECK(info.ok);
    CHECK_EQ(info.width, 1920);
    CHECK_EQ(info.height, 1080);
}

TEST("SPS parsers reject other NAL types, truncation and impossible values") {
    const std::string h264 = fromHex(kH264Vectors[0].hex);
    const std::string h265 = fromHex(kH265Vectors[0].hex);
    CHECK(!anpr::net::parseH264Sps("").ok);
    CHECK(!anpr::net::parseH265Sps("").ok);
    CHECK(!anpr::net::parseH264Sps(h265).ok);  // Wrong NAL type for each other.
    CHECK(!anpr::net::parseH265Sps(h264).ok);
    std::string pps = h264;
    pps[0] = '\x68';
    CHECK(!anpr::net::parseH264Sps(pps).ok);
    for (std::size_t length = 1; length < 11; ++length) {
        CHECK(!anpr::net::parseH264Sps(h264.substr(0, length)).ok);
    }
    for (std::size_t length = 1; length < 20; ++length) {
        CHECK(!anpr::net::parseH265Sps(h265.substr(0, length)).ok);
    }

    // Cropping away the whole picture is invalid.
    BitWriter crop_all;
    crop_all.bits(66, 8);
    crop_all.bits(0, 8);
    crop_all.bits(30, 8);
    crop_all.ue(0);
    h264Tail(crop_all, 2, 2, true, 0, 0, 0, 16);
    CHECK(!anpr::net::parseH264Sps(crop_all.nal(std::string(1, '\x67'))).ok);

    // chroma_format_idc 4 does not exist.
    BitWriter bad_chroma;
    bad_chroma.bits(100, 8);
    bad_chroma.bits(0, 8);
    bad_chroma.bits(40, 8);
    bad_chroma.ue(0);
    bad_chroma.ue(4);
    h264Tail(bad_chroma, 120, 68, true, 0, 0, 0, 0);
    CHECK(!anpr::net::parseH264Sps(bad_chroma.nal(std::string(1, '\x67'))).ok);

    // An all-zero payload is an endless exp-Golomb prefix.
    CHECK(!anpr::net::parseH264Sps(std::string("\x67\x64\x00\x28", 4) + std::string(16, '\0')).ok);
}

TEST("SDP parsing finds the Hikvision H.264 video section and its size") {
    const auto video = anpr::net::parseSdpVideo(kHikvisionH264Sdp);
    CHECK(video.present);
    CHECK_EQ(video.codec, VideoCodec::kH264);
    CHECK_EQ(video.encoding_name, std::string("H264"));
    CHECK_EQ(video.payload_type, 96);
    CHECK_EQ(video.clock_rate, 90000);
    CHECK_EQ(video.control,
             std::string("rtsp://192.168.1.64:554/Streaming/Channels/101/trackID=1?transportmode="
                         "unicast"));
    CHECK_EQ(video.fmtp.size(), std::size_t{3});
    CHECK_EQ(video.fmtp.at("profile-level-id"), std::string("640028"));
    CHECK_EQ(video.fmtp.at("packetization-mode"), std::string("1"));
    CHECK_EQ(video.fmtp.at("sprop-parameter-sets"),
             std::string("Z2QAKKzZQHgCJ+XARAAAAwAEAAADAMg8YMZY,aOvjyyLA"));
    CHECK_NEAR(video.framerate, 25.0, 1e-9);
    CHECK_EQ(video.width, 1920);
    CHECK_EQ(video.height, 1080);
}

TEST("SDP parsing handles H.265 after an audio section with LF line endings") {
    const auto video = anpr::net::parseSdpVideo(kHikvisionH265Sdp);
    CHECK(video.present);
    CHECK_EQ(video.codec, VideoCodec::kH265);
    CHECK_EQ(video.encoding_name, std::string("H265"));
    CHECK_EQ(video.control, std::string("trackID=1"));  // Not the audio or metadata control.
    CHECK_EQ(video.fmtp.count("sprop-vps"), std::size_t{1});
    CHECK_EQ(video.fmtp.at("sprop-pps"), std::string("RAHBcrQiQA=="));
    CHECK_EQ(video.width, 2688);
    CHECK_EQ(video.height, 1520);
    CHECK_EQ(video.framerate, 0.0);

    // "HEVC" is accepted as an encoding name, and parameter names are case-insensitive.
    const std::string hevc =
        "v=0\r\nm=video 5000 RTP/AVP 98\r\na=rtpmap:98 hevc/90000\r\n"
        "a=fmtp:98 SPROP-SPS=" +
        anpr::net::base64Encode(fromHex(kH265Vectors[3].hex)) + "\r\n";
    const auto named = anpr::net::parseSdpVideo(hevc);
    CHECK_EQ(named.codec, VideoCodec::kH265);
    CHECK_EQ(named.encoding_name, std::string("hevc"));
    CHECK_EQ(named.width, 1916);
    CHECK_EQ(named.height, 1076);
}

TEST("SDP parsing uses the first video payload type and finds the SPS among the sets") {
    // The PPS comes first here; the attributes of payload 97 are ignored.
    const std::string sdp =
        "v=0\r\n"
        "m=video 0 RTP/AVP 96 97\r\n"
        "a=fmtp:97 sprop-parameter-sets=garbage\r\n"
        "a=rtpmap:97 H265/90000\r\n"
        "a=fmtp:96 packetization-mode=1;sprop-parameter-sets=aOvjyyA=,"
        "Z01AH+ygKALdgIgAAAMACAAAAwGQeMGMsA==;flag\r\n"
        "a=rtpmap:96 H264/90000\r\n"
        "m=video 0 RTP/AVP 100\r\n"
        "a=rtpmap:100 H265/90000\r\n";
    const auto video = anpr::net::parseSdpVideo(sdp);
    CHECK_EQ(video.payload_type, 96);
    CHECK_EQ(video.codec, VideoCodec::kH264);
    CHECK_EQ(video.width, 1280);
    CHECK_EQ(video.height, 720);
    CHECK_EQ(video.fmtp.count("flag"), std::size_t{1});
    CHECK(video.fmtp.at("flag").empty());
}

TEST("SDP parsing covers static payloads, unknown codecs and missing video") {
    auto video = anpr::net::parseSdpVideo("v=0\r\nm=video 0 RTP/AVP 26\r\na=control:track1\r\n");
    CHECK(video.present);
    CHECK_EQ(video.codec, VideoCodec::kMjpeg);
    CHECK_EQ(video.encoding_name, std::string("JPEG"));
    CHECK_EQ(video.clock_rate, 90000);
    CHECK_EQ(video.width, 0);

    video = anpr::net::parseSdpVideo(
        "v=0\r\nm=video 0 RTP/AVP 96\r\na=rtpmap:96 MP4V-ES/90000\r\n"
        "a=fmtp:96 config=000001B0\r\n");
    CHECK_EQ(video.codec, VideoCodec::kMpeg4);
    CHECK_EQ(video.fmtp.at("config"), std::string("000001B0"));

    video = anpr::net::parseSdpVideo("v=0\r\nm=video 0 RTP/AVP 96\r\na=rtpmap:96 VP8/90000\r\n");
    CHECK_EQ(video.codec, VideoCodec::kOther);

    video = anpr::net::parseSdpVideo("v=0\r\nm=video 0 RTP/AVP 96\r\n");
    CHECK(video.present);
    CHECK_EQ(video.codec, VideoCodec::kUnknown);
    CHECK_EQ(video.payload_type, 96);

    // A broken parameter set leaves the size unknown instead of failing the parse.
    video = anpr::net::parseSdpVideo("v=0\r\nm=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\n"
                                     "a=fmtp:96 sprop-parameter-sets=Z2QA,!!!\r\n");
    CHECK_EQ(video.codec, VideoCodec::kH264);
    CHECK_EQ(video.width, 0);
    CHECK_EQ(video.height, 0);

    video = anpr::net::parseSdpVideo("v=0\r\nm=audio 0 RTP/AVP 0\r\na=rtpmap:0 PCMU/8000\r\n");
    CHECK(!video.present);
    CHECK_EQ(video.payload_type, -1);
    CHECK(!anpr::net::parseSdpVideo("").present);
    CHECK(!anpr::net::parseSdpVideo("garbage without equals signs").present);
}

TEST("codec names for reports") {
    CHECK_EQ(anpr::net::toString(VideoCodec::kH264), std::string("H.264"));
    CHECK_EQ(anpr::net::toString(VideoCodec::kH265), std::string("H.265"));
    CHECK_EQ(anpr::net::toString(VideoCodec::kMjpeg), std::string("MJPEG"));
    CHECK_EQ(anpr::net::toString(VideoCodec::kMpeg4), std::string("MPEG-4"));
    CHECK_EQ(anpr::net::toString(VideoCodec::kOther), std::string("other"));
    CHECK_EQ(anpr::net::toString(VideoCodec::kUnknown), std::string("unknown"));
    CHECK_EQ(anpr::net::shortName(VideoCodec::kH264), std::string("H264"));
    CHECK_EQ(anpr::net::shortName(VideoCodec::kH265), std::string("H265"));
    CHECK_EQ(anpr::net::shortName(VideoCodec::kMpeg4), std::string("MPEG4"));
}
