#!/usr/bin/env python3
"""PreToolUse hook: keep a read-only subagent's Bash tool read-only.

A read-only agent definition (`.claude/agents/*.md`) can drop Edit and Write
through `disallowedTools`, but Bash stays a general shell. The Codex twin of the
same agent runs under `sandbox_mode = "read-only"`, which the OS enforces. This
hook is the Claude Code side of that pair: it is declared in the agent's
frontmatter `hooks.PreToolUse` (matcher `Bash`) and blocks the commands that
turn a review into a write.

Blocked (exit 2, reason on stderr so the agent sees why):

- `git` subcommands that change the index, the history or the working tree
  (add, commit, push, checkout, switch, restore, reset, stash, rebase, merge,
  cherry-pick, revert, clean, branch -d/-D/-m, tag, am, apply, mv, rm, worktree,
  submodule, config --global/--system, remote add/set-url/remove).
- Output redirection (`>`, `>>`) to anything other than /dev/null.
- `tee`, `sed -i`, `perl -i`, `truncate`, `rm`, `mv`, `cp`, `mkdir`, `touch`,
  `chmod`, `ln`, `install`, `dd`, `cmake --build`, `cmake --preset`, `ninja`,
  `ctest` (build directories are the build runner's, not a reviewer's).

Everything else exits 0. A parse failure also exits 0: the hook errs on the side
of not blocking, because a blocked read costs a review round and CI is still the
canonical gate. `scripts/check_agent_definitions.py` verifies that every
read-only agent declares this hook; `scripts/tests/test_agent_readonly_guard.py`
pins the allow/deny table below.

The hook reads the documented PreToolUse stdin JSON (`tool_name`,
`tool_input.command`). It is Python because python3 is already required by the
docs checkers and Git Bash / WSL / native Windows all run it the same way.
"""

from __future__ import annotations

import json
import re
import shlex
import sys


GIT_WRITE_SUBCOMMANDS = {
    "add",
    "am",
    "apply",
    "cherry-pick",
    "checkout",
    "clean",
    "commit",
    "merge",
    "mv",
    "pull",
    "push",
    "rebase",
    "reset",
    "restore",
    "revert",
    "rm",
    "stash",
    "submodule",
    "switch",
    "tag",
    "worktree",
}
GIT_BRANCH_WRITE_FLAGS = {"-d", "-D", "-m", "-M", "-c", "-C", "--delete", "--move", "--copy"}
GIT_REMOTE_WRITE_VERBS = {"add", "remove", "rm", "rename", "set-url", "prune"}
WRITE_PROGRAMS = {
    "chmod",
    "chown",
    "cp",
    "dd",
    "install",
    "ln",
    "mkdir",
    "mv",
    "rm",
    "rmdir",
    "tee",
    "touch",
    "truncate",
}
BUILD_PROGRAMS = {"cmake", "ninja", "ctest", "msbuild", "MSBuild"}
INPLACE_EDITORS = {"sed", "perl"}
REDIRECT_TOKEN_PATTERN = re.compile(r"^\d?(>>|>)(?!&)(.*)$")
NULL_SINKS = {"/dev/null", "nul", "NUL"}


def split_commands(command: str) -> list[list[str]]:
    """Split a shell line into simple commands on ; && || | and newlines."""
    try:
        tokens = shlex.split(command, posix=True)
    except ValueError:
        return []
    commands: list[list[str]] = []
    current: list[str] = []
    for token in tokens:
        if token in {";", "&&", "||", "|", "&"}:
            if current:
                commands.append(current)
            current = []
        else:
            current.append(token)
    if current:
        commands.append(current)
    return commands


def strip_leading_env(words: list[str]) -> list[str]:
    index = 0
    while index < len(words) and re.match(r"^[A-Za-z_][A-Za-z0-9_]*=", words[index]):
        index += 1
    if index < len(words) and words[index] in {"env", "command", "exec", "nice", "time"}:
        index += 1
    return words[index:]


def git_reason(words: list[str]) -> str | None:
    args = [word for word in words[1:] if not word.startswith("-")]
    if not args:
        return None
    subcommand = args[0]
    if subcommand in GIT_WRITE_SUBCOMMANDS:
        return f"git {subcommand} は working tree / index / 履歴を変える"
    if subcommand == "branch" and any(word in GIT_BRANCH_WRITE_FLAGS for word in words):
        return "git branch の作成・削除・rename"
    if subcommand == "branch" and len(args) >= 2:
        return "git branch <name> は branch を作る"
    if subcommand == "remote" and len(args) >= 2 and args[1] in GIT_REMOTE_WRITE_VERBS:
        return f"git remote {args[1]} は remote 設定を変える"
    if subcommand == "config" and any(word in {"--global", "--system", "--unset", "--add"} for word in words):
        return "git config の書き込み"
    if subcommand == "config" and len(args) >= 3 and "--get" not in words and "-l" not in words and "--list" not in words:
        return "git config の書き込み"
    return None


def command_reason(words: list[str]) -> str | None:
    words = strip_leading_env(words)
    if not words:
        return None
    program = words[0].rsplit("/", 1)[-1]
    if program == "git":
        return git_reason(words)
    if program in WRITE_PROGRAMS:
        return f"{program} はファイルシステムを変える"
    if program in INPLACE_EDITORS and any(word == "-i" or word.startswith("-i.") or word.startswith("-pi") for word in words[1:]):
        return f"{program} -i はファイルを書き換える"
    if program in BUILD_PROGRAMS:
        if program == "cmake" and not any(word in {"--build", "--preset", "--install", "-B", "-S"} or word.startswith("--preset=") for word in words[1:]):
            return None
        return f"{program} は build directory へ書く（windows-build-runner の担当）"
    return None


def redirect_reason(words: list[str]) -> str | None:
    """Detect `>`/`>>` after shell quoting is resolved, so `echo 'a > b'` passes."""
    for index, token in enumerate(words):
        match = REDIRECT_TOKEN_PATTERN.match(token)
        if not match:
            continue
        target = match.group(2) or (words[index + 1] if index + 1 < len(words) else "")
        if target in NULL_SINKS:
            continue
        return f"出力リダイレクト `{match.group(1)} {target}` はファイルを書く"
    return None


def reasons(command: str) -> list[str]:
    found: list[str] = []
    for words in split_commands(command):
        redirect = redirect_reason(words)
        if redirect:
            found.append(redirect)
        reason = command_reason(words)
        if reason:
            found.append(reason)
    return found


def main() -> int:
    try:
        payload = json.load(sys.stdin)
    except (json.JSONDecodeError, ValueError):
        return 0
    if not isinstance(payload, dict) or payload.get("tool_name") != "Bash":
        return 0
    tool_input = payload.get("tool_input")
    command = tool_input.get("command") if isinstance(tool_input, dict) else None
    if not isinstance(command, str):
        return 0
    found = reasons(command)
    if not found:
        return 0
    sys.stderr.write(
        "read-only agent のため実行を止めました: "
        + "; ".join(found)
        + "。書き込みが必要なら結論として親へ返してください。\n"
    )
    return 2


if __name__ == "__main__":
    sys.exit(main())
