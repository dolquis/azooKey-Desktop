---
paths:
  - ".agents/skills/**"
  - ".claude/skills/**"
  - "docs/**"
  - "plans/**"
---

# ドキュメント

- `azookey-doc-governance` を使い、必要なら `japanese-doc-workflow` を入口にする。`docs/` と `plans/` の個別規則は各ディレクトリの `AGENTS.md`（同じ場所の `CLAUDE.md` が import する）、Skill の規則は `.claude/skills/AGENTS.md` が持つ。
- 共有 Skill は `dolquis/agent-ops` を正典とし、repo 固有 Skill は `.agents` / `.claude` の本文と `references/` を同期する。
- docs / Skill 変更後は `python3 scripts/docs-lint.py --baseline .docs-lint-baseline.json` を実行し、ベースラインからの増加がないことを確認する。
