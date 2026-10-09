#pragma once

#include <string>

namespace anpr {

/// Clockwise rotation, in degrees (0, 90, 180 or 270), that a video file's first video track asks
/// the player to apply before display, read from the track header matrix of an MP4/QuickTime
/// file. 0 for anything else: no video track, an unrotated or non-right-angle matrix, a file that
/// is not MP4/QuickTime, a truncated or unreadable file.
///
/// Phones record portrait video as landscape pictures plus this matrix. OpenCV applies it only
/// in some versions and builds (OpenCV 4.2 never does; newer FFmpeg backends do by default), so
/// the file source reads it itself, switches OpenCV's own rotation off where it exists and
/// rotates the frames, and a portrait clip reaches the detector upright on every machine.
int mp4DisplayRotation(const std::string& path);

/// The rotation encoded by a track header matrix (a, b, c, d in 16.16 fixed point), the same
/// quantity FFmpeg reports as the "rotate" tag. 0 when it is not a right-angle rotation.
int matrixRotationDegrees(int a, int b, int c, int d);

}  // namespace anpr
