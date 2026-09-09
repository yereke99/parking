#pragma once

#include <cstdint>
#include <mutex>
#include <sstream>
#include <string>

namespace anpr {

enum class LogLevel { kError = 0, kWarn = 1, kInfo = 2, kDebug = 3 };

std::string toString(LogLevel level);
LogLevel logLevelFromString(const std::string& text, LogLevel fallback);

/// Structured single-line logger. One event per line, `key=value` fields, monotonic timestamp.
/// Events are named, never free text, so downstream log processing stays stable.
/// Nothing here is called per frame; the pipeline logs state transitions and recognition steps.
class Logger {
public:
    static Logger& instance();

    void setLevel(LogLevel level) { level_ = level; }
    [[nodiscard]] LogLevel level() const { return level_; }
    [[nodiscard]] bool enabled(LogLevel level) const { return level <= level_; }

    /// Writes `ts_ms=... level=... event=<event> <fields>`. `fields` is already formatted.
    void log(LogLevel level, const std::string& event, const std::string& fields);

private:
    Logger() = default;
    LogLevel level_{LogLevel::kInfo};
    std::mutex mutex_;
};

/// Accumulates `key=value` pairs without touching the log stream until the event is emitted.
class LogFields {
public:
    template <typename T>
    LogFields& add(const char* key, const T& value) {
        if (!stream_.str().empty()) {
            stream_ << ' ';
        }
        stream_ << key << '=' << value;
        return *this;
    }

    [[nodiscard]] std::string str() const { return stream_.str(); }

private:
    std::ostringstream stream_;
};

void logEvent(LogLevel level, const std::string& event, const LogFields& fields);
void logEvent(LogLevel level, const std::string& event);

}  // namespace anpr
