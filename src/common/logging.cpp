#include "anpr/common/logging.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <iostream>

namespace anpr {
namespace {

std::int64_t monotonicMs() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

}  // namespace

std::string toString(LogLevel level) {
    switch (level) {
        case LogLevel::kError:
            return "error";
        case LogLevel::kWarn:
            return "warn";
        case LogLevel::kInfo:
            return "info";
        case LogLevel::kDebug:
            return "debug";
    }
    return "info";
}

LogLevel logLevelFromString(const std::string& text, LogLevel fallback) {
    std::string lowered = text;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (lowered == "error") {
        return LogLevel::kError;
    }
    if (lowered == "warn" || lowered == "warning") {
        return LogLevel::kWarn;
    }
    if (lowered == "info") {
        return LogLevel::kInfo;
    }
    if (lowered == "debug") {
        return LogLevel::kDebug;
    }
    return fallback;
}

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::log(LogLevel level, const std::string& event, const std::string& fields) {
    if (!enabled(level)) {
        return;
    }
    std::ostringstream line;
    line << "ts_ms=" << monotonicMs() << " level=" << toString(level) << " event=" << event;
    if (!fields.empty()) {
        line << ' ' << fields;
    }
    line << '\n';

    const std::lock_guard<std::mutex> guard(mutex_);
    // Diagnostics go to stderr, so stdout carries nothing but the JSON recognition events.
    std::cerr << line.str();
    std::cerr.flush();
}

void logEvent(LogLevel level, const std::string& event, const LogFields& fields) {
    Logger::instance().log(level, event, fields.str());
}

void logEvent(LogLevel level, const std::string& event) {
    Logger::instance().log(level, event, {});
}

}  // namespace anpr
