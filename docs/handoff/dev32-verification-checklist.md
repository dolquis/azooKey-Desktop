# DEV-32 実機確認チェックリスト

azooKey TIP の実機動線検証（打鍵 → preedit → 候補 → 確定）用チェックリスト。記入して **DEV-32 にコメント**する。環境準備・登録・ログ取得の手順は [`hyper-v-tip-verification.md`](./hyper-v-tip-verification.md) を参照。

## 検証環境（記入）
- 検証日 / 検証者:
- OS（`winver` のビルド番号）:
- VM: Hyper-V / セッション種別 = **基本セッション（★必須）**:
- ビルド構成: ☐ Debug（ログ取得可） ☐ Release / commit:
- バックエンド: ☐ CPU(SimpleConverter) ☐ gguf / 辞書: ☐ `--mock-dict` 使用 ☐ なし

> ⚠️ **変換能力の前提**: 現状 Zenzai 推論は未実装（DEV-190）。`--mock-dict <TSV>` を使わない限り、**辞書外の語は漢字に変換されない**（SimpleConverter の静的辞書＝わたし/にほん/とうきょう 等＋学習語のみ）。A5 を「Pass」で評価したい場合は、辞書内語または `--mock-dict` を用意すること。

## 事前確認（検証開始前）
- ☐ `register.ps1` が `TSF TIP registration complete (machine-wide).` を出力
- ☐ host 稼働:`Get-Process azookey_inference_host`
- ☐ host 稼働の確認は上の `Get-Process` で足りる（Release の Hidden 起動ではログ非表示）。**host をコンソール/Debug 起動した場合のみ** `named pipe listening: \\.\pipe\azookey-<SID>` をログで確認
- ☐ DebugView 起動・**Capture Global Win32** ON・フィルタ `[azooKey TIP]`（Debug 時）
- ☐ 標準 Microsoft IME が残っている（切替不能時の保険）
- ☐ VM チェックポイント取得済み

---

## A. コア動線（DEV-32 必須）

> **クローズ判定について**: 各行で `☐ 既知` を選んだ Fail（= その機能に Linear 追跡中の既知バグがあるケース）は DEV-32 のクローズを **単独ではブロックしない**。本チェックの主目的は **新規リグレッションの検出**。既知バグが未解消の間、該当行は `☐ 既知` を選んでよい。判定基準は末尾「検証 run の判定」を参照。

**A1. IME 認識・切替（M1/M2）** — Win+Space で「azooKey」を選択できる
☐ PASS ☐ FAIL — 備考:

**A2. ローマ字→かな preedit（M3）** — `ka` → 「か」が下線付き preedit
☐ PASS ☐ FAIL — 備考:

**A3. 連続・拗音/促音/撥音（M3）** — `nihongo` / `kitto` / `syatu`(しゃつ) / `siro`(しろ) / `nn`(ん) が正しく生成
☐ PASS ☐ FAIL ☐ 既知(DEV-198/199) — 備考:

**A4. 編集（M3）** — Backspace で1文字戻る / ESC で composition 全クリア
☐ PASS ☐ FAIL — 備考:

**A5. 候補変換（M4/M5）** — `watashi`（組込辞書語）→ Space で「私」等の漢字候補が出る
☐ PASS ☐ FAIL — 備考:（`watashi`/`nihon`/`toukyou` は組込辞書で必ず変換される。ここでの Fail は IPC/flush/候補 UI 等の **新規リグレッション**）

**A5-opt. 辞書外語の変換（任意・DEV-190 確認用。結果サマリのコア 8 にはカウントしない）** — `nihongo`（辞書外語）→ Space。`--mock-dict`/学習が無ければ漢字化されない（Zenzai 未実装）
☐ 既知(DEV-190: 漢字が出ない) ☐ PASS（`--mock-dict`/学習時に漢字化）— 備考:

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
☐ PASS ☐ FAIL — 備考:（B2 は DEV-166 早打ち耐性の確認。preedit 反映遅延の体感は A3/DEV-199 側で記録）

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
- 本検証で参照する主な既知バグ: DEV-197 / DEV-198 / DEV-199 / DEV-190 / DEV-171 / DEV-173 / DEV-160（各 ID を Linear で開いて最新状態を参照）

## 結果サマリ
- コア(A): ___ / 8（PASS ___ / 既知 ___ / 新規 FAIL ___）
- 拡張(B): ___ / 7（PASS ___ / 既知 ___ / 新規 FAIL ___）
- **新規リグレッション**: ☐ なし ☐ あり
- 新規検出した問題（→ Linear 起票。ラベル必須: `repo:*` + `area:*` + `agent:*`。実機確認など人間専任タスクは `agent:*` の代わりに `gate:human-required` を付与）:

## 検証 run の判定
- **本チェックの合否 = 新規リグレッションの有無**。`☐ 既知` を選んだ Fail（= 既に Linear 追跡中の既知バグ）は新規 FAIL に **数えない**。`☐ FAIL`（既知でない失敗）があれば新規リグレッション。
- ☐ 新規リグレッションなし（`☐ FAIL` が 0、Fail は `☐ 既知` のみ） ☐ 新規リグレッションあり（→ 新規 Linear 起票）

> **DEV-32 自体のクローズについて**: DEV-32（実機動線検証）のクローズ可否は **Linear 側で判断**する。本チェックの役割は「新規リグレッションが無いこと」の確認であり、既知バグの修正完了は各行で参照した DEV 課題が個別に負う。**既知バグの Done を DEV-32 クローズの前提にしない**（DEV-32 は新規リグレッションが無ければクローズ可、再修正の追跡は各課題側）。

## 後始末
- ☐ 記入済みチェックリスト・スクショ・ログを DEV-32 にコメント（→ DEV-5 human gate 判断）
- ☐ host 停止 → `unregister.ps1` → またはチェックポイント復元
