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

thread_local std::string t_log_context;

}  // namespace

std::string quoteLogValue(const std::string& value) {
    const bool needs_quotes =
        value.empty() || std::any_of(value.begin(), value.end(), [](unsigned char ch) {
            return std::isspace(ch) != 0 || ch == '"' || ch == '=' || ch == '\\';
        });
    if (!needs_quotes) {
        return value;
    }
    std::string quoted;
    quoted.reserve(value.size() + 2);
    quoted.push_back('"');
    for (const char ch : value) {
        if (ch == '"' || ch == '\\') {
            quoted.push_back('\\');
            quoted.push_back(ch);
        } else if (ch == '\n' || ch == '\r' || ch == '\t') {
            quoted.push_back(' ');
        } else {
            quoted.push_back(ch);
        }
    }
    quoted.push_back('"');
    return quoted;
}

LogFields& LogFields::addQuoted(const char* key, const std::string& value) {
    return add(key, quoteLogValue(value));
}

LogContext::LogContext(std::string fields) : previous_(t_log_context) {
    t_log_context = std::move(fields);
}

LogContext::~LogContext() {
    t_log_context = std::move(previous_);
}

const std::string& LogContext::current() {
    return t_log_context;
}

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
    if (!t_log_context.empty()) {
        line << ' ' << t_log_context;
    }
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
