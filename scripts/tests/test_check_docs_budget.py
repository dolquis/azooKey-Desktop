#!/usr/bin/env python3

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock


sys.dont_write_bytecode = True
SCRIPT = Path(__file__).resolve().parents[1] / "check_docs_budget.py"
SPEC = importlib.util.spec_from_file_location("check_docs_budget", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)

REPO_ROOT = SCRIPT.parents[1]


def write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def make_root(baseline: dict, cap: dict, specs: dict[str, str] | None = None) -> Path:
    tmp = Path(tempfile.mkdtemp())
    write(tmp / MODULE.BASELINE, json.dumps(baseline))
    write(tmp / MODULE.CAP, json.dumps(cap))
    for name, text in (specs or {}).items():
        write(tmp / "docs" / name, text)
    return tmp


def run(root: Path, env: dict[str, str] | None = None) -> tuple[int, str, str]:
    out, err = io.StringIO(), io.StringIO()
    with mock.patch.dict(os.environ, env or {}, clear=False), \
            contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = MODULE.main(["--root", str(root)])
    return code, out.getvalue(), err.getvalue()


class CapTest(unittest.TestCase):
    def test_equal_passes(self) -> None:
        data = {"status": {"A.md": 2}}
        self.assertEqual(MODULE.cap_problems(make_root(data, data)), [])

    def test_raise_above_cap_fails(self) -> None:
        problems = MODULE.cap_problems(make_root({"status": {"A.md": 3}}, {"status": {"A.md": 2}}))
        self.assertEqual(len(problems), 1)
        self.assertIn("上限 2 件", problems[0])

    def test_new_file_in_baseline_fails(self) -> None:
        problems = MODULE.cap_problems(make_root({"status": {"A.md": 1, "B.md": 1}}, {"status": {"A.md": 1}}))
        self.assertEqual(len(problems), 1)
        self.assertIn("B.md", problems[0])

    def test_new_category_fails(self) -> None:
        baseline = {"status": {"A.md": 1}, "decisive": {"A.md": 1}}
        problems = MODULE.cap_problems(make_root(baseline, {"status": {"A.md": 1}}))
        self.assertEqual(len(problems), 1)
        self.assertIn("decisive", problems[0])

    def test_reduction_requires_lowering_the_cap(self) -> None:
        problems = MODULE.cap_problems(make_root({"status": {"A.md": 1}}, {"status": {"A.md": 2}}))
        self.assertEqual(len(problems), 1)
        self.assertIn("1 件へ下げてください", problems[0])

    def test_removed_baseline_entry_requires_dropping_the_cap(self) -> None:
        problems = MODULE.cap_problems(make_root({"status": {}}, {"status": {"A.md": 2}}))
        self.assertEqual(len(problems), 1)
        self.assertIn("0 件へ下げてください", problems[0])

    def test_zero_counts_are_absent(self) -> None:
        self.assertEqual(MODULE.cap_problems(make_root({"status": {"A.md": 0}}, {"status": {}})), [])

    def test_missing_file_is_reported(self) -> None:
        problems = MODULE.cap_problems(Path(tempfile.mkdtemp()))
        self.assertEqual(len(problems), 1)
        self.assertIn("存在しません", problems[0])

    def test_broken_json_is_reported(self) -> None:
        root = make_root({}, {})
        write(root / MODULE.CAP, "{")
        problems = MODULE.cap_problems(root)
        self.assertEqual(len(problems), 1)
        self.assertIn("JSON", problems[0])


class SizeTest(unittest.TestCase):
    def test_small_spec_is_quiet(self) -> None:
        root = make_root({}, {}, {"a-spec.md": "# A\n\n## 1. x\n"})
        self.assertEqual(MODULE.size_warnings(root), [])

    def test_large_spec_and_section_warn(self) -> None:
        body = "# A\n\n## 1. Big\n" + "line\n" * MODULE.SPEC_LINE_WARN
        warnings = MODULE.size_warnings(make_root({}, {}, {"a-spec.md": body}))
        messages = " ".join(m for _, m in warnings)
        self.assertIn("分割を検討", messages)
        self.assertIn("節「1. Big」", messages)

    def test_section_just_under_the_threshold_is_quiet(self) -> None:
        body = "## 1. x\n" + "line\n" * (MODULE.SECTION_LINE_WARN - 2)
        self.assertEqual(MODULE.size_warnings(make_root({}, {}, {"a-spec.md": body})), [])

    def test_section_at_the_threshold_warns(self) -> None:
        body = "## 1. x\n" + "line\n" * (MODULE.SECTION_LINE_WARN - 1)
        self.assertEqual(len(MODULE.size_warnings(make_root({}, {}, {"a-spec.md": body}))), 1)

    def test_heading_like_line_in_a_code_block_is_not_a_boundary(self) -> None:
        body = "## 1. x\n```\n## not a section\n```\n" + "line\n" * (MODULE.SECTION_LINE_WARN - 2)
        warnings = MODULE.size_warnings(make_root({}, {}, {"a-spec.md": body}))
        self.assertEqual(len(warnings), 1)
        self.assertIn("節「1. x」", warnings[0][1])

    def test_non_spec_documents_are_ignored(self) -> None:
        body = "## 1. Big\n" + "line\n" * 2000
        self.assertEqual(MODULE.size_warnings(make_root({}, {}, {"guide.md": body})), [])


class MainTest(unittest.TestCase):
    def test_warnings_never_fail(self) -> None:
        body = "## 1. Big\n" + "line\n" * 2000
        data = {"status": {"A.md": 1}}
        code, out, _ = run(make_root(data, data, {"a-spec.md": body}), {"GITHUB_ACTIONS": "false"})
        self.assertEqual(code, 0)
        self.assertIn("警告:", out)

    def test_github_annotation_format(self) -> None:
        body = "## 1. Big\n" + "line\n" * 2000
        code, out, _ = run(make_root({}, {}, {"a-spec.md": body}), {"GITHUB_ACTIONS": "true"})
        self.assertEqual(code, 0)
        self.assertIn("::warning file=docs/a-spec.md::", out)

    def test_cap_violation_fails(self) -> None:
        code, _, err = run(make_root({"status": {"A.md": 2}}, {"status": {"A.md": 1}}))
        self.assertEqual(code, 1)
        self.assertIn("上限", err)


class RepositoryTest(unittest.TestCase):
    def test_repository_cap_matches_baseline(self) -> None:
        self.assertEqual(MODULE.cap_problems(REPO_ROOT), [])


if __name__ == "__main__":
    unittest.main()
