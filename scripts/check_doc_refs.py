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
   rows).
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

ALLOWED_KINDS = {
    "planned": ("milestone", "spec"),
    "origin": (),
    "external": (),
    "historical": (),
    "runtime": (),
    "unowned": (),
    "pending-review": (),
}

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
    result = subprocess.run(
        ["git", "-C", str(root), *arguments],
        input=stdin,
        capture_output=True,
        text=True,
        encoding="utf-8",
        check=False,
    )
    if result.returncode not in (0, 1):
        raise RuntimeError(f"git {' '.join(arguments)} failed: {result.stderr.strip()}")
    return [line for line in result.stdout.split("\0" if "-z" in arguments else "\n") if line]


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
    return repo


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
            number = HEADING_NUMBER_PATTERN.match(heading.group(2).strip())
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
            + [(match.start(), match) for match in SAME_DOCUMENT_PATTERN.finditer(line)],
            key=lambda event: event[0],
        )
        for _, match in events:
            if match.re is SAME_DOCUMENT_PATTERN:
                document = last_document
                numbers = [match.group("number")]
            else:
                if is_doc and "section" in match.group(0):
                    continue
                document = resolve_document(repo, match.group("doc"), source)
                last_document = document
                numbers = [match.group("number")]
                rest = line[match.end():]
                while chained := CHAINED_SECTION_PATTERN.match(rest):
                    numbers.append(chained.group("number"))
                    rest = rest[chained.end():]
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


def resolve_section(repo: Repository, document: str, section: str) -> str | None:
    numbers = repo.section_numbers(document)
    if numbers is None:
        if under_submodule(repo, document):
            return None
        return "§ 参照先の文書が存在しない"
    if section in numbers:
        return None
    return "見出しに解決しない § 参照"


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
    return check_code_span(repo, ref) is None and (
        "::" in ref or repo.exists(ref) or glob_matches(repo, ref)
    )


def ignored_paths(root: Path, references: set[str]) -> set[str]:
    """Return the references `.gitignore` covers (build output, runtime files)."""
    paths = {reference.partition("::")[0] for reference in references}
    if not paths:
        return set()
    queries = sorted(paths | {path + "/" for path in paths})
    matched = git_lines(root, "check-ignore", "--no-index", "--stdin", stdin="\n".join(queries))
    return {path.rstrip("/") for path in matched}


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

    raw: list[Finding] = []
    for path in target_documents(repo):
        for line, reference, message in doc_findings(repo, path):
            raw.append(Finding(path, line, reference, message))
    for path in code_files(repo):
        for line, reference, message in section_findings(repo, repo.read(path), path, is_doc=False):
            raw.append(Finding(path, line, reference, message))

    ignored = ignored_paths(
        repo_root, {finding.reference for finding in raw if "§" not in finding.reference}
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
