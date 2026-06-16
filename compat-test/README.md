# compat-test — アプリ互換性テストハーネス雛形（M50）

主要 Windows アプリで TSF composition / 候補ウィンドウ位置 / 確定 /
キャンセル / Unicode / 絵文字 / Undo / Redo が壊れないことを半自動で
検証するハーネスの雛形。仕様の正典は
[`docs/dev-infrastructure-spec.md` §13](../docs/dev-infrastructure-spec.md)。

> 状態 / 進捗・優先度は Linear（team `Dev` / project *azooKey Desktop /
> Windows IME MVP*）が正典。本 README には進捗を書かない（`AGENTS.md`）。

## ビルド配線

この `compat-test/` はトップ `CMakeLists.txt` のビルド対象に登録しない方針。
トップは `add_subdirectory` を明示列挙する方式のため、本ディレクトリを置いても
既存のビルド・CTest には影響しない。runner / cases を実装する際に
`add_subdirectory(compat-test)` を追加して配線する。

## 想定ディレクトリ構成（§13.4）

```
compat-test/
├── CMakeLists.txt              # runner / cases を配線する compat_test ターゲット
├── runner/
│   ├── CompatRunner.cpp        # UI Automation + SendInput
│   ├── ScreenshotCapture.cpp   # GDI 経由のスクリーンショット
│   └── ReportWriter.cpp
├── cases/
│   ├── C001_basic_input.cpp
│   ├── C002_backspace.cpp
│   └── ...
└── targets/
    ├── notepad.json            # 本雛形に同梱（サンプル）
    ├── edge.json
    └── ...
```

各 `targets/*.json` は AppId / window class / 自動化レベル
（`full` / `best-effort` / `recorder`）を定義する（§13.2）。

## 出力レイアウト（§13.5）

```
compat-report-YYYYMMDD-HHMMSS/
├── report.md         # 人間向けサマリ（PR コメント用）
├── report.json       # CI artifact 用
├── screenshots/
├── logs/
└── failures/
```

テストケース一覧（C-001〜C-012）と CI 連携は §13.3 / §13.6 を参照。

## 観測性との整合

ログ・成果物は `docs/dev-infrastructure-spec.md` §7.6 の redaction ポリシーに
従い、Release 既定で入力本文・候補語を残さないことを検証対象に含める。
