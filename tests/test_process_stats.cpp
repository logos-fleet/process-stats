#include <gtest/gtest.h>
#include "process_stats.h"
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <unistd.h>
#include <sys/wait.h>
#include <atomic>
#include <optional>

class ProcessStatsTest : public ::testing::Test {
protected:
    std::vector<pid_t> m_childPids;

    void SetUp() override
    {
        ProcessStats::clearHistory();
    }

    void TearDown() override
    {
        ProcessStats::clearHistory();
        for (pid_t pid : m_childPids) {
            if (pid > 0) {
                kill(pid, SIGTERM);
                int st = 0;
                waitpid(pid, &st, 0);
            }
        }
        m_childPids.clear();
    }

    /// Spawn `sleep 10` child; returns -1 if fork fails. Caller should ASSERT_GT(pid, 0).
    pid_t spawnSleepChild()
    {
        pid_t pid = fork();
        if (pid < 0)
            return -1;
        if (pid == 0) {
            execlp("sleep", "sleep", "10", nullptr);
            std::perror("execlp sleep");
            _exit(127);
        }
        m_childPids.push_back(pid);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        return pid;
    }
};

TEST_F(ProcessStatsTest, GetProcessStats_ReturnsZeroedStatsForNegativePid)
{
    ProcessStats::ProcessStatsData stats = ProcessStats::getProcessStats(-1);

    EXPECT_EQ(stats.cpuPercent, 0.0);
    EXPECT_EQ(stats.cpuTimeSeconds, 0.0);
    EXPECT_EQ(stats.memoryMB, 0.0);
}

TEST_F(ProcessStatsTest, GetProcessStats_ReturnsZeroedStatsForZeroPid)
{
    ProcessStats::ProcessStatsData stats = ProcessStats::getProcessStats(0);

    EXPECT_EQ(stats.cpuPercent, 0.0);
    EXPECT_EQ(stats.cpuTimeSeconds, 0.0);
    EXPECT_EQ(stats.memoryMB, 0.0);
}

TEST_F(ProcessStatsTest, GetProcessStats_ReturnsValidStatsForCurrentProcess)
{
    int64_t currentPid = static_cast<int64_t>(getpid());

    ProcessStats::ProcessStatsData stats = ProcessStats::getProcessStats(currentPid);

    EXPECT_GT(stats.memoryMB, 0.0);
    EXPECT_GE(stats.cpuTimeSeconds, 0.0);
}

TEST_F(ProcessStatsTest, GetProcessStats_MemoryIsNonNegative)
{
    int64_t currentPid = static_cast<int64_t>(getpid());

    ProcessStats::ProcessStatsData stats = ProcessStats::getProcessStats(currentPid);

    EXPECT_GE(stats.memoryMB, 0.0);
}

TEST_F(ProcessStatsTest, GetProcessStats_CpuTimeIsNonNegative)
{
    int64_t currentPid = static_cast<int64_t>(getpid());

    ProcessStats::ProcessStatsData stats = ProcessStats::getProcessStats(currentPid);

    EXPECT_GE(stats.cpuTimeSeconds, 0.0);
}

TEST_F(ProcessStatsTest, GetProcessStats_CpuPercentIsZeroOnFirstCall)
{
    int64_t currentPid = static_cast<int64_t>(getpid());

    ProcessStats::clearHistory();

    ProcessStats::ProcessStatsData stats = ProcessStats::getProcessStats(currentPid);

    EXPECT_EQ(stats.cpuPercent, 0.0);
}

TEST_F(ProcessStatsTest, GetProcessStats_CpuPercentUpdatesOnSecondCall)
{
    int64_t currentPid = static_cast<int64_t>(getpid());

    ProcessStats::getProcessStats(currentPid);

    volatile double sum = 0.0;
    for (int i = 0; i < 1000000; ++i) {
        sum += i * 0.1;
    }

    usleep(10000);

    ProcessStats::ProcessStatsData stats = ProcessStats::getProcessStats(currentPid);

    EXPECT_GE(stats.cpuPercent, 0.0);
}

TEST_F(ProcessStatsTest, GetModuleStats_ReturnsEmptyArrayWhenNoPlugins)
{
    std::unordered_map<std::string, int64_t> emptyProcesses;
    char* result = ProcessStats::getModuleStats(emptyProcesses);

    ASSERT_NE(result, nullptr);

    nlohmann::json doc = nlohmann::json::parse(result);

    EXPECT_TRUE(doc.is_array());
    EXPECT_EQ(doc.size(), 0u);

    delete[] result;
}

TEST_F(ProcessStatsTest, GetModuleStats_ReturnsNonNullPointer)
{
    std::unordered_map<std::string, int64_t> emptyProcesses;
    char* result = ProcessStats::getModuleStats(emptyProcesses);

    ASSERT_NE(result, nullptr);

    delete[] result;
}

TEST_F(ProcessStatsTest, GetModuleStats_ReturnsValidJsonStructure)
{
    pid_t pid = spawnSleepChild();
    ASSERT_GT(pid, 0);

    std::unordered_map<std::string, int64_t> processes;
    processes["test_plugin"] = static_cast<int64_t>(pid);

    char* result = ProcessStats::getModuleStats(processes);

    ASSERT_NE(result, nullptr);

    nlohmann::json doc = nlohmann::json::parse(result);

    EXPECT_TRUE(doc.is_array());
    ASSERT_EQ(doc.size(), 1u);

    auto moduleObj = doc[0];
    EXPECT_TRUE(moduleObj.contains("name"));
    EXPECT_TRUE(moduleObj.contains("pid"));
    EXPECT_TRUE(moduleObj.contains("cpu_percent"));
    EXPECT_TRUE(moduleObj.contains("cpu_time_seconds"));
    EXPECT_TRUE(moduleObj.contains("memory_mb"));

    EXPECT_EQ(moduleObj["name"].get<std::string>(), "test_plugin");
    EXPECT_EQ(moduleObj["pid"].get<int64_t>(), static_cast<int64_t>(pid));
    EXPECT_GE(moduleObj["cpu_percent"].get<double>(), 0.0);
    EXPECT_GE(moduleObj["cpu_time_seconds"].get<double>(), 0.0);
    EXPECT_GE(moduleObj["memory_mb"].get<double>(), 0.0);

    delete[] result;
}

TEST_F(ProcessStatsTest, GetModuleStats_IncludesAllPassedProcesses)
{
    pid_t pid1 = spawnSleepChild();
    pid_t pid2 = spawnSleepChild();
    ASSERT_GT(pid1, 0);
    ASSERT_GT(pid2, 0);

    std::unordered_map<std::string, int64_t> processes;
    processes["plugin_one"] = static_cast<int64_t>(pid1);
    processes["plugin_two"] = static_cast<int64_t>(pid2);

    char* result = ProcessStats::getModuleStats(processes);

    ASSERT_NE(result, nullptr);

    nlohmann::json doc = nlohmann::json::parse(result);

    EXPECT_TRUE(doc.is_array());

    ASSERT_EQ(doc.size(), 2u);

    std::set<std::string> names;
    for (const auto& val : doc) {
        names.insert(val["name"].get<std::string>());
    }

    EXPECT_TRUE(names.count("plugin_one"));
    EXPECT_TRUE(names.count("plugin_two"));

    delete[] result;
}

TEST_F(ProcessStatsTest, GetModuleStats_SkipsInvalidPids)
{
    pid_t validPid = spawnSleepChild();
    ASSERT_GT(validPid, 0);

    std::unordered_map<std::string, int64_t> processes;
    processes["valid_plugin"] = static_cast<int64_t>(validPid);
    processes["invalid_plugin"] = -1;
    processes["zero_plugin"] = 0;

    char* result = ProcessStats::getModuleStats(processes);

    ASSERT_NE(result, nullptr);

    nlohmann::json doc = nlohmann::json::parse(result);

    EXPECT_TRUE(doc.is_array());

    ASSERT_EQ(doc.size(), 1u);

    EXPECT_EQ(doc[0]["name"].get<std::string>(), "valid_plugin");

    delete[] result;
}

// =============================================================================
// In-process accounting: measuring code that has NO pid
// =============================================================================
//
// A module in the Native container runs inside the host's own image and reports
// pid -1, so everything above reads nothing about it: getProcessStats reads a
// PROCESS and it is not one. These two primitives are what a container can use
// instead — the footprint of the IMAGE the module was dlopen'd from, and the
// CPU of the THREAD its handlers run on.

// An address that is certainly inside this test binary's own mapped image.
static void anAddressInThisImage() {}

TEST_F(ProcessStatsTest, GetImageStats_ResolvesTheImageAnAddressIsIn)
{
    ProcessStats::ImageStatsData stats =
        ProcessStats::getImageStats(reinterpret_cast<const void*>(&anAddressInThisImage));

    EXPECT_TRUE(stats.resolved);
    EXPECT_GT(stats.mappedBytes, 0u);
    EXPECT_FALSE(stats.path.empty());
}

TEST_F(ProcessStatsTest, GetImageStats_ResidentIsASubsetOfTheMapping)
{
    ProcessStats::ImageStatsData stats =
        ProcessStats::getImageStats(reinterpret_cast<const void*>(&anAddressInThisImage));

    ASSERT_TRUE(stats.resolved);
    if (!stats.residentKnown)
        GTEST_SKIP() << "this platform will not report residency";

    // The pages of this image that are in physical memory are a SUBSET of the
    // ones reserved for it; a larger figure would mean the walk had strayed
    // outside the image.
    EXPECT_LE(stats.residentBytes, stats.mappedBytes);
    // The function whose address was passed is in this image and this test is
    // running, so at least one page of it is resident.
    EXPECT_GT(stats.residentBytes, 0u);
}

TEST_F(ProcessStatsTest, GetImageStats_RefusesAnAddressInNoImage)
{
    ProcessStats::ImageStatsData stats =
        ProcessStats::getImageStats(reinterpret_cast<const void*>(0x1));

    EXPECT_FALSE(stats.resolved);
    EXPECT_EQ(stats.mappedBytes, 0u);
    EXPECT_EQ(stats.residentBytes, 0u);
}

TEST_F(ProcessStatsTest, GetImageStats_RefusesNull)
{
    ProcessStats::ImageStatsData stats = ProcessStats::getImageStats(nullptr);

    EXPECT_FALSE(stats.resolved);
}

TEST_F(ProcessStatsTest, ThreadCpuClock_IsInvalidUntilItIsCaptured)
{
    ProcessStats::ThreadCpuClock clock;

    EXPECT_FALSE(clock.valid());
    EXPECT_FALSE(clock.cpuTimeSeconds().has_value());
}

TEST_F(ProcessStatsTest, ThreadCpuClock_ReadsAnotherThreadsCpuTime)
{
    // The shape the Native container needs: the clock is captured ON the
    // module's worker thread and read from whichever thread asks for stats.
    std::atomic<bool> captured{false};
    std::atomic<bool> burned{false};
    std::atomic<bool> release{false};
    ProcessStats::ThreadCpuClock clock;

    std::thread worker([&] {
        clock = ProcessStats::ThreadCpuClock::forCurrentThread();
        captured = true;
        volatile double sum = 0.0;
        for (long i = 0; i < 40000000; ++i)
            sum += i * 0.5;
        burned = true;
        while (!release.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    });

    while (!captured.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    EXPECT_TRUE(clock.valid());

    while (!burned.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    const std::optional<double> seconds = clock.cpuTimeSeconds();
    release = true;
    worker.join();

    ASSERT_TRUE(seconds.has_value());
    EXPECT_GT(*seconds, 0.0);
}

TEST_F(ProcessStatsTest, ThreadCpuClock_MeasuresOnlyItsOwnThread)
{
    // Two threads, one busy: the idle one's clock must not pick up the busy
    // one's work. That is the whole point of measuring per-thread — a
    // per-PROCESS reading would give both the same number.
    std::atomic<bool> captured{false};
    std::atomic<bool> release{false};
    ProcessStats::ThreadCpuClock idleClock;

    std::thread idle([&] {
        idleClock = ProcessStats::ThreadCpuClock::forCurrentThread();
        captured = true;
        while (!release.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    });

    while (!captured.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    volatile double sum = 0.0;
    for (long i = 0; i < 60000000; ++i)
        sum += i * 0.5;

    const std::optional<double> idleSeconds = idleClock.cpuTimeSeconds();
    const std::optional<double> selfSeconds =
        ProcessStats::ThreadCpuClock::forCurrentThread().cpuTimeSeconds();
    release = true;
    idle.join();

    ASSERT_TRUE(idleSeconds.has_value());
    ASSERT_TRUE(selfSeconds.has_value());
    EXPECT_LT(*idleSeconds, *selfSeconds)
        << "an idle thread's clock picked up the busy thread's work";
}
