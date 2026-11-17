/* Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
  Licensed under Mulan PSL v2. See the repository LICENSE file. */

#include "native_key.h"

#include <iostream>

namespace {
constexpr int kKeyArgument = 1;
} // namespace

int main(int, char **argv)
{
    const auto [model, nativeKey] = ock::mmc::TranslateKey(argv[kKeyArgument]);
    std::cout << model << '\n' << nativeKey << '\n';
}
