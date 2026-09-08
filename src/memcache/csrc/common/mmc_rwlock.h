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
#ifndef MEMCACHE_MMC_RWLOCK_H
#define MEMCACHE_MMC_RWLOCK_H

#include <pthread.h>

namespace ock {
namespace mmc {
class MmcReadWriteLock {
public:
    MmcReadWriteLock()
    {
        pthread_rwlock_init(&mLock, nullptr);
    }

    ~MmcReadWriteLock()
    {
        pthread_rwlock_destroy(&mLock);
    }

    MmcReadWriteLock(const MmcReadWriteLock &) = delete;
    MmcReadWriteLock &operator=(const MmcReadWriteLock &) = delete;
    MmcReadWriteLock(MmcReadWriteLock &&) = delete;
    MmcReadWriteLock &operator=(MmcReadWriteLock &&) = delete;

    inline void LockRead()
    {
        pthread_rwlock_rdlock(&mLock);
    }

    inline void LockWrite()
    {
        pthread_rwlock_wrlock(&mLock);
    }

    inline void UnLock()
    {
        pthread_rwlock_unlock(&mLock);
    }

private:
    pthread_rwlock_t mLock{};
};

class MmcWriteGuard {
public:
    MmcWriteGuard(MmcReadWriteLock &lock) : lock_(lock)
    {
        lock_.LockWrite();
    }

    ~MmcWriteGuard()
    {
        lock_.UnLock();
    }

    MmcWriteGuard(const MmcWriteGuard &) = delete;
    MmcWriteGuard &operator=(const MmcWriteGuard &) = delete;
    MmcWriteGuard(MmcWriteGuard &&) = delete;
    MmcWriteGuard &operator=(MmcWriteGuard &&) = delete;

private:
    MmcReadWriteLock &lock_;
};

class MmcReadGuard {
public:
    MmcReadGuard(MmcReadWriteLock &lock) : lock_(lock)
    {
        lock_.LockRead();
    }

    ~MmcReadGuard()
    {
        lock_.UnLock();
    }

    MmcReadGuard(const MmcReadGuard &) = delete;
    MmcReadGuard &operator=(const MmcReadGuard &) = delete;
    MmcReadGuard(MmcReadGuard &&) = delete;
    MmcReadGuard &operator=(MmcReadGuard &&) = delete;

private:
    MmcReadWriteLock &lock_;
};

} // namespace mmc
} // namespace ock
#endif
