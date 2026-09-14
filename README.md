# Process Stats Library

A cross-platform C++17 library for monitoring process CPU and memory statistics. Uses **nlohmann/json** for JSON output (no Qt).

## Building

### With Nix

```bash
nix build
```

### With CMake

Requires **nlohmann_json** on the CMake search path (e.g. from your distro or Nix shell).

```bash
mkdir build && cd build
cmake .. -GNinja
ninja
```

## Running Tests

```bash
# With Nix
nix build .#process-stats-tests
./result/bin/process_stats_tests

# With CMake
cd build
ninja process_stats_tests
./bin/process_stats_tests
```

## API

```cpp
#include <process_stats/process_stats.h>
#include <unordered_map>
#include <string>
#include <cstdint>

// Get stats for a single process
ProcessStats::ProcessStatsData stats = ProcessStats::getProcessStats(static_cast<int64_t>(pid));
// stats.cpuPercent - CPU usage percentage
// stats.cpuTimeSeconds - Total CPU time in seconds
// stats.memoryMB - Memory usage in megabytes

// Get stats for multiple processes as JSON
std::unordered_map<std::string, int64_t> processes;
processes["my_process"] = static_cast<int64_t>(pid);
char* json = ProcessStats::getModuleStats(processes);
// Returns: [{"name":"my_process","pid":1234,"cpu_percent":1.5,"cpu_time_seconds":10.2,"memory_mb":45.3}]
delete[] json;

// Clear internal CPU time history (useful for tests)
ProcessStats::clearHistory();
```

## Output format

`ProcessStats::getModuleStats()` returns a JSON array with one entry per process in the input map (entries with an invalid PID, i.e. `pid <= 0`, are skipped). Each entry has the following fields:

- `name` — string. The process name, taken from the key in the input map.
- `pid` — integer. The process ID, taken from the value in the input map.
- `cpu_percent` — number. CPU usage percentage since the previous sample for this PID (`0.0` on the first sample).
- `cpu_time_seconds` — number. Total CPU time consumed by the process, in seconds.
- `memory_mb` — number. Memory usage of the process, in megabytes.

Example with a single entry:

```json
[
  {
    "name": "my_process",
    "pid": 1234,
    "cpu_percent": 1.5,
    "cpu_time_seconds": 10.2,
    "memory_mb": 45.3
  }
]
```

## Measuring code that has no process

`getProcessStats` and `getModuleStats` read a **process**. Code the host runs
inside its own image — a module in Logos' Native container, which is the only
arrangement a phone Store app allows — has no pid, so asking them about it
produces zeroes that are indistinguishable from an idle process.

Two primitives measure such code directly. Neither names a process, and neither
is folded into `getModuleStats`: what they need (an address inside the image, a
handle captured on the thread) is knowledge only the loader holds.

```cpp
// The image a dlopen'd module was mapped from. Pass any address inside it —
// one of the symbols the loader resolved out of it will do.
ProcessStats::ImageStatsData image = ProcessStats::getImageStats(someSymbolInIt);
// image.resolved       — false if the address is in no known image
// image.mappedBytes    — address space the loader reserved for it
// image.residentBytes  — how much of that is in physical memory
// image.residentKnown  — false where the OS will not answer (then treat
//                        residentBytes as absent, not as zero)
// image.path           — the image's path on disk

// One thread's CPU, readable from any other thread. CAPTURE IT ON THE THREAD
// IT MEASURES: the handle is only obtainable from inside.
ProcessStats::ThreadCpuClock clock = ProcessStats::ThreadCpuClock::forCurrentThread();
// ... later, from the thread collecting stats:
std::optional<double> seconds = clock.cpuTimeSeconds();
```

A `ThreadCpuClock` goes stale when its thread exits: an exited thread's handle
can be recycled, so a late read is not reliably an error. Stop reading it when
you join the thread.

Platform support: `residentBytes` comes from `mincore` on Apple and Linux;
Windows reports `mappedBytes` only and leaves `residentKnown` false. Thread CPU
uses the mach thread port on Apple (there is no `pthread_getcpuclockid` there),
`pthread_getcpuclockid` on Linux, and `GetThreadTimes` on Windows.
