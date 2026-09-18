# DEV-32 実機確認チェックリスト

azooKey TIP の実機動線検証（打鍵 → preedit → 候補 → 確定）用チェックリスト。記入して **DEV-32 にコメント**する。環境準備・登録・ログ取得の手順は [`hyper-v-tip-verification.md`](./hyper-v-tip-verification.md) を参照。

## 検証環境（記入）
- 検証日 / 検証者:
- OS（`winver` のビルド番号）:
- VM: Hyper-V / セッション種別 = **基本セッション（★必須）**:
- ビルド構成: ☐ Debug（ログ取得可） ☐ Release / commit:
- バックエンド: ☐ CPU(SimpleConverter) ☐ gguf / 辞書: ☐ `--mock-dict` 使用 ☐ なし

> ⚠️ **変換能力の前提**: Host は `--model <GGUF>` 指定時のみ llama.cpp 経由の実推論を行う。
> `--model` 未指定かつ `--mock-dict <TSV>` を使わない場合、**辞書外の語は漢字に変換されない**
> （SimpleConverter の静的辞書＝わたし/にほん/とうきょう 等＋学習語のみ）。A5 を「Pass」で
> 評価したい場合は、辞書内語、`--mock-dict`、または実 GGUF モデルの `--model` 指定のいずれかを
> 用意すること。

## 事前確認（検証開始前）
- ☐ `register-dev.ps1` が `TSF TIP registration complete (machine-wide).` を出力
- ☐ host 稼働:`Get-Process azookey_inference_host`
- ☐ host 稼働の確認は上の `Get-Process` で足りる。Release の Hidden 起動でも `AZOOKEY_LOG=1` ならファイルログを取得できる。**host をコンソール/Debug 起動した場合のみ** `named pipe listening: \\.\pipe\azookey-<SID>` を stderr で確認
- ☐ DebugView 起動・**Capture Global Win32** ON・フィルタ `[azooKey TIP]`（Debug 時）
- ☐ 標準 Microsoft IME が残っている（切替不能時の保険）
- ☐ VM チェックポイント取得済み

## compat runner との分担

`compat_test.exe` が判定する内容を、人間が同じセッションで繰り返さないための対応表である。
ケース定義の正典は [`docs/dev-infrastructure-spec.md`](../dev-infrastructure-spec.md) §13.3 で、本節はケース ID だけを参照する。
A 系・B 系の各行は次の 3 分類のどれかに属する。

- **(a) 機械判定**: 行の判定内容を、対応ケースが同じ観点で判定する。下記の省略条件をすべて満たす場合に限り、人間セッションで省略できる。
- **(b) 人間判定**: 対応ケースが無い、または判定に操作 UI、視覚、体感が要る。
- **(c) 補助**: 対応ケースの結果を根拠として参照できるが、行の一部をケースが判定しない。最終判断は人間が行い、省略しない。

| 行 | 分類 | 対応ケース | ケースが判定しない部分 |
|---|---|---|---|
| A1 | (b) | なし | Win+Space での選択。runner は azooKey を選択しない |
| A2 | (c) | C-002 / C-003 / C-012 | `ka` の入力と下線の描画。ケースは `nihongo` と `ja` 系を送り、preedit の文字列だけを読む |
| A3 | (c) | C-002 / C-012 | `kitto` / `syatu` / `siro` / `nn`。C-002 は `nihongo` の preedit、C-012 は `ja` 系だけを判定する |
| A4 | (a) | C-002 / C-003 | なし |
| A5 | (c) | C-001 / C-004 | 組込辞書語 `watashi` での候補と、候補ウィンドウの見え方。C-001 は辞書外の `nihongo` を使う |
| A5-opt | (a) | C-001 | なし |
| A6 | (b) | なし | ↑↓ での選択移動と preedit の更新 |
| A7 | (c) | C-001 | 数字キーでの確定、候補ウィンドウが閉じること、確定後のキャレット位置。C-001 は Enter で確定した文字列だけを見る |
| A8 | (c) | C-001 | TIP と Host のログでの往復の確認 |
| B1 | (c) | C-011 / C-008 | Alt+Tab。C-011 は Ctrl+A/C/V/L/S、Alt メニュー、Win キーを、C-008 は Ctrl+Z / Ctrl+Y を送る |
| B2 | (b) | なし | 即 Space と高速入力からの復帰 |
| B3 | (c) | C-009 | アプリ終了と IME 切替での残留、ゴースト候補窓。C-009 は crash しないことだけを見る |
| B4 | (c) | C-010 | host の後起動と、再接続後に候補が戻ること。C-010 は kill 後の pipe 復帰までを見る |
| B5 | (c) | C-010 | host の無応答と、host 異常中の IME 切替とアプリ終了 |
| B6 | (c) | C-004 / C-006 | 可読性と高 DPI での崩れ。ケースは候補ウィンドウの位置だけを見る |
| B7 | (c) | Notepad / Edge / VS Code の各 target の C-001〜C-012 | 32 bit アプリと Office。Office は同 spec §13.3.1 の手動チェックリストが扱う |

**省略条件。** (a) の行を省略してよいのは、次をすべて満たす compat の report がある場合に限る。
1 つでも欠ければ省略せず実施する。

1. compat を実行した検証 zip と、本セッションの検証 zip が同じ commit である。`report.json` は commit を持たないため、zip の `manifest.json` の commit で照合する。
2. ビルド構成（preset）が同じである。zip の `manifest.json` の `preset` で照合する。
3. 省略する行を確認するアプリと同じ target の report である。report が無いアプリでは省略しない。
4. バックエンドと辞書の条件（上の「バックエンド」欄）が同じである。
5. report で C-001 が pass であり、行の対応ケースもすべて pass である。C-011 を除く C-002 以降は、C-001 が pass でないと `failing-skip` になる。C-001 は辞書外の `nihongo` を確定するため、`--mock-dict`、`--model`、学習語のいずれも無い環境では fail になり、その場合は全行を実施する。
6. report はゲストのコンソールの対話セッションで得たものである（`vm-verify-session.ps1 -Run` のスケジュールタスク実行、または基本セッションでの手動実行）。拡張セッションで得た結果を根拠にしない。

**`failing-skip` と fail の扱い。**
`failing-skip` は機械が判定できなかったことを表し、pass ではない。
対応ケースが `failing-skip` の行は人間が実施し、人間の結果だけで PASS / FAIL を記録する。
機械の `failing-skip` を人間の PASS に読み替えない。
対応ケースが fail の行も省略しない。
機械の fail を FAIL へそのまま転記せず、人間が同じ行を実施して、環境起因（変換辞書が無いための C-001 の fail など）か回帰かを切り分ける。

**記録。** 省略した行は `☐ compat` を選び、備考に根拠の report（target、出力先、`manifest.json` の commit）を書く。
(b) と (c) の行で compat の結果を参照した場合は、備考に target とケースの結果を書く。

---

## A. コア動線（DEV-32 必須）

> **クローズ判定について**: 各行で `☐ 既知` を選んだ Fail（= その機能に Linear 追跡中の既知バグがあるケース）は DEV-32 のクローズを **単独ではブロックしない**。本チェックの主目的は **新規リグレッションの検出**。既知バグが未解消の間、該当行は `☐ 既知` を選んでよい。判定基準は末尾「検証 run の判定」を参照。

**A1. IME 認識・切替（M1/M2）** — Win+Space で「azooKey」を選択できる
☐ PASS ☐ FAIL — 備考:

**A2. ローマ字→かな preedit（M3）** — `ka` → 「か」が下線付き preedit
☐ PASS ☐ FAIL — 備考:

**A3. 連続・拗音/促音/撥音（M3）** — `nihongo` / `kitto` / `syatu`(しゃつ) / `siro`(しろ) / `nn`(ん) が正しく生成
☐ PASS ☐ FAIL ☐ 既知(DEV-198) — 備考:

**A4. 編集（M3）** — Backspace で1文字戻る / ESC で composition 全クリア
☐ PASS ☐ FAIL ☐ compat — 備考:

**A5. 候補変換（M4/M5）** — `watashi`（組込辞書語）→ Space で「私」等の漢字候補が出る
☐ PASS ☐ FAIL — 備考:（`watashi`/`nihon`/`toukyou` は組込辞書で必ず変換される。ここでの Fail は IPC/flush/候補 UI 等の **新規リグレッション**）

**A5-opt. 辞書外語の変換（任意。結果サマリのコア 8 にはカウントしない）** — `nihongo`（辞書外語）→ Space。`--mock-dict`/学習/実 GGUF モデル（`--model`）のいずれも無ければ漢字化されない
☐ 既知(GGUF 未指定: 漢字が出ない) ☐ PASS（`--mock-dict`/学習/`--model`使用時に漢字化） ☐ compat — 備考:

**A6. 候補選択（M5）** — ↑↓ で選択移動、preedit 更新
☐ PASS ☐ FAIL — 備考:

**A7. 確定（M6）** — Enter / 数字で確定、テキスト挿入、候補窓が閉じる、**確定後カーソルが確定文字の末尾にある**（DEV-197）
☐ PASS ☐ FAIL ☐ 既知(DEV-197) — 備考:

**A8. IPC 往復・Host 由来（M4/M6）** — 候補が host 由来（候補が返る＝往復成立）
ログ: `[azooKey TIP] IPC: connected to host <ver>` / host stderr に query・応答
☐ PASS ☐ FAIL — 備考:

---

## B. 拡張・回帰確認

**B1. ショートカット透過（DEV-165）** — 非 preedit 時の Ctrl+A/C/V/Z/S・Alt+Tab・Win が IME に食われない
☐ PASS ☐ FAIL — 備考:

**B2. 早打ち / 候補未到着（DEV-166）** — 即 Space・高速入力で固まらず復帰
☐ PASS ☐ FAIL — 備考:（B2 は DEV-166 早打ち耐性の確認。preedit 反映は A3 側で記録）

**B3. ライフサイクル・残留（DEV-175/170）** — フォーカス移動/アプリ終了/IME 切替で入力残留・ゴースト候補窓が出ない
☐ PASS ☐ FAIL — 備考:

**B4. host 後起動・再接続（DEV-168/169）** — host 停止→起動 / kill→再起動で候補復帰
☐ PASS ☐ FAIL — 備考:

**B5. host 異常時の安定性（DEV-173）** — host 強制終了/無応答で IME 切替・アプリ終了しても固まらない
☐ PASS ☐ FAIL ☐ 既知(DEV-173) — 備考:

**B6. 候補ウィンドウ表示品質（DEV-171/172）** — キャレット近くに出る・可読・高 DPI で崩れない
☐ PASS ☐ FAIL ☐ 既知(DEV-171) — 備考:

**B7. 複数アプリ互換（DEV-160/153）** — メモ帳 / ブラウザ / (32bit) / (Office)
確認アプリ: ☐ メモ帳 ☐ ブラウザ ☐ 32bit ☐ Office
☐ PASS ☐ FAIL ☐ 既知(DEV-160) — 備考:（32bit 非対応は DEV-160）

---

## 既知バグの追跡先（状態は Linear が正典）
既知の未解決バグの一覧・優先度・状態は **Linear で管理**する（`AGENTS.md`: docs に進捗/状態リストを置かない）。本チェックリストは状態を持たず、各行の `☐ 既知(DEV-xxx)` がそのバグへの参照。現在の状態は Linear で確認すること:

- team `Dev` / project `azooKey Desktop / Windows IME MVP` / label `repo:azooKey-Desktop`
- 本検証で参照する主な既知バグ: DEV-197 / DEV-198 / DEV-190 / DEV-171 / DEV-173 / DEV-160（各 ID を Linear で開いて最新状態を参照）

## 結果サマリ
- コア(A): ___ / 8（PASS ___ / compat ___ / 既知 ___ / 新規 FAIL ___）
- 拡張(B): ___ / 7（PASS ___ / 既知 ___ / 新規 FAIL ___）
- `☐ compat` の行は PASS に数えず、compat の欄に数える。人間が確認した行と機械が判定した行を合計で混ぜないためである。
- **新規リグレッション**: ☐ なし ☐ あり
- 新規検出した問題（→ Linear 起票。ラベル必須: `repo:*` + `area:*` + `agent:*`。実機確認など人間専任タスクは `agent:*` の代わりに `gate:human-required` を付与）:

## 検証 run の判定
- **本チェックの合否 = 新規リグレッションの有無**。`☐ 既知` を選んだ Fail（= 既に Linear 追跡中の既知バグ）は新規 FAIL に **数えない**。`☐ FAIL`（既知でない失敗）があれば新規リグレッション。
- ☐ 新規リグレッションなし（`☐ FAIL` が 0、Fail は `☐ 既知` のみ） ☐ 新規リグレッションあり（→ 新規 Linear 起票）

> **DEV-32 自体のクローズについて**: DEV-32（実機動線検証）のクローズ可否は **Linear 側で判断**する。本チェックの役割は「新規リグレッションが無いこと」の確認であり、既知バグの修正完了は各行で参照した DEV 課題が個別に負う。**既知バグの Done を DEV-32 クローズの前提にしない**（DEV-32 は新規リグレッションが無ければクローズ可、再修正の追跡は各課題側）。

## 後始末
- ☐ 記入済みチェックリスト・スクショ・ログを DEV-32 にコメント（→ DEV-5 human gate 判断）
- ☐ host 停止 → `unregister-dev.ps1` → またはチェックポイント復元
