#pragma once

#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "anpr/pipeline/recognition_event.hpp"

namespace anpr {

/// Delivery point for finished recognition sessions.
///
/// This is the seam where the barrier is wired in. Implement it to call a database, post to an
/// HTTP API, toggle a GPIO relay or hand the plate to an access-control service. The pipeline
/// knows nothing about any of those: it produces `PlateRecognitionEvent` and hands it over.
///
/// Called from the processing thread (one per camera in camera mode, serialized by
/// SynchronizedSink). An implementation that can block, such as a network call,
/// must hand the event to its own queue and return promptly, otherwise it adds latency to the
/// next vehicle.
class PlateSink {
public:
    virtual ~PlateSink() = default;

    /// Every finished session, accepted or not. Check `event.status` before opening a barrier.
    virtual void onRecognition(const PlateRecognitionEvent& event) = 0;

    /// Another kind of event, already serialised as one JSON object (for example
    /// `hikvision_anpr`, the camera's own plate reading). Sinks that write JSON lines write it
    /// unchanged; others ignore it.
    virtual void onRawEvent(const std::string& json_line) { (void)json_line; }
};

/// Writes one JSON object per event to stdout. The default sink.
class JsonStdoutSink final : public PlateSink {
public:
    void onRecognition(const PlateRecognitionEvent& event) override;
    void onRawEvent(const std::string& json_line) override;
};

/// Appends one JSON object per event to a file and flushes it, so another process can follow
/// the file (`tail -F`) and nothing written is lost when the process stops. The file is opened
/// in append mode, so `logrotate` with `copytruncate` can rotate it under a running process.
/// A failed write (a full disk) is logged and the next event is tried again; it never stops
/// the pipeline.
class JsonLinesFileSink final : public PlateSink {
public:
    explicit JsonLinesFileSink(const std::string& path);
    [[nodiscard]] bool ok() const { return static_cast<bool>(out_); }
    void onRecognition(const PlateRecognitionEvent& event) override;
    void onRawEvent(const std::string& json_line) override;

private:
    std::string path_;
    std::ofstream out_;

    void writeLine(const std::string& line);
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
    void onRawEvent(const std::string& json_line) override;

private:
    std::vector<std::shared_ptr<PlateSink>> sinks_;
};

/// Serializes calls from several processing threads (one per camera) into a sink that is not
/// thread-safe, so JSON lines never interleave.
class SynchronizedSink final : public PlateSink {
public:
    explicit SynchronizedSink(std::shared_ptr<PlateSink> inner) : inner_(std::move(inner)) {}
    void onRecognition(const PlateRecognitionEvent& event) override;
    void onRawEvent(const std::string& json_line) override;

private:
    std::shared_ptr<PlateSink> inner_;
    std::mutex mutex_;
};

}  // namespace anpr
