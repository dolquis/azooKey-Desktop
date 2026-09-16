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
"""

from __future__ import annotations

import argparse
import importlib.util
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


def load_test_inventory_module():
    script = REPO_ROOT / "scripts" / "check_test_inventory.py"
    spec = importlib.util.spec_from_file_location("check_test_inventory", script)
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
    classifications = parse_manifest(manifest_path.read_text(encoding="utf-8"))
    problems: list[str] = []

    present = {entry.name for entry in skills_dir.iterdir() if entry.is_dir()}
    for name in sorted(present - classifications.keys()):
        problems.append(f"{MANIFEST_RELATIVE_PATH}: 表に無い Skill ディレクトリ `{name}`")
    for name in sorted(classifications.keys() - present):
        problems.append(f"{MANIFEST_RELATIVE_PATH}: 実在しない Skill の行 `{name}`")

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
