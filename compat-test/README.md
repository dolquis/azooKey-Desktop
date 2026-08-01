# compat-test — アプリ互換性テストハーネス（M50）

主要 Windows アプリで TSF composition / 候補ウィンドウ位置 / 確定 /
キャンセル / Unicode / 絵文字 / Undo / Redo が壊れないことを半自動で
検証するハーネス。仕様の正典は
[`docs/dev-infrastructure-spec.md` §13](../docs/dev-infrastructure-spec.md)。

> 状態 / 進捗・優先度は Linear（team `Dev` / project *azooKey Desktop /
> Windows IME MVP*）が正典。本 README には進捗を書かない（`AGENTS.md`）。

## ビルドと実行

Windows プリセットで `compat_test` ターゲットをビルドし、対話セッションで
実行する。runner は自分が起動した新規 Notepad ウィンドウだけを操作する。
`--output` が既存の非空ディレクトリを指す場合は、古い artifact の混在を避ける
ため実行を拒否する。

```powershell
cmake --build --preset windows-release --target compat_test
.\build\windows-release\compat-test\compat_test.exe --output compat-report
```

azooKey TIP の登録と選択は machine-wide 設定を含むため、runner は行わない。
実行前に利用者が登録・選択を完了する。対話セッション、対象アプリ、UI Automation
のいずれかを利用できない場合は、silent skip せず `failing-skip` として報告する。

## ディレクトリ構成（§13.4）

```
compat-test/
├── CMakeLists.txt              # runner / cases を配線する compat_test ターゲット
├── runner/
│   ├── CompatRunner.cpp        # UI Automation + SendInput
│   ├── CaseSupport.cpp         # 物理 screen 座標の共通判定
│   ├── ClipboardIsolation.cpp  # C-011 の退避・固定データ置換・復元
│   ├── ScreenshotCapture.cpp   # 矩形だけを描く WIC PNG
│   └── ReportWriter.cpp
├── cases/
│   ├── C001_basic_input.cpp
│   ├── C002_backspace.cpp
│   ├── C003_escape.cpp
│   ├── C004_candidate_position.cpp
│   ├── C005_monitor_clamp.cpp
│   ├── C006_dpi_scaling.cpp
│   ├── C007_surrogate_pair.cpp
│   ├── C008_undo_redo.cpp
│   ├── C009_focus_transition.cpp
│   ├── C010_host_recovery.cpp
│   ├── C011_shortcut_routing.cpp
│   └── C012_romanization.cpp
└── targets/
    ├── notepad.json
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
│   └── notepad_C-001_fail.png
└── failures/
    └── notepad_C-001_fail/
        ├── failure.log
        └── screenshot.png
```

テストケース一覧（C-001〜C-012）と CI 連携は §13.3 / §13.6 を参照。
Notepad では C-001〜C-012 を実装する。環境条件を満たせず自動判定できないケースも
silent skip せず `failing-skip` としてレポートへ残す。終了コードは
全件 pass が `0`、fail を含む場合が `1`、fail は無いが failing-skip を含む場合が `2`。
C-002〜C-010 と C-012 は、英数入力でも成立する誤 pass を避けるため、C-001 の変換成功で
azooKey の基準動作を確認できた場合だけ実行する。

C-007 の自動操作は、サロゲートペアを一括した `SendInput` で注入し、対象アプリが
UTF-16 のペアを壊さず保持することまで確認する。この入力は TSF を通らないため、
azooKey の候補から絵文字を確定したことを表す pass にはせず、
`surrogate-pair-tip-path-unverified` の `failing-skip` とする。TIP 経由の確認は
`gate:human-required` の実機検証で補完する。

C-011 はクリップボードの全 format を実行前に即時複製してから固定のテストデータへ
置き換え、ショートカットの判定後に元の内容を復元する。復元後は複製時のformatと、
比較可能な `HGLOBAL` データのsize、hashが一致することを確認する。退避できない環境は
`failing-skip`、復元失敗は `fail` とし、元の内容をレポート、ログ、スクリーンショットへ
出力しない。

C-010 は正規の PowerShell supervisor が起動したHostだけを対象とし、再起動後の
per-user named pipeへ接続できるまで復帰とは判定しない。runnerによる代替Host起動は
行わず、再起動できない場合は `host-recovery-failed` とする。再接続待ちが他のケースへ
波及しないよう、Notepad targetではC-010を最後に実行する。

## 手動チェックリスト

runner 実装前でも実機で確認できる手動チェックリストを同居させる。

- [`m3_display_attribute_checklist.md`](m3_display_attribute_checklist.md) —
  M3 の DisplayAttribute / CompositionSink のアプリ別描画差（D-01〜D-10）。
  M50 のハーネス実装後は追加ケースとして取り込む。

## 観測性との整合

ログ・成果物は `docs/dev-infrastructure-spec.md` §7.6 の redaction ポリシーに
従う。レポートと失敗ログには固定の reason code だけを書き、観測した入力本文・
候補本文は書かない。失敗スクリーンショットは元の画面ピクセルを取得せず、
対象アプリと候補ウィンドウの矩形だけを空の画像へ安全な枠線で描く。
