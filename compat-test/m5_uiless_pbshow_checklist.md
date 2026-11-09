# M5 UI-less / `pbShow` アプリ互換チェックリスト

候補 UI の「両立 (coexistence)」方式（`docs/tsf-deep-integration-spec.md` §2.8〜§2.11）が、
主要アプリで期待どおりに描画責務を切り替えるかを実機 Win11 で確認するためのチェックリスト。
計測手順・対象アプリ・合格条件（安定仕様）のみを定める。

> **アプリ別の実測結果（pass / fail・スクリーンショット・所見）は本ファイルに書かない。**
> 実走は Linear の `gate:human-required` の課題で行い、結果は当該課題のコメントに記録する
> （`AGENTS.md`「進捗・状態は Linear に一本化」）。記録の環境ブロックは
> `docs/handoff/human-gate-batch-runbook.md`「Part C：検証メモのひな形」にあり、アプリ別の記録項目は課題本文が持つ。

## 1. 位置づけ

本チェックリストは `docs/tsf-deep-integration-spec.md` §2.11 から参照される。
`pbShow` の戻り値とアプリの描画責務はアプリ実装に依存するため、代表アプリでの実測が要る。
M50 のアプリ互換性テストハーネス（`docs/dev-infrastructure-spec.md` §13）は入力動線全体の回帰を見る。
本チェックリストは候補 UI の描画責務だけに絞った実機確認であり、
`compat-test/m3_display_attribute_checklist.md`（表示属性の描画）とは目的が異なる。

## 2. 計測手順

各対象アプリで `nihongo` → Space で候補を表示し、次を確認する。

- (a) TIP が activate されるか
- (b) `BeginUIElement` の `pbShow` の戻り値
- (c) 自前 HWND が出るか、OS・アプリ側の UI に候補が乗るか

`pbShow` はデバッグウィンドウ（`docs/legacy-parity-spec.md` §8）か構造化ログで確認する。
ログの採取方法と、転記するフィールドは `docs/debugging.md`「候補 UI の `pbShow` を収集する」にある。

## 3. 対象アプリと期待挙動

pass 基準の定義である。実測値は Linear に記録する。

| アプリ | 区分 | 期待挙動 |
|---|---|---|
| メモ帳 (Notepad) | レガシー Win32 | activate / `pbShow==TRUE` / 自前 HWND |
| Edge（アドレスバー / テキストエリア） | Chromium | activate（`pbShow` はホスト依存） |
| Chrome | Chromium | activate（`pbShow` はホスト依存） |
| VS Code | Electron | activate（`pbShow` はホスト依存） |
| Windows ターミナル | Win32 | activate / 自前 HWND |
| Win11 スタート検索 | UI-less | activate + 入力（統合インライン検索表示は M21） |
| Office 365（Word） | Office | activate（`pbShow` はホスト依存） |

Win11 スタート検索の**統合インライン検索**体験（候補が検索ボックス直下に統合表示
される）は検索統合 API（`ITfIntegratableCandidateListUIElement` +
`ITfFnSearchCandidateProvider`）を要し M21 の範囲である（`docs/tsf-deep-integration-spec.md`
§2.7、[IME search integration requirements](https://learn.microsoft.com/windows/apps/develop/input/input-method-editor-requirements#ime-search-integration)）。
M5 ではスタート検索で TIP が activate され入力できること（統合表示なしの劣化モード
可）までを範囲とする。

## 4. 合格条件

- UI-less / アプリ描画ホスト（`pbShow == FALSE` を返すアプリ）で TIP が
  activate され、自前 HWND が出ず OS/アプリ UI に候補が乗る。
- レガシー Win32（メモ帳等）で従来通り自前 HWND が出る（`pbShow == TRUE` 経路）。
- Win11 スタート検索で TIP が activate され入力できる（統合インライン検索表示は M21
  で検証。§3 の注記参照）。
- いずれのアプリでも TIP が activate されない事象が出ないこと（`ActivateEx` /
  `ITfTextInputProcessorEx` / カテゴリ登録の不備の早期検出）。

## 5. 参照

- `docs/tsf-deep-integration-spec.md` §2.8（候補 UI 統合方針）、§2.9（カテゴリ登録）、§2.10（`ActivateEx`）、§2.11
- `docs/dev-infrastructure-spec.md` §13（M50 ハーネス）、§7.2（構造化ログ）
- `docs/debugging.md`「候補 UI の `pbShow` を収集する」
- `plans/windows-port-roadmap.md` M5 / M21
