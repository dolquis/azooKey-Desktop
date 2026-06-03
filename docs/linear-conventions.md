<!--
  SHARED CORE — Agent / Linear 運用規約（管制塔モデル）
  この「共有コア」は全リポジトリで同一内容をミラーする。
  個別 repo で直接編集しない。編集は origin（後述）で行い、各 repo へ伝播する。
  version: 0.3-draft   updated: 2026-06-03
  status: 規約確定・origin = dolquis/agent-ops に確定。repo 新設/配置・ラベル移行(Phase 4)は未実施。
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

Issue には「次の AI 役割」を示す `agent:*` を 1 つ付ける。ただし `gate:human-required` の人間専任タスクは `agent:*` を省略してよい。

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

全プレフィックスをコロン式に統一する（2026-06-03 決定）。

| プレフィックス | 用途 | 値 |
| -- | -- | -- |
| `repo:` | 対象 GitHub リポジトリ（実 repo 名をそのまま反映） | 各 1 つ必須 |
| `area:` | 技術領域 | 1 つ以上 |
| `agent:` | 次の AI 役割（§2） | 原則 1 つ |
| `type:` | Issue の役割 | `tracking` / `implementation` / `review` |
| `gate:` | 人間ゲート（横断フラグ） | `human-required` |
| `kind:` | 変更カテゴリ | `feature` / `improvement` / `bug` / `docs` |
| `Migrated` | 由来フラグ（他サービス起票） | 任意 |

ルール:
- `area:` の語区切りはハイフン（例 `area:converter-core`）。`repo:` は実 GitHub repo 名をそのまま使い区切りを正規化しない。
- **人間ゲートは `type:` ではなく `gate:human-required`（横断フラグ）で表す。** 例: 人間確認が必要なレビュー Issue は `type:review` + `gate:human-required`。旧 `type:human-gate` は本規約で廃止。移行期は旧ラベルが残る Issue があるため Phase 4 で `gate:human-required` へ付け替える（読むときは両対応）。
- **変更カテゴリは `kind:*` を正典とする**（`feature` / `improvement` / `bug` / `docs`）。旧 `Feature` / `Improvement` / `Bug` / `enhancement` / `documentation` は Phase 4 まで移行互換として扱い、Phase 4 で物理退役（ラベル削除は設定画面で手動）する。`Migrated` は由来フラグとして存続。
- **Phase 4 完了までの移行互換**: 旧 `repo_*` / `area_*` / 旧カテゴリラベルのみが付いた Issue も、Ready / Missing Metadata / 週次監査では欠落扱いしない。新規作成・更新時は repo の Delta または既存 `AGENTS.md` の現行ラベルを優先し、Phase 4 後に `repo:` / `area:` / `kind:*` へ収束する。

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
```

Issue タイプ（`type:`）:
- `tracking` — 進捗管理・子 Issue 集約・順序整理。直接実装しない。
- `implementation` — 1 PR で完了可能な実装・修正・テスト追加。
- `review` — 設計レビュー・PR レビュー・整合性確認。

Tracking Issue または Project description 上部には、見出しを **Next AI Tasks:** として最大 3 件を書く。

---

## 6. Agent Run Report Format（作業後コメント）

作業後に Linear Issue へ残すログ形式（仕様ではなく作業ログ）:

- **Agent**: Claude Code / Codex / ChatGPT / other
- **Read**: 確認した正典（AGENTS / roadmap / README / 連携 GitHub Issue / 関連 docs）
- **Changed**: 変更ファイル、または Linear のみの変更
- **Validation**: test / lint / build / 手動確認、スキップ時は理由
- **Findings**: リスク・ブロッカー・follow-up
- **Next**: 次アクションと次オーナー

短く保ち、仕様はコピーせず GitHub へリンクする。

---

## 7. Definition of Ready / Done

### Ready
- Project が正しく設定されている。
- `repo:*` ラベルがある（Phase 4 完了までは repo 固有の旧 repo ラベルも有効）。
- 関連する `area:*` が 1 つ以上ある（Phase 4 完了までは repo 固有の旧 area ラベルも有効）。
- 次の AI 役割を示す `agent:*` がある。ただし `gate:human-required` の人間専任タスクは `agent:*` を省略してよい。
- 可能なら GitHub Issue / PR リンクが添付されている。
- Goal と done criteria が明確。
- 実行順序が重要な場合、ブロッカーが relation で表現されている。

### Done
- 作業結果が記録されている。
- 検証結果が記録されている（スキップ時は理由）。
- GitHub 正典に対するドキュメント影響を確認済み。
- 可能なら関連 PR / GitHub Issue がリンクされている。
- 必要な follow-up Issue が作成/リンクされている。
- `gate:human-required` の Issue は人間確認が取れている。
- repo 固有 Delta / `AGENTS.md` / `WORKFLOW.md` が追加ゲート（PR マージ、検証メモなど）を要求する場合、それを満たしている。

Done は「Linear 上で運用的に完了」を意味し、GitHub docs のリリース基準を置き換えない。

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

---

## 10. Control Tower Views（推奨ビュー）

各プロジェクトで以下のフィルタビューを用意する（Project でスコープ）:

- **Ready for Claude Design**: `agent:claude-design` + Backlog/Todo
- **Ready for Codex Implementation**: `agent:codex-impl` + Backlog/Todo + 非ブロック
- **Ready for Review**: (`agent:claude-review` か `agent:codex-pr-review`) + In Review
- **Needs Human Verification**: `gate:human-required`
- **Missing Metadata**: repo / area / agent ラベルまたは GitHub リンク欠落（移行期の旧 repo/area ラベルと `gate:human-required` 人間専任タスクの `agent:*` 免除を考慮）

ビューはレーダー画面であって仕様ではない。曖昧なら GitHub docs と連携 GitHub Issue を見てから動く。

---

## 11. Recurring Control Tower Audit（週次・統一チェックリスト）

各プロジェクトに `[Recurring] Linear control tower audit — <PROJECT>` を 1 件持つ
（`type:review` + `agent:claude-review` + `repo:*`）。チェック項目:

- [ ] Project 未設定の Issue
- [ ] `repo:` ラベル欠落（Phase 4 完了までは repo 固有の旧 repo ラベルも有効）
- [ ] `area:` ラベル欠落（Phase 4 完了までは repo 固有の旧 area ラベルも有効）
- [ ] `agent:` ラベル欠落（`gate:human-required` の人間専任タスクを除く）
- [ ] Migrated なのに GitHub リンク欠落
- [ ] 人間確認が要るのに `gate:human-required` 欠落
- [ ] Tracking Issue で子が未リンク
- [ ] 実行順序を表さなくなったブロッカー
- [ ] Done なのに検証ノート欠落

Rule: Linear のルーティングのみを点検する。GitHub docs が正典。

---

## 12. Project description テンプレート

各プロジェクト description 冒頭は次の固定形にする:

```md
## Current control policy

Lead: <name>
Current focus: <一文>
Next checkpoint: <YYYY-MM-DD>。<その日に判定する内容>

Next AI Tasks:
1. <DEV-xx> <内容>
2. <DEV-xx> <内容>
3. <DEV-xx> <内容>   （最大3件）

Human Gate:
* <DEV-xx> <内容> は人間確認必須。

---
正典: <REPO>/docs/linear-conventions.md, <REPO>/docs/<WORKFLOW or ROADMAP>.md
Stage map: <ステージ定義>
```

見出し名は上記に統一する（`Next AI Task` 等の表記揺れを使わない）。

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
- AREA_LABELS: area:tsf-tip, area:inference-host, area:ipc, area:learning, area:converter-core
- LINEAR_STATUS_MAP: Backlog/Todo → In Progress → In Review（Draft PR 提出済み）→ Done（レビュー合格 + マージ + 検証メモ記載後）
- STAGE_MAP:
  - MVP-0 ビルド・テスト基盤安定化
  - MVP-1 TSF TIP 基本入力
  - MVP-2 Inference Host / IPC 安定化
  - 以降: 残ハードニング + 実機 Win11 ゲート
- DELTA_DOCS（この repo 固有で別途維持）: GitHub↔Linear Mapping, Decision Log, Agent Prompt Cards
