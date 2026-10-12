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
#ifndef MEMCACHE_SMEM_ACC_API_H
#define MEMCACHE_SMEM_ACC_API_H

#include <atomic>
#include <mutex>

#include "mmc_common_includes.h"
#include "smem_acc_def.h"

namespace ock {
namespace mmc {

using smemAccTcpServerCreateFunc = smem_acc_server_t (*)();
using smemAccHttpServerCreateFunc = smem_acc_server_t (*)();
using smemAccServerDestroyFunc = void (*)(smem_acc_server_t);
using smemAccTcpServerStartFunc = int32_t (*)(smem_acc_server_t, const smem_acc_tcp_server_options_t *,
                                              const smem_tls_options_t *);
using smemAccHttpServerStartFunc = int32_t (*)(smem_acc_server_t, const smem_acc_http_server_options_t *,
                                               const smem_tls_options_t *);
using smemAccServerStopFunc = void (*)(smem_acc_server_t);
using smemAccServerLoadTlsLibraryFunc = int32_t (*)(smem_acc_server_t, const char *);
using smemAccServerSetDecryptHandlerFunc = void (*)(smem_acc_server_t, smem_decrypt_cb, void *);
using smemAccServerSetNewLinkHandlerFunc = void (*)(smem_acc_server_t, smem_acc_new_link_cb, void *);
using smemAccServerSetRequestHandlerFunc = void (*)(smem_acc_server_t, int16_t, smem_acc_request_cb_v2, void *);
using smemAccServerSetSentHandlerFunc = void (*)(smem_acc_server_t, int16_t, smem_acc_sent_cb_v2, void *);
using smemAccServerSetLinkBrokenHandlerFunc = void (*)(smem_acc_server_t, smem_acc_link_broken_cb, void *);
using smemAccServerConnectFunc = int32_t (*)(smem_acc_server_t, const char *, uint16_t,
                                             const smem_acc_connect_request_t *, uint32_t, smem_acc_link_t *);
using smemAccHttpServerRegisterHandlerFunc = int32_t (*)(smem_acc_server_t, uint8_t, const char *, smem_acc_http_cb,
                                                         void *);
using smemAccGetClusterStatusFunc = int32_t (*)(char *, uint32_t, uint32_t *);
using smemAccLinkRetainFunc = smem_acc_link_t (*)(smem_acc_link_t);
using smemAccLinkReleaseFunc = void (*)(smem_acc_link_t);
using smemAccLinkIdFunc = uint32_t (*)(smem_acc_link_t);
using smemAccLinkSetContextFunc = void (*)(smem_acc_link_t, uint64_t);
using smemAccLinkGetContextFunc = uint64_t (*)(smem_acc_link_t);
using smemAccLinkSendFunc = int32_t (*)(smem_acc_link_t, int16_t, int16_t, uint32_t, const void *, uint32_t,
                                        const void *, uint32_t);
using smemAccHttpRequestBodyFunc = const void *(*)(smem_acc_http_request_t, uint32_t *);
using smemAccHttpRequestParamFunc = const char *(*)(smem_acc_http_request_t, const char *);
using smemAccHttpRequestVisitParamsFunc = int32_t (*)(smem_acc_http_request_t, smem_acc_http_param_cb, void *);
using smemAccHttpRequestReplyFunc = int32_t (*)(smem_acc_http_request_t, uint16_t, const char *, const void *,
                                                uint32_t);

class MFAccApi {
public:
    static constexpr uint32_t kProbeClusterStatusLen = 4096;
    // 与下层 smem.h 公共错误码家族保持一致（人工同步，勿随意改值）
    static constexpr int32_t kClusterStatusBufferTooSmall = -2015; // SMEM_BUFFER_TOO_SMALL
    static constexpr int32_t kClusterStatusNoStore = -2007;        // SMEM_NOT_EXIST
    static Result LoadLibrary();
    static void CleanupLibrary();

    static smem_acc_server_t CreateTcpServer()
    {
        return gSmemAccTcpServerCreate();
    }

    static smem_acc_server_t CreateHttpServer()
    {
        return gSmemAccHttpServerCreate();
    }

    static void DestroyServer(smem_acc_server_t server)
    {
        gSmemAccServerDestroy(server);
    }

    static int32_t StartTcpServer(smem_acc_server_t server, const smem_acc_tcp_server_options_t *options,
                                  const smem_tls_options_t *tls)
    {
        return gSmemAccTcpServerStart(server, options, tls);
    }

    static int32_t StartHttpServer(smem_acc_server_t server, const smem_acc_http_server_options_t *options,
                                   const smem_tls_options_t *tls)
    {
        return gSmemAccHttpServerStart(server, options, tls);
    }

    static void StopServer(smem_acc_server_t server)
    {
        gSmemAccServerStop(server);
    }

    static int32_t GetClusterStatus(std::string &jsonBuffer)
    {
        // 探测缓冲高水位，只增不减
        static std::atomic<uint32_t> sProbeLen{kProbeClusterStatusLen};
        std::vector<char> buf(sProbeLen.load(std::memory_order_relaxed));
        for (int attempt = 0; attempt < 3; ++attempt) {
            uint32_t outLen = 0;
            auto ret = gSmemAccGetClusterStatus(buf.data(), static_cast<uint32_t>(buf.size()), &outLen);
            if (ret == kClusterStatusBufferTooSmall) {
                const uint32_t want = outLen + outLen / 2 + 1;
                buf.resize(want);
                sProbeLen.store(want, std::memory_order_relaxed);
                continue;
            }
            if (ret == MMC_OK || ret == kClusterStatusNoStore) {
                jsonBuffer.assign(buf.data(), outLen);
            }
            return ret;
        }
        return kClusterStatusBufferTooSmall;
    }

    static int32_t LoadDynamicLib(smem_acc_server_t server, const char *path)
    {
        return gSmemAccServerLoadTlsLibrary(server, path);
    }

    static void RegisterDecryptHandler(smem_acc_server_t server, smem_decrypt_cb callback, void *context)
    {
        gSmemAccServerSetDecryptHandler(server, callback, context);
    }

    static void RegisterNewLinkHandler(smem_acc_server_t server, smem_acc_new_link_cb callback, void *context)
    {
        gSmemAccServerSetNewLinkHandler(server, callback, context);
    }

    static void RegisterNewRequestHandler(smem_acc_server_t server, int16_t type, smem_acc_request_cb_v2 callback,
                                          void *context)
    {
        gSmemAccServerSetRequestHandler(server, type, callback, context);
    }

    static void RegisterRequestSentHandler(smem_acc_server_t server, int16_t type, smem_acc_sent_cb_v2 callback,
                                           void *context)
    {
        gSmemAccServerSetSentHandler(server, type, callback, context);
    }

    static void RegisterLinkBrokenHandler(smem_acc_server_t server, smem_acc_link_broken_cb callback, void *context)
    {
        gSmemAccServerSetLinkBrokenHandler(server, callback, context);
    }

    static int32_t ConnectToPeerServer(smem_acc_server_t server, const char *ip, uint16_t port,
                                       const smem_acc_connect_request_t *request, uint32_t retries,
                                       smem_acc_link_t *link)
    {
        return gSmemAccServerConnect(server, ip, port, request, retries, link);
    }

    static int32_t RegisterHttpHandler(smem_acc_server_t server, uint8_t method, const char *path,
                                       smem_acc_http_cb callback, void *context)
    {
        return gSmemAccHttpServerRegisterHandler(server, method, path, callback, context);
    }

    static smem_acc_link_t RetainLink(smem_acc_link_t link)
    {
        return gSmemAccLinkRetain(link);
    }

    static void ReleaseLink(smem_acc_link_t link)
    {
        gSmemAccLinkRelease(link);
    }

    static uint32_t Id(smem_acc_link_t link)
    {
        return gSmemAccLinkId(link);
    }

    static void UpCtx(smem_acc_link_t link, uint64_t context)
    {
        gSmemAccLinkSetContext(link, context);
    }

    static uint64_t UpCtx(smem_acc_link_t link)
    {
        return gSmemAccLinkGetContext(link);
    }

    static int32_t NonBlockSend(smem_acc_link_t link, int16_t type, int16_t result, uint32_t sequence, const void *data,
                                uint32_t length, const void *callbackData, uint32_t callbackDataLen)
    {
        return gSmemAccLinkSend(link, type, result, sequence, data, length, callbackData, callbackDataLen);
    }

    static const void *Body(smem_acc_http_request_t request, uint32_t *length)
    {
        return gSmemAccHttpRequestBody(request, length);
    }

    static const char *GetParam(smem_acc_http_request_t request, const char *name)
    {
        return gSmemAccHttpRequestParam(request, name);
    }

    static int32_t VisitParams(smem_acc_http_request_t request, smem_acc_http_param_cb callback, void *context)
    {
        return gSmemAccHttpRequestVisitParams(request, callback, context);
    }

    static int32_t Reply(smem_acc_http_request_t request, uint16_t status, const char *contentType, const void *body,
                         uint32_t length)
    {
        return gSmemAccHttpRequestReply(request, status, contentType, body, length);
    }

private:
    static Result LoadAllSymbols();
    static void ClearAllSymbols();

private:
    static bool gLoaded;
    static std::mutex gMutex;
    static void *gSmemHandle;
    static const char *gSmemLibName;

    static smemAccTcpServerCreateFunc gSmemAccTcpServerCreate;
    static smemAccHttpServerCreateFunc gSmemAccHttpServerCreate;
    static smemAccServerDestroyFunc gSmemAccServerDestroy;
    static smemAccTcpServerStartFunc gSmemAccTcpServerStart;
    static smemAccHttpServerStartFunc gSmemAccHttpServerStart;
    static smemAccServerStopFunc gSmemAccServerStop;
    static smemAccServerLoadTlsLibraryFunc gSmemAccServerLoadTlsLibrary;
    static smemAccServerSetDecryptHandlerFunc gSmemAccServerSetDecryptHandler;
    static smemAccServerSetNewLinkHandlerFunc gSmemAccServerSetNewLinkHandler;
    static smemAccServerSetRequestHandlerFunc gSmemAccServerSetRequestHandler;
    static smemAccServerSetSentHandlerFunc gSmemAccServerSetSentHandler;
    static smemAccServerSetLinkBrokenHandlerFunc gSmemAccServerSetLinkBrokenHandler;
    static smemAccServerConnectFunc gSmemAccServerConnect;
    static smemAccHttpServerRegisterHandlerFunc gSmemAccHttpServerRegisterHandler;
    static smemAccGetClusterStatusFunc gSmemAccGetClusterStatus;
    static smemAccLinkRetainFunc gSmemAccLinkRetain;
    static smemAccLinkReleaseFunc gSmemAccLinkRelease;
    static smemAccLinkIdFunc gSmemAccLinkId;
    static smemAccLinkSetContextFunc gSmemAccLinkSetContext;
    static smemAccLinkGetContextFunc gSmemAccLinkGetContext;
    static smemAccLinkSendFunc gSmemAccLinkSend;
    static smemAccHttpRequestBodyFunc gSmemAccHttpRequestBody;
    static smemAccHttpRequestParamFunc gSmemAccHttpRequestParam;
    static smemAccHttpRequestVisitParamsFunc gSmemAccHttpRequestVisitParams;
    static smemAccHttpRequestReplyFunc gSmemAccHttpRequestReply;
};

} // namespace mmc
} // namespace ock

#endif // MEMCACHE_SMEM_ACC_API_H
