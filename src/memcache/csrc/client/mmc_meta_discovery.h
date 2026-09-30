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
#ifndef MMC_META_DISCOVERY_H
#define MMC_META_DISCOVERY_H

#include <atomic>
#include <cstdint>
#include <string>

#include "mmc_def.h"

namespace ock {
namespace mmc {
/**
 * @brief 判断 config store URL 是否走 etcd HA（动态端口 + etcd 发布地址）。
 */
bool IsEtcdStoreUrl(const char *storeUrl);

/**
 * @brief 按 config store URL 发现当前 MetaService 的 tcp://ip:port。
 *
 * 供 client-only 进程使用（不创建 BM、不注册 rank）：走 libsmem 的
 * smem_bm_get_bindable_leader_address_by_store_url。未发现、旧库缺符号或非 etcd
 * URL 时返回空串，由调用方决定回退与重试。
 *
 * @param storeUrl  config store URL，如 etcd://ip:port#instanceId
 * @param worldSize world size（需与 MetaService 侧一致）
 * @return "tcp://ip:port"，失败返回空串
 */
std::string DiscoverMetaServiceUrlByStoreUrl(const std::string &storeUrl, const mmc_tls_config *tlsOption,
                                             uint32_t worldSize);

/**
 * @brief 带重试的发现：2s 间隔、最多 60s，支持 stopFlag 提前中断。
 *
 * 发现失败与连接无关，仅负责产出地址；调用方拿到地址后再做 Connect。
 *
 * @param storeUrl  config store URL
 * @param worldSize world size
 * @param rankForLog 仅用于日志定位
 * @param stopFlag  可为空；置位后立即返回空串
 * @return "tcp://ip:port"，超时/中断返回空串
 */
std::string DiscoverMetaServiceUrlWithRetry(const std::string &storeUrl, const mmc_tls_config *tlsOption,
                                            uint32_t worldSize, uint32_t rankForLog, const std::atomic<bool> *stopFlag);
} // namespace mmc
} // namespace ock
#endif // MMC_META_DISCOVERY_H
