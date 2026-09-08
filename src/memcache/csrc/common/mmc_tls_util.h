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

#ifndef MEMCACHE_MMC_TLS_UTIL_H
#define MEMCACHE_MMC_TLS_UTIL_H

#include <algorithm>
#include <cstdint>
#include <dlfcn.h>

namespace ock {
namespace mmc {

using MmcDecryptFunc = int (*)(const char *cipherText, const size_t cipherTextLen, char *plainText,
                               size_t plainTextLen);

class MmcTlsUtil {
public:
    static inline int32_t DefaultDecrypter(const char *cipherText, const size_t cipherTextLen, char *plainText,
                                           const size_t plainTextLen)
    {
        std::copy_n(cipherText, plainTextLen, plainText);
        return 0;
    }

    static inline void **GetTlsLibHandler()
    {
        static void *decryptLibHandle = nullptr;
        return &decryptLibHandle;
    }

    static MmcDecryptFunc LoadDecryptFunction(const char *decrypterLibPath);

    static inline void CloseTlsLib()
    {
        void **decryptLibHandlePtr = GetTlsLibHandler();
        if (*decryptLibHandlePtr != nullptr) {
            dlclose(*decryptLibHandlePtr);
            *decryptLibHandlePtr = nullptr;
            decryptLibHandlePtr = nullptr;
        }
    }
};
} // namespace mmc
} // namespace ock

#endif // MEMCACHE_MMC_TLS_UTIL_H
