# ドキュメント整理計画

本書は、repo docs（`README.md`、`AGENTS.md`、`CLAUDE.md`、`docs/`、`plans/`）の陳腐化・矛盾・重複を解消し、再発を防ぐ仕組みを入れるための一回限りの引き継ぎ文書である。
進捗と状態は Linear の tracking Issue DEV-1847 とその子 Issue が持ち、本書は持たない。
本書の完了基準（末尾）をすべて満たしたら、`docs/archive/` へ移す。

規約の正典は `azookey-doc-governance` の `references/canonical-matrix.md` と `references/doc-lifecycle.md` である。本書はそれを繰り返さず、整理作業に固有の判断と手順だけを持つ。

## 1. 問題の系統

| 系統 | 内容 | 主に扱う段階 |
|---|---|---|
| A. 事実の誤り | ファイル名、シンボル、手順、版、IPC の型一覧が実体と違う | 段階 1 |
| B. 正典の重複 | 受け入れ条件、payload の写し、CI の記述、式、設定既定値を複数の文書が詳述し、食い違っている | 段階 2 |
| C. 文書間の矛盾 | 方針そのものが文書ごとに違う | 2 節の決定に従い、段階 1〜2 |
| D. 状態・履歴の混入 | 見出しや冒頭の DEV 番号、日付、「確定」、作成日・更新日、推奨実装時期 | 段階 3 |
| E. 計画の残骸 | 存在しないファイルを、既存の契約のように書いている | 段階 1 と 3 |
| F. 肥大と構造 | 性質の違う内容の同居、「節 → M 対応表」の欠落、依存図の欠落 | 段階 2 と 4 |
| G. 検査の死角 | docs-lint は文書からコードへの参照を見ない。docs の CI はコードだけの変更では起動しない | 5 節 |

## 2. 方針の決定

整理の前提として、文書間で矛盾していた方針を次のとおり決めた。決定の経緯は DEV-1847 にある。

| # | 論点 | 決定 |
|---|---|---|
| D1 | CUDA の扱い | 正典は `copilot-pc-backend-spec.md` §4.3 とする。他の文書は条件付きの 1 行とリンクにする |
| D2 | 署名ゲート | roadmap M29 の位置づけを `sideload-packaging-spec.md` §0.1 に合わせる |
| D3 | MSI 実機検証の入力先 | Microsoft Store / UWP アプリは範囲外とする。正典は `sideload-packaging-spec.md` §0.1 |
| D4 | モデルの起動時自動取得と autoselect | 起動時の自動取得は将来の判断として残す。spec の中で「自動スキャンしない」と矛盾する記述を解消する |
| D5 | M31（Inno / LTSC） | 廃止する。LTSC を含む配布は `sideload-packaging-spec.md` §4 の MSI 経路で扱う |
| D6 | 実装の無い設計案 | Linear に所有 Issue がある予定は「予定」と明示して残す。所有 Issue が無いものは spec の責務境界に寄せ、コード側の不整合は Linear へ起票する |
| D7 | 決定記録と測定記録 | `docs/decisions/` を新設して移す。spec には結論と条件だけを残す |
| D8 | 巨大 spec の分割 | 段階 1〜3 の後に、§ 参照の棚卸しを前提として判断する |

## 3. 作業の原則

- § 番号を維持する。spec の § はコードコメント、schema、justfile からも参照されている。中身を移すときは、元の見出しに移設先への 1 行を残す。
- 見出しの文言は、本文と食い違うときに限り直してよい。直す前に、見出し文字列とアンカーへの参照が repo に無いことを `rg` で確かめる。
- 編集しない領域: `legacy/`、`docs/linear-conventions.md` §1〜§12、ベンダリングした共有 Skill、`scripts/docs-lint.py`。直したい点は origin（`dolquis/agent-ops`）への提案にする。
- コードの不具合は docs の PR に混ぜず、Linear へ起票する。
- docs-lint のベースラインは下げる方向にだけ更新する。誤検知には `lint:allow` を付ける。
- 受け入れ条件の分担: 条件の文言は roadmap が持つ。測定方法とテスト名は spec と `docs/test-inventory.md` に置く。
- 監査の所見は、編集の前に実ファイルで照合し直す。

## 4. 段階

### 段階 1: 事実の誤りを直す

構造は変えず、実体と食い違う記述だけを局所的に直す。対象は、存在しないパスとシンボル、古い手順（学習データのリセット、CI の再現、`just` の範囲）、IPC の型一覧、評価データと baseline の置き場所、設定表と既定値の衝突、2 節の決定に反する記述である。

### 段階 2: 正典を一本化する

先に、全ファイルへ同じ規則を当てる一律パスを行う。

1. spec 側の受け入れ条件を roadmap へ集約する。spec にしかない項目は roadmap へ移し、spec 側は roadmap へのリンク 1 行にする。外から参照されている節はスタブを残すか、参照元を同じ PR で直す。
2. payload・schema・式の写しを、正典（`ipc/include/azookey/ipc/Payloads.h`、`settings/*.schema.json`、正典の spec 節）へのリンクと意味表に置き換える。
3. 複数の M を扱う spec に「節 → M 対応表」を置く。手本は `neural-reranker-spec.md` §0。

続いて、領域ごとに正典を決めて重複を削る。

| 契約 | 正典 |
|---|---|
| IPC の型一覧 | `Payloads.h` と各 spec。`windows-tsf-host-architecture.md` は型から正典 spec への索引にする |
| CI ジョブの一覧と版 | `docs/test-inventory.md` と workflow。`dev-infrastructure-spec.md` §4 は方針だけを持つ |
| モデル取得の `.part`、Range、SHA256、原子置換 | `model-management-spec.md` §3.1.2 |
| degraded / fallback の規則 | `zenzai-inference-spec.md` §7.4 |
| `final_score` 式 | `neural-reranker-spec.md` §9 |
| `reranker.modernbert*` キー | `modernbert-ja-scoring-spec.md` §7 |
| 学習パラメータ（flush、GC、減衰） | `user-learning-enhancement-spec.md` と `learning-data-management-spec.md` |
| userdict / newwords の CLI 契約と writer の責務 | `learning-data-management-spec.md`（architecture から移す） |
| 第三者資産の attribution | `THIRD_PARTY_LICENSES` |
| エージェント運用規約 | `AGENTS.md`（入口）→ `docs/linear-conventions.md` §2.1 → `docs/handoff/agent-orchestration.md` → `.claude/agents/MANIFEST.md`（所有の情報だけ） |
| アプリ互換チェックリスト | `dev-infrastructure-spec.md` §13 と `compat-test/` |

正典を宣言した箇所には、宣言された側からの逆リンクを張る。

### 段階 3: 状態・履歴・計画の残骸を除去する

- roadmap: DEV の経緯、日付つきの決定、取り消し線つきの項目、推奨実装時期、週数見積りを除く。依存関係から決まる順序は残す。「既知のテストギャップ」は Linear と `docs/test-inventory.md` へ移す。
- spec: 冒頭の作成日・更新日・位置づけ、見出しの「確定」「DEV-」「日付」「（新規）」「（改訂）」を除く。実装計画の残骸（変更対象ファイル一覧、実装後の検証手順）を削る。
- 決定記録と測定記録を `docs/decisions/` へ移す（D7）。
- handoff を寿命で分ける。`human-gate-batch-runbook.md` は恒常部分だけを残す。`hyper-v-vm-verification-plan.md` §4.5（VM 内の TIP 登録委任の正典）は恒常 runbook へ移し、参照元を同じ PR で張り替える。課題番号を名前に持つ `dev32-verification-checklist.md` は、ファイル名を参照するスクリプトと同じ PR で rename する。`docs/linear-conventions.md` §13 から昇格済みの項目と日付を除く。
- 各 PR で docs-lint のベースラインを下げる。

### 段階 4: 構造を再編する（任意）

§ 参照の棚卸し表（docs、コードコメント、schema、justfile、スクリプト、Skill）を作ってから行う。

- roadmap の各 M 節を「目的 / 前提 / 変更対象 / 受け入れ条件 / 参照仕様」に揃え、依存図を 1 つに統合する。
- `dev-infrastructure-spec.md` を、§ 番号を保ったまま、CI、IPC の堅牢化、観測性、Host の可用性、診断ウィザード、互換ハーネスに分ける。元のファイルには § ごとの転送表を残す。
- `sideload-packaging-spec.md` §3（設定アプリ）、`auto-word-registration-spec.md` §15（物理層）、`inline-english-candidate-spec.md` §4.5〜4.8（格納層）、`neural-reranker-spec.md` の Track A / B を分ける候補とする。`rich-features-spec.md` は、実装のある契約を所有 spec へ移して解体する。
- `docs/README.md` の handoff 表を、検証系とエージェント運用系に分ける。

## 5. 再発防止

段階 1〜3 と並行して入れる。生成ではなく突合（check-only）を選ぶ。生成物を commit しない規約（`AGENTS.md`）と衝突せず、`scripts/check_test_inventory.py` と同じ型で保守できるからである。

| 順 | 施策 | 内容 |
|---|---|---|
| 1 | 参照の実在検査 | `scripts/check_doc_refs.py` を新設する。バッククォートのパスと `file::symbol` 表記の実在、spec の § 参照の解決を検査する。予定ファイルは、M 番号と正典 spec を書いた例外リストに登録したものだけを許し、ファイルが実在したら登録の削除を求める |
| 2 | docs の CI を全 PR で起動 | `.github/workflows/docs.yml` の paths 制限を外すか、コード側のパスを加える。コードだけの変更で docs が偽になる穴を塞ぐ |
| 3 | `docs/decisions/` | 日付、DEV 番号、経緯、実測値を書いてよい場所を用意する（D7） |
| 4 | docs impact ゲート | `Payloads.h`、`MessageType`、設定 schema、`CMakePresets.json`、workflow を変更して `docs/` と `plans/` を変更しない PR は、本文に影響なしの理由を求める |
| 5 | 実体との突合 | IPC の型索引と `MessageType`・Dispatcher、spec の設定キー表と schema、文書中の preset 名と `CMakePresets.json` |
| 6 | 書式の検査 | spec の必須節（節 → M 対応表、正典参照）と禁止事項（受け入れ条件の本文、作成日・更新日、見出しの状態表記）。roadmap の全 M が依存図に現れること |
| 7 | 文書メタデータ | 冒頭に種別・対象 M・正典範囲を持たせ、`docs/README.md` の索引、寿命、双方向の正典宣言を突合する |
| 8 | 定期監査とサイズの予算 | read-only の docs 監査を手順として固定する。spec の行数に警告閾値を置く |

5〜7 は段階 2〜3 で文書の形が揃ってから入れる。先に入れると、既存の違反でベースラインが膨らむだけになる。

origin（`dolquis/agent-ops`）への提案は次のとおりである。

- docs-lint で、見出しの「確定」「日付」「DEV-」と、「（新規）」つきの不在パスを検出する。バッククォートのパスを link 検査の対象にする。
- `docs/linear-conventions.md` で、節単位の lint 除外を指定できるようにし、§13 を lint の対象に戻す。origin にしか無いファイルへの参照に修飾を付ける。
- hooks の出自を明記する。

## 6. 完了基準

- [ ] 段階 1 の子 Issue が Done になっている
- [ ] 段階 2 で、spec 側の受け入れ条件の本文がなくなり、各 spec が roadmap へリンクしている
- [ ] 段階 2 で、2 節と 4 節の表に挙げた正典が一本化され、正典宣言が双方向になっている
- [ ] 段階 3 で、docs-lint の HEURISTIC が減り、残りは `lint:allow` で理由が示されている
- [ ] 5 節の施策 1〜4 が CI で動いている
- [ ] 段階 4 を行うかどうかの判断が DEV-1847 に記録されている
