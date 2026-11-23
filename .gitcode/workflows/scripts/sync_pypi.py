#!/usr/bin/env python3
# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# MemCache_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.

"""Synchronize locally built wheels to the Ascend PyPI repository."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
from urllib.parse import quote, urlsplit


PUBLIC = "https://ascend.devcloud.huaweicloud.com/pypi/"
DEFAULT_ROOT = Path(__file__).resolve().parents[3] / "BeiMing_output"
COMPONENTS = ("memcache-hybrid", "memfabric-hybrid")
MANAGEMENT_URL_ENV = "ASCEND_DEVCLOUD_PYPI_MANAGEMENT_URL"
USER_ENV = "ASCEND_DEVCLOUD_PYPI_USERNAME"
PASSWORD_ENV = "ASCEND_DEVCLOUD_PYPI_PASSWORD"


class SyncError(Exception):
    pass


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def management_url():
    value = os.environ.get(MANAGEMENT_URL_ENV)
    if not value:
        raise SyncError(f"Configure the GitCode Actions variable {MANAGEMENT_URL_ENV}.")
    try:
        parsed = urlsplit(value)
    except ValueError:
        parsed = None
    if (
        parsed is None
        or parsed.scheme != "https"
        or not parsed.hostname
        or parsed.username is not None
        or re.search(r'[\s\x00-\x1f\x7f"\\?#]', value)
    ):
        raise SyncError(f"{MANAGEMENT_URL_ENV} must be an HTTPS repository URL without credentials, query parameters, or a fragment.")
    return value.rstrip("/") + "/"


def curl_config(username, password):
    value = username + ":" + password
    if any(c in value for c in "\r\n\x00") or ":" in username:
        raise SyncError("The username or password contains unsupported characters.")
    # curl config is passed through stdin, never in the command line or a file.
    return 'user = "' + value.replace("\\", "\\\\").replace('"', '\\"') + '"\n'


def make_url(base, relative):
    return base + quote(relative.as_posix(), safe="/")


def curl_base(curl):
    return [curl, "--disable", "--silent", "--show-error", "--connect-timeout", "30", "--max-time", "900"]


def child_environment():
    return {key: value for key, value in os.environ.items() if key not in (MANAGEMENT_URL_ENV, USER_ENV, PASSWORD_ENV)}


def management_request(curl, auth, url, upload=None):
    args = curl_base(curl) + [
        "--config",
        "-",
        "--proto",
        "=https",
        "--globoff",
        "--write-out",
        "\nCODEX_HTTP_STATUS:%{http_code}",
    ]
    if upload is None:
        args.append("--head")
    else:
        args += ["--upload-file", str(upload)]
    config = f'{auth}url = "{url}"\n'
    result = subprocess.run(
        args, input=config, text=True, capture_output=True, encoding="utf-8", errors="replace", env=child_environment()
    )
    if result.returncode:
        raise SyncError(f"Management repository request failed (curl {result.returncode}).")
    body, marker, status = result.stdout.rpartition("\nCODEX_HTTP_STATUS:")
    if not marker or not status.isdigit():
        raise SyncError("The management repository returned an unrecognized status.")
    return int(status), body


def remote_checksum(headers):
    # Only inspect the last HTTP response, after a possible proxy CONNECT.
    parts = re.split(r"(?im)^HTTP/\S+\s+\d+[^\r\n]*\r?\n", headers)
    for line in parts[-1].splitlines():
        name, sep, value = line.partition(":")
        if sep and name.lower() == "x-checksum-sha256":
            return value.strip().lower()
    return None


def verify_public(curl, url, expected):
    args = curl_base(curl) + ["--fail", "--location", "--proto", "=https", "--proto-redir", "=https", url]
    digest = hashlib.sha256()
    with tempfile.TemporaryFile() as error_file:
        with subprocess.Popen(args, stdout=subprocess.PIPE, stderr=error_file, env=child_environment()) as proc:
            for block in iter(lambda: proc.stdout.read(1024 * 1024), b""):
                digest.update(block)
            code = proc.wait()
        error_file.seek(0)
        error = error_file.read().decode("utf-8", "replace").strip()
    if code:
        raise SyncError(f"Download from the public URL failed (curl {code}): {error}")
    if digest.hexdigest() != expected:
        raise SyncError("The SHA-256 of the public download does not match the local file.")


def main():
    parser = argparse.ArgumentParser(description="Check and sync wheels built by the current job to Ascend PyPI.")
    parser.add_argument("--root", type=Path, default=DEFAULT_ROOT, help="Directory containing wheels archived to OBS by this build")
    parser.add_argument("--component", choices=COMPONENTS, required=True, help="PyPI package name for the current repository")
    parser.add_argument("--version", required=True, help="Base version from the VERSION file, e.g. 1.5.0")
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--build", type=str.lower, help="Internal beta build number, e.g. b013")
    mode.add_argument("--nightly", action="store_true", help="Sync development packages generated by the daily build")
    parser.add_argument("--apply", action="store_true", help="Upload missing files and verify public downloads; check only by default")
    args = parser.parse_args()
    if not re.fullmatch(r"[0-9]+(?:\.[0-9]+)*", args.version):
        raise SyncError(f"Invalid base version: {args.version!r}")
    if args.nightly:
        destination = Path("nightly") / args.component
        version_pattern = re.escape(args.version) + r"\.dev[0-9]+"
    else:
        if not re.fullmatch(r"b[0-9]+", args.build):
            raise SyncError(f"Invalid internal beta build number: {args.build!r}")
        destination = Path(args.component) / args.version
        version_pattern = re.escape(args.version + "+" + args.build)

    distribution = args.component.replace("-", "_")
    wheel_prefix = re.compile(rf"{distribution}-{version_pattern}-")
    root = args.root.resolve()
    files = sorted(
        path for path in root.glob(f"{distribution}-*.whl") if path.is_file() and wheel_prefix.match(path.name)
    )
    if not files:
        raise SyncError(f"In {root}, no wheels found for {args.component} {args.version}; destination: {destination}.")

    management = management_url()
    username = os.environ.get(USER_ENV)
    password = os.environ.get(PASSWORD_ENV)
    if not username or not password:
        raise SyncError(f"Configure the GitCode Actions secrets {USER_ENV} and {PASSWORD_ENV}.")
    curl = shutil.which("curl")
    if not curl:
        raise SyncError("curl was not found. Install it or add it to PATH.")
    auth = curl_config(username, password)
    pending = []
    conflicts = []
    print(f"Checking {len(files)} files ({'upload mode' if args.apply else 'check-only mode'}).", flush=True)
    for path in files:
        rel = destination / path.name
        checksum = sha256(path)
        status, headers = management_request(curl, auth, make_url(management, rel))
        if status == 404:
            state = "pending upload"
            pending.append((path, rel, checksum))
        elif status == 200:
            remote = remote_checksum(headers)
            if remote != checksum:
                conflicts.append(rel)
                state = "conflict or missing remote SHA-256"
            else:
                state = "already exists with matching checksum"
        else:
            raise SyncError(f"Checking {rel.as_posix()} returned HTTP {status}; stopping.")
        print(f"{state}: {rel.as_posix()}", flush=True)
    if conflicts:
        raise SyncError(f"Found {len(conflicts)} conflicting or unverifiable files; no files were uploaded.")
    if not args.apply:
        print(f"Check complete: {len(pending)} files pending upload. Add --apply to upload and verify public downloads.")
        return
    for path, rel, checksum in pending:
        status, body = management_request(curl, auth, make_url(management, rel), upload=path)
        if status not in (200, 201):
            raise SyncError(f"Uploading {rel.as_posix()} returned HTTP {status}; check the destination state before retrying.")
        try:
            remote = json.loads(body).get("checksums", {}).get("sha256", "").lower()
        except (ValueError, AttributeError):
            remote = ""
        if remote != checksum:
            raise SyncError(f"Management repository SHA-256 is missing or does not match after uploading {rel.as_posix()}.")
        print(f"Uploaded and verified: {rel.as_posix()}", flush=True)
    for path in files:
        rel = destination / path.name
        verify_public(curl, make_url(PUBLIC, rel), sha256(path))
        print(f"Public download verified: {rel.as_posix()}", flush=True)
    print(f"Complete: {len(pending)} new uploads; {len(files)} public downloads verified.")


if __name__ == "__main__":
    try:
        main()
    except (SyncError, OSError) as exc:
        print(f"Error: {exc}", file=sys.stderr)
        sys.exit(1)
