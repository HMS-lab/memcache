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

#ifndef MEMCACHE_BENCH_QUEUE_DEPTH_H
#define MEMCACHE_BENCH_QUEUE_DEPTH_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <new>
#include <sstream>
#include <string>
#include <sys/mman.h>
#include <thread>

namespace bench {
constexpr size_t kQueueDepthCacheLineBytes = 64;
constexpr int kQueueDepthColumnWidth = 12;
constexpr int kQueueDepthIntervalMs = 1000;
constexpr int kQueueDepthHeaderLines = 2;

// Independent cache lines keep workers from contending when publishing their own depth.
struct alignas(kQueueDepthCacheLineBytes) QueueDepthSlot {
    std::atomic<uint32_t> depth{0};
};
static_assert(std::atomic<uint32_t>::is_always_lock_free, "Shared depth counters must be lock-free");

class QueueDepthMonitor {
public:
    QueueDepthMonitor(int processes, int workers) : processes_(processes), workers_(workers)
    {
        bytes_ = static_cast<size_t>(processes) * workers * sizeof(QueueDepthSlot);
        void *memory = mmap(nullptr, bytes_, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if (memory == MAP_FAILED) {
            return;
        }
        slots_ = static_cast<QueueDepthSlot *>(memory);
        for (size_t i = 0; i < static_cast<size_t>(processes) * workers; ++i) {
            new (&slots_[i]) QueueDepthSlot;
        }
    }

    ~QueueDepthMonitor()
    {
        Stop();
        if (slots_ != nullptr) {
            munmap(slots_, bytes_);
        }
    }

    bool Valid() const
    {
        return slots_ != nullptr;
    }

    std::atomic<uint32_t> *Counter(int process, int worker)
    {
        return &slots_[static_cast<size_t>(process) * workers_ + worker].depth;
    }

    void Start()
    {
        stopped_ = false;
        printed_ = false;
        Print();
        thread_ = std::thread([this]() {
            std::unique_lock<std::mutex> lock(mutex_);
            while (!wake_.wait_for(lock, std::chrono::milliseconds(kQueueDepthIntervalMs),
                                   [this]() { return stopped_; })) {
                Print();
            }
        });
    }

    void Stop()
    {
        if (!thread_.joinable()) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopped_ = true;
        }
        wake_.notify_one();
        thread_.join();
        Print();
    }

private:
    void Print()
    {
        std::ostringstream output;
        if (printed_) {
            // Leave the cursor below the table so final reports follow it normally.
            output << "\033[" << processes_ + kQueueDepthHeaderLines << "A";
        } else {
            output << "\n";
        }
        output << "\r\033[2KCurrent queue depth (pending RPCs, sampled every 1 s)\n"
               << "\r\033[2K" << std::left << std::setw(kQueueDepthColumnWidth) << "Client";
        for (int worker = 0; worker < workers_; ++worker) {
            output << std::setw(kQueueDepthColumnWidth) << ("worker_" + std::to_string(worker));
        }
        output << "\n";
        for (int process = 0; process < processes_; ++process) {
            output << "\r\033[2K" << std::setw(kQueueDepthColumnWidth) << ("client_" + std::to_string(process));
            for (int worker = 0; worker < workers_; ++worker) {
                output << std::setw(kQueueDepthColumnWidth) << Counter(process, worker)->load(std::memory_order_relaxed);
            }
            output << "\n";
        }
        std::cout << output.str() << std::flush;
        printed_ = true;
    }

    int processes_;
    int workers_;
    size_t bytes_{0};
    QueueDepthSlot *slots_{nullptr};
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stopped_{true};
    bool printed_{false};
    std::thread thread_;
};
} // namespace bench

#endif
