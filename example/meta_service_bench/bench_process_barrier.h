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

#ifndef MEMCACHE_BENCH_PROCESS_BARRIER_H
#define MEMCACHE_BENCH_PROCESS_BARRIER_H

#include <cerrno>
#include <functional>
#include <poll.h>
#include <sys/socket.h>
#include <vector>

namespace bench {
constexpr int kBarrierSocketCount = 2;
constexpr int kBarrierParentEnd = 0;
constexpr int kBarrierClientEnd = 1;

enum class BarrierMessage : char { READY = 'R', START = 'S', DRAINED = 'D', TEARDOWN = 'T' };

inline bool SendBarrierMessage(int socketFd, BarrierMessage message)
{
    ssize_t sent;
    do {
        sent = send(socketFd, &message, sizeof(message), MSG_NOSIGNAL);
    } while (sent < 0 && errno == EINTR);
    return sent == static_cast<ssize_t>(sizeof(message));
}

inline bool ReceiveBarrierMessage(int socketFd, BarrierMessage expected)
{
    BarrierMessage message{};
    ssize_t received;
    do {
        received = recv(socketFd, &message, sizeof(message), 0);
    } while (received < 0 && errno == EINTR);
    return received == static_cast<ssize_t>(sizeof(message)) && message == expected;
}

inline bool WaitForBarrier(int socketFd, BarrierMessage arrival, BarrierMessage release)
{
    return SendBarrierMessage(socketFd, arrival) && ReceiveBarrierMessage(socketFd, release);
}

// The parent watches every channel, including clients already waiting for release.
// EOF or an unexpected message aborts coordination instead of stranding peers.
inline bool CoordinateBarrier(const std::vector<int> &socketFds, BarrierMessage arrival, BarrierMessage release,
                              const std::function<bool()> &beforeRelease = {})
{
    std::vector<pollfd> watchers;
    for (int socketFd : socketFds) {
        watchers.push_back({socketFd, POLLIN, 0});
    }
    std::vector<bool> arrived(socketFds.size(), false);
    size_t remaining = socketFds.size();
    while (remaining != 0) {
        int ready = poll(watchers.data(), watchers.size(), -1);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        for (size_t i = 0; i < watchers.size(); ++i) {
            if (watchers[i].revents == 0) {
                continue;
            }
            if ((watchers[i].revents & (POLLERR | POLLNVAL | POLLHUP)) != 0 || arrived[i] ||
                !ReceiveBarrierMessage(watchers[i].fd, arrival)) {
                return false;
            }
            arrived[i] = true;
            --remaining;
        }
    }
    // All clients are parked here. Run lifecycle actions before any client is released.
    if (beforeRelease && !beforeRelease()) {
        return false;
    }
    for (int socketFd : socketFds) {
        if (!SendBarrierMessage(socketFd, release)) {
            return false;
        }
    }
    return true;
}

inline bool CoordinateClients(const std::vector<int> &socketFds, const std::function<bool()> &beforeStart = {},
                              const std::function<bool()> &beforeTeardown = {})
{
    // On failure the caller must close all parent endpoints and reap/stop its children.
    return !socketFds.empty() &&
           CoordinateBarrier(socketFds, BarrierMessage::READY, BarrierMessage::START, beforeStart) &&
           CoordinateBarrier(socketFds, BarrierMessage::DRAINED, BarrierMessage::TEARDOWN, beforeTeardown);
}
} // namespace bench

#endif // MEMCACHE_BENCH_PROCESS_BARRIER_H
