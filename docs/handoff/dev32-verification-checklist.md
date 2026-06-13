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
- ☐ host ログに `named pipe listening: \\.\pipe\azookey-<SID>`
- ☐ DebugView 起動・**Capture Global Win32** ON・フィルタ `[azooKey TIP]`（Debug 時）
- ☐ 標準 Microsoft IME が残っている（切替不能時の保険）
- ☐ VM チェックポイント取得済み

---

## A. コア動線（DEV-32 必須）

> **クローズ判定について**: 既知不具合（DEV-197/198/199/190/171）による Fail は DEV-32 のクローズを **単独ではブロックしない**（各課題で個別に追跡）。本チェックの主目的は **新規リグレッションの検出**。既知課題が未解消の間、A3/A5 は Fail のままで構わない（`☐ 既知` を選ぶ）。クローズ条件は末尾「DEV-32 クローズ判定」を参照。

**A1. IME 認識・切替（M1/M2）** — Win+Space で「azooKey」を選択できる
☐ PASS ☐ FAIL — 備考:

**A2. ローマ字→かな preedit（M3）** — `ka` → 「か」が下線付き preedit
☐ PASS ☐ FAIL — 備考:

**A3. 連続・拗音/促音/撥音（M3）** — `nihongo` / `kitto` / `syatu`(しゃつ) / `siro`(しろ) / `nn`(ん) が正しく生成
☐ PASS ☐ FAIL ☐ 既知(DEV-198/199) — 備考:

**A4. 編集（M3）** — Backspace で1文字戻る / ESC で composition 全クリア
☐ PASS ☐ FAIL — 備考:

**A5. 候補変換（M4/M5）** — `nihongo` → Space で候補ウィンドウに漢字候補
☐ PASS ☐ FAIL ☐ 既知(DEV-190) — 備考:（※ 漢字変換は「変換能力の前提」を満たした上で評価）

**A6. 候補選択（M5）** — ↑↓ で選択移動、preedit 更新
☐ PASS ☐ FAIL — 備考:

**A7. 確定（M6）** — Enter / 数字で確定、テキスト挿入、候補窓が閉じる
☐ PASS ☐ FAIL — 備考:

**A8. IPC 往復・Host 由来（M4/M6）** — 候補が host 由来（候補が返る＝往復成立）
ログ: `[azooKey TIP] IPC: connected to host <ver>` / host stderr に query・応答
☐ PASS ☐ FAIL — 備考:

---

## B. 拡張・回帰確認

**B1. ショートカット透過（DEV-165）** — 非 preedit 時の Ctrl+A/C/V/Z/S・Alt+Tab・Win が IME に食われない
☐ PASS ☐ FAIL — 備考:

**B2. 早打ち / 候補未到着（DEV-166）** — 即 Space・高速入力で固まらず復帰
☐ PASS ☐ FAIL — 備考:（既知: preedit 遅延 DEV-199）

**B3. ライフサイクル・残留（DEV-175/170）** — フォーカス移動/アプリ終了/IME 切替で入力残留・ゴースト候補窓が出ない
☐ PASS ☐ FAIL — 備考:

**B4. host 後起動・再接続（DEV-168/169）** — host 停止→起動 / kill→再起動で候補復帰
☐ PASS ☐ FAIL — 備考:

**B5. host 異常時の安定性（DEV-173）** — host 強制終了/無応答で IME 切替・アプリ終了しても固まらない
☐ PASS ☐ FAIL ☐ 既知(DEV-173) — 備考:

**B6. 候補ウィンドウ表示品質（DEV-171/172）** — キャレット近くに出る・可読・高 DPI で崩れない
☐ PASS ☐ FAIL — 備考:（既知: 候補窓サイズ DEV-171）

**B7. 複数アプリ互換（DEV-160/153）** — メモ帳 / ブラウザ / (32bit) / (Office)
☐ メモ帳 ☐ ブラウザ ☐ 32bit ☐ Office — 備考:

---

## 既知の未解決不具合（再検証で解消を確認するチェック）
- ☐ DEV-197（P1）確定後カーソルが文字列先頭へ移動（メモ帳/WordPad、Edge では発生せず）
- ☐ DEV-198 訓令式ローマ字（si/tu/sya）が子音残り
- ☐ DEV-199 `nn`→「ん」が preedit に即時表示されない / preedit 反映遅延
- ☐ DEV-190 一般かな漢字変換が出ない（Zenzai 推論未実装。辞書/学習でのみ漢字化）
- ☐ DEV-171 候補ウィンドウが小さい（DPI/フォント）

## 結果サマリ
- コア(A): ___ / 8（PASS ___ / 既知 ___ / 新規 FAIL ___）
- 拡張(B): ___ / 7（PASS ___ / 既知 ___ / 新規 FAIL ___）
- **新規リグレッション**: ☐ なし ☐ あり
- 新規検出した問題（→ Linear 起票、`repo:*`/`area:*` ラベル付与）:

## DEV-32 クローズ判定
既知 Fail 単独ではクローズをブロックしない。次の **両方** を満たしたときにクローズする:
1. **新規リグレッションが無い**（既知不具合以外に A/B の Fail が無い）。
2. **既知不具合 DEV-197/198/199/190/171 がすべて解消済み**（各課題が Done）。

☐ クローズ可 ☐ 継続（未解消の既知課題: ____）

## 後始末
- ☐ 記入済みチェックリスト・スクショ・ログを DEV-32 にコメント（→ DEV-5 human gate 判断）
- ☐ host 停止 → `unregister.ps1` → またはチェックポイント復元
