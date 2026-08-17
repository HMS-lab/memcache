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

# =============================================================================
# collect_logs.sh - One-click log collection for troubleshooting
# =============================================================================
# Collects environment info and runtime logs into a single tar.gz archive:
#   1. Environment: kernel/OS/HDK/CANN/memcache/memfabric/vllm_ascend versions
#   2. Logs:
#      - ptracer/telemetry: /var/log/memfabric_hybrid/
#      - meta service:      /var/log/memcache_hybrid/
#      - Ascend plog:       /root/ascend/log/debug/plog
#      - kernel ring buf:   dmesg
#
# Usage:
#   bash collect_logs.sh                              # collect all, output to current dir
#   bash collect_logs.sh --output-dir=/data/logs      # specify output directory
#   bash collect_logs.sh --no-plog                    # skip plog (can be large)
#   bash collect_logs.sh --no-dmesg                   # skip dmesg
#   bash collect_logs.sh --max-size=4096              # per-dir size limit in MB
#
# NOTE: Some log paths (plog, dmesg) may require root privileges to read.
#       Review the archive for sensitive data (paths, hostnames) before sharing.
# =============================================================================

set -uo pipefail

# ---- Constants --------------------------------------------------------------
readonly SCRIPT_NAME="$(basename "$0")"
readonly TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
readonly DEFAULT_OUTPUT_DIR="."

# Log source directories (issue #228).
readonly MEMFABRIC_LOG_DIR="/var/log/memfabric_hybrid"
readonly MEMCACHE_LOG_DIR="/var/log/memcache_hybrid"
readonly PLOG_DIR="/root/ascend/log/debug/plog"

# Version / environment info file locations.
readonly MEMCACHE_INSTALL_DIR="/usr/local/memcache_hybrid"
readonly ASCEND_DRIVER_INFO="/usr/local/Ascend/driver/version.info"
readonly ASCEND_INSTALL_INFO="/etc/ascend_install.info"
readonly ASCEND_TOOLKIT_DIR="/usr/local/Ascend/ascend-toolkit/latest"

# Default per-directory size limit (MB). Directories larger than this are skipped.
readonly DEFAULT_MAX_SIZE_MB=2048

# Archive top-level folder name.
readonly ARCHIVE_PREFIX="memcache_log_dump"

# ---- Options (mutable) -----------------------------------------------------
OUTPUT_DIR="${DEFAULT_OUTPUT_DIR}"
COLLECT_PLOG="y"
COLLECT_DMESG="y"
MAX_SIZE_MB="${DEFAULT_MAX_SIZE_MB}"

# ---- Helpers ---------------------------------------------------------------
print_info()  { echo "[INFO]  $*"; }
print_warn()  { echo -e "\e[33m[WARN]\e[0m  $*"; }
print_error() { echo -e "\e[31m[ERROR]\e[0m $*"; }
print_ok()    { echo -e "\e[32m[OK]\e[0m    $*"; }

print_help() {
    cat <<EOF
Usage: bash ${SCRIPT_NAME} [OPTIONS]

One-click collection of MemCache environment info and logs for troubleshooting.

Options:
  --output-dir=<dir>   Output directory for the tar.gz archive (default: current dir)
  --no-plog            Skip Ascend plog collection (useful when plog is huge)
  --no-dmesg           Skip kernel dmesg collection
  --max-size=<MB>      Max size (MB) per log directory, skip if larger (default: ${DEFAULT_MAX_SIZE_MB})
  --help, -h           Show this help message

Collected items:
  Environment:
    - kernel version, OS version
    - HDK/driver version, CANN version
    - memcache, memfabric, vllm_ascend versions
    - NPU device info, running processes, disk/memory usage
  Logs:
    - ptracer/telemetry:  ${MEMFABRIC_LOG_DIR}/
    - meta service:       ${MEMCACHE_LOG_DIR}/
    - Ascend plog:         ${PLOG_DIR}/
    - kernel ring buffer:  dmesg

Output:
  <output-dir>/${ARCHIVE_PREFIX}_${TIMESTAMP}.tar.gz
EOF
}

parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
        --output-dir=*)
            OUTPUT_DIR="${1#*=}"
            shift
            ;;
        --no-plog)
            COLLECT_PLOG="n"
            shift
            ;;
        --no-dmesg)
            COLLECT_DMESG="n"
            shift
            ;;
        --max-size=*)
            local val="${1#*=}"
            if ! [[ "$val" =~ ^[0-9]+$ ]] || [[ "$val" -le 0 ]]; then
                print_error "invalid --max-size value: '$val', must be a positive integer"
                exit 1
            fi
            MAX_SIZE_MB="$val"
            shift
            ;;
        --help|-h)
            print_help
            exit 0
            ;;
        *)
            print_error "unknown option: $1"
            print_help
            exit 1
            ;;
        esac
    done
}

# Echo a command's stdout+stderr, swallowing non-zero exit status.
# Usage: run_or_skip "description" command [args...]
run_or_skip() {
    local desc="$1"; shift
    local out
    if out=$("$@" 2>&1); then
        echo "$out"
    else
        echo "$desc: command failed or not available"
    fi
}

# Print version info for a Python package by reading the VERSION file
# from site-packages. Falls back to pip3 show if the package or VERSION
# file is not found.
# Usage: print_pkg_version <package_name>
print_pkg_version() {
    local pkg="$1"
    local pkg_dir
    pkg_dir=$(python3 -c "import ${pkg}, os; print(os.path.dirname(${pkg}.__file__))" 2>/dev/null)
    if [[ -n "$pkg_dir" && -f "${pkg_dir}/VERSION" ]]; then
        echo "[site-packages: ${pkg_dir}/VERSION]"
        cat "${pkg_dir}/VERSION"
    else
        if [[ -n "$pkg_dir" ]]; then
            echo "VERSION not found in package dir: ${pkg_dir}"
        fi
        echo "[pip3 show ${pkg}]"
        run_or_skip "pip3 show" pip3 show "$pkg" 2>/dev/null
    fi
}

# ---- Environment info collection -------------------------------------------
collect_env_info() {
    local out_file="$1"
    print_info "collecting environment info ..."
    {
        echo "========================================================"
        echo " MemCache Log Dump - Environment Info"
        echo " Collected at: $(date '+%Y-%m-%d %H:%M:%S %Z')"
        echo " Hostname:     $(hostname 2>/dev/null || echo N/A)"
        echo " Collector:    $(whoami 2>/dev/null || echo N/A)"
        echo "========================================================"
        echo ""

        echo "===== Kernel Version ====="
        uname -a 2>/dev/null || echo "uname failed"
        echo ""

        echo "===== OS Version ====="
        if [[ -f /etc/os-release ]]; then
            cat /etc/os-release
        else
            run_or_skip "lsb_release" lsb_release -a 2>/dev/null
        fi
        echo ""

        echo "===== HDK / Driver Version ====="
        if [[ -f "${ASCEND_DRIVER_INFO}" ]]; then
            echo "[${ASCEND_DRIVER_INFO}]"
            cat "${ASCEND_DRIVER_INFO}"
        else
            echo "driver version.info not found at ${ASCEND_DRIVER_INFO}"
        fi
        echo ""
        if [[ -f "${ASCEND_INSTALL_INFO}" ]]; then
            echo "[${ASCEND_INSTALL_INFO}]"
            cat "${ASCEND_INSTALL_INFO}"
        fi
        echo ""

        echo "===== NPU Device Info ====="
        if command -v npu-smi &>/dev/null; then
            npu-smi info 2>/dev/null || echo "npu-smi info failed"
        else
            echo "npu-smi not found in PATH"
        fi
        echo ""

        echo "===== CANN Version ====="
        local cann_path="${ASCEND_HOME_PATH:-${ASCEND_TOOLKIT_DIR}}"
        if [[ -f "${cann_path}/version.info" ]]; then
            echo "[${cann_path}/version.info]"
            cat "${cann_path}/version.info"
        elif [[ -f "${ASCEND_TOOLKIT_DIR}/version.info" ]]; then
            echo "[${ASCEND_TOOLKIT_DIR}/version.info]"
            cat "${ASCEND_TOOLKIT_DIR}/version.info"
        else
            echo "CANN version.info not found (checked ASCEND_HOME_PATH=${cann_path})"
        fi
        echo ""

        echo "===== MemCache Version ====="
        local mc_ver_file="${MEMCACHE_INSTALL_DIR}/latest/version.info"
        if [[ -f "${mc_ver_file}" ]]; then
            echo "[install dir: ${mc_ver_file}]"
            cat "${mc_ver_file}"
        else
            echo "version.info not found at ${mc_ver_file}"
        fi
        echo ""
        print_pkg_version "memcache_hybrid"
        echo ""

        echo "===== MemFabric Version ====="
        print_pkg_version "memfabric_hybrid"
        echo ""

        echo "===== vllm_ascend Version ====="
        echo "[pip3 show vllm_ascend]"
        run_or_skip "pip3 show" pip3 show vllm_ascend 2>/dev/null
        echo ""

        echo "===== MemCache Related Processes ====="
        local procs
        procs=$(pgrep -fa "mmc_meta_service|memcache_hybrid|memfabric" 2>/dev/null || true)
        if [[ -n "$procs" ]]; then
            echo "$procs"
        else
            echo "no matching processes found"
        fi
        echo ""

        echo "===== MemCache Environment Variables ====="
        local env_keys=(MMC_META_CONFIG_PATH MMC_LOCAL_CONFIG_PATH MEMCACHE_HYBRID_HOME_PATH \
                        LD_LIBRARY_PATH ASCEND_HOME_PATH PYTHON_HOME)
        for k in "${env_keys[@]}"; do
            local v="${!k:-}"
            if [[ -n "$v" ]]; then
                printf '%-30s = %s\n' "$k" "$v"
            else
                printf '%-30s = (unset)\n' "$k"
            fi
        done
        echo ""

        echo "===== Disk Usage ====="
        df -h 2>/dev/null || echo "df failed"
        echo ""

        echo "===== Memory ====="
        free -h 2>/dev/null || echo "free not available"
        echo ""

        echo "===== Network Interfaces ====="
        ip -brief addr 2>/dev/null || ifconfig -a 2>/dev/null || echo "network tools not available"
        echo ""

    } > "$out_file" 2>&1
    print_ok "environment info -> $(basename "$out_file")"
}

# ---- Config file collection ------------------------------------------------
collect_config_files() {
    local dest_dir="$1"
    mkdir -p "$dest_dir"

    local meta_cfg="${MMC_META_CONFIG_PATH:-${MEMCACHE_INSTALL_DIR}/latest/config/mmc-meta.conf}"
    local local_cfg="${MMC_LOCAL_CONFIG_PATH:-${MEMCACHE_INSTALL_DIR}/latest/config/mmc-local.conf}"

    for cfg in "$meta_cfg" "$local_cfg"; do
        if [[ -f "$cfg" ]]; then
            cp -a "$cfg" "$dest_dir/" 2>/dev/null && print_ok "config: $(basename "$cfg")"
        fi
    done
}

# ---- Log directory collection ----------------------------------------------
# Copy a log directory into staging if it exists and is within size limit.
# Usage: collect_log_dir <source_dir> <dest_dir> <label>
collect_log_dir() {
    local src_dir="$1"
    local dest_dir="$2"
    local label="$3"

    if [[ ! -d "$src_dir" ]]; then
        print_warn "skip ${label}: directory not found (${src_dir})"
        return 0
    fi

    local size_kb
    size_kb=$(du -sk "$src_dir" 2>/dev/null | awk '{print $1}')
    if [[ -z "$size_kb" ]]; then
        print_warn "skip ${label}: cannot determine size of ${src_dir}"
        return 0
    fi

    local size_mb=$(( size_kb / 1024 ))
    if [[ "$size_mb" -gt "$MAX_SIZE_MB" ]]; then
        print_warn "skip ${label}: ${size_mb} MB exceeds limit ${MAX_SIZE_MB} MB (${src_dir})"
        print_warn "  increase --max-size or skip this source to proceed"
        return 0
    fi

    mkdir -p "$dest_dir"
    if cp -a "$src_dir/." "$dest_dir/" 2>/dev/null; then
        print_ok "${label} (${size_mb} MB) -> ${dest_dir}"
    else
        print_warn "partial copy for ${label} (some files may be unreadable)"
    fi
}

# ---- dmesg collection -------------------------------------------------------
collect_dmesg() {
    local out_file="$1"
    {
        echo "=== dmesg (kernel ring buffer) ==="
        echo "Collected at: $(date '+%Y-%m-%d %H:%M:%S %Z')"
        echo ""
        if dmesg 2>/dev/null; then
            :
        elif command -v journalctl &>/dev/null; then
            echo "[dmesg unavailable, falling back to journalctl -k]"
            journalctl -k --no-pager 2>/dev/null || echo "journalctl also failed"
        else
            echo "dmesg not available (may require root privileges)"
        fi
    } > "$out_file" 2>&1
    print_ok "dmesg -> $(basename "$out_file")"
}

# ---- Main -------------------------------------------------------------------
main() {
    parse_args "$@"

    if [[ ! -d "$OUTPUT_DIR" ]]; then
        print_error "output directory does not exist: $OUTPUT_DIR"
        exit 1
    fi

    local staging_dir
    staging_dir="${OUTPUT_DIR}/${ARCHIVE_PREFIX}_${TIMESTAMP}"
    rm -rf "$staging_dir"
    mkdir -p "$staging_dir/logs"
    chmod 750 "$staging_dir"

    print_info "staging directory: $staging_dir"
    print_info "output archive will be: ${staging_dir}.tar.gz"
    echo ""

    # 1. Environment info
    collect_env_info "${staging_dir}/env_info.txt"

    # 2. Config files (bonus: useful for troubleshooting, contains only paths)
    collect_config_files "${staging_dir}/config"

    # 3. Log directories
    echo ""
    collect_log_dir "$MEMFABRIC_LOG_DIR" "${staging_dir}/logs/memfabric_hybrid" "memfabric_hybrid"
    collect_log_dir "$MEMCACHE_LOG_DIR"  "${staging_dir}/logs/memcache_hybrid"  "memcache_hybrid"

    if [[ "$COLLECT_PLOG" == "y" ]]; then
        collect_log_dir "$PLOG_DIR" "${staging_dir}/logs/plog" "plog"
    else
        print_info "skip plog (--no-plog)"
    fi

    # 4. dmesg
    echo ""
    if [[ "$COLLECT_DMESG" == "y" ]]; then
        collect_dmesg "${staging_dir}/logs/dmesg.log"
    else
        print_info "skip dmesg (--no-dmesg)"
    fi

    # 5. Write a manifest with collection summary
    local manifest="${staging_dir}/MANIFEST.txt"
    {
        echo "MemCache Log Dump Manifest"
        echo "Generated:   $(date '+%Y-%m-%d %H:%M:%S %Z')"
        echo "Hostname:     $(hostname 2>/dev/null || echo N/A)"
        echo "Staging dir:  $staging_dir"
        echo ""
        echo "Contents:"
        (cd "$staging_dir" && find . -maxdepth 2 \( -type f -o -type d \) | sort)
        echo ""
        echo "Total size:"
        du -sh "$staging_dir" 2>/dev/null || echo "unknown"
    } > "$manifest" 2>&1

    # 6. Create tar.gz archive
    echo ""
    print_info "creating archive ..."
    local archive="${staging_dir}.tar.gz"
    if tar czf "$archive" -C "$OUTPUT_DIR" "$(basename "$staging_dir")" 2>/dev/null; then
        chmod 640 "$archive"
        print_ok "archive created: $archive"
        local arch_size
        arch_size=$(du -sh "$archive" 2>/dev/null | awk '{print $1}')
        print_info "archive size: ${arch_size}"
    else
        print_error "failed to create archive"
        exit 1
    fi

    # 7. Cleanup staging
    rm -rf "$staging_dir"

    echo ""
    print_ok "done. share this archive with support: $archive"
}

main "$@"
