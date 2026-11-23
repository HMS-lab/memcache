/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * MemCache_Hybrid is licensed under Mulan PSL v2.
 */

#include <atomic>
#include <cstdint>
#include <memory>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "mmc_blob_allocator.h"
#include "ptracer.h"

extern "C" {
ptracer g_mmc_tracer{};
}

namespace py = pybind11;
using namespace ock::mmc;

namespace {

constexpr uint64_t SIM_BASE_ADDR = 0x100000000ULL;

void CheckSize(uint64_t size)
{
    if (size == 0 || size > std::numeric_limits<uint64_t>::max() - (SLAB_MIN_ALIGN_SIZE - 1)) {
        throw std::invalid_argument("Size must be positive and must not overflow alignment.");
    }
}

class NativeAllocatorSim {
public:
    NativeAllocatorSim(uint64_t capacity, const std::string &mode)
        : capacity_(capacity), mode_(mode), maxAlignedSize_(0)
    {
        CheckSize(capacity);
        MmcOutLogger::Instance().SetLogLevel(ERROR_LEVEL);
        if (mode == "slab") {
            isSlab_ = true;
            slabAlloc_ = new MmcSlabAllocator(0, MEDIA_DRAM, SIM_BASE_ADDR, capacity, &maxAlignedSize_);
            alloc_ = MmcRef<MmcBlobAllocator>(slabAlloc_);
        } else if (mode == "max-value-align") {
            isSlab_ = false;
            alloc_ = MmcMakeRef<MmcBlobAllocator>(0, MEDIA_DRAM, SIM_BASE_ADDR, capacity, true, &maxAlignedSize_);
        } else if (mode == "4k-align") {
            isSlab_ = false;
            alloc_ = MmcMakeRef<MmcBlobAllocator>(0, MEDIA_DRAM, SIM_BASE_ADDR, capacity, false, &maxAlignedSize_);
        } else {
            throw std::invalid_argument("Unsupported allocator mode: " + mode);
        }
        alloc_->Start();
    }

    ~NativeAllocatorSim()
    {
        if (alloc_ != nullptr) {
            alloc_->Stop();
        }
    }

    bool can_alloc(uint64_t size)
    {
        CheckSize(size);
        return alloc_->CanAlloc(size);
    }

    py::object alloc(uint64_t size)
    {
        CheckSize(size);
        MmcMemBlobPtr blob = alloc_->Alloc(size);
        if (blob == nullptr) {
            return py::none();
        }
        uint64_t handle = nextHandle_++;
        uint64_t gva = blob->Gva();
        uint64_t allocSize = blob->AllocSize();
        liveBlobs_.emplace(handle, std::move(blob));
        return py::make_tuple(handle, gva, allocSize);
    }

    bool release(uint64_t handle)
    {
        auto it = liveBlobs_.find(handle);
        if (it == liveBlobs_.end()) {
            throw std::invalid_argument("Unknown or already freed handle.");
        }
        Result res = alloc_->Release(it->second);
        if (res != MMC_OK) {
            throw std::runtime_error("Native release failed.");
        }
        liveBlobs_.erase(it);
        return true;
    }

    uint64_t capacity() const
    {
        return capacity_;
    }

    uint64_t allocated_size()
    {
        return alloc_->GetUsageInfo().second;
    }

    uint64_t reserved_size()
    {
        if (isSlab_ && slabAlloc_ != nullptr) {
            return slabAlloc_->GetReservedSize();
        }
        return alloc_->GetUsageInfo().second;
    }

    uint64_t fragmentation_bytes() const
    {
        return alloc_->GetFragmentationBytes();
    }

    uint64_t max_contiguous_free_size()
    {
        return alloc_->GetMaxContiguousFreeSize();
    }

    py::list free_segments()
    {
        py::list result;
        std::vector<SpaceRange> segs = alloc_->GetFreeSegments();
        for (const auto &s : segs) {
            result.append(py::make_tuple(s.offset_, s.size_));
        }
        return result;
    }

    py::dict slab_free_stats()
    {
        py::dict d;
        if (!isSlab_ || slabAlloc_ == nullptr) {
            return d;
        }
        SlabFreeSummary s = slabAlloc_->GetSlabFreeStats();
        d["frame_size"] = s.frameSize;
        d["free_full_frames"] = s.freeFullFrames;
        d["free_full_frame_bytes"] = s.freeFullFrameBytes;
        d["free_tail_frame_bytes"] = s.freeTailFrameBytes;

        uint64_t totalTailWaste = 0;
        uint64_t totalFreeSlots = 0;
        py::list poolList;
        for (const auto &p : s.pools) {
            py::dict pd;
            pd["slot_size"] = p.slotSize;
            pd["active_frames"] = p.activeFrames;
            pd["allocated_slots"] = p.allocatedSlots;
            pd["free_slots"] = p.freeSlots;
            pd["free_bytes"] = p.freeBytes;
            pd["tail_waste_bytes"] = p.tailWasteBytes;
            totalTailWaste += p.tailWasteBytes;
            totalFreeSlots += p.freeSlots;
            poolList.append(pd);
        }
        d["pools"] = poolList;
        d["total_tail_waste_bytes"] = totalTailWaste;
        d["total_free_slots"] = totalFreeSlots;
        return d;
    }

    py::dict snapshot(bool includeSegments = true)
    {
        uint64_t allocBytes = alloc_->GetUsageInfo().second;
        uint64_t internalFrag = alloc_->GetFragmentationBytes();
        uint64_t reqBytes = (allocBytes >= internalFrag) ? (allocBytes - internalFrag) : 0;
        uint64_t largestFree = alloc_->GetMaxContiguousFreeSize();

        py::dict snap;
        snap["capacity"] = capacity_;
        snap["requested"] = reqBytes;
        snap["allocated_slots_bytes"] = allocBytes;
        snap["largest_free_range"] = largestFree;

        if (isSlab_ && slabAlloc_ != nullptr) {
            py::dict slabStats = slab_free_stats();
            uint64_t tailWaste = slabStats["total_tail_waste_bytes"].cast<uint64_t>();
            uint64_t reservedTotal = allocBytes + tailWaste;
            uint64_t freeTotal = (capacity_ >= reservedTotal) ? (capacity_ - reservedTotal) : 0;

            snap["reserved"] = reservedTotal;
            snap["free"] = freeTotal;
            snap["internal_fragmentation"] = internalFrag + tailWaste;
            snap["alignment_waste"] = internalFrag;
            snap["max_value_align_padding"] = 0ULL;
            snap["frame_tail_waste"] = tailWaste;
            snap["free_ranges_count"] =
                slabStats["free_full_frames"].cast<uint64_t>() + slabStats["total_free_slots"].cast<uint64_t>();
            snap["slab_stats"] = slabStats;
            snap["free_ranges"] = py::list();
        } else {
            uint64_t freeTotal = (capacity_ >= allocBytes) ? (capacity_ - allocBytes) : 0;
            snap["reserved"] = allocBytes;
            snap["free"] = freeTotal;
            snap["internal_fragmentation"] = internalFrag;
            snap["alignment_waste"] = (mode_ == "4k-align") ? internalFrag : 0ULL;
            snap["max_value_align_padding"] = (mode_ == "max-value-align") ? internalFrag : 0ULL;
            snap["frame_tail_waste"] = 0ULL;

            if (includeSegments) {
                py::list segs = free_segments();
                snap["free_ranges_count"] = py::len(segs);
                snap["free_ranges"] = segs;
            } else {
                snap["free_ranges_count"] = 0ULL;
                snap["free_ranges"] = py::list();
            }
        }
        return snap;
    }

private:
    uint64_t capacity_ = 0;
    std::string mode_;
    bool isSlab_ = false;
    std::atomic<uint64_t> maxAlignedSize_{0};
    MmcSlabAllocator *slabAlloc_ = nullptr;
    MmcRef<MmcBlobAllocator> alloc_;
    uint64_t nextHandle_ = 1;
    std::unordered_map<uint64_t, MmcMemBlobPtr> liveBlobs_;
};

} // namespace

PYBIND11_MODULE(_allocator_sim_native, m)
{
    m.doc() = "Native C++ binding for MmcBlobAllocator & MmcSlabAllocator simulator";

    py::class_<NativeAllocatorSim>(m, "NativeAllocatorSim")
        .def(py::init<uint64_t, const std::string &>(), py::arg("capacity"), py::arg("mode") = "4k-align")
        .def("can_alloc", &NativeAllocatorSim::can_alloc)
        .def("alloc", &NativeAllocatorSim::alloc)
        .def("release", &NativeAllocatorSim::release)
        .def("capacity", &NativeAllocatorSim::capacity)
        .def("allocated_size", &NativeAllocatorSim::allocated_size)
        .def("reserved_size", &NativeAllocatorSim::reserved_size)
        .def("fragmentation_bytes", &NativeAllocatorSim::fragmentation_bytes)
        .def("max_contiguous_free_size", &NativeAllocatorSim::max_contiguous_free_size)
        .def("free_segments", &NativeAllocatorSim::free_segments)
        .def("slab_free_stats", &NativeAllocatorSim::slab_free_stats)
        .def("snapshot", &NativeAllocatorSim::snapshot, py::arg("include_segments") = true);
}
