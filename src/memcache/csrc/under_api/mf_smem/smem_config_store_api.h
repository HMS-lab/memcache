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
#ifndef MEMCACHE_SMEM_CONFIG_STORE_API_H
#define MEMCACHE_SMEM_CONFIG_STORE_API_H

#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

#include "mmc_common_includes.h"
#include "mmc_ref.h"
#include "smem_bm_def.h"
#include "smem_config_store_def.h"

namespace ock {
namespace mmc_smem {

using smemConfigStoreSetTlsFunc = int32_t (*)(const smem_tls_options_t *, const char *, const char *);
using smemConfigStoreCreateServerFunc = smem_config_store_t (*)(const char *);
using smemConfigStoreDestroyServerFunc = void (*)(smem_config_store_t, const char *);
using smemConfigStoreSetServerBrokenHandlerFunc = void (*)(smem_config_store_t, smem_config_store_broken_cb, void *);
using smemConfigStoreSetLeaderHandlerFunc = int32_t (*)(smem_config_store_t, smem_config_store_leader_change_cb,
                                                        void *);

enum ConfigStoreModel : uint16_t {
    CSM_CLIENT = 0,
    CSM_SERVER = 1,
    CSM_BOTH = 2,
};

enum StoreErrorCode : int16_t {
    SUCCESS = 0,
    ERROR = -1,
    INVALID_MESSAGE = -400,
    INVALID_KEY = -401,
    NOT_EXIST = -404,
    RESTORE = -405,
    TIMEOUT = -601,
    IO_ERROR = -602,
};

class StoreBackend : public mmc::MmcReferable {};
using StoreBackendPtr = mmc::MmcRef<StoreBackend>;
using ConfigStoreServerBrokenHandler = std::function<int32_t(uint32_t, StoreBackendPtr)>;

class StoreFactory;

class ConfigStore : public mmc::MmcReferable {
public:
    ~ConfigStore() override;

    void RegisterServerBrokenHandler(const ConfigStoreServerBrokenHandler &handler) noexcept;
    uint32_t GetRankIdByLinkId(uint32_t linkId) const noexcept;

protected:
    ConfigStore(smem_config_store_t handle, std::string url) : handle_(handle), url_(std::move(url)) {}

    static mmc::Result LoadLibrary();
    static void CleanupLibrary();
    static mmc::Result LoadAllSymbols();
    static void ClearAllSymbols();
    static int32_t OnServerBroken(void *context, uint32_t linkId, uint32_t rankId);

protected:
    static bool gLoaded;
    static std::mutex gMutex;
    static void *gSmemHandle;
    static const char *gSmemLibName;

    static smemConfigStoreSetTlsFunc gSmemConfigStoreSetTls;
    static smemConfigStoreCreateServerFunc gSmemConfigStoreCreateServer;
    static smemConfigStoreDestroyServerFunc gSmemConfigStoreDestroyServer;
    static smemConfigStoreSetServerBrokenHandlerFunc gSmemConfigStoreSetServerBrokenHandler;
    static smemConfigStoreSetLeaderHandlerFunc gSmemConfigStoreSetLeaderHandler;

    smem_config_store_t handle_{nullptr};
    std::string url_;
    ConfigStoreServerBrokenHandler serverBrokenHandler_;
    mutable std::mutex callbackMutex_;
    mutable std::mutex rankIdMutex_;
    std::unordered_map<uint32_t, uint32_t> rankIds_;

    friend class StoreFactory;
};
using StorePtr = mmc::MmcRef<ConfigStore>;

class HaConfigStore final : public ConfigStore {
public:
    struct LeaderAddresses {
        bool isLeader{false};
        std::string metaServiceAddr;
    };
    using LeaderChangeCallback = std::function<void(const LeaderAddresses &)>;

    ~HaConfigStore() override;
    void RegisterLeaderChangeCallback(LeaderChangeCallback callback) noexcept;

private:
    HaConfigStore(smem_config_store_t handle, std::string url) : ConfigStore(handle, std::move(url)) {}
    static void OnLeaderChange(void *context, const smem_config_store_leader_info_t *leader);

private:
    LeaderChangeCallback leaderChangeCallback_;
    mutable std::mutex leaderCallbackMutex_;

    friend class StoreFactory;
};

class StoreFactory {
public:
    static void SetTlsInfo(const smem_tls_config &tlsOption) noexcept;
    static StorePtr CreateStoreByUrl(const std::string &storeUrl, uint16_t model, uint32_t worldSize = UINT32_MAX,
                                     int32_t rankId = -1, int32_t connMaxRetry = -1, bool skipRecover = true) noexcept;
    static void DestroyStore(const std::string &storeUrl) noexcept;

private:
    static std::mutex storesMutex_;
    static std::unordered_map<std::string, StorePtr> stores_;
};

} // namespace mmc_smem
} // namespace ock

namespace ock {
// Preserve existing ock::smem call sites while routing them to MemCache's runtime-loaded wrappers,
// avoiding a compile-time dependency on MemFabric.
namespace smem = mmc_smem;
} // namespace ock

#endif // MEMCACHE_SMEM_CONFIG_STORE_API_H
