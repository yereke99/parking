#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "anpr/camera/video_orientation.hpp"
#include "anpr/common/filesystem.hpp"

#include "test_framework.hpp"

namespace {

using Bytes = std::string;

Bytes u32(std::uint32_t value) {
    Bytes out(4, '\0');
    for (int i = 0; i < 4; ++i) {
        out[static_cast<std::size_t>(i)] = static_cast<char>((value >> (24 - 8 * i)) & 0xFFU);
    }
    return out;
}

Bytes box(const std::string& type, const Bytes& payload) {
    return u32(static_cast<std::uint32_t>(8 + payload.size())) + type + payload;
}

/// A 64-bit "largesize" box header, which some muxers write for big boxes.
Bytes largeBox(const std::string& type, const Bytes& payload) {
    return u32(1) + type + u32(0) + u32(static_cast<std::uint32_t>(16 + payload.size())) + payload;
}

Bytes tkhd(int a, int b, int c, int d, bool version1 = false) {
    Bytes payload;
    payload += version1 ? Bytes("\x01\0\0\x07", 4) : Bytes("\0\0\0\x07", 4);
    payload += Bytes(version1 ? 32 : 20, '\0');  // times, track id, reserved, duration
    payload += Bytes(16, '\0');                  // reserved, layer, group, volume, reserved
    const int matrix[9] = {a, b, 0, c, d, 0, 0, 0, 0x40000000};
    for (const int value : matrix) {
        payload += u32(static_cast<std::uint32_t>(value));
    }
    payload += u32(1920U << 16U) + u32(1080U << 16U);
    return box("tkhd", payload);
}

Bytes trak(const std::string& handler, const Bytes& header) {
    const Bytes hdlr = box("hdlr", Bytes(8, '\0') + handler + Bytes(12, '\0') + Bytes("\0", 1));
    return box("trak", header + box("mdia", box("mdhd", Bytes(24, '\0')) + hdlr));
}

std::string writeTemp(const Bytes& bytes, const std::string& name) {
    const anpr::filesystem::path path = anpr::filesystem::temp_directory_path() / name;
    std::ofstream out(path.string(), std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return path.string();
}

constexpr int kOne = 0x10000;

}  // namespace

TEST("track matrix maps to the clockwise display rotation") {
    CHECK_EQ(anpr::matrixRotationDegrees(kOne, 0, 0, kOne), 0);
    CHECK_EQ(anpr::matrixRotationDegrees(0, kOne, -kOne, 0), 90);
    CHECK_EQ(anpr::matrixRotationDegrees(-kOne, 0, 0, -kOne), 180);
    CHECK_EQ(anpr::matrixRotationDegrees(0, -kOne, kOne, 0), 270);
    // A mirror or a scaled matrix is not a plain rotation: left alone.
    CHECK_EQ(anpr::matrixRotationDegrees(-kOne, 0, 0, kOne), 0);
    CHECK_EQ(anpr::matrixRotationDegrees(0, 2 * kOne, -2 * kOne, 0), 0);
}

TEST("mp4 rotation is read from the video track, wherever moov is") {
    const Bytes ftyp = box("ftyp", Bytes("isom\0\0\0\0isomiso2", 16));
    const Bytes mdat = box("mdat", Bytes(64, 'x'));
    const Bytes portrait = trak("vide", tkhd(0, kOne, -kOne, 0));

    CHECK_EQ(anpr::mp4DisplayRotation(writeTemp(ftyp + box("moov", portrait) + mdat, "kz_rot_a.mp4")),
             90);
    // moov after the media data, as most cameras write it.
    CHECK_EQ(anpr::mp4DisplayRotation(writeTemp(ftyp + mdat + box("moov", portrait), "kz_rot_b.mp4")),
             90);
    // A sound track first: its matrix must not be taken for the video's.
    const Bytes sound = trak("soun", tkhd(-kOne, 0, 0, -kOne));
    const Bytes upside_down = trak("vide", tkhd(-kOne, 0, 0, -kOne, true));
    CHECK_EQ(anpr::mp4DisplayRotation(
                 writeTemp(ftyp + box("moov", sound + upside_down) + mdat, "kz_rot_c.mp4")),
             180);
    CHECK_EQ(anpr::mp4DisplayRotation(writeTemp(
                 ftyp + largeBox("moov", trak("vide", tkhd(0, -kOne, kOne, 0))) + mdat,
                 "kz_rot_d.mp4")),
             270);
    CHECK_EQ(anpr::mp4DisplayRotation(
                 writeTemp(ftyp + box("moov", trak("vide", tkhd(kOne, 0, 0, kOne))), "kz_rot_e.mp4")),
             0);
}

TEST("mp4 rotation is 0 for anything it cannot read") {
    CHECK_EQ(anpr::mp4DisplayRotation("/nonexistent/clip.mp4"), 0);
    CHECK_EQ(anpr::mp4DisplayRotation(writeTemp("not a video at all", "kz_rot_f.mp4")), 0);
    // A moov that claims to be longer than the file.
    const Bytes truncated = box("ftyp", Bytes(8, '\0')) + u32(4096) + "moov" + Bytes(20, '\0');
    CHECK_EQ(anpr::mp4DisplayRotation(writeTemp(truncated, "kz_rot_g.mp4")), 0);
    // A track header cut before its matrix.
    const Bytes short_tkhd = box("tkhd", Bytes(30, '\0'));
    const Bytes cut = box("moov", trak("vide", short_tkhd));
    CHECK_EQ(anpr::mp4DisplayRotation(writeTemp(cut, "kz_rot_h.mp4")), 0);
    // Only an audio track.
    CHECK_EQ(anpr::mp4DisplayRotation(
                 writeTemp(box("moov", trak("soun", tkhd(0, kOne, -kOne, 0))), "kz_rot_i.mp4")),
             0);
}
