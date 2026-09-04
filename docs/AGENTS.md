# docs/ instructions

- README、AGENTS、CLAUDE、`docs/`、`plans/` を変更するときは `azookey-doc-governance` を使う。
- roadmap や spec は対象 ID・見出しを `rg` で特定し、必要な節から読む。全体監査が必要な場合を除き、一律に全文を読み込まない。
- コード変更で仕様、責務境界、fallback、ログ、設定、ユーザー可視挙動が変わる場合は対応する spec を更新する。
- 状態、進捗、行番号付きコード参照、変動するテスト件数を恒常文書へ複製しない。
- 新規または rename した文書は `docs/README.md` に索引する。恒常 runbook は `docs/handoff/`、一回限りの記録は完了基準を満たしたら `docs/archive/` に置く。
- `python3 scripts/docs-lint.py --baseline .docs-lint-baseline.json` を実行し、ベースラインからの増加がないことを確認する。日本語文書を新規作成または大きく推敲するときは `japanese-doc-workflow` を入口にする。
