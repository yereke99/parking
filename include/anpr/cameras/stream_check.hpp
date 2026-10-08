#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "anpr/cameras/camera_mode_config.hpp"
#include "anpr/cameras/diagnostics.hpp"
#include "anpr/cameras/gst_pipeline.hpp"
#include "anpr/cameras/rtsp_preflight.hpp"
#include "anpr/net/network_topology.hpp"
#include "anpr/net/sdp.hpp"

namespace anpr::cameras {

/// The full per-camera stream preflight: network, RTSP port, authentication, stream path, codec,
/// then the real decoder opened inside this container and frames read for `duration_ms`.
struct StreamCheckResult {
    CameraError error{CameraError::kNone};
    std::string detail;
    RtspCheck rtsp;
    /// Decoder that produced frames ("nvidia_hardware", "software_gstreamer", "software_ffmpeg").
    std::string decoder;
    /// Decoders tried before the one that worked, with why each failed.
    std::vector<std::string> fallbacks;
    /// Caveats about the decoder that produced frames (DecoderAttempt::note) or the plan.
    std::vector<std::string> notes;
    net::VideoCodec codec{net::VideoCodec::kUnknown};
    int width{0};
    int height{0};
    /// Frames per second measured over the read window.
    double measured_fps{0.0};
    /// The stream's nominal rate (SDP or decoder), 0 when unknown.
    double nominal_fps{0.0};
    /// From starting to open the decoder to the first frame.
    double first_frame_ms{0.0};
    int frames{0};

    [[nodiscard]] bool ready() const { return error == CameraError::kNone && frames > 0; }
};

StreamCheckResult checkStream(const CameraTarget& target, const CameraModeConfig& config,
                              const DecoderCapabilities& capabilities,
                              const net::NetworkSnapshot& snapshot,
                              const net::CameraLanSelection& lan, std::int64_t duration_ms);

/// The CAMERA block of `make camera-check`:
///   CAMERA camera-01
///     IP: 192.168.10.21   MAC: 44:19:b6:..   Vendor: Hikvision   Model: DS-TCG406-E
///     RTSP: OK   Stream: main   URL: rtsp://<redacted>@192.168.10.21:554/Streaming/Channels/101
///     Codec: H.264   Resolution: 2688x1520   FPS: 20.0   First frame: 430 ms
///     Decoder: nvidia_hardware
///     STATUS: READY
std::string formatStreamCheck(const CameraTarget& target, const StreamCheckResult& result);

/// The RTSP column of the reports for a camera's last diagnostic: "AUTH" (rejected login),
/// "NOCRED", "PORT", "PATH", "DOWN" (unreachable), "ERROR" (not RTSP), "-" (never contacted:
/// other subnet, duplicate IP, not activated) and "OK" for anything found after RTSP answered
/// (decoder and stream problems) or no error at all when `rtsp_ok`.
std::string rtspStatusLabel(CameraError error, bool rtsp_ok);

}  // namespace anpr::cameras
