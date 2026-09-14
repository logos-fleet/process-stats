#ifndef PROCESS_STATS_H
#define PROCESS_STATS_H

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

namespace ProcessStats {
    struct ProcessStatsData {
        double cpuPercent = 0.0;
        double cpuTimeSeconds = 0.0;
        double memoryMB = 0.0;
    };

    ProcessStatsData getProcessStats(int64_t pid);

    /// @param processes map of module name -> process ID
    /// @return JSON array string; caller must `delete[]` the pointer
    char* getModuleStats(const std::unordered_map<std::string, int64_t>& processes);

    void clearHistory();

    // ── Accounting for code that has NO process of its own ──────────────────
    //
    // Everything above reads a PROCESS. A module the host runs inside its own
    // image (liblogos' Native container, ADR 0003 — a Store app gets no
    // subprocess per module) has no pid to read, and asking for one produces
    // the zeroes that are indistinguishable from an idle process.
    //
    // What such a module actually IS in the host process is one dlopen'd image
    // and one worker thread. The two primitives below measure exactly those,
    // and neither of them names a process. They are deliberately NOT folded
    // into getModuleStats(): what a caller has to pass — an address inside the
    // image, a handle captured on the thread — is knowledge only the container
    // that loaded the module holds.

    /// One loaded image's footprint: a dylib, a framework, a .so, or the main
    /// executable.
    struct ImageStatsData {
        /// False when the address belongs to no image the loader knows, which
        /// is the only case in which the other fields mean nothing.
        bool resolved = false;
        /// Every byte of address space the loader reserved for the image — its
        /// Mach-O segments or its ELF PT_LOAD headers.
        std::uint64_t mappedBytes = 0;
        /// How much of that is in physical memory right now. Meaningful only
        /// when `residentKnown`; a platform that refuses to answer leaves this
        /// zero rather than guessing.
        std::uint64_t residentBytes = 0;
        bool residentKnown = false;
        /// The image's path as the loader knows it. Empty when unresolved.
        std::string path;
    };

    /// The image containing `addressInImage`. Pass any address inside it — a
    /// symbol it exports is the usual one, since a host that dlopen'd an image
    /// is holding several.
    ImageStatsData getImageStats(const void* addressInImage);

    /// One THREAD's CPU time, readable from any OTHER thread.
    ///
    /// MUST BE CAPTURED ON THE THREAD IT MEASURES. What it holds is a handle
    /// obtainable only from inside that thread — a mach port on Apple, a POSIX
    /// per-thread clock id on Linux, a thread id on Windows — so the shape of
    /// use is: capture in the thread's entry point, read wherever stats are
    /// asked for.
    ///
    /// IT GOES STALE WHEN THE THREAD EXITS, and its owner must stop reading it
    /// then: an exited thread's handle can be recycled, so a late read is not
    /// reliably an error, it can be another thread's time. The owner knows when
    /// the thread is joined; this class cannot.
    ///
    /// Trivially copyable, so it can be handed across a lock without lifetime
    /// questions.
    class ThreadCpuClock {
    public:
        ThreadCpuClock() = default;

        /// Capture the calling thread.
        static ThreadCpuClock forCurrentThread();

        bool valid() const { return m_valid; }

        /// Total user+system CPU this thread has consumed. nullopt when the
        /// clock was never captured, or when the platform refuses the read
        /// (which it does once the thread is gone, on the platforms that can
        /// tell).
        std::optional<double> cpuTimeSeconds() const;

    private:
        /// The platform handle, widened to one type so this class stays
        /// trivially copyable on every platform.
        std::uint64_t m_handle = 0;
        bool m_valid = false;
    };
}

#endif
