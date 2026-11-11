#!/usr/bin/env python3

from __future__ import annotations

import contextlib
import importlib.util
import io
from pathlib import Path
from typing import Mapping
import sys
import tempfile
import unittest


sys.dont_write_bytecode = True
SCRIPT = Path(__file__).resolve().parents[1] / "check_doc_meta.py"
SPEC = importlib.util.spec_from_file_location("check_doc_meta", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)

REPO_ROOT = SCRIPT.parents[1]

INDEX = """\
# ドキュメント案内

## アーキテクチャ・設計

| ドキュメント | 役割 |
|---|---|
| [`arch.md`](arch.md) | 分離設計の正典 |
| [`old-report.md`](old-report.md) | 調査。参考資料であり正典ではない |

## 機能仕様（`*-spec.md`）

| ドキュメント | 対応フェーズ／マイルストーン |
|---|---|
| [`a-spec.md`](a-spec.md) | M13〜M15 と M58-A |

## 運用

| ドキュメント | 役割 |
|---|---|
| [`rules.md`](rules.md) | 作業規則の正典 |
| [`linear-conventions.md`](linear-conventions.md) | Linear の規約 |

### 恒常 runbook（`docs/handoff/`）

| ドキュメント | 役割 |
|---|---|
| [`handoff/run.md`](handoff/run.md) | VM の手順 |

### 一回限りの引き継ぎ（`docs/handoff/`）

| ドキュメント | 役割 |
|---|---|
| [`handoff/plan.md`](handoff/plan.md) | 整理計画 |

## 決定記録と測定記録（`docs/decisions/`）

| ドキュメント | 種別 | 結論の正典 |
|---|---|---|
| [`decisions/2026-01-x.md`](decisions/2026-01-x.md) | 決定記録 | [`a-spec.md`](a-spec.md) §1 |
"""

PLAN = """\
---
kind: one-off
dod: "§6"
---

# 整理計画

## 6. 完了基準

- [x] 段階 1
- [ ] 段階 2
"""

DOCS = {
    "docs/README.md": INDEX,
    "docs/arch.md": '---\nkind: design\ncanonical_for: ["分離設計"]\n---\n\n# Arch\n',
    "docs/old-report.md": "---\nkind: reference\n---\n\n<!-- lint:allow-file status -->\n# Report\n",
    "docs/a-spec.md": "# A spec\n\n経緯は decisions/2026-01-x.md にある。\n",
    "docs/rules.md": "---\nkind: policy\ncanonical_for: [作業規則]\n---\n\n# Rules\n",
    "docs/linear-conventions.md": "<!-- SHARED CORE -->\n# Linear\n",
    "docs/handoff/run.md": '---\nkind: runbook\ncanonical_for: ["VM の手順"]\n---\n\n# Run\n',
    "docs/handoff/plan.md": PLAN,
    "docs/decisions/README.md": "# decisions\n",
    "docs/decisions/2026-01-x.md": (
        "# X\n\n- 種別: 決定記録\n- 結論の正典: [`a-spec.md`](../a-spec.md) §1\n"
    ),
    "docs/archive/done.md": "# Done\n",
    "plans/windows-port-roadmap.md": "# Roadmap\n",
}


def root_with(files: Mapping[str, str | None]) -> Path:
    tmp = Path(tempfile.mkdtemp())
    docs = dict(DOCS)
    for rel, text in files.items():
        if text is None:
            docs.pop(rel, None)
        else:
            docs[rel] = text
    for rel, text in docs.items():
        path = tmp / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8", newline="")
    return tmp


def problems(files: Mapping[str, str | None]) -> list[str]:
    return MODULE.check(root_with(files))[0]


def warnings(files: Mapping[str, str | None]) -> list[str]:
    return MODULE.check(root_with(files))[1]


def has(found: list[str], text: str) -> bool:
    return any(text in item for item in found)


class FrontMatterParseTest(unittest.TestCase):
    def test_scalars_and_lists(self) -> None:
        meta = MODULE.parse_front_matter(
            ["---", "kind: spec", 'milestones: [M1, "M2", M58-A]', 'dod: "§6"', "---", "# T"]
        )
        self.assertEqual(meta, {"kind": "spec", "milestones": ["M1", "M2", "M58-A"], "dod": "§6"})

    def test_quoted_comma_stays_in_one_item(self) -> None:
        meta = MODULE.parse_front_matter(["---", 'canonical_for: ["a、b, c", d]', "---"])
        self.assertEqual(meta, {"canonical_for": ["a、b, c", "d"]})

    def test_no_front_matter(self) -> None:
        self.assertIsNone(MODULE.parse_front_matter(["# Title", "---"]))

    def test_rejects_unclosed_block(self) -> None:
        with self.assertRaises(MODULE.FrontMatterError):
            MODULE.parse_front_matter(["---", "kind: spec", "# Title"])

    def test_rejects_nested_and_multiline_values(self) -> None:
        for line in ("kind:", "  - item", "kind: [a, b", "kind: a: b", "kind: 'x'", "kind: x # c"):
            with self.assertRaises(MODULE.FrontMatterError, msg=line):
                MODULE.parse_front_matter(["---", line, "---"])

    def test_rejects_duplicate_key(self) -> None:
        with self.assertRaises(MODULE.FrontMatterError):
            MODULE.parse_front_matter(["---", "kind: spec", "kind: design", "---"])


class CleanTreeTest(unittest.TestCase):
    def test_clean_tree_passes(self) -> None:
        found, warned = MODULE.check(root_with({}))
        self.assertEqual(found, [])
        self.assertEqual(warned, [])

    def test_main_exit_codes(self) -> None:
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(MODULE.main(["--root", str(root_with({}))]), 0)
            bad = root_with({"docs/rules.md": "# Rules\n"})
            self.assertEqual(MODULE.main(["--root", str(bad)]), 1)


class M1Test(unittest.TestCase):
    def test_missing_front_matter(self) -> None:
        self.assertTrue(has(problems({"docs/rules.md": "# Rules\n"}), "front matter（kind ほか）がありません"))

    def test_spec_without_front_matter_is_allowed(self) -> None:
        self.assertEqual(problems({}), [])

    def test_unknown_kind_and_key(self) -> None:
        found = problems({"docs/rules.md": "---\nkind: note\n---\n"})
        self.assertTrue(has(found, "kind が無いか語彙にありません"), found)
        found = problems({"docs/rules.md": "---\nkind: policy\ncanonical_for: [作業規則]\nowner: x\n---\n"})
        self.assertTrue(has(found, "未知のキー owner"), found)

    def test_location_kinds_cannot_be_declared(self) -> None:
        found = problems({"docs/rules.md": "---\nkind: decision\n---\n"})
        self.assertTrue(has(found, "置き場所で決まり"), found)

    def test_location_documents_take_no_front_matter(self) -> None:
        found = problems({"docs/linear-conventions.md": "---\nkind: policy\n---\n# Linear\n"})
        self.assertTrue(has(found, "front matter を持ちません"), found)

    def test_spec_kind_follows_file_name(self) -> None:
        found = problems({"docs/rules.md": "---\nkind: spec\nmilestones: [M1]\ncanonical_for: [作業規則]\n---\n"})
        self.assertTrue(has(found, "*-spec.md だけに付け"), found)

    def test_required_and_forbidden_keys(self) -> None:
        found = problems({"docs/rules.md": "---\nkind: policy\n---\n"})
        self.assertTrue(has(found, "kind policy には canonical_for が要ります"), found)
        found = problems({"docs/old-report.md": "---\nkind: reference\ncanonical_for: [調査]\n---\n"})
        self.assertTrue(has(found, "kind reference には canonical_for を書きません"), found)
        found = problems({"docs/handoff/plan.md": "---\nkind: one-off\n---\n"})
        self.assertTrue(has(found, "kind one-off には dod が要ります"), found)
        found = problems({"docs/rules.md": "---\nkind: policy\ncanonical_for: [作業規則]\nmilestones: [M1]\n---\n"})
        self.assertTrue(has(found, "kind policy には milestones を書きません"), found)

    def test_milestone_id_shape(self) -> None:
        text = '---\nkind: design\ncanonical_for: ["分離設計"]\nmilestones: [M13〜M15]\n---\n'
        found = problems({"docs/arch.md": text})
        self.assertTrue(has(found, "M<n>、M<n>-<A>、X-<n> の形ではありません"), found)

    def test_canonical_for_must_be_in_the_index_row(self) -> None:
        found = problems({"docs/rules.md": "---\nkind: policy\ncanonical_for: [別の規則]\n---\n"})
        self.assertTrue(has(found, "canonical_for の「別の規則」が索引の行にありません"), found)

    def test_canonical_for_ignores_back_quotes(self) -> None:
        index = INDEX.replace("作業規則の正典", "`AZOOKEY_X` 作業規則の正典")
        text = "---\nkind: policy\ncanonical_for: [AZOOKEY_X 作業規則]\n---\n"
        self.assertEqual(problems({"docs/README.md": index, "docs/rules.md": text}), [])


class M2Test(unittest.TestCase):
    def test_not_indexed(self) -> None:
        found = problems({"docs/handoff/new.md": '---\nkind: runbook\ncanonical_for: ["x"]\n---\n'})
        self.assertTrue(has(found, "docs/handoff/new.md: M2 docs/README.md の表に載っていません"), found)

    def test_wrong_table(self) -> None:
        text = '---\nkind: one-off\ndod: "§6"\n---\n\n## 6. 完了基準\n\n- [ ] a\n'
        found = problems({"docs/handoff/run.md": text})
        self.assertTrue(has(found, "kind one-off の文書が索引の「恒常 runbook"), found)

    def test_reference_row_must_say_not_canonical(self) -> None:
        found = problems({"docs/README.md": INDEX.replace("参考資料であり正典ではない", "参考資料")})
        self.assertTrue(has(found, "reference の索引行に「正典ではない」がありません"), found)

    def test_design_row_must_not_say_not_canonical(self) -> None:
        found = problems({"docs/README.md": INDEX.replace("分離設計の正典", "分離設計。正典ではない")})
        self.assertTrue(has(found, "kind reference にします"), found)

    def test_archive_must_not_be_indexed(self) -> None:
        index = INDEX + "| [`archive/done.md`](archive/done.md) | 済み |\n"
        found = problems({"docs/README.md": index})
        self.assertTrue(has(found, "archive の文書が索引の表に載っています"), found)

    def test_listed_twice(self) -> None:
        index = INDEX.replace("| [`rules.md`](rules.md) | 作業規則の正典 |", "| [`rules.md`](rules.md) | 作業規則の正典 |\n| [`rules.md`](rules.md) | 再掲 |")
        found = problems({"docs/README.md": index})
        self.assertTrue(has(found, "表に 2 回載っています"), found)

    def test_location_policy_follows_the_index(self) -> None:
        index = INDEX.replace("| [`linear-conventions.md`](linear-conventions.md) | Linear の規約 |\n", "")
        found = problems({"docs/README.md": index})
        self.assertTrue(has(found, "docs/linear-conventions.md: M2"), found)


class M3Test(unittest.TestCase):
    SPEC = '---\nkind: spec\nmilestones: [{ms}]\ncanonical_for: ["M13〜M15"]\n---\n\n# A\n'

    def test_range_in_row_is_expanded(self) -> None:
        text = self.SPEC.format(ms="M13, M14, M15, M58-A")
        self.assertEqual(problems({"docs/a-spec.md": text + "decisions/2026-01-x.md\n"}), [])

    def test_mismatch(self) -> None:
        text = self.SPEC.format(ms="M13, M15, M58-A") + "decisions/2026-01-x.md\n"
        found = problems({"docs/a-spec.md": text})
        self.assertTrue(has(found, "M3 milestones"), found)

    def test_spec_front_matter_needs_milestones(self) -> None:
        found = problems({"docs/a-spec.md": '---\nkind: spec\ncanonical_for: ["M13〜M15"]\n---\n'})
        self.assertTrue(has(found, "kind spec には milestones が要ります"), found)


class M4Test(unittest.TestCase):
    def test_missing_back_link(self) -> None:
        found = problems({"docs/a-spec.md": "# A spec\n"})
        self.assertTrue(has(found, "M4 結論の正典 docs/a-spec.md に 2026-01-x.md への逆リンクがありません"), found)

    def test_header_and_index_differ(self) -> None:
        record = "# X\n\n- 種別: 決定記録\n- 結論の正典: [`arch.md`](../arch.md)、[`a-spec.md`](../a-spec.md) §1\n"
        found = problems({"docs/decisions/2026-01-x.md": record, "docs/arch.md": DOCS["docs/arch.md"] + "2026-01-x.md\n"})
        self.assertTrue(has(found, "が索引の行"), found)

    def test_header_fields_required(self) -> None:
        found = problems({"docs/decisions/2026-01-x.md": "# X\n\n- 種別: メモ\n"})
        self.assertTrue(has(found, "「- 種別: 決定記録 | 測定記録」がありません"), found)
        self.assertTrue(has(found, "「- 結論の正典:」がありません"), found)

    def test_roadmap_target_resolves_from_the_decisions_directory(self) -> None:
        record = "# X\n\n- 種別: 測定記録\n- 結論の正典: [`plans/windows-port-roadmap.md`](../../plans/windows-port-roadmap.md) M8\n"
        index = INDEX.replace("[`a-spec.md`](a-spec.md) §1 |", "[`plans/windows-port-roadmap.md`](../plans/windows-port-roadmap.md) M8 |")
        files = {
            "docs/decisions/2026-01-x.md": record,
            "docs/README.md": index,
            "plans/windows-port-roadmap.md": "# Roadmap\n\n2026-01-x.md\n",
        }
        self.assertEqual(problems(files), [])


class M5Test(unittest.TestCase):
    def test_defers_to_needs_back_link(self) -> None:
        text = "---\nkind: policy\ncanonical_for: [作業規則]\ndefers_to: [docs/a-spec.md §1]\n---\n"
        found = problems({"docs/rules.md": text})
        self.assertTrue(has(found, "M5 defers_to の docs/a-spec.md に rules.md への逆リンクがありません"), found)
        spec = DOCS["docs/a-spec.md"] + "規則は rules.md。\n"
        self.assertEqual(problems({"docs/rules.md": text, "docs/a-spec.md": spec}), [])

    def test_defers_to_missing_file(self) -> None:
        text = "---\nkind: policy\ncanonical_for: [作業規則]\ndefers_to: [docs/none.md]\n---\n"
        self.assertTrue(has(problems({"docs/rules.md": text}), "M5 defers_to の docs/none.md がありません"))


class DodTest(unittest.TestCase):
    def test_dod_section_must_have_a_checklist(self) -> None:
        found = problems({"docs/handoff/plan.md": PLAN.replace('"§6"', '"§7"')})
        self.assertTrue(has(found, "M6 dod の §7 にチェックリストの節がありません"), found)

    def test_open_checklist_is_quiet(self) -> None:
        self.assertEqual(warnings({}), [])

    def test_ticked_checklist_asks_for_archive(self) -> None:
        found = warnings({"docs/handoff/plan.md": PLAN.replace("- [ ] 段階 2", "- [x] 段階 2")})
        self.assertTrue(has(found, "W1 完了基準がすべて満たされています"), found)


class ReferenceWarningTest(unittest.TestCase):
    def test_spec_citing_a_reference_warns_only(self) -> None:
        spec = DOCS["docs/a-spec.md"] + "根拠は old-report.md。\n"
        found, warned = MODULE.check(root_with({"docs/a-spec.md": spec}))
        self.assertEqual(found, [])
        self.assertTrue(has(warned, "docs/a-spec.md: W2 参考資料 docs/old-report.md を 1 か所"), warned)


class ReviewFixTest(unittest.TestCase):
    def test_row_milestones_need_a_word_boundary(self) -> None:
        self.assertEqual(MODULE.row_milestones("| x | ARM64 と MAX-1 と M3 |"), {"M3"})

    def test_row_milestones_expand_theme_ranges_and_keep_sub_ids(self) -> None:
        found = MODULE.row_milestones("| x | X-1〜X-3、M16 / X-3-3 |")
        self.assertEqual(found, {"X-1", "X-2", "X-3", "X-3-3", "M16"})
        self.assertTrue(MODULE.MILESTONE_ID.match("X-3-3"))

    def test_empty_canonical_phrase_fails(self) -> None:
        found = problems({"docs/rules.md": '---\nkind: policy\ncanonical_for: [""]\n---\n'})
        self.assertTrue(has(found, "canonical_for に空の句"), found)

    def test_canonical_phrase_is_matched_against_the_role_column_only(self) -> None:
        found = problems({"docs/rules.md": "---\nkind: policy\ncanonical_for: [rules.md]\n---\n"})
        self.assertTrue(has(found, "canonical_for の「rules.md」が索引の行にありません"), found)

    def test_bom_before_front_matter_is_read(self) -> None:
        text = '﻿---\nkind: spec\ncanonical_for: ["M13〜M15"]\n---\n\n# A\ndecisions/2026-01-x.md\n'
        found = problems({"docs/a-spec.md": text})
        self.assertTrue(has(found, "kind spec には milestones が要ります"), found)

    def test_crlf_front_matter_is_read(self) -> None:
        text = DOCS["docs/rules.md"].replace("\n", "\r\n")
        self.assertEqual(problems({"docs/rules.md": text}), [])

    def test_spec_name_below_docs_is_not_a_spec(self) -> None:
        index = INDEX.replace(
            "| [`handoff/run.md`](handoff/run.md) | VM の手順 |",
            "| [`handoff/run.md`](handoff/run.md) | VM の手順 |\n| [`handoff/x-spec.md`](handoff/x-spec.md) | 検証の手順 |",
        )
        text = '---\nkind: runbook\ncanonical_for: ["検証の手順"]\n---\n'
        self.assertEqual(problems({"docs/README.md": index, "docs/handoff/x-spec.md": text}), [])

    def test_dod_section_is_not_a_subsection_or_a_fenced_line(self) -> None:
        plan = PLAN.replace(
            "## 6. 完了基準",
            "```\n## 6. 例\n- [x] a\n```\n\n## 6.1 準備\n\n- [x] 準備\n\n## 6. 完了基準",
        )
        self.assertEqual(warnings({"docs/handoff/plan.md": plan}), [])
        self.assertEqual(problems({"docs/handoff/plan.md": plan}), [])

    def test_quote_inside_an_unquoted_item_is_rejected(self) -> None:
        with self.assertRaises(MODULE.FrontMatterError):
            MODULE.parse_value('[x"y, z"w]')

    def test_warnings_alone_keep_exit_code_zero(self) -> None:
        root = root_with({"docs/a-spec.md": DOCS["docs/a-spec.md"] + "根拠は old-report.md。\n"})
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(MODULE.main(["--root", str(root)]), 0)
        self.assertIn("warning: docs/a-spec.md: W2", out.getvalue())


class RepositoryTest(unittest.TestCase):
    def test_repository_conforms(self) -> None:
        found, _ = MODULE.check(REPO_ROOT)
        self.assertEqual(found, [])


if __name__ == "__main__":
    unittest.main()
