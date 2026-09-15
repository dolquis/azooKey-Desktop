#!/usr/bin/env python3
"""Codex PostToolUse adapter for the vendored docs-lint hook.

Codex reports file edits as ``tool_name: "apply_patch"`` with the whole patch in
``tool_input.command``; the vendored hook at .claude/hooks/post-edit-docs-lint.py
expects Claude Code's ``tool_input.file_path``. This adapter extracts the edited
paths from the patch and forwards one synthesized payload per Markdown file to the
vendored hook, so both harnesses run the same checker without a repo-side fork.

Never blocks: every failure path exits 0 and prints nothing. Findings reach Codex
through the vendored hook's hookSpecificOutput.additionalContext, which has the same
shape for Codex and Claude Code. This file is repo-owned; the vendored hook is not.
"""

import json
import os
import re
import subprocess
import sys

VENDORED = os.path.join(".claude", "hooks", "post-edit-docs-lint.py")
PATH_LINE = re.compile(r"^\*\*\* (?:Add|Update) File: (.+?)\s*$", re.MULTILINE)
WATCHED_SUFFIXES = (".md",)
WATCHED_NAMES = (".docs-lint.toml",)
TIMEOUT_SECONDS = 40


def edited_paths(payload):
    tool_input = payload.get("tool_input") or {}
    if not isinstance(tool_input, dict):
        return []
    direct = tool_input.get("file_path")
    if direct:
        return [str(direct)]
    command = tool_input.get("command")
    if isinstance(command, list):
        command = " ".join(str(part) for part in command)
    if not isinstance(command, str):
        return []
    return PATH_LINE.findall(command)


def main():
    try:
        payload = json.load(sys.stdin)
    except (ValueError, OSError):
        return 0
    if not isinstance(payload, dict):
        return 0

    root = payload.get("cwd") or os.getcwd()
    vendored = os.path.join(root, VENDORED)
    if not os.path.exists(vendored):
        return 0

    for path in edited_paths(payload):
        name = os.path.basename(path.replace("\\", "/"))
        if not (name.endswith(WATCHED_SUFFIXES) or name in WATCHED_NAMES):
            continue
        synthesized = json.dumps({
            "hook_event_name": "PostToolUse",
            "tool_name": "Edit",
            "tool_input": {"file_path": path},
            "cwd": root,
        })
        env = dict(os.environ, CLAUDE_PROJECT_DIR=root)
        try:
            done = subprocess.run([sys.executable, vendored], input=synthesized.encode(),
                                  cwd=root, env=env, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, timeout=TIMEOUT_SECONDS)
        except (OSError, subprocess.SubprocessError):
            return 0
        out = done.stdout.decode("utf-8", "replace").strip()
        if out:
            # docs-lint reads the whole repo, so one run already covers every edited
            # file; forward the first report and stop.
            sys.stdout.write(out)
            return 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
