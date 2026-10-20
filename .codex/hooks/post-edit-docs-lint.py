#!/usr/bin/env python3
"""Advisory Codex adapter for the unmodified, vendored docs-lint hook.

Patch paths are relative to payload.cwd, not necessarily the repository root.
Shell/unified-exec hooks use the canonical name Bash; inspect dirty documents
after those calls without interpreting their command text. One vendored run
covers the repository, prioritizing AGENTS.md for its extra budget check.

Successful/irrelevant invocations are quiet. Adapter failures produce a short
PostToolUse diagnostic without raw stderr, payloads, or exception text. Vendor
reports are reduced to counts and fixed labels, never forwarded verbatim. The
vendored hook still masks some internal checker failures, so an empty response
is not proof of a passed check and must never populate a success cache.
"""

import json
import os
from pathlib import Path
import re
import subprocess
import sys

VENDORED = Path(".claude/hooks/post-edit-docs-lint.py")
PATH_LINE = re.compile(r"^\*\*\* (?:(?:Add|Update|Delete) File|Move to): (.+?)\s*$",
                       re.MULTILINE)
WATCHED_SUFFIXES = (".md",)
WATCHED_NAMES = (".docs-lint.toml",)
TIMEOUT_SECONDS = 65  # Three vendored subprocesses can each take 20 seconds.
GIT_TIMEOUT_SECONDS = 5


def edited_paths(payload):
    tool_input = payload.get("tool_input") or {}
    if not isinstance(tool_input, dict):
        return []
    direct = tool_input.get("file_path")
    if isinstance(direct, str) and direct:
        return [direct]
    command = tool_input.get("command")
    if isinstance(command, list) and all(isinstance(part, str) for part in command):
        command = " ".join(command)
    return PATH_LINE.findall(command) if isinstance(command, str) else []


def watched(path):
    return path.name.endswith(WATCHED_SUFFIXES) or path.name in WATCHED_NAMES


def emit_context(message):
    json.dump({"hookSpecificOutput": {"hookEventName": "PostToolUse",
                                      "additionalContext": message}},
              sys.stdout, ensure_ascii=True)
    sys.stdout.write("\n")


def diagnostic(reason):
    # Reasons are fixed strings; never forward subprocess stderr/exception text.
    emit_context("docs-lint: 未検査（" + reason + "）。"
                 "編集は適用済み。文書検査を直接実行して確認してください。")
    return "unavailable"


def report_context(message):
    """The vendor merges checker stderr into its context: treat it as untrusted.

Only fixed checker labels and decimal counts leave this adapter. In particular,
paths, snippets, traceback text and exception messages must not be forwarded.
This is advisory reporting, not a claim that every checker completed.
"""
    if re.search(r"^(?:Traceback \(most recent call last\):|"
                 r"[\w.]+(?:Error|Exception):|error:|instruction-size: error)",
                 message, re.MULTILINE):
        return diagnostic("検査スクリプトが実行エラーを報告")
    notes = []
    for tier in ("DECISIVE", "HEURISTIC"):
        count = re.search(r"^=== " + tier + r": ([0-9]+) 件", message, re.MULTILINE)
        if count:
            notes.append(tier + ": " + count[1] + " 件")
    budget = re.search(r"^instruction-size: status=(warning|error) ", message, re.MULTILINE)
    if budget:
        notes.append("AGENTS.md の予算: " + budget[1])
    if not notes:
        return diagnostic("検査結果の形式を確認できない")
    emit_context("文書検査から指摘があります。" + " / ".join(notes)
                 + "。詳細と検査の完了は docs-lint / AGENTS.md 予算検査を直接実行して確認してください。")
    return "reported"


def dirty_documents(root):
    """Include staged/unstaged deletions, renames, and non-ignored new docs."""
    paths = set()
    for args in (["diff", "--name-only", "-z", "HEAD", "--", "*.md", ".docs-lint.toml"],
                 ["ls-files", "--others", "--exclude-standard", "-z", "--",
                  "*.md", ".docs-lint.toml"]):
        done = subprocess.run(["git", "-c", "core.fsmonitor=false", *args], cwd=root,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              timeout=GIT_TIMEOUT_SECONDS)
        if done.returncode != 0:
            raise OSError("document status unavailable")
        paths.update(os.fsdecode(path) for path in done.stdout.split(b"\0") if path)
    return paths


def run_hook(payload, root=None):
    root = (Path(root) if root is not None else Path(__file__).resolve().parents[2]).resolve()
    cwd = payload.get("cwd")
    if cwd is not None and (not isinstance(cwd, str) or not cwd):
        return diagnostic("作業ディレクトリが不正")
    cwd = Path(cwd) if cwd else Path.cwd()
    if not cwd.is_absolute():
        return diagnostic("作業ディレクトリが絶対パスではない")
    try:
        if payload.get("tool_name") == "Bash":
            paths = dirty_documents(root)
            path_base = root
        else:
            paths = edited_paths(payload)
            path_base = cwd
        selected = set()
        for name in paths:
            path = Path(name.replace("\\", "/"))
            path = (path if path.is_absolute() else path_base / path).resolve()
            if path.is_relative_to(root) and watched(path):
                selected.add(path)
    except (OSError, ValueError, subprocess.SubprocessError):
        return diagnostic("編集対象を確認できない")
    if not selected:
        return "skipped"

    vendored = root / VENDORED
    required = [vendored, root / "scripts/docs-lint.py"]
    agents = root / "AGENTS.md"
    if agents in selected:
        required.append(root / "scripts/check_agent_instruction_size.py")
    if any(not path.is_file() for path in required):
        return diagnostic("必要なhookまたは検査スクリプトがない")
    path = agents if agents in selected else sorted(selected)[0]
    synthesized = json.dumps({"hook_event_name": "PostToolUse", "tool_name": "Edit",
                              "tool_input": {"file_path": str(path)}, "cwd": str(root)})
    env = dict(os.environ, CLAUDE_PROJECT_DIR=str(root), PYTHONIOENCODING="utf-8")
    try:
        done = subprocess.run([sys.executable, "-B", str(vendored)],
                              input=synthesized.encode("utf-8"), cwd=root, env=env,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              timeout=TIMEOUT_SECONDS)
    except subprocess.TimeoutExpired:
        return diagnostic("hookがタイムアウト")
    except (OSError, subprocess.SubprocessError):
        return diagnostic("hookを実行できない")
    if done.returncode != 0:
        return diagnostic("hookが異常終了")
    if done.stderr.strip():
        return diagnostic("hookが標準エラーを出力")
    out = done.stdout.decode("utf-8", "replace").strip()
    if out:
        try:
            report = json.loads(out)
            specific = report["hookSpecificOutput"]
            if specific["hookEventName"] != "PostToolUse":
                raise ValueError("unexpected hook event")
            message = specific["additionalContext"]
            if not isinstance(message, str):
                raise ValueError("invalid message")
        except (ValueError, TypeError, KeyError):
            return diagnostic("hookの応答形式が不正")
        return report_context(message)
    return "completed"


def main():
    try:
        # Hook transport is UTF-8 even when the Windows console uses CP932.
        raw = getattr(sys.stdin, "buffer", sys.stdin).read()
        payload = json.loads(raw.decode("utf-8") if isinstance(raw, bytes) else raw)
    except (ValueError, OSError):
        diagnostic("hook入力を読み取れない")
        return 0
    if not isinstance(payload, dict):
        diagnostic("hook入力の形式が不正")
        return 0
    run_hook(payload)
    return 0


if __name__ == "__main__":
    sys.exit(main())
