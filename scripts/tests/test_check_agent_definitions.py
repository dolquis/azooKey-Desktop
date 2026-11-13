#!/usr/bin/env python3
"""Tests for scripts/check_agent_definitions.py."""

from __future__ import annotations

import contextlib
import importlib.util
import io
from pathlib import Path
import sys
import tempfile
import unittest


sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("check_agent_definitions", ROOT / "scripts" / "check_agent_definitions.py")
assert SPEC is not None and SPEC.loader is not None
CHECKER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECKER)

GUARD_HOOK = """hooks:
  PreToolUse:
    - matcher: Bash
      hooks:
        - type: command
          command: python3 "$CLAUDE_PROJECT_DIR/.claude/hooks/agent-readonly-guard.py"
"""
BODY = "# Reviewer\n\n## 境界\n\n- ファイルを書かない。`docs/guide.md` を読む。\n"


def claude_text(name: str = "reviewer", body: str = BODY, extra: str = "", tools: str | None = "Read, Grep, Glob, Bash",
                description: str = "差分を読む read-only レビュー。") -> str:
    tools_line = f"tools: {tools}\n" if tools is not None else ""
    return (
        f"---\n# 同期の注記\nname: {name}\ndescription: {description}\n{tools_line}"
        f"disallowedTools: Edit, Write, NotebookEdit\n{extra}{GUARD_HOOK}---\n\n{body}"
    )


def codex_text(name: str = "reviewer", body: str = BODY, extra: str = "",
               description: str = "差分を読む read-only レビュー。") -> str:
    return f'name = "{name}"\ndescription = "{description}"\n{extra}developer_instructions = \'\'\'\n{body}\'\'\'\n'


CONFIG = """[agents.reviewer]
write_boundary = "read-only"

[boundary]
require_declaration = true
heading = "## 境界"

[references]
roots = "auto"
"""


class CheckerTestCase(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.write(".claude/agents/reviewer.md", claude_text())
        self.write(".codex/agents/reviewer.toml", codex_text())
        self.write(".claude/hooks/agent-readonly-guard.py", "# guard\n")
        self.write("docs/guide.md", "guide\n")
        self.write("scripts/check_agent_definitions.toml", CONFIG)

    def write(self, name: str, text: str) -> None:
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8", newline="\n")

    def config(self, text: str) -> None:
        self.write("scripts/check_agent_definitions.toml", text)

    def run_checker(self, *extra: str) -> tuple[int, str, str]:
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            code = CHECKER.main(["--root", str(self.root), *extra])
        return code, stdout.getvalue(), stderr.getvalue()

    def assertFinding(self, fragment: str) -> None:
        code, out, err = self.run_checker()
        self.assertEqual(code, 1, out + err)
        self.assertIn(fragment, out)

    def assertClean(self) -> None:
        code, out, err = self.run_checker()
        self.assertEqual(code, 0, out + err)


class SetupTests(CheckerTestCase):
    def test_clean_fixture_passes(self) -> None:
        self.assertClean()

    def test_missing_config_is_a_setup_error(self) -> None:
        (self.root / "scripts/check_agent_definitions.toml").unlink()
        code, _, err = self.run_checker()
        self.assertEqual(code, 2)
        self.assertIn("設定ファイルが無い", err)

    def test_malformed_or_invalid_config_is_a_setup_error(self) -> None:
        for text in ("[agents\n", '[agents.reviewer]\nwrite_boundary = "write-all"\n', '[body]\nnormalize = "loose"\n',
                     '[mirror]\nclaude_only = "reviewer"\n', '[agents.reviewer]\ncopies = ".claude/x.md"\n',
                     "[references]\nroots = 3\n", '[skills]\nprefixes = [""]\n', '[layout]\nunknown = "x"\n'):
            with self.subTest(text=text):
                self.config(text)
                self.assertEqual(self.run_checker()[0], 2)

    def test_unknown_sections_and_keys_are_setup_errors(self) -> None:
        for text, fragment in (("[referenses]\nroots = \"auto\"\n", "[referenses]"),
                               ('[agents.reviewer]\nwrite_boundry = "read-only"\n', "[agents.reviewer].write_boundry"),
                               ("[boundary]\nheadng = \"## 境界\"\n", "[boundary].headng"),
                               ('[skills]\nprefix = ["demo-"]\n', "[skills].prefix")):
            with self.subTest(fragment=fragment):
                self.config(text)
                code, _, err = self.run_checker()
                self.assertEqual(code, 2)
                self.assertIn(fragment, err)

    def test_help_works_without_docstrings(self) -> None:
        original = CHECKER.__doc__
        CHECKER.__doc__ = None
        try:
            with self.assertRaises(SystemExit) as raised, contextlib.redirect_stdout(io.StringIO()):
                CHECKER.main(["--help"])
            self.assertEqual(raised.exception.code, 0)
        finally:
            CHECKER.__doc__ = original

    def test_empty_sections_and_values_are_setup_errors(self) -> None:
        agent = '[agents.reviewer]\nwrite_boundary = "read-only"\n\n'
        cases = (
            (agent + "[manifest]\n", "[manifest] が空"),
            (agent + "[codex_config]\n", "[codex_config] が空"),
            (agent + "[body]\n", "[body] が空"),
            (agent + '[limits]\ncodex_config = ".codex/config.toml"\n', "[limits] に必須キー"),
            (agent + "[skills]\nprefixes = []\n", "[skills].prefixes"),
            (agent + '[skills]\nprefixes = ["demo-", " "]\n', "[skills].prefixes"),
            (agent + "[references]\nroots = []\n", "[references].roots"),
            (agent + '[references]\nroots = ["docs", ""]\n', "[references].roots"),
            (agent + '[boundary]\nheading = ""\n', "[boundary].heading"),
            (agent + '[boundary]\nguard_script = ""\n', "[boundary].guard_script"),
            (agent + "[frontmatter]\nforbidden = []\n", "[frontmatter].forbidden"),
            (agent + '[manifest]\npath = ""\n', "[manifest].path"),
            (agent + '[layout]\nclaude_dir = ""\n', "[layout].claude_dir"),
            (agent + '[body]\nreplace = [["", "x"]]\n', "[body].replace"),
            ('[agents.reviewer]\ncopies = [""]\n', "[agents.reviewer].copies"),
            ('[agents.reviewer]\nclaude = ""\n', "[agents.reviewer].claude"),
        )
        for text, fragment in cases:
            with self.subTest(text=text):
                self.config(text)
                code, _, err = self.run_checker()
                self.assertEqual(code, 2, err)
                self.assertIn(fragment, err)

    def test_empty_lists_that_record_intent_are_accepted(self) -> None:
        self.config(CONFIG + "\n[mirror]\nclaude_only = []\ncodex_only = []\n")
        self.assertClean()
        self.config('[agents.reviewer]\nwrite_boundary = "read-only"\n\n[references]\n')
        self.assertClean()

    def test_config_option_points_outside_the_repository(self) -> None:
        outside = Path(self.temp.name) / "elsewhere.toml"
        outside.write_text(CONFIG, encoding="utf-8")
        (self.root / "scripts/check_agent_definitions.toml").unlink()
        self.assertEqual(self.run_checker("--config", str(outside))[0], 0)

    def test_zero_agents_pass_only_when_explicit(self) -> None:
        for path in (".claude/agents/reviewer.md", ".codex/agents/reviewer.toml"):
            (self.root / path).unlink()
        self.config("[boundary]\nheading = \"## 境界\"\n")
        self.assertFinding("0 件")
        self.config("agents = {}\n")
        self.assertClean()


class PairTests(CheckerTestCase):
    def test_undeclared_agent_file_is_reported(self) -> None:
        self.write(".claude/agents/extra.md", claude_text("extra"))
        self.assertFinding("extra: agent ファイルがあるが設定の [agents] に無い")

    def test_non_agent_markdown_is_not_discovered(self) -> None:
        self.write(".claude/agents/MANIFEST.md", "# manifest\n")
        self.assertClean()

    def test_missing_side_and_mirror_exception(self) -> None:
        (self.root / ".codex/agents/reviewer.toml").unlink()
        self.assertFinding("pair: .codex/agents/reviewer.toml: 無い")
        self.config(CONFIG + '\n[mirror]\nclaude_only = ["reviewer"]\n')
        self.assertClean()

    def test_mirror_names_must_be_configured_and_one_sided(self) -> None:
        for text, fragment in (('[mirror]\nclaude_only = ["reviewer"]\ncodex_only = ["reviewer"]\n', "両方にある: reviewer"),
                               ('[mirror]\nclaude_only = ["ghost"]\n', "`ghost` が [agents] に無い")):
            with self.subTest(fragment=fragment):
                self.config(CONFIG + "\n" + text)
                code, _, err = self.run_checker()
                self.assertEqual(code, 2)
                self.assertIn(fragment, err)

    def test_one_sided_agent_must_not_have_the_other_file(self) -> None:
        self.config(CONFIG + '\n[mirror]\nclaude_only = ["reviewer"]\n')
        self.assertFinding("pair: .codex/agents/reviewer.toml: [mirror].claude_only の agent なのに Codex 側がある")
        self.config(CONFIG + '\n[mirror]\ncodex_only = ["reviewer"]\n')
        self.assertFinding("pair: .claude/agents/reviewer.md: [mirror].codex_only の agent なのに Claude 側がある")

    def test_agent_with_no_file_is_reported(self) -> None:
        for path in (".claude/agents/reviewer.md", ".codex/agents/reviewer.toml"):
            (self.root / path).unlink()
        self.config(CONFIG + '\n[mirror]\nclaude_only = ["reviewer"]\n')
        self.assertFinding("pair: reviewer: 検査したファイルが 1 つも無い")

    def test_configured_agent_without_files_is_reported(self) -> None:
        self.config(CONFIG + '\n[agents.ghost]\nwrite_boundary = "read-only"\n')
        self.assertFinding("pair: .claude/agents/ghost.md: 無い")

    def test_override_paths_and_copies(self) -> None:
        self.write("templates/reviewer.md", claude_text())
        self.write("templates/reviewer.toml", codex_text())
        (self.root / ".codex/agents/reviewer.toml").unlink()
        override = CONFIG.replace(
            'write_boundary = "read-only"\n',
            'write_boundary = "read-only"\nclaude = "templates/reviewer.md"\ncodex = "templates/reviewer.toml"\n')
        self.config(override)
        self.assertFinding("上書き先 templates/reviewer.md と別にあり")
        self.config(override.replace('codex = "templates/reviewer.toml"\n',
                                     'codex = "templates/reviewer.toml"\ncopies = [".claude/agents/reviewer.md"]\n'))
        self.assertClean()
        self.write(".claude/agents/reviewer.md", claude_text(body=BODY + "\n追記\n"))
        self.assertFinding("pair: .claude/agents/reviewer.md: templates/reviewer.md と byte 一致しない")


class ContentTests(CheckerTestCase):
    def test_name_description_and_body_drift(self) -> None:
        self.write(".codex/agents/reviewer.toml", codex_text(name="other"))
        self.assertFinding("name: .codex/agents/reviewer.toml")
        self.write(".codex/agents/reviewer.toml", codex_text(description="別の説明"))
        self.assertFinding("description が一致しない")
        self.write(".codex/agents/reviewer.toml", codex_text(body=BODY + "追記\n"))
        self.assertFinding("本文と developer_instructions が一致しない")

    def test_default_strip_keeps_inner_whitespace_significant(self) -> None:
        self.write(".codex/agents/reviewer.toml", codex_text(body="\n\n" + BODY.replace("\n\n", "\n") + "\n"))
        self.assertFinding("本文と developer_instructions が一致しない")
        self.config(CONFIG + '\n[body]\nnormalize = "nonblank-lines"\n')
        self.assertClean()

    def test_body_replace_maps_the_codex_skill_tree(self) -> None:
        self.write("docs/x.md", "x\n")
        self.write(".claude/agents/reviewer.md", claude_text(body=BODY + "`.claude/skills/` を読む。\n"))
        self.write(".codex/agents/reviewer.toml", codex_text(body=BODY + "`.agents/skills/` を読む。\n"))
        self.assertFinding("本文と developer_instructions が一致しない")
        (self.root / ".claude/skills").mkdir(parents=True)
        (self.root / ".agents/skills").mkdir(parents=True)
        self.config(CONFIG + '\n[body]\nreplace = [[".agents/skills/", ".claude/skills/"]]\n')
        self.assertClean()

    def test_tools_must_be_explicit_and_leave_agent_out(self) -> None:
        self.write(".claude/agents/reviewer.md", claude_text(tools=None))
        self.assertFinding("tools を明示していない")
        self.write(".claude/agents/reviewer.md", claude_text(tools="[Read, Agent]"))
        self.assertFinding("tools に subagent ツール Agent がある")
        self.write(".claude/agents/reviewer.md", claude_text(tools="\n  - Read\n  - Agent"))
        self.assertFinding("tools に subagent ツール Agent がある")
        self.write(".claude/agents/reviewer.md", claude_text(tools="\n  - Read\n  - Bash"))
        self.assertClean()
        for tools, fragment in (("Read, Task", "Task"), ("Read, Agent(Explore)", "Agent(Explore)"),
                                ("Read, Task(general-purpose)", "Task(general-purpose)")):
            with self.subTest(tools=tools):
                self.write(".claude/agents/reviewer.md", claude_text(tools=tools))
                self.assertFinding(f"tools に subagent ツール {fragment} がある")

    def test_unsupported_frontmatter_is_reported(self) -> None:
        for extra, fragment in (("summary: |\n  text\n", "block scalar"), ("tools: Read\n", "重複"),
                                ("not a pair\n", "key: value の形でない行")):
            with self.subTest(extra=extra):
                self.write(".claude/agents/reviewer.md", claude_text(extra=extra))
                self.assertFinding(fragment)

    def test_forbidden_frontmatter_keys(self) -> None:
        self.write(".claude/agents/reviewer.md", claude_text(extra="memory: project\n"))
        self.assertClean()
        self.config(CONFIG + '\n[frontmatter]\nforbidden = ["memory", "background"]\n')
        self.assertFinding("frontmatter: .claude/agents/reviewer.md: memory を置かない")


class CodexTests(CheckerTestCase):
    def test_session_keys_are_rejected(self) -> None:
        for key, value in (("sandbox_mode", '"read-only"'), ("sandbox_workspace_write", "{ network_access = true }"),
                           ("approval_policy", '"never"'), ("windows", "{ sandbox = \"elevated\" }"),
                           ("model_provider", '"openai"'), ("mcp_servers", "{}"),
                           ("sandbox_private_desktop", "true")):
            with self.subTest(key=key):
                self.write(".codex/agents/reviewer.toml", codex_text(extra=f"{key} = {value}\n"))
                self.assertFinding(f"{key} は custom agent で上書きできない")

    def test_model_settings_are_allowed_but_must_be_nonempty(self) -> None:
        self.write(".codex/agents/reviewer.toml", codex_text(extra='model_reasoning_effort = "low"\n'))
        self.assertClean()
        self.write(".codex/agents/reviewer.toml", codex_text(extra='model = ""\n'))
        self.assertFinding("model は空でない文字列が必要")

    def test_unparsable_toml_is_reported(self) -> None:
        self.write(".codex/agents/reviewer.toml", 'name = "reviewer\n')
        self.assertFinding("TOML として parse できない")

    def test_codex_config_types(self) -> None:
        self.config(CONFIG + '\n[codex_config]\npath = ".codex/config.toml"\n')
        self.write(".codex/config.toml", "[agents]\nenabled = false\n")
        self.assertClean()
        self.write(".codex/config.toml", '[agents]\nenabled = "yes"\nmax_threads = 0\n[windows]\nsandbox_private_desktop = true\n')
        code, out, _ = self.run_checker()
        self.assertEqual(code, 1)
        for fragment in ("[agents].enabled は boolean", "[agents].max_threads は正の整数", "sandbox_private_desktop は未対応"):
            self.assertIn(fragment, out)


class BoundaryTests(CheckerTestCase):
    def test_read_only_needs_disallowed_tools_hook_and_guard_script(self) -> None:
        text = claude_text()
        self.write(".claude/agents/reviewer.md", text.replace("disallowedTools: Edit, Write, NotebookEdit\n", "disallowedTools: Edit\n"))
        self.assertFinding("disallowedTools に Write, NotebookEdit が無い")
        self.write(".claude/agents/reviewer.md", text.replace(GUARD_HOOK, ""))
        self.assertFinding("hooks.PreToolUse（matcher Bash")
        self.write(".claude/agents/reviewer.md", text.replace("matcher: Bash", "matcher: Read"))
        self.assertFinding("hooks.PreToolUse（matcher Bash")
        self.write(".claude/agents/reviewer.md", text)
        (self.root / ".claude/hooks/agent-readonly-guard.py").unlink()
        self.assertFinding("guard script `.claude/hooks/agent-readonly-guard.py` が無い")

    def test_guard_hook_must_run_the_configured_guard_script(self) -> None:
        other = claude_text().replace(".claude/hooks/agent-readonly-guard.py", "tools/my-agent-readonly-guard.py")
        self.write(".claude/agents/reviewer.md", other)
        self.assertFinding("command が `.claude/hooks/agent-readonly-guard.py` を実行")
        self.write("tools/my-agent-readonly-guard.py", "# guard\n")
        self.config(CONFIG.replace("[boundary]\n", '[boundary]\nguard_script = "tools/my-agent-readonly-guard.py"\n'))
        self.assertClean()

    def test_controls_off_warns_on_pass_and_fail(self) -> None:
        self.config(CONFIG.replace("[boundary]\n", "[boundary]\ncontrols = false\n"))
        self.write(".claude/agents/reviewer.md", claude_text().replace(GUARD_HOOK, ""))
        code, out, _ = self.run_checker()
        self.assertEqual(code, 0, out)
        self.assertIn("warning: [boundary].controls = false", out)
        self.assertIn("reviewer(read-only)", out)
        self.write(".codex/agents/reviewer.toml", codex_text(description="別の説明"))
        code, out, _ = self.run_checker()
        self.assertEqual(code, 1, out)
        self.assertIn("warning: [boundary].controls = false", out)
        self.config(CONFIG)
        self.write(".claude/agents/reviewer.md", claude_text())
        self.write(".codex/agents/reviewer.toml", codex_text())
        self.assertNotIn("warning", self.run_checker()[1])

    def test_build_write_needs_disallowed_tools_only(self) -> None:
        self.config(CONFIG.replace('"read-only"', '"build-write"'))
        self.write(".claude/agents/reviewer.md", claude_text().replace(GUARD_HOOK, ""))
        self.assertClean()
        self.write(".claude/agents/reviewer.md", claude_text().replace(GUARD_HOOK, "").replace(
            "disallowedTools: Edit, Write, NotebookEdit\n", ""))
        self.assertFinding("disallowedTools に Edit, Write, NotebookEdit が無い")

    def test_writing_tools_fail_even_without_controls(self) -> None:
        self.config(CONFIG.replace("[boundary]\n", "[boundary]\ncontrols = false\n"))
        self.write(".claude/agents/reviewer.md", "---\nname: reviewer\ndescription: 差分を読む read-only レビュー。\n"
                   "tools: Read, Bash\n---\n\n" + BODY)
        self.assertClean()
        self.write(".claude/agents/reviewer.md", "---\nname: reviewer\ndescription: 差分を読む read-only レビュー。\n"
                   "tools: Read, Edit\n---\n\n" + BODY)
        self.assertFinding("tools に書込みツール Edit がある")

    def test_declaration_and_heading_are_configurable(self) -> None:
        self.config(CONFIG.replace('write_boundary = "read-only"\n', ""))
        self.assertFinding("write_boundary の宣言が設定に無い")
        self.config("[agents.reviewer]\n")
        self.assertClean()
        self.write(".claude/agents/reviewer.md", claude_text(body=BODY.replace("## 境界", "## 制約")))
        self.write(".codex/agents/reviewer.toml", codex_text(body=BODY.replace("## 境界", "## 制約")))
        self.assertClean()
        self.config(CONFIG)
        self.assertFinding("本文に「## 境界」見出しが無い")


class ReferenceTests(CheckerTestCase):
    def body_with(self, text: str) -> None:
        self.write(".claude/agents/reviewer.md", claude_text(body=BODY + text))
        self.write(".codex/agents/reviewer.toml", codex_text(body=BODY + text))

    def test_missing_path_is_reported_on_both_sides(self) -> None:
        self.body_with("`docs/missing.md` を読む。\n")
        code, out, _ = self.run_checker()
        self.assertEqual(code, 1)
        self.assertIn("reference: .claude/agents/reviewer.md: 存在しないパス `docs/missing.md`", out)
        self.assertIn("reference: .codex/agents/reviewer.toml: 存在しないパス `docs/missing.md`", out)

    def test_globs_placeholders_and_unknown_roots(self) -> None:
        self.body_with("`docs/*.md`、`docs/<name>.md`、`origin/main`、`git diff docs/missing.md` を使う。\n")
        self.assertClean()
        self.body_with("`docs/*.txt` を使う。\n")
        self.assertFinding("存在しないパス `docs/*.txt`")

    def test_explicit_roots_and_exclude(self) -> None:
        self.write("build/keep.txt", "x\n")
        self.body_with("`build/out/` を使う。\n")
        self.assertFinding("存在しないパス `build/out/`")
        self.config(CONFIG.replace('roots = "auto"\n', 'roots = "auto"\nexclude = ["build"]\n'))
        self.assertClean()
        self.config(CONFIG.replace('roots = "auto"\n', 'roots = ["docs"]\n'))
        self.assertClean()

    def test_references_are_off_without_the_section(self) -> None:
        self.config(CONFIG.replace('[references]\nroots = "auto"\n', ""))
        self.body_with("`docs/missing.md` を読む。\n")
        self.assertClean()

    def test_skill_names_need_a_configured_prefix(self) -> None:
        self.config(CONFIG + '\n[skills]\nprefixes = ["demo-"]\n')
        self.write(".claude/skills/demo-review/SKILL.md", "skill\n")
        self.write("demo-tool/main.c", "x\n")
        self.body_with("`demo-review`、`demo-tool`、`read-only`、`docs-lint` を使う。\n")
        self.assertClean()
        self.body_with("`demo-missing` を使う。\n")
        self.assertFinding("skill: .claude/agents/reviewer.md: 存在しない Skill `demo-missing`")


class CrossFileTests(CheckerTestCase):
    def test_limits_must_match(self) -> None:
        self.config(CONFIG + '\n[limits]\ncodex_config = ".codex/config.toml"\ncodex_key = "agents.max_concurrent_threads_per_session"\n'
                    'claude_settings = ".claude/settings.json"\nclaude_env = "CLAUDE_CODE_MAX_CONCURRENT_SUBAGENTS"\n')
        self.write(".codex/config.toml", "[agents]\nmax_concurrent_threads_per_session = 4\n")
        self.write(".claude/settings.json", '{"env": {"CLAUDE_CODE_MAX_CONCURRENT_SUBAGENTS": "4"}}\n')
        self.assertClean()
        self.write(".claude/settings.json", '{"env": {"CLAUDE_CODE_MAX_CONCURRENT_SUBAGENTS": "6"}}\n')
        self.assertFinding("が一致しない")
        self.write(".claude/settings.json", "{}\n")
        self.assertFinding("env に CLAUDE_CODE_MAX_CONCURRENT_SUBAGENTS が無い")

    def test_manifest_rows_must_match_the_config(self) -> None:
        self.config(CONFIG + '\n[manifest]\npath = ".claude/agents/MANIFEST.md"\n')
        self.write(".claude/agents/MANIFEST.md", "| agent | 区分 |\n|---|---|\n| `reviewer` | repo |\n")
        self.assertClean()
        self.write(".claude/agents/MANIFEST.md", "| agent | 区分 |\n|---|---|\n| `other` | repo |\n")
        code, out, _ = self.run_checker()
        self.assertEqual(code, 1)
        self.assertIn("表にあるが設定に無い agent `other`", out)
        self.assertIn("設定にあるが表に無い agent `reviewer`", out)


class RepositoryTests(unittest.TestCase):
    def test_repository_definitions_pass(self) -> None:
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(io.StringIO()):
            code = CHECKER.main(["--root", str(ROOT)])
        self.assertEqual(code, 0, stdout.getvalue())


if __name__ == "__main__":
    unittest.main()
