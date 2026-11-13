#!/usr/bin/env python3
"""Ask a PR that changes a repo-owned Skill's source paths for its reason.

Repo-owned Skills (`.claude/skills/MANIFEST.md`, class `repo`) are updated by
the implementation PRs that move the code they describe, but nothing told a PR
author that a Skill was affected. DEV-1990 found `tsf-tip-development` 33
commits and `azookey-inference-model-workflow` 27 commits behind their source.
This script is the same kind of gate as `check_docs_impact.py`, applied to
Skills. It reads the files a PR changes and the PR body, and fails when:

  1. a changed file matches an owned path of a Skill (the「担当パス」table of
     the manifest), and
  2. no changed file is under that Skill's directory (`.claude/skills/<name>/`
     or `.agents/skills/<name>/`), and
  3. the body's "Skill impact" section does not both tick "No Skill impact"
     and give a non-empty `Reason:`.

One reason covers every Skill the PR leaves untouched, so it should say why the
Skills' routing tables, test tables and procedures need no change. Body parsing
follows `check_docs_impact.py` (CRLF is normalised, `<!-- ... -->` comments are
removed, a placeholder reason such as「影響なし」counts as empty). Owned-path
globs use `check_docs_impact.path_matches`.

`--freshness` prints, instead of gating, how many commits reached each Skill's
owned paths since the Skill directory last changed. It replaces the manual count
of the weekly audit. The Skill's last change is the newest commit touching
`.claude/skills/<name>` or `.agents/skills/<name>`. A commit that touches several
Skills at once (a bulk sync) resets all of them to 0, whether or not it reviewed
each one, so read a 0 right after such a commit as "just synced", not "verified".

The gate fails (exit 1) when the manifest has no owned path at all, so a renamed
heading or a broken table cannot switch it off silently.

Bot PRs whose bodies do not follow the template (Dependabot) are excluded by the
workflow's `if:`, not here. Exit status is 0 when the PR passes (or the gate does
not apply), 1 when it fails or its inputs cannot be read, and 2 for a
command-line usage error.
"""

from __future__ import annotations

import argparse
import importlib.util
from pathlib import Path
import re
import subprocess
import sys
from types import ModuleType


sys.dont_write_bytecode = True
REPO_ROOT = Path(__file__).resolve().parents[1]
SCRIPTS_DIRECTORY = Path(__file__).resolve().parent

SECTION_HEADING = "Skill impact"
NO_IMPACT_LABEL = "No Skill impact"
SKILL_DIRECTORIES = (".claude/skills", ".agents/skills")


def load_script(name: str) -> ModuleType:
    spec = importlib.util.spec_from_file_location(name, SCRIPTS_DIRECTORY / f"{name}.py")
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


DOCS_IMPACT = load_script("check_docs_impact")
SKILL_REFERENCES = load_script("check_skill_references")


def load_ownership(repo_root: Path) -> dict[str, list[str]]:
    """Return {skill: owned globs} for Skills that own at least one path.

    Raises OSError when the manifest cannot be read or lists no owned path.
    """
    manifest = repo_root / SKILL_REFERENCES.MANIFEST_RELATIVE_PATH
    ownership, _ = SKILL_REFERENCES.parse_ownership(manifest.read_text(encoding="utf-8"))
    owning = {name: globs for name, globs in ownership.items() if globs}
    if not owning:
        # A renamed heading or a broken table must not turn the gate off silently.
        raise OSError(f"{SKILL_REFERENCES.MANIFEST_RELATIVE_PATH} の「担当パス」の表を読めません")
    return owning


def skill_patterns(skill: str) -> tuple[str, ...]:
    return tuple(f"{directory}/{skill}/**" for directory in SKILL_DIRECTORIES)


def extract_section(body: str) -> list[str] | None:
    """Return the lines of the Skill impact section, or None."""
    lines = DOCS_IMPACT.COMMENT_PATTERN.sub("", body.replace("\r\n", "\n")).split("\n")
    section: list[str] | None = None
    for line in lines:
        heading = DOCS_IMPACT.HEADING_PATTERN.match(line)
        if heading:
            if section is not None:
                break
            if heading.group(2).strip().casefold() == SECTION_HEADING.casefold():
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
        checkbox = DOCS_IMPACT.CHECKBOX_PATTERN.match(line)
        if checkbox and checkbox.group(1).lower() == "x" and label.match(checkbox.group(2)):
            return True
    return False


def evaluate(
    changed: list[str], body: str, ownership: dict[str, list[str]]
) -> tuple[bool, list[str]]:
    """Return (passed, message lines)."""
    untouched: dict[str, list[str]] = {}
    updated: list[str] = []
    for skill, globs in sorted(ownership.items()):
        owned = DOCS_IMPACT.matching(changed, tuple(globs))
        if not owned:
            continue
        if DOCS_IMPACT.matching(changed, skill_patterns(skill)):
            updated.append(skill)
        else:
            untouched[skill] = owned
    if not untouched:
        suffix = f"（Skill を更新済み: {', '.join(updated)}）" if updated else ""
        return True, [f"Skill impact: 担当パスの変更に対して Skill の更新漏れはありません{suffix}"]

    section = extract_section(body)
    checked = section is not None and is_no_impact_checked(section)
    reason = DOCS_IMPACT.extract_reason(section) if section is not None else ""
    if checked and reason:
        return True, [
            f"Skill impact: Skill {len(untouched)} 件の担当パスを変更し Skill は変更していませんが、"
            f"{NO_IMPACT_LABEL} と理由の記入を確認しました（{', '.join(untouched)}）"
        ]

    messages = [
        "Skill impact: 次の Skill の担当パスを変更していますが、その Skill を変更しておらず、"
        "PR 本文に影響なしの理由もありません。",
        "",
    ]
    for skill, paths in untouched.items():
        shown = paths[:3]
        more = f" ほか {len(paths) - len(shown)} 件" if len(paths) > len(shown) else ""
        messages.append(f"  - {skill}: {', '.join(shown)}{more}")
    messages.append("")
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
                "（コメントや「なし」「N/A」だけの記入は空とみなします）。"
            )
    messages += [
        "",
        "次のどちらかにしてください:",
        "  1. 上の Skill の routing 表・テスト表・手順のうち、この変更で古くなる箇所を同じ PR で更新する"
        "（`.claude/skills/` と `.agents/skills/` の両方）。",
        f"  2. Skill の入口表が変わらないなら、「{SECTION_HEADING}」節に次のように書く:",
        f"       - [x] {NO_IMPACT_LABEL}",
        "       Reason: <どの Skill の表も変わらない理由。例: 内部の整理でファイル名とテスト target は同じ>",
    ]
    return False, messages


def run_git(repo_root: Path, *arguments: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(repo_root), *arguments],
        capture_output=True,
        text=True,
        encoding="utf-8",
        check=False,
    )
    if result.returncode != 0:
        raise RuntimeError(f"git {' '.join(arguments)} に失敗しました: {result.stderr.strip()}")
    return result.stdout


def freshness(repo_root: Path, ownership: dict[str, list[str]], head: str) -> list[tuple[str, int, str]]:
    """Return (skill, commits since the Skill last changed, last change) rows."""
    rows: list[tuple[str, int, str]] = []
    for skill, globs in ownership.items():
        skill_paths = [f"{directory}/{skill}" for directory in SKILL_DIRECTORIES]
        last = run_git(repo_root, "log", "-1", "--format=%H %cs", head, "--", *skill_paths).split()
        if not last:
            rows.append((skill, -1, "（Skill の履歴なし）"))
            continue
        pathspecs = [f":(glob){pattern}" for pattern in globs]
        count = run_git(repo_root, "rev-list", "--count", f"{last[0]}..{head}", "--", *pathspecs)
        rows.append((skill, int(count.strip()), f"{last[1]} {last[0][:9]}"))
    return sorted(rows, key=lambda row: (-row[1], row[0]))


def print_freshness(rows: list[tuple[str, int, str]]) -> None:
    print("Skill の最終更新以降に担当パスへ入った commit 数（多い順）")
    print("")
    print(f"{'Skill':<42}{'commits':>8}  最終更新")
    for skill, count, last in rows:
        shown = "-" if count < 0 else str(count)
        print(f"{skill:<42}{shown:>8}  {last}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--freshness",
        action="store_true",
        help="ゲートを回さず、Skill ごとの最終更新以降の担当パスの commit 数を出す",
    )
    parser.add_argument(
        "--changed-files",
        type=Path,
        help="変更ファイルの一覧（1 行 1 パス、リポジトリルートからの相対）",
    )
    parser.add_argument("--base", help="比較の base commit（--head と組で使う）")
    parser.add_argument("--head", help="比較の head commit（--freshness では集計の終点。既定 HEAD）")
    parser.add_argument(
        "--body-file",
        type=Path,
        help="PR 本文を書いたファイル（空のファイルは空の本文として扱う）",
    )
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=REPO_ROOT,
        help="検査対象のリポジトリルート（既定: このスクリプトの親ディレクトリ）",
    )
    arguments = parser.parse_args(argv)
    repo_root = arguments.repo_root.resolve()

    try:
        ownership = load_ownership(repo_root)
    except OSError as error:
        print(f"Skill impact: manifest を読めません: {error}", file=sys.stderr)
        return 1

    if arguments.freshness:
        if arguments.changed_files or arguments.base or arguments.body_file:
            parser.error("--freshness は --head と --repo-root だけを受け付けます")
        try:
            print_freshness(freshness(repo_root, ownership, arguments.head or "HEAD"))
        except RuntimeError as error:
            print(f"Skill impact: {error}", file=sys.stderr)
            return 1
        return 0

    has_list = arguments.changed_files is not None
    has_range = arguments.base is not None or arguments.head is not None
    if has_list == has_range or (has_range and (arguments.base is None or arguments.head is None)):
        parser.error("--changed-files か、--base と --head の組のどちらか一方を指定してください")
    if arguments.body_file is None:
        parser.error("--body-file を指定してください")

    try:
        if has_list:
            changed = DOCS_IMPACT.read_changed_files(arguments.changed_files)
        else:
            changed = DOCS_IMPACT.git_changed_files(repo_root, arguments.base, arguments.head)
        body = arguments.body_file.read_text(encoding="utf-8")
    except (OSError, RuntimeError) as error:
        print(f"Skill impact: 入力を読めません: {error}", file=sys.stderr)
        return 1

    passed, messages = evaluate(changed, body, ownership)
    stream = sys.stdout if passed else sys.stderr
    for line in messages:
        print(line, file=stream)
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
