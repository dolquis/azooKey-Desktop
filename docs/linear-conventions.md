<!--
  SHARED CORE — Agent / Linear 運用規約（管制塔モデル）
  この「共有コア」は全リポジトリで同一内容をミラーする。
  個別 repo で直接編集しない。編集は origin（後述）で行い、各 repo へ伝播する。
  version: 0.10  updated: 2026-09-04
  改訂履歴は origin の git log を正典とし、本ヘッダには追記しない。
  上書き型で日付を持たない器へ履歴を手書きすると、§7.2 が禁じている
  手書きキャッシュそのものになるためである。
  origin(編集の起点・単一正典): dolquis/agent-ops/linear-conventions.md（このファイル）
  各 repo の docs/linear-conventions.md は本ファイルのベンダリングコピー + §13 Delta。
  プロジェクト固有の差分は各 repo の「Project Delta」節（本ファイル末尾）に置く。
-->

# Linear 管制塔モデル 運用規約（Shared Core）

Dev チーム配下の全プロジェクトで共通の、Linear 運用ルール。
**詳細仕様・ロードマップ・本規約の正典は GitHub repo docs。Linear ドキュメントはそのミラー。**

このファイルは、全 repo で同一内容をミラーする「共有コア」（§1–§12）と、各 repo だけが書き換える「Project Delta」（§13）で構成される。共有コアは origin で 1 回編集し、各 repo へ同一内容を伝播する（個別 repo で直接編集しない）。

---

## 1. 役割分担（control tower）

- **Linear = 管制塔**: 状態・優先度・進捗・親子関係・依存・担当エージェント・「次に AI へ渡す Issue」のルーティング。
- **GitHub = 正典(source of truth)**: 実装規約・ビルド/テスト手順・詳細仕様・ロードマップ、そして本運用規約。
- **Repository docs**: `AGENTS.md` / `README.md` / `docs/*` / `ROADMAP`（または `WORKFLOW.md`）。

原則: Linear に仕様を複製しない。詳細は GitHub 正典を参照する。

---

## 2. エージェント分業

| ラベル | 役割 |
| -- | -- |
| `agent:claude-design` | 仕様整理・設計・タスク分割・レビュー観点作成 |
| `agent:claude-review` | 実装後レビュー・整合性確認・リスク洗い出し |
| `agent:codex-impl` | 実装担当 |
| `agent:codex-pr-review` | PR 差分レビュー担当 |

Issue には「次の AI 役割」を示す `agent:*` を 1 つ付ける。ただし `gate:human-required` または旧 `type:human-gate` の人間専任タスクは `agent:*` を省略してよい。

---

## 2.1 Codex Execution Policy（Codex 実行ポリシー）

対象: Codex Cloud（"Codex for Linear"）。Linear で Issue を Codex に assign / delegate する、コメントで mention トークン（`@`+`Codex`）を付ける、または triage rule で自動 delegate すると起動する。ローカルの Codex App は Codex チャット（Linear 管轄外）から起動し、Linear のラベルでは起動しない。

- `agent:codex-impl` / `agent:codex-pr-review` は **ルーティング（候補）ラベル**であり、Codex Cloud の実行を許可しない（滑走路前の待機列）。
- Claude は Codex 候補 Issue の作成・分割・ラベル付け・関連付け・整理と、実行指示文の下書きまで行ってよい。ただし Codex への assign / delegate / mention は **行わない**。
- Codex Cloud の実行には人間 lead の明示許可（Issue コメント）が必要。Claude / エージェントはいかなる Linear コメント / Issue 本文 / テンプレートにもリテラルな mention トークン（`@`+`Codex`）を再生産しない（無害化する）。承認後に実際の mention で起動するのは人間 lead のみ。
- triage rule による Codex 自動 delegate は使わない。
- 実行したら Codex Run Record（§6）に approval / Codex task link / branch / commit / PR / validation / remaining risk を記録する。
- 無許可で Codex Cloud が動いた場合はインシデントとして扱う: delegate を解除して Issue を候補へ戻し、GitHub に branch / PR が到達していないか確認し、Issue に記録する。

### 実行許可フォーマット（人間 → Issue コメント）

- Issue / Repo / Scope（Acceptance Criteria のみ）
- Allowed output: summary only | branch only | draft PR | PR（repo の PR 規約に従う。Draft PR 必須の repo では draft PR までとする）
- Human gate: none | required before Done
- Prohibited: 無関係な refactor / スコープ変更 / main への直接 push / human-gate 判断の変更

このコメントがある場合に限り、人間が Codex への delegate / mention を行う。

---

## 3. ワークフロー状態

標準フロー（Linear のステータス名が完全一致しなくても、この順序で解釈する）:

1. **Backlog / Todo** — 未整理または未着手
2. **Design** — Claude Code で仕様整理・実装方針作成
3. **Implement** — Codex で実装
4. **Review** — Claude Code または Codex で差分レビュー（Linear の In Review。open な PR がある）
5. **Merged** — PR マージ済み・検証メモ待ち（Done 判定前の滞留を可視化する）
6. **Done** — repo 固有の完了条件を満たし、作業結果・検証・PR・ドキュメント影響確認が記録済み

### 3.1 状態に意味を重ねない

In Review が「PR レビュー待ち」「マージ済み検証メモ待ち」「人間ゲート待ち」を兼ねると、キューの内訳が Linear 単体では分からず、監査のたびに GitHub の open PR 実態と突合することになる（2026-08-31 監査では In Review 18 件のうち open PR を持つ課題は 1 件だった。DEV-923）。次のように使い分ける。

- **In Review** は open な PR（Draft 含む）を持つ課題だけに使う。open PR が無い課題を In Review に置かない。
- **Merged** は PR がマージ済みで、検証メモの記載と Done への明示遷移（§7.1.3）が済んでいない課題に使う。
- **人間ゲート課題**（`gate:human-required` の人間専任 Issue）は PR を持たないため In Review / Merged に置かず、Todo のまま人間の着手を待つ。キューは §10 の Needs Human Verification ビューが担う（started 系の状態に置くと、着手できないまま cycle 集計に乗り続ける）。
- **`type:tracking`** は子 Issue の進行中は In Progress を維持する。子 PR のマージは束ねの完了ではないため、In Review / Merged に置かない。

### 3.2 Cycle は commitment（scope の上限と、外す条件）

Linear は cycle の終了時に未完了 Issue を次の cycle へ自動で繰り越す。入れた分は消えず、残った分がそのまま次の scope になるため、Cycle を「やりたいこと置き場」として使うと scope は単調に増え、burndown は平坦なままになる（Cycle 8 は scope 96 件・completed 0 件で終わった）。Cycle には **その週に状態が動く見込みのあるものだけ**を入れる。

数値（件数・Project 数・持ち越し回数）は人間 lead が決め、cycle の実績で見直す。

1. **投入は commitment として決める**。その週に状態が動く見込みのあるものだけを入れる。上限は 20 件。
2. **同時 In Progress は 8 件まで**（`type:tracking` と `gate:human-required` を除く）。上限に達したら、新規着手より In Review / Merged の解消を先に行う。
3. **`type:tracking` を Cycle に入れない**。束ね Issue は 1 週間で閉じないため、scope と burndown の両方を歪める。追跡は Project とラベルで行い、Cycle には子の implementation Issue を入れる。
4. **外部要因で止まっているものは Cycle から外す**。第三者の返答、他者のリリース、ベンダーの回答を待つ Issue は、こちらの帯域を消費しないまま未完了として積み上がる。
5. **人間ゲートは、その週に人間が処理すると決めたものだけ入れる**。`gate:human-required` は AI 側から動かせないため、入れっぱなしにすると滞留の芯になる。
6. **`[Recurring] ... control tower audit`（§11）を Cycle に入れない**。週次トリガで回し、scope に数えない。
7. **同時に進める Project は 3 つまで**。残りは Cycle の外に置き、Project の Status Update に止めている理由と再開条件を書く（§12）。
8. **PR がマージされたら In Progress に留めない**。マージ → Merged（Merged state を持たない間は In Review）→ 検証メモ記載 → Done（§7.1.3）。`Part of` / `Refs` のような closing キーワードでない参照ではこの遷移が自動で起きないため、手で動かす。検出は `scripts/linear-audit.py`（§11）が持つ。
9. **止めるときにアーカイブを使わない**。アーカイブした Issue は通常ビュー・Cycle・Project の集計から消えるため、状態の正典が Linear であるという前提が崩れる。取り下げは `Canceled`、続けるが今は動かさないものは Backlog へ戻し、いずれも理由をコメントに書く（team に停止用の状態を設けるならそれを使う）。未完了のままアーカイブされた Issue の検出は `scripts/linear-audit.py` が持つ。
10. **2 cycle 続けて持ち越された Issue は Cycle から外す**。外すのは取り下げではなく triage への差し戻しで、Project・Priority・ラベルは保持したまま Backlog / Todo に置く。繰り越し以外の出口が無いと、Cycle は commitment ではなく堆積物になる。
11. **同じ所見が 2 回続けて出たら扱いを変える**。ラベル・メタデータ級は監査セッションがその場で直して記録する。構造・方針級は人間へ通知し、通知しないまま次回へ持ち越さない。監査が同じ指摘を書き足すだけの装置になると、検出はできても処置されない状態が固定する。

Cycle から外した Issue は Project・Priority・ラベルを保持するため、Backlog / Todo からいつでも再投入できる。外すことと取り下げることを混同しない。

---

## 4. ラベル分類体系（コロン式に統一）

ラベル prefix はコロン式に統一する。技術領域 `area:` も複数付与するが、Linear 上では label group 化せずフラットなラベルとして運用するため、コロン式でも複数選択できる（Linear のラベルグループ排他制約は明示的なグループにのみ働き、`area:foo` のような prefix 付きフラットラベルには働かない。2026-06-03 決定、後日コロン式へ統一）。Phase 4 完了までは各 repo の Delta / `AGENTS.md` が定める active label を優先する。

| プレフィックス | 用途 | 値 |
| -- | -- | -- |
| `repo:` | 対象 GitHub リポジトリ（実 repo 名をそのまま反映） | 各 1 つ必須（Phase 4 完了までは旧 repo ラベルも有効） |
| `area:` | 技術領域 | 1 つ以上（複数付与可。Linear ではフラットなラベルとして運用。旧 `area_*` は Phase 4 で付け替え） |
| `agent:` | 次の AI 役割（§2） | 原則 1 つ（人間専任タスクを除く） |
| `type:` | Issue の役割 | `tracking` / `implementation` / `review` |
| `gate:` | 人間ゲート（横断フラグ） | `human-required` |
| `kind:` | 変更カテゴリ | `feature` / `improvement` / `bug` / `docs` |
| `Migrated` | 由来フラグ（他サービス起票） | 任意 |

ルール:
- `area:` は複数選択可能にするため Linear label group にせずフラットなラベルとして運用する（例 `area:converter-core`）。`repo:` は実 GitHub repo 名をそのまま使い区切りを正規化しない。
- **`area:docs` は repo 横断の共有 area**（ドキュメント・ガバナンス・規約の整合）。特定 repo の技術領域に属さない文書系 Issue に付与する。repo 固有の技術領域 area（例: azooKey `area:settings`（設定アプリ / schema）/ `area:build`（CMake・CTest・CI・コード署名・MSIX パッケージング））は各 repo の Delta / `AGENTS.md` で定義する。
- **人間ゲートは `type:` ではなく `gate:human-required`（横断フラグ）で表す。** 例: 人間確認が必要なレビュー Issue は `type:review` + `gate:human-required`。旧 `type:human-gate` は本規約で廃止。移行期は旧ラベルが残る Issue があるため Phase 4 で `gate:human-required` へ付け替える（読むときは両対応）。
- **変更カテゴリは `kind:*` を正典とする**（`feature` / `improvement` / `bug` / `docs`）。旧 `Feature` / `Improvement` / `Bug` / `enhancement` / `documentation` は Phase 4 まで移行互換として扱い、Phase 4 で物理退役（ラベル削除は設定画面で手動）する。`Migrated` は由来フラグとして存続。
- **Phase 4 完了までの移行互換**: 旧 `repo_*` / 旧 `area_*` / 旧カテゴリラベルのみが付いた Issue も、Ready / Missing Metadata / 週次監査では欠落扱いしない。新規作成・更新時は repo の Delta または既存 `AGENTS.md` の現行ラベルを優先し、Phase 4 後に `repo:` / `area:` / `kind:*` へ収束する。

---

## 5. Agent Task Format（Issue 記述）

Fields: Background / Goal / Repository / Area label / Files / Expected behavior / Plan / Done criteria / Owner / Handoff notes

Claude が Issue を作成・更新・分割するときは、本文冒頭に **Agent Handoff** ブロックを置く:

```md
## Agent Handoff

- Type:
- Agent:
- Repo:
- Goal:
- First files to inspect:
- Protected areas:
- Required validation:
- Expected PR size:
- Blocks:
- Blocked by:
- Codex safety: `agent:codex-*` はルーティングのみ。Claude は Codex へ delegate / assign / mention しない（実行は人間 lead のみ。mention トークンは候補段階で書かない）。
```

Issue タイプ（`type:`）:
- `tracking` — 進捗管理・子 Issue 集約・順序整理。直接実装しない。
- `implementation` — 1 PR で完了可能な実装・修正・テスト追加。
- `review` — 設計レビュー・PR レビュー・整合性確認。

次に AI へ渡す Issue の並び（最大 3 件）は description に書かない。Project は Project Status Update に、tracking Issue は `Status snapshot YYYY-MM-DD` 見出しのコメントに置く。いずれも日付つきの追記型で古さを測れる器である（§7.2 / §12）。

---

## 6. Agent Run Report Format（作業後コメント）

作業後に Linear Issue へ残すログ形式（仕様ではなく作業ログ）:

- **Agent**: Claude Code / Codex / ChatGPT / other
- **Read**: 確認した正典（AGENTS / roadmap / README / 連携 GitHub Issue / 関連 docs）
- **Changed**: 変更ファイル、または Linear のみの変更
- **Validation**: test / lint / build / 手動確認、スキップ時は理由
- **Findings**: リスク・ブロッカー・follow-up
- **Next**: 次アクションと次オーナー

Codex Cloud を実行した場合は、追加で **Codex Run Record** を残す:

- Execution approved by / approval comment
- Codex task link / branch / commit / PR
- Validation / known limitations
- Human gate required / next reviewer

短く保ち、仕様はコピーせず GitHub へリンクする。

---

## 7. Definition of Ready / Done

### Ready
- Project が正しく設定されている。
- `repo:*` ラベルがある（Phase 4 完了までは repo 固有の旧 repo ラベルも有効）。
- 関連する `area:*` が 1 つ以上ある（Phase 4 完了までは repo 固有の旧 `area_*` ラベルも有効）。
- 次の AI 役割を示す `agent:*` がある。ただし `gate:human-required` または旧 `type:human-gate` の人間専任タスクは `agent:*` を省略してよい。
- 可能なら GitHub Issue / PR リンクが添付されている。
- Goal と done criteria が明確。
- 実行順序が重要な場合、ブロッカーが relation で表現されている。

### Done
- 作業結果が記録されている。
- 検証結果が記録されている（スキップ時は理由）。
- GitHub 正典に対するドキュメント影響を確認済み。
- 可能なら関連 PR / GitHub Issue がリンクされている。
- 必要な follow-up Issue が作成/リンクされている。
- `gate:human-required` または旧 `type:human-gate` の Issue は人間確認が取れている。
- repo 固有 Delta / `AGENTS.md` / `WORKFLOW.md` が追加ゲート（PR マージ、検証メモなど）を要求する場合、それを満たしている。

Done は「Linear 上で運用的に完了」を意味し、GitHub docs のリリース基準を置き換えない。

---

## 7.1 Design / Gate Split（設計層と人間ゲートの分離）

`agent:claude-design` / `agent:claude-review` の AI 設計作業と `gate:human-required` の人間判断が両方絡む Issue は、原則として **設計 Issue** と **人間ゲート Issue** の 2 件に分離する。1 件に同居させると、設計 PR のマージで Issue 全体が誤って Done 化し、未達の人間ゲートを飛び越える（管制塔の状態が実態と乖離する）。

### 7.1.1 分離テスト（分割するか否か）

人間が必要とする作業が、**AI セッションでは生み出せず、かつ同一レビュー内で人間が即座に記録もできない**もの（実機・実データ計測 / 購入・契約 / 外部アカウント開設 / 法務・ライセンス確定 / 署名値設定 / 本番デプロイ 等）を含むなら **分割必須**。

人間の入力が「AI が用意した決定ブリーフの『決定』欄を埋める」程度で、**同一サイクル内に完了**するなら、分割せず単一 Issue のままその場でゲートをクリアしてよい（決定内容・合意日を検証メモに記録）。

### 7.1.2 分割後の規格

| 項目 | 設計 Issue | 人間ゲート Issue |
| -- | -- | -- |
| タイトル | 元のまま | `Human Gate: <決定内容>` で開始（roadmap コード併記可: 例 `D-04-A`） |
| `agent:*` | `agent:claude-design` / `claude-review` | 付けない（人間専任。§2 / §7 Ready で免除） |
| `gate:human-required` | **付けない** | 付ける |
| `type:` | `review` 等 | `review` |
| `repo:` / `area:` / `kind:` | 通常どおり | 通常どおり |
| 関連付け | — | 設計 Issue へ `related`、所属 tracking / epic を parent、リリース律速なら下流へ `blocks` |
| Done 条件 | 調査 / spec / 雛形 / ハーネスが PR マージ + 検証メモで確定 | 人間判断・実機検証 + 決定 / 計測値の記録（検証メモ） |

設計 Issue は純 AI スコープになるため、Done 化時に `gate:human-required` を残さない（クリア済みなら除去、未クリアなら Done にしない）。

### 7.1.3 自動 Done の防止（設定・運用）

事故の根本原因は、Linear–GitHub 連携が PR マージ / ブランチ名連動で Issue を Done 化し、人間ゲートを飛び越える点にある。次の多層で防ぐ:

1. **連携設定（採用）**: チームの GitHub 連携で「PR マージ時の遷移先」を **Done ではなく Merged** にする（`Merged` は team `Dev` に作成済み。2026-09-01・DEV-924）。最終 Done は必ず人間 / Claude の明示操作とする。これにより設計 PR のマージは Merged で止まり、人間ゲートの取りこぼしが構造的に起きない。マージ済みの課題がレビュー待ちの課題と混ざらないため、In Review は「open PR あり」を保ち続ける（§3.1。この設定変更は人間 lead が Linear 側で行う）。**ただし自動遷移が働くのはクローズ系リンク（`Fixes` / `Closes` / `Resolves`）で参照した課題に限る。** 非クローズ参照の課題は連携が動かさないため、-4 の手動処置が唯一の経路になる（2026-09-01 に PR 10 本をマージした際、`Part of` で参照された DEV-928 / DEV-929 が In Progress のまま残った）。
2. **closing キーワードの使い分け**: 設計 PR は設計 Issue のみを `Fixes DEV-<design>` で閉じる。人間ゲート Issue は closing キーワードで参照せず `Ref DEV-<gate>` / `Part of DEV-<gate>` のみとし、PR リンクは attachment で手動付与する。 GitHub ミラー Issue も同様に、人間ゲート / 検証メモ待ちの Issue では `Fixes #<N>`（マージで GitHub Issue をクローズ → Linear 同期で Done 化し Merged を迂回する）を避け、`Refs #<N>` 等の非クローズ参照にする。 この書き分けの副作用として、`Part of` / `Refs` で参照した課題は merge 自動遷移も止まる。自動 Done を防ぐのと引き換えに Merged への遷移も自動では起きないため、-4 で手動処置する。
3. **分割の徹底**: §7.1.1 に該当する Issue は分割し、auto-close が人間ゲート Issue に当たらないようにする。
4. **検証メモのフロー化**（§7.2 遷移時記録原則の適用例）: PR のマージを検知したセッション（マージを実行した人間から引き継いだエージェント・PR 監視エージェント・直後に該当 repo で作業するセッション）は、その場で検証メモ（確認したテスト名・CI ジョブ名を含む）を Issue にコメントし、`gate:human-required` が無ければ Done へ明示遷移する。**非クローズ参照の課題では Merged への遷移そのものも手動で行う**（-2 のとおり連携は動かさない）。自動遷移を前提にしない。遷移時にやることは §7 Done チェックリストに集約してある。検証メモを週次監査でまとめて書く運用は Merged の滞留を生む（DEV-662 で 18 件、DEV-925 で 11 件が一括処理になった）ため、監査での記入は取りこぼしの回収に限る。

本節の遷移規約（PR マージ→Merged、Done は明示遷移）は、各 repo の `AGENTS.md` / `docs/GITHUB_LINEAR_MAPPING.md` / `docs/WORKFLOW.md` 等のライフサイクル要約より**優先**する。要約側が「PR マージ→Done」と記す場合は本節に読み替え、可能なら要約側も更新する。

---

## 7.2 記録の鮮度

tracking Issue の進捗欄と Project description は、子 Issue の状態を人手で写した**手書きキャッシュ**である。無効化の仕組みがないため、元データが動くたびに黙って実態とずれる（DEV-775: 進捗欄が 27 日放置され、description は全面更新の 2 時間後に子 3 件の Done 遷移で再び陳腐化し、検出から修正まで 15 日かかった）。全面更新の 2 時間後に腐る以上、更新頻度を上げても解決しない。書ける内容を絞り、腐る面そのものを減らす。

**原則**: Linear の状態から**導出できる記述を description に書かない**。現在は生データ（sub-issue リスト・blocked-by リレーション・mention チップ）に語らせる。導出できない意図・構造・過去の事実は書いてよい。

| 区分 | 例 | 判定 |
| -- | -- | -- |
| 構造と意図 | 分解の意図、spec アンカー、正典の所在、依存の理由 | 書いてよい（腐らない） |
| 日付つき完了記録 | 「Done（PR #89、2026-08-31）」 | 書いてよい（過去の事実） |
| 状態依存の記録 | Current focus、Next AI Tasks、Health | description に書かず、**日付つき追記型の器**へ置く（下記） |
| 導出可能な現在 | 状態名（`In Review` / `Merged` 等）、「着手可」「〜待ち」「残 N 件」 | 書かない |
| 状態スナップショット | 「進捗（YYYY-MM-DD 時点）」節 | description に置かず、`Status snapshot YYYY-MM-DD` 見出しのコメントへ積む |

「導出可能な現在」は上の語彙の列挙ではなく、**Linear を引けば分かる記述すべて**を指す（列挙は例示）。コメントは日付を持つ過去ログとして読まれるため腐らない。description が腐るのは「現在」を名乗るからである。

**器で担保する**: 状態依存の記録をゼロにはできない。次に何を渡すかは Linear から導出できないからである。消せない以上、鮮度は規律ではなく**器**に持たせる。Project Status Update と Issue コメントは日付つきの追記型で、最終更新からの経過日数を API が返す。つまり古さを機械判定できる。description は上書き型で日付を持たないため、古さを測れない。測れない面に「毎回更新する」という規律を課しても守られなかった（実測: `Next checkpoint` の 63 日超過、Project の Next AI Tasks への Done / Canceled 混入 27 件）。

- Project の状態依存記録 → **Project Status Update**（書式と周期は §12）。
- tracking Issue の状態依存記録 → **`Status snapshot YYYY-MM-DD` 見出しのコメント**。

**遷移時記録原則**: 器が日付を持つぶん、個々の遷移を追いかけて直す義務は課さない。読者は日付を見て古さを判断できるためである。義務は**周期内に 1 本、全節そろったエントリを積むこと**に置く（§12）。節を欠いたエントリを積むと「最新を読めば足りる」が崩れ、読者が履歴を遡ることになる。周期の逸脱は週次監査（§11）が機械判定で拾う。

**到達性**: 現在を description から外すぶん、状態は Linear を引いて得る。作業開始時に対象 Issue の子・blocked-by と §10 のキューを取得してから動く。

---

## 8. Navigation Rules

- Linear リンクはナビゲーションと計画にのみ使う。
- Tracking item は小さな作業項目を束ねる。
- Order/blocking マーカーは実際の順序にのみ使う。
- Reference マーカーは緩い関連に使う。
- Same/duplicate マーカーは真に同一の作業にのみ使う。

GitHub docs remain canonical.

---

## 9. Do Not Do

- GitHub roadmap の詳細を Linear docs にコピーしない。
- Linear docs を仕様(spec)として扱わない。
- 人間ゲート Issue を手動確認なしでクローズしない。
- 実機・署名など人間判断が必要な作業を AI 判断だけで Done にしない。
- Migrated 作業で GitHub Issue リンクを省略しない。
- Linear 作業から README に進捗表/TODO を増やさない。
- Claude から Codex へ assign / delegate / mention しない。実行は人間 lead が明示許可コメント後に自ら行う（Claude は実行指示文の下書きまで）。
- Claude / エージェントはいかなる Linear コメント / Issue にもリテラルな Codex mention トークン（`@` + `Codex`）を再生産しない（無害化する）。承認後に実際の mention で起動するのは人間 lead のみ。
- triage rule で Codex を自動 delegate しない。
- `agent:claude-*`（design / review）と `gate:human-required` を同居させたまま PR で auto-close される構成にしない。分離テスト（§7.1.1）に該当するなら 2 件に分割する。
- Done 化時に `gate:human-required` を残さない（人間判断がクリア済みなら除去、未クリアなら Done にしない）。

---

## 10. Control Tower Views（推奨ビュー）

各プロジェクトで以下のフィルタビューを用意する（Project でスコープ）:

- **Ready for Claude Design**: `agent:claude-design` + Backlog/Todo
- **Ready for Claude Review**: `agent:claude-review` + Todo/In Review
- **Codex Candidate Queue**: `agent:codex-impl` + Todo + delegate なし + 非ブロック（候補。実行はしない）
- **Codex Review Candidate Queue**: `agent:codex-pr-review` + Todo/In Review + delegate なし（候補。実行はしない）
- **Delegated to Codex**: delegate = Codex（暴発・実行中・実行済みの監査用。Candidate と必ず分離する）
- **Needs Human Verification**: `gate:human-required`（+ not Done。旧 `type:human-gate` も読む）
- **In Review**: status In Review（open PR のレビュー・マージ待ち。open PR の無い課題が現れたら §11 で再分類する）
- **Merged / Needs Verification Memo**: status Merged（マージ済み・検証メモ待ち。滞留は検証メモ債務として §11 で点検する）
- **Missing Metadata**: repo / area / agent ラベル欠落、または Migrated Issue の GitHub リンク欠落（移行期の旧 repo/area ラベルと `gate:human-required` / 旧 `type:human-gate` 人間専任タスクの `agent:*` 免除を考慮）

Codex Candidate（`agent:codex-*` 候補）と Delegated to Codex（delegate 済み・実行）は絶対に混ぜない。`delegate = Codex` が見えたら必ずレビュー対象にする。

ビューはレーダー画面であって仕様ではない。曖昧なら GitHub docs と連携 GitHub Issue を見てから動く。

---

## 11. Recurring Control Tower Audit（週次・統一チェックリスト）

各プロジェクトに `[Recurring] Linear control tower audit — <PROJECT>` を 1 件持つ
（`type:review` + `agent:claude-review` + `repo:*`。この recurring audit Issue 自体は `area:*` 免除）。チェック項目:

- [ ] Project 未設定の Issue
- [ ] `repo:` ラベル欠落（Phase 4 完了までは repo 固有の旧 repo ラベルも有効）
- [ ] `area:*` ラベル欠落（Phase 4 完了までは repo 固有の旧 `area_*` ラベルも有効。recurring audit Issue 自体を除く）
- [ ] `agent:` ラベル欠落（`gate:human-required` または旧 `type:human-gate` の人間専任タスクを除く）
- [ ] Migrated なのに GitHub リンク欠落
- [ ] 人間確認が要るのに `gate:human-required` 欠落
- [ ] Tracking Issue で子が未リンク
- [ ] 実行順序を表さなくなったブロッカー
- [ ] Done なのに検証ノート欠落
- [ ] `agent:claude-*` と `gate:human-required` が同居した Issue（Design / Gate 分割漏れ。§7.1）
- [ ] Done の設計 Issue（`agent:claude-*` 付き）に `gate:human-required` が残っている（分割後に除去すべき stale ラベル。人間ゲート Issue 自体が検証メモ付きで Done なのは正常）
- [ ] 人間ゲート Issue が PR の auto-close 対象になっている（closing キーワードで参照されている）
- [ ] `type:tracking` Issue が Codex 実行候補（`agent:codex-*` + Todo・delegate なし・非ブロック）として Candidate Queue（§10）に現れている（tracking は束ね専用で直接実装しない。§5 / §8。実行は子 implementation Issue へ降ろす）

Codex safety checks:

- [ ] 人間 lead の明示許可なく Codex へ delegate された Issue がない
- [ ] 明示許可なく Codex mention トークン（`@`+`Codex`）を含むコメント / Issue 本文 / テンプレートがない
- [ ] `agent:codex-*` をルーティング（候補）ラベルとしてのみ扱っている
- [ ] Codex 実行開始後に Todo へ放置された delegate 済み Issue がない
- [ ] Codex 完了タスクに task / PR / commit リンク・検証・残リスクが記録されている
- [ ] 人間ゲート Issue が人間確認なしで Done になっていない
- [ ] ブロック中の Codex 候補が Ready として表示されていない
- [ ] 依存する設計 Issue（`agent:claude-design` / `agent:claude-review`）が未完了（not Done）のまま、その下流 implementation Issue に Codex 実行許可（§2.1 の許可コメント / delegate）が出ていない（設計固定前の実装着手＝仕様の雰囲気決定を防ぐ。§2.1 / §3 / §7.1）

機械判定項目（レーン衛生・記録の鮮度・期日）は **origin の `scripts/linear-audit.py` が正典**として持つ。監査セッションはまずこれを実行し、`CONFIRMED` を処置してから、人力を上下の判断が要る項目に充てる。`REVIEW` はヒューリスティック（description の文言と実状態の突合）で過検出を前提とするため、採否は人が決める。規約側に項目を列挙し直さない（インシデントごとに足し続けると、規約自身が無効化機構のない手書きキャッシュになる）。

Design / Implementation spec-first checks（設計 §2 / §7.1 の spec-first 分業の担保）:

- [ ] `agent:codex-impl` のフィーチャー（`kind:feature`。Phase 4 完了までは旧 `Feature` / `enhancement` も対象）が、対応する `docs/*-spec.md` 節（または roadmap の該当マイルストーン節）で当該サブ課題の難所（IPC payload / JSON schema・境界値・アルゴリズム・責務境界）を確定する前に In Progress 以降へ入っていないか（未着手の課題だけでなく、既に In Progress の課題も対象）。spec が未確定のまま実装着手していないか。
- [ ] 専用 `agent:claude-design` 課題を持たないフィーチャーでも、tracking / 比較レポート / roadmap 節 / `docs/*-spec.md` のいずれかで難所が上流確定され、実装課題からアンカー参照されているか。
- [ ] 波（wave）分割されたフィーチャーで、先行波の spec だけが書かれ後続波が未確定のまま実装着手していないか（just-in-time spec が波ごとに守られているか）。

Rule: Linear のルーティングのみを点検する。GitHub docs が正典。

---

## 12. Project の記録（description と Status Update）

Project の記録は 2 つの器に分ける。**腐らないものを description に、状態依存を Status Update に置く**（§7.2）。

### description（上書き型・日付なし）

次の 4 つの H2 だけを置く（順序もこの通り）。状態は書かない。

```md
## Operating contract

Lead: <name>
Codex safety: `agent:codex-*` は候補ラベルのみ。Claude は Codex へ delegate / assign / mention しない（実行は人間 lead のみ）。
Current focus / Next AI Tasks / Next checkpoint / Health は最新の Project Status Update を参照する（§7.2）。

## Human Gate

* <DEV-xx> <判断の内容> は人間確認必須。（状態は書かない。キューは §10 の Needs Human Verification ビュー）

## Canonical docs

正典: <REPO>/docs/linear-conventions.md, <REPO>/docs/<WORKFLOW or ROADMAP>.md

## Stage map

<ステージ名と定義だけを置く。達成状態・残件・進行中の Issue は書かない（正典は repo の roadmap。§1 / §7.2）>
```

Operating contract の最終行は Status Update への**ポインタ**である。参照先の構造を指すだけで状態を含まないため腐らない。

### Status Update（追記型・日付つき）

**14 日に 1 本以上**積む。各エントリは次の 3 節をすべて持たせる。1 節でも欠けると「最新を読めば足りる」が崩れ、読者が履歴を遡ることになる。

```md
## Current focus

<いま何を優先するかと、その理由。1〜3 文>

## Next AI Tasks

1. DEV-xx <内容>
2. DEV-xx <内容>
3. DEV-xx <内容>   （最大 3 件・すべて not Done）

## Next checkpoint

<YYYY-MM-DD>。<その日に判定する内容>
```

Health は Linear の health フィールドで設定し、本文に重ねて書かない。

### マイルストーン命名
- 1 プロジェクト内では 1 つのトークン体系に統一する（`Stage N` / `P N` / `MVP-N` のいずれか）。
- 数字を綴り字にしない（`Stage Three` ではなく `Stage 3`）。
- 各マイルストーンは Stage map のステージに対応させる。

---

## 13. Project Delta — azooKey Desktop

- PROJECT_NAME: azooKey Desktop / Windows IME MVP
- REPO: dolquis/azooKey-Desktop
- REPO_LABEL: repo:azooKey-Desktop
- CANONICAL_DOCS: AGENTS.md, README.md, plans/windows-port-roadmap.md, docs/*-spec.md
- AREA_LABELS: area:tsf-tip, area:inference-host, area:ipc, area:learning, area:converter-core, area:settings, area:build, area:docs
- LINEAR_STATUS_MAP: Backlog/Todo → In Progress → In Review（Draft PR 提出済み）→ Merged（PR マージ済み・検証メモ待ち）→ Done（レビュー合格 + マージ + 検証メモ記載後）
- STAGE_MAP: `plans/windows-port-roadmap.md` の M-number / Phase 定義を正典とする（例: M0, M1, M2... と Phase 1〜7 + 独立トラック）。Linear 側へ転記する場合も roadmap の milestone 名を使う。

### 週次監査の repo 固有追加項目（spec-first 分業の担保）

共有コア §11 の週次 control tower audit チェックリストに加えて、本 repo では設計（`agent:claude-design`）/ 実装（`agent:codex-impl`）分業（§2 / §7.1）の spec-first 規律を次の項目で点検する:

- [ ] `agent:codex-impl` のフィーチャー（`kind:feature`。Phase 4 完了までは旧 `Feature` / `enhancement` も対象）が、対応する `docs/*-spec.md` 節（または roadmap の該当 M 節）で当該サブ課題の難所（IPC payload / JSON schema・境界値・アルゴリズム・責務境界）を確定する前に In Progress 以降へ入っていないか（未着手の課題だけでなく、既に In Progress の課題も対象）。spec が未確定のまま実装着手していないか。
- [ ] 専用 `agent:claude-design` 課題を持たないフィーチャーでも、tracking / 比較レポート / roadmap 節 / `docs/*-spec.md` のいずれかで難所が上流確定され、実装課題からアンカー参照されているか。
- [ ] 波（wave）分割されたフィーチャー（例 M62-A/B/C/D、モデルライフサイクル A〜D）で、先行波の spec だけが書かれ後続波が未確定のまま実装着手していないか（just-in-time spec が波ごとに守られているか）。
- [ ] ユーザーから見える既定の挙動を新たに決める `kind:bug`（既存仕様からの逸脱を直すのではなく、未定義だった既定値・状態遷移を確定する修正）が、対応する `docs/*-spec.md` 節を確定する前に In Progress 以降へ入っていないか。`kind:feature` と同じ spec-first 規律の対象とする。

この点検は「難しい仕様を実装時に codex が雰囲気で決める」状態を構造的に防ぐためのもの。カテゴリラベルが `kind:bug` でも、決めるものが既定値や状態遷移であれば実質は仕様策定であり、同じ規律が要る（例 DEV-738: composition 中の OEM 記号キーの既定字種と状態遷移は起票時点で未定義で、`docs/tsf-deep-integration-spec.md` §3.5 が新設されるまで正典が無かった）。設計/実装分業は共有コア（§2 / §7.1）に属するため、本追加項目は origin（`dolquis/agent-ops`）での共有コア §11 への昇格候補とし、昇格までは本 Delta を暫定の正典とする。

### 週次監査の repo 固有追加項目（アーカイブ衛生）

共有コア §11 に加えて、本 repo では次を点検する:

- [ ] 未完了（Backlog / Todo / In Progress / In Review）のままアーカイブされた Issue がない（アーカイブは Done / Canceled / Duplicate に限る。検出したら復帰または Canceled / Duplicate として明示クローズする）

アーカイブされた課題は通常のビュー・Cycle・Project 集計から消えるため、未完了のままアーカイブすると「Linear が状態の正典」という管制塔の前提が崩れる（DEV-663 で検出された再発防止項目）。本項目も共有コア §11 への昇格候補とし、昇格までは本 Delta を暫定の正典とする。

### 週次監査の repo 固有追加項目（保留理由の失効検出）

共有コア §11 に加えて、本 repo では次を点検する:

- [ ] 「前提となる仕組みが無い」ことを理由に In Review / Todo で保留されている Issue のうち、その仕組みを用意する Issue が既に Done になっているものがないか
- [ ] テストギャップ・CI ギャップを解消する Issue を Done にするとき、そのギャップを保留理由に挙げている Issue を逆参照して判定をやり直したか

検証メモは書いた時点の前提を固定するが、その前提は他 Issue の着地で失効しうる。失効しても検証メモは自動更新されないため、保留が惰性で続く。DEV-543 は「CI に pin モデルの実ロードジョブが無い」ことを Done 保留の理由にしていたが、この前提は DEV-547（当該 CI ジョブの追加）の着地時点で失効していた。にもかかわらずメモは更新されず、9 日間 In Review に留まった。ギャップ解消側の Issue には必ず「そのギャップを理由に保留されている Issue」が存在するため、Done 遷移時の逆参照を規律として持つ。本項目も共有コア §11 への昇格候補とし、昇格までは本 Delta を暫定の正典とする。

### 週次監査の repo 固有追加項目（実装課題と人間ゲートの分離）

共有コア §11 に加えて、本 repo では次を点検する:

- [ ] `agent:codex-impl` の実装 Issue に `gate:human-required` が同居していないか（分離テスト §7.1.1 に該当するなら `Human Gate: …` へ分割し、実装側からラベルを外す）

共有コア §11 の分割漏れ検出は `agent:claude-*` との同居のみを見ており、`agent:codex-impl` との同居を拾わない。しかし §7.1.1 の分離テストは担当エージェントではなく**人間作業の性質**（実機・実データ計測など）で分割要否を決めるため、実装 Issue でも同じ規律が要る。むしろ実装 Issue のほうが PR マージを伴うぶん、§7.1.3 が挙げる事故（マージで Issue が Done 化し未達の人間ゲートを飛び越える）に近い。

2026-08-10 の点検では 6 件（DEV-91 / 178 / 181 / 217 / 443 / 444）が該当し、いずれも本文に「実機 Win11 確認」を明記したまま `agent:codex-impl` と同居していた。分割して DEV-757〜762 を新設し、実装側からラベルを外した。本項目も共有コア §11 への昇格候補とし、昇格までは本 Delta を暫定の正典とする。

### 週次監査の repo 固有追加項目（親 Done への巻き込み一括 Done 検出）

共有コア §11 に加えて、本 repo では次を点検する:

- [ ] 親 Issue を Done にした前後の短い時間帯（目安 5 分以内）に、子 / 関連 Issue が複数まとめて Done へ遷移していないか。該当する場合、各 Issue が個別の検証メモまたは対応 PR を持つか
- [ ] 直前のコメントが「未完了のまま維持する」「別課題で追跡する」と明記した Issue が、その直後に無記録で Done 化されていないか
- [ ] `startedAt` が null（In Progress を一度も経ていない）まま Backlog / Todo から直接 Done になった `type:implementation` Issue がないか
- [ ] **子 Issue を Done にした直後（目安 1 分以内）に、その親 Issue も無記録で Done へ遷移していないか**（automation 起因の逆方向の巻き込み。下記）

共有コア §11 の「Done なのに検証ノート欠落」は Issue を単体で見るため、一括誤 Done の**同時性**を拾えない。
検証メモが 1 件でも付いていれば個別チェックは通過してしまい、同じバッチで巻き込まれた無記録の兄弟 Issue が残る。

2026-08-16 の点検では DEV-767（`gate:human-required`）と DEV-768 が、親 DEV-674 の Done 遷移と同一秒（`14:12:49`）に無記録で Done 化されていた（DEV-772）。
DEV-674 の検証メモ自身が「DEV-767 は未完了のまま維持します」と明記した直後の遷移であり、コメント本文と状態遷移が食い違っていた。
その後の調査で DEV-768 の変更は `origin/main` に存在しないことが確認され、両者とも Todo へ差し戻した。
同種の一括誤 Done は 2026-06-11 にも発生しており（DEV-124 派生 8 件）、DEV-15 の再発防止策だけでは検出できていない。

3 項目目の `startedAt` 検査は、遷移時刻に頼らず単体で判定できる補助線として持つ。
`type:implementation` の Issue が実装フェーズを経ずに完了することは通常なく、巻き込み Done の機械的な指標になる。

4 項目目は**逆方向の巻き込み**を拾う。
1〜3 項目目はいずれも「親を Done にして子が巻き込まれる」向きを前提としており、**子の完了が親を自動 Done 化する**経路を検出できない。
これは人間やエージェントの操作ミスではなく Linear 側の自動遷移であり、操作者に自覚がないぶん記録が残らない。

2026-08-21 に DEV-555 で観測した。
唯一の子である DEV-765 を Done にした 375 ms 後に、親の DEV-555 が Done へ遷移している。
DEV-555 は pipe DACL の AppContainer capability ACE を扱う未解決の `kind:bug` で、検証メモも無かった。
Todo へ差し戻し、経緯を当該 Issue へ記録した。

この向きは、子を 1 件しか持たない親で特に起きやすい。
最後の子を閉じた瞬間が親の自動完了条件を満たすためである。
検出は 1〜3 項目目と同じく遷移時刻の近接で行うが、**親子関係の向きを逆に見る**点だけが違う。

以上 4 項目も共有コア §11 への昇格候補とし、昇格までは本 Delta を暫定の正典とする。

### 週次監査の repo 固有追加項目（Linear の状態と main の実体の突合）

共有コア §11 に加えて、本 repo では次を点検する:

- [ ] **方向 A（Done だが成果物が main に無い）** 直近サイクルで Done へ遷移した `type:implementation` Issue について、本文の受け入れ条件が名指しするファイル / ジョブ / フラグが `origin/main` に実在するか。`startedAt` が null のまま Done になった Issue は重点対象とする（既存の一括誤 Done 検出と同じ signal を使い回せる）
- [ ] **方向 B（成果物はあるが課題が開いたまま）** 未 Done の実装 Issue のうち、本文の「現象」が現行 `origin/main` で再現しないもの（対応するテストが存在する、当該コードが書き換わっているなど）がないか
- [ ] 検出時の扱い: 方向 A は元 Issue を復帰させず**新規 Issue として再起票**し、元 Issue を related で参照する（Done の履歴を書き換えない）。方向 B は検証メモを添えて Done / Canceled へ遷移させる
- [ ] 点検はサンプリングでよく、全 Done Issue の全数確認は求めない（監査コストが実装コストを超えないこと）

既存の点検項目はいずれも Linear の中だけを見ており、Issue の記述と repo の実体が一致しているかを問わない。
検証メモ欠落の検出（共有コア §11 および DEV-772）が見るのはメモの有無であって、メモが指す成果物の実在ではない。
アーカイブ衛生と一括誤 Done 検出が見るのは status と遷移時刻であり、これも repo を参照しない。
そのため、Done の記録が残ったまま成果物だけが存在しない状態は、どの項目にもかからずに残り続ける。

方向 A は 2026-08-21 の洗い出しで 3 件見つかっており、単発の事故ではない。
DEV-471（`CI: pre-commit 一式の独立品質ゲート（quality.yml）`、Priority High）は 2026-07-05 の作成から約 2 時間後に Done となり、`startedAt` は null のままだった。
`.github/workflows/quality.yml` は存在せず、CI に pre-commit / gitleaks の実行も 1 件も無い。
High 優先度の CI 秘密走査が約 1 か月半のあいだ誰にも所有されないまま放置され、DEV-827 として再起票して初めて追跡対象へ戻った。
DEV-473（`bench: --json 出力と p95/p99 回帰検知`）は Done かつ archived だが `bench/live_bench.cpp` / `bench/zenzai_bench.cpp` に `--json` / `--output` が無く、DEV-829 として再起票した。
このずれ自体は DEV-604 が 2026-07-20 の時点で本文に記録していたが、監査の定型点検に無いため tracking Issue の本文に留まっていた。
DEV-469（`CI: sanitizer プリセット（ASan/UBSan）と nightly 実行`）も同様に Done / archived で成果物が無く、DEV-606 が follow-up として起票し直してから実装が入っている（PR #233）。

方向 B は、既に直っている Issue が Todo に残って triage のたびに再評価コストを払う状態を指す。
DEV-72（`LearningStore: reading に raw tab を含むと Save でフィールド境界が壊れる`）が実例で、`learning/src/LearningStore.cpp` の `EscapeTsvField` / `UnescapeTsvField` / `SplitKey` によりエスケープ済み形式が実装され、`learning/tests/learning_test.cpp` が reading への tab / backslash 混入の往復と旧形式からの移行まで検証している。
方向 A ほど害は大きくないが、放置すると Todo が「着手すべき作業」の一覧として信用できなくなる。

以上 2 方向も共有コア §11 への昇格候補とし、昇格までは本 Delta を暫定の正典とする。

### 管制塔改善パイロット（暫定運用規則、2026-08-08〜）

「Linear 開発モデル改善提案（2026-08-03）」のレビューで採用した規則を、本 repo と
`dolquis/QC_Compass` の 2 repo で先行運用する。4 週間の検証後、実証された規則のみ
共有コア 0.6 として origin（`dolquis/agent-ops`）へ昇格させる。昇格までは本 Delta を暫定の正典とする。

1. **動的状態は Status Update へ** — Current focus / Next AI Tasks / Next checkpoint / Health は
   Project description に書かず、週次の Project Status Update に記録する。description は静的な
   運用契約（責務・Human Gate 分類・正典リンク・Stage map）のみを持つ。
2. **Project Status の判定基準** — 非 tracking の Issue が実際に In Progress（または今週開始予定）の
   ときのみ Project を In Progress とする。停止中は Planned へ戻し、最新 Status Update に停止理由と
   再開条件を書く。
3. **Milestone 運用** — active stage と next stage の Milestone を維持し、design / implementation Issue は
   起票時に Milestone を設定する（Ready 条件に追加）。tracking の進捗計算は子 Issue で行う。
   期限（target date / due date）は実際に判定・実行する日にだけ設定し、超過したまま放置しない
   （再設定するか撤去し、理由を Status Update に書く）。
4. **Cycle 規律** — `type:tracking` と、今週実行しない `gate:human-required` Issue は Cycle に入れない。
5. **ラベル整合** — `type:tracking` に `agent:codex-*` を付けない（tracking の agent は
   `claude-design` / `claude-review`、またはなし）。
6. **merged + In Review の滞留監査** — 週次監査で「関連 PR がマージ済みなのに In Review のままの
   非ゲート Issue」を列挙し、検証メモ記入 → Done 遷移で 24 時間以内に解消する。
   自動 Done への変更は行わない（§7.1.3 の維持が前提）。
7. **対応表の状態列廃止** — GitHub ↔ Linear Mapping 文書へ動的状態（Issue 状態・PR マージ状態・
   Issue / PR 一覧）を手動転記しない。規約と例外の記録のみを置く。スナップショットが必要なら
   Linear / GitHub API からの生成物とする（生成物を手編集しない）。
8. **Maintenance exception** — Dependabot / 軽微な CI・docs 保守 / 緊急対応 / ユーザー明示の単発保守は
   Linear Issue なしの PR を許可し、PR 本文に区分・理由・製品挙動への影響・後追い起票の要否を
   記録する。製品挙動・仕様・Human Gate に影響する場合は後追いで起票する。

週次監査の追加点検項目（監査 CLI 導入までは手動）: merged + In Review 24 時間超の非ゲート件数 /
tracking + `agent:codex-*` 件数 / `agent:claude-*` + `gate:human-required` 同居件数 /
description 内の期限切れ checkpoint・Done 済み Next task 件数。いずれも 0 を目標とする。
