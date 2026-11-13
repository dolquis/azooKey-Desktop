#!/usr/bin/env python3
"""Pin the SubagentStart / SubagentStop behaviour of .claude/hooks/subagent-snapshot.py."""

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


sys.dont_write_bytecode = True
REPO_ROOT = Path(__file__).resolve().parents[2]
HOOK = REPO_ROOT / ".claude" / "hooks" / "subagent-snapshot.py"

FULL_REPORT = (
    "1. 対象 snapshot: Base abc123、Review head def456、Boundary ipc。\n"
    "2. Findings: 該当なし。ipc/ の差分を読んだ。\n"
    "3. 実行した command: git diff。\n"
    "4. 確認できなかった範囲: なし。\n"
    "5. 次の検証: ctest。\n"
) * 4


RETURN_DOC = "docs/agent-orchestration.md「統一返却形式」"


def run(payload, *options: str) -> subprocess.CompletedProcess[str]:
    text = payload if isinstance(payload, str) else json.dumps(payload, ensure_ascii=False)
    return subprocess.run(
        [sys.executable, str(HOOK), *options], input=text, text=True, encoding="utf-8", capture_output=True,
        check=False,
    )


def context_of(result: subprocess.CompletedProcess[str]) -> str:
    output = json.loads(result.stdout)
    return output["hookSpecificOutput"]["additionalContext"]


class SubagentStartTest(unittest.TestCase):
    def test_injects_checkout_snapshot(self) -> None:
        result = run({"hook_event_name": "SubagentStart", "agent_type": "diff-auditor", "cwd": str(REPO_ROOT)})
        self.assertEqual(result.returncode, 0)
        output = json.loads(result.stdout)
        self.assertEqual(output["hookSpecificOutput"]["hookEventName"], "SubagentStart")
        head = subprocess.run(
            ["git", "rev-parse", "HEAD"], cwd=REPO_ROOT, capture_output=True, text=True, check=True
        ).stdout.strip()
        self.assertIn(head, context_of(result))
        self.assertIn("origin/main との merge-base", context_of(result))

    def test_base_ref_option_names_the_ref(self) -> None:
        result = run(
            {"hook_event_name": "SubagentStart", "cwd": str(REPO_ROOT)}, "--base-ref", "refs/heads/no-such-ref"
        )
        self.assertEqual(result.returncode, 0)
        self.assertIn("(refs/heads/no-such-ref が無い)", context_of(result))

    def test_outside_a_repository_stays_silent(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            result = run({"hook_event_name": "SubagentStart", "cwd": directory})
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "")


class SubagentStopTest(unittest.TestCase):
    def stop(self, message, active: bool = False, *options: str) -> subprocess.CompletedProcess[str]:
        return run({
            "hook_event_name": "SubagentStop",
            "agent_type": "spec-drift-checker",
            "stop_hook_active": active,
            "last_assistant_message": message,
        }, *(options or ("--return-format-doc", RETURN_DOC)))

    def test_complete_report_passes(self) -> None:
        result = self.stop(FULL_REPORT)
        self.assertEqual((result.returncode, result.stdout), (0, ""))

    def test_missing_sections_get_feedback_not_block(self) -> None:
        result = self.stop("Findings: P2 ipc/src/Payloads.cpp の境界チェックが無い。" * 20)
        self.assertEqual(result.returncode, 0)
        output = json.loads(result.stdout)
        self.assertNotIn("decision", output)
        context = context_of(result)
        self.assertIn("対象 snapshot", context)
        self.assertIn("確認できなかった範囲", context)
        self.assertIn(RETURN_DOC, context)

    def test_without_return_format_doc_stop_is_a_noop(self) -> None:
        result = self.stop("Findings: P2 ipc/src/Payloads.cpp の境界チェックが無い。" * 20, False, "--base-ref", "origin/main")
        self.assertEqual((result.returncode, result.stdout), (0, ""))

    def test_short_or_empty_message_passes(self) -> None:
        for message in ("", "完了しました。", None):
            with self.subTest(message=message):
                result = self.stop(message)
                self.assertEqual((result.returncode, result.stdout), (0, ""))

    def test_second_round_passes(self) -> None:
        result = self.stop("Findings only. " * 60, active=True)
        self.assertEqual((result.returncode, result.stdout), (0, ""))


class BadInputTest(unittest.TestCase):
    def test_bad_input_passes_through(self) -> None:
        for payload in ("not json", "[]", {"hook_event_name": "PreToolUse"}):
            with self.subTest(payload=payload):
                result = run(payload)
                self.assertEqual((result.returncode, result.stdout), (0, ""))


if __name__ == "__main__":
    unittest.main()
