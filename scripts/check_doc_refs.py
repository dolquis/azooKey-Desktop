#!/usr/bin/env python3
"""Check that repository docs only point at things that exist.

docs-lint reads prose, not the code it describes, so a rename or a deleted
file leaves the docs naming something that is gone, and nothing fails. This
script closes that gap. It only compares; it never rewrites a doc.

Rules:

1. Path. In the target docs (README.md, AGENTS.md, CLAUDE.md, docs/**/*.md,
   plans/*.md, .claude/rules/*.md; docs/archive/ and docs/decisions/ are frozen
   records and the docs-refactoring plan is a one-off, so they are skipped),
   every inline code span outside a code fence that looks like a repository
   path must name a file or directory that exists. A span looks like a path
   when its first segment is a top-level entry of the repository and it either
   contains `/` or has a file extension; spans written relative to the doc
   (`decisions/x.md` from docs/README.md, `../README.md`) count too. A trailing
   `:line` or `#anchor` is dropped first. Spans with spaces, `<placeholder>`,
   `%VAR%` / `$VAR`, URLs and braces are not path claims. A glob (`*`, `?`)
   and an elided path (`legacy/.../Foo.swift`) must match at least one file.
   Paths that `.gitignore` covers (build output, runtime captures) and paths
   inside a submodule are accepted without looking.
2. Symbol. A span `path::Name` (or `path::Class::Method`) needs the file to
   exist and the last identifier on the right to occur in it as a word. The
   left side is a path as in rule 1, or a bare source file name
   (`TextService.cpp::OnKeyDown`) that exactly one file in the repository has.
3. Section. `docs/<name>.md §N`, `plans/<name>.md §N` and `<name>-spec(.md)? §N`
   (with or without back-quotes or a link, in docs and in code comments under
   the code directories, the justfile and scripts/) must name a section of that
   document: a heading whose text starts with the number (`N.`, `N.M `, `7-3.`,
   `§N`), or a bold numbered item (`- **6.4.7 名前**`). Numbers may carry a
   letter (`6.4.1a`). `<name>-spec.md section N` in code is read the same way,
   and a spec name wrapped across comment lines is joined first. A run such as
   `§3、§4.2` after one document checks every number, and `同 §N` reuses the
   document named last before it in the same paragraph (never across table
   rows). A short name counts too (`app-profile §3.1`): it stands for
   `docs/<name>-spec.md`, `docs/<name>.md`, `docs/handoff/<name>.md`, or, when
   none of those exists, every `docs/<name>-*-spec.md` (for names of four
   letters or more). One document is checked as usual. Two or more make the
   name ambiguous, and the reference is reported until it is written with the
   `-spec` name (otherwise it would silently stop being checked when a spec is
   split). A name that stands for none is prose and is ignored, and so are
   `README`, `AGENTS` and `CLAUDE`, which also name root files. An ordinary
   word that happens to be the start of a `docs/<word>-*-spec.md` is read as
   that document, so write `§` after a plain word only for a spec.
   A section the document no longer holds resolves through its forwarding
   table (rule 6).
4. Allowlist. A reference that does not exist yet but is correct (a planned
   file, a file that lives only in the origin repository, a path in another
   repository, a historical or runtime path) passes only when
   `scripts/doc-refs-allowlist.toml` lists it with a `kind` and a `reason`;
   `planned` entries also name the `milestone` and the canonical `spec`
   section (checked like rule 3), and `files` limits the docs an entry may
   excuse. An entry whose
   target now exists, or that no doc mentions any more, fails so the list
   cannot rot.
5. Line exemption. `<!-- doc-refs:allow -->` in a doc exempts the line it is
   on, or the next line when it stands alone. An exemption that hides nothing
   fails.
6. Forwarding table. A spec that moved sections to other documents keeps a
   `## 転送表` heading (any level) whose table has the header `旧節 | 移動先`
   and one row per destination (a table inside a code fence is a sample and is
   ignored):

       | 旧節 | 移動先 |
       |---|---|
       | §2、§3、§9〜§11 | `docs/new-spec.md` |
       | §12.3 | `docs/other-spec.md` §M |
       | X-3-6 | `docs/typo-spec.md` §M |

   The left cell lists `§N` keys (a `§A〜§B` range of whole numbers expands),
   or `X-N-M` keys, separated by `、` or `,`. The right cell is one back-quoted
   `docs/**.md` path that exists and is not the document itself; the optional
   `§M` after it (M is a section number) renumbers: the key's number is
   replaced by M and the rest of the reference keeps its separators (a key
   `X-7` renumbered to `§8` turns a reference ending `-6` into `8-6`). Without
   it the number is kept. A key covers its
   sub-sections (`§4` covers `§4.3` and `§4.3a`); the longest key wins.
   `<doc> §N` is resolved by the document's own headings first, then by the
   table, and the destination must hold the resulting number in its headings.
   A destination may forward again (at most 3 hops; a cycle fails). Every row
   is checked: the destination must hold the key's own number (or `M`) in its
   headings. A heading holds one table, written without a blank line inside; a
   second table needs a `転送表` heading of its own.
7. Item. An `X-N-M` item number (rich-features) written anywhere in the tracked
   files with a text suffix listed in ITEM_SCAN_SUFFIXES (docs, code comments,
   skills, schemas; not the frozen docs, not scripts/tests/, not the
   allowlist) must be a heading `X-N-M.` of
   docs/rich-features-spec.md or be covered by a forwarding table row, in that
   document or in any other.
8. Forwarding group. A document with forwarding table rows that keep the
   number, and those rows' destinations, form one `§` namespace. A bare `§N`
   (no document named) in a group member must name a heading of any member.
   A `§` is not looked at when it may point at another document: a document
   name (`-spec`, `.md`, `docs/`, `plans/`, `roadmap`, `architecture`) comes
   earlier on the line, an ASCII word (`M60`, `app-profile`, `DEV-1975`) comes
   right before it, or a `同` or a list separator comes right before it. When
   the `§` starts the line, the end of the previous non-blank line stands in:
   a document name in its last 80 characters, or an ASCII word at its end,
   also skips the `§`. In a list
   (`§4、§5`) the head of the list decides for all of it. A leftover false
   positive takes a `<!-- doc-refs:allow -->` marker. Specs outside a group
   are not checked for bare `§N`.

The file list comes from `git ls-files` (tracked plus untracked files that are
not ignored), so a file you have created but not committed counts locally;
CI sees only what the commit holds.

Exit status: 0 when every reference resolves, 1 otherwise.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass, field
import fnmatch
from pathlib import Path, PurePosixPath
import posixpath
import re
import subprocess
import sys
import tomllib


sys.dont_write_bytecode = True
REPO_ROOT = Path(__file__).resolve().parents[1]
ALLOWLIST_RELATIVE_PATH = "scripts/doc-refs-allowlist.toml"

ROOT_DOCUMENTS = ("README.md", "AGENTS.md", "CLAUDE.md")
# Every .md under docs/ (any depth), and only the top level of these.
FLAT_DOCUMENT_DIRECTORIES = ("plans", ".claude/rules")
DOC_EXCLUDED_PREFIXES = ("docs/archive/", "docs/decisions/")
DOC_EXCLUDED_FILES = {"docs/handoff/docs-refactoring-plan.md"}

CODE_DIRECTORIES = (
    "core/",
    "ipc/",
    "inference-host/",
    "tsf-tip/",
    "learning/",
    "settings/",
    "settings-app/",
    "diagnostics/",
    "bench/",
    "compat-test/",
    "pkg/",
)
CODE_SUFFIXES = (".h", ".hpp", ".cpp", ".ps1", ".json", ".cmake")
CODE_NAMES = ("CMakeLists.txt",)

# Never walked: nested worktrees, build trees and vendored code are not docs or
# comments this repository owns.
SKIPPED_PREFIXES = (".claude/worktrees/", "build/", "out/", "third_party/", "legacy/")

# `git check-ignore` に渡す仮のファイル名。`.gitignore` の規則に一致しない名前にする
# （`__*` や `*probe*` のような広い規則を足すと、全パスが無視扱いになる）。
IGNORE_PROBE_NAME = "__doc_refs_probe__"

ALLOWED_KINDS = {
    "planned": ("milestone", "spec"),
    "origin": (),
    "external": (),
    "historical": (),
    "runtime": (),
    # 所有する M が無い予定は、判断を追う Linear の課題を必ず持たせる（放置の逃げ道にしない）。
    "unowned": ("issue",),
}
ISSUE_PATTERN = re.compile(r"^DEV-[0-9]+$")

FENCE_PATTERN = re.compile(r"^\s*(```|~~~)")
INLINE_CODE_PATTERN = re.compile(r"(`+)(.+?)\1")
HEADING_PATTERN = re.compile(r"^(#{1,6})\s+(.*)$")
HEADING_NUMBER_PATTERN = re.compile(r"^§?\s*(\d+(?:[.-]\d+)*[a-z]?)(?=[.\s:：）)]|$)")
# `- **6.4.7 名前** — ...`: numbered items inside a section are anchors too.
BOLD_ITEM_PATTERN = re.compile(r"^\s*(?:[-*]|\d+\.)\s+\*\*§?\s*(\d+(?:[.-]\d+)+[a-z]?)(?=[.\s:：）)*])")
LINE_SUFFIX_PATTERN = re.compile(r"(?::\d+(?:[-:]\d+)*|#[^/]*)$")
IDENTIFIER_PATTERN = re.compile(r"[A-Za-z_~][A-Za-z0-9_]*")
EXTENSION_PATTERN = re.compile(r"\.[A-Za-z0-9]{1,8}$")
ALLOW_MARKER_PATTERN = re.compile(r"<!--\s*doc-refs:allow\b[^>]*-->")

SECTION_NUMBER = r"\d+(?:[.-]\d+)*[a-z]?"
# A document followed by `§N`: either a repo .md path or a `<name>-spec` name,
# optionally in back-quotes or as a markdown link target.
DOCUMENT_THEN_SECTION_PATTERN = re.compile(
    r"(?P<doc>(?:(?:docs|plans)/[\w./-]*?\.md)|(?:[\w-]+-spec(?:\.md)?))"
    r"(?:`|\))?\s*(?:§\s*|section\s+)(?P<number>" + SECTION_NUMBER + r")",
    re.ASCII,
)
# Further numbers chained after a reference: `§3、§4.2` / `§3 と §4` / `§3/§4`.
CHAINED_SECTION_PATTERN = re.compile(
    r"^\s*(?:、|,|，|/|と|および|及び|or|and|～|〜|-)\s*§\s*(?P<number>" + SECTION_NUMBER + r")"
)
SAME_DOCUMENT_PATTERN = re.compile(r"同\s*§\s*(?P<number>" + SECTION_NUMBER + r")")
# A short document name before `§N`: `app-profile §3.1`, `linear-conventions.md §2`.
SHORT_NAME_PATTERN = re.compile(
    r"(?<![\w/.-])(?P<name>[A-Za-z][A-Za-z0-9]*(?:-[A-Za-z0-9]+)*)(?:\.md)?`?\s*§\s*(?P<number>"
    + SECTION_NUMBER
    + r")",
    re.ASCII,
)
SHORT_NAME_MIN_PREFIX = 4
# These also name a root file (README.md, AGENTS.md, CLAUDE.md), so they stay prose.
SHORT_NAME_ROOT_DOCUMENTS = ("README", "AGENTS", "CLAUDE")

# rich-features item numbers (`X-3-6`); a heading may also be `X-3`.
ITEM_ID = r"X-\d+(?:-\d+)*"
ITEM_ID_PATTERN = re.compile(ITEM_ID)
ITEM_PATTERN = re.compile(r"(?<![A-Za-z0-9_-])(?P<id>X-\d+-\d+)(?![A-Za-z0-9_-])")
ITEM_HEADING_PATTERN = re.compile(r"^(" + ITEM_ID + r")(?=[.\s:：）)]|$)")
ITEM_DOCUMENT = "docs/rich-features-spec.md"
ITEM_SCAN_SUFFIXES = (
    ".md",
    ".h",
    ".hpp",
    ".c",
    ".cc",
    ".cpp",
    ".cs",
    ".ps1",
    ".py",
    ".json",
    ".toml",
    ".yml",
    ".yaml",
    ".cmake",
    ".txt",
    ".idl",
    ".xaml",
    ".wxs",
)
ITEM_SCAN_NAMES = ("justfile",)
ITEM_EXCLUDED_PREFIXES = ("scripts/tests/",)

# Forwarding table: a `転送表` heading (a section number may precede it), then a
# table `旧節 | 移動先`.
FORWARD_HEADING_PATTERN = re.compile(r"^#{1,6}\s+(?:§?\d+(?:[.-]\d+)*\.?\s+)?転送表\s*$")
FORWARD_HEADER = ["旧節", "移動先"]
FORWARD_SEPARATOR_PATTERN = re.compile(r"^:?-{3,}:?$")
FORWARD_KEY_SEPARATOR_PATTERN = re.compile(r"\s*[、,，]\s*")
FORWARD_RANGE_PATTERN = re.compile(r"^§\s*(\d+)\s*[〜~～]\s*§?\s*(\d+)$")
FORWARD_KEY_PATTERN = re.compile(r"^(?:§\s*(" + SECTION_NUMBER + r")|(" + ITEM_ID + r"))$")
FORWARD_DESTINATION_PATTERN = re.compile(
    r"^`(?P<path>[^`\s]+)`(?:\s*§\s*(?P<section>" + SECTION_NUMBER + r"))?$"
)
FORWARD_RANGE_LIMIT = 200
FORWARD_MAX_HOPS = 3
UNRESOLVED_SECTION = "見出しに解決しない § 参照"
# What may follow a forwarding key inside a section number: `.4`, `-2`, `.4.1a`, `a`.
SECTION_SUFFIX_PATTERN = re.compile(r"(?:[.-]\d+)*[a-z]?")

# A bare `§N` beside a document name, or after `同` or a list separator, names
# another document.
BARE_SECTION_PATTERN = re.compile(r"§\s*(?P<number>" + SECTION_NUMBER + r")")
BARE_OTHER_DOCUMENT_PATTERN = re.compile(
    r"-spec|\.md|docs/|plans/|roadmap|architecture", re.IGNORECASE
)
BARE_CHAIN_BEFORE_PATTERN = re.compile(r"(?:同|[/、,，・〜～]|と|および|及び)\s*$")
# `§4、§5、` before a `§`: the references of one list, which share the head's document.
BARE_CHAIN_ITEMS_PATTERN = re.compile(
    r"(?:§\s*" + SECTION_NUMBER + r"\s*(?:、|,|，|/|・|〜|～|と|および|及び)\s*)+$"
)
# An ASCII word right before a `§` (`M60 §4`, `DEV-1975 §3`, `**app-profile** §3`) is a
# name, so the section is not this document's.
BARE_NAME_BEFORE_PATTERN = re.compile(r"[A-Za-z0-9_][*_`)\]]*\s*$")


@dataclass(frozen=True)
class Finding:
    location: str
    line: int
    reference: str
    message: str

    def render(self) -> str:
        return f"{self.location}:{self.line}: {self.message} `{self.reference}`"


@dataclass
class AllowEntry:
    ref: str
    kind: str
    reason: str
    milestone: str = ""
    spec: str = ""
    files: tuple[str, ...] = ()
    used: bool = False

    def covers(self, finding: "Finding") -> bool:
        return finding.reference == self.ref and (
            not self.files or finding.location in self.files
        )


@dataclass(frozen=True)
class ForwardRule:
    """One key of a forwarding table: where a moved section (or item) lives now."""

    key: str  # `4`, `4.3`, `X-3-6`
    destination: str  # `docs/x.md`
    section: str = ""  # renumbering target; empty keeps the number
    line: int = 0


@dataclass
class Repository:
    root: Path
    files: set[str]
    directories: set[str]
    top_level: set[str]
    submodules: tuple[str, ...]
    headings: dict[str, set[str] | None] = field(default_factory=dict)
    casefolded: dict[str, str] = field(default_factory=dict)
    basenames: dict[str, list[str]] = field(default_factory=dict)
    texts: dict[str, str] = field(default_factory=dict)
    # Forwarding tables, by the document that holds them: {document: {key: rule}}.
    forwarding: dict[str, dict[str, ForwardRule]] = field(default_factory=dict)
    forwarding_problems: list[str] = field(default_factory=list)
    forwarding_lines: dict[str, set[int]] = field(default_factory=dict)
    # Documents that share one `§` namespace: {member: all members of its groups}.
    groups: dict[str, frozenset[str]] = field(default_factory=dict)
    short_documents: dict[str, list[str]] = field(default_factory=dict)

    def exists(self, path: str) -> bool:
        return path in self.files or path in self.directories

    def read(self, path: str) -> str:
        if path not in self.texts:
            try:
                self.texts[path] = (self.root / path).read_text(encoding="utf-8")
            except (OSError, UnicodeDecodeError):
                self.texts[path] = ""
        return self.texts[path]

    def section_numbers(self, path: str) -> set[str] | None:
        if path not in self.headings:
            self.headings[path] = (
                collect_section_numbers(self.read(path)) if path in self.files else None
            )
        return self.headings[path]


def git_lines(root: Path, *arguments: str, stdin: str | None = None) -> list[str]:
    # stdin と stdout は bytes で受け渡す。text=True にすると Windows では stdin の
    # "\n" が "\r\n" に変わり、git が CR 付きのパスを受け取って判定が CI と食い違う。
    result = subprocess.run(
        ["git", "-C", str(root), *arguments],
        input=None if stdin is None else stdin.encode("utf-8"),
        capture_output=True,
        check=False,
    )
    if result.returncode not in (0, 1):
        message = result.stderr.decode("utf-8", errors="replace").strip()
        raise RuntimeError(f"git {' '.join(arguments)} failed: {message}")
    separator = "\0" if "-z" in arguments else "\n"
    return [line for line in result.stdout.decode("utf-8").split(separator) if line]


def load_repository(root: Path) -> Repository:
    listed = git_lines(root, "ls-files", "-z", "--cached", "--others", "--exclude-standard")
    files = {
        path
        for path in listed
        if not path.endswith("/") and not path.startswith(".claude/worktrees/")
    }
    submodules = tuple(
        line.split("\t", 1)[1]
        for line in git_lines(root, "ls-files", "-s")
        if line.startswith("160000 ")
    )
    directories: set[str] = set(submodules)
    for path in files | set(submodules):
        parent = PurePosixPath(path).parent
        while str(parent) != ".":
            if str(parent) in directories:
                break
            directories.add(str(parent))
            parent = parent.parent
    top_level = {path.split("/", 1)[0] for path in files | directories}
    repo = Repository(root, files, directories, top_level, submodules)
    repo.casefolded = {path.casefold(): path for path in files if path.endswith(".md")}
    for path in sorted(files):
        repo.basenames.setdefault(posixpath.basename(path), []).append(path)
    collect_forwarding(repo)
    return repo


def split_table_row(line: str) -> list[str]:
    row = line.strip()
    row = row.removeprefix("|").removesuffix("|")
    return [cell.strip() for cell in row.split("|")]


def parse_forward_keys(cell: str) -> tuple[list[str], str | None]:
    """Return (keys, problem) for the left cell of a forwarding row."""
    keys: list[str] = []
    for token in FORWARD_KEY_SEPARATOR_PATTERN.split(cell.strip()):
        token = token.strip()
        span = FORWARD_RANGE_PATTERN.match(token)
        single = FORWARD_KEY_PATTERN.match(token)
        if span:
            first, last = int(span.group(1)), int(span.group(2))
            if not first <= last <= first + FORWARD_RANGE_LIMIT:
                return [], f"範囲 `{token}` が昇順の整数でない（{FORWARD_RANGE_LIMIT} 節まで）"
            keys.extend(str(number) for number in range(first, last + 1))
        elif single:
            keys.append(single.group(1) or single.group(2))
        else:
            return [], f"節 `{token}` を読めない（`§N`、`§A〜§B`、`X-N-M` のいずれか）"
    return keys, None


def parse_forwarding(
    repo: Repository, path: str
) -> tuple[dict[str, ForwardRule], list[str], set[int]]:
    """Read every `転送表` table of a document: (rules, problems, table line numbers)."""
    lines = strip_code_fences(repo.read(path).splitlines())
    rules: dict[str, ForwardRule] = {}
    problems: list[str] = []
    table_lines: set[int] = set()
    index = 0
    while index < len(lines):
        if not FORWARD_HEADING_PATTERN.match(lines[index]):
            index += 1
            continue
        heading_line = index + 1
        index += 1
        while (
            index < len(lines)
            and not lines[index].lstrip().startswith("|")
            and not HEADING_PATTERN.match(lines[index])
        ):
            index += 1
        rows: list[tuple[int, list[str]]] = []
        while index < len(lines) and lines[index].lstrip().startswith("|"):
            rows.append((index + 1, split_table_row(lines[index])))
            index += 1
        if not rows:
            problems.append(f"{path}:{heading_line}: 転送表の見出しの下に表が無い")
            continue
        follow = index
        while follow < len(lines) and not lines[follow].strip():
            follow += 1
        if follow < len(lines) and lines[follow].lstrip().startswith("|"):
            problems.append(
                f"{path}:{follow + 1}: 転送表の後に別の表が続く"
                "（空行で表を分けず、別の表には見出しを付ける）"
            )
        table_lines.update(number for number, _ in rows)
        if rows[0][1] != FORWARD_HEADER:
            problems.append(f"{path}:{rows[0][0]}: 転送表の見出し行は `| 旧節 | 移動先 |`")
            continue
        if len(rows) < 3 or not all(FORWARD_SEPARATOR_PATTERN.match(c) for c in rows[1][1]):
            problems.append(f"{path}:{rows[0][0]}: 転送表に区切り行（`|---|---|`）か行が無い")
            continue
        for number, cells in rows[2:]:
            if len(cells) != 2:
                problems.append(f"{path}:{number}: 転送表の行は 2 列（旧節 | 移動先）")
                continue
            keys, problem = parse_forward_keys(cells[0])
            if problem:
                problems.append(f"{path}:{number}: 転送表 {problem}")
                continue
            target = FORWARD_DESTINATION_PATTERN.match(cells[1])
            if not target:
                problems.append(
                    f"{path}:{number}: 転送先 `{cells[1]}` を読めない（`docs/x.md` か `docs/x.md` §M）"
                )
                continue
            destination = target.group("path")
            section = target.group("section") or ""
            if not (destination.startswith("docs/") and destination.endswith(".md")):
                problems.append(f"{path}:{number}: 転送先 `{destination}` は docs/ の .md に限る")
                continue
            if destination == path:
                problems.append(f"{path}:{number}: 転送先が自分自身")
                continue
            if destination not in repo.files:
                problems.append(f"{path}:{number}: 転送先 `{destination}` が存在しない")
                continue
            if section and len(keys) > 1:
                problems.append(f"{path}:{number}: 複数の節の行は転送先の § を書けない")
                continue
            for key in keys:
                if key in rules:
                    problems.append(f"{path}:{number}: 転送表の節 `{key}` が重複している")
                    continue
                rules[key] = ForwardRule(key, destination, section, number)
    return rules, problems, table_lines


def collect_forwarding(repo: Repository) -> None:
    """Fill repo.forwarding and repo.groups from the `転送表` tables under docs/."""
    for path in sorted(repo.files):
        if not (path.startswith("docs/") and is_target_document(path)):
            continue
        if "転送表" not in repo.read(path):
            continue
        rules, problems, table_lines = parse_forwarding(repo, path)
        repo.forwarding_problems.extend(problems)
        if table_lines:
            repo.forwarding_lines[path] = table_lines
        if rules:
            repo.forwarding[path] = rules
    members: dict[str, set[str]] = {}
    for source, rules in repo.forwarding.items():
        kept = {
            rule.destination
            for rule in rules.values()
            if not rule.section and not ITEM_ID_PATTERN.fullmatch(rule.key)
        }
        if not kept:
            continue
        for member in {source} | kept:
            members.setdefault(member, set()).update({source} | kept)
    repo.groups = {path: frozenset(group) for path, group in members.items()}


def collect_section_numbers(text: str) -> set[str]:
    numbers: set[str] = set()
    in_fence = False
    for line in text.splitlines():
        if FENCE_PATTERN.match(line):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        heading = HEADING_PATTERN.match(line)
        if heading:
            title = heading.group(2).strip()
            number = HEADING_NUMBER_PATTERN.match(title) or ITEM_HEADING_PATTERN.match(title)
            if number:
                numbers.add(number.group(1))
            continue
        item = BOLD_ITEM_PATTERN.match(line)
        if item:
            numbers.add(item.group(1))
    return numbers


def is_target_document(path: str) -> bool:
    if path in DOC_EXCLUDED_FILES or path.startswith(DOC_EXCLUDED_PREFIXES):
        return False
    if path in ROOT_DOCUMENTS:
        return True
    if not path.endswith(".md"):
        return False
    directory = str(PurePosixPath(path).parent)
    return path.startswith("docs/") or directory in FLAT_DOCUMENT_DIRECTORIES


def target_documents(repo: Repository) -> list[str]:
    return sorted(path for path in repo.files if is_target_document(path))


def code_files(repo: Repository) -> list[str]:
    selected = []
    for path in sorted(repo.files):
        if path.startswith(SKIPPED_PREFIXES):
            continue
        name = PurePosixPath(path).name
        if path == "justfile":
            selected.append(path)
        elif path.startswith(CODE_DIRECTORIES) and (
            name.endswith(CODE_SUFFIXES) or name in CODE_NAMES
        ):
            selected.append(path)
        elif path.startswith("scripts/") and path.count("/") == 1 and name.endswith((".py", ".ps1")):
            selected.append(path)
    return selected


def normalize_path_token(token: str) -> str | None:
    """Return the path text a code span claims, or None if it claims none."""
    token = token.strip().replace("\\", "/")
    if not token or any(character in token for character in " \t<>{}|\"'$%@="):
        return None
    if "://" in token or token.startswith(("-", "/", "~")):
        return None
    if token.startswith("./"):
        token = token[2:]
    token = LINE_SUFFIX_PATTERN.sub("", token)
    # `legacy/.../Foo.swift` elides directories; check it as a glob.
    token = "/".join("*" if part in ("...", "…") else part for part in token.split("/"))
    return token or None


def resolve_path_claim(repo: Repository, token: str, base: str = "") -> str | None:
    """Return the repository path a code span names, or None if it is not a path.

    `base` is the directory of the document, for spans such as `decisions/x.md`
    or `../README.md` that are written relative to it.
    """
    path = normalize_path_token(token)
    if path is None:
        return None
    head, separator, _ = path.partition("/")
    if head in repo.top_level:
        return path if separator or EXTENSION_PATTERN.search(path) else None
    if not separator or not base:
        return None
    if head == "..":
        resolved = posixpath.normpath(posixpath.join(base, path))
        if resolved.startswith("..") or resolved == ".":
            return None
        return resolved + ("/" if path.endswith("/") else "")
    if posixpath.join(base, head) in repo.directories:
        return posixpath.join(base, path)
    return None


def under_submodule(repo: Repository, path: str) -> bool:
    return any(path == module or path.startswith(module + "/") for module in repo.submodules)


def glob_matches(repo: Repository, pattern: str) -> bool:
    pattern = pattern.rstrip("/")
    return any(fnmatch.fnmatchcase(path, pattern) for path in repo.files | repo.directories)


def strip_code_fences(lines: list[str]) -> list[str]:
    """Blank out lines inside fenced code blocks, keeping line numbers."""
    visible = []
    in_fence = False
    for line in lines:
        if FENCE_PATTERN.match(line):
            in_fence = not in_fence
            visible.append("")
            continue
        visible.append("" if in_fence else line)
    return visible


SOURCE_NAME_PATTERN = re.compile(r"^[A-Za-z0-9_.-]+\.(?:cpp|h|hpp|c|cc|cs|ps1|py)$")


def unique_basename(repo: Repository, name: str) -> str | None:
    """`TextService.cpp::OnKeyDown`: a bare file name counts when only one file has it."""
    if not SOURCE_NAME_PATTERN.match(name):
        return None
    matches = repo.basenames.get(name, [])
    if not matches:
        # 一致 0 件は rename・削除された可能性が高いので、不在として報告させる。
        return name
    return matches[0] if len(matches) == 1 else None


def check_code_span(repo: Repository, token: str, base: str = "") -> tuple[str, str] | None:
    """Return (reference, message) when the span names something missing."""
    if "::" in token:
        left, _, right = token.partition("::")
        left_path = resolve_path_claim(repo, left, base) or unique_basename(repo, left)
        if left_path is None or any(character in left_path for character in "*?["):
            return None
        reference = f"{left_path}::{right}"
        if not repo.exists(left_path):
            if under_submodule(repo, left_path):
                return None
            return reference, "存在しないファイル"
        identifiers = IDENTIFIER_PATTERN.findall(right)
        if not identifiers or left_path not in repo.files:
            return None
        symbol = identifiers[-1]
        if not re.search(
            r"(?<![A-Za-z0-9_])" + re.escape(symbol) + r"(?![A-Za-z0-9_])", repo.read(left_path)
        ):
            return reference, f"ファイルに現れないシンボル `{symbol}`"
        return None
    path = resolve_path_claim(repo, token, base)
    if path is None:
        return None
    path = path.rstrip("/")
    if any(character in path for character in "*?["):
        return None if glob_matches(repo, path) else (path, "一致するファイルが無い glob")
    if repo.exists(path) or under_submodule(repo, path):
        return None
    return path, "存在しないパス"


def resolve_document(repo: Repository, name: str, source: str) -> str:
    """Map a document name in a § reference to a repository path."""
    if name.startswith(("docs/", "plans/")):
        return name
    if not name.endswith(".md"):
        name += ".md"
    for candidate in (posixpath.join(posixpath.dirname(source), name), f"docs/{name}"):
        if candidate in repo.files:
            return candidate
        folded = repo.casefolded.get(candidate.casefold())
        if folded:
            return folded
    return f"docs/{name}"


def section_findings(repo: Repository, text: str, source: str, is_doc: bool) -> list[tuple[int, str, str]]:
    """Return (line, reference, message) for § references that do not resolve."""
    results: list[tuple[int, str, str]] = []
    lines = text.splitlines()
    if is_doc:
        lines = strip_code_fences(lines)
    else:
        lines = join_wrapped_names(lines)
    last_document: str | None = None
    for number, line in enumerate(lines, start=1):
        # `同 §N` reuses the document named last in the same paragraph, but
        # never one from another table row.
        if not line.strip() or line.lstrip().startswith("|"):
            last_document = None
        events = sorted(
            [(match.start(), match) for match in DOCUMENT_THEN_SECTION_PATTERN.finditer(line)]
            + [(match.start(), match) for match in SHORT_NAME_PATTERN.finditer(line)]
            + [(match.start(), match) for match in SAME_DOCUMENT_PATTERN.finditer(line)],
            key=lambda event: event[0],
        )
        for _, match in events:
            numbers = [match.group("number")]
            ambiguous: list[str] = []
            if match.re is SAME_DOCUMENT_PATTERN:
                document = last_document
            else:
                if match.re is SHORT_NAME_PATTERN:
                    # `-spec` names belong to DOCUMENT_THEN_SECTION_PATTERN.
                    if match.group("name").endswith("-spec"):
                        continue
                    candidates = short_document_candidates(repo, match.group("name"))
                    if not candidates:
                        continue
                    document = candidates[0] if len(candidates) == 1 else None
                    ambiguous = candidates if len(candidates) > 1 else []
                else:
                    if is_doc and "section" in match.group(0):
                        continue
                    document = resolve_document(repo, match.group("doc"), source)
                last_document = document
                rest = line[match.end():]
                while chained := CHAINED_SECTION_PATTERN.match(rest):
                    numbers.append(chained.group("number"))
                    rest = rest[chained.end():]
            if ambiguous:
                # A short name that several documents share would silently stop being
                # checked, so it is reported until it is written with the `-spec` name.
                named = "、".join(f"`{path}`" for path in ambiguous)
                for section in numbers:
                    results.append(
                        (
                            number,
                            f"{match.group('name')} §{section}",
                            f"曖昧な短縮名（候補 {named}）。`-spec` 付きの文書名で書く",
                        )
                    )
                continue
            if document is None:
                continue
            for section in numbers:
                problem = resolve_section(repo, document, section)
                if problem:
                    results.append((number, f"{document} §{section}", problem))
    return results


WRAPPED_HEAD_PATTERN = re.compile(r"([A-Za-z][\w-]*-)$")
COMMENT_LEADER_PATTERN = re.compile(r"^\s*(?://+|#+|\*|;+|--|<!--)?\s*")


def join_wrapped_names(lines: list[str]) -> list[str]:
    """Undo a comment wrapped inside a hyphenated name (`user-learning-` / `enhancement-spec`)."""
    joined = list(lines)
    for index in range(1, len(lines)):
        head = WRAPPED_HEAD_PATTERN.search(lines[index - 1].rstrip())
        if not head:
            continue
        leader = COMMENT_LEADER_PATTERN.match(lines[index])
        rest = lines[index][leader.end():] if leader else lines[index]
        if re.match(r"[A-Za-z0-9_-]*spec\b", rest):
            joined[index] = head.group(1) + rest
    return joined


def match_forward_rule(rules: dict[str, ForwardRule], section: str) -> ForwardRule | None:
    """The rule whose key is `section` or its longest ancestor (`4` covers `4.3a`)."""
    best: ForwardRule | None = None
    for key, rule in rules.items():
        rest = section[len(key) :]
        if not section.startswith(key) or (rest and not SECTION_SUFFIX_PATTERN.fullmatch(rest)):
            continue
        if best is None or len(key) > len(best.key):
            best = rule
    return best


def resolve_section(
    repo: Repository, document: str, section: str, trail: tuple[tuple[str, str], ...] = ()
) -> str | None:
    """None when `document` holds `section` (itself, or through its forwarding table)."""
    numbers = repo.section_numbers(document)
    if numbers is None:
        if under_submodule(repo, document):
            return None
        return "§ 参照先の文書が存在しない"
    if section in numbers:
        return None
    rule = match_forward_rule(repo.forwarding.get(document, {}), section)
    if rule is None:
        return UNRESOLVED_SECTION
    step = (document, section)
    if step in trail:
        return f"転送表が循環している（`{document}` §{section}）"
    if len(trail) >= FORWARD_MAX_HOPS:
        return f"転送表の連鎖が {FORWARD_MAX_HOPS} 段を超える（`{document}` §{section}）"
    target = rule.section + section[len(rule.key) :] if rule.section else section
    problem = resolve_section(repo, rule.destination, target, (*trail, step))
    if problem is None:
        return None
    if problem != UNRESOLVED_SECTION:
        # A cycle, a chain too long, or a destination that already says what is missing.
        return problem
    return f"{UNRESOLVED_SECTION}（転送先 `{rule.destination}` に §{target} が無い）"


def short_document_candidates(repo: Repository, name: str) -> list[str]:
    """The documents a short name (`app-profile`) may stand for: none is prose, one is
    the document, several make the name ambiguous."""
    if name.upper() in SHORT_NAME_ROOT_DOCUMENTS:
        return []
    if name not in repo.short_documents:
        found = [
            path
            for path in (f"docs/{name}-spec.md", f"docs/{name}.md", f"docs/handoff/{name}.md")
            if path in repo.files
        ]
        if not found and len(name) >= SHORT_NAME_MIN_PREFIX:
            found = sorted(
                path
                for path in repo.files
                if path.startswith(f"docs/{name}-")
                and path.endswith("-spec.md")
                and path.count("/") == 1
            )
        repo.short_documents[name] = found
    return repo.short_documents[name]


def resolve_item(repo: Repository, item: str) -> str | None:
    """None when an `X-N-M` item has a heading or a forwarding row somewhere."""
    owners = [ITEM_DOCUMENT] if ITEM_DOCUMENT in repo.files else []
    owners += sorted(
        path
        for path, rules in repo.forwarding.items()
        if path != ITEM_DOCUMENT and any(ITEM_ID_PATTERN.fullmatch(key) for key in rules)
    )
    if any(resolve_section(repo, owner, item) is None for owner in owners):
        return None
    return "見出しにも転送表にも無い X-N-M 参照"


def forwarding_table_problems(repo: Repository) -> list[str]:
    """Unreadable tables, plus rows whose destination does not hold what the row promises."""
    problems = list(repo.forwarding_problems)
    for path in sorted(repo.forwarding):
        for rule in repo.forwarding[path].values():
            wanted = rule.section or rule.key
            problem = resolve_section(repo, rule.destination, wanted)
            if problem is None:
                continue
            if problem.startswith("転送表"):
                problems.append(f"{path}:{rule.line}: {problem}")
            else:
                label = f"§{wanted}" if wanted[0].isdigit() else wanted
                problems.append(
                    f"{path}:{rule.line}: 転送先 `{rule.destination}` の見出しに {label} が無い"
                )
    return problems


def item_findings(repo: Repository) -> list[Finding]:
    """`X-N-M` references, in every tracked text file, that resolve nowhere."""
    findings: list[Finding] = []
    for path in sorted(repo.files):
        name = PurePosixPath(path).name
        if path.startswith(SKIPPED_PREFIXES + ITEM_EXCLUDED_PREFIXES):
            continue
        if path in DOC_EXCLUDED_FILES or path.startswith(DOC_EXCLUDED_PREFIXES):
            continue
        # The allowlist is the registry that excuses references, so it cannot be a
        # place that mentions them: an entry would excuse itself.
        if path == ALLOWLIST_RELATIVE_PATH:
            continue
        if not (name.endswith(ITEM_SCAN_SUFFIXES) or name in ITEM_SCAN_NAMES):
            continue
        lines = repo.read(path).splitlines()
        if path.endswith(".md"):
            lines = strip_code_fences(lines)
        table_lines = repo.forwarding_lines.get(path, set())
        for number, line in enumerate(lines, start=1):
            if number in table_lines or "X-" not in line:
                continue
            for item in dict.fromkeys(match.group("id") for match in ITEM_PATTERN.finditer(line)):
                problem = resolve_item(repo, item)
                if problem:
                    findings.append(Finding(path, number, item, problem))
    return findings


def names_this_document(before: str, previous_line: str) -> bool:
    """True when a bare `§` after `before` can only mean the document it is written in.

    `before` is the text ahead of the `§` on its line; `previous_line` is the last
    non-blank line above, which stands in when the `§` starts the line. Anything that
    may name another document (a document name, an ASCII word, `同`, a list separator)
    makes the answer False, so the check errs toward saying nothing.
    """
    chain = BARE_CHAIN_ITEMS_PATTERN.search(before)
    head = before[: chain.start()] if chain else before
    if BARE_CHAIN_BEFORE_PATTERN.search(head) or BARE_OTHER_DOCUMENT_PATTERN.search(head):
        return False
    text = head.rstrip()
    if not text:
        if BARE_OTHER_DOCUMENT_PATTERN.search(previous_line[-80:]):
            return False
        text = previous_line.rstrip()
    return not BARE_NAME_BEFORE_PATTERN.search(text)


def group_findings(repo: Repository, path: str) -> list[tuple[int, str, str]]:
    """Bare `§N` of a forwarding-group member that no member's headings hold."""
    members = repo.groups.get(path)
    if members is None:
        return []
    results: list[tuple[int, str, str]] = []
    table_lines = repo.forwarding_lines.get(path, set())
    lines = strip_code_fences(repo.read(path).splitlines())
    previous = ""
    for number, line in enumerate(lines, start=1):
        previous_line, previous = previous, (line if line.strip() else previous)
        if number in table_lines:
            continue
        for match in BARE_SECTION_PATTERN.finditer(line):
            if not names_this_document(line[: match.start()], previous_line):
                continue
            section = match.group("number")
            if any(resolve_section(repo, member, section) is None for member in sorted(members)):
                continue
            results.append((number, f"{path} §{section}", "転送グループの見出しに解決しない裸の § 参照"))
    return results


def doc_findings(repo: Repository, path: str) -> list[tuple[int, str, str]]:
    lines = strip_code_fences(repo.read(path).splitlines())
    base = posixpath.dirname(path)
    results: list[tuple[int, str, str]] = []
    for number, line in enumerate(lines, start=1):
        seen: set[str] = set()
        for match in INLINE_CODE_PATTERN.finditer(line):
            token = match.group(2).strip()
            if token in seen:
                continue
            seen.add(token)
            problem = check_code_span(repo, token, base)
            if problem:
                results.append((number, problem[0], problem[1]))
    results.extend(section_findings(repo, "\n".join(lines), path, is_doc=True))
    return results


def allow_lines(text: str) -> dict[int, int]:
    """Return {exempted line: marker line} for doc-refs:allow markers."""
    exempted: dict[int, int] = {}
    for number, line in enumerate(text.splitlines(), start=1):
        if ALLOW_MARKER_PATTERN.search(line):
            exempted[number] = number
            if not ALLOW_MARKER_PATTERN.sub("", line).strip():
                exempted[number + 1] = number
    return exempted


def load_allowlist(path: Path) -> tuple[dict[str, AllowEntry], list[str]]:
    if not path.exists():
        return {}, []
    try:
        data = tomllib.loads(path.read_text(encoding="utf-8"))
    except tomllib.TOMLDecodeError as error:
        return {}, [f"{ALLOWLIST_RELATIVE_PATH}: TOML として読めない: {error}"]
    entries: dict[str, AllowEntry] = {}
    problems: list[str] = []
    for index, raw in enumerate(data.get("allow", []), start=1):
        where = f"{ALLOWLIST_RELATIVE_PATH}: allow[{index}]"
        ref = str(raw.get("ref", "")).strip()
        kind = str(raw.get("kind", "")).strip()
        reason = str(raw.get("reason", "")).strip()
        if not ref:
            problems.append(f"{where}: `ref` が無い")
            continue
        if kind not in ALLOWED_KINDS:
            problems.append(f"{where} `{ref}`: `kind` は {sorted(ALLOWED_KINDS)} のいずれか")
        if not reason:
            problems.append(f"{where} `{ref}`: `reason` が無い")
        for required in ALLOWED_KINDS.get(kind, ()):
            if not str(raw.get(required, "")).strip():
                problems.append(f"{where} `{ref}`: kind = \"{kind}\" には `{required}` が要る")
        issue = str(raw.get("issue", "")).strip()
        if issue and not ISSUE_PATTERN.match(issue):
            problems.append(f"{where} `{ref}`: `issue` は Linear の課題 ID（DEV-番号）")
        files = raw.get("files", [])
        if not isinstance(files, list) or not all(isinstance(item, str) for item in files):
            problems.append(f"{where} `{ref}`: `files` は文書パスの配列")
            files = []
        if ref in entries:
            problems.append(f"{where}: `{ref}` が重複している")
            continue
        entries[ref] = AllowEntry(
            ref=ref,
            kind=kind,
            reason=reason,
            milestone=str(raw.get("milestone", "")),
            spec=str(raw.get("spec", "")),
            files=tuple(files),
        )
    return entries, problems


def reference_resolves(repo: Repository, ref: str) -> bool:
    """True when an allowlisted reference no longer needs the allowlist."""
    section = re.fullmatch(r"(.+?) §(" + SECTION_NUMBER + r")", ref)
    if section:
        return resolve_section(repo, section.group(1), section.group(2)) is None
    if ITEM_ID_PATTERN.fullmatch(ref):
        return resolve_item(repo, ref) is None
    return check_code_span(repo, ref) is None and (
        "::" in ref or repo.exists(ref) or glob_matches(repo, ref)
    )


def ignored_paths(root: Path, references: set[str]) -> set[str]:
    """Return the references `.gitignore` covers (build output, runtime files)."""
    paths = {reference.partition("::")[0] for reference in references}
    if not paths:
        return set()
    # path が directory のとき中身を無視する規則（`build/` など）を見るため、path 自体に加えて
    # `path/<probe>` を問い合わせる。末尾 "/" の問い合わせは使わない。CRLF の .gitignore では
    # 空行が空パターンになり、basename が空の問い合わせへ誤って一致する（何でも無視と判定される）。
    queries = sorted(paths | {f"{path}/{IGNORE_PROBE_NAME}" for path in paths})
    # -z で入出力を NUL 区切りにすると、改行コードにもパスの引用符化にも左右されない。
    matched = git_lines(
        root, "check-ignore", "-z", "--no-index", "--stdin", stdin="\0".join(queries)
    )
    return {path.removesuffix(f"/{IGNORE_PROBE_NAME}") for path in matched}


def check(repo_root: Path) -> list[str]:
    repo = load_repository(repo_root)
    allowlist, problems = load_allowlist(repo_root / ALLOWLIST_RELATIVE_PATH)

    for entry in allowlist.values():
        canonical = re.fullmatch(r"(\S+\.md) §(" + SECTION_NUMBER + r")", entry.spec)
        if canonical and resolve_section(repo, canonical.group(1), canonical.group(2)):
            problems.append(
                f"{ALLOWLIST_RELATIVE_PATH}: `{entry.ref}` の spec `{entry.spec}` が見出しに解決しない"
            )
        elif entry.spec and not canonical:
            document = entry.spec.split()[0]
            if document.endswith(".md") and document not in repo.files:
                problems.append(
                    f"{ALLOWLIST_RELATIVE_PATH}: `{entry.ref}` の spec に存在しない文書 `{document}`"
                )
        for path in entry.files:
            if path not in repo.files:
                problems.append(
                    f"{ALLOWLIST_RELATIVE_PATH}: `{entry.ref}` の files に存在しない文書 `{path}`"
                )

    problems.extend(forwarding_table_problems(repo))

    raw: list[Finding] = []
    for path in target_documents(repo):
        for line, reference, message in doc_findings(repo, path):
            raw.append(Finding(path, line, reference, message))
        for line, reference, message in group_findings(repo, path):
            raw.append(Finding(path, line, reference, message))
    for path in code_files(repo):
        for line, reference, message in section_findings(repo, repo.read(path), path, is_doc=False):
            raw.append(Finding(path, line, reference, message))
    raw.extend(item_findings(repo))

    ignored = ignored_paths(
        repo_root,
        {
            finding.reference
            for finding in raw
            if "§" not in finding.reference and not ITEM_ID_PATTERN.fullmatch(finding.reference)
        },
    )

    markers: dict[str, dict[int, int]] = {}
    used_markers: set[tuple[str, int]] = set()
    for finding in raw:
        if finding.reference.partition("::")[0] in ignored:
            continue
        entry = allowlist.get(finding.reference)
        if entry is not None and entry.covers(finding):
            entry.used = True
            continue
        if finding.location not in markers:
            markers[finding.location] = (
                allow_lines(repo.read(finding.location))
                if finding.location.endswith(".md")
                else {}
            )
        marker = markers[finding.location].get(finding.line)
        if marker is not None:
            used_markers.add((finding.location, marker))
            continue
        problems.append(finding.render())

    for path in target_documents(repo):
        lines = markers.get(path)
        if lines is None:
            lines = allow_lines(repo.read(path))
        for marker in sorted(set(lines.values())):
            if (path, marker) not in used_markers:
                problems.append(f"{path}:{marker}: 何も免除していない doc-refs:allow（削除する）")

    for entry in allowlist.values():
        if entry.used:
            continue
        if reference_resolves(repo, entry.ref):
            problems.append(
                f"{ALLOWLIST_RELATIVE_PATH}: `{entry.ref}` は実在する。allowlist から削除する"
            )
        else:
            problems.append(
                f"{ALLOWLIST_RELATIVE_PATH}: `{entry.ref}` を参照する文書が無い。allowlist から削除する"
            )
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=REPO_ROOT,
        help="検査対象のリポジトリルート（既定: このスクリプトの親ディレクトリ）",
    )
    arguments = parser.parse_args(argv)
    problems = check(arguments.repo_root.resolve())
    if problems:
        print("文書の参照が実体に解決しません:", file=sys.stderr)
        for problem in problems:
            print(f"  - {problem}", file=sys.stderr)
        print(
            "\n文書の参照を実体へ合わせるか、正しい予定参照なら "
            f"{ALLOWLIST_RELATIVE_PATH} に kind と reason を書いて登録してください。",
            file=sys.stderr,
        )
        return 1
    print("文書の参照はすべて実体に解決します。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
