"""Real Ninja dependency checks without a compiler or product build."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest


REPO_ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(
    all(shutil.which(tool) for tool in ("cmake", "ninja", "git")),
    "CMake, Ninja and Git are required",
)
class BenchmarkCommitFreshnessTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="benchmark-freshness-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "source"
        self.source.mkdir()
        self.build = self.root / "build"
        helper = (REPO_ROOT / "bench/BenchmarkCommit.cmake").as_posix()
        (self.source / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.20)\n'
            'project(BenchmarkFreshness NONE)\n'
            'set(AZOOKEY_BENCH_GIT_COMMIT "" CACHE STRING "Commit override")\n'
            'set(AZOOKEY_BENCH_COMMIT_HEADER "${CMAKE_BINARY_DIR}/BenchmarkCommit.h")\n'
            f'include("{helper}")\n'
            'azookey_add_benchmark_commit_header()\n'
            'add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/consumer.stamp"\n'
            ' COMMAND "${CMAKE_COMMAND}" -E copy "${AZOOKEY_BENCH_COMMIT_HEADER}" "${CMAKE_BINARY_DIR}/consumer.stamp"\n'
            ' DEPENDS "${AZOOKEY_BENCH_COMMIT_HEADER}"\n'
            ' "${CMAKE_SOURCE_DIR}/consumer.cpp" "${CMAKE_SOURCE_DIR}/consumer.h"\n'
            ' COMMENT "Building benchmark consumer")\n'
            'add_custom_target(consumer ALL DEPENDS "${CMAKE_BINARY_DIR}/consumer.stamp")\n'
            'add_dependencies(consumer azookey_benchmark_commit_header)\n',
            encoding="utf-8",
        )
        for name in ("consumer.cpp", "consumer.h"):
            (self.source / name).write_text("// fixture\n", encoding="utf-8")
        self.git("init", "-b", "main")
        self.git("config", "user.email", "fixture@example.invalid")
        self.git("config", "user.name", "Fixture")
        self.git("add", ".")
        self.git("commit", "-m", "initial")

    def run_command(self, *args, cwd=None):
        result = subprocess.run(
            args, cwd=cwd or self.source, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            env={
                **os.environ,
                "GIT_CONFIG_NOSYSTEM": "1",
                "GIT_CONFIG_GLOBAL": os.devnull,
            },
            timeout=60,
        )
        self.assertEqual(result.returncode, 0, result.stdout)
        return result.stdout.strip()

    def git(self, *args):
        return self.run_command("git", *args)

    def configure(self, override=""):
        self.run_command(
            "cmake", "-S", str(self.source), "-B", str(self.build), "-G", "Ninja",
            f"-DAZOOKEY_BENCH_GIT_COMMIT={override}",
        )

    def dry_run(self):
        return self.run_command("ninja", "-C", str(self.build), "-n", "consumer")

    def assert_clean(self, expected=None):
        self.run_command("cmake", "--build", str(self.build), "--target", "consumer")
        self.assertIn("ninja: no work to do.", self.dry_run())
        self.git("status", "--short")
        self.assertIn("ninja: no work to do.", self.dry_run())
        expected = expected or self.git("rev-parse", "HEAD")
        self.assertIn(
            f'#define AZOOKEY_BENCH_COMMIT "{expected}"',
            (self.build / "BenchmarkCommit.h").read_text(encoding="utf-8"),
        )
        self.assertIn(
            f'#define AZOOKEY_BENCH_COMMIT "{expected}"',
            (self.build / "consumer.stamp").read_text(encoding="utf-8"),
        )

    def assert_stale(self):
        self.assertNotIn("ninja: no work to do.", self.dry_run())

    def advance_head(self):
        # Also works on filesystems with one-second metadata timestamps.
        time.sleep(1.1)
        self.git("commit", "--allow-empty", "-m", "advance")
        self.assert_stale()
        self.assert_clean()

    def test_clean_source_header_head_and_override(self):
        self.configure()
        self.assert_clean()
        for name in ("consumer.cpp", "consumer.h"):
            time.sleep(1.1)
            with (self.source / name).open("a", encoding="utf-8") as stream:
                stream.write("// changed\n")
            self.assert_stale()
            self.assertIn("Building benchmark consumer", self.dry_run())
            self.assert_clean()
        self.advance_head()
        self.configure("explicit-commit")
        self.assert_stale()
        self.assert_clean("explicit-commit")
        time.sleep(1.1)
        self.git("commit", "--allow-empty", "-m", "advance under override")
        self.assertIn("ninja: no work to do.", self.dry_run())
        self.assert_clean("explicit-commit")
        self.configure()
        self.assert_stale()
        self.assert_clean()

    def test_detached_head(self):
        self.git("checkout", "--detach")
        self.configure()
        self.assert_clean()
        self.advance_head()

    def test_packed_branch_becomes_loose(self):
        self.git("checkout", "-b", "nested/branch")
        self.git("pack-refs", "--all", "--prune")
        self.configure()
        self.assert_clean()
        self.advance_head()
        time.sleep(1.1)
        self.git("pack-refs", "--all", "--prune")
        self.assert_stale()
        self.assert_clean()

    def test_loose_branch_becomes_packed(self):
        self.configure()
        self.assert_clean()
        time.sleep(1.1)
        self.git("pack-refs", "--all", "--prune")
        self.assert_stale()
        self.assert_clean()
        self.advance_head()

    def test_linked_worktree(self):
        worktree = self.root / "worktree"
        self.git("worktree", "add", "-b", "linked", str(worktree))
        self.source = worktree
        self.configure()
        self.assert_clean()
        self.advance_head()

    def test_branch_switch(self):
        self.configure()
        self.assert_clean()
        initial = self.git("rev-parse", "HEAD")
        self.advance_head()
        time.sleep(1.1)
        self.git("checkout", "-b", "previous", initial)
        self.assert_stale()
        self.assert_clean(initial)


if __name__ == "__main__":
    unittest.main()
