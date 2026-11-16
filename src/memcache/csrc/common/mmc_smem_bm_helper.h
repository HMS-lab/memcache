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
#ifndef MF_HYBRID_MMC_SMEM_BM_HELPER_H
#define MF_HYBRID_MMC_SMEM_BM_HELPER_H

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>
#include "config/mmc_functions.h"
#include "mmc_def.h"
#include "mmc_types.h"
#include "smem_bm_def.h"

namespace ock {
namespace mmc {
class MmcSmemBmHelper {
public:
    static inline smem_bm_data_op_type TransSingleDataOpType(const std::string &dataOpType)
    {
        if (dataOpType == "device_sdma") {
            return SMEMB_DATA_OP_SDMA;
        }
        if (dataOpType == "device_rdma") {
            return SMEMB_DATA_OP_DEVICE_RDMA;
        }
        if (dataOpType == "device_urma") {
            return SMEMB_DATA_OP_DEVICE_URMA;
        }
        if (dataOpType == "host_tcp") {
            return SMEMB_DATA_OP_HOST_TCP;
        }
        if (dataOpType == "host_rdma") {
            return SMEMB_DATA_OP_HOST_RDMA;
        }
        if (dataOpType == "host_urma") {
            return SMEMB_DATA_OP_HOST_URMA;
        }
        if (dataOpType == "host_shm") {
            return SMEMB_DATA_OP_HOST_SHM;
        }
        return SMEMB_DATA_OP_BUTT;
    }

    static inline Result ParseSmemBmProtocol(const std::string &protocol, smem_bm_data_op_type &dataOpType,
                                             std::string &tagOpInfo)
    {
        std::vector<std::string> layers;
        SplitStr(protocol, ":", layers);
        dataOpType = static_cast<smem_bm_data_op_type>(0U);
        tagOpInfo.clear();
        std::string parsedTagOpInfo;
        bool hasEnabledLayer = false;
        bool hasLayer = false;
        for (const auto &layer : layers) {
            if (!layer.empty()) {
                const auto layerOpType = TransSingleDataOpType(layer);
                if (layerOpType == SMEMB_DATA_OP_BUTT) {
                    return MMC_INVALID_PARAM;
                }
                dataOpType = static_cast<smem_bm_data_op_type>(dataOpType | layerOpType);
                hasEnabledLayer = true;
            }
            if (hasLayer) {
                parsedTagOpInfo.push_back(':');
            }
            hasLayer = true;
            parsedTagOpInfo.append(layer);
        }

        if (!hasEnabledLayer) {
            return MMC_INVALID_PARAM;
        }
        if (layers.size() > 1U) {
            tagOpInfo = parsedTagOpInfo;
        }
        return MMC_OK;
    }

    static inline bool IsHostShmCombo(smem_bm_data_op_type opType)
    {
        constexpr uint32_t deviceInterMask = SMEMB_DATA_OP_DEVICE_RDMA | SMEMB_DATA_OP_DEVICE_URMA;
        return (opType & SMEMB_DATA_OP_HOST_SHM) != 0U && (opType & deviceInterMask) != 0U;
    }

    static inline bool IsProviderConfig(smem_bm_data_op_type opType, uint64_t localDRAMSize, uint64_t localHBMSize)
    {
        return IsHostShmCombo(opType) && localDRAMSize > 0U && localHBMSize == 0U;
    }

    static inline bool IsProxyConfig(smem_bm_data_op_type opType, uint64_t localDRAMSize, uint64_t localHBMSize)
    {
        return IsHostShmCombo(opType) && localDRAMSize == 0U && localHBMSize == 0U;
    }

    static inline Result ResolveSmemBmFlags(const std::string &dataOpType, uint64_t localDRAMSize,
                                            uint64_t localHBMSize, uint32_t &flags)
    {
        smem_bm_data_op_type opType = SMEMB_DATA_OP_BUTT;
        std::string tagOpInfo;
        if (ParseSmemBmProtocol(dataOpType, opType, tagOpInfo) != MMC_OK) {
            return MMC_INVALID_PARAM;
        }
        (void)tagOpInfo;

        constexpr uint32_t providerProxyFlagMask = SMEM_BM_FLAG_CREATE_FOR_PROXY | SMEM_BM_FLAG_MULTI_NPU_PROVIDER;
        if ((flags & providerProxyFlagMask) == providerProxyFlagMask) {
            return MMC_INVALID_PARAM;
        }

        uint32_t derivedFlags = 0;
        if (!IsHostShmCombo(opType)) {
            if ((flags & providerProxyFlagMask) != 0U) {
                return MMC_INVALID_PARAM;
            }
            return MMC_OK;
        }

        if (IsProviderConfig(opType, localDRAMSize, localHBMSize)) {
            derivedFlags = SMEM_BM_FLAG_MULTI_NPU_PROVIDER;
        } else if (IsProxyConfig(opType, localDRAMSize, localHBMSize)) {
            derivedFlags = SMEM_BM_FLAG_CREATE_FOR_PROXY;
        } else {
            return MMC_INVALID_PARAM;
        }

        if ((flags & providerProxyFlagMask) != 0U && (flags & providerProxyFlagMask) != derivedFlags) {
            return MMC_INVALID_PARAM;
        }
        flags |= derivedFlags;
        return MMC_OK;
    }

    static inline smem_tls_config TransSmemTlsConfig(const mmc_tls_config &config)
    {
        smem_tls_config smemConfig = {};
        smemConfig.tlsEnable = config.tlsEnable;
        std::copy_n(config.caPath, SMEM_TLS_PATH_SIZE, smemConfig.caPath);
        std::copy_n(config.crlPath, SMEM_TLS_PATH_SIZE, smemConfig.crlPath);
        std::copy_n(config.certPath, SMEM_TLS_PATH_SIZE, smemConfig.certPath);
        std::copy_n(config.keyPath, SMEM_TLS_PATH_SIZE, smemConfig.keyPath);
        std::copy_n(config.keyPassPath, SMEM_TLS_PATH_SIZE, smemConfig.keyPassPath);
        std::copy_n(config.packagePath, SMEM_TLS_PATH_SIZE, smemConfig.packagePath);
        std::copy_n(config.decrypterLibPath, SMEM_TLS_PATH_SIZE, smemConfig.decrypterLibPath);

        return smemConfig;
    }
};

} // namespace mmc
} // namespace ock
#endif // MF_HYBRID_MMC_SMEM_BM_HELPER_H
