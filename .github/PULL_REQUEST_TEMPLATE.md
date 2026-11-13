## 対応課題

- Linear: DEV-
- GitHub mirror: <!-- `Fixes #<番号>` または、人間ゲートでは `Refs #<番号>` -->

## 変更内容

<!-- 変更の目的と、実装上の要点を簡潔に記載してください。 -->

## 検証

<!-- 実行した build、test、lint、実機確認と、その結果を記載してください。 -->

## Skill impact

- [ ] No Skill impact

<!-- `.claude/skills/MANIFEST.md` の「担当パス」に挙げたパス（tsf-tip/src、inference-host/src、ipc、pkg、.github/workflows など）を変更し、その Skill（`.claude/skills/<名前>/` と `.agents/skills/<名前>/`）を変更しない PR では、No Skill impact にチェックを入れ、Reason: の後に Skill の routing 表・テスト表・手順が変わらない理由を必ず書いてください。空欄、このコメントだけ、「なし」「N/A」だけの記入は、検査（`python3 scripts/check_skill_impact.py`）が理由なしとして扱います。 -->

Reason: <!-- 例: 内部の整理だけで、Skill が挙げるファイル名とテスト target は変わらない -->

## Documentation impact

- [ ] Roadmap updated
- [ ] Spec docs updated
- [ ] README update not needed
- [ ] No documentation impact
- [ ] docs/README.md index updated (new/renamed docs, moved to archive)
- [ ] docs-lint run (`python3 scripts/docs-lint.py --baseline .docs-lint-baseline.json`); no growth over the baseline
- [ ] Stale spec text removed (status words, superseded "現状" notes made true by this change)

<!-- 契約ファイル（`python3 scripts/check_docs_impact.py --print-contract-paths`）を変更し、docs/・plans/・README.md を変更しない PR では、No documentation impact にチェックを入れ、Reason: の後に文書が変わらない理由を必ず書いてください。空欄、このコメントだけ、「なし」「N/A」だけの記入では CI の docs impact 検査が落ちます。 -->

Reason: <!-- 例: 内部の整理だけで wire 形式と設定の既定値は変わらない -->
