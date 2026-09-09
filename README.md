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
