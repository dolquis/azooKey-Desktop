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
from unittest import mock


sys.dont_write_bytecode = True
SCRIPT = Path(__file__).resolve().parents[1] / "check_doc_refs.py"
SPEC = importlib.util.spec_from_file_location("check_doc_refs", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
# dataclasses resolve string annotations through sys.modules.
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)

REPO_ROOT = SCRIPT.parents[1]

SPEC_DOC = """\
# Sample spec

## 1. Purpose

### 1.2 Scope

- **1.2.3 Item anchor** — numbered items inside a section are anchors.
- **1.2.3a Lettered item** — so are lettered ones.

### 2-1. Hyphenated

## §3 Section-sign heading
"""

SOURCE = """\
namespace azookey {
void TextService::OnKeyDown() {}
}
"""


def write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def make_repo(root: Path, files: dict[str, str]) -> None:
    if shutil.which("git") is None:
        raise unittest.SkipTest("git が無い環境では一時 repo を作れない")
    subprocess.run(["git", "init", "-q", str(root)], check=True)
    base = {
        "README.md": "",
        "docs/sample-spec.md": SPEC_DOC,
        "docs/decisions/0001-record.md": "",
        "tsf-tip/src/TextService.cpp": SOURCE,
        "core/src/Foo.h": "",
        ".gitignore": "build/\ndiagnostics/captures/\n",
    }
    base.update(files)
    for relative, text in base.items():
        write(root / relative, text)


def problems_for(files: dict[str, str]) -> list[str]:
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        make_repo(root, files)
        return MODULE.check(root)


class PathReferenceTest(unittest.TestCase):
    def test_existing_paths_pass(self) -> None:
        self.assertEqual(
            problems_for(
                {
                    "docs/guide.md": (
                        "`core/src/Foo.h` `core/src/Foo.h:12` `docs/sample-spec.md#1-purpose`\n"
                        "`core/` `./core/src/Foo.h` `core\\src\\Foo.h` `README.md`\n"
                        "Relative to the doc: `decisions/0001-record.md` `../README.md`.\n"
                        "Globs and elisions: `core/src/*.h` `tsf-tip/.../TextService.cpp`.\n"
                    ),
                }
            ),
            [],
        )

    def test_non_path_spans_are_ignored(self) -> None:
        self.assertEqual(
            problems_for(
                {
                    "docs/guide.md": (
                        "`foo/bar.cpp` `<build>/core/x.exe` `%LOCALAPPDATA%\\azooKey\\x` "
                        "`https://example.com/core/x` `core/a b.cpp` `--flag` `core/{a,b}.h` "
                        "`$HOME/core/x` `Foo::Bar` `core`\n"
                    ),
                }
            ),
            [],
        )

    def test_missing_path_and_glob_are_reported(self) -> None:
        problems = problems_for(
            {"docs/guide.md": "`core/src/Gone.cpp` `core/src/*.rs` `decisions/none.md`\n"}
        )
        self.assertEqual(len(problems), 3, problems)
        self.assertTrue(any("docs/guide.md:1:" in p and "`core/src/Gone.cpp`" in p for p in problems))
        self.assertTrue(any("glob" in p and "`core/src/*.rs`" in p for p in problems))
        self.assertTrue(any("`docs/decisions/none.md`" in p for p in problems))

    def test_code_fences_and_frozen_documents_are_skipped(self) -> None:
        self.assertEqual(
            problems_for(
                {
                    "docs/guide.md": "```\n`core/src/Gone.cpp`\n```\n",
                    "docs/archive/old.md": "`core/src/Gone.cpp`\n",
                    "docs/decisions/0002-old.md": "`core/src/Gone.cpp`\n",
                    "docs/handoff/docs-refactoring-plan.md": "`core/src/Gone.cpp`\n",
                    "core/README.md": "`core/src/Gone.cpp` is not a target document\n",
                }
            ),
            [],
        )

    def test_gitignored_paths_are_accepted(self) -> None:
        self.assertEqual(
            problems_for({"docs/guide.md": "`diagnostics/captures` `build/windows-debug/x.exe`\n"}),
            [],
        )

    def test_crlf_gitignore_blank_lines_do_not_hide_missing_paths(self) -> None:
        # CRLF の .gitignore（Windows の autocrlf checkout）では、git が空行を空パターンとして
        # 読み、末尾 "/" の問い合わせへ一致する。存在しないパスまで無視扱いになってはならない（DEV-2012）。
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            make_repo(root, {"docs/guide.md": "`core/src/Gone.cpp` `diagnostics/captures`\n"})
            (root / ".gitignore").write_bytes(b"build/\r\n\r\ndiagnostics/captures/\r\n\r\n")
            problems = MODULE.check(root)
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("`core/src/Gone.cpp`", problems[0])

    def test_ignored_paths_sends_nul_separated_bytes_to_git(self) -> None:
        # text=True で stdin を渡すと Windows が "\n" を "\r\n" に変え、git が CR 付きのパスを受け取る
        # （DEV-2012）。どの OS でも、改行変換の掛からない NUL 区切りの bytes で渡すことを固定する。
        sent: list[bytes] = []
        commands: list[list[str]] = []
        text_modes: list[object] = []

        def fake_run(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[bytes]:
            commands.append(command)
            text_modes.append(kwargs.get("text"))
            payload = kwargs.get("input")
            assert isinstance(payload, bytes), type(payload)
            sent.append(payload)
            return subprocess.CompletedProcess(
                command,
                0,
                stdout=b".claude/worktrees/__doc_refs_probe__\0core/src/Foo.h\0",
                stderr=b"",
            )

        with mock.patch.object(MODULE.subprocess, "run", fake_run):
            ignored = MODULE.ignored_paths(
                Path("."), {".claude/worktrees", "core/src/Foo.h::Bar", "tsf-tip/x.h"}
            )

        self.assertEqual(ignored, {".claude/worktrees", "core/src/Foo.h"})
        self.assertEqual(len(sent), 1)
        self.assertIn("-z", commands[0])
        self.assertFalse(text_modes[0])
        self.assertNotIn(b"\r", sent[0])
        self.assertNotIn(b"\n", sent[0])
        queries = sent[0].decode("utf-8").split("\0")
        self.assertIn("core/src/Foo.h", queries)
        self.assertIn(".claude/worktrees/__doc_refs_probe__", queries)
        self.assertFalse(any(query.endswith("/") for query in queries), queries)

    def test_target_documents_cover_rules_and_plans(self) -> None:
        problems = problems_for(
            {
                ".claude/rules/x.md": "`core/src/Gone.cpp`\n",
                "plans/roadmap.md": "`core/src/Gone.cpp`\n",
                "AGENTS.md": "`core/src/Gone.cpp`\n",
            }
        )
        self.assertEqual(len(problems), 3, problems)


class SymbolReferenceTest(unittest.TestCase):
    def test_symbol_present_passes(self) -> None:
        self.assertEqual(
            problems_for(
                {
                    "docs/guide.md": (
                        "`tsf-tip/src/TextService.cpp::TextService::OnKeyDown` "
                        "`TextService.cpp::OnKeyDown()`\n"
                    )
                }
            ),
            [],
        )

    def test_missing_symbol_and_file_are_reported(self) -> None:
        problems = problems_for(
            {
                "docs/guide.md": (
                    "`tsf-tip/src/TextService.cpp::OnKeyUp` `TextService.cpp::OnTap` "
                    "`core/src/Gone.cpp::Run` `Nowhere.cpp::Run`\n"
                )
            }
        )
        self.assertEqual(len(problems), 4, problems)
        self.assertTrue(any("`Nowhere.cpp::Run`" in p for p in problems))
        self.assertTrue(any("`OnKeyUp`" in p for p in problems))
        self.assertTrue(any("`tsf-tip/src/TextService.cpp::OnTap`" in p for p in problems))
        self.assertTrue(any("`core/src/Gone.cpp::Run`" in p for p in problems))

    def test_ambiguous_bare_file_name_is_not_checked(self) -> None:
        self.assertEqual(
            problems_for(
                {
                    "core/src/TextService.cpp": "",
                    "docs/guide.md": "`TextService.cpp::Anything`\n",
                }
            ),
            [],
        )


class SectionReferenceTest(unittest.TestCase):
    def test_resolving_sections_pass(self) -> None:
        self.assertEqual(
            problems_for(
                {
                    "docs/guide.md": (
                        "`docs/sample-spec.md` §1.2、§2-1 と §3。sample-spec §1.2.3、"
                        "[sample-spec.md](sample-spec.md) §1.2.3a。\n"
                        "続けて同 §1 を見る。\n"
                    ),
                    "core/src/Foo.cpp": (
                        "// docs/sample-spec.md §1.2 and sample-spec.md section 1\n"
                        "// see the sample-\n// spec §1.2 wrapped\n"
                        "// Sample-Spec §1 in another case\n"
                    ),
                }
            ),
            [],
        )

    def test_unresolved_sections_are_reported(self) -> None:
        problems = problems_for(
            {
                "docs/guide.md": "`docs/sample-spec.md` §1.3、§4。missing-spec §1。\n同 §9。\n",
                "core/src/Foo.cpp": "// sample-spec §5\n",
                "core/src/Bar.cpp": "// sample-\n// spec section 6\n",
                "scripts/tool.py": "# sample-spec.md §7\n",
            }
        )
        expected = [
            "docs/guide.md:1:",
            "docs/sample-spec.md §4",
            "docs/missing-spec.md §1",
            "docs/guide.md:2:",
            "core/src/Foo.cpp:1:",
            "core/src/Bar.cpp:2:",
            "scripts/tool.py:1:",
        ]
        self.assertEqual(len(problems), 7, problems)
        for fragment in expected:
            self.assertTrue(any(fragment in p for p in problems), fragment)
        # 同 follows the document named last in the paragraph.
        self.assertTrue(any("docs/missing-spec.md §9" in p for p in problems))

    def test_same_document_follows_position_and_not_table_rows(self) -> None:
        problems = problems_for(
            {
                "docs/other-spec.md": "# Other\n\n## 9. Only\n",
                "docs/guide.md": (
                    "`docs/sample-spec.md` §1 の後に（同 §2-1）、`docs/other-spec.md` §9。\n"
                    "\n"
                    "| a | `docs/other-spec.md` §9 |\n"
                    "| b | 同 §1 |\n"
                ),
            }
        )
        self.assertEqual(problems, [])

    def test_section_inside_code_fence_is_ignored(self) -> None:
        self.assertEqual(
            problems_for({"docs/guide.md": "```\nsample-spec §99\n```\n"}),
            [],
        )


class AllowlistTest(unittest.TestCase):
    def test_allowlist_suppresses_and_scopes(self) -> None:
        allowlist = (
            '[[allow]]\nref = "core/src/Planned.cpp"\nkind = "planned"\n'
            'milestone = "M99"\nspec = "docs/sample-spec.md §1"\nreason = "M99 で作る"\n'
            'files = ["docs/guide.md"]\n'
        )
        self.assertEqual(
            problems_for(
                {
                    MODULE.ALLOWLIST_RELATIVE_PATH: allowlist,
                    "docs/guide.md": "`core/src/Planned.cpp`\n",
                }
            ),
            [],
        )
        problems = problems_for(
            {
                MODULE.ALLOWLIST_RELATIVE_PATH: allowlist,
                "docs/guide.md": "`core/src/Planned.cpp`\n",
                "docs/other.md": "`core/src/Planned.cpp`\n",
            }
        )
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("docs/other.md:1:", problems[0])

    def test_existing_and_unused_entries_fail(self) -> None:
        allowlist = (
            '[[allow]]\nref = "core/src/Foo.h"\nkind = "historical"\nreason = "x"\n'
            '[[allow]]\nref = "core/src/Unused.cpp"\nkind = "historical"\nreason = "x"\n'
            '[[allow]]\nref = "docs/sample-spec.md §1"\nkind = "historical"\nreason = "x"\n'
        )
        problems = problems_for(
            {MODULE.ALLOWLIST_RELATIVE_PATH: allowlist, "docs/guide.md": "`core/src/Foo.h`\n"}
        )
        self.assertEqual(len(problems), 3, problems)
        self.assertTrue(any("`core/src/Foo.h` は実在する" in p for p in problems))
        self.assertTrue(any("`docs/sample-spec.md §1` は実在する" in p for p in problems))
        self.assertTrue(any("`core/src/Unused.cpp` を参照する文書が無い" in p for p in problems))

    def test_malformed_entries_are_reported(self) -> None:
        allowlist = (
            '[[allow]]\nref = "core/src/A.cpp"\nkind = "planned"\nreason = "x"\n'
            '[[allow]]\nref = "core/src/B.cpp"\nkind = "someday"\nreason = ""\n'
            'files = ["docs/nowhere.md"]\n'
            '[[allow]]\nkind = "origin"\nreason = "x"\n'
            '[[allow]]\nref = "core/src/C.cpp"\nkind = "unowned"\nreason = "x"\n'
            '[[allow]]\nref = "core/src/D.cpp"\nkind = "unowned"\nreason = "x"\nissue = "1912"\n'
        )
        problems = problems_for(
            {
                MODULE.ALLOWLIST_RELATIVE_PATH: allowlist,
                "docs/guide.md": "`core/src/A.cpp` `core/src/B.cpp` `core/src/C.cpp` `core/src/D.cpp`\n",
            }
        )
        joined = "\n".join(problems)
        self.assertIn("`issue` が要る", joined)
        self.assertIn("Linear の課題 ID", joined)
        self.assertIn("`milestone` が要る", joined)
        self.assertIn("`spec` が要る", joined)
        self.assertIn("`kind` は", joined)
        self.assertIn("`reason` が無い", joined)
        self.assertIn("`ref` が無い", joined)
        self.assertIn("存在しない文書 `docs/nowhere.md`", joined)


    def test_planned_spec_must_resolve(self) -> None:
        allowlist = (
            '[[allow]]\nref = "core/src/A.cpp"\nkind = "planned"\nmilestone = "M1"\n'
            'spec = "docs/sample-spec.md §8"\nreason = "x"\n'
            '[[allow]]\nref = "core/src/B.cpp"\nkind = "planned"\nmilestone = "M1"\n'
            'spec = "docs/gone-spec.md X-1-2"\nreason = "x"\n'
        )
        problems = problems_for(
            {
                MODULE.ALLOWLIST_RELATIVE_PATH: allowlist,
                "docs/guide.md": "`core/src/A.cpp` `core/src/B.cpp`\n",
            }
        )
        self.assertEqual(len(problems), 2, problems)
        self.assertTrue(any("`docs/sample-spec.md §8` が見出しに解決しない" in p for p in problems))
        self.assertTrue(any("存在しない文書 `docs/gone-spec.md`" in p for p in problems))


class LineExemptionTest(unittest.TestCase):
    def test_marker_exempts_its_line_or_the_next(self) -> None:
        self.assertEqual(
            problems_for(
                {
                    "docs/guide.md": (
                        "`core/src/Gone.cpp` <!-- doc-refs:allow 例示 -->\n"
                        "<!-- doc-refs:allow -->\n"
                        "`core/src/Also.cpp`\n"
                    )
                }
            ),
            [],
        )

    def test_marker_that_exempts_nothing_fails(self) -> None:
        problems = problems_for({"docs/guide.md": "`core/src/Foo.h` <!-- doc-refs:allow -->\n"})
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("docs/guide.md:1:", problems[0])


def forwarding_table(*rows: str) -> str:
    return "## 転送表\n\n| 旧節 | 移動先 |\n|---|---|\n" + "".join(f"{row}\n" for row in rows)


OLD_SPEC = (
    "# Old spec\n\n## 1. Stays\n\n"
    + forwarding_table("| §2、§4〜§5 | `docs/new-spec.md` |", "| §7 | `docs/other-spec.md` §17 |")
)
NEW_SPEC = "# New spec\n\n## 2. Moved\n\n### 2.1 Sub\n\n## 4. Four\n\n## 5. Five\n"
OTHER_SPEC = "# Other spec\n\n## 17. Renumbered\n\n### 17.3 Deep\n"
FORWARD_FILES = {
    "docs/old-spec.md": OLD_SPEC,
    "docs/new-spec.md": NEW_SPEC,
    "docs/other-spec.md": OTHER_SPEC,
}


class ForwardingTableTest(unittest.TestCase):
    def test_sections_resolve_through_the_table(self) -> None:
        guide = "old-spec §1 old-spec §2.1 old-spec §4 old-spec §5 old-spec §7 old-spec §7.3\n"
        self.assertEqual(problems_for({**FORWARD_FILES, "docs/guide.md": guide}), [])

    def test_missing_sections_are_reported(self) -> None:
        guide = "old-spec §3 old-spec §6.1 old-spec §7.9 old-spec §30\n"
        problems = problems_for({**FORWARD_FILES, "docs/guide.md": guide})
        self.assertEqual(len(problems), 4, problems)
        self.assertTrue(any("`docs/old-spec.md §3`" in p for p in problems))
        self.assertTrue(any("`docs/old-spec.md §6.1`" in p for p in problems))
        self.assertTrue(any("`docs/old-spec.md §30`" in p for p in problems))
        renumbered = [p for p in problems if "`docs/old-spec.md §7.9`" in p]
        self.assertEqual(len(renumbered), 1, problems)
        self.assertIn("転送先 `docs/other-spec.md` に §17.9 が無い", renumbered[0])

    def test_longest_key_wins_and_renumbers_the_rest(self) -> None:
        files = {
            "docs/old-spec.md": "# Old\n\n"
            + forwarding_table("| §3 | `docs/new-spec.md` |", "| §3.2 | `docs/other-spec.md` §17 |"),
            "docs/new-spec.md": "# New\n\n## 3. Three\n\n### 3.1 One\n",
            "docs/other-spec.md": "# Other\n\n## 17. Seventeen\n\n### 17.1 One\n",
            "docs/guide.md": "old-spec §3.1 old-spec §3.2.1 old-spec §3a\n",
        }
        problems = problems_for(files)
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("`docs/old-spec.md §3a`", problems[0])

    def test_rows_are_validated(self) -> None:
        table = forwarding_table(
            "| §9 | `docs/gone-spec.md` |",
            "| §8 | `core/src/Foo.h` |",
            "| §6 | `docs/new-spec.md` |",
            "| §3 | `docs/old-spec.md` |",
            "| §3〜§1 | `docs/new-spec.md` |",
            "| 第3節 | `docs/new-spec.md` |",
            "| §4、§5 | `docs/new-spec.md` §4 |",
            "| §2 | docs/new-spec.md |",
        )
        problems = [
            p for p in problems_for({**FORWARD_FILES, "docs/old-spec.md": "# Old\n\n" + table})
            if "転送" in p
        ]
        self.assertEqual(len(problems), 8, problems)
        for fragment in (
            "転送先 `docs/gone-spec.md` が存在しない",
            "転送先 `core/src/Foo.h` は docs/ の .md に限る",
            "転送先 `docs/new-spec.md` の見出しに §6 が無い",
            "転送先が自分自身",
            "範囲 `§3〜§1` が昇順の整数でない",
            "節 `第3節` を読めない",
            "複数の節の行は転送先の § を書けない",
            "転送先 `docs/new-spec.md` を読めない",
        ):
            self.assertTrue(any(fragment in p for p in problems), (fragment, problems))

    def test_malformed_tables_are_reported(self) -> None:
        cases = {
            "## 転送表\n\n本文だけ\n\n## 次\n": "転送表の見出しの下に表が無い",
            "## 転送表\n\n| 節 | 先 |\n|---|---|\n| §2 | `docs/new-spec.md` |\n": "`| 旧節 | 移動先 |`",
            "## 転送表\n\n| 旧節 | 移動先 |\n| §2 | `docs/new-spec.md` |\n": "区切り行",
            forwarding_table("| §2 | `docs/new-spec.md` |", "| §2 | `docs/other-spec.md` |"): "`2` が重複",
            forwarding_table("| §2 |"): "2 列",
        }
        for body, fragment in cases.items():
            problems = problems_for({**FORWARD_FILES, "docs/old-spec.md": "# Old\n\n" + body})
            self.assertTrue(any(fragment in p for p in problems), (fragment, problems))

    def test_table_in_a_code_fence_is_only_a_sample(self) -> None:
        sample = "```\n" + forwarding_table("| §9 | `docs/gone-spec.md` |") + "```\n"
        self.assertEqual(problems_for({"docs/guide.md": sample}), [])

    def test_cycles_and_long_chains_are_reported(self) -> None:
        def stub(destination: str | None) -> str:
            table = forwarding_table(f"| §9 | `docs/{destination}.md` |") if destination else ""
            return "# Doc\n\n" + ("" if destination else "## 9. Here\n\n") + table

        cycle = {"docs/a-spec.md": stub("b-spec"), "docs/b-spec.md": stub("a-spec")}
        problems = problems_for({**cycle, "docs/guide.md": "a-spec §9\n"})
        self.assertTrue(any("循環" in p for p in problems), problems)

        names = ["d1-spec", "d2-spec", "d3-spec", "d4-spec", "d5-spec"]
        three_hops = {f"docs/{n}.md": stub(names[i + 1]) for i, n in enumerate(names[:3])}
        three_hops["docs/d4-spec.md"] = stub(None)
        self.assertEqual(problems_for({**three_hops, "docs/guide.md": "d1-spec §9\n"}), [])
        four_hops = {f"docs/{n}.md": stub(names[i + 1]) for i, n in enumerate(names[:4])}
        four_hops["docs/d5-spec.md"] = stub(None)
        problems = problems_for({**four_hops, "docs/guide.md": "d1-spec §9\n"})
        self.assertTrue(any("3 段を超える" in p for p in problems), problems)

    def test_a_key_covers_only_its_own_subsections(self) -> None:
        table = forwarding_table(
            "| §4 | `docs/new-spec.md` |",
            "| §7 | `docs/new-spec.md` |",
            "| §6.4.1 | `docs/new-spec.md` |",
            "| X-3 | `docs/new-spec.md` |",
        )
        new_spec = (
            "# New\n\n## 4. Four\n\n### 4.3a Letter\n\n## 7. Seven\n\n### 7-3. Hyphen\n\n"
            "### 6.4.1 Base\n\n### 6.4.1a Lettered\n\n## X-3. Items\n\n### X-3-6. Item\n"
        )
        guide = "old-spec §4.3a old-spec §7-3 old-spec §6.4.1a\nold-spec §40 old-spec §6.4.2 X-3-6 X-30-1\n"
        problems = problems_for(
            {
                "docs/old-spec.md": "# Old\n\n" + table,
                "docs/new-spec.md": new_spec,
                "docs/guide.md": guide,
            }
        )
        self.assertEqual(len(problems), 3, problems)
        for fragment in ("`docs/old-spec.md §40`", "`docs/old-spec.md §6.4.2`", "`X-30-1`"):
            found = [p for p in problems if fragment in p]
            self.assertEqual(len(found), 1, (fragment, problems))
            self.assertNotIn("転送先", found[0])

    def test_forwarding_heading_may_be_numbered_and_the_table_must_be_one(self) -> None:
        row = "| 旧節 | 移動先 |\n|---|---|\n| §2 | `docs/new-spec.md` |\n"
        files = {
            "docs/old-spec.md": "# Old\n\n## 15. 転送表\n\n" + row,
            "docs/new-spec.md": NEW_SPEC,
            "docs/guide.md": "old-spec §2.1\n",
        }
        self.assertEqual(problems_for(files), [])
        split = "# Old\n\n## 転送表\n\n" + row + "\n| §4 | `docs/new-spec.md` |\n"
        problems = problems_for({**files, "docs/old-spec.md": split})
        self.assertTrue(any("別の表が続く" in p for p in problems), problems)
        # A second table is fine under a `転送表` heading of its own.
        second = "| 旧節 | 移動先 |\n|---|---|\n| §4 | `docs/new-spec.md` |\n"
        two_headings = "# Old\n\n## 転送表\n\n" + row + "\n## 15. 転送表\n\n" + second
        files_two = {
            **files,
            "docs/old-spec.md": two_headings,
            "docs/guide.md": "old-spec §2.1 old-spec §4\n",
        }
        self.assertEqual(problems_for(files_two), [])

    def test_missing_section_names_the_last_destination(self) -> None:
        def stub(destination: str | None) -> str:
            if destination is None:
                return "# Doc\n"
            return "# Doc\n\n" + forwarding_table(f"| §9 | `docs/{destination}.md` |")

        chain = {
            "docs/a-spec.md": stub("b-spec"),
            "docs/b-spec.md": stub("c-spec"),
            "docs/c-spec.md": stub(None),
        }
        problems = problems_for({**chain, "docs/guide.md": "a-spec §9\n"})
        found = [p for p in problems if p.startswith("docs/guide.md:1:")]
        self.assertEqual(len(found), 1, problems)
        self.assertIn("転送先 `docs/c-spec.md` に §9 が無い", found[0])
        self.assertNotIn("`docs/b-spec.md`", found[0])

        cycle = {"docs/a-spec.md": stub("b-spec"), "docs/b-spec.md": stub("a-spec")}
        problems = problems_for(cycle)
        self.assertTrue(
            any(p.startswith("docs/a-spec.md:") and "循環" in p for p in problems), problems
        )

    def test_planned_spec_resolves_through_the_table(self) -> None:
        allowlist = (
            '[[allow]]\nref = "core/src/Planned.cpp"\nkind = "planned"\n'
            'milestone = "M99"\nspec = "docs/old-spec.md §2.1"\nreason = "M99 で作る"\n'
        )
        problems = problems_for(
            {
                **FORWARD_FILES,
                MODULE.ALLOWLIST_RELATIVE_PATH: allowlist,
                "docs/guide.md": "`core/src/Planned.cpp`\n",
            }
        )
        self.assertEqual(problems, [])


RICH_SPEC = "# Rich\n\n## X-1. A\n\n### X-1-2. Tempo\n\n### X-3-6. Batch\n"


class ItemReferenceTest(unittest.TestCase):
    def test_items_with_a_heading_resolve(self) -> None:
        files = {
            "docs/rich-features-spec.md": RICH_SPEC,
            "docs/guide.md": "X-1-2 と X-3-6。\n",
            "tsf-tip/src/Note.cpp": "// see X-3-6\n",
            ".claude/skills/demo/references/x.md": "X-1-2\n",
        }
        self.assertEqual(problems_for(files), [])

    def test_missing_items_are_reported_outside_frozen_and_test_files(self) -> None:
        files = {
            "docs/rich-features-spec.md": RICH_SPEC,
            "docs/guide.md": "X-9-9 と X-9-9。\n\n```\nX-8-8\n```\n",
            "tsf-tip/src/Note.cpp": "// see X-9-9\n",
            ".claude/skills/demo/references/x.md": "X-9-9\n",
            "scripts/tests/fixture.py": "# X-9-9\n",
            "docs/archive/old.md": "X-9-9\n",
            "docs/decisions/0002-x.md": "X-9-9\n",
            "docs/words.md": "PREFIX-9-9 X-9 X-9-9a\n",
        }
        problems = problems_for(files)
        self.assertEqual(len(problems), 3, problems)
        for location in ("docs/guide.md:1:", "tsf-tip/src/Note.cpp:1:", ".claude/skills/demo/references/x.md:1:"):
            self.assertTrue(any(location in p and "`X-9-9`" in p for p in problems), (location, problems))

    def test_items_resolve_through_a_forwarding_table(self) -> None:
        stub = "# Rich\n\n" + forwarding_table(
            "| X-3-6 | `docs/typo-spec.md` §4 |", "| X-1 | `docs/live-spec.md` |"
        )
        files = {
            "docs/rich-features-spec.md": stub,
            "docs/typo-spec.md": "# Typo\n\n## 4. Batch\n",
            "docs/live-spec.md": "# Live\n\n## X-1. Live\n\n### X-1-2. Tempo\n",
            "docs/guide.md": "X-3-6 X-1-2 X-3-7\n",
        }
        problems = problems_for(files)
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("docs/guide.md:1:", problems[0])
        self.assertIn("`X-3-7`", problems[0])

    def test_a_table_outside_the_item_document_counts(self) -> None:
        files = {
            "docs/moved.md": "# Moved\n\n" + forwarding_table("| X-3-6 | `docs/typo-spec.md` §4 |"),
            "docs/typo-spec.md": "# Typo\n\n## 4. Batch\n",
            "docs/guide.md": "X-3-6\n",
        }
        self.assertEqual(problems_for(files), [])

    def test_allowlist_covers_an_item(self) -> None:
        allowlist = '[[allow]]\nref = "X-9-9"\nkind = "historical"\nreason = "旧版の項番"\n'
        files = {"docs/guide.md": "X-9-9\n", MODULE.ALLOWLIST_RELATIVE_PATH: allowlist}
        self.assertEqual(problems_for(files), [])
        self.assertEqual(len(problems_for({MODULE.ALLOWLIST_RELATIVE_PATH: allowlist})), 1)


class ShortNameReferenceTest(unittest.TestCase):
    SPECS = {
        "docs/app-profile-spec.md": "# App\n\n## 3. Three\n\n### 3.1 One\n",
        "docs/linear-conventions.md": "# Linear\n\n## 2. Two\n\n### 2.1 One\n",
        "docs/sideload-packaging-spec.md": "# Sideload\n\n## 3. Three\n",
        "docs/handoff/hyper-v-plan.md": "# Hyper-V\n\n## 4. Four\n",
    }

    def test_short_names_resolve_to_one_document(self) -> None:
        guide = (
            "app-profile §3.1 と `app-profile` §3、linear-conventions §2.1、"
            "linear-conventions.md §2、sideload §3、hyper-v-plan §4\n"
        )
        self.assertEqual(problems_for({**self.SPECS, "docs/guide.md": guide}), [])

    def test_unresolved_short_name_sections_are_reported(self) -> None:
        guide = "app-profile §9.9 と同 §8 / linear-conventions §7 / sideload §1\n"
        problems = problems_for({**self.SPECS, "docs/guide.md": guide})
        self.assertEqual(len(problems), 4, problems)
        for reference in (
            "`docs/app-profile-spec.md §9.9`",
            "`docs/app-profile-spec.md §8`",
            "`docs/linear-conventions.md §7`",
            "`docs/sideload-packaging-spec.md §1`",
        ):
            self.assertTrue(any(reference in p for p in problems), (reference, problems))

    def test_names_that_resolve_to_nothing_are_prose(self) -> None:
        files = {
            "docs/barz-a-spec.md": "# A\n",
            "docs/guide.md": "M60 §3 and §4、the model §2、abc §3、tsf-tip §5\n",
        }
        self.assertEqual(problems_for(files), [])

    def test_a_name_shared_by_several_documents_is_ambiguous(self) -> None:
        files = {
            "docs/barz-a-spec.md": "# A\n\n## 1. One\n",
            "docs/dual-spec.md": "# Dual\n\n## 1. One\n",
            "docs/dual.md": "# Dual\n",
            "docs/guide.md": "barz §1 と同 §2、barz §3、§4、dual §1\n",
        }
        # One candidate: checked as usual.
        self.assertEqual(problems_for({**files, "docs/guide.md": "barz §1\n"}), [])
        # A second spec with the same prefix: the reference is reported, not skipped.
        files["docs/barz-b-spec.md"] = "# B\n\n## 1. One\n"
        problems = problems_for(files)
        self.assertEqual(len(problems), 4, problems)
        for reference in ("`barz §1`", "`barz §3`", "`barz §4`", "`dual §1`"):
            self.assertTrue(any(reference in p for p in problems), (reference, problems))
        self.assertTrue(all("曖昧な短縮名" in p for p in problems), problems)
        self.assertTrue(any("`docs/barz-a-spec.md`、`docs/barz-b-spec.md`" in p for p in problems))
        self.assertTrue(any("`docs/dual-spec.md`、`docs/dual.md`" in p for p in problems))

    def test_prefix_needs_four_letters_and_root_names_stay_prose(self) -> None:
        files = {
            "docs/abc-x-spec.md": "# Abc\n",
            "docs/abcd-x-spec.md": "# Abcd\n",
            "docs/README.md": "# Index\n",
            "docs/AGENTS.md": "# Agents\n",
            "docs/guide.md": "abc §3 abcd §3 README §1 AGENTS §3 CLAUDE §2\n",
        }
        problems = problems_for(files)
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("`docs/abcd-x-spec.md §3`", problems[0])

    def test_short_name_in_a_code_comment(self) -> None:
        files = {**self.SPECS, "tsf-tip/src/Note.cpp": "// app-profile §3.1 / app-profile §9.9\n"}
        problems = problems_for(files)
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("tsf-tip/src/Note.cpp:1:", problems[0])
        self.assertIn("`docs/app-profile-spec.md §9.9`", problems[0])


GROUP_OLD = (
    "# Old spec\n\n## 1. Stays\n\n"
    "§1 を見る。\n"
    "§4.2 は新しい文書にある。\n"
    "§9 は無い。\n"
    "§1、§4.2、§5 と並べる。\n"
    "§1、§9 と並べる。\n"
    "§9 は例外。 <!-- doc-refs:allow -->\n\n"
    + forwarding_table("| §4〜§5 | `docs/new-spec.md` |")
)
GROUP_NEW = (
    "# New spec\n\n## 4. Four\n\n### 4.2 Sub\n\n## 5. Five\n\n"
    "§1 は旧文書にある。\n"
    "§8 は無い。\n"
    "同 §8 と同 §7、x-spec §8 は別文書を指す。\n"
    "M60 §99 と **app-profile** §98 と `app-profile` §97 と DEV-1975 §96 は別文書を指す。\n"
    "詳しくは docs/other-spec.md の\n"
    "§95 を見る。\n"
)


class ForwardingGroupTest(unittest.TestCase):
    def test_bare_sections_resolve_across_the_group(self) -> None:
        files = {
            "docs/old-spec.md": GROUP_OLD,
            "docs/new-spec.md": GROUP_NEW,
            "docs/solo-spec.md": "# Solo\n\n§9 は無い。\n",
        }
        problems = [p for p in problems_for(files) if "x-spec" not in p]
        self.assertEqual(len(problems), 3, problems)
        # `§9` alone, and `§9` as the second item of a list whose head is local.
        old = [p for p in problems if "`docs/old-spec.md §9`" in p]
        self.assertEqual(sorted(p.split(":")[1] for p in old), ["7", "9"], problems)
        self.assertTrue(any("docs/new-spec.md:" in p and "`docs/new-spec.md §8`" in p for p in problems))

    def test_renumbering_rows_do_not_form_a_group(self) -> None:
        files = {
            "docs/old-spec.md": "# Old\n\n" + forwarding_table("| §1 | `docs/moved-spec.md` §11 |"),
            "docs/moved-spec.md": "# Moved\n\n## 11. Here\n\n§99 は無い。\n",
        }
        self.assertEqual(problems_for(files), [])

    def test_documents_outside_a_group_are_not_checked_for_bare_sections(self) -> None:
        files = {"docs/solo-spec.md": "# Solo\n\n## 1. One\n\n§9 は無い。\n"}
        self.assertEqual(problems_for(files), [])


class CommandTest(unittest.TestCase):
    def test_exit_status(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            make_repo(root, {"docs/guide.md": "`core/src/Foo.h`\n"})
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(MODULE.main(["--repo-root", str(root)]), 0)
            write(root / "docs" / "guide.md", "`core/src/Gone.cpp`\n")
            with contextlib.redirect_stderr(io.StringIO()) as stderr:
                self.assertEqual(MODULE.main(["--repo-root", str(root)]), 1)
            self.assertIn("core/src/Gone.cpp", stderr.getvalue())

    def test_paths_under_a_submodule_are_not_looked_into(self) -> None:
        repo = MODULE.Repository(
            root=Path("."),
            files=set(),
            directories={"third_party/wil"},
            top_level={"third_party"},
            submodules=("third_party/wil",),
        )
        self.assertIsNone(MODULE.check_code_span(repo, "third_party/wil/include/wil/com.h"))
        self.assertIsNotNone(MODULE.check_code_span(repo, "third_party/other/x.h"))

    def test_real_repository_is_consistent(self) -> None:
        self.assertEqual(MODULE.check(REPO_ROOT), [])


if __name__ == "__main__":
    unittest.main()
