# Agent manifest

`.claude/agents/*.md` と `.codex/agents/*.toml` に置くカスタム agent の区分、origin、取り込み時点の記録。
この manifest は `.claude/agents/MANIFEST.md` の 1 ファイルだけを持ち、`.codex/agents/` には複製しない。`scripts/check_agent_definitions.py` は repo 所有の `scripts/check_agent_definitions.toml` に宣言した agent を検査し、本表と突き合わせる。checker とテストは `dolquis/agent-ops` から配布する。検査項目の正典は
[`docs/handoff/agent-orchestration.md`](../../docs/handoff/agent-orchestration.md)「機械検査」である。

区分の意味:

- `shared` — `dolquis/agent-ops` を origin とする共有 agent。本文はこの repo で編集しない。Codex 版の追加も origin で行い、再配布で取り込む。checker は設定に従って両ハーネスの定義と役割境界を検査する。
- `repo` — この repo が所有する azooKey 固有 agent。両ハーネスの本文を同じ PR で更新する。

「取り込み revision」は origin 側の commit を指す。origin 側の commit が記録に残っていない場合は
`未記録` とし、取り込んだこの repo 側の commit だけを添える。推測で hash を書かない。

設定に宣言した Claude agent は `tools` を明示の allowlist にし、`Agent` を含めない（subagent を spawn しない）。「権限」列は本文で指示する役割上の境界である。`read-only` はファイルを書かず、`build-write` は build directory だけに書く。
Claude 側は両者に `disallowedTools: Edit, Write, NotebookEdit` を要求し、`read-only` には
`hooks.PreToolUse`（`agent-readonly-guard.py`）も要求する。build runner に read-only guard は付けない。

権限の強制方法（Codex の sandbox 継承、Claude 側の guard の限界）は `docs/handoff/agent-orchestration.md` の不変条件 8、`[agents].enabled = false` の扱いを含む縮退は同「parent-only fallback」が持つ。

| agent | 区分 | origin | 取り込み revision | 権限 | Codex 側 |
|---|---|---|---|---|---|
| `diff-auditor` | shared | `dolquis/agent-ops` | `9851ba3` | read-only（Claude 側の `disallowedTools` と `hooks.PreToolUse`、guard も origin から取り込む） | あり |
| `spec-drift-checker` | repo | この repo | 本 repo で作成 | read-only | あり |
| `windows-build-runner` | repo | この repo | 本 repo で作成 | build-write | あり |
| `quality-gate-runner` | repo | この repo | 本 repo で作成 | read-only | あり |

## 更新の規則

- agent を追加・削除・rename したら本表を同じ変更で更新する。checker は本表に無い agent ファイルと、本表にあるが実在しない行を検出する。
- Claude / Codex の本文は frontmatter / TOML wrapper を除いて byte 一致にする。同期の注記は本文ではなく frontmatter の YAML コメントと TOML コメントに置く。
- `shared` を再配布したら「取り込み revision」に origin の commit を書く。
- 分担の規約（分割レベル、route、snapshot 契約、返却形式）は `docs/handoff/agent-orchestration.md` が持つ。本表は所有と権限の記録に限る。
- agent を追加したら、`docs/handoff/agent-orchestration.md` の route 表に起動条件を足す。route 表に載っていて本表に無い agent の扱いは同じ「parent-only fallback」に従う。
