#!/usr/bin/env python3

from __future__ import annotations

import contextlib
import importlib.util
import io
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


sys.dont_write_bytecode = True
SCRIPT = Path(__file__).resolve().parents[1] / "check_skill_impact.py"
SPEC = importlib.util.spec_from_file_location("check_skill_impact", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)

REPO_ROOT = SCRIPT.parents[1]

OWNERSHIP = {
    "tip-skill": ["tip/src/**", "tip/include/**"],
    "ipc-skill": ["ipc/*"],
}

MANIFEST = """\
| Skill | 区分 | origin | 取り込み revision | Codex 側 |
|---|---|---|---|---|
| `tip-skill` | repo | この repo | `abc1234` | あり |
| `ipc-skill` | repo | この repo | `abc1234` | あり |
| `process-skill` | repo | この repo | `abc1234` | あり |

## 担当パス

| Skill | 担当パス |
|---|---|
| `tip-skill` | `tip/src/**`、`tip/include/**` |
| `ipc-skill` | `ipc/*` |
| `process-skill` | — |
"""

CHECKED_WITH_REASON = (
    "## Skill impact\n\n- [x] No Skill impact\n\nReason: 内部の整理でファイル名とテスト target は変わらない\n"
)


class ExtractTest(unittest.TestCase):
    def test_section_ends_at_next_heading(self) -> None:
        body = "## 変更内容\nx\n## Skill impact\n- [x] No Skill impact\nReason: r\n## 検証\nReason: other\n"
        section = MODULE.extract_section(body)
        self.assertIsNotNone(section)
        assert section is not None
        self.assertTrue(MODULE.is_no_impact_checked(section))
        self.assertEqual("r", MODULE.DOCS_IMPACT.extract_reason(section))

    def test_missing_section_is_none(self) -> None:
        self.assertIsNone(MODULE.extract_section("## Documentation impact\n- [x] No Skill impact\n"))

    def test_documentation_impact_checkbox_is_not_skill_impact(self) -> None:
        section = MODULE.extract_section("## Skill impact\n- [x] No documentation impact\n")
        assert section is not None
        self.assertFalse(MODULE.is_no_impact_checked(section))

    def test_crlf_and_comments_are_normalised(self) -> None:
        body = "## Skill impact\r\n- [X] No Skill impact\r\n<!-- Reason: hint -->\r\nReason: 理由あり\r\n"
        section = MODULE.extract_section(body)
        assert section is not None
        self.assertTrue(MODULE.is_no_impact_checked(section))
        self.assertEqual("理由あり", MODULE.DOCS_IMPACT.extract_reason(section))


class EvaluateTest(unittest.TestCase):
    def evaluate(self, changed: list[str], body: str = "") -> tuple[bool, str]:
        passed, messages = MODULE.evaluate(changed, body, OWNERSHIP)
        return passed, "\n".join(messages)

    def test_no_owned_path_changed_passes(self) -> None:
        passed, text = self.evaluate(["docs/x.md", "tip/tests/a_test.cpp"])
        self.assertTrue(passed, text)

    def test_owned_path_without_skill_change_or_body_fails(self) -> None:
        passed, text = self.evaluate(["tip/src/a.cpp"])
        self.assertFalse(passed)
        self.assertIn("tip-skill", text)
        self.assertIn("「## Skill impact」節がありません", text)

    def test_skill_change_in_either_tree_passes(self) -> None:
        for path in (
            ".claude/skills/tip-skill/references/routing.md",
            ".agents/skills/tip-skill/SKILL.md",
        ):
            with self.subTest(path=path):
                passed, text = self.evaluate(["tip/src/a.cpp", path])
                self.assertTrue(passed, text)

    def test_other_skill_change_does_not_cover(self) -> None:
        passed, text = self.evaluate(["tip/src/a.cpp", ".claude/skills/ipc-skill/SKILL.md"])
        self.assertFalse(passed)
        self.assertIn("tip-skill", text)

    def test_each_owning_skill_needs_its_own_update(self) -> None:
        passed, text = self.evaluate(
            ["tip/src/a.cpp", "ipc/Payloads.h", ".claude/skills/tip-skill/SKILL.md"]
        )
        self.assertFalse(passed)
        self.assertIn("ipc-skill", text)
        self.assertNotIn("  - tip-skill", text)

    def test_checked_box_with_reason_passes(self) -> None:
        passed, text = self.evaluate(["tip/src/a.cpp", "ipc/Payloads.h"], CHECKED_WITH_REASON)
        self.assertTrue(passed, text)

    def test_unchecked_box_fails(self) -> None:
        body = CHECKED_WITH_REASON.replace("[x]", "[ ]")
        passed, text = self.evaluate(["tip/src/a.cpp"], body)
        self.assertFalse(passed)
        self.assertIn("チェックが入っていません", text)

    def test_empty_or_placeholder_reason_fails(self) -> None:
        for reason in ("", "影響なし", "N/A", "なし", "<!-- 例: 内部の整理 -->"):
            with self.subTest(reason=reason):
                body = f"## Skill impact\n\n- [x] No Skill impact\n\nReason: {reason}\n"
                passed, text = self.evaluate(["tip/src/a.cpp"], body)
                self.assertFalse(passed)
                self.assertIn("理由がありません", text)

    def test_nested_path_is_not_matched_by_single_segment_glob(self) -> None:
        passed, text = self.evaluate(["ipc/sub/Payloads.h"])
        self.assertTrue(passed, text)


class CommandLineTest(unittest.TestCase):
    def run_main(self, *arguments: str) -> tuple[int, str, str]:
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            try:
                code = MODULE.main(list(arguments))
            except SystemExit as exit_:
                code = int(exit_.code or 0)
        return code, out.getvalue(), err.getvalue()

    def make_repo(self, root: Path) -> None:
        manifest = root / ".claude" / "skills" / "MANIFEST.md"
        manifest.parent.mkdir(parents=True)
        manifest.write_text(MANIFEST, encoding="utf-8")

    def test_changed_files_with_body(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.make_repo(root)
            changed = root / "changed.txt"
            changed.write_text("tip/src/a.cpp\n", encoding="utf-8")
            body = root / "body.md"
            body.write_text("", encoding="utf-8")
            code, _, err = self.run_main(
                "--repo-root", str(root), "--changed-files", str(changed), "--body-file", str(body)
            )
            self.assertEqual(1, code)
            self.assertIn("tip-skill", err)
            body.write_text(CHECKED_WITH_REASON, encoding="utf-8")
            code, out, _ = self.run_main(
                "--repo-root", str(root), "--changed-files", str(changed), "--body-file", str(body)
            )
            self.assertEqual(0, code, out)

    def test_skill_without_paths_is_never_gated(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.make_repo(root)
            self.assertNotIn("process-skill", MODULE.load_ownership(root))

    def test_manifest_without_owned_paths_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.make_repo(root)
            manifest = root / ".claude" / "skills" / "MANIFEST.md"
            manifest.write_text(MANIFEST.replace("## 担当パス", "## 担当 パス"), encoding="utf-8")
            changed = root / "changed.txt"
            changed.write_text("tip/src/a.cpp\n", encoding="utf-8")
            body = root / "body.md"
            body.write_text("", encoding="utf-8")
            code, _, err = self.run_main(
                "--repo-root", str(root), "--changed-files", str(changed), "--body-file", str(body)
            )
            self.assertEqual(1, code)
            self.assertIn("担当パス", err)

    def test_missing_inputs_are_errors(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.make_repo(root)
            code, _, err = self.run_main(
                "--repo-root", str(root), "--changed-files", str(root / "absent"), "--body-file", str(root / "b")
            )
            self.assertEqual(1, code)
            self.assertIn("入力を読めません", err)

    def test_usage_errors_exit_2(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.make_repo(root)
            self.assertEqual(2, self.run_main("--repo-root", str(root), "--body-file", "x")[0])
            self.assertEqual(2, self.run_main("--repo-root", str(root), "--changed-files", "x")[0])
            self.assertEqual(2, self.run_main("--repo-root", str(root), "--freshness", "--body-file", "x")[0])


def git(root: Path, *arguments: str) -> None:
    subprocess.run(
        ["git", "-C", str(root), "-c", "user.name=t", "-c", "user.email=t@example.com", *arguments],
        check=True,
        capture_output=True,
    )


class FreshnessTest(unittest.TestCase):
    def commit(self, root: Path, relative: str, message: str) -> None:
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(message, encoding="utf-8")
        git(root, "add", "--all")
        git(root, "commit", "-m", message)

    def test_counts_commits_since_the_skill_last_changed(self) -> None:
        with tempfile.TemporaryDirectory(ignore_cleanup_errors=True) as directory:
            root = Path(directory)
            git(root, "init", "-q")
            self.commit(root, ".claude/skills/MANIFEST.md", MANIFEST)
            self.commit(root, "tip/src/a.cpp", "before the skill update")
            self.commit(root, ".claude/skills/tip-skill/SKILL.md", "skill update")
            self.commit(root, "tip/src/b.cpp", "after 1")
            self.commit(root, "tip/include/c.h", "after 2")
            self.commit(root, "tip/tests/t.cpp", "tests are not owned")
            self.commit(root, "ipc/a.h", "ipc change")
            rows = {row[0]: row[1] for row in MODULE.freshness(root, MODULE.load_ownership(root), "HEAD")}
            self.assertEqual(2, rows["tip-skill"])
            # ipc-skill has no directory yet, so there is no last update to count from.
            self.assertEqual(-1, rows["ipc-skill"])

    def test_git_range_gate_reads_the_pr_diff(self) -> None:
        with tempfile.TemporaryDirectory(ignore_cleanup_errors=True) as directory:
            root = Path(directory)
            git(root, "init", "-q")
            self.commit(root, ".claude/skills/MANIFEST.md", MANIFEST)
            git(root, "tag", "base")
            self.commit(root, "tip/src/nested/a.cpp", "owned change")
            body = root / "body.md"
            body.write_text("", encoding="utf-8")
            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                code = MODULE.main(
                    ["--repo-root", str(root), "--base", "base", "--head", "HEAD", "--body-file", str(body)]
                )
            self.assertEqual(1, code)
            self.assertIn("tip-skill", err.getvalue())
            self.commit(root, ".agents/skills/tip-skill/SKILL.md", "skill update")
            code = MODULE.main(
                ["--repo-root", str(root), "--base", "base", "--head", "HEAD", "--body-file", str(body)]
            )
            self.assertEqual(0, code)

    def test_cli_prints_table(self) -> None:
        with tempfile.TemporaryDirectory(ignore_cleanup_errors=True) as directory:
            root = Path(directory)
            git(root, "init", "-q")
            self.commit(root, ".claude/skills/MANIFEST.md", MANIFEST)
            self.commit(root, ".agents/skills/ipc-skill/SKILL.md", "agents tree counts as an update")
            self.commit(root, "ipc/a.h", "ipc change")
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                code = MODULE.main(["--repo-root", str(root), "--freshness"])
            self.assertEqual(0, code)
            self.assertRegex(out.getvalue(), r"ipc-skill\s+1\s")


class RepositoryTest(unittest.TestCase):
    def test_repository_ownership_loads(self) -> None:
        ownership = MODULE.load_ownership(REPO_ROOT)
        self.assertIn("tsf-tip-development", ownership)
        self.assertNotIn("azookey-doc-governance", ownership)

    def test_pr_template_has_the_section_the_gate_reads(self) -> None:
        template = (REPO_ROOT / ".github" / "PULL_REQUEST_TEMPLATE.md").read_text(encoding="utf-8")
        section = MODULE.extract_section(template)
        self.assertIsNotNone(section)
        # The template ships unchecked with an empty reason, so a bare copy must fail the gate.
        passed, _ = MODULE.evaluate(["tsf-tip/src/a.cpp"], template, MODULE.load_ownership(REPO_ROOT))
        self.assertFalse(passed)


if __name__ == "__main__":
    unittest.main()
