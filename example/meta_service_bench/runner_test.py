#!/usr/bin/env python3
# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemCache_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.

"""Exercise profiling handshakes with real subprocesses, without native/BCC dependencies."""

import contextlib
import importlib.util
import io
import os
from pathlib import Path
import signal
import sys
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest import mock


def load_runner():
    """Replace only optional database/config integrations, which these tests do not exercise."""
    spec = importlib.util.spec_from_file_location("bench_runner", Path(__file__).with_name("runner.py"))
    module = importlib.util.module_from_spec(spec)
    with mock.patch.dict(sys.modules, {name: mock.MagicMock() for name in ("yaml", "pymongo", "pymongo.errors")}):
        spec.loader.exec_module(module)
    return module


RUNNER = load_runner()
BENCHMARK = """
import os
from pathlib import Path
import socket
import sys
import time
control = socket.socket(fileno=int(sys.argv[-1]))
assert not Path('collector-ready').exists()
time.sleep(0.08)
Path('warmup-done').touch()
control.send(('R ' + str(os.getpid())).encode())
if control.recv(1) != b'S':
    sys.exit(3)
assert Path('collector-ready').exists()
Path('workload-started').touch()
print('workload: café', flush=True)
time.sleep(0.03)
if os.environ.get('BENCH_TEST_ABORT'):
    sys.exit(4)
control.send(b'D')
if control.recv(1) != b'T':
    sys.exit(5)
assert Path('collector-stopped').exists()
print('teardown', flush=True)
control.close()
"""
COLLECTOR = """
from pathlib import Path
import signal
import sys
import time
assert Path('warmup-done').exists()
assert '-p' in sys.argv
assert sys.argv[sys.argv.index('-p') + 1].isdigit()
def finish(signum, frame):
    print('net_pool0;handle_request 15000')
    print('AccWrk:0;epoll_wait 1000')
    Path('collector-stopped').touch()
    sys.exit(0)
signal.signal(signal.SIGINT, finish)
Path('collector-ready').touch()
while True:
    time.sleep(0.01)
"""


class LatencyReportTest(unittest.TestCase):
    # Both pool columns must reach MongoDB with numeric values, sample counts, and nulls for unavailable metrics.
    def test_transposed_net_worker_stats_in_mongo_document(self):
        cfg = {"bench": {"cwd": "."}, "mongo": {"database": "memcache", "collection": "bench_meta_service"}}
        output = """Benchmark report: maximum logical concurrency=2
MetaService network worker statistics (across workers)
Metric (avg / stddev)     net_pool (MemCache workers)      AccWrk (transport layer)
Worker count                                   1,600                            4
CPU                                 52.00% / 4.50 pp              32.00% / 6.00 pp
User CPU                            43.25% / 4.00 pp              22.00% / 4.00 pp
Kernel CPU                           8.75% / 0.50 pp              10.00% / 2.00 pp
Runnable wait               2.00% / 1.00 pp (n=1,200)                         N/A
Estimated sleep            46.00% / 5.50 pp (n=1,200)                         N/A
Voluntary switches             12,345.75 / 12,000.50                324.00 / 6.00
Involuntary switches                  123.00 / 25.00                  10.00 / 2.00
Benchmark finished.
"""
        client = mock.MagicMock()
        with (
            mock.patch.object(RUNNER, "build_command", return_value=["bench_meta_service"]),
            mock.patch.object(RUNNER, "run", return_value=(0, output)),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            self.assertEqual(RUNNER.run_workload(cfg, SimpleNamespace(), client, "local", "mixed", 1), 0)

        insert = client["memcache"]["bench_meta_service"].insert_one
        insert.assert_called_once()
        stats = insert.call_args.args[0]["meta_net_worker_stats"]
        self.assertEqual(stats, {
            "net_pool": {
                "cpu_pct": {"workers": 1600, "avg": 52.0, "stddev": 4.5},
                "user_cpu_pct": {"workers": 1600, "avg": 43.25, "stddev": 4.0},
                "kernel_cpu_pct": {"workers": 1600, "avg": 8.75, "stddev": 0.5},
                "runnable_wait_pct": {"workers": 1200, "avg": 2.0, "stddev": 1.0},
                "sleep_estimate_pct": {"workers": 1200, "avg": 46.0, "stddev": 5.5},
                "voluntary_switches": {"workers": 1600, "avg": 12345.75, "stddev": 12000.5},
                "involuntary_switches": {"workers": 1600, "avg": 123.0, "stddev": 25.0},
            },
            "AccWrk": {
                "cpu_pct": {"workers": 4, "avg": 32.0, "stddev": 6.0},
                "user_cpu_pct": {"workers": 4, "avg": 22.0, "stddev": 4.0},
                "kernel_cpu_pct": {"workers": 4, "avg": 10.0, "stddev": 2.0},
                "runnable_wait_pct": {"workers": 0, "avg": None, "stddev": None},
                "sleep_estimate_pct": {"workers": 0, "avg": None, "stddev": None},
                "voluntary_switches": {"workers": 4, "avg": 324.0, "stddev": 6.0},
                "involuntary_switches": {"workers": 4, "avg": 10.0, "stddev": 2.0},
            },
        })

    # Startup settings and the commit read from the target cwd must reach MongoDB alongside latency metrics.
    def test_meta_service_settings_in_mongo_document(self):
        cwd = "/workspace/target-checkout"
        git_commit = "0123456789abcdef" * 2 + "01234567"
        cfg = {"bench": {"cwd": cwd}, "mongo": {"database": "memcache", "collection": "bench_meta_service"}}
        for build_mode, lru_type, shards, expected_count in (
            ("DEBUG", "lru", "N/A", None),
            ("RELEASE", "sharded_lru", "32", 32),
        ):
            with self.subTest(build_mode=build_mode, lru_type=lru_type):
                output = f"""Configuration:
  Build mode:   {build_mode}
  Processes:    2
  MetaService LRU type:    {lru_type}
  LRU shards:              {shards}
  Async network workers:   16
  TCP transport workers:   4

Benchmark report: maximum logical concurrency=2
Client-observed RPC round-trip latency (us = microseconds)
put-alloc 2 5.50 5.00 6.00 6.00 6.00 5.00 6.00
Benchmark finished.
"""
                client = mock.MagicMock()
                with (
                    mock.patch.object(RUNNER, "build_command", return_value=["bench_meta_service"]),
                    mock.patch.object(RUNNER, "run", return_value=(0, output)),
                    mock.patch.object(RUNNER.subprocess, "run", return_value=SimpleNamespace(stdout=git_commit + "\n"))
                    as git,
                    contextlib.redirect_stdout(io.StringIO()) as stdout,
                ):
                    self.assertEqual(RUNNER.run_workload(cfg, SimpleNamespace(), client, "local", "mixed", 1), 0)
                self.assertEqual(git.call_args.args[0], ["git", "rev-parse", "--verify", "HEAD"])
                self.assertEqual(git.call_args.kwargs["cwd"], cwd)
                self.assertIn(f"Git commit: {git_commit}", stdout.getvalue())
                insert = client["memcache"]["bench_meta_service"].insert_one
                insert.assert_called_once()
                doc = insert.call_args.args[0]
                self.assertEqual(doc["git_commit"], git_commit)
                self.assertEqual(doc["build_mode"], build_mode)
                self.assertEqual(doc["meta_lru_type"], lru_type)
                self.assertEqual(doc["meta_lru_shard_count"], expected_count)
                self.assertEqual(doc["meta_net_async_workers"], 16)
                self.assertEqual(doc["meta_net_tcp_workers"], 4)
                self.assertEqual(doc["rpc_put_alloc_p99_us"], 6.0)
                self.assertEqual(doc["output"], output)

    # A run that stops after configuration still yields the available settings and a failed completion marker.
    def test_meta_service_settings_without_final_report(self):
        output = """Configuration:
  LRU shards:              32
Benchmark failed.
"""
        results = RUNNER.parse_report(output)
        self.assertEqual(results["meta_lru_shard_count"], 32)
        self.assertFalse(results["finished"])

    # Separate client distributions must retain their counts and percentiles without overwriting or averaging.
    def test_per_process_percentiles(self):
        output = """Benchmark report: queue depth/thread=1, completion mode=poll, maximum logical concurrency=2
Client process: rank=1, pid=101
Client-observed RPC round-trip latency (us = microseconds)
get 2500 70.25 65.25 90.25 120.25 125.25 60.25 130.25
Submission duration (us = microseconds; overlaps RPC latency)
get 2501 5.25 4.25 6.25 8.25 9.25 1.25 10.25
Client process: rank=2, pid=102
Client-observed RPC round-trip latency (us = microseconds)
get 100 80.25 75.25 95.25 125.25 130.25 65.25 130.25
Submission duration (us = microseconds; overlaps RPC latency)
get 101 6.25 5.25 7.25 9.25 10.25 2.25 11.25
Logical Put: success=0 fail=0
get: success=2600 fail=0 transport failures=0 timeouts=0
Total logical ops: 2600, achieved logical ops/sec: 1300.00
Benchmark finished.
"""
        results = RUNNER.parse_report(output)
        first, second = results["per_process"]
        self.assertEqual((first["rank"], first["pid"]), (1, 101))
        self.assertEqual(first["rpc_get_samples"], 2500)
        self.assertEqual(first["sub_get_samples"], 2501)
        self.assertEqual(first["rpc_get_p99_us"], 120.25)
        self.assertEqual(second["rpc_get_p99_us"], 125.25)
        self.assertEqual(second["sub_get_p999_us"], 10.25)
        self.assertNotIn("rpc_get_p99_us", results)
        self.assertEqual(results["get_success"], 2600)
        self.assertTrue(results["finished"])

    # Older aggregate reports must retain top-level latency and settings printed in the final report.
    def test_legacy_aggregate_async_report(self):
        output = """Benchmark report:
MetaService: LRU=lru, LRU shards=N/A, async network workers=16, TCP transport workers=4
Client-observed RPC round-trip latency
get 100 70 64 128 128 256 10 200
Submission duration
get 100 5 4 8 8 16 1 12
"""
        results = RUNNER.parse_report(output)
        self.assertEqual(results["rpc_get_p99_us"], 128)
        self.assertEqual(results["sub_get_samples"], 100)
        self.assertIsNone(results["meta_lru_shard_count"])
        self.assertEqual(results["meta_net_tcp_workers"], 4)
        self.assertNotIn("per_process", results)


class ProfilingTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        previous_directory = os.getcwd()
        os.chdir(self.root)
        self.addCleanup(os.chdir, previous_directory)
        # The fake tool runs in the runner's directory, just like the real BCC tool.
        collector = self.root / "offcpu-tool"
        collector.write_text(f"#!{sys.executable}\n{COLLECTOR}", encoding="utf-8")
        collector.chmod(0o700)
        self.collector = collector
        self.cfg = {"offcpu": {"enabled": True, "tool": str(collector), "startup_wait_seconds": 0.15}}
        self.profile = RUNNER.WorkloadProfile(self.cfg, str(self.root / "perf.data"))
        self.addCleanup(self.profile.close)

    def run_benchmark(self, benchmark=BENCHMARK):
        # Keep cwd changes local to these sequential tests; no external services are contacted.
        with contextlib.redirect_stdout(io.StringIO()), \
                contextlib.redirect_stderr(io.StringIO()):
            return RUNNER.run([sys.executable, "-c", benchmark], str(self.root), self.profile.on_event)

    def test_collection_is_between_warmup_and_teardown(self):
        start = time.monotonic()
        exit_code, output = self.run_benchmark()
        elapsed = time.monotonic() - start
        self.assertEqual(exit_code, 0, output)
        self.assertIn("workload: café", output)
        self.assertIn("teardown", output)
        self.assertIsNotNone(self.profile.started_at)
        self.assertGreater(self.profile.duration_s, 0)
        self.assertLess(self.profile.duration_s, elapsed - 0.15)
        self.assertTrue(self.profile.offcpu["stopped"])
        self.assertTrue(self.profile.offcpu["valid"])
        self.assertIsNotNone(self.profile.offcpu["process"].poll())
        self.assertTrue(self.profile.offcpu["file"].closed)
        with mock.patch.object(RUNNER, "upload_folded", return_value=True) as upload:
            RUNNER.finish_offcpu(self.cfg, SimpleNamespace(pyroscope_name="test"), self.profile.offcpu,
                                 self.profile.started_at, self.profile.duration_s)
        self.assertEqual(upload.call_args.args[-2:], (self.profile.started_at, self.profile.duration_s))
        self.assertIn("net_pool0;", Path(self.profile.offcpu["path"]).read_text(encoding="utf-8"))
        self.assertIn("AccWrk:0;", Path(self.profile.offcpu["path"]).read_text(encoding="utf-8"))

    def test_startup_failure_does_not_release_workload(self):
        self.collector.write_text(f"#!{sys.executable}\nraise SystemExit(2)\n", encoding="utf-8")
        exit_code, output = self.run_benchmark()
        self.assertEqual(exit_code, 3, output)
        self.assertFalse((self.root / "workload-started").exists())
        self.assertIsNone(self.profile.started_at)

    def test_aborted_benchmark_still_reaps_collector(self):
        with mock.patch.dict("os.environ", {"BENCH_TEST_ABORT": "1"}):
            exit_code, output = self.run_benchmark()
        self.assertEqual(exit_code, 4, output)
        self.assertIsNone(self.profile.duration_s)
        self.profile.close()
        self.profile.close()
        self.assertTrue((self.root / "collector-stopped").exists())
        self.assertIsNotNone(self.profile.offcpu["process"].poll())

    def test_protocol_rejects_out_of_order_messages(self):
        with self.assertRaises(RuntimeError):
            self.profile.on_event(b"D")
        with self.assertRaises(ValueError):
            self.profile.on_event(b"R invalid-pid")

    def test_collector_exit_during_workload_rejects_profile(self):
        self.collector.write_text(
            f"#!{sys.executable}\nfrom pathlib import Path\nimport time\n"
            "Path('collector-ready').touch()\ntime.sleep(0.25)\n", encoding="utf-8")
        exit_code, output = self.run_benchmark(BENCHMARK.replace("time.sleep(0.03)", "time.sleep(0.4)"))
        self.assertEqual(exit_code, 5, output)
        self.assertFalse(self.profile.offcpu["valid"])
        self.assertTrue(self.profile.offcpu["stopped"])

    def test_disabled_offcpu_still_tracks_workload_window(self):
        self.cfg["offcpu"]["enabled"] = False
        self.profile.on_event(b"R 123")
        self.profile.on_event(b"D")
        self.assertIsNone(self.profile.offcpu)
        self.assertIsNotNone(self.profile.duration_s)

    def test_offcpu_only_run_forwards_selected_target(self):
        values = {name: default for name, _, _, default, _ in RUNNER.PARAMS}
        values.update(perf=False, perf_freq=99, perf_target="clients", meta_numa=None, worker_numa=None,
                      completion_mode="poll", numa_mem="none")
        command = RUNNER.build_command({"bench": {"binary": "bench"}}, SimpleNamespace(**values))
        self.assertNotIn("--perf", command)
        self.assertEqual(command[command.index("--perf-target") + 1], "clients")

    def test_plain_run_preserves_exit_code_and_output(self):
        with contextlib.redirect_stdout(io.StringIO()):
            exit_code, output = RUNNER.run([sys.executable, "-c", "print('hello'); raise SystemExit(7)"], None)
        self.assertEqual(exit_code, 7)
        self.assertEqual(output, "hello\n")

    def test_collector_descendants_are_reaped(self):
        children = """
import os
import signal
from pathlib import Path
import time
live = os.fork()
if live == 0:
    signal.signal(signal.SIGINT, signal.SIG_DFL)
    while True:
        time.sleep(1)
zombie = os.fork()
if zombie == 0:
    os._exit(0)
Path('collector-children').write_text(f'{live} {zombie}')
"""
        self.collector.write_text(f"#!{sys.executable}\n{children}\n{COLLECTOR}", encoding="utf-8")
        exit_code, output = self.run_benchmark()
        self.assertEqual(exit_code, 0, output)
        for pid in (self.root / "collector-children").read_text().split():
            self.assertFalse(Path(f"/proc/{pid}").exists(), f"collector child {pid} was not reaped")


class ProcessCleanupTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)

    def assert_reaped(self):
        pids = (self.root / "pids").read_text().split()
        self.assertEqual(len(pids), 3)
        for pid in pids:
            self.assertFalse(Path(f"/proc/{pid}").exists(), f"process {pid} still exists (including zombies)")

    def benchmark(self, wait_for_control=False):
        return """
import os
from pathlib import Path
import signal
import socket
import sys
import time
live = os.fork()
if live == 0:
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    Path('child-ready').touch()
    while True:
        time.sleep(1)
zombie = os.fork()
if zombie == 0:
    os._exit(0)
while not Path('child-ready').exists():
    time.sleep(0.01)
Path('pids').write_text(f'{os.getpid()} {live} {zombie}')
time.sleep(0.05)
""" + ("""
control = socket.socket(fileno=int(sys.argv[-1]))
control.send(('R ' + str(os.getpid())).encode())
control.recv(1)
time.sleep(60)
""" if wait_for_control else "os._exit(7)\n")

    def test_crashed_coordinator_reaps_orphans_and_escalates_shutdown(self):
        with mock.patch.object(RUNNER, "PROCESS_STOP_TIMEOUT_SECONDS", 0.15), \
                contextlib.redirect_stdout(io.StringIO()):
            exit_code, _ = RUNNER.run([sys.executable, "-c", self.benchmark()], str(self.root))
        self.assertEqual(exit_code, 7)
        self.assert_reaped()

    def test_sigterm_reaps_coordinator_and_descendants(self):
        previous = signal.getsignal(signal.SIGTERM)

        def interrupt(_):
            os.kill(os.getpid(), signal.SIGTERM)

        with mock.patch.object(RUNNER, "PROCESS_STOP_TIMEOUT_SECONDS", 0.15), \
                contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(KeyboardInterrupt):
                RUNNER.run([sys.executable, "-c", self.benchmark(wait_for_control=True)],
                           str(self.root), interrupt)
        self.assertEqual(signal.getsignal(signal.SIGTERM), previous)
        self.assert_reaped()

    def test_ctrl_c_reaps_coordinator_and_descendants(self):
        def interrupt(_):
            raise KeyboardInterrupt

        with mock.patch.object(RUNNER, "PROCESS_STOP_TIMEOUT_SECONDS", 0.15), \
                contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(KeyboardInterrupt):
                RUNNER.run([sys.executable, "-c", self.benchmark(wait_for_control=True)],
                           str(self.root), interrupt)
        self.assert_reaped()


if __name__ == "__main__":
    unittest.main()
