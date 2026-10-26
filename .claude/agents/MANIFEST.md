# Agent manifest

`.claude/agents/*.md` と `.codex/agents/*.toml` に置くカスタム agent の区分、origin、取り込み時点の記録。
この manifest は `.claude/agents/MANIFEST.md` の 1 ファイルだけを持ち、`.codex/agents/` には複製しない。`scripts/check_agent_definitions.py` は本表を読み、
区分 `repo` の agent について Claude / Codex ペアの存在、`name` の一致、本文の byte 一致、
Claude 側の guard 宣言、Codex 設定の限定的な型・無効な上書き指定、参照する repo path と Skill の実在を検査する。

区分の意味:

- `shared` — `dolquis/agent-ops` を origin とする共有 agent。本文はこの repo で編集しない。Codex 版の追加も origin で行い、再配布で取り込む。checker は Claude 側ファイルの存在だけを見る。
- `repo` — この repo が所有する azooKey 固有 agent。両ハーネスの本文を同じ PR で更新する。

「取り込み revision」は origin 側の commit を指す。origin 側の commit が記録に残っていない場合は
`未記録` とし、取り込んだこの repo 側の commit だけを添える。推測で hash を書かない。

区分 `repo` の Claude agent は `tools` を明示の allowlist にし、`Agent` を含めない（subagent を spawn しない）。「権限」列は本文で指示する役割上の境界である。`read-only` はファイルを書かず、`build-write` は build directory だけに書く。
Claude 側は両者に `disallowedTools: Edit, Write, NotebookEdit` を要求し、`read-only` には
`hooks.PreToolUse`（`agent-readonly-guard.py`）も要求する。build runner に read-only guard は付けない。

Codex は親ターンの sandbox / approval を継承する。custom agent の `sandbox_mode` や `approval_policy` では上書きできず、本文の制約は独立した OS sandbox を作らない。
checker はこの無効な指定を検出するが、実効権限や実行時の遵守を保証しない。親が実行前に権限と書込み範囲を確認し、必要な境界を確保できない場合は親へ戻す。
`[agents].enabled = false` の扱いを含む parent-only fallback は `docs/handoff/agent-orchestration.md`「parent-only fallback」に従う。

| agent | 区分 | origin | 取り込み revision | 権限 | Codex 側 |
|---|---|---|---|---|---|
| `diff-auditor` | shared | `dolquis/agent-ops` | 未記録（repo 側 `b2d2cce`、`39af1e9` で更新） | read-only（origin の定義に従う。Claude 側の hook も origin から取り込む） | なし（origin が配布したときに取り込み、本表の revision を更新する） |
| `spec-drift-checker` | repo | この repo | 本 repo で作成 | read-only | あり |
| `windows-build-runner` | repo | この repo | 本 repo で作成 | build-write | あり |

## 更新の規則

- agent を追加・削除・rename したら本表を同じ変更で更新する。checker は本表に無い agent ファイルと、本表にあるが実在しない行を検出する。
- `repo` の本文は frontmatter / TOML wrapper を除いて byte 一致にする。同期の注記は本文ではなく frontmatter の YAML コメントと TOML コメントに置く。
- `shared` を再配布したら「取り込み revision」に origin の commit を書く。
- 分担の規約（分割レベル、route、snapshot 契約、返却形式）は `docs/handoff/agent-orchestration.md` が持つ。本表は所有と権限の記録に限る。
- agent を追加したら、`docs/handoff/agent-orchestration.md` の route 表に起動条件を足す。route 表に載っていて本表に無い agent の扱いは同じ「parent-only fallback」に従う。
