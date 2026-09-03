#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
import unittest


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


if __name__ == "__main__":
    unittest.main()
