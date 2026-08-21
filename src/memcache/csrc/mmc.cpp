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
#include "mmc.h"

#include <functional>

#include "mmc_client.h"
#include "mmc_common_includes.h"
#include "mmc_config_reload_utils.h"
#include "mmc_configuration.h"
#include "mmc_def.h"
#include "mmc_define.h"
#include "mmc_env.h"
#include "mmc_logger.h"
#include "mmc_local_config_reload.h"
#include "mmc_service.h"
#include "mmc_thread_pool.h"
#include "mmc_types.h"
#include "mmc_meta_manager.h"
#include "mmc_periodic_task.h"

using namespace ock::mmc;
static mmc_local_service_t g_localService;

static std::mutex gMmcMutex;
static bool g_mmcSetup = false;
static bool mmcInit = false;
static ClientConfig g_clientConfig{};
static mmc_local_service_config_t g_localServiceConfig{};

// Process-wide config file polling: reloads hot-reloadable settings (log_level and client
// tunables) from the local config file and applies them without restart. A single periodic
// task parses the file once per poll and applies all setting groups.
static const char *const K_CONFIG_POLL_TASK_NAME = "ProcessConfigPolling";
static time_t g_lastConfigMtime = 0;

static void StartConfigPolling(const char *taskName, time_t &lastConfigMtime,
                               const std::function<bool(const std::string &, time_t &)> &reloadFunc)
{
    if (!g_localServiceConfig.dynamicConfigEnable || g_localServiceConfig.configFilePath[0] == '\0') {
        return;
    }
    IsConfigFileModified(g_localServiceConfig.configFilePath, lastConfigMtime);
    auto ret =
        RegisterPeriodicTask(taskName, g_localServiceConfig.dynamicConfigInterval, [reloadFunc, &lastConfigMtime]() {
            reloadFunc(g_localServiceConfig.configFilePath, lastConfigMtime);
        });
    if (ret != MMC_OK) {
        MMC_LOG_ERROR("Failed to register config polling task: " << taskName);
        return;
    }
    MMC_LOG_TRACE("Config polling registered, task: " << taskName
                                                      << ", interval: " << g_localServiceConfig.dynamicConfigInterval
                                                      << "s, file: " << g_localServiceConfig.configFilePath);
}

mmc_meta_service_config_t create_default_meta_config()
{
    mmc_meta_service_config_t config{};
    SafeCopy("tcp://127.0.0.1:5000", config.discoveryURL, sizeof(config.discoveryURL));
    SafeCopy("tcp://127.0.0.1:6000", config.configStoreURL, sizeof(config.configStoreURL));
    SafeCopy("http://127.0.0.1:8000", config.httpURL, sizeof(config.httpURL));
    config.haEnable = false;
    config.logLevel = INFO_LEVEL;
    SafeCopy("/var/log/memcache_hybrid", config.logPath, sizeof(config.logPath));
    config.logRotationFileSize = 20 * MB_NUM;
    config.logRotationFileCount = 50;
    config.logOutputTarget = LOG_OUTPUT_TARGET_FILE;
    config.evictThresholdHigh = 90U;
    config.evictThresholdLow = 80U;
    config.rewarmDramWatermark = DEFAULT_REWARM_HIGH_WATERMARK;
    config.prefetchEnabled = false;
    config.pendingWaitTimeoutMs = 300U;
    config.leaseTtlMs = MMC_DATA_TTL_MS;
    config.evictYieldInterval = 0U; /* disabled by default, set e.g. 100 to enable */
    config.evictYieldDurationUs = 100U;
    config.accTlsConfig.tlsEnable = false;
    config.configStoreTlsConfig.tlsEnable = false;
    config.metricsReportIntervalSeconds = 0U;
    config.evictCheckIntervalSeconds = 5U;
    config.kvEvents.enable = false;
    config.kvEvents.transportType = static_cast<int32_t>(KV_EVENT_TRANSPORT::ZMQ);
    SafeCopy("", config.kvEvents.endpoint, sizeof(config.kvEvents.endpoint));
    SafeCopy("", config.kvEvents.modelName, sizeof(config.kvEvents.modelName));
    SafeCopy("default", config.kvEvents.tenantId, sizeof(config.kvEvents.tenantId));
    config.kvEvents.blockSize = 0U;
    config.kvEvents.queueCapacity = 65536U;
    config.kvEvents.hashAsInt = true;
    config.zmq.enableCurve = false;
    SafeCopy("", config.zmq.publicKey, sizeof(config.zmq.publicKey));
    SafeCopy("", config.zmq.secretKey, sizeof(config.zmq.secretKey));
    return config;
}

std::string meta_config_to_string(const mmc_meta_service_config_t &config)
{
    std::ostringstream oss;
    oss << "MetaConfig {\n";
    oss << "  meta_service_url: " << config.discoveryURL << "\n";
    oss << "  config_store_url: " << config.configStoreURL << "\n";
    oss << "  metrics_url: " << config.httpURL << "\n";
    oss << "  ha_enable: " << (config.haEnable ? "true" : "false") << "\n";
    oss << "  log_level: " << config.logLevel << "\n";
    oss << "  log_path: " << config.logPath << "\n";
    oss << "  log_rotation_file_size: " << config.logRotationFileSize << "\n";
    oss << "  log_rotation_file_count: " << config.logRotationFileCount << "\n";
    oss << "  log_output_target: " << config.logOutputTarget << "\n";
    oss << "  evict_threshold_high: " << config.evictThresholdHigh << "\n";
    oss << "  evict_threshold_low: " << config.evictThresholdLow << "\n";
    oss << "  evict_check_interval_seconds: " << config.evictCheckIntervalSeconds << "\n";
    oss << "  kv_events_enable: " << (config.kvEvents.enable ? "true" : "false") << "\n";
    oss << "  kv_events_transport_type: " << config.kvEvents.transportType << "\n";
    oss << "  kv_events_endpoint: " << config.kvEvents.endpoint << "\n";
    oss << "  kv_events_hash_as_int: " << (config.kvEvents.hashAsInt ? "true" : "false") << "\n";
    oss << "  zmq_curve_enable: " << (config.zmq.enableCurve ? "true" : "false") << "\n";
    oss << "  zmq_public_key: " << config.zmq.publicKey << "\n";
    oss << "  lease_ttl_ms: " << config.leaseTtlMs << "\n";
    oss << "  evict_yield_interval: " << config.evictYieldInterval << "\n";
    oss << "  evict_yield_duration_us: " << config.evictYieldDurationUs << "\n";
    oss << "  tls_enable: " << (config.accTlsConfig.tlsEnable ? "true" : "false") << "\n";
    oss << "  tls_ca_path: " << config.accTlsConfig.caPath << "\n";
    oss << "  tls_ca_crl_path: " << config.accTlsConfig.crlPath << "\n";
    oss << "  tls_cert_path: " << config.accTlsConfig.certPath << "\n";
    oss << "  tls_key_path: " << config.accTlsConfig.keyPath << "\n";
    oss << "  tls_key_pass_path: " << config.accTlsConfig.keyPassPath << "\n";
    oss << "  tls_package_path: " << config.accTlsConfig.packagePath << "\n";
    oss << "  tls_decrypter_path: " << config.accTlsConfig.decrypterLibPath << "\n";
    oss << "  config_store_tls_enable: " << (config.configStoreTlsConfig.tlsEnable ? "true" : "false") << "\n";
    oss << "  config_store_tls_ca_path: " << config.configStoreTlsConfig.caPath << "\n";
    oss << "  config_store_tls_ca_crl_path: " << config.configStoreTlsConfig.crlPath << "\n";
    oss << "  config_store_tls_cert_path: " << config.configStoreTlsConfig.certPath << "\n";
    oss << "  config_store_tls_key_path: " << config.configStoreTlsConfig.keyPath << "\n";
    oss << "  config_store_tls_key_pass_path: " << config.configStoreTlsConfig.keyPassPath << "\n";
    oss << "  config_store_tls_package_path: " << config.configStoreTlsConfig.packagePath << "\n";
    oss << "  config_store_tls_decrypter_path: " << config.configStoreTlsConfig.decrypterLibPath << "\n";
    oss << "}";
    return oss.str();
}

local_config create_default_local_config()
{
    local_config cfg{};
    SafeCopy("tcp://127.0.0.1:5000", cfg.meta_service_url, sizeof(cfg.meta_service_url));
    SafeCopy("tcp://127.0.0.1:6000", cfg.config_store_url, sizeof(cfg.config_store_url));
    SafeCopy("", cfg.backend_id, sizeof(cfg.backend_id));
    SafeCopy("info", cfg.log_level, sizeof(cfg.log_level));
    cfg.world_size = 256UL;
    SafeCopy("host_rdma", cfg.protocol, sizeof(cfg.protocol));
    SafeCopy("tcp://127.0.0.1:7000", cfg.hcom_url, sizeof(cfg.hcom_url));
    SafeCopy("1GB", cfg.dram_size, sizeof(cfg.dram_size));
    SafeCopy("0", cfg.hbm_size, sizeof(cfg.hbm_size));
    SafeCopy("1TB", cfg.max_dram_size, sizeof(cfg.max_dram_size));
    SafeCopy("0", cfg.max_hbm_size, sizeof(cfg.max_hbm_size));
    cfg.client_retry_milliseconds = 0;
    cfg.client_timeout_seconds = 60UL;
    cfg.read_thread_pool_size = 32UL;
    cfg.write_thread_pool_size = 4UL;
    cfg.aggregate_io = true;
    cfg.aggregate_num = 122UL;
    cfg.tls_enable = false;
    cfg.config_store_tls_enable = false;
    cfg.hcom_tls_enable = false;
    cfg.dynamic_config_enable = true;
    cfg.dynamic_config_interval = DEFAULT_DYNAMIC_CONFIG_INTERVAL;
    return cfg;
}

std::string local_config_to_string(const local_config &config)
{
    std::ostringstream oss;
    oss << "LocalConfig {\n";
    oss << "  meta_service_url: " << config.meta_service_url << "\n";
    oss << "  config_store_url: " << config.config_store_url << "\n";
    oss << "  backend_id: " << config.backend_id << "\n";
    oss << "  log_level: " << config.log_level << "\n";
    oss << "  world_size: " << config.world_size << "\n";
    oss << "  protocol: " << config.protocol << "\n";
    oss << "  hcom_url: " << config.hcom_url << "\n";
    oss << "  dram_size: " << config.dram_size << "\n";
    oss << "  hbm_size: " << config.hbm_size << "\n";
    oss << "  max_dram_size: " << config.max_dram_size << "\n";
    oss << "  max_hbm_size: " << config.max_hbm_size << "\n";
    oss << "  client_retry_milliseconds: " << config.client_retry_milliseconds << "\n";
    oss << "  client_timeout_seconds: " << config.client_timeout_seconds << "\n";
    oss << "  read_thread_pool_size: " << config.read_thread_pool_size << "\n";
    oss << "  write_thread_pool_size: " << config.write_thread_pool_size << "\n";
    oss << "  aggregate_io: " << (config.aggregate_io ? "true" : "false") << "\n";
    oss << "  aggregate_num: " << config.aggregate_num << "\n";
    oss << "  tls_enable: " << (config.tls_enable ? "true" : "false") << "\n";
    oss << "  tls_ca_path: " << config.tls_ca_path << "\n";
    oss << "  tls_ca_crl_path: " << config.tls_ca_crl_path << "\n";
    oss << "  tls_cert_path: " << config.tls_cert_path << "\n";
    oss << "  tls_key_path: " << config.tls_key_path << "\n";
    oss << "  tls_key_pass_path: " << config.tls_key_pass_path << "\n";
    oss << "  tls_package_path: " << config.tls_package_path << "\n";
    oss << "  tls_decrypter_path: " << config.tls_decrypter_path << "\n";
    oss << "  config_store_tls_enable: " << (config.config_store_tls_enable ? "true" : "false") << "\n";
    oss << "  config_store_tls_ca_path: " << config.config_store_tls_ca_path << "\n";
    oss << "  config_store_tls_ca_crl_path: " << config.config_store_tls_ca_crl_path << "\n";
    oss << "  config_store_tls_cert_path: " << config.config_store_tls_cert_path << "\n";
    oss << "  config_store_tls_key_path: " << config.config_store_tls_key_path << "\n";
    oss << "  config_store_tls_key_pass_path: " << config.config_store_tls_key_pass_path << "\n";
    oss << "  config_store_tls_package_path: " << config.config_store_tls_package_path << "\n";
    oss << "  config_store_tls_decrypter_path: " << config.config_store_tls_decrypter_path << "\n";
    oss << "  hcom_tls_enable: " << (config.hcom_tls_enable ? "true" : "false") << "\n";
    oss << "  hcom_tls_ca_path: " << config.hcom_tls_ca_path << "\n";
    oss << "  hcom_tls_ca_crl_path: " << config.hcom_tls_ca_crl_path << "\n";
    oss << "  hcom_tls_cert_path: " << config.hcom_tls_cert_path << "\n";
    oss << "  hcom_tls_key_path: " << config.hcom_tls_key_path << "\n";
    oss << "  hcom_tls_key_pass_path: " << config.hcom_tls_key_pass_path << "\n";
    oss << "  hcom_tls_decrypter_path: " << config.hcom_tls_decrypter_path << "\n";
    oss << "  dynamic_config_enable: " << (config.dynamic_config_enable ? "true" : "false") << "\n";
    oss << "  dynamic_config_interval: " << config.dynamic_config_interval << "\n";
    oss << "}";
    return oss.str();
}

static int32_t LoadAndValidateConfig(const local_config *config)
{
    if (!g_clientConfig.Setup(config)) {
        MMC_LOG_ERROR("Failed to setup config");
        return MMC_ERROR;
    }

    const std::vector<std::string> validationError = g_clientConfig.ValidateConf();
    if (!validationError.empty()) {
        MMC_LOG_ERROR("Wrong configuration because of following mistakes:");
        for (auto &item : validationError) {
            MMC_LOG_ERROR(item);
        }
        return MMC_INVALID_PARAM;
    }

    g_localServiceConfig.flags = 0;
    g_clientConfig.GetLocalServiceConfig(g_localServiceConfig);
    if (config->config_path[0] != '\0') {
        SafeCopy(config->config_path, g_localServiceConfig.configFilePath, PATH_MAX_SIZE);
    }
    if (g_clientConfig.ValidateLocalServiceConfig(g_localServiceConfig) != MMC_OK) {
        MMC_LOG_ERROR("Invalid local service config");
        return MMC_INVALID_PARAM;
    }

    MMC_RETURN_ERROR(MmcOutLogger::Instance().SetLogLevel(static_cast<LogLevel>(g_localServiceConfig.logLevel)),
                     "failed to set log level " << g_localServiceConfig.logLevel);
    if (g_localServiceConfig.logFunc != nullptr) {
        MmcOutLogger::Instance().SetExternalLogFunction(g_localServiceConfig.logFunc);
    }

    // Record the fixed baseline (initialized config values + built-in defaults)
    // for non-hot-reloadable change reporting.
    SetNonHotReloadableBaseline(g_clientConfig.GetAllItems());
    return MMC_OK;
}

MMC_API int32_t mmc_setup(const local_config *config)
{
    MMC_VALIDATE_RETURN(config != nullptr, "local config is null", MMC_INVALID_PARAM);
    std::lock_guard<std::mutex> lock(gMmcMutex);
    if (g_mmcSetup) {
        MMC_LOG_TRACE("mmc is already setup");
        return MMC_OK;
    }

    auto ret = LoadAndValidateConfig(config);
    if (ret != MMC_OK) {
        return ret;
    }

    g_mmcSetup = true;
    MMC_LOG_TRACE("mmc setup success");
    return MMC_OK;
}

MMC_API int32_t mmc_init(const mmc_init_config *config)
{
    static constexpr int32_t PROCESS_NICE = -5;
    MmcThreadPool::TrySetProcessNice(PROCESS_NICE);
    MMC_VALIDATE_RETURN(config != nullptr, "config is null", MMC_INVALID_PARAM);
    MMC_VALIDATE_RETURN(config->deviceId <= MAX_DEVICE_ID, "Invalid param deviceId: " << config->deviceId,
                        MMC_INVALID_PARAM);
    std::lock_guard<std::mutex> lock(gMmcMutex);
    if (mmcInit) {
        MMC_LOG_TRACE("mmc is already init");
        return MMC_OK;
    }

    // 1. load g_clientConfig and g_localServiceConfig config
    if (!g_mmcSetup) {
        std::string configPath = MMC_LOCAL_CONF_PATH;
        MMC_VALIDATE_RETURN(!configPath.empty(), "MMC_LOCAL_CONFIG_PATH is not set", MMC_INVALID_PARAM);

        local_config localConfig{};
        SafeCopy(configPath, localConfig.config_path, PATH_MAX_SIZE);
        auto ret = LoadAndValidateConfig(&localConfig);
        if (ret != MMC_OK) {
            return ret;
        }
    }

    g_localServiceConfig.deviceId = config->deviceId;

    // 2. if provide memory to memory pool, start local service
    if (config->initBm) {
        g_localService = mmcs_local_service_start(&g_localServiceConfig);
        MMC_VALIDATE_RETURN(g_localService != nullptr, "failed to create or start local service", MMC_ERROR);
    }

    // 3. init client sdk
    mmc_client_config_t clientConfig{};
    g_clientConfig.GetClientConfig(clientConfig);
    auto ret = mmcc_init(&clientConfig);
    if (ret != MMC_OK) {
        MMC_LOG_ERROR("mmcc init failed, ret:" << ret);
        mmcs_local_service_stop(g_localService);
        g_localService = nullptr;
        return ret;
    }
    mmcInit = true;
    StartConfigPolling(K_CONFIG_POLL_TASK_NAME, g_lastConfigMtime, ReloadLocalConfigFromFile);
    return ret;
}

MMC_API int32_t mmc_set_extern_logger(void (*func)(int level, const char *msg))
{
    if (func == nullptr) {
        return MMC_INVALID_PARAM;
    }
    ock::mmc::MmcOutLogger::Instance().SetExternalLogFunction(func);
    return MMC_OK;
}

MMC_API int32_t mmc_set_log_level(int level)
{
    if (level < DEBUG_LEVEL || level >= BUTT_LEVEL) {
        return MMC_INVALID_PARAM;
    }
    ock::mmc::MmcOutLogger::Instance().SetLogLevel(static_cast<LogLevel>(level));
    return MMC_OK;
}

MMC_API void mmc_uninit()
{
    std::lock_guard<std::mutex> lock(gMmcMutex);
    if (!mmcInit) {
        MMC_LOG_TRACE("mmc is not init");
        return;
    }

    UnregisterPeriodicTask(K_CONFIG_POLL_TASK_NAME);
    MmcPeriodicTaskFactory::DestroyInstance();
    if (g_localService != nullptr) {
        mmcs_local_service_stop(g_localService);
        g_localService = nullptr;
    }
    mmcc_uninit();
    g_mmcSetup = false;
    mmcInit = false;
}
