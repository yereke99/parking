// Jetson-wide resource numbers from procfs/sysfs for the periodic summary and the
// OUT_OF_MEMORY_RISK check. Every source is optional: a missing file leaves its field unset.
#include "anpr/cameras/system_stats.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <utility>
#include <vector>

namespace anpr::cameras {
namespace {

/// cgroup v1 reports "no limit" as a huge page-aligned number (9223372036854771712); anything
/// at or above 2^60 bytes is not a real limit.
constexpr std::int64_t kAbsurdLimitBytes = std::int64_t{1} << 60;

std::optional<std::string> readText(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

std::string trim(const std::string& text) {
    const std::size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    const std::size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

bool parseInt64(const std::string& text, std::int64_t& out) {
    const std::string value = trim(text);
    if (value.empty()) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const long long number = std::strtoll(value.c_str(), &end, 10);
    if (errno != 0 || end == value.c_str() || *end != '\0') {
        return false;
    }
    out = static_cast<std::int64_t>(number);
    return true;
}

/// The number after `key` ("MemTotal:") on its own line, in whatever unit the file uses.
bool findKeyedNumber(const std::string& text, const std::string& key, std::int64_t& out) {
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        if (line.compare(0, key.size(), key) != 0) {
            continue;
        }
        std::istringstream rest(line.substr(key.size()));
        long long number = 0;
        if (rest >> number) {
            out = static_cast<std::int64_t>(number);
            return true;
        }
        return false;
    }
    return false;
}

std::optional<std::int64_t> readBytes(const std::string& path) {
    const std::optional<std::string> text = readText(path);
    std::int64_t value = 0;
    if (!text || !parseInt64(*text, value) || value < 0) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::int64_t> readLimit(const std::string& path) {
    const std::optional<std::string> text = readText(path);
    if (!text || trim(*text) == "max") {
        return std::nullopt;
    }
    std::int64_t value = 0;
    if (!parseInt64(*text, value) || value <= 0 || value >= kAbsurdLimitBytes) {
        return std::nullopt;
    }
    return value;
}

}  // namespace

bool parseMeminfo(const std::string& text, std::int64_t& total_kb, std::int64_t& available_kb) {
    std::int64_t total = 0;
    std::int64_t available = 0;
    if (!findKeyedNumber(text, "MemTotal:", total) ||
        !findKeyedNumber(text, "MemAvailable:", available)) {
        return false;
    }
    total_kb = total;
    available_kb = available;
    return true;
}

std::int64_t parseVmRssKb(const std::string& text) {
    std::int64_t rss = 0;
    return findKeyedNumber(text, "VmRSS:", rss) ? rss : -1;
}

bool parseProcStatCpu(const std::string& text, CpuTimes& times) {
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream columns(line);
        std::string label;
        if (!(columns >> label) || label != "cpu") {
            continue;
        }
        // user nice system idle iowait irq softirq steal [guest guest_nice]. guest time is
        // already counted in user/nice, so it is left out of the total.
        std::vector<std::uint64_t> values;
        unsigned long long value = 0;
        while (values.size() < 8 && columns >> value) {
            values.push_back(static_cast<std::uint64_t>(value));
        }
        if (values.size() < 4) {
            return false;
        }
        std::uint64_t total = 0;
        for (const std::uint64_t part : values) {
            total += part;
        }
        times.idle = values[3] + (values.size() > 4 ? values[4] : 0);
        times.total = total;
        return true;
    }
    return false;
}

SystemStatsReader::SystemStatsReader(std::string proc_root, std::string sys_root)
    : proc_root_(std::move(proc_root)), sys_root_(std::move(sys_root)) {}

SystemStats SystemStatsReader::read() {
    SystemStats stats;
    if (const auto meminfo = readText(proc_root_ + "/meminfo")) {
        parseMeminfo(*meminfo, stats.mem_total_kb, stats.mem_available_kb);
    }
    if (const auto status = readText(proc_root_ + "/self/status")) {
        stats.process_rss_kb = std::max<std::int64_t>(0, parseVmRssKb(*status));
    }
    if (const auto loadavg = readText(proc_root_ + "/loadavg")) {
        std::istringstream in(*loadavg);
        double one = 0.0;
        double five = 0.0;
        double fifteen = 0.0;
        if (in >> one >> five >> fifteen) {
            stats.load1 = one;
            stats.load5 = five;
            stats.load15 = fifteen;
        }
    }
    if (const auto stat_text = readText(proc_root_ + "/stat")) {
        CpuTimes now;
        if (parseProcStatCpu(*stat_text, now)) {
            if (previous_ && now.total > previous_->total) {
                // iowait may step backwards on tickless kernels; a negative idle delta is noise.
                const double total = static_cast<double>(now.total - previous_->total);
                const double idle = now.idle > previous_->idle
                                        ? static_cast<double>(now.idle - previous_->idle)
                                        : 0.0;
                stats.cpu_percent = std::min(100.0, std::max(0.0, 100.0 * (1.0 - idle / total)));
            }
            previous_ = now;
        }
    }

    // cgroup v1 (L4T R32 kernels) first, then the unified v2 hierarchy.
    const std::string cgroup = sys_root_ + "/fs/cgroup";
    if (const auto usage = readBytes(cgroup + "/memory/memory.usage_in_bytes")) {
        stats.cgroup_usage_bytes = usage;
        stats.cgroup_limit_bytes = readLimit(cgroup + "/memory/memory.limit_in_bytes");
    } else if (const auto current = readBytes(cgroup + "/memory.current")) {
        stats.cgroup_usage_bytes = current;
        stats.cgroup_limit_bytes = readLimit(cgroup + "/memory.max");
    }

    // The integrated GPU's load in tenths of a percent; the platform path is the Nano's gk20a.
    for (const char* path : {"/devices/gpu.0/load", "/devices/platform/host1x/57000000.gpu/load"}) {
        const std::optional<std::string> text = readText(sys_root_ + path);
        std::int64_t tenths = 0;
        if (text && parseInt64(*text, tenths) && tenths >= 0) {
            stats.gpu_percent = std::min(100.0, static_cast<double>(tenths) / 10.0);
            break;
        }
    }
    return stats;
}

}  // namespace anpr::cameras
