#!/usr/bin/env python3
"""Regression tests for the repository-owned Codex docs adapter."""

import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
REPO = Path(__file__).resolve().parents[2]
HOOK = REPO / ".codex/hooks/post-edit-docs-lint.py"
SPEC = importlib.util.spec_from_file_location("codex_docs_hook", HOOK)
hook = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(hook)


class DocsHookTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        for name in (hook.VENDORED, Path("scripts/docs-lint.py"),
                     Path("scripts/check_agent_instruction_size.py")):
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("", encoding="utf-8")
        (self.root / "docs").mkdir()

    def payload(self, *paths, cwd=None, tool="apply_patch"):
        return {"cwd": str(cwd or self.root), "tool_name": tool,
                "tool_input": {"command": "\n".join(
                    "*** Update File: " + path for path in paths)}}

    def invoke(self, payload, result=None, error=None):
        result = result or subprocess.CompletedProcess([], 0, b"", b"")
        output = io.StringIO()
        with patch.object(hook.subprocess, "run", return_value=result,
                          side_effect=error) as run, contextlib.redirect_stdout(output):
            state = hook.run_hook(payload, self.root)
        return state, output.getvalue(), run

    def test_subdirectory_cwd_resolves_patch_from_cwd_but_runs_at_root(self):
        state, output, run = self.invoke(self.payload("guide.md", cwd=self.root / "docs"))
        self.assertEqual((state, output), ("completed", ""))
        self.assertEqual(run.call_args.kwargs["cwd"], self.root)
        forwarded = json.loads(run.call_args.kwargs["input"])
        self.assertEqual(forwarded["tool_input"]["file_path"], str(self.root / "docs/guide.md"))
        self.assertEqual(run.call_args.kwargs["env"]["CLAUDE_PROJECT_DIR"], str(self.root))

    def test_root_is_derived_from_adapter_location(self):
        output = io.StringIO()
        adapter = self.root / ".codex/hooks/post-edit-docs-lint.py"
        with patch.object(hook, "__file__", str(adapter)), \
                patch.object(hook.subprocess, "run", return_value=
                             subprocess.CompletedProcess([], 0, b"", b"")) as run, \
                contextlib.redirect_stdout(output):
            hook.run_hook(self.payload("guide.md", cwd=self.root / "docs"))
        self.assertEqual(run.call_args.kwargs["cwd"], self.root)

    def test_multiple_docs_run_once_and_prioritize_agents_budget(self):
        state, output, run = self.invoke(self.payload("README.md", "AGENTS.md", "docs/a.md"))
        self.assertEqual((state, output, run.call_count), ("completed", "", 1))
        self.assertEqual(json.loads(run.call_args.kwargs["input"])["tool_input"]["file_path"],
                         str(self.root / "AGENTS.md"))

    def test_non_docs_and_outside_paths_are_quiet(self):
        for name in ("core/main.cpp", "../outside.md", str(self.root.parent / "other.md")):
            with self.subTest(name=name):
                state, output, run = self.invoke(self.payload(name))
                self.assertEqual((state, output), ("skipped", ""))
                run.assert_not_called()

    def test_delete_move_and_windows_separator_paths(self):
        payload = self.payload()
        payload["tool_input"]["command"] = (
            "*** Delete File: docs/old.md\n*** Update File: source.txt\n"
            "*** Move to: docs\\new.md\n")
        state, _, run = self.invoke(payload)
        self.assertEqual((state, run.call_count), ("completed", 1))

    def test_direct_file_path_and_malformed_input(self):
        for value in (None, [], {"file_path": 42}, {"command": [42]}):
            payload = self.payload()
            payload["tool_input"] = value
            self.assertEqual(self.invoke(payload)[:2], ("skipped", ""))
        payload["tool_input"] = {"file_path": "README.md"}
        self.assertEqual(self.invoke(payload)[0], "completed")

    def test_missing_dependencies_are_unavailable(self):
        for name in (hook.VENDORED, Path("scripts/docs-lint.py"),
                     Path("scripts/check_agent_instruction_size.py")):
            with self.subTest(name=name):
                (self.root / name).unlink()
                state, output, run = self.invoke(self.payload("AGENTS.md"))
                self.assertEqual(state, "unavailable")
                self.assertIn("未検査", json.loads(output)["hookSpecificOutput"]["additionalContext"])
                run.assert_not_called()
                (self.root / name).write_text("", encoding="utf-8")

    def test_failures_do_not_expose_exception_stdout_or_stderr(self):
        secret = "private-canary-do-not-forward"
        cases = [({"error": OSError(secret)}, "実行できない"),
                 ({"error": subprocess.TimeoutExpired(secret, 1)}, "タイムアウト"),
                 ({"result": subprocess.CompletedProcess([], 1, secret.encode(), b"")}, "異常終了"),
                 ({"result": subprocess.CompletedProcess([], 0, b"", secret.encode())}, "標準エラー")]
        for kwargs, expected in cases:
            with self.subTest(expected=expected):
                state, output, _ = self.invoke(self.payload("README.md"), **kwargs)
                self.assertEqual(state, "unavailable")
                self.assertIn(expected, json.loads(output)["hookSpecificOutput"]["additionalContext"])
                self.assertNotIn(secret, output)
                self.assertEqual(json.loads(output)["hookSpecificOutput"]["hookEventName"],
                                 "PostToolUse")

    def test_valid_findings_forward_only_fixed_labels_and_counts(self):
        report = {"hookSpecificOutput": {"hookEventName": "PostToolUse",
                                         "additionalContext":
                                         "=== DECISIVE: 2 件（指摘）===\n"
                                         "private-canary filename / snippet\n"
                                         "=== HEURISTIC: 3 件（指摘）==="},
                  "unrelated": "not-forwarded"}
        state, output, _ = self.invoke(self.payload("README.md"),
            subprocess.CompletedProcess([], 0, json.dumps(report).encode(), b""))
        self.assertEqual(state, "reported")
        context = json.loads(output)["hookSpecificOutput"]["additionalContext"]
        self.assertIn("DECISIVE: 2", context)
        self.assertIn("HEURISTIC: 3", context)
        self.assertNotIn("private-canary", output)
        self.assertNotIn("not-forwarded", output)

    def test_checker_traceback_wrapped_by_vendor_is_fixed_diagnostic(self):
        for message in ("Traceback (most recent call last):\nprivate-canary\nValueError: secret",
                        "=== DECISIVE: 0 件\nerror: private-canary",
                        "instruction-size: error path=private-canary reason=secret",
                        "unknown-output private-canary"):
            report = {"hookSpecificOutput": {"hookEventName": "PostToolUse",
                                             "additionalContext": message}}
            state, output, _ = self.invoke(self.payload("README.md"),
                subprocess.CompletedProcess([], 0, json.dumps(report).encode(), b""))
            self.assertEqual(state, "unavailable")
            self.assertNotIn("private-canary", output)

    def test_budget_finding_keeps_status_without_path(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            state = hook.report_context("instruction-size: status=warning path=private-canary bytes=123")
        self.assertEqual(state, "reported")
        self.assertIn("warning", output.getvalue())
        self.assertNotIn("private-canary", output.getvalue())

    def test_ascii_console_real_process_handles_diagnostics_and_unicode_findings(self):
        adapter = self.root / ".codex/hooks/post-edit-docs-lint.py"
        adapter.parent.mkdir(parents=True)
        adapter.write_bytes(HOOK.read_bytes())
        report = {"hookSpecificOutput": {"hookEventName": "PostToolUse",
                                         "additionalContext": "=== DECISIVE: 1 件\n秘密😀"}}
        (self.root / hook.VENDORED).write_text(
            "print(" + repr(json.dumps(report)) + ")\n", encoding="utf-8")
        unicode_cwd = self.root / "日本語"
        unicode_cwd.mkdir()
        payload = json.dumps(self.payload("README.md", cwd=unicode_cwd),
                             ensure_ascii=False).encode("utf-8")
        for encoding in ("ascii", "cp932"):
            for raw in (b"null", payload):
                done = subprocess.run([sys.executable, "-B", str(adapter)], input=raw,
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                      env=dict(os.environ, PYTHONIOENCODING=encoding), check=False)
                self.assertEqual((done.returncode, done.stderr), (0, b""))
                context = json.loads(done.stdout.decode("ascii"))["hookSpecificOutput"]["additionalContext"]
                self.assertIn("未検査" if raw == b"null" else "DECISIVE: 1", context)
                self.assertNotIn("秘密", context)

    def test_cp932_stdin_preserves_literal_utf8_japanese_cwd(self):
        payload = self.payload("README.md", cwd=Path("C:/review/日本語"))
        raw = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        with io.TextIOWrapper(io.BytesIO(raw), encoding="cp932") as stream, \
                patch.object(sys, "stdin", stream), patch.object(hook, "run_hook") as run:
            self.assertEqual(hook.main(), 0)
        run.assert_called_once_with(payload)

    def test_real_vendor_wrapped_checker_stderr_does_not_escape_adapter(self):
        adapter = self.root / ".codex/hooks/post-edit-docs-lint.py"
        adapter.parent.mkdir(parents=True)
        adapter.write_bytes(HOOK.read_bytes())
        (self.root / hook.VENDORED).write_bytes((REPO / hook.VENDORED).read_bytes())
        (self.root / "scripts/docs-lint.py").write_text(
            "import sys\nprint('Traceback (most recent call last):', file=sys.stderr)\n"
            "print('ValueError: private-checker-canary', file=sys.stderr)\nsys.exit(1)\n",
            encoding="utf-8")
        done = subprocess.run([sys.executable, "-B", str(adapter)],
                              input=json.dumps(self.payload("README.md")).encode("ascii"),
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
        self.assertEqual((done.returncode, done.stderr), (0, b""))
        self.assertNotIn(b"private-checker-canary", done.stdout)
        context = json.loads(done.stdout)["hookSpecificOutput"]["additionalContext"]
        self.assertIn("実行エラー", context)

    def test_invalid_vendor_response_is_not_forwarded(self):
        for raw in (b"private-canary", b"[]", b"null", b'{"hookSpecificOutput":null}'):
            state, output, _ = self.invoke(self.payload("README.md"),
                subprocess.CompletedProcess([], 0, raw, b""))
            self.assertEqual(state, "unavailable")
            self.assertNotIn("private-canary", output)

    def test_shell_uses_dirty_paths_without_interpreting_command(self):
        with patch.object(hook, "dirty_documents", return_value={"docs/new.md"}) as dirty:
            payload = self.payload(tool="Bash")
            payload["tool_input"]["command"] = "opaque shell command"
            state, _, run = self.invoke(payload)
        self.assertEqual((state, run.call_count), ("completed", 1))
        dirty.assert_called_once_with(self.root)

    def test_clean_shell_is_quiet_and_status_failure_is_diagnostic(self):
        with patch.object(hook, "dirty_documents", return_value=set()):
            state, output, run = self.invoke(self.payload(tool="Bash"))
        self.assertEqual((state, output, run.call_count), ("skipped", "", 0))
        with patch.object(hook, "dirty_documents", side_effect=OSError("private-canary")):
            state, output, _ = self.invoke(self.payload(tool="Bash"))
        self.assertEqual(state, "unavailable")
        self.assertNotIn("private-canary", output)

    def test_invalid_payload_is_advisory_and_diagnostic(self):
        for raw in ("broken", "null", "[]"):
            output = io.StringIO()
            with patch.object(sys, "stdin", io.StringIO(raw)), contextlib.redirect_stdout(output):
                self.assertEqual(hook.main(), 0)
            self.assertIn("未検査", json.loads(output.getvalue())["hookSpecificOutput"]["additionalContext"])

    def test_real_git_detects_modified_deleted_and_untracked_docs(self):
        def git(*args):
            subprocess.run(["git", "-c", "core.fsmonitor=false", *args], cwd=self.root,
                           check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        git("init", "-q")
        (self.root / "docs/tracked.md").write_text("before", encoding="utf-8")
        (self.root / "docs/deleted.md").write_text("before", encoding="utf-8")
        git("add", "docs")
        git("-c", "user.name=Hook test", "-c", "user.email=hook@example.invalid",
            "-c", "commit.gpgsign=false", "commit", "-qm", "fixture")
        (self.root / "docs/tracked.md").write_text("after", encoding="utf-8")
        (self.root / "docs/deleted.md").unlink()
        (self.root / "docs/new.md").write_text("new", encoding="utf-8")
        self.assertEqual(hook.dirty_documents(self.root),
                         {"docs/tracked.md", "docs/deleted.md", "docs/new.md"})

    def test_hook_configuration_matches_canonical_shell_and_timeout_budget(self):
        config = json.loads((REPO / ".codex/hooks.json").read_text(encoding="utf-8"))
        entry = config["hooks"]["PostToolUse"][0]
        self.assertTrue(re.search(entry["matcher"], "Bash"))
        self.assertTrue(re.search(entry["matcher"], "apply_patch"))
        command = entry["hooks"][0]
        self.assertIn("py -3", command["commandWindows"])
        self.assertGreater(command["timeout"], hook.TIMEOUT_SECONDS + 2 * hook.GIT_TIMEOUT_SECONDS)


if __name__ == "__main__":
    unittest.main()
