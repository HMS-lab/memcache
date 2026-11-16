/* Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 * Licensed under Mulan PSL v2. */

#include "manager_adapter.h"

#include <iostream>
#include <memory>
#include <stdexcept>
#include <unistd.h>

using ock::mmc::Json;
using ock::mmc::Manager;

namespace {
constexpr size_t kReadFieldCount = 2;
constexpr size_t kReadKeyIndex = 0;
constexpr size_t kReadSizeIndex = 1;
} // namespace

int main()
{
    std::unique_ptr<Manager> manager;
    std::string line;
    std::string key;
    uint64_t readIndex = 0;
    uint64_t size = 0;

    try {
        while (std::getline(std::cin, line)) {
            Json reply;
            bool stop = false;

            // Note: Release request buffers before Python samples the worker's RSS
            {
                const auto request = Json::parse(line);
                const auto operation = request.at("op").get<std::string>();

                if (operation == "init" && !manager) {
                    manager = std::make_unique<Manager>(request.at("config"), request.at("detailed_stats").get<bool>());
                    reply = manager->Snapshot();
                    reply["pid"] = getpid();
                } else if (!manager) {
                    throw std::runtime_error("Initialize first");
                } else if (operation == "read") {
                    const auto model = request.at("model").get<std::string>();
                    // Each read is a [key, size] pair supplied by Python
                    for (const auto &read : request.at("reads")) {
                        if (!read.is_array() || read.size() != kReadFieldCount || !read[kReadKeyIndex].is_string() ||
                            !read[kReadSizeIndex].is_number_unsigned()) {
                            throw std::runtime_error("Expected [key, positive size]");
                        }
                        key = read[kReadKeyIndex].get<std::string>();
                        size = read[kReadSizeIndex].get<uint64_t>();
                        manager->Access(key, size, model);
                        ++readIndex;
                    }
                    reply = manager->Progress();
                } else if (operation == "snapshot") {
                    reply = manager->Snapshot();
                } else if (operation == "stop") {
                    manager->Close();
                    reply = {{"stopped", true}};
                    stop = true;
                } else {
                    throw std::runtime_error("Unknown command");
                }
            }

            // Note: Flushing each reply lets the sequential Python caller continue
            std::cout << Json{{"ok", true}, {"result", reply}}.dump() << std::endl;
            if (stop) {
                return 0;
            }
        }
        return 0;
    } catch (const std::exception &error) {
        std::cout << Json{{"ok", false},
                          {"error", error.what()},
                          {"read_index", readIndex},
                          {"key", key},
                          {"size_bytes", size}}
                         .dump()
                  << std::endl;
        return 1;
    }
}
