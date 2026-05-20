# AIエージェント向け GitHub作業・PR作成ルール

このリポジトリで作業するAIエージェントは、以下のルールを必ず守ること。

## 最重要方針

- PRは必ずDraft PRとして作成する。
- フォーク元リポジトリにPRを出さない。
- 通常作業のPR作成先は、必ず `dolquis/<repository-name>` にする。
- `gh pr create` を使う場合は、必ず `--repo dolquis/<repository-name>` を明示する。
- `main` ブランチへ直接pushしない。
- フォーク元リポジトリへPRを出す必要がある場合は、作成前に必ずユーザーへ確認する。
- base repository / base branch / head repository / compare branch を確認せずにPRを作成してはいけない。

## 通常のPR作成コマンド

通常作業では、必ず以下の形式でDraft PRを作成する。

```bash
gh pr create \
  --repo dolquis/<repository-name> \
  --base main \
  --head <branch-name> \
  --draft
```

## セルフレビュー方針

変更をpushしてPRを作成する前に、必ずセルフレビューを実施すること。

- `git diff` で全変更内容を最初から最後まで確認する。
- 変更が依頼内容を満たしているか、意図しない変更や無関係なファイルが
  含まれていないかを点検する。
- デバッグ用コード・コメントアウト・一時ファイルなど不要な残骸を削除する。
- 機密情報（鍵・トークン・認証情報など）が含まれていないか確認する。
- 可能な場合はビルド・テスト・Lintを実行し、結果が通ることを確認する。
- セルフレビューで問題が見つかった場合は、push前に修正する。

## README 編集ルール

`README.md` はフォーク元 (ensan-hcl/azooKey) のオリジナル版に近い、
簡潔な紹介ドキュメントとして保つ。以下を厳守すること：

- 詳細な実装プラン・進捗状況・マイルストーン履歴を README に書かない。
  - 実装プラン → `plans/development-plan.md`
  - マイルストーン定義・現状 → `plans/windows-port-roadmap.md`
  - 機能ごとの仕様・ロジック → `docs/*-spec.md`
- README に新セクションを足す前に、その内容が `plans/` か `docs/` に
  収まらないかを必ず先に検討する。
- 「## 状態」「## 進捗」「## TODO」のような時系列で陳腐化するセクションを
  README に追加しない。該当情報は `plans/windows-port-roadmap.md` の
  各マイルストーン「現状」欄に書く。
- ロードマップへのリンクは README に置いてよいが、ロードマップの中身を
  README にコピーしない（リンクのみ、要約は 1〜2 行まで）。
- 例外的に README を肥大化させる必要があるときは、ユーザーに事前確認する。
