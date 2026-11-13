#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest


sys.dont_write_bytecode = True
SCRIPT = Path(__file__).resolve().parents[1] / "check_skill_tables.py"
SPEC = importlib.util.spec_from_file_location("check_skill_tables", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)

REPO_ROOT = SCRIPT.parents[1]

WINDOWS_YML = """\
name: CI
on: push
jobs:
  changes:
    name: Detect changes
    runs-on: ubuntu-24.04
  build:
    name: Build
    needs: changes
  tidy:
    name: Tidy
    needs: changes
  ci-gate:
    name: CI gate
    needs:
      # comment between items
      - changes
      - build
    if: always()
"""

DOCS_YML = """\
name: Docs lint
jobs:
  docs-lint:
    steps:
      - name: Lint
        run: python3 scripts/docs-lint.py --baseline .docs-lint-baseline.json
      - name: Test the checker
        run: python3 scripts/tests/test_check_alpha.py
      - name: Check alpha
        run: |
          set -e
          python3 scripts/check_alpha.py
"""

JOB_TABLE = """\
# Routing

## windows.yml の job

| job | ci-gate | 内容 |
|---|---|---|
| `changes` | 必須 | 分類 |
| `build` | 必須 | build |
| `tidy` | advisory | tidy |

## 次の節

| `unrelated` | 必須 | 見出しの外の行は読まない |
"""

GATE_BODY = "Run `python3 scripts/docs-lint.py` and `python3 scripts/check_alpha.py`.\n"


def write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def make_repo(
    root: Path,
    windows: str = WINDOWS_YML,
    docs: str = DOCS_YML,
    table: str = JOB_TABLE,
    gate: str = GATE_BODY,
) -> None:
    write(root / MODULE.WINDOWS_WORKFLOW, windows)
    write(root / MODULE.DOCS_WORKFLOW, docs)
    for relative in MODULE.JOB_TABLE_FILES:
        write(root / relative, table)
    for relative in MODULE.GATE_DEFINITIONS:
        write(root / relative, gate)


class ParserTest(unittest.TestCase):
    def test_jobs_and_block_needs(self) -> None:
        jobs = MODULE.parse_workflow_jobs(WINDOWS_YML)
        self.assertEqual(["changes", "build", "tidy", "ci-gate"], list(jobs))
        self.assertEqual({"changes", "build"}, MODULE.parse_needs(jobs["ci-gate"]))

    def test_inline_and_scalar_needs(self) -> None:
        self.assertEqual({"a", "b"}, MODULE.parse_needs(["    needs: [a, b]"]))
        self.assertEqual({"changes"}, MODULE.parse_needs(["    needs: changes"]))
        self.assertEqual(set(), MODULE.parse_needs(["    runs-on: x"]))

    def test_job_table_stops_at_next_heading(self) -> None:
        table = MODULE.parse_job_table(JOB_TABLE)
        self.assertEqual({"changes": "必須", "build": "必須", "tidy": "advisory"}, table)

    def test_quoted_needs_and_trailing_comments(self) -> None:
        self.assertEqual({"a", "b"}, MODULE.parse_needs(["    needs: ['a', \"b\"]  # why"]))
        self.assertEqual({"a"}, MODULE.parse_needs(["    needs:", "      - 'a'  # why"]))

    def test_unparseable_needs_forms_are_empty(self) -> None:
        self.assertEqual(set(), MODULE.parse_needs(["    needs: [", "      a,", "    ]"]))
        self.assertEqual(set(), MODULE.parse_needs(["    needs:", "    - a"]))

    def test_quoted_job_key_is_a_job(self) -> None:
        jobs = MODULE.parse_workflow_jobs('jobs:\n  "quoted-job":\n    name: x\n  plain:\n    name: y\n')
        self.assertEqual(["quoted-job", "plain"], list(jobs))

    def test_duplicate_table_rows_are_collected(self) -> None:
        duplicates: list[str] = []
        MODULE.parse_job_table(JOB_TABLE.replace("| `tidy` |", "| `build` | advisory | x |\n| `tidy` |"), duplicates)
        self.assertEqual(["build"], duplicates)

    def test_checkers_ignore_tests_directory(self) -> None:
        self.assertEqual({"check_alpha.py", "docs-lint.py"}, MODULE.workflow_checkers(DOCS_YML))

    def test_commented_out_checker_is_not_run(self) -> None:
        text = DOCS_YML + "      # - run: python3 scripts/check_old.py\n        # python3 scripts/check_older.py\n"
        self.assertEqual({"check_alpha.py", "docs-lint.py"}, MODULE.workflow_checkers(text))


class CheckSkillTablesTest(unittest.TestCase):
    def problems(self, **overrides: str) -> list[str]:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            make_repo(root, **overrides)
            return MODULE.check(root)

    def test_agreeing_lists_pass(self) -> None:
        self.assertEqual([], self.problems())

    def test_table_job_missing_from_workflow_fails(self) -> None:
        table = JOB_TABLE.replace("| `tidy` | advisory | tidy |", "| `tidy` | advisory | tidy |\n| `ghost` | advisory | 無い job |")
        problems = self.problems(table=table)
        self.assertTrue(any("`ghost`" in p and "に無い job" in p for p in problems), problems)

    def test_workflow_job_missing_from_table_fails(self) -> None:
        windows = WINDOWS_YML.replace("  ci-gate:", "  extra:\n    name: Extra\n  ci-gate:")
        problems = self.problems(windows=windows)
        self.assertTrue(any("`extra`" in p and "表にありません" in p for p in problems), problems)

    def test_new_needs_entry_still_marked_advisory_fails(self) -> None:
        windows = WINDOWS_YML.replace("      - build\n", "      - build\n      - tidy\n")
        problems = self.problems(windows=windows)
        self.assertTrue(any("`tidy`" in p and "`必須`" in p for p in problems), problems)

    def test_required_row_not_in_needs_fails(self) -> None:
        windows = WINDOWS_YML.replace("      - build\n", "")
        problems = self.problems(windows=windows)
        self.assertTrue(any("`build`" in p and "`advisory`" in p for p in problems), problems)

    def test_unknown_gate_column_fails(self) -> None:
        problems = self.problems(table=JOB_TABLE.replace("| `tidy` | advisory |", "| `tidy` | maybe |"))
        self.assertTrue(any("`tidy`" in p and "`maybe`" in p for p in problems), problems)

    def test_duplicate_table_row_fails(self) -> None:
        table = JOB_TABLE.replace("| `tidy` | advisory | tidy |", "| `tidy` | advisory | tidy |\n| `tidy` | 必須 | 重複 |")
        problems = self.problems(table=table)
        self.assertTrue(any("`tidy`" in p and "複数" in p for p in problems), problems)

    def test_unparseable_ci_gate_needs_is_reported_as_such(self) -> None:
        windows = WINDOWS_YML.replace("      # comment between items\n      - changes\n      - build\n", "")
        windows = windows.replace("    needs:\n", "    needs: [\n      changes,\n      build ]\n")
        problems = self.problems(windows=windows)
        self.assertEqual(1, len(problems), problems)
        self.assertIn("needs を解釈できません", problems[0])

    def test_missing_table_fails(self) -> None:
        problems = self.problems(table="# Routing\n")
        self.assertTrue(any("job の表がありません" in p for p in problems), problems)

    def test_needs_pointing_at_missing_job_fails(self) -> None:
        windows = WINDOWS_YML.replace("      - build\n", "      - build\n      - vanished\n")
        problems = self.problems(windows=windows)
        self.assertTrue(any("`vanished`" in p for p in problems), problems)

    def test_checker_missing_from_gate_definition_fails(self) -> None:
        problems = self.problems(gate="Run `python3 scripts/docs-lint.py` only.\n")
        self.assertEqual(2, sum("scripts/check_alpha.py" in p and "ゲート表にありません" in p for p in problems), problems)

    def test_gate_checker_not_run_by_docs_workflow_fails(self) -> None:
        problems = self.problems(gate=GATE_BODY + "Also `python3 scripts/check_beta.py`.\n")
        self.assertEqual(2, sum("scripts/check_beta.py" in p and "実行していません" in p for p in problems), problems)

    def test_missing_gate_definition_fails(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            make_repo(root)
            (root / MODULE.GATE_DEFINITIONS[1]).unlink()
            problems = MODULE.check(root)
        self.assertTrue(any("ファイルがありません" in p for p in problems), problems)


class RepositoryTest(unittest.TestCase):
    def test_repository_lists_match_ci(self) -> None:
        self.assertEqual([], MODULE.check(REPO_ROOT))

    def test_exclusions_name_real_things(self) -> None:
        jobs = MODULE.parse_workflow_jobs((REPO_ROOT / MODULE.WINDOWS_WORKFLOW).read_text(encoding="utf-8"))
        for job in MODULE.EXCLUDED_WORKFLOW_JOBS:
            self.assertIn(job, jobs)
        docs = MODULE.workflow_checkers((REPO_ROOT / MODULE.DOCS_WORKFLOW).read_text(encoding="utf-8"))
        for script in MODULE.GATE_EXCLUDED_CHECKERS:
            self.assertIn(script, docs)


if __name__ == "__main__":
    unittest.main()
