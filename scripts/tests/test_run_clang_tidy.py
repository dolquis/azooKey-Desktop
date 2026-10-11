"""Compile database filtering and advisory reporting contract tests."""

from contextlib import redirect_stdout
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch


SCRIPT = Path(__file__).resolve().parents[1] / "run-clang-tidy.py"
SPEC = importlib.util.spec_from_file_location("run_clang_tidy", SCRIPT)
TIDY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(TIDY)


class RunClangTidyTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.build = self.root / "build"
        self.build.mkdir()
        self.database = self.build / "compile_commands.json"
        self.summary = self.root / "summary.md"
        self.calls = []
        self.changed = ["inference-host/src/production.cpp",
                        "inference-host/tests/new test.cpp"]
        self.exit_codes = {}
        self.git_error = None
        self.tidy_error = None
        self.write_database([{"directory": str(self.root), "file": self.changed[0],
                              "command": "c++ -c " + self.changed[0]}])
        for path, contents in zip(self.changed, ("int main() {}", "#include <gtest/gtest.h>")):
            source = self.root / path
            source.parent.mkdir(parents=True, exist_ok=True)
            source.write_text(contents, encoding="utf-8")

    def write_database(self, entries):
        self.database.write_text(json.dumps(entries), encoding="utf-8")

    def run_command(self, command, **kwargs):
        if command[0] == "git":
            self.assertIn("-z", command)
            self.assertIn("--diff-filter=ACMR", command)
            if self.git_error:
                raise self.git_error
            return subprocess.CompletedProcess(command, 0,
                                               stdout=("\0".join(self.changed) + "\0").encode("utf-8"))
        self.calls.append(command)
        self.assertEqual(Path(kwargs["cwd"]), self.root)
        if self.tidy_error:
            raise self.tidy_error
        return subprocess.CompletedProcess(command, self.exit_codes.get(command[-1], 0))

    def run_tidy(self, *extra):
        with patch.object(TIDY.Path, "cwd", return_value=self.root), \
                patch.object(TIDY.subprocess, "run", side_effect=self.run_command), \
                redirect_stdout(io.StringIO()):
            code = TIDY.main(["--base", "base-sha", "--build-dir", str(self.build),
                              "--summary", str(self.summary), *extra])
        return code, self.summary.read_text(encoding="utf-8")

    def test_production_and_new_gtest_tu_only_analyzes_database_member(self):
        code, summary = self.run_tidy()
        self.assertEqual(code, 0)
        self.assertEqual([call[-1] for call in self.calls], [self.changed[0]])
        self.assertIn("| 2 | 1 | 1 | 0 | 0 |", summary)
        self.assertIn(self.changed[1], summary)
        self.assertIn("not in compile_commands.json", summary)

    def test_relative_and_absolute_database_paths_and_duplicate_entries(self):
        self.write_database([
            {"directory": "../inference-host/src", "file": "./production.cpp"},
            {"directory": str(self.build), "file": str(self.root / self.changed[0])},
            {"directory": str(self.root / "inference-host/tests"), "file": "new test.cpp"},
        ])
        code, summary = self.run_tidy()
        self.assertEqual(code, 0)
        self.assertEqual([call[-1] for call in self.calls], self.changed)
        self.assertIn("| 2 | 2 | 0 | 0 | 0 |", summary)

    def test_analysis_failure_reports_path_exit_code_and_keeps_analyzing(self):
        self.changed.append("core/src/another.cpp")
        self.write_database([{"directory": str(self.root), "file": path} for path in self.changed])
        self.exit_codes[self.changed[0]] = 1
        code, summary = self.run_tidy()
        self.assertEqual(code, 1)
        self.assertEqual(len(self.calls), 3)
        self.assertIn("| 3 | 2 | 0 | 1 | 0 |", summary)
        self.assertIn(self.changed[0], summary)
        self.assertIn("clang-tidy exited 1", summary)

    def test_missing_executable_is_visible_for_each_selected_path(self):
        self.tidy_error = FileNotFoundError("missing analyzer")
        code, summary = self.run_tidy()
        self.assertEqual(code, 1)
        self.assertIn("| 2 | 0 | 1 | 1 | 0 |", summary)
        self.assertIn("clang-tidy could not start", summary)

    def test_failed_configure_does_not_use_stale_database(self):
        code, summary = self.run_tidy("--configure-exit-code", "3")
        self.assertEqual(code, 1)
        self.assertEqual(self.calls, [])
        self.assertIn("| 2 | 0 | 0 | 2 | 1 |", summary)
        self.assertIn("offline configure exited 3", summary)
        for path in self.changed:
            self.assertIn(path, summary)

    def test_missing_database_reports_changed_paths(self):
        self.database.unlink()
        code, summary = self.run_tidy()
        self.assertEqual(code, 1)
        self.assertEqual(self.calls, [])
        self.assertIn("| 2 | 0 | 0 | 2 | 1 |", summary)
        self.assertIn("compile_commands.json unavailable or invalid", summary)

    def test_invalid_database_is_not_reported_as_a_skip(self):
        for payload in ("{", "{}", '[{"file": "source.cpp"}]'):
            with self.subTest(payload=payload):
                self.database.write_text(payload, encoding="utf-8")
                self.summary.unlink(missing_ok=True)
                code, summary = self.run_tidy()
                self.assertEqual(code, 1)
                self.assertIn("| 2 | 0 | 0 | 2 | 1 |", summary)
                self.assertEqual(self.calls, [])

    def test_changed_sources_outside_portable_subset_are_ignored(self):
        self.changed = ["tsf-tip/src/windows.cpp", "core/include/header.h", "settings/readme.md"]
        code, summary = self.run_tidy()
        self.assertEqual(code, 0)
        self.assertEqual(self.calls, [])
        self.assertIn("| 0 | 0 | 0 | 0 | 0 |", summary)

    def test_real_git_rename_selects_destination_tu(self):
        def git(*args):
            return subprocess.run(
                ["git", "-c", "core.fsmonitor=false", "-c", "user.name=Tidy Fixture",
                 "-c", "user.email=tidy-fixture@example.invalid", *args],
                cwd=self.root, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            ).stdout

        git("init", "--quiet")
        git("config", "core.fsmonitor", "false")
        git("add", "--", *self.changed)
        git("commit", "--quiet", "--no-gpg-sign", "-m", "base fixture")
        base = git("rev-parse", "HEAD").decode("ascii").strip()
        destination = self.root / "core/src/renamed production.cpp"
        destination.parent.mkdir(parents=True)
        (self.root / self.changed[0]).rename(destination)
        git("add", "--all")
        git("commit", "--quiet", "--no-gpg-sign", "-m", "rename fixture")
        self.assertEqual(TIDY.changed_sources(self.root, base),
                         ["core/src/renamed production.cpp"])

    def test_git_failure_is_visible_even_without_known_changed_paths(self):
        self.git_error = subprocess.CalledProcessError(128, "git")
        code, summary = self.run_tidy()
        self.assertEqual(code, 1)
        self.assertIn("changed-file selection failed", summary)
        self.assertIn("| 0 | 0 | 0 | 0 | 1 |", summary)

    def test_skip_paths_escape_markdown_and_append_to_existing_summary(self):
        self.changed = ["core/tests/pipe|<tag>.cpp"]
        self.summary.write_text("Existing summary\n", encoding="utf-8")
        code, summary = self.run_tidy()
        self.assertEqual(code, 0)
        self.assertTrue(summary.startswith("Existing summary\n"))
        self.assertIn("core/tests/pipe&#124;&lt;tag&gt;.cpp", summary)


if __name__ == "__main__":
    unittest.main()
