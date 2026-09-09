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

<!-- fleet ci probe -->
