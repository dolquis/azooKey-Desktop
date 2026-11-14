#!/usr/bin/env python3
"""Check the metadata at the top of each doc against the docs index.

`docs/README.md` is the map of the docs: which table a document sits in says
what kind of document it is, and the decisions table names where each record's
conclusion is canonical. docs-lint only checks that a top-level file name occurs
somewhere in the index. This script gives every document a declared kind and
matches the declaration against the index, the lifetime rules and the
canonical declarations. Like its siblings it only checks; nothing is rewritten.

Front matter
  A document starts with a block between two `---` lines. The format is a
  restricted subset of YAML that the standard library can read: one
  `key: value` per line, a value is a plain or double-quoted string, or a flow
  list `[a, b, "c d"]`. Comments, nesting and multi-line values are not
  accepted.

    kind           required. One of KINDS.
    milestones     spec: required. design: optional. Others: not allowed.
                   Every ID is written out (`M13, M14`), no ranges. A step
                   (`M58-A`), a theme (`X-1`) and a theme item (`X-3-3`) are
                   written as the index row writes them. A spec that belongs
                   to no milestone writes `[]`, and its index row then names
                   no ID either (M3).
    canonical_for  spec / design / policy / runbook: required. Others: not
                   allowed. Each phrase must occur in the document's row of the
                   index, so the two cannot drift apart.
    defers_to      optional. Documents (repository paths, optionally followed
                   by ` §N`) that this document declares canonical for part of
                   its subject. Each must exist and link back.
    dod            one-off: required. Others: not allowed. A section reference
                   (`§6`) to the document's completion checklist.

Which documents
  Every `docs/**/*.md` except the agent files and the index itself
  (`NOT_GOVERNED`), `docs/decisions/` (frozen records with their own fixed
  header, read instead of front matter), `docs/archive/` (kind by location) and
  `docs/linear-conventions.md` (a mirror of a shared core; its kind comes from
  the index). Every spec carries front matter.

Rules (fail)
  M1  Front matter syntax, keys, vocabulary and the per-kind required and
      forbidden keys. A `*-spec.md` is kind spec and nothing else is.
  M2  The index row matches the kind: a governed document is in exactly one
      table of INDEX_SECTIONS, that table allows the kind, a reference row
      says 「正典ではない」 and a design row does not. An archived document
      is in no table.
  M3  A spec's milestones equal the IDs in its index row (`M13〜M19` and
      `X-1〜X-4` are expanded; an ID inside a word, such as `ARM64`, is not
      an ID).
  M4  A decision record's header has 種別 and 結論の正典. The documents its
      結論の正典 links to equal the ones the index row links to, and every one
      of them mentions the record's file name (the back link).
  M5  Every `defers_to` target exists and mentions this document's file name.
  M6  A one-off's `dod` names a section of the document that has checklist
      items.

Warnings (exit status unchanged)
  W1  A one-off whose `dod` checklist is fully ticked: move it to
      `docs/archive/`.
  W2  A spec or the roadmap mentions a reference document. A snapshot may be
      cited, but a definition must not depend on it.

`--root` is for tests. On GitHub Actions the warnings are also emitted as
`::warning` annotations.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path, PurePosixPath
import re
import sys

INDEX = "docs/README.md"
ROADMAP = "plans/windows-port-roadmap.md"

KINDS = ("spec", "design", "runbook", "one-off", "reference", "decision", "policy", "archive")
KEYS = ("kind", "milestones", "canonical_for", "defers_to", "dod")
MILESTONES_REQUIRED = {"spec"}
MILESTONES_ALLOWED = {"spec", "design"}
CANONICAL_REQUIRED = {"spec", "design", "policy", "runbook"}
DOD_REQUIRED = {"one-off"}
# Kinds that front matter may declare. decision and archive are decided by the
# directory, and those directories carry no front matter.
DECLARABLE = {"spec", "design", "runbook", "one-off", "reference", "policy"}

NOT_GOVERNED = ("docs/README.md", "docs/AGENTS.md", "docs/CLAUDE.md", "docs/decisions/README.md")
NO_FRONT_MATTER = {"docs/linear-conventions.md": "policy"}

# Table headings of the index and the kinds each one holds. A heading matches
# when it starts with the key.
INDEX_SECTIONS = {
    "アーキテクチャ・設計": {"design", "reference"},
    "機能仕様": {"spec"},
    "運用": {"policy"},
    "恒常 runbook": {"runbook"},
    "一回限りの引き継ぎ": {"one-off"},
    "決定記録と測定記録": {"decision"},
}
NOT_CANONICAL = "正典ではない"
DECISION_TYPES = {"決定記録", "測定記録", "測定記録・決定記録", "決定記録・測定記録"}

# A milestone is `M<n>` with an optional step letter (`M58-A`), or a
# cross-cutting theme `X-<n>` with optional sub-items (`X-3-3`), written the way
# the index rows write them.
MILESTONE_ID = re.compile(r"^(?:M\d+(?:-[A-Z])?|X-\d+(?:-\d+)*)$")
# The boundaries keep `ARM64` from yielding `M64` and `MAX-1` from yielding `X-1`.
ROW_MILESTONE = re.compile(r"(?<![A-Za-z0-9-])(?:M\d+(?:-[A-Z](?![A-Za-z]))?|X-\d+(?:-\d+)*)(?![0-9])")
ROW_RANGE = re.compile(r"(?<![A-Za-z0-9-])(M|X-)(\d+)〜(?:M|X-)(\d+)(?![0-9-])")
LINK = re.compile(r"\[[^\]]*\]\(([^)\s]+)\)")
KEY_LINE = re.compile(r"^([a-z_]+):(?: (.*))?$")
BOM = chr(0xFEFF)
CHECKBOX = re.compile(r"^\s*[-*] \[([ xX])\] ")


class FrontMatterError(ValueError):
    pass


def read_lines(path: Path) -> list[str]:
    """The lines of a document, split as `check_doc_format.py` splits them.

    A UTF-8 BOM is dropped so that it cannot hide the opening `---`, and a
    trailing `\\r` is dropped so that CRLF files read the same as LF files.
    """
    text = path.read_text(encoding="utf-8-sig")
    return [line.rstrip("\r") for line in text.split("\n")]


def front_matter_end(lines: list[str]) -> int:
    """Index of the first line after the front matter, or 0 when there is none.

    `check_doc_format.py` uses this function, so both scripts agree on where
    the front matter ends.
    """
    if not lines or lines[0].lstrip(BOM).rstrip("\r") != "---":
        return 0
    for i in range(1, len(lines)):
        if lines[i].rstrip("\r") == "---":
            return i + 1
    return 0


def is_spec(rel: str) -> bool:
    """A spec is `docs/<name>-spec.md`, directly under `docs/`."""
    path = PurePosixPath(rel)
    return str(path.parent) == "docs" and path.name.endswith("-spec.md")


def parse_scalar(text: str) -> str:
    text = text.strip()
    if len(text) >= 2 and text[0] == text[-1] == '"':
        inner = text[1:-1]
        if '"' in inner or "\\" in inner:
            raise FrontMatterError(f"引用符の中に \" や \\ は書けません: {text}")
        return inner
    if not text or text[0] in "'[]{}#&*!|>%@`" or '"' in text or ": " in text or " #" in text:
        raise FrontMatterError(f"値は素の文字列か \"...\" で書いてください: {text}")
    return text


def parse_value(text: str) -> str | list[str]:
    text = text.strip()
    if text.startswith("["):
        if not text.endswith("]"):
            raise FrontMatterError(f"リストは 1 行の [a, b] で書いてください: {text}")
        inner = text[1:-1].strip()
        if not inner:
            return []
        items, buf, quoted = [], "", False
        for ch in inner:
            # A quote opens only at the start of an item and closes the quoted
            # item; a quote inside an unquoted item is left for parse_scalar
            # to reject.
            if ch == '"' and (quoted or not buf.strip()):
                quoted = not quoted
            if ch == "," and not quoted:
                items.append(buf)
                buf = ""
            else:
                buf += ch
        if quoted:
            raise FrontMatterError(f"引用符が閉じていません: {text}")
        items.append(buf)
        return [parse_scalar(item) for item in items]
    return parse_scalar(text)


def parse_front_matter(lines: list[str]) -> dict[str, str | list[str]] | None:
    """The front matter as a dict, or None when the document has none."""
    if not lines or lines[0] != "---":
        return None
    end = front_matter_end(lines)
    if end == 0:
        raise FrontMatterError("front matter の終わりの --- がありません")
    data: dict[str, str | list[str]] = {}
    for line in lines[1 : end - 1]:
        m = KEY_LINE.match(line)
        if not m or m.group(2) is None:
            raise FrontMatterError(f"key: value の形ではない行があります: {line!r}")
        key = m.group(1)
        if key in data:
            raise FrontMatterError(f"キー {key} が重複しています")
        data[key] = parse_value(m.group(2))
    return data


def as_list(value: str | list[str]) -> list[str]:
    return value if isinstance(value, list) else [value]


def index_rows(root: Path) -> list[tuple[str, str, str]]:
    """(section heading, document repo path, row) for each index table row."""
    rows = []
    section = ""
    fenced = False
    for line in read_lines(root / INDEX):
        if line.startswith("```"):
            fenced = not fenced
            continue
        if fenced:
            continue
        m = re.match(r"^#{2,3} (.+)$", line)
        if m:
            section = m.group(1)
            continue
        if not line.startswith("|"):
            continue
        cells = line.split("|")
        if len(cells) < 3:
            continue
        for target in LINK.findall(cells[1]):
            rel = os.path.normpath(os.path.join("docs", target.split("#")[0])).replace(os.sep, "/")
            rows.append((section, rel, line))
    return rows


def section_kinds(section: str) -> set[str] | None:
    for key, kinds in INDEX_SECTIONS.items():
        if section.startswith(key):
            return kinds
    return None


def row_milestones(row: str) -> set[str]:
    cells = row.split("|")
    text = cells[2] if len(cells) > 2 else ""
    found = set(ROW_MILESTONE.findall(text))
    for prefix, a, b in ROW_RANGE.findall(text):
        found.update(f"{prefix}{n}" for n in range(int(a), int(b) + 1))
    return found


def link_targets(text: str, base: str) -> set[str]:
    out = set()
    for target in LINK.findall(text):
        target = target.split("#")[0]
        if target.endswith(".md") and "://" not in target:
            out.add(os.path.normpath(os.path.join(base, target)).replace(os.sep, "/"))
    return out


def mentions(root: Path, rel: str, name: str) -> bool:
    path = root / rel
    return path.is_file() and name in path.read_text(encoding="utf-8")


def find_section(lines: list[str], ref: str) -> tuple[int, int] | None:
    """(start, end) of the section `§N` / `N`, the heading line excluded."""
    number = ref.strip().lstrip("§").strip()
    if not number:
        return None
    # `6` must not match `6.1`: a dot is a terminator only when no digit follows.
    pattern = re.compile(
        r"^(#{1,6}) (?:§\s*)?" + re.escape(number) + r"(?:[.．](?![0-9])|[:：]|\s)"
    )
    fenced = []
    in_fence = False
    for line in lines:
        if line.startswith("```"):
            in_fence = not in_fence
            fenced.append(True)
        else:
            fenced.append(in_fence)
    for i, line in enumerate(lines):
        m = None if fenced[i] else pattern.match(line)
        if m:
            level = len(m.group(1))
            end = next(
                (
                    j
                    for j in range(i + 1, len(lines))
                    if not fenced[j] and re.match(r"#{1,%d} " % level, lines[j])
                ),
                len(lines),
            )
            return i + 1, end
    return None


def governed_docs(root: Path) -> list[str]:
    docs = []
    for path in sorted((root / "docs").rglob("*.md")):
        rel = path.relative_to(root).as_posix()
        if rel in NOT_GOVERNED:
            continue
        docs.append(rel)
    return docs


def kind_by_location(rel: str) -> str | None:
    if rel.startswith("docs/decisions/"):
        return "decision"
    if rel.startswith("docs/archive/"):
        return "archive"
    return NO_FRONT_MATTER.get(rel)


def check(root: Path) -> tuple[list[str], list[str]]:
    problems: list[str] = []
    warnings: list[str] = []
    rows = index_rows(root)
    rows_by_doc: dict[str, list[tuple[str, str]]] = {}
    for section, rel, row in rows:
        rows_by_doc.setdefault(rel, []).append((section, row))

    kinds: dict[str, str] = {}
    for rel in governed_docs(root):
        lines = read_lines(root / rel)
        spec_name = is_spec(rel)
        kind = kind_by_location(rel)
        meta: dict[str, str | list[str]] | None = None
        if kind is None:
            try:
                meta = parse_front_matter(lines)
            except FrontMatterError as exc:
                problems.append(f"{rel}: M1 {exc}")
                continue
            if meta is None:
                problems.append(f"{rel}: M1 冒頭に front matter（kind ほか）がありません")
                continue
        elif front_matter_end(lines):
            problems.append(f"{rel}: M1 この文書は front matter を持ちません（種別は置き場所と索引で決まります）")
            continue

        if meta is not None:
            problems.extend(check_meta(root, rel, meta, lines, spec_name, warnings))
            kind = str(meta.get("kind", ""))
            if kind not in KINDS:
                continue
        assert kind is not None
        kinds[rel] = kind
        problems.extend(check_index_row(rel, kind, rows_by_doc.get(rel, [])))
        if meta is not None and kind == "spec" and "milestones" in meta and rows_by_doc.get(rel):
            declared = set(as_list(meta["milestones"]))
            indexed = row_milestones(rows_by_doc[rel][0][1])
            if declared != indexed:
                problems.append(
                    f"{rel}: M3 milestones {sorted(declared)} が索引の行 {sorted(indexed)} と一致しません"
                )
        if kind == "decision":
            problems.extend(check_decision(root, rel, lines, rows_by_doc.get(rel, [])))

    references = [rel for rel, kind in kinds.items() if kind == "reference"]
    sources = [rel for rel, kind in kinds.items() if kind == "spec"] + [ROADMAP]
    for src in sources:
        path = root / src
        if not path.is_file():
            continue
        text = path.read_text(encoding="utf-8")
        for ref in references:
            count = text.count(Path(ref).name)
            if count:
                warnings.append(
                    f"{src}: W2 参考資料 {ref} を {count} か所で参照しています（引用は可。定義を依存させない）"
                )
    return problems, warnings


def check_meta(
    root: Path,
    rel: str,
    meta: dict[str, str | list[str]],
    lines: list[str],
    spec_name: bool,
    warnings: list[str],
) -> list[str]:
    problems = []
    for key in meta:
        if key not in KEYS:
            problems.append(f"{rel}: M1 未知のキー {key}（使えるのは {', '.join(KEYS)}）")
    kind = meta.get("kind")
    if not isinstance(kind, str) or kind not in KINDS:
        problems.append(f"{rel}: M1 kind が無いか語彙にありません（{', '.join(KINDS)}）")
        return problems
    if kind not in DECLARABLE:
        problems.append(f"{rel}: M1 kind {kind} は置き場所で決まり、front matter には書けません")
    if spec_name != (kind == "spec"):
        problems.append(f"{rel}: M1 kind spec は *-spec.md だけに付け、*-spec.md は kind spec にします")
    for key, required, allowed in (
        ("milestones", MILESTONES_REQUIRED, MILESTONES_ALLOWED),
        ("canonical_for", CANONICAL_REQUIRED, CANONICAL_REQUIRED),
        ("dod", DOD_REQUIRED, DOD_REQUIRED),
    ):
        if kind in required and key not in meta:
            problems.append(f"{rel}: M1 kind {kind} には {key} が要ります")
        if kind not in allowed and key in meta:
            problems.append(f"{rel}: M1 kind {kind} には {key} を書きません")
    for key in ("milestones", "canonical_for", "defers_to"):
        # A spec outside every milestone declares that with `milestones: []`.
        empty_ok = key == "milestones" and kind == "spec"
        if key in meta and (not isinstance(meta[key], list) or not (meta[key] or empty_ok)):
            problems.append(f"{rel}: M1 {key} は空でないリスト [a, b] で書きます（spec の milestones だけ [] を許します）")
    if isinstance(meta.get("milestones"), list):
        for item in meta["milestones"]:
            if not MILESTONE_ID.match(item):
                problems.append(f"{rel}: M1 milestones の {item!r} は M<n>、M<n>-<A>、X-<n> の形ではありません")
    if isinstance(meta.get("dod"), list):
        problems.append(f"{rel}: M1 dod は節の参照（例 §6）を 1 つ書きます")
    if isinstance(meta.get("canonical_for"), list):
        rows = [row for _, d, row in index_rows(root) if d == rel]
        cells = rows[0].split("|") if rows else []
        # The role column only: the document column holds the file name.
        role = cells[2].replace("`", "") if len(cells) > 2 else ""
        for phrase in meta["canonical_for"]:
            if not phrase.strip():
                problems.append(f"{rel}: M1 canonical_for に空の句があります")
            elif phrase.replace("`", "") not in role:
                problems.append(f"{rel}: M1 canonical_for の「{phrase}」が索引の行にありません")
    for target in as_list(meta.get("defers_to", [])):
        path = target.split(" §")[0].strip()
        if not (root / path).is_file():
            problems.append(f"{rel}: M5 defers_to の {path} がありません")
        elif not mentions(root, path, Path(rel).name):
            problems.append(f"{rel}: M5 defers_to の {path} に {Path(rel).name} への逆リンクがありません")
    dod = meta.get("dod")
    if isinstance(dod, str):
        span = find_section(lines, dod)
        matches = [CHECKBOX.match(line) for line in lines[span[0] : span[1]]] if span else []
        boxes = [m for m in matches if m]
        if not boxes:
            problems.append(f"{rel}: M6 dod の {dod} にチェックリストの節がありません")
        elif all(m.group(1) != " " for m in boxes):
            warnings.append(f"{rel}: W1 完了基準がすべて満たされています。docs/archive/ へ移してください")
    return problems


def check_index_row(rel: str, kind: str, entries: list[tuple[str, str]]) -> list[str]:
    if kind == "archive":
        return [f"{rel}: M2 archive の文書が索引の表に載っています" for _ in entries[:1]]
    if not entries:
        return [f"{rel}: M2 {INDEX} の表に載っていません"]
    if len(entries) > 1:
        return [f"{rel}: M2 {INDEX} の表に {len(entries)} 回載っています"]
    section, row = entries[0]
    allowed = section_kinds(section)
    if allowed is None or kind not in allowed:
        return [f"{rel}: M2 kind {kind} の文書が索引の「{section}」の表にあります"]
    if kind == "reference" and NOT_CANONICAL not in row:
        return [f"{rel}: M2 reference の索引行に「{NOT_CANONICAL}」がありません"]
    if kind == "design" and NOT_CANONICAL in row:
        return [f"{rel}: M2 索引行が「{NOT_CANONICAL}」と書く文書は kind reference にします"]
    return []


def check_decision(root: Path, rel: str, lines: list[str], entries: list[tuple[str, str]]) -> list[str]:
    problems = []
    header = {}
    for line in lines[:20]:
        m = re.match(r"^- (種別|結論の正典): (.*)$", line)
        if m:
            header[m.group(1)] = m.group(2)
    if header.get("種別") not in DECISION_TYPES:
        problems.append(f"{rel}: M4 冒頭に「- 種別: 決定記録 | 測定記録」がありません")
    if "結論の正典" not in header:
        problems.append(f"{rel}: M4 冒頭に「- 結論の正典:」がありません")
        return problems
    declared = link_targets(header["結論の正典"], os.path.dirname(rel))
    if entries:
        cells = entries[0][1].split("|")
        indexed = link_targets(cells[3] if len(cells) > 3 else "", "docs")
        if declared != indexed:
            problems.append(
                f"{rel}: M4 結論の正典 {sorted(declared)} が索引の行 {sorted(indexed)} と一致しません"
            )
    name = Path(rel).name
    for target in sorted(declared):
        if not (root / target).is_file():
            problems.append(f"{rel}: M4 結論の正典 {target} がありません")
        elif not mentions(root, target, name):
            problems.append(f"{rel}: M4 結論の正典 {target} に {name} への逆リンクがありません")
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args(argv)
    problems, warnings = check(args.root)
    annotate = os.environ.get("GITHUB_ACTIONS") == "true"
    for warning in warnings:
        print(f"warning: {warning}")
        if annotate:
            print(f"::warning::{warning}")
    if problems:
        print("文書のメタデータが索引と一致しません:", file=sys.stderr)
        for problem in problems:
            print(f"  - {problem}", file=sys.stderr)
        return 1
    print("文書のメタデータは索引と一致しています。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
