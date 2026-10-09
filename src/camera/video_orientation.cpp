#include "anpr/camera/video_orientation.hpp"

#include <cstdint>
#include <fstream>
#include <optional>

namespace anpr {
namespace {

constexpr int kOne = 0x10000;  // 1.0 in 16.16 fixed point

std::uint32_t be32(const unsigned char* bytes) {
    return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
           (static_cast<std::uint32_t>(bytes[1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[2]) << 8U) | static_cast<std::uint32_t>(bytes[3]);
}

std::uint64_t be64(const unsigned char* bytes) {
    return (static_cast<std::uint64_t>(be32(bytes)) << 32U) | be32(bytes + 4);
}

/// One box header: where its payload starts and ends in the file.
struct Box {
    std::string type;
    std::uint64_t payload_begin{0};
    std::uint64_t end{0};
};

/// Reads the box header at `offset`, bounded by `limit`. nullopt at the end or on corruption.
std::optional<Box> readBox(std::ifstream& file, std::uint64_t offset, std::uint64_t limit) {
    if (offset + 8 > limit) {
        return std::nullopt;
    }
    unsigned char header[16];
    file.clear();
    file.seekg(static_cast<std::streamoff>(offset));
    if (!file.read(reinterpret_cast<char*>(header), 8)) {
        return std::nullopt;
    }
    std::uint64_t size = be32(header);
    std::uint64_t header_size = 8;
    if (size == 1) {
        if (offset + 16 > limit || !file.read(reinterpret_cast<char*>(header + 8), 8)) {
            return std::nullopt;
        }
        size = be64(header + 8);
        header_size = 16;
    } else if (size == 0) {
        size = limit - offset;  // the box runs to the end of its parent
    }
    if (size < header_size || offset + size > limit) {
        return std::nullopt;
    }
    return Box{std::string(reinterpret_cast<const char*>(header + 4), 4), offset + header_size,
               offset + size};
}

/// The first child box of `type` inside [begin, end).
std::optional<Box> findChild(std::ifstream& file, std::uint64_t begin, std::uint64_t end,
                             const std::string& type) {
    std::uint64_t offset = begin;
    // A bound on the walk, so a corrupt file of zero-progress boxes cannot spin.
    for (int guard = 0; guard < 4096; ++guard) {
        const std::optional<Box> box = readBox(file, offset, end);
        if (!box) {
            return std::nullopt;
        }
        if (box->type == type) {
            return box;
        }
        offset = box->end;
    }
    return std::nullopt;
}

bool isVideoTrack(std::ifstream& file, const Box& trak) {
    const std::optional<Box> mdia = findChild(file, trak.payload_begin, trak.end, "mdia");
    if (!mdia) {
        return false;
    }
    const std::optional<Box> hdlr = findChild(file, mdia->payload_begin, mdia->end, "hdlr");
    if (!hdlr || hdlr->end - hdlr->payload_begin < 12) {
        return false;
    }
    // version/flags (4), pre_defined (4), handler_type (4)
    unsigned char bytes[12];
    file.clear();
    file.seekg(static_cast<std::streamoff>(hdlr->payload_begin));
    if (!file.read(reinterpret_cast<char*>(bytes), sizeof(bytes))) {
        return false;
    }
    return std::string(reinterpret_cast<const char*>(bytes + 8), 4) == "vide";
}

std::optional<int> trackRotation(std::ifstream& file, const Box& trak) {
    const std::optional<Box> tkhd = findChild(file, trak.payload_begin, trak.end, "tkhd");
    if (!tkhd) {
        return std::nullopt;
    }
    unsigned char version = 0;
    file.clear();
    file.seekg(static_cast<std::streamoff>(tkhd->payload_begin));
    if (!file.read(reinterpret_cast<char*>(&version), 1)) {
        return std::nullopt;
    }
    // version/flags, then times, track id, reserved and duration (32- or 64-bit), then reserved
    // (8), layer, alternate group, volume, reserved (2 each), then the 3x3 matrix.
    const std::uint64_t matrix_offset = tkhd->payload_begin + 4 + (version == 1 ? 32 : 20) + 16;
    if (matrix_offset + 36 > tkhd->end) {
        return std::nullopt;
    }
    unsigned char matrix[36];
    file.clear();
    file.seekg(static_cast<std::streamoff>(matrix_offset));
    if (!file.read(reinterpret_cast<char*>(matrix), sizeof(matrix))) {
        return std::nullopt;
    }
    const auto entry = [&matrix](int index) {
        return static_cast<int>(static_cast<std::int32_t>(be32(matrix + index * 4)));
    };
    return matrixRotationDegrees(entry(0), entry(1), entry(3), entry(4));
}

}  // namespace

int matrixRotationDegrees(int a, int b, int c, int d) {
    // Pure rotations only, as written by phones and cameras. FFmpeg names [0 1; -1 0] a 90 degree
    // ("rotate=90") clockwise display rotation.
    if (a == kOne && b == 0 && c == 0 && d == kOne) {
        return 0;
    }
    if (a == 0 && b == kOne && c == -kOne && d == 0) {
        return 90;
    }
    if (a == -kOne && b == 0 && c == 0 && d == -kOne) {
        return 180;
    }
    if (a == 0 && b == -kOne && c == kOne && d == 0) {
        return 270;
    }
    return 0;
}

int mp4DisplayRotation(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return 0;
    }
    file.seekg(0, std::ios::end);
    const std::streamoff length = file.tellg();
    if (length <= 0) {
        return 0;
    }
    const auto size = static_cast<std::uint64_t>(length);
    const std::optional<Box> moov = findChild(file, 0, size, "moov");
    if (!moov) {
        return 0;
    }
    std::uint64_t offset = moov->payload_begin;
    for (int guard = 0; guard < 256; ++guard) {
        const std::optional<Box> trak = findChild(file, offset, moov->end, "trak");
        if (!trak) {
            return 0;
        }
        if (isVideoTrack(file, *trak)) {
            return trackRotation(file, *trak).value_or(0);
        }
        offset = trak->end;
    }
    return 0;
}

}  // namespace anpr
