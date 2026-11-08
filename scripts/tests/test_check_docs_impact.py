#!/usr/bin/env python3

from __future__ import annotations

import contextlib
import importlib.util
import io
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


sys.dont_write_bytecode = True
SCRIPT = Path(__file__).resolve().parents[1] / "check_docs_impact.py"
SPEC = importlib.util.spec_from_file_location("check_docs_impact", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)

REPO_ROOT = SCRIPT.parents[1]
TEMPLATE = REPO_ROOT / ".github" / "PULL_REQUEST_TEMPLATE.md"

CONTRACT = "ipc/include/azookey/ipc/Payloads.h"


def body(checkbox: str = "[ ]", reason: str = "") -> str:
    return (
        "## 変更内容\n\n内部の整理\n\n"
        "## Documentation impact\n\n"
        "- [ ] Spec docs updated\n"
        f"- {checkbox} No documentation impact\n"
        "- [ ] docs-lint run\n\n"
        f"Reason:{reason}\n"
    )


def passes(changed: list[str], text: str) -> bool:
    return MODULE.evaluate(changed, text)[0]


def run_main(arguments: list[str]) -> tuple[int, str]:
    output = io.StringIO()
    with contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
        status = MODULE.main(arguments)
    return status, output.getvalue()


class ContractPathTest(unittest.TestCase):
    def test_listed_contract_files_match(self) -> None:
        for path in (
            CONTRACT,
            "ipc/src/Messages.cpp",
            "settings/mvp-settings.schema.json",
            "settings/default-settings.sample.json",
            "CMakePresets.json",
            ".github/workflows/docs.yml",
        ):
            with self.subTest(path=path):
                self.assertEqual(MODULE.matching([path], MODULE.CONTRACT_PATHS), [path])

    def test_star_does_not_cross_directories(self) -> None:
        for path in (
            "foo/settings/mvp-settings.schema.json",
            "settings/nested/x.schema.json",
            ".github/workflows/sub/x.yml",
            "inference-host/src/Dispatcher.cpp",
            "settings/default-model-catalog.json",
        ):
            with self.subTest(path=path):
                self.assertEqual(MODULE.matching([path], MODULE.CONTRACT_PATHS), [])

    def test_only_root_readme_counts_as_docs(self) -> None:
        self.assertTrue(passes([CONTRACT, "docs/ipc-spec.md"], ""))
        self.assertTrue(passes([CONTRACT, "plans/windows-port-roadmap.md"], ""))
        self.assertTrue(passes([CONTRACT, "README.md"], ""))
        self.assertFalse(passes([CONTRACT, "ipc/README.md"], ""))


class BodyTest(unittest.TestCase):
    def test_no_contract_change_passes_with_empty_body(self) -> None:
        self.assertTrue(passes(["core/src/Romaji.cpp", "docs/x.md"], ""))
        self.assertTrue(passes([], ""))

    def test_checked_box_with_reason_passes(self) -> None:
        for checkbox in ("[x]", "[X]", "[ x ]"):
            with self.subTest(checkbox=checkbox):
                self.assertTrue(
                    passes([CONTRACT], body(checkbox, " 内部の整理で wire 形式は同じ"))
                )

    def test_crlf_body_and_bold_reason_pass(self) -> None:
        text = body("[x]").replace("Reason:", "**Reason:** フィールド順の整理だけ")
        self.assertTrue(passes([CONTRACT], text.replace("\n", "\r\n")))

    def test_reason_on_following_line_passes(self) -> None:
        self.assertTrue(passes([CONTRACT], body("[x]", "\n\naction の pin の更新だけ")))

    def test_template_body_fails(self) -> None:
        template = TEMPLATE.read_text(encoding="utf-8")
        self.assertFalse(passes([CONTRACT], template))
        checked = template.replace(
            "- [ ] No documentation impact", "- [x] No documentation impact"
        )
        self.assertNotEqual(checked, template)
        self.assertFalse(passes([CONTRACT], checked))
        self.assertTrue(passes([CONTRACT], checked + " action の pin の更新だけ\n"))

    def test_comment_or_placeholder_reason_fails(self) -> None:
        for reason in (" <!-- 理由 -->", " なし", " N/A", " 影響なし。", " -", "   "):
            with self.subTest(reason=reason):
                self.assertFalse(passes([CONTRACT], body("[x]", reason)))

    def test_checkbox_and_reason_are_both_required(self) -> None:
        self.assertFalse(passes([CONTRACT], body("[ ]", " 理由はある")))
        self.assertFalse(passes([CONTRACT], body("[x]", "")))

    def test_reason_outside_the_section_does_not_count(self) -> None:
        text = body("[x]") + "\n## 検証\n\nReason: 別の節の理由\n"
        self.assertFalse(passes([CONTRACT], text))

    def test_commented_out_checkbox_does_not_count(self) -> None:
        text = body("[ ]", " 理由").replace(
            "- [ ] Spec", "<!-- - [x] No documentation impact -->\n- [ ] Spec"
        )
        self.assertFalse(passes([CONTRACT], text))

    def test_missing_section_fails_with_guidance(self) -> None:
        passed, messages = MODULE.evaluate([CONTRACT], "## 変更内容\n\nReason: x\n")
        self.assertFalse(passed)
        text = "\n".join(messages)
        self.assertIn(CONTRACT, text)
        self.assertIn("Documentation impact", text)


class MainTest(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.directory)

    def write(self, name: str, text: str) -> Path:
        path = self.directory / name
        path.write_text(text, encoding="utf-8")
        return path

    def test_print_contract_paths_lists_the_constant(self) -> None:
        status, output = run_main(["--print-contract-paths"])
        self.assertEqual(status, 0)
        self.assertEqual(tuple(output.splitlines()), MODULE.CONTRACT_PATHS)

    def test_help_exits_zero(self) -> None:
        with self.assertRaises(SystemExit) as raised:
            run_main(["--help"])
        self.assertEqual(raised.exception.code, 0)

    def test_changed_files_list_ignores_blank_lines(self) -> None:
        changed = self.write("changed.txt", f"\n./{CONTRACT}\r\n\n")
        empty = self.write("body.md", "")
        status, output = run_main(
            ["--changed-files", str(changed), "--body-file", str(empty)]
        )
        self.assertEqual(status, 1)
        self.assertIn(CONTRACT, output)
        reasoned = self.write("reasoned.md", body("[x]", " 内部の整理"))
        status, _ = run_main(
            ["--changed-files", str(changed), "--body-file", str(reasoned)]
        )
        self.assertEqual(status, 0)

    def test_both_or_neither_input_is_a_usage_error(self) -> None:
        changed = self.write("changed.txt", "")
        empty = self.write("body.md", "")
        for arguments in (
            ["--body-file", str(empty)],
            ["--changed-files", str(changed), "--base", "a", "--head", "b",
             "--body-file", str(empty)],
            ["--base", "a", "--body-file", str(empty)],
            ["--changed-files", str(changed)],
        ):
            with self.subTest(arguments=arguments):
                with self.assertRaises(SystemExit) as raised:
                    run_main(arguments)
                self.assertEqual(raised.exception.code, 2)

    def test_missing_body_file_fails(self) -> None:
        changed = self.write("changed.txt", "core/a.cpp\n")
        status, output = run_main(
            ["--changed-files", str(changed), "--body-file",
             str(self.directory / "missing.md")]
        )
        self.assertEqual(status, 1)
        self.assertIn("入力を読めません", output)

    @unittest.skipIf(shutil.which("git") is None, "git is not available")
    def test_git_range_counts_a_renamed_contract_file(self) -> None:
        repo = self.directory / "repo"
        source = repo / "ipc" / "include" / "azookey" / "ipc"
        source.mkdir(parents=True)
        (source / "Payloads.h").write_text("struct A {};\n", encoding="utf-8")

        def git(*arguments: str) -> str:
            return subprocess.run(
                ["git", "-C", str(repo), *arguments],
                check=True, capture_output=True, text=True,
            ).stdout.strip()

        git("init", "-q")
        git("-c", "user.name=t", "-c", "user.email=t@example.com",
            "add", "-A")
        git("-c", "user.name=t", "-c", "user.email=t@example.com",
            "commit", "-q", "-m", "base")
        base = git("rev-parse", "HEAD")
        git("mv", "ipc/include/azookey/ipc/Payloads.h",
            "ipc/include/azookey/ipc/PayloadTypes.h")
        git("-c", "user.name=t", "-c", "user.email=t@example.com",
            "commit", "-q", "-m", "rename")
        head = git("rev-parse", "HEAD")

        empty = self.write("body.md", "")
        status, output = run_main(
            ["--repo-root", str(repo), "--base", base, "--head", head,
             "--body-file", str(empty)]
        )
        self.assertEqual(status, 1)
        self.assertIn(CONTRACT, output)

        status, output = run_main(
            ["--repo-root", str(repo), "--base", base, "--head", "no-such-ref",
             "--body-file", str(empty)]
        )
        self.assertEqual(status, 1)
        self.assertIn("git diff", output)


if __name__ == "__main__":
    unittest.main()
