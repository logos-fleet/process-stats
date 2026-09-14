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
#include <mach/task_info.h>
#include <sys/sysctl.h>
#elif defined(__linux__)
#include <sys/resource.h>
#include <sys/times.h>
#include <fstream>
#include <sstream>
#elif defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#endif

// The in-process primitives below need a DIFFERENT set of headers, and the
// difference is the point of this file's iOS split: reading THIS process's own
// images and threads is allowed everywhere, including iOS, where reading
// another process is not. So these are included on all of Apple, not just
// macOS.
#if defined(__APPLE__)
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>
#elif defined(__linux__)
#include <dlfcn.h>
#include <link.h>
#include <pthread.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <vector>
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

// ── Accounting for code that has no process of its own ──────────────────────

namespace {

#if defined(__APPLE__) || defined(__linux__)

// mincore's residency vector is `char*` on Darwin and `unsigned char*` on
// Linux, and nothing else about the call differs.
#if defined(__APPLE__)
using MincoreVec = char;
#else
using MincoreVec = unsigned char;
#endif

// How many bytes of [addr, addr+length) are in physical memory, or nullopt if
// the kernel will not say. A refusal is reported rather than counted as zero:
// "we could not look" and "nothing is resident" are different answers and only
// one of them is a measurement.
std::optional<std::uint64_t> residentBytesIn(const void* addr, std::uint64_t length)
{
    const long pageSize = ::sysconf(_SC_PAGESIZE);
    if (pageSize <= 0 || length == 0)
        return std::nullopt;

    // mincore needs a page-aligned start; round the start down and the length
    // up so the whole requested range is still covered.
    const std::uintptr_t start = reinterpret_cast<std::uintptr_t>(addr);
    const std::uintptr_t aligned = start & ~static_cast<std::uintptr_t>(pageSize - 1);
    const std::uint64_t span = length + (start - aligned);
    const std::uint64_t pages = (span + pageSize - 1) / pageSize;

    std::vector<MincoreVec> vec(static_cast<std::size_t>(pages), 0);
    if (::mincore(reinterpret_cast<void*>(aligned),
                  static_cast<std::size_t>(pages * pageSize),
                  vec.data()) != 0)
        return std::nullopt;

    std::uint64_t resident = 0;
    for (MincoreVec entry : vec) {
        if (entry & 1)  // MINCORE_INCORE on Darwin, the same bit on Linux
            resident += static_cast<std::uint64_t>(pageSize);
    }
    return resident;
}

#endif // __APPLE__ || __linux__

} // namespace

ImageStatsData getImageStats(const void* addressInImage)
{
    ImageStatsData stats{};
    if (!addressInImage)
        return stats;

#if defined(__APPLE__)
    Dl_info info{};
    // dladdr resolves the address to the image the dynamic loader has mapped it
    // from, which is exactly the question: for a dlopen'd Bare module the
    // caller holds its ABI entry points and nothing else.
    if (::dladdr(addressInImage, &info) == 0 || info.dli_fbase == nullptr)
        return stats;

    const auto* header = static_cast<const mach_header*>(info.dli_fbase);
    if (header->magic != MH_MAGIC_64 && header->magic != MH_MAGIC)
        return stats;
    const bool is64 = header->magic == MH_MAGIC_64;
    const char* commands = static_cast<const char*>(info.dli_fbase)
        + (is64 ? sizeof(mach_header_64) : sizeof(mach_header));

    // THE SLIDE, and why it has to be computed rather than assumed. A Mach-O
    // records the vmaddr it was LINKED at; dyld maps it wherever it likes and
    // the difference is the slide. A dylib is normally linked at 0, so the
    // slide is just the header's address — but the main executable is not, and
    // this function is documented to take any image. The linked-at base is the
    // vmaddr of the segment covering file offset 0, which is the one carrying
    // the header itself.
    std::uint64_t linkedBase = 0;
    bool haveBase = false;
    const load_command* lc = reinterpret_cast<const load_command*>(commands);
    for (std::uint32_t i = 0; i < header->ncmds && !haveBase; ++i) {
        if (lc->cmd == LC_SEGMENT_64) {
            const auto* seg = reinterpret_cast<const segment_command_64*>(lc);
            if (seg->fileoff == 0 && seg->filesize > 0) { linkedBase = seg->vmaddr; haveBase = true; }
        } else if (lc->cmd == LC_SEGMENT) {
            const auto* seg = reinterpret_cast<const segment_command*>(lc);
            if (seg->fileoff == 0 && seg->filesize > 0) { linkedBase = seg->vmaddr; haveBase = true; }
        }
        lc = reinterpret_cast<const load_command*>(
            reinterpret_cast<const char*>(lc) + lc->cmdsize);
    }
    if (!haveBase)
        return stats;
    const std::uint64_t slide =
        reinterpret_cast<std::uintptr_t>(info.dli_fbase) - linkedBase;

    bool residentKnown = false;
    lc = reinterpret_cast<const load_command*>(commands);
    for (std::uint32_t i = 0; i < header->ncmds; ++i) {
        std::uint64_t vmaddr = 0;
        std::uint64_t vmsize = 0;
        if (lc->cmd == LC_SEGMENT_64) {
            const auto* seg = reinterpret_cast<const segment_command_64*>(lc);
            vmaddr = seg->vmaddr;
            vmsize = seg->vmsize;
        } else if (lc->cmd == LC_SEGMENT) {
            const auto* seg = reinterpret_cast<const segment_command*>(lc);
            vmaddr = seg->vmaddr;
            vmsize = seg->vmsize;
        }
        // vmaddr 0 is __PAGEZERO: address space reserved to make a null
        // dereference fault, never mapped and never the image's cost.
        if (vmsize > 0 && vmaddr != 0) {
            stats.mappedBytes += vmsize;
            if (const std::optional<std::uint64_t> resident = residentBytesIn(
                    reinterpret_cast<const void*>(
                        static_cast<std::uintptr_t>(vmaddr + slide)), vmsize)) {
                stats.residentBytes += *resident;
                residentKnown = true;
            }
        }
        lc = reinterpret_cast<const load_command*>(
            reinterpret_cast<const char*>(lc) + lc->cmdsize);
    }

    stats.resolved = stats.mappedBytes > 0;
    stats.residentKnown = residentKnown;
    if (info.dli_fname)
        stats.path = info.dli_fname;
    if (!stats.resolved)
        stats.residentBytes = 0;
    return stats;

#elif defined(__linux__)
    Dl_info info{};
    ElfW(Sym)* symbol = nullptr;
    void* mapBase = nullptr;
    // dladdr1 with RTLD_DL_LINKMAP hands back the link_map, whose l_addr is the
    // load bias this image was mapped with — the ELF half of the Mach-O slide
    // above. Plain dladdr's dli_fbase is the same value, but only dladdr1 also
    // gives the entry to match against dl_iterate_phdr below.
    link_map* map = nullptr;
    if (::dladdr1(addressInImage, &info, reinterpret_cast<void**>(&map), RTLD_DL_LINKMAP) == 0
        || map == nullptr)
        return stats;
    (void)symbol;
    (void)mapBase;

    struct Walk {
        ElfW(Addr) wanted;
        std::uint64_t mapped;
        std::uint64_t resident;
        bool residentKnown;
        bool found;
    } walk{map->l_addr, 0, 0, false, false};

    ::dl_iterate_phdr(
        [](dl_phdr_info* phdr, std::size_t, void* data) -> int {
            auto* w = static_cast<Walk*>(data);
            if (phdr->dlpi_addr != w->wanted)
                return 0;
            w->found = true;
            for (int i = 0; i < phdr->dlpi_phnum; ++i) {
                const ElfW(Phdr)& h = phdr->dlpi_phdr[i];
                if (h.p_type != PT_LOAD || h.p_memsz == 0)
                    continue;
                w->mapped += h.p_memsz;
                const void* addr = reinterpret_cast<const void*>(
                    static_cast<std::uintptr_t>(phdr->dlpi_addr + h.p_vaddr));
                if (const std::optional<std::uint64_t> r = residentBytesIn(addr, h.p_memsz)) {
                    w->resident += *r;
                    w->residentKnown = true;
                }
            }
            return 1;  // stop: the image is found
        },
        &walk);

    if (!walk.found || walk.mapped == 0)
        return stats;
    stats.resolved = true;
    stats.mappedBytes = walk.mapped;
    stats.residentBytes = walk.resident;
    stats.residentKnown = walk.residentKnown;
    // l_name is empty for the main executable; dli_fname names it.
    stats.path = (map->l_name && *map->l_name) ? map->l_name
                                               : (info.dli_fname ? info.dli_fname : "");
    return stats;

#elif defined(_WIN32)
    // GetModuleHandleEx with FROM_ADDRESS is the Win32 spelling of dladdr: it
    // maps any address to the module mapped over it. MODULEINFO gives the size
    // of that mapping; Windows offers no per-module residency, so residentKnown
    // stays false rather than reporting the mapping as if it were resident.
    HMODULE module = nullptr;
    if (!::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                  | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              static_cast<LPCSTR>(addressInImage), &module)
        || module == nullptr)
        return stats;

    MODULEINFO info{};
    if (!::GetModuleInformation(::GetCurrentProcess(), module, &info, sizeof(info)))
        return stats;

    stats.resolved = info.SizeOfImage > 0;
    stats.mappedBytes = info.SizeOfImage;

    char path[MAX_PATH] = {};
    if (::GetModuleFileNameA(module, path, MAX_PATH) > 0)
        stats.path = path;
    return stats;

#else
    return stats;
#endif
}

ThreadCpuClock ThreadCpuClock::forCurrentThread()
{
    ThreadCpuClock clock;

#if defined(__APPLE__)
    // A mach thread port names the thread to thread_info(), which is what makes
    // the reading possible from another thread at all. Apple has no
    // pthread_getcpuclockid.
    clock.m_handle = static_cast<std::uint64_t>(::pthread_mach_thread_np(::pthread_self()));
    clock.m_valid = clock.m_handle != 0;

#elif defined(__linux__)
    clockid_t clockId{};
    if (::pthread_getcpuclockid(::pthread_self(), &clockId) == 0) {
        clock.m_handle = static_cast<std::uint64_t>(clockId);
        clock.m_valid = true;
    }

#elif defined(_WIN32)
    // The thread ID rather than a HANDLE, so this class stays trivially
    // copyable and owns nothing: the read below opens and closes its own
    // handle. An ID can be recycled after the thread exits, which is the same
    // caveat the header states for every platform.
    clock.m_handle = static_cast<std::uint64_t>(::GetCurrentThreadId());
    clock.m_valid = clock.m_handle != 0;
#endif

    return clock;
}

std::optional<double> ThreadCpuClock::cpuTimeSeconds() const
{
    if (!m_valid)
        return std::nullopt;

#if defined(__APPLE__)
    thread_basic_info_data_t info{};
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    const kern_return_t kr = ::thread_info(static_cast<thread_act_t>(m_handle),
                                           THREAD_BASIC_INFO,
                                           reinterpret_cast<thread_info_t>(&info),
                                           &count);
    if (kr != KERN_SUCCESS)
        return std::nullopt;
    return (info.user_time.seconds + info.user_time.microseconds / 1e6)
         + (info.system_time.seconds + info.system_time.microseconds / 1e6);

#elif defined(__linux__)
    struct timespec ts{};
    if (::clock_gettime(static_cast<clockid_t>(m_handle), &ts) != 0)
        return std::nullopt;
    return ts.tv_sec + ts.tv_nsec / 1e9;

#elif defined(_WIN32)
    const HANDLE thread = ::OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE,
                                       static_cast<DWORD>(m_handle));
    if (thread == nullptr)
        return std::nullopt;
    FILETIME creation{}, exit{}, kernel{}, user{};
    const BOOL ok = ::GetThreadTimes(thread, &creation, &exit, &kernel, &user);
    ::CloseHandle(thread);
    if (!ok)
        return std::nullopt;
    // FILETIME is a split 64-bit count of 100-nanosecond intervals.
    const auto toSeconds = [](const FILETIME& ft) {
        ULARGE_INTEGER u;
        u.LowPart = ft.dwLowDateTime;
        u.HighPart = ft.dwHighDateTime;
        return static_cast<double>(u.QuadPart) / 1e7;
    };
    return toSeconds(kernel) + toSeconds(user);

#else
    return std::nullopt;
#endif
}

}
