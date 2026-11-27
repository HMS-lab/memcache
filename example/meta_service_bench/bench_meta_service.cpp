/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * MemCache_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

// Benchmark: meta_service metadata processing performance under multi-process
// concurrent Put/Get pressure.
//
// Put = Alloc + Update (simulating MmcClientDefault::Put, BM data copy stubbed)
// Get = GetRequest (BM data copy stubbed)
// Both support single and batch variants. Batch size is randomly picked from
// a configurable [min, max] range per operation.
//
// MMC library logs are redirected to a file; only benchmark messages appear
// on screen.
//
// Usage:
//   bench_meta_service [options]
// Options:
//   -p <processes>    Client process count                    (default: 4)
//   -t <threads>      Threads per process                     (default: 4)
//   -n <ops>          Operations per thread                    (default: 5000)
//   -k <keyspace>     Key space size                          (default: 20000)
//   -w <warmup>       Warmup key count (Put pre-fill)         (default: 5000)
//   -s <blobsize>     Blob size in bytes                      (default: 32768)
//   -r <putPercent>   Put percentage (1-99)                    (default: 50)
//   -b <min,max>      Batch size range (comma-separated)      (default: 1,32)
//   -c <capacityMB>   Mock HBM capacity per rank in MB        (default: 16384)
//   -l <logFile>      Log file for MMC library output          (default: bench_meta_service.log)
//   --perf             Enable perf profiling of meta_service     (default: off)
//   --perf-freq <hz>   perf record sample frequency                (default: 99)
//   --perf-target <who>  Which processes perf records: meta|clients|client0 (default: meta)
//   --metrics-interval-ms <ms>  Sample /metrics every ms; 0 disables  (default: 1000)
//   -q, --queue-depth <q>  Logical operations in flight per thread    (default: 1)
//   --meta-numa <nodes>   NUMA nodes for meta_service: 5, 4,6, 4-7 or !5 (default: unbound)
//   --worker-numa <nodes> NUMA nodes for coordinator + clients, same format (default: unbound)
//   --numa-mem <policy>   Memory policy of NUMA binding: bind|preferred|none (default: bind)

#include <algorithm>
#include <array>
#include <atomic>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <locale>
#include <map>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <new>
#include <optional>
#include <random>
#include <sched.h>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "bench_async_transport.h"
#include "bench_latency.h"
#include "bench_net_worker_stats.h"
#include "bench_process_barrier.h"
#include "bench_queue_depth.h"
#include "mmc_blob_allocator.h"
#include "mmc_blob_state.h"
#include "mmc_def.h"
#include "mmc_logger.h"
#include "mmc_meta_net_client.h"
#include "mmc_meta_service.h"
#include "mmc_msg_client_meta.h"
#include "mmc_net_common.h"
#include "mmc_ptracer.h"
#include "mmc_ref.h"
#include "mmc_types.h"

using namespace std;
using namespace ock::mmc;

namespace {
constexpr int kDefaultProcessCount = 4;
constexpr int kDefaultThreadCount = 4;
constexpr int kDefaultOpsPerThread = 5000;
constexpr int kDefaultKeySpaceSize = 20000;
constexpr int kDefaultWarmupKeyCount = 5000;
constexpr uint64_t kDefaultBlobSize = SIZE_32K;
constexpr int kDefaultPutPercent = 50;
constexpr int kDefaultMinBatch = 1;
constexpr int kDefaultMaxBatch = 32;
constexpr uint64_t kDefaultCapacityMB = 16384;
constexpr const char *kDefaultLogFile = "bench_meta_service.log";

constexpr int kMetaServicePort = 16078;
constexpr int kConfigStorePort = 16081;
constexpr int kMetricsPort = 16180;
constexpr uint64_t kMockHbmGvaBase = 0x200000000ULL;
constexpr uint64_t kMockHbmGvaStride = 0x100000000ULL;
constexpr uint16_t kEvictThresholdHigh = 95;
constexpr uint16_t kEvictThresholdLow = 90;
constexpr int kLogRotationFileSize = 2 * 1024 * 1024;
constexpr int kLogRotationFileCount = 20;
constexpr const char *kPtracerDumpPath = "/var/log/memfabric_hybrid";
constexpr int kPtracerType = 1;
constexpr int kRpcTimeoutMs = 30000;
constexpr int kNetClientThreadCount = 2;
constexpr int kMetaServiceRankId = 0;
constexpr int kMicroSecPerSec = 1000000;
constexpr int kPercentileDivisor = 100;
constexpr int kTableWidth = 100;
constexpr int kMaxProcessCount = 32;
constexpr int kPerfReadyWaitSec = 1;
constexpr int kPerfStopTimeoutSec = 10;
constexpr int kPerfExecFailCode = 127;
constexpr int kDefaultPerfFreq = 99;
constexpr const char *kDefaultPerfDataFile = "perf.data";
constexpr int kDefaultMetricsIntervalMs = 1000;
constexpr int kMetricsConnectTimeoutMs = 500;
constexpr int kMetricsReceiveBufferSize = 16 * 1024;

constexpr uint64_t kBytesPerMB = 1024 * 1024;
constexpr int kIecUnitScale = 1024;
constexpr int kScaledMemoryPrecision = 2;

constexpr int kMaxNumaNodeCount = 1024;
constexpr int kDecimalBase = 10;
constexpr const char *kNumaSysfsNodePrefix = "/sys/devices/system/node/node";
constexpr const char *kNumaSysfsOnline = "/sys/devices/system/node/online";
// Linux mempolicy modes from <linux/mempolicy.h>, defined here so libnuma headers are not required.
constexpr int kMpolPreferred = 1;
constexpr int kMpolBind = 2;
constexpr const char *kMetaServiceProcName = "meta_service";
constexpr const char *kClientProcNamePrefix = "bench_client_";
constexpr const char *kCoordinatorProcName = "bench_coord";
constexpr int kChildStopTimeoutSec = 5;
constexpr int kChildPollIntervalMs = 50;
constexpr int kSignalExitCodeBase = 128;


int gTermFd = STDERR_FILENO;
volatile sig_atomic_t gStopSignal = 0;

void HandleStopSignal(int signalNumber)
{
    gStopSignal = signalNumber;
}

bool InstallStopHandlers()
{
    struct sigaction action{};
    action.sa_handler = HandleStopSignal;
    sigemptyset(&action.sa_mask);
    return sigaction(SIGINT, &action, nullptr) == 0 && sigaction(SIGTERM, &action, nullptr) == 0;
}

enum class NumaMemPolicy : int { kNone = 0, kPreferred, kBind };

enum class PerfTarget : int { kMeta = 0, kClients, kClient0 };

inline const char *PerfTargetName(PerfTarget target)
{
    switch (target) {
        case PerfTarget::kMeta:
            return "meta_service";
        case PerfTarget::kClients:
            return "all clients";
        case PerfTarget::kClient0:
            return "client 0";
        default:
            return "unknown";
    }
}

inline const char *NumaMemPolicyName(NumaMemPolicy policy)
{
    switch (policy) {
        case NumaMemPolicy::kNone:
            return "none";
        case NumaMemPolicy::kPreferred:
            return "preferred";
        case NumaMemPolicy::kBind:
            return "bind";
        default:
            return "unknown";
    }
}

enum OpType : int { kOpPut = 0, kOpGet, kOpAlloc, kOpUpdate, kOpTypeCount };

inline const char *OpTypeName(OpType t)
{
    switch (t) {
        case kOpPut:
            return "Put";
        case kOpGet:
            return "Get";
        case kOpAlloc: return "  Alloc";
        case kOpUpdate: return "  Update";
        default:
            return "Unknown";
    }
}

struct BenchConfig {
    int rpcTimeoutMs = kRpcTimeoutMs;
    int queueDepth = 1;
    bool waitCompletions = false;
    int processCount = kDefaultProcessCount;
    int threadCount = kDefaultThreadCount;
    int opsPerThread = kDefaultOpsPerThread;
    int keySpaceSize = kDefaultKeySpaceSize;
    int warmupKeyCount = kDefaultWarmupKeyCount;
    uint64_t blobSize = kDefaultBlobSize;
    int putPercent = kDefaultPutPercent;
    int minBatch = kDefaultMinBatch;
    int maxBatch = kDefaultMaxBatch;
    uint64_t capacityMB = kDefaultCapacityMB;
    std::string logFile = kDefaultLogFile;
    bool perfEnable = false;
    int profileControlFd = -1; // Internal runner handshake; not used for standalone runs.
    int perfFreq = kDefaultPerfFreq;
    int metricsIntervalMs = kDefaultMetricsIntervalMs;
    PerfTarget perfTarget = PerfTarget::kMeta;
    std::string metaNumaNodes;   // empty = unbound
    std::string workerNumaNodes; // empty = unbound
    NumaMemPolicy numaMemPolicy = NumaMemPolicy::kBind;
};

void TermLog(const char *fmt, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (len > 0) {
        if (write(gTermFd, buf, static_cast<size_t>(len)) < 0) {
            return;
        }
    }
}

void PrepareChildProcess(pid_t parentPid)
{
    struct sigaction action{};
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    // Children must not inherit the coordinator's cancellation-only handlers.
    if (sigaction(SIGINT, &action, nullptr) < 0 || sigaction(SIGTERM, &action, nullptr) < 0 ||
        prctl(PR_SET_PDEATHSIG, SIGKILL) < 0) {
        TermLog("Failed to configure child process cleanup: %s\n", strerror(errno));
        _exit(EXIT_FAILURE);
    }
    // The parent may have exited between fork() and prctl().
    if (getppid() != parentPid) {
        _exit(EXIT_FAILURE);
    }
}

bool ReapChildren(const std::vector<pid_t> &pids, int terminationSignal = 0,
                  int timeoutSeconds = kChildStopTimeoutSec)
{
    std::vector<pid_t> remaining = pids;
    bool success = true;
    bool stopping = terminationSignal != 0;
    if (stopping) {
        for (pid_t pid : remaining) {
            kill(pid, terminationSignal);
        }
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
    while (!remaining.empty()) {
        if (gStopSignal != 0 && !stopping) {
            for (pid_t pid : remaining) {
                kill(pid, SIGTERM);
            }
            stopping = true;
        }
        for (auto it = remaining.begin(); it != remaining.end();) {
            int status = 0;
            pid_t result = waitpid(*it, &status, WNOHANG);
            if (result == *it || (result < 0 && errno != EINTR)) {
                success = success && result > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
                it = remaining.erase(it);
            } else {
                ++it;
            }
        }
        if (remaining.empty()) {
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            TermLog("Child shutdown timed out; killing %zu remaining processes\n", remaining.size());
            for (pid_t pid : remaining) {
                kill(pid, SIGKILL);
            }
            for (pid_t pid : remaining) {
                while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
            }
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kChildPollIntervalMs));
    }
    return success;
}

void RedirectLogsToFile(const std::string &logFile)
{
    if (freopen(logFile.c_str(), "a", stdout) == nullptr) {
        TermLog("Failed to redirect stdout to %s\\n", logFile.c_str());
    }
    if (freopen(logFile.c_str(), "a", stderr) == nullptr) {
        TermLog("Failed to redirect stderr to %s\\n", logFile.c_str());
    }
    setbuf(stdout, nullptr);
    setbuf(stderr, nullptr);
}

struct NumaBinding {
    std::string spec;
    std::vector<int> nodes;
    std::string cpuList;
    cpu_set_t cpuSet{};
};

// Parses the kernel cpulist format, e.g. "0-23,48-71".
bool ParseCpuList(const std::string &cpuList, cpu_set_t &cpuSet)
{
    CPU_ZERO(&cpuSet);
    std::istringstream stream(cpuList);
    std::string range;
    int cpuCount = 0;
    while (std::getline(stream, range, ',')) {
        if (range.empty()) {
            continue;
        }
        char *end = nullptr;
        const long first = std::strtol(range.c_str(), &end, kDecimalBase);
        if (end == range.c_str()) {
            return false;
        }
        long last = first;
        if (*end == '-') {
            const char *lastStart = end + 1;
            last = std::strtol(lastStart, &end, kDecimalBase);
            if (end == lastStart) {
                return false;
            }
        }
        if (*end != '\0' || first < 0 || last < first || last >= CPU_SETSIZE) {
            return false;
        }
        for (long cpu = first; cpu <= last; ++cpu) {
            CPU_SET(static_cast<int>(cpu), &cpuSet);
            ++cpuCount;
        }
    }
    return cpuCount > 0;
}

// Parses a node list such as "4", "4,6" or "4-7" (same format as the kernel cpulist).
bool ParseNodeList(const std::string &list, std::set<int> &nodes)
{
    std::istringstream stream(list);
    std::string range;
    while (std::getline(stream, range, ',')) {
        if (range.empty()) {
            continue;
        }
        char *end = nullptr;
        const long first = std::strtol(range.c_str(), &end, kDecimalBase);
        if (end == range.c_str()) {
            return false;
        }
        long last = first;
        if (*end == '-') {
            const char *lastStart = end + 1;
            last = std::strtol(lastStart, &end, kDecimalBase);
            if (end == lastStart) {
                return false;
            }
        }
        if (*end != '\0' || first < 0 || last < first || last >= kMaxNumaNodeCount) {
            return false;
        }
        for (long node = first; node <= last; ++node) {
            nodes.insert(static_cast<int>(node));
        }
    }
    return !nodes.empty();
}

bool ReadNodeCpuList(int node, std::string &cpuList, cpu_set_t &cpuSet)
{
    std::ifstream file(kNumaSysfsNodePrefix + std::to_string(node) + "/cpulist");
    if (!file.is_open() || !std::getline(file, cpuList)) {
        return false;
    }
    return ParseCpuList(cpuList, cpuSet);
}

// Online nodes with at least one CPU this process may run on (respects the container cpuset).
bool UsableNumaNodes(std::set<int> &nodes)
{
    std::ifstream onlineFile(kNumaSysfsOnline);
    std::string onlineList;
    std::set<int> onlineNodes;
    if (!onlineFile.is_open() || !std::getline(onlineFile, onlineList) || !ParseNodeList(onlineList, onlineNodes)) {
        return false;
    }
    cpu_set_t allowed;
    if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0) {
        return false;
    }
    for (int node : onlineNodes) {
        std::string cpuList;
        cpu_set_t cpus;
        if (!ReadNodeCpuList(node, cpuList, cpus)) {
            continue;
        }
        cpu_set_t common;
        CPU_AND(&common, &cpus, &allowed);
        if (CPU_COUNT(&common) > 0) {
            nodes.insert(node);
        }
    }
    return !nodes.empty();
}

std::string JoinNodes(const std::vector<int> &nodes)
{
    std::string text;
    for (int node : nodes) {
        text += (text.empty() ? "" : ",") + std::to_string(node);
    }
    return text;
}

// Validates the nodes in the coordinator so a bad --*-numa value fails before any process is forked.
// Accepts "4", "4,6", "4-7" or "!5" (all usable nodes except 5).
bool ResolveNumaBinding(const std::string &spec, NumaBinding &binding)
{
    binding.spec = spec;
    binding.nodes.clear();
    binding.cpuList.clear();
    CPU_ZERO(&binding.cpuSet);
    if (spec.empty() || spec == "-1") {
        return true;
    }
    std::set<int> nodes;
    if (spec[0] == '!') {
        std::set<int> excluded;
        if (!ParseNodeList(spec.substr(1), excluded)) {
            std::cerr << "Invalid NUMA node list: " << spec << "\n";
            return false;
        }
        if (!UsableNumaNodes(nodes)) {
            std::cerr << "Cannot determine usable NUMA nodes for " << spec << "\n";
            return false;
        }
        for (int node : excluded) {
            nodes.erase(node);
        }
    } else if (!ParseNodeList(spec, nodes)) {
        std::cerr << "Invalid NUMA node list: " << spec << "\n";
        return false;
    }
    if (nodes.empty()) {
        std::cerr << "No NUMA nodes left for " << spec << "\n";
        return false;
    }
    for (int node : nodes) {
        std::string cpuList;
        cpu_set_t cpus;
        if (!ReadNodeCpuList(node, cpuList, cpus)) {
            std::cerr << "NUMA node " << node << " does not exist or has no CPUs\n";
            return false;
        }
        CPU_OR(&binding.cpuSet, &binding.cpuSet, &cpus);
        binding.cpuList += (binding.cpuList.empty() ? "" : ",") + cpuList;
        binding.nodes.push_back(node);
    }
    return true;
}

// Binds the calling process (before it spawns threads) to the nodes' CPUs and memory. Threads and
// processes created afterwards inherit both the CPU affinity and the memory policy.
bool ApplyNumaBinding(const NumaBinding &binding, NumaMemPolicy memPolicy, const char *owner)
{
    if (binding.nodes.empty()) {
        return true;
    }
    const std::string nodes = JoinNodes(binding.nodes);
    if (sched_setaffinity(0, sizeof(binding.cpuSet), &binding.cpuSet) != 0) {
        TermLog("[%s] Failed to set CPU affinity to NUMA nodes %s: %s\n", owner, nodes.c_str(), strerror(errno));
        return false;
    }
    if (memPolicy != NumaMemPolicy::kNone) {
        constexpr size_t kBitsPerMaskWord = sizeof(unsigned long) * CHAR_BIT;
        std::array<unsigned long, kMaxNumaNodeCount / kBitsPerMaskWord> nodeMask{};
        for (int node : binding.nodes) {
            const auto bit = static_cast<size_t>(node);
            nodeMask[bit / kBitsPerMaskWord] |= 1UL << (bit % kBitsPerMaskWord);
        }
        const int mode = memPolicy == NumaMemPolicy::kBind ? kMpolBind : kMpolPreferred;
        // The kernel reads maxnode - 1 bits of the mask (same convention as libnuma).
        if (syscall(SYS_set_mempolicy, mode, nodeMask.data(), kMaxNumaNodeCount + 1) != 0) {
            TermLog("[%s] Failed to set memory policy %s to NUMA nodes %s: %s\n", owner, NumaMemPolicyName(memPolicy),
                    nodes.c_str(), strerror(errno));
            return false;
        }
    }
    TermLog("[%s] pid=%d bound to NUMA nodes %s (cpus %s, mem %s)\n", owner, getpid(), nodes.c_str(),
            binding.cpuList.c_str(), NumaMemPolicyName(memPolicy));
    return true;
}

std::string DescribeNumaBinding(const NumaBinding &binding)
{
    if (binding.nodes.empty()) {
        return "unbound";
    }
    return "nodes " + JoinNodes(binding.nodes) + " (cpus " + binding.cpuList + ")";
}

void SetProcessName(const std::string &name, char *processTitle, size_t processTitleSize)
{
    // Set both comm (top's default) and argv (top -c / ps args).
    if (prctl(PR_SET_NAME, name.c_str(), 0, 0, 0) != 0) {
        TermLog("[%s] Failed to set process name: %s\n", name.c_str(), strerror(errno));
    }
    std::memset(processTitle, 0, processTitleSize);
    std::memcpy(processTitle, name.c_str(), std::min(name.size(), processTitleSize - 1));
}


using bench::LatencyHistogram;

constexpr std::array kTimedOperations{kOpAlloc, kOpUpdate, kOpGet};
using SampleFile = std::unique_ptr<std::FILE, decltype(&std::fclose)>;

struct ThreadStats {
    ThreadStats()
    {
        for (auto type : kTimedOperations) {
            histogram[type].ReserveBuckets();
            submission[type].ReserveBuckets();
        }
    }

    uint64_t successCount[kOpTypeCount] = {0};
    uint64_t failureCount[kOpTypeCount] = {0};
    LatencyHistogram histogram[kOpTypeCount];
    LatencyHistogram submission[kOpTypeCount];
    uint64_t transportFailures[kOpTypeCount]{};
    uint64_t timeouts[kOpTypeCount]{};
};

struct PerOpResult {
    uint64_t successCount;
    uint64_t failureCount;
    uint64_t transportFailures{};
    uint64_t timeouts{};
};

struct ProcessResult {
    int valid;
    PerOpResult ops[kOpTypeCount];
    double durationSec;
    int padding[8];
};

struct MetaServiceReport {
    uint32_t lruType;
    uint32_t lruShardCount;
    uint32_t asyncWorkerCount;
    uint16_t tcpWorkerCount;
};

struct SharedResults {
    MetaServiceReport metaService;
    ProcessResult clients[kMaxProcessCount];
};

struct ProcessLatencies {
    LatencyHistogram histogram[kOpTypeCount];
    LatencyHistogram submission[kOpTypeCount];

    bool MergeSamples(std::FILE *file)
    {
        if (std::fseek(file, 0, SEEK_SET) != 0) {
            return false;
        }
        for (auto type : kTimedOperations) {
            if (!histogram[type].MergeSamples(file) || !submission[type].MergeSamples(file)) {
                return false;
            }
        }
        return true;
    }
};

inline double NowUs();

struct MetricsSample {
    double timestampSec;
    std::map<std::string, double> values;
};

class MetricsSampler {
public:
    explicit MetricsSampler(int intervalMs) : intervalMs_(intervalMs) {}

    void Start()
    {
        if (intervalMs_ <= 0) {
            return;
        }
        running_.store(true, std::memory_order_relaxed);
        worker_ = std::thread([this]() { Run(); });
    }

    void Stop()
    {
        SampleOnce();
        running_.store(false, std::memory_order_relaxed);
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    void PrintReport() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (samples_.size() < 2) {
            std::cout << "Metrics samples: " << samples_.size() << " (need at least 2 for delta report)\n";
            return;
        }
        const auto &first = samples_.front();
        const auto &last = samples_.back();
        const auto Delta = [&first, &last](const char *name) {
            const auto firstIter = first.values.find(name);
            const auto lastIter = last.values.find(name);
            if (firstIter == first.values.end() || lastIter == last.values.end()) {
                return 0.0;
            }
            return lastIter->second - firstIter->second;
        };
        double evictRunningPeak = 0.0;
        for (const auto &sample : samples_) {
            auto iter = sample.values.find("memcache_evict_running");
            if (iter != sample.values.end()) {
                evictRunningPeak = std::max(evictRunningPeak, iter->second);
            }
        }
        const auto ValueOrZero = [](const MetricsSample &sample, const char *name) {
            const auto iter = sample.values.find(name);
            return iter == sample.values.end() ? 0.0 : iter->second;
        };
        std::cout << "\n========== MetaService Metrics (sampled every " << intervalMs_ << " ms) ==========\n"
                  << "Samples: " << samples_.size() << ", duration: " << std::fixed << std::setprecision(2)
                  << (last.timestampSec - first.timestampSec) << " sec\n"
                  << "Eviction: attempts=" << Delta("memcache_evict_operations_total")
                  << ", failures=" << Delta("memcache_evict_failures_total")
                  << ", released_bytes=" << Delta("memcache_evict_bytes_total") << ", running_peak=" << evictRunningPeak
                  << "\n"
                  << "Requests: alloc=" << Delta("memcache_alloc_requests_total")
                  << ", get=" << Delta("memcache_get_requests_total")
                  << ", batch_alloc=" << Delta("memcache_batch_alloc_requests_total")
                  << ", batch_get=" << Delta("memcache_batch_get_requests_total") << "\n"
                  << "Stored keys: first=" << ValueOrZero(first, "memcache_stored_keys")
                  << ", last=" << ValueOrZero(last, "memcache_stored_keys") << "\n";
    }

private:
    static bool Fetch(std::map<std::string, double> &values)
    {
        constexpr const char *kRequest = "GET /metrics HTTP/1.0\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            return false;
        }
        timeval timeout{};
        timeout.tv_sec = kMetricsConnectTimeoutMs / 1000;
        timeout.tv_usec = (kMetricsConnectTimeoutMs % 1000) * 1000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(kMetricsPort);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
            send(fd, kRequest, std::strlen(kRequest), 0) < 0) {
            close(fd);
            return false;
        }
        std::string response;
        std::vector<char> buffer(kMetricsReceiveBufferSize);
        ssize_t bytes = 0;
        while ((bytes = recv(fd, buffer.data(), buffer.size(), 0)) > 0) {
            response.append(buffer.data(), static_cast<size_t>(bytes));
        }
        close(fd);
        const size_t bodyPos = response.find("\r\n\r\n");
        if (bodyPos == std::string::npos) {
            return false;
        }
        std::istringstream stream(response.substr(bodyPos + 4));
        std::string line;
        while (std::getline(stream, line)) {
            if (line.empty() || line[0] == '#') {
                continue;
            }
            const size_t separator = line.find(' ');
            if (separator == std::string::npos) {
                continue;
            }
            std::string name = line.substr(0, separator);
            const size_t labelPos = name.find('{');
            if (labelPos != std::string::npos) {
                continue;
            }
            const std::string value = line.substr(separator + 1);
            char *end = nullptr;
            const double parsed = std::strtod(value.c_str(), &end);
            if (end != value.c_str()) {
                values[name] = parsed;
            }
        }
        return !values.empty();
    }

    void Run()
    {
        while (running_.load(std::memory_order_relaxed)) {
            SampleOnce();
            std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs_));
        }
    }

    void SampleOnce()
    {
        if (intervalMs_ <= 0) {
            return;
        }
        std::map<std::string, double> values;
        if (Fetch(values)) {
            std::lock_guard<std::mutex> lock(mutex_);
            samples_.push_back({NowUs() / kMicroSecPerSec, std::move(values)});
        }
    }

    int intervalMs_;
    std::atomic<bool> running_{false};
    std::thread worker_;
    mutable std::mutex mutex_;
    std::vector<MetricsSample> samples_;
};

// Fixed SHA-256-shaped text shared by all benchmark keys.
constexpr char kKeyHexSegment[] = "2e3f71ee82c2e0f4c0e5dbe107b92d368cc34dc724c15dee5862abec4d25f437";

inline std::string MakeKey(int index)
{
    return std::string("bench_key_") + kKeyHexSegment + "_" + std::to_string(index);
}
struct PreparedKeys {
    std::vector<std::string> readKeys;
    std::vector<std::string> putPrefixes;
};

PreparedKeys PrepareKeys(const BenchConfig &cfg, uint32_t rank)
{
    PreparedKeys keys;
    // With warmup disabled, preserve reads of the missing key at index zero.
    const int readKeyCount = std::max(1, cfg.warmupKeyCount);
    keys.readKeys.reserve(static_cast<size_t>(readKeyCount));
    for (int index = 0; index < readKeyCount; ++index) {
        keys.readKeys.push_back(MakeKey(index));
    }
    keys.putPrefixes.reserve(static_cast<size_t>(cfg.threadCount));
    for (int tid = 0; tid < cfg.threadCount; ++tid) {
        keys.putPrefixes.push_back(std::string("bench_") + kKeyHexSegment + "_p" + std::to_string(rank) +
            "_t" + std::to_string(tid) + "_");
    }
    return keys;
}

inline std::string MakePutKey(const PreparedKeys &keys, int tid, uint64_t counter)
{
    constexpr size_t maxCounterDecimalDigits = 20;
    const auto &prefix = keys.putPrefixes[static_cast<size_t>(tid)];
    std::string key;
    key.reserve(prefix.size() + maxCounterDecimalDigits);
    key.append(prefix).append(std::to_string(counter));
    return key;
}
inline double NowUs()
{
    return static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(
                                   std::chrono::high_resolution_clock::now().time_since_epoch())
                                   .count());
}
void CopyStr(const std::string &src, char *dst, size_t sz)
{
    size_t n = std::min(src.length(), sz - 1);
    std::memcpy(dst, src.c_str(), n);
    dst[n] = '\0';
}

void PrintSep()
{
    std::cout << "+" << std::string(kTableWidth - 2, '-') << "+\n";
}

void PrintAggReport(const std::vector<ProcessResult> &results, ProcessLatencies &latencies, const BenchConfig &cfg)
{
    std::array<PerOpResult, kOpTypeCount> total{};
    double duration = 0;
    for (const auto &process : results) {
        duration = std::max(duration, process.durationSec);
        for (int i = 0; i < kOpTypeCount; ++i) {
            total[i].successCount += process.ops[i].successCount;
            total[i].failureCount += process.ops[i].failureCount;
            total[i].transportFailures += process.ops[i].transportFailures;
            total[i].timeouts += process.ops[i].timeouts;
        }
    }
    std::cout << "\nBenchmark report: queue depth/thread=" << cfg.queueDepth
              << ", completion mode=" << (cfg.waitCompletions ? "wait" : "poll")
              << ", maximum logical concurrency=" << static_cast<uint64_t>(cfg.processCount) * cfg.threadCount * cfg.queueDepth << "\n";
    constexpr int operationColumnWidth = 14;
    constexpr int valueColumnWidth = 12;
    constexpr int metricColumnCount = 8;
    constexpr int latencyPrecision = 2;
    auto operationName = [](OpType type) {
        return type == kOpAlloc ? "put-alloc" : type == kOpUpdate ? "put-update" : "get";
    };
    auto printTable = [&](LatencyHistogram (&samples)[kOpTypeCount], const char *title) {
        const auto savedFlags = std::cout.flags();
        const auto savedPrecision = std::cout.precision();
        std::cout << "\n" << title << "\n"
                  << std::left << std::setw(operationColumnWidth) << "Operation" << std::right;
        for (const char *heading : {"Samples", "Avg (us)", "P50 (us)", "P95 (us)", "P99 (us)", "P999 (us)",
                                    "Min (us)", "Max (us)"}) {
            std::cout << std::setw(valueColumnWidth) << heading;
        }
        std::cout << "\n" << std::string(operationColumnWidth + metricColumnCount * valueColumnWidth, '-') << "\n";
        std::cout << std::fixed << std::setprecision(latencyPrecision);
        for (auto type : kTimedOperations) {
            const auto histogram = samples[type].Summarize();
            std::cout << std::left << std::setw(operationColumnWidth) << operationName(type)
                      << std::right << std::setw(valueColumnWidth) << histogram.count
                      << std::setw(valueColumnWidth) << histogram.avgUs
                      << std::setw(valueColumnWidth) << histogram.p50Us
                      << std::setw(valueColumnWidth) << histogram.p95Us
                      << std::setw(valueColumnWidth) << histogram.p99Us
                      << std::setw(valueColumnWidth) << histogram.p999Us
                      << std::setw(valueColumnWidth) << histogram.minUs
                      << std::setw(valueColumnWidth) << histogram.maxUs << "\n";
        }
        std::cout.flags(savedFlags);
        std::cout.precision(savedPrecision);
    };
    printTable(latencies.histogram, "Client-observed RPC round-trip latency (us = microseconds)");
    printTable(latencies.submission, "Submission duration (us = microseconds; overlaps RPC latency)");
    std::cout << "\nLogical Put: success=" << total[kOpPut].successCount << " fail=" << total[kOpPut].failureCount
              << "\nRPC outcomes:\n";
    for (auto type : kTimedOperations) {
        std::cout << "  " << operationName(type) << ": success=" << total[type].successCount
                  << " fail=" << total[type].failureCount << " transport failures=" << total[type].transportFailures
                  << " timeouts=" << total[type].timeouts << "\n";
    }
    const uint64_t count = total[kOpPut].successCount + total[kOpPut].failureCount +
        total[kOpGet].successCount + total[kOpGet].failureCount;
    std::cout << "Total logical ops: " << count << ", achieved logical ops/sec: " << (duration > 0 ? count / duration : 0)
              << "\nRPC timing includes submission, transport, network, server queues and processing; excludes benchmark construction and response handling.\n";
}

Result RegisterMockBm(MetaNetClient &client, uint32_t rank, uint64_t gva, uint64_t capacity)
{
    BmRegisterRequest req;
    req.rank_ = rank;
    req.mediaType_ = {MEDIA_HBM};
    req.addr_ = {gva};
    req.capacity_ = {capacity};
    req.storageEnabled_ = false;
    Response resp;
    Result ret = client.SyncCall(req, resp, kRpcTimeoutMs);
    return ret == MMC_OK ? resp.ret_ : ret;
}

void RegisterMockBackupHandler(MetaNetClient &client)
{
    client.RegisterRetryHandler({},
                                [](const std::vector<uint32_t> &ops, const std::vector<std::string> &keys,
                                   const std::vector<MmcMemBlobDesc> &blobs,
                                   std::vector<Result> &keyResults) -> int32_t {
                                    if (ops.size() != keys.size() || keys.size() != blobs.size()) {
                                        keyResults.assign(keys.size(), MMC_ERROR);
                                        return MMC_INVALID_PARAM;
                                    }
                                    // The benchmark has no local storage service. Acknowledge each metadata backup
                                    // so MetaService can exercise async-flush RPC without requiring a real BM.
                                    keyResults.assign(keys.size(), MMC_OK);
                                    return MMC_OK;
                                },
                                {});
}

Result DoPut(MetaNetClient &client, const std::string &key, uint32_t rank, uint64_t blobSize)
{
    uint64_t operateId = GenerateOperateId(rank);
    AllocRequest allocReq;
    allocReq.key_ = key;
    allocReq.options_ = AllocOptions(blobSize, 1, MEDIA_HBM, {rank}, 0);
    allocReq.operateId_ = operateId;
    allocReq.rankId_ = rank;
    AllocResponse allocResp;
    if (client.SyncCall(allocReq, allocResp, kRpcTimeoutMs) != MMC_OK)
        return MMC_ERROR;
    if (allocResp.result_ != MMC_OK)
        return allocResp.result_;
    if (allocResp.blobs_.empty())
        return MMC_ERROR;
    auto &blob = allocResp.blobs_[0];
    UpdateRequest updateReq(MMC_WRITE_OK, key, blob.rank_, blob.mediaType_, operateId);
    Response updateResp;
    Result ret = client.SyncCall(updateReq, updateResp, kRpcTimeoutMs);
    return ret == MMC_OK ? updateResp.ret_ : ret;
}

template<typename ResponseType>
bool DecodeResponse(const std::string &bytes, ResponseType &response)
{
    try {
        NetMsgUnpacker unpacker(bytes);
        if (response.Deserialize(unpacker) != MMC_OK) return false;
        // Some existing message deserializers do not propagate field read failures.
        // Canonical reserialization detects truncated or otherwise malformed payloads.
        NetMsgPacker verified;
        return response.Serialize(verified) == MMC_OK && verified.String() == bytes;
    } catch (const std::exception &) {
        return false;
    }
}

struct PendingOperation {
    bool put;
    bool batch;
    bool failed{false};
    OpType stage;
    uint64_t operateId;
    std::vector<std::string> keys;
    size_t updateCount{0};
    double start{0};
};

void WorkerWithPreparedKeys(bench::AsyncTransport &transport, const BenchConfig &cfg, const PreparedKeys &keys,
                            ThreadStats &stats, int tid, uint32_t rank, std::atomic<uint32_t> *queueDepth = nullptr)
{
    bench::CompletionQueue queue;
    std::unordered_map<uint32_t, PendingOperation> pending;
    const auto publishDepth = [&]() {
        if (queueDepth != nullptr) {
            queueDepth->store(static_cast<uint32_t>(pending.size()), std::memory_order_relaxed);
        }
    };
    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> batchDist(cfg.minBatch, cfg.maxBatch);
    std::uniform_int_distribution<int> readDist(0, std::max(0, cfg.warmupKeyCount - 1));
    std::uniform_int_distribution<int> ratioDist(1, kPercentileDivisor);
    uint64_t putCounter = 0;
    int issued = 0;
    auto finish = [&](const PendingOperation &operation, bool ok) {
        const auto type = operation.put ? kOpPut : kOpGet;
        (ok ? stats.successCount[type] : stats.failureCount[type])++;
    };
    auto submit = [&](PendingOperation operation, const auto &request) {
        NetMsgPacker packer;
        if (request.Serialize(packer) != MMC_OK) {
            stats.failureCount[operation.stage]++;
            if (operation.put) finish(operation, false);
            return;
        }
        const std::string bytes = packer.String();
        uint32_t sequence = transport.Register(queue);
        if (sequence == 0) {
            stats.failureCount[operation.stage]++;
            stats.transportFailures[operation.stage]++;
            if (operation.put) finish(operation, false);
            return;
        }
        pending.emplace(sequence, std::move(operation));
        publishDepth();
        auto &entry = pending.at(sequence);
        double end = 0;
        int result = transport.Send(sequence, request.msgId, bytes, entry.start, end);
        stats.submission[entry.stage].Record(end - entry.start);
        if (result != MMC_OK) {
            transport.Cancel(sequence);
            queue.Push({sequence, end, result, {}});
        }
    };
    while (issued < cfg.opsPerThread || !pending.empty()) {
        if (transport.Failed()) {
            for (const auto &entry : pending) {
                stats.failureCount[entry.second.stage]++;
                stats.transportFailures[entry.second.stage]++;
                if (entry.second.put) finish(entry.second, false);
                transport.Cancel(entry.first);
            }
            pending.clear();
            publishDepth();
            break;
        }
        while (issued < cfg.opsPerThread && pending.size() < static_cast<size_t>(cfg.queueDepth)) {
            PendingOperation operation{};
            operation.put = ratioDist(rng) <= cfg.putPercent;
            operation.stage = operation.put ? kOpAlloc : kOpGet;
            operation.operateId = GenerateOperateId(rank);
            int size = batchDist(rng);
            operation.batch = size > 1;
            for (int key = 0; key < size; ++key) {
                operation.keys.push_back(operation.put ? MakePutKey(keys, tid, putCounter++) :
                    keys.readKeys[static_cast<size_t>(readDist(rng))]);
            }
            ++issued;
            if (operation.put) {
                std::vector<AllocOptions> options(operation.keys.size(), AllocOptions(cfg.blobSize, 1, MEDIA_HBM, {rank}, 0));
                if (operation.batch) submit(operation, BatchAllocRequest(operation.keys, options, 0, operation.operateId, rank));
                else submit(operation, AllocRequest(operation.keys[0], options[0], operation.operateId, rank));
            } else if (operation.batch) {
                submit(operation, BatchGetRequest(operation.keys, rank, operation.operateId));
            } else {
                submit(operation, GetRequest(operation.keys[0], rank, operation.operateId, true));
            }
        }
        double deadline = bench::ClockUs() + cfg.rpcTimeoutMs * 1000.0;
        for (const auto &entry : pending) deadline = std::min(deadline, entry.second.start + cfg.rpcTimeoutMs * 1000.0);
        if (pending.empty()) continue;
        auto completions = queue.Drain(cfg.waitCompletions, deadline);
        for (auto &completion : completions) {
            auto found = pending.find(completion.sequence);
            if (found == pending.end()) continue;
            PendingOperation operation = std::move(found->second);
            pending.erase(found);
            publishDepth();
            bool ok = completion.result == MMC_OK && !completion.bytes.empty();
            BatchUpdateRequest update;
            if (ok) {
                stats.histogram[operation.stage].Record(completion.arrival - operation.start);
                if (operation.stage == kOpUpdate) {
                    if (operation.batch) {
                        BatchUpdateResponse response;
                        ok = DecodeResponse(completion.bytes, response) && response.results_.size() == operation.updateCount &&
                            std::all_of(response.results_.begin(), response.results_.end(), [](Result r) { return r == MMC_OK; });
                    } else {
                        Response response;
                        ok = DecodeResponse(completion.bytes, response) && response.ret_ == MMC_OK;
                    }
                } else {
                    BatchAllocResponse response;
                    if (operation.batch) {
                        ok = DecodeResponse(completion.bytes, response) && response.results_.size() == operation.keys.size() &&
                            response.blobs_.size() == operation.keys.size();
                    } else {
                        AllocResponse single;
                        ok = DecodeResponse(completion.bytes, single);
                        response.results_ = {single.result_};
                        response.blobs_ = {single.blobs_};
                    }
                    if (ok) {
                        for (size_t i = 0; i < operation.keys.size(); ++i) {
                            if (response.results_[i] != MMC_OK || response.blobs_[i].empty()) { ok = false; continue; }
                            if (!operation.put) continue;
                            for (const auto &blob : response.blobs_[i]) {
                                update.keys_.push_back(operation.keys[i]);
                                update.ranks_.push_back(blob.rank_);
                                update.mediaTypes_.push_back(blob.mediaType_);
                                update.operateIds_.push_back(operation.operateId);
                                update.actionResults_.push_back(MMC_WRITE_OK);
                            }
                        }
                    }
                }
            } else stats.transportFailures[operation.stage]++;
            (ok ? stats.successCount[operation.stage] : stats.failureCount[operation.stage])++;
            operation.failed = operation.failed || !ok;
            if (operation.stage == kOpAlloc && !update.keys_.empty()) {
                operation.updateCount = update.keys_.size();
                operation.stage = kOpUpdate;
                if (operation.batch) submit(operation, update);
                else submit(operation, UpdateRequest(MMC_WRITE_OK, update.keys_[0], update.ranks_[0],
                    update.mediaTypes_[0], operation.operateId));
            } else if (operation.put) finish(operation, !operation.failed);
        }
        for (auto it = pending.begin(); it != pending.end();) {
            if (bench::ClockUs() - it->second.start < cfg.rpcTimeoutMs * 1000.0) { ++it; continue; }
            transport.Cancel(it->first);
            stats.timeouts[it->second.stage]++;
            stats.failureCount[it->second.stage]++;
            if (it->second.put) finish(it->second, false);
            it = pending.erase(it);
            publishDepth();
        }
    }
}

// Preserve the original interface for in-process tests. Production clients prepare keys before START.
[[maybe_unused]] void Worker(bench::AsyncTransport &transport, const BenchConfig &cfg, ThreadStats &stats,
                             int tid, uint32_t rank)
{
    const PreparedKeys keys = PrepareKeys(cfg, rank);
    WorkerWithPreparedKeys(transport, cfg, keys, stats, tid, rank);
}

bool WriteResult(ProcessResult &out, const ThreadStats &stats, double durationSec, std::FILE *sampleFile)
{
    for (auto type : kTimedOperations) {
        if (!stats.histogram[type].WriteSamples(sampleFile) || !stats.submission[type].WriteSamples(sampleFile)) {
            return false;
        }
    }
    if (std::fflush(sampleFile) != 0) {
        return false;
    }
    out.durationSec = durationSec;
    for (int i = 0; i < kOpTypeCount; ++i) {
        out.ops[i].successCount = stats.successCount[i];
        out.ops[i].failureCount = stats.failureCount[i];
        out.ops[i].transportFailures = stats.transportFailures[i];
        out.ops[i].timeouts = stats.timeouts[i];
    }
    out.valid = 1;
    return true;
}

// perf record takes a comma separated pid list, so one perf can follow every client process.
std::string JoinPids(const std::vector<pid_t> &pids)
{
    std::string text;
    for (pid_t pid : pids) {
        text += (text.empty() ? "" : ",") + std::to_string(pid);
    }
    return text;
}

pid_t StartPerfOnPids(const std::vector<pid_t> &pids, const BenchConfig &cfg)
{
    std::string pidList = JoinPids(pids);
    std::string freqStr = std::to_string(cfg.perfFreq);
    const pid_t parentPid = getpid();
    pid_t pid = fork();
    if (pid != 0) {
        return pid;
    }
    PrepareChildProcess(parentPid);
    execlp("perf", "perf", "record", "-F", freqStr.c_str(), "-g", "-p", pidList.c_str(), "-o", kDefaultPerfDataFile,
           nullptr);
    TermLog("[perf] failed to launch perf: %s\n", strerror(errno));
    _exit(kPerfExecFailCode);
}

// Starts perf and verifies it survived the first second; returns -1 when profiling is unavailable.
pid_t StartPerfChecked(const std::vector<pid_t> &pids, const BenchConfig &cfg)
{
    pid_t perfPid = StartPerfOnPids(pids, cfg);
    if (perfPid <= 0) {
        TermLog("[perf] failed to fork perf process, profiling disabled\n");
        return -1;
    }
    sleep(kPerfReadyWaitSec);
    int perfStatus = 0;
    pid_t reaped;
    do {
        reaped = waitpid(perfPid, &perfStatus, WNOHANG);
    } while (reaped < 0 && errno == EINTR);
    if (reaped == perfPid || reaped == -1) {
        TermLog("[perf] perf exited early (perf installed? permissions? ulimit -n?), profiling disabled\n");
        return -1;
    }
    TermLog("[perf] profiling %s (pid %s) -> %s\n", PerfTargetName(cfg.perfTarget),
            JoinPids(pids).c_str(), kDefaultPerfDataFile);
    return perfPid;
}

std::optional<int64_t> ReadRssKiB(pid_t pid)
{
    constexpr std::string_view field = "VmRSS:";
    std::ifstream status("/proc/" + std::to_string(pid) + "/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.compare(0, field.size(), field) == 0) {
            std::istringstream value(line.substr(field.size()));
            int64_t rss = 0;
            std::string unit;
            // procfs labels its KiB values as kB.
            if ((value >> rss >> unit) && rss >= 0 && unit == "kB") {
                return rss;
            }
            break;
        }
    }
    return std::nullopt;
}

void PrintRssDelta(std::optional<int64_t> startRssKiB, std::optional<int64_t> endRssKiB)
{
    std::ostringstream report;
    report.imbue(std::locale::classic());
    report << "\nMetaService RSS change: ";
    if (!startRssKiB || !endRssKiB) {
        report << "N/A";
    } else {
        constexpr std::array units{"KiB", "MiB", "GiB", "TiB", "PiB", "EiB"};
        const int64_t delta = *endRssKiB - *startRssKiB;
        int64_t scale = 1;
        size_t unit = 0;
        while (unit + 1 < units.size() && std::abs(delta) / scale >= kIecUnitScale) {
            scale *= kIecUnitScale;
            ++unit;
        }
        if (delta > 0) {
            report << '+';
        }
        if (unit == 0) {
            report << delta;
        } else {
            report << std::fixed << std::setprecision(kScaledMemoryPrecision) << static_cast<double>(delta) / scale;
        }
        report << ' ' << units[unit];
    }
    std::cout << report.str() << '\n';
}

void StopPerf(pid_t perfPid)
{
    if (perfPid <= 0) {
        return;
    }
    ReapChildren({perfPid}, SIGINT, kPerfStopTimeoutSec);
}

int RunMetaService(const std::string &metaUrl, const std::string &bmUrl, const std::string &metricsUrl,
                   const std::string &logFile, uint64_t capacityMB, MetaServiceReport &report, int readyFd)
{
    // Block before Start() creates threads so every service thread inherits the mask.
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    pthread_sigmask(SIG_BLOCK, &mask, nullptr);
    RedirectLogsToFile(logFile);

    mmc_meta_service_config_t cfg{};
    CopyStr(metaUrl, cfg.discoveryURL, sizeof(cfg.discoveryURL));
    CopyStr(bmUrl, cfg.configStoreURL, sizeof(cfg.configStoreURL));
    CopyStr(metricsUrl, cfg.httpURL, sizeof(cfg.httpURL));
    cfg.haEnable = false;
    cfg.lruShardCount = MMC_DEFAULT_META_LRU_SHARD_COUNT;
    cfg.logLevel = WARN_LEVEL;
    cfg.evictThresholdHigh = kEvictThresholdHigh;
    cfg.evictThresholdLow = kEvictThresholdLow;
    // MmcMetaService validates these even when the benchmark does not use SSD
    // async flush. Keep them aligned with the library's default configuration.
    cfg.asyncFlushIntervalMs = 200;
    cfg.asyncFlushBatchLimit = 20;
    cfg.logRotationFileSize = kLogRotationFileSize;
    cfg.logRotationFileCount = kLogRotationFileCount;
    cfg.accTlsConfig.tlsEnable = false;

    ptracer_config_t ptraceConfig{.tracerType = kPtracerType, .dumpFilePath = kPtracerDumpPath};
    const auto ptraceResult = ptracer_init(&ptraceConfig);
    if (ptraceResult != MMC_OK) {
        TermLog("[meta_service] Warning, init ptracer failed, result: %d, msg: %s\n", ptraceResult,
                ptracer_get_last_err_msg());
    }

    auto svc = MmcMakeRef<MmcMetaService>("benchMetaService");
    if (svc->Start(cfg) != MMC_OK) {
        TermLog("[meta_service] Failed to start!\n");
        ptracer_uninit();
        return 1;
    }
    const auto &options = svc->Options();
    report = {options.lruType, options.lruShardCount, options.netPoolBase, svc->GetTcpWorkerCount()};
    TermLog("[meta_service] Started at %s\n", metaUrl.c_str());
    const bool ready = bench::SendBarrierMessage(readyFd, bench::BarrierMessage::READY);
    close(readyFd);
    if (!ready) {
        svc->Stop();
        ptracer_uninit();
        return 1;
    }

    int sig = 0;
    sigwait(&mask, &sig);

    // The parent sends SIGTERM after all benchmark clients exit. Take one final
    // snapshot before Stop() clears the meta manager and its container.
    size_t keyCount = 0;
    const auto &metaMgrProxy = svc->GetMetaMgrProxy();
    if (metaMgrProxy != nullptr && metaMgrProxy->GetMetaManager() != nullptr) {
        Result countRet = metaMgrProxy->GetMetaManager()->GetKeyCount(keyCount);
        if (countRet != MMC_OK) {
            TermLog("[meta_service] Failed to print final shard distribution: %d\n", countRet);
        }
    }
    svc->Stop();
    ptracer_uninit();
    TermLog("[meta_service] Stopped (signal %d)\n", sig);
    return 0;
}

int RunClient(const BenchConfig &cfg, int clientIndex, ProcessResult *resultSlot, const std::string &metaUrl,
              const std::string &logFile, int barrierFd, bench::QueueDepthMonitor &queueDepthMonitor,
              std::FILE *sampleFile)
{
    RedirectLogsToFile(logFile);

    uint32_t rank = static_cast<uint32_t>(clientIndex);
    uint64_t gva = kMockHbmGvaBase + static_cast<uint64_t>(clientIndex) * kMockHbmGvaStride;
    uint64_t capacity = cfg.capacityMB * kBytesPerMB;
    std::string clientName = "BenchClient_" + std::to_string(clientIndex);

    auto client = MetaNetClientFactory::GetInstance(metaUrl, clientName);
    if (client.Get() == nullptr) return 1;
    if (!client->Status()) {
        NetEngineOptions opts;
        opts.name = clientName;
        opts.threadCount = kNetClientThreadCount;
        opts.rankId = rank;
        opts.startListener = false;
        opts.tlsOption.tlsEnable = false;
        opts.logLevel = WARN_LEVEL;
        if (client->Start(opts) != MMC_OK || client->Connect(metaUrl) != MMC_OK) {
            TermLog("[client %d] Failed to connect!\n", clientIndex);
            return 1;
        }
    }

    RegisterMockBackupHandler(*client.Get());
    Result bmRet = RegisterMockBm(*client.Get(), rank, gva, capacity);
    if (bmRet != MMC_OK) {
        TermLog("[client %d] BM register failed: %d\n", clientIndex, bmRet);
        client->Stop();
        return 1;
    }
    TermLog("[client %d] Connected, rank=%u, capacity=%llu MB\n", clientIndex, rank,
            static_cast<unsigned long long>(cfg.capacityMB));

    const PreparedKeys keys = PrepareKeys(cfg, rank);
    int warmupStart = static_cast<int>(static_cast<int64_t>(clientIndex) * cfg.warmupKeyCount / cfg.processCount);
    int warmupEnd = static_cast<int>(static_cast<int64_t>(clientIndex + 1) * cfg.warmupKeyCount / cfg.processCount);
    int warmupPerClient = warmupEnd - warmupStart;
    for (int i = 0; i < warmupPerClient; ++i) {
        const auto &key = keys.readKeys[static_cast<size_t>(warmupStart + i)];
        Result warmupRet = DoPut(*client.Get(), key, rank, cfg.blobSize);
        if (warmupRet != MMC_OK) {
            TermLog("[client %d] Warmup Put failed for %s: %d\n", clientIndex, key.c_str(), warmupRet);
            return 1;
        }
    }

    std::vector<ThreadStats> threadStats(static_cast<size_t>(cfg.threadCount));
    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(cfg.threadCount));

    bench::AsyncTransport transport;
    constexpr uint16_t minSendCapacity = 32;
    if (!transport.Start(metaUrl, std::max<uint16_t>(minSendCapacity, cfg.threadCount * cfg.queueDepth))) {
        TermLog("[client %d] Async transport setup failed\n", clientIndex);
        return 1;
    }
    TermLog("[client %d] Queue depth/thread=%d, mode=%s, maximum logical concurrency=%llu\n", clientIndex,
        cfg.queueDepth, cfg.waitCompletions ? "wait" : "poll",
        static_cast<unsigned long long>(cfg.processCount) * cfg.threadCount * cfg.queueDepth);
    // No process may read the shared key range until every process has populated its partition.
    if (!bench::WaitForBarrier(barrierFd, bench::BarrierMessage::READY, bench::BarrierMessage::START)) {
        TermLog("[client %d] Startup barrier aborted\n", clientIndex);
        return 1;
    }
    double startUs = NowUs();
    for (int t = 0; t < cfg.threadCount; ++t) {
        workers.emplace_back(WorkerWithPreparedKeys, std::ref(transport), std::ref(cfg), std::cref(keys),
                             std::ref(threadStats[static_cast<size_t>(t)]), t, rank,
                             queueDepthMonitor.Counter(clientIndex, t));
    }
    for (auto &w : workers)
        w.join();
    double endUs = NowUs();
    double durationSec = (endUs - startUs) / static_cast<double>(kMicroSecPerSec);

    // The duration above excludes this wait. Keep rank resources and callbacks alive for slower clients.
    if (transport.Failed() ||
        !bench::WaitForBarrier(barrierFd, bench::BarrierMessage::DRAINED, bench::BarrierMessage::TEARDOWN)) {
        TermLog("[client %d] Teardown barrier aborted\n", clientIndex);
        return 1;
    }
    ThreadStats merged;
    for (auto &ts : threadStats) {
        for (int i = 0; i < kOpTypeCount; ++i) {
            merged.successCount[i] += ts.successCount[i];
            merged.failureCount[i] += ts.failureCount[i];
            merged.histogram[i].MergeFrom(ts.histogram[i]);
            merged.submission[i].MergeFrom(ts.submission[i]);
            merged.transportFailures[i] += ts.transportFailures[i];
            merged.timeouts[i] += ts.timeouts[i];
        }
    }

    uint64_t totalSucc = 0, totalFail = 0;
    for (int i = 0; i < kOpAlloc; ++i) {
        totalSucc += merged.successCount[i];
        totalFail += merged.failureCount[i];
    }
    TermLog("[client %d] Done: %llu succ, %llu fail, %.2fs\n", clientIndex, static_cast<unsigned long long>(totalSucc),
            static_cast<unsigned long long>(totalFail), durationSec);

    if (!WriteResult(*resultSlot, merged, durationSec, sampleFile)) {
        TermLog("[client %d] Failed to write latency samples\n", clientIndex);
        return 1;
    }
    // Keep all clients alive until every process has published its samples.
    if (!bench::WaitForBarrier(barrierFd, bench::BarrierMessage::READY, bench::BarrierMessage::START)) {
        TermLog("[client %d] Reporting barrier aborted\n", clientIndex);
        return 1;
    }
    client->Stop();
    return transport.Failed() ? 1 : 0;
}

bool ParseArgs(int argc, char *argv[], BenchConfig &cfg)
{
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            std::cout
                << "Usage: bench_meta_service [options]\n"
                << "  -q, --queue-depth <depth>  Logical operations per thread (default: 1)\n"
                << "  --completion-mode poll|wait (default: poll)\n"
                << "  -p <processes>    Client process count                    (default: " << kDefaultProcessCount
                << ")\n"
                << "  -t <threads>      Threads per process                     (default: " << kDefaultThreadCount
                << ")\n"
                << "  -n <ops>          Operations per thread                    (default: " << kDefaultOpsPerThread
                << ")\n"
                << "  -k <keyspace>     Key space size                          (default: " << kDefaultKeySpaceSize
                << ")\n"
                << "  -w <warmup>       Warmup key count (Put pre-fill)         (default: " << kDefaultWarmupKeyCount
                << ")\n"
                << "  -s <blobsize>     Blob size in bytes                      (default: " << kDefaultBlobSize << ")\n"
                << "  -r <putPercent>   Put percentage (1-99)                    (default: " << kDefaultPutPercent
                << ")\n"
                << "  -b <min,max>      Batch size range (comma-separated)      (default: " << kDefaultMinBatch << ","
                << kDefaultMaxBatch << ")\n"
                << "  -c <capacityMB>   Mock HBM capacity per rank in MB        (default: " << kDefaultCapacityMB
                << ")\n"
                << "  -l <logFile>      Log file for MMC library output          (default: " << kDefaultLogFile << ")\n"
                << "  --perf             Enable perf profiling of meta_service     (default: off)\n"
                << "  --perf-freq <hz>   perf record sample frequency                (default: " << kDefaultPerfFreq
                << ")\n"
                << "  --metrics-interval-ms <ms>  Sample /metrics every ms; 0 disables (default: "
                << kDefaultMetricsIntervalMs << ")\n"
                << "  --perf-target <who>  Profile meta|clients|client0                 (default: meta)\n"
                << "  --meta-numa <nodes>   NUMA nodes for meta_service, e.g. 5, 4,6, 4-7, !5 (default: unbound)\n"
                << "  --worker-numa <nodes> NUMA nodes for coordinator + clients, same format  (default: unbound)\n"
                << "  --numa-mem <policy>   NUMA memory policy: bind|preferred|none             (default: bind)\n";
            return false;
        }
        if (a == "--perf") {
            cfg.perfEnable = true;
            continue;
        }
        if (i + 1 >= argc) {
            std::cerr << "Missing value for " << a << "\n";
            return false;
        }
        std::string v = argv[++i];
        if (a == "-q" || a == "--queue-depth") {
            char *end = nullptr;
            long depth = std::strtol(v.c_str(), &end, 10);
            if (end == v.c_str() || *end != '\0' || depth <= 0 || depth > UINT16_MAX) {
                std::cerr << "Queue depth must be a positive integer within transport limits\n";
                return false;
            }
            cfg.queueDepth = static_cast<int>(depth);
        } else if (a == "--completion-mode") {
            if (v != "poll" && v != "wait") {
                std::cerr << "Completion mode must be poll or wait\n";
                return false;
            }
            cfg.waitCompletions = v == "wait";
        } else if (a == "-p")
            cfg.processCount = std::max(1, std::min(kMaxProcessCount, std::atoi(v.c_str())));
        else if (a == "-t")
            cfg.threadCount = std::max(1, std::atoi(v.c_str()));
        else if (a == "-n")
            cfg.opsPerThread = std::max(1, std::atoi(v.c_str()));
        else if (a == "-k")
            cfg.keySpaceSize = std::max(1, std::atoi(v.c_str()));
        else if (a == "-w")
            cfg.warmupKeyCount = std::max(0, std::atoi(v.c_str()));
        else if (a == "-s")
            cfg.blobSize = static_cast<uint64_t>(std::max(1, std::atoi(v.c_str())));
        else if (a == "-r")
            cfg.putPercent = std::max(1, std::min(99, std::atoi(v.c_str())));
        else if (a == "-b") {
            size_t comma = v.find(',');
            if (comma != std::string::npos) {
                cfg.minBatch = std::max(1, std::atoi(v.substr(0, comma).c_str()));
                cfg.maxBatch = std::max(cfg.minBatch, std::atoi(v.substr(comma + 1).c_str()));
            } else {
                cfg.minBatch = 1;
                cfg.maxBatch = std::max(1, std::atoi(v.c_str()));
            }
        } else if (a == "-c")
            cfg.capacityMB = static_cast<uint64_t>(std::max(1, std::atoi(v.c_str())));
        else if (a == "-l")
            cfg.logFile = v;
        else if (a == "--perf-freq")
            cfg.perfFreq = std::max(1, std::atoi(v.c_str()));
        else if (a == "--metrics-interval-ms")
            cfg.metricsIntervalMs = std::max(0, std::atoi(v.c_str()));
        else if (a == "--perf-target") {
            if (v == "meta")
                cfg.perfTarget = PerfTarget::kMeta;
            else if (v == "clients")
                cfg.perfTarget = PerfTarget::kClients;
            else if (v == "client0")
                cfg.perfTarget = PerfTarget::kClient0;
            else {
                std::cerr << "Invalid --perf-target: " << v << " (expected meta|clients|client0)\n";
                return false;
            }
        } else if (a == "--profile-control-fd") {
            char *end = nullptr;
            long descriptor = std::strtol(v.c_str(), &end, kDecimalBase);
            if (end == v.c_str() || *end != '\0' || descriptor < 0 || descriptor > INT_MAX) {
                std::cerr << "Invalid profile control descriptor\n";
                return false;
            }
            cfg.profileControlFd = static_cast<int>(descriptor);
        } else if (a == "--meta-numa") {
            cfg.metaNumaNodes = v;
        } else if (a == "--worker-numa") {
            cfg.workerNumaNodes = v;
        } else if (a == "--numa-mem") {
            if (v == "bind")
                cfg.numaMemPolicy = NumaMemPolicy::kBind;
            else if (v == "preferred")
                cfg.numaMemPolicy = NumaMemPolicy::kPreferred;
            else if (v == "none")
                cfg.numaMemPolicy = NumaMemPolicy::kNone;
            else {
                std::cerr << "Invalid --numa-mem: " << v << " (expected bind|preferred|none)\n";
                return false;
            }
        } else {
            std::cerr << "Unknown: " << a << "\n";
            return false;
        }
    }
    if (static_cast<uint64_t>(cfg.threadCount) * cfg.queueDepth > UINT16_MAX) {
        std::cerr << "Threads times queue depth exceeds transport capacity\n";
        return false;
    }
    return true;
}
} // namespace

int main(int argc, char *argv[])
{
    BenchConfig cfg;
    if (!ParseArgs(argc, argv, cfg))
        return 1;
    if (!InstallStopHandlers()) {
        std::cerr << "Failed to install shutdown handlers: " << strerror(errno) << std::endl;
        return 1;
    }

    if (cfg.profileControlFd >= 0 && fcntl(cfg.profileControlFd, F_SETFD, FD_CLOEXEC) < 0) {
        std::cerr << "Invalid profile control descriptor: " << strerror(errno) << std::endl;
        return 1;
    }

    // Arguments have been copied into cfg. Reuse only contiguous argv storage,
    // leaving the environment untouched. Children inherit this storage and size.
    char *processTitle = argv[0];
    size_t processTitleSize = std::strlen(argv[0]) + 1;
    for (int i = 1; i < argc && argv[i] == processTitle + processTitleSize; ++i) {
        processTitleSize += std::strlen(argv[i]) + 1;
    }
    SetProcessName(kCoordinatorProcName, processTitle, processTitleSize);

    NumaBinding metaNuma;
    NumaBinding workerNuma;
    if (!ResolveNumaBinding(cfg.metaNumaNodes, metaNuma) || !ResolveNumaBinding(cfg.workerNumaNodes, workerNuma))
        return 1;

    gTermFd = dup(STDERR_FILENO);
    if (gTermFd < 0)
        gTermFd = STDERR_FILENO;

    FILE *truncateLog = fopen(cfg.logFile.c_str(), "w");
    if (truncateLog)
        fclose(truncateLog);

    std::cout << "============================================================\n"
              << "  MetaService Multi-Process Benchmark (Put/Get)\n"
              << "============================================================" << std::endl;
    for (int node : metaNuma.nodes) {
        if (std::find(workerNuma.nodes.begin(), workerNuma.nodes.end(), node) != workerNuma.nodes.end()) {
            TermLog("[numa] Warning, meta_service and workers share NUMA node %d\n", node);
        }
    }

    std::string metaUrl = "tcp://127.0.0.1:" + std::to_string(kMetaServicePort);
    std::string bmUrl = "tcp://127.0.0.1:" + std::to_string(kConfigStorePort);
    std::string metricsUrl = "http://0.0.0.0:" + std::to_string(kMetricsPort);

    constexpr size_t shmSize = sizeof(SharedResults);
    void *resultMapping = mmap(nullptr, shmSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (resultMapping == MAP_FAILED) {
        std::cerr << "Failed to create shared memory!" << std::endl;
        return 1;
    }
    auto *sharedResults = new (resultMapping) SharedResults{};

    bench::QueueDepthMonitor queueDepthMonitor(cfg.processCount, cfg.threadCount);
    if (!queueDepthMonitor.Valid()) {
        std::cerr << "Failed to allocate queue depth counters!" << std::endl;
        munmap(sharedResults, shmSize);
        return 1;
    }

    int metaControl[bench::kBarrierSocketCount];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, metaControl) != 0) {
        std::cerr << "Failed to create MetaService startup channel: " << strerror(errno) << std::endl;
        munmap(sharedResults, shmSize);
        return 1;
    }
    std::cout << "[1/3] Starting meta_service process..." << std::endl;
    const pid_t coordinatorPid = getpid();
    pid_t metaPid = fork();
    if (metaPid == 0) {
        PrepareChildProcess(coordinatorPid);
        close(metaControl[bench::kBarrierParentEnd]);
        if (cfg.profileControlFd >= 0) {
            close(cfg.profileControlFd);
        }
        SetProcessName(kMetaServiceProcName, processTitle, processTitleSize);
        if (!ApplyNumaBinding(metaNuma, cfg.numaMemPolicy, kMetaServiceProcName)) {
            _exit(1);
        }
        return RunMetaService(metaUrl, bmUrl, metricsUrl, cfg.logFile, cfg.capacityMB, sharedResults->metaService,
                              metaControl[bench::kBarrierClientEnd]);
    }
    close(metaControl[bench::kBarrierClientEnd]);
    if (metaPid < 0) {
        std::cerr << "Failed to fork meta_service!" << std::endl;
        close(metaControl[bench::kBarrierParentEnd]);
        munmap(sharedResults, shmSize);
        return 1;
    }
    // Bind the coordinator only after meta_service is forked, so that perf, the metrics sampler and all
    // client processes (forked below) inherit the worker nodes while meta_service keeps its own nodes.
    if (!ApplyNumaBinding(workerNuma, cfg.numaMemPolicy, "coordinator/clients")) {
        close(metaControl[bench::kBarrierParentEnd]);
        ReapChildren({metaPid}, SIGTERM);
        munmap(sharedResults, shmSize);
        return 1;
    }
    const bool metaReady =
        bench::ReceiveBarrierMessage(metaControl[bench::kBarrierParentEnd], bench::BarrierMessage::READY, &gStopSignal);
    close(metaControl[bench::kBarrierParentEnd]);
    if (!metaReady) {
        std::cerr << "MetaService startup did not complete." << std::endl;
        ReapChildren({metaPid}, SIGTERM);
        munmap(sharedResults, shmSize);
        return gStopSignal != 0 ? kSignalExitCodeBase + gStopSignal : 1;
    }
    std::cout << "meta_service started (pid=" << metaPid << ")" << std::endl;

    const auto &metaService = sharedResults->metaService;
    const bool sharded = metaService.lruType == MMC_META_LRU_TYPE_SHARDED_LRU;
    std::cout << "\nConfiguration:\n"
              << "  Build mode:   " << BENCH_BUILD_MODE << "\n"
              << "  Processes:    " << cfg.processCount << "\n"
              << "  Threads/proc: " << cfg.threadCount << "\n"
              << "  Ops/thread:   " << cfg.opsPerThread << "\n"
              << "  Total ops:    " << (cfg.processCount * cfg.threadCount * cfg.opsPerThread) << "\n"
              << "  Key space:    " << cfg.keySpaceSize << "\n"
              << "  Blob size:    " << cfg.blobSize << " bytes\n"
              << "  Warmup keys:  " << cfg.warmupKeyCount << "\n"
              << "  Put percent:  " << cfg.putPercent << "%\n"
              << "  Batch range:  [" << cfg.minBatch << ", " << cfg.maxBatch << "]\n"
              << "  HBM/proc:     " << cfg.capacityMB << " MB\n"
              << "  Log file:      " << cfg.logFile << "\n"
              << "  Queue depth:  " << cfg.queueDepth << " (in flight/proc: "
              << (cfg.threadCount * cfg.queueDepth) << ")\n"
              << "  Perf:          "
              << (cfg.perfEnable ? std::string("record ") + PerfTargetName(cfg.perfTarget) + " -> " +
                                       kDefaultPerfDataFile
                                 : std::string("off"))
              << "\n"
              << "  Metrics:       "
              << (cfg.metricsIntervalMs > 0 ? std::to_string(cfg.metricsIntervalMs) + " ms" : std::string("off"))
              << "\n"
              << "  NUMA meta:     " << DescribeNumaBinding(metaNuma) << "\n"
              << "  NUMA worker:   " << DescribeNumaBinding(workerNuma) << "\n"
              << "  NUMA mem:      " << NumaMemPolicyName(cfg.numaMemPolicy) << "\n"
              << "  MetaService LRU type:    " << (sharded ? "sharded_lru" : "lru") << "\n"
              << "  LRU shards:              " << (sharded ? std::to_string(metaService.lruShardCount) : "N/A") << "\n"
              << "  Async network workers:   " << metaService.asyncWorkerCount << "\n"
              << "  TCP transport workers:   " << metaService.tcpWorkerCount << "\n"
              << std::endl;

    MetricsSampler metricsSampler(cfg.metricsIntervalMs);
    metricsSampler.Start();

    pid_t perfPid = -1;

    std::cout << "\n[2/3] Forking " << cfg.processCount << " client processes..." << std::endl;
    std::vector<pid_t> clientPids;
    std::vector<int> barrierFds;
    std::vector<SampleFile> sampleFiles;
    for (int i = 0; i < cfg.processCount; ++i) {
        if (gStopSignal != 0) {
            break;
        }
        // Each worker can publish after timing stops, while the coordinator waits at the reporting barrier.
        const int sampleFd = memfd_create("bench-latency", MFD_CLOEXEC);
        SampleFile sampleFile(sampleFd < 0 ? nullptr : fdopen(sampleFd, "w+b"), std::fclose);
        if (!sampleFile) {
            std::cerr << "Failed to open client latency sample file: " << strerror(errno) << std::endl;
            if (sampleFd >= 0) {
                close(sampleFd);
            }
            break;
        }
        int controlSocket[bench::kBarrierSocketCount];
        if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, controlSocket) != 0) {
            std::cerr << "Failed to create client barrier channel: " << strerror(errno) << std::endl;
            break;
        }
        pid_t pid = fork();
        if (pid == 0) {
            PrepareChildProcess(coordinatorPid);
            if (cfg.profileControlFd >= 0) {
                close(cfg.profileControlFd);
            }
            SetProcessName(kClientProcNamePrefix + std::to_string(i), processTitle, processTitleSize);
            close(controlSocket[bench::kBarrierParentEnd]);
            for (int barrierFd : barrierFds) {
                close(barrierFd);
            }
            sampleFiles.clear();
            int ret = RunClient(cfg, i, &sharedResults->clients[i], metaUrl, cfg.logFile,
                                controlSocket[bench::kBarrierClientEnd], queueDepthMonitor, sampleFile.get());
            close(controlSocket[bench::kBarrierClientEnd]);
            _exit(ret);
        }
        close(controlSocket[bench::kBarrierClientEnd]);
        if (pid < 0) {
            close(controlSocket[bench::kBarrierParentEnd]);
            std::cerr << "Failed to fork client " << i << "!" << std::endl;
            break;
        } else {
            clientPids.push_back(pid);
            barrierFds.push_back(controlSocket[bench::kBarrierParentEnd]);
            sampleFiles.push_back(std::move(sampleFile));
        }
    }

    bench::NetWorkerStats netWorkerStats(metaPid);
    std::optional<int64_t> startRssKiB;
    std::optional<int64_t> endRssKiB;
    const auto startProfiling = [&]() {
        if (gStopSignal != 0) {
            return false;
        }
        // Every client has finished warmup and transport setup and is waiting for START.
        std::vector<pid_t> targets = clientPids;
        if (cfg.perfTarget == PerfTarget::kMeta) {
            targets = {metaPid};
        } else if (cfg.perfTarget == PerfTarget::kClient0) {
            targets = {clientPids.front()};
        }
        if (cfg.perfEnable) {
            perfPid = StartPerfChecked(targets, cfg);
        }
        if (cfg.profileControlFd >= 0) {
            // Include target process IDs so the runner can capture every named pool thread.
            std::string request = "R " + JoinPids(targets);
            ssize_t sent;
            do {
                sent = send(cfg.profileControlFd, request.data(), request.size(), MSG_NOSIGNAL);
            } while (sent < 0 && errno == EINTR);
            if (sent != static_cast<ssize_t>(request.size()) ||
                !bench::ReceiveBarrierMessage(cfg.profileControlFd, bench::BarrierMessage::START, &gStopSignal)) {
                return false;
            }
        }
        queueDepthMonitor.Start();
        netWorkerStats.Start();
        startRssKiB = ReadRssKiB(metaPid);
        return true;
    };
    const auto stopProfiling = [&]() {
        if (perfPid <= 0) {
            return true;
        }
        StopPerf(perfPid);
        perfPid = -1;
        TermLog("[perf] data saved to %s (view: perf report -i %s)\n", kDefaultPerfDataFile, kDefaultPerfDataFile);
        return true;
    };
    const auto finishProfiling = [&]() {
        endRssKiB = ReadRssKiB(metaPid);
        netWorkerStats.Stop();
        queueDepthMonitor.Stop();
        stopProfiling();
        // Ask the runner to freeze its collection before releasing any client to disconnect.
        bool runnerStopped = cfg.profileControlFd < 0 ||
                             bench::WaitForBarrier(cfg.profileControlFd, bench::BarrierMessage::DRAINED,
                                                   bench::BarrierMessage::TEARDOWN, &gStopSignal);
        return runnerStopped;
    };

    std::cout << "\n[3/3] Waiting for " << clientPids.size() << " clients to finish..." << std::endl;
    bool clientsFailed = clientPids.size() != static_cast<size_t>(cfg.processCount);
    if (!clientsFailed) {
        clientsFailed = !bench::CoordinateClients(barrierFds, startProfiling, finishProfiling, &gStopSignal);
        if (!clientsFailed) {
            // Publishing latency samples may take longer than the child shutdown timeout.
            clientsFailed = !bench::CoordinateBarrier(barrierFds, bench::BarrierMessage::READY,
                                                     bench::BarrierMessage::START, {}, &gStopSignal);
        }
    }
    queueDepthMonitor.Stop();
    // Also stop an active recorder if coordination failed before every client drained.
    stopProfiling();
    if (cfg.profileControlFd >= 0) {
        close(cfg.profileControlFd);
    }
    if (clientsFailed) {
        std::cerr << "Client coordination failed; aborting benchmark." << std::endl;
    }
    for (int barrierFd : barrierFds) {
        close(barrierFd);
    }
    // Reap every child even when an earlier child failed or shutdown was interrupted.
    if (!ReapChildren(clientPids, clientsFailed ? SIGTERM : 0)) {
        clientsFailed = true;
    }

    metricsSampler.Stop();

    if (!ReapChildren({metaPid}, SIGTERM)) {
        clientsFailed = true;
    }

    std::vector<ProcessResult> results;
    ProcessLatencies latencies;
    for (size_t i = 0; i < sampleFiles.size(); ++i) {
        if (sharedResults->clients[i].valid) {
            if (!latencies.MergeSamples(sampleFiles[i].get())) {
                std::cerr << "Failed to read latency samples from client " << i << std::endl;
                clientsFailed = true;
                results.clear();
                break;
            }
            results.push_back(sharedResults->clients[i]);
        }
        sampleFiles[i].reset();
    }

    if (!results.empty()) {
        PrintAggReport(results, latencies, cfg);
    } else {
        std::cerr << "No valid results collected!" << std::endl;
    }
    netWorkerStats.PrintReport(std::cout);
    PrintRssDelta(startRssKiB, endRssKiB);
    metricsSampler.PrintReport();

    munmap(sharedResults, shmSize);
    std::cout << (clientsFailed ? "Benchmark failed." : "Benchmark finished.") << std::endl;
    return gStopSignal != 0 ? kSignalExitCodeBase + gStopSignal : (clientsFailed || results.empty() ? 1 : 0);
}
