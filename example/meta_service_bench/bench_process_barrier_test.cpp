/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemCache_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#include <array>
#include <cstdlib>
#include <iostream>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include "bench_process_barrier.h"

namespace {
constexpr int kTestClientCount = 2;
constexpr int kFastClient = 0;
constexpr int kSlowClient = 1;
constexpr int kTestFailureExit = 1;
constexpr int kNegativeWaitMs = 50;
constexpr unsigned int kTestTimeoutSec = 10;
constexpr nfds_t kSingleDescriptorCount = 1;
using Channel = std::array<int, bench::kBarrierSocketCount>;

void Check(bool condition)
{
    if (!condition) {
        std::cerr << "Process barrier test failed\n";
        std::abort();
    }
}

Channel CreateChannel()
{
    Channel channel{};
    Check(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, channel.data()) == 0);
    return channel;
}

void Reap(pid_t pid, int expectedExit)
{
    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    Check(waited == pid && WIFEXITED(status) && WEXITSTATUS(status) == expectedExit);
}

void CheckNoMessage(int socketFd)
{
    pollfd watcher{socketFd, POLLIN, 0};
    int result;
    do {
        result = poll(&watcher, kSingleDescriptorCount, kNegativeWaitMs);
    } while (result < 0 && errno == EINTR);
    Check(result == 0);
}

void RunOrderingTest(int clientCount)
{
    std::vector<Channel> barriers;
    std::vector<Channel> observers;
    for (int i = 0; i < clientCount; ++i) {
        barriers.push_back(CreateChannel());
        observers.push_back(CreateChannel());
    }
    Channel gate = CreateChannel();
    std::vector<pid_t> pids;
    for (int i = 0; i < clientCount; ++i) {
        pid_t pid = fork();
        Check(pid >= 0);
        if (pid == 0) {
            alarm(kTestTimeoutSec);
            close(gate[bench::kBarrierParentEnd]);
            for (int peer = 0; peer < clientCount; ++peer) {
                close(barriers[peer][bench::kBarrierParentEnd]);
                close(observers[peer][bench::kBarrierParentEnd]);
                if (peer != i) {
                    close(barriers[peer][bench::kBarrierClientEnd]);
                    close(observers[peer][bench::kBarrierClientEnd]);
                }
            }
            if (i == kSlowClient) {
                Check(bench::ReceiveBarrierMessage(gate[bench::kBarrierClientEnd], bench::BarrierMessage::READY));
            }
            Check(bench::WaitForBarrier(barriers[i][bench::kBarrierClientEnd], bench::BarrierMessage::READY,
                                       bench::BarrierMessage::START));
            Check(bench::SendBarrierMessage(observers[i][bench::kBarrierClientEnd], bench::BarrierMessage::START));
            if (i == kSlowClient) {
                Check(bench::ReceiveBarrierMessage(gate[bench::kBarrierClientEnd], bench::BarrierMessage::DRAINED));
            }
            Check(bench::WaitForBarrier(barriers[i][bench::kBarrierClientEnd], bench::BarrierMessage::DRAINED,
                                       bench::BarrierMessage::TEARDOWN));
            Check(bench::SendBarrierMessage(observers[i][bench::kBarrierClientEnd], bench::BarrierMessage::TEARDOWN));
            _exit(EXIT_SUCCESS);
        }
        pids.push_back(pid);
    }
    close(gate[bench::kBarrierClientEnd]);
    std::vector<int> parentFds;
    for (int i = 0; i < clientCount; ++i) {
        close(barriers[i][bench::kBarrierClientEnd]);
        close(observers[i][bench::kBarrierClientEnd]);
        parentFds.push_back(barriers[i][bench::kBarrierParentEnd]);
    }
    bool coordinated = false;
    std::thread coordinator([&] { coordinated = bench::CoordinateClients(parentFds); });
    if (clientCount > kSlowClient) {
        // A ready client cannot start while another client is still warming up.
        CheckNoMessage(observers[kFastClient][bench::kBarrierParentEnd]);
        Check(bench::SendBarrierMessage(gate[bench::kBarrierParentEnd], bench::BarrierMessage::READY));
    }
    for (const auto &observer : observers) {
        Check(bench::ReceiveBarrierMessage(observer[bench::kBarrierParentEnd], bench::BarrierMessage::START));
    }
    if (clientCount > kSlowClient) {
        // A drained client cannot disconnect while another client still has requests.
        CheckNoMessage(observers[kFastClient][bench::kBarrierParentEnd]);
        Check(bench::SendBarrierMessage(gate[bench::kBarrierParentEnd], bench::BarrierMessage::DRAINED));
    }
    coordinator.join();
    Check(coordinated);
    for (int i = 0; i < clientCount; ++i) {
        Check(bench::ReceiveBarrierMessage(observers[i][bench::kBarrierParentEnd], bench::BarrierMessage::TEARDOWN));
        close(observers[i][bench::kBarrierParentEnd]);
        close(barriers[i][bench::kBarrierParentEnd]);
        Reap(pids[i], EXIT_SUCCESS);
    }
    close(gate[bench::kBarrierParentEnd]);
}

void RunExitTest(bool afterStartup)
{
    std::array<Channel, kTestClientCount> channels{CreateChannel(), CreateChannel()};
    std::vector<pid_t> pids;
    for (int i = 0; i < kTestClientCount; ++i) {
        pid_t pid = fork();
        Check(pid >= 0);
        if (pid == 0) {
            alarm(kTestTimeoutSec);
            for (int peer = 0; peer < kTestClientCount; ++peer) {
                close(channels[peer][bench::kBarrierParentEnd]);
                if (peer != i) {
                    close(channels[peer][bench::kBarrierClientEnd]);
                }
            }
            if (i == kSlowClient && !afterStartup) {
                _exit(kTestFailureExit);
            }
            bool started = bench::WaitForBarrier(channels[i][bench::kBarrierClientEnd], bench::BarrierMessage::READY,
                                                 bench::BarrierMessage::START);
            if (i == kSlowClient) {
                _exit(kTestFailureExit);
            }
            Check(afterStartup ? started : !started);
            if (started) {
                Check(!bench::WaitForBarrier(channels[i][bench::kBarrierClientEnd], bench::BarrierMessage::DRAINED,
                                            bench::BarrierMessage::TEARDOWN));
            }
            _exit(EXIT_SUCCESS);
        }
        pids.push_back(pid);
    }
    std::vector<int> parentFds;
    for (const auto &channel : channels) {
        close(channel[bench::kBarrierClientEnd]);
        parentFds.push_back(channel[bench::kBarrierParentEnd]);
    }
    Check(!bench::CoordinateClients(parentFds));
    for (int fd : parentFds) {
        close(fd);
    }
    Reap(pids[kFastClient], EXIT_SUCCESS);
    Reap(pids[kSlowClient], kTestFailureExit);
}

void RunClosedWaitingClientTest()
{
    auto waiting = CreateChannel();
    auto unfinished = CreateChannel();
    Check(bench::SendBarrierMessage(waiting[bench::kBarrierClientEnd], bench::BarrierMessage::READY));
    close(waiting[bench::kBarrierClientEnd]);
    Check(!bench::CoordinateClients({waiting[bench::kBarrierParentEnd], unfinished[bench::kBarrierParentEnd]}));
    close(waiting[bench::kBarrierParentEnd]);
    close(unfinished[bench::kBarrierParentEnd]);
    close(unfinished[bench::kBarrierClientEnd]);
}
} // namespace

int main()
{
    alarm(kTestTimeoutSec);
    constexpr int singleClient = 1;
    RunOrderingTest(singleClient);
    RunOrderingTest(kTestClientCount);
    RunExitTest(false);
    RunExitTest(true);
    RunClosedWaitingClientTest();
    std::cout << "Process barrier tests passed\n";
    return EXIT_SUCCESS;
}
