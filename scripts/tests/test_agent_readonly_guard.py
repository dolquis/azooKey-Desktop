#!/usr/bin/env python3

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import unittest


sys.dont_write_bytecode = True
HOOK = Path(__file__).resolve().parents[2] / ".claude" / "hooks" / "agent-readonly-guard.py"

ALLOWED = [
    "git status -sb",
    "git diff --stat origin/main",
    "git log --oneline -5",
    "git show HEAD:docs/README.md",
    "git merge-base HEAD origin/main",
    "git branch --show-current",
    "git remote -v",
    "git config --get user.name",
    "rg -n 'MessageType' ipc/include",
    "python3 scripts/docs-lint.py --baseline .docs-lint-baseline.json",
    "python3 scripts/check_test_inventory.py 2>&1",
    "cmake --version",
    "ls build/ > /dev/null",
    "sed -n 1,20p docs/README.md",
    "cat docs/README.md | head -20",
    "echo 'a > b'",
]

BLOCKED = [
    "git add -A",
    "git commit -m msg",
    "git push -u origin feature",
    "git checkout -- .",
    "git switch main",
    "git restore docs/README.md",
    "git reset --hard",
    "git stash",
    "git rebase main",
    "git merge origin/main",
    "git branch -D feature",
    "git branch feature",
    "git remote set-url origin https://example.invalid/x.git",
    "git config user.name x",
    "rg foo > out.txt",
    "echo x >> notes.md",
    "git status && git add .",
    "cat a | tee b",
    "sed -i 's/a/b/' docs/README.md",
    "rm -rf build",
    "mv a b",
    "touch marker",
    "cmake --build --preset windows-debug",
    "cmake --preset windows-debug",
    "ninja -C build/windows-debug",
    "ctest --preset linux-debug",
    "AZOOKEY_LOG=1 git commit -m x",
]


def run(command: str, tool_name: str = "Bash") -> subprocess.CompletedProcess:
    payload = json.dumps({"tool_name": tool_name, "tool_input": {"command": command}})
    return subprocess.run(
        [sys.executable, str(HOOK)], input=payload, text=True, capture_output=True, check=False
    )


class AgentReadonlyGuardTest(unittest.TestCase):
    def test_read_only_commands_pass(self) -> None:
        for command in ALLOWED:
            with self.subTest(command=command):
                result = run(command)
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_write_commands_are_blocked_with_reason(self) -> None:
        for command in BLOCKED:
            with self.subTest(command=command):
                result = run(command)
                self.assertEqual(result.returncode, 2, command)
                self.assertIn("read-only agent", result.stderr)

    def test_other_tools_and_bad_input_pass_through(self) -> None:
        self.assertEqual(run("git push", tool_name="Read").returncode, 0)
        result = subprocess.run(
            [sys.executable, str(HOOK)], input="not json", text=True, capture_output=True, check=False
        )
        self.assertEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
