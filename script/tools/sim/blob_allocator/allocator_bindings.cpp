/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 * Licensed under Mulan PSL v2.
 * http://license.coscl.org.cn/MulanPSL2
 */
#include <limits>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include "mmc_blob_allocator.h"
#include "ptracer.h"

extern "C" {
ptracer g_mmc_tracer{};
}
namespace py = pybind11;
namespace ock::mmc {
namespace {
constexpr uint64_t kAlignment = 4096;
constexpr uint32_t kRank = 0;

void CheckSize(uint64_t size)
{
    if (size == 0 || size > std::numeric_limits<uint64_t>::max() - (kAlignment - 1)) {
        throw std::invalid_argument("Size must be positive and must not overflow alignment.");
    }
}

class Allocator {
public:
    Allocator(uint64_t capacity, bool maxValueAlignment) : maxValueAlignment_(maxValueAlignment)
    {
        CheckSize(capacity);
        MmcOutLogger::Instance().SetLogLevel(ERROR_LEVEL);
        allocator_ = std::make_unique<MmcBlobAllocator>(kRank, MEDIA_DRAM, 0, capacity, maxValueAlignment_, &maximum_);
        allocator_->Start();
    }

    static uint64_t AlignSize(uint64_t size)
    {
        CheckSize(size);
        return MmcBlobAllocator::AllocSizeAlignUp(size);
    }

    uint64_t RequiredSize(uint64_t size) const
    {
        auto alignedSize = AlignSize(size);
        return maxValueAlignment_ ? std::max(alignedSize, maximum_.load()) : alignedSize;
    }

    py::object Alloc(uint64_t size)
    {
        CheckSize(size);
        bool expected = allocator_->CanAlloc(size);
        auto blob = allocator_->Alloc(size);
        if (blob == nullptr) {
            if (expected) {
                throw std::runtime_error("Native allocation failed after a positive capacity check.");
            }
            return py::none();
        }
        if (!expected) {
            throw std::runtime_error("Native allocation contradicted its capacity check.");
        }
        auto handle = ++nextHandle_;
        blobs_.emplace(handle, blob);
        return py::int_(handle);
    }

    void Free(uint64_t handle)
    {
        auto it = blobs_.find(handle);
        if (it == blobs_.end()) {
            throw std::invalid_argument("Unknown or already freed handle.");
        }
        if (allocator_->Release(it->second) != MMC_OK) {
            throw std::runtime_error("Native release failed.");
        }
        blobs_.erase(it);
    }

    py::dict BlobInfo(uint64_t handle) const
    {
        auto it = blobs_.find(handle);
        if (it == blobs_.end()) {
            throw std::invalid_argument("Unknown or already freed handle.");
        }
        py::dict info;
        info["address"] = it->second->Gva();
        info["requested"] = it->second->Size();
        info["reserved"] = it->second->AllocSize();
        return info;
    }

    py::dict Snapshot(bool includeRanges)
    {
        auto [capacity, reserved] = allocator_->GetUsageInfo();
        py::dict info;
        info["capacity"] = capacity;
        info["reserved"] = reserved;
        info["internal_bytes"] = allocator_->GetFragmentationBytes();
        info["maximum_aligned_size"] = maximum_.load();
        info["largest_free"] = allocator_->GetMaxContiguousFreeSize();
        if (includeRanges) {
            py::list ranges;
            for (const auto &range : allocator_->GetFreeSegments()) {
                ranges.append(py::make_tuple(range.offset_, range.size_));
            }
            info["ranges"] = ranges;
        }
        return info;
    }

private:
    std::atomic<uint64_t> maximum_{0};
    bool maxValueAlignment_;
    std::unique_ptr<MmcBlobAllocator> allocator_;
    std::unordered_map<uint64_t, MmcMemBlobPtr> blobs_;
    uint64_t nextHandle_ = 0;
};
} // namespace
} // namespace ock::mmc

PYBIND11_MODULE(_allocator_sim_native, module)
{
    using ock::mmc::Allocator;
    py::class_<Allocator>(module, "Allocator")
        .def(py::init<uint64_t, bool>(), py::arg("capacity"), py::arg("max_value_alignment") = false)
        .def("alloc", &Allocator::Alloc)
        .def("free", &Allocator::Free)
        .def("blob_info", &Allocator::BlobInfo)
        .def_static("align_size", &Allocator::AlignSize)
        .def("required_size", &Allocator::RequiredSize)
        .def("snapshot", &Allocator::Snapshot, py::arg("include_ranges") = true);
}
