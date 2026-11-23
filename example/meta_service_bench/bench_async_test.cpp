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

// Reuse the benchmark state machine with a deterministic in-process transport.
#define main BenchmarkMain
#include "bench_meta_service.cpp"
#undef main
#include <stdexcept>

namespace bench {
struct AsyncTransportTest {
    static void SetSend(AsyncTransport &transport,
        std::function<int(uint32_t, int16_t, const std::string &)> send)
    {
        transport.testSend_ = std::move(send);
    }
    static void Break(AsyncTransport &transport)
    {
        AsyncTransport::Broken(&transport, nullptr);
    }
    static void Reply(AsyncTransport &transport, uint32_t sequence, const std::string &bytes)
    {
        smem_acc_message_v2_t message{};
        message.seq_no = sequence;
        AsyncTransport::Receive(&transport, &message, bytes.data(), bytes.size(), nullptr);
    }
};
}
void Check(bool condition)
{
    if (!condition) throw std::runtime_error("async benchmark assertion failed");
}

void RunChildReapingTest()
{
    pid_t child = fork();
    Check(child >= 0);
    if (child == 0) {
        _exit(EXIT_SUCCESS);
    }
    Check(ReapChildren({child}));
    Check(waitpid(child, nullptr, WNOHANG) == -1 && errno == ECHILD);

    int channel[bench::kBarrierSocketCount];
    Check(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, channel) == 0);
    child = fork();
    Check(child >= 0);
    if (child == 0) {
        close(channel[bench::kBarrierParentEnd]);
        signal(SIGTERM, SIG_IGN);
        Check(bench::SendBarrierMessage(channel[bench::kBarrierClientEnd], bench::BarrierMessage::READY));
        while (true) {
            pause();
        }
    }
    close(channel[bench::kBarrierClientEnd]);
    Check(bench::ReceiveBarrierMessage(channel[bench::kBarrierParentEnd], bench::BarrierMessage::READY));
    close(channel[bench::kBarrierParentEnd]);
    constexpr int immediateTimeoutSec = 0;
    Check(!ReapChildren({child}, SIGTERM, immediateTimeoutSec));
    Check(waitpid(child, nullptr, WNOHANG) == -1 && errno == ECHILD);
}

int main()
{
    RunChildReapingTest();
    constexpr int testOperations = 8;
    constexpr int testDepth = 4;
    constexpr int allPut = 100;
    for (bool wait : {false, true}) {
        bench::AsyncTransport transport;
        BenchConfig config;
        config.opsPerThread = testOperations;
        config.queueDepth = testDepth;
        config.putPercent = allPut;
        config.minBatch = config.maxBatch = 1;
        config.waitCompletions = wait;
        ThreadStats stats;
        int allocations = 0;
        int updates = 0;
        bench::AsyncTransportTest::SetSend(transport, [&](uint32_t sequence, int16_t opcode, const std::string &) {
            NetMsgPacker packer;
            if (opcode == ML_ALLOC_REQ) {
                ++allocations;
                AllocResponse response;
                response.result_ = MMC_OK;
                response.blobs_.resize(1);
                response.Serialize(packer);
            } else {
                Check(opcode == ML_UPDATE_REQ && allocations > updates);
                ++updates;
                Response response(MMC_OK);
                response.Serialize(packer);
            }
            // Completion before Send returns, followed by a duplicate.
            bench::AsyncTransportTest::Reply(transport, sequence, packer.String());
            bench::AsyncTransportTest::Reply(transport, sequence, packer.String());
            return MMC_OK;
        });
        Worker(transport, config, stats, 0, 0);
        Check(allocations == testOperations && updates == testOperations);
        Check(stats.successCount[kOpPut] == testOperations && stats.histogram[kOpPut].count == 0);
        Check(stats.histogram[kOpAlloc].count == testOperations && stats.submission[kOpUpdate].count == testOperations);
        Check(stats.failureCount[kOpPut] == 0);
    }
    // A rejected send must release each logical slot without producing RTT samples.
    bench::AsyncTransport transport;
    bench::AsyncTransportTest::SetSend(transport, [](uint32_t, int16_t, const std::string &) { return MMC_ERROR; });
    BenchConfig config;
    config.opsPerThread = testOperations;
    config.putPercent = allPut;
    ThreadStats stats;
    Worker(transport, config, stats, 0, 0);
    Check(stats.failureCount[kOpPut] == testOperations && stats.histogram[kOpAlloc].count == 0);
    Check(stats.submission[kOpAlloc].count == testOperations && stats.submission[kOpUpdate].count == 0);
    // Hold replies until the full window has been submitted, then complete in reverse order.
    for (bool wait : {false, true}) {
        bench::AsyncTransport unorderedTransport;
        std::vector<uint32_t> held;
        BenchConfig reads;
        reads.opsPerThread = testOperations;
        reads.queueDepth = testDepth;
        reads.putPercent = 0;
        reads.minBatch = reads.maxBatch = 1;
        reads.waitCompletions = wait;
        ThreadStats readStats;
        bench::AsyncTransportTest::SetSend(unorderedTransport,
            [&](uint32_t sequence, int16_t opcode, const std::string &) {
                Check(opcode == ML_GET_REQ);
                held.push_back(sequence);
                Check(held.size() <= testDepth);
                if (held.size() == testDepth) {
                    AllocResponse response;
                    response.blobs_.resize(1);
                    NetMsgPacker packer;
                    response.Serialize(packer);
                    for (auto it = held.rbegin(); it != held.rend(); ++it) {
                        bench::AsyncTransportTest::Reply(unorderedTransport, *it, packer.String());
                    }
                    held.clear();
                }
                return MMC_OK;
            });
        Worker(unorderedTransport, reads, readStats, 0, 0);
        Check(readStats.successCount[kOpGet] == testOperations);
        Check(readStats.histogram[kOpGet].count == testOperations);
    }
    // No response: release slots as timeouts, without manufacturing RTT samples.
    bench::AsyncTransport silent;
    bench::AsyncTransportTest::SetSend(silent, [](uint32_t, int16_t, const std::string &) { return MMC_OK; });
    config.rpcTimeoutMs = 0;
    ThreadStats timeoutStats;
    Worker(silent, config, timeoutStats, 0, 0);
    Check(timeoutStats.timeouts[kOpAlloc] == testOperations);
    Check(timeoutStats.failureCount[kOpPut] == testOperations);
    Check(timeoutStats.histogram[kOpAlloc].count == 0);
    // Partial batch allocation updates its successful entries but fails the logical Put.
    bench::AsyncTransport partial;
    constexpr int testBatchSize = 2;
    int batchUpdates = 0;
    bench::AsyncTransportTest::SetSend(partial, [&](uint32_t sequence, int16_t opcode, const std::string &bytes) {
        NetMsgPacker packer;
        if (opcode == ML_BATCH_ALLOC_REQ) {
            BatchAllocResponse response;
            response.results_ = {MMC_OK, MMC_ERROR};
            response.blobs_.resize(testBatchSize);
            response.blobs_[0].resize(1);
            response.Serialize(packer);
        } else {
            Check(opcode == ML_BATCH_UPDATE_REQ);
            BatchUpdateRequest request;
            Check(DecodeResponse(bytes, request) && request.keys_.size() == 1);
            ++batchUpdates;
            BatchUpdateResponse response;
            response.results_ = {MMC_OK};
            response.Serialize(packer);
        }
        bench::AsyncTransportTest::Reply(partial, sequence, packer.String());
        return MMC_OK;
    });
    BenchConfig batches;
    batches.opsPerThread = testOperations;
    batches.queueDepth = testDepth;
    batches.putPercent = allPut;
    batches.minBatch = batches.maxBatch = testBatchSize;
    ThreadStats batchStats;
    Worker(partial, batches, batchStats, 0, 0);
    Check(batchUpdates == testOperations && batchStats.failureCount[kOpPut] == testOperations);
    Check(batchStats.failureCount[kOpAlloc] == testOperations && batchStats.successCount[kOpUpdate] == testOperations);

    // A disconnect aborts issuance and fails the in-flight logical operation once.
    bench::AsyncTransport disconnected;
    bench::AsyncTransportTest::SetSend(disconnected, [&](uint32_t, int16_t, const std::string &) {
        bench::AsyncTransportTest::Break(disconnected);
        return MMC_OK;
    });
    ThreadStats disconnectedStats;
    Worker(disconnected, config, disconnectedStats, 0, 0);
    Check(disconnected.Failed() && disconnectedStats.failureCount[kOpPut] == 1);

    // Response arrival and submission return can overlap: never subtract the intervals.
    bench::AsyncTransport delayedReturn;
    constexpr int submissionDelayMs = 2;
    bench::AsyncTransportTest::SetSend(delayedReturn, [&](uint32_t sequence, int16_t, const std::string &) {
        AllocResponse response;
        response.blobs_.resize(1);
        NetMsgPacker packer;
        response.Serialize(packer);
        bench::AsyncTransportTest::Reply(delayedReturn, sequence, packer.String());
        std::this_thread::sleep_for(std::chrono::milliseconds(submissionDelayMs));
        return MMC_OK;
    });
    BenchConfig reads;
    reads.opsPerThread = testOperations;
    reads.putPercent = 0;
    reads.minBatch = reads.maxBatch = 1;
    ThreadStats timingStats;
    Worker(delayedReturn, reads, timingStats, 0, 0);
    Check(timingStats.submission[kOpGet].totalUs > timingStats.histogram[kOpGet].totalUs);
    Check(timingStats.histogram[kOpGet].count == testOperations);
    AllocResponse malformed;
    Check(!DecodeResponse(std::string(), malformed));
    std::cout << "Async benchmark tests passed\n";
}
