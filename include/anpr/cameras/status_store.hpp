#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace anpr::cameras {

/// Health of one camera as shown by `make camera-status` and written to the status file
/// (var/cameras/status.json) by a running `make run-cameras`.
struct CameraStatus {
    std::string id;
    std::string ip;
    std::string mac;
    std::string model;
    /// "OK" (reachable), "DOWN" or "-".
    std::string link{"-"};
    /// "OK", "AUTH", "PORT", "PATH", "NOCRED", "DOWN", "ERROR" or "-".
    std::string rtsp{"-"};
    /// Codec short name ("H264", "H265") when frames arrive, else "-".
    std::string video{"-"};
    std::string decoder;
    int width{0};
    int height{0};
    /// "RUNNING", "STARTING", "RECONNECTING", "STALLED", "ERROR", "DISABLED", "OFFLINE",
    /// "STANDBY" (over the camera limit), "STOPPED" (no ANPR process running).
    std::string anpr{"-"};
    /// Last diagnostic code ("RTSP_AUTH_FAILED"), empty when healthy.
    std::string error;
    std::string action;
    std::string detail;

    double input_fps{0.0};
    double processed_fps{0.0};
    double detector_fps{0.0};
    double detector_avg_ms{0.0};
    std::int64_t ocr_calls{0};
    double ocr_avg_ms{0.0};
    std::int64_t plates_confirmed{0};
    std::int64_t live_dropped{0};
    std::int64_t stale_dropped{0};
    std::int64_t reconnects{0};
    double capture_to_process_ms{0.0};
    double first_frame_ms{0.0};
    std::string last_plate;
    std::string last_plate_time;
};

struct SystemStatus {
    std::int64_t mem_total_mb{0};
    std::int64_t mem_available_mb{0};
    std::int64_t process_rss_mb{0};
    /// cgroup memory usage of the container, -1 when unknown.
    std::int64_t container_mem_mb{-1};
    double load1{0.0};
    /// Whole-system CPU use since the previous sample, -1 when unknown.
    double cpu_percent{-1.0};
    /// Jetson GPU load (0..100), -1 when unknown.
    double gpu_percent{-1.0};
    std::string detector_backend;
    std::string ocr_backend;
};

struct StatusSnapshot {
    std::string updated_at;
    std::int64_t updated_unix_ms{0};
    int pid{0};
    /// "eth0 192.168.10.5/24" or the error code.
    std::string camera_lan;
    /// "wwan0 ONLINE" style summary.
    std::string internet;
    std::vector<CameraStatus> cameras;
    SystemStatus system;
};

std::string toJson(const StatusSnapshot& snapshot);
bool fromJson(const std::string& text, StatusSnapshot& snapshot, std::string& error);

/// Atomic write (temporary file + rename), creating parent directories.
bool writeStatusFile(const std::string& path, const StatusSnapshot& snapshot, std::string& error);
bool readStatusFile(const std::string& path, StatusSnapshot& snapshot, std::string& error);

/// The concise table:
///   CAMERA     IP             LINK  RTSP  VIDEO  ANPR     ERROR
///   camera-01  192.168.10.21  OK    OK    H264   RUNNING
std::string formatStatusTable(const StatusSnapshot& snapshot);

}  // namespace anpr::cameras
