#!/usr/bin/env python3
"""Check custom agent definitions against `.claude/agents/MANIFEST.md`.

Claude Code reads `.claude/agents/<name>.md` (YAML frontmatter + Markdown body)
and Codex reads `.codex/agents/<name>.toml` (`developer_instructions`). Both are
supposed to carry the same instructions, and a read-only agent is supposed to be
read-only on both sides. Nothing enforces either when one file is edited without
the other, so this script does:

- every agent file appears in the manifest and every manifest row has a file;
- for `repo` agents the pair exists, `name` matches the file name on both
  sides, `description` matches, and the body matches byte for byte (Claude body
  below the frontmatter vs. Codex `developer_instructions`, both stripped);
- the manifest's 権限 column is honoured: `read-only` needs Claude
  `disallowedTools` (Edit, Write, NotebookEdit) plus a `hooks.PreToolUse` entry
  on `Bash` that runs `agent-readonly-guard.py`, and Codex
  `sandbox_mode = "read-only"`; `build-write` needs the Claude `disallowedTools`
  and Codex `workspace-write`; every `repo` agent lists its `tools` explicitly
  and leaves `Agent` out, so it cannot spawn subagents;
- back-quoted repository paths in `repo` bodies exist, and back-quoted tokens
  that look like repo-owned Skill names (`azookey-*`, `tsf-*`) are Skills;
- `.codex/config.toml` and every `.codex/agents/*.toml` parse, and
  `[agents].enabled` is not `false`.

`shared` agents are distributed from `dolquis/agent-ops`; only the Claude-side
file is required and its body is not compared, because a finding there could
only be fixed at the origin. The manifest, not this script, decides which
agents are shared.

Frontmatter is parsed with PyYAML when it is installed. Without it a line-based
fallback handles the layout used in this repo: two-space indentation, one
`key: value` per line, `tools` / `disallowedTools` as a comma list or an inline
`[a, b]` list, and the `hooks` block written as in `spec-drift-checker.md`.
"""

from __future__ import annotations

import argparse
import importlib.util
from pathlib import Path
import re
import sys
import tomllib

try:  # PyYAML is optional: the fallback parser below handles the canonical layout.
    import yaml
except ImportError:  # pragma: no cover - exercised in the test suite by forcing None
    yaml = None


sys.dont_write_bytecode = True
REPO_ROOT = Path(__file__).resolve().parents[1]
MANIFEST_RELATIVE_PATH = ".claude/agents/MANIFEST.md"
CLAUDE_AGENTS_DIRECTORY = ".claude/agents"
CODEX_AGENTS_DIRECTORY = ".codex/agents"
CODEX_CONFIG_PATH = ".codex/config.toml"
SKILLS_DIRECTORY = ".claude/skills"
GUARD_HOOK_NAME = "agent-readonly-guard.py"
REQUIRED_DISALLOWED_TOOLS = ("Edit", "Write", "NotebookEdit")
PERMISSIONS = ("read-only", "build-write")
CLASSIFICATIONS = ("shared", "repo")

MANIFEST_ROW_PATTERN = re.compile(
    r"^\|\s*`([^`]+)`\s*\|\s*([a-z-]+)\s*\|[^|]*\|[^|]*\|\s*([a-z-]+)"
)
FRONTMATTER_PATTERN = re.compile(r"\A---\n(.*?)\n---\n(.*)\Z", re.DOTALL)
BACKQUOTE_PATTERN = re.compile(r"`([^`\n]+)`")
REPO_SKILL_PATTERN = re.compile(r"^(azookey|tsf)-[a-z0-9-]+$")


def load_skill_reference_module():
    script = REPO_ROOT / "scripts" / "check_skill_references.py"
    spec = importlib.util.spec_from_file_location("check_skill_references", script)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def parse_manifest(text: str) -> dict[str, tuple[str, str]]:
    """Return {agent name: (classification, permission)} from the manifest table."""
    rows: dict[str, tuple[str, str]] = {}
    for line in text.splitlines():
        match = MANIFEST_ROW_PATTERN.match(line)
        if match:
            rows[match.group(1)] = (match.group(2), match.group(3))
    return rows


def split_frontmatter(text: str) -> tuple[str, str] | None:
    match = FRONTMATTER_PATTERN.match(text)
    if not match:
        return None
    return match.group(1), match.group(2)


def parse_frontmatter(frontmatter: str) -> dict:
    """Return the frontmatter as a dict, via PyYAML or the line-based fallback."""
    if yaml is not None:
        loaded = yaml.safe_load(frontmatter)
        return loaded if isinstance(loaded, dict) else {}
    return fallback_parse(frontmatter)


def fallback_parse(frontmatter: str) -> dict:
    """Parse the canonical layout without PyYAML.

    Top-level `key: value` lines become strings. A `hooks:` block is reduced to
    {"PreToolUse": [{"matcher": ..., "hooks": [{"command": ...}]}]} by treating
    each `- matcher:` line as a new entry and attaching the `command:` lines that
    follow it, until the next entry or the next top-level key.
    """
    data: dict = {}
    hooks: dict[str, list[dict]] = {}
    event: str | None = None
    entry: dict | None = None
    in_hooks = False
    for raw in frontmatter.splitlines():
        if not raw.strip() or raw.lstrip().startswith("#"):
            continue
        indent = len(raw) - len(raw.lstrip(" "))
        line = raw.strip()
        if indent == 0:
            key, separator, value = line.partition(":")
            if not separator:
                continue
            key = key.strip()
            in_hooks = key == "hooks"
            event = entry = None
            if not in_hooks:
                data[key] = value.strip()
            continue
        if not in_hooks:
            continue
        if indent == 2 and line.endswith(":"):
            event = line[:-1]
            hooks.setdefault(event, [])
            entry = None
            continue
        if event is None:
            continue
        if line.startswith("- "):
            key, _, value = line[2:].partition(":")
            if key.strip() == "matcher":
                entry = {"matcher": value.strip().strip("\"'"), "hooks": []}
                hooks[event].append(entry)
            elif key.strip() == "type" and entry is not None:
                entry["hooks"].append({})
            continue
        key, separator, value = line.partition(":")
        if separator and entry is not None and key.strip() == "command":
            if not entry["hooks"]:
                entry["hooks"].append({})
            entry["hooks"][-1]["command"] = value.strip().strip("\"'")
    if hooks:
        data["hooks"] = hooks
    return data


def as_tool_set(value) -> set[str]:
    """Normalise `tools` / `disallowedTools` written as a comma string or a list."""
    if value is None:
        return set()
    if isinstance(value, str):
        value = value.strip().strip("[]").split(",")
    return {str(item).strip().strip("\"'") for item in value if str(item).strip()}


def declares_guard_hook(frontmatter: dict) -> bool:
    """True when one hooks.PreToolUse entry matches Bash and runs the guard script."""
    hooks = frontmatter.get("hooks")
    if not isinstance(hooks, dict):
        return False
    for entry in hooks.get("PreToolUse") or []:
        if not isinstance(entry, dict) or str(entry.get("matcher", "")).strip("\"'") != "Bash":
            continue
        for hook in entry.get("hooks") or []:
            if isinstance(hook, dict) and GUARD_HOOK_NAME in str(hook.get("command", "")):
                return True
    return False


def check_references(body: str, relative: str, repo_root: Path, paths_module) -> list[str]:
    problems: list[str] = []
    seen: set[str] = set()
    skills_dir = repo_root / SKILLS_DIRECTORY
    for match in BACKQUOTE_PATTERN.finditer(body):
        token = match.group(1).strip()
        if token in seen:
            continue
        seen.add(token)
        if paths_module.is_repository_path(token):
            if not (repo_root / token.rstrip("/")).exists():
                problems.append(f"{relative}: 存在しないパス `{token}`")
        elif REPO_SKILL_PATTERN.match(token) and not (skills_dir / token).is_dir() and not (repo_root / token).is_dir():
            problems.append(f"{relative}: 存在しない Skill `{token}`")
    return problems


def check_pair(
    name: str,
    permission: str,
    claude_path: Path,
    codex_path: Path,
    repo_root: Path,
    paths_module,
) -> list[str]:
    problems: list[str] = []
    claude_rel = claude_path.relative_to(repo_root).as_posix()
    codex_rel = codex_path.relative_to(repo_root).as_posix()

    parts = split_frontmatter(claude_path.read_text(encoding="utf-8"))
    if parts is None:
        return [f"{claude_rel}: frontmatter（--- で囲む YAML）が無い"]
    frontmatter_text, claude_body = parts
    try:
        frontmatter = parse_frontmatter(frontmatter_text)
    except Exception as error:  # yaml.YAMLError or a fallback slip
        return [f"{claude_rel}: frontmatter を parse できない: {error}"]
    try:
        codex = tomllib.loads(codex_path.read_text(encoding="utf-8"))
    except tomllib.TOMLDecodeError as error:
        return [f"{codex_rel}: TOML として parse できない: {error}"]

    if str(frontmatter.get("name", "")).strip() != name:
        problems.append(f"{claude_rel}: frontmatter の name がファイル名 `{name}` と違う")
    if codex.get("name") != name:
        problems.append(f"{codex_rel}: name がファイル名 `{name}` と違う")
    if str(frontmatter.get("description", "")).strip() != str(codex.get("description", "")).strip():
        problems.append(f"{claude_rel} / {codex_rel}: description が一致しない")

    codex_body = codex.get("developer_instructions")
    if not isinstance(codex_body, str):
        problems.append(f"{codex_rel}: developer_instructions が無い")
    elif claude_body.strip() != codex_body.strip():
        problems.append(f"{claude_rel} / {codex_rel}: 本文が byte 一致しない")

    tools = as_tool_set(frontmatter.get("tools"))
    if not tools:
        problems.append(f"{claude_rel}: tools を明示していない（allowlist が無いと Agent を含む全ツールを継承する）")
    elif "Agent" in tools:
        problems.append(f"{claude_rel}: tools に Agent がある（reviewer / runner は subagent を spawn しない）")
    disallowed = as_tool_set(frontmatter.get("disallowedTools"))
    missing = [tool for tool in REQUIRED_DISALLOWED_TOOLS if tool not in disallowed]
    if missing:
        problems.append(f"{claude_rel}: disallowedTools に {', '.join(missing)} が無い")
    sandbox = codex.get("sandbox_mode")
    if permission == "read-only":
        if not declares_guard_hook(frontmatter):
            problems.append(
                f"{claude_rel}: hooks.PreToolUse（matcher Bash、{GUARD_HOOK_NAME}）が無い"
            )
        if sandbox != "read-only":
            problems.append(f"{codex_rel}: sandbox_mode が read-only ではない（{sandbox!r}）")
    elif permission == "build-write":
        if sandbox != "workspace-write":
            problems.append(f"{codex_rel}: sandbox_mode が workspace-write ではない（{sandbox!r}）")

    problems.extend(check_references(claude_body, claude_rel, repo_root, paths_module))
    return problems


def check_codex_config(repo_root: Path) -> list[str]:
    config_path = repo_root / CODEX_CONFIG_PATH
    if not config_path.exists():
        return [f"{CODEX_CONFIG_PATH}: 存在しない"]
    try:
        config = tomllib.loads(config_path.read_text(encoding="utf-8"))
    except tomllib.TOMLDecodeError as error:
        return [f"{CODEX_CONFIG_PATH}: TOML として parse できない: {error}"]
    agents = config.get("agents", {})
    if isinstance(agents, dict) and agents.get("enabled") is False:
        return [f"{CODEX_CONFIG_PATH}: [agents].enabled = false は subagent を無効化する"]
    return []


def check(repo_root: Path) -> list[str]:
    manifest_path = repo_root / MANIFEST_RELATIVE_PATH
    if not manifest_path.exists():
        return [f"{MANIFEST_RELATIVE_PATH}: 存在しない"]
    rows = parse_manifest(manifest_path.read_text(encoding="utf-8"))
    claude_dir = repo_root / CLAUDE_AGENTS_DIRECTORY
    codex_dir = repo_root / CODEX_AGENTS_DIRECTORY
    problems: list[str] = []

    claude_present = {path.stem for path in claude_dir.glob("*.md") if path.name != "MANIFEST.md"}
    codex_present = {path.stem for path in codex_dir.glob("*.toml")} if codex_dir.exists() else set()
    for name in sorted((claude_present | codex_present) - rows.keys()):
        problems.append(f"{MANIFEST_RELATIVE_PATH}: 表に無い agent `{name}`")
    for name in sorted(rows.keys() - claude_present):
        problems.append(f"{MANIFEST_RELATIVE_PATH}: `{name}` の {CLAUDE_AGENTS_DIRECTORY}/{name}.md が無い")

    for name, (classification, permission) in sorted(rows.items()):
        if classification not in CLASSIFICATIONS:
            problems.append(f"{MANIFEST_RELATIVE_PATH}: `{name}` の区分 `{classification}` は未定義")
            continue
        if permission not in PERMISSIONS:
            problems.append(f"{MANIFEST_RELATIVE_PATH}: `{name}` の権限 `{permission}` は未定義")
            continue
        if name not in claude_present:
            continue
        if classification == "shared":
            codex_path = codex_dir / f"{name}.toml"
            if codex_path.exists():
                try:
                    codex = tomllib.loads(codex_path.read_text(encoding="utf-8"))
                except tomllib.TOMLDecodeError as error:
                    problems.append(f"{codex_path.relative_to(repo_root).as_posix()}: TOML として parse できない: {error}")
                    continue
                if codex.get("name") != name:
                    problems.append(f"{codex_path.relative_to(repo_root).as_posix()}: name がファイル名 `{name}` と違う")
            continue
        codex_path = codex_dir / f"{name}.toml"
        if not codex_path.exists():
            problems.append(f"{CODEX_AGENTS_DIRECTORY}/{name}.toml: repo 所有 agent の Codex 側が無い")
            continue
        problems.extend(
            check_pair(name, permission, claude_dir / f"{name}.md", codex_path, repo_root, load_skill_reference_module())
        )

    problems.extend(check_codex_config(repo_root))
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=REPO_ROOT,
        help="検査対象のリポジトリルート（既定: このスクリプトの親ディレクトリ）",
    )
    arguments = parser.parse_args(argv)
    problems = check(arguments.repo_root.resolve())
    if problems:
        print("agent 定義が MANIFEST と一致しません:", file=sys.stderr)
        for problem in problems:
            print(f"  - {problem}", file=sys.stderr)
        print(
            "\nClaude / Codex の本文と権限を揃えるか、"
            f"{MANIFEST_RELATIVE_PATH} の区分・権限を見直してください。",
            file=sys.stderr,
        )
        return 1
    print("agent 定義は MANIFEST と一致しています。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
