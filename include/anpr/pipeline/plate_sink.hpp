#pragma once

#include <memory>
#include <vector>

#include "anpr/pipeline/recognition_event.hpp"

namespace anpr {

/// Delivery point for finished recognition sessions.
///
/// This is the seam where the barrier is wired in. Implement it to call a database, post to an
/// HTTP API, toggle a GPIO relay or hand the plate to an access-control service. The pipeline
/// knows nothing about any of those: it produces `PlateRecognitionEvent` and hands it over.
///
/// Called from the processing thread. An implementation that can block, such as a network call,
/// must hand the event to its own queue and return promptly, otherwise it adds latency to the
/// next vehicle.
class PlateSink {
public:
    virtual ~PlateSink() = default;

    /// Every finished session, accepted or not. Check `event.status` before opening a barrier.
    virtual void onRecognition(const PlateRecognitionEvent& event) = 0;
};

/// Writes one JSON object per event to stdout. The default sink.
class JsonStdoutSink final : public PlateSink {
public:
    void onRecognition(const PlateRecognitionEvent& event) override;
};

/// Keeps events in memory. Used by tests and the benchmark tool.
class CollectingSink final : public PlateSink {
public:
    void onRecognition(const PlateRecognitionEvent& event) override { events_.push_back(event); }
    [[nodiscard]] const std::vector<PlateRecognitionEvent>& events() const { return events_; }

private:
    std::vector<PlateRecognitionEvent> events_;
};

/// Forwards each event to several sinks in order.
class FanOutSink final : public PlateSink {
public:
    void add(std::shared_ptr<PlateSink> sink) { sinks_.push_back(std::move(sink)); }
    void onRecognition(const PlateRecognitionEvent& event) override;

private:
    std::vector<std::shared_ptr<PlateSink>> sinks_;
};

}  // namespace anpr
