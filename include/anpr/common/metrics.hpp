#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace anpr {

/// Fixed-capacity latency accumulator. Percentiles come from the last `kCapacity` samples so the
/// structure never allocates after construction and never grows during 24/7 operation.
class LatencyStat {
public:
    static constexpr std::size_t kCapacity = 512;

    void add(double value_ms);

    [[nodiscard]] std::int64_t count() const { return count_; }
    [[nodiscard]] double sumMs() const { return sum_ms_; }
    [[nodiscard]] double avgMs() const {
        return count_ > 0 ? sum_ms_ / static_cast<double>(count_) : 0.0;
    }
    [[nodiscard]] double maxMs() const { return max_ms_; }
    [[nodiscard]] double percentileMs(double fraction) const;
    void reset();

private:
    std::array<double, kCapacity> samples_{};
    std::size_t filled_{0};
    std::size_t next_{0};
    std::int64_t count_{0};
    double sum_ms_{0.0};
    double max_ms_{0.0};
};

/// Scoped timer that feeds a LatencyStat on destruction.
class ScopedTimer {
public:
    explicit ScopedTimer(LatencyStat& stat)
        : stat_(stat), started_(std::chrono::steady_clock::now()) {}
    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;
    ScopedTimer(ScopedTimer&&) = delete;
    ScopedTimer& operator=(ScopedTimer&&) = delete;

    ~ScopedTimer() {
        const auto finished = std::chrono::steady_clock::now();
        stat_.add(std::chrono::duration<double, std::milli>(finished - started_).count());
    }

private:
    LatencyStat& stat_;
    std::chrono::steady_clock::time_point started_;
};

/// Counters and per-stage latencies for one pipeline run. Owned by the processing thread, so it
/// needs no synchronisation; the capture-side counters are copied in under the pump's own lock.
struct PipelineMetrics {
    LatencyStat capture_latency;      ///< frame capture timestamp to processing pick-up
    LatencyStat motion_latency;       ///< cheap ROI frame differencing
    LatencyStat detector_preprocess;  ///< letterbox plus tensor fill
    LatencyStat detector_inference;   ///< session run only
    LatencyStat detector_total;       ///< preprocess, inference, decode, NMS
    LatencyStat ocr_preprocess;
    LatencyStat ocr_inference;
    LatencyStat ocr_total;
    LatencyStat quality_latency;
    LatencyStat frame_total;             ///< whole processing tick
    LatencyStat recognition_latency;     ///< stop confirmed to event emitted

    std::int64_t frames_captured{0};
    std::int64_t frames_processed{0};
    std::int64_t frames_dropped{0};
    std::int64_t detector_calls{0};
    std::int64_t detections{0};
    std::int64_t ocr_calls{0};
    std::int64_t ocr_empty{0};
    std::int64_t crops_rejected_quality{0};
    std::int64_t crops_rejected_size{0};
    std::int64_t crops_rejected_roi{0};
    std::int64_t observations_invalid_format{0};
    std::int64_t recognition_sessions{0};
    std::int64_t plates_confirmed{0};
    std::int64_t recognition_timeouts{0};
    std::int64_t camera_reconnects{0};

    [[nodiscard]] std::string summary() const;
};

}  // namespace anpr
