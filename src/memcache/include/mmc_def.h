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
#ifndef __MEMFABRIC_MMC_DEF_H__
#define __MEMFABRIC_MMC_DEF_H__

#include <stdint.h>

#define DISCOVERY_URL_SIZE               1024
#define PATH_MAX_SIZE                    1024
#define PROTOCOL_SIZE                    64
#define KV_EVENT_FIELD_SIZE              256
#define MAX_BATCH_OP_COUNT               16384
#define TLS_PATH_SIZE                    256
#define MMC_TAG_SIZE                     32
#define MMC_TAG_OP_INFO_SIZE             256
#define TLS_PATH_MAX_LEN                 (TLS_PATH_SIZE - 1)
#define MMC_DEFAULT_META_LRU_SHARD_COUNT 16U
#define MMC_MAX_META_LRU_SHARD_COUNT     1024U
#define MMC_META_LRU_TYPE_LRU            0U
#define MMC_META_LRU_TYPE_SHARDED_LRU    1U

/* Media type values, consistent with internal enum MediaType (see common/mmc_types.h) */
#define MMC_MEDIA_HBM  0
#define MMC_MEDIA_DRAM 1
#define MMC_MEDIA_SSD  2
#define MMC_MEDIA_NONE 3

#ifdef __cplusplus
extern "C" {
#endif

typedef void *mmc_meta_service_t;
typedef void *mmc_local_service_t;
typedef void *mmc_client_t;
#ifndef MMC_OUT_LOGGER
typedef void (*ExternalLog)(int level, const char *msg);
#endif

typedef struct {
    bool tlsEnable;
    char caPath[TLS_PATH_SIZE];
    char crlPath[TLS_PATH_SIZE];
    char certPath[TLS_PATH_SIZE];
    char keyPath[TLS_PATH_SIZE];
    char keyPassPath[TLS_PATH_SIZE];
    char packagePath[TLS_PATH_SIZE];
    char decrypterLibPath[TLS_PATH_SIZE];
} mmc_tls_config;

typedef struct {
    bool enable;
    char endpoint[DISCOVERY_URL_SIZE]; /* ZMQ PUB bind, e.g. tcp://0.0.0.0:5557 */
    char modelName[KV_EVENT_FIELD_SIZE];
    char tenantId[KV_EVENT_FIELD_SIZE];
    uint32_t blockSize;
    uint32_t queueCapacity;
    bool hashAsInt;        /* true: u64 hash (default); false: hex string */
    int32_t transportType; /* KV_EVENT_TRANSPORT enum value, see common/mmc_types.h */
} mmc_kv_events_config_t;

typedef struct {
    bool enableCurve;                    /* ZMQ CURVE encryption (server-only auth), default false */
    char publicKey[KV_EVENT_FIELD_SIZE]; /* Z85 encoded 40-char server public key */
    char secretKey[KV_EVENT_FIELD_SIZE]; /* Z85 encoded 40-char server secret key, keep secret */
} mmc_zmq_config_t;

typedef struct {
    char discoveryURL[DISCOVERY_URL_SIZE];   /* composed by schema and url, e.g. tcp:// or etcd:// or zk:// */
    char configStoreURL[DISCOVERY_URL_SIZE]; /* composed by schema and url, e.g. tcp:// or etcd:// or zk:// */
    char httpURL[DISCOVERY_URL_SIZE];
    bool haEnable;
    int32_t logLevel;
    char logPath[PATH_MAX_SIZE];
    int32_t logRotationFileSize;
    int32_t logRotationFileCount;
    int32_t logOutputTarget; /* 0=screen, 1=file, 2=both */
    uint16_t evictThresholdHigh;
    uint16_t evictThresholdLow;
    uint16_t defragEvictThreshold;
    uint64_t leaseTtlMs;
    uint32_t evictYieldInterval;   /* GC eviction yield: evict this many objects then sleep, 0=disabled */
    uint32_t evictYieldDurationUs; /* GC eviction yield: sleep duration in microseconds per yield */
    mmc_tls_config accTlsConfig;
    mmc_tls_config configStoreTlsConfig;
    mmc_tls_config metricsTlsConfig;
    uint32_t metricsReportIntervalSeconds;
    /* KV cache event publisher (opt-in, default disabled). */
    mmc_kv_events_config_t kvEvents;
    bool prefetchEnabled;
    uint32_t asyncFlushIntervalMs;
    uint32_t asyncFlushBatchLimit;
    /* ZMQ transport plugin config (independent of kv_events). */
    mmc_zmq_config_t zmq;
    uint64_t pendingWaitTimeoutMs;
    uint32_t evictCheckIntervalSeconds;
    /* Meta LRU implementation: MMC_META_LRU_TYPE_LRU or MMC_META_LRU_TYPE_SHARDED_LRU. */
    uint32_t lruType;
    /* Used only when lruType is MMC_META_LRU_TYPE_SHARDED_LRU. Must be a power of two in [1, 1024]. */
    uint32_t lruShardCount;
    // SSD 元数据模式属于 MetaService 启动配置；零值保持原有 aggregated 行为。
    uint32_t ssdMetadataMode; // 0: aggregated; 1: separated
} mmc_meta_service_config_t;

typedef struct {
    char discoveryURL[DISCOVERY_URL_SIZE];
    uint32_t deviceId;
    uint32_t rankId; // bmRankId: BM全局统一编号
    uint32_t worldSize;
    char bmIpPort[DISCOVERY_URL_SIZE];
    char bmHcomUrl[DISCOVERY_URL_SIZE];
    char backendId[DISCOVERY_URL_SIZE]; // Backend identity for kv_event (pod IP etc.)
    uint32_t createId;
    char dataOpType[PROTOCOL_SIZE];
    uint64_t localDRAMSize;
    uint64_t localMaxDRAMSize;
    uint64_t localHBMSize;
    uint64_t localMaxHBMSize;
    bool storageEnabled;
    bool enableMaxValueMode;
    uint32_t flags;
    mmc_tls_config accTlsConfig;
    int32_t logLevel;
    ExternalLog logFunc;
    mmc_tls_config hcomTlsConfig;
    mmc_tls_config configStoreTlsConfig;
    bool dynamicConfigEnable;
    uint32_t dynamicConfigInterval;
    char configFilePath[PATH_MAX_SIZE];
    char bmIpPortDomain[DISCOVERY_URL_SIZE]; // original (domain) form of config_store_url, for periodic re-resolution
    char tag[MMC_TAG_SIZE];
    char tagOpInfo[MMC_TAG_OP_INFO_SIZE];
} mmc_local_service_config_t;

typedef struct {
    char discoveryURL[DISCOVERY_URL_SIZE];
    uint32_t rankId;
    uint32_t rpcRetryTimeOut;
    uint32_t timeOut;
    uint32_t readThreadPoolNum;
    uint32_t writeThreadPoolNum;
    bool aggregateIO;
    int32_t aggregateNum;
    int32_t logLevel;
    ExternalLog logFunc;
    mmc_tls_config tlsConfig;
    char dataOpType[PROTOCOL_SIZE];
    uint64_t batchChunkSize;
    uint32_t batchChunkCount;
    /* 以下三项供 etcd HA 模式下的 client-only 进程发现 MetaService 地址。 */
    char configStoreURL[DISCOVERY_URL_SIZE]; // local_service.config_store_url
    mmc_tls_config configStoreTlsConfig;     // config store TLS，缺失会让 TLS 集群静默连不上
    uint32_t worldSize;                      // local_service.world_size
} mmc_client_config_t;

typedef struct {
    uint64_t addr;
    uint32_t type; // enum MediaType
    uint64_t offset;
    uint64_t len;
} mmc_buffer;

enum affinity_policy : int {
    NATIVE_AFFINITY = 0,
};

#define MAX_BLOB_COPIES 8

constexpr uint32_t GET_KEY_INFO_FOR_LAYER_WISE = 1;
constexpr uint32_t GET_DATA_FOR_LAYER_WISE = 1;

typedef struct {
    uint16_t mediaType;
    affinity_policy policy;
    uint16_t replicaNum; // Less than or equal to MAX_BLOB_COPIES=8
    int32_t preferredLocalServiceIDs[MAX_BLOB_COPIES];
} mmc_put_options;

typedef struct {
    uint64_t size;
    uint16_t prot;
    uint8_t numBlobs;
    bool valid;
    uint32_t ranks[MAX_BLOB_COPIES];
    uint16_t types[MAX_BLOB_COPIES];
    uint64_t gvas[MAX_BLOB_COPIES];
} mmc_data_info;

#ifdef __cplusplus
}
#endif

#endif //__MEMFABRIC_MMC_DEF_H__
