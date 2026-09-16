---
name: azookey-settings-app-ui
description: azooKey Desktop の settings-app/ 配下の WinUI 3 / C++/WinRT 設定アプリ（azookey_settings.exe）、MainWindow.xaml、SettingsDocument による settings.json の保存、SettingsIpcClient の UpdateConfig 送信、LaunchArguments、Windows App SDK self-contained と Hybrid CRT の vcxproj、MSBuild を呼ぶ CMake target を変更、レビュー、デバッグするときに使う。「設定画面に項目を足す」「設定アプリが起動しない」「設定が保存されない」のような依頼でも、settings-app/ に触れるなら必ずこのスキルを使う。
---

# azooKey 設定アプリ UI

`settings.json` を保存する writer は設定アプリだけで、Host は parse 失敗時の quarantine rename だけを同じロック区間で行い、TIP は読むだけである（正典は `docs/windows-tsf-host-architecture.md`「共有ユーザーデータの writer 責務」）。
UI の変更は schema、runtime 既定値、Host の再読込、TIP のローカル監視まで一つの契約として扱う。
schema と既定値の同期そのものは `azookey-settings-schema-evolution` が持ち、本スキルは
UI 実装と設定アプリ固有のビルド経路を扱う。

## 手順

1. `references/spec-routing.md` を読み、対象が UI 表示、保存、Host 通知、起動引数、ビルドの
   どれかを決める。設定キーの追加・変更を伴うなら先に `azookey-settings-schema-evolution` を読む。
2. ツール選択・初期化は AGENTS.md「調査と実装」に従い、`EditableSettings` の読込・保存経路と
   `MainWindow.xaml.cpp` のバインディング、`SettingsIpcClient` の送信経路を確認する。
3. 既定値、範囲、無効値、隠しキー（UI に出さないが保持するキー）の扱いを列挙する。
4. 最小差分で実装し、`SettingsDocument` と `LaunchArguments` の単体テストを更新する。
5. `azookey_settings_persistence_tests`、`azookey_settings_launch_arguments_tests` を実行し、
   Host 側は `host_settings_store_tests`、TIP 側は `tsf_tip_local_settings_tests` へ広げる。
6. 表示文言、項目、保存形式、起動経路が変わる場合は `docs/native-ui-spec.md` と
   機能別 spec の「UI」節を同じ変更で更新する。

## 必須ガードレール

- `settings.json` の保存は `SettingsDocument` の atomic replace
  （`learning` の `WriteTextFileAtomically`）だけに残す。Host は quarantine rename 以外を書かず、TIP は書かない。
  「設定アプリだけが書くので競合しない」とは扱わず、`FileLock.h` の排他を保存と quarantine の両方で取る。
- 不正な `settings.json` は quarantine してから保存する既存契約を、無言の上書きに変えない。
- UI が持つ既定値を schema、sample、runtime と一致させる。UI だけで既定値を決めない。
- C++/WinRT と Windows App SDK は `settings-app/` に閉じる。`tsf-tip/` へ持ち込まない。
- 起動引数（`--langid`、profile GUID）は `ITfFnConfigure::Show` から
  `tsf-tip/src/SettingsLauncher.cpp` が渡す。引数の追加は TIP 側と同じ変更で行う。
- Host への通知は `Handshake` と `UpdateConfig` の既存 IPC 契約に従う。
  新しい message が要るなら `tsf-ipc-protocol` を併用する。
- API key、token、credential を UI のログ、テスト fixture、スクリーンショットに残さない。
- バージョンは `AZOOKEY_PRODUCT_VERSION` から `azookey_settings.rc` へ流れ、release workflow が
  `FileVersion` を検証する。手で版数を書かない。
- MSBuild、Windows App SDK、Hybrid CRT を要するため、UI 本体のビルドと表示確認は Windows
  でしかできない。単体テストの成功を UI の動作確認と読み替えない。

## 完了条件

- 保存、再読込、無効値、隠しキー、起動引数のテスト結果を報告する。
- 実機での表示確認（Windows-MCP の UI Automation か人手）を行った範囲と、行っていない範囲を分ける。
- 設定キーを増やした場合は schema、sample、runtime、docs の同期状況を報告する。
