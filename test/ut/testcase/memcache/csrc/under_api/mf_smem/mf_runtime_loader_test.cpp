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
#include <dlfcn.h>
#include <cstring>
#include <gtest/gtest.h>
#include "smem_acc_api.h"
#include "smem_bm_api.h"
#include "smem_version_api.h"

namespace ock::mmc {
namespace {
const char *gMissingSymbol = nullptr;
char gMissingSymbolError[] = "required symbol missing in UT";
bool gMissingLibrary = false;
bool gIncompatibleVersion = false;
bool gReportLookupError = true;
bool gPendingError = false;
void *gTrackedHandle = nullptr;
unsigned int gCloseCount = 0;

uint32_t IncompatibleAbiVersion()
{
    constexpr uint32_t kMajorShift = 16;
    constexpr uint32_t kDifferentMinorBit = 1;
    return (static_cast<uint32_t>(VERSION_MAJOR) << kMajorShift) |
           (static_cast<uint32_t>(VERSION_MINOR) ^ kDifferentMinorBit);
}

const char *const kRequiredSymbols[] = {"smem_acc_tcp_server_create",
                                        "smem_acc_http_server_create",
                                        "smem_acc_server_destroy",
                                        "smem_acc_tcp_server_start",
                                        "smem_acc_http_server_start",
                                        "smem_acc_server_stop",
                                        "smem_acc_server_load_tls_library",
                                        "smem_acc_server_set_decrypt_handler",
                                        "smem_acc_server_set_new_link_handler",
                                        "smem_acc_server_set_request_handler_v2",
                                        "smem_acc_server_set_sent_handler_v2",
                                        "smem_acc_server_set_link_broken_handler",
                                        "smem_acc_server_connect",
                                        "smem_acc_http_server_register_handler",
                                        "smem_acc_link_retain",
                                        "smem_acc_link_release",
                                        "smem_acc_link_id",
                                        "smem_acc_link_set_context",
                                        "smem_acc_link_get_context",
                                        "smem_acc_link_send_v2",
                                        "smem_acc_http_request_body",
                                        "smem_acc_http_request_param",
                                        "smem_acc_http_request_visit_params",
                                        "smem_acc_http_request_reply",
                                        "smem_init",
                                        "smem_uninit",
                                        "smem_set_extern_logger",
                                        "smem_set_log_level",
                                        "smem_get_last_err_msg",
                                        "smem_get_and_clear_last_err_msg",
                                        "smem_bm_config_init",
                                        "smem_bm_init",
                                        "smem_bm_uninit",
                                        "smem_bm_get_rank_id",
                                        "smem_bm_create2",
                                        "smem_bm_destroy",
                                        "smem_bm_join",
                                        "smem_bm_ptr_by_mem_type",
                                        "smem_bm_get_local_mem_size_by_mem_type",
                                        "smem_bm_copy",
                                        "smem_bm_copy_batch",
                                        "smem_bm_register_user_mem",
                                        "smem_bm_unregister_user_mem",
                                        "smem_bm_wait",
                                        "smem_bm_gva_to_va",
                                        "smem_bm_update_store_url",
                                        "smem_bm_get_meta_service_info"};
} // namespace

extern "C" void *__real_dlopen(const char *, int);
extern "C" void *__real_dlsym(void *, const char *);
extern "C" char *__real_dlerror();
extern "C" int __real_dlclose(void *);

// Link-time wrapping keeps failure injection reliable on ARM without patching instructions.
extern "C" void *__wrap_dlopen(const char *name, int flags)
{
    if (gMissingLibrary) {
        gPendingError = true;
        return nullptr;
    }
    return __real_dlopen(name, flags);
}

extern "C" void *__wrap_dlsym(void *handle, const char *name)
{
    if (gMissingSymbol != nullptr && std::strcmp(name, gMissingSymbol) == 0) {
        gPendingError = gReportLookupError;
        return nullptr;
    }
    if (gIncompatibleVersion && std::strcmp(name, "smem_get_abi_version") == 0) {
        return reinterpret_cast<void *>(&IncompatibleAbiVersion);
    }
    return __real_dlsym(handle, name);
}

extern "C" char *__wrap_dlerror()
{
    if (gPendingError) {
        gPendingError = false;
        return gMissingSymbolError;
    }
    return __real_dlerror();
}

extern "C" int __wrap_dlclose(void *handle)
{
    if (handle == gTrackedHandle) {
        ++gCloseCount;
    }
    return __real_dlclose(handle);
}

class MfRuntimeLoaderTest : public testing::TestWithParam<const char *> {
public:
    void SetUp() override
    {
        MFAccApi::CleanupLibrary();
        MFSmemApi::CleanupLibrary();
        handle_ = dlopen("libmf_smem.so", RTLD_NOW | RTLD_LOCAL);
        ASSERT_NE(handle_, nullptr);
    }

    void TearDown() override
    {
        gMissingSymbol = nullptr;
        gMissingLibrary = false;
        gIncompatibleVersion = false;
        gReportLookupError = true;
        gPendingError = false;
        gTrackedHandle = nullptr;
        MFAccApi::CleanupLibrary();
        MFSmemApi::CleanupLibrary();
        if (handle_ != nullptr) {
            dlclose(handle_);
        }
    }

protected:
    void *handle_{nullptr};
};

TEST_P(MfRuntimeLoaderTest, MissingRequiredSymbolClosesHandleOnceAndAllowsReload)
{
    gMissingSymbol = GetParam();
    constexpr size_t kAccPrefixLength = sizeof("smem_acc_") - 1;
    const bool isAcc = std::strncmp(gMissingSymbol, "smem_acc_", kAccPrefixLength) == 0;
    const auto load = isAcc ? MFAccApi::LoadLibrary : MFSmemApi::LoadLibrary;
    constexpr unsigned int kSingleClose = 1;
    gTrackedHandle = handle_;
    gCloseCount = 0;
    EXPECT_NE(load(), MMC_OK) << gMissingSymbol;
    EXPECT_EQ(gCloseCount, kSingleClose) << gMissingSymbol;
    gMissingSymbol = nullptr;
    EXPECT_EQ(load(), MMC_OK) << gMissingSymbol;
}

INSTANTIATE_TEST_SUITE_P(RequiredSymbols, MfRuntimeLoaderTest, testing::ValuesIn(kRequiredSymbols));

TEST_F(MfRuntimeLoaderTest, RejectsMissingAbiVersionSymbol)
{
    gMissingSymbol = "smem_get_abi_version";
    gReportLookupError = false;
    EXPECT_EQ(CheckMemfabricAbiVersion(handle_), MMC_NOT_INITIALIZED);
}

TEST_F(MfRuntimeLoaderTest, RejectsMissingLibrary)
{
    gMissingLibrary = true;
    EXPECT_NE(MFAccApi::LoadLibrary(), MMC_OK);
    EXPECT_NE(MFSmemApi::LoadLibrary(), MMC_OK);
}

TEST_F(MfRuntimeLoaderTest, RejectsAbiSymbolLookupError)
{
    gMissingSymbol = "smem_get_abi_version";
    EXPECT_EQ(CheckMemfabricAbiVersion(handle_), MMC_NOT_INITIALIZED);
}

TEST_F(MfRuntimeLoaderTest, RejectsDifferentMajorMinorVersion)
{
    gIncompatibleVersion = true;
    EXPECT_EQ(CheckMemfabricAbiVersion(handle_), MMC_NOT_INITIALIZED);
}
} // namespace ock::mmc
