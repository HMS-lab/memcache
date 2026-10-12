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
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "smem_acc_def.h"
#include "smem_config_store_def.h"
#include "mock_acc_http_server.h"
#include <cstring>

namespace {
int g_mockHandle;
struct MockServer;

struct MockLink {
    uint32_t id{0};
    uint64_t context{0};
    std::weak_ptr<MockServer> server;
    std::weak_ptr<MockLink> peer;
};

template<typename Callback>
struct MockHandler {
    Callback callback{nullptr};
    void *context{nullptr};
};

struct MockServer {
    MockAccHttpServer http;
    bool started{false};
    uint16_t port{0};
    MockHandler<smem_acc_new_link_cb> newLink;
    MockHandler<smem_acc_link_broken_cb> broken;
    std::map<int16_t, MockHandler<smem_acc_request_cb_v2>> requests;
    std::map<int16_t, MockHandler<smem_acc_sent_cb_v2>> sent;
    std::vector<std::shared_ptr<MockLink>> links;
    std::vector<std::thread> workers;
};

using MockServerHandle = std::shared_ptr<MockServer>;
using MockLinkHandle = std::shared_ptr<MockLink>;
std::recursive_mutex gMockMutex;
std::map<uint16_t, std::weak_ptr<MockServer>> gMockListeners;
uint32_t gMockNextLinkId{0};

smem_acc_server_t CreateServer()
{
    return new MockServerHandle(std::make_shared<MockServer>());
}

void StopServer(smem_acc_server_t handle)
{
    auto server = *static_cast<MockServerHandle *>(handle);
    server->http.Stop();
    std::vector<std::thread> workers;
    {
        std::lock_guard<std::recursive_mutex> guard(gMockMutex);
        server->started = false;
        if (gMockListeners[server->port].lock() == server) {
            gMockListeners.erase(server->port);
        }
        for (const auto &link : server->links) {
            auto peer = link->peer.lock();
            auto peerServer = peer == nullptr ? nullptr : peer->server.lock();
            if (peerServer != nullptr && peerServer->started && peerServer->broken.callback != nullptr) {
                const auto broken = peerServer->broken;
                // Like ACC's worker, notify asynchronously so the caller can restart the server.
                peerServer->workers.emplace_back([peer, broken]() {
                    MockLinkHandle peerHandle = peer;
                    broken.callback(broken.context, &peerHandle);
                });
            }
        }
        server->links.clear();
        workers.swap(server->workers);
    }
    for (auto &worker : workers) {
        worker.join();
    }
}

void DestroyServer(smem_acc_server_t handle)
{
    StopServer(handle);
    delete static_cast<MockServerHandle *>(handle);
}

int32_t StartTcp(smem_acc_server_t handle, const smem_acc_tcp_server_options_t *options, const smem_tls_options_t *)
{
    std::lock_guard<std::recursive_mutex> guard(gMockMutex);
    auto server = *static_cast<MockServerHandle *>(handle);
    if (options->enable_listener) {
        if (!gMockListeners[options->listen_port].expired()) {
            return -1;
        }
        server->port = options->listen_port;
        gMockListeners[server->port] = server;
    }
    server->started = true;
    return 0;
}

int32_t StartHttp(smem_acc_server_t handle, const smem_acc_http_server_options_t *options, const smem_tls_options_t *)
{
    return (*static_cast<MockServerHandle *>(handle))->http.Start(*options);
}

int32_t LoadTls(smem_acc_server_t, const char *)
{
    return 0;
}

void SetDecrypt(smem_acc_server_t, smem_decrypt_cb, void *) {}

void SetNewLink(smem_acc_server_t handle, smem_acc_new_link_cb callback, void *context)
{
    std::lock_guard<std::recursive_mutex> guard(gMockMutex);
    (*static_cast<MockServerHandle *>(handle))->newLink = {callback, context};
}

void SetRequest(smem_acc_server_t, int16_t, smem_acc_request_cb, void *) {}

void SetSent(smem_acc_server_t, int16_t, smem_acc_sent_cb, void *) {}

void SetRequestV2(smem_acc_server_t handle, int16_t type, smem_acc_request_cb_v2 callback, void *context)
{
    std::lock_guard<std::recursive_mutex> guard(gMockMutex);
    (*static_cast<MockServerHandle *>(handle))->requests[type] = {callback, context};
}

void SetSentV2(smem_acc_server_t handle, int16_t type, smem_acc_sent_cb_v2 callback, void *context)
{
    std::lock_guard<std::recursive_mutex> guard(gMockMutex);
    (*static_cast<MockServerHandle *>(handle))->sent[type] = {callback, context};
}

void SetBroken(smem_acc_server_t handle, smem_acc_link_broken_cb callback, void *context)
{
    std::lock_guard<std::recursive_mutex> guard(gMockMutex);
    (*static_cast<MockServerHandle *>(handle))->broken = {callback, context};
}

int32_t Connect(smem_acc_server_t handle, const char *, uint16_t port, const smem_acc_connect_request_t *request,
                uint32_t, smem_acc_link_t *link)
{
    std::lock_guard<std::recursive_mutex> guard(gMockMutex);
    *link = nullptr;
    auto client = *static_cast<MockServerHandle *>(handle);
    auto server = gMockListeners[port].lock();
    if (!client->started || server == nullptr || !server->started) {
        return -1;
    }
    auto local = std::make_shared<MockLink>();
    auto remote = std::make_shared<MockLink>();
    local->id = ++gMockNextLinkId;
    remote->id = ++gMockNextLinkId;
    local->server = client;
    remote->server = server;
    local->peer = remote;
    remote->peer = local;
    MockLinkHandle remoteHandle = remote;
    if (server->newLink.callback != nullptr &&
        server->newLink.callback(server->newLink.context, request, &remoteHandle) != 0) {
        return -1;
    }
    client->links.push_back(local);
    server->links.push_back(remote);
    *link = new MockLinkHandle(local);
    return 0;
}

int32_t RegisterHttp(smem_acc_server_t handle, uint8_t method, const char *path, smem_acc_http_cb callback,
                     void *context)
{
    return (*static_cast<MockServerHandle *>(handle))->http.Register(method, path, callback, context);
}

smem_acc_link_t RetainLink(smem_acc_link_t link)
{
    return new MockLinkHandle(*static_cast<MockLinkHandle *>(link));
}

void ReleaseLink(smem_acc_link_t link)
{
    delete static_cast<MockLinkHandle *>(link);
}

uint32_t LinkId(smem_acc_link_t link)
{
    return (*static_cast<MockLinkHandle *>(link))->id;
}

void SetLinkContext(smem_acc_link_t link, uint64_t context)
{
    std::lock_guard<std::recursive_mutex> guard(gMockMutex);
    (*static_cast<MockLinkHandle *>(link))->context = context;
}

uint64_t GetLinkContext(smem_acc_link_t link)
{
    std::lock_guard<std::recursive_mutex> guard(gMockMutex);
    return (*static_cast<MockLinkHandle *>(link))->context;
}

int32_t Send(smem_acc_link_t handle, int16_t type, int16_t result, uint32_t sequence, const void *data, uint32_t length,
             const void *callbackData = nullptr, uint32_t callbackLength = 0)
{
    std::lock_guard<std::recursive_mutex> guard(gMockMutex);
    auto link = *static_cast<MockLinkHandle *>(handle);
    auto peer = link->peer.lock();
    auto sender = link->server.lock();
    auto receiver = peer == nullptr ? nullptr : peer->server.lock();
    if (sender == nullptr || receiver == nullptr || !sender->started || !receiver->started) {
        return -1;
    }
    const auto request = receiver->requests[type];
    if (request.callback == nullptr) {
        return -1;
    }
    const smem_acc_message_v2_t message{type, result, length, sequence, 0};
    MockLinkHandle peerHandle = peer;
    const auto status = request.callback(request.context, &message, data, length, &peerHandle);
    const auto sent = sender->sent[type];
    if (sent.callback != nullptr) {
        sent.callback(sent.context, status, &message, callbackData, callbackLength);
    }
    return 0;
}

const void *RequestBody(smem_acc_http_request_t handle, uint32_t *size)
{
    auto *request = static_cast<MockHttpRequest *>(handle);
    *size = request->body.size();
    return request->body.data();
}

const char *RequestParam(smem_acc_http_request_t handle, const char *name)
{
    auto *request = static_cast<MockHttpRequest *>(handle);
    const auto found = request->params.find(name);
    return found == request->params.end() ? nullptr : found->second.c_str();
}

int32_t Reply(smem_acc_http_request_t handle, uint16_t status, const char *contentType, const void *data,
              uint32_t length)
{
    return static_cast<MockHttpRequest *>(handle)->Reply(status, contentType, data, length);
}

int32_t SetStoreTls(const smem_tls_options_t *, const char *, const char *)
{
    return 0;
}

smem_config_store_t CreateStore(const char *)
{
    return &g_mockHandle;
}

void DestroyStore(smem_config_store_t, const char *) {}

void SetStoreBroken(smem_config_store_t, smem_config_store_broken_cb, void *) {}

int32_t SetLeader(smem_config_store_t, smem_config_store_leader_change_cb, void *)
{
    return 0;
}

} // namespace

extern "C" smem_acc_server_t smem_acc_tcp_server_create(void)
{
    return CreateServer();
}

extern "C" smem_acc_server_t smem_acc_http_server_create(void)
{
    return CreateServer();
}

extern "C" void smem_acc_server_destroy(smem_acc_server_t server)
{
    DestroyServer(server);
}

extern "C" int32_t smem_acc_tcp_server_start(smem_acc_server_t server, const smem_acc_tcp_server_options_t *options,
                                             const smem_tls_options_t *tls)
{
    return StartTcp(server, options, tls);
}

extern "C" int32_t smem_acc_http_server_start(smem_acc_server_t server, const smem_acc_http_server_options_t *options,
                                              const smem_tls_options_t *tls)
{
    return StartHttp(server, options, tls);
}

extern "C" void smem_acc_server_stop(smem_acc_server_t server)
{
    StopServer(server);
}

extern "C" int32_t smem_acc_server_load_tls_library(smem_acc_server_t server, const char *path)
{
    return LoadTls(server, path);
}

extern "C" void smem_acc_server_set_decrypt_handler(smem_acc_server_t server, smem_decrypt_cb callback, void *context)
{
    SetDecrypt(server, callback, context);
}

extern "C" void smem_acc_server_set_new_link_handler(smem_acc_server_t server, smem_acc_new_link_cb callback,
                                                     void *context)
{
    SetNewLink(server, callback, context);
}

extern "C" void smem_acc_server_set_request_handler(smem_acc_server_t server, int16_t type,
                                                    smem_acc_request_cb callback, void *context)
{
    SetRequest(server, type, callback, context);
}

extern "C" void smem_acc_server_set_sent_handler(smem_acc_server_t server, int16_t type, smem_acc_sent_cb callback,
                                                 void *context)
{
    SetSent(server, type, callback, context);
}

extern "C" void smem_acc_server_set_request_handler_v2(smem_acc_server_t server, int16_t type,
                                                       smem_acc_request_cb_v2 callback, void *context)
{
    SetRequestV2(server, type, callback, context);
}

extern "C" void smem_acc_server_set_sent_handler_v2(smem_acc_server_t server, int16_t type,
                                                    smem_acc_sent_cb_v2 callback, void *context)
{
    SetSentV2(server, type, callback, context);
}

extern "C" void smem_acc_server_set_link_broken_handler(smem_acc_server_t server, smem_acc_link_broken_cb callback,
                                                        void *context)
{
    SetBroken(server, callback, context);
}

extern "C" int32_t smem_acc_server_connect(smem_acc_server_t server, const char *ip, uint16_t port,
                                           const smem_acc_connect_request_t *request, uint32_t retries,
                                           smem_acc_link_t *link)
{
    return Connect(server, ip, port, request, retries, link);
}

extern "C" int32_t smem_acc_http_server_register_handler(smem_acc_server_t server, uint8_t method, const char *path,
                                                         smem_acc_http_cb callback, void *context)
{
    return RegisterHttp(server, method, path, callback, context);
}

extern "C" smem_acc_link_t smem_acc_link_retain(smem_acc_link_t link)
{
    return RetainLink(link);
}

extern "C" void smem_acc_link_release(smem_acc_link_t link)
{
    ReleaseLink(link);
}

extern "C" uint32_t smem_acc_link_id(smem_acc_link_t link)
{
    return LinkId(link);
}

extern "C" void smem_acc_link_set_context(smem_acc_link_t link, uint64_t context)
{
    SetLinkContext(link, context);
}

extern "C" uint64_t smem_acc_link_get_context(smem_acc_link_t link)
{
    return GetLinkContext(link);
}

extern "C" int32_t smem_acc_link_send(smem_acc_link_t link, int16_t type, int16_t result, uint32_t sequence,
                                      const void *data, uint32_t length)
{
    return Send(link, type, result, sequence, data, length);
}

extern "C" int32_t smem_acc_link_send_v2(smem_acc_link_t link, int16_t type, int16_t result, uint32_t sequence,
                                         const void *data, uint32_t length, const void *callbackData,
                                         uint32_t callbackLength)
{
    return Send(link, type, result, sequence, data, length, callbackData, callbackLength);
}

extern "C" const void *smem_acc_http_request_body(smem_acc_http_request_t request, uint32_t *length)
{
    return RequestBody(request, length);
}

extern "C" const char *smem_acc_http_request_param(smem_acc_http_request_t request, const char *name)
{
    return RequestParam(request, name);
}

extern "C" int32_t smem_acc_http_request_visit_params(smem_acc_http_request_t handle, smem_acc_http_param_cb callback,
                                                      void *context)
{
    for (const auto &param : static_cast<MockHttpRequest *>(handle)->params) {
        callback(context, param.first.c_str(), param.second.c_str());
    }
    return 0;
}

extern "C" int32_t smem_acc_http_request_reply(smem_acc_http_request_t request, uint16_t status,
                                               const char *contentType, const void *body, uint32_t length)
{
    return Reply(request, status, contentType, body, length);
}

extern "C" int32_t smem_config_store_set_tls(const smem_tls_options_t *tls, const char *tlsLibrary,
                                             const char *decrypterLibrary)
{
    return SetStoreTls(tls, tlsLibrary, decrypterLibrary);
}

extern "C" smem_config_store_t smem_config_store_create_server(const char *url)
{
    return CreateStore(url);
}

extern "C" void smem_config_store_destroy_server(smem_config_store_t store, const char *url)
{
    DestroyStore(store, url);
}

extern "C" void smem_config_store_set_server_broken_handler(smem_config_store_t store,
                                                            smem_config_store_broken_cb callback, void *context)
{
    SetStoreBroken(store, callback, context);
}

extern "C" int32_t smem_config_store_set_leader_handler(smem_config_store_t store,
                                                        smem_config_store_leader_change_cb callback, void *context)
{
    return SetLeader(store, callback, context);
}

extern "C" {
uint32_t smem_get_abi_version()
{
    return (static_cast<uint32_t>(VERSION_MAJOR) << 16U) | static_cast<uint32_t>(VERSION_MINOR);
}

int32_t smem_init(uint32_t flags)
{
    return 0;
}

int32_t smem_set_extern_logger(void (*func)(int level, const char *msg))
{
    return 0;
}

int32_t smem_set_log_level(int level)
{
    return 0;
}

void smem_uninit()
{
    return;
}

const char *smem_get_last_err_msg()
{
    return "";
}

const char *smem_get_and_clear_last_err_msg()
{
    return "";
}

extern "C" int32_t smem_config_store_get_cluster_status(char *json, uint32_t bufLen, uint32_t *outLen)
{
    constexpr char kMockClusterStatus[] = "[{\"type\":\"mock\"}]";
    constexpr uint32_t kMockClusterStatusLen = static_cast<uint32_t>(sizeof(kMockClusterStatus));
    constexpr int32_t kMockRetBufferTooSmall = -2015; // SMEM_BUFFER_TOO_SMALL
    if (outLen != nullptr) {
        *outLen = kMockClusterStatusLen - 1;
    }
    if (json == nullptr) {
        return 0;
    }
    if (bufLen < kMockClusterStatusLen) {
        *outLen = kMockClusterStatusLen;
        return kMockRetBufferTooSmall;
    }
    memcpy(json, kMockClusterStatus, kMockClusterStatusLen);
    return 0;
}
}
