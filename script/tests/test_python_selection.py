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

"""Regression checks for build interpreter selection; no compiler or NPU is needed."""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
import venv


class PythonSelectionTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="memcache python selection ")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.scripts = Path(__file__).resolve().parents[1]
        self.environment = os.environ.copy()
        for name in ("PYTHON_EXECUTABLE", "PYTHON_HOME", "VIRTUAL_ENV", "PYTHONPATH", "PYTHONHOME"):
            self.environment.pop(name, None)
        # Model a base installation's development files without requiring the host's dev package.
        self.development = self.root / "base development"
        self.development.mkdir()
        (self.development / "Python.h").touch()
        (self.development / "libpython-test.so").touch()
        self.modules = self.root / "modules"
        self.modules.mkdir()
        (self.modules / "sysconfig.py").write_text(
            f"def get_path(name): return {str(self.development)!r}\n"
            f"def get_config_var(name): return {{'LIBDIR': {str(self.development)!r}, "
            "'LDLIBRARY': 'libpython-test.so'}.get(name)\n",
            encoding="utf-8",
        )
        self.environment["PYTHONPATH"] = str(self.modules)
        self.virtual = self.root / "active venv"
        venv.EnvBuilder(with_pip=False).create(self.virtual)

    def select(self):
        return subprocess.run(
            ["bash", "-ec", 'source "$1"; printf "SELECTED=%s\n" "$PYTHON_EXECUTABLE"',
             "selection-test", str(self.scripts / "select_python.sh")],
            env=self.environment, capture_output=True, text=True, check=False,
        )

    def test_active_venv_beats_python_home(self):
        self.environment.update(VIRTUAL_ENV=str(self.virtual), PYTHON_HOME="/missing-python")
        result = self.select()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"SELECTED={self.virtual}/bin/python\n", result.stdout)
        self.assertIn(str(self.development), result.stdout)

    def test_explicit_interpreter_beats_venv(self):
        self.environment.update(VIRTUAL_ENV=str(self.virtual), PYTHON_EXECUTABLE=sys.executable)
        result = self.select()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"SELECTED={sys.executable}\n", result.stdout)

    def test_python_home_is_used_without_active_venv(self):
        self.environment["PYTHON_HOME"] = str(self.virtual)
        result = self.select()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"SELECTED={self.virtual}/bin/python3\n", result.stdout)

    def test_invalid_explicit_interpreter_does_not_fall_back(self):
        self.environment.update(PYTHON_EXECUTABLE="/missing-python", VIRTUAL_ENV=str(self.virtual))
        result = self.select()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Cannot find the selected Python", result.stderr)

    def test_missing_development_files_are_actionable(self):
        (self.development / "libpython-test.so").unlink()
        self.environment["PYTHON_EXECUTABLE"] = sys.executable
        result = self.select()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Install development headers and the shared library", result.stderr)

    def test_python_option_requires_value(self):
        result = subprocess.run(
            ["bash", str(self.scripts / "build_and_pack_run.sh"), "--python"],
            env=self.environment, capture_output=True, text=True, check=False,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--python requires", result.stderr)

    def test_incremental_build_passes_matching_cmake_artifacts(self):
        project = self.root / "project"
        scripts = project / "script"
        scripts.mkdir(parents=True)
        for name in ("build.sh", "select_python.sh", "build_and_pack_run.sh"):
            shutil.copyfile(self.scripts / name, scripts / name)
        (project / "VERSION").write_text("1.5.0\n", encoding="utf-8")
        (project / "src/memcache/python/memcache_hybrid").mkdir(parents=True)
        (project / "build").mkdir()
        sentinel = project / "build/old-cache-marker"
        sentinel.touch()
        fake_bin = self.root / "tools"
        fake_bin.mkdir()
        # Stop before compilation: check the exact configuration submitted by the supported wrapper.
        configure = fake_bin / "cmake"
        configure.write_text('#!/bin/bash\nprintf "%s\n" "$@"\nexit 81\n', encoding="utf-8")
        configure.chmod(0o700)
        self.environment["PATH"] = f"{fake_bin}:{self.environment['PATH']}"
        self.environment["PYTHON_EXECUTABLE"] = "/missing-env-python"
        chosen = str(self.virtual / "bin/python")
        result = subprocess.run(
            ["bash", str(scripts / "build_and_pack_run.sh"), "--incremental", "--python", chosen],
            env=self.environment, capture_output=True, text=True, check=False,
        )
        self.assertEqual(result.returncode, 81, result.stderr)
        self.assertTrue(sentinel.exists())
        self.assertIn(f"-DPYTHON_EXECUTABLE={chosen}\n", result.stdout)
        self.assertIn(f"-DPython_EXECUTABLE={chosen}\n", result.stdout)
        self.assertIn(f"-DPython_INCLUDE_DIR={self.development}\n", result.stdout)
        self.assertIn(f"-DPython_LIBRARY={self.development}/libpython-test.so\n", result.stdout)


if __name__ == "__main__":
    unittest.main()
