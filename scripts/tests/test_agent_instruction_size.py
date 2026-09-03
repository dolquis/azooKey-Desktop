#!/usr/bin/env python3

from __future__ import annotations

from contextlib import redirect_stdout
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch


sys.dont_write_bytecode = True
SCRIPT = Path(__file__).resolve().parents[1] / "check_agent_instruction_size.py"
SPEC = importlib.util.spec_from_file_location("check_agent_instruction_size", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class ClassifyTests(unittest.TestCase):
    def test_target_boundary_is_ok(self) -> None:
        self.assertEqual(MODULE.classify(12_288), "ok")

    def test_byte_above_target_warns(self) -> None:
        self.assertEqual(MODULE.classify(12_289), "warning")

    def test_max_boundary_warns(self) -> None:
        self.assertEqual(MODULE.classify(16_384), "warning")

    def test_byte_above_max_fails(self) -> None:
        self.assertEqual(MODULE.classify(16_385), "error")


class OutputTests(unittest.TestCase):
    def run_main(self, size: int, *extra_args: str) -> tuple[int, list[str]]:
        with tempfile.TemporaryDirectory() as temporary_directory:
            path = Path(temporary_directory) / "AGENTS.md"
            path.write_bytes(b"x" * size)
            output = io.StringIO()
            with patch.object(sys, "argv", [str(SCRIPT), str(path), *extra_args]):
                with redirect_stdout(output):
                    exit_code = MODULE.main()
        return exit_code, output.getvalue().splitlines()

    def test_warning_emits_github_annotation(self) -> None:
        exit_code, lines = self.run_main(2, "--target", "1", "--max", "3")

        self.assertEqual(exit_code, 0)
        self.assertEqual(len(lines), 2)
        self.assertIn("status=warning", lines[0])
        self.assertTrue(lines[1].startswith("::warning file="))
        self.assertIn("target is 1 bytes and maximum is 3 bytes", lines[1])

    def test_repository_relative_annotation_path_is_preserved(self) -> None:
        annotation = MODULE.warning_annotation(Path("AGENTS.md"), 2, 1, 3)

        self.assertTrue(annotation.startswith("::warning file=AGENTS.md,"))

    def test_json_output_remains_single_machine_readable_line(self) -> None:
        exit_code, lines = self.run_main(2, "--target", "1", "--max", "3", "--json")

        self.assertEqual(exit_code, 0)
        self.assertEqual(len(lines), 1)
        self.assertEqual(json.loads(lines[0])["status"], "warning")


if __name__ == "__main__":
    unittest.main()
