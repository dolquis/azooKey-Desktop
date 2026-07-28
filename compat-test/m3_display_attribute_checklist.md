# M3 DisplayAttribute / CompositionSink アプリ互換チェックリスト

M3（Composition / Preedit 表示）で TIP が付ける未確定文字列の表示属性が、
主要アプリで期待どおりに描画されるかを実機 Win11 で確認するためのチェックリスト。
確認手順・対象アプリ・確認項目・合格条件（安定仕様）のみを定める。

> **実測結果（pass / fail・スクリーンショット・所見）は本ファイルに書かない。**
> 実走は Linear の `Human Gate: M3 DisplayAttribute/CompositionSink 実機アプリ互換
> チェックリストの実走`（DEV-365・`gate:human-required`）で行い、結果は当該課題の
> コメントに記録する（`AGENTS.md`「進捗・状態は Linear に一本化」）。

## 1. 位置づけ

M3 の受け入れ条件（下線付きで表示・ESC で破棄・Backspace で 1 文字戻る）は
`tsf_tip_display_attribute_tests` などの単体テストで covered だが、
`GUID_PROP_ATTRIBUTE` に載せた属性を**どう描くかはアプリ側の実装に依存する**。
下線が出ないアプリや、他 IME と見た目が揃わないアプリは実機でしか観測できない。

M50 のアプリ互換性テストハーネス（`docs/dev-infrastructure-spec.md` §13）の
C-001〜C-012 は入力動線全体の回帰を見る。本チェックリストの D-01〜D-10 は
DisplayAttribute の描画と `ITfCompositionSink` の終了経路だけに絞った補完で、
M50 のハーネス実装後は同ハーネスの追加ケースとして取り込む。

属性の定義と M23（複合 DisplayAttribute）との対応関係は
`docs/tsf-deep-integration-spec.md` §5.6 が正典。

## 2. 前提

- Windows 11 実機（VM 可）。TIP を登録済み（登録手順は `README.md`）。
- Inference Host が起動していること。Host 停止時の挙動は C-010 の範囲であり本
  チェックリストの対象外。
- 打鍵する文字列は `nihongo`（→ 未確定「にほんご」→ 確定「日本語」）に固定する。
  スクリーンショットに個人の入力が写らないようにするため、任意の文章を打たない
  （`docs/dev-infrastructure-spec.md` §7.6 の redaction ポリシー）。
- 実走前に次を記録する: Windows のビルド番号 / 対象アプリのバージョン /
  表示スケール / モニタ構成 / azooKey のビルド構成（Debug or Release）と git commit。

## 3. 対象アプリ

| アプリ | 区分 | TSF 実装系統 | §13.2 自動化レベル | 必須 |
|---|---|---|---|---|
| Notepad | 標準 | Win32 / RichEdit 系 | full | ✔ |
| WordPad | 標準 | Win32 RichEdit | best-effort | |
| Edge | ブラウザ | Chromium | full | ✔ |
| Chrome | ブラウザ | Chromium | full | |
| Firefox | ブラウザ | Gecko 独自 TSF | best-effort | ✔ |
| VS Code | Electron | Chromium + Monaco | full | ✔ |
| Discord | Electron | Chromium `contenteditable` | best-effort | |
| Slack | Electron | Chromium `contenteditable` | best-effort | |
| Word | Office | Office 独自 | recorder | ✔ |
| Excel | Office | Office 独自（セル / 数式バー） | recorder | |
| Outlook | Office | Office 独自 | recorder | |
| Windows Terminal | ターミナル | conhost / TSF level 差あり | full | ✔ |

「必須」列は TSF 実装系統ごとの代表 1 本で、実走時に必ず埋める対象。
残りは時間の許す範囲で確認する。未実施でも §7 の記録テンプレートには 12 本すべての
行を残し、未実施の欄を `-` で埋めて報告する（行ごと落とすと「確認済み」と読まれるため）。

## 4. 確認項目

| ID | 項目 | 操作 | 期待 |
|---|---|---|---|
| D-01 | preedit 表示 | `nihongo` と打鍵 | キャレット位置に未確定文字列「にほんご」が出る |
| D-02 | 下線 | D-01 の状態を目視 | 未確定文字列に実線（非太字）の下線が付く |
| D-03 | 色 | D-01 の状態を目視 | 文字色・背景色がアプリ既定のまま。反転表示や読めない配色にならない |
| D-04 | Backspace | D-01 の状態で Backspace を 1 回ずつ | 未確定文字列が 1 文字ずつ短くなり、下線が残りの範囲に追従する |
| D-05 | Esc | D-01 の状態で Esc | 未確定文字列が消え、本文に文字が残らない |
| D-06 | 確定 | D-01 の状態で Space → Enter | 下線が消え、確定文字列「日本語」だけが残る |
| D-07 | アプリ主導の composition 終了 | 未確定中に本文の別位置をマウスクリック、または別アプリへフォーカスを移す | アプリ側で composition が終了しても TIP が状態を持ち越さず、次の打鍵で新しい未確定文字列が始まる（`ITfCompositionSink::OnCompositionTerminated`） |
| D-08 | DPI 150% / 200% | 表示スケールを変えてサインインし直し、D-01〜D-06 を再走 | 下線の位置・太さが文字に追従し、未確定文字列がずれない |
| D-09 | 高 DPI 全画面 / モニタ跨ぎ | 全画面表示、および DPI の異なるモニタへウィンドウを移動して D-01〜D-02 を再走 | 移動後も下線が出て、位置がずれない |
| D-10 | 他 IME との比較 | 同じ操作を MS-IME で行い見比べる | 下線の有無・太さ・色に目立つ差がない。差があれば症状を記録する |

D-08 / D-09 の対象は Notepad / Edge / VS Code の 3 本で、この 3 本では実施が必須
（省略は不合格。§5）。他のアプリでは `N/A` としてよい。DPI 追従はアプリ固有では
なく TSF とアプリのレイアウト側の問題として現れるため、系統の異なる 3 本で足りる。

## 5. 合格条件

各項目は「pass 必須」（1 件でも不一致があれば不合格）か「follow-up 必須」
（不一致を即 fail とせず、症状の記録と follow-up Issue 起票を必須とする）の
どちらかに属する。どちらにも属さない項目は置かない。

| 項目 | 区分 | 合格条件 |
|---|---|---|
| D-01 / D-04 / D-05 / D-06 / D-07 | pass 必須 | 必須アプリすべてで期待どおりである |
| D-03 | pass 必須 | 文字が読めなくなる（前景色と背景色が同化する、選択範囲と区別できない）事象が 1 件もない |
| D-02 | follow-up 必須 | 必須アプリすべてで下線が出るのが合格。出ないアプリがあれば、対象アプリと症状を DEV-365 に列挙して follow-up Issue を起票・リンクする（§6 の既知差異により Chromium 系と conhost 系では不一致が出うるため即 fail としない） |
| D-08 / D-09 | follow-up 必須 | 対象の Notepad / Edge / VS Code すべてで実施し、下線の追従が崩れた場合は対象アプリ・スケール・モニタ構成と症状を DEV-365 に記録して follow-up Issue を起票・リンクする。実施自体を省略した場合は不合格 |
| D-10 | 記録のみ | 合否に含めない。差分は所見として記録する |

これらに加えて、確定後に下線が残る、未確定文字列が二重に表示される、
composition がアプリをまたいで持ち越される事象が 1 件もないことを合格条件とする
（いずれも pass 必須）。

follow-up 必須の項目は、**起票した Issue を DEV-365 にリンクするまで完了と
しない**。不一致を「既知差異だから」で閉じない。

## 6. 既知の差異

着手前に `docs/dev-infrastructure-spec.md` §13.3.2（アプリ別 workaround /
既知の差異）を読む。本チェックリストに直接効くものを再掲する。

| アプリ / 種別 | 効いてくる項目 | 内容 |
|---|---|---|
| Edge / Chrome（Chromium） | D-02 / D-03 | display attribute が完全には反映されず、下線が IME 既定描画になる場合がある |
| Firefox | D-02 / D-09 | TSF サポートが best-effort。属性の反映と矩形取得に差が出る |
| VS Code（Monaco） | D-01 / D-07 | 未確定中に Monaco 独自の補完が出て、composition の見た目が競合する |
| Discord / Slack | D-01 / D-09 | `contenteditable` で未確定文字列の位置精度が低い |
| Word / Excel / Outlook | D-02 / D-03 | アプリ側が独自に未確定文字列を描く。Excel はセル編集と数式バーで context が切り替わる |
| Windows Terminal | D-02 | conhost 系で TSF level が下がる場面があり、下線が出ないことがある |

## 7. 記録テンプレート

次を DEV-365 のコメントに貼り、埋めて報告する（`P` = 期待どおり、`F` = 不一致、
`N/A` = 対象外、`-` = 未実施）。

```md
### 環境

- Windows: 11 <build>
- azooKey: <git commit> / <Debug|Release>
- 表示スケール: <100% / 150% / 200%>
- モニタ構成: <単一 / 複数（DPI 差の有無）>

### 結果（必須アプリ）

| アプリ | D-01 | D-02 | D-03 | D-04 | D-05 | D-06 | D-07 | D-08 | D-09 | D-10 |
|---|---|---|---|---|---|---|---|---|---|---|
| Notepad | | | | | | | | | | |
| Edge | | | | | | | | | | |
| Firefox | | | | | | | | | | |
| VS Code | | | | | | | | | | |
| Word | | | | | | | | | | |
| Windows Terminal | | | | | | | | | | |

### 結果（任意アプリ・未実施なら全欄 `-`）

| アプリ | D-01 | D-02 | D-03 | D-04 | D-05 | D-06 | D-07 | D-08 | D-09 | D-10 |
|---|---|---|---|---|---|---|---|---|---|---|
| WordPad | | | | | | | | | | |
| Chrome | | | | | | | | | | |
| Discord | | | | | | | | | | |
| Slack | | | | | | | | | | |
| Excel | | | | | | | | | | |
| Outlook | | | | | | | | | | |

D-08 / D-09 は Notepad / Edge / VS Code のみ必須で、他アプリでは `N/A` でよい。

### 不一致の所見

- <アプリ> / <ID>: <症状> / <スクリーンショット> / <起票した follow-up>
```

スクリーンショットは不一致を記録した項目について取得し、DEV-365 に添付する。

## 8. 参照

- `docs/tsf-deep-integration-spec.md` §5.6（M3 属性と M23 複合属性の対応）、§5.7
- `docs/dev-infrastructure-spec.md` §13（M50 ハーネス）、§13.3.2（既知の差異）、§7.6（redaction）
- `docs/legacy-parity-spec.md` §12（UI-less / `pbShow` の実機チェック。目的が異なる別チェック）
- `plans/windows-port-roadmap.md` M3 / M23 / M50
- [Using Display Attributes](https://learn.microsoft.com/windows/win32/tsf/using-display-attributes) /
  [Providing Display Attributes](https://learn.microsoft.com/windows/win32/tsf/providing-display-attributes) /
  [TF_DA_ATTR_INFO](https://learn.microsoft.com/windows/win32/api/msctf/ne-msctf-tf_da_attr_info)
