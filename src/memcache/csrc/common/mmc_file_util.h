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
#ifndef MEMCACHE_MMC_FILE_UTIL_H
#define MEMCACHE_MMC_FILE_UTIL_H

#include <cstdint>
#include <cstdio>
#include <climits>
#include <string>
#include <unistd.h>

namespace ock {
namespace mmc {
constexpr size_t PATH_MAX_LIMIT = 4096L;

class FileUtil {
public:
    static constexpr size_t GetSafePathMax();
    static bool Exist(const std::string &path);
    static bool Realpath(std::string &path);
    static size_t GetFileSize(const std::string &filePath);
    static void CloseFile(FILE *fp);
    static bool CheckFileSize(const std::string &path, uint32_t maxSize);
};

inline bool FileUtil::Exist(const std::string &path)
{
    return access(path.c_str(), 0) != -1;
}

inline void FileUtil::CloseFile(FILE *fp)
{
    if (fp == nullptr) {
        return;
    }

    (void)fclose(fp);
}

inline bool FileUtil::CheckFileSize(const std::string &path, uint32_t maxSize)
{
    if (!Exist(path)) {
        return false;
    }

    return GetFileSize(path) <= static_cast<size_t>(maxSize);
}

inline constexpr size_t FileUtil::GetSafePathMax()
{
#ifdef PATH_MAX
    return (PATH_MAX < PATH_MAX_LIMIT) ? PATH_MAX : PATH_MAX_LIMIT;
#else
    return PATH_MAX_LIMIT;
#endif
}

} // namespace mmc
} // namespace ock

#endif // MEMCACHE_MMC_FILE_UTIL_H
