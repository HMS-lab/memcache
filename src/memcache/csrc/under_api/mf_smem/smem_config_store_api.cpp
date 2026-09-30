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
#include <new>

#include "mmc_functions.h"
#include "smem_version_api.h"
#include "smem_config_store_api.h"

namespace ock {
namespace mmc_smem {

using mmc::MMC_ERROR;

bool ConfigStore::gLoaded = false;
std::mutex ConfigStore::gMutex;
void *ConfigStore::gSmemHandle = nullptr;
const char *ConfigStore::gSmemLibName = "libmf_smem.so";

smemConfigStoreSetTlsFunc ConfigStore::gSmemConfigStoreSetTls = nullptr;
smemConfigStoreCreateServerFunc ConfigStore::gSmemConfigStoreCreateServer = nullptr;
smemConfigStoreDestroyServerFunc ConfigStore::gSmemConfigStoreDestroyServer = nullptr;
smemConfigStoreSetServerBrokenHandlerFunc ConfigStore::gSmemConfigStoreSetServerBrokenHandler = nullptr;
smemConfigStoreSetLeaderHandlerFunc ConfigStore::gSmemConfigStoreSetLeaderHandler = nullptr;

std::mutex StoreFactory::storesMutex_;
std::unordered_map<std::string, StorePtr> StoreFactory::stores_;

mmc::Result ConfigStore::LoadAllSymbols()
{
    DL_LOAD_SYM(gSmemConfigStoreSetTls, smemConfigStoreSetTlsFunc, gSmemHandle, "smem_config_store_set_tls");
    DL_LOAD_SYM(gSmemConfigStoreCreateServer, smemConfigStoreCreateServerFunc, gSmemHandle,
                "smem_config_store_create_server");
    DL_LOAD_SYM(gSmemConfigStoreDestroyServer, smemConfigStoreDestroyServerFunc, gSmemHandle,
                "smem_config_store_destroy_server");
    DL_LOAD_SYM(gSmemConfigStoreSetServerBrokenHandler, smemConfigStoreSetServerBrokenHandlerFunc, gSmemHandle,
                "smem_config_store_set_server_broken_handler");
    DL_LOAD_SYM(gSmemConfigStoreSetLeaderHandler, smemConfigStoreSetLeaderHandlerFunc, gSmemHandle,
                "smem_config_store_set_leader_handler");
    return mmc::MMC_OK;
}

mmc::Result ConfigStore::LoadLibrary()
{
    std::lock_guard<std::mutex> guard(gMutex);
    if (gLoaded) {
        return mmc::MMC_OK;
    }

    gSmemHandle = dlopen(gSmemLibName, RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND);
    if (gSmemHandle == nullptr) {
        MMC_LOG_ERROR("Failed to load MemFabric ConfigStore library [" << gSmemLibName << "], error: " << dlerror());
        return mmc::MMC_NOT_INITIALIZED;
    }

    if (mmc::CheckMemfabricAbiVersion(gSmemHandle) != mmc::MMC_OK) {
        dlclose(gSmemHandle);
        gSmemHandle = nullptr;
        return mmc::MMC_NOT_INITIALIZED;
    }

    if (LoadAllSymbols() != mmc::MMC_OK) {
        // DL_LOAD_SYM closes the handle when a required symbol is missing.
        gSmemHandle = nullptr;
        ClearAllSymbols();
        return mmc::MMC_NOT_INITIALIZED;
    }

    gLoaded = true;
    return mmc::MMC_OK;
}

ConfigStore::~ConfigStore()
{
    if (handle_ != nullptr) {
        gSmemConfigStoreSetServerBrokenHandler(handle_, nullptr, nullptr);
        gSmemConfigStoreDestroyServer(handle_, url_.c_str());
    }
}

void ConfigStore::RegisterServerBrokenHandler(const ConfigStoreServerBrokenHandler &handler) noexcept
{
    {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        serverBrokenHandler_ = handler;
    }
    gSmemConfigStoreSetServerBrokenHandler(handle_, handler ? OnServerBroken : nullptr, handler ? this : nullptr);
}

uint32_t ConfigStore::GetRankIdByLinkId(uint32_t linkId) const noexcept
{
    std::lock_guard<std::mutex> lock(rankIdMutex_);
    auto iter = rankIds_.find(linkId);
    return iter == rankIds_.end() ? UINT32_MAX : iter->second;
}

int32_t ConfigStore::OnServerBroken(void *context, uint32_t linkId, uint32_t rankId)
{
    auto *store = static_cast<ConfigStore *>(context);
    if (store == nullptr) {
        return mmc::MMC_INVALID_PARAM;
    }
    {
        std::lock_guard<std::mutex> lock(store->rankIdMutex_);
        store->rankIds_[linkId] = rankId;
    }
    ConfigStoreServerBrokenHandler handler;
    {
        std::lock_guard<std::mutex> lock(store->callbackMutex_);
        handler = store->serverBrokenHandler_;
    }
    return handler ? handler(linkId, nullptr) : mmc::MMC_OK;
}

HaConfigStore::~HaConfigStore()
{
    if (handle_ != nullptr) {
        gSmemConfigStoreSetLeaderHandler(handle_, nullptr, nullptr);
    }
}

void HaConfigStore::RegisterLeaderChangeCallback(LeaderChangeCallback callback) noexcept
{
    const bool registered = static_cast<bool>(callback);
    {
        std::lock_guard<std::mutex> lock(leaderCallbackMutex_);
        leaderChangeCallback_ = std::move(callback);
    }
    gSmemConfigStoreSetLeaderHandler(handle_, registered ? OnLeaderChange : nullptr, registered ? this : nullptr);
}

void HaConfigStore::OnLeaderChange(void *context, const smem_config_store_leader_info_t *leader)
{
    auto *store = static_cast<HaConfigStore *>(context);
    if (store == nullptr || leader == nullptr) {
        return;
    }
    LeaderAddresses addresses;
    addresses.isLeader = leader->is_leader != 0;
    if (leader->bindable_leader_address != nullptr) {
        addresses.metaServiceAddr = leader->bindable_leader_address;
    }
    LeaderChangeCallback callback;
    {
        std::lock_guard<std::mutex> lock(store->leaderCallbackMutex_);
        callback = store->leaderChangeCallback_;
    }
    if (callback) {
        callback(addresses);
    }
}

void StoreFactory::SetTlsInfo(const smem_tls_config &tlsOption) noexcept
{
    if (ConfigStore::LoadLibrary() != mmc::MMC_OK) {
        return;
    }
    const smem_tls_options_t options{static_cast<uint8_t>(tlsOption.tlsEnable),
                                     "/",
                                     tlsOption.certPath,
                                     "/",
                                     "/",
                                     tlsOption.caPath,
                                     tlsOption.crlPath,
                                     tlsOption.keyPath,
                                     tlsOption.keyPassPath};
    if (ConfigStore::gSmemConfigStoreSetTls(&options, tlsOption.packagePath, tlsOption.decrypterLibPath) !=
        mmc::MMC_OK) {
        MMC_LOG_ERROR("Failed to configure config store TLS");
    }
}

StorePtr StoreFactory::CreateStoreByUrl(const std::string &storeUrl, uint16_t model, uint32_t worldSize, int32_t rankId,
                                        int32_t connMaxRetry, bool skipRecover) noexcept
{
    (void)worldSize;
    (void)rankId;
    (void)connMaxRetry;
    (void)skipRecover;
    if (model != ConfigStoreModel::CSM_SERVER || ConfigStore::LoadLibrary() != mmc::MMC_OK) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(storesMutex_);
    auto iter = stores_.find(storeUrl);
    if (iter != stores_.end()) {
        return iter->second;
    }

    auto handle = ConfigStore::gSmemConfigStoreCreateServer(storeUrl.c_str());
    if (handle == nullptr) {
        return nullptr;
    }

    const bool isHa = storeUrl.rfind("etcd://", 0) == 0 || storeUrl.rfind("reg://", 0) == 0;
    ConfigStore *native = isHa ? static_cast<ConfigStore *>(new (std::nothrow) HaConfigStore(handle, storeUrl))
                               : new (std::nothrow) ConfigStore(handle, storeUrl);
    if (native == nullptr) {
        ConfigStore::gSmemConfigStoreDestroyServer(handle, storeUrl.c_str());
        return nullptr;
    }
    StorePtr store = native;
    stores_.emplace(storeUrl, store);
    return store;
}

void StoreFactory::DestroyStore(const std::string &storeUrl) noexcept
{
    StorePtr store;
    {
        std::lock_guard<std::mutex> lock(storesMutex_);
        auto iter = stores_.find(storeUrl);
        if (iter == stores_.end()) {
            return;
        }
        store = std::move(iter->second);
        stores_.erase(iter);
    }
    store = nullptr;
}

void ConfigStore::ClearAllSymbols()
{
    gSmemConfigStoreSetTls = nullptr;
    gSmemConfigStoreCreateServer = nullptr;
    gSmemConfigStoreDestroyServer = nullptr;
    gSmemConfigStoreSetServerBrokenHandler = nullptr;
    gSmemConfigStoreSetLeaderHandler = nullptr;
}

void ConfigStore::CleanupLibrary()
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

} // namespace mmc_smem
} // namespace ock
