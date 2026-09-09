#include "anpr/common/metrics.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace anpr {

void LatencyStat::add(double value_ms) {
    samples_[next_] = value_ms;
    next_ = (next_ + 1) % kCapacity;
    filled_ = std::min(filled_ + 1, kCapacity);
    ++count_;
    sum_ms_ += value_ms;
    max_ms_ = std::max(max_ms_, value_ms);
}

double LatencyStat::percentileMs(double fraction) const {
    if (filled_ == 0) {
        return 0.0;
    }
    std::vector<double> ordered(samples_.begin(), samples_.begin() + static_cast<std::ptrdiff_t>(filled_));
    std::sort(ordered.begin(), ordered.end());
    const double clamped = std::clamp(fraction, 0.0, 1.0);
    const auto index =
        static_cast<std::size_t>(clamped * static_cast<double>(ordered.size() - 1));
    return ordered[index];
}

void LatencyStat::reset() {
    samples_.fill(0.0);
    filled_ = 0;
    next_ = 0;
    count_ = 0;
    sum_ms_ = 0.0;
    max_ms_ = 0.0;
}

std::string PipelineMetrics::summary() const {
    std::ostringstream out;
    out << std::fixed << std::setprecision(3);
    auto stat = [&out](const char* name, const LatencyStat& value) {
        if (value.count() == 0) {
            return;
        }
        out << ' ' << name << "_calls=" << value.count() << ' ' << name
            << "_avg_ms=" << value.avgMs() << ' ' << name << "_p95_ms=" << value.percentileMs(0.95)
            << ' ' << name << "_max_ms=" << value.maxMs();
    };

    out << "frames_captured=" << frames_captured << " frames_processed=" << frames_processed
        << " frames_dropped=" << frames_dropped << " detections=" << detections
        << " ocr_empty=" << ocr_empty << " crops_rejected_quality=" << crops_rejected_quality
        << " crops_rejected_size=" << crops_rejected_size
        << " crops_rejected_roi=" << crops_rejected_roi
        << " observations_invalid_format=" << observations_invalid_format
        << " recognition_sessions=" << recognition_sessions
        << " plates_confirmed=" << plates_confirmed
        << " recognition_timeouts=" << recognition_timeouts
        << " camera_reconnects=" << camera_reconnects;

    stat("capture_latency", capture_latency);
    stat("motion", motion_latency);
    stat("detector_preprocess", detector_preprocess);
    stat("detector_inference", detector_inference);
    stat("detector", detector_total);
    stat("ocr_preprocess", ocr_preprocess);
    stat("ocr_inference", ocr_inference);
    stat("ocr", ocr_total);
    stat("quality", quality_latency);
    stat("frame", frame_total);
    stat("recognition", recognition_latency);
    return out.str();
}

}  // namespace anpr
