# AGENTS.md

このリポジトリで作業するAIエージェントは、以下のルールを必ず守ること。

## GitHub PR作成ルール

- PRは必ずDraft PRとして作成する。
- 通常作業では、フォーク元リポジトリにPRを出さない。
- 通常作業のPR作成先は、必ず `dolquis/<repository-name>` にする。
- `gh pr create` を使う場合は、必ず `--repo dolquis/<repository-name>` を明示する。
- `main` ブランチへ直接pushしない。
- フォーク元リポジトリへPRを出す必要がある場合は、作成前に必ずユーザーへ確認する。
- base repository / base branch / head repository / compare branch を確認せずにPRを作成してはいけない。

## README 編集ルール

- `README.md` はフォーク元のオリジナル版に近い、簡潔な紹介ドキュメントに保つ。
- 実装プラン・進捗・マイルストーン履歴・機能仕様を README に書かない。
  - 実装プラン → `plans/development-plan.md`
  - マイルストーン定義・現状 → `plans/windows-port-roadmap.md`
  - 機能ごとの仕様・ロジック → `docs/*-spec.md`
- 「## 状態」「## 進捗」「## TODO」など陳腐化するセクションを README に追加しない。
- ロードマップ等へのリンクは置いてよいが、要約は 1〜2 行まで。中身のコピーはしない。
- README を肥大化させる必要があるときは、事前にユーザーへ確認する。
