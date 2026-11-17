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

#ifndef BENCH_ASYNC_TRANSPORT_H
#define BENCH_ASYNC_TRANSPORT_H

#include <condition_variable>
#include <chrono>
#include <mutex>
#include <functional>
#include <string>
#include <deque>
#include <memory>
#include <unordered_map>
#include "smem_acc_api.h"
#include "mmc_net_common.h"

namespace bench {
inline double ClockUs()
{
    return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
struct Completion {
    uint32_t sequence;
    double arrival;
    int result;
    std::string bytes;
};
struct CompletionQueue {
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<Completion> entries;
    void Push(Completion value)
    {
        std::lock_guard<std::mutex> lock(mutex);
        entries.push_back(std::move(value));
        ready.notify_one();
    }
    std::deque<Completion> Drain(bool wait, double deadline)
    {
        std::unique_lock<std::mutex> lock(mutex);
        if (wait && entries.empty()) {
            ready.wait_until(lock, std::chrono::steady_clock::time_point(
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double, std::micro>(deadline))), [&] { return !entries.empty(); });
        }
        std::deque<Completion> result;
        result.swap(entries);
        return result;
    }
};
class AsyncTransport {
    friend struct AsyncTransportTest;
public:
    ~AsyncTransport()
    {
        if (server_ != nullptr) {
            ock::mmc::MFAccApi::StopServer(server_);
            if (link_ != nullptr) {
                ock::mmc::MFAccApi::ReleaseLink(link_);
            }
            ock::mmc::MFAccApi::DestroyServer(server_);
        }
    }
    bool Start(const std::string &url, uint16_t capacity)
    {
        using namespace ock::mmc;
        constexpr int16_t kNetServerMagic = 3867;
        if (MFAccApi::LoadLibrary() != MMC_OK) return false;
        server_ = MFAccApi::CreateTcpServer();
        if (server_ == nullptr) return false;
        MFAccApi::RegisterNewRequestHandler(server_, 0, Receive, this);
        MFAccApi::RegisterRequestSentHandler(server_, 0, Sent, this);
        MFAccApi::RegisterLinkBrokenHandler(server_, Broken, this);
        constexpr uint16_t workers = 2;
        smem_acc_tcp_server_options_t options{"127.0.0.1", 0, workers, capacity, kNetServerMagic,
            static_cast<int16_t>(NetProtoVersion::VERSION_1), 0};
        smem_tls_options_t tls{};
        if (MFAccApi::StartTcpServer(server_, &options, &tls) != MMC_OK) return false;
        NetEngineOptions address;
        if (NetEngineOptions::ExtractIpPortFromUrl(url, address) != MMC_OK) return false;
        smem_acc_connect_request_t request{kNetServerMagic,
            static_cast<int16_t>(NetProtoVersion::VERSION_1), 0, UINT64_MAX};
        constexpr uint32_t retries = 5;
        return MFAccApi::ConnectToPeerServer(server_, address.ip.c_str(), address.port, &request,
            retries, &link_) == MMC_OK;
    }
    uint32_t Register(CompletionQueue &queue)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (broken_ || next_ == UINT32_MAX) return 0;
        const auto id = ++next_;
        pending_.emplace(id, &queue);
        return id;
    }
    int Send(uint32_t sequence, int16_t opcode, const std::string &bytes, double &start, double &end)
    {
        const auto *data = bytes.data();
        const auto length = static_cast<uint32_t>(bytes.size());
        if (testSend_) {
            start = ClockUs();
            int result = testSend_(sequence, opcode, bytes);
            end = ClockUs();
            return result;
        }
        start = ClockUs();
        int result = ock::mmc::MFAccApi::NonBlockSend(link_, 0, opcode, sequence, data, length, nullptr, 0);
        end = ClockUs();
        return result;
    }
    void Cancel(uint32_t sequence)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.erase(sequence);
    }
    bool Failed()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return broken_;
    }
private:
    void Complete(uint32_t sequence, double arrival, int result, const void *data, uint32_t length)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto found = pending_.find(sequence);
        if (found == pending_.end()) return;
        found->second->Push({sequence, arrival, result,
            length == 0 ? std::string() : std::string(static_cast<const char *>(data), length)});
        pending_.erase(found);
    }
    static int32_t Receive(void *self, const smem_acc_message_v2_t *message, const void *data,
        uint32_t length, smem_acc_link_t)
    {
        const double arrival = ClockUs();
        static_cast<AsyncTransport *>(self)->Complete(message->seq_no, arrival, 0, data, length);
        return 0;
    }
    static int32_t Sent(void *self, int32_t result, const smem_acc_message_v2_t *message, const void *, uint32_t)
    {
        if (result != 0) static_cast<AsyncTransport *>(self)->Complete(message->seq_no, ClockUs(), result, nullptr, 0);
        return 0;
    }
    static int32_t Broken(void *self, smem_acc_link_t)
    {
        auto &transport = *static_cast<AsyncTransport *>(self);
        std::lock_guard<std::mutex> lock(transport.mutex_);
        transport.broken_ = true;
        for (auto &entry : transport.pending_) entry.second->Push({entry.first, ClockUs(), -1, {}});
        transport.pending_.clear();
        return 0;
    }
    std::function<int(uint32_t, int16_t, const std::string &)> testSend_;
    smem_acc_server_t server_{nullptr};
    smem_acc_link_t link_{nullptr};
    std::mutex mutex_;
    uint32_t next_{0};
    bool broken_{false};
    std::unordered_map<uint32_t, CompletionQueue *> pending_;
};
} // namespace bench
#endif
