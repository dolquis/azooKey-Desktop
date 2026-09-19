#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest


sys.dont_write_bytecode = True
SCRIPT = Path(__file__).resolve().parents[1] / "check_agent_definitions.py"
SPEC = importlib.util.spec_from_file_location("check_agent_definitions", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)

REPO_ROOT = SCRIPT.parents[1]

MANIFEST = """\
| agent | 区分 | origin | 取り込み revision | 権限 | Codex 側 |
|---|---|---|---|---|---|
| `shared-agent` | shared | `dolquis/agent-ops` | 未記録 | read-only | なし |
| `reviewer` | repo | この repo | 本 repo で作成 | read-only | あり |
| `builder` | repo | この repo | 本 repo で作成 | build-write | あり |
"""

BODY = "# Reviewer\n\nRead `docs/spec.md` and `tsf-skill`.\n"

GUARD_HOOK = """\
hooks:
  PreToolUse:
    - matcher: Bash
      hooks:
        - type: command
          command: python3 "$CLAUDE_PROJECT_DIR/.claude/hooks/agent-readonly-guard.py"
"""


def claude_file(name: str, description: str, body: str, *, disallowed: bool = True, hook: bool = True) -> str:
    lines = [f"name: {name}", f"description: {description}", "tools: Read, Grep, Glob, Bash"]
    if disallowed:
        lines.append("disallowedTools: Edit, Write, NotebookEdit")
    lines.append("maxTurns: 10")
    frontmatter = "\n".join(lines) + "\n" + (GUARD_HOOK if hook else "")
    return f"---\n{frontmatter}---\n\n{body}"


def codex_file(name: str, description: str, body: str, sandbox: str) -> str:
    return (
        f'name = "{name}"\n'
        f'description = "{description}"\n'
        f'sandbox_mode = "{sandbox}"\n'
        f"developer_instructions = '''\n{body}'''\n"
    )


def write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def make_repo(root: Path, manifest: str = MANIFEST, config: str = "[agents]\nenabled = true\n") -> None:
    write(root / MODULE.MANIFEST_RELATIVE_PATH, manifest)
    write(root / MODULE.CODEX_CONFIG_PATH, config)
    write(root / "docs" / "spec.md", "")
    write(root / ".claude" / "skills" / "tsf-skill" / "SKILL.md", "")
    write(root / ".claude" / "agents" / "shared-agent.md", "---\nname: shared-agent\ndescription: shared\n---\nanything\n")
    write(root / ".claude" / "agents" / "reviewer.md", claude_file("reviewer", "review", BODY))
    write(root / ".codex" / "agents" / "reviewer.toml", codex_file("reviewer", "review", BODY, "read-only"))
    write(root / ".claude" / "agents" / "builder.md", claude_file("builder", "build", BODY, hook=False))
    write(root / ".codex" / "agents" / "builder.toml", codex_file("builder", "build", BODY, "workspace-write"))


class CheckAgentDefinitionsTest(unittest.TestCase):
    def test_consistent_repo_passes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            make_repo(root)
            self.assertEqual(MODULE.check(root), [])

    def test_body_and_description_drift_are_reported(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            make_repo(root)
            write(root / ".codex" / "agents" / "reviewer.toml", codex_file("reviewer", "review!", BODY + "extra\n", "read-only"))
            problems = MODULE.check(root)
            self.assertTrue(any("description が一致しない" in p for p in problems))
            self.assertTrue(any("本文が byte 一致しない" in p for p in problems))

    def test_missing_pair_and_manifest_mismatch_are_reported(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            make_repo(root, manifest=MANIFEST + "| `ghost` | repo | x | y | read-only | あり |\n")
            (root / ".codex" / "agents" / "reviewer.toml").unlink()
            write(root / ".claude" / "agents" / "orphan.md", "---\nname: orphan\n---\nx\n")
            problems = MODULE.check(root)
            self.assertTrue(any("`ghost`" in p for p in problems))
            self.assertTrue(any("`orphan`" in p for p in problems))
            self.assertTrue(any("reviewer.toml" in p and "Codex 側が無い" in p for p in problems))

    def test_read_only_contract_is_enforced_on_both_sides(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            make_repo(root)
            write(root / ".claude" / "agents" / "reviewer.md", claude_file("reviewer", "review", BODY, disallowed=False, hook=False))
            write(root / ".codex" / "agents" / "reviewer.toml", codex_file("reviewer", "review", BODY, "workspace-write"))
            problems = MODULE.check(root)
            self.assertTrue(any("disallowedTools" in p for p in problems))
            self.assertTrue(any("hooks.PreToolUse" in p for p in problems))
            self.assertTrue(any("read-only ではない" in p for p in problems))

    def test_build_write_needs_workspace_write(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            make_repo(root)
            write(root / ".codex" / "agents" / "builder.toml", codex_file("builder", "build", BODY, "read-only"))
            problems = MODULE.check(root)
            self.assertTrue(any("workspace-write ではない" in p for p in problems))

    def test_name_mismatch_and_missing_references_are_reported(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            make_repo(root)
            body = "See `docs/gone.md` and `azookey-missing-skill`.\n"
            write(root / ".claude" / "agents" / "reviewer.md", claude_file("other", "review", body))
            write(root / ".codex" / "agents" / "reviewer.toml", codex_file("reviewer", "review", body, "read-only"))
            problems = MODULE.check(root)
            self.assertTrue(any("name がファイル名" in p for p in problems))
            self.assertTrue(any("`docs/gone.md`" in p for p in problems))
            self.assertTrue(any("`azookey-missing-skill`" in p for p in problems))

    def test_codex_config_is_parsed_and_agents_stay_enabled(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            make_repo(root, config="[agents]\nenabled = false\n")
            self.assertTrue(any("[agents].enabled = false" in p for p in MODULE.check(root)))
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            make_repo(root, config="[agents\nenabled = true\n")
            self.assertTrue(any("parse できない" in p for p in MODULE.check(root)))

    def test_shared_agent_needs_only_claude_side(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            make_repo(root)
            (root / ".claude" / "agents" / "shared-agent.md").unlink()
            self.assertTrue(any("shared-agent.md が無い" in p for p in MODULE.check(root)))

    def test_real_repository_is_consistent(self) -> None:
        self.assertEqual(MODULE.check(REPO_ROOT), [])


if __name__ == "__main__":
    unittest.main()
