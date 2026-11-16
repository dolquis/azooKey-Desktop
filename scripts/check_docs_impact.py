#!/usr/bin/env python3
"""Ask a PR that changes a contract file but no docs for its reason.

The PR template's "Documentation impact" section was self-reported only, so a
PR could change an IPC payload, a settings schema or a CMake preset, leave the
spec that defines it untouched, and still merge. This script turns that section
into a gate. It reads the files a PR changes and the PR body, and fails when:

  1. a changed file matches one of `CONTRACT_PATHS`, and
  2. no changed file is under `docs/` or `plans/`, nor is the root `README.md`
     (`DOCS_PATHS`), and
  3. the body's "Documentation impact" section does not both tick
     "No documentation impact" and give a non-empty `Reason:`.

`CONTRACT_PATHS` is the single list of contract files. The governance Skill
refers to this constant (and to `--print-contract-paths`) instead of copying
it. Patterns are matched segment by segment with `fnmatch`, so `*` never
crosses `/`: `settings/*.schema.json` does not match a nested directory.

Body rules:

  - CRLF is normalised and `<!-- ... -->` comments are removed first, so the
    template's guidance comment alone is an empty reason. A `<!--` that is never
    closed hides the rest of the body, except one inside a code span or a fence.
  - Fenced code blocks (``` and ~~~, read as CommonMark does) are dropped, so a
    section heading or a reason written inside one does not count.
  - The section is the level-2 heading `## Documentation impact` (any case) up
    to the next heading of any level. A heading of another level is not the
    section.
  - The checkbox line is `- [x] No documentation impact`; `[X]`, `[ x ]` and
    `*` / `+` bullets are accepted.
  - The reason is the text after `Reason:` (`**Reason:**` and a full-width
    colon are accepted) plus the following lines of the section. A reason that
    is empty when `is_substantive_reason` rejects it: a placeholder in
    `PLACEHOLDER_REASONS`, a `TODO` / `TBD` / `FIXME` marker, text left in the
    template's brackets (`<理由>`), or fewer than `MIN_REASON_CHARACTERS`
    letters and digits once symbols are ignored.

The changed files come either from `--changed-files <file>` (one path per
line) or from `git diff --name-only --no-renames <base>...<head>`. Renames are
split into a deletion and an addition so that renaming a contract file still
counts as changing it.

Bot PRs whose bodies do not follow the template (Dependabot) are excluded by
the workflow's `if:`, not here; this script has no bypass option.

Exit status is 0 when the PR passes (or the gate does not apply), 1 when it
fails or its inputs cannot be read, and 2 for a command-line usage error.
"""

from __future__ import annotations

import argparse
import fnmatch
from pathlib import Path
import re
import subprocess
import sys


REPO_ROOT = Path(__file__).resolve().parents[1]

# The contract files: a change to one of these changes behaviour that a spec,
# the roadmap or the README defines. Keep the list to files whose change
# almost always needs a docs decision; a file that changes with ordinary
# implementation work makes the gate noise and teaches people to write
# throwaway reasons.
CONTRACT_PATHS = (
    # IPC wire schema: payload structs and their JSON codec.
    "ipc/include/azookey/ipc/Payloads.h",
    "ipc/src/Payloads.cpp",
    # MessageType and the envelope. Messages.cpp holds TypeToString, the
    # `type` string on the wire, so it is part of the wire format too.
    "ipc/include/azookey/ipc/Messages.h",
    "ipc/src/Messages.cpp",
    # Frame and request limits. ipc-persistence-hardening-spec declares itself
    # the canonical source of these values and Limits.h follows it.
    "ipc/include/azookey/ipc/Limits.h",
    # Settings and model catalog schemas.
    "settings/*.schema.json",
    # The default settings sample: its defaults are what the settings spec
    # documents. The default model catalog is data validated by its schema,
    # not a contract, so it is left out.
    "settings/default-settings.sample.json",
    # Build and test presets, which README and build-ci-spec name.
    "CMakePresets.json",
    # CI jobs, their triggers and required checks.
    ".github/workflows/*",
)
# Deliberately not listed: inference-host/src/Dispatcher.cpp. It holds handler
# bodies and changes with most Host work; a new message type already shows up
# through Messages.h.

# A change under any of these counts as having looked at the docs.
DOCS_PATHS = ("docs/**", "plans/**", "README.md")

SECTION_HEADING = "Documentation impact"
NO_IMPACT_LABEL = "No documentation impact"

# Reasons that say nothing. "影響なし" as the reason for "No documentation
# impact" is exactly what the gate exists to stop.
PLACEHOLDER_REASONS = frozenset(
    {
        "-",
        "n/a",
        "na",
        "none",
        "no impact",
        "tbd",
        "なし",
        "無し",
        "特になし",
        "影響なし",
    }
)

# A reason shorter than this, counting letters and digits only, is too thin to
# say why no document changes. "内部整理" (4) and "テストのみ" (5) pass; "なし"
# and "N/A" are caught here as well as by `PLACEHOLDER_REASONS`.
MIN_REASON_CHARACTERS = 4
SECTION_LEVEL = 2
# A reason that is only one of these, or one followed by a colon ("TODO: 後で"),
# was never filled in. "TODO コメントの修正のみ" is a real reason and passes.
UNFINISHED_MARKERS = ("todo", "tbd", "fixme")

HEADING_PATTERN = re.compile(r"^\s{0,3}(#{1,6})\s+(.*?)\s*#*\s*$")
FENCE_PATTERN = re.compile(r"^\s{0,3}(`{3,}|~{3,})(.*)$")
INLINE_CODE_OR_COMMENT_OPEN = re.compile(r"`[^`\n]*`|<!--")
# A reason that is nothing but a bracketed phrase: the template's `<理由>`.
BRACKETED_PATTERN = re.compile(
    r"^(?:<[^<>]*>|\[[^\[\]]*\]|［[^［］]*］|【[^【】]*】|＜[^＜＞]*＞)$"
)
CHECKBOX_PATTERN = re.compile(r"^\s*[-*+]\s*\[\s*([^\]]?)\s*\]\s*(.*)$")
REASON_PATTERN = re.compile(
    r"^\s*(?:[-*+]\s*)?(?:\*\*|__)?Reason\s*[:：]\s*(?:\*\*|__)?(.*)$",
    re.IGNORECASE,
)


def path_matches(path: str, pattern: str) -> bool:
    """Match `path` against `pattern` one `/` segment at a time.

    A trailing `**` segment matches any number of remaining segments.
    """
    path_parts = path.split("/")
    pattern_parts = pattern.split("/")
    if pattern_parts[-1] == "**":
        prefix = pattern_parts[:-1]
        return len(path_parts) > len(prefix) and all(
            fnmatch.fnmatchcase(part, glob)
            for part, glob in zip(path_parts, prefix)
        )
    return len(path_parts) == len(pattern_parts) and all(
        fnmatch.fnmatchcase(part, glob)
        for part, glob in zip(path_parts, pattern_parts)
    )


def matching(paths: list[str], patterns: tuple[str, ...]) -> list[str]:
    return [
        path for path in paths if any(path_matches(path, p) for p in patterns)
    ]


def read_changed_files(path: Path) -> list[str]:
    lines = path.read_text(encoding="utf-8").splitlines()
    return [normalise_path(line) for line in lines if line.strip()]


def normalise_path(path: str) -> str:
    path = path.strip().replace("\\", "/")
    while path.startswith("./"):
        path = path[2:]
    return path


def git_changed_files(repo_root: Path, base: str, head: str) -> list[str]:
    result = subprocess.run(
        [
            "git",
            "-C",
            str(repo_root),
            "diff",
            "--name-only",
            "--no-renames",
            f"{base}...{head}",
        ],
        capture_output=True,
        text=True,
        encoding="utf-8",
        check=False,
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"git diff {base}...{head} に失敗しました: {result.stderr.strip()}"
        )
    return [normalise_path(line) for line in result.stdout.splitlines() if line]


def visible_lines(body: str) -> list[str]:
    """Return the body's lines without fenced code and HTML comments.

    One pass keeps the two apart: a `<!--` inside a fence is code, and a fence
    inside a comment is part of the comment, so neither hides the other. A
    comment that is never closed hides the rest of the body.

    Fences follow CommonMark: a backtick fence's info string cannot hold a
    backtick (so a line starting with an inline ```code``` is not a fence), and
    a closing fence carries nothing after the fence characters. A `<!--` inside
    an inline code span is text, not a comment.
    """
    kept: list[str] = []
    fence: str | None = None
    in_comment = False
    for raw in body.replace("\r\n", "\n").split("\n"):
        line = raw
        if in_comment:
            end = line.find("-->")
            if end < 0:
                continue
            line = line[end + 3 :]
            in_comment = False
        elif fence is not None:
            match = FENCE_PATTERN.match(line)
            if (
                match
                and match.group(1)[0] == fence[0]
                and len(match.group(1)) >= len(fence)
                and not match.group(2).strip()
            ):
                fence = None
            continue
        else:
            match = FENCE_PATTERN.match(line)
            if match and not (match.group(1)[0] == "`" and "`" in match.group(2)):
                fence = match.group(1)
                continue
        # Remove comments from the rest of the line; a code span hides its `<!--`.
        text = ""
        position = 0
        for token in INLINE_CODE_OR_COMMENT_OPEN.finditer(line):
            if token.group() != "<!--" or token.start() < position:
                continue
            text += line[position : token.start()]
            end = line.find("-->", token.end())
            if end < 0:
                in_comment = True
                position = len(line)
                break
            position = end + 3
        kept.append(text + line[position:])
    return kept


def extract_section(body: str, title: str = SECTION_HEADING) -> list[str] | None:
    """Return the lines of the level-2 section titled `title`, or None.

    Both impact gates read their section through this function, so they cannot
    disagree on comments, code fences or heading levels.
    """
    lines = visible_lines(body)
    section: list[str] | None = None
    for line in lines:
        heading = HEADING_PATTERN.match(line)
        if heading:
            if section is not None:
                break
            if (
                len(heading.group(1)) == SECTION_LEVEL
                and heading.group(2).strip().casefold() == title.casefold()
            ):
                section = []
            continue
        if section is not None:
            section.append(line)
    return section


def is_no_impact_checked(section: list[str]) -> bool:
    label = re.compile(
        r"^" + r"\s+".join(map(re.escape, NO_IMPACT_LABEL.split())) + r"\b",
        re.IGNORECASE,
    )
    for line in section:
        checkbox = CHECKBOX_PATTERN.match(line)
        if checkbox and checkbox.group(1).lower() == "x" and label.match(
            checkbox.group(2)
        ):
            return True
    return False


def extract_reason(section: list[str]) -> str:
    reason_lines: list[str] | None = None
    for line in section:
        if reason_lines is None:
            match = REASON_PATTERN.match(line)
            if match:
                reason_lines = [match.group(1)]
            continue
        if CHECKBOX_PATTERN.match(line):
            break
        reason_lines.append(line)
    if reason_lines is None:
        return ""
    reason = "\n".join(reason_lines).strip()
    return reason if is_substantive_reason(reason) else ""


def is_substantive_reason(reason: str) -> bool:
    """Return False for a reason that only fills the field in."""
    plain = reason.strip("*_ \t\n.。")
    folded = plain.casefold()
    if folded in PLACEHOLDER_REASONS:
        return False
    if any(re.match(rf"{marker}(?:\s*[:：]|$)", folded) for marker in UNFINISHED_MARKERS):
        return False
    if BRACKETED_PATTERN.match(plain):
        return False
    return sum(character.isalnum() for character in reason) >= MIN_REASON_CHARACTERS


def evaluate(changed: list[str], body: str) -> tuple[bool, list[str]]:
    """Return (passed, message lines)."""
    contracts = matching(changed, CONTRACT_PATHS)
    if not contracts:
        return True, [
            f"docs impact: 契約ファイルの変更なし（変更ファイル {len(changed)} 件）"
        ]
    docs = matching(changed, DOCS_PATHS)
    if docs:
        return True, [
            f"docs impact: 契約ファイル {len(contracts)} 件の変更と、"
            f"docs/・plans/・README.md の変更 {len(docs)} 件を確認しました"
        ]

    section = extract_section(body)
    checked = section is not None and is_no_impact_checked(section)
    reason = extract_reason(section) if section is not None else ""
    if checked and reason:
        return True, [
            f"docs impact: 契約ファイル {len(contracts)} 件の変更に対し、"
            "No documentation impact と理由の記入を確認しました"
        ]

    messages = [
        "docs impact: 契約ファイルを変更していますが、docs/・plans/・README.md の"
        "変更がなく、PR 本文に影響なしの理由もありません。",
        "",
        "変更した契約ファイル:",
        *(f"  - {path}" for path in contracts),
        "",
    ]
    if section is None:
        messages.append(
            f"PR 本文に「## {SECTION_HEADING}」節がありません。"
            ".github/PULL_REQUEST_TEMPLATE.md の節を本文へ戻してください。"
        )
    else:
        if not checked:
            messages.append(f"「{NO_IMPACT_LABEL}」にチェックが入っていません。")
        if not reason:
            messages.append(
                "「Reason:」の後に理由がありません"
                "（コメント、「なし」「N/A」、TODO、記号だけ、<理由> のままの記入、"
                f"{MIN_REASON_CHARACTERS} 字に満たない短い記入は空とみなします）。"
            )
    messages += [
        "",
        "次のどちらかにしてください:",
        "  1. 契約の変更を定義する spec・roadmap・README を同じ PR で更新する。",
        f"  2. 文書に影響しないなら、「{SECTION_HEADING}」節に次のように書く:",
        f"       - [x] {NO_IMPACT_LABEL}",
        "       Reason: <どの文書も変わらない理由。例: 内部の整理で wire 形式は同じ>",
    ]
    return False, messages


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--print-contract-paths",
        action="store_true",
        help="契約ファイルのパターン（CONTRACT_PATHS）を 1 行ずつ出して終了する",
    )
    parser.add_argument(
        "--changed-files",
        type=Path,
        help="変更ファイルの一覧（1 行 1 パス、リポジトリルートからの相対）",
    )
    parser.add_argument("--base", help="比較の base commit（--head と組で使う）")
    parser.add_argument("--head", help="比較の head commit（--base と組で使う）")
    parser.add_argument(
        "--body-file",
        type=Path,
        help="PR 本文を書いたファイル（空のファイルは空の本文として扱う）",
    )
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=REPO_ROOT,
        help="git diff を実行するリポジトリルート（既定: このスクリプトの親ディレクトリ）",
    )
    arguments = parser.parse_args(argv)

    if arguments.print_contract_paths:
        for pattern in CONTRACT_PATHS:
            print(pattern)
        return 0

    has_list = arguments.changed_files is not None
    has_range = arguments.base is not None or arguments.head is not None
    if has_list == has_range or (
        has_range and (arguments.base is None or arguments.head is None)
    ):
        parser.error(
            "--changed-files か、--base と --head の組のどちらか一方を指定してください"
        )
    if arguments.body_file is None:
        parser.error("--body-file を指定してください")

    try:
        if has_list:
            changed = read_changed_files(arguments.changed_files)
        else:
            changed = git_changed_files(
                arguments.repo_root, arguments.base, arguments.head
            )
        body = arguments.body_file.read_text(encoding="utf-8")
    except (OSError, RuntimeError) as error:
        print(f"docs impact: 入力を読めません: {error}", file=sys.stderr)
        return 1

    passed, messages = evaluate(changed, body)
    stream = sys.stdout if passed else sys.stderr
    for line in messages:
        print(line, file=stream)
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
