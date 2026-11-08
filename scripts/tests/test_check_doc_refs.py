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
