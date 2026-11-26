#!/usr/bin/env python3
"""Run every configured workload against every configured benchmark binary."""
import argparse
import codecs
import contextlib
import ctypes
import json
import os
import re
import selectors
import shlex
import shutil
import signal
import socket
import subprocess
import sys
import threading
import time
from datetime import datetime, timezone
from types import SimpleNamespace

import yaml
from pymongo import MongoClient
from pymongo.errors import PyMongoError

# name, flag, type, example, description
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


NUMA_NODES_RE = re.compile(r"!?[0-9]+(-[0-9]+)?(,[0-9]+(-[0-9]+)?)*")
BATCH_RE = re.compile(r"[0-9]+(,[0-9]+)?")
MAX_INT = (1 << 31) - 1
MAX_TRANSPORT_DEPTH = (1 << 16) - 1
MAX_PROCESSES = 32
MAX_NUMA_NODE = 1023
WORKLOAD_OPTIONS = {
    "completion_mode": ("poll", "wait"),
    "numa_mem": ("bind", "preferred", "none"),
}
NUMA_OPTIONS = ("meta_numa", "worker_numa")


def check_explicit_keys(loader, node, location):
    """Check matrix mappings before SafeLoader can merge or overwrite their keys."""
    if not isinstance(node, yaml.MappingNode):
        return
    seen = set()
    for key_node, value_node in node.value:
        if key_node.tag == "tag:yaml.org,2002:merge":
            raise ValueError(f"{location} cannot use YAML merge keys")
        if not isinstance(key_node, yaml.ScalarNode) or key_node.tag != "tag:yaml.org,2002:str":
            raise ValueError(f"{location} keys must be strings")
        key = loader.construct_object(key_node)
        if key in seen:
            raise ValueError(f"{location} has duplicate key {key!r}")
        seen.add(key)
        if location in ("bench", "iterations"):
            check_explicit_keys(loader, value_node, f"{location}[{key!r}]")


def load_config(stream):
    """Load YAML normally outside the two explicit matrix sections."""
    class ConfigLoader(yaml.SafeLoader):
        depth = 0
        matrix_section = None

        def compose_node(self, parent, index):
            previous_section = self.matrix_section
            if self.depth == 1 and isinstance(index, yaml.ScalarNode) and index.value in ("bench", "iterations"):
                self.matrix_section = index.value
            self.depth += 1
            try:
                if self.matrix_section and self.check_event(yaml.AliasEvent):
                    raise ValueError(f"{self.matrix_section} cannot use YAML aliases")
                return super().compose_node(parent, index)
            finally:
                self.depth -= 1
                self.matrix_section = previous_section

    loader = ConfigLoader(stream)
    try:
        root = loader.get_single_node()
        if not isinstance(root, yaml.MappingNode):
            raise ValueError("expected a configuration mapping")
        sections = set()
        for key, value in root.value:
            if not isinstance(key, yaml.ScalarNode) or key.value not in ("bench", "iterations"):
                continue
            if key.value in sections:
                raise ValueError(f"duplicate section {key.value!r}")
            sections.add(key.value)
            check_explicit_keys(loader, value, key.value)
        if missing := {"bench", "iterations"} - sections:
            raise ValueError(f"missing explicit section(s): {', '.join(sorted(missing))}")
        return loader.construct_document(root)
    finally:
        loader.dispose()


def validate_fields(values, required, location):
    if not isinstance(values, dict):
        raise ValueError(f"{location} must be a mapping")
    if missing := required - values.keys():
        raise ValueError(f"{location} is missing required field(s): {', '.join(sorted(missing))}")
    if unknown := values.keys() - required:
        raise ValueError(f"{location} has unknown field(s): {', '.join(sorted(unknown))}")


def validate_int(value, minimum, maximum, location):
    if type(value) is not int or not minimum <= value <= maximum:
        raise ValueError(f"{location} must be an integer between {minimum} and {maximum}")


def validate_string(value, location):
    if not isinstance(value, str) or not value or "\0" in value:
        raise ValueError(f"{location} must be a nonempty string without NUL characters")


def parse_targets(bench):
    if not isinstance(bench, dict) or not bench.keys() - {"rerun"}:
        raise ValueError("bench must contain at least one named benchmark binary")
    rerun = bench.get("rerun")
    validate_int(rerun, 1, sys.maxsize, "bench.rerun")

    targets = {}
    for name, target in bench.items():
        if name == "rerun":
            continue
        location = f"bench[{name!r}]"
        validate_fields(target, {"binary", "cwd"}, location)
        validate_string(target["cwd"], f"{location}.cwd")
        validate_string(target["binary"], f"{location}.binary")
        cwd = os.path.join(os.getcwd(), target["cwd"])
        if not os.path.isdir(cwd) or not os.access(cwd, os.X_OK):
            raise ValueError(f"{location}.cwd is not an accessible directory: {cwd!r}")

        binary = target["binary"]
        if os.path.dirname(binary):
            executable = os.path.join(cwd, binary)
        else:
            # Popen searches PATH after changing to cwd, including for relative PATH entries.
            path = os.pathsep.join(os.path.join(cwd, entry) for entry in os.get_exec_path())
            executable = shutil.which(binary, path=path)
        if not executable or not os.path.isfile(executable) or not os.access(executable, os.X_OK):
            raise ValueError(f"{location}.binary is not an executable file: {binary!r} (cwd {cwd!r})")
        targets[name] = {"binary": executable, "cwd": cwd}
    return targets, rerun


def parse_iterations(iterations):
    if not isinstance(iterations, dict) or not iterations:
        raise ValueError("iterations must contain at least one named workload")
    required = {name for name, _, _, _, _ in PARAMS} | WORKLOAD_OPTIONS.keys() | set(NUMA_OPTIONS)
    ranges = {
        "processes": (1, MAX_PROCESSES),
        "warmup": (0, MAX_INT),
        "metrics_interval_ms": (0, MAX_INT),
        "put_percent": (1, 99),
        "queue_depth": (1, MAX_TRANSPORT_DEPTH),
    }
    for name, values in iterations.items():
        location = f"iterations[{name!r}]"
        validate_fields(values, required, location)
        for field, _, field_type, example, description in PARAMS:
            value = values[field]
            field_location = f"{location}.{field}"
            if field_type is int:
                validate_int(value, *ranges.get(field, (1, MAX_INT)), field_location)
            else:
                validate_string(value, field_location)
            if field == "batch":
                if not BATCH_RE.fullmatch(value):
                    raise ValueError(f"{field_location} must be a {description.lower()}, e.g. {example!r}")
                bounds = [int(bound) for bound in value.split(",")]
                if not 1 <= bounds[0] <= bounds[-1] <= MAX_INT:
                    raise ValueError(f"{field_location} must use positive, ascending bounds")
        if values["threads"] * values["queue_depth"] > MAX_TRANSPORT_DEPTH:
            raise ValueError(f"{location}: threads times queue_depth exceeds {MAX_TRANSPORT_DEPTH}")
        for field, choices in WORKLOAD_OPTIONS.items():
            if values[field] not in choices:
                raise ValueError(f"{location}.{field} must be one of {', '.join(choices)}")
        for field in NUMA_OPTIONS:
            value = values[field]
            if value is None:
                continue
            if not isinstance(value, str) or not NUMA_NODES_RE.fullmatch(value):
                raise ValueError(f"{location}.{field} must be null or a quoted NUMA node list, e.g. '4-7'")
            for entry in value.lstrip("!").split(","):
                bounds = [int(bound) for bound in entry.split("-")]
                if not bounds[0] <= bounds[-1] <= MAX_NUMA_NODE:
                    raise ValueError(f"{location}.{field} must use ascending node ranges within 0-{MAX_NUMA_NODE}")
    return iterations


PERF_TARGETS = ("meta", "clients", "client0")


def apply_config_options(args, cfg):
    """Pull the profiling options out of the config and onto args."""
    perf = cfg.get("perf", {})
    pyroscope = cfg.get("pyroscope", {})

    args.perf = bool(perf.get("enabled", False))
    args.perf_freq = int(perf.get("freq", 99))
    args.perf_target = str(perf.get("target", "meta"))
    args.flamegraph_max_stack = int(perf.get("max_stack", 0) or 0)
    args.pyroscope_name = pyroscope.get("name") or None

    if args.perf_target not in PERF_TARGETS:
        sys.exit(f"config perf.target: {args.perf_target!r} (expected {'|'.join(PERF_TARGETS)})")
    if args.perf_freq < 1:
        sys.exit("config perf.freq must be at least 1")
    if args.flamegraph_max_stack < 0:
        sys.exit("config perf.max_stack must be 0 (full stacks) or more")
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


PROCESS_STOP_TIMEOUT_SECONDS = 30
PROCESS_POLL_SECONDS = 0.05


def enable_child_subreaper():
    """Adopt orphaned descendants on Linux so this runner can reap them."""
    pr_set_child_subreaper = 36
    libc = ctypes.CDLL(None, use_errno=True)
    if libc.prctl(pr_set_child_subreaper, 1, 0, 0, 0) != 0:
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error))


@contextlib.contextmanager
def interrupt_on_sigterm():
    """Give SIGTERM the same cleanup path as Ctrl+C."""
    if threading.current_thread() is not threading.main_thread():
        yield
        return

    def interrupt(signum, frame):
        raise KeyboardInterrupt

    previous = signal.signal(signal.SIGTERM, interrupt)
    try:
        yield
    finally:
        signal.signal(signal.SIGTERM, previous)


def signal_process_group(process, signum):
    try:
        os.killpg(process.pid, signum)
    except ProcessLookupError:
        pass


def stop_process_group(process):
    """Stop one private process group and reap its leader and adopted descendants."""
    def reap_descendants(block=False):
        while True:
            try:
                pid, _ = os.waitpid(-process.pid, 0 if block else os.WNOHANG)
            except ChildProcessError:
                return
            except InterruptedError:
                continue
            if pid == 0:
                return

    signal_process_group(process, signal.SIGTERM)
    deadline = time.monotonic() + PROCESS_STOP_TIMEOUT_SECONDS
    while time.monotonic() < deadline:
        # Let Popen reap its own child before reaping adopted grandchildren.
        if process.poll() is not None:
            reap_descendants()
        try:
            os.killpg(process.pid, 0)
        except ProcessLookupError:
            process.wait()
            return
        time.sleep(PROCESS_POLL_SECONDS)
    signal_process_group(process, signal.SIGKILL)
    process.wait()
    reap_descendants(block=True)


def run(cmd, cwd, on_profile=None):
    """Run the command, echoing its output live. Returns (exit_code, output)."""
    chunks = []
    enable_child_subreaper()
    decoder = codecs.getincrementaldecoder("utf-8")(errors="replace")
    control, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    with control, child, selectors.DefaultSelector() as watcher, interrupt_on_sigterm():
        command = list(cmd)
        inherited = ()
        if on_profile is not None:
            command += ["--profile-control-fd", str(child.fileno())]
            inherited = (child.fileno(),)
        with subprocess.Popen(command, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              pass_fds=inherited, start_new_session=True) as proc:
            child.close()
            watcher.register(proc.stdout, selectors.EVENT_READ, "output")
            if on_profile is not None:
                watcher.register(control, selectors.EVENT_READ, "control")
            group_stopped = False
            try:
                while watcher.get_map():
                    # An orphan may hold stdout/control open after the coordinator exits.
                    if proc.poll() is not None and not group_stopped:
                        stop_process_group(proc)
                        group_stopped = True
                    for key, _ in watcher.select(PROCESS_POLL_SECONDS):
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
            finally:
                if not group_stopped:
                    stop_process_group(proc)
        return proc.returncode, "".join(chunks)


def number(text):
    return float(text) if "." in text else int(text)


LATENCY_COLUMNS = ("samples", "avg_us", "p50_us", "p95_us", "p99_us", "p999_us", "min_us", "max_us")
LATENCY_ROW_RE = re.compile(r"^(put-alloc|put-update|get)((?:\s+[\d.]+){8})$")
OUTCOME_RE = re.compile(r"^(put-alloc|put-update|get): success=(\d+) fail=(\d+) "
                        r"transport failures=(\d+) timeouts=(\d+)$")
LEGACY_NET_WORKER_ROW_RE = re.compile(
    r"^(net_pool|AccWrk)[ \t]+([a-z_]+)[ \t]+(\d+)[ \t]+(\d+\.\d+|N/A)[ \t]+(\d+\.\d+|N/A)$",
    re.MULTILINE,
)
NET_WORKER_POOLS = {"net_pool (MemCache workers)": "net_pool", "AccWrk (transport layer)": "AccWrk"}
NET_WORKER_METRICS = {
    "CPU": "cpu_pct",
    "User CPU": "user_cpu_pct",
    "Kernel CPU": "kernel_cpu_pct",
    "Runnable wait": "runnable_wait_pct",
    "Estimated sleep": "sleep_estimate_pct",
    "Voluntary switches": "voluntary_switches",
    "Involuntary switches": "involuntary_switches",
}
GROUPED_INTEGER = r"(?:\d{1,3}(?:,\d{3})+|\d+)"
NET_WORKER_CELL_RE = re.compile(
    rf"({GROUPED_INTEGER}\.\d+)%? / ({GROUPED_INTEGER}\.\d+)(?: pp)?(?: \(n=({GROUPED_INTEGER})\))?"
)
META_RSS_CHANGE_RE = re.compile(r"^MetaService RSS change: (?:([+-]?\d+(?:\.\d+)?) ([KMGTPE]iB)|N/A)$", re.MULTILINE)
META_SERVICE_RE = re.compile(r"^[ \t]*MetaService: LRU=(lru|sharded_lru), LRU shards=(\d+|N/A), "
                             r"async network workers=(\d+), TCP transport workers=(\d+)$", re.MULTILINE)
CONFIG_FIELD_RE = re.compile(
    r"^[ \t]*(?:Build mode:[ \t]*(?P<build_mode>\S+)"
    r"|MetaService LRU type:[ \t]*(?P<meta_lru_type>lru|sharded_lru)"
    r"|LRU shards:[ \t]*(?P<meta_lru_shard_count>\d+|N/A)"
    r"|Async network workers:[ \t]*(?P<meta_net_async_workers>\d+)"
    r"|TCP transport workers:[ \t]*(?P<meta_net_tcp_workers>\d+))[ \t]*$",
    re.MULTILINE,
)


def parse_net_worker_stats(output):
    """Read the pool columns and preserve the MongoDB metric keys and worker counts."""
    _, _, section = output.partition("MetaService network worker statistics (across workers)\n")
    rows = [re.split(r"[ \t]{2,}", line) for line in section.splitlines()]
    if (
        len(rows) >= 2
        and rows[0] == ["Metric (avg / stddev)", *NET_WORKER_POOLS]
        and len(rows[1]) == len(rows[0])
        and rows[1][0] == "Worker count"
        and all(re.fullmatch(GROUPED_INTEGER, value) for value in rows[1][1:])
    ):
        stats = {pool: {} for pool in NET_WORKER_POOLS.values()}
        for label, *cells in rows[2:]:
            if len(cells) != len(stats) or label not in NET_WORKER_METRICS:
                break
            for pool, workers, cell in zip(stats, rows[1][1:], cells):
                metric = NET_WORKER_METRICS[label]
                if cell == "N/A":
                    stats[pool][metric] = {"workers": 0, "avg": None, "stddev": None}
                elif match := NET_WORKER_CELL_RE.fullmatch(cell):
                    avg, stddev, count = match.groups()
                    stats[pool][metric] = {
                        "workers": int((count or workers).replace(",", "")),
                        "avg": float(avg.replace(",", "")),
                        "stddev": float(stddev.replace(",", "")),
                    }
        return stats

    stats = {}
    for pool, metric, workers, avg, stddev in LEGACY_NET_WORKER_ROW_RE.findall(output):
        stats.setdefault(pool, {})[metric] = {
            "workers": int(workers),
            "avg": None if avg == "N/A" else float(avg),
            "stddev": None if stddev == "N/A" else float(stddev),
        }
    return stats


def parse_report(output):
    """Parse the configuration and the async or legacy benchmark report."""
    if "Benchmark report:" in output:
        results = parse_async_report(output)
    else:
        results = parse_legacy_report(output)
    if match := META_SERVICE_RE.search(output):
        lru_type, shards, async_workers, tcp_workers = match.groups()
        results.update(
            meta_lru_type=lru_type,
            meta_lru_shard_count=None if shards == "N/A" else int(shards),
            meta_net_async_workers=int(async_workers),
            meta_net_tcp_workers=int(tcp_workers),
        )
    for match in CONFIG_FIELD_RE.finditer(output):
        field = match.lastgroup
        value = match[field]
        if field in ("build_mode", "meta_lru_type"):
            results[field] = value
        else:
            results[field] = None if value == "N/A" else int(value)
    if worker_stats := parse_net_worker_stats(output):
        results["meta_net_worker_stats"] = worker_stats
    if match := META_RSS_CHANGE_RE.search(output):
        value, unit = match.groups()
        results["meta_rss_delta"] = {"value": number(value), "unit": unit} if value is not None else None
    return add_run_markers(results, output)


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
    """Parse aggregate latency and RPC outcomes, including older per-process latency tables."""
    _, _, report = output.rpartition("Benchmark report:")
    results = {}
    prefix = None
    process = None

    for line in report.splitlines():
        line = line.strip()
        if match := re.match(r"Client process: rank=(\d+), pid=(\d+)$", line):
            process = {"rank": int(match.group(1)), "pid": int(match.group(2))}
            results.setdefault("per_process", []).append(process)
            prefix = None
        elif line.startswith("Client-observed RPC round-trip latency"):
            prefix = "rpc"
        elif line.startswith("Submission duration"):
            prefix = "sub"
        elif (match := LATENCY_ROW_RE.match(line)) and prefix:
            op = match.group(1).replace("-", "_")
            for column, value in zip(LATENCY_COLUMNS, match.group(2).split()):
                destination = process if process is not None else results
                destination[f"{prefix}_{op}_{column}"] = number(value)
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
            enable_child_subreaper()
            process = subprocess.Popen(command, stdout=handle, stderr=subprocess.PIPE, text=True,
                                       start_new_session=True)
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
                signal_process_group(process, signal.SIGINT)
            except ProcessLookupError:
                offcpu_handle["valid"] = False
        else:
            offcpu_handle["valid"] = False
        _, errors = process.communicate(timeout=PROCESS_STOP_TIMEOUT_SECONDS)
    except subprocess.TimeoutExpired:
        signal_process_group(process, signal.SIGKILL)
        _, errors = process.communicate()
        offcpu_handle["valid"] = False
        print("Warning: off-CPU tool did not stop, profile dropped", file=sys.stderr)
    except BaseException:
        signal_process_group(process, signal.SIGKILL)
        process.communicate()
        offcpu_handle["valid"] = False
        raise
    finally:
        try:
            stop_process_group(process)
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


def read_git_commit(cwd):
    """Read the full HEAD commit from the benchmark's working directory."""
    try:
        result = subprocess.run(
            ["git", "rev-parse", "--verify", "HEAD"],
            cwd=cwd, check=True, text=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
        )
    except (OSError, subprocess.CalledProcessError):
        return None
    return result.stdout.strip()


def run_workload(cfg, args, client, target_name, iteration_name, rerun):
    mongo = cfg.get("mongo", {})
    cwd = cfg["bench"]["cwd"]
    cmd = build_command(cfg, args)
    command = shlex.join(cmd)
    params = vars(args).copy()
    params["rerun"] = rerun
    final_exit_code = 0
    for run_number in range(1, rerun + 1):
        git_commit = read_git_commit(cwd)
        label = f"bench={target_name!r} iteration={iteration_name!r} run {run_number}/{rerun}"
        print(f"{label}: {command}\n  Git commit: {git_commit or 'N/A'}", flush=True)
        annotation_id = grafana_start_annotation(cfg, f"{label}: {command}")
        perf_data = os.path.join(cwd, "perf.data")
        profile = WorkloadProfile(cfg, perf_data)

        started_at = datetime.now(timezone.utc)
        start = time.monotonic()
        try:
            exit_code, output = run(cmd, cwd, profile.on_event)
        finally:
            profile.close()
        duration_s = time.monotonic() - start
        final_exit_code = final_exit_code or exit_code

        grafana_end_annotation(cfg, annotation_id,
                               f"{label} ({duration_s:.0f}s, exit={exit_code}): {command}")

        offcpu_name = None
        if profile.duration_s is not None:
            offcpu_name = finish_offcpu(cfg, args, profile.offcpu, profile.started_at, profile.duration_s)

        metrics = parse_report(output)
        if not metrics or ("Benchmark report:" not in output and "Benchmark Report" not in output):
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
                "command": command,
                "bench_name": target_name,
                "iteration_name": iteration_name,
                "cwd": cwd,
                "git_commit": git_commit,
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

    return final_exit_code


def main():
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--config", required=True, help="YAML configuration path")
    config_path = parser.parse_args().config
    try:
        with open(config_path, encoding="utf-8") as stream:
            cfg = load_config(stream)
        targets, rerun = parse_targets(cfg["bench"])
        iterations = parse_iterations(cfg["iterations"])
    except (OSError, ValueError, yaml.YAMLError) as error:
        sys.exit(f"{config_path}: {error}")

    profiling = apply_config_options(SimpleNamespace(), cfg)
    mongo = cfg.get("mongo", {})
    client = connect_mongo(mongo["url"]) if tool_enabled(cfg, "mongo") else None

    final_exit_code = 0
    try:
        for iteration_name, values in iterations.items():
            args = SimpleNamespace(**values, **vars(profiling))
            for target_name, target in targets.items():
                exit_code = run_workload({**cfg, "bench": target}, args, client, target_name, iteration_name, rerun)
                final_exit_code = final_exit_code or exit_code
    finally:
        if client is not None:
            client.close()
    sys.exit(final_exit_code)


if __name__ == "__main__":
    main()
