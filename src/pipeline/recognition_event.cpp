#include "anpr/pipeline/recognition_event.hpp"

#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>

#include "anpr/common/logging.hpp"
#include "anpr/pipeline/plate_sink.hpp"

namespace anpr {
namespace {

std::string escapeJson(const std::string& value) {
    std::ostringstream out;
    for (const char ch : value) {
        switch (ch) {
            case '\\':
                out << "\\\\";
                break;
            case '"':
                out << "\\\"";
                break;
            case '\n':
                out << "\\n";
                break;
            case '\r':
                out << "\\r";
                break;
            case '\t':
                out << "\\t";
                break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<int>(static_cast<unsigned char>(ch)) << std::dec
                        << std::setfill(' ');
                } else {
                    out << ch;
                }
                break;
        }
    }
    return out.str();
}

void writeOptional(std::ostringstream& out, const char* key,
                   const std::optional<std::string>& value) {
    out << '"' << key << "\":";
    if (value) {
        out << '"' << escapeJson(*value) << '"';
    } else {
        out << "null";
    }
}

/// ISO 8601 in UTC with milliseconds, for example 2026-10-08T07:12:03.120Z.
std::string formatUtc(std::int64_t unix_time_ms) {
    std::int64_t seconds = unix_time_ms / 1000;
    std::int64_t millis = unix_time_ms % 1000;
    if (millis < 0) {
        millis += 1000;
        --seconds;
    }
    const auto time = static_cast<std::time_t>(seconds);
    std::tm utc{};
    gmtime_r(&time, &utc);
    std::ostringstream out;
    out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.' << std::setw(3) << std::setfill('0')
        << millis << 'Z';
    return out.str();
}

}  // namespace

std::string toString(RecognitionStatus status) {
    switch (status) {
        case RecognitionStatus::kValidHighConfidence:
            return "VALID_HIGH_CONFIDENCE";
        case RecognitionStatus::kValidLowConfidence:
            return "VALID_LOW_CONFIDENCE";
        case RecognitionStatus::kInvalidFormat:
            return "INVALID_FORMAT";
        case RecognitionStatus::kInsufficientImageQuality:
            return "INSUFFICIENT_IMAGE_QUALITY";
        case RecognitionStatus::kNoPlate:
            return "NO_PLATE";
        case RecognitionStatus::kAmbiguous:
            return "AMBIGUOUS";
        case RecognitionStatus::kLowConfidence:
            return "LOW_CONFIDENCE";
        case RecognitionStatus::kTimeout:
            return "TIMEOUT";
    }
    return "NO_PLATE";
}

RecognitionStatus toRecognitionStatus(PlateValidationStatus status) {
    switch (status) {
        case PlateValidationStatus::kValidHighConfidence:
            return RecognitionStatus::kValidHighConfidence;
        case PlateValidationStatus::kValidLowConfidence:
            return RecognitionStatus::kValidLowConfidence;
        case PlateValidationStatus::kAmbiguous:
            return RecognitionStatus::kAmbiguous;
        case PlateValidationStatus::kInvalidFormat:
            return RecognitionStatus::kInvalidFormat;
    }
    return RecognitionStatus::kInvalidFormat;
}

bool isAccepted(RecognitionStatus status) {
    return status == RecognitionStatus::kValidHighConfidence ||
           status == RecognitionStatus::kValidLowConfidence;
}

std::string toJson(const PlateRecognitionEvent& event) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(4);
    out << "{\"event\":\"plate_recognition\"";
    out << ",\"status\":\"" << toString(event.status) << '"';
    out << ",\"normalized_plate\":\"" << escapeJson(event.normalized_plate) << '"';
    out << ",\"raw_plate\":\"" << escapeJson(event.raw_plate) << '"';
    out << ",\"confidence\":" << event.confidence;
    out << ",\"timestamp_ms\":" << event.timestamp_ms;
    out << ",\"time\":\"" << formatUtc(event.unix_time_ms) << '"';
    out << ",\"camera_id\":\"" << escapeJson(event.camera_id) << '"';
    out << ",\"plate_box\":{\"x\":" << event.plate_box.x << ",\"y\":" << event.plate_box.y
        << ",\"width\":" << event.plate_box.width << ",\"height\":" << event.plate_box.height << '}';
    out << ',';
    writeOptional(out, "region_code", event.region_code);
    out << ',';
    writeOptional(out, "region_name", event.region_name);
    out << ",\"format\":\"" << escapeJson(event.format_name) << '"';
    out << ",\"recognition_latency_ms\":" << event.recognition_latency_ms;
    out << ",\"observation_count\":" << event.observation_count;
    out << ",\"agreeing_observations\":" << event.agreeing_observations;
    out << ',';
    writeOptional(out, "best_crop_path", event.best_crop_path);
    if (event.snapshot_path) {
        out << ',';
        writeOptional(out, "snapshot_path", event.snapshot_path);
        if (event.snapshot_timestamp_ms) {
            out << ",\"snapshot_timestamp_ms\":" << *event.snapshot_timestamp_ms;
        }
    }
    out << '}';
    return out.str();
}

void JsonStdoutSink::onRecognition(const PlateRecognitionEvent& event) {
    onRawEvent(toJson(event));
}

void JsonStdoutSink::onRawEvent(const std::string& json_line) {
    std::cout << json_line << '\n';
    std::cout.flush();
}

JsonLinesFileSink::JsonLinesFileSink(const std::string& path)
    : path_(path), out_(path, std::ios::app) {}

void JsonLinesFileSink::onRecognition(const PlateRecognitionEvent& event) {
    writeLine(toJson(event));
}

void JsonLinesFileSink::onRawEvent(const std::string& json_line) {
    writeLine(json_line);
}

void JsonLinesFileSink::writeLine(const std::string& line) {
    out_ << line << '\n';
    out_.flush();
    if (!out_) {
        logEvent(LogLevel::kError, "events_file_write_failed", LogFields().add("path", path_));
        out_.clear();
    }
}

void FanOutSink::onRecognition(const PlateRecognitionEvent& event) {
    for (const auto& sink : sinks_) {
        if (sink) {
            sink->onRecognition(event);
        }
    }
}

void FanOutSink::onRawEvent(const std::string& json_line) {
    for (const auto& sink : sinks_) {
        if (sink) {
            sink->onRawEvent(json_line);
        }
    }
}

void SynchronizedSink::onRecognition(const PlateRecognitionEvent& event) {
    const std::lock_guard<std::mutex> guard(mutex_);
    if (inner_) {
        inner_->onRecognition(event);
    }
}

void SynchronizedSink::onRawEvent(const std::string& json_line) {
    const std::lock_guard<std::mutex> guard(mutex_);
    if (inner_) {
        inner_->onRawEvent(json_line);
    }
}

}  // namespace anpr
