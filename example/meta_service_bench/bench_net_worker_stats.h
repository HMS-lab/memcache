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

#ifndef BENCH_NET_WORKER_STATS_H
#define BENCH_NET_WORKER_STATS_H

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace bench {
class NetWorkerStats {
public:
    explicit NetWorkerStats(pid_t metaPid) : taskPath_("/proc/" + std::to_string(metaPid) + "/task") {}

    void Start()
    {
        std::error_code error;
        for (std::filesystem::directory_iterator entry(taskPath_, error), end; entry != end && !error;
             entry.increment(error)) {
            const auto &path = entry->path();
            std::string name;
            std::getline(std::ifstream(path / "comm"), name);
            for (const auto &pool : kPools) {
                if (name.compare(0, pool.prefix.size(), pool.prefix) != 0 || name.size() == pool.prefix.size() ||
                    name.find_first_not_of("0123456789", pool.prefix.size()) != std::string::npos) {
                    continue;
                }
                if (auto start = Sample(path)) {
                    workers_.push_back({path, pool.index, *start});
                }
                break;
            }
        }
    }

    void Stop()
    {
        for (const auto &worker : workers_) {
            if (auto end = Sample(worker.path); end && end->startTicks == worker.start.startTicks) {
                AddSample(rows_[worker.pool], worker.start, *end);
            }
        }
        completed_ = true;
    }

    void PrintReport(std::ostream &output) const
    {
        if (!completed_) {
            return;
        }
        std::ostringstream report;
        report.imbue(std::locale::classic());
        report << "\nMetaService network worker statistics (across workers)\n"
               << std::left << std::setw(kPoolWidth) << "Pool" << std::setw(kMetricWidth) << "Metric"
               << std::right << std::setw(kColumnWidth) << "Workers" << std::setw(kColumnWidth) << "Avg"
               << std::setw(kColumnWidth) << "Stddev" << '\n'
               << std::fixed << std::setprecision(kReportPrecision);
        for (const auto &pool : kPools) {
            for (size_t i = 0; i < kMetricNames.size(); ++i) {
                const auto &metric = rows_[pool.index][i];
                report << std::left << std::setw(kPoolWidth) << pool.label << std::setw(kMetricWidth) << kMetricNames[i]
                       << std::right << std::setw(kColumnWidth) << metric.count;
                if (metric.count == 0) {
                    report << ' ' << std::setw(kColumnWidth) << "N/A" << ' ' << std::setw(kColumnWidth) << "N/A";
                } else {
                    report << ' ' << std::setw(kColumnWidth) << metric.mean
                           << ' ' << std::setw(kColumnWidth) << std::sqrt(metric.squaredDeviation / metric.count);
                }
                report << '\n';
            }
        }
        output << report.str();
    }

private:
    using Clock = std::chrono::steady_clock;

    enum PoolIndex { ASYNC, TCP, POOL_COUNT };
    struct Pool {
        PoolIndex index;
        const char *label;
        std::string_view prefix;
    };
    static constexpr std::array<Pool, POOL_COUNT> kPools{{
        {ASYNC, "net_pool", "net_pool"},
        {TCP, "AccWrk", "AccWrk:"},
    }};

    enum MetricIndex { CPU, USER, KERNEL, RUNNABLE, SLEEP, VOLUNTARY, INVOLUNTARY, METRIC_COUNT };
    static constexpr std::array<const char *, METRIC_COUNT> kMetricNames{{
        "cpu_pct", "user_cpu_pct", "kernel_cpu_pct", "runnable_wait_pct", "sleep_estimate_pct",
        "voluntary_switches", "involuntary_switches",
    }};
    static constexpr int kPoolWidth = 12;
    static constexpr int kMetricWidth = 24;
    static constexpr int kColumnWidth = 16;
    static constexpr int kReportPrecision = 6;
    static constexpr double kPercent = 100.0;
    // /proc/<pid>/task/<tid>/stat field numbers (one-based).
    static constexpr int kStateField = 3;
    static constexpr int kUserTimeField = 14;
    static constexpr int kAfterKernelTimeField = 16;
    static constexpr int kStartTimeField = 22;

    struct CounterSample {
        Clock::time_point cpuTime;
        Clock::time_point schedulerTime;
        uint64_t startTicks = 0;
        uint64_t userTicks = 0;
        uint64_t kernelTicks = 0;
        uint64_t runtimeNs = 0;
        uint64_t runQueueNs = 0;
        uint64_t timeslices = 0;
        bool schedulerReadable = false;
        std::optional<uint64_t> voluntarySwitches;
        std::optional<uint64_t> involuntarySwitches;
    };

    struct Worker {
        std::filesystem::path path;
        PoolIndex pool;
        CounterSample start;
    };

    struct Metric {
        size_t count = 0;
        double mean = 0;
        double squaredDeviation = 0;

        void Add(double value)
        {
            const double difference = value - mean;
            mean += difference / ++count;
            squaredDeviation += difference * (value - mean);
        }
    };

    static std::optional<CounterSample> Sample(const std::filesystem::path &path)
    {
        CounterSample sample;
        std::string line;
        std::getline(std::ifstream(path / "stat"), line);
        sample.cpuTime = Clock::now();
        // The thread name is parenthesized and may itself contain whitespace or parentheses.
        const auto commEnd = line.rfind(')');
        if (commEnd == std::string::npos) {
            return std::nullopt;
        }
        std::istringstream stat(line.substr(commEnd + 1));
        std::string field;
        for (int i = kStateField; i < kUserTimeField; ++i) {
            stat >> field;
        }
        stat >> sample.userTicks >> sample.kernelTicks;
        for (int i = kAfterKernelTimeField; i < kStartTimeField; ++i) {
            stat >> field;
        }
        if (!(stat >> sample.startTicks)) {
            return std::nullopt;
        }

        std::ifstream schedstat(path / "schedstat");
        sample.schedulerReadable = bool(schedstat >> sample.runtimeNs >> sample.runQueueNs >> sample.timeslices);
        sample.schedulerTime = Clock::now();

        std::ifstream status(path / "status");
        while (std::getline(status, line)) {
            std::istringstream fields(line);
            std::string key;
            uint64_t value = 0;
            fields >> key;
            if (key == "voluntary_ctxt_switches:" && fields >> value) {
                sample.voluntarySwitches = value;
            } else if (key == "nonvoluntary_ctxt_switches:" && fields >> value) {
                sample.involuntarySwitches = value;
            }
        }
        return sample;
    }

    static void AddSwitches(Metric &metric, std::optional<uint64_t> start, std::optional<uint64_t> end)
    {
        if (start && end && *end >= *start) {
            metric.Add(*end - *start);
        }
    }

    void AddSample(std::array<Metric, METRIC_COUNT> &metrics, const CounterSample &start,
                   const CounterSample &end) const
    {
        const double elapsedTicks = std::chrono::duration<double>(end.cpuTime - start.cpuTime).count() * ticksPerSec_;
        if (elapsedTicks > 0 && end.userTicks >= start.userTicks && end.kernelTicks >= start.kernelTicks) {
            const double user = kPercent * (end.userTicks - start.userTicks) / elapsedTicks;
            const double kernel = kPercent * (end.kernelTicks - start.kernelTicks) / elapsedTicks;
            metrics[USER].Add(user);
            metrics[KERNEL].Add(kernel);
            metrics[CPU].Add(user + kernel);
        }

        const double elapsedNs = std::chrono::duration<double, std::nano>(end.schedulerTime - start.schedulerTime)
                                     .count();
        // Some kernels expose schedstat with the scheduling count left at zero when accounting is disabled.
        if (elapsedNs > 0 && start.schedulerReadable && end.schedulerReadable && end.timeslices > 0 &&
            end.timeslices >= start.timeslices && end.runtimeNs >= start.runtimeNs &&
            end.runQueueNs >= start.runQueueNs) {
            const double runtimeNs = end.runtimeNs - start.runtimeNs;
            const double runQueueNs = end.runQueueNs - start.runQueueNs;
            metrics[RUNNABLE].Add(kPercent * runQueueNs / elapsedNs);
            metrics[SLEEP].Add(kPercent * std::max(0.0, elapsedNs - runtimeNs - runQueueNs) / elapsedNs);
        }

        AddSwitches(metrics[VOLUNTARY], start.voluntarySwitches, end.voluntarySwitches);
        AddSwitches(metrics[INVOLUNTARY], start.involuntarySwitches, end.involuntarySwitches);
    }

    const std::filesystem::path taskPath_;
    const long ticksPerSec_ = sysconf(_SC_CLK_TCK);
    std::vector<Worker> workers_;
    std::array<std::array<Metric, METRIC_COUNT>, POOL_COUNT> rows_{};
    bool completed_ = false;
};
} // namespace bench

#endif
