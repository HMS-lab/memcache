/* Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
  Licensed under Mulan PSL v2. See the repository LICENSE file. */

#ifndef META_MANAGER_SIM_NATIVE_KEY_H
#define META_MANAGER_SIM_NATIVE_KEY_H

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <openssl/sha.h>

namespace ock::mmc {

inline std::pair<std::string, std::string> TranslateKey(const std::string &key)
{
    constexpr int hexByteWidth = 2;

    // Note: Split at the last colon; unprefixed keys have an empty model
    const auto separator = key.rfind(':');
    const auto model = separator == std::string::npos ? std::string{} : key.substr(0, separator);
    const auto blockId = separator == std::string::npos ? key : key.substr(separator + 1);
    auto prefix = model;
    std::replace(prefix.begin(), prefix.end(), '-', '_');
    prefix += "_" + blockId + "_tp_0_pp_0";

    // Note: Hash the normalized prefix to match the native key format
    unsigned char digest[SHA256_DIGEST_LENGTH];
    if (SHA256(reinterpret_cast<const unsigned char *>(prefix.data()), prefix.size(), digest) == nullptr) {
        throw std::runtime_error("SHA-256 failed");
    }

    std::ostringstream nativeKey;
    nativeKey << prefix << '_' << std::hex << std::setfill('0');
    for (auto byte : digest) {
        nativeKey << std::setw(hexByteWidth) << static_cast<unsigned int>(byte);
    }
    return {model, nativeKey.str()};
}

} // namespace ock::mmc

#endif // META_MANAGER_SIM_NATIVE_KEY_H
