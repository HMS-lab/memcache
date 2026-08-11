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

#include <iostream>
#include <thread>
#include <vector>
#include "gtest/gtest.h"
#include "mmc_read_write_lock.h"
#include "config/mmc_functions.h"
#include "mmc_ref.h"

using namespace testing;
using namespace std;
using namespace ock::mmc;

class TestFunctions : public testing::Test {
public:
    TestFunctions();

    void SetUp() override;

    void TearDown() override;

protected:
};
TestFunctions::TestFunctions() {}

void TestFunctions::SetUp() {}

void TestFunctions::TearDown() {}

TEST_F(TestFunctions, test_str_utils)
{
    uint64_t uint64{0};
    float aFloat{0};
    bool aBool{true};
    aBool = OckStoULL("1", uint64);
    ASSERT_TRUE(uint64 == 1u);
    aBool = OckStoULL("1a", uint64);
    ASSERT_TRUE(!aBool);
    aBool = OckStof("aa", aFloat);
    ASSERT_TRUE(!aBool);
    aBool = OckStof("1.1", aFloat);
    ASSERT_TRUE(aBool);
    std::string path = "tmp";
    ASSERT_TRUE(GetRealPath(path) == false);
    path = "/tmp";
    ASSERT_TRUE(GetRealPath(path) == true);

    std::set<std::string> set;
    std::vector<std::string> vector;
    SplitStr("a,b", ",", set);
    SplitStr("a,b", ",", vector);
    ASSERT_TRUE(set.size() == 2u);
    ASSERT_TRUE(vector.size() == 2u);
}

TEST_F(TestFunctions, OckStoULL_EdgeCases)
{
    uint64_t val = 99;
    EXPECT_TRUE(OckStoULL("0", val));
    EXPECT_EQ(val, 0u);

    EXPECT_FALSE(OckStoULL("", val));
    EXPECT_FALSE(OckStoULL("abc", val));
}

TEST_F(TestFunctions, OckStof_EdgeCases)
{
    float val = 0;
    EXPECT_TRUE(OckStof("0.0", val));
    EXPECT_TRUE(val >= -0.000001f && val <= 0.000001f);

    EXPECT_TRUE(OckStof("-1.5", val));
    EXPECT_TRUE(val < -1.0f);
}

TEST_F(TestFunctions, OckStol_Conversions)
{
    constexpr long expectedLong = 123;
    long val = 0;
    EXPECT_TRUE(OckStol("123", val));
    EXPECT_EQ(val, expectedLong);

    EXPECT_TRUE(OckStol("0", val));
    EXPECT_EQ(val, 0);

    EXPECT_FALSE(OckStol("abc", val));
    EXPECT_FALSE(OckStol("", val));
}

TEST_F(TestFunctions, IsBool_Test)
{
    bool val = false;
    EXPECT_TRUE(IsBool("true", val));
    EXPECT_TRUE(val);

    EXPECT_TRUE(IsBool("false", val));
    EXPECT_FALSE(val);

    EXPECT_TRUE(IsBool("1", val));
    EXPECT_TRUE(val);

    EXPECT_TRUE(IsBool("0", val));
    EXPECT_FALSE(val);

    EXPECT_FALSE(IsBool("maybe", val));
}

TEST_F(TestFunctions, StartsWith_Test)
{
    EXPECT_TRUE(StartsWith("ock.mmc.key", "ock.mmc."));
    EXPECT_TRUE(StartsWith("ock", "ock"));
    EXPECT_FALSE(StartsWith("ock", "ock.mmc"));
    EXPECT_FALSE(StartsWith("", "ock"));
    EXPECT_TRUE(StartsWith("ock", ""));
}

TEST_F(TestFunctions, OckTrimString_EmptyAndWhitespace)
{
    std::string empty;
    OckTrimString(empty);
    EXPECT_TRUE(empty.empty());

    std::string s = "  hello  ";
    OckTrimString(s);
    EXPECT_EQ(s, "hello");
}

TEST_F(TestFunctions, SplitStr_EmptySeparator)
{
    std::set<std::string> setRes;
    SplitStr("hello", "", setRes);
    EXPECT_EQ(setRes.size(), 1u);

    std::set<std::string> emptySet;
    SplitStr("", "", emptySet);
    EXPECT_TRUE(emptySet.empty());

    std::vector<std::string> vecRes;
    SplitStr("hello", "", vecRes);
    EXPECT_EQ(vecRes.size(), 1u);

    std::vector<std::string> emptyVec;
    SplitStr("", "", emptyVec);
    EXPECT_TRUE(emptyVec.empty());
}

TEST_F(TestFunctions, SplitStr_TrailingSeparator)
{
    std::set<std::string> setRes;
    SplitStr("a,b,", ",", setRes);
    EXPECT_EQ(setRes.size(), 2u);

    std::vector<std::string> vecRes;
    SplitStr("a,b,", ",", vecRes);
    EXPECT_EQ(vecRes.size(), 2u);
    EXPECT_EQ(vecRes[0], "a");
    EXPECT_EQ(vecRes[1], "b");
}
