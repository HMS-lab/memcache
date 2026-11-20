/* Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
  Licensed under Mulan PSL v2. See the repository LICENSE file. */

#ifndef META_MANAGER_SIM_PROMETHEUS_SIMPLEAPI_H
#define META_MANAGER_SIM_PROMETHEUS_SIMPLEAPI_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

namespace prometheus {

class Registry {};
namespace simpleapi {

class AtomicMetric {
public:
    AtomicMetric(const char *, const char *) {}
    void operator++(int)
    {
        ++value_;
    }
    void operator+=(int64_t amount)
    {
        value_.fetch_add(amount);
    }
    void operator-=(int64_t amount)
    {
        value_.fetch_sub(amount);
    }
    void operator=(int64_t amount)
    {
        value_.store(amount);
    }
    int64_t value() const
    {
        return value_.load();
    }

private:
    std::atomic<int64_t> value_{0};
};

using counter_metric_t = AtomicMetric;
using gauge_metric_t = AtomicMetric;

} // namespace simpleapi
} // namespace prometheus

#endif /* META_MANAGER_SIM_PROMETHEUS_SIMPLEAPI_H */
