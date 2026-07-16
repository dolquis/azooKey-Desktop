# ライセンス・第三者資産 attribution 規約

azooKey-Desktop（Windows 版）へ第三者由来のデータ・コードを同梱する際の attribution
規約。集約表記の実体はルート [`THIRD_PARTY_LICENSES`](../THIRD_PARTY_LICENSES)、本書は
その**運用規約の正典**である。

## 適用範囲

- 対象: 保守対象の Windows 実装ツリー（`core/` `ipc/` `tsf-tip/` `inference-host/`
  `learning/` `settings/` `bench/` `scripts/` など）が**再配布**する資産。
- 対象外: `legacy/`（未保守の macOS 実装・参照資産）。
- 対象外: リポジトリ同梱のエージェントツール（`.claude/skills/` `.agents/skills/` の
  ベンダリングスキル等）。製品の再配布資産ではないため集約しないが、各ディレクトリ同梱の
  `NOTICE` / `LICENSE`（例: `.claude/skills/doc-coauthoring`、Apache-2.0 © Anthropic, PBC）を
  出典として保持する（in-place 表記のバイパス禁止）。
- ビルド時のみ取得しソースツリーへ含めない依存（`FetchContent` の llama.cpp /
  GoogleTest 等）は「未同梱」として `THIRD_PARTY_LICENSES` に区別して記載する。
  配布物（MSIX）への同梱段階の再配布 attribution は
  [`sideload-packaging-spec.md`](sideload-packaging-spec.md) が扱う。

## 三層 attribution モデル

第三者資産（特に派生・再ポートしたデータ）は、次の三層で出典を保持する。

1. **ファイルヘッダ**: 同梱ファイル先頭に「由来元 + ライセンス + `THIRD_PARTY_LICENSES`
   への参照」を記す。機械可読形式（JSON / YAML / TSV）はコメント行、または隣接する
   `*.license` ファイル・ヘッダ行で表現する。
2. **集約ファイル**: ルート `THIRD_PARTY_LICENSES` に資産ごとの節を追加し、SPDX 識別子・
   原著作権表記・全文（必要なライセンス）・格納パス・派生の有無を集約する。
3. **再生成注記**: 原典から変換・再ポートしたデータは「原典 + 変換手順（スクリプトまたは
   手順書）」を残し、逐語コピーでないことと再現方法を明示する。

## データ採用ワークフロー

1. 採用可否を判定する（ライセンス互換性・配布条項・来歴の明確さ）。
   MIT / Apache-2.0 / BSD-3-Clause / CC0 / CC-BY-4.0 / 権利主張なし（PD 相当）は同梱可、
   コピーレフト・来歴不透明は不可（`plans/windows-port-roadmap.md` の配布ガード方針に整合）。
2. データを azooKey 形式へ再ポートする（逐語コピーせず、原典準拠で変換）。
3. 三層 attribution を付す（上記）。
4. `THIRD_PARTY_LICENSES` の該当節・追記テンプレートに沿って追記する。
5. 配布ガード（MSIX 構築時の attribution 存在チェック）に載せる。CI チェックは
   follow-up（本規約整備の後続タスク）。

## ライセンス別の要点

- **BSD-3-Clause（Mozc 由来 `symbol.tsv` / `emoji_data.tsv`、© Google）**: 原著作権表記と
  第 3 条項（名前による推奨・保証の禁止）を含む全文を保持する。派生データにも表記を継承する。
- **Unicode License v3（CLDR）**: CLDR データ条項に従い、出典バージョンと Unicode 表記を保持する。
- **Apache-2.0（SudachiDict）**: `NOTICE` の継承が必要。内包 UniDic（BSD-3-Clause）の
  ライセンス連鎖も併記する。standalone NEologd は同梱せず別 DL pack とする方針
  （`plans/windows-port-roadmap.md` M53）。

## 正典の対応

| 対象 | 正典 |
| -- | -- |
| 集約 attribution の実体 | ルート `THIRD_PARTY_LICENSES` |
| attribution 運用規約（本書） | `docs/licensing-policy.md` |
| 評価データ（`bench/data/`）のライセンス宣言 | `docs/conversion-quality-benchmark-spec.md` §13 / §14（CC0 / PD / authored 限定） |
| 配布物への同梱・再配布 attribution | `docs/sideload-packaging-spec.md`（Phase 7） |
| M62-C/D・M53 の定義・受け入れ条件 | `plans/windows-port-roadmap.md` |
| 状態・進捗・課題トラッキング | Linear（team `Dev`） |
