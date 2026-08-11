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

#include "gtest/gtest.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <unistd.h>

#define private public
#include "mmc_kv_parser.h"
#undef private

using namespace testing;
using namespace std;
using namespace ock::mmc;

class TestKVParser : public testing::Test {
public:
    void SetUp() override {}
    void TearDown() override {}
};

// ======================== ParseLine prefix filter ========================

TEST_F(TestKVParser, ParseLine_AcceptsMmcPrefixedKey)
{
    KVParser parser;
    string line = "ock.mmc.test_key = test_value";
    EXPECT_EQ(parser.ParseLine(line), MMC_OK);
    EXPECT_EQ(parser.Size(), 1U);
}

TEST_F(TestKVParser, ParseLine_SkipsNonMmcPrefixedKey)
{
    KVParser parser;
    string line = "ock.ubs.test_key = test_value";
    EXPECT_EQ(parser.ParseLine(line), MMC_OK);
    EXPECT_EQ(parser.Size(), 0U);
}

TEST_F(TestKVParser, ParseLine_SkipsMmcWithoutTrailingDot)
{
    KVParser parser;
    string line = "ock.mmc_extra = value";
    EXPECT_EQ(parser.ParseLine(line), MMC_OK);
    EXPECT_EQ(parser.Size(), 0U);
}

TEST_F(TestKVParser, ParseLine_SkipsKeyWithoutOckPrefix)
{
    KVParser parser;
    string line = "random_key = value";
    EXPECT_EQ(parser.ParseLine(line), MMC_OK);
    EXPECT_EQ(parser.Size(), 0U);
}

TEST_F(TestKVParser, ParseLine_CommentAndEmptyStillSkipped)
{
    KVParser parser;
    string commentLine = "# ock.mmc.key = value";
    EXPECT_EQ(parser.ParseLine(commentLine), MMC_OK);
    string emptyLine = "";
    EXPECT_EQ(parser.ParseLine(emptyLine), MMC_OK);
    string spaces = "   ";
    EXPECT_EQ(parser.ParseLine(spaces), MMC_OK);
    EXPECT_EQ(parser.Size(), 0U);
}

TEST_F(TestKVParser, ParseLine_KeyWithOnlyMmcPrefixNoDot)
{
    KVParser parser;
    string line = "ock.mmc = value";
    EXPECT_EQ(parser.ParseLine(line), MMC_OK);
    EXPECT_EQ(parser.Size(), 0U);
}

// ======================== FromFile integration ========================

namespace {

string WriteTempConfigFile(const string &content)
{
    char tmpl[] = "/tmp/mmc_kv_parser_test_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd == -1) {
        return {};
    }
    close(fd);

    string filePath = string(tmpl) + ".conf";
    ofstream ofs(filePath, ios::trunc);
    if (!ofs.is_open()) {
        return {};
    }
    ofs << content;
    ofs.close();
    remove(tmpl);
    return filePath;
}

} // namespace

TEST_F(TestKVParser, FromFile_MixedKeys_OnlyMmcLoaded)
{
    string content = "# comment line\n"
                     "ock.mmc.meta_service_url = tcp://127.0.0.1:5000\n"
                     "ock.ubs.config = some_value\n"
                     "\n"
                     "ock.mmc.log_level = info\n"
                     "random_key = random_value\n"
                     "ock.mmc.local_service.world_size = 256\n";

    string filePath = WriteTempConfigFile(content);
    ASSERT_FALSE(filePath.empty());

    KVParser parser;
    EXPECT_EQ(parser.FromFile(filePath), MMC_OK);
    EXPECT_EQ(parser.Size(), 3U);

    string key, value;
    parser.GetI(0, key, value);
    EXPECT_EQ(key, "ock.mmc.meta_service_url");
    parser.GetI(1, key, value);
    EXPECT_EQ(key, "ock.mmc.log_level");
    parser.GetI(2, key, value);
    EXPECT_EQ(key, "ock.mmc.local_service.world_size");

    remove(filePath.c_str());
}

TEST_F(TestKVParser, FromFile_AllNonMmc_Skipped)
{
    string content = "ock.ubs.key1 = value1\n"
                     "other.key2 = value2\n";

    string filePath = WriteTempConfigFile(content);
    ASSERT_FALSE(filePath.empty());

    KVParser parser;
    EXPECT_EQ(parser.FromFile(filePath), MMC_OK);
    EXPECT_EQ(parser.Size(), 0U);

    remove(filePath.c_str());
}

TEST_F(TestKVParser, FromFile_AllMmc_AllLoaded)
{
    string content = "ock.mmc.key1 = value1\n"
                     "ock.mmc.key2 = value2\n";

    string filePath = WriteTempConfigFile(content);
    ASSERT_FALSE(filePath.empty());

    KVParser parser;
    EXPECT_EQ(parser.FromFile(filePath), MMC_OK);
    EXPECT_EQ(parser.Size(), 2U);

    remove(filePath.c_str());
}

TEST_F(TestKVParser, FromFile_DuplicateMmcKey_ReturnsError)
{
    string content = "ock.mmc.key1 = value1\n"
                     "ock.mmc.key1 = value2\n";

    string filePath = WriteTempConfigFile(content);
    ASSERT_FALSE(filePath.empty());

    KVParser parser;
    EXPECT_NE(parser.FromFile(filePath), MMC_OK);

    remove(filePath.c_str());
}

// ======================== GetItem / GetI edge cases ========================

TEST_F(TestKVParser, GetItem_NotFound_ReturnsError)
{
    KVParser parser;
    string line = "ock.mmc.key1 = value1";
    parser.ParseLine(line);
    string val;
    EXPECT_NE(parser.GetItem("ock.mmc.nonexistent", val), MMC_OK);
}

TEST_F(TestKVParser, GetItem_Found_ReturnsValue)
{
    KVParser parser;
    string line = "ock.mmc.key1 = value1";
    parser.ParseLine(line);
    string val;
    EXPECT_EQ(parser.GetItem("ock.mmc.key1", val), MMC_OK);
    EXPECT_EQ(val, "value1");
}

TEST_F(TestKVParser, GetI_OutOfBounds_DoesNothing)
{
    constexpr uint32_t outOfBoundsIndex = 100;
    KVParser parser;
    string line = "ock.mmc.key1 = value1";
    parser.ParseLine(line);
    string key;
    string val;
    parser.GetI(outOfBoundsIndex, key, val);
    EXPECT_TRUE(key.empty());
    EXPECT_TRUE(val.empty());
}

TEST_F(TestKVParser, GetI_ValidIndex_ReturnsKeyAndValue)
{
    KVParser parser;
    string line = "ock.mmc.key1 = value1";
    parser.ParseLine(line);
    string key;
    string val;
    parser.GetI(0, key, val);
    EXPECT_EQ(key, "ock.mmc.key1");
    EXPECT_EQ(val, "value1");
}

// ======================== ParseLine error branches ========================

TEST_F(TestKVParser, ParseLine_EmptyKeyAfterEquals_ReturnsError)
{
    KVParser parser;
    string line = "= value_without_key";
    EXPECT_NE(parser.ParseLine(line), MMC_OK);
}

TEST_F(TestKVParser, ParseLine_LineTooLong_ReturnsError)
{
    constexpr size_t overLimitLineLength = 5000;
    KVParser parser;
    string longLine = "ock.mmc.key = ";
    longLine.append(overLimitLineLength, 'x');
    EXPECT_NE(parser.ParseLine(longLine), MMC_OK);
}

TEST_F(TestKVParser, ParseLine_LineWithoutEquals_Skipped)
{
    KVParser parser;
    string line = "ock.mmc.key_without_equals";
    EXPECT_EQ(parser.ParseLine(line), MMC_OK);
    EXPECT_EQ(parser.Size(), 0U);
}

// ======================== CheckSet ========================

TEST_F(TestKVParser, CheckSet_MissingKey_ReturnsFalse)
{
    KVParser parser;
    string line = "ock.mmc.key1 = value1";
    parser.ParseLine(line);
    vector<string> keys = {"ock.mmc.key1", "ock.mmc.missing"};
    EXPECT_FALSE(parser.CheckSet(keys));
}

TEST_F(TestKVParser, CheckSet_AllKeysPresent_ReturnsTrue)
{
    KVParser parser;
    string line1 = "ock.mmc.key1 = value1";
    string line2 = "ock.mmc.key2 = value2";
    parser.ParseLine(line1);
    parser.ParseLine(line2);
    vector<string> keys = {"ock.mmc.key1", "ock.mmc.key2"};
    EXPECT_TRUE(parser.CheckSet(keys));
}

// ======================== Dump / Size ========================

TEST_F(TestKVParser, Dump_DoesNotCrash)
{
    KVParser parser;
    string line1 = "ock.mmc.key1 = value1";
    string line2 = "ock.mmc.key2 = value2";
    parser.ParseLine(line1);
    parser.ParseLine(line2);
    testing::internal::CaptureStdout();
    parser.Dump();
    string output = testing::internal::GetCapturedStdout();
    EXPECT_TRUE(output.find("ock.mmc.key1 = value1") != string::npos);
    EXPECT_TRUE(output.find("ock.mmc.key2 = value2") != string::npos);
}

TEST_F(TestKVParser, Size_ReturnsCorrectCount)
{
    KVParser parser;
    EXPECT_EQ(parser.Size(), 0U);
    string line1 = "ock.mmc.key1 = value1";
    parser.ParseLine(line1);
    EXPECT_EQ(parser.Size(), 1U);
    string line2 = "ock.mmc.key2 = value2";
    parser.ParseLine(line2);
    EXPECT_EQ(parser.Size(), 2U);
}
