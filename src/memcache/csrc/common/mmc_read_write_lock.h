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
#ifndef MEMFABRIC_HYBRID_MMC_READWRITELOCK_H
#define MEMFABRIC_HYBRID_MMC_READWRITELOCK_H

#include <memory>
#include <shared_mutex>

namespace ock {
namespace mmc {
class ReadWriteLock {
public:
    ReadWriteLock() = default;

    void LockRead()
    {
        mutex_.lock_shared();
    }

    void UnlockRead()
    {
        mutex_.unlock_shared();
    }

    void LockWrite()
    {
        mutex_.lock();
    }

    void UnlockWrite()
    {
        mutex_.unlock();
    }

private:
    // The allocator is read-mostly: Alloc/Free/GetUsedInfo all take the shared
    // lock, so concurrent readers must not be serialized through a mutex.
    std::shared_mutex mutex_;
};

class ReadLock {
public:
    explicit ReadLock(ReadWriteLock &rwLock) : rwLock_(rwLock)
    {
        rwLock_.LockRead();
    }
    ~ReadLock()
    {
        rwLock_.UnlockRead();
    }

    // forbid copy like the standard library
    ReadLock(const ReadLock &) = delete;
    ReadLock &operator=(const ReadLock &) = delete;

private:
    ReadWriteLock &rwLock_;
};

class WriteLock {
public:
    explicit WriteLock(ReadWriteLock &rwLock) : rwLock_(rwLock)
    {
        rwLock_.LockWrite();
    }
    ~WriteLock()
    {
        rwLock_.UnlockWrite();
    }

    // forbid copy like the standard library
    WriteLock(const ReadLock &) = delete;
    WriteLock &operator=(const ReadLock &) = delete;

private:
    ReadWriteLock &rwLock_;
};

} // namespace mmc
} // namespace ock

#endif // MEMFABRIC_HYBRID_MMC_READWRITELOCK_H
