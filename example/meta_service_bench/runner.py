#!/usr/bin/env python3
"""Run bench_meta_service and store the result in MongoDB.

Bench parameters come from the CLI, environment settings from config.yml.
Usage: python3 runner.py [--config config.yml] [-p 8 -t 2 ...]
"""
import argparse
import codecs
import json
import os
import re
import selectors
import signal
import socket
import subprocess
import sys
import time
from datetime import datetime, timezone

import yaml
from pymongo import MongoClient
from pymongo.errors import PyMongoError

# name, flag, type, default, help
PARAMS = [
    ("processes", "-p", int, 4, "Client process count"),
    ("threads", "-t", int, 4, "Threads per process"),
    ("ops", "-n", int, 5000, "Operations per thread"),
    ("keyspace", "-k", int, 20000, "Key space size"),
    ("warmup", "-w", int, 5000, "Warmup key count (Put pre-fill)"),
    ("blobsize", "-s", int, 32768, "Blob size in bytes"),
    ("put_percent", "-r", int, 50, "Put percentage (1-99)"),
    ("batch", "-b", str, "1,32", "Batch size range (min,max)"),
    ("capacity_mb", "-c", int, 16384, "Mock HBM capacity per rank in MB"),
    ("log_file", "-l", str, "bench_meta_service.log", "Log file for MMC library output"),
    ("metrics_interval_ms", "--metrics-interval-ms", int, 1000,
     "Sample /metrics every ms; 0 disables"),
    ("queue_depth", "--queue-depth", int, 1, "Logical operations in flight per thread"),
]


def positive_int(text):
    value = int(text)
    if value < 1:
        raise argparse.ArgumentTypeError("must be at least 1")
    return value


NUMA_NODES_RE = re.compile(r"!?\d+(-\d+)?(,\d+(-\d+)?)*")


def numa_nodes(text):
    """Accept a NUMA node list such as 5, 4,6, 4-7 or !5."""
    if not NUMA_NODES_RE.fullmatch(text):
        raise argparse.ArgumentTypeError("expected e.g. 5, 4,6, 4-7 or !5")
    return text


def parse_args():
    parser = argparse.ArgumentParser(
        description="Run bench_meta_service and store the result in MongoDB.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--config", default="config.yml", help="Path to config file")
    for name, flag, type_, default, help_ in PARAMS:
        parser.add_argument(flag, dest=name, type=type_, default=default, help=help_)
    parser.add_argument("--completion-mode", dest="completion_mode",
                        choices=["poll", "wait"], default="poll",
                        help="How client threads reap completions")
    parser.add_argument("--meta-numa", dest="meta_numa", type=numa_nodes, default=None,
                        help="NUMA nodes for meta_service: 5, 4,6, 4-7 or !5 (default: unbound)")
    parser.add_argument("--worker-numa", dest="worker_numa", type=numa_nodes, default=None,
                        help="NUMA nodes for coordinator + clients, same format (default: unbound)")
    parser.add_argument("--numa-mem", dest="numa_mem", choices=["bind", "preferred", "none"],
                        default="bind", help="NUMA memory policy")
    return parser.parse_args()


PERF_TARGETS = ("meta", "clients", "client0")


def apply_config_options(args, cfg):
    """Pull the profiling and rerun knobs out of the config and onto args."""
    perf = cfg.get("perf", {})
    pyroscope = cfg.get("pyroscope", {})
    bench = cfg.get("bench", {})

    args.perf = bool(perf.get("enabled", False))
    args.perf_freq = int(perf.get("freq", 99))
    args.perf_target = str(perf.get("target", "meta"))
    args.flamegraph_max_stack = int(perf.get("max_stack", 0) or 0)
    args.pyroscope_name = pyroscope.get("name") or None
    args.rerun = int(bench.get("rerun", 1))

    if args.perf_target not in PERF_TARGETS:
        sys.exit(f"config perf.target: {args.perf_target!r} (expected {'|'.join(PERF_TARGETS)})")
    if args.perf_freq < 1:
        sys.exit("config perf.freq must be at least 1")
    if args.flamegraph_max_stack < 0:
        sys.exit("config perf.max_stack must be 0 (full stacks) or more")
    if args.rerun < 1:
        sys.exit("config bench.rerun must be at least 1")
    return args


def tool_enabled(cfg, section):
    """A tool runs only when its config section says enabled: true."""
    return bool(cfg.get(section, {}).get("enabled", False))


def build_command(cfg, args):
    cmd = [cfg["bench"]["binary"]]
    for name, flag, _, _, _ in PARAMS:
        cmd += [flag, str(getattr(args, name))]
    if args.perf or tool_enabled(cfg, "pyroscope"):
        cmd += ["--perf", "--perf-freq", str(args.perf_freq)]
    cmd += ["--perf-target", args.perf_target]
    if args.meta_numa is not None:
        cmd += ["--meta-numa", str(args.meta_numa)]
    if args.worker_numa is not None:
        cmd += ["--worker-numa", str(args.worker_numa)]
    cmd += ["--completion-mode", args.completion_mode, "--numa-mem", args.numa_mem]
    return cmd


def run(cmd, cwd, on_profile=None):
    """Run the command, echoing its output live. Returns (exit_code, output)."""
    chunks = []
    decoder = codecs.getincrementaldecoder("utf-8")(errors="replace")
    control, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    with control, child, selectors.DefaultSelector() as watcher:
        command = list(cmd)
        inherited = ()
        if on_profile is not None:
            command += ["--profile-control-fd", str(child.fileno())]
            inherited = (child.fileno(),)
        with subprocess.Popen(command, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              pass_fds=inherited) as proc:
            child.close()
            watcher.register(proc.stdout, selectors.EVENT_READ, "output")
            if on_profile is not None:
                watcher.register(control, selectors.EVENT_READ, "control")
            try:
                while watcher.get_map():
                    for key, _ in watcher.select():
                        if key.data == "output":
                            data = os.read(proc.stdout.fileno(), 65536)
                            text = decoder.decode(data, final=not data)
                            print(text, end="", flush=True)
                            chunks.append(text)
                            if not data:
                                watcher.unregister(proc.stdout)
                            continue
                        message = control.recv(65536)
                        if not message:
                            watcher.unregister(control)
                            continue
                        try:
                            on_profile(message)
                        except (OSError, RuntimeError, ValueError) as error:
                            print(f"Profiling synchronization failed: {error}", file=sys.stderr)
                            control.sendall(b"!")
                            watcher.unregister(control)
                        else:
                            control.sendall({b"R": b"S", b"D": b"T"}[message[:1]])
            except BaseException:
                proc.kill()
                raise
        return proc.returncode, "".join(chunks)


def number(text):
    return float(text) if "." in text else int(text)


LATENCY_COLUMNS = ("samples", "avg_us", "p50_us", "p95_us", "p99_us", "p999_us", "min_us", "max_us")
LATENCY_ROW_RE = re.compile(r"^(put-alloc|put-update|get)((?:\s+[\d.]+){8})$")
OUTCOME_RE = re.compile(r"^(put-alloc|put-update|get): success=(\d+) fail=(\d+) "
                        r"transport failures=(\d+) timeouts=(\d+)$")


def parse_report(output):
    """Parse the benchmark report, async format first, then the legacy one."""
    if "Benchmark report:" in output:
        return add_run_markers(parse_async_report(output), output)
    return add_run_markers(parse_legacy_report(output), output)


def add_run_markers(results, output):
    """Fields that both report formats share; {} stays {} so the caller can warn."""
    if not results:
        return results
    if match := re.search(r"Metrics samples: (\d+)", output):
        results["metrics_samples"] = int(match.group(1))
    if match := re.search(r"\[meta_service\] Started at (\S+)", output):
        results["meta_service_addr"] = match.group(1)
    if match := re.search(r"meta_service started \(pid=(\d+)\)", output):
        results["meta_service_pid"] = int(match.group(1))
    results["finished"] = "Benchmark finished." in output
    return results


def parse_async_report(output):
    """Parse the queue-depth report: two latency tables plus per-RPC outcomes."""
    _, _, report = output.rpartition("Benchmark report:")
    results = {}
    prefix = None

    for line in report.splitlines():
        line = line.strip()
        if line.startswith("Client-observed RPC round-trip latency"):
            prefix = "rpc"
        elif line.startswith("Submission duration"):
            prefix = "sub"
        elif (match := LATENCY_ROW_RE.match(line)) and prefix:
            op = match.group(1).replace("-", "_")
            for column, value in zip(LATENCY_COLUMNS, match.group(2).split()):
                results[f"{prefix}_{op}_{column}"] = number(value)
        elif match := OUTCOME_RE.match(line):
            op = match.group(1).replace("-", "_")
            keys = ("success", "fail", "transport_failures", "timeouts")
            for key, value in zip(keys, match.groups()[1:]):
                results[f"{op}_{key}"] = int(value)
        elif match := re.match(r"Logical Put: success=(\d+) fail=(\d+)", line):
            results["logical_put_success"] = int(match.group(1))
            results["logical_put_fail"] = int(match.group(2))
        elif match := re.match(r"Total logical ops: (\d+), achieved logical ops/sec: ([\d.]+)", line):
            results["total_logical_ops"] = int(match.group(1))
            results["logical_ops_sec"] = float(match.group(2))
            # Same meaning as the legacy field, kept so existing charts keep working.
            results["throughput_ops_sec"] = float(match.group(2))
        elif match := re.match(r"maximum logical concurrency=(\d+)", line.split(", ")[-1]):
            results["max_logical_concurrency"] = int(match.group(1))
    return results


def parse_legacy_report(output):
    """Parse the old "Benchmark Report" section. Returns {} when it is missing."""
    _, found, report = output.rpartition("Benchmark Report")
    if not found:
        return {}

    results = {"per_process": []}
    columns = None
    for line in report.splitlines():
        line = line.strip()
        cells = [c.strip() for c in line.strip("|").split("|")]

        if line.startswith("| Op"):
            columns = [c.lower().replace("(us)", "_us") for c in cells[1:]]
        elif line.startswith("|") and columns and len(cells) == len(columns) + 1:
            op = cells[0].lower()
            for column, value in zip(columns, cells[1:]):
                results[f"{op}_{column}"] = number(value)
        elif line.startswith("Global P999(us):"):
            for op, value in re.findall(r"(\w+)=([\d.]+)", line):
                results[f"{op.lower()}_p999_us"] = number(value)
        elif match := re.match(r"Total ops: (\d+) \(success: (\d+), failure: (\d+)\)", line):
            keys = ("total_ops", "total_success", "total_failure")
            results.update(zip(keys, map(int, match.groups())))
        elif match := re.match(r"Max duration \(wall\):\s*([\d.]+)", line):
            results["max_duration_s"] = float(match.group(1))
        elif match := re.match(r"Throughput:\s*([\d.]+)", line):
            results["throughput_ops_sec"] = float(match.group(1))
        elif len(cells) == 6 and all(re.fullmatch(r"[\d.]+", c) for c in cells):
            results["per_process"].append({
                "rank": int(cells[0]),
                "pid": int(cells[1]),
                "ops": int(cells[2]),
                "success": int(cells[3]),
                "fail": int(cells[4]),
                "duration_s": float(cells[5]),
            })
    return results


SYMBOL_DSO_RE = re.compile(r"\s+\((?:/|\[)[^)]*\)$")   # trailing " (/path/lib.so)" or " ([kernel])"
SYMBOL_OFFSET_RE = re.compile(r"\+0x[0-9a-fA-F]+$")


def shorten_symbol(symbol, max_length=120):
    """Collapse C++ template arguments and parameter lists: they dwarf the folded file.

    std::_Hashtable<800 chars of template args>::_M_find_before_node(int) -> the same with <> ().
    """
    out = []
    depth = 0
    for char in symbol:
        if char in "<(":
            depth += 1
            if depth == 1:
                out.append(char)
        elif char in ">)":
            if depth == 1:
                out.append(char)
            depth = max(0, depth - 1)
        elif depth == 0:
            out.append(char)
    short = "".join(out)
    return short if len(short) <= max_length else short[:max_length] + "~"


def fold_perf_script(text):
    """Collapse `perf script` output into folded stacks: "comm;caller;callee count"."""
    counts = {}
    comm = None
    stack = []

    def flush():
        if comm and stack:
            key = ";".join([comm] + stack[::-1])
            counts[key] = counts.get(key, 0) + 1

    for line in text.splitlines():
        if not line.strip():
            flush()
            comm, stack = None, []
            continue
        if not line[0].isspace():
            flush()
            comm, stack = line.split()[0], []
            continue
        parts = line.split(None, 1)
        if len(parts) < 2:
            continue
        symbol = SYMBOL_DSO_RE.sub("", parts[1].strip())
        symbol = SYMBOL_OFFSET_RE.sub("", symbol).strip()
        if symbol:
            stack.append(shorten_symbol(symbol).replace(";", ":"))
    flush()
    return "".join(f"{key} {count}\n" for key, count in counts.items())


def send_flamegraph(cfg, args, perf_data, started_at, duration_s):
    """Convert perf.data to folded stacks and POST them to Pyroscope.

    Returns the Pyroscope name on success, None otherwise.
    """
    if not tool_enabled(cfg, "pyroscope"):
        return None
    pyroscope = cfg["pyroscope"]
    if not os.path.exists(perf_data):
        print(f"Warning: {perf_data} not found, flamegraph not sent", file=sys.stderr)
        return None

    script_cmd = ["perf", "script", "-i", perf_data]
    if args.flamegraph_max_stack:
        script_cmd += ["--max-stack", str(args.flamegraph_max_stack)]
    script = subprocess.run(script_cmd, capture_output=True, text=True, errors="replace")
    if script.returncode != 0:
        print(f"Warning: perf script failed: {script.stderr.strip()}", file=sys.stderr)
        return None
    folded = fold_perf_script(script.stdout)
    if not folded:
        print("Warning: no stacks in perf.data, flamegraph not sent", file=sys.stderr)
        return None

    folded_file = f"{os.path.splitext(perf_data)[0]}.folded"
    with open(folded_file, "w") as f:
        f.write(folded)
    print(f"Folded stacks: {len(folded) / (1024 * 1024):.1f} MB in {folded_file}", flush=True)

    name = profile_name(args, started_at)
    return name if upload_folded(cfg, folded_file, name, args.perf_freq, started_at,
                                 duration_s) else None


def profile_name(args, started_at):
    # Pyroscope rejects ':' in an app name with HTTP 500, hence '-' between the time fields.
    return args.pyroscope_name or started_at.astimezone().strftime("%d-%m-%yt%H-%M-%S")


def upload_folded(cfg, folded_file, name, sample_rate, started_at, duration_s):
    """POST a folded-stacks file to Pyroscope. Returns True when it was accepted."""
    start = int(started_at.timestamp())
    url = (f"{cfg['pyroscope']['url'].rstrip('/')}/ingest?name={name}&format=folded"
           f"&sampleRate={sample_rate}&spyName=perf_script"
           f"&from={start}&until={start + max(1, int(duration_s))}")

    upload = subprocess.run(["curl", "-sS", "-g", "--noproxy", "*", "-X", "POST",
                             "-w", "\n%{http_code}", "--data-binary", f"@{folded_file}", url],
                            capture_output=True, text=True, errors="replace")
    if upload.returncode != 0:
        print(f"Warning: upload of {name} failed: {upload.stderr.strip()}", file=sys.stderr)
        return False
    body, _, status = upload.stdout.rpartition("\n")
    if not status.strip().startswith("2"):
        print(f"Warning: Pyroscope rejected {name} (HTTP {status.strip()}): "
              f"{body.strip()[:300]}", file=sys.stderr)
        print(f"         URL: {url}", file=sys.stderr)
        if status.strip() == "413":
            print("         Payload too large: shorten the run, lower perf.freq or set "
                  "perf.max_stack.", file=sys.stderr)
        return False
    print(f"Sent to Pyroscope as {name} ({folded_file})\n         {url}", flush=True)
    return True


OFFCPU_SAMPLE_RATE = 1000000   # offcputime counts microseconds, so 1e6 makes Pyroscope show seconds


def start_offcpu(cfg, perf_data, target_pids):
    """Start off-CPU sampling while workload clients are held at the startup barrier."""
    if not tool_enabled(cfg, "offcpu"):
        return None
    offcpu = cfg["offcpu"]
    path = f"{os.path.splitext(perf_data)[0]}.offcpu.folded"
    command = [offcpu.get("tool", "offcputime-bpfcc"), "-df", "-p", target_pids,
               str(int(offcpu.get("max_seconds", 86400)))]
    if offcpu.get("min_block_us"):
        command += ["-m", str(int(offcpu["min_block_us"]))]
    startup_wait = float(offcpu.get("startup_wait_seconds", 2))
    if not 0 < startup_wait < float("inf"):
        raise ValueError("offcpu.startup_wait_seconds must be finite and positive")
    try:
        handle = open(path, "w")
        try:
            process = subprocess.Popen(command, stdout=handle, stderr=subprocess.PIPE, text=True)
        except OSError:
            handle.close()
            raise
    except OSError as error:
        print(f"Warning: cannot start {command[0]}: {error}", file=sys.stderr)
        return None
    result = {"process": process, "file": handle, "path": path, "stopped": False, "valid": False}
    try:
        # Folded-output BCC tools have no readiness message. Allow probe compilation/attachment
        # before releasing the workload; the grace period is configurable for slower hosts.
        process.wait(timeout=startup_wait)
    except subprocess.TimeoutExpired:
        result["valid"] = True
        print(f"Off-CPU sampling started: {' '.join(command)}", flush=True)
        return result
    except BaseException:
        stop_offcpu(result)
        raise
    stop_offcpu(result)
    raise RuntimeError("off-CPU tool exited during startup; workload was not released")


def stop_offcpu(offcpu_handle):
    """Stop and reap a collector before client teardown; safe to call again during cleanup."""
    if offcpu_handle is None:
        return True
    if offcpu_handle["stopped"]:
        return offcpu_handle["valid"]
    process = offcpu_handle["process"]
    try:
        if process.poll() is None:
            try:
                process.send_signal(signal.SIGINT)
            except ProcessLookupError:
                offcpu_handle["valid"] = False
        else:
            offcpu_handle["valid"] = False
        _, errors = process.communicate(timeout=60)
    except subprocess.TimeoutExpired:
        process.kill()
        _, errors = process.communicate()
        offcpu_handle["valid"] = False
        print("Warning: off-CPU tool did not stop, profile dropped", file=sys.stderr)
    except BaseException:
        process.kill()
        process.communicate()
        offcpu_handle["valid"] = False
        raise
    finally:
        offcpu_handle["file"].close()
        offcpu_handle["stopped"] = True
    if process.returncode != 0:
        offcpu_handle["valid"] = False
    if errors.strip():
        print(f"Off-CPU tool: {errors.strip()}", file=sys.stderr)
    return offcpu_handle["valid"]


class WorkloadProfile:
    """Synchronize runner-owned collection with the native client's two barriers."""

    def __init__(self, cfg, perf_data):
        self.cfg = cfg
        self.perf_data = perf_data
        self.offcpu = None
        self.started_at = None
        self.start = None
        self.duration_s = None

    def on_event(self, message):
        if message.startswith(b"R ") and self.started_at is None:
            target_pids = message[2:].decode("ascii")
            if not re.fullmatch(r"[1-9]\d*(,[1-9]\d*)*", target_pids):
                raise ValueError("invalid profiling target process IDs")
            self.offcpu = start_offcpu(self.cfg, self.perf_data, target_pids)
            if tool_enabled(self.cfg, "offcpu") and self.offcpu is None:
                raise RuntimeError("off-CPU collection could not start")
            self.started_at = datetime.now(timezone.utc)
            self.start = time.monotonic()
        elif message == b"D" and self.started_at is not None and self.duration_s is None:
            self.duration_s = time.monotonic() - self.start
            if not stop_offcpu(self.offcpu):
                raise RuntimeError("off-CPU collection failed or ended before the workload drained")
        else:
            raise RuntimeError(f"unexpected profile control message: {message!r}")

    def close(self):
        stop_offcpu(self.offcpu)


def finish_offcpu(cfg, args, offcpu_handle, started_at, duration_s):
    """Stop off-CPU sampling, keep the benchmark's own stacks and send them to Pyroscope."""
    if offcpu_handle is None:
        return None
    if not stop_offcpu(offcpu_handle):
        return None
    path = offcpu_handle["path"]

    # The collector filters by process ID, preserving named workers such as net_pool0 and AccWrk:0.
    pattern = cfg["offcpu"].get("comm_filter") or ""
    with open(path, errors="replace") as f:
        lines = [line for line in f if re.match(pattern, line)] if pattern else f.readlines()
    if not lines:
        print(f"Warning: no off-CPU stacks matching {pattern!r} in {path}", file=sys.stderr)
        return None
    with open(path, "w") as f:
        f.writelines(lines)
    total_us = sum(int(line.rsplit(" ", 1)[1]) for line in lines if line.rsplit(" ", 1)[-1].strip().isdigit())
    print(f"Off-CPU stacks: {len(lines)} lines, {total_us / 1e6:.1f} s blocked in {path}", flush=True)

    name = f"{profile_name(args, started_at)}-offcpu"
    return name if upload_folded(cfg, path, name, OFFCPU_SAMPLE_RATE, started_at,
                                 duration_s) else None


def grafana_request(cfg, method, path, payload):
    """One Grafana API call. Returns the parsed body, or None when it did not go through."""
    grafana = cfg["grafana"]
    url = grafana.get("url_local") or grafana.get("url")
    token = grafana.get("api_key") or grafana.get("token")
    if not url:
        print("Warning: no Grafana url in the config, annotation not sent", file=sys.stderr)
        return None

    command = ["curl", "-sS", "--noproxy", "*", "-X", method, "-w", "\n%{http_code}",
               "-H", "Content-Type: application/json"]
    if token:
        command += ["-H", f"Authorization: Bearer {token}"]
    elif grafana.get("user"):
        command += ["-u", f"{grafana['user']}:{grafana.get('password', '')}"]
    command += ["--data", "@-", f"{url.rstrip('/')}{path}"]

    result = subprocess.run(command, input=json.dumps(payload), capture_output=True, text=True,
                            errors="replace")
    if result.returncode != 0:
        print(f"Warning: Grafana call failed: {result.stderr.strip()}", file=sys.stderr)
        return None
    body, _, status = result.stdout.rpartition("\n")
    if not status.strip().startswith("2"):
        print(f"Warning: Grafana rejected the annotation (HTTP {status.strip()}): "
              f"{body.strip()[:200]}", file=sys.stderr)
        return None
    try:
        return json.loads(body)
    except ValueError:
        return {}


def grafana_start_annotation(cfg, text):
    """Open a region annotation at the start of the run. Returns its id, or None."""
    if not tool_enabled(cfg, "grafana"):
        return None
    grafana = cfg["grafana"]
    now = int(time.time() * 1000)
    payload = {"time": now, "timeEnd": now, "tags": list(grafana.get("tags", [])), "text": text}
    if dashboard := (grafana.get("dashboard_uuid") or grafana.get("dashboard_uid")):
        payload["dashboardUID"] = dashboard
    if grafana.get("panel_id"):
        payload["panelId"] = int(grafana["panel_id"])

    body = grafana_request(cfg, "POST", "/api/annotations", payload)
    if body is None:
        return None
    annotation_id = body.get("id")
    print(f"Grafana annotation {annotation_id} opened: {text}", flush=True)
    return annotation_id


def grafana_end_annotation(cfg, annotation_id, text):
    """Stretch the annotation over the whole run, so it shows up as a region."""
    if not tool_enabled(cfg, "grafana") or annotation_id is None:
        return
    payload = {"timeEnd": int(time.time() * 1000), "text": text}
    if grafana_request(cfg, "PATCH", f"/api/annotations/{annotation_id}", payload) is not None:
        print(f"Grafana annotation {annotation_id} closed", flush=True)


def connect_mongo(url):
    """Return a client with a verified connection, or exit before the run."""
    client = MongoClient(url, serverSelectionTimeoutMS=5000)
    try:
        client.admin.command("ping")
    except PyMongoError as e:
        client.close()
        sys.exit(f"Cannot connect to MongoDB at {url}: {e}")
    print(f"MongoDB connection OK: {url}", flush=True)
    return client


def main():
    args = parse_args()
    with open(args.config) as f:
        cfg = yaml.safe_load(f)
    apply_config_options(args, cfg)

    mongo = cfg.get("mongo", {})
    client = connect_mongo(mongo["url"]) if tool_enabled(cfg, "mongo") else None

    cmd = build_command(cfg, args)
    params = vars(args).copy()
    del params["config"]

    final_exit_code = 0
    for run_number in range(1, args.rerun + 1):
        print(f"Run {run_number}/{args.rerun}:", " ".join(cmd), flush=True)

        label = f"run {run_number}/{args.rerun}" if args.rerun > 1 else "run"
        annotation_id = grafana_start_annotation(cfg, f"{label}: {' '.join(cmd)}")
        perf_data = os.path.join(cfg["bench"].get("cwd") or ".", "perf.data")
        profile = WorkloadProfile(cfg, perf_data)

        started_at = datetime.now(timezone.utc)
        start = time.monotonic()
        try:
            exit_code, output = run(cmd, cfg["bench"].get("cwd"), profile.on_event)
        finally:
            profile.close()
        duration_s = time.monotonic() - start
        final_exit_code = final_exit_code or exit_code

        grafana_end_annotation(cfg, annotation_id,
                               f"{label} ({duration_s:.0f}s, exit={exit_code}): {' '.join(cmd)}")

        offcpu_name = None
        if profile.duration_s is not None:
            offcpu_name = finish_offcpu(cfg, args, profile.offcpu, profile.started_at, profile.duration_s)

        metrics = parse_report(output)
        if not metrics:
            print("Warning: Benchmark Report not found in output", file=sys.stderr)

        flamegraph_name, flamegraph_pid = None, None
        if tool_enabled(cfg, "pyroscope") and profile.duration_s is not None:
            flamegraph_name = send_flamegraph(cfg, args, perf_data, profile.started_at, profile.duration_s)
            if flamegraph_name:
                flamegraph_pid = metrics.get("meta_service_pid")

        if client is not None:
            doc = {
                "started_at": started_at,
                "duration_s": duration_s,
                "workload_started_at": profile.started_at,
                "workload_duration_s": profile.duration_s,
                "host": socket.gethostname(),
                "command": " ".join(cmd),
                "exit_code": exit_code,
                "run": run_number,
                "flamegraph_name": flamegraph_name,
                "offcpu_name": offcpu_name,
                "flamegraph_pid": flamegraph_pid,
                **params,
                **metrics,
                "output": output,
            }
            result = client[mongo["database"]][mongo["collection"]].insert_one(doc)
            print(f"Saved to MongoDB: {result.inserted_id}", flush=True)

    if client is not None:
        client.close()
    sys.exit(final_exit_code)


if __name__ == "__main__":
    main()
