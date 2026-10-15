#!/usr/bin/env python3
"""PreToolUse hook: let an ordinary feature-branch `git push` to a dolquis/ remote run
without a prompt, stop known violations, and stay silent about everything else.

agent-ops is the origin; `scripts/vendor-docs-governance.sh` vendors this file into each
product repo, whose `.claude/settings.json` registers it on `PreToolUse` with matcher
`Bash`. The answer is the documented PreToolUse JSON (`hookSpecificOutput.
permissionDecision`), and the process always exits 0.

The common, correct push should need neither a human prompt nor a judgement call from
the auto mode classifier. A hook `ask` forces a prompt even in auto mode, so the hook
asks only for pushes that look deliberately hidden or rewrite remote history, and leaves
anything else it cannot vouch for to the normal permission flow.

- `allow`: every push in the command line goes to a remote whose push URLs all start
  with `https://github.com/dolquis/` or `git@github.com:dolquis/`, no destination is
  `main` / `master`, nothing is deleted, mirrored or force-pushed, and every other step
  in the line is a known local step: `cd <dir>`, `git add` / `commit` / `status` / `log`
  / `diff` / `show` / `fetch` / `rev-parse`, `echo`, `true`, `pwd`, or a read-only
  filter (`tail`, `head`, `cat`, `grep`, `wc`). Heredoc bodies (a commit message passed
  through `$(cat <<'EOF' ... EOF)`) are not searched, so a message that mentions a push
  does not matter. `cd <dir>` before the push is applied as `git -C <dir>`.
- `deny`: a push known to target `main` / `master` (`HEAD:main`, `refs/heads/main`,
  `heads/main`, a refspec-less push from or tracking main), a remote outside dolquis/
  (the upstream included), `--mirror` / `--all` / `--branches` / `--delete` / `-d` /
  `--prune`, or a `:<ref>` deletion. Any one denied push denies the whole line.
- `ask`: a force push (`-f`, `--force`, `--force-with-lease`, `--force-if-includes`,
  `+<ref>`), a tag or other non-branch destination, a destination that git may remap
  (`remote.<r>.push`, an upstream with another name, `push.default=upstream`), a `#`
  anywhere in the line (bash comments start only at a word boundary and `#` is legal
  in ref names), or a push hidden from analysis: quotes, escapes or `$` inside the push words
  (the shell joins `HEAD:"main"` into one word), a push inside `$(...)`, backticks,
  `bash -c`, `eval` or another interpreter or wrapper, or git global options other than
  `-C` in front of push.
- no opinion (prints nothing): everything else, such as an unknown option, a remote that
  cannot be resolved, a detached HEAD, a path-qualified program, or a line with a step
  the hook does not vet (including git steps whose options run programs or write files). The settings deny / ask / allow rules decide, then a human prompt or the auto
  mode classifier.

A git alias that expands to push is not detected. `scripts/tests/test_git_push_guard.py`
pins the allow / deny / ask / no-opinion table.
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
HEREDOC = re.compile(
    r"<<-?[ \t]*(['\"]?)([A-Za-z_][A-Za-z0-9_]*)\1([^\n]*)\n.*?\n[ \t]*\2[ \t]*(?=\n|$)",
    re.DOTALL,
)
SEPARATORS = {";", "&&", "||", "|", "&", "|&", "\n"}
REDIRECTIONS = {">", ">>", ">&", "&>", "&>>", "<", ">|"}
HARMLESS_TARGETS = {"/dev/null", "1", "2"}
WRAPPERS = {"bash", "sh", "zsh", "dash", "pwsh", "powershell", "cmd", "wsl", "xargs",
            "python", "python3", "py", "node", "perl", "ruby", "eval", "source", ".",
            "env", "timeout", "nohup", "nice", "sudo", "command", "exec", "time"}
SAFE_GIT_STEPS = {"add", "commit", "status", "log", "diff", "show", "fetch", "rev-parse"}
SAFE_PROGRAMS = {"echo", "true", "pwd", "tail", "head", "cat", "grep", "wc"}
# Options that make a "read-only" git step run programs or write files.
UNSAFE_OPTION_MARKS = ("upload-pack", "receive-pack", "exec", "output", "config", "ext-diff", "textconv")
SAFE_LONG_OPTIONS = {"--verbose", "--quiet", "--set-upstream", "--dry-run", "--porcelain",
                     "--progress", "--no-progress", "--atomic", "--no-atomic", "--verify",
                     "--thin", "--no-thin", "--ipv4", "--ipv6"}
SAFE_SHORT_FLAGS = set("vqun46")
DENY_LONG_OPTIONS = {"--mirror", "--all", "--branches", "--delete", "--prune"}
FORCE_LONG_OPTIONS = {"--force", "--force-with-lease", "--force-if-includes"}
TAG_LONG_OPTIONS = {"--tags", "--follow-tags"}

# A decision is (kind, reason). kind is "allow", "deny", "ask", or "pass" (no opinion).
Decision = tuple
RunGit = Callable[[list], Optional[str]]


def program_name(word: str) -> str:
    """Lower-cased command name; a word with a path separator is never a known name."""
    if "/" in word or "\\" in word:
        return "<path>" + word
    name = word.lower()
    return name[:-4] if name.endswith(".exe") else name


def unquote(token: str) -> str:
    if len(token) >= 2 and token[0] == token[-1] and token[0] in "\"'":
        return token[1:-1]
    return token


def bare(word: str) -> str:
    """The word as the shell would join it, ignoring quotes and backslashes."""
    return word.replace('"', "").replace("'", "").replace("\\", "")


def looks_like_push(text: str) -> bool:
    return bool(PUSH_TEXT.search(text) or PUSH_TEXT.search(bare(text)))


def strip_heredocs(command: str) -> str:
    """Drop heredoc bodies, keeping the rest of the line that opened each one."""
    return HEREDOC.sub(lambda match: "__HEREDOC__" + match.group(3), command)


def substitutions(command: str) -> tuple[str, bool] | None:
    """Replace `$(...)` and backtick substitutions with a placeholder word.

    Returns (rewritten command, whether any substitution mentions a push), or None
    when a substitution is not closed.
    """
    out: list[str] = []
    hides_push = False
    index = 0
    while index < len(command):
        if command.startswith("$(", index):
            depth, end = 1, index + 2
            while end < len(command) and depth:
                depth += {"(": 1, ")": -1}.get(command[end], 0)
                end += 1
            if depth:
                return None
            hides_push = hides_push or looks_like_push(command[index + 2:end - 1])
            out.append("__SUBST__")
            index = end
        elif command[index] == "`":
            end = command.find("`", index + 1)
            if end < 0:
                return None
            hides_push = hides_push or looks_like_push(command[index + 1:end])
            out.append("__SUBST__")
            index = end + 1
        else:
            out.append(command[index])
            index += 1
    return "".join(out), hides_push


def tokenize(command: str) -> list[str] | None:
    lexer = shlex.shlex(command, posix=False, punctuation_chars="();<>|&\n")
    lexer.whitespace = " \t\r"
    lexer.whitespace_split = True
    lexer.commenters = ""  # bash comments start only at a word boundary; handled in decide()
    try:
        return list(lexer)
    except ValueError:
        return None


def split_commands(tokens: list[str]) -> list[list[str]]:
    commands: list[list[str]] = [[]]
    for token in tokens:
        if token in SEPARATORS:
            commands.append([])
        else:
            commands[-1].append(token)
    return [words for words in commands if words]


def strip_redirections(words: list[str]) -> tuple[list[str], bool]:
    """Drop `2>&1`, `>/dev/null`, `> file` style redirections.

    Returns (remaining words, whether one of them writes a regular file).
    """
    kept: list[str] = []
    writes_file = False
    skip = False
    for word in words:
        if skip:
            skip = False
            if unquote(word) not in HARMLESS_TARGETS:
                writes_file = True
            continue
        if word in REDIRECTIONS:
            if kept and kept[-1].isdigit():
                kept.pop()
            skip = True
            continue
        kept.append(word)
    return kept, writes_file


def git_subcommand(words: list[str]) -> tuple[list[str], int] | None:
    """For `git [-C dir]... <sub> ...` return (-C options, index of <sub>).

    Returns None when the words do not run git, and ([], -1) when git global
    options other than `-C` precede the subcommand.
    """
    if program_name(unquote(words[0])) != "git":
        return None
    index, location = 1, []
    while index < len(words) and words[index].startswith("-"):
        if words[index] == "-C" and index + 1 < len(words):
            location += ["-C", unquote(words[index + 1])]
            index += 2
            continue
        return [], -1
    return (location, index) if index < len(words) else None


def is_push_command(words: list[str]) -> bool:
    """True when the words run `git ... push`, however the words are quoted.

    `git log --grep push` and `git grep push` are not pushes: the first word after
    git that is neither an option nor the value of `-C` / `-c` is the subcommand.
    """
    git_seen = False
    previous = ""
    for word in words:
        if not git_seen:
            if program_name(unquote(word)) == "git" or program_name(bare(word)) == "git":
                git_seen = True
        elif bare(word) == "push":
            return True
        elif not word.startswith("-") and previous not in {"-C", "-c"}:
            return False
        previous = word
    return False


def cd_target(words: list[str]) -> str | None:
    if program_name(words[0]) != "cd" or len(words) != 2:
        return None
    target = unquote(words[1])
    if not target or any(char in target for char in "$`\"'~"):
        return None
    return target


def safe_step(words: list[str]) -> bool:
    words, writes_file = strip_redirections(words)
    if writes_file or not words:
        return False
    if program_name(unquote(words[0])) in SAFE_PROGRAMS:
        return True
    split = git_subcommand(words)
    if not split or split[1] < 0:
        return False
    subcommand = words[split[1]]
    if subcommand not in SAFE_GIT_STEPS:
        return False
    for word in words[split[1] + 1:]:
        option = unquote(word)
        if any(mark in option for mark in UNSAFE_OPTION_MARKS) or option in {"-c", "-o"}:
            return False
        if subcommand == "commit" and (option == "--no-verify"
                                       or (option[:1] == "-" and option[1:2] != "-" and "n" in option[1:])):
            return False
    return True


def classify_options(args: list[str]) -> tuple[list[str], Decision | None]:
    """Return (positional arguments, decision forced by an option)."""
    positional: list[str] = []
    unknown: list[str] = []
    force: list[str] = []
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
            elif name in FORCE_LONG_OPTIONS or name in TAG_LONG_OPTIONS:
                force.append(name)
            elif name not in SAFE_LONG_OPTIONS:
                unknown.append(name)
        elif "d" in arg[1:]:
            deny = deny or ("deny", "`-d` は remote の ref を削除する")
        elif "f" in arg[1:]:
            force.append(arg)
        elif not set(arg[1:]) <= SAFE_SHORT_FLAGS:
            unknown.append(arg)
    if deny:
        return positional, deny
    if force:
        return positional, ("ask", f"force / tag の push（{', '.join(force)}）は人間が判断する")
    if unknown:
        return positional, ("pass", f"判定しないオプション {', '.join(unknown)}")
    return positional, None


def short_branch(ref: str) -> str:
    return ref[len("refs/heads/"):] if ref.startswith("refs/heads/") else ref


def refspec_decision(refspec: str, current: Callable[[], Optional[str]]) -> Decision | None:
    if refspec.startswith(":"):
        return ("deny", f"`{refspec}` は remote の ref を削除する")
    source, _, destination = refspec.lstrip("+").partition(":")
    if not destination:
        destination = source
        if source in {"HEAD", "@"}:
            branch = current()
            if branch is None:
                return ("pass", "HEAD が branch を指していない")
            destination = branch
    if destination.startswith("heads/"):
        destination = "refs/" + destination  # git resolves `heads/x` as refs/heads/x
    if short_branch(destination) in PROTECTED_BRANCHES:
        return ("deny", f"push 先 `{short_branch(destination)}` は保護 branch（PR 経由にする）")
    if refspec.startswith("+"):
        return ("ask", f"`{refspec}` は force push の refspec")
    if ":" in refspec and destination in {"HEAD", "@"}:
        return ("ask", f"`{refspec}` は remote に `HEAD` という名前の ref を作る")
    if "*" in refspec:
        return ("ask", f"`{refspec}` は glob の refspec")
    if destination.startswith("refs/") and not destination.startswith("refs/heads/"):
        return ("ask", f"`{destination}` は branch 以外の ref")
    if destination.startswith(("tags/", "remotes/")):
        return ("ask", f"`{destination}` は git の短縮解決で branch 以外の ref になりうる")
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


def upstream_mapping_decision(refspecs: list[str], git: RunGit) -> Decision | None:
    """With push.default=upstream, git maps a colonless refspec to its upstream branch."""
    if (git(["config", "--get", "push.default"]) or "simple") != "upstream":
        return None
    for refspec in refspecs:
        if ":" in refspec or refspec in {"HEAD", "@"}:
            continue
        source = short_branch(refspec.lstrip("+"))
        merge = git(["config", "--get", f"branch.{source}.merge"])
        if merge and short_branch(merge) in PROTECTED_BRANCHES:
            return ("deny", f"push.default=upstream で `{source}` が `{short_branch(merge)}` へ写像される")
        if merge and short_branch(merge) != source:
            return ("ask", f"push.default=upstream で `{source}` が `{merge}` へ写像される")
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


def stronger(current: Decision | None, new: Decision | None) -> Decision | None:
    """Keep the stronger of two non-allow decisions: deny > ask > pass."""
    rank = {"pass": 0, "ask": 1, "deny": 2}
    if new is None:
        return current
    if current is None or rank[new[0]] > rank[current[0]]:
        return new
    return current


def push_decision(words: list[str], location: list[str], git: RunGit) -> Decision:
    """Decide one `git [-C dir] push ...` command (redirections already removed)."""
    if any(char in word for word in words for char in "\"'\\$"):
        return ("ask", "引用符・エスケープ・変数を含む push は解析しない（連結後の語を確定できない）")
    split = git_subcommand(words)
    if split is None:
        return ("ask", f"`{words[0]}` を前置した push は解析しない（環境変数・ラッパー）")
    extra, index = split
    if index < 0:
        return ("ask", "git の大域オプション（-C 以外）付きの push は解析しない")
    location = location + extra
    args = words[index + 1:]

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
    undecided = forced
    for refspec in positional[1:]:
        decision = refspec_decision(refspec, current)
        if decision and decision[0] == "deny":
            return decision
        undecided = stronger(undecided, decision)

    remote = positional[0] if positional else resolve_remote(current(), run)
    if not remote:
        return undecided or ("pass", "push 先 remote を解決できない（detached HEAD または upstream 未設定）")
    urls = destination_urls(remote, run)
    if not urls:
        return undecided or ("pass", f"remote `{remote}` の push URL を取得できない")
    decision = url_decision(urls, remote)
    if decision:
        return decision
    if any(":" not in refspec for refspec in positional[1:]) and not is_url(remote) \
            and run(["config", "--get-all", f"remote.{remote}.push"]):
        return ("ask", f"`remote.{remote}.push` が宛先を書き換えうる（`<src>:<dst>` で明示する）")
    decision = upstream_mapping_decision(positional[1:], run)
    if decision and decision[0] == "deny":
        return decision
    undecided = stronger(undecided, decision)
    if len(positional) <= 1:
        branch = current()
        if branch is None:
            return undecided or ("pass", "HEAD が branch を指していない")
        decision = implicit_destination_decision(branch, remote, run)
        if decision and decision[0] == "deny":
            return decision
        undecided = stronger(undecided, decision)
    return undecided or ("allow", f"push 先 `{remote}` は dolquis/ 配下で、保護 branch・削除・force を含まない")


def decide(command: str, git: RunGit) -> Decision | None:
    """Return None (no opinion) or (permissionDecision, reason).

    `git(args)` runs git with `args` and returns stripped stdout, or None when the
    command fails or prints nothing.
    """
    if not looks_like_push(command):
        return None
    command = strip_heredocs(command)
    if not looks_like_push(command):
        return None
    rewritten = substitutions(command)
    if rewritten is None:
        return None
    command, hidden = rewritten
    if hidden:
        return ("ask", "コマンド置換の中の push は解析しない")
    tokens = tokenize(command)
    if tokens is None or any(token in {"(", ")", "{", "}"} for token in tokens):
        return None
    if any("#" in token and token[:1] not in "\"'" for token in tokens):
        return ("ask", "`#` を含む push は解析しない（bash のコメントと ref 名の区別がつかない）")
    location: list[str] = []
    decisions: list[Decision] = []
    unvetted = False
    for words in split_commands(tokens):
        program = program_name(unquote(words[0]))
        if {program, program_name(bare(words[0]))} & WRAPPERS and looks_like_push(" ".join(words)):
            return ("ask", f"{program} 経由の push は解析しない")
        if is_push_command(words):
            kept, writes_file = strip_redirections(words)
            decisions.append(push_decision(kept, location, git))
            unvetted = unvetted or writes_file
            continue
        target = cd_target(words)
        if target is not None:
            location = location + ["-C", target]
        elif not safe_step(words):
            unvetted = True
    for kind in ("deny", "ask"):
        for decision in decisions:
            if decision[0] == kind:
                return decision
    if not decisions or unvetted or any(decision[0] != "allow" for decision in decisions):
        return None
    return decisions[0]


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
    except Exception:  # noqa: BLE001 - an internal failure leaves the call to normal permissions
        return 0
    if result:
        emit(*result)
    return 0


if __name__ == "__main__":
    for stream in (sys.stdin, sys.stdout):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8")
    sys.exit(main())
