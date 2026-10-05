#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import unittest


sys.dont_write_bytecode = True
HOOK = Path(__file__).resolve().parents[2] / ".claude" / "hooks" / "git-push-guard.py"
SPEC = importlib.util.spec_from_file_location("git_push_guard", HOOK)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)

DOLQUIS = "https://github.com/dolquis/azooKey-Desktop.git"
UPSTREAM = "https://github.com/azooKey/azooKey-Desktop.git"

# A feature branch tracking itself on a dolquis origin, plus an upstream remote.
BASE_CONFIG = {
    ("symbolic-ref", "--quiet", "--short", "HEAD"): "feature",
    ("config", "--get", "branch.feature.remote"): "origin",
    ("config", "--get", "branch.feature.merge"): "refs/heads/feature",
    ("remote", "get-url", "--push", "--all", "origin"): DOLQUIS,
    ("remote", "get-url", "--push", "--all", "upstream"): UPSTREAM,
    ("remote", "get-url", "--push", "--all", "fork"): "git@github.com:dolquis/azooKey-Desktop.git",
    ("-C", "sub", "symbolic-ref", "--quiet", "--short", "HEAD"): "feature",
    ("-C", "sub", "remote", "get-url", "--push", "--all", "origin"): DOLQUIS,
}


def fake_git(overrides: dict | None = None):
    table = {**BASE_CONFIG, **(overrides or {})}

    def git(arguments: list) -> str | None:
        return table.get(tuple(arguments))
    return git


ALLOW = [
    "git push -u origin feature",
    "git push origin feature",
    "git push origin HEAD",
    "git push origin HEAD:feature",
    "git push origin feature:refs/heads/feature",
    "git push --set-upstream origin feature",
    "git push -uv origin feature",
    "git push --dry-run origin feature",
    "git push fork feature",
    "git push",
    "git -C sub push origin feature",
    "git.exe push origin feature",
    "git push https://github.com/dolquis/azooKey-Desktop.git feature",
    "git push origin feature -u",
    "git push origin feature 2>&1",
    "git push -u origin dolquis/dev-1-x > /dev/null",
]

DENY = [
    "git push origin main",
    "git push origin master",
    "git push origin HEAD:main",
    "git push origin feature:refs/heads/main",
    "git push origin refs/heads/main",
    "git push --mirror origin",
    "git push --all origin",
    "git push --branches origin",
    "git push --delete origin feature",
    "git push -d origin feature",
    "git push -ud origin feature",
    "git push --prune origin",
    "git push origin :feature",
    "git push upstream feature",
    "git push https://example.invalid/x.git feature",
    "git push origin feature HEAD:main",
    "git push --force origin main",
    "git push origin heads/main",
    "git push origin HEAD:heads/main",
    "git push origin feature:heads/master",
    "git push origin main 2>&1",
]

ASK = [
    "git status && git push origin feature",
    "git push origin feature; echo done",
    "git push origin feature | cat",
    "(git push origin feature)",
    "cd sub && git push origin feature",
    "echo $(git push origin feature)",
    "echo `git push origin feature`",
    "git status\ngit push origin main",
    'bash -c "git push origin feature"',
    'pwsh -Command "git push origin feature"',
    "xargs git push < remotes",
    "GIT_DIR=x git push origin feature",
    "timeout 60 git push origin feature",
    "git -c remote.origin.pushurl=x push origin feature",
    "git --git-dir=x push origin feature",
    "git push -f origin feature",
    "git push --force-with-lease origin feature",
    "git push origin +feature",
    "git push --no-verify origin feature",
    "git push --tags origin",
    "git push origin 'feature/*'",
    "git push origin refs/tags/v1",
    "git push --repo=origin",
    "git push nowhere feature",
    "git push origin 'unterminated",
    # The shell joins quoted pieces into one word; the hook does not try to.
    'git push origin HEAD:"main"',
    "git push origin HEAD:ma''in",
    'git push origin main""',
    "git push origin HEAD:m\\ain",
    "git push origin '--'mirror",
    "git push origin feature '--'delete",
    'git push origin feature "-"f',
    'git "push" origin main',
    "git 'push' origin main",
    '"git" push origin main',
    "git p\\ush origin main",
    "git push origin tags/v1",
    "git push origin feature:remotes/origin/main",
]


class GitPushGuardDecisionTest(unittest.TestCase):
    def assert_decision(self, commands: list[str], expected: str, git=None) -> None:
        for command in commands:
            with self.subTest(command=command):
                result = MODULE.decide(command, git or fake_git())
                self.assertIsNotNone(result, command)
                self.assertEqual(result[0], expected, f"{command}: {result[1]}")

    def test_dolquis_feature_pushes_are_allowed(self) -> None:
        self.assert_decision(ALLOW, "allow")

    def test_known_violations_are_denied(self) -> None:
        self.assert_decision(DENY, "deny")

    def test_unparsed_pushes_go_back_to_the_human(self) -> None:
        self.assert_decision(ASK, "ask")

    def test_non_push_commands_have_no_opinion(self) -> None:
        for command in ["git status -sb", "git log --oneline", 'rg -n "git push" docs/',
                        "echo 'git push origin main'", "git pull origin main"]:
            with self.subTest(command=command):
                self.assertIsNone(MODULE.decide(command, fake_git()))

    def test_refspec_less_push_follows_branch_and_upstream(self) -> None:
        on_main = {("symbolic-ref", "--quiet", "--short", "HEAD"): "main",
                   ("config", "--get", "branch.main.remote"): "origin"}
        self.assert_decision(["git push", "git push origin", "git push origin HEAD"], "deny", fake_git(on_main))
        tracking_main = {("config", "--get", "branch.feature.merge"): "refs/heads/main"}
        self.assert_decision(["git push", "git push origin"], "deny", fake_git(tracking_main))
        self.assert_decision(["git push origin feature"], "allow", fake_git(tracking_main))
        other_upstream = {("config", "--get", "branch.feature.merge"): "refs/heads/other"}
        self.assert_decision(["git push"], "ask", fake_git(other_upstream))
        matching = {("config", "--get", "push.default"): "matching"}
        self.assert_decision(["git push"], "ask", fake_git(matching))
        push_mapping = {("config", "--get-all", "remote.origin.push"): "refs/heads/*:refs/heads/*"}
        self.assert_decision(["git push"], "ask", fake_git(push_mapping))

    def test_remote_resolution_follows_git_precedence(self) -> None:
        push_remote = {("config", "--get", "branch.feature.pushRemote"): "upstream"}
        self.assert_decision(["git push"], "deny", fake_git(push_remote))
        push_default = {("config", "--get", "remote.pushDefault"): "upstream"}
        self.assert_decision(["git push"], "deny", fake_git(push_default))
        no_remote = {("config", "--get", "branch.feature.remote"): None}
        self.assert_decision(["git push"], "ask", fake_git(no_remote))
        detached = {("symbolic-ref", "--quiet", "--short", "HEAD"): None}
        self.assert_decision(["git push", "git push origin HEAD"], "ask", fake_git(detached))

    def test_every_push_url_must_be_dolquis(self) -> None:
        mixed = {("remote", "get-url", "--push", "--all", "origin"): f"{DOLQUIS}\n{UPSTREAM}"}
        self.assert_decision(["git push origin feature"], "deny", fake_git(mixed))
        dotted = {("remote", "get-url", "--push", "--all", "origin"): "https://github.com/dolquis/../evil/x.git"}
        self.assert_decision(["git push origin feature"], "ask", fake_git(dotted))
        rewrite = {("config", "--get-regexp", r"^url\..*insteadof$"):
                   "url.https://example.invalid/.pushinsteadof https://github.com/dolquis/"}
        self.assert_decision(["git push https://github.com/dolquis/azooKey-Desktop.git feature"], "ask",
                             fake_git(rewrite))
        self.assert_decision(["git push origin feature"], "allow", fake_git(rewrite))

    def test_remote_push_mapping_blocks_source_only_refspecs(self) -> None:
        mapping = {("config", "--get-all", "remote.origin.push"): "refs/heads/feature:refs/heads/main"}
        self.assert_decision(["git push origin feature", "git push origin HEAD"], "ask", fake_git(mapping))
        self.assert_decision(["git push origin feature:feature"], "allow", fake_git(mapping))


def run(payload: object) -> subprocess.CompletedProcess:
    text = payload if isinstance(payload, str) else json.dumps(payload)
    return subprocess.run(
        [sys.executable, str(HOOK)], input=text, text=True, encoding="utf-8",
        capture_output=True, check=False,
    )


class GitPushGuardProcessTest(unittest.TestCase):
    def test_non_push_and_bad_input_print_nothing(self) -> None:
        for payload in [
            {"tool_name": "Bash", "tool_input": {"command": "git status"}},
            {"tool_name": "Read", "tool_input": {"command": "git push origin main"}},
            "not json",
        ]:
            with self.subTest(payload=payload):
                result = run(payload)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout, "")

    def test_push_prints_a_permission_decision(self) -> None:
        result = run({"tool_name": "Bash", "tool_input": {"command": "git push origin HEAD:main"}})
        self.assertEqual(result.returncode, 0, result.stderr)
        output = json.loads(result.stdout)["hookSpecificOutput"]
        self.assertEqual(output["hookEventName"], "PreToolUse")
        self.assertEqual(output["permissionDecision"], "deny")
        self.assertIn("main", output["permissionDecisionReason"])


if __name__ == "__main__":
    unittest.main()
