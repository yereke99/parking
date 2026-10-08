#pragma once

#include <cstdint>
#include <mutex>
#include <sstream>
#include <string>

namespace anpr {

enum class LogLevel { kError = 0, kWarn = 1, kInfo = 2, kDebug = 3 };

std::string toString(LogLevel level);
LogLevel logLevelFromString(const std::string& text, LogLevel fallback);

/// Structured single-line logger on stderr. One event per line, `key=value` fields, monotonic
/// timestamp. stdout is reserved for the JSON recognition events.
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

    /// Adds `key="value"` when the value is empty or contains spaces, quotes or `=`, so free text
    /// such as an operator action stays one field for log parsers. Plain tokens stay unquoted.
    LogFields& addQuoted(const char* key, const std::string& value);

    [[nodiscard]] std::string str() const { return stream_.str(); }

private:
    std::ostringstream stream_;
};

/// `value` as one log field value: quoted and escaped when it is empty or contains whitespace,
/// quotes or `=`, unchanged otherwise.
std::string quoteLogValue(const std::string& value);

/// Fields prepended to every log line written by the current thread while this object lives, for
/// example `camera_id=camera-02` in that camera's capture and processing threads. Nesting
/// restores the previous context. A thread without a context logs exactly as before.
class LogContext {
public:
    explicit LogContext(std::string fields);
    ~LogContext();
    LogContext(const LogContext&) = delete;
    LogContext& operator=(const LogContext&) = delete;

    /// The current thread's context, empty when none is set.
    static const std::string& current();

private:
    std::string previous_;
};

void logEvent(LogLevel level, const std::string& event, const LogFields& fields);
void logEvent(LogLevel level, const std::string& event);

}  // namespace anpr
