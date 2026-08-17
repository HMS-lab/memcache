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
#include "mmc_kv_event_zmq_transport.h"

#include <zmq.h>

#include <cstring>
#include <utility>

#include "mmc_kv_event.h"
#include "mmc_logger.h"

namespace ock {
namespace mmc {
namespace kv_event {
namespace {

constexpr int K_ZMQ_SEND_HWM = 10000;
constexpr int K_ZMQ_LINGER_MS = 0;
constexpr int K_ZMQ_IPV6_ENABLED = 1;
constexpr size_t K_CURVE_KEY_BIN_SIZE = 32;
constexpr size_t K_CURVE_KEY_Z85_LEN = 40;

void ApplyCurveServer(void *socket, const KvEventConfig &config)
{
    if (!config.curveEnable) {
        return;
    }
    if (config.curveServerPublicKey.empty() || config.curveServerSecretKey.empty()) {
        MMC_LOG_ERROR("kv_events: curve_enable=true but server key is missing, FALLBACK TO PLAINTEXT");
        return;
    }
    if (config.curveServerPublicKey.size() != K_CURVE_KEY_Z85_LEN ||
        config.curveServerSecretKey.size() != K_CURVE_KEY_Z85_LEN) {
        MMC_LOG_ERROR("kv_events: curve key length must be " << K_CURVE_KEY_Z85_LEN
                                                             << " Z85 chars, FALLBACK TO PLAINTEXT");
        return;
    }
    uint8_t dummy[K_CURVE_KEY_BIN_SIZE] = {0};
    if (zmq_z85_decode(dummy, config.curveServerPublicKey.c_str()) == nullptr ||
        zmq_z85_decode(dummy, config.curveServerSecretKey.c_str()) == nullptr) {
        MMC_LOG_ERROR("kv_events: invalid CURVE key, FALLBACK TO PLAINTEXT");
        return;
    }
    int on = 1;
    // ZMQ_CURVE_PUBLICKEY/SECRETKEY expect the 40-char Z85 string (41 bytes incl. NUL),
    // NOT the decoded 32-byte binary. Length < 41 yields EINVAL.
    // Set ZMQ_CURVE_SERVER LAST: if any key option fails, the socket must stay in
    // plaintext mode, otherwise a half-configured CURVE server would break zmq_bind
    // while the log claims a plaintext fallback.
    const size_t curveKeyOptLen = K_CURVE_KEY_Z85_LEN + 1;
    if (zmq_setsockopt(socket, ZMQ_CURVE_PUBLICKEY, config.curveServerPublicKey.c_str(), curveKeyOptLen) != 0) {
        MMC_LOG_ERROR("kv_events: failed to set ZMQ_CURVE_PUBLICKEY: " << zmq_strerror(zmq_errno())
                                                                       << ", FALLBACK TO PLAINTEXT");
        return;
    }
    if (zmq_setsockopt(socket, ZMQ_CURVE_SECRETKEY, config.curveServerSecretKey.c_str(), curveKeyOptLen) != 0) {
        MMC_LOG_ERROR("kv_events: failed to set ZMQ_CURVE_SECRETKEY: " << zmq_strerror(zmq_errno())
                                                                       << ", FALLBACK TO PLAINTEXT");
        return;
    }
    if (zmq_setsockopt(socket, ZMQ_CURVE_SERVER, &on, sizeof(on)) != 0) {
        MMC_LOG_ERROR("kv_events: failed to set ZMQ_CURVE_SERVER: " << zmq_strerror(zmq_errno())
                                                                    << ", FALLBACK TO PLAINTEXT");
        return;
    }
    MMC_LOG_INFO("kv_events: ZMQ CURVE encryption enabled (server-only auth)");
}

class ZmqMsg {
public:
    ZmqMsg()
    {
        zmq_msg_init(&msg_);
    }
    explicit ZmqMsg(size_t size)
    {
        zmq_msg_init_size(&msg_, size);
    }
    ~ZmqMsg()
    {
        zmq_msg_close(&msg_);
    }

    ZmqMsg(const ZmqMsg &) = delete;
    ZmqMsg &operator=(const ZmqMsg &) = delete;

    zmq_msg_t *Get()
    {
        return &msg_;
    }
    void *Data()
    {
        return zmq_msg_data(&msg_);
    }

private:
    zmq_msg_t msg_{};
};

class ZmqPubTransport : public IKvEventTransport {
public:
    ZmqPubTransport(void *context, void *socket, std::string topic)
        : context_(context), socket_(socket), topic_(std::move(topic))
    {}

    ~ZmqPubTransport() override
    {
        if (socket_ != nullptr) {
            zmq_close(socket_);
            socket_ = nullptr;
        }
        if (context_ != nullptr) {
            zmq_ctx_destroy(context_);
            context_ = nullptr;
        }
    }

    ZmqPubTransport(const ZmqPubTransport &) = delete;
    ZmqPubTransport &operator=(const ZmqPubTransport &) = delete;

    bool Send(uint64_t seq, const std::string &payload) override
    {
        MMC_LOG_DEBUG("kv_events: prepare to send payload[:10]=" << payload.substr(0, 10) << " seq=" << seq);
        const auto seqBe = EncodeBigEndianU64(seq);

        ZmqMsg topicMsg(topic_.size());
        ZmqMsg seqMsg(seqBe.size());
        ZmqMsg payloadMsg(payload.size());
        std::memcpy(topicMsg.Data(), topic_.data(), topic_.size());
        std::memcpy(seqMsg.Data(), seqBe.data(), seqBe.size());
        std::memcpy(payloadMsg.Data(), payload.data(), payload.size());

        if (zmq_msg_send(topicMsg.Get(), socket_, ZMQ_SNDMORE | ZMQ_DONTWAIT) < 0) {
            MMC_LOG_WARN("kv_events: send topic frame failed: " << zmq_strerror(zmq_errno()));
            return false;
        }
        if (zmq_msg_send(seqMsg.Get(), socket_, ZMQ_SNDMORE | ZMQ_DONTWAIT) < 0) {
            MMC_LOG_WARN("kv_events: send seq frame failed: " << zmq_strerror(zmq_errno()));
            return false;
        }
        if (zmq_msg_send(payloadMsg.Get(), socket_, ZMQ_DONTWAIT) < 0) {
            MMC_LOG_WARN("kv_events: send payload frame failed: " << zmq_strerror(zmq_errno()));
            return false;
        }
        return true;
    }

private:
    void *context_{nullptr};
    void *socket_{nullptr};
    std::string topic_;
};

} // namespace

std::unique_ptr<IKvEventTransport> MakeZmqPubTransport(const KvEventConfig &config)
{
    if (!config.enabled) {
        return nullptr;
    }
    if (config.bindEndpoint.empty()) {
        MMC_LOG_ERROR("kv_events enabled but bind_endpoint is empty, publisher disabled");
        return nullptr;
    }
    if (config.topic.empty()) {
        MMC_LOG_ERROR("kv_events enabled but topic is empty, publisher disabled");
        return nullptr;
    }

    void *context = zmq_ctx_new();
    if (context == nullptr) {
        MMC_LOG_ERROR("kv_events: failed to create ZMQ context");
        return nullptr;
    }
    void *socket = zmq_socket(context, ZMQ_PUB);
    if (socket == nullptr) {
        MMC_LOG_ERROR("kv_events: failed to create ZMQ PUB socket: " << zmq_strerror(zmq_errno()));
        zmq_ctx_destroy(context);
        return nullptr;
    }

    int hwm = K_ZMQ_SEND_HWM;
    (void)zmq_setsockopt(socket, ZMQ_SNDHWM, &hwm, sizeof(hwm));
    int linger = K_ZMQ_LINGER_MS;
    (void)zmq_setsockopt(socket, ZMQ_LINGER, &linger, sizeof(linger));
    // ZMQ_IPV6 must be set before bind. Enabling it makes the socket dual-stack:
    // it accepts connections from both IPv4 and IPv6 hosts (see ZMQ_IPV6 in the
    // zmq_setsockopt docs), so binding "tcp://*:port" covers both families.
    int ipv6 = K_ZMQ_IPV6_ENABLED;
    (void)zmq_setsockopt(socket, ZMQ_IPV6, &ipv6, sizeof(ipv6));

    ApplyCurveServer(socket, config);

    if (zmq_bind(socket, config.bindEndpoint.c_str()) != 0) {
        MMC_LOG_ERROR("kv_events: zmq_bind failed for " << config.bindEndpoint << ": " << zmq_strerror(zmq_errno()));
        zmq_close(socket);
        zmq_ctx_destroy(context);
        return nullptr;
    }

    MMC_LOG_INFO("kv_events publisher bound on " << config.bindEndpoint << " topic=" << config.topic << " model_name="
                                                 << config.modelName << " tenant_id=" << config.tenantId);
    return std::make_unique<ZmqPubTransport>(context, socket, config.topic);
}

} // namespace kv_event
} // namespace mmc
} // namespace ock
