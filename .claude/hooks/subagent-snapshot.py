#!/usr/bin/env python3
"""SubagentStart / SubagentStop hook for a repo's specialist agents.

A parent that delegates review or gate work hands the specialist a snapshot
(base, review head) and asks for a report that states what it checked and what
it could not. This hook takes the mechanical part of both off the parent:

- SubagentStart: inject the branch, HEAD, the merge-base with the base ref
  (`origin/main` unless `--base-ref` says otherwise) and whether the working
  tree is dirty as `additionalContext`, so the agent can check the snapshot the
  parent wrote against the actual checkout. Any git failure produces no output:
  the parent's prompt stays the source of truth.
- SubagentStop: only when `--return-format-doc` names the repo's return format
  (for example `--return-format-doc 'docs/agent-orchestration.md「統一返却形式」'`).
  When the final text clearly lacks the snapshot or the "not covered" part, ask
  once for them through `hookSpecificOutput.additionalContext` (non-error
  feedback). It never uses `decision: "block"`, and it passes when
  `stop_hook_active` is true, when the final text is empty or short (structured
  output via a Workflow schema and SubagentHandback leave the report outside
  `last_assistant_message`), and for any input it cannot read. Without the
  option the Stop event is a no-op, so a repo without a written return format
  gets no false prompts.

The origin is `dolquis/agent-ops`; `scripts/vendor-docs-governance.sh` copies
it into product repos, so a product repo edits neither this file nor its test.
The repo wires it in `.claude/settings.json` (`hooks.SubagentStart` /
`hooks.SubagentStop` with a matcher listing its own specialist agents, so
built-in agents and Claude Code's internal agents never reach it) and passes
the options on that command line. `scripts/tests/test_subagent_snapshot.py`
pins the behaviour.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys


MIN_REPORT_LENGTH = 400
SNAPSHOT_MARKERS = ("Base", "merge-base", "snapshot", "Review head", "commit SHA")
NOT_COVERED_MARKERS = ("確認できなかった", "実行しなかった", "実行していない", "未確認", "未実行", "notCovered")


def parse_options(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--base-ref", default="origin/main")
    parser.add_argument("--return-format-doc", default="")
    return parser.parse_known_args(argv)[0]


def git(cwd: str, *arguments: str) -> str | None:
    try:
        result = subprocess.run(
            ["git", *arguments], cwd=cwd or None, capture_output=True, text=True, timeout=5, check=False
        )
    except (OSError, subprocess.SubprocessError):
        return None
    if result.returncode != 0:
        return None
    return result.stdout.strip()


def start_context(cwd: str, base_ref: str) -> str | None:
    head = git(cwd, "rev-parse", "HEAD")
    if not head:
        return None
    branch = git(cwd, "branch", "--show-current") or "(detached)"
    base = git(cwd, "merge-base", "HEAD", base_ref) or f"({base_ref} が無い)"
    status = git(cwd, "status", "--porcelain")
    dirty = "あり" if status else ("なし" if status == "" else "(取得できない)")
    return (
        "実行時点の checkout（hook が付与）: "
        f"branch {branch}、HEAD {head}、{base_ref} との merge-base {base}、未 commit の変更 {dirty}。"
        "親の prompt の Base / Review head と食い違うときは、prompt を優先して食い違いを返却に書く。"
    )


def stop_feedback(message: str, return_format_doc: str) -> str | None:
    if len(message) < MIN_REPORT_LENGTH:
        return None
    missing = []
    if not any(marker in message for marker in SNAPSHOT_MARKERS):
        missing.append("対象 snapshot（Base、Review head）")
    if not any(marker in message for marker in NOT_COVERED_MARKERS):
        missing.append("確認できなかった範囲と理由")
    if not missing:
        return None
    return (
        "返却に " + "、".join(missing) + " が見当たりません。"
        f"{return_format_doc} に従い、その節だけを追記して終えてください。"
    )


def emit(event: str, context: str) -> None:
    json.dump({"hookSpecificOutput": {"hookEventName": event, "additionalContext": context}}, sys.stdout,
              ensure_ascii=False)


def main(argv: list[str]) -> int:
    options = parse_options(argv)
    try:
        payload = json.load(sys.stdin)
    except (json.JSONDecodeError, ValueError):
        return 0
    if not isinstance(payload, dict):
        return 0
    event = payload.get("hook_event_name")
    if event == "SubagentStart":
        context = start_context(str(payload.get("cwd") or ""), options.base_ref)
        if context:
            emit(event, context)
        return 0
    if event == "SubagentStop":
        if not options.return_format_doc or payload.get("stop_hook_active"):
            return 0
        message = payload.get("last_assistant_message")
        if not isinstance(message, str):
            return 0
        feedback = stop_feedback(message, options.return_format_doc)
        if feedback:
            emit(event, feedback)
        return 0
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
