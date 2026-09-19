#!/usr/bin/env python3
"""PreToolUse hook: keep a read-only subagent's Bash tool read-only.

A read-only agent definition (`.claude/agents/*.md`) can drop Edit and Write
through `disallowedTools`, but Bash stays a general shell. The Codex twin of the
same agent runs under `sandbox_mode = "read-only"`, which the OS enforces. This
hook is the Claude Code side of that pair: it is declared in the agent's
frontmatter `hooks.PreToolUse` (matcher `Bash`) and blocks the commands that
turn a review into a write.

What it blocks (exit 2, reason on stderr so the agent sees why):

- `git` subcommands that change the index, history, refs or working tree
  (add, commit, push, pull, checkout, switch, restore, reset, stash, rebase,
  merge, cherry-pick, revert, clean, am, apply, mv, rm, worktree, submodule,
  tag, notes, update-ref, symbolic-ref, replace, filter-branch, gc, prune,
  reflog, branch create/delete/rename, remote add/remove/set-url, config writes).
  `git -C <dir>`, `git -c k=v`, `--git-dir=`/`--work-tree=` are skipped first,
  and `git.exe` counts as `git`.
- Output redirection (`>`, `>>`, `&>`, `>&file`) to anything but /dev/null.
  Quoted text such as `rg ">>" ipc/` or `echo 'a > b'` is not a redirection.
- Programs that write the filesystem: tee, rm, mv, cp, mkdir, touch, chmod,
  chown, ln, install, dd, truncate, rmdir, `find -delete`, `xargs`.
- In-place editors: `sed -i` in any option bundle (`-Ei`, `-ni`, `-i.bak`,
  `--in-place`), `perl -i` / `-pi`.
- Build tools that write build directories: `cmake --build/--preset/-B/-S/
  --install`, `cmake -E <verb>` other than read-only verbs, ninja, ctest,
  msbuild. Those belong to `windows-build-runner`.
- Wrappers are unwrapped before the check: env assignments, `env`, `command`,
  `exec`, `nice`, `time`, `timeout <n>`, `nohup`, `sudo`. A `bash -c` /
  `sh -c` / `zsh -c` string and `$(...)` substitutions are checked recursively.

What it cannot see: writes hidden in an interpreter (`python3 -c
"open(...,'w')"`, `powershell -Command Set-Content`), or a script the agent
runs. The hook raises the cost of a slip; it is not a sandbox. On the Codex side
the sandbox is. `docs/handoff/agent-orchestration.md` states that asymmetry.

Everything else exits 0. A parse failure also exits 0: the hook errs on the side
of not blocking, because a blocked read costs a review round and CI is still the
canonical gate. `python3` must be on PATH; Claude Code treats a hook it cannot
start as non-blocking, so a missing interpreter silently removes the guard.
`scripts/check_agent_definitions.py` verifies that every read-only agent
declares this hook; `scripts/tests/test_agent_readonly_guard.py` pins the
allow / deny table.

The hook reads the documented PreToolUse stdin JSON (`tool_name`,
`tool_input.command`).
"""

from __future__ import annotations

import json
import re
import shlex
import sys


GIT_WRITE_SUBCOMMANDS = {
    "add", "am", "apply", "cherry-pick", "checkout", "clean", "commit",
    "filter-branch", "gc", "merge", "mv", "notes", "prune", "pull", "push",
    "rebase", "reflog", "replace", "reset", "restore", "revert", "rm", "stash",
    "submodule", "switch", "symbolic-ref", "tag", "update-ref", "worktree",
}
# Subcommands above whose listing forms are still read-only.
GIT_READ_FORMS = {
    "stash": {"list", "show"},
    "worktree": {"list"},
    "submodule": {"status", "summary"},
    "tag": {"-l", "--list", "-n", "--contains", "--points-at", "--merged", "--no-merged"},
    "notes": {"list", "show"},
    "reflog": {"show"},
    "remote": {"-v", "show", "get-url"},
}
GIT_BRANCH_WRITE_FLAGS = {"-d", "-D", "-m", "-M", "-c", "-C", "--delete", "--move", "--copy",
                          "--set-upstream-to", "-u", "--unset-upstream", "--edit-description"}
GIT_BRANCH_READ_FLAGS = {"--list", "-l", "-a", "-r", "-v", "-vv", "--verbose", "--contains",
                         "--no-contains", "--merged", "--no-merged", "--points-at", "--show-current",
                         "--format", "--sort", "--all", "--remotes"}
GIT_REMOTE_WRITE_VERBS = {"add", "remove", "rm", "rename", "set-url", "prune", "set-head", "set-branches"}
GIT_GLOBAL_OPTIONS_WITH_VALUE = {"-C", "-c", "--git-dir", "--work-tree", "--namespace", "--exec-path"}
WRITE_PROGRAMS = {"chmod", "chown", "cp", "dd", "install", "ln", "mkdir", "mv", "rm", "rmdir",
                  "tee", "touch", "truncate", "xargs"}
BUILD_PROGRAMS = {"cmake", "ninja", "ctest", "msbuild"}
CMAKE_WRITE_FLAGS = {"--build", "--preset", "--install", "--open", "--fresh", "-B", "-S", "-P"}
CMAKE_E_READ_VERBS = {"capabilities", "cat", "compare_files", "echo", "echo_append", "environment",
                      "false", "md5sum", "sha1sum", "sha224sum", "sha256sum", "sha384sum",
                      "sha512sum", "sleep", "time", "true"}
TRANSPARENT_WRAPPERS = {"env", "command", "exec", "nice", "time", "nohup", "sudo"}
WRAPPERS_WITH_ARGUMENT = {"timeout"}
SHELLS = {"bash", "sh", "zsh", "dash"}
NULL_SINKS = {"/dev/null", "nul", "NUL"}
SEPARATORS = {";", "&&", "||", "|", "&", "|&"}
SUBSTITUTION_PATTERN = re.compile(r"\$\(([^()]*)\)")
ENV_ASSIGNMENT_PATTERN = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*=")


def tokenize(command: str) -> list[str]:
    """Split on shell punctuation while keeping quoted text intact (quotes kept)."""
    lexer = shlex.shlex(command, posix=False, punctuation_chars=True)
    lexer.whitespace_split = True
    try:
        return list(lexer)
    except ValueError:
        return []


def unquote(token: str) -> str:
    if len(token) >= 2 and token[0] == token[-1] and token[0] in "\"'":
        return token[1:-1]
    return token


def split_commands(tokens: list[str]) -> list[list[str]]:
    commands: list[list[str]] = []
    current: list[str] = []
    for token in tokens:
        if token in SEPARATORS:
            if current:
                commands.append(current)
            current = []
        else:
            current.append(token)
    if current:
        commands.append(current)
    return commands


def redirect_reason(words: list[str]) -> str | None:
    """Detect redirection operators as their own tokens (quotes already separate them)."""
    for index, token in enumerate(words):
        following = words[index + 1] if index + 1 < len(words) else ""
        if token in {">", ">>", "&>", "&>>"}:
            if unquote(following) in NULL_SINKS:
                continue
            return f"出力リダイレクト `{token} {following}` はファイルを書く"
        if token == ">&":
            if re.fullmatch(r"\d+", following) or following == "-":
                continue  # 2>&1 style descriptor duplication
            if unquote(following) in NULL_SINKS:
                continue
            return f"出力リダイレクト `>& {following}` はファイルを書く"
    return None


def unwrap(words: list[str]) -> list[str]:
    """Strip env assignments and transparent wrappers in front of the real program."""
    index = 0
    while index < len(words):
        word = unquote(words[index])
        if ENV_ASSIGNMENT_PATTERN.match(word):
            index += 1
            continue
        program = program_name(word)
        if program in TRANSPARENT_WRAPPERS:
            index += 1
            continue
        if program in WRAPPERS_WITH_ARGUMENT:
            index += 1
            while index < len(words) and unquote(words[index]).startswith("-"):
                index += 1
            index += 1  # the duration / argument itself
            continue
        break
    return [unquote(word) for word in words[index:]]


def program_name(word: str) -> str:
    name = word.rsplit("/", 1)[-1].rsplit("\\", 1)[-1]
    if name.lower().endswith(".exe"):
        name = name[:-4]
    return name.lower()


def git_reason(words: list[str]) -> str | None:
    rest = words[1:]
    index = 0
    while index < len(rest) and rest[index].startswith("-"):
        option = rest[index]
        if option in GIT_GLOBAL_OPTIONS_WITH_VALUE:
            index += 2
        else:
            index += 1
    args = rest[index:]
    if not args:
        return None
    subcommand = args[0]
    tail = args[1:]
    if subcommand in GIT_WRITE_SUBCOMMANDS:
        read_forms = GIT_READ_FORMS.get(subcommand)
        if read_forms and tail and tail[0] in read_forms:
            return None
        if subcommand == "tag" and tail and all(word.startswith("-") for word in tail) and set(tail) <= read_forms:
            return None
        return f"git {subcommand} は working tree / index / 参照を変える"
    if subcommand == "branch":
        if any(word in GIT_BRANCH_WRITE_FLAGS for word in tail):
            return "git branch の作成・削除・rename"
        positional = [word for word in tail if not word.startswith("-")]
        if positional and not any(word in GIT_BRANCH_READ_FLAGS or word.startswith("--format") or word.startswith("--sort") for word in tail):
            return "git branch <name> は branch を作る"
        return None
    if subcommand == "remote":
        if tail and tail[0] in GIT_REMOTE_WRITE_VERBS:
            return f"git remote {tail[0]} は remote 設定を変える"
        return None
    if subcommand == "config":
        if any(word in {"--edit", "-e", "--unset", "--unset-all", "--add", "--replace-all", "--rename-section", "--remove-section"} for word in tail):
            return "git config の書き込み"
        positional = [word for word in tail if not word.startswith("-")]
        reading = any(word in {"--get", "--get-all", "--get-regexp", "-l", "--list", "--show-origin", "--show-scope"} for word in tail)
        if len(positional) >= 2 and not reading:
            return "git config の書き込み"
        return None
    return None


def command_reason(words: list[str]) -> list[str]:
    words = unwrap(words)
    if not words:
        return []
    program = program_name(words[0])
    if program == "git":
        reason = git_reason(words)
        return [reason] if reason else []
    if program in SHELLS:
        for index, word in enumerate(words[1:], start=1):
            if word in {"-c", "-lc", "-ec", "-euc"} and index + 1 < len(words):
                return reasons(words[index + 1])
        return []
    if program in WRITE_PROGRAMS:
        return [f"{program} はファイルシステムを変える"]
    if program == "find" and any(word in {"-delete", "-exec", "-execdir", "-ok"} for word in words[1:]):
        return ["find -delete / -exec はファイルを変えうる"]
    if program == "sed" and any(
        word == "--in-place" or word.startswith("--in-place=") or (word.startswith("-") and not word.startswith("--") and "i" in word[1:])
        for word in words[1:]
    ):
        return ["sed -i はファイルを書き換える"]
    if program == "perl" and any(word.startswith("-") and not word.startswith("--") and "i" in word[1:] for word in words[1:]):
        return ["perl -i はファイルを書き換える"]
    if program in BUILD_PROGRAMS:
        if program == "cmake":
            options = words[1:]
            if "-E" in options:
                verb = options[options.index("-E") + 1] if options.index("-E") + 1 < len(options) else ""
                if verb in CMAKE_E_READ_VERBS:
                    return []
                return [f"cmake -E {verb} はファイルを変える"]
            if not any(word in CMAKE_WRITE_FLAGS or word.startswith("--preset=") for word in options):
                return []
        return [f"{program} は build directory へ書く（windows-build-runner の担当）"]
    return []


def reasons(command: str) -> list[str]:
    found: list[str] = []
    for words in split_commands(tokenize(command)):
        redirect = redirect_reason(words)
        if redirect:
            found.append(redirect)
        found.extend(command_reason(words))
    for match in SUBSTITUTION_PATTERN.finditer(command):
        found.extend(reasons(match.group(1)))
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
        + "; ".join(dict.fromkeys(found))
        + "。書き込みが必要なら結論として親へ返してください。\n"
    )
    return 2


if __name__ == "__main__":
    sys.exit(main())
