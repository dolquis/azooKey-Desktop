<!--
  SHARED CORE — Agent / Linear 運用規約（管制塔モデル）
  この「共有コア」は全リポジトリで同一内容をミラーする。
  個別 repo で直接編集しない。編集は origin（後述）で行い、各 repo へ伝播する。
  version: 0.5-draft   updated: 2026-06-19
  status: 規約確定・origin = dolquis/agent-ops に確定。repo 新設/配置・ラベル移行(Phase 4)は未実施。§2.1 Codex Execution Policy を追加(2026-06-07)。§7.1 Design / Gate Split と PR マージ→In Review 設定を追加(2026-06-19)。
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
4. **Review** — Claude Code または Codex で差分レビュー
5. **Done** — repo 固有の完了条件を満たし、作業結果・検証・PR・ドキュメント影響確認が記録済み

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

Tracking Issue または Project description 上部には、`## Next AI Tasks` 見出しで最大 3 件を書く（Project description ではこの見出しを含む §12 の 5 H2 形式に従う）。

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

1. **連携設定（採用）**: チームの GitHub 連携で「PR マージ時の遷移先」を **Done ではなく In Review** にする。最終 Done は必ず人間 / Claude の明示操作とする。これにより設計 PR のマージは In Review で止まり、人間ゲートの取りこぼしが構造的に起きない（この設定変更は人間 lead が Linear 側で行う）。
2. **closing キーワードの使い分け**: 設計 PR は設計 Issue のみを `Fixes DEV-<design>` で閉じる。人間ゲート Issue は closing キーワードで参照せず `Ref DEV-<gate>` / `Part of DEV-<gate>` のみとし、PR リンクは attachment で手動付与する。 GitHub ミラー Issue も同様に、人間ゲート / 検証メモ待ちの Issue では `Fixes #<N>`（マージで GitHub Issue をクローズ → Linear 同期で Done 化し In Review を迂回する）を避け、`Refs #<N>` 等の非クローズ参照にする。
3. **分割の徹底**: §7.1.1 に該当する Issue は分割し、auto-close が人間ゲート Issue に当たらないようにする。

本節の遷移規約（PR マージ→In Review、Done は明示遷移）は、各 repo の `AGENTS.md` / `docs/GITHUB_LINEAR_MAPPING.md` / `docs/WORKFLOW.md` 等のライフサイクル要約より**優先**する。要約側が「PR マージ→Done」と記す場合は本節に読み替え、可能なら要約側も更新する。

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
- **In Review**: status In Review（PR・設計・実行結果のレビュー待ち）
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

Codex safety checks:

- [ ] 人間 lead の明示許可なく Codex へ delegate された Issue がない
- [ ] 明示許可なく Codex mention トークン（`@`+`Codex`）を含むコメント / Issue 本文 / テンプレートがない
- [ ] `agent:codex-*` をルーティング（候補）ラベルとしてのみ扱っている
- [ ] Codex 実行開始後に Todo へ放置された delegate 済み Issue がない
- [ ] Codex 完了タスクに task / PR / commit リンク・検証・残リスクが記録されている
- [ ] 人間ゲート Issue が人間確認なしで Done になっていない
- [ ] Project description の Next AI Tasks に Done / Canceled の Issue が含まれない
- [ ] ブロック中の Codex 候補が Ready として表示されていない

Rule: Linear のルーティングのみを点検する。GitHub docs が正典。

---

## 12. Project description テンプレート

各プロジェクト description は次の 5 つの独立した H2 セクションで構成する（順序もこの通り）:

```md
## Current control policy

Lead: <name>
Current focus: <一文>
Codex safety: `agent:codex-*` は候補ラベルのみ。Claude は Codex へ delegate / assign / mention しない（実行は人間 lead のみ）。
Next checkpoint: <YYYY-MM-DD>。<その日に判定する内容>

## Next AI Tasks

1. <DEV-xx> <内容>
2. <DEV-xx> <内容>
3. <DEV-xx> <内容>   （最大3件）

## Human Gate

* <DEV-xx> <内容> は人間確認必須。

## Canonical docs

正典: <REPO>/docs/linear-conventions.md, <REPO>/docs/<WORKFLOW or ROADMAP>.md

## Stage map

<ステージ定義>
```

見出しは上記 5 つ（`## Current control policy` / `## Next AI Tasks` / `## Human Gate` / `## Canonical docs` / `## Stage map`）に統一する。単一ブロック化したり `Next AI Task` 等へ表記を揺らさない。

### マイルストーン命名
- 1 プロジェクト内では 1 つのトークン体系に統一する（`Stage N` / `P N` / `MVP-N` のいずれか）。
- 数字を綴り字にしない（`Stage Three` ではなく `Stage 3`）。
- 各マイルストーンは Stage map のステージに対応させる。

---

## 13. Project Delta（各 repo 固有・ここだけ repo ごとに書き換える）

```md
- PROJECT_NAME: <例: azooKey Desktop / Windows IME MVP>
- REPO: <例: dolquis/azooKey-Desktop>
- REPO_LABEL: <例: repo:azooKey-Desktop>
- CANONICAL_DOCS: <例: AGENTS.md, plans/windows-port-roadmap.md, docs/*-spec.md>
- AREA_LABELS: <例: area:tsf-tip, area:inference-host, area:ipc, area:learning, area:converter-core>
- STAGE_MAP: <例: MVP-0 基盤 / MVP-1 TIP / MVP-2 Host・IPC / ...>
```

Delta として各 repo 個別に保持する文書（共有コアには入れない）:
- GitHub ↔ Linear Mapping（対応表データ）
- Decision Log（決定記録）
- Agent Prompt Cards（プロジェクト色のあるプロンプト雛形）

## 13. Project Delta — azooKey Desktop

- PROJECT_NAME: azooKey Desktop / Windows IME MVP
- REPO: dolquis/azooKey-Desktop
- REPO_LABEL: repo:azooKey-Desktop
- CANONICAL_DOCS: AGENTS.md, README.md, plans/windows-port-roadmap.md, docs/*-spec.md
- AREA_LABELS: area:tsf-tip, area:inference-host, area:ipc, area:learning, area:converter-core, area:settings, area:build, area:docs
- LINEAR_STATUS_MAP: Backlog/Todo → In Progress → In Review（Draft PR 提出済み）→ Done（レビュー合格 + マージ + 検証メモ記載後）
- STAGE_MAP: `plans/windows-port-roadmap.md` の M-number / Phase 定義を正典とする（例: M0, M1, M2... と Phase 1〜4）。Linear 側へ転記する場合も roadmap の milestone 名を使う。
- DELTA_DOCS（この repo 固有で別途維持）: GitHub↔Linear Mapping, Decision Log, Agent Prompt Cards

### 週次監査の repo 固有追加項目（spec-first 分業の担保）

共有コア §11 の週次 control tower audit チェックリストに加えて、本 repo では設計（`agent:claude-design`）/ 実装（`agent:codex-impl`）分業（§2 / §7.1）の spec-first 規律を次の項目で点検する:

- [ ] `agent:codex-impl` のフィーチャー（`kind:feature`。Phase 4 完了までは旧 `Feature` / `enhancement` も対象）が、対応する `docs/*-spec.md` 節（または roadmap の該当 M 節）で当該サブ課題の難所（IPC payload / JSON schema・境界値・アルゴリズム・責務境界）を確定する前に In Progress 以降へ入っていないか（未着手の課題だけでなく、既に In Progress の課題も対象）。spec が未確定のまま実装着手していないか。
- [ ] 専用 `agent:claude-design` 課題を持たないフィーチャーでも、tracking / 比較レポート / roadmap 節 / `docs/*-spec.md` のいずれかで難所が上流確定され、実装課題からアンカー参照されているか。
- [ ] 波（wave）分割されたフィーチャー（例 M62-A/B/C/D、モデルライフサイクル A〜D）で、先行波の spec だけが書かれ後続波が未確定のまま実装着手していないか（just-in-time spec が波ごとに守られているか）。

この点検は「難しい仕様を実装時に codex が雰囲気で決める」状態を構造的に防ぐためのもの。設計/実装分業は共有コア（§2 / §7.1）に属するため、本追加項目は origin（`dolquis/agent-ops`）での共有コア §11 への昇格候補とし、昇格までは本 Delta を暫定の正典とする。
