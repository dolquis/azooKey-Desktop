#!/usr/bin/env python3
"""ドキュメント整合性チェック（読み取り専用・warning-only）。

AGENTS.md の以下の規約を機械的に検査する:
  - R1 状態禁止: README / docs / plans に進捗・状態を示す語を書かない
  - R5 索引: docs/README.md がドキュメント一覧の正典
  - R6 実体との一致: § 参照・スキルミラーが実体と一致する
  - コード参照に行番号・実測件数を書かない（陳腐化するため）

いずれの検査も判定はできるが判断はできない。誤検知は
`<!-- lint:allow <category> -->` をその行（または直前の行）に置くと
その行のその検査だけを免除する。

使い方:
    python3 scripts/docs-lint.py            # 一覧を表示
    python3 scripts/docs-lint.py --strict    # 1件でもあれば exit 1
"""
from __future__ import annotations

import argparse
import os
import re
import sys
from collections import defaultdict
from dataclasses import dataclass, field

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

EXCLUDE_DIR_PREFIXES = ("legacy/", "build/", "third_party/", ".git/", "docs/archive/")

# このファイル自体は運用ドキュメントであり、状態遷移そのものが主題のため
# status-word 検査から除外する（AGENTS.md「Linear 運用」節が正典として指定）。
STATUS_WORD_EXEMPT_FILES = {
    "docs/linear-conventions.md",
}

# 拡張子を持たないが README/docs/plans と同格の統治対象として扱うファイル。
EXTRA_TEXT_FILES = ["THIRD_PARTY_LICENSES"]

# AGENTS.md「エージェントツール構成」が明示する意図的な非対称スキル
# （Claude 専用スキルで .agents/skills/ には置かない）。
MIRROR_ALLOWED_ASYMMETRIC = {"doc-coauthoring"}


def _posix(path: str) -> str:
    """リポジトリ相対パスを常に `/` 区切りへ統一する（Windows では os.sep が `\\`）。"""
    return path.replace(os.sep, "/")


STATUS_WORDS = [
    "現状", "現時点", "実装済み", "未実装", "対応済み", "着手済み", "着手前",
    "暫定", "当面", "現行", "未定", "未確定", "未検証", "残作業", "残課題",
    "完了後", "完了済み", "完了している", "完了しました", "待ち）", "待ちである",
    "TODO", "FIXME", "既知の未確定",
]
STATUS_WORD_RE = re.compile("|".join(re.escape(w) for w in STATUS_WORDS))

# コード参照の行番号（`Foo.cpp:123` 等）と実測件数（「157件中156 passed」等）。
LINE_REF_RE = re.compile(r"`[\w./-]+\.(?:cpp|h|hpp|cc|ps1|cs|py)(?::\d+(?:[-,]\d+)*)`")
COUNT_RE = re.compile(r"\d+\s*件中\s*\d+|全\s*\d+\s*(?:種|個|件)")

ALLOW_RE = re.compile(r"<!--\s*lint:allow\s+([\w,-]+)\s*-->")

MD_LINK_RE = re.compile(r"\[[^\]]*\]\(([^)\s]+)\)")
SECTION_REF_RE = re.compile(
    r"([A-Za-z0-9_./-]+\.md)[`\)\]]*\s*(?:の\s*)?§\s*([0-9]+(?:[.-][0-9]+)*|X-[0-9]+(?:-[0-9]+)*|B[0-9]+)"
)
HEADING_NUM_RE = re.compile(r"^#+\s*(?:§\s*)?([0-9]+(?:[.-][0-9]+)*)[.．\s:：]")
HEADING_MTAG_RE = re.compile(r"^#+\s*(?:§\s*)?(X-[0-9]+(?:-[0-9]+)*|B[0-9]+|M[0-9]+(?:-[A-Z])?)\b")


@dataclass
class Finding:
    category: str
    file: str
    line: int
    message: str


@dataclass
class LintResult:
    findings: list = field(default_factory=list)

    def add(self, category: str, file: str, line: int, message: str) -> None:
        self.findings.append(Finding(category, file, line, message))


def iter_md_files(root: str) -> list[str]:
    out = []
    for dirpath, dirnames, filenames in os.walk(root):
        rel_dir = os.path.relpath(dirpath, root)
        rel_dir = "" if rel_dir == "." else rel_dir + "/"
        if any(rel_dir.startswith(p) for p in EXCLUDE_DIR_PREFIXES):
            dirnames[:] = []
            continue
        dirnames[:] = [d for d in dirnames if not any((rel_dir + d + "/").startswith(p) for p in EXCLUDE_DIR_PREFIXES)]
        for fn in filenames:
            if fn.endswith(".md"):
                out.append(_posix(os.path.normpath(os.path.join(rel_dir, fn))))
    return sorted(out)


def read_lines(root: str, relpath: str) -> list[str]:
    with open(os.path.join(root, relpath), encoding="utf-8", errors="replace") as f:
        return f.readlines()


def allow_categories_for_line(lines: list[str], idx0: int) -> set[str]:
    """idx0（0-index）の行、またはその直前行にある lint:allow を集める。"""
    cats: set[str] = set()
    for i in (idx0, idx0 - 1):
        if 0 <= i < len(lines):
            m = ALLOW_RE.search(lines[i])
            if m:
                cats.update(c.strip() for c in m.group(1).split(","))
    return cats


def check_links(root: str, files: list[str], result: LintResult) -> None:
    for f in files:
        lines = read_lines(root, f)
        for i, line in enumerate(lines):
            for m in MD_LINK_RE.finditer(line):
                target = m.group(1)
                if target.startswith(("http://", "https://", "mailto:", "#")):
                    continue
                path, _, _anchor = target.partition("#")
                if not path:
                    continue
                resolved = os.path.normpath(os.path.join(os.path.dirname(f), path))
                if not os.path.exists(os.path.join(root, resolved)):
                    if "link" in allow_categories_for_line(lines, i):
                        continue
                    result.add("link", f, i + 1, f"リンク切れ: {target}")


def collect_headings(root: str, files: list[str]) -> dict[str, set[str]]:
    heads: dict[str, set[str]] = {}
    for f in files:
        nums: set[str] = set()
        for line in read_lines(root, f):
            m = HEADING_NUM_RE.match(line)
            if m:
                nums.add(m.group(1).replace("-", "."))
            m = HEADING_MTAG_RE.match(line)
            if m:
                nums.add(m.group(1))
        heads[f] = nums
    return heads


def check_sections(root: str, files: list[str], result: LintResult) -> None:
    heads = collect_headings(root, files)
    for f in files:
        lines = read_lines(root, f)
        for i, line in enumerate(lines):
            for m in SECTION_REF_RE.finditer(line):
                target, sec = m.group(1), m.group(2)
                sec_norm = sec if sec.startswith(("X", "B")) else sec.replace("-", ".")
                candidates = [
                    target,
                    _posix(os.path.normpath(os.path.join(os.path.dirname(f), target))),
                    _posix(os.path.normpath(os.path.join("docs", os.path.basename(target)))),
                ]
                matched = next((c for c in candidates if c in heads), None)
                if matched is None:
                    # heads に無い = 走査対象の .md として見つからなかった。legacy/ や
                    # docs/archive/ 配下など実在するが対象外のファイルは黙って除外するが、
                    # docs/ や plans/ を指しながら実ファイルが一つも無い場合は typo の
                    # 可能性が高いため missing target として報告する。
                    if "section" in allow_categories_for_line(lines, i):
                        continue
                    on_disk = any(os.path.exists(os.path.join(root, c)) for c in candidates)
                    in_scope = any(c.startswith(("docs/", "plans/")) for c in candidates)
                    if not on_disk and in_scope:
                        result.add("section", f, i + 1, f"§ 参照先ファイルが存在しない: {target}")
                    continue
                if sec_norm not in heads[matched]:
                    if "section" in allow_categories_for_line(lines, i):
                        continue
                    result.add("section", f, i + 1, f"未解決の § 参照: {target} §{sec}")


def check_index(root: str, result: LintResult) -> None:
    index_path = os.path.join(root, "docs/README.md")
    if not os.path.exists(index_path):
        return
    index_text = open(index_path, encoding="utf-8", errors="replace").read()
    targets = []
    for pat, base in (
        ("docs/*.md", "docs"),
        ("docs/handoff/*.md", "docs/handoff"),
        ("plans/*.md", "plans"),
    ):
        d = os.path.join(root, base)
        if not os.path.isdir(d):
            continue
        for fn in sorted(os.listdir(d)):
            if fn.endswith(".md"):
                targets.append(_posix(os.path.normpath(os.path.join(base, fn))))
    for t in targets:
        if t in ("docs/README.md",):
            continue
        basename = os.path.basename(t)
        if basename not in index_text:
            result.add("index", "docs/README.md", 1, f"未索引: {t}")


def check_mirror(root: str, result: LintResult) -> None:
    claude_dir = os.path.join(root, ".claude/skills")
    agents_dir = os.path.join(root, ".agents/skills")
    if not (os.path.isdir(claude_dir) and os.path.isdir(agents_dir)):
        return
    claude_names = set(os.listdir(claude_dir))
    agents_names = set(os.listdir(agents_dir))
    for name in sorted(claude_names ^ agents_names):
        if name in MIRROR_ALLOWED_ASYMMETRIC:
            continue
        if name in claude_names:
            result.add("mirror", f".claude/skills/{name}", 1, ".agents 側にスキルディレクトリが存在しない")
        else:
            result.add("mirror", f".agents/skills/{name}", 1, ".claude 側にスキルディレクトリが存在しない")
    shared = sorted(claude_names & agents_names)
    for name in shared:
        for rel in ("SKILL.md",):
            cp = os.path.join(claude_dir, name, rel)
            ap = os.path.join(agents_dir, name, rel)
            if not (os.path.exists(cp) and os.path.exists(ap)):
                continue
            c_body = strip_frontmatter(open(cp, encoding="utf-8", errors="replace").read())
            a_body = strip_frontmatter(open(ap, encoding="utf-8", errors="replace").read())
            if c_body != a_body:
                result.add("mirror", f".claude/skills/{name}/SKILL.md", 1, f"本文が .agents 側と不一致: {name}")
        for sub in ("references",):
            cdir = os.path.join(claude_dir, name, sub)
            adir = os.path.join(agents_dir, name, sub)
            if os.path.isdir(cdir) and os.path.isdir(adir):
                for fn in sorted(set(os.listdir(cdir)) | set(os.listdir(adir))):
                    cf, af = os.path.join(cdir, fn), os.path.join(adir, fn)
                    if os.path.exists(cf) != os.path.exists(af):
                        result.add("mirror", f".claude/skills/{name}/{sub}/{fn}", 1, "片方のツリーにのみ存在")
                    elif os.path.exists(cf) and open(cf, encoding="utf-8", errors="replace").read() != open(
                        af, encoding="utf-8", errors="replace"
                    ).read():
                        result.add("mirror", f".claude/skills/{name}/{sub}/{fn}", 1, "本文が .agents 側と不一致")


def strip_frontmatter(text: str) -> str:
    if text.startswith("---\n"):
        end = text.find("\n---\n", 4)
        if end != -1:
            return text[end + 5 :]
    return text


def check_status_words(root: str, files: list[str], result: LintResult) -> None:
    scoped = [
        f
        for f in files + EXTRA_TEXT_FILES
        if (f == "README.md" or f.startswith("docs/") or f.startswith("plans/") or f in EXTRA_TEXT_FILES)
        and f not in STATUS_WORD_EXEMPT_FILES
    ]
    for f in scoped:
        lines = read_lines(root, f)
        for i, line in enumerate(lines):
            m = STATUS_WORD_RE.search(line)
            if not m:
                continue
            if "status" in allow_categories_for_line(lines, i):
                continue
            result.add("status", f, i + 1, f"状態語の可能性: 「{m.group(0)}」")


def check_line_refs(root: str, files: list[str], result: LintResult) -> None:
    scoped = [
        f
        for f in files + EXTRA_TEXT_FILES
        if f == "README.md" or f.startswith("docs/") or f.startswith("plans/") or f in EXTRA_TEXT_FILES
    ]
    for f in scoped:
        lines = read_lines(root, f)
        for i, line in enumerate(lines):
            allow = allow_categories_for_line(lines, i)
            for m in LINE_REF_RE.finditer(line):
                if "line-ref" in allow:
                    continue
                result.add("line-ref", f, i + 1, f"行番号付きコード参照: {m.group(0)}")
            for m in COUNT_RE.finditer(line):
                if "line-ref" in allow:
                    continue
                result.add("line-ref", f, i + 1, f"実測件数の記述: {m.group(0)}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--strict", action="store_true", help="1件でもあれば exit 1")
    parser.add_argument(
        "--category",
        action="append",
        choices=["link", "section", "index", "mirror", "status", "line-ref"],
        help="実行するカテゴリを限定（複数指定可、省略時は全部）",
    )
    args = parser.parse_args()

    files = iter_md_files(ROOT)
    result = LintResult()
    categories = set(args.category) if args.category else {"link", "section", "index", "mirror", "status", "line-ref"}

    if "link" in categories:
        check_links(ROOT, files + EXTRA_TEXT_FILES, result)
    if "section" in categories:
        check_sections(ROOT, files, result)
    if "index" in categories:
        check_index(ROOT, result)
    if "mirror" in categories:
        check_mirror(ROOT, result)
    if "status" in categories:
        check_status_words(ROOT, files, result)
    if "line-ref" in categories:
        check_line_refs(ROOT, files, result)

    by_cat: dict[str, list[Finding]] = defaultdict(list)
    for finding in result.findings:
        by_cat[finding.category].append(finding)

    total = len(result.findings)
    for cat in sorted(by_cat):
        items = sorted(by_cat[cat], key=lambda x: (x.file, x.line))
        print(f"\n== {cat} ({len(items)}) ==")
        for it in items:
            print(f"  {it.file}:{it.line}  {it.message}")

    print(f"\n合計 {total} 件（warning）")
    if args.strict and total:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
