#!/usr/bin/env python3
"""PreToolUse hook: let `git push` to a dolquis/ remote run without a prompt.

`.claude/settings.json` registers this hook on `Bash` in place of a blanket
`Bash(git push:*)` ask rule. The approved policy is: a push whose destination is
verified to be a `dolquis/` remote may run without human approval. The hook
answers with the documented PreToolUse JSON (`hookSpecificOutput.
permissionDecision`) and always exits 0.

- No `git ... push` text in the command: print nothing. The normal permission
  flow applies.
- `allow` only for one plain `git [-C <dir>] push ...` where all of these hold:
  - every push URL of the destination remote starts with
    `https://github.com/dolquis/` or `git@github.com:dolquis/`. The remote is the
    positional argument (a remote name or a URL), or else git's own order
    `branch.<b>.pushRemote` > `remote.pushDefault` > `branch.<b>.remote`.
    Names are checked through `git remote get-url --push --all`.
  - no destination ref is `main` / `master` (`HEAD:main`, `refs/heads/main`,
    `heads/main`, bare `main`, and a refspec-less push from or tracking main
    included).
  - no `--mirror`, `--all`, `--branches`, `--delete`, `-d`, `--prune`, or
    `:<ref>` deletion.
- `deny` when one of the conditions above is known to be false.
- `ask` (back to the human) for everything the hook cannot vouch for: compound
  commands, newlines, subshells, `$(...)` / backticks, `bash -c` and other
  interpreters, quotes or backslashes anywhere in the push (the shell joins
  `HEAD:"main"` into one word), env assignments or `git -c` / `--git-dir` in
  front of push, force pushes, `--no-verify`, tags, globs, unknown options, a
  detached HEAD, an unresolvable remote, a `remote.<r>.push` mapping that may
  rewrite a `<src>`-only refspec, a literal URL while `url.*.insteadOf` /
  `pushInsteadOf` is configured, a failing git lookup, or an internal error.

A non-dolquis destination (the `azooKey/` upstream included) is denied, so such
a push has to be run by the human outside the agent. Quoted mentions in a
non-interpreter (`rg "git push" docs/`) pass through silently. A git alias that
expands to push is not detected; normal permissions still apply to it.
`scripts/tests/test_git_push_guard.py` pins the allow / deny / ask table.
"""

from __future__ import annotations

import json
import re
import shlex
import subprocess
import sys
from typing import Callable, Optional

ALLOWED_URL_PREFIXES = ("https://github.com/dolquis/", "git@github.com:dolquis/")
PROTECTED_BRANCHES = {"main", "master"}
PUSH_TEXT = re.compile(r"\bgit(?:\.exe)?\b.*\bpush\b", re.DOTALL)
SEPARATORS = {";", "&&", "||", "|", "&", "|&"}
REDIRECTIONS = {">", ">>", ">&", "&>", "&>>", "<", ">|"}
INTERPRETERS = {"bash", "sh", "zsh", "dash", "pwsh", "powershell", "cmd", "wsl", "xargs",
                "python", "python3", "py", "node", "perl", "ruby", "eval", "source", "."}
SAFE_LONG_OPTIONS = {"--verbose", "--quiet", "--set-upstream", "--dry-run", "--porcelain",
                     "--progress", "--no-progress", "--atomic", "--no-atomic", "--verify",
                     "--thin", "--no-thin", "--ipv4", "--ipv6"}
SAFE_SHORT_FLAGS = set("vqun46")
DENY_LONG_OPTIONS = {"--mirror", "--all", "--branches", "--delete", "--prune"}

Decision = tuple
RunGit = Callable[[list], Optional[str]]


def tokenize(command: str) -> list[str] | None:
    lexer = shlex.shlex(command, posix=False, punctuation_chars=True)
    lexer.whitespace_split = True
    try:
        return list(lexer)
    except ValueError:
        return None


def unquote(token: str) -> str:
    if len(token) >= 2 and token[0] == token[-1] and token[0] in "\"'":
        return token[1:-1]
    return token


def program_name(word: str) -> str:
    name = word.rsplit("/", 1)[-1].rsplit("\\", 1)[-1].lower()
    return name[:-4] if name.endswith(".exe") else name


def split_commands(tokens: list[str]) -> list[list[str]]:
    commands: list[list[str]] = [[]]
    for token in tokens:
        if token in SEPARATORS:
            commands.append([])
        else:
            commands[-1].append(token)
    return [words for words in commands if words]


def bare(word: str) -> str:
    """The word as the shell would join it, ignoring quotes and backslashes."""
    return word.replace('"', "").replace("'", "").replace("\\", "")


def looks_like_push(text: str) -> bool:
    return bool(PUSH_TEXT.search(text) or PUSH_TEXT.search(bare(text)))


def mentions_push(words: list[str]) -> bool:
    """True when a `git` word is later followed by a `push` word (quotes and escapes ignored)."""
    seen_git = False
    for word in words:
        if program_name(unquote(word)) == "git" or program_name(bare(word)) == "git":
            seen_git = True
        elif seen_git and bare(word) == "push":
            return True
    return False


def strip_redirections(words: list[str]) -> list[str]:
    """Drop `>file`, `2>&1` style redirections; they are not git arguments."""
    kept: list[str] = []
    skip = False
    for word in words:
        if skip:
            skip = False
            continue
        if word in REDIRECTIONS:
            if kept and kept[-1].isdigit():
                kept.pop()
            skip = True
            continue
        kept.append(word)
    return kept


def push_words(command: str) -> tuple[list[str] | None, Decision | None]:
    """Return (words of the single plain push, None) or (None, early decision or None)."""
    if any(mark in command for mark in ("\n", "\r", "`", "$(", "<<")):
        return None, ("ask", "改行・コマンド置換・ヒアドキュメントを含む push は解析しない")
    tokens = tokenize(command)
    if tokens is None or any(token in {"(", ")", "{", "}"} for token in tokens):
        return None, ("ask", "サブシェルまたは解析できない引用を含む push は解析しない")
    commands = split_commands(tokens)
    pushes = []
    for words in commands:
        program = program_name(unquote(words[0]))
        if {program, program_name(bare(words[0]))} & INTERPRETERS and looks_like_push(" ".join(words)):
            return None, ("ask", f"{program} 経由の push は解析しない")
        if mentions_push(words):
            pushes.append(words)
    if not pushes:
        return None, None
    if len(commands) > 1:
        return None, ("ask", "複合コマンドの中の push は解析しない")
    words = strip_redirections(pushes[0])
    if any(char in word for word in words for char in "\"'\\"):
        return None, ("ask", "引用符・エスケープを含む push は解析しない（連結後の語を確定できない）")
    return words, None


def git_prefix(words: list[str]) -> tuple[list[str], list[str]] | Decision:
    """Split `git [-C dir] push ARGS` into (-C options, ARGS), or return an ask decision."""
    if program_name(unquote(words[0])) != "git":
        return ("ask", f"`{words[0]}` を前置した push は解析しない（環境変数・ラッパー）")
    index = 1
    location: list[str] = []
    while index < len(words) and words[index] != "push":
        if words[index] == "-C" and index + 1 < len(words):
            location += ["-C", unquote(words[index + 1])]
            index += 2
            continue
        return ("ask", f"git の大域オプション `{words[index]}` 付きの push は解析しない")
    if index >= len(words):
        return ("ask", "push サブコマンドを特定できない")
    return location, [unquote(word) for word in words[index + 1:]]


def classify_options(args: list[str]) -> tuple[list[str], Decision | None]:
    """Return (positional arguments, decision forced by an option)."""
    positional: list[str] = []
    asks: list[str] = []
    deny: Decision | None = None
    options_done = False
    for arg in args:
        if options_done or not arg.startswith("-") or arg == "-":
            positional.append(arg)
            continue
        if arg == "--":
            options_done = True
        elif arg.startswith("--"):
            name = arg.split("=", 1)[0]
            if name in DENY_LONG_OPTIONS:
                deny = deny or ("deny", f"`{name}` は複数 ref の一括更新・削除になる")
            elif name not in SAFE_LONG_OPTIONS:
                asks.append(name)
        elif "d" in arg[1:]:
            deny = deny or ("deny", "`-d` は remote の ref を削除する")
        elif not set(arg[1:]) <= SAFE_SHORT_FLAGS:
            asks.append(arg)
    if deny:
        return positional, deny
    if asks:
        return positional, ("ask", f"想定外のオプション {', '.join(asks)}（force・tag・hook 回避などは人間が判断する）")
    return positional, None


def short_branch(ref: str) -> str:
    return ref[len("refs/heads/"):] if ref.startswith("refs/heads/") else ref


def refspec_decision(refspec: str, current: Callable[[], Optional[str]]) -> Decision | None:
    if refspec.startswith(":"):
        return ("deny", f"`{refspec}` は remote の ref を削除する")
    if refspec.startswith("+") or "*" in refspec:
        return ("ask", f"`{refspec}` は force または glob の refspec")
    source, _, destination = refspec.partition(":")
    if not destination:
        destination = source
        if source in {"HEAD", "@"}:
            branch = current()
            if branch is None:
                return ("ask", "HEAD が branch を指していない")
            destination = branch
    if destination.startswith("refs/") and not destination.startswith("refs/heads/"):
        return ("ask", f"`{destination}` は branch 以外の ref")
    if destination.startswith(("tags/", "remotes/")):
        return ("ask", f"`{destination}` は git の短縮解決で branch 以外の ref になりうる")
    if destination.startswith("heads/"):
        destination = "refs/" + destination  # git resolves `heads/x` as refs/heads/x
    if short_branch(destination) in PROTECTED_BRANCHES:
        return ("deny", f"push 先 `{short_branch(destination)}` は保護 branch（PR 経由にする）")
    return None


def implicit_destination_decision(branch: str, remote: str, git: RunGit) -> Decision | None:
    """Refspec-less push: the destination comes from push.default and the upstream."""
    if branch in PROTECTED_BRANCHES:
        return ("deny", f"現在の branch `{branch}` をそのまま push すると保護 branch へ入る")
    merge = git(["config", "--get", f"branch.{branch}.merge"])
    if merge and short_branch(merge) in PROTECTED_BRANCHES:
        return ("deny", f"`{branch}` の upstream が `{short_branch(merge)}` のため、refspec を明示する")
    if merge and short_branch(merge) != branch:
        return ("ask", f"`{branch}` の upstream `{merge}` が branch 名と異なる")
    if git(["config", "--get-all", f"remote.{remote}.push"]):
        return ("ask", f"`remote.{remote}.push` が設定されている")
    if (git(["config", "--get", "push.default"]) or "simple") not in {"simple", "current", "upstream"}:
        return ("ask", "push.default が simple / current / upstream ではない")
    return None


def resolve_remote(branch: str | None, git: RunGit) -> str | None:
    if branch is None:
        return None
    for key in (f"branch.{branch}.pushRemote", "remote.pushDefault", f"branch.{branch}.remote"):
        value = git(["config", "--get", key])
        if value:
            return value
    return None


def is_url(remote: str) -> bool:
    return "://" in remote or ":" in remote or "/" in remote


def destination_urls(remote: str, git: RunGit) -> list[str] | None:
    if is_url(remote):
        # git rewrites a literal URL through url.<base>.insteadOf / pushInsteadOf.
        if git(["config", "--get-regexp", r"^url\..*insteadof$"]):
            return None
        return [remote]
    output = git(["remote", "get-url", "--push", "--all", remote])
    if not output:
        return None
    return [line.strip() for line in output.splitlines() if line.strip()]


def url_decision(urls: list[str], target: str) -> Decision | None:
    for url in urls:
        if not url.startswith(ALLOWED_URL_PREFIXES):
            return ("deny", f"`{target}` の push URL `{url}` は dolquis/ 配下ではない")
        if ".." in url or "\\" in url or any(char.isspace() for char in url):
            return ("ask", f"push URL `{url}` の形を確認できない")
    return None


def decide(command: str, git: RunGit) -> Decision | None:
    """Return None (no opinion) or (permissionDecision, reason).

    `git(args)` runs git with `args` and returns stripped stdout, or None when
    the command fails or prints nothing.
    """
    if not looks_like_push(command):
        return None
    words, early = push_words(command)
    if words is None:
        return early
    split = git_prefix(words)
    if isinstance(split[0], str):
        return split
    location, args = split

    def run(arguments: list[str]) -> Optional[str]:
        return git(location + arguments)

    cache: dict = {}

    def current() -> Optional[str]:
        if "branch" not in cache:
            cache["branch"] = run(["symbolic-ref", "--quiet", "--short", "HEAD"])
        return cache["branch"]

    positional, forced = classify_options(args)
    if forced and forced[0] == "deny":
        return forced
    ask = forced
    for refspec in positional[1:]:
        decision = refspec_decision(refspec, current)
        if decision and decision[0] == "deny":
            return decision
        ask = ask or decision
    if ask:
        return ask

    remote = positional[0] if positional else resolve_remote(current(), run)
    if not remote:
        return ("ask", "push 先 remote を解決できない（detached HEAD または remote 未設定）")
    urls = destination_urls(remote, run)
    if not urls:
        return ("ask", f"remote `{remote}` の push URL を取得できない")
    decision = url_decision(urls, remote)
    if decision:
        return decision
    if any(":" not in refspec for refspec in positional[1:]) and not is_url(remote) \
            and run(["config", "--get-all", f"remote.{remote}.push"]):
        return ("ask", f"`remote.{remote}.push` が宛先を書き換えうる（`<src>:<dst>` で明示する）")
    if len(positional) <= 1:
        branch = current()
        if branch is None:
            return ("ask", "HEAD が branch を指していない")
        decision = implicit_destination_decision(branch, remote, run)
        if decision:
            return decision
    return ("allow", f"push 先 `{remote}` は dolquis/ 配下で、保護 branch と削除を含まない")


def make_git(cwd: str | None) -> RunGit:
    def git(arguments: list) -> Optional[str]:
        try:
            result = subprocess.run(
                ["git", *arguments], cwd=cwd or None, capture_output=True, text=True,
                encoding="utf-8", errors="replace", timeout=5, check=False,
            )
        except (OSError, subprocess.SubprocessError):
            return None
        if result.returncode != 0:
            return None
        return result.stdout.strip() or None
    return git


def emit(decision: str, reason: str) -> None:
    sys.stdout.write(json.dumps({
        "hookSpecificOutput": {
            "hookEventName": "PreToolUse",
            "permissionDecision": decision,
            "permissionDecisionReason": f"git-push-guard: {reason}",
        }
    }, ensure_ascii=False) + "\n")


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
    cwd = payload.get("cwd") if isinstance(payload.get("cwd"), str) else None
    try:
        result = decide(command, make_git(cwd))
    except Exception as error:  # noqa: BLE001 - any failure must fall back to a human
        if looks_like_push(command):
            emit("ask", f"検査中の内部エラー（{type(error).__name__}）")
        return 0
    if result:
        emit(*result)
    return 0


if __name__ == "__main__":
    for stream in (sys.stdin, sys.stdout):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8")
    sys.exit(main())
