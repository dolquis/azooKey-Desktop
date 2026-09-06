# plans/ instructions

- roadmap を変更するときは `azookey-doc-governance` を使い、対象 ID・見出しを `rg` で特定して必要な節から読む。
- roadmap はマイルストーン定義、依存関係、受け入れ条件、リスクだけを持ち、状態、進捗、優先度、担当は Linear を正典とする。
- マイルストーン定義やリスクが変わる場合だけ roadmap を更新する。`plans/` は roadmap 1 本を置く場所であり、調査スナップショットや参考資料は `docs/` に置く。
- `python3 scripts/docs-lint.py --baseline .docs-lint-baseline.json` を実行し、ベースラインからの増加がないことを確認する。大きく推敲するときは `japanese-doc-workflow` を入口にする。
