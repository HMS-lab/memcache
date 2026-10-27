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
#ifndef MEM_FABRIC_MOBS_NET_COMMON_H
#define MEM_FABRIC_MOBS_NET_COMMON_H

#include "mmc_common_includes.h"
#include "mmc_def.h"
#include "mmc_ip_validator.h"

namespace ock {
namespace mmc {
const std::string PROTOCOL_TCP = "tcp://";

enum NetProtoVersion : int16_t {
    VERSION_1 = 1,
};

enum NetProtocol {
    NET_NONE = 1,
    NET_RPC_TCP,
    NET_RPC_RDMA,
    NET_RPC_URMA,
    NET_IPC_UDS,
    NET_IPC_SHM,
};
using ExternalLog = void (*)(int, const char *);
struct NetEngineOptions {
    std::string name;                                      /* name of engine */
    std::string ip;                                        /* ip */
    uint16_t port = 9980L;                                 /* listen port */
    uint16_t threadCount = 2;                              /* worker thread count */
    uint16_t netPoolBase = MMC_DEFAULT_META_NET_POOL_BASE; /* async request pool thread count */
    uint16_t rankId = UINT16_MAX;                          /* rank id */
    bool startListener = false;                            /* start listener or not */
    mmc_tls_config tlsOption{};                            /* TLS communication options */
    int32_t logLevel = 3;
    ExternalLog logFunc = nullptr;

    /* functions */
    std::string ToString() const;

    static Result ExtractIpPortFromUrl(const std::string &url, NetEngineOptions &option);
};

class NetContext;
class NetLink;
class NetEngine;
using NetContextPtr = MmcRef<NetContext>;
using NetLinkPtr = MmcRef<NetLink>;
using NetEnginePtr = MmcRef<NetEngine>;

/* inline function for NetEngineOption */
inline std::string NetEngineOptions::ToString() const
{
    std::ostringstream oss;
    oss << "NetEngineOptions [name " << name << ", ip: " << ip << ", port: " << port << ", threadCount: " << threadCount
        << ", netPoolBase: " << netPoolBase << ", rankId " << rankId << ", startListener: " << startListener
        << ", tlsEnables: " << tlsOption.tlsEnable << "]";
    return oss.str();
}

inline Result NetEngineOptions::ExtractIpPortFromUrl(const std::string &url, NetEngineOptions &option)
{
    UrlParser parser;
    MMC_ASSERT_LOG_AND_RETURN(parser.Initialize(url), "Failed to extract url", MMC_INVALID_PARAM);

    /* set ip and port */
    option.ip = parser.GetIp();
    option.port = parser.GetPort();
    return MMC_OK;
}
} // namespace mmc
} // namespace ock

#endif // MEM_FABRIC_MOBS_NET_COMMON_H
