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

#ifndef MEM_FABRIC_HYBRID_SMEM_COMMON_FUNC_H
#define MEM_FABRIC_HYBRID_SMEM_COMMON_FUNC_H

#include <algorithm>
#include <arpa/inet.h>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <dlfcn.h>
#include <ifaddrs.h>
#include <iomanip>
#include <net/if.h>
#include <netinet/in.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include "mmc_types.h"
#include "mmc_logger.h"

namespace ock {
namespace mmc {

#define DL_LOAD_SYM(TARGET_FUNC_VAR, TARGET_FUNC_TYPE, FILE_HANDLE, SYMBOL_NAME)           \
    do {                                                                                   \
        TARGET_FUNC_VAR = (TARGET_FUNC_TYPE)dlsym(FILE_HANDLE, SYMBOL_NAME);               \
        if ((TARGET_FUNC_VAR) == nullptr) {                                                \
            MMC_LOG_ERROR("Failed to call dlsym to load SYMBOL_NAME, error" << dlerror()); \
            dlclose(FILE_HANDLE);                                                          \
            return MMC_ERROR;                                                              \
        }                                                                                  \
    } while (0)

template<typename FuncPtr>
inline void DlLoadSymOptional(FuncPtr &targetFunc, void *handle, const char *symbolName)
{
    targetFunc = reinterpret_cast<FuncPtr>(dlsym(handle, symbolName));
    if (targetFunc == nullptr) {
        MMC_LOG_WARN("Failed to call dlsym to load " << symbolName << " (non-fatal), error: " << dlerror());
    }
}

class Func {
public:
    /**
     * @brief Get real path
     *
     * @param path         [in/out] input path, converted realpath
     * @return true if successful
     */
    static bool Realpath(std::string &path);

    /**
     * @brief Get real path of a library and check if exists
     *
     * @param libDirPath   [in] dir path of the library
     * @param libName      [in] library name
     * @param realPath     [out] realpath of the library
     * @return 0 if successful
     */
    static Result LibraryRealPath(const std::string &libDirPath, const std::string &libName, std::string &realPath);
};

inline bool Func::Realpath(std::string &path)
{
    if (path.empty() || path.size() > PATH_MAX) {
        MMC_LOG_ERROR("Failed to get realpath of [" << path << "] as path is invalid");
        return false;
    }

    /* It will allocate memory to store path */
    char *realPath = realpath(path.c_str(), nullptr);
    if (realPath == nullptr) {
        MMC_LOG_ERROR("Failed to get realpath of [" << path << "] as error " << errno);
        return false;
    }

    path = realPath;
    free(realPath);
    realPath = nullptr;
    return true;
}

inline Result Func::LibraryRealPath(const std::string &libDirPath, const std::string &libName, std::string &realPath)
{
    std::string tmpFullPath = libDirPath;
    if (!Realpath(tmpFullPath)) {
        MMC_LOG_ERROR("directory is a symlink.");
        return MMC_INVALID_PARAM;
    }

    if (tmpFullPath.back() != '/') {
        tmpFullPath.push_back('/');
    }

    tmpFullPath.append(libName);

    if (!Realpath(tmpFullPath)) {
        MMC_LOG_ERROR("library path is a symlink.");
        return MMC_INVALID_PARAM;
    }

    auto ret = ::access(tmpFullPath.c_str(), F_OK);
    if (ret != 0) {
        MMC_LOG_ERROR(tmpFullPath << " cannot be accessed, ret: " << ret);
        return MMC_ERROR;
    }

    realPath = tmpFullPath;
    return MMC_OK;
}

/**
 * 校验路径存在且不是软链接
 * @param path 要校验的路径
 * @return true: 路径存在且不是软链接, false: 路径不存在或是软链接
 */
inline int ValidatePathNotSymlink(const char *path)
{
    struct stat path_stat {};

    if (path == nullptr) {
        MMC_LOG_ERROR("null path");
        return MMC_INVALID_PARAM;
    }

    // 检查路径是否存在
    if (access(path, F_OK) != 0) {
        MMC_LOG_ERROR("path " << path << " does not exist. ");
        return MMC_ERROR;
    }

    // 使用lstat检查是否为软链接
    if (lstat(path, &path_stat) != 0) {
        MMC_LOG_ERROR("lstat failed for path " << path << ", error: " << errno);
        return MMC_ERROR;
    }

    // 检查是否为软链接
    if (S_ISLNK(path_stat.st_mode)) {
        MMC_LOG_ERROR("path " << path << " is a symlink. ");
        return MMC_ERROR;
    }

    return MMC_OK;
}

inline void SafeCopy(const std::string &src, char *dst, const size_t dstSize)
{
    if (dst == nullptr || dstSize == 0) {
        return;
    }
    const size_t count = std::min(src.length(), dstSize - 1);
    std::copy_n(src.c_str(), count, dst);
    dst[count] = '\0';
}

inline std::string SafeGetEnv(const char *name) noexcept
{
    const auto value = std::getenv(name);
    if (value == nullptr) {
        return "";
    }
    return value;
}

/**
 * @brief 把字节数格式化为人类可读容量，风格接近 `free -h`。
 *
 * 规则：
 *   - 0~1023 输出 "<n>B"；
 *   - >= 1KiB 按 1024 进制选择 KiB/MiB/GiB/TiB/PiB/EiB；
 *   - 数值小于 10 时保留 1 位小数，否则取整（与 free -h 的有效位风格一致）；
 *   - 四舍五入会到达 1024 时先进位到下一单位，避免出现 "1024.0KiB"。
 * 示例：28179398656 -> "26GiB"，4509715660800 -> "4.1TiB"，0 -> "0B"。
 */
inline std::string FormatBytesHuman(uint64_t bytes)
{
    constexpr uint64_t kBytesPerUnit = 1024;
    constexpr double kBytesPerUnitValue = static_cast<double>(kBytesPerUnit);
    constexpr double kOneDecimalUpperBound = 10.0;
    constexpr double kUnitCarryBoundary = 1023.5; // 取整后会显示成 1024，需进位到下一单位
    constexpr int kOneDecimalPlaces = 1;
    constexpr int kNoDecimalPlaces = 0;
    static const char *const kUnitSuffixes[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB"};
    constexpr size_t kMaxUnitIndex = sizeof(kUnitSuffixes) / sizeof(kUnitSuffixes[0]) - 1;

    if (bytes < kBytesPerUnit) {
        return std::to_string(bytes) + kUnitSuffixes[0];
    }

    double value = static_cast<double>(bytes);
    size_t unitIndex = 0;
    while (unitIndex < kMaxUnitIndex && value >= kBytesPerUnitValue) {
        value /= kBytesPerUnitValue;
        ++unitIndex;
    }
    if (unitIndex < kMaxUnitIndex && value >= kUnitCarryBoundary) {
        value /= kBytesPerUnitValue;
        ++unitIndex;
    }

    std::ostringstream text;
    text << std::fixed;
    if (value < kOneDecimalUpperBound) {
        text << std::setprecision(kOneDecimalPlaces);
    } else {
        text << std::setprecision(kNoDecimalPlaces);
    }
    text << value << kUnitSuffixes[unitIndex];
    return text.str();
}

/**
 * @brief 把「已用/总量」格式化为人类可读文本，并追加使用率百分比。
 *
 * 示例：used=28179398656，total=4509715660800 -> "26GiB/4.1TiB (0.62%)"。
 * total 为 0（例如未配置 SSD）时不输出百分比，只返回 "0B/0B"。
 */
inline std::string FormatUsageHuman(uint64_t usedBytes, uint64_t totalBytes)
{
    constexpr double kPercentScale = 100.0;
    constexpr int kPercentDecimalPlaces = 2;

    std::string text = FormatBytesHuman(usedBytes) + "/" + FormatBytesHuman(totalBytes);
    if (totalBytes == 0) {
        return text;
    }

    std::ostringstream percent;
    percent << std::fixed << std::setprecision(kPercentDecimalPlaces)
            << kPercentScale * static_cast<double>(usedBytes) / static_cast<double>(totalBytes);
    return text + " (" + percent.str() + "%)";
}

// bit ops
template<typename T>
inline void SetBit(T &value, uint32_t bit)
{
    constexpr uint32_t maxBits = sizeof(T) * 8;
    if (bit < maxBits) {
        value |= (T{1} << bit);
    }
}

template<typename T>
inline bool IsBitSet(T value, uint32_t bit)
{
    constexpr uint32_t maxBits = sizeof(T) * 8;
    return (bit < maxBits) && ((value & (T{1} << bit)) != 0);
}

template<typename T>
inline void ClearBit(T &value, uint32_t bit)
{
    constexpr uint32_t maxBits = sizeof(T) * 8;
    if (bit < maxBits) {
        value &= ~(T{1} << bit);
    }
}

template<typename T>
inline void ToggleBit(T &value, uint32_t bit)
{
    constexpr uint32_t maxBits = sizeof(T) * 8;
    if (bit < maxBits) {
        value ^= (T{1} << bit); // 翻转指定位
    }
}

template<typename T>
inline void SetBits(T &value, T mask)
{
    value |= mask;
}

/**
 * @brief 判断网卡名是否属于「虚拟/非业务」网卡（用于本机 IP 兜底时过滤）
 * @param name 网卡名，例如 eth0 / docker0 / vethxxxx / br-xxxx
 * @return true 表示应跳过
 */
inline bool IsVirtualNetDeviceName(const std::string &name)
{
    static const char *const kVirtualPrefixes[] = {"lo",  "docker",  "veth", "br-",   "tun",  "tap",
                                                   "cni", "flannel", "cali", "virbr", "kube", "dummy"};
    for (const auto *prefix : kVirtualPrefixes) {
        const std::string p(prefix);
        if (name.compare(0, p.size(), p) == 0) {
            return true;
        }
    }
    return false;
}

/**
 * @brief 尝试找一个本机可用的 IP（Linux 平台兜底用，例如 backend_id 未配置时）
 *
 * 实现说明：用 getifaddrs() 枚举本机全部网卡地址（底层走 netlink RTM_GETADDR，
 * 返回 ifaddrs 链表，每项带 网卡名 / flags / 地址），按下列规则挑一个：
 *   1. 跳过 down、loopback、以及常见虚拟网卡（见 IsVirtualNetDeviceName）；
 *   2. 优先 IPv4；只有完全没有 IPv4 时才退到 IPv6（跳过 link-local / loopback / 未指定）；
 *   3. 同等条件下优先 eth0（仅作为偏好，没有 eth0 时取第一个可用网卡）。
 *
 * @return 形如 "192.168.0.55" 或 "fd00::1" 的地址；找不到返回空串
 * @note 容器 net=host 时拿到的是宿主地址；容器独立网络命名空间时拿到的是容器地址，
 *       同机多实例仍会得到同一个值，可满足「同机共享 backendId」的语义。
 */
inline std::string TryFindLinuxFirstIp()
{
    struct ifaddrs *ifap = nullptr;
    if (getifaddrs(&ifap) != 0) {
        MMC_LOG_DEBUG("TryFindLinuxFirstIp: getifaddrs failed");
        return std::string();
    }

    std::string firstIpv4;
    std::string eth0Ipv4;
    std::string firstIpv6;
    std::string eth0Ipv6;

    for (auto *ifa = ifap; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr) {
            continue;
        }
        const int family = ifa->ifa_addr->sa_family;
        if (family != AF_INET && family != AF_INET6) {
            continue;
        }
        if ((ifa->ifa_flags & IFF_UP) == 0 || (ifa->ifa_flags & IFF_LOOPBACK) != 0) {
            continue;
        }
        const std::string devName = (ifa->ifa_name != nullptr) ? ifa->ifa_name : "";
        if (IsVirtualNetDeviceName(devName)) {
            continue;
        }

        char buf[INET6_ADDRSTRLEN] = {0};
        if (family == AF_INET) {
            auto *addr4 = reinterpret_cast<struct sockaddr_in *>(ifa->ifa_addr);
            if (inet_ntop(AF_INET, &addr4->sin_addr, buf, sizeof(buf)) == nullptr) {
                continue;
            }
            if (firstIpv4.empty()) {
                firstIpv4 = buf;
            }
            if (eth0Ipv4.empty() && devName == "eth0") {
                eth0Ipv4 = buf;
            }
            continue;
        }

        auto *addr6 = reinterpret_cast<struct sockaddr_in6 *>(ifa->ifa_addr);
        const struct in6_addr *in6 = &addr6->sin6_addr;
        if (IN6_IS_ADDR_LINKLOCAL(in6) || IN6_IS_ADDR_LOOPBACK(in6) || IN6_IS_ADDR_UNSPECIFIED(in6)) {
            continue;
        }
        if (inet_ntop(AF_INET6, in6, buf, sizeof(buf)) == nullptr) {
            continue;
        }
        if (firstIpv6.empty()) {
            firstIpv6 = buf;
        }
        if (eth0Ipv6.empty() && devName == "eth0") {
            eth0Ipv6 = buf;
        }
    }
    freeifaddrs(ifap);

    if (!eth0Ipv4.empty()) {
        return eth0Ipv4;
    }
    if (!firstIpv4.empty()) {
        return firstIpv4;
    }
    if (!eth0Ipv6.empty()) {
        return eth0Ipv6;
    }
    return firstIpv6;
}

} // namespace mmc
} // namespace ock
#endif // MEM_FABRIC_HYBRID_SMEM_COMMON_FUNC_H
