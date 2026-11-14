#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest


sys.dont_write_bytecode = True
SCRIPT = Path(__file__).resolve().parents[1] / "check_doc_contracts.py"
SPEC = importlib.util.spec_from_file_location("check_doc_contracts", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)

REPO_ROOT = SCRIPT.parents[1]

MESSAGES = """\
enum class MessageType {
  Handshake,
  QueryCandidates,
  Cancel,
  QueryCorrections,
  // comment, not an enumerator
  Unknown,
};
"""

PAYLOADS = """\
std::string BuildHandshakeRequest(const HandshakeRequest& p);
std::string BuildQueryCandidatesRequest(const QueryCandidatesRequest& p);
std::string BuildCancel(const CancelPayload& p);
"""

DISPATCHER = """\
std::optional<ipc::Envelope> Dispatcher::Dispatch(const ipc::Envelope& req) {
  switch (req.type) {
    case ipc::MessageType::Handshake:
      return HandleHandshake(req);
    case ipc::MessageType::QueryCandidates: {
      return MakeResponse(req, "{}");
    }
    case ipc::MessageType::Cancel:
      HandleCancel(req);
      return std::nullopt;
    default:
      return MakeResponse(req, "{}");
  }
}

std::optional<ipc::Envelope> Dispatcher::HandleUnauthenticated(const ipc::Envelope& req) {
  switch (req.type) {
    case ipc::MessageType::QueryCorrections:
      return std::nullopt;
  }
}
"""

TIP = """\
void Send() {
  request.type = MessageType::Handshake;
  PostIpcSend(ipc::MessageType::QueryCandidates, body, true);
  env.type = ipc::MessageType::Cancel;
  if (response->type == ipc::MessageType::QueryCorrections) return;
}
"""

ARCHITECTURE = """\
## IPC メッセージ

型は `MessageType` の順で載せる。応答を返さない fire-and-forget の型は `Cancel` である。

| 型 | 正典の節 |
|---|---|
| `Handshake` | 本書 |
| `QueryCandidates` | spec |
| `Cancel` | spec |
| `QueryCorrections` | ハンドラを持たない |
| `Unknown` | spec |

- `FutureThing`（予定）: spec

## 次の節
"""

FRAME_FORMAT = """\
## メッセージ種別 (`ipc::MessageType`)

次表は enum の並び順に従う。

| 種別 | 現在の配線 | 役割 |
|---|---|---|
| `Handshake` | codec + Host + TIP | x |
| `QueryCandidates` | codec + Host + TIP | x |
| `Cancel` | codec + Host + TIP | x |
| `QueryCorrections` | enumのみ | x |
| `Unknown` | sentinel | x |
"""

SCHEMA = {
    "type": "object",
    "properties": {
        "liveConversion": {"type": "boolean", "default": False},
        "maxCandidates": {"type": "integer", "default": 9},
        "rulesPath": {"type": "string", "default": "%LOCALAPPDATA%\\azooKey\\rules.tsv"},
        "emojiRewriter": {"type": "boolean", "default": False},
        "emojiDataPath": {"type": "string", "default": ""},
        "model": {"type": "object", "properties": {"enabled": {"type": "boolean", "default": True}}},
    },
}

SIDELOAD = """\
## 3. 設定

### 3.6 設定スキーマの確定

本節は schema の最終形（全 top-level キー）の索引である。

| キー | 型 / enum | 既定 | 正典 |
|---|---|---|---|
| `liveConversion` | bool | `false` | x |
| `maxCandidates` | integer | `9` | x |
| `rulesPath` | string | `…\\rules.tsv` | x |
| `emoji*` の 2 キー | bool / string | 各キーの schema 既定 | x |
| `model` | object | — | x |
| `futureKey` | bool | `true` | schema 未登録（予定） |

### 3.7 次
"""

FEATURE_SPEC = """\
| キー | 型 | 既定 | 説明 |
|---|---|---|---|
| `liveConversion` | boolean | `false` | x |
| `model.enabled` / `maxCandidates` | bool / int | true / 9 | x |

| キー | 型 | 既定 | 説明 |
|---|---|---|---|
| `profileName` | string | "" | relative keys are skipped |
| `predictionEnabled` | bool | true | x |
| `style` | enum | "auto" | x |
"""

PRESETS = {
    "configurePresets": [{"name": "windows-debug"}, {"name": "linux-debug"}],
    "buildPresets": [{"name": "windows-debug"}],
    "testPresets": [{"name": "windows-debug"}],
}

README = """\
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug
cmake --build --preset <preset> --target x
The `windows-build` job is a CI job id, not a preset.
"""

DEV_INFRA = """\
| プリセット | 用途 |
|---|---|
| `windows-debug` | x |
| `linux-debug` | x |

`linux-debug` preset で検証する。
"""

JUSTFILE = """\

preset := "windows-debug"
build preset=preset:
    cmake --build --preset {{preset}}
"""


def write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def make_repo(root: Path, **overrides: str) -> None:
    files = {
        MODULE.MESSAGES_HEADER: MESSAGES,
        MODULE.PAYLOADS_HEADER: PAYLOADS,
        MODULE.DISPATCHER_SOURCE: DISPATCHER,
        f"{MODULE.TIP_SOURCE_DIRECTORY}/TextService.cpp": TIP,
        MODULE.ARCHITECTURE_DOC: ARCHITECTURE,
        MODULE.FRAME_FORMAT_DOCS[0]: FRAME_FORMAT,
        MODULE.FRAME_FORMAT_DOCS[1]: FRAME_FORMAT,
        "settings/mvp-settings.schema.json": json.dumps(SCHEMA),
        "settings/model-catalog.schema.json": json.dumps({"oneOf": []}),
        MODULE.SETTINGS_INDEX_DOC: SIDELOAD,
        "docs/feature-spec.md": FEATURE_SPEC,
        "CMakePresets.json": json.dumps(PRESETS),
        "README.md": README,
        "docs/build-ci-spec.md": DEV_INFRA,
        ".claude/rules/build.md": "Run `cmake --build --preset windows-debug`.\n",
        MODULE.JUSTFILE: JUSTFILE,
    }
    files.update(overrides)
    for relative, text in files.items():
        write(root / relative, text)


class CheckDocContractsTest(unittest.TestCase):
    def run_check(self, **overrides: str) -> list[str]:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            make_repo(root, **overrides)
            return MODULE.check(root)

    def assert_reported(self, problems: list[str], *fragments: str) -> None:
        for fragment in fragments:
            self.assertTrue(
                any(fragment in problem for problem in problems),
                f"{fragment!r} not in {problems}",
            )

    def test_consistent_fixture_passes(self) -> None:
        self.assertEqual(self.run_check(), [])

    def test_architecture_table_must_match_enum_and_dispatcher(self) -> None:
        text = (
            ARCHITECTURE.replace("| `Cancel` | spec |\n", "")
            .replace("| `QueryCandidates` | spec |", "| `QueryCandidates` | ハンドラを持たない |")
            .replace("| `QueryCorrections` | ハンドラを持たない |", "| `QueryCorrections` | spec |\n| `Ghost` | x |")
            .replace("- `FutureThing`（予定）", "- `Handshake`（予定）")
            .replace("型は `Cancel` である", "型は `Cancel` と `QueryCandidates` である")
        )
        problems = self.run_check(**{MODULE.ARCHITECTURE_DOC: text})
        self.assert_reported(
            problems,
            "`Cancel` の行がありません",
            "`Ghost` は MessageType にありません",
            "`QueryCandidates` は「ハンドラを持たない」とあるが",
            "`QueryCorrections` は Dispatch に case が無いのに",
            "予定の型 `Handshake`",
            "fire-and-forget の型",
        )

    def test_order_is_enforced_when_declared(self) -> None:
        swapped = FRAME_FORMAT.replace(
            "| `Handshake` | codec + Host + TIP | x |\n| `QueryCandidates` | codec + Host + TIP | x |",
            "| `QueryCandidates` | codec + Host + TIP | x |\n| `Handshake` | codec + Host + TIP | x |",
        )
        problems = self.run_check(**{MODULE.FRAME_FORMAT_DOCS[1]: swapped})
        self.assert_reported(problems, "並びが MessageType の並びと一致しません")
        self.assertTrue(all(MODULE.FRAME_FORMAT_DOCS[1] in problem for problem in problems))

    def test_wiring_column_must_match_code(self) -> None:
        text = (
            FRAME_FORMAT.replace("| `Handshake` | codec + Host + TIP |", "| `Handshake` | codec + Host |")
            .replace("| `Cancel` | codec + Host + TIP |", "| `Cancel` | Host + TIP + RPC |")
            .replace("| `QueryCandidates` | codec + Host + TIP |", "| `QueryCandidates` | enumのみ |")
            .replace("| `QueryCorrections` | enumのみ |", "| `QueryCorrections` | codec + Host + TIP |")
        )
        problems = self.run_check(**{MODULE.FRAME_FORMAT_DOCS[0]: text})
        self.assert_reported(
            problems,
            "`Handshake` は tsf-tip/src が送るのに配線に `TIP` がありません",
            "`Cancel` の配線に未知の語",
            "`QueryCandidates` は `enumのみ` とあるが",
            "`QueryCorrections` は `codec` とあるが",
            "`QueryCorrections` は `Host` とあるが",
            "`QueryCorrections` は `TIP` とあるが",
        )

    def test_comparison_is_not_a_tip_send(self) -> None:
        tip = TIP.replace("request.type = MessageType::Handshake;", "if (x.type == MessageType::Handshake) {}")
        problems = self.run_check(**{f"{MODULE.TIP_SOURCE_DIRECTORY}/TextService.cpp": tip})
        self.assert_reported(problems, "`Handshake` は `TIP` とあるが")

    def test_settings_keys_and_defaults_must_match_schema(self) -> None:
        sideload = (
            SIDELOAD.replace("| `maxCandidates` | integer | `9` |", "| `maxCandidates` | integer | `8` |")
            .replace("`…\\rules.tsv`", "`…\\other.tsv`")
            .replace("`emoji*` の 2 キー", "`emoji*` の 3 キー")
            .replace("| `model` | object | — | x |\n", "| `ghostKey` | bool | `false` | x |\n")
        )
        feature = FEATURE_SPEC.replace("| true / 9 |", "| false / 9 |")
        problems = self.run_check(
            **{MODULE.SETTINGS_INDEX_DOC: sideload, "docs/feature-spec.md": feature}
        )
        self.assert_reported(
            problems,
            "`maxCandidates` の既定 `8`",
            "`rulesPath` の既定",
            "`emoji*` は schema に 2 キーあります",
            "`ghostKey` は schema にありません",
            "schema の top-level キー `model` が表にありません",
            "docs/feature-spec.md",
        )
        self.assertFalse(any("`profileName`" in problem for problem in problems))

    def test_missing_keys_are_allowed_only_without_full_listing(self) -> None:
        sideload = SIDELOAD.replace("（全 top-level キー）", "").replace("| `model` | object | — | x |\n", "")
        self.assertEqual(self.run_check(**{MODULE.SETTINGS_INDEX_DOC: sideload}), [])

    def test_unknown_presets_are_reported_by_kind(self) -> None:
        readme = README + "ctest --preset linux-debug\ncmake --preset windows-gone\n"
        problems = self.run_check(
            **{
                "README.md": readme,
                MODULE.JUSTFILE: JUSTFILE.replace('"windows-debug"', '"windows-old"'),
                "docs/debugging.md": "Use the `linux-gone` preset.\n",
            }
        )
        self.assert_reported(
            problems,
            "preset `linux-debug` は CMakePresets.json の testPresets にありません",
            "preset `windows-gone` は CMakePresets.json の configurePresets にありません",
            "justfile:2: preset `windows-old`",
            "docs/debugging.md:1: preset `linux-gone`",
        )
        self.assertFalse(any("windows-build" in problem for problem in problems))

    def test_presets_in_the_specs_split_from_dev_infrastructure_are_checked(self) -> None:
        problems = self.run_check(
            **{
                "docs/vm-verification-package-spec.md": "Use the `windows-gone` preset.\n",
                "docs/diagnostics-compat-spec.md": "cmake --build --preset linux-gone\n",
            }
        )
        self.assert_reported(
            problems,
            "docs/vm-verification-package-spec.md:1: preset `windows-gone`",
            "docs/diagnostics-compat-spec.md:1: preset `linux-gone`",
        )

    def test_settings_key_table_is_read_from_settings_app_spec(self) -> None:
        self.assertEqual(MODULE.SETTINGS_INDEX_DOC, "docs/settings-app-spec.md")
        self.assertTrue((REPO_ROOT / MODULE.SETTINGS_INDEX_DOC).is_file())

    def test_real_repository_is_consistent(self) -> None:
        self.assertEqual(MODULE.check(REPO_ROOT), [])


if __name__ == "__main__":
    unittest.main()
