#!/usr/bin/env python3
"""Static checks for repository documentation drift (read-only).

This script is the canonical implementation of the checks that the
`doc-governance` shared skill teaches, and the single source of truth for the
status vocabulary — `--print-words` emits it, so no document has to restate it
(a hand-copied word list is exactly the kind of un-invalidated cache these
checks exist to catch).

Each product repo vendors this file into `scripts/docs-lint.py` and keeps only
its repo-specific settings in `.docs-lint.toml`. It runs with no config at all.
Edit it in agent-ops and re-vendor; a repo-side edit is a silent fork, which
`vendor-docs-governance.sh --check` reports.

Findings come in two tiers, and the split is the point:

  DECISIVE    A hit is a real finding: something references a file, section,
              document or skill that does not exist, or a heading asserts a
              state the repo does not track in docs. Safe to gate CI on.
  HEURISTIC   Text-shaped guesses about prose. Over-detection is expected by
              design; a human decides. Never gate on these, and never print
              them line-by-line unless asked — a check that cries wolf stops
              being read, and that is how the useful half dies too.

Checks:

  link           broken relative link / image target            DECISIVE
  section        `<file>.md §N.M` pointing at no such heading   DECISIVE
  index          governed doc missing from the docs index       DECISIVE
  mirror         .claude/skills vs .agents/skills divergence    DECISIVE
  heading-state  a heading asserting progress or a snapshot     DECISIVE
  line-ref       line-numbered code refs, measured test counts  DECISIVE
  config         .docs-lint.toml naming paths that do not exist DECISIVE
  dead-allow     a lint:allow suppressing nothing               DECISIVE
  status         status vocabulary in prose                     HEURISTIC

Why headings get a stricter rule than the paragraphs under them: prose can
legitimately record state — a dated decision, a rationale that quotes how
things stood — so a line carrying a date reads as a past record and is left
alone. A heading cannot do that. It is the document's skeleton, it is what a
reader skims, and it has no room for the date stamp that would make the claim
honest. So the dated-line exemption deliberately does not apply to headings:
"## 進捗（2026-06-09 時点）" is wrong the day after it is written, and no
reader can tell.

Usage:
  scripts/docs-lint.py [--root DIR] [--category C ...] [--verbose]
  scripts/docs-lint.py --strict [decisive|all]      # exit 1 on that tier
  scripts/docs-lint.py --baseline .docs-lint-baseline.json   # exit 1 on growth
  scripts/docs-lint.py --write-baseline .docs-lint-baseline.json
  scripts/docs-lint.py --print-words                # the status vocabulary
  scripts/docs-lint.py --print-version              # content hash, for drift checks

Escape hatches — every exemption shrinks what a check means, so prefer fixing:
  <!-- lint:allow <category>[,...] -->       exempts that line (or the next one)
  <!-- lint:allow-file <category>[,...] -->  exempts the file (put it near the
                                             top; on a dated snapshot document
                                             this doubles as the honest "this
                                             is a snapshot" marker)
An allow that stops suppressing anything is reported as `dead-allow`, so the
escape hatches clean themselves up instead of accumulating.
"""
from __future__ import annotations

import argparse
import fnmatch
import hashlib
import json
import os
import re
import subprocess
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from datetime import date

VENDORED_FROM = "dolquis/agent-ops:scripts/docs-lint.py"

CONFIG_NAMES = (".docs-lint.toml", ".docs-lint.json")

DECISIVE = ("link", "section", "index", "mirror", "heading-state", "line-ref",
            "config", "dead-allow")
HEURISTIC = ("status",)
ALL_CATEGORIES = DECISIVE + HEURISTIC

# Fallback only: the file list normally comes from `git ls-files`, so .gitignore
# already excludes dependency and build trees and no repo has to enumerate them.
FALLBACK_EXCLUDE = [
    ".git", "node_modules", ".venv", "venv", "__pycache__", "target", "dist",
    "build", ".build", ".next", "vendor", "third_party", "coverage", "Pods",
]

# Files and directories governed by the prose checks (status / heading-state /
# line-ref). Link, section and mirror run over the whole repo instead: a
# reference to something that does not exist is worth catching wherever it is.
DEFAULT_PROSE_ROOTS = [
    "README.md", "README.ja.md", "AGENTS.md", "CLAUDE.md", "ROADMAP.md",
    "docs", "plans",
]

# Genres that are past records by construction. Exempting them by genre beats
# making every repo enumerate its own files, and beats sprinkling `lint:allow`
# through documents nobody should be editing any more.
DEFAULT_PROSE_EXEMPT = [
    "**/archive/**", "**/decisions/**", "**/decisions.md", "**/DECISION_LOG.md",
    "**/decision-log.md", "**/linear-conventions.md", "**/CHANGELOG.md",
]

DEFAULT_LINE_REF_EXTENSIONS = [
    "c", "cc", "cpp", "cs", "go", "h", "hpp", "js", "jsx", "kt", "m", "mm",
    "php", "ps1", "py", "rb", "rs", "sh", "swift", "ts", "tsx",
]

# --- status vocabulary (this script is the only place it is defined) ---------
# Strong words assert a current state on their own; a reader has no way to tell
# when they stopped being true.
STATUS_WORDS_STRONG = [
    "現状", "現時点", "実装済み", "未実装", "対応済み", "着手済み", "着手前",
    "完了済み", "完了している", "完了しました", "残作業", "残課題", "未検証",
    "暫定", "TODO", "FIXME",
]
# Weak words have common legitimate uses ("現行の開発対象は Windows 版"), so on
# their own they are noise. They become a finding next to an Issue or PR
# reference, because that combination is the signature of a document mirroring
# the tracker — which is the duplication the conventions forbid.
STATUS_WORDS_WEAK = [
    "現行", "当面", "未定", "未確定", "完了後", "既知の未確定",
    "待ち）", "待ちである",
]

# --- heading-state -----------------------------------------------------------
# A heading may not assert progress. Bare 完了 is excluded when it introduces a
# *definition* (完了条件 / 完了基準 / DoD), which is what a roadmap heading
# should contain. `DONE` is matched case-sensitively so "Definition of Done"
# and "Done criteria" — both definitions — stay clear.
HEADING_PROGRESS_RE = re.compile(
    r"[✅✔☑🟢🟩]"
    r"|実装済み|対応済み|完了済み|着手済み|導入済み|移行済み|廃止済み"
    r"|未実装|未着手|未対応"
    r"|着手中|実装中|作業中|調査中|検討中|一部着手|保留中"
    r"|完了(?!条件|基準|要件|定義|判定|後|時|の)"
    r"|\bDONE\b|\bWIP\b|\bTBD\b"
)
HEADING_SNAPSHOT_RE = re.compile(
    r"\d{4}-\d{2}-\d{2}\s*時点|as of\s+\d{4}-\d{2}-\d{2}", re.I)
# 「進捗管理」「進捗反映」 name a practice, which a heading may introduce;
# a bare 「進捗」 names the present.
HEADING_CURRENT_RE = re.compile(r"現状|現況|概況|進捗(?!管理|反映|報告|の管理)")
DATE_RE = re.compile(r"(\d{4})-(\d{2})-(\d{2})")

# A date makes a prose line a record of a past fact, which §7.2 permits.
# Deliberately dates only: `#\d+` appears too freely in repo docs to carry that
# meaning, and an issue reference means the opposite (see ISSUE_REF_RE).
DATED_RECORD_RE = re.compile(r"\d{4}-\d{2}-\d{2}")
ISSUE_REF_RE = re.compile(r"\b[A-Z]{2,5}-\d{1,6}\b|#\d{1,6}\b|/(?:issues|pull)/\d+")

COUNT_RE = re.compile(r"\d+\s*件中\s*\d+|全\s*\d+\s*(?:種|個|件)")
FENCE_RE = re.compile(r"^ {0,3}(`{3,}|~{3,})")
ALLOW_RE = re.compile(r"<!--\s*lint:allow\s+([\w,\s-]+?)\s*-->")
ALLOW_FILE_RE = re.compile(r"<!--\s*lint:allow-file\s+([\w,\s-]+?)\s*-->")
MD_LINK_RE = re.compile(r"!?\[[^\]]*\]\(([^)\s]+)\)")
SECTION_REF_RE = re.compile(
    r"([A-Za-z0-9_./-]+\.md)[`\)\]]*\s*(?:の\s*)?§\s*"
    r"([0-9]+(?:[.-][0-9]+)*|[A-Z]+-?[0-9]+(?:-[0-9]+)*)")
HEADING_NUM_RE = re.compile(r"^#+\s*(?:§\s*)?([0-9]+(?:[.-][0-9]+)*)[.．\s:：]")
HEADING_TAG_RE = re.compile(r"^#+\s*(?:§\s*)?([A-Z]+-?[0-9]+(?:-[A-Z0-9]+)*)\b")


def posix(path: str) -> str:
    """Repo-relative paths stay `/`-separated (os.sep is `\\` on Windows)."""
    return path.replace(os.sep, "/")


@dataclass
class Finding:
    category: str
    file: str
    line: int
    message: str

    @property
    def tier(self) -> str:
        return "decisive" if self.category in DECISIVE else "heuristic"


@dataclass
class Config:
    root: str
    path: str = ""
    configured: set = field(default_factory=set)
    prose_roots: list = field(default_factory=lambda: list(DEFAULT_PROSE_ROOTS))
    prose_exempt: list = field(default_factory=lambda: list(DEFAULT_PROSE_EXEMPT))
    remove_words: list = field(default_factory=list)
    line_ref_extensions: list = field(default_factory=lambda: list(DEFAULT_LINE_REF_EXTENSIONS))
    index_file: str = ""
    index_covers: list = field(default_factory=list)
    mirror_claude_only: list = field(default_factory=list)
    mirror_allow_single_tree: bool = False
    heading_state_grace_days: int = 0
    extra_text_files: list = field(default_factory=list)
    fallback_exclude: list = field(default_factory=lambda: list(FALLBACK_EXCLUDE))


def load_config(root: str) -> Config:
    """Read `.docs-lint.toml` (or `.docs-lint.json`) if present.

    Every field has a working default, so a repo adopts the checks first and
    tunes them once it has seen its own baseline. Note what is *not*
    configurable: the status vocabulary can be narrowed (`remove_words`) but not
    extended. Per-repo word lists would drift into nine private dialects of the
    same rule; new vocabulary belongs here, once, for everyone.
    """
    cfg = Config(root=root)
    data: dict = {}
    for name in CONFIG_NAMES:
        path = os.path.join(root, name)
        if not os.path.exists(path):
            continue
        cfg.path = name
        if name.endswith(".toml"):
            try:
                import tomllib
            except ModuleNotFoundError:
                sys.stderr.write(
                    "warning: %s found but tomllib needs Python 3.11+; using "
                    "defaults. Rename it to .docs-lint.json to configure on an "
                    "older interpreter.\n" % name)
                cfg.path = ""
                break
            with open(path, "rb") as handle:
                data = tomllib.load(handle)
        else:
            with open(path, encoding="utf-8") as handle:
                data = json.load(handle)
        break

    scan = data.get("scan", {})
    if "prose_roots" in scan:
        cfg.configured.add("scan.prose_roots")
    cfg.prose_roots = scan.get("prose_roots", cfg.prose_roots)
    cfg.extra_text_files = scan.get("extra_text_files", [])
    cfg.fallback_exclude = sorted(set(cfg.fallback_exclude) | set(scan.get("exclude", [])))

    status = data.get("status", {})
    cfg.prose_exempt = sorted(set(cfg.prose_exempt) | set(status.get("exempt", [])))
    cfg.remove_words = status.get("remove_words", [])

    cfg.heading_state_grace_days = data.get("heading_state", {}).get("grace_days", 0)
    cfg.line_ref_extensions = data.get("line_ref", {}).get(
        "extensions", cfg.line_ref_extensions)

    index = data.get("index", {})
    cfg.index_file = index.get("file", "")
    cfg.index_covers = index.get("covers", [])
    mirror = data.get("mirror", {})
    cfg.mirror_claude_only = mirror.get("claude_only", [])
    cfg.mirror_allow_single_tree = mirror.get("allow_single_tree", False)
    return cfg


def find_root(start: str) -> str:
    """Walk up for a config file, else the git root, else the starting dir."""
    current = os.path.abspath(start)
    while True:
        for name in CONFIG_NAMES + (".git",):
            if os.path.exists(os.path.join(current, name)):
                return current
        parent = os.path.dirname(current)
        if parent == current:
            return os.path.abspath(start)
        current = parent


def git_markdown_files(root: str) -> list:
    """Tracked and untracked-but-not-ignored Markdown, straight from git.

    Using git means .gitignore already answers "is this our file", so no repo
    has to hand-maintain a list of dependency directories to skip — a list that
    would itself go stale the first time a toolchain changed.
    """
    try:
        out = subprocess.run(
            ["git", "-C", root, "ls-files", "--cached", "--others",
             "--exclude-standard", "-z", "*.md"],
            capture_output=True, check=True, timeout=30)
    except (OSError, subprocess.SubprocessError):
        return []
    return sorted(p for p in out.stdout.decode("utf-8", "replace").split("\0") if p)


def walk_markdown_files(root: str, exclude: list) -> list:
    excluded = set(exclude)
    out = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in excluded]
        rel_dir = os.path.relpath(dirpath, root)
        rel_dir = "" if rel_dir == "." else rel_dir + "/"
        for name in filenames:
            if name.endswith(".md"):
                out.append(posix(os.path.normpath(rel_dir + name)))
    return sorted(out)


class Linter:
    def __init__(self, cfg: Config):
        self.cfg = cfg
        self.findings: list = []
        self._lines: dict = {}
        self._fenced: dict = {}
        # (file, line0, category) actually suppressed by an allow comment.
        self._allow_used: set = set()
        self._allow_seen: set = set()
        # Two things `git ls-files` does that would otherwise corrupt the run:
        #
        # `--cached` still lists a tracked file deleted from the worktree whose
        # deletion is not staged yet — a normal state midway through removing a
        # document. Reading it would abort the whole run.
        #
        # During an unresolved merge it lists a conflicted file once per stage,
        # so every finding in that file would be counted two or three times and
        # `--baseline` would report growth that does not exist.
        #
        # Both are handled once, here, rather than guarded at each of the
        # half-dozen places that open a file. `dict.fromkeys` keeps the order.
        self.md_files = list(dict.fromkeys(
            f for f in (git_markdown_files(cfg.root)
                        or walk_markdown_files(cfg.root, cfg.fallback_exclude))
            if os.path.exists(os.path.join(cfg.root, f))))
        self.prose_files = [f for f in self.md_files if self.is_prose(f)]
        self.prose_files += [f for f in dict.fromkeys(cfg.extra_text_files)
                             if os.path.exists(os.path.join(cfg.root, f))
                             and f not in self.md_files]

    # -- infrastructure -------------------------------------------------------

    def add(self, category: str, file: str, line: int, message: str) -> None:
        self.findings.append(Finding(category, file, line, message))

    def fenced(self, relpath: str) -> set:
        """Line indices inside ``` fences.

        Documentation *about* these checks quotes the allow comments as
        examples. Without this the tool reads its own manual as a pile of
        suppressions — and then reports them as dead. Prose in backticks is
        skipped for the same reason.
        """
        if relpath not in self._fenced:
            out, opener = set(), None
            for i, line in enumerate(self.lines(relpath)):
                match = FENCE_RE.match(line)
                if match:
                    delim = match.group(1)
                    if opener is None:
                        opener = delim
                        out.add(i)
                        continue
                    if delim[0] == opener[0] and len(delim) >= len(opener):
                        opener = None
                        out.add(i)
                        continue
                if opener is not None:
                    out.add(i)
            self._fenced[relpath] = out
        return self._fenced[relpath]

    @staticmethod
    def _in_code_span(line: str, pos: int) -> bool:
        return line.count("`", 0, pos) % 2 == 1

    def _real_allow(self, relpath: str, idx0: int):
        """The allow comment on this line, if it is a real one."""
        if idx0 in self.fenced(relpath):
            return None
        line = self.lines(relpath)[idx0]
        match = ALLOW_RE.search(line)
        if not match or self._in_code_span(line, match.start()):
            return None
        return match

    def lines(self, relpath: str) -> list:
        if relpath not in self._lines:
            with open(os.path.join(self.cfg.root, relpath),
                      encoding="utf-8", errors="replace") as handle:
                self._lines[relpath] = handle.readlines()
        return self._lines[relpath]

    def is_prose(self, relpath: str) -> bool:
        return any(relpath == root or relpath.startswith(root.rstrip("/") + "/")
                   for root in self.cfg.prose_roots)

    def is_prose_exempt(self, relpath: str) -> bool:
        return any(fnmatch.fnmatch(relpath, pat) for pat in self.cfg.prose_exempt)

    def file_allows(self, relpath: str) -> set:
        cats: set = set()
        fenced = self.fenced(relpath)
        for i, line in enumerate(self.lines(relpath)[:20]):
            match = ALLOW_FILE_RE.search(line)
            if match and i not in fenced and not self._in_code_span(line, match.start()):
                cats.update(c.strip() for c in match.group(1).split(","))
        return cats

    def exempt(self, relpath: str, idx0: int, category: str, file_allows: set) -> bool:
        """True when an allow comment covers this finding, recording the use."""
        if category in file_allows:
            return True
        for i in (idx0, idx0 - 1):
            if not 0 <= i < len(self.lines(relpath)):
                continue
            match = self._real_allow(relpath, i)
            if match and category in {c.strip() for c in match.group(1).split(",")}:
                self._allow_used.add((relpath, i, category))
                return True
        return False

    # -- checks ---------------------------------------------------------------

    def check_config(self) -> None:
        """A setting naming a path that no longer exists silently stops working."""
        if not self.cfg.path:
            return
        # Only settings the config file actually names are checked. The
        # defaults deliberately list paths many repos do not have (no AGENTS.md,
        # no plans/); reporting those would make the check noise on day one.
        checks = [("index.file", [self.cfg.index_file] if self.cfg.index_file else []),
                  ("index.covers", self.cfg.index_covers),
                  ("scan.prose_roots", self.cfg.prose_roots
                   if "scan.prose_roots" in self.cfg.configured else []),
                  ("scan.extra_text_files", self.cfg.extra_text_files)]
        for key, values in checks:
            for value in values:
                if value and not os.path.exists(os.path.join(self.cfg.root, value)):
                    self.add("config", self.cfg.path, 1,
                             "設定 %s が存在しないパスを指している: %s" % (key, value))
        skills_dir = os.path.join(self.cfg.root, ".claude/skills")
        for name in self.cfg.mirror_claude_only:
            if not os.path.isdir(os.path.join(skills_dir, name)):
                self.add("config", self.cfg.path, 1,
                         "設定 mirror.claude_only が存在しないスキルを指している: %s" % name)

    def check_links(self) -> None:
        for relpath in self.md_files + self.cfg.extra_text_files:
            if not os.path.exists(os.path.join(self.cfg.root, relpath)):
                continue
            allows = self.file_allows(relpath)
            fenced = self.fenced(relpath)
            for i, line in enumerate(self.lines(relpath)):
                # Documentation that *shows* Markdown — a fenced sample, an
                # inline `[label](path.md)` — is not linking anywhere, and
                # Markdown does not render it as a link either. Treating the
                # example as live would put a DECISIVE finding on a document
                # that is correct, which is the fastest way to lose a gate.
                if i in fenced:
                    continue
                for match in MD_LINK_RE.finditer(line):
                    if self._in_code_span(line, match.start()):
                        continue
                    target = match.group(1)
                    if target.startswith(("http://", "https://", "mailto:", "tel:", "#", "<")):
                        continue
                    path = target.partition("#")[0]
                    if not path:
                        continue
                    resolved = os.path.normpath(
                        os.path.join(os.path.dirname(relpath), path))
                    if os.path.exists(os.path.join(self.cfg.root, resolved)):
                        continue
                    if self.exempt(relpath, i, "link", allows):
                        continue
                    self.add("link", relpath, i + 1, "リンク切れ: %s" % target)

    def _headings(self) -> dict:
        heads: dict = {}
        for relpath in self.md_files:
            nums: set = set()
            for line in self.lines(relpath):
                match = HEADING_NUM_RE.match(line)
                if match:
                    nums.add(match.group(1).replace("-", "."))
                match = HEADING_TAG_RE.match(line)
                if match:
                    nums.add(match.group(1))
            heads[relpath] = nums
        return heads

    @staticmethod
    def _section_exists(normalized: str, headings: set) -> bool:
        if normalized in headings:
            return True
        # A tag reference may name a *group*: "§P0" is satisfied by P0-1 … P0-5,
        # and "§P0-x" with a placeholder suffix reduces to the same reference.
        return any(h.startswith(normalized + "-") for h in headings)

    def check_sections(self) -> None:
        heads = self._headings()
        dir_roots = [r for r in self.cfg.prose_roots if not r.endswith(".md")]
        for relpath in self.md_files:
            allows = self.file_allows(relpath)
            fenced = self.fenced(relpath)
            for i, line in enumerate(self.lines(relpath)):
                if i in fenced:
                    continue
                for match in SECTION_REF_RE.finditer(line):
                    target, section = match.group(1), match.group(2)
                    normalized = (section if re.match(r"^[A-Z]", section)
                                  else section.replace("-", "."))
                    candidates = [
                        target,
                        posix(os.path.normpath(
                            os.path.join(os.path.dirname(relpath), target))),
                    ] + [posix(os.path.normpath(
                        os.path.join(root, os.path.basename(target))))
                        for root in dir_roots]
                    if self.exempt(relpath, i, "section", allows):
                        continue
                    matched = next((c for c in candidates if c in heads), None)
                    if matched is None:
                        # Really a link check: a document nobody can open is
                        # worth reporting even from a frozen record.
                        if not any(os.path.exists(os.path.join(self.cfg.root, c))
                                   for c in candidates):
                            self.add("section", relpath, i + 1,
                                     "§ 参照先ファイルが存在しない: %s" % target)
                        continue
                    if self.is_prose_exempt(relpath):
                        # Frozen records cite the living docs as they stood.
                        # Their section numbers are expected to drift, and
                        # editing them would rewrite history.
                        continue
                    if not self._section_exists(normalized, heads[matched]):
                        self.add("section", relpath, i + 1,
                                 "未解決の § 参照: %s §%s" % (target, section))

    def check_index(self) -> None:
        """Every governed document must appear in the repo's docs index.

        An index that quietly falls behind is worse than none, because readers
        trust it to be the map. Repos with no index file skip this check.
        """
        index_file = self.cfg.index_file
        if not index_file:
            default = "docs/README.md"
            if os.path.exists(os.path.join(self.cfg.root, default)):
                index_file = default
        if not index_file or not os.path.exists(os.path.join(self.cfg.root, index_file)):
            return
        covers = self.cfg.index_covers or [
            r for r in self.cfg.prose_roots if not r.endswith(".md")]
        index_text = "".join(self.lines(index_file))
        for cover in covers:
            directory = os.path.join(self.cfg.root, cover)
            if not os.path.isdir(directory):
                continue
            for name in sorted(os.listdir(directory)):
                if not name.endswith(".md"):
                    continue
                rel = posix(os.path.normpath(os.path.join(cover, name)))
                if rel == index_file or self.is_prose_exempt(rel):
                    continue
                if name not in index_text:
                    self.add("index", index_file, 1, "未索引: %s" % rel)

    def check_mirror(self) -> None:
        """`.claude/skills/` and `.agents/skills/` must carry the same guidance.

        Frontmatter may differ per harness; the body may not. A skill that tells
        Claude one thing and Codex another is a fork nobody declared.
        """
        claude_dir = os.path.join(self.cfg.root, ".claude/skills")
        agents_dir = os.path.join(self.cfg.root, ".agents/skills")
        has_claude, has_agents = os.path.isdir(claude_dir), os.path.isdir(agents_dir)
        if not (has_claude or has_agents):
            return
        if not (has_claude and has_agents):
            # Exactly one tree exists. Staying silent here would hide the very
            # state this check is for — a repo that adopted the skills for one
            # harness only. One finding for the missing root, not one per
            # skill: the fix is a single vendoring run, and N copies of the
            # same instruction would drown the rest of the report.
            if self.cfg.mirror_allow_single_tree:
                return
            missing = ".agents/skills" if has_claude else ".claude/skills"
            present = ".claude/skills" if has_claude else ".agents/skills"
            self.add("mirror", missing, 1,
                     "%s があるのに %s が無い（vendor-shared-skills.sh で配る。"
                     "片側だけで運用するなら mirror.allow_single_tree = true）"
                     % (present, missing))
            return
        claude_names = {n for n in os.listdir(claude_dir)
                        if os.path.isdir(os.path.join(claude_dir, n))}
        agents_names = {n for n in os.listdir(agents_dir)
                        if os.path.isdir(os.path.join(agents_dir, n))}
        claude_only = set(self.cfg.mirror_claude_only)
        for name in sorted(claude_names ^ agents_names):
            if name in claude_only:
                continue
            if name in claude_names:
                self.add("mirror", ".claude/skills/%s" % name, 1,
                         ".agents 側にスキルディレクトリが無い"
                         "（Claude 専用なら mirror.claude_only に登録する）")
            else:
                self.add("mirror", ".agents/skills/%s" % name, 1,
                         ".claude 側にスキルディレクトリが無い")
        for name in sorted(claude_names & agents_names):
            self._compare_skill_trees(name, os.path.join(claude_dir, name),
                                      os.path.join(agents_dir, name))

    def _compare_skill_trees(self, name: str, cdir: str, adir: str) -> None:
        cskill, askill = os.path.join(cdir, "SKILL.md"), os.path.join(adir, "SKILL.md")
        if os.path.exists(cskill) and os.path.exists(askill):
            if strip_frontmatter(read_text(cskill)) != strip_frontmatter(read_text(askill)):
                self.add("mirror", ".claude/skills/%s/SKILL.md" % name, 1,
                         "本文が .agents 側と不一致: %s" % name)
        for sub in ("references", "assets", "scripts"):
            csub, asub = os.path.join(cdir, sub), os.path.join(adir, sub)
            if not (os.path.isdir(csub) or os.path.isdir(asub)):
                continue
            cfiles = set(os.listdir(csub)) if os.path.isdir(csub) else set()
            afiles = set(os.listdir(asub)) if os.path.isdir(asub) else set()
            for fname in sorted(cfiles | afiles):
                cpath, apath = os.path.join(csub, fname), os.path.join(asub, fname)
                rel = ".claude/skills/%s/%s/%s" % (name, sub, fname)
                if os.path.isfile(cpath) != os.path.isfile(apath):
                    self.add("mirror", rel, 1, "片方のツリーにのみ存在")
                elif os.path.isfile(cpath) and read_text(cpath) != read_text(apath):
                    self.add("mirror", rel, 1, "本文が .agents 側と不一致")

    def check_heading_state(self) -> None:
        today = date.today()
        for relpath in self.prose_files:
            if self.is_prose_exempt(relpath):
                continue
            allows = self.file_allows(relpath)
            in_fence = False
            for i, line in enumerate(self.lines(relpath)):
                stripped = line.lstrip()
                if stripped.startswith("```"):
                    in_fence = not in_fence
                    continue
                if in_fence or not stripped.startswith("#"):
                    continue
                reason = self._heading_state_reason(stripped, today)
                if reason and not self.exempt(relpath, i, "heading-state", allows):
                    self.add("heading-state", relpath, i + 1, reason)

    def _heading_state_reason(self, heading: str, today: date) -> str:
        snapshot = HEADING_SNAPSHOT_RE.search(heading)
        if snapshot:
            age = heading_age_days(heading, today)
            if age is None or age >= self.cfg.heading_state_grace_days:
                suffix = "（%d 日経過）" % age if age is not None else ""
                return "見出しが日付つきの現状スナップショット%s: 「%s」" % (
                    suffix, snapshot.group(0))
        current = HEADING_CURRENT_RE.search(heading)
        if current:
            return "見出しが現在の状態を名乗っている: 「%s」" % current.group(0)
        progress = HEADING_PROGRESS_RE.search(heading)
        if progress:
            return "見出しが進捗・完了を主張している: 「%s」" % progress.group(0)
        return ""

    def check_status_words(self) -> None:
        strong, weak = status_vocabulary(self.cfg.remove_words)
        if not (strong or weak):
            return
        strong_re = re.compile("|".join(re.escape(w) for w in strong)) if strong else None
        weak_re = re.compile("|".join(re.escape(w) for w in weak)) if weak else None
        for relpath in self.prose_files:
            if self.is_prose_exempt(relpath):
                continue
            allows = self.file_allows(relpath)
            for i, line in enumerate(self.lines(relpath)):
                # A date makes the line a record of a past fact (§7.2).
                if DATED_RECORD_RE.search(line):
                    continue
                match = strong_re.search(line) if strong_re else None
                if not match and weak_re and ISSUE_REF_RE.search(line):
                    # A weak word beside an issue reference means this document
                    # is mirroring the tracker, which is the real problem.
                    match = weak_re.search(line)
                if not match or self.exempt(relpath, i, "status", allows):
                    continue
                self.add("status", relpath, i + 1,
                         "状態語の可能性: 「%s」" % match.group(0))

    def check_line_refs(self) -> None:
        if not self.cfg.line_ref_extensions:
            return
        ref_re = re.compile(r"`[\w./-]+\.(?:%s)(?::\d+(?:[-,]\d+)*)`"
                            % "|".join(re.escape(e) for e in self.cfg.line_ref_extensions))
        for relpath in self.prose_files:
            if self.is_prose_exempt(relpath):
                continue
            allows = self.file_allows(relpath)
            for i, line in enumerate(self.lines(relpath)):
                hits = [m.group(0) for m in ref_re.finditer(line)]
                hits += [m.group(0) for m in COUNT_RE.finditer(line)]
                if not hits or self.exempt(relpath, i, "line-ref", allows):
                    continue
                for hit in hits:
                    self.add("line-ref", relpath, i + 1,
                             "行番号・実測件数の記述: %s" % hit)

    def check_dead_allows(self, ran: set) -> None:
        """An allow comment that no longer suppresses anything is itself stale.

        Without this the escape hatches accumulate: a line gets rewritten, the
        finding goes away, and the suppression stays behind pretending a problem
        is still there. Only categories that actually ran can be judged.
        """
        for relpath in self.md_files:
            if not os.path.exists(os.path.join(self.cfg.root, relpath)):
                continue
            for i in range(len(self.lines(relpath))):
                match = self._real_allow(relpath, i)
                if not match:
                    continue
                for cat in (c.strip() for c in match.group(1).split(",")):
                    if cat not in ran or cat not in ALL_CATEGORIES:
                        continue
                    if ((relpath, i, cat) in self._allow_used
                            or (relpath, i - 1, cat) in self._allow_used):
                        continue
                    self.add("dead-allow", relpath, i + 1,
                             "何も抑止していない lint:allow: %s（削除してよい）" % cat)

    def run(self, categories: set) -> None:
        runners = [
            ("config", self.check_config),
            ("link", self.check_links),
            ("section", self.check_sections),
            ("index", self.check_index),
            ("mirror", self.check_mirror),
            ("heading-state", self.check_heading_state),
            ("line-ref", self.check_line_refs),
            ("status", self.check_status_words),
        ]
        for name, runner in runners:
            if name in categories:
                runner()
        if "dead-allow" in categories:
            self.check_dead_allows(categories)


def status_vocabulary(remove: list) -> tuple:
    removed = set(remove or ())
    return ([w for w in STATUS_WORDS_STRONG if w not in removed],
            [w for w in STATUS_WORDS_WEAK if w not in removed])


def read_text(path: str) -> str:
    with open(path, encoding="utf-8", errors="replace") as handle:
        return handle.read()


def strip_frontmatter(text: str) -> str:
    if text.startswith("---\n"):
        end = text.find("\n---\n", 4)
        if end != -1:
            return text[end + 5:]
    return text


def heading_age_days(heading: str, today: date):
    match = DATE_RE.search(heading)
    if not match:
        return None
    try:
        stamped = date(int(match.group(1)), int(match.group(2)), int(match.group(3)))
    except ValueError:
        return None
    return (today - stamped).days


def counts_by_file(findings: list) -> dict:
    out: dict = defaultdict(lambda: defaultdict(int))
    for finding in findings:
        out[finding.category][finding.file] += 1
    return {cat: dict(files) for cat, files in out.items()}


def compare_baseline(findings: list, baseline: dict) -> list:
    """Report only where a category grew for a file — the ratchet.

    Repos start with a real backlog; demanding zero on day one means the check
    is switched off instead. Freezing today's counts and failing on growth makes
    it useful immediately, and the baseline is machine-written so it cannot rot
    the way a hand-kept list would.
    """
    current = counts_by_file(findings)
    regressions = []
    for category, files in sorted(current.items()):
        known = baseline.get(category, {})
        for path, count in sorted(files.items()):
            allowed = known.get(path, 0)
            if count > allowed:
                regressions.append("%s: %s が %d → %d 件に増えた"
                                   % (category, path, allowed, count))
    return regressions


def report(findings: list, verbose: bool) -> None:
    by_tier: dict = defaultdict(lambda: defaultdict(list))
    for finding in findings:
        by_tier[finding.tier][finding.category].append(finding)

    decisive = by_tier.get("decisive", {})
    total = sum(len(v) for v in decisive.values())
    print("\n=== DECISIVE: %d 件（実在しない参照・状態を主張する見出し。処置対象）===" % total)
    for category in sorted(decisive):
        items = sorted(decisive[category], key=lambda f: (f.file, f.line))
        print("\n-- %s (%d) --" % (category, len(items)))
        for item in items:
            print("  %s:%d  %s" % (item.file, item.line, item.message))

    heuristic = by_tier.get("heuristic", {})
    htotal = sum(len(v) for v in heuristic.values())
    print("\n=== HEURISTIC: %d 件（過検出前提。採否は人が決める）===" % htotal)
    for category in sorted(heuristic):
        items = heuristic[category]
        print("\n-- %s (%d) --" % (category, len(items)))
        if verbose:
            for item in sorted(items, key=lambda f: (f.file, f.line)):
                print("  %s:%d  %s" % (item.file, item.line, item.message))
        else:
            # Per-file counts by default. Line-by-line output here is what
            # drowns the decisive findings above.
            per_file: dict = defaultdict(int)
            for item in items:
                per_file[item.file] += 1
            for path, count in sorted(per_file.items(), key=lambda kv: (-kv[1], kv[0])):
                print("  %4d  %s" % (count, path))
            print("  （行単位で見るには --verbose）")

    print("\n合計 %d 件（decisive %d / heuristic %d）" % (len(findings), total, htotal))


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", default=None,
                        help="リポジトリルート（省略時は設定ファイル / .git を探す）")
    parser.add_argument("--category", action="append", choices=list(ALL_CATEGORIES),
                        help="実行するカテゴリを限定（複数可、省略時は全部）")
    parser.add_argument("--strict", nargs="?", const="decisive",
                        choices=["decisive", "all"], default=None,
                        help="該当ティアに findings があれば exit 1（既定 decisive）")
    parser.add_argument("--baseline", default=None,
                        help="ベースライン JSON と比較し、増えた分だけ exit 1")
    parser.add_argument("--write-baseline", default=None,
                        help="現在の件数をベースライン JSON として書き出す")
    parser.add_argument("--json", action="store_true", help="機械可読出力")
    parser.add_argument("--verbose", action="store_true",
                        help="heuristic も行単位で表示する")
    parser.add_argument("--print-words", action="store_true",
                        help="状態語の語彙を出力する（語彙の正典は本スクリプト）")
    parser.add_argument("--print-version", action="store_true",
                        help="本スクリプトの内容ハッシュ（ベンダリング乖離の検出用）")
    args = parser.parse_args()

    if args.print_version:
        digest = hashlib.sha256(read_text(os.path.abspath(__file__)).encode()).hexdigest()
        print("%s sha256:%s" % (VENDORED_FROM, digest[:16]))
        return 0

    root = os.path.abspath(args.root) if args.root else find_root(os.getcwd())
    cfg = load_config(root)

    if args.print_words:
        strong, weak = status_vocabulary(cfg.remove_words)
        print("strong（単独で検出）: " + "・".join(strong))
        print("weak（Issue / PR 参照と同じ行にあるときだけ検出）: " + "・".join(weak))
        return 0

    categories = set(args.category) if args.category else set(ALL_CATEGORIES)
    linter = Linter(cfg)
    linter.run(categories)
    findings = linter.findings

    if args.write_baseline:
        path = args.write_baseline if os.path.isabs(args.write_baseline) \
            else os.path.join(root, args.write_baseline)
        with open(path, "w", encoding="utf-8") as handle:
            json.dump(counts_by_file(findings), handle,
                      ensure_ascii=False, indent=2, sort_keys=True)
            handle.write("\n")
        print("baseline written: %s" % path)
        return 0

    if args.json:
        print(json.dumps({
            "root": root,
            "categories": sorted(categories),
            "counts": counts_by_file(findings),
            "findings": [{"tier": f.tier, "category": f.category, "file": f.file,
                          "line": f.line, "message": f.message}
                         for f in sorted(findings, key=lambda f: (f.category, f.file, f.line))],
        }, ensure_ascii=False, indent=2))
    else:
        report(findings, args.verbose)

    if args.baseline:
        path = args.baseline if os.path.isabs(args.baseline) \
            else os.path.join(root, args.baseline)
        if not os.path.exists(path):
            sys.stderr.write("error: baseline not found: %s "
                             "(--write-baseline で作成する)\n" % path)
            return 2
        with open(path, encoding="utf-8") as handle:
            regressions = compare_baseline(findings, json.load(handle))
        if regressions:
            print("\n=== ベースラインからの増加 %d 件 ===" % len(regressions))
            for line in regressions:
                print("  " + line)
            return 1
        print("\nベースラインからの増加なし。")
        return 0

    if args.strict == "all" and findings:
        return 1
    if args.strict == "decisive" and any(f.tier == "decisive" for f in findings):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
