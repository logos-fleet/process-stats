#include "process_stats.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <unordered_set>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

// libproc and the mach task_info APIs below exist on macOS only; iOS sandboxes
// away every other process. TARGET_OS_IPHONE (from <TargetConditionals.h>, 0 on
// macOS, 1 on iOS and the simulator) is the SDK's own answer; no compiler or
// SDK defines an __IOS__ macro.
#if defined(__APPLE__) && !TARGET_OS_IPHONE
#include <libproc.h>
#include <mach/mach.h>
#include <mach/task_info.h>
#include <sys/sysctl.h>
#elif defined(__linux__)
#include <sys/resource.h>
#include <sys/times.h>
#include <unistd.h>
#include <fstream>
#include <sstream>
#elif defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#endif

namespace ProcessStats {

namespace {
    std::mutex s_cache_mutex;
    std::unordered_map<int64_t, std::pair<double, int64_t>> s_previous_cpu_times;
}

void clearHistory()
{
    std::lock_guard<std::mutex> lock(s_cache_mutex);
    s_previous_cpu_times.clear();
}

static int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

ProcessStatsData getProcessStats(int64_t pid)
{
    ProcessStatsData stats{};

    if (pid <= 0)
        return stats;

#if defined(__APPLE__) && !TARGET_OS_IPHONE
    struct proc_taskinfo taskInfo;
    int ret = proc_pidinfo(static_cast<int>(pid), PROC_PIDTASKINFO, 0, &taskInfo, sizeof(taskInfo));

    if (ret == sizeof(taskInfo)) {
        uint64_t totalTime = taskInfo.pti_total_user + taskInfo.pti_total_system;
        stats.cpuTimeSeconds = totalTime / 1e6;
        stats.memoryMB = taskInfo.pti_resident_size / (1024.0 * 1024.0);

        const int64_t currentTime = now_ms();
        {
            std::lock_guard<std::mutex> lock(s_cache_mutex);
            auto it = s_previous_cpu_times.find(pid);
            if (it != s_previous_cpu_times.end()) {
                const double timeDelta = (currentTime - it->second.second) / 1000.0;
                const double cpuDelta = stats.cpuTimeSeconds - it->second.first;
                if (timeDelta > 0)
                    stats.cpuPercent = (cpuDelta / timeDelta) * 100.0;
            }
            s_previous_cpu_times[pid] = {stats.cpuTimeSeconds, currentTime};
        }
    }

#elif defined(__linux__)
    const std::string statPath = "/proc/" + std::to_string(pid) + "/stat";
    const std::string statusPath = "/proc/" + std::to_string(pid) + "/status";

    std::ifstream statFile(statPath);
    if (statFile.is_open()) {
        std::string line;
        std::getline(statFile, line);
        std::istringstream iss(line);
        std::string token;
        for (int i = 0; i < 14 && iss >> token; ++i) {
        }
        unsigned long utime = 0, stime = 0;
        if (iss >> utime && iss >> stime) {
            const long clockTicks = sysconf(_SC_CLK_TCK);
            if (clockTicks > 0)
                stats.cpuTimeSeconds = (utime + stime) / static_cast<double>(clockTicks);
        }
    }

    std::ifstream statusFile(statusPath);
    if (statusFile.is_open()) {
        std::string line;
        while (std::getline(statusFile, line)) {
            if (line.rfind("VmRSS:", 0) == 0) {
                std::istringstream iss(line);
                std::string label, value, unit;
                iss >> label >> value >> unit;
                if (!value.empty()) {
                    const double memoryKB = std::stod(value);
                    stats.memoryMB = memoryKB / 1024.0;
                }
                break;
            }
        }
    }

    const int64_t currentTime = now_ms();
    {
        std::lock_guard<std::mutex> lock(s_cache_mutex);
        auto it = s_previous_cpu_times.find(pid);
        if (it != s_previous_cpu_times.end()) {
            const double timeDelta = (currentTime - it->second.second) / 1000.0;
            const double cpuDelta = stats.cpuTimeSeconds - it->second.first;
            if (timeDelta > 0)
                stats.cpuPercent = (cpuDelta / timeDelta) * 100.0;
        }
        s_previous_cpu_times[pid] = {stats.cpuTimeSeconds, currentTime};
    }

#elif defined(_WIN32)
    // PROCESS_QUERY_LIMITED_INFORMATION is the least privilege that reads
    // another same-user process's times and memory without demanding debug
    // rights; it also succeeds for elevated processes where the older
    // PROCESS_QUERY_INFORMATION would not.
    const HANDLE h = ::OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (h != nullptr) {
        FILETIME creation{}, exit{}, kernel{}, user{};
        if (::GetProcessTimes(h, &creation, &exit, &kernel, &user)) {
            // FILETIME is a split 64-bit count of 100-nanosecond intervals, so
            // recombine before scaling. Kernel+user matches what the Apple
            // branch sums (system+user) and what Linux reads as stime+utime.
            auto toSeconds = [](const FILETIME& ft) {
                ULARGE_INTEGER u;
                u.LowPart = ft.dwLowDateTime;
                u.HighPart = ft.dwHighDateTime;
                return static_cast<double>(u.QuadPart) / 1e7;
            };
            stats.cpuTimeSeconds = toSeconds(kernel) + toSeconds(user);
        }

        PROCESS_MEMORY_COUNTERS pmc{};
        if (::GetProcessMemoryInfo(h, &pmc, sizeof(pmc))) {
            // WorkingSetSize is the resident set: the Windows analogue of
            // Apple's pti_resident_size and Linux's VmRSS.
            stats.memoryMB = pmc.WorkingSetSize / (1024.0 * 1024.0);
        }
        ::CloseHandle(h);
    }

    const int64_t currentTime = now_ms();
    {
        std::lock_guard<std::mutex> lock(s_cache_mutex);
        auto it = s_previous_cpu_times.find(pid);
        if (it != s_previous_cpu_times.end()) {
            const double timeDelta = (currentTime - it->second.second) / 1000.0;
            const double cpuDelta = stats.cpuTimeSeconds - it->second.first;
            if (timeDelta > 0)
                stats.cpuPercent = (cpuDelta / timeDelta) * 100.0;
        }
        s_previous_cpu_times[pid] = {stats.cpuTimeSeconds, currentTime};
    }

#else
    std::fprintf(stderr, "process-stats: process monitoring not supported on this platform\n");
#endif

    return stats;
}

char* getModuleStats(const std::unordered_map<std::string, int64_t>& processes)
{
    nlohmann::json modulesArray = nlohmann::json::array();

    std::unordered_set<int64_t> activePids;
    for (const auto& e : processes)
        activePids.insert(e.second);

    {
        std::lock_guard<std::mutex> lock(s_cache_mutex);
        for (auto it = s_previous_cpu_times.begin(); it != s_previous_cpu_times.end();) {
            if (!activePids.count(it->first))
                it = s_previous_cpu_times.erase(it);
            else
                ++it;
        }
    }

    for (const auto& e : processes) {
        const std::string& pluginName = e.first;
        const int64_t pid = e.second;
        if (pid <= 0) {
            std::fprintf(stderr, "process-stats: invalid PID for plugin: %s\n", pluginName.c_str());
            continue;
        }
        ProcessStatsData st = getProcessStats(pid);
        nlohmann::json moduleObj;
        moduleObj["name"] = pluginName;
        moduleObj["pid"] = pid;
        moduleObj["cpu_percent"] = st.cpuPercent;
        moduleObj["cpu_time_seconds"] = st.cpuTimeSeconds;
        moduleObj["memory_mb"] = st.memoryMB;
        modulesArray.push_back(moduleObj);
    }

    const std::string jsonStr = modulesArray.dump();
    char* result = new char[jsonStr.size() + 1];
    std::memcpy(result, jsonStr.c_str(), jsonStr.size() + 1);
    return result;
}

}
