#!/bin/bash
# Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
# MemCache_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.

set -e
readonly ROOT_PATH=$(dirname $(readlink -f "$0"))
CURRENT_DIR=$(pwd)

BUILD_MODE="RELEASE"
BUILD_PYTHON="ON"
BUILD_TEST="OFF"
INCREMENTAL="OFF"
BUILD_UBSIO="ON"
BUILD_CPP_EXAMPLE="OFF"
BUILD_PYTHON_WHL_TYPE="release"

show_help() {
    echo "Usage: $0 [options]"
    echo "Options:"
    echo "  --build_mode <mode>     Set build mode (RELEASE/DEBUG/ASAN), default: RELEASE"
    echo "  --build_test <ON/OFF>   Enable/disable package test utilities, default: OFF"
    echo "  --incremental           Enable incremental build (skip clean), default: OFF"
    echo "  --build_ubsio <ON/OFF>  Enable/disable build and package ubs-io (SSD backend), default: ON"
    echo "  --build_cpp_example <ON/OFF> Enable/disable in-tree C++ examples, default: OFF"
    echo "  --build_python_whl_type <release/daily> Python release/daily whl build, default: release"
    echo "  --help                  Show this help message"
    echo ""
    echo "Example:"
    echo "  $0 --build_mode DEBUG --incremental"
    echo ""
}

while [[ "$#" -gt 0 ]]; do
    case "$1" in
        --build_mode)
            BUILD_MODE="$2"
            shift 2
            ;;
        --build_test)
            BUILD_TEST="$2"
            shift 2
            ;;
        --incremental)
            INCREMENTAL="ON"
            shift 1
            ;;
        --build_ubsio)
            BUILD_UBSIO="$2"
            shift 2
            ;;
        --build_cpp_example)
            BUILD_CPP_EXAMPLE="$2"
            shift 2
            ;;
        --build_python_whl_type)
            BUILD_PYTHON_WHL_TYPE="$2"
            shift 2
            ;;
        --help)
            show_help
            exit 0
            ;;
        *)
            echo "Unknown argument: $1"
            echo ""
            show_help
            exit 1
            ;;
    esac
done

echo "BUILD_MODE: $BUILD_MODE"
echo "BUILD_PYTHON: $BUILD_PYTHON"
echo "BUILD_UBSIO: $BUILD_UBSIO"
echo "BUILD_CPP_EXAMPLE: $BUILD_CPP_EXAMPLE"
echo "BUILD_PYTHON_WHL_TYPE: $BUILD_PYTHON_WHL_TYPE"

cd "${ROOT_PATH}"

bash build.sh "${BUILD_MODE}" OFF OFF "${BUILD_PYTHON}" ON "${INCREMENTAL}" "${BUILD_UBSIO}" "${BUILD_CPP_EXAMPLE}" "${BUILD_PYTHON_WHL_TYPE}"

bash run_pkg_maker/make_run.sh "${BUILD_TEST}" "${BUILD_UBSIO}"

cd "${CURRENT_DIR}"
