# ローカル agent の分担規約（orchestration）

本書は、Claude Code と Codex で親 agent が 1 Issue 内の調査・実装・レビュー・build を分担させるときの恒常 runbook である。
分担の共通規則（何を渡すか、親が統合すること、共有ファイルと build directory の競合回避、Codex Cloud 起動との区別）は `docs/linear-conventions.md` §2.1 が正典で、本書はそれを azooKey-Desktop の agent 定義に結び付ける。
Claude Code 固有の agent 一覧は `CLAUDE.md`「サブエージェントの使い分け」、agent の所有と権限は `.claude/agents/MANIFEST.md`、ホスト側ツールの接続は `docs/handoff/agent-tooling-setup.md` が持つ。
本書には進捗、現在の Issue、pilot の結果を書かない。それらは Linear の tracking Issue のコメントに置く。

## 責務の分離

| 層 | 正典 | 持つもの |
|---|---|---|
| Control | Linear | Issue、依存、状態、Priority、次の AI 役割、Human Gate |
| Policy | `AGENTS.md`、`CLAUDE.md`、Skill、本書 | 何を誰へ委任できるか、安全境界、完了条件 |
| Execution | 親 agent とカスタム agent | 調査、単一 writer による編集、read-only レビュー、build / test |
| Evidence | git diff、テストログ、CI、Linear コメント、PR 本文 | 実行した事実、実行しなかった検証とその理由、残リスク |

## 不変条件

1. 1 Linear Issue = 1 branch = 1 Draft PR。commit、push、PR の作成・更新、Linear の状態変更は親 agent が直列に行う。
2. subagent は別 Issue、別 branch、別 PR を作らない。
3. 同じファイルを複数 writer に割り当てない。CMake build directory、生成物、Serena の対象切替を複数 agent で共有しない。
4. Human Gate（実機確認、管理者権限、TIP 登録、署名）を CI、agent レビュー、シミュレーションで代替しない。
5. Codex Cloud への assign / delegate / mention は `docs/linear-conventions.md` §2.1 の人間承認規則に従う。
6. agent の報告は完了判定ではなく入力である。親が実ファイルと最新 diff で検証する。
7. reviewer と build runner は subagent を spawn しない。Claude Code の subagent は既定で再帰 spawn できるため、agent 本文で禁じ、`.claude/settings.json` の env で深さも制限する。
8. read-only は文章ではなく機構で担保する。Claude 側は `disallowedTools` と frontmatter `hooks.PreToolUse`（`.claude/hooks/agent-readonly-guard.py`）、Codex 側は `sandbox_mode = "read-only"`。`windows-build-runner` だけは build directory へ書くため、この規則の意図的な例外として `MANIFEST.md` に記録する。
9. 親が編集中の working tree を reviewer に読ませない。レビューは編集を止めた checkpoint に対して行う。
10. typo、1 行の可逆な修正、直列依存だけの仕事、1 ファイルの明白な変更は分割しない。

## 分割レベル

| レベル | 条件 | 実行形 |
|---|---|---|
| L0 直接実行 | typo、1 行修正、調査 1 件、直列依存のみ | 親だけ |
| L1 単一専門確認 | 1 つの明確な境界に触れる | 親 + specialist 1 体 |
| L2 並列 read-only レビュー | 2 つ以上の独立した境界に触れ、編集が停止済み | 親 + specialist 2〜3 体 |
| L3 1 Issue 内の並列実装 | ファイル所有権を分割でき、契約（IPC payload、schema、関数シグネチャ）が先に固定済み | Agent Teams または独立 worker。writer ごとに非重複領域 |
| L4 Issue 間並列 | Linear 上で独立 Issue と依存が確定済み | Issue ごとに別 worktree / branch / Draft PR |

L2 を標準とする。L3 は例外運用で、TSF、IPC、settings、build 定義が交差しやすい azooKey では同一 Issue 内の複数 writer は read-only 分担より衝突コストが高い。
L4 で Claude Code の `isolation: worktree` を使うと build directory は共有されないが、worktree ごとに configure からやり直すため full rebuild になる。使うときは親が spawn 時に明示し、agent 定義には固定しない。

specialist の同時起動は通常 2 体、横断変更でも 3 体までとし、4 枠目は Explore か build runner に残す。Codex の `.codex/config.toml` は `max_concurrent_threads_per_session = 4`（親を除く spawn 済み thread の上限）で、この配分に合わせてある。

## touched path からの route

| 変更範囲 | 必須 | 条件付き |
|---|---|---|
| `tsf-tip/**` | `boundary-reviewer`（tsf） | IPC も変わるなら `boundary-reviewer`（ipc）を別 spawn |
| `ipc/**`、wire schema | `boundary-reviewer`（ipc） | TIP consumer 変更なら tsf |
| `core/**`、候補生成 | `boundary-reviewer`（conversion） | spec 変更なら `spec-drift-checker` |
| `learning/**`、privacy 設定 | `boundary-reviewer`（learning-privacy） | IPC 経由なら ipc |
| `docs/*-spec.md`、schema、`docs/test-inventory.md` | `spec-drift-checker` | 対応する境界 |
| C++ を含む重要差分 | `diff-auditor` | `pr-review-toolkit`、対応する境界 |
| Windows configure / build / test | `windows-build-runner` | build 完了後に read-only review |

`boundary-reviewer` は境界名を受け取る汎用 read-only agent で、境界ごとに別 spawn する（1 体に複数境界を渡さない）。定義が無いハーネスや未導入の期間は、同じ境界の確認を親が対応 Skill（`tsf-tip-development`、`tsf-ipc-protocol`、`azookey-core-conversion`、`azookey-learning-data-safety`）を読んで担当する（parent-only fallback）。
`diff-auditor` は差分と契約の整合、`spec-drift-checker` は spec 側の更新漏れ、`pr-review-toolkit` はコードの質、`windows-build-runner` は実行と抽出だけを担う。これらは代替関係ではなく、C++ の変更を含む PR では `diff-auditor` と `pr-review-toolkit` の両方を掛ける。

## background と並列実行

background へ回してよいもの:

- 変更停止後の read-only 差分レビュー。
- 互いに独立した spec drift 確認と境界の不変条件確認。
- 親が生成したログ・評価出力の読み解き。
- 専用 build directory を確保し、他の writer が停止している状態での長時間 build / test。

background へ回さないもの:

- commit、push、PR、Linear 操作。
- 同じ working tree への複数 writer。
- 親が編集中のファイルを前提にするレビュー。
- 管理者権限、TIP 登録、署名、実機アプリ互換性確認。
- 出力先を分離できない生成処理。

agent 定義には `background: true` と `isolation: worktree` を固定しない。同じ定義を Agent Teams の teammate と通常 subagent の双方に使うため、実行形は親が spawn 時に選ぶ。

## snapshot 契約

Claude Code の subagent も Codex の subagent も親の会話履歴を見ない。契約は spawn の prompt に全て書き、全履歴を複製しない。

```text
Issue: DEV-xxxx
Base: <merge-base SHA>
Review head: <commit SHA、または HEAD + 作業ツリー（親は稼働中に対象 path を編集しない）>
Scope: <paths / symbols>
Out of scope: <paths / decisions>
Boundary: tsf | ipc | conversion | learning-privacy | (none)
Canonical skill / references to read: <skill name、references/*.md>
Write boundary: read-only | <exact paths>
Spawn: forbidden
Expected return: 本書「統一返却形式」
Validation owned by parent: <commands>
```

親が checkpoint 後に対象差分を変更した場合、その報告は stale として扱い、最終判断に流用しない。build / test runner は実行対象の commit SHA と使用した preset / build directory を返す。

## 統一返却形式

各 agent は次の順序で返す。

1. 対象 snapshot（Base、Review head、Boundary）と確認範囲。
2. Findings。1 件ごとに深刻度（P1 / P2 / P3）、file / symbol、根拠、影響、推奨対応。確認できたことと推測を分ける。
3. 実行した read-only command、または受け取ったログの位置。
4. 確認できなかった範囲と理由。
5. 親が次に実行すべき検証。
6. 該当が無い場合も「該当なし」と書き、確認した範囲と確認できなかった範囲を明記する。

通常の返却量は 4〜8 KiB を目安にし、追加の証拠は親の求めに応じて段階的に返す（`docs/handoff/agent-tooling-setup.md`「長い出力の扱い」）。

## ロールバック

問題が出た場合は製品コードを戻さず Execution 層だけを縮退する。

1. Codex は `.codex/config.toml` の `[agents].enabled` を `false` にするか、該当 agent 定義を revert する。`scripts/check_agent_definitions.py` は `enabled = false` を検出するので、縮退中は CI の docs-lint ジョブがその旨を報告する。
2. Claude Code は `.claude/settings.json` の `CLAUDE_CODE_EXPERIMENTAL_AGENT_TEAMS` を外し、通常の単一 subagent または親のみへ戻す。
3. agent が使えない環境では、上記 parent-only fallback のとおり同じ検証範囲を親が担当する。
4. pilot Issue は Canceled または Backlog へ戻し、理由をコメントに残す。アーカイブしない。

## 機械検査

`scripts/check_agent_definitions.py`（`.github/workflows/docs.yml` の `docs-lint` ジョブ）が、`.claude/agents/MANIFEST.md` の区分 `repo` の agent について Claude / Codex ペアの存在、`name` と `description` の一致、本文の byte 一致、権限列（read-only / build-write）と両側の設定の対応、本文が参照する repo path と Skill の実在、`.codex/config.toml` と `.codex/agents/*.toml` の TOML parse を検査する。
`scripts/tests/test_agent_readonly_guard.py` は read-only guard hook の allow / deny 表を固定する。
