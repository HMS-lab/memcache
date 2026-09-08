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

#include "mmc_functions.h"
#include "smem_version_api.h"
#include "smem_acc_api.h"

namespace ock {
namespace mmc {

bool MFAccApi::gLoaded = false;
std::mutex MFAccApi::gMutex;
void *MFAccApi::gSmemHandle = nullptr;
const char *MFAccApi::gSmemLibName = "libmf_smem.so";

smemAccTcpServerCreateFunc MFAccApi::gSmemAccTcpServerCreate = nullptr;
smemAccHttpServerCreateFunc MFAccApi::gSmemAccHttpServerCreate = nullptr;
smemAccServerDestroyFunc MFAccApi::gSmemAccServerDestroy = nullptr;
smemAccTcpServerStartFunc MFAccApi::gSmemAccTcpServerStart = nullptr;
smemAccHttpServerStartFunc MFAccApi::gSmemAccHttpServerStart = nullptr;
smemAccServerStopFunc MFAccApi::gSmemAccServerStop = nullptr;
smemAccServerLoadTlsLibraryFunc MFAccApi::gSmemAccServerLoadTlsLibrary = nullptr;
smemAccServerSetDecryptHandlerFunc MFAccApi::gSmemAccServerSetDecryptHandler = nullptr;
smemAccServerSetNewLinkHandlerFunc MFAccApi::gSmemAccServerSetNewLinkHandler = nullptr;
smemAccServerSetRequestHandlerFunc MFAccApi::gSmemAccServerSetRequestHandler = nullptr;
smemAccServerSetSentHandlerFunc MFAccApi::gSmemAccServerSetSentHandler = nullptr;
smemAccServerSetLinkBrokenHandlerFunc MFAccApi::gSmemAccServerSetLinkBrokenHandler = nullptr;
smemAccServerConnectFunc MFAccApi::gSmemAccServerConnect = nullptr;
smemAccHttpServerRegisterHandlerFunc MFAccApi::gSmemAccHttpServerRegisterHandler = nullptr;
smemAccLinkRetainFunc MFAccApi::gSmemAccLinkRetain = nullptr;
smemAccLinkReleaseFunc MFAccApi::gSmemAccLinkRelease = nullptr;
smemAccLinkIdFunc MFAccApi::gSmemAccLinkId = nullptr;
smemAccLinkSetContextFunc MFAccApi::gSmemAccLinkSetContext = nullptr;
smemAccLinkGetContextFunc MFAccApi::gSmemAccLinkGetContext = nullptr;
smemAccLinkSendFunc MFAccApi::gSmemAccLinkSend = nullptr;
smemAccHttpRequestBodyFunc MFAccApi::gSmemAccHttpRequestBody = nullptr;
smemAccHttpRequestParamFunc MFAccApi::gSmemAccHttpRequestParam = nullptr;
smemAccHttpRequestVisitParamsFunc MFAccApi::gSmemAccHttpRequestVisitParams = nullptr;
smemAccHttpRequestReplyFunc MFAccApi::gSmemAccHttpRequestReply = nullptr;

Result MFAccApi::LoadAllSymbols()
{
    DL_LOAD_SYM(gSmemAccTcpServerCreate, smemAccTcpServerCreateFunc, gSmemHandle, "smem_acc_tcp_server_create");
    DL_LOAD_SYM(gSmemAccHttpServerCreate, smemAccHttpServerCreateFunc, gSmemHandle, "smem_acc_http_server_create");
    DL_LOAD_SYM(gSmemAccServerDestroy, smemAccServerDestroyFunc, gSmemHandle, "smem_acc_server_destroy");
    DL_LOAD_SYM(gSmemAccTcpServerStart, smemAccTcpServerStartFunc, gSmemHandle, "smem_acc_tcp_server_start");
    DL_LOAD_SYM(gSmemAccHttpServerStart, smemAccHttpServerStartFunc, gSmemHandle, "smem_acc_http_server_start");
    DL_LOAD_SYM(gSmemAccServerStop, smemAccServerStopFunc, gSmemHandle, "smem_acc_server_stop");
    DL_LOAD_SYM(gSmemAccServerLoadTlsLibrary, smemAccServerLoadTlsLibraryFunc, gSmemHandle,
                "smem_acc_server_load_tls_library");
    DL_LOAD_SYM(gSmemAccServerSetDecryptHandler, smemAccServerSetDecryptHandlerFunc, gSmemHandle,
                "smem_acc_server_set_decrypt_handler");
    DL_LOAD_SYM(gSmemAccServerSetNewLinkHandler, smemAccServerSetNewLinkHandlerFunc, gSmemHandle,
                "smem_acc_server_set_new_link_handler");
    DL_LOAD_SYM(gSmemAccServerSetRequestHandler, smemAccServerSetRequestHandlerFunc, gSmemHandle,
                "smem_acc_server_set_request_handler_v2");
    DL_LOAD_SYM(gSmemAccServerSetSentHandler, smemAccServerSetSentHandlerFunc, gSmemHandle,
                "smem_acc_server_set_sent_handler_v2");
    DL_LOAD_SYM(gSmemAccServerSetLinkBrokenHandler, smemAccServerSetLinkBrokenHandlerFunc, gSmemHandle,
                "smem_acc_server_set_link_broken_handler");
    DL_LOAD_SYM(gSmemAccServerConnect, smemAccServerConnectFunc, gSmemHandle, "smem_acc_server_connect");
    DL_LOAD_SYM(gSmemAccHttpServerRegisterHandler, smemAccHttpServerRegisterHandlerFunc, gSmemHandle,
                "smem_acc_http_server_register_handler");
    DL_LOAD_SYM(gSmemAccLinkRetain, smemAccLinkRetainFunc, gSmemHandle, "smem_acc_link_retain");
    DL_LOAD_SYM(gSmemAccLinkRelease, smemAccLinkReleaseFunc, gSmemHandle, "smem_acc_link_release");
    DL_LOAD_SYM(gSmemAccLinkId, smemAccLinkIdFunc, gSmemHandle, "smem_acc_link_id");
    DL_LOAD_SYM(gSmemAccLinkSetContext, smemAccLinkSetContextFunc, gSmemHandle, "smem_acc_link_set_context");
    DL_LOAD_SYM(gSmemAccLinkGetContext, smemAccLinkGetContextFunc, gSmemHandle, "smem_acc_link_get_context");
    DL_LOAD_SYM(gSmemAccLinkSend, smemAccLinkSendFunc, gSmemHandle, "smem_acc_link_send_v2");
    DL_LOAD_SYM(gSmemAccHttpRequestBody, smemAccHttpRequestBodyFunc, gSmemHandle, "smem_acc_http_request_body");
    DL_LOAD_SYM(gSmemAccHttpRequestParam, smemAccHttpRequestParamFunc, gSmemHandle, "smem_acc_http_request_param");
    DL_LOAD_SYM(gSmemAccHttpRequestVisitParams, smemAccHttpRequestVisitParamsFunc, gSmemHandle,
                "smem_acc_http_request_visit_params");
    DL_LOAD_SYM(gSmemAccHttpRequestReply, smemAccHttpRequestReplyFunc, gSmemHandle, "smem_acc_http_request_reply");
    return MMC_OK;
}

Result MFAccApi::LoadLibrary()
{
    std::lock_guard<std::mutex> guard(gMutex);
    if (gLoaded) {
        return MMC_OK;
    }

    gSmemHandle = dlopen(gSmemLibName, RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND);
    if (gSmemHandle == nullptr) {
        MMC_LOG_ERROR("Failed to load MemFabric ACC library [" << gSmemLibName << "], error: " << dlerror());
        return MMC_NOT_INITIALIZED;
    }

    if (CheckMemfabricAbiVersion(gSmemHandle) != MMC_OK) {
        dlclose(gSmemHandle);
        gSmemHandle = nullptr;
        return MMC_NOT_INITIALIZED;
    }

    if (LoadAllSymbols() != MMC_OK) {
        // DL_LOAD_SYM closes the handle when a required symbol is missing.
        gSmemHandle = nullptr;
        ClearAllSymbols();
        return MMC_NOT_INITIALIZED;
    }

    gLoaded = true;
    return MMC_OK;
}

void MFAccApi::ClearAllSymbols()
{
    gSmemAccTcpServerCreate = nullptr;
    gSmemAccHttpServerCreate = nullptr;
    gSmemAccServerDestroy = nullptr;
    gSmemAccTcpServerStart = nullptr;
    gSmemAccHttpServerStart = nullptr;
    gSmemAccServerStop = nullptr;
    gSmemAccServerLoadTlsLibrary = nullptr;
    gSmemAccServerSetDecryptHandler = nullptr;
    gSmemAccServerSetNewLinkHandler = nullptr;
    gSmemAccServerSetRequestHandler = nullptr;
    gSmemAccServerSetSentHandler = nullptr;
    gSmemAccServerSetLinkBrokenHandler = nullptr;
    gSmemAccServerConnect = nullptr;
    gSmemAccHttpServerRegisterHandler = nullptr;
    gSmemAccLinkRetain = nullptr;
    gSmemAccLinkRelease = nullptr;
    gSmemAccLinkId = nullptr;
    gSmemAccLinkSetContext = nullptr;
    gSmemAccLinkGetContext = nullptr;
    gSmemAccLinkSend = nullptr;
    gSmemAccHttpRequestBody = nullptr;
    gSmemAccHttpRequestParam = nullptr;
    gSmemAccHttpRequestVisitParams = nullptr;
    gSmemAccHttpRequestReply = nullptr;
}

void MFAccApi::CleanupLibrary()
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
}

} // namespace mmc
} // namespace ock
