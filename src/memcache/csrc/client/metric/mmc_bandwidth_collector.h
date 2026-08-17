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

#ifndef MEM_FABRIC_MMC_BANDWIDTH_COLLECTOR_H
#define MEM_FABRIC_MMC_BANDWIDTH_COLLECTOR_H

#include <chrono>

#include "mmc_bandwidth_sampler.h"
#include "mmc_client_metric_collector.h"
#include "mmc_types.h"

namespace ock {
namespace mmc {

// 带宽指标 collector: 按 MetricOp::COUNT 持有独立 sampler 实例, Observe() 按操作类型分发
class BandwidthCollector : public MmcClientMetricCollector {
public:
    std::string Name() const override
    {
        return "bandwidth";
    }
    void Collect(ClientMetricSnapshot &out) const override;
    void Reset() override;

    void Observe(MetricOp op, uint64_t bytes, uint64_t durationUs)
    {
        if (op >= MetricOp::COUNT) {
            return;
        }
        samplers_[static_cast<size_t>(op)].Record(bytes, durationUs);
    }

private:
    MmcBandwidthSampler samplers_[static_cast<size_t>(MetricOp::COUNT)];
};

// 供 latency-only 操作(无数据搬运, 如 EXIST)的 guard 默认参数引用, 程序级生命期, 避免引用悬空
inline const std::vector<MmcBufferArray> kEmptyBufArrs;
inline const std::vector<int> kEmptyResults;

// RAII guard: 析构时自动计算耗时并按成功条目汇总字节, 上报到 BandwidthCollector
class BandwidthGuard {
public:
    BandwidthGuard(MetricOp op, BandwidthCollector *collector,
                   const std::vector<MmcBufferArray> &bufArrs = kEmptyBufArrs,
                   const std::vector<int> &results = kEmptyResults)
        : start_(std::chrono::steady_clock::now()), op_(op), collector_(collector), bufArrs_(bufArrs), results_(results)
    {}

    ~BandwidthGuard()
    {
        if (collector_ == nullptr) {
            return;
        }
        auto durationUs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start_).count());
        uint64_t totalBytes = 0;
        for (size_t i = 0; i < results_.size(); ++i) {
            if (results_[i] == MMC_OK) {
                totalBytes += bufArrs_[i].TotalSize();
            }
        }
        collector_->Observe(op_, totalBytes, durationUs);
    }

    BandwidthGuard(const BandwidthGuard &) = delete;
    BandwidthGuard &operator=(const BandwidthGuard &) = delete;

private:
    std::chrono::steady_clock::time_point start_;
    MetricOp op_;
    BandwidthCollector *collector_;
    const std::vector<MmcBufferArray> &bufArrs_;
    const std::vector<int> &results_;
};

} // namespace mmc
} // namespace ock

#endif // MEM_FABRIC_MMC_BANDWIDTH_COLLECTOR_H
