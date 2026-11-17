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

#include <algorithm>
#include <array>
#include <atomic>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <netinet/in.h>
#include <random>
#include <sstream>
#include <string>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "bench_async_transport.h"
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
constexpr int kMetricsPort = 16080;
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
constexpr int kPercentileP50 = 50;
constexpr int kPercentileP95 = 95;
constexpr int kPercentileP99 = 99;
constexpr int kPercentileP999 = 999;
constexpr int kPercentileP999Divisor = 1000;
constexpr size_t kLatencyHistogramBucketCount = 64;
constexpr int kTableWidth = 100;
constexpr int kMetaServiceReadyWaitSec = 3;
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

int gTermFd = STDERR_FILENO;

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
    int perfFreq = kDefaultPerfFreq;
    int metricsIntervalMs = kDefaultMetricsIntervalMs;
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

struct LatencyHistogram {
    std::array<uint64_t, kLatencyHistogramBucketCount> buckets{};
    uint64_t count{0};
    double totalUs{0.0};
    double minUs{0.0};
    double maxUs{0.0};

    void Record(double us)
    {
        const uint64_t value = static_cast<uint64_t>(std::max(0.0, us));
        size_t bucket = 0;
        uint64_t upper = 1;
        while (bucket + 1 < buckets.size() && value > upper) {
            upper <<= 1;
            ++bucket;
        }
        ++buckets[bucket];
        ++count;
        totalUs += us;
        minUs = count == 1 ? us : std::min(minUs, us);
        maxUs = std::max(maxUs, us);
    }

    void MergeFrom(const LatencyHistogram &other)
    {
        for (size_t i = 0; i < buckets.size(); ++i) {
            buckets[i] += other.buckets[i];
        }
        if (other.count > 0) {
            minUs = count == 0 ? other.minUs : std::min(minUs, other.minUs);
            maxUs = std::max(maxUs, other.maxUs);
        }
        count += other.count;
        totalUs += other.totalUs;
    }

    double Percentile(int numerator, int denominator) const
    {
        if (count == 0) {
            return 0.0;
        }
        const uint64_t rank =
            (count * static_cast<uint64_t>(numerator) + denominator - 1) / static_cast<uint64_t>(denominator);
        uint64_t cumulative = 0;
        uint64_t upper = 1;
        for (size_t i = 0; i < buckets.size(); ++i) {
            cumulative += buckets[i];
            if (cumulative >= rank) {
                return static_cast<double>(upper);
            }
            upper <<= 1;
        }
        return maxUs;
    }
};

struct LatencySamples {
    std::vector<double> samplesUs;
    void Record(double us)
    {
        samplesUs.push_back(us);
    }
    void MergeFrom(const LatencySamples &o)
    {
        samplesUs.insert(samplesUs.end(), o.samplesUs.begin(), o.samplesUs.end());
    }
    void ComputeStats(double &avg, double &p50, double &p95, double &p99, double &mn, double &mx) const
    {
        if (samplesUs.empty()) {
            avg = p50 = p95 = p99 = mn = mx = 0.0;
            return;
        }
        std::vector<double> s = samplesUs;
        std::sort(s.begin(), s.end());
        double sum = 0.0;
        for (double v : s)
            sum += v;
        avg = sum / static_cast<double>(s.size());
        mn = s.front();
        mx = s.back();
        auto pct = [&](int p) -> double {
            size_t i = static_cast<size_t>(static_cast<int>(s.size()) * p / kPercentileDivisor);
            return s[std::min(i, s.size() - 1)];
        };
        p50 = pct(kPercentileP50);
        p95 = pct(kPercentileP95);
        p99 = pct(kPercentileP99);
    }
};

struct ThreadStats {
    uint64_t successCount[kOpTypeCount] = {0};
    uint64_t failureCount[kOpTypeCount] = {0};
    LatencySamples latency[kOpTypeCount];
    LatencyHistogram histogram[kOpTypeCount];
    LatencyHistogram submission[kOpTypeCount];
    uint64_t transportFailures[kOpTypeCount]{};
    uint64_t timeouts[kOpTypeCount]{};
};

struct PerOpResult {
    uint64_t successCount;
    uint64_t failureCount;
    double avgUs;
    double p50Us;
    double p95Us;
    double p99Us;
    double minUs;
    double maxUs;
    LatencyHistogram histogram;
    LatencyHistogram submission;
    uint64_t transportFailures{};
    uint64_t timeouts{};
};

struct ProcessResult {
    int rank;
    int pid;
    int valid;
    PerOpResult ops[kOpTypeCount];
    double durationSec;
    int padding[8];
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

inline std::string MakeKey(int index)
{
    return "bench_key_" + std::to_string(index);
}
inline std::string MakePutKey(uint32_t rank, int tid, uint64_t counter)
{
    return "bench_p" + std::to_string(rank) + "_t" + std::to_string(tid) + "_" + std::to_string(counter);
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

void PrintAggReport(const std::vector<ProcessResult> &results, const BenchConfig &cfg)
{
    ThreadStats total;
    double duration = 0;
    for (const auto &process : results) {
        duration = std::max(duration, process.durationSec);
        for (int i = 0; i < kOpTypeCount; ++i) {
            total.successCount[i] += process.ops[i].successCount;
            total.failureCount[i] += process.ops[i].failureCount;
            total.histogram[i].MergeFrom(process.ops[i].histogram);
            total.submission[i].MergeFrom(process.ops[i].submission);
            total.transportFailures[i] += process.ops[i].transportFailures;
            total.timeouts[i] += process.ops[i].timeouts;
        }
    }
    std::cout << "\nBenchmark report: queue depth/thread=" << cfg.queueDepth
              << ", completion mode=" << (cfg.waitCompletions ? "wait" : "poll")
              << ", maximum logical concurrency=" << static_cast<uint64_t>(cfg.processCount) * cfg.threadCount * cfg.queueDepth << "\n";
    auto print = [](const LatencyHistogram &h) {
        std::cout << " samples=" << h.count << " avg=" << (h.count ? h.totalUs / h.count : 0)
                  << " P50=" << h.Percentile(kPercentileP50, kPercentileDivisor)
                  << " P95=" << h.Percentile(kPercentileP95, kPercentileDivisor)
                  << " P99=" << h.Percentile(kPercentileP99, kPercentileDivisor)
                  << " P999=" << h.Percentile(kPercentileP999, kPercentileP999Divisor)
                  << " min=" << h.minUs << " max=" << h.maxUs << "\n";
    };
    std::cout << "Client-observed RPC round-trip latency (us)\n";
    for (auto type : {kOpPut, kOpAlloc, kOpUpdate, kOpGet}) {
        std::cout << OpTypeName(type) << " success=" << total.successCount[type]
                  << " fail=" << total.failureCount[type];
        if (type == kOpPut) std::cout << " latency=—\n";
        else {
            std::cout << " transport failures=" << total.transportFailures[type] << " timeouts=" << total.timeouts[type];
            print(total.histogram[type]);
        }
    }
    std::cout << "Submission duration (us; overlaps RPC latency)\n";
    for (auto type : {kOpAlloc, kOpUpdate, kOpGet}) {
        std::cout << OpTypeName(type);
        print(total.submission[type]);
    }
    const uint64_t count = total.successCount[kOpPut] + total.failureCount[kOpPut] +
        total.successCount[kOpGet] + total.failureCount[kOpGet];
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
    return client.SyncCall(req, resp, kRpcTimeoutMs);
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
    if (allocResp.result_ != MMC_OK || allocResp.blobs_.empty())
        return allocResp.result_;
    auto &blob = allocResp.blobs_[0];
    UpdateRequest updateReq(MMC_WRITE_OK, key, blob.rank_, blob.mediaType_, operateId);
    Response updateResp;
    return client.SyncCall(updateReq, updateResp, kRpcTimeoutMs);
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

void Worker(bench::AsyncTransport &transport, const BenchConfig &cfg, ThreadStats &stats, int tid, uint32_t rank)
{
    bench::CompletionQueue queue;
    std::unordered_map<uint32_t, PendingOperation> pending;
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
                operation.keys.push_back(operation.put ? MakePutKey(rank, tid, putCounter++) : MakeKey(readDist(rng)));
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
        }
    }
}

void WriteResult(ProcessResult &out, const ThreadStats &stats, double durationSec, int rank, int pid)
{
    out.rank = rank;
    out.pid = pid;
    out.durationSec = durationSec;
    for (int i = 0; i < kOpTypeCount; ++i) {
        out.ops[i].successCount = stats.successCount[i];
        out.ops[i].failureCount = stats.failureCount[i];
        stats.latency[i].ComputeStats(out.ops[i].avgUs, out.ops[i].p50Us, out.ops[i].p95Us, out.ops[i].p99Us,
                                      out.ops[i].minUs, out.ops[i].maxUs);
        out.ops[i].histogram = stats.histogram[i];
        out.ops[i].submission = stats.submission[i];
        out.ops[i].transportFailures = stats.transportFailures[i];
        out.ops[i].timeouts = stats.timeouts[i];
    }
    out.valid = 1;
}

pid_t StartPerfOnMetaService(int metaPid, const BenchConfig &cfg)
{
    std::string metaPidStr = std::to_string(metaPid);
    std::string freqStr = std::to_string(cfg.perfFreq);
    pid_t pid = fork();
    if (pid != 0) {
        return pid;
    }
    execlp("perf", "perf", "record", "-F", freqStr.c_str(), "-g", "-p", metaPidStr.c_str(), "-o", kDefaultPerfDataFile,
           nullptr);
    TermLog("[perf] failed to launch perf: %s\n", strerror(errno));
    _exit(kPerfExecFailCode);
}

void StopPerf(pid_t perfPid)
{
    if (perfPid <= 0) {
        return;
    }
    kill(perfPid, SIGINT);
    for (int i = 0; i < kPerfStopTimeoutSec; ++i) {
        int status = 0;
        pid_t r = waitpid(perfPid, &status, WNOHANG);
        if (r == perfPid || r == -1) {
            return;
        }
        sleep(1);
    }
    kill(perfPid, SIGKILL);
    int status = 0;
    waitpid(perfPid, &status, 0);
}

int RunMetaService(const std::string &metaUrl, const std::string &bmUrl, const std::string &metricsUrl,
                   const std::string &logFile, uint64_t capacityMB)
{
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
    TermLog("[meta_service] Started at %s\n", metaUrl.c_str());

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    pthread_sigmask(SIG_BLOCK, &mask, nullptr);
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
              const std::string &logFile)
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

    int warmupStart = static_cast<int>(static_cast<int64_t>(clientIndex) * cfg.warmupKeyCount / cfg.processCount);
    int warmupEnd = static_cast<int>(static_cast<int64_t>(clientIndex + 1) * cfg.warmupKeyCount / cfg.processCount);
    int warmupPerClient = warmupEnd - warmupStart;
    for (int i = 0; i < warmupPerClient; ++i) {
        std::string key = MakeKey(warmupStart + i);
        DoPut(*client.Get(), key, rank, cfg.blobSize);
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
    double startUs = NowUs();
    for (int t = 0; t < cfg.threadCount; ++t) {
        workers.emplace_back(Worker, std::ref(transport), std::ref(cfg),
                             std::ref(threadStats[static_cast<size_t>(t)]), t, rank);
    }
    for (auto &w : workers)
        w.join();
    double endUs = NowUs();
    double durationSec = (endUs - startUs) / static_cast<double>(kMicroSecPerSec);

    ThreadStats merged;
    for (const auto &ts : threadStats) {
        for (int i = 0; i < kOpTypeCount; ++i) {
            merged.successCount[i] += ts.successCount[i];
            merged.failureCount[i] += ts.failureCount[i];
            merged.latency[i].MergeFrom(ts.latency[i]);
            merged.histogram[i].MergeFrom(ts.histogram[i]);
            merged.submission[i].MergeFrom(ts.submission[i]);
            merged.transportFailures[i] += ts.transportFailures[i];
            merged.timeouts[i] += ts.timeouts[i];
        }
    }

    WriteResult(*resultSlot, merged, durationSec, static_cast<int>(rank), getpid());

    uint64_t totalSucc = 0, totalFail = 0;
    for (int i = 0; i < kOpAlloc; ++i) {
        totalSucc += merged.successCount[i];
        totalFail += merged.failureCount[i];
    }
    TermLog("[client %d] Done: %llu succ, %llu fail, %.2fs\n", clientIndex, static_cast<unsigned long long>(totalSucc),
            static_cast<unsigned long long>(totalFail), durationSec);

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
                << kDefaultMetricsIntervalMs << ")\n";
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
        else {
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

    gTermFd = dup(STDERR_FILENO);
    if (gTermFd < 0)
        gTermFd = STDERR_FILENO;

    FILE *truncateLog = fopen(cfg.logFile.c_str(), "w");
    if (truncateLog)
        fclose(truncateLog);

    std::cout << "============================================================\n"
              << "  MetaService Multi-Process Benchmark (Put/Get)\n"
              << "============================================================\n"
              << "Configuration:\n"
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
              << "  Perf:          "
              << (cfg.perfEnable ? std::string("record -> ") + kDefaultPerfDataFile : std::string("off")) << "\n"
              << "  Metrics:       "
              << (cfg.metricsIntervalMs > 0 ? std::to_string(cfg.metricsIntervalMs) + " ms" : std::string("off"))
              << "\n"
              << std::endl;

    std::string metaUrl = "tcp://127.0.0.1:" + std::to_string(kMetaServicePort);
    std::string bmUrl = "tcp://127.0.0.1:" + std::to_string(kConfigStorePort);
    std::string metricsUrl = "http://127.0.0.1:" + std::to_string(kMetricsPort);

    size_t shmSize = sizeof(ProcessResult) * static_cast<size_t>(cfg.processCount);
    auto *sharedResults =
        static_cast<ProcessResult *>(mmap(nullptr, shmSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
    if (sharedResults == MAP_FAILED) {
        std::cerr << "Failed to create shared memory!" << std::endl;
        return 1;
    }
    std::memset(sharedResults, 0, shmSize);

    std::cout << "[1/3] Starting meta_service process..." << std::endl;
    pid_t metaPid = fork();
    if (metaPid == 0) {
        return RunMetaService(metaUrl, bmUrl, metricsUrl, cfg.logFile, cfg.capacityMB);
    }
    if (metaPid < 0) {
        std::cerr << "Failed to fork meta_service!" << std::endl;
        munmap(sharedResults, shmSize);
        return 1;
    }
    sleep(kMetaServiceReadyWaitSec);
    std::cout << "meta_service started (pid=" << metaPid << ")" << std::endl;

    MetricsSampler metricsSampler(cfg.metricsIntervalMs);
    metricsSampler.Start();

    pid_t perfPid = -1;
    if (cfg.perfEnable) {
        perfPid = StartPerfOnMetaService(metaPid, cfg);
        if (perfPid > 0) {
            sleep(kPerfReadyWaitSec);
            int perfStatus = 0;
            pid_t r = waitpid(perfPid, &perfStatus, WNOHANG);
            if (r == perfPid || r == -1) {
                TermLog("[perf] perf exited early (perf installed? permissions?), profiling disabled\n");
                perfPid = -1;
            } else {
                TermLog("[perf] profiling meta_service (pid=%d) -> %s\n", metaPid, kDefaultPerfDataFile);
            }
        } else {
            TermLog("[perf] failed to fork perf process, profiling disabled\n");
            perfPid = -1;
        }
    }

    std::cout << "\n[2/3] Forking " << cfg.processCount << " client processes..." << std::endl;
    std::vector<pid_t> clientPids;
    for (int i = 0; i < cfg.processCount; ++i) {
        pid_t pid = fork();
        if (pid == 0) {
            int ret = RunClient(cfg, i, &sharedResults[i], metaUrl, cfg.logFile);
            _exit(ret);
        }
        if (pid < 0) {
            std::cerr << "Failed to fork client " << i << "!" << std::endl;
        } else {
            clientPids.push_back(pid);
        }
    }

    std::cout << "\n[3/3] Waiting for " << clientPids.size() << " clients to finish..." << std::endl;
    bool clientsFailed = clientPids.size() != static_cast<size_t>(cfg.processCount);
    for (pid_t pid : clientPids) {
        int status = 0;
        if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) clientsFailed = true;
    }

    metricsSampler.Stop();

    if (perfPid > 0) {
        StopPerf(perfPid);
        TermLog("[perf] data saved to %s (view: perf report -i %s)\n", kDefaultPerfDataFile, kDefaultPerfDataFile);
    }

    kill(metaPid, SIGTERM);
    int metaStatus = 0;
    waitpid(metaPid, &metaStatus, 0);

    std::vector<ProcessResult> results;
    for (int i = 0; i < cfg.processCount; ++i) {
        if (sharedResults[i].valid) {
            results.push_back(sharedResults[i]);
        }
    }

    if (!results.empty()) {
        PrintAggReport(results, cfg);
    } else {
        std::cerr << "No valid results collected!" << std::endl;
    }
    metricsSampler.PrintReport();

    munmap(sharedResults, shmSize);
    std::cout << (clientsFailed ? "Benchmark failed." : "Benchmark finished.") << std::endl;
    return clientsFailed || results.empty() ? 1 : 0;
}
