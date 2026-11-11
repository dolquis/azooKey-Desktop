#!/usr/bin/env python3

from __future__ import annotations

import contextlib
import importlib.util
import io
from pathlib import Path
import sys
import tempfile
import unittest


sys.dont_write_bytecode = True
SCRIPT = Path(__file__).resolve().parents[1] / "check_doc_format.py"
SPEC = importlib.util.spec_from_file_location("check_doc_format", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)

REPO_ROOT = SCRIPT.parents[1]

ROADMAP = """\
# Roadmap

### M1: First

- **前提**: なし

### M2: Second

- **前提**: M1
  M3（続きの行）。

### M3: Third

- **前提**: M1

### M31: Withdrawn

廃止。

## Phase 5〜7 の依存関係と実行順

```
M1 ─┬─→ M2
    └─→ M3
M1 / M3 ─→ M2
（M31 = 廃止）
```
"""

SPEC_OK = """\
# Sample spec

本書は M1 を扱う。

## 1. Purpose

### 1.1 受け入れ条件

受け入れ条件の定義の正典は roadmap の M1 節とする。
"""


def write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def make_root(roadmap: str = ROADMAP, specs: dict[str, str] | None = None) -> Path:
    tmp = Path(tempfile.mkdtemp())
    write(tmp / MODULE.ROADMAP, roadmap)
    for name, text in (specs if specs is not None else {"ok-spec.md": SPEC_OK}).items():
        write(tmp / "docs" / name, text)
    return tmp


def run(root: Path) -> tuple[int, str]:
    err = io.StringIO()
    with contextlib.redirect_stderr(err), contextlib.redirect_stdout(io.StringIO()):
        code = MODULE.main(["--root", str(root)])
    return code, err.getvalue()


class RoadmapTest(unittest.TestCase):
    def test_clean_roadmap_passes(self) -> None:
        self.assertEqual(MODULE.check_roadmap(make_root()), [])

    def test_milestone_missing_from_diagram(self) -> None:
        roadmap = ROADMAP.replace("### M31: Withdrawn", "### M4: Fourth\n\n- **前提**: M1\n\n### M31: Withdrawn")
        problems = MODULE.check_roadmap(make_root(roadmap))
        self.assertTrue(any("M4 が依存図に現れません" in p for p in problems), problems)

    def test_missing_prerequisite_line(self) -> None:
        roadmap = ROADMAP.replace("### M3: Third\n\n- **前提**: M1\n", "### M3: Third\n\n本文だけ。\n")
        problems = MODULE.check_roadmap(make_root(roadmap))
        self.assertTrue(any("M3 の節に「- **前提**:」" in p for p in problems), problems)

    def test_exempt_milestone_may_lack_the_line(self) -> None:
        problems = MODULE.check_roadmap(make_root())
        self.assertFalse(any("M31" in p for p in problems), problems)

    def test_prerequisite_not_in_diagram(self) -> None:
        roadmap = ROADMAP.replace("- **前提**: M1\n\n### M31", "- **前提**: M9\n\n### M31")
        problems = MODULE.check_roadmap(make_root(roadmap))
        self.assertTrue(any("M3 の前提 M9 が依存図に現れません" in p for p in problems), problems)

    def test_continuation_line_counts_as_prerequisite(self) -> None:
        # M2 names M3 only on the continuation line; the edge `M1 / M3 ─→ M2` needs it.
        self.assertEqual(MODULE.check_roadmap(make_root()), [])
        roadmap = ROADMAP.replace("  M3（続きの行）。\n", "")
        problems = MODULE.check_roadmap(make_root(roadmap))
        self.assertTrue(any("M3 → M2" in p for p in problems), problems)

    def test_direct_edge_without_prerequisite(self) -> None:
        roadmap = ROADMAP.replace("- **前提**: M1\n  M3（続きの行）。", "- **前提**: M3")
        problems = MODULE.check_roadmap(make_root(roadmap))
        self.assertTrue(any("M1 → M2" in p for p in problems), problems)

    def test_branch_lines_are_not_extracted(self) -> None:
        # `└─→ M3` has no source on its line, so no edge is derived from it,
        # even when M3 names a different prerequisite.
        roadmap = ROADMAP.replace("### M3: Third\n\n- **前提**: M1", "### M3: Third\n\n- **前提**: M2")
        problems = MODULE.check_roadmap(make_root(roadmap))
        self.assertFalse(any("→ M3" in p for p in problems), problems)

    def test_chained_edges_are_all_checked(self) -> None:
        # `M1 ─→ M2 ─→ M3`: the second edge starts at the target of the first.
        roadmap = ROADMAP.replace("（M31 = 廃止）", "M1 ─→ M2 ─→ M3")
        problems = MODULE.check_roadmap(make_root(roadmap))
        self.assertTrue(any("M2 → M3" in p for p in problems), problems)
        self.assertFalse(any("M1 → M2" in p for p in problems), problems)

    def test_diagram_missing(self) -> None:
        problems = MODULE.check_roadmap(make_root("# Roadmap\n\n### M1: First\n"))
        self.assertTrue(any("依存図" in p for p in problems), problems)

    def test_roadmap_missing(self) -> None:
        problems = MODULE.check_roadmap(Path(tempfile.mkdtemp()))
        self.assertTrue(any("存在しません" in p for p in problems), problems)


class SpecTest(unittest.TestCase):
    def check(self, text: str) -> list[str]:
        return MODULE.check_specs(make_root(specs={"x-spec.md": text}))

    def test_clean_spec_passes(self) -> None:
        self.assertEqual(self.check(SPEC_OK), [])

    def test_acceptance_section_with_list_fails(self) -> None:
        problems = self.check(SPEC_OK + "\n- 条件 A\n- 条件 B\n")
        self.assertTrue(any("箇条書き・表が 2 行" in p for p in problems), problems)

    def test_acceptance_section_with_table_fails(self) -> None:
        problems = self.check(SPEC_OK + "\n| a | b |\n|---|---|\n| 1 | 2 |\n")
        self.assertTrue(any("箇条書き・表が 3 行" in p for p in problems), problems)

    def test_comment_line_in_a_code_block_does_not_end_the_section(self) -> None:
        text = SPEC_OK + "\n```\n# comment\n```\n\n- 条件 A\n"
        problems = self.check(text)
        self.assertTrue(any("箇条書き・表が 1 行" in p for p in problems), problems)

    def test_list_after_the_section_is_allowed(self) -> None:
        self.assertEqual(self.check(SPEC_OK + "\n### 1.2 Next\n\n- item\n"), [])

    def test_date_in_head_fails(self) -> None:
        problems = self.check("# Sample\n\n更新日: x\n\n" + SPEC_OK)
        self.assertTrue(any("作成日・更新日" in p for p in problems), problems)

    def test_date_word_deep_in_the_body_is_allowed(self) -> None:
        self.assertEqual(self.check(SPEC_OK + "\n" * 20 + "更新日: x\n"), [])

    def test_multi_milestone_spec_needs_table(self) -> None:
        text = "# Sample\n\nM1 / M2 / M3 を扱う。\n\n## 1. Purpose\n"
        problems = self.check(text)
        self.assertTrue(any("節とマイルストーンの対応" in p for p in problems), problems)

    def test_table_heading_satisfies(self) -> None:
        text = "# Sample\n\nM1 / M2 / M3 を扱う。\n\n## 0. 節とマイルストーンの対応\n\n## 1. Purpose\n"
        self.assertEqual(self.check(text), [])

    def test_table_header_row_satisfies(self) -> None:
        text = "# Sample\n\nM1 / M2 / M3 を扱う。\n\n| 節 | 内容 | マイルストーン |\n|---|---|---|\n"
        self.assertEqual(self.check(text), [])

    def test_two_milestones_need_no_table(self) -> None:
        self.assertEqual(self.check("# Sample\n\nM1 と M2 を扱う。\n"), [])


class MainTest(unittest.TestCase):
    def test_failure_exit_code(self) -> None:
        code, err = run(make_root(specs={"x-spec.md": "# S\n\n更新日: x\n"}))
        self.assertEqual(code, 1)
        self.assertIn("作成日・更新日", err)

    def test_success_exit_code(self) -> None:
        self.assertEqual(run(make_root())[0], 0)


class RepositoryTest(unittest.TestCase):
    def test_repository_conforms(self) -> None:
        code, err = run(REPO_ROOT)
        self.assertEqual(code, 0, err)


if __name__ == "__main__":
    unittest.main()
