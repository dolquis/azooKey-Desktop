#!/usr/bin/env python3

from __future__ import annotations

from contextlib import redirect_stderr, redirect_stdout
import importlib.util
import io
from pathlib import Path, PurePosixPath, PureWindowsPath
import sys
import tempfile
import unittest


sys.dont_write_bytecode = True
SCRIPT = Path(__file__).resolve().parents[1] / "check_test_inventory.py"
SPEC = importlib.util.spec_from_file_location("check_test_inventory", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)

REPO_ROOT = SCRIPT.parents[1]

ROOT_CMAKE = """\
add_subdirectory(core/tests)

function(add_tsf_tip_unit_test target_name test_source)
  add_executable(${target_name} src/TextService.cpp ${test_source})
  azookey_discover_tests(${target_name})
endfunction()
"""

CORE_CMAKE = """\
# add_test(NAME commented_out COMMAND true)
add_executable(core_tests
  romaji_kana_converter_test.cpp
  utf8_test.cpp
)
target_link_libraries(core_tests PRIVATE azookey_core)
azookey_discover_tests(core_tests)

add_executable(support_lib_helper Helper.cpp)

add_test(
  NAME core_cli_smoke
  COMMAND core_cli --help
)
"""

TSF_CMAKE = """\
add_tsf_tip_unit_test(tsf_tip_staleness_tests tests/staleness_test.cpp)
"""

INVENTORY_ROWS = [
    "| `core_tests` | `core/tests/romaji_kana_converter_test.cpp` | ローマ字かな変換 |",
    "| `core_tests` | `core/tests/utf8_test.cpp` | UTF-8 境界 |",
    "| `tsf_tip_staleness_tests` | `tsf-tip/tests/staleness_test.cpp` | 応答の陳腐化判定 |",
    "| `core_cli_smoke` | `core_cli` | CLI の起動 |",
]


def build_document(rows: list[str]) -> str:
    return "\n".join(
        [
            "# テスト一覧",
            "",
            "## 現存テスト一覧",
            "",
            "| ターゲット | テスト | 主要シナリオ |",
            "|---|---|---|",
            *rows,
            "",
            "## CTest 以外の自動検査",
            "",
            "| `core_tests` | `core/tests/never_parsed_test.cpp` | 別表なので無視される |",
            "",
        ]
    )


class FixtureRepository:
    def __init__(self, directory: Path, rows: list[str]) -> None:
        self.root = directory
        (directory / "core" / "tests").mkdir(parents=True)
        (directory / "tsf-tip").mkdir(parents=True)
        (directory / "docs").mkdir(parents=True)
        (directory / "build" / "generated").mkdir(parents=True)
        (directory / "CMakeLists.txt").write_text(ROOT_CMAKE, encoding="utf-8")
        (directory / "core" / "tests" / "CMakeLists.txt").write_text(
            CORE_CMAKE, encoding="utf-8"
        )
        (directory / "tsf-tip" / "CMakeLists.txt").write_text(
            TSF_CMAKE, encoding="utf-8"
        )
        # A stale copy under build/ must not be collected.
        (directory / "build" / "generated" / "CMakeLists.txt").write_text(
            "add_test(NAME stale_generated_test COMMAND true)\n", encoding="utf-8"
        )
        # A linked worktree of the same repository: its `.git` is a file.
        worktree = directory / ".claude" / "worktrees" / "other"
        worktree.mkdir(parents=True)
        (worktree / ".git").write_text("gitdir: /elsewhere\n", encoding="utf-8")
        (worktree / "CMakeLists.txt").write_text(
            "add_test(NAME worktree_leak COMMAND true)\n", encoding="utf-8"
        )
        # The same, outside a dot-directory: only the `.git` file stops it.
        linked = directory / "linked" / "other"
        linked.mkdir(parents=True)
        (linked / ".git").write_text("gitdir: /elsewhere\n", encoding="utf-8")
        (linked / "CMakeLists.txt").write_text(
            "add_test(NAME linked_worktree_leak COMMAND true)\n", encoding="utf-8"
        )
        # A nested checkout outside any dot-directory: its `.git` is a directory.
        nested = directory / "vendor" / "nested"
        (nested / ".git").mkdir(parents=True)
        (nested / "CMakeLists.txt").write_text(
            "add_test(NAME nested_checkout_leak COMMAND true)\n", encoding="utf-8"
        )
        (directory / "docs" / "test-inventory.md").write_text(
            build_document(rows), encoding="utf-8"
        )


def run_main(rows: list[str]) -> tuple[int, str]:
    with tempfile.TemporaryDirectory() as temporary_directory:
        FixtureRepository(Path(temporary_directory), rows)
        stdout, stderr = io.StringIO(), io.StringIO()
        with redirect_stdout(stdout), redirect_stderr(stderr):
            status = MODULE.main(["--repo-root", temporary_directory])
        return status, stdout.getvalue() + stderr.getvalue()


class CollectionTests(unittest.TestCase):
    def collect(self) -> MODULE.Registrations:
        with tempfile.TemporaryDirectory() as temporary_directory:
            FixtureRepository(Path(temporary_directory), INVENTORY_ROWS)
            return MODULE.collect_registrations(Path(temporary_directory))

    def test_collects_gtest_pairs_from_add_executable(self) -> None:
        registrations = self.collect()
        self.assertIn(
            ("core_tests", "core/tests/romaji_kana_converter_test.cpp"),
            registrations.gtest_pairs,
        )
        self.assertIn(
            ("core_tests", "core/tests/utf8_test.cpp"), registrations.gtest_pairs
        )

    def test_collects_gtest_pair_from_the_tsf_tip_wrapper(self) -> None:
        # The wrapper's source is relative to the calling CMakeLists, not to the
        # file that defines the function.
        registrations = self.collect()
        self.assertIn(
            ("tsf_tip_staleness_tests", "tsf-tip/tests/staleness_test.cpp"),
            registrations.gtest_pairs,
        )

    def test_ignores_wrapper_body_placeholders_and_non_test_targets(self) -> None:
        registrations = self.collect()
        self.assertNotIn("${target_name}", registrations.targets)
        self.assertNotIn("support_lib_helper", registrations.targets)

    def test_collects_multiline_add_test_and_skips_comments_and_build(self) -> None:
        registrations = self.collect()
        self.assertIn("core_cli_smoke", registrations.ctest_names)
        self.assertNotIn("commented_out", registrations.ctest_names)
        self.assertNotIn("stale_generated_test", registrations.ctest_names)

    def test_skips_worktrees_and_nested_checkouts(self) -> None:
        # Another checkout's CMake must not be attributed to this one.
        registrations = self.collect()
        self.assertNotIn("worktree_leak", registrations.ctest_names)
        self.assertNotIn("linked_worktree_leak", registrations.ctest_names)
        self.assertNotIn("nested_checkout_leak", registrations.ctest_names)


class PathTests(unittest.TestCase):
    def test_windows_paths_become_slash_separated(self) -> None:
        # A Windows checkout must produce the same logical paths as a POSIX
        # one; backslashes would never match the table's slash-separated cells.
        root = PureWindowsPath(r"C:\src\azooKey-Desktop")
        self.assertEqual(
            MODULE.repo_relative_posix(root / "core" / "tests" / "CMakeLists.txt", root),
            "core/tests/CMakeLists.txt",
        )
        self.assertEqual(
            MODULE.repo_relative_posix(root / "core" / "tests", root), "core/tests"
        )
        self.assertEqual(MODULE.repo_relative_posix(root, root), ".")

    def test_posix_paths_are_unchanged(self) -> None:
        root = PurePosixPath("/src/azooKey-Desktop")
        self.assertEqual(
            MODULE.repo_relative_posix(root / "core" / "tests" / "CMakeLists.txt", root),
            "core/tests/CMakeLists.txt",
        )

    def test_resolve_source_accepts_the_repository_root_directory(self) -> None:
        self.assertEqual(
            MODULE.resolve_source(".", "tests/root_test.cpp"), "tests/root_test.cpp"
        )


class ComparisonTests(unittest.TestCase):
    def test_matching_inventory_succeeds(self) -> None:
        status, output = run_main(INVENTORY_ROWS)
        self.assertEqual(status, 0, output)

    def test_row_with_the_wrong_directory_fails(self) -> None:
        # A source moved between directories, or a mistyped directory in the
        # table, must not pass just because the file name still matches.
        rows = [
            row.replace("`core/tests/utf8_test.cpp`", "`totally/wrong/utf8_test.cpp`")
            for row in INVENTORY_ROWS
        ]
        status, output = run_main(rows)
        self.assertEqual(status, 1)
        self.assertIn("totally/wrong/utf8_test.cpp", output)

    def test_removed_row_fails(self) -> None:
        rows = [row for row in INVENTORY_ROWS if "utf8_test.cpp" not in row]
        status, output = run_main(rows)
        self.assertEqual(status, 1)
        self.assertIn("utf8_test.cpp", output)

    def test_removed_ctest_row_fails(self) -> None:
        rows = [row for row in INVENTORY_ROWS if "core_cli_smoke" not in row]
        status, output = run_main(rows)
        self.assertEqual(status, 1)
        self.assertIn("core_cli_smoke", output)

    def test_phantom_target_row_fails(self) -> None:
        rows = INVENTORY_ROWS + [
            "| `ghost_tests` | `core/tests/ghost_test.cpp` | 実在しない |"
        ]
        status, output = run_main(rows)
        self.assertEqual(status, 1)
        self.assertIn("ghost_tests", output)

    def test_row_with_the_wrong_source_fails(self) -> None:
        rows = [
            row.replace("utf8_test.cpp", "utf16_test.cpp") for row in INVENTORY_ROWS
        ]
        status, output = run_main(rows)
        self.assertEqual(status, 1)
        self.assertIn("utf16_test.cpp", output)


class RepositoryTests(unittest.TestCase):
    def test_the_committed_document_matches_the_committed_cmake(self) -> None:
        stdout = io.StringIO()
        with redirect_stdout(stdout):
            status = MODULE.main(["--repo-root", str(REPO_ROOT)])
        self.assertEqual(status, 0, stdout.getvalue())


if __name__ == "__main__":
    unittest.main()
