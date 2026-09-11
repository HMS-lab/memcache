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
#include <dlfcn.h>

#include "mmc_functions.h"
#include "smem_version_api.h"
#include "smem_bm_api.h"

namespace ock {
namespace mmc {

bool MFSmemApi::gLoaded = false;
std::mutex MFSmemApi::gMutex;
void *MFSmemApi::gSmemHandle = nullptr;
const char *MFSmemApi::gSmemLibName = "libmf_smem.so";

smemInitFunc MFSmemApi::gSmemInit = nullptr;
smemUnInitFunc MFSmemApi::gSmemUnInit = nullptr;
smemSetExternLoggerFunc MFSmemApi::gSmemSetExternLogger = nullptr;
smemSetLogLevelFunc MFSmemApi::gSmemSetLogLevel = nullptr;
smemGetLastErrMsgFunc MFSmemApi::gSemGetLastErrMsg = nullptr;
smemGetAndClearLastErrMsgFunc MFSmemApi::gSmemGetAndClearLastErrMsg = nullptr;

smemBmConfigInitFunc MFSmemApi::gSmemBmConfigInit = nullptr;
smemBmInitFunc MFSmemApi::gSmemBmInit = nullptr;
smemBmUnInitFunc MFSmemApi::gSmemBmUnInit = nullptr;
smemBmGetRankIdFunc MFSmemApi::gSmemBmGetRankId = nullptr;
smemBmCreate2Func MFSmemApi::gSmemBmCreate2 = nullptr;
smemBmDestroyFunc MFSmemApi::gSmemBmDestroy = nullptr;
smemBmJoinFunc MFSmemApi::gSmemBmJoin = nullptr;
smemBmPtrByMemTypeFunc MFSmemApi::gSmemBmPtrByMemType = nullptr;
smemBmGetLocalMemSizeByMemTypeFunc MFSmemApi::gSmemBmGetLocalMemSizeByMemType = nullptr;
smemBmCopyFunc MFSmemApi::gSmemBmCopy = nullptr;
smemBmCopyBatchFunc MFSmemApi::gSmemBmCopyBatch = nullptr;
smemBmRegisterUserMemFunc MFSmemApi::gSmemBmRegisterUserMem = nullptr;
smemBmUnregisterUserMemFunc MFSmemApi::gSmemBmUnregisterUserMem = nullptr;
smemBmWaitFunc MFSmemApi::gSmemBmWait = nullptr;
smemBmGvaToVaFunc MFSmemApi::gSmemBmGvaToVa = nullptr;
smemBmUpdateStoreUrlFunc MFSmemApi::gSmemBmUpdateStoreUrl = nullptr;
smemBmGetMetaServiceInfoFunc MFSmemApi::gSmemBmGetMetaServiceInfo = nullptr;
smemBmRegisterLeaderChangeCallbackFunc MFSmemApi::gSmemBmRegisterLeaderChangeCallback = nullptr;

Result MFSmemApi::LoadAllSymbols()
{
    DL_LOAD_SYM(gSmemInit, smemInitFunc, gSmemHandle, "smem_init");
    DL_LOAD_SYM(gSmemUnInit, smemUnInitFunc, gSmemHandle, "smem_uninit");
    DL_LOAD_SYM(gSmemSetExternLogger, smemSetExternLoggerFunc, gSmemHandle, "smem_set_extern_logger");
    DL_LOAD_SYM(gSmemSetLogLevel, smemSetLogLevelFunc, gSmemHandle, "smem_set_log_level");
    DL_LOAD_SYM(gSemGetLastErrMsg, smemGetLastErrMsgFunc, gSmemHandle, "smem_get_last_err_msg");
    DL_LOAD_SYM(gSmemGetAndClearLastErrMsg, smemGetAndClearLastErrMsgFunc, gSmemHandle,
                "smem_get_and_clear_last_err_msg");
    DL_LOAD_SYM(gSmemBmConfigInit, smemBmConfigInitFunc, gSmemHandle, "smem_bm_config_init");
    DL_LOAD_SYM(gSmemBmInit, smemBmInitFunc, gSmemHandle, "smem_bm_init");
    DL_LOAD_SYM(gSmemBmUnInit, smemBmUnInitFunc, gSmemHandle, "smem_bm_uninit");
    DL_LOAD_SYM(gSmemBmGetRankId, smemBmGetRankIdFunc, gSmemHandle, "smem_bm_get_rank_id");
    DL_LOAD_SYM(gSmemBmCreate2, smemBmCreate2Func, gSmemHandle, "smem_bm_create2");
    DL_LOAD_SYM(gSmemBmDestroy, smemBmDestroyFunc, gSmemHandle, "smem_bm_destroy");
    DL_LOAD_SYM(gSmemBmJoin, smemBmJoinFunc, gSmemHandle, "smem_bm_join");
    DL_LOAD_SYM(gSmemBmPtrByMemType, smemBmPtrByMemTypeFunc, gSmemHandle, "smem_bm_ptr_by_mem_type");
    DL_LOAD_SYM(gSmemBmGetLocalMemSizeByMemType, smemBmGetLocalMemSizeByMemTypeFunc, gSmemHandle,
                "smem_bm_get_local_mem_size_by_mem_type");
    DL_LOAD_SYM(gSmemBmCopy, smemBmCopyFunc, gSmemHandle, "smem_bm_copy");
    DL_LOAD_SYM(gSmemBmCopyBatch, smemBmCopyBatchFunc, gSmemHandle, "smem_bm_copy_batch");
    DL_LOAD_SYM(gSmemBmRegisterUserMem, smemBmRegisterUserMemFunc, gSmemHandle, "smem_bm_register_user_mem");
    DL_LOAD_SYM(gSmemBmUnregisterUserMem, smemBmUnregisterUserMemFunc, gSmemHandle, "smem_bm_unregister_user_mem");
    DL_LOAD_SYM(gSmemBmWait, smemBmWaitFunc, gSmemHandle, "smem_bm_wait");
    DL_LOAD_SYM(gSmemBmGvaToVa, smemBmGvaToVaFunc, gSmemHandle, "smem_bm_gva_to_va");
    DL_LOAD_SYM(gSmemBmUpdateStoreUrl, smemBmUpdateStoreUrlFunc, gSmemHandle, "smem_bm_update_store_url");
    DL_LOAD_SYM(gSmemBmGetMetaServiceInfo, smemBmGetMetaServiceInfoFunc, gSmemHandle, "smem_bm_get_meta_service_info");
    DL_LOAD_SYM(gSmemBmRegisterLeaderChangeCallback, smemBmRegisterLeaderChangeCallbackFunc, gSmemHandle,
                "smem_bm_register_leader_change_callback");
    return MMC_OK;
}

Result MFSmemApi::LoadLibrary()
{
    std::lock_guard<std::mutex> guard(gMutex);
    if (gLoaded) {
        return MMC_OK;
    }

    MMC_LOG_TRACE("MFSmemApi LoadLibrary: dlopen " << gSmemLibName);
    gSmemHandle = dlopen(gSmemLibName, RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND);
    if (gSmemHandle == nullptr) {
        MMC_LOG_ERROR("MFSmemApi LoadLibrary: dlopen failed, lib: " << gSmemLibName << ", error: " << dlerror());
        return MMC_ERROR;
    }

    if (CheckMemfabricAbiVersion(gSmemHandle) != MMC_OK) {
        dlclose(gSmemHandle);
        gSmemHandle = nullptr;
        return MMC_NOT_INITIALIZED;
    }

    if (LoadAllSymbols() != MMC_OK) {
        MMC_LOG_ERROR("MFSmemApi LoadLibrary: dlsym smem_bm symbols failed, lib: " << gSmemLibName);
        // DL_LOAD_SYM closes the handle when a required symbol is missing.
        gSmemHandle = nullptr;
        ClearAllSymbols();
        return MMC_ERROR;
    }

    gLoaded = true;
    MMC_LOG_TRACE("MFSmemApi LoadLibrary: loaded 23 smem_bm symbols from " << gSmemLibName);
    return MMC_OK;
}

void MFSmemApi::ClearAllSymbols()
{
    gSmemInit = nullptr;
    gSmemUnInit = nullptr;
    gSmemSetExternLogger = nullptr;
    gSmemSetLogLevel = nullptr;
    gSemGetLastErrMsg = nullptr;
    gSmemGetAndClearLastErrMsg = nullptr;
    gSmemBmConfigInit = nullptr;
    gSmemBmInit = nullptr;
    gSmemBmUnInit = nullptr;
    gSmemBmGetRankId = nullptr;
    gSmemBmCreate2 = nullptr;
    gSmemBmDestroy = nullptr;
    gSmemBmJoin = nullptr;
    gSmemBmPtrByMemType = nullptr;
    gSmemBmGetLocalMemSizeByMemType = nullptr;
    gSmemBmCopy = nullptr;
    gSmemBmCopyBatch = nullptr;
    gSmemBmRegisterUserMem = nullptr;
    gSmemBmUnregisterUserMem = nullptr;
    gSmemBmWait = nullptr;
    gSmemBmGvaToVa = nullptr;
    gSmemBmUpdateStoreUrl = nullptr;
    gSmemBmGetMetaServiceInfo = nullptr;
    gSmemBmRegisterLeaderChangeCallback = nullptr;
}

void MFSmemApi::CleanupLibrary()
{
    std::lock_guard<std::mutex> guard(gMutex);
    if (!gLoaded) {
        return;
    }

    ClearAllSymbols();
    if (gSmemHandle != nullptr) {
        dlclose(gSmemHandle);
        gSmemHandle = nullptr;
    }
    gLoaded = false;
    MMC_LOG_TRACE("MFSmemApi CleanupLibrary: unloaded " << gSmemLibName);
}
} // namespace mmc
} // namespace ock
