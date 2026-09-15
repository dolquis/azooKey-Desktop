# 可観測性と診断のルーティング

## 仕様、実装、テスト

| 変更領域 | 最初に読む仕様 | 主な実装 | 優先テスト |
|---|---|---|---|
| 構造化ランタイムログ（JSON Lines、環境変数、ローテーション） | `docs/dev-infrastructure-spec.md` §7.2〜§7.5、`docs/debugging.md`「ログ収集」 | `core/src/RuntimeLogger.cpp`、`core/include/azookey/logging/RuntimeLogger.h` | `runtime_logger_tests`（OS mutex のため `RESOURCE_LOCK` 付き） |
| redaction | `docs/dev-infrastructure-spec.md` §7.6、`docs/privacy-and-secure-input-spec.md` §8 | `core/src/Redaction.cpp` | `core_tests`（`core/tests/redaction_test.cpp`） |
| ETW イベント | `docs/dev-infrastructure-spec.md` §7.3、§7.7、`docs/windows-tsf-host-architecture.md` | `core/src/EtwLogger.cpp`、`diagnostics/etw/AzooKey.man`（provider `azooKey-Desktop`）、`diagnostics/azookey-diagnostics.wprp` | `core_tests`（`core/tests/etw_logger_test.cpp`） |
| crash 収集と同意 | `docs/sideload-packaging-spec.md`（同梱と同意の扱い）、`plans/windows-port-roadmap.md` の該当 M | `core/src/CrashReporting.cpp` | `crash_reporting_tests`（Windows では `dbghelp` をリンク） |
| dump の保持と削除 | 同上 | `core/src/CrashRetention.cpp` | `core_tests`（`core/tests/crash_retention_test.cpp`） |
| 診断ウィザードと CLI | `docs/dev-infrastructure-spec.md` §12 | `diagnostics/Diagnostics.cpp`、`diagnostics/azookey_diag.cpp`、`diagnostics/SettingsSchema.h.in` | `diagnostics_tests` |
| Host 側の診断応答 | `docs/dev-infrastructure-spec.md` §12.2、`tsf-ipc-protocol` skill | `inference-host/src/Dispatcher.cpp` の `QueryDiagnostics` | `host_dispatcher_tests` |
| 実機での採取手順 | `docs/handoff/windows-diagnostics-playbook.md` | ProcDump / WPR / `azookey_diag` の組み合わせ | Human Gate（自動テストなし） |

## 検証コマンドの選び方

- ログと redaction: `runtime_logger_tests`、`core_tests`
- crash: `crash_reporting_tests`、`core_tests`
- 診断 CLI: `diagnostics_tests`（Windows 構成のみ登録）
- Host 応答: `host_dispatcher_tests`
- 横断確認: `cmake --build --preset windows-debug --target azookey_check`

上の target 名は入口であり網羅ではない。CTest 一覧の正典は `docs/test-inventory.md` で、
target の追加・削除はそちらと `scripts/check_test_inventory.py` が追う。

## 実装後に失効する spec 記述の削除

ログ項目、診断チェック ID（`D-xxx`）、zip の収集対象を追加したら、
`docs/dev-infrastructure-spec.md` §7 / §12 の一覧と `docs/debugging.md` の手順を
同じ変更で更新する。件数や「対応済み」の実況は書かず、定義だけを残す
（詳細は `azookey-doc-governance` スキル）。
