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

#include "bench_latency.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

namespace {
constexpr size_t kTestSampleCount = 2500;
constexpr double kSampleStepUs = 0.015625;
constexpr double kBoundaryUs = 64;
constexpr double kBoundaryOffsetUs = 0.25;
constexpr double kLargeSampleUs = 1e30;
constexpr int kMedianDivisor = 2;
constexpr double kComparisonTolerance = 1e-12;

void Check(bool condition)
{
    if (!condition) {
        std::cerr << "Latency test failed\n";
        std::abort();
    }
}

void CheckNear(double actual, double expected)
{
    const double scale = std::max({1.0, std::abs(actual), std::abs(expected)});
    Check(std::abs(actual - expected) <= kComparisonTolerance * scale);
}

// Empty and singleton distributions return zero or the original fractional sample, respectively.
void TestEmptyAndSingleton()
{
    bench::LatencyHistogram samples;
    samples.ReserveBuckets();
    Check(samples.Summarize().count == 0);
    CheckNear(samples.Summarize().p999Us, 0);
    samples.Record(kBoundaryUs + kBoundaryOffsetUs);
    const auto summary = samples.Summarize();
    Check(summary.count == 1);
    CheckNear(summary.p50Us, kBoundaryUs + kBoundaryOffsetUs);
    CheckNear(summary.p999Us, summary.maxUs);
    CheckNear(summary.minUs, summary.maxUs);
    CheckNear(summary.avgUs, summary.minUs);
}

// Merging thread-local samples and growing buckets preserves exact ranks across all four percentiles.
void TestMergedPercentiles()
{
    bench::LatencyHistogram first;
    bench::LatencyHistogram second;
    first.ReserveBuckets();
    second.ReserveBuckets();
    std::vector<double> expected;
    for (size_t i = kTestSampleCount; i > 0; --i) {
        const double sample = kBoundaryUs + i * kSampleStepUs;
        (i % kMedianDivisor ? first : second).Record(sample);
        expected.push_back(sample);
    }
    first.MergeFrom(second);
    Check(second.count == 0);
    std::sort(expected.begin(), expected.end());
    for (int percentile : {bench::kPercentileP50, bench::kPercentileP95, bench::kPercentileP99}) {
        const size_t rank = (expected.size() * percentile + bench::kPercentileDivisor - 1) / bench::kPercentileDivisor;
        CheckNear(first.Percentile(percentile, bench::kPercentileDivisor), expected[rank - 1]);
    }
    const size_t rank = (expected.size() * bench::kPercentileP999 + bench::kPercentileP999Divisor - 1) /
        bench::kPercentileP999Divisor;
    CheckNear(first.Summarize().p999Us, expected[rank - 1]);
    Check(first.Summarize().count == expected.size());
    CheckNear(first.Summarize().avgUs, (expected.front() + expected.back()) / kMedianDivisor);
    CheckNear(first.Summarize().minUs, expected.front());
    CheckNear(first.Summarize().maxUs, expected.back());
}

// Bucket boundaries, duplicate samples, and the overflow bucket retain their original values.
void TestBoundariesAndOverflow()
{
    bench::LatencyHistogram samples;
    for (double sample : {kLargeSampleUs, kBoundaryUs + kBoundaryOffsetUs, kBoundaryUs,
                         kBoundaryUs - kBoundaryOffsetUs, kBoundaryUs}) {
        samples.Record(sample);
    }
    CheckNear(samples.Summarize().p50Us, kBoundaryUs);
    CheckNear(samples.Summarize().p999Us, kLargeSampleUs);
    // Adding observations after a query must invalidate any sorted bucket.
    constexpr int extraSamples = 3;
    for (int i = 0; i < extraSamples; ++i) {
        samples.Record(kBoundaryUs - kMedianDivisor * kBoundaryOffsetUs);
    }
    CheckNear(samples.Summarize().p50Us, kBoundaryUs - kBoundaryOffsetUs);
}

// Two processes with unequal sample counts must yield exact pooled ranks after transferring their samples.
void TestProcessSampleMerge()
{
    constexpr size_t lowCount = 1000;
    constexpr size_t highCount = 5;
    constexpr double lowUs = kBoundaryUs + kBoundaryOffsetUs;
    constexpr double highUs = kBoundaryUs + kMedianDivisor * kBoundaryOffsetUs;
    bench::LatencyHistogram pooled;
    for (const auto &[us, count] : {std::pair{lowUs, lowCount}, std::pair{highUs, highCount}}) {
        const int fd = memfd_create("bench-latency-test", MFD_CLOEXEC);
        Check(fd >= 0);
        std::FILE *file = fdopen(fd, "w+b");
        Check(file != nullptr);
        const pid_t pid = fork();
        Check(pid >= 0);
        if (pid == 0) {
            bench::LatencyHistogram worker;
            for (size_t i = 0; i < count; ++i) {
                worker.Record(us);
            }
            _exit(worker.WriteSamples(file) && std::fflush(file) == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
        }
        int status = 0;
        Check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS);
        Check(std::fseek(file, 0, SEEK_SET) == 0);
        Check(pooled.MergeSamples(file));
        Check(std::fclose(file) == 0);
    }
    const auto summary = pooled.Summarize();
    Check(summary.count == lowCount + highCount);
    CheckNear(summary.avgUs, (lowCount * lowUs + highCount * highUs) / summary.count);
    CheckNear(summary.p50Us, lowUs);
    CheckNear(summary.p95Us, lowUs);
    CheckNear(summary.p99Us, lowUs);
    CheckNear(summary.p999Us, highUs);
    CheckNear(summary.minUs, lowUs);
    CheckNear(summary.maxUs, highUs);
}
} // namespace

// All percentile and sample merge scenarios must pass.
int main()
{
    TestEmptyAndSingleton();
    TestMergedPercentiles();
    TestBoundariesAndOverflow();
    TestProcessSampleMerge();
    std::cout << "Latency tests passed\n";
}
