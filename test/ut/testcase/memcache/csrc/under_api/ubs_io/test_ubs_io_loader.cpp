/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 * MemCache_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#include <cstring>
#include <gtest/gtest.h>
#include <mockcpp/mockcpp.hpp>
#include "dl_ubsio_api.h"

using namespace ock::mmc;

namespace {
int32_t UnusedInit(int32_t)
{
    return MMC_OK;
}

void UnusedExit() {}

void *LoadWithoutUnderFsTypeQuery(void *, const char *name)
{
    if (std::strcmp(name, "UbsioGetUnderFsType") == 0) {
        return nullptr;
    }
    if (std::strcmp(name, "UbsioKvCacheExit") == 0) {
        return reinterpret_cast<void *>(UnusedExit);
    }
    return reinterpret_cast<void *>(UnusedInit);
}
} // namespace

TEST(UbsIoLoaderTest, MissingUnderFsTypeSymbolRejectsLibrary)
{
    DlUbsioApi::CleanupLibrary();
    int handleStorage = 0;
    void *handle = &handleStorage;
    MOCKER(static_cast<void *(*)(const char *, int)>(dlopen)).stubs().will(returnValue(handle));
    MOCKER(static_cast<void *(*)(void *, const char *)>(dlsym)).stubs().will(invoke(LoadWithoutUnderFsTypeQuery));
    MOCKER(static_cast<int (*)(void *)>(dlclose)).expects(once()).with(eq(handle)).will(returnValue(0));
    EXPECT_EQ(DlUbsioApi::LoadLibrary(), MMC_ERROR);
    UbsioUnderFsType type = UBSIO_UNDERFS_BUTT;
    EXPECT_EQ(DlUbsioApi::UbsioGetUnderFsType(type), MMC_NOT_INITIALIZED);
    EXPECT_EQ(type, UBSIO_UNDERFS_BUTT);
    DlUbsioApi::CleanupLibrary();
    GlobalMockObject::verify();
}
