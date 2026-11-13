#!/usr/bin/env python3
"""Check copied contract tables in the docs against the code they describe.

Some documents carry a hand-written copy of a contract whose canonical form is
code: the IPC type list, the settings keys and their defaults, and the CMake
preset names. Nothing invalidated those copies when the code moved, so they
drifted. This script reads both sides and fails when they disagree. Like
`check_test_inventory.py` it only *checks*: the tables carry prose that no
generator can derive, so they are never rewritten.

Three contracts are checked.

IPC types
  Source of truth: `enum class MessageType` in `ipc/include/azookey/ipc/Messages.h`,
  the `Build*` / `Parse*` declarations in `ipc/include/azookey/ipc/Payloads.h`,
  the `case` labels of the switch in `Dispatcher::Dispatch`
  (`inference-host/src/Dispatcher.cpp`; the switch of `HandleUnauthenticated`
  is not counted), and the send sites in `tsf-tip/src/*.cpp`.

  a. The index table of `docs/windows-tsf-host-architecture.md`「IPC メッセージ」
     (header `| 型 | 正典の節 |`) must list every enumerator once, `Unknown`
     included, in enum order. A row whose second cell says「ハンドラを持たない」
     must name a type without a `Dispatch` case, and every type other than
     `Unknown` without a case must say so. The sentence naming the
     fire-and-forget types (「fire-and-forget の型は `A` と `B` である」) must
     name exactly the cases that return `std::nullopt` without a response.
     A type listed as planned (a bullet starting with a back-quoted name and
     「（予定」) must not be an enumerator yet.
  b. The `MessageType` table of the repo-owned Skill `tsf-ipc-protocol`
     (`references/frame-format.md` under both `.claude/skills/` and
     `.agents/skills/`) must list every enumerator once, in enum order. Its
     「現在の配線」column is a `+`-separated list over a fixed vocabulary;
     an unknown word fails. Each word is judged as follows:
       codec           some `Build<T>…` / `Parse<T>…` function exists
                       (`SHARED_CODECS` maps a type whose payload has another
                       name). A type with such functions must say `codec` or
                       `response codec`.
       response codec  `Build<T>Response` exists and no request builder does.
       Host            `Dispatch` has a case for the type, and a type with a
                       case must say `Host`.
       TIP             `tsf-tip/src` sends the type, and a type the TIP sends
                       must say `TIP`. A send is exactly one of the patterns in
                       `TIP_SEND_PATTERNS` (`.type = MessageType::T` or
                       `PostIpcSend(MessageType::T`); comparisons are not sends.
       診断CLI / newwords CLI
                       the named client source mentions `MessageType::T`.
                       Only the claim is checked; other senders (the settings
                       app, the user dictionary CLI) are prose, not vocabulary.
       enumのみ        alone in the cell, and no codec, case or TIP send exists.
       sentinel        only for `Unknown`, and `Unknown` must say it.
     The role column is prose and is not checked.

Settings keys
  Source of truth: `settings/*.schema.json` files that have top-level
  `properties` (today `mvp-settings.schema.json`; `model-catalog.schema.json`
  is a versioned `oneOf` and is not checked). A key is the dotted path through
  nested `properties`; map-shaped objects (`additionalProperties` schemas) have
  no fixed sub-keys.

  a. The key table of `docs/settings-app-spec.md` §3.6 (first table with
     a `キー` header). A key cell holds back-quoted keys; `prefix*` followed by
     「の N キー」 stands for every top-level key with that prefix and N must
     match the schema. A key absent from the schema is allowed only when its
     row contains `UNREGISTERED_MARKER`. Because the section declares that it
     lists all top-level keys (`ALL_KEYS_MARKER`), a schema top-level key that no
     row covers fails too.
  b. Every other `docs/*-spec.md` table whose header starts with `キー` and has
     a `既定` / `既定値` column. Such a table is treated as a settings table only
     when more than half of its single-key rows name schema keys (tables of
     profile fields, catalog fields or keys relative to an object are skipped
     that way); then every key must exist or carry `UNREGISTERED_MARKER`.
  In both, when the default cell holds a value and the schema has a `default`,
  they must be equal after normalisation (back-quotes and `"…"` removed,
  booleans, numbers, `{}` / `[]` as JSON, a leading `…` meaning “ends with”).
  A row naming several keys separated by ` / ` pairs them with a default cell
  split the same way. Cells such as `—` or 「各キーの schema 既定」 are skipped.
  Types, enums and ranges are not compared.

Preset names
  Source of truth: `configurePresets` / `buildPresets` / `testPresets` of
  `CMakePresets.json`. Scanned files: `README.md`, `docs/debugging.md`,
  `docs/test-inventory.md`, `docs/dev-infrastructure-spec.md`,
  `.claude/rules/*.md` and `justfile`. A name is taken only from
    `cmake --preset N` (configure), `cmake --build --preset N` (build),
    `ctest --preset N` (test), a bare `--preset N` (any kind),
    a back-quoted `windows-*` / `linux-*` name directly followed by “preset”,
    the first cell of a table whose header starts with「プリセット」/ `preset`
    (configure), and a justfile assignment `…preset := "N"` (configure).
  Placeholders (`<preset>`, `{{preset}}`) are skipped. Back-quoted
  `windows-*` names elsewhere are mostly CI job ids and are not checked.

Exit status is 0 when every contract agrees with the repository, 1 otherwise.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys


REPO_ROOT = Path(__file__).resolve().parents[1]

MESSAGES_HEADER = "ipc/include/azookey/ipc/Messages.h"
PAYLOADS_HEADER = "ipc/include/azookey/ipc/Payloads.h"
DISPATCHER_SOURCE = "inference-host/src/Dispatcher.cpp"
TIP_SOURCE_DIRECTORY = "tsf-tip/src"
ARCHITECTURE_DOC = "docs/windows-tsf-host-architecture.md"
ARCHITECTURE_HEADING = "IPC メッセージ"
FRAME_FORMAT_DOCS = (
    ".claude/skills/tsf-ipc-protocol/references/frame-format.md",
    ".agents/skills/tsf-ipc-protocol/references/frame-format.md",
)
FRAME_FORMAT_HEADING = "メッセージ種別"

# Types whose payload functions carry another name than the type itself.
SHARED_CODECS = {"QueryCatalogFetch": "CatalogFetch"}

# The only ways the TIP sends a message. A comparison such as
# `response->type == MessageType::Health` is not a send.
TIP_SEND_PATTERNS = (
    r"\.type\s*=(?!=)\s*(?:ipc::)?MessageType::{name}\b",
    r"PostIpcSend\(\s*(?:ipc::)?MessageType::{name}\b",
)

# Wiring word -> client source whose mention of the type backs the claim.
CLIENT_SOURCES = {
    "診断CLI": "diagnostics",
    "newwords CLI": "inference-host/src/NewWordsCli.cpp",
}
WIRING_WORDS = (
    "codec",
    "response codec",
    "Host",
    "TIP",
    "enumのみ",
    "sentinel",
    *CLIENT_SOURCES,
)
NO_HANDLER_MARKER = "ハンドラを持たない"
ENUM_ORDER_MARKERS = ("enum の並び順", "`MessageType` の順")

SETTINGS_DIRECTORY = "settings"
SETTINGS_INDEX_DOC = "docs/settings-app-spec.md"
SETTINGS_INDEX_HEADING = "3.6 "
UNREGISTERED_MARKER = "schema 未登録"
ALL_KEYS_MARKER = "全 top-level キー"
DEFAULT_HEADERS = ("既定", "既定値")
SKIPPED_DEFAULT_PHRASES = ("既定に従う", "schema 既定", "必須")

PRESET_DOCS = (
    "README.md",
    "docs/debugging.md",
    "docs/test-inventory.md",
    "docs/dev-infrastructure-spec.md",
)
PRESET_RULES_GLOB = ".claude/rules/*.md"
JUSTFILE = "justfile"
PRESET_KINDS = {
    "configure": "configurePresets",
    "build": "buildPresets",
    "test": "testPresets",
}
PRESET_TABLE_HEADERS = ("プリセット", "preset")

IDENTIFIER = r"[A-Za-z_][A-Za-z0-9_]*"
BACKQUOTED = re.compile(r"`([^`\n]+)`")
HEADING = re.compile(r"^(#{1,6})\s+(.*?)\s*$")
PRESET_OPTION = re.compile(r"--preset[ =]([A-Za-z0-9][A-Za-z0-9_.-]*)")
PRESET_BEFORE_WORD = re.compile(r"`((?:windows|linux)-[A-Za-z0-9_-]+)`\s*preset")
JUST_PRESET_ASSIGNMENT = re.compile(
    r"^[ \t]*[A-Za-z0-9_]*preset[ \t]*:=[ \t]*\"([^\"]+)\"", re.MULTILINE
)
CODEC_FUNCTION = re.compile(r"\b(Build|Parse)(" + IDENTIFIER + r")\s*\(")
NUMBER = re.compile(r"^-?\d+(?:\.\d+)?$")


# --------------------------------------------------------------------------
# Markdown helpers


def read(repo_root: Path, relative: str) -> str:
    return (repo_root / relative).read_text(encoding="utf-8")


def extract_section(text: str, title_prefix: str) -> str:
    """Return the body under the first heading whose title starts with the prefix."""
    lines = text.splitlines()
    for index, line in enumerate(lines):
        match = HEADING.match(line)
        if not match or not match.group(2).startswith(title_prefix):
            continue
        level = len(match.group(1))
        body = []
        for following in lines[index + 1 :]:
            inner = HEADING.match(following)
            if inner and len(inner.group(1)) <= level:
                break
            body.append(following)
        return "\n".join(body)
    return ""


def split_row(line: str) -> list[str]:
    """Split a Markdown table row on `|` outside back-quotes."""
    cells, current, in_code = [], [], False
    body = line.strip()
    if body.startswith("|"):
        body = body[1:]
    if body.endswith("|"):
        body = body[:-1]
    for character in body:
        if character == "`":
            in_code = not in_code
        if character == "|" and not in_code:
            cells.append("".join(current).strip())
            current = []
        else:
            current.append(character)
    cells.append("".join(current).strip())
    return cells


def parse_tables(text: str, start_line: int = 1) -> list[dict]:
    """Return every table as {header, rows: [(line number, cells)]}."""
    tables = []
    lines = text.splitlines()
    index = 0
    while index < len(lines):
        line = lines[index]
        if (
            line.lstrip().startswith("|")
            and index + 1 < len(lines)
            and re.match(r"^\s*\|[\s:|-]+\|\s*$", lines[index + 1])
        ):
            table = {"header": split_row(line), "rows": []}
            index += 2
            while index < len(lines) and lines[index].lstrip().startswith("|"):
                table["rows"].append((start_line + index, split_row(lines[index])))
                index += 1
            tables.append(table)
            continue
        index += 1
    return tables


def first_name(cell: str) -> str | None:
    match = BACKQUOTED.search(cell)
    return match.group(1) if match else None


# --------------------------------------------------------------------------
# IPC types


def strip_cpp_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", "", text)


def matching_brace(text: str, open_index: int) -> int:
    depth = 0
    for index in range(open_index, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return index
    return len(text)


def collect_message_types(repo_root: Path) -> list[str]:
    text = strip_cpp_comments(read(repo_root, MESSAGES_HEADER))
    match = re.search(r"enum\s+class\s+MessageType\b[^{]*\{", text)
    if not match:
        return []
    body = text[match.end() : matching_brace(text, match.end() - 1)]
    names = []
    for item in body.split(","):
        name = item.split("=")[0].strip()
        if name:
            names.append(name)
    return names


def collect_codec_functions(repo_root: Path) -> set[str]:
    """Return `Build…` / `Parse…` names declared in Payloads.h."""
    text = strip_cpp_comments(read(repo_root, PAYLOADS_HEADER))
    return {kind + rest for kind, rest in CODEC_FUNCTION.findall(text)}


def codec_state(name: str, functions: set[str]) -> tuple[bool, bool]:
    """Return (has request codec, has response codec) for a type."""
    base = SHARED_CODECS.get(name, name)
    request = f"Build{base}" in functions or f"Build{base}Request" in functions
    response = f"Build{base}Response" in functions
    return request, response


def collect_dispatch_cases(repo_root: Path) -> tuple[set[str], set[str]]:
    """Return (types with a Dispatch case, those that reply with nothing)."""
    text = strip_cpp_comments(read(repo_root, DISPATCHER_SOURCE))
    match = re.search(r"\bDispatcher::Dispatch\s*\([^)]*\)[^{;]*\{", text)
    if not match:
        return set(), set()
    body = text[match.end() : matching_brace(text, match.end() - 1)]
    handled: set[str] = set()
    silent: set[str] = set()
    labels: list[str] = []
    statements: list[str] = []

    def close_group() -> None:
        if labels:
            handled.update(labels)
            code = "\n".join(statements)
            if "return std::nullopt" in code and not re.search(
                r"return\s+(?:Handle|MakeResponse)", code
            ):
                silent.update(labels)

    for line in body.splitlines():
        case = re.match(r"\s*case\s+(?:ipc::)?MessageType::(" + IDENTIFIER + r")\s*:", line)
        if case:
            if statements:
                close_group()
                labels, statements = [], []
            labels.append(case.group(1))
            remainder = line[case.end() :]
            if remainder.strip():
                statements.append(remainder)
            continue
        if re.match(r"\s*default\s*:", line):
            close_group()
            labels, statements = [], []
            continue
        if labels:
            statements.append(line)
    close_group()
    return handled, silent


def collect_tip_sends(repo_root: Path, names: list[str]) -> set[str]:
    sources = sorted((repo_root / TIP_SOURCE_DIRECTORY).glob("*.cpp"))
    text = "\n".join(strip_cpp_comments(path.read_text(encoding="utf-8")) for path in sources)
    sent = set()
    for name in names:
        if any(re.search(pattern.format(name=name), text) for pattern in TIP_SEND_PATTERNS):
            sent.add(name)
    return sent


def client_mentions(repo_root: Path, relative: str, name: str) -> bool:
    target = repo_root / relative
    paths = sorted(target.glob("*.cpp")) if target.is_dir() else [target]
    pattern = re.compile(r"MessageType::" + name + r"\b")
    return any(path.is_file() and pattern.search(path.read_text(encoding="utf-8")) for path in paths)


def check_type_rows(label: str, rows: list[str], enum: list[str], ordered: bool) -> list[str]:
    problems = []
    seen = set()
    for name in rows:
        if name in seen:
            problems.append(f"{label}: 型 `{name}` の行が重複しています")
        seen.add(name)
        if name not in enum:
            problems.append(f"{label}: 型 `{name}` は MessageType にありません")
    for name in enum:
        if name not in seen:
            problems.append(f"{label}: MessageType の `{name}` の行がありません")
    if ordered and not problems and rows != enum:
        problems.append(f"{label}: 行の並びが MessageType の並びと一致しません")
    return problems


def find_type_table(section: str, first_header: str) -> list[tuple[int, list[str]]]:
    for table in parse_tables(section):
        if table["header"] and table["header"][0] == first_header:
            return table["rows"]
    return []


def check_architecture(repo_root: Path, enum: list[str], handled: set[str], silent: set[str]) -> list[str]:
    label = f"{ARCHITECTURE_DOC}「{ARCHITECTURE_HEADING}」"
    text = read(repo_root, ARCHITECTURE_DOC)
    section = extract_section(text, ARCHITECTURE_HEADING)
    rows = find_type_table(section, "型")
    if not rows:
        return [f"{label}: `| 型 |` で始まる索引表が見つかりません"]
    names = [first_name(cells[0]) or cells[0] for _line, cells in rows]
    ordered = any(marker in text for marker in ENUM_ORDER_MARKERS)
    problems = check_type_rows(label, names, enum, ordered)
    for name, (_line, cells) in zip(names, rows):
        if name not in enum or name == "Unknown":
            continue
        says_no_handler = NO_HANDLER_MARKER in " ".join(cells[1:])
        if says_no_handler and name in handled:
            problems.append(f"{label}: `{name}` は「{NO_HANDLER_MARKER}」とあるが Dispatch に case があります")
        if not says_no_handler and name not in handled:
            problems.append(f"{label}: `{name}` は Dispatch に case が無いのに「{NO_HANDLER_MARKER}」とありません")

    sentence = re.search(r"fire-and-forget の型は(.+?)である", section, flags=re.DOTALL)
    if sentence:
        named = set(BACKQUOTED.findall(sentence.group(1)))
        if named != silent:
            problems.append(
                f"{label}: fire-and-forget の型 {sorted(named)} が"
                f" Dispatch で応答を返さない型 {sorted(silent)} と一致しません"
            )
    for match in re.finditer(r"^\s*-\s*`(" + IDENTIFIER + r")`（予定", section, flags=re.MULTILINE):
        planned = match.group(1)
        if planned in enum:
            problems.append(f"{label}: 予定の型 `{planned}` は MessageType にあるので表へ移してください")
    return problems


def check_frame_format(
    repo_root: Path,
    relative: str,
    enum: list[str],
    functions: set[str],
    handled: set[str],
    tip_sends: set[str],
) -> list[str]:
    label = f"{relative}「{FRAME_FORMAT_HEADING}」"
    if not (repo_root / relative).is_file():
        return [f"{relative} がありません"]
    text = read(repo_root, relative)
    section = extract_section(text, FRAME_FORMAT_HEADING)
    rows = find_type_table(section, "種別")
    if not rows:
        return [f"{label}: `| 種別 |` で始まる表が見つかりません"]
    names = [first_name(cells[0]) or cells[0] for _line, cells in rows]
    ordered = any(marker in section for marker in ENUM_ORDER_MARKERS)
    problems = check_type_rows(label, names, enum, ordered)

    for alias_type, base in SHARED_CODECS.items():
        if alias_type in enum and f"Build{base}Response" not in functions:
            problems.append(f"{label}: `{alias_type}` が共有する `Build{base}Response` がありません")

    for name, (_line, cells) in zip(names, rows):
        if name not in enum:
            continue
        if len(cells) < 2:
            problems.append(f"{label}: `{name}` の行に「現在の配線」列がありません")
            continue
        words = [word.strip() for word in cells[1].split("+")]
        unknown = [word for word in words if word not in WIRING_WORDS]
        if unknown:
            problems.append(f"{label}: `{name}` の配線に未知の語 {unknown} があります（語彙: {list(WIRING_WORDS)}）")
            continue
        request, response = codec_state(name, functions)
        has_codec = request or response
        says = set(words)

        def report(message: str) -> None:
            problems.append(f"{label}: `{name}` {message}")

        if name == "Unknown":
            if says != {"sentinel"}:
                report("は `sentinel` だけを書きます")
            continue
        if "sentinel" in says:
            report("は `Unknown` ではないので `sentinel` と書けません")
        if "enumのみ" in says:
            if len(says) > 1:
                report("の `enumのみ` は単独で書きます")
            if has_codec or name in handled or name in tip_sends:
                report("は `enumのみ` とあるが、codec・Dispatch の case・TIP の送信のいずれかがあります")
            continue
        if "codec" in says and "response codec" in says:
            report("に `codec` と `response codec` が並んでいます")
        if "codec" in says and not has_codec:
            report(f"は `codec` とあるが Payloads.h に Build{name}… / Parse{name}… がありません")
        if "response codec" in says and not (response and not request):
            report("は `response codec` とあるが、応答だけの codec になっていません")
        if has_codec and not ({"codec", "response codec"} & says):
            report("は Payloads.h に codec があるのに配線に `codec` がありません")
        if ("Host" in says) != (name in handled):
            if name in handled:
                report("は Dispatch に case があるのに配線に `Host` がありません")
            else:
                report("は `Host` とあるが Dispatch に case がありません")
        if ("TIP" in says) != (name in tip_sends):
            if name in tip_sends:
                report(f"は {TIP_SOURCE_DIRECTORY} が送るのに配線に `TIP` がありません")
            else:
                report(f"は `TIP` とあるが {TIP_SOURCE_DIRECTORY} に送信箇所がありません")
        for word, source in CLIENT_SOURCES.items():
            if word in says and not client_mentions(repo_root, source, name):
                report(f"は `{word}` とあるが `{source}` に MessageType::{name} がありません")
    return problems


def check_ipc(repo_root: Path) -> list[str]:
    enum = collect_message_types(repo_root)
    if not enum:
        return [f"{MESSAGES_HEADER} から MessageType を読み取れません"]
    functions = collect_codec_functions(repo_root)
    handled, silent = collect_dispatch_cases(repo_root)
    if not handled:
        return [f"{DISPATCHER_SOURCE} から Dispatcher::Dispatch の case を読み取れません"]
    tip_sends = collect_tip_sends(repo_root, enum)
    problems = check_architecture(repo_root, enum, handled, silent)
    for relative in FRAME_FORMAT_DOCS:
        problems += check_frame_format(repo_root, relative, enum, functions, handled, tip_sends)
    return problems


# --------------------------------------------------------------------------
# Settings keys


def flatten_schema(node: dict, prefix: str = "") -> dict[str, dict]:
    keys = {}
    for key, child in (node.get("properties") or {}).items():
        path = f"{prefix}{key}"
        if isinstance(child, dict):
            keys[path] = child
            keys.update(flatten_schema(child, path + "."))
    return keys


def collect_schema_keys(repo_root: Path) -> tuple[dict[str, dict], list[str]]:
    keys: dict[str, dict] = {}
    top_level: list[str] = []
    for path in sorted((repo_root / SETTINGS_DIRECTORY).glob("*.schema.json")):
        schema = json.loads(path.read_text(encoding="utf-8"))
        if not isinstance(schema.get("properties"), dict):
            continue
        keys.update(flatten_schema(schema))
        top_level.extend(schema["properties"])
    return keys, top_level


def parse_default(text: str):
    """Normalise one default value; return (ok, value)."""
    value = text.strip()
    if value.startswith("`") and value.endswith("`") and value.count("`") == 2:
        value = value[1:-1]
    if value in ("true", "false"):
        return True, value == "true"
    if NUMBER.match(value):
        return True, float(value)
    if value.startswith(("{", "[")):
        try:
            return True, json.loads(value)
        except ValueError:
            return False, None
    if len(value) >= 2 and value[0] == value[-1] == '"':
        return True, value[1:-1]
    if value in ("", "—", "-") or "`" in value:
        return False, None
    return True, value


def default_cells(cell: str, count: int) -> list[str] | None:
    """Return one default text per key, or None when the cell is not a value."""
    if any(phrase in cell for phrase in SKIPPED_DEFAULT_PHRASES):
        return None
    cell = re.split(r"[（(]", cell, maxsplit=1)[0].strip()
    if not cell or cell in ("—", "-"):
        return None
    parts = [part.strip() for part in cell.split(" / ")] if count > 1 else [cell]
    if len(parts) != count:
        return None
    return parts


def defaults_equal(documented, schema_default) -> bool:
    if isinstance(schema_default, bool) or isinstance(documented, bool):
        return documented is schema_default
    if isinstance(schema_default, (int, float)):
        return isinstance(documented, float) and documented == float(schema_default)
    if isinstance(schema_default, str) and isinstance(documented, str):
        if documented.startswith("…"):
            return schema_default.endswith(documented[1:])
        return documented == schema_default
    return documented == schema_default


def row_keys(cell: str) -> tuple[list[str], list[tuple[str, int]]]:
    """Return (exact keys, [(wildcard prefix, declared count)]) of a key cell."""
    exact, wildcards = [], []
    for match in BACKQUOTED.finditer(cell):
        token = match.group(1)
        if token.endswith("*"):
            count = re.match(r"\s*の\s*(\d+)\s*キー", cell[match.end() :])
            wildcards.append((token[:-1], int(count.group(1)) if count else -1))
        else:
            exact.append(token)
    return exact, wildcards


def check_key_rows(
    label: str,
    rows: list[tuple[int, list[str]]],
    default_column: int | None,
    schema: dict[str, dict],
    top_level: list[str],
) -> tuple[list[str], set[str]]:
    problems, covered = [], set()
    for line, cells in rows:
        exact, wildcards = row_keys(cells[0])
        marked = UNREGISTERED_MARKER in " ".join(cells)
        for prefix, count in wildcards:
            matching = {key for key in top_level if key.startswith(prefix)}
            covered |= matching
            if count != len(matching):
                problems.append(
                    f"{label} {line} 行: `{prefix}*` は schema に {len(matching)} キーあります"
                    f"（表は {count if count >= 0 else '件数なし'}）"
                )
        for key in exact:
            if key in schema:
                covered.add(key)
            elif not marked:
                problems.append(
                    f"{label} {line} 行: `{key}` は schema にありません"
                    f"（予定のキーなら行に「{UNREGISTERED_MARKER}」と明記します）"
                )
        if default_column is None or default_column >= len(cells) or wildcards:
            continue
        parts = default_cells(cells[default_column], len(exact))
        if parts is None:
            continue
        for key, part in zip(exact, parts):
            node = schema.get(key)
            if node is None or "default" not in node:
                continue
            ok, value = parse_default(part)
            if ok and not defaults_equal(value, node["default"]):
                problems.append(
                    f"{label} {line} 行: `{key}` の既定 `{part.strip('`')}` が schema の"
                    f" {json.dumps(node['default'], ensure_ascii=False)} と一致しません"
                )
    return problems, covered


def default_column_of(header: list[str]) -> int | None:
    for index, cell in enumerate(header):
        if cell in DEFAULT_HEADERS:
            return index
    return None


def check_settings(repo_root: Path) -> list[str]:
    schema, top_level = collect_schema_keys(repo_root)
    if not schema:
        return [f"{SETTINGS_DIRECTORY}/*.schema.json から設定キーを読み取れません"]
    problems = []

    text = read(repo_root, SETTINGS_INDEX_DOC)
    lines = text.splitlines()
    start = next(
        (
            index + 2
            for index, line in enumerate(lines)
            if (match := HEADING.match(line)) and match.group(2).startswith(SETTINGS_INDEX_HEADING)
        ),
        None,
    )
    section = extract_section(text, SETTINGS_INDEX_HEADING)
    index_table = next(
        (table for table in parse_tables(section, start or 1) if table["header"][0] == "キー"),
        None,
    )
    label = f"{SETTINGS_INDEX_DOC} §{SETTINGS_INDEX_HEADING.strip()}"
    index_lines = set()
    if start is None or index_table is None:
        problems.append(f"{label}: `| キー |` で始まる表が見つかりません")
    else:
        index_lines = {line for line, _cells in index_table["rows"]}
        found, covered = check_key_rows(
            label, index_table["rows"], default_column_of(index_table["header"]), schema, top_level
        )
        problems += found
        if ALL_KEYS_MARKER in section:
            for key in top_level:
                if key not in covered:
                    problems.append(f"{label}: schema の top-level キー `{key}` が表にありません")

    for path in sorted((repo_root / "docs").glob("*-spec.md")):
        relative = path.relative_to(repo_root).as_posix()
        for table in parse_tables(path.read_text(encoding="utf-8")):
            header = table["header"]
            column = default_column_of(header)
            if not header or header[0] != "キー" or column is None:
                continue
            if relative == SETTINGS_INDEX_DOC and {line for line, _ in table["rows"]} & index_lines:
                continue
            single = [row_keys(cells[0])[0] for _line, cells in table["rows"]]
            single = [keys[0] for keys in single if len(keys) == 1]
            if not single or sum(key in schema for key in single) * 2 <= len(single):
                continue
            found, _covered = check_key_rows(relative, table["rows"], column, schema, top_level)
            problems += found
    return problems


# --------------------------------------------------------------------------
# Preset names


def collect_presets(repo_root: Path) -> dict[str, set[str]]:
    data = json.loads(read(repo_root, "CMakePresets.json"))
    return {
        kind: {preset["name"] for preset in data.get(key, [])}
        for kind, key in PRESET_KINDS.items()
    }


def preset_kind(line: str, option_index: int) -> str | None:
    before = line[:option_index]
    tool = max(before.rfind("cmake "), before.rfind("ctest "))
    if tool < 0:
        return None
    if before[tool:].startswith("ctest"):
        return "test"
    return "build" if "--build" in before[tool:] else "configure"


def preset_claims(relative: str, text: str) -> list[tuple[int, str, str | None]]:
    """Return (line, name, kind or None for any) claims found in a file."""
    claims = []
    lines = text.splitlines()
    for number, line in enumerate(lines, start=1):
        for match in PRESET_OPTION.finditer(line):
            claims.append((number, match.group(1), preset_kind(line, match.start())))
        for match in PRESET_BEFORE_WORD.finditer(line):
            claims.append((number, match.group(1), None))
    if relative == JUSTFILE:
        for match in JUST_PRESET_ASSIGNMENT.finditer(text):
            claims.append((text.count("\n", 0, match.start()) + 1, match.group(1), "configure"))
    else:
        for table in parse_tables(text):
            if table["header"] and table["header"][0] in PRESET_TABLE_HEADERS:
                for number, cells in table["rows"]:
                    for name in BACKQUOTED.findall(cells[0]):
                        claims.append((number, name, "configure"))
    return claims


def check_presets(repo_root: Path) -> list[str]:
    presets = collect_presets(repo_root)
    every = set().union(*presets.values())
    files = [relative for relative in PRESET_DOCS if (repo_root / relative).is_file()]
    files += sorted(
        path.relative_to(repo_root).as_posix() for path in repo_root.glob(PRESET_RULES_GLOB)
    )
    if (repo_root / JUSTFILE).is_file():
        files.append(JUSTFILE)
    problems = []
    for relative in files:
        for line, name, kind in preset_claims(relative, read(repo_root, relative)):
            known = presets[kind] if kind else every
            if name not in known:
                where = PRESET_KINDS[kind] if kind else "どの preset 一覧"
                problems.append(f"{relative}:{line}: preset `{name}` は CMakePresets.json の {where} にありません")
    return problems


# --------------------------------------------------------------------------


def check(repo_root: Path) -> list[str]:
    return check_ipc(repo_root) + check_settings(repo_root) + check_presets(repo_root)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=REPO_ROOT,
        help="検査対象のリポジトリルート（既定: このスクリプトの親ディレクトリ）",
    )
    arguments = parser.parse_args(argv)
    problems = check(arguments.repo_root.resolve())
    if problems:
        print("文書の契約の写しが実体（MessageType・schema・CMakePresets）と一致しません:", file=sys.stderr)
        for problem in problems:
            print(f"  - {problem}", file=sys.stderr)
        print(
            "\n実体を正として表を直してください（実体の変更と同じ PR で写しも更新します）。",
            file=sys.stderr,
        )
        return 1
    print("IPC の型の表、設定キーの表、文書中の preset 名は実体と一致しています。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
