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

Linux/macOS 上では `tsf-tip` は `if(WIN32)` ガードにより自動的にスキップされるため、`core/`, `ipc/`, `learning/`, `inference-host/`, `bench/` の単体検証は他 OS でも可能です。

`scripts/test-powershell-quality.ps1` は `PSScriptAnalyzer` と `Pester` のローカル PowerShell モジュールを使い、開発用 TIP 登録スクリプトの静的解析と安全な分岐テストを実行します。実際の machine-wide 登録は行いません。

## TIP の登録 / 解除（Windows、管理者権限）

```powershell
cmake --preset windows-llama-debug -DAZOOKEY_FETCH_GOOGLETEST=ON
cmake --build --preset windows-llama-debug
./scripts/register-dev.ps1
./scripts/unregister-dev.ps1 -TipDllPath ./build/windows-llama-debug/tsf-tip/azookey_tsf_tip.dll
```

`windows-llama-debug` は pin 済みの llama.cpp を取得し、実 Zenzai モデルを読み込める
開発用成果物を生成します。通常の `windows-debug` は高速な no-egress mock テスト用です。
`register-dev.ps1` は登録前に `llama_cpp=1` を確認し、mock ホストの誤登録を拒否します。
fallback-only の TIP テストに限り、明示的に `-AllowMockHost` を指定できます。
実モデルの検証手順は [`docs/zenzai-gpu-route.md`](./docs/zenzai-gpu-route.md) を参照してください。

machine-wide 登録のため管理者権限が必要です（非管理者で実行すると自動で UAC 昇格します）。
`-dev` 接尾辞は `regsvr32` 開発用経路であることを示します（MSIX 配布経路とは別。
`docs/sideload-packaging-spec.md` §1.1.1）。

## ロードマップ

開発計画・マイルストーン定義・v1.0 までの実行計画は
[`plans/windows-port-roadmap.md`](./plans/windows-port-roadmap.md) を参照してください。
機能ごとの仕様は [`docs/legacy-parity-spec.md`](./docs/legacy-parity-spec.md) 以下の
`docs/*-spec.md` にまとまっています。

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
