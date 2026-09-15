# 設定アプリ変更のルーティング

## 仕様、実装、テスト

| 変更領域 | 最初に読む仕様 | 主な実装 | 優先テスト |
|---|---|---|---|
| 画面と項目 | `docs/native-ui-spec.md`、機能別 spec の「UI」節（例 `docs/app-profile-spec.md` §8、`docs/privacy-and-secure-input-spec.md` §6） | `settings-app/MainWindow.xaml`、`settings-app/MainWindow.xaml.cpp`、`settings-app/Strings/` | Windows-MCP の UI Automation か人手 |
| 保存と読込 | `settings/mvp-settings.schema.json`、`docs/dev-infrastructure-spec.md` §5 | `settings-app/SettingsDocument.cpp`（`EditableSettings`、quarantine、atomic replace） | `azookey_settings_persistence_tests`（`settings-app/tests/settings_document_test.cpp`） |
| Host への反映 | `docs/windows-tsf-host-architecture.md`、`tsf-ipc-protocol` skill | `settings-app/SettingsIpcClient.cpp`、`inference-host/src/SettingsStore.cpp` | `azookey_settings_persistence_tests`（`settings-app/tests/settings_ipc_client_test.cpp`）、`host_settings_store_tests` |
| TIP 側の同じファイルの監視 | `docs/windows-tsf-host-architecture.md` | `tsf-tip/src/TipLocalSettings.cpp` | `tsf_tip_local_settings_tests` |
| 起動引数と TIP からの起動 | `docs/tsf-deep-integration-spec.md` の `ITfFnConfigure` 節 | `settings-app/LaunchArguments.cpp`、`tsf-tip/src/SettingsLauncher.cpp` | `azookey_settings_launch_arguments_tests` |
| ビルドと配布物 | `docs/sideload-packaging-spec.md`、`pkg/msi/README.md` | `settings-app/CMakeLists.txt`（`azookey_settings` custom target）、`settings-app/azookey_settings.vcxproj`、`settings-app/HybridCRT.props`、`settings-app/packages.lock.json`、`settings-app/app.manifest` | `.github/workflows/release.yml` の settings build と version 検証 |

## 検証コマンドの選び方

- 保存と IPC: `azookey_settings_persistence_tests`（label `settings-app`、Windows 構成のみ登録）
- 起動引数: `azookey_settings_launch_arguments_tests`
- Host 側: `host_settings_store_tests`
- TIP 側: `tsf_tip_local_settings_tests`
- UI 本体: `cmake --build --preset windows-release --target azookey_settings`
- 横断確認: `cmake --build --preset windows-debug --target azookey_check`

上の target 名は入口であり網羅ではない。CTest 一覧の正典は `docs/test-inventory.md` で、
target の追加・削除はそちらと `scripts/check_test_inventory.py` が追う。

## 実装後に失効する spec 記述の削除

`pkg/msi/README.md` や各 spec の「UI」節は設定アプリの項目を「追加する」と将来形で
書きがちである。項目を実装したら、対応する記述を定義文へ直す
（詳細は `azookey-doc-governance` スキル）。
