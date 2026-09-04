---
paths:
  - "README.md"
  - "AGENTS.md"
  - "CLAUDE.md"
  - "docs/**"
  - "plans/**"
---

# ドキュメント

- `azookey-doc-governance` を使い、必要なら `japanese-doc-workflow` を入口にする。
- roadmap や spec は対象 ID・見出しを `rg` で特定し、必要な節から読む。全体監査が必要な場合を除き、一律に全文を読み込まない。
- 状態、進捗、行番号付きコード参照、変動するテスト件数を恒常文書へ複製しない。
- 新規または rename した文書は `docs/README.md` に索引し、docs / Skill 変更後は `python3 scripts/docs-lint.py` を実行する。
