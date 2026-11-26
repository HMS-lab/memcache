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

#ifndef MEMCACHE_BENCH_LATENCY_H
#define MEMCACHE_BENCH_LATENCY_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <numeric>
#include <vector>

namespace bench {
constexpr double kLatencyBucketGrowth = 2;
constexpr size_t kLatencyBucketCount = 64;
constexpr size_t kLatencyBucketInitialCapacity = 1024;
constexpr int kPercentileDivisor = 100;
constexpr int kPercentileP50 = 50;
constexpr int kPercentileP95 = 95;
constexpr int kPercentileP99 = 99;
constexpr int kPercentileP999 = 999;
constexpr int kPercentileP999Divisor = 1000;

struct LatencySummary {
    uint64_t count{0};
    double avgUs{0};
    double p50Us{0};
    double p95Us{0};
    double p99Us{0};
    double p999Us{0};
    double minUs{0};
    double maxUs{0};
};

class LatencyHistogram {
public:
    uint64_t count{0};
    double totalUs{0};
    double minUs{0};
    double maxUs{0};

    void ReserveBuckets()
    {
        for (auto &bucket : buckets_) {
            bucket.reserve(kLatencyBucketInitialCapacity);
        }
    }

    void Record(double us)
    {
        size_t bucket = 0;
        double upper = 1;
        while (bucket + 1 < buckets_.size() && us > upper) {
            upper *= kLatencyBucketGrowth;
            ++bucket;
        }
        buckets_[bucket].push_back(us);
        sorted_[bucket] = false;
        ++count;
        totalUs += us;
        minUs = count == 1 ? us : std::min(minUs, us);
        maxUs = count == 1 ? us : std::max(maxUs, us);
    }

    // Called after workers stop; moving whole buckets avoids copying their samples.
    void MergeFrom(LatencyHistogram &other)
    {
        for (size_t i = 0; i < buckets_.size(); ++i) {
            auto &bucket = buckets_[i];
            auto &source = other.buckets_[i];
            if (bucket.empty()) {
                bucket.swap(source);
            } else {
                bucket.insert(bucket.end(), source.begin(), source.end());
            }
            sorted_[i] = false;
            source.clear();
        }
        if (other.count > 0) {
            minUs = count == 0 ? other.minUs : std::min(minUs, other.minUs);
            maxUs = count == 0 ? other.maxUs : std::max(maxUs, other.maxUs);
        }
        count += other.count;
        totalUs += other.totalUs;
        other.count = 0;
        other.totalUs = other.minUs = other.maxUs = 0;
    }

    bool WriteSamples(std::FILE *file) const
    {
        for (const auto &bucket : buckets_) {
            const size_t size = bucket.size();
            if (std::fwrite(&size, sizeof(size), 1, file) != 1 ||
                (size != 0 && std::fwrite(bucket.data(), sizeof(double), size, file) != size)) {
                return false;
            }
        }
        return true;
    }

    bool MergeSamples(std::FILE *file)
    {
        for (size_t i = 0; i < buckets_.size(); ++i) {
            auto &bucket = buckets_[i];
            size_t size = 0;
            if (std::fread(&size, sizeof(size), 1, file) != 1 || size > bucket.max_size() - bucket.size()) {
                return false;
            }
            if (size == 0) {
                continue;
            }
            const size_t offset = bucket.size();
            bucket.resize(offset + size);
            double *first = bucket.data() + offset;
            if (std::fread(first, sizeof(double), size, file) != size) {
                bucket.resize(offset);
                return false;
            }
            const auto bounds = std::minmax_element(first, first + size);
            minUs = count == 0 ? *bounds.first : std::min(minUs, *bounds.first);
            maxUs = count == 0 ? *bounds.second : std::max(maxUs, *bounds.second);
            totalUs = std::accumulate(first, first + size, totalUs);
            count += size;
            sorted_[i] = false;
        }
        return true;
    }

    double Percentile(int numerator, int denominator)
    {
        if (count == 0) {
            return 0;
        }
        // Nearest rank, computed without multiplying the full sample count.
        const uint64_t rank = (count / denominator) * numerator +
            ((count % denominator) * numerator + denominator - 1) / denominator;
        uint64_t cumulative = 0;
        for (size_t i = 0; i < buckets_.size(); ++i) {
            auto &bucket = buckets_[i];
            if (rank <= cumulative + bucket.size()) {
                if (rank == cumulative + bucket.size()) {
                    return *std::max_element(bucket.begin(), bucket.end());
                }
                if (!sorted_[i]) {
                    std::sort(bucket.begin(), bucket.end());
                    sorted_[i] = true;
                }
                return bucket[static_cast<size_t>(rank - cumulative - 1)];
            }
            cumulative += bucket.size();
        }
        return maxUs;
    }

    LatencySummary Summarize()
    {
        return {count, count ? totalUs / count : 0,
            Percentile(kPercentileP50, kPercentileDivisor), Percentile(kPercentileP95, kPercentileDivisor),
            Percentile(kPercentileP99, kPercentileDivisor), Percentile(kPercentileP999, kPercentileP999Divisor),
            minUs, maxUs};
    }

private:
    std::array<std::vector<double>, kLatencyBucketCount> buckets_;
    std::array<bool, kLatencyBucketCount> sorted_{};
};
} // namespace bench

#endif
