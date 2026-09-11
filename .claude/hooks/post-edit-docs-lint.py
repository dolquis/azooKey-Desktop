#!/usr/bin/env python3
"""PostToolUse hook: run the docs governance checkers after a document edit.

docs-lint and the AGENTS.md budget checker are CI gates, so a regression found
at PR time costs a round trip. This hook runs the same checkers right after
Claude Code writes a Markdown file, so the finding lands next to the edit that
caused it.

It runs *ahead of* the CI gate, not instead of it. CI stays the canonical gate
and is the one that also covers Codex CLI, which has no PostToolUse equivalent.

Never blocks: the edit has already been applied when a PostToolUse hook runs, and
every failure path here exits 0. Findings reach Claude through the documented
hookSpecificOutput.additionalContext field rather than a non-zero status, so a
checker that is absent, broken or slow degrades to silence instead of noise.

Written in Python, not shell: python3 is already required to run the checkers,
and emitting the JSON contract correctly matters more than matching the shell
hooks next to it.

agent-ops is the origin. Each repo carries a copy distributed by
scripts/vendor-docs-governance.sh; a repo-side edit is a silent fork, which
`scripts/vendor-docs-governance.sh --check` reports.
"""

import json
import os
import subprocess
import sys

# docs-lint reads the whole repo in well under a second, so the match below only
# decides *whether* to run, not what to inspect. That is why it stays a blunt
# "any Markdown file" test instead of trying to reproduce docs-lint's prose
# roots, which .docs-lint.toml can redefine per repo. Over-triggering costs half
# a second; under-triggering silently drops the check this hook exists for.
WATCHED_SUFFIXES = (".md",)
WATCHED_NAMES = (".docs-lint.toml",)

LINT = "scripts/docs-lint.py"
BUDGET = "scripts/check_agent_instruction_size.py"
BASELINE = ".docs-lint-baseline.json"
TIMEOUT_SECONDS = 60
MAX_MESSAGE_CHARS = 2000


def edited_path(payload):
    for container, key in (("tool_input", "file_path"),
                           ("tool_use_result", "filePath"),
                           ("tool_response", "filePath")):
        value = (payload.get(container) or {})
        if isinstance(value, dict) and value.get(key):
            return str(value[key])
    return ""


def run(root, args):
    """Return (ok, output). A checker that cannot run counts as ok: this hook
    reports documents, not a broken toolchain."""
    try:
        done = subprocess.run([sys.executable] + args, cwd=root,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              timeout=TIMEOUT_SECONDS)
    except (OSError, subprocess.SubprocessError):
        return True, ""
    return done.returncode == 0, done.stdout.decode("utf-8", "replace")


def main():
    try:
        payload = json.load(sys.stdin)
    except (ValueError, OSError):
        return 0
    if not isinstance(payload, dict):
        return 0

    path = edited_path(payload)
    if not path:
        return 0

    root = os.environ.get("CLAUDE_PROJECT_DIR") or payload.get("cwd") or os.getcwd()
    # On Windows the tool reports a native path while the hook may run under Git
    # Bash. Compare both sides with one separator so the containment test below
    # does not quietly reject every edit there.
    path = path.replace("\\", "/")
    root = os.path.abspath(root.replace("\\", "/"))
    if not os.path.isabs(path):
        path = os.path.join(root, path)
    path = os.path.abspath(path)

    rel = os.path.relpath(path, root).replace(os.sep, "/")
    if rel.startswith("../"):
        return 0
    if not (rel.endswith(WATCHED_SUFFIXES) or os.path.basename(rel) in WATCHED_NAMES):
        return 0
    if not os.path.exists(os.path.join(root, LINT)):
        return 0

    notes = []

    # A repo with a baseline gates on the increase, which is the same verdict its
    # CI job reaches. A repo without one has nothing frozen, so fall back to the
    # DECISIVE tier only: HEURISTIC is over-detection by construction, and a repo
    # that carries a standing pile of it would see the same irrelevant list after
    # every Markdown edit. The origin's CI still requires zero of both tiers.
    if os.path.exists(os.path.join(root, BASELINE)):
        lint_args = [LINT, "--baseline", BASELINE]
    else:
        lint_args = [LINT, "--strict", "decisive"]
    ok, out = run(root, lint_args)
    if not ok:
        notes.append("docs-lint が指摘を報告した（%s）:\n%s"
                     % (" ".join(lint_args[1:]), out.strip()))

    # The budget checker measures the root AGENTS.md only, so run it for that
    # file. Exit 0 covers both ok and the over-target warning; the warning is
    # worth surfacing here because CI only annotates it.
    if rel == "AGENTS.md" and os.path.exists(os.path.join(root, BUDGET)):
        ok, out = run(root, [BUDGET])
        if not ok or "status=ok" not in out:
            notes.append("AGENTS.md の予算検査:\n%s" % out.strip())

    if not notes:
        return 0

    message = ("編集後の文書検査（.claude/hooks/post-edit-docs-lint.py）。"
               "この編集が原因なら今直す。無関係な既存の指摘なら直さず報告する。\n\n"
               + "\n\n".join(notes))
    if len(message) > MAX_MESSAGE_CHARS:
        message = message[:MAX_MESSAGE_CHARS] + "\n…（以降省略。全文は docs-lint を直接実行して確認する）"

    json.dump({"hookSpecificOutput": {"hookEventName": "PostToolUse",
                                      "additionalContext": message}}, sys.stdout,
              ensure_ascii=False)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
