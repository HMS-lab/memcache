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
#include <limits>
#include <gtest/gtest.h>
#include "mmc_acc_runtime.h"

namespace ock::mmc_acc {
namespace {
constexpr uint32_t kOversizedBuffer = MAX_RECV_BODY_LEN + 1;
constexpr uint32_t kExpandedBufferSize = 64;
constexpr char kBufferContents[] = "buffer contents";
} // namespace

TEST(AccDataBufferTest, RejectsOversizedConstructionAndRetry)
{
    EXPECT_EQ(AccDataBuffer::Create(kOversizedBuffer).Get(), nullptr);
    EXPECT_EQ(AccDataBuffer::Create(kBufferContents, kOversizedBuffer).Get(), nullptr);
    EXPECT_EQ(AccDataBuffer::Create(std::numeric_limits<uint32_t>::max()).Get(), nullptr);
    AccDataBuffer buffer(kOversizedBuffer);
    EXPECT_EQ(buffer.DataPtr(), nullptr);
    EXPECT_FALSE(buffer.AllocIfNeed());
    EXPECT_FALSE(buffer.AllocIfNeed(sizeof(kBufferContents)));
    EXPECT_EQ(buffer.DataPtr(), nullptr);
}

TEST(AccDataBufferTest, GrowthPreservesContentsAndRejectsOversizedRequests)
{
    auto buffer = AccDataBuffer::Create(kBufferContents, sizeof(kBufferContents));
    ASSERT_NE(buffer.Get(), nullptr);
    ASSERT_TRUE(buffer->AllocIfNeed(kExpandedBufferSize));
    EXPECT_EQ(buffer->DataLen(), sizeof(kBufferContents));
    EXPECT_STREQ(reinterpret_cast<const char *>(buffer->DataPtr()), kBufferContents);
    auto *data = buffer->DataPtr();
    EXPECT_FALSE(buffer->AllocIfNeed(kOversizedBuffer));
    EXPECT_EQ(buffer->DataPtr(), data);
    EXPECT_EQ(buffer->MemSize(), kExpandedBufferSize);
    EXPECT_STREQ(reinterpret_cast<const char *>(buffer->DataPtr()), kBufferContents);
}

TEST(AccDataBufferTest, AcceptsMaximumBufferSize)
{
    auto buffer = AccDataBuffer::Create(MAX_RECV_BODY_LEN);
    ASSERT_NE(buffer.Get(), nullptr);
    EXPECT_EQ(buffer->MemSize(), MAX_RECV_BODY_LEN);
    EXPECT_TRUE(buffer->AllocIfNeed(MAX_RECV_BODY_LEN));
}
} // namespace ock::mmc_acc
