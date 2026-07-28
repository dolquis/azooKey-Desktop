# azooKey on Windows

[azooKey](https://github.com/ensan-hcl/azooKey) の Windows 版（実験的実装）です。高精度なニューラルかな漢字変換エンジン「Zenzai」を搭載した、オープンソースの日本語入力システムを目指して開発しています。

**現在開発中の MVP のため、動作は一切保証できません。**

> 本リポジトリは元々 macOS 向けに開発されていましたが、Windows 移植に方針転換しました。macOS 向けのソース・ビルド資産は `legacy/` 配下に保全されていますが、現在は保守されていません。詳細は [`legacy/README.md`](./legacy/README.md) を参照してください。

## アーキテクチャ

Windows 版は **TSF TIP (in-process DLL)** と **Inference Host (別プロセス)** を分離する構成です。

```
   IME 対応アプリ
        │
        ▼  (TSF)
   tsf-tip/         …… Text Services Framework TIP（in-proc DLL）
        │
        ▼  (ipc/: JSON + length-prefix Named Pipe)
   inference-host/  …… 推論ホストプロセス（CPU、将来的に CUDA バックエンド）
        │
        ▼
   core/            …… OS 非依存のかな漢字変換コア
   learning/        …… 頻度 + 時間減衰による再ランキング
```

## リポジトリ構成

- `tsf-tip/` — TSF Text Service DLL
- `inference-host/` — 推論ホスト（CPU/CUDA）
- `core/` — OS 非依存の変換コア
- `ipc/` — JSON + length-prefix IPC 定義
- `learning/` — 学習・再ランキング
- `bench/` — レイテンシ計測 CLI
- `scripts/` — TIP 登録/解除 PowerShell スクリプト
- `docs/` — 設計メモ・機能仕様・デバッグ手順
- `settings/` — MVP 設定スキーマ
- `plans/` — ロードマップ・設計プラン
- `legacy/` — 旧 macOS 実装（参照用、未保守）

## ビルド要件

- Windows 10/11
- Visual Studio 2022（C++ デスクトップ開発ワークロード）
- CMake ≥ 3.21
- Ninja
- Windows SDK

## ビルド & テスト

```powershell
git submodule update --init third_party/wil
cmake --preset windows-debug -DAZOOKEY_FETCH_GOOGLETEST=ON
cmake --build --preset windows-debug
ctest --preset windows-debug --output-on-failure
./build/windows-debug/bench/azookey_bench.exe
./scripts/test-powershell-quality.ps1
```

単体テストは GoogleTest を使う。`-DAZOOKEY_FETCH_GOOGLETEST=ON` は GoogleTest が
ローカルに見つからないときに `FetchContent` でダウンロードする。システムに
GoogleTest を導入済みなら省略可。フラグなし・未導入の場合はテストのみスキップ
してビルドは継続する（オフライン環境向け）。
WIL は commit SHA に固定した header-only submodule で、Windows IPC のビルド前に
`git submodule update --init third_party/wil` で初期化する。CI など submodule を使わない
環境では `-DAZOOKEY_FETCH_WIL=ON` で同じ revision を取得できる。既定値は no-egress の
ため `OFF` とする。

Linux/macOS 上では `tsf-tip` は `if(WIN32)` ガードにより自動的にスキップされるため、`core/`, `ipc/`, `learning/`, `inference-host/`, `bench/` の単体検証は他 OS でも可能です。

`scripts/test-powershell-quality.ps1` は `PSScriptAnalyzer` と `Pester` のローカル PowerShell モジュールを使い、開発用 TIP 登録スクリプトの静的解析と安全な分岐テストを実行します。実際の machine-wide 登録は行いません。

## 配布パッケージ

MVP の直接配布には、x64 の per-machine **未署名 MSI** を使用します。
MSI は TIP、Inference Host、および必要な MSVC runtime を同梱します。
未署名のため、インストール時に Windows Defender SmartScreen の警告と
UAC の「不明な発行元」が表示されます。
配布物は本リポジトリの GitHub Releases から取得し、出所を確認してから実行してください。

MSI のビルド手順と同梱範囲は [`pkg/msi/README.md`](./pkg/msi/README.md)、
配布方針は [`docs/sideload-packaging-spec.md`](./docs/sideload-packaging-spec.md) §4 を
参照してください。

## TIP の登録 / 解除（Windows、管理者権限）

```powershell
cmake --preset windows-llama-debug -DAZOOKEY_FETCH_GOOGLETEST=ON -DAZOOKEY_FETCH_WIL=ON
cmake --build --preset windows-llama-debug
./scripts/register-dev.ps1
# 実 Zenzai モデルを使う場合
./scripts/register-dev.ps1 -ModelPath "$env:LOCALAPPDATA\azooKey\models\zenzai\<model>.gguf"
./scripts/unregister-dev.ps1 -TipDllPath ./build/windows-llama-debug/tsf-tip/azookey_tsf_tip.dll
```

`windows-llama-debug` は pin 済みの llama.cpp を取得し、実 Zenzai モデルを読み込める
開発用成果物を生成します。通常の `windows-debug` は高速な no-egress mock テスト用です。
`register-dev.ps1` は登録前に `llama_cpp=1` を確認し、mock ホストの誤登録を拒否します。
fallback-only の TIP テストに限り、明示的に `-AllowMockHost` を指定できます。
現時点では `models\zenzai\` へ GGUF を配置しただけではモデルを自動選択しません。
実モデルの登録では `-ModelPath` に配置済み GGUF の絶対パスを指定してください。
実モデルの検証手順は [`docs/zenzai-gpu-route.md`](./docs/zenzai-gpu-route.md) を参照してください。

`register-dev.ps1` は登録時に、TIP DLL とその親ディレクトリへ
`ALL APPLICATION PACKAGES`（SID `S-1-15-2-1`）の読み取り+実行を付与します。これが無いと
Microsoft Store など AppContainer で動くアプリが TIP DLL をロードできません。
`unregister-dev.ps1` は同じ ACE を取り消します。付与を避けたい場合は両スクリプトに
`-SkipAppContainerAcl` を指定してください（設計は
[`docs/sideload-packaging-spec.md`](./docs/sideload-packaging-spec.md) §1.7）。

machine-wide 登録のため管理者権限が必要です（非管理者で実行すると自動で UAC 昇格します）。
`-dev` 接尾辞は `regsvr32` 開発用経路であることを示します（MSIX 配布経路とは別。
`docs/sideload-packaging-spec.md` §1.1.1）。

## ロードマップ

開発計画・マイルストーン定義・v1.0 までの実行計画は
[`plans/windows-port-roadmap.md`](./plans/windows-port-roadmap.md) を参照してください。
機能ごとの仕様は [`docs/legacy-parity-spec.md`](./docs/legacy-parity-spec.md) 以下の
`docs/*-spec.md` にまとまっています。

## ライセンス

本体は MIT License（[`LICENSE`](./LICENSE)）。製品（Windows 実装ツリー）が再配布する
第三者資産の attribution は [`THIRD_PARTY_LICENSES`](./THIRD_PARTY_LICENSES)（規約は
[`docs/licensing-policy.md`](./docs/licensing-policy.md)）に集約します。リポジトリ同梱の
エージェントツールのうち第三者由来は [`.claude/skills/doc-coauthoring`](./.claude/skills/doc-coauthoring)
（Apache-2.0 © Anthropic, PBC）のみで、同梱の `NOTICE` / `LICENSE` を出典とします（上記集約のスコープ外）。
他のスキルは本プロジェクト自身のものです。

## コミュニティ

azooKey の開発に参加したい方、使い方に質問がある方、要望や不具合報告がある方は、ぜひ [azooKey の Discord サーバ](https://discord.gg/dY9gHuyZN5) にご参加ください。

### 支援

GitHub Sponsors をご利用ください。

## 関連プロジェクト

- [azooKey-Windows](https://github.com/fkunn1326/azooKey-Windows) — @fkunn1326 さんによる先行 Windows 実装
- [fcitx5-hazkey](https://github.com/7ka-Hiira/fcitx5-hazkey) — @7ka-Hiira さんによる Linux 向け実装
- [azoo-key-skkserv](https://github.com/gitusp/azoo-key-skkserv) — @gitusp さんによる SKK サーバ実装

## Acknowledgement

本プロジェクトは情報処理推進機構 (IPA) による [2024 年度未踏 IT 人材発掘・育成事業](https://www.ipa.go.jp/jinzai/mitou/it/2024/koubokekka.html) の支援を受けて開発を行いました。
