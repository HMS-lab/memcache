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
# post_create.sh — MemCache Dev Container bootstrap
# =============================================================================
# This script runs once after the container is created.  It:
#   1. Initializes git submodules (third-party dependencies).
#   2. Installs Python dev / test dependencies.
#   3. Verifies that the build toolchain is functional.
#   4. Runs a quick CMake configure smoke test.
#   5. Installs Go toolchain (for gitleaks hook) and pre-commit hooks.
#
# VS Code's `postCreateCommand` in devcontainer.json invokes this file.
# Git submodule initialisation is performed in step 1 below (runs once
# after container creation; re-run manually if you switch branches).
#
# IMPORTANT — working directory:
#   Dev Containers run postCreateCommand in /workspaces/<repo-name> by default
#   (the mount point of the project root).  This script uses the PROJECT_DIR
#   variable, which resolves to the directory containing this .sh file's parent
#   (i.e. the repo root).  No hardcoded paths.
# =============================================================================

set -euo pipefail

# Resolve project root: the script lives at .devcontainer/post_create.sh,
# so its grandparent is the repo root.  No dependency on $PWD.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

echo "============================================"
echo " MemCache Dev Container — Post-Create Setup"
echo " Project dir: ${PROJECT_DIR}"
echo "============================================"

# ------------------------------------------------------------------
# 1. third party development dependencies
# ------------------------------------------------------------------

# Network hardening for flaky/restricted CI networks — MUST run before any
# submodule clone: force HTTP/1.1 (avoids "curl 16 HTTP2 framing" / "expected
# flush after ref listing" aborts), raise the post buffer, disable the
# slow-transfer timeout, and bypass SSL verification (corp networks often
# MITM https). Reused by the pre-commit step below, so set once here.
export GIT_SSL_NO_VERIFY=1
git config --global http.sslVerify false
git config --global http.version HTTP/1.1
git config --global http.lowSpeedLimit 0
git config --global http.postBuffer 524288000

# Redirect github.com submodule URLs to gitcode.com mirrors via per-URL
# insteadOf — rewriting happens at fetch time, so .gitmodules is left
# untouched. Lets submodule clones avoid flaky/blocked github.com. gitcode
# has no mirror for mockcpp / prometheus-cpp-lite; those stay on github and
# rely on the retries below.
_git_mirror_pairs=(
    "https://github.com/google/googletest.git|https://gitcode.com/gh_mirrors/go/googletest.git"
    "https://github.com/gabime/spdlog.git|https://gitcode.com/gh_mirrors/sp/spdlog.git"
    "https://github.com/nlohmann/json.git|https://gitcode.com/gh_mirrors/js/json.git"
    "https://github.com/msgpack/msgpack-c.git|https://gitcode.com/gh_mirrors/ms/msgpack-c.git"
    "https://github.com/zeromq/libzmq.git|https://gitcode.com/gh_mirrors/li/libzmq.git"
)
for _pair in "${_git_mirror_pairs[@]}"; do
    _src="${_pair%%|*}"
    _dst="${_pair##*|}"
    git config --global "url.${_dst}.insteadOf" "$_src"
done
unset _git_mirror_pairs _pair _src _dst

echo "[1/5] Initializing git submodules …"
# Sync .gitmodules URLs (now gitcode.com mirrors) into .git/config and
# .git/modules remotes — otherwise an existing checkout keeps the cached
# github.com URLs and the update below still hits github.
GIT_SSL_NO_VERIFY=1 git -c protocol.file.allow=always submodule sync 2>/dev/null || true
# Retry: github.com is intermittently unreachable from some networks; a
# transient clone failure should not abort setup. (Most submodules now clone
# from gitcode.com mirrors via .gitmodules; retries cover the residual two.)
SUBMODULE_OK=0
for _attempt in 1 2 3; do
    if GIT_SSL_NO_VERIFY=1 git -c protocol.file.allow=always \
        submodule update --init --recursive 3rdparty/ test/3rdparty/; then
        SUBMODULE_OK=1
        break
    fi
    echo "  ! submodule init attempt $_attempt failed; retrying in 5s …"
    sleep 5
done
if [[ $SUBMODULE_OK -ne 1 ]]; then
    echo "  ! submodule init failed after retries — continuing; build/test may fail later if 3rdparty is missing."
fi

# ------------------------------------------------------------------
# 1. Python development dependencies
# ------------------------------------------------------------------
echo ""
echo "[2/5] Installing Python development dependencies …"
if [ -f "${SCRIPT_DIR}/requirements.txt" ]; then
    pip3 install --no-cache-dir --break-system-packages \
        -r "${SCRIPT_DIR}/requirements.txt"
    echo "  ✓ Python dependencies installed."
else
    echo "  - requirements.txt not found — skipping."
fi

# ------------------------------------------------------------------
# 3. Verify toolchain
# ------------------------------------------------------------------
echo ""
echo "[3/5] Verifying build toolchain …"
echo "  gcc:  $(gcc --version | head -1)"
echo "  g++:  $(g++ --version | head -1)"
echo "  cmake: $(cmake --version | head -1)"
echo "  make: $(make --version | head -1)"
echo "  python: $(python3 --version)"
echo "  pip:  $(pip3 --version | head -1)"
echo "  ✓ Toolchain ready."

# ------------------------------------------------------------------
# 4. (Optional) Quick CMake configure smoke-test
# ------------------------------------------------------------------
echo ""
echo "[4/5] Quick CMake smoke test …"
if [[ -f /usr/local/Ascend/ascend-toolkit/set_env.sh ]]; then
    source /usr/local/Ascend/ascend-toolkit/set_env.sh
else
    echo "WARNING: Ascend toolkit env script not found, cmake may fail"
fi
cd "${PROJECT_DIR}"
cmake -S . -B /tmp/build-smoke \
    -DCMAKE_BUILD_TYPE=DEBUG \
    -DBUILD_TESTS=OFF \
    -DBUILD_PYTHON=OFF \
    -DBUILD_OPEN_ABI=OFF \
    -DENABLE_PTRACER=OFF \
    -DBUILD_UBSIO=OFF \
    -DBUILD_GIT_COMMIT=OFF \

rm -rf /tmp/build-smoke
echo "  ✓ CMake configure passed (smoke test)."

# ------------------------------------------------------------------
# 5. Install Go toolchain and pre-commit hooks
# ------------------------------------------------------------------
# pre-commit's golang language probes https://go.dev/dl/?mode=json to infer the
# latest Go version; that endpoint is SSL-unstable on restricted networks, so
# the gitleaks hook install fails.  A system `go` on PATH makes pre-commit use
# language_version "system" and skip the probe.  GOPROXY=goproxy.cn serves
# gitleaks' deps; GOTOOLCHAIN=local blocks toolchain auto-download from go.dev.
# Best-effort: a download failure is reported but does not abort setup.
echo ""
echo "[5/5] Installing Go toolchain and pre-commit hooks …"
NEEDED_GO_VERSION="1.26.5"
GO_TARBALL="/tmp/go${NEEDED_GO_VERSION}.tar.gz"
CURRENT_GO_VERSION="$(/usr/local/go/bin/go version 2>/dev/null | grep -oP 'go\K[0-9]+\.[0-9]+\.[0-9]+' || true)"

if [ "${CURRENT_GO_VERSION}" = "${NEEDED_GO_VERSION}" ]; then
    echo "  - go already at required version ${NEEDED_GO_VERSION}"
else
    ARCH="$(dpkg --print-architecture)"
    echo "  - installing go${NEEDED_GO_VERSION} linux-${ARCH} (was: ${CURRENT_GO_VERSION:-none}) …"
    rm -rf /usr/local/go
    if curl -fsSL "https://golang.google.cn/dl/go${NEEDED_GO_VERSION}.linux-${ARCH}.tar.gz" -o "${GO_TARBALL}"; then
        tar -xzf "${GO_TARBALL}" -C /usr/local && rm -f "${GO_TARBALL}"
        echo "  - installed: $(/usr/local/go/bin/go version 2>&1 | head -1)"
    else
        rm -f "${GO_TARBALL}"
        echo "  ! go download failed — gitleaks pre-commit hook will not build (check network)."
    fi
fi
if [ -x /usr/local/go/bin/go ]; then
    ln -sf /usr/local/go/bin/go /usr/local/bin/go
    ln -sf /usr/local/go/bin/gofmt /usr/local/bin/gofmt
fi
export GOPROXY="https://goproxy.cn,direct"
export GOTOOLCHAIN="local"

# (git http/SSL network hardening already configured in step 1 above.)

cd "${PROJECT_DIR}"
if [ -f ".pre-commit-config.yaml" ]; then
    export PATH="/usr/local/python3.11.15/bin:$HOME/.local/bin:$PATH"
    pip install --root-user-action=ignore pre-commit
    hash -r
    pre-commit install
    echo "  ✓ pre-commit hooks installed."
    # Pre-warm hook environments (notably gitleaks' Go build) into the
    # persistent pre-commit cache volume so the first commit is instant.
    if ! pre-commit install-hooks; then
        echo "  ! some hook envs failed to pre-warm — will build on first commit."
    else
        echo "  ✓ pre-commit hook environments pre-warmed."
    fi
else
    echo "  - .pre-commit-config.yaml not found — skipping."
fi

echo ""
echo "============================================"
echo " MemCache Dev Container is ready!"
echo "============================================"
