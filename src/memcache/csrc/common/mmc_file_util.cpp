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

#include "mmc_file_util.h"

#include <array>

namespace ock {
namespace mmc {

bool FileUtil::Realpath(std::string &path)
{
    if (path.empty() || path.size() > PATH_MAX_LIMIT) {
        return false;
    }

    std::array<char, PATH_MAX_LIMIT + 1> resolvedPath{};
    if (realpath(path.c_str(), resolvedPath.data()) == nullptr) {
        return false;
    }

    path = resolvedPath.data();
    return true;
}

size_t FileUtil::GetFileSize(const std::string &path)
{
    if (!Exist(path)) {
        return 0;
    }

    std::string realFilePath = path;
    if (!Realpath(realFilePath)) {
        return 0;
    }

    FILE *fp = fopen(realFilePath.c_str(), "rb");
    if (fp == nullptr) {
        return 0;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        CloseFile(fp);
        return 0;
    }

    const long pos = ftell(fp);
    if (pos < 0) {
        CloseFile(fp);
        return 0;
    }
    const size_t fileSize = static_cast<size_t>(pos);
    if (fseek(fp, 0, SEEK_END) != 0) {
        CloseFile(fp);
        return 0;
    }

    CloseFile(fp);
    return fileSize;
}

} // namespace mmc
} // namespace ock
