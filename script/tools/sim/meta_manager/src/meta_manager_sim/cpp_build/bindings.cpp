/* Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 * Licensed under Mulan PSL v2. */
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include "manager_adapter.h"
namespace py = pybind11;
PYBIND11_MODULE(_meta_manager_native, module)
{
    using namespace ock::mmc;
    py::enum_<BlobState>(module, "BlobState").value("ALLOCATED", ALLOCATED).value("READABLE", READABLE);
    py::class_<MmcMemBlobDesc>(module, "Blob")
        .def_readonly("gva", &MmcMemBlobDesc::gva_)
        .def_readonly("rank", &MmcMemBlobDesc::rank_)
        .def_readonly("media", &MmcMemBlobDesc::mediaType_)
        .def_readonly("size", &MmcMemBlobDesc::size_)
        .def_readonly("reserved", &MmcMemBlobDesc::allocSize_)
        .def_readonly("state", &MmcMemBlobDesc::state_)
        .def_readonly("remaining_lease_ttl_ms", &MmcMemBlobDesc::leaseTimeoutTtlMs_);
    py::class_<MmcMemMetaDesc>(module, "Descriptor")
        .def_readonly("blobs", &MmcMemMetaDesc::blobs_)
        .def_readonly("num_blobs", &MmcMemMetaDesc::numBlobs_)
        .def_readonly("size", &MmcMemMetaDesc::size_);
    py::class_<OperationResult>(module, "OperationResult")
        .def_readonly("code", &OperationResult::code)
        .def_readonly("operation_id", &OperationResult::operationId)
        .def_readonly("elapsed_ns", &OperationResult::elapsedNs)
        .def_readonly("descriptor", &OperationResult::descriptor);
    py::enum_<BlobActionResult>(module, "Action").value("WRITE_OK", MMC_WRITE_OK).value("READ_FINISH", MMC_READ_FINISH);
    py::class_<Manager>(module, "Manager")
        .def(py::init<uint64_t, uint16_t, uint16_t, uint64_t, bool, const std::string &, uint32_t>())
        .def("alloc", &Manager::Alloc, py::call_guard<py::gil_scoped_release>())
        .def("get", &Manager::Get, py::call_guard<py::gil_scoped_release>())
        .def("update", &Manager::Update, py::call_guard<py::gil_scoped_release>())
        .def("remove", &Manager::Remove, py::call_guard<py::gil_scoped_release>())
        .def("check_and_evict", &Manager::CheckAndEvict, py::call_guard<py::gil_scoped_release>())
        .def("wait_for_cleanup", &Manager::WaitForCleanup, py::call_guard<py::gil_scoped_release>())
        .def("segment_info", &Manager::SegmentInfo)
        .def("key_count", &Manager::KeyCount)
        .def("keys", &Manager::Keys)
        .def_property_readonly("peak_calls", &Manager::PeakCalls)
        .def_property_readonly("reclaimed", &Manager::Reclaimed)
        .def("close", &Manager::Close, py::call_guard<py::gil_scoped_release>());
    module.attr("READABLE") = int(READABLE);
    module.attr("ALLOCATED") = int(ALLOCATED);
    module.attr("OK") = int(MMC_OK);
    module.attr("MISSING") = int(MMC_UNMATCHED_KEY);
    module.attr("DUPLICATE") = int(MMC_DUPLICATED_OBJECT);
    module.attr("ALLOCATION_FAILED") = int(MMC_ERROR);
}
