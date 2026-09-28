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
#ifndef MEMFABRIC_MMC_MSG_PACKER_H
#define MEMFABRIC_MMC_MSG_PACKER_H

#include <vector>
#include "mmc_common_includes.h"

constexpr size_t MAX_CONTAINER_SIZE = 1024 * 1024;

namespace ock {
namespace mmc {
class NetMsgPacker {
public:
    /**
     * @brief Append a POD(Plain Old Data) struct
     *
     * @tparam T           [in] type of POD
     * @param val          [in] value of POD
     */
    template<typename T>
    void Serialize(const T &val, typename std::enable_if<std::is_trivially_copyable<T>::value, int>::type = 0)
    {
        outStream_.write(reinterpret_cast<const char *>(&val), sizeof(T));
    }

    /**
     * @brief Append a string
     *
     * @param val          [in] value of string
     */
    void Serialize(const std::string &val)
    {
        const uint32_t size = val.size();
        outStream_.write(reinterpret_cast<const char *>(&size), sizeof(size));
        outStream_.write(val.data(), size);
    }

    /**
     * @brief Append a pair
     *
     * @tparam K           [in] type of K of pair
     * @tparam V           [in] type of V of pair
     * @param val          [in] value of pair
     */
    template<typename K, typename V>
    void Serialize(const std::pair<K, V> &val)
    {
        Serialize(val.first);
        Serialize(val.second);
    }

    /**
     * @brief Append a vector
     *
     * @tparam V           [in] type of vector element
     * @param container    [in] vector to be appended
     */
    template<typename V>
    void Serialize(const std::vector<V> &container)
    {
        const std::size_t size = container.size();
        outStream_.write(reinterpret_cast<const char *>(&size), sizeof(size));
        for (const auto &item : container) {
            Serialize(item);
        }
    }

    /**
     * @brief Append a map
     *
     * @tparam K           [in] type of map key
     * @tparam V           [in] type of map value
     * @param container    [in] map to be appended
     */
    template<typename K, typename V>
    void Serialize(const std::map<K, V> &container)
    {
        const std::size_t size = container.size();
        outStream_.write(reinterpret_cast<const char *>(&size), sizeof(size));
        for (const auto &item : container) {
            Serialize(item);
        }
    }

    /**
     * @brief Get the serialized result
     *
     * @return String value of serialized
     */
    std::string String() const
    {
        return outStream_.str();
    }

private:
    std::ostringstream outStream_;
};

class NetMsgUnpacker {
public:
    /**
     * @brief Create unpacker with serialized data
     *
     * @param value        [in] serialized data with string type
     */
    explicit NetMsgUnpacker(const std::string &value) : inStream_(value) {}

    /**
     * @brief Take data and deserialize to POD data
     *
     * @tparam T           [in] type of POD
     * @param val          [in/out] result data of POD
     */
    template<typename T>
    Result Deserialize(T &val, typename std::enable_if<std::is_trivially_copyable<T>::value, int>::type = 0)
    {
        inStream_.read(reinterpret_cast<char *>(&val), sizeof(T));
        if (!inStream_.good()) {
            MMC_LOG_ERROR("Failed to deserialize POD type, stream error");
            return MMC_ERROR;
        }
        return MMC_OK;
    }

    /**
     * @brief Take data and deserialize to string
     *
     * @param val          [in/out] result data of string
     */
    Result Deserialize(std::string &val)
    {
        uint32_t size = 0;
        inStream_.read(reinterpret_cast<char *>(&size), sizeof(size));
        if (!inStream_.good() || size > MAX_CONTAINER_SIZE) {
            MMC_LOG_ERROR("string size: " << size << " exceeds limit: " << MAX_CONTAINER_SIZE);
            return MMC_ERROR;
        }
        val.resize(size);
        inStream_.read(&val[0], size);
        if (!inStream_.good()) {
            MMC_LOG_ERROR("Failed to read string data, size: " << size);
            return MMC_ERROR;
        }
        return MMC_OK;
    }

    /**
     * @brief Take data and deserialize to vector
     *
     * @tparam V           [in] type of vector element
     * @param container    [in/out] result data of vector
     */
    template<typename V>
    Result Deserialize(std::vector<V> &container)
    {
        std::size_t size = 0;
        inStream_.read(reinterpret_cast<char *>(&size), sizeof(size));
        if (!inStream_.good() || size > MAX_CONTAINER_SIZE) {
            MMC_LOG_ERROR("container size: " << size << " exceeds limit: " << MAX_CONTAINER_SIZE);
            return MMC_ERROR;
        }
        container.clear();
        container.reserve(size);
        for (std::size_t i = 0; i < size; ++i) {
            V item;
            if (Deserialize(item) != MMC_OK) {
                MMC_LOG_ERROR("Failed to deserialize vector element at index " << i);
                return MMC_ERROR;
            }
            container.emplace_back(std::move(item));
        }
        return MMC_OK;
    }

    /**
     * @brief Take data and deserialize to vector
     *
     * @tparam K           [in] type of map key
     * @tparam V           [in] type of map value
     * @param container    [in/out] result data of map
     */
    template<typename K, typename V>
    Result Deserialize(std::map<K, V> &container)
    {
        std::size_t size = 0;
        inStream_.read(reinterpret_cast<char *>(&size), sizeof(size));
        if (!inStream_.good() || size > MAX_CONTAINER_SIZE) {
            MMC_LOG_ERROR("container size: " << size << " exceeds limit: " << MAX_CONTAINER_SIZE);
            return MMC_ERROR;
        }
        container.clear();
        for (std::size_t i = 0; i < size; ++i) {
            K key;
            V value;
            if (Deserialize(key) != MMC_OK) {
                MMC_LOG_ERROR("Failed to deserialize map key at index " << i);
                return MMC_ERROR;
            }
            if (Deserialize(value) != MMC_OK) {
                MMC_LOG_ERROR("Failed to deserialize map value at index " << i);
                return MMC_ERROR;
            }
            container.emplace(std::move(key), std::move(value));
        }
        return MMC_OK;
    }

    /**
     * @brief Take data and deserialize to pair
     *
     * @tparam K           [in] type of pair first
     * @tparam V           [in] type of pair second
     * @param val          [in/out] result data of pair
     */
    template<typename K, typename V>
    Result Deserialize(std::pair<K, V> &val)
    {
        if (Deserialize(val.first) != MMC_OK) {
            return MMC_ERROR;
        }
        return Deserialize(val.second);
    }

private:
    std::istringstream inStream_;
};
} // namespace mmc
} // namespace ock

#endif // MEMFABRIC_MMC_MSG_PACKER_H
