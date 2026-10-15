"""Run: py -3.13 -B -m unittest discover -s scripts/tests -p test_codex_push.py

Alternative: python3 -B -m unittest discover -s scripts/tests -p test_codex_push.py
Most Git processes are mocked. One temporary local fixture uses push --dry-run;
these tests never publish refs or contact an external remote.
"""

import contextlib
import importlib.util
import io
from pathlib import Path
import subprocess
import shutil
import tempfile
import unittest
from unittest.mock import patch


spec = importlib.util.spec_from_file_location("codex_push", Path(__file__).parents[1] / "codex_push.py")
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)
BRANCH = "dolquis/dev-123-safe-push"
SHA = "a" * 40
URL = "https://github.com/dolquis/azooKey-Desktop.git"


class PushTests(unittest.TestCase):
    def invoke(self, arguments=(), outputs=None):
        responses = outputs if outputs is not None else [BRANCH, URL, SHA, ""]
        results = [subprocess.CompletedProcess([], 0, value, "") for value in responses]
        with patch.object(helper.subprocess, "run", side_effect=results) as run:
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                code = helper.main(["--branch", BRANCH, *arguments])
        return code, run.call_args_list

    def test_default_never_pushes(self):
        code, calls = self.invoke()
        self.assertEqual(code, 0)
        self.assertEqual(len(calls), 3)

    def test_publish_pins_sha_and_overrides_scope_expanding_config(self):
        code, calls = self.invoke(["--publish"])
        self.assertEqual(code, 0)
        self.assertEqual(calls[-1].args[0], [
            "git", "-C", str(helper.ROOT), "-c", "remote.origin.mirror=false", "push", "--no-mirror",
            "--no-follow-tags", "--recurse-submodules=no", "origin",
            f"{SHA}:refs/heads/{BRANCH}",
        ])
        self.assertFalse(calls[-1].kwargs["shell"])
        self.assertEqual(calls[-1].kwargs["timeout"], 120)

    def test_dry_run_is_explicit(self):
        code, calls = self.invoke(["--dry-run"])
        self.assertEqual(code, 0)
        self.assertIn("--dry-run", calls[-1].args[0])

    def test_invalid_branch_and_arbitrary_options_rejected(self):
        for branch in ("main", "dolquis/dev-1-../main", "dolquis/dev-1-x:y", "dolquis/dev-1-X"):
            with self.subTest(branch=branch), patch.object(helper.subprocess, "run") as run:
                with contextlib.redirect_stderr(io.StringIO()):
                    self.assertEqual(helper.main(["--branch", branch, "--publish"]), 1)
                run.assert_not_called()
        for options in (["--pub"], ["--force"], ["--publish", "--dry-run"], ["--remote", "other"]):
            with self.subTest(options=options), self.assertRaises(SystemExit) as caught:
                with contextlib.redirect_stderr(io.StringIO()):
                    helper.main(["--branch", BRANCH, *options])
            self.assertEqual(caught.exception.code, 2)

    def test_current_branch_must_match(self):
        code, calls = self.invoke(["--publish"], ["main"])
        self.assertEqual(code, 1)
        self.assertEqual(len(calls), 1)

    def test_urls_are_exact_and_single(self):
        for url in (URL + "\n" + URL, " " + URL, URL + " ", URL + "/", "https://secret@github.com/dolquis/azooKey-Desktop.git", "https://github.com/other/azooKey-Desktop.git", ""):
            with self.subTest(url=url):
                code, calls = self.invoke(["--publish"], [BRANCH, url])
                self.assertEqual(code, 1)
                self.assertEqual(len(calls), 2)
        for url in helper.URLS:
            self.assertEqual(self.invoke(outputs=[BRANCH, url, SHA])[0], 0)

    def test_invalid_commit_id_rejected(self):
        code, calls = self.invoke(["--publish"], [BRANCH, URL, "HEAD"])
        self.assertEqual(code, 1)
        self.assertEqual(len(calls), 3)

    def test_errors_do_not_leak_process_output(self):
        for outcome in (subprocess.TimeoutExpired("secret", 30, stderr="secret"), OSError("secret"), subprocess.CompletedProcess([], 1, "secret", "secret")):
            kwargs = {"side_effect": outcome} if isinstance(outcome, Exception) else {"return_value": outcome}
            with patch.object(helper.subprocess, "run", **kwargs):
                stderr = io.StringIO()
                with contextlib.redirect_stderr(stderr):
                    self.assertEqual(helper.main(["--branch", BRANCH]), 1)
                self.assertNotIn("secret", stderr.getvalue())

    def test_repository_routing_environment_removed(self):
        with patch.dict(helper.os.environ, {"GIT_DIR": "other", "GIT_NAMESPACE": "other"}):
            code, calls = self.invoke()
        self.assertEqual(code, 0)
        for call in calls:
            self.assertNotIn("GIT_DIR", call.kwargs["env"])
            self.assertNotIn("GIT_NAMESPACE", call.kwargs["env"])

    @unittest.skipUnless(shutil.which("git"), "Git is required for the local fixture")
    def test_scope_flags_override_mirror_and_follow_tags_in_real_git(self):
        _, calls = self.invoke(["--dry-run"])
        with tempfile.TemporaryDirectory(prefix="codex-push-test-") as directory:
            root = Path(directory)
            source, target = root / "source", root / "target.git"
            env = helper.os.environ.copy()
            for key in list(env):
                if key.startswith("GIT_"):
                    env.pop(key)
            env.update(GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=str(root / "absent"))

            def run(*args):
                result = subprocess.run(["git", *map(str, args)], capture_output=True,
                                        text=True, timeout=30, env=env)
                self.assertEqual(result.returncode, 0, result.stderr)
                return result

            run("init", "--bare", target)
            run("init", "-b", BRANCH, source)
            run("-C", source, "-c", "user.name=Test", "-c", "user.email=test@example.invalid",
                "commit", "--allow-empty", "-m", "fixture")
            sha = run("-C", source, "rev-parse", "HEAD").stdout.strip()
            run("-C", source, "branch", "unrelated")
            run("-C", source, "-c", "user.name=Test", "-c", "user.email=test@example.invalid",
                "tag", "-a", "unrelated-tag", "-m", "fixture")
            run("-C", source, "remote", "add", "origin", target)
            run("-C", source, "config", "remote.origin.mirror", "true")
            run("-C", source, "config", "push.followTags", "true")
            run("-C", source, "config", "remote.origin.push", "refs/heads/unrelated:refs/heads/unrelated")
            command = calls[-1].args[0][3:]
            command[-1] = f"{sha}:refs/heads/{BRANCH}"
            result = run("-C", source, *command)
            self.assertIn(BRANCH, result.stderr)
            self.assertNotIn("unrelated", result.stderr)
            self.assertEqual(run("-C", target, "for-each-ref").stdout, "")


if __name__ == "__main__":
    unittest.main()
