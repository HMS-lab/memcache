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
#include <gtest/gtest.h>

#include "mmc_msg_packer.h"

using namespace ock::mmc;

class MmcMsgPacker : public testing::Test {
public:
    static void SetUpTestCase() {}

    static void TearDownTestCase() {}

    void SetUp() override {}

    void TearDown() override {}
};

TEST_F(MmcMsgPacker, pack_and_unpack)
{
    int32_t a = 1;
    std::string b = "hello";
    std::vector<int32_t> c = {1, 2, 3, 4, 5};
    std::map<std::string, int32_t> d = {{"one", 1}, {"two", 2}};
    NetMsgPacker packer;
    packer.Serialize(a);
    packer.Serialize(b);
    packer.Serialize(c);
    packer.Serialize(d);

    auto serialized = packer.String();

    int32_t aa = 0;
    std::string bb = "";
    std::vector<int32_t> cc;
    std::map<std::string, int32_t> dd;
    NetMsgUnpacker unpacker(serialized);
    unpacker.Deserialize(aa);
    unpacker.Deserialize(bb);
    unpacker.Deserialize(cc);
    unpacker.Deserialize(dd);

    ASSERT_EQ(aa == a, true);
    ASSERT_EQ(bb == b, true);
    ASSERT_EQ(cc == c, true);
    ASSERT_EQ(dd == d, true);

    std::cout << aa << " " << bb << " " << std::endl;
}

TEST_F(MmcMsgPacker, deserialize_oversized_string)
{
    std::string raw;
    const uint32_t hugeSize = static_cast<uint32_t>(MAX_CONTAINER_SIZE) + 1;
    raw.append(reinterpret_cast<const char *>(&hugeSize), sizeof(hugeSize));
    NetMsgUnpacker unpacker(raw);
    std::string val = "nonempty";
    unpacker.Deserialize(val);
    EXPECT_EQ(val, "nonempty");
}

TEST_F(MmcMsgPacker, deserialize_oversized_vector)
{
    std::string raw;
    const std::size_t hugeSize = static_cast<std::size_t>(MAX_CONTAINER_SIZE) + 1;
    raw.append(reinterpret_cast<const char *>(&hugeSize), sizeof(hugeSize));
    NetMsgUnpacker unpacker(raw);
    std::vector<int32_t> vec;
    vec.push_back(1);
    unpacker.Deserialize(vec);
    EXPECT_EQ(vec.size(), 1u);
}

TEST_F(MmcMsgPacker, deserialize_oversized_map)
{
    std::string raw;
    const std::size_t hugeSize = static_cast<std::size_t>(MAX_CONTAINER_SIZE) + 1;
    raw.append(reinterpret_cast<const char *>(&hugeSize), sizeof(hugeSize));
    NetMsgUnpacker unpacker(raw);
    std::map<std::string, int32_t> mp;
    mp["x"] = 1;
    unpacker.Deserialize(mp);
    EXPECT_EQ(mp.size(), 1u);
}

TEST_F(MmcMsgPacker, deserialize_empty_vector_and_map)
{
    NetMsgPacker packer;
    std::vector<int32_t> emptyVec;
    std::map<std::string, int32_t> emptyMap;
    packer.Serialize(emptyVec);
    packer.Serialize(emptyMap);
    auto serialized = packer.String();

    NetMsgUnpacker unpacker(serialized);
    std::vector<int32_t> vec;
    std::map<std::string, int32_t> mp;
    unpacker.Deserialize(vec);
    unpacker.Deserialize(mp);
    EXPECT_TRUE(vec.empty());
    EXPECT_TRUE(mp.empty());
}
