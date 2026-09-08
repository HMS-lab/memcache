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
#ifndef MEMCACHE_MOCK_ACC_HTTP_SERVER_H
#define MEMCACHE_MOCK_ACC_HTTP_SERVER_H

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include "smem_acc_def.h"

struct MockHttpRequest {
    int socket{-1};
    std::string body;
    std::multimap<std::string, std::string> params;

    int32_t Reply(uint16_t status, const char *contentType, const void *data, uint32_t length) const
    {
        std::ostringstream header;
        header << "HTTP/1.1 " << status << " Result\r\nContent-Type: " << contentType
               << "\r\nContent-Length: " << length << "\r\nConnection: close\r\n\r\n";
        auto response = header.str();
        if (length != 0) {
            response.append(static_cast<const char *>(data), length);
        }
        size_t offset = 0;
        while (offset < response.size()) {
            const auto sent = send(socket, response.data() + offset, response.size() - offset, MSG_NOSIGNAL);
            if (sent <= 0) {
                return -1;
            }
            offset += sent;
        }
        return 0;
    }
};

// Socket transport for the existing HTTP contract UTs; business handlers remain in MMC.
class MockAccHttpServer {
public:
    ~MockAccHttpServer()
    {
        Stop();
    }

    int32_t Register(uint8_t method, const char *path, smem_acc_http_cb callback, void *context)
    {
        std::lock_guard<std::mutex> guard(mutex_);
        handlers_[{method, path}] = {callback, context};
        return 0;
    }

    int32_t Start(const smem_acc_http_server_options_t &options)
    {
        socket_ = socket(AF_INET, SOCK_STREAM, 0);
        if (socket_ < 0) {
            return -1;
        }
        constexpr int kEnableReuse = 1;
        constexpr int kListenBacklog = 16;
        setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, &kEnableReuse, sizeof(kEnableReuse));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(options.listen_port);
        if (inet_pton(AF_INET, options.listen_ip, &address.sin_addr) != kEnableReuse ||
            bind(socket_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
            listen(socket_, kListenBacklog) != 0) {
            close(socket_);
            socket_ = -1;
            return -1;
        }
        maxBodySize_ = options.max_body_size;
        running_ = true;
        worker_ = std::thread([this]() { Run(); });
        return 0;
    }

    void Stop()
    {
        running_ = false;
        if (socket_ >= 0) {
            shutdown(socket_, SHUT_RDWR);
        }
        if (worker_.joinable()) {
            worker_.join();
        }
        if (socket_ >= 0) {
            close(socket_);
            socket_ = -1;
        }
    }

private:
    struct Handler {
        smem_acc_http_cb callback{nullptr};
        void *context{nullptr};
    };

    static std::string Decode(const std::string &value)
    {
        std::string decoded;
        constexpr size_t kHexDigitCount = 2;
        for (size_t pos = 0; pos < value.size(); ++pos) {
            if (value[pos] == '%' && pos + kHexDigitCount < value.size()) {
                unsigned int character = 0;
                std::istringstream hex(value.substr(pos + 1, kHexDigitCount));
                if (hex >> std::hex >> character) {
                    decoded += static_cast<char>(character);
                    pos += kHexDigitCount;
                    continue;
                }
            }
            decoded += value[pos] == '+' ? ' ' : value[pos];
        }
        return decoded;
    }

    void Handle(int client)
    {
        constexpr size_t kMaxHeaderSize = 64 * 1024;
        constexpr size_t kHeaderTerminatorSize = 4;
        std::string header;
        char character = 0;
        while (header.size() < kMaxHeaderSize && header.find("\r\n\r\n") == std::string::npos) {
            if (recv(client, &character, sizeof(character), 0) <= 0) {
                return;
            }
            header += character;
        }
        if (header.size() < kHeaderTerminatorSize || header.find("\r\n\r\n") == std::string::npos) {
            return;
        }
        std::istringstream input(header);
        std::string method;
        std::string target;
        std::string protocol;
        std::string line;
        input >> method >> target >> protocol;
        size_t length = 0;
        while (std::getline(input, line)) {
            const std::string prefix = "Content-Length: ";
            if (line.compare(0, prefix.size(), prefix) == 0) {
                std::istringstream(line.substr(prefix.size())) >> length;
            }
        }
        if (length > maxBodySize_) {
            return;
        }
        MockHttpRequest request;
        request.socket = client;
        request.body.resize(length);
        size_t received = 0;
        while (received < length) {
            const auto count = recv(client, &request.body[received], length - received, 0);
            if (count <= 0) {
                return;
            }
            received += count;
        }
        const auto queryPos = target.find('?');
        if (queryPos != std::string::npos) {
            std::istringstream query(target.substr(queryPos + 1));
            while (std::getline(query, line, '&')) {
                const auto equal = line.find('=');
                request.params.emplace(Decode(line.substr(0, equal)),
                                       equal == std::string::npos ? "" : Decode(line.substr(equal + 1)));
            }
            target.resize(queryPos);
        }
        const std::string methods[] = {"GET", "HEAD", "POST", "PUT", "DELETE", "CONNECT", "OPTIONS", "TRACE", "PATCH"};
        Handler handler;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            for (uint8_t index = 0; index < sizeof(methods) / sizeof(methods[0]); ++index) {
                if (methods[index] == method) {
                    const auto found = handlers_.find({index, Decode(target)});
                    if (found != handlers_.end()) {
                        handler = found->second;
                    }
                    break;
                }
            }
        }
        if (handler.callback != nullptr) {
            handler.callback(handler.context, &request);
        } else {
            constexpr uint16_t kNotFound = 404;
            request.Reply(kNotFound, "text/plain", nullptr, 0);
        }
    }

    void Run()
    {
        constexpr time_t kSocketTimeoutSeconds = 2;
        while (running_) {
            const int client = accept(socket_, nullptr, nullptr);
            if (client < 0) {
                break;
            }
            const timeval timeout{kSocketTimeoutSeconds, 0};
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
            Handle(client);
            close(client);
        }
    }

    int socket_{-1};
    size_t maxBodySize_{0};
    std::atomic<bool> running_{false};
    std::thread worker_;
    std::mutex mutex_;
    std::map<std::pair<uint8_t, std::string>, Handler> handlers_;
};
#endif // MEMCACHE_MOCK_ACC_HTTP_SERVER_H
