# Skill instructions

- Skill の区分（shared / repo / third-party）、origin、取り込み revision は `.claude/skills/MANIFEST.md` が正典。Skill を追加・削除・rename したら同じ変更で表を更新する。
- repo 固有 Skill の作成・更新には `skill-creator` を使う（Claude Code は `.claude/settings.json` の `enabledPlugins` で有効化済み。Codex では SKILL.md の frontmatter と本文構成を同 Skill の規約に手で合わせる）。共有 Skill は `dolquis/agent-ops` を正典として product repo で直接改訂しない。
- repo 固有 Skill は `.agents/skills/` と `.claude/skills/` の本文・`references/` を同期し、ハーネス固有 frontmatter と `agents/openai.yaml` だけ差異を許す。
- 変更した各 Skill に `skill-creator` の `quick_validate.py` を実行し、`python3 scripts/check_skill_references.py` と `python3 scripts/docs-lint.py --baseline .docs-lint-baseline.json` で、参照の失効と mirror の指摘が増えていないことを確認する。
