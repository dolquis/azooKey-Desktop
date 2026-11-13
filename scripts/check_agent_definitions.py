#!/usr/bin/env python3
"""Check custom agent definitions for Claude Code and Codex against one config.

Claude Code reads `.claude/agents/<name>.md` (YAML frontmatter + Markdown body)
and Codex reads `.codex/agents/<name>.toml` (`developer_instructions`). Both are
meant to carry the same role. This static check does not prove runtime
permissions or that an agent follows its instructions.

The repo-specific values live in `scripts/check_agent_definitions.toml`
(`--config` points elsewhere). A missing or malformed config is a setup error.

Always-on checks (category in brackets):

- [pair] each agent in the config has both files, unless `[mirror]` lists it
  as one-sided; a one-sided agent must not have the other file; an agent with
  no file at all is a finding; agent files that the config does not list are
  findings, and so is a config with no agents unless it says `agents = {}`.
- [name] / [description] / [body] the frontmatter `name`, the TOML `name` and
  the config key agree; the two descriptions agree after strip; the Claude body
  and `developer_instructions` agree after the `[body]` normalisation.
- [tools] the Claude side lists `tools` explicitly (without it every tool,
  `Agent` included, is inherited) and the list leaves out the subagent tools:
  `Agent`, its older name `Task`, and the `Agent(...)` / `Task(...)` forms.
- [codex] the TOML parses; `name`, `description` and `developer_instructions`
  are non-empty strings; the optional model keys are non-empty strings; the
  session keys in `CODEX_SESSION_KEYS` are rejected, because a custom agent
  inherits sandbox and approval from the parent turn and cannot override them.
- [frontmatter] the frontmatter is the supported layout (see `parse_frontmatter`).

Checks the config turns on:

- [boundary] `write_boundary` per agent. Every value forbids writing tools in
  `tools`. With `[boundary].controls` (default true) `read-only` also needs the
  Claude `disallowedTools` (Edit, Write, NotebookEdit) and a `hooks.PreToolUse`
  entry on `Bash` whose command runs `[boundary].guard_script` (default
  `.claude/hooks/agent-readonly-guard.py`), and that script must exist;
  `build-write` and `target-only` need the `disallowedTools` only.
  `controls = false` is a migration switch for repositories that have not
  installed the guard yet; every run then prints a warning naming the agents
  whose controls were not checked, pass or fail.
  `require_declaration` makes the key mandatory; `heading` requires a heading
  line in the Claude body.
- [frontmatter] `[frontmatter].forbidden` keys.
- [limits] a Codex config value equals a Claude settings env value.
- [reference] back-quoted repository paths in either body exist. A token counts
  as a path when it has no whitespace or `<>{}` placeholder, contains `/`, and
  its first segment is one of `[references].roots` (`"auto"` = the top-level
  directories of the repository, minus `.git` and `exclude`). A token with `*`
  is a glob and passes when it matches something.
- [skill] back-quoted repo-owned Skill names exist as `<skills_dir>/<name>/`.
  A token is treated as a Skill name only when it is a whole back-quoted
  kebab-case token that starts with one of `[skills].prefixes`. Bare kebab-case
  is not enough: agent bodies quote `read-only`, `docs-lint`, `merge-base` and
  `git-clang-format`, which are not Skills, so treating every kebab-case token
  as a Skill would fail on ordinary prose. The prefixes name the families the
  repository owns (for example `azookey-`), which are the ones a rename or a
  removal in the same repository can break; shared Skill names come from the
  origin and are not listed. A token that is also a top-level directory (such
  as `tsf-tip`) is a path, not a Skill.
- [manifest] the agent names in the first column of a Markdown table in
  `[manifest].path` equal the agents in the config.
- [codex-config] `[codex_config].path` parses and selected `[agents]` keys have
  the right types; `sandbox_private_desktop` is rejected as unsupported.

Exit status: 0 when clean (warnings may still be printed), 1 on any finding,
2 on a setup error: the config is missing, cannot be read or parsed, has an
unknown section or key, an empty section or value, or a value of the wrong
type; or an agent file cannot be read (missing files are findings, not setup
errors).
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys
import tomllib


sys.dont_write_bytecode = True
REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_CONFIG = Path("scripts/check_agent_definitions.toml")
DEFAULT_LAYOUT = {
    "claude_dir": ".claude/agents",
    "codex_dir": ".codex/agents",
    "skills_dir": ".claude/skills",
}
# Markdown files in the Claude agents directory that are not agents.
NON_AGENT_FILES = frozenset({"MANIFEST.md", "README.md", "AGENTS.md", "CLAUDE.md"})
WRITE_BOUNDARIES = ("read-only", "build-write", "target-only")
WRITING_TOOLS = frozenset({"Edit", "Write", "MultiEdit", "NotebookEdit"})
REQUIRED_DISALLOWED_TOOLS = ("Edit", "Write", "NotebookEdit")
DEFAULT_GUARD_SCRIPT = ".claude/hooks/agent-readonly-guard.py"
# Session settings that a custom agent inherits from the parent turn.
CODEX_SESSION_KEYS = (
    "sandbox_mode", "sandbox_workspace_write", "approval_policy", "windows",
    "model_provider", "mcp_servers", "sandbox_private_desktop",
)
CODEX_REQUIRED_STRINGS = ("name", "description", "developer_instructions")
CODEX_OPTIONAL_STRINGS = ("model", "model_reasoning_effort", "model_reasoning_summary", "model_verbosity")
NORMALIZE_MODES = ("strip", "nonblank-lines")
SUBAGENT_TOOLS = frozenset({"Agent", "Task"})
# Sections that do nothing without these keys; an empty section is a setup error.
REQUIRED_KEYS: dict[str, tuple[str, ...]] = {
    "frontmatter": ("forbidden",),
    "limits": ("codex_config", "codex_key", "claude_settings", "claude_env"),
    "skills": ("prefixes",),
    "manifest": ("path",),
    "codex_config": ("path",),
}
# Known config sections and their keys. `agents` holds one table per agent.
AGENT_KEYS = frozenset({"write_boundary", "claude", "codex", "copies"})
CONFIG_SCHEMA: dict[str, frozenset[str]] = {
    "agents": AGENT_KEYS,
    "layout": frozenset(DEFAULT_LAYOUT),
    "mirror": frozenset({"claude_only", "codex_only"}),
    "body": frozenset({"normalize", "replace"}),
    "boundary": frozenset({"controls", "require_declaration", "heading", "guard_script"}),
    "frontmatter": frozenset({"forbidden"}),
    "limits": frozenset({"codex_config", "codex_key", "claude_settings", "claude_env"}),
    "references": frozenset({"roots", "exclude"}),
    "skills": frozenset({"prefixes"}),
    "manifest": frozenset({"path"}),
    "codex_config": frozenset({"path"}),
}
DESCRIPTION = "Check custom agent definitions for Claude Code and Codex against one config."

FRONTMATTER_PATTERN = re.compile(r"\A---\n(.*?)\n---\n(.*)\Z", re.DOTALL)
BACKQUOTE_PATTERN = re.compile(r"`([^`\n]+)`")
KEBAB_PATTERN = re.compile(r"^[a-z0-9]+(?:-[a-z0-9]+)+$")
MANIFEST_ROW_PATTERN = re.compile(r"^\|\s*`([^`]+)`\s*\|")


class SetupError(Exception):
    """The config or the command line cannot be used; exit status 2."""


class Finding:
    def __init__(self, category: str, location: str, message: str) -> None:
        self.category = category
        self.location = location
        self.message = message

    def __str__(self) -> str:
        return f"{self.category}: {self.location}: {self.message}"


# ---------------------------------------------------------------- frontmatter


def parse_frontmatter(text: str) -> tuple[dict, str]:
    """Split and parse the frontmatter without PyYAML; raise ValueError if unsupported.

    Supported layout: top-level `key: value` lines (one pair of surrounding
    quotes is stripped, `[a, b]` stays a string for `as_tool_set`), `#` comment
    lines, `tools` / `disallowedTools` as an indented `- item` list, and a
    `hooks:` block written as
    `hooks: / PreToolUse: / - matcher: Bash / hooks: / - type: command / command: ...`
    with two-space indentation. Block scalars (`|`, `>`) and duplicate keys
    are rejected rather than read wrongly. Other indented blocks are kept as
    present keys with an empty value.
    """
    match = FRONTMATTER_PATTERN.match(text)
    if match is None:
        raise ValueError("frontmatter（--- で始まり --- で閉じる）が無い")
    frontmatter, body = match.group(1), match.group(2)
    meta: dict = {}
    blocks: dict[str, list[str]] = {}
    current: str | None = None
    for raw in frontmatter.splitlines():
        if not raw.strip() or raw.lstrip().startswith("#"):
            continue
        if raw[0] in " \t":
            if current is None:
                raise ValueError(f"key の無い字下げ行: {raw.strip()}")
            blocks[current].append(raw)
            continue
        key, separator, value = raw.partition(":")
        key, value = key.strip(), value.strip()
        if not separator or not key:
            raise ValueError(f"key: value の形でない行: {raw.strip()}")
        if key in meta:
            raise ValueError(f"key `{key}` が重複している")
        if value[:1] in ("|", ">"):
            raise ValueError(f"`{key}` の block scalar は未対応（1 行で書く）")
        if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
            value = value[1:-1]
        meta[key] = value
        blocks[key] = []
        current = key
    if blocks.get("hooks"):
        meta["hooks"] = parse_hooks_block(blocks["hooks"])
    for key in ("tools", "disallowedTools"):
        lines = [line.strip() for line in blocks.get(key, [])]
        if lines and not meta[key] and all(line.startswith("- ") for line in lines):
            meta[key] = ", ".join(line[2:].strip() for line in lines)
    return meta, body


def parse_hooks_block(lines: list[str]) -> dict[str, list[dict]]:
    """Reduce the hooks block to {event: [{"matcher": ..., "hooks": [{"command": ...}]}]}."""
    hooks: dict[str, list[dict]] = {}
    event: str | None = None
    entry: dict | None = None
    for raw in lines:
        indent = len(raw) - len(raw.lstrip(" "))
        line = raw.strip()
        if indent == 2 and line.endswith(":") and not line.startswith("- "):
            event = line[:-1].strip()
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
    return hooks


def as_tool_set(value) -> set[str]:
    """Normalise `tools` / `disallowedTools` written as `a, b` or `[a, b]`."""
    if not isinstance(value, str):
        return set()
    return {item.strip().strip("\"'") for item in value.strip().strip("[]").split(",") if item.strip()}


def declares_guard_hook(meta: dict, guard_script: str) -> bool:
    """True when one hooks.PreToolUse entry matches Bash and its command runs `guard_script`.

    The path must appear as a whole path segment run (preceded by the start, a
    quote, whitespace or `/`, as in `"$CLAUDE_PROJECT_DIR/.claude/hooks/x.py"`),
    so a hook that runs a different script with a similar name does not count.
    """
    hooks = meta.get("hooks")
    if not isinstance(hooks, dict):
        return False
    pattern = re.compile(r"(?:^|[\s\"'/])" + re.escape(guard_script.strip("/")) + r"(?:$|[\s\"'])")
    for entry in hooks.get("PreToolUse") or []:
        if entry.get("matcher") != "Bash":
            continue
        if any(pattern.search(hook.get("command", "")) for hook in entry.get("hooks") or []):
            return True
    return False


def is_subagent_tool(tool: str) -> bool:
    return tool in SUBAGENT_TOOLS or tool.startswith(tuple(f"{name}(" for name in SUBAGENT_TOOLS))


# --------------------------------------------------------------------- config


def load_config(root: Path, config_path: Path | None) -> tuple[dict, Path]:
    path = config_path if config_path is not None else root / DEFAULT_CONFIG
    if not path.is_absolute():
        path = root / path
    if not path.is_file():
        raise SetupError(f"設定ファイルが無い: {path}")
    try:
        config = tomllib.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, tomllib.TOMLDecodeError) as error:
        raise SetupError(f"設定ファイルを読めない: {path}: {error}") from error
    validate_config(config)
    return config, path


def is_string_list(value, *, allow_empty: bool = True) -> bool:
    """A list of non-empty strings; `allow_empty` decides whether `[]` itself is accepted."""
    return (isinstance(value, list) and (allow_empty or bool(value))
            and all(isinstance(item, str) and item.strip() for item in value))


def is_nonempty_string(value) -> bool:
    return isinstance(value, str) and bool(value.strip())


def unknown_config_keys(config: dict) -> list[str]:
    """Return unknown sections and keys, so a misspelling cannot switch a check off."""
    unknown = [f"[{key}]" for key in config if key not in CONFIG_SCHEMA]
    for section, allowed in CONFIG_SCHEMA.items():
        value = config.get(section)
        if not isinstance(value, dict):
            continue
        if section == "agents":
            for name, spec in value.items():
                if isinstance(spec, dict):
                    unknown += [f"[agents.{name}].{key}" for key in spec if key not in AGENT_KEYS]
            continue
        unknown += [f"[{section}].{key}" for key in value if key not in allowed]
    return unknown


def validate_config(config: dict) -> None:
    """Reject config values the checks would otherwise misread or silently skip."""
    unknown = unknown_config_keys(config)
    if unknown:
        raise SetupError(f"設定に未知の節・キーがある: {', '.join(unknown)}")

    def table(key: str) -> dict:
        value = config.get(key, {})
        if not isinstance(value, dict):
            raise SetupError(f"[{key}] は table が必要")
        return value

    for section in config:
        # `agents = {}` declares zero agents; an empty `[references]` means roots = "auto".
        if section not in ("agents", "references") and table(section) == {}:
            raise SetupError(f"[{section}] が空（不要なら節ごと消す）")
    for section, keys in REQUIRED_KEYS.items():
        if section in config:
            missing = [key for key in keys if key not in table(section)]
            if missing:
                raise SetupError(f"[{section}] に必須キー {', '.join(missing)} が無い")

    agents = table("agents")
    for name, spec in agents.items():
        if not isinstance(spec, dict):
            raise SetupError(f"[agents.{name}] は table が必要")
        boundary = spec.get("write_boundary")
        if boundary is not None and boundary not in WRITE_BOUNDARIES:
            raise SetupError(f"[agents.{name}].write_boundary は {', '.join(WRITE_BOUNDARIES)} のどれか")
        for key in ("claude", "codex"):
            if key in spec and not is_nonempty_string(spec[key]):
                raise SetupError(f"[agents.{name}].{key} は空でないパス文字列が必要")
        if not is_string_list(spec.get("copies", [])):
            raise SetupError(f"[agents.{name}].copies は空でないパス文字列の配列が必要")
    for key, value in table("layout").items():
        if not is_nonempty_string(value):
            raise SetupError(f"[layout].{key} は空でない文字列が必要")
    mirror = table("mirror")
    for key in ("claude_only", "codex_only"):
        # `[]` is allowed: it records that no asymmetry is intended.
        if not is_string_list(mirror.get(key, [])):
            raise SetupError(f"[mirror].{key} は空でない文字列の配列が必要")
        for name in mirror.get(key, []):
            if name not in agents:
                raise SetupError(f"[mirror].{key} の `{name}` が [agents] に無い")
    both = set(mirror.get("claude_only", [])) & set(mirror.get("codex_only", []))
    if both:
        raise SetupError(f"[mirror] の claude_only と codex_only の両方にある: {', '.join(sorted(both))}")
    body = table("body")
    if body.get("normalize", "strip") not in NORMALIZE_MODES:
        raise SetupError(f"[body].normalize は {', '.join(NORMALIZE_MODES)} のどれか")
    replace = body.get("replace", [])
    if not isinstance(replace, list) or not all(
            isinstance(pair, list) and len(pair) == 2 and all(isinstance(item, str) for item in pair)
            and is_nonempty_string(pair[0]) for pair in replace):
        raise SetupError("[body].replace は [from, to] の配列が必要（from は空にしない）")
    boundary = table("boundary")
    for key in ("controls", "require_declaration"):
        if key in boundary and type(boundary[key]) is not bool:
            raise SetupError(f"[boundary].{key} は boolean が必要")
    for key in ("heading", "guard_script"):
        if key in boundary and not is_nonempty_string(boundary[key]):
            raise SetupError(f"[boundary].{key} は空でない文字列が必要")
    if "frontmatter" in config and not is_string_list(table("frontmatter")["forbidden"], allow_empty=False):
        raise SetupError("[frontmatter].forbidden は空でない文字列の配列が必要")
    limits = table("limits")
    for key in REQUIRED_KEYS["limits"]:
        if key in limits and not is_nonempty_string(limits[key]):
            raise SetupError(f"[limits].{key} は空でない文字列が必要")
    references = table("references")
    roots = references.get("roots", "auto")
    if roots != "auto" and not is_string_list(roots, allow_empty=False):
        raise SetupError('[references].roots は "auto" か空でない文字列の配列が必要')
    if not is_string_list(references.get("exclude", [])):
        raise SetupError("[references].exclude は空でない文字列の配列が必要")
    if "skills" in config and not is_string_list(table("skills")["prefixes"], allow_empty=False):
        raise SetupError("[skills].prefixes は空でない文字列の配列が必要")
    for key in ("manifest", "codex_config"):
        section = table(key)
        if "path" in section and not is_nonempty_string(section["path"]):
            raise SetupError(f"[{key}].path は空でない文字列が必要")


def layout(config: dict, key: str) -> str:
    return config.get("layout", {}).get(key, DEFAULT_LAYOUT[key])


def relative(path: Path, root: Path) -> str:
    try:
        return path.relative_to(root).as_posix()
    except ValueError:
        return path.as_posix()


# --------------------------------------------------------------------- checks


def normalize_body(body: str, config: dict) -> str | list[str]:
    options = config.get("body", {})
    for old, new in options.get("replace", []):
        body = body.replace(old, new)
    if options.get("normalize", "strip") == "nonblank-lines":
        return [line.strip() for line in body.splitlines() if line.strip()]
    return body.strip()


def check_codex_agent(codex: dict, location: str) -> list[Finding]:
    findings: list[Finding] = []
    for key in CODEX_REQUIRED_STRINGS:
        if not isinstance(codex.get(key), str) or not codex[key].strip():
            findings.append(Finding("codex", location, f"{key} は空でない文字列が必要"))
    for key in CODEX_OPTIONAL_STRINGS:
        if key in codex and (not isinstance(codex[key], str) or not codex[key].strip()):
            findings.append(Finding("codex", location, f"{key} は空でない文字列が必要"))
    for key in CODEX_SESSION_KEYS:
        if key in codex:
            findings.append(Finding("codex", location, f"{key} は custom agent で上書きできない（親の設定を継承）"))
    return findings


def check_claude_agent(name: str, meta: dict, body: str, location: str, root: Path, config: dict) -> list[Finding]:
    findings: list[Finding] = []
    if meta.get("name") != name:
        findings.append(Finding("name", location, f"frontmatter の name {meta.get('name')!r} が `{name}` と違う"))
    for key in config.get("frontmatter", {}).get("forbidden", []):
        if key in meta:
            findings.append(Finding("frontmatter", location, f"{key} を置かない"))
    tools = as_tool_set(meta.get("tools"))
    if not tools:
        findings.append(Finding("tools", location, "tools を明示していない（allowlist が無いと Agent を含む全ツールを継承する）"))
    elif any(is_subagent_tool(tool) for tool in tools):
        spawning = ", ".join(sorted(tool for tool in tools if is_subagent_tool(tool)))
        findings.append(Finding("tools", location, f"tools に subagent ツール {spawning} がある（subagent を spawn しない）"))

    options = config.get("boundary", {})
    boundary = config.get("agents", {}).get(name, {}).get("write_boundary")
    if boundary is None:
        if options.get("require_declaration", False):
            findings.append(Finding("boundary", location, "write_boundary の宣言が設定に無い"))
    else:
        if tools & WRITING_TOOLS:
            findings.append(Finding("boundary", location, f"tools に書込みツール {', '.join(sorted(tools & WRITING_TOOLS))} がある"))
        if options.get("controls", True):
            disallowed = as_tool_set(meta.get("disallowedTools"))
            missing = [tool for tool in REQUIRED_DISALLOWED_TOOLS if tool not in disallowed]
            if missing:
                findings.append(Finding("boundary", location, f"disallowedTools に {', '.join(missing)} が無い"))
            if boundary == "read-only":
                guard = options.get("guard_script", DEFAULT_GUARD_SCRIPT)
                if not declares_guard_hook(meta, guard):
                    findings.append(Finding("boundary", location, f"hooks.PreToolUse（matcher Bash、command が `{guard}` を実行）が無い"))
                if not (root / guard).is_file():
                    findings.append(Finding("boundary", location, f"guard script `{guard}` が無い"))
    heading = options.get("heading")
    if heading and not any(line.strip() == heading for line in body.splitlines()):
        findings.append(Finding("boundary", location, f"本文に「{heading}」見出しが無い"))
    return findings


def reference_roots(root: Path, config: dict) -> set[str]:
    references = config.get("references", {})
    roots = references.get("roots", "auto")
    if roots == "auto":
        exclude = set(references.get("exclude", [])) | {".git"}
        return {entry.name for entry in root.iterdir() if entry.is_dir() and entry.name not in exclude}
    return set(roots)


def check_references(body: str, location: str, root: Path, config: dict, roots: set[str] | None) -> list[Finding]:
    findings: list[Finding] = []
    prefixes = tuple(config.get("skills", {}).get("prefixes", []))
    skills_dir = root / layout(config, "skills_dir")
    top_dirs = {entry.name for entry in root.iterdir() if entry.is_dir()}
    seen: set[str] = set()
    for match in BACKQUOTE_PATTERN.finditer(body):
        token = match.group(1).strip()
        if token in seen:
            continue
        seen.add(token)
        if roots is not None and is_repository_path(token, roots):
            pattern = token.rstrip("/")
            exists = any(root.glob(pattern)) if "*" in pattern else (root / pattern).exists()
            if not exists:
                findings.append(Finding("reference", location, f"存在しないパス `{token}`"))
        elif prefixes and KEBAB_PATTERN.match(token) and token.startswith(prefixes) and token not in top_dirs:
            if not (skills_dir / token).is_dir():
                findings.append(Finding("skill", location, f"存在しない Skill `{token}`"))
    return findings


def is_repository_path(token: str, roots: set[str]) -> bool:
    if any(character in token for character in "<>{}") or any(character.isspace() for character in token):
        return False
    head, separator, _ = token.rstrip("/").partition("/")
    return bool(separator) and head in roots


def check_limits(root: Path, config: dict) -> list[Finding]:
    limits = config.get("limits", {})
    if not limits:
        return []
    location = f"{limits['codex_config']} / {limits['claude_settings']}"
    try:
        codex = tomllib.loads((root / limits["codex_config"]).read_text(encoding="utf-8"))
        settings = json.loads((root / limits["claude_settings"]).read_text(encoding="utf-8"))
    except (OSError, UnicodeError, ValueError) as error:
        return [Finding("limits", location, f"設定を読めない: {error}")]
    codex_value: object = codex
    for part in limits["codex_key"].split("."):
        codex_value = codex_value.get(part) if isinstance(codex_value, dict) else None
    env = settings.get("env") if isinstance(settings, dict) else None
    claude_value = env.get(limits["claude_env"]) if isinstance(env, dict) else None
    findings: list[Finding] = []
    if codex_value is None:
        findings.append(Finding("limits", location, f"{limits['codex_key']} が無い"))
    if claude_value is None:
        findings.append(Finding("limits", location, f"env に {limits['claude_env']} が無い"))
    if not findings and str(codex_value) != str(claude_value):
        findings.append(Finding("limits", location, f"{limits['codex_key']}={codex_value} と {limits['claude_env']}={claude_value} が一致しない"))
    return findings


def check_manifest(root: Path, config: dict) -> list[Finding]:
    manifest = config.get("manifest", {})
    if not manifest:
        return []
    location = manifest["path"]
    try:
        text = (root / location).read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        return [Finding("manifest", location, f"読めない: {error}")]
    rows = {match.group(1) for match in map(MANIFEST_ROW_PATTERN.match, text.splitlines()) if match}
    configured = set(config.get("agents", {}))
    findings = [Finding("manifest", location, f"表にあるが設定に無い agent `{name}`") for name in sorted(rows - configured)]
    findings += [Finding("manifest", location, f"設定にあるが表に無い agent `{name}`") for name in sorted(configured - rows)]
    return findings


def check_codex_config(root: Path, config: dict) -> list[Finding]:
    section = config.get("codex_config", {})
    if not section:
        return []
    location = section["path"]
    try:
        codex = tomllib.loads((root / location).read_text(encoding="utf-8"))
    except (OSError, UnicodeError, tomllib.TOMLDecodeError) as error:
        return [Finding("codex-config", location, f"読めない: {error}")]
    findings: list[Finding] = []
    agents = codex.get("agents", {})
    if not isinstance(agents, dict):
        findings.append(Finding("codex-config", location, "agents は table が必要"))
    else:
        for key in ("enabled", "interrupt_message"):
            if key in agents and type(agents[key]) is not bool:
                findings.append(Finding("codex-config", location, f"[agents].{key} は boolean が必要"))
        for key in ("max_concurrent_threads_per_session", "max_threads"):
            if key in agents and (type(agents[key]) is not int or agents[key] < 1):
                findings.append(Finding("codex-config", location, f"[agents].{key} は正の整数が必要"))
        for key in ("default_subagent_model", "default_subagent_reasoning_effort"):
            if key in agents and (not isinstance(agents[key], str) or not agents[key].strip()):
                findings.append(Finding("codex-config", location, f"[agents].{key} は空でない文字列が必要"))
    windows = codex.get("windows", {})
    if "sandbox_private_desktop" in codex or (isinstance(windows, dict) and "sandbox_private_desktop" in windows):
        findings.append(Finding("codex-config", location, "sandbox_private_desktop は未対応の設定"))
    return findings


def agent_paths(name: str, root: Path, config: dict) -> tuple[Path, Path]:
    spec = config.get("agents", {}).get(name, {})
    claude = spec.get("claude", f"{layout(config, 'claude_dir')}/{name}.md")
    codex = spec.get("codex", f"{layout(config, 'codex_dir')}/{name}.toml")
    return root / claude, root / codex


def check_agent(name: str, root: Path, config: dict, roots: set[str] | None) -> list[Finding]:
    findings: list[Finding] = []
    claude_path, codex_path = agent_paths(name, root, config)
    claude_rel, codex_rel = relative(claude_path, root), relative(codex_path, root)
    mirror = config.get("mirror", {})
    has_claude, has_codex = claude_path.is_file(), codex_path.is_file()
    if not has_claude and name not in mirror.get("codex_only", []):
        findings.append(Finding("pair", claude_rel, "無い（片側だけにするなら [mirror].codex_only へ）"))
    if not has_codex and name not in mirror.get("claude_only", []):
        findings.append(Finding("pair", codex_rel, "無い（片側だけにするなら [mirror].claude_only へ）"))
    if has_codex and name in mirror.get("claude_only", []):
        findings.append(Finding("pair", codex_rel, "[mirror].claude_only の agent なのに Codex 側がある"))
    if has_claude and name in mirror.get("codex_only", []):
        findings.append(Finding("pair", claude_rel, "[mirror].codex_only の agent なのに Claude 側がある"))
    if not has_claude and not has_codex:
        findings.append(Finding("pair", name, "検査したファイルが 1 つも無い"))

    meta: dict | None = None
    claude_body = ""
    if has_claude:
        claude_text = claude_path.read_text(encoding="utf-8")
        try:
            meta, claude_body = parse_frontmatter(claude_text)
        except ValueError as error:
            findings.append(Finding("frontmatter", claude_rel, str(error)))
        else:
            findings += check_claude_agent(name, meta, claude_body, claude_rel, root, config)
            findings += check_references(claude_body, claude_rel, root, config, roots)
        for copy in config.get("agents", {}).get(name, {}).get("copies", []):
            copy_path = root / copy
            if not copy_path.is_file() or copy_path.read_bytes() != claude_path.read_bytes():
                findings.append(Finding("pair", copy, f"{claude_rel} と byte 一致しない"))

    codex: dict | None = None
    if has_codex:
        try:
            codex = tomllib.loads(codex_path.read_text(encoding="utf-8"))
        except tomllib.TOMLDecodeError as error:
            findings.append(Finding("codex", codex_rel, f"TOML として parse できない: {error}"))
        else:
            findings += check_codex_agent(codex, codex_rel)
            if codex.get("name") != name:
                findings.append(Finding("name", codex_rel, f"name {codex.get('name')!r} が `{name}` と違う"))
            if isinstance(codex.get("developer_instructions"), str):
                findings += check_references(codex["developer_instructions"], codex_rel, root, config, roots)

    if meta is not None and codex is not None:
        location = f"{claude_rel} / {codex_rel}"
        if str(meta.get("description", "")).strip() != str(codex.get("description", "")).strip():
            findings.append(Finding("description", location, "description が一致しない"))
        instructions = codex.get("developer_instructions")
        if isinstance(instructions, str) and normalize_body(claude_body, config) != normalize_body(instructions, config):
            findings.append(Finding("body", location, "本文と developer_instructions が一致しない"))
    return findings


def discover(root: Path, config: dict) -> set[str]:
    """Agent names that have a file at the default Claude or Codex location."""
    claude_dir = root / layout(config, "claude_dir")
    codex_dir = root / layout(config, "codex_dir")
    names = {path.stem for path in claude_dir.glob("*.md") if path.name not in NON_AGENT_FILES}
    names |= {path.stem for path in codex_dir.glob("*.toml")}
    return names


def check(root: Path, config: dict) -> list[Finding]:
    agents = config.get("agents")
    findings: list[Finding] = []
    configured = set(agents or {})
    for name in sorted(discover(root, config) - configured):
        findings.append(Finding("pair", name, "agent ファイルがあるが設定の [agents] に無い"))
    if agents is None and not findings:
        findings.append(Finding("pair", "[agents]", "検査対象の agent が 0 件（意図的なら agents = {} と明示する）"))
    copies = {copy for spec in (agents or {}).values() for copy in spec.get("copies", [])}
    for name in sorted(configured):
        claude_path, codex_path = agent_paths(name, root, config)
        default_claude = root / layout(config, "claude_dir") / f"{name}.md"
        default_codex = root / layout(config, "codex_dir") / f"{name}.toml"
        for default, chosen in ((default_claude, claude_path), (default_codex, codex_path)):
            if default != chosen and default.is_file() and relative(default, root) not in copies:
                findings.append(Finding("pair", relative(default, root), f"上書き先 {relative(chosen, root)} と別にあり、copies にも無い"))
    roots = reference_roots(root, config) if "references" in config else None
    for name in sorted(configured):
        findings += check_agent(name, root, config, roots)
    findings += check_limits(root, config)
    findings += check_manifest(root, config)
    findings += check_codex_config(root, config)
    return findings


def warnings(config: dict) -> list[str]:
    """Notes printed on every run, whatever the result."""
    if config.get("boundary", {}).get("controls", True):
        return []
    unchecked = [f"{name}({spec['write_boundary']})" for name, spec in sorted(config.get("agents", {}).items())
                 if spec.get("write_boundary")]
    if not unchecked:
        return []
    return [
        "warning: [boundary].controls = false のため、disallowedTools と read-only の guard hook を検査していない: "
        + ", ".join(unchecked)
    ]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=DESCRIPTION)
    parser.add_argument("--root", "--repo-root", dest="root", type=Path, default=REPO_ROOT,
                        help="検査対象のリポジトリルート（既定: このスクリプトの親ディレクトリ）")
    parser.add_argument("--config", type=Path, default=None,
                        help=f"設定ファイル（既定: <root>/{DEFAULT_CONFIG.as_posix()}）")
    arguments = parser.parse_args(argv)
    root = arguments.root.resolve()
    try:
        config, _ = load_config(root, arguments.config)
        findings = check(root, config)
    except (SetupError, OSError, UnicodeError) as error:
        print(f"setup error: {error}", file=sys.stderr)
        return 2
    for note in warnings(config):
        print(note)
    for finding in findings:
        print(finding)
    if findings:
        print(f"{len(findings)} finding(s)")
        return 1
    print(f"agent definitions: ok ({len(config.get('agents', {}))} agent(s))")
    return 0


if __name__ == "__main__":
    sys.exit(main())
