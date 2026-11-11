#!/usr/bin/env python3
"""Check the shape of the specs and the roadmap.

`docs-lint` looks for state words and duplicated prose, and `check_doc_refs.py`
and `check_doc_contracts.py` match documents against code. None of them looks at
the *structure* that keeps the specs and the roadmap from drifting apart. This
script does. Like its siblings it only checks: the structure carries prose that
no generator could write, so nothing is rewritten.

Roadmap (`plans/windows-port-roadmap.md`)
  R1  Every `### M<n>` section appears in the dependency diagram (the first code
      block under 「Phase 5〜7 の依存関係と実行順」).
  R2  Every `### M<n>` section has a `- **前提**:` line (`NO_PREREQUISITE_LINE`
      lists the exceptions), and every M named in such a line appears in the
      diagram.
  R3  An edge the diagram states directly on one line (`A ─→ B`, `A / B ─→ C`,
      `A + B ─→ C`, and chains `A ─→ B ─→ C`) must be named in the 前提 of the
      target. `/` and `+` both enumerate sources. An ID with a suffix
      (`M58-A`) is compared by its base (`M58`), because the 前提 lines are
      written per milestone section; an edge between two steps of one
      milestone (`M61-A ─→ M61-B`) is not an edge between milestones and is
      skipped. Not extracted: branches drawn across lines
      (`┬`, `├`, `└`) and dotted arrows (`⇢`). The full edge-by-edge match is
      deliberately not attempted.

  How the 前提 is read: all `- **前提**` bullets of a `### M<n>` section are
  merged (continuation lines included). "Appears in the diagram" means the M
  number occurs anywhere in the diagram block, so a mention in a note line
  counts.

Specs (`docs/*-spec.md`)
  S1  A section whose heading mentions 受け入れ条件 holds no list item or table
      row. The definition lives in the roadmap; the spec keeps a pointer.
  S2  The first lines of a spec carry no creation or update date.
  S3  A spec that names three or more milestones in its first lines has a
      section-to-milestone table near the top: a heading containing
      「マイルストーン」 and 「対応」, or a table whose header row has the
      columns 「節」 and 「マイルストーン」.

  "The first lines" of S2 and S3 are counted after the front matter (the
  `---` block that `check_doc_meta.py` reads), so that metadata neither
  pushes the opening prose out of the window nor counts as a mention.

Heading state words (確定・DEV 番号・日付) are the origin docs-lint's job and are
not repeated here.
"""

from __future__ import annotations

import argparse
import importlib.util
from pathlib import Path
import re
import sys


def _load_doc_meta():
    """`check_doc_meta.py` owns the front matter rule; load it by path.

    The script is run both directly and from `scripts/tests/`, so the module is
    loaded from the file next to this one rather than imported by name.
    """
    path = Path(__file__).with_name("check_doc_meta.py")
    spec = importlib.util.spec_from_file_location("check_doc_meta_for_format", path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


front_matter_end = _load_doc_meta().front_matter_end

ROADMAP = "plans/windows-port-roadmap.md"
DIAGRAM_HEADING = "## Phase 5〜7 の依存関係と実行順"

# M sections that have no 前提 line. M31 was withdrawn and keeps only a notice.
NO_PREREQUISITE_LINE = {"M31"}

HEAD_LINES_FOR_DATE = 15
HEAD_LINES_FOR_MILESTONES = 25
HEAD_LINES_FOR_TABLE = 60
MULTI_MILESTONE_THRESHOLD = 3

M_TOKEN = re.compile(r"M\d+")
M_ID = re.compile(r"M\d+(?:-[A-Z])?")
DATE_LINE = re.compile(
    r"^\s*(?:[>*\-]\s*)*(?:\*\*)?(?:作成日|更新日|最終更新|最終更新日|Last updated)\b"
)
ACCEPTANCE_HEADING = re.compile(r"^(#{2,4}) .*受け入れ条件")
LIST_OR_TABLE_ROW = re.compile(r"^\s*(?:[-*] |\d+\. |\|)")
EDGE_CHARS = "─┬┴├└┘┐│"
# The target is a lookahead so that a chain `A ─→ B ─→ C` yields both edges:
# B is the target of the first and the source of the second.
EDGE = re.compile(
    r"(?P<left>M\d+(?:-[A-Z])?(?:\s*[/+]\s*M\d+(?:-[A-Z])?)*)"
    r"\s*[" + EDGE_CHARS + r"]*─→\s*"
    r"(?=(?P<right>M\d+(?:-[A-Z])?))"
)


def read_lines(path: Path) -> list[str]:
    return path.read_text(encoding="utf-8-sig").split("\n")


def diagram_block(lines: list[str]) -> list[str] | None:
    """Lines of the first code block under the dependency heading."""
    start = next((i for i, l in enumerate(lines) if l.startswith(DIAGRAM_HEADING)), None)
    if start is None:
        return None
    open_at = next((i for i in range(start, len(lines)) if lines[i].startswith("```")), None)
    if open_at is None:
        return None
    close_at = next((i for i in range(open_at + 1, len(lines)) if lines[i].startswith("```")), None)
    if close_at is None:
        return None
    return lines[open_at + 1 : close_at]


def milestone_sections(lines: list[str]) -> list[tuple[str, list[str]]]:
    """(`M<n>`, section lines) for each `### M<n>` heading."""
    heads = [(i, m.group(1)) for i, l in enumerate(lines) if (m := re.match(r"### (M\d+)\b", l))]
    out = []
    for k, (i, name) in enumerate(heads):
        end = heads[k + 1][0] if k + 1 < len(heads) else len(lines)
        # A section ends at the next `## ` heading too.
        stop = next((j for j in range(i + 1, end) if lines[j].startswith("## ")), end)
        out.append((name, lines[i:stop]))
    return out


def prerequisites(section: list[str]) -> set[str] | None:
    """M numbers named by the 前提 bullet, continuation lines included."""
    found: set[str] | None = None
    in_bullet = False
    for line in section:
        if line.startswith("- **前提**"):
            in_bullet = True
            found = (found or set()) | set(M_TOKEN.findall(line))
        elif in_bullet and re.match(r"  (?![-*] )\S", line):
            found = (found or set()) | set(M_TOKEN.findall(line))
        else:
            in_bullet = False
    return found


def check_roadmap(root: Path) -> list[str]:
    path = root / ROADMAP
    if not path.is_file():
        return [f"{ROADMAP}: 存在しません"]
    lines = read_lines(path)
    diagram = diagram_block(lines)
    if diagram is None:
        return [f"{ROADMAP}: 「{DIAGRAM_HEADING[3:]}」の依存図（コードブロック）が見つかりません"]
    in_diagram = set(M_TOKEN.findall("\n".join(diagram)))
    sections = milestone_sections(lines)
    prereq = {name: prerequisites(sec) for name, sec in sections}
    problems: list[str] = []
    for name, _ in sections:
        if name not in in_diagram:
            problems.append(f"{ROADMAP}: {name} が依存図に現れません")
        pre = prereq[name]
        if pre is None:
            if name not in NO_PREREQUISITE_LINE:
                problems.append(f"{ROADMAP}: {name} の節に「- **前提**:」の行がありません")
            continue
        for dep in sorted(pre):
            if dep not in in_diagram:
                problems.append(f"{ROADMAP}: {name} の前提 {dep} が依存図に現れません")
    for line in diagram:
        for edge in EDGE.finditer(line):
            target = edge.group("right")
            base = M_TOKEN.match(target)
            assert base is not None
            pre = prereq.get(base.group(0))
            if pre is None:
                continue
            for src in M_ID.findall(edge.group("left")):
                src_base = M_TOKEN.match(src)
                assert src_base is not None
                if src_base.group(0) == base.group(0):
                    continue  # a step inside one milestone (`M61-A ─→ M61-B`)
                if src_base.group(0) not in pre:
                    problems.append(
                        f"{ROADMAP}: 依存図の辺 {src} → {target} が、{base.group(0)} の前提に書かれていません"
                    )
    return problems


def check_spec(path: Path, rel: str) -> list[str]:
    lines = read_lines(path)
    problems: list[str] = []
    start = front_matter_end(lines)
    for i, line in enumerate(lines[start : start + HEAD_LINES_FOR_DATE], start):
        if DATE_LINE.match(line):
            problems.append(f"{rel}:{i + 1}: 冒頭に作成日・更新日を置かないでください（履歴は git と Linear が持ちます）")
    # Headings inside a fenced code block are comments, not sections.
    in_fence = [False] * len(lines)
    fenced = False
    for i, line in enumerate(lines):
        if line.startswith("```"):
            fenced = not fenced
            in_fence[i] = True
        else:
            in_fence[i] = fenced
    for i, line in enumerate(lines):
        m = ACCEPTANCE_HEADING.match(line)
        if not m or in_fence[i]:
            continue
        level = len(m.group(1))
        end = next(
            (
                j
                for j in range(i + 1, len(lines))
                if not in_fence[j] and re.match(r"#{1,%d} " % level, lines[j])
            ),
            len(lines),
        )
        count = sum(1 for body in lines[i + 1 : end] if LIST_OR_TABLE_ROW.match(body))
        if count:
            problems.append(
                f"{rel}:{i + 1}: 受け入れ条件の節に箇条書き・表が {count} 行あります"
                "（定義は roadmap の該当 M 節に置き、spec には参照だけを残します）"
            )
    head = "\n".join(lines[start : start + HEAD_LINES_FOR_MILESTONES])
    if len(set(M_TOKEN.findall(head))) >= MULTI_MILESTONE_THRESHOLD:
        has_table = any(
            (l.startswith("#") and "マイルストーン" in l and "対応" in l)
            or (l.startswith("|") and "節" in l and "マイルストーン" in l)
            for l in lines[start : start + HEAD_LINES_FOR_TABLE]
        )
        if not has_table:
            problems.append(
                f"{rel}: 冒頭で {MULTI_MILESTONE_THRESHOLD} 個以上のマイルストーンを扱いますが、"
                f"冒頭 {HEAD_LINES_FOR_TABLE} 行に「節とマイルストーンの対応」の節がありません"
            )
    return problems


def check_specs(root: Path) -> list[str]:
    problems: list[str] = []
    for path in sorted((root / "docs").glob("*-spec.md")):
        problems.extend(check_spec(path, path.relative_to(root).as_posix()))
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args(argv)
    problems = check_roadmap(args.root) + check_specs(args.root)
    if problems:
        print("spec と roadmap の書式が規則から外れています:", file=sys.stderr)
        for problem in problems:
            print(f"  - {problem}", file=sys.stderr)
        return 1
    print("spec と roadmap の書式は規則に沿っています。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
