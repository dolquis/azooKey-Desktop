# Skill instructions

- repo 固有 Skill の作成・更新には `skill-creator` を使い、共有 Skill は `dolquis/agent-ops` を正典として product repo で直接改訂しない。
- repo 固有 Skill は `.agents/skills/` と `.claude/skills/` の本文・`references/` を同期し、ハーネス固有 frontmatter だけ差異を許す。
- 変更した各 Skill に `quick_validate.py` を実行し、`python3 scripts/docs-lint.py --baseline .docs-lint-baseline.json` で mirror の指摘が増えていないことを確認する。
