#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest


sys.dont_write_bytecode = True
SCRIPT = Path(__file__).resolve().parents[1] / "check_skill_references.py"
SPEC = importlib.util.spec_from_file_location("check_skill_references", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)

REPO_ROOT = SCRIPT.parents[1]

MANIFEST = """\
| Skill | 区分 | origin | 取り込み revision | Codex 側 |
|---|---|---|---|---|
| `shared-skill` | shared | `dolquis/agent-ops` | `0cbbe66` | あり |
| `repo-skill` | repo | この repo | `abc1234` | あり |

## 担当パス

| Skill | 担当パス |
|---|---|
| `repo-skill` | `core/tests/*`、`docs/**` |
"""

CMAKE = """\
add_executable(core_tests romaji_test.cpp)
azookey_discover_tests(core_tests)
add_test(NAME azookey_smoke COMMAND true)
"""


def write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def make_repo(root: Path, skill_body: str, manifest: str = MANIFEST) -> None:
    write(root / MODULE.MANIFEST_RELATIVE_PATH, manifest)
    write(root / "core" / "CMakeLists.txt", CMAKE)
    write(root / "core" / "tests" / "romaji_test.cpp", "")
    write(root / "docs" / "spec.md", "")
    write(root / ".claude" / "skills" / "repo-skill" / "SKILL.md", skill_body)
    write(root / ".claude" / "skills" / "repo-skill" / "references" / "routing.md", "")
    write(
        root / ".claude" / "skills" / "shared-skill" / "SKILL.md",
        "shared skills are not checked: `docs/missing.md` `missing_tests`\n",
    )


class CheckSkillReferencesTest(unittest.TestCase):
    def test_valid_references_pass(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            make_repo(
                root,
                "Read `docs/spec.md`, `core/tests/romaji_test.cpp`, `references/routing.md`,\n"
                "run `core_tests` and `azookey_smoke`. Symbols like `Foo::Bar` and\n"
                "`--flag` are ignored, as are external layouts like `crates/x/y.rs`.\n",
            )
            self.assertEqual(MODULE.check(root), [])

    def test_missing_path_target_and_reference_are_reported(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            make_repo(
                root,
                "`docs/gone.md` `core_tests` `dropped_tests` `references/nope.md`\n",
            )
            problems = MODULE.check(root)
            self.assertEqual(len(problems), 3)
            self.assertTrue(any("`docs/gone.md`" in p for p in problems))
            self.assertTrue(any("`dropped_tests`" in p for p in problems))
            self.assertTrue(any("`references/nope.md`" in p for p in problems))

    def test_manifest_and_directories_must_agree(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            make_repo(root, "no claims here\n", manifest=MANIFEST + "| `ghost` | repo | x | y | z |\n")
            write(root / ".claude" / "skills" / "orphan" / "SKILL.md", "")
            problems = MODULE.check(root)
            self.assertTrue(any("`ghost`" in p for p in problems))
            self.assertTrue(any("`orphan`" in p for p in problems))

    def ownership_problems(self, ownership_rows: str) -> list[str]:
        manifest = (
            MANIFEST.split("## 担当パス")[0]
            + "## 担当パス\n\n| Skill | 担当パス |\n|---|---|\n"
            + ownership_rows
        )
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            make_repo(root, "no claims here\n", manifest=manifest)
            return MODULE.check(root)

    def test_ownership_without_paths_passes(self) -> None:
        self.assertEqual([], self.ownership_problems("| `repo-skill` | — |\n"))

    def test_repo_skill_without_ownership_row_is_reported(self) -> None:
        problems = self.ownership_problems("")
        self.assertEqual(1, len(problems), problems)
        self.assertIn("`repo-skill` の行がありません", problems[0])

    def test_ownership_row_for_non_repo_skill_is_reported(self) -> None:
        problems = self.ownership_problems("| `repo-skill` | — |\n| `shared-skill` | `docs/**` |\n")
        self.assertEqual(1, len(problems), problems)
        self.assertIn("`shared-skill` は区分 `repo` の Skill ではありません", problems[0])

    def test_duplicate_ownership_row_is_reported(self) -> None:
        problems = self.ownership_problems("| `repo-skill` | — |\n| `repo-skill` | `docs/**` |\n")
        self.assertEqual(1, len(problems), problems)
        self.assertIn("複数", problems[0])

    def test_glob_matching_nothing_is_reported(self) -> None:
        problems = self.ownership_problems("| `repo-skill` | `core/src/**`、`docs/*` |\n")
        self.assertEqual(1, len(problems), problems)
        self.assertIn("`core/src/**` に一致するパスがありません", problems[0])

    def test_glob_selects_files_like_the_impact_gate(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "empty" / "sub").mkdir(parents=True)
            write(root / "pkg" / "deep" / "a.txt", "")
            write(root / "flat" / "b.txt", "")
            write(root / "flat" / "inner" / "c.txt", "")
            write(root / "only_inner" / "inner" / "d.txt", "")
            matches = MODULE.glob_matches_a_file
            self.assertFalse(matches(root, "empty/**"))  # directories only
            self.assertTrue(matches(root, "pkg/**"))
            self.assertTrue(matches(root, "flat/*"))
            self.assertFalse(matches(root, "only_inner/*"))  # `*` stays in one segment
            self.assertTrue(matches(root, "flat/b.txt"))
            self.assertFalse(matches(root, "flat/inner"))  # a directory is not a file
            self.assertFalse(matches(root, "absent/**"))

    def test_malformed_globs_are_reported(self) -> None:
        for pattern in ("/docs/*", "docs/**/x.md", "docs/..", "docs\\x", "do**/x", "docs//x", "not a glob"):
            with self.subTest(pattern=pattern):
                cell = pattern if " " in pattern else f"`{pattern}`"
                problems = self.ownership_problems(f"| `repo-skill` | {cell} |\n")
                self.assertEqual(1, len(problems), problems)
                self.assertIn("が不正です", problems[0])

    def test_real_repository_is_consistent(self) -> None:
        self.assertEqual(MODULE.check(REPO_ROOT), [])


if __name__ == "__main__":
    unittest.main()
