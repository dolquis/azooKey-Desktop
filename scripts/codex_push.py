#!/usr/bin/env python3
"""Check a narrowly scoped push; publish only for an already authorized request.

This is not a human-approval mechanism. The checkout, Git configuration, hooks,
and environment must be trusted; concurrent configuration changes are outside
this helper's guarantee. --dry-run contacts the remote; the default does not.
"""

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
DESTINATION = "dolquis/azooKey-Desktop"
BRANCH = re.compile(r"dolquis/dev-[0-9]+-[a-z0-9]+(?:-[a-z0-9]+)*", re.ASCII)
URLS = {
    f"{prefix}{DESTINATION}{suffix}"
    for prefix in ("https://github.com/", "git@github.com:")
    for suffix in ("", ".git")
}


class PushError(Exception):
    """A sanitized failure suitable for displaying to the caller."""


def git(*args: str, timeout: int = 30) -> str:
    env = os.environ.copy()
    # -C alone cannot override these repository-routing environment variables.
    for key in ("GIT_DIR", "GIT_WORK_TREE", "GIT_COMMON_DIR", "GIT_NAMESPACE"):
        env.pop(key, None)
    env["GIT_TERMINAL_PROMPT"] = "0"
    try:
        result = subprocess.run(
            ["git", "-C", str(ROOT), *args],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            shell=False,
            timeout=timeout,
            env=env,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise PushError("Git could not finish; no raw diagnostics are displayed.") from exc
    if result.returncode:
        raise PushError("Git failed; no raw diagnostics are displayed.")
    return result.stdout.rstrip("\r\n")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--branch", required=True)
    action = parser.add_mutually_exclusive_group()
    action.add_argument("--publish", action="store_true")
    action.add_argument("--dry-run", action="store_true")
    args = parser.parse_args(argv)
    try:
        if not BRANCH.fullmatch(args.branch):
            raise PushError("Branch must match dolquis/dev-<number>-<lowercase-slug>.")
        if git("symbolic-ref", "--quiet", "--short", "HEAD") != args.branch:
            raise PushError("Current branch does not match --branch.")
        urls = git("remote", "get-url", "--push", "--all", "origin").splitlines()
        if len(urls) != 1 or urls[0] not in URLS:
            raise PushError("origin must have exactly one permitted push URL.")
        sha = git("rev-parse", "--verify", "HEAD^{commit}")
        if not re.fullmatch(r"[0-9a-f]{40}|[0-9a-f]{64}", sha):
            raise PushError("Git did not return a valid commit ID.")
        mode = "publish" if args.publish else "dry-run" if args.dry_run else "check"
        print(f"{mode}: {DESTINATION} {args.branch} {sha}", flush=True)
        if args.publish or args.dry_run:
            flags = ["--no-mirror", "--no-follow-tags", "--recurse-submodules=no"]
            if args.dry_run:
                flags.append("--dry-run")
            # Git reads remote.*.mirror after parsing --no-mirror on some versions.
            git("-c", "remote.origin.mirror=false", "push", *flags, "origin",
                f"{sha}:refs/heads/{args.branch}", timeout=120)
            print(f"{mode} completed.")
        return 0
    except PushError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
