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

#include "common/mmc_functions.h"
#include "gtest/gtest.h"
#include <array>
#include <cstdlib>

using ock::mmc::SafeCopy;

TEST(SafeCopyDeathTest, NullDestinationReturnsNormally)
{
    EXPECT_EXIT(
        {
            SafeCopy("payload", nullptr, sizeof("payload"));
            SafeCopy("", nullptr, sizeof("payload"));
            SafeCopy("payload", nullptr, 0);
            std::_Exit(EXIT_SUCCESS);
        },
        testing::ExitedWithCode(EXIT_SUCCESS), "");
}

TEST(SafeCopyTest, ZeroCapacityLeavesDestinationUnchanged)
{
    constexpr char marker = '#';
    std::array<char, sizeof("payload")> destination{};
    destination.fill(marker);
    const auto expected = destination;

    SafeCopy("payload", destination.data(), 0);
    EXPECT_EQ(destination, expected);

    destination.fill(marker);
    SafeCopy("", destination.data(), 0);
    EXPECT_EQ(destination, expected);
}
