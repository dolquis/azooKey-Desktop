#!/usr/bin/env python3
"""Check repo-owned Skill files against the repository they describe.

`.claude/skills/MANIFEST.md` classifies every Skill. For the ones it marks
`repo`, the routing tables in `SKILL.md` and `references/*.md` name source
files, docs and CTest targets by hand. Nothing invalidates those names when the
code moves, so this script does: every back-quoted repository path must exist
and every back-quoted `*_tests` name must be a target CMake registers.

Shared (`dolquis/agent-ops`) and third-party Skills are skipped on purpose:
this repo must not edit them, so a finding there could only be fixed at the
origin. The manifest itself is checked both ways — a Skill directory missing
from the table and a table row without a directory are both reported.

The manifest's「担当パス」table says which source paths each `repo` Skill
owns. It feeds `check_skill_impact.py`, so it is checked here too: every `repo`
Skill has exactly one row, every glob has the shape that script understands and
matches at least one path of the repository.
"""

from __future__ import annotations

import argparse
import importlib.util
import os
from pathlib import Path
import re
import sys


sys.dont_write_bytecode = True
REPO_ROOT = Path(__file__).resolve().parents[1]
MANIFEST_RELATIVE_PATH = ".claude/skills/MANIFEST.md"
SKILLS_RELATIVE_DIRECTORY = ".claude/skills"

# Top-level directories a back-quoted token may start with to count as a
# repository path. Anything else in back-quotes (symbols, flags, URLs, sample
# project layouts) is not a path claim and is ignored.
REPOSITORY_ROOTS = (
    ".agents",
    ".claude",
    ".github",
    "bench",
    "cmake",
    "compat-test",
    "core",
    "data",
    "diagnostics",
    "dictbuild",
    "docs",
    "inference-host",
    "ipc",
    "learning",
    "pkg",
    "plans",
    "scripts",
    "settings",
    "settings-app",
    "tsf-tip",
)

BACKQUOTE_PATTERN = re.compile(r"`([^`\n]+)`")
TEST_TARGET_PATTERN = re.compile(r"^[a-z][a-z0-9_]*_tests$")
MANIFEST_ROW_PATTERN = re.compile(r"^\|\s*`([^`]+)`\s*\|\s*([a-z-]+)\s*\|")
OWNERSHIP_HEADING = "担当パス"
OWNERSHIP_ROW_PATTERN = re.compile(r"^\|\s*`([^`]+)`\s*\|\s*(.*?)\s*\|\s*$")
HEADING_PATTERN = re.compile(r"^(#{1,6})\s+(.*?)\s*$")
NO_OWNERSHIP = "—"


def load_test_inventory_module():
    script = REPO_ROOT / "scripts" / "check_test_inventory.py"
    spec = importlib.util.spec_from_file_location("check_test_inventory", script)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def load_docs_impact_module():
    script = REPO_ROOT / "scripts" / "check_docs_impact.py"
    spec = importlib.util.spec_from_file_location("check_docs_impact", script)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def parse_manifest(text: str) -> dict[str, str]:
    """Return {skill name: classification} from the manifest table."""
    classifications: dict[str, str] = {}
    for line in text.splitlines():
        match = MANIFEST_ROW_PATTERN.match(line)
        if match:
            classifications[match.group(1)] = match.group(2)
    return classifications


def parse_ownership(text: str) -> tuple[dict[str, list[str]], list[str]]:
    """Return ({skill: globs}, duplicated skill names) from the 担当パス table."""
    ownership: dict[str, list[str]] = {}
    duplicates: list[str] = []
    in_section = False
    section_level = 0
    for line in text.splitlines():
        heading = HEADING_PATTERN.match(line)
        if heading:
            if in_section and len(heading.group(1)) <= section_level:
                break
            if heading.group(2) == OWNERSHIP_HEADING:
                in_section = True
                section_level = len(heading.group(1))
            continue
        if not in_section:
            continue
        match = OWNERSHIP_ROW_PATTERN.match(line)
        if not match:
            continue
        name, cell = match.group(1), match.group(2)
        if name in ownership:
            duplicates.append(name)
        globs = BACKQUOTE_PATTERN.findall(cell)
        if cell != NO_OWNERSHIP and not globs:
            globs = [cell]  # reported as an invalid glob
        ownership[name] = [] if cell == NO_OWNERSHIP else globs
    return ownership, duplicates


def glob_shape_problem(pattern: str) -> str | None:
    """Return why `pattern` is not a glob check_skill_impact.py can match."""
    if not pattern or pattern.startswith("/") or "\\" in pattern or " " in pattern:
        return "リポジトリルートからの相対パスで、空白とバックスラッシュを含めずに書く"
    parts = pattern.split("/")
    if ".." in parts or "." in parts or "" in parts:
        return "`.`、`..`、空の segment を含めない"
    if any("**" in part and part != "**" for part in parts) or "**" in parts[:-1]:
        return "`**` は末尾の 1 segment だけに使える"
    return None


def glob_matches_a_file(repo_root: Path, pattern: str) -> bool:
    """Whether some file under `repo_root` matches `pattern`.

    Uses `check_docs_impact.path_matches`, the matcher the Skill impact gate
    applies to changed files, so a glob that passes here selects what the gate
    selects. `Path.glob` is not used: its `**` differs between Python versions.
    Only the directory the pattern's literal prefix names is walked, and only
    as deep as the pattern reaches.
    """
    parts = pattern.split("/")
    literal = 0
    while literal < len(parts) and not any(c in parts[literal] for c in "*?["):
        literal += 1
    if literal == len(parts):
        return (repo_root / pattern).is_file()
    base = repo_root.joinpath(*parts[:literal])
    if not base.is_dir():
        return False
    max_depth = None if parts[-1] == "**" else len(parts) - literal
    matches = load_docs_impact_module().path_matches
    for directory, directories, files in os.walk(base):
        depth = len(Path(directory).relative_to(base).parts) + 1
        if max_depth is not None and depth >= max_depth:
            directories[:] = []
        for name in files:
            relative = (Path(directory) / name).relative_to(repo_root).as_posix()
            if matches(relative, pattern):
                return True
    return False


def check_ownership(
    repo_root: Path, text: str, repo_skills: set[str]
) -> list[str]:
    ownership, duplicates = parse_ownership(text)
    problems: list[str] = []
    for name in sorted(set(duplicates)):
        problems.append(f"{MANIFEST_RELATIVE_PATH}: 「{OWNERSHIP_HEADING}」の表に `{name}` の行が複数あります")
    for name in sorted(repo_skills - ownership.keys()):
        problems.append(f"{MANIFEST_RELATIVE_PATH}: 「{OWNERSHIP_HEADING}」の表に区分 `repo` の Skill `{name}` の行がありません")
    for name in sorted(ownership.keys() - repo_skills):
        problems.append(f"{MANIFEST_RELATIVE_PATH}: 「{OWNERSHIP_HEADING}」の表の `{name}` は区分 `repo` の Skill ではありません")
    for name, globs in sorted(ownership.items()):
        for pattern in globs:
            shape = glob_shape_problem(pattern)
            if shape is not None:
                problems.append(f"{MANIFEST_RELATIVE_PATH}: `{name}` の担当パス `{pattern}` が不正です（{shape}）")
            elif not glob_matches_a_file(repo_root, pattern):
                problems.append(f"{MANIFEST_RELATIVE_PATH}: `{name}` の担当パス `{pattern}` に一致するパスがありません")
    return problems


def is_repository_path(token: str) -> bool:
    if any(character in token for character in "*<>{} "):
        return False
    if token.endswith("/"):
        token = token[:-1]
    head, separator, _ = token.partition("/")
    return bool(separator) and head in REPOSITORY_ROOTS


def check_skill_file(
    path: Path, repo_root: Path, skill_dir: Path, registered_targets: set[str]
) -> list[str]:
    relative = path.relative_to(repo_root).as_posix()
    problems: list[str] = []
    seen: set[str] = set()
    for match in BACKQUOTE_PATTERN.finditer(path.read_text(encoding="utf-8")):
        token = match.group(1).strip()
        if token in seen:
            continue
        seen.add(token)
        if TEST_TARGET_PATTERN.match(token):
            if token not in registered_targets:
                problems.append(f"{relative}: CMake に登録されていないテスト target `{token}`")
        elif token.startswith("references/"):
            if not (skill_dir / token).exists():
                problems.append(f"{relative}: Skill 内に存在しない参照 `{token}`")
        elif is_repository_path(token):
            if not (repo_root / token.rstrip("/")).exists():
                problems.append(f"{relative}: 存在しないパス `{token}`")
    return problems


def check(repo_root: Path) -> list[str]:
    manifest_path = repo_root / MANIFEST_RELATIVE_PATH
    skills_dir = repo_root / SKILLS_RELATIVE_DIRECTORY
    manifest_text = manifest_path.read_text(encoding="utf-8")
    classifications = parse_manifest(manifest_text)
    problems: list[str] = []

    present = {entry.name for entry in skills_dir.iterdir() if entry.is_dir()}
    for name in sorted(present - classifications.keys()):
        problems.append(f"{MANIFEST_RELATIVE_PATH}: 表に無い Skill ディレクトリ `{name}`")
    for name in sorted(classifications.keys() - present):
        problems.append(f"{MANIFEST_RELATIVE_PATH}: 実在しない Skill の行 `{name}`")

    repo_skills = {name for name, kind in classifications.items() if kind == "repo"}
    problems.extend(check_ownership(repo_root, manifest_text, repo_skills))

    inventory = load_test_inventory_module()
    registrations = inventory.collect_registrations(repo_root)
    registered_targets = registrations.targets | set(registrations.ctest_names)

    for name, classification in sorted(classifications.items()):
        if classification != "repo" or name not in present:
            continue
        skill_dir = skills_dir / name
        files = [skill_dir / "SKILL.md", *sorted((skill_dir / "references").glob("*.md"))]
        for path in files:
            if path.exists():
                problems.extend(check_skill_file(path, repo_root, skill_dir, registered_targets))
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=REPO_ROOT,
        help="検査対象のリポジトリルート（既定: このスクリプトの親ディレクトリ）",
    )
    arguments = parser.parse_args(argv)
    problems = check(arguments.repo_root.resolve())
    if problems:
        print("repo 固有 Skill の参照が実体と一致しません:", file=sys.stderr)
        for problem in problems:
            print(f"  - {problem}", file=sys.stderr)
        print(
            "\nSkill の routing 表を実体へ合わせるか、"
            f"{MANIFEST_RELATIVE_PATH} の区分を見直してください。",
            file=sys.stderr,
        )
        return 1
    print("repo 固有 Skill の参照は実体と一致しています。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
