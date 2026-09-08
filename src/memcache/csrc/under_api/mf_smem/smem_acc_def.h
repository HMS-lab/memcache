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
#ifndef MEMCACHE_SMEM_ACC_DEF_H
#define MEMCACHE_SMEM_ACC_DEF_H

#include <stddef.h>
#include <stdint.h>

#include "smem_tls_options.h"

typedef void *smem_acc_server_t;
typedef void *smem_acc_link_t;
typedef void *smem_acc_http_request_t;

typedef struct {
    const char *listen_ip;
    uint16_t listen_port;
    uint16_t worker_count;
    uint16_t link_send_queue_size;
    int16_t magic;
    int16_t version;
    uint8_t enable_listener;
} smem_acc_tcp_server_options_t;

typedef struct {
    const char *listen_ip;
    uint16_t listen_port;
    uint16_t worker_count;
    size_t max_body_size;
} smem_acc_http_server_options_t;

typedef struct {
    int16_t magic;
    int16_t version;
    uint8_t reconnect;
    uint64_t rank_id;
} smem_acc_connect_request_t;

typedef struct {
    int16_t type;
    int16_t result;
    uint32_t body_len;
    uint32_t seq_no;
} smem_acc_message_t;

typedef struct {
    int16_t type;
    int16_t result;
    uint32_t body_len;
    uint32_t seq_no;
    uint32_t crc;
} smem_acc_message_v2_t;

typedef int32_t (*smem_acc_new_link_cb)(void *context, const smem_acc_connect_request_t *request, smem_acc_link_t link);
typedef int32_t (*smem_acc_request_cb)(void *context, const smem_acc_message_t *message, const void *data,
                                       uint32_t data_len, smem_acc_link_t link);
typedef int32_t (*smem_acc_sent_cb)(void *context, int32_t result, const smem_acc_message_t *message);
typedef int32_t (*smem_acc_request_cb_v2)(void *context, const smem_acc_message_v2_t *message, const void *data,
                                          uint32_t data_len, smem_acc_link_t link);
typedef int32_t (*smem_acc_sent_cb_v2)(void *context, int32_t result, const smem_acc_message_v2_t *message,
                                       const void *callback_data, uint32_t callback_data_len);
typedef int32_t (*smem_acc_link_broken_cb)(void *context, smem_acc_link_t link);
typedef int32_t (*smem_acc_http_cb)(void *context, smem_acc_http_request_t request);
typedef void (*smem_acc_http_param_cb)(void *context, const char *name, const char *value);
typedef int32_t (*smem_decrypt_cb)(void *context, const char *cipher_text, size_t cipher_text_len, uint8_t *plain_text,
                                   size_t plain_text_len);

#endif // MEMCACHE_SMEM_ACC_DEF_H
