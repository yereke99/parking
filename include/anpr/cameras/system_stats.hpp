#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace anpr::cameras {

/// Jetson-wide numbers for the periodic summary and the OUT_OF_MEMORY_RISK check. All read from
/// procfs/sysfs; anything unavailable (a non-Jetson host, a restricted container) stays unset.
struct SystemStats {
    std::int64_t mem_total_kb{0};
    std::int64_t mem_available_kb{0};
    std::int64_t process_rss_kb{0};
    std::optional<std::int64_t> cgroup_usage_bytes;
    std::optional<std::int64_t> cgroup_limit_bytes;
    double load1{0.0};
    double load5{0.0};
    double load15{0.0};
    /// Whole-system CPU use since the previous `read`, 0..100.
    std::optional<double> cpu_percent;
    /// Jetson integrated GPU load, 0..100 (/sys/devices/gpu.0/load is in tenths of a percent).
    std::optional<double> gpu_percent;
};

struct CpuTimes {
    std::uint64_t idle{0};
    std::uint64_t total{0};
};

/// MemTotal / MemAvailable in kB from /proc/meminfo text; false when either is missing.
bool parseMeminfo(const std::string& text, std::int64_t& total_kb, std::int64_t& available_kb);
/// VmRSS in kB from /proc/self/status text, -1 when missing.
std::int64_t parseVmRssKb(const std::string& text);
/// The aggregate "cpu" line of /proc/stat.
bool parseProcStatCpu(const std::string& text, CpuTimes& times);

class SystemStatsReader {
public:
    explicit SystemStatsReader(std::string proc_root = "/proc", std::string sys_root = "/sys");
    SystemStats read();

private:
    std::string proc_root_;
    std::string sys_root_;
    std::optional<CpuTimes> previous_;
};

}  // namespace anpr::cameras
