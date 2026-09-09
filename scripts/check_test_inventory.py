#!/usr/bin/env python3
"""Check the test inventory document against the CMake registrations.

`docs/test-inventory.md` is the canonical list of the test suite, but nothing
stopped its table from drifting behind the `CMakeLists.txt` files that actually
register the tests. This script closes that loop: it reads the registrations
out of CMake, reads the table out of the document, and fails when the two
disagree.

It deliberately *checks* rather than *generates*. The table's third column is
hand-written prose about what each test covers, and no generator can derive
that from CMake; regenerating the table would throw it away on every run.

Two kinds of registration are collected:

  gtest target   a target handed to `azookey_discover_tests()` (directly or
                 through the `add_tsf_tip_unit_test()` wrapper). Its test
                 sources are the `*_test.cpp` / `*_tests.cpp` files of its
                 `add_executable()` call, resolved against the directory of the
                 `CMakeLists.txt` that declares them — the whole repo-relative
                 path is compared, so moving a test between directories is drift
                 too. Each (target, source) pair needs one table row.
  CTest entry    an `add_test(NAME ...)` written directly in CMake. Each name
                 needs one table row.

Registrations guarded by `if(WIN32)` or by a build option are collected too:
the table documents the whole suite, not one platform's slice of it.

Exit status is 0 when the table and CMake agree, 1 otherwise.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys


REPO_ROOT = Path(__file__).resolve().parents[1]
INVENTORY_RELATIVE_PATH = "docs/test-inventory.md"
INVENTORY_HEADING = "現存テスト一覧"

# Directories whose CMake files describe vendored or unmaintained trees.
EXCLUDED_TOP_LEVEL_DIRECTORIES = ("build", "legacy", "third_party")

TEST_SOURCE_PATTERN = re.compile(r"_tests?\.cpp$")

ADD_EXECUTABLE_PATTERN = re.compile(r"\badd_executable\s*\(", re.MULTILINE)
DISCOVER_TESTS_PATTERN = re.compile(r"\bazookey_discover_tests\s*\(", re.MULTILINE)
TSF_TIP_WRAPPER_PATTERN = re.compile(r"\badd_tsf_tip_unit_test\s*\(", re.MULTILINE)
ADD_TEST_PATTERN = re.compile(r"\badd_test\s*\(\s*NAME\s+([A-Za-z0-9_]+)")

BACKTICKED_PATTERN = re.compile(r"`([^`]+)`")


def strip_cmake_comments(text: str) -> str:
    """Drop whole-line CMake comments so prose cannot look like a call."""
    return "\n".join(
        line for line in text.splitlines() if not line.lstrip().startswith("#")
    )


def read_call_arguments(text: str, open_parenthesis_index: int) -> list[str]:
    """Return the whitespace-separated arguments of a call, quotes removed."""
    depth = 0
    for index in range(open_parenthesis_index, len(text)):
        character = text[index]
        if character == "(":
            depth += 1
        elif character == ")":
            depth -= 1
            if depth == 0:
                body = text[open_parenthesis_index + 1 : index]
                return [argument.strip('"') for argument in body.split()]
    return []


def resolve_source(cmakelists_relative_directory: str, source: str) -> str | None:
    """Resolve a source listed in CMake to a repo-relative path."""
    if "$" in source:
        return None
    parts = [
        part
        for part in f"{cmakelists_relative_directory}/{source}".split("/")
        if part not in ("", ".")
    ]
    return "/".join(parts)


def collect_cmake_files(repo_root: Path) -> list[Path]:
    files = []
    for path in sorted(repo_root.rglob("CMakeLists.txt")):
        relative = path.relative_to(repo_root)
        if relative.parts[0] in EXCLUDED_TOP_LEVEL_DIRECTORIES:
            continue
        files.append(path)
    return files


class Registrations:
    def __init__(self) -> None:
        # (gtest target, repo-relative test source) -> defining CMake file
        self.gtest_pairs: dict[tuple[str, str], str] = {}
        # add_test(NAME ...) -> defining CMake file
        self.ctest_names: dict[str, str] = {}

    @property
    def targets(self) -> set[str]:
        return {target for target, _ in self.gtest_pairs}


def collect_registrations(repo_root: Path) -> Registrations:
    registrations = Registrations()

    for path in collect_cmake_files(repo_root):
        relative = str(path.relative_to(repo_root))
        directory = str(path.parent.relative_to(repo_root))
        text = strip_cmake_comments(path.read_text(encoding="utf-8"))

        executable_sources: dict[str, list[str]] = {}
        for match in ADD_EXECUTABLE_PATTERN.finditer(text):
            arguments = read_call_arguments(text, match.end() - 1)
            if not arguments or arguments[0].startswith("$"):
                # `add_executable(${target_name} ...)` inside a wrapper
                # function: the concrete target comes from the call site.
                continue
            executable_sources[arguments[0]] = arguments[1:]

        discovered_targets = []
        for match in DISCOVER_TESTS_PATTERN.finditer(text):
            arguments = read_call_arguments(text, match.end() - 1)
            if arguments and not arguments[0].startswith("$"):
                discovered_targets.append(arguments[0])

        for target in discovered_targets:
            for source in executable_sources.get(target, []):
                if not TEST_SOURCE_PATTERN.search(source):
                    continue
                resolved = resolve_source(directory, source)
                if resolved is not None:
                    registrations.gtest_pairs[(target, resolved)] = relative

        for match in TSF_TIP_WRAPPER_PATTERN.finditer(text):
            arguments = read_call_arguments(text, match.end() - 1)
            if len(arguments) < 2:
                continue
            target, source = arguments[0], arguments[1]
            resolved = resolve_source(directory, source)
            if resolved is not None:
                registrations.gtest_pairs[(target, resolved)] = relative

        for match in ADD_TEST_PATTERN.finditer(text):
            registrations.ctest_names[match.group(1)] = relative

    return registrations


def extract_inventory_section(document_text: str) -> list[str]:
    lines = document_text.splitlines()
    try:
        start = next(
            index
            for index, line in enumerate(lines)
            if line.startswith("#") and line.lstrip("#").strip() == INVENTORY_HEADING
        )
    except StopIteration:
        raise SystemExit(
            f"{INVENTORY_RELATIVE_PATH} に見出し「{INVENTORY_HEADING}」がありません。"
        )
    section = []
    for line in lines[start + 1 :]:
        if line.startswith("#"):
            break
        section.append(line)
    return section


def parse_inventory_rows(section_lines: list[str]) -> list[tuple[str, str]]:
    """Return (first cell token, second cell token) for each table row."""
    rows = []
    for line in section_lines:
        stripped = line.strip()
        if not stripped.startswith("|"):
            continue
        cells = [cell.strip() for cell in stripped.strip("|").split("|")]
        if len(cells) < 2:
            continue
        if set(cells[0]) <= set("-: "):
            continue
        first = BACKTICKED_PATTERN.search(cells[0])
        second = BACKTICKED_PATTERN.search(cells[1])
        if first is None:
            continue
        rows.append((first.group(1), second.group(1) if second else ""))
    return rows


def compare(
    registrations: Registrations, rows: list[tuple[str, str]]
) -> list[str]:
    problems = []

    documented_pairs: set[tuple[str, str]] = set()
    documented_ctest_names: set[str] = set()

    for first, second in rows:
        if first in registrations.targets:
            if (first, second) in registrations.gtest_pairs:
                documented_pairs.add((first, second))
            else:
                problems.append(
                    f"表の行 `{first}` / `{second}` は CMake の登録に対応しません"
                )
        elif first in registrations.ctest_names:
            documented_ctest_names.add(first)
        else:
            problems.append(
                f"表の行 `{first}` は CMake に登録されていません"
            )

    for (target, source), origin in sorted(registrations.gtest_pairs.items()):
        if (target, source) not in documented_pairs:
            problems.append(
                f"{origin} の `{target}` / `{source}` が表にありません"
            )

    for name, origin in sorted(registrations.ctest_names.items()):
        if name not in documented_ctest_names:
            problems.append(f"{origin} の CTest `{name}` が表にありません")

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

    repo_root = arguments.repo_root.resolve()
    document_path = repo_root / INVENTORY_RELATIVE_PATH

    registrations = collect_registrations(repo_root)
    section = extract_inventory_section(document_path.read_text(encoding="utf-8"))
    rows = parse_inventory_rows(section)
    problems = compare(registrations, rows)

    if problems:
        print(
            f"{INVENTORY_RELATIVE_PATH} の「{INVENTORY_HEADING}」と"
            " CMake の登録が一致しません:",
            file=sys.stderr,
        )
        for problem in problems:
            print(f"  - {problem}", file=sys.stderr)
        print(
            "\n表を CMake の登録へ合わせてください"
            "（テストを追加・削除した PR は同じ PR で表を更新します）。",
            file=sys.stderr,
        )
        return 1

    print("現存テスト一覧と CMake の登録は一致しています。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
