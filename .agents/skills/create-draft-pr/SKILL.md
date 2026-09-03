---
name: create-draft-pr
description: GitHubで新規Pull Requestを作成するときに使用する。通常はdolquis配下の対象repositoryのmainをbase、現在の作業branchをheadとしてDraft PRを作成し、upstreamへの誤作成、重複PR、誤ったcompare範囲を防ぐ。PRの閲覧やレビューだけには使用しない。
---

# Create Draft PR

## 目的

新規PRを、正しいrepository、base branch、head branch、compare範囲へDraftとして安全に作成する。fork元やupstreamへの誤送信と、同じbranchからの重複PRを防止する。

## 前提

- GitHub操作は、各ハーネスで利用可能なGitHub連携（connector、plugin、MCP）を優先し、必要に応じて `gh` CLIを使用する。
- 新規PR作成前に `pre-pr-self-review` を完了する。
- `main` branchへ直接pushしない。
- 通常のPR作成先は `dolquis/<repository-name>`、base branchは `main` とする。
- fork元やupstreamへPRを作成する必要がある場合は、作成前に必ずユーザーへ確認する。
- 既存PRがReady状態の場合は、無理にDraftへ戻さない。

## 手順1: Repositoryとbranchの特定

1. 次の読み取りコマンドなどで、現在の状態を確認する。

```bash
git status -sb
git branch --show-current
git remote -v
git remote get-url --push <dolquis-remote>
```

2. 現在のbranch名を確認し、detached HEADではなく、`main` でもないことを確認する。
3. remote一覧とfetch/push URLを確認し、repository名、`dolquis/<repository-name>` を指すremote、upstream相当のremoteを識別する。
4. 通常のbase repositoryを `dolquis/<repository-name>` として確定する。
5. base branchを `main`、head repositoryを `dolquis/<repository-name>`、head branchを現在の作業branchとして確定する。
6. base repository、base branch、head repository、head branchのいずれかが確定できない場合は、推測でPRを作成しない。

確認項目:

- base repository: `dolquis/<repository-name>`
- base branch: `main`
- head repository: 作業branchをpushする `dolquis/<repository-name>`
- head branch: 現在の作業branch
- push先: `dolquis/<repository-name>` を指すremote
- fork元やupstreamがbase repositoryまたはpush先になっていないこと

## 手順2: 既存PRの確認

利用可能なGitHub連携または `gh` を使い、同じhead branchから対象repositoryへ作成済みのPRがないか、Open、Draft、Closed、Mergedの全状態を確認する。

- 既存PRがある場合は、新しいPRを重複作成しない。
- 既存PRがDraftなら、そのPRを更新対象として扱う。
- 既存PRがReadyなら、Ready状態を維持する。自動的にDraftへ戻さない。
- 同じhead branchのPRがClosedなら、自動的に新しいPRを作成しない。reopenまたは別branchでの新規PRが必要か確認する。
- 同じhead branchのPRがMergedなら、そのbranchを新規PRに再利用しない。更新済みのbase branchから新しい作業branchを用意する。

`gh` CLIを使用する場合の確認例:

```bash
gh pr list --repo dolquis/<repository-name> --head <branch-name> --state all --json number,state,isDraft,baseRefName,headRefName,url
```

## 手順3: セルフレビューと変更範囲の確定

1. `pre-pr-self-review` を実行し、必要なレビューと検証を完了する。
2. `dolquis/<repository-name>` の `main` に対応するremote-tracking refを `<base-ref>` として確定し、必要に応じてそのremoteをfetchする。
3. `git merge-base HEAD <base-ref>` でmerge-baseを求める。
4. merge-baseからHEADまでのcommit済み差分と、最新base tipからHEADまでの最終tree差分を確認する。

```bash
git diff --stat <merge-base> HEAD
git diff --name-status <merge-base> HEAD
git diff --check <merge-base> HEAD
git diff <merge-base> HEAD
git diff --stat <base-ref> HEAD
git diff --name-status <base-ref> HEAD
git diff --check <base-ref> HEAD
git diff <base-ref> HEAD
git status -sb
```

5. PR対象の変更がcommitされていることを確認する。未commit変更が残る場合は、それがPRに含まれないことを認識し、意図的な残存か確認する。
6. compare範囲に、要求外のcommit、別タスクの変更、生成物、secretが含まれていないことを確認する。
7. squashまたはrebaseで既存PRが取り込まれた後は、merge-baseからの差分に取り込み済み変更が再表示される場合がある。`<base-ref>` からHEADまでの差分に、base側で追加されたファイルの意図しない削除や取り込み済み変更の再提案が見える場合は、pushやPR作成を停止し、更新済みbaseから新しいbranchを用意する。

## 手順4: head branchの公開

head branchが対象の `dolquis/<repository-name>` に存在しない、または最新commitがpushされていない場合は、対象remoteを再確認してから現在の作業branchをpushする。

```bash
git push -u <dolquis-remote> <branch-name>
```

- `<dolquis-remote>` のfetch URLとpush URLが `dolquis/<repository-name>` を指すことを確認する。
- push後、対象remote上のhead branchが存在し、そのcommitがローカルの `HEAD` と一致することを確認する。
- `main` はpushしない。
- `--force` または `--force-with-lease` は、ユーザーの明示依頼や既存方針がない限り使用しない。
- fork元やupstreamへpushしない。

## 手順5: Draft PRの作成

利用可能なGitHub連携を使用する場合も、次の値を明示する。

- repository: `dolquis/<repository-name>`
- base: `main`
- head repository / `head_repo`: `dolquis/<repository-name>`
- head: 現在の作業branch
- draft: `true`

`gh` CLIを使用する場合は、次の形式で作成する。

```bash
gh pr create --repo dolquis/<repository-name> --base main --head <branch-name> --draft
```

`gh pr create --dry-run` はGit変更をpushする場合があるため、読み取り専用の検証としては実行しない。作成前の検証には、`gh pr create --help`、既存PRの読み取り、remoteとcompare範囲の確認、コマンドの組み立てを使用する。

タイトルと本文は、実際の差分と検証結果に基づいて作成する。PR本文には少なくとも次を含める。

- 変更の目的と概要
- 主な実装内容
- 実行したtest、lint、buildなどの検証結果
- 実行できなかった検証、失敗した検証、既存失敗
- 必要に応じて残存リスクやレビュー観点

## 手順6: 作成後の検証

PR作成後、GitHub上のPR情報を取得し、次を再確認する。

- Draft PRである
- base repositoryが `dolquis/<repository-name>` である
- base branchが `main` である
- head repositoryが `dolquis/<repository-name>` である
- head branchが意図した作業branchである
- compare差分が意図した変更を含む
- fork元やupstreamへ作成されていない

不一致がある場合は、作成成功として報告せず、可能な範囲で安全に修正する。誤ったrepositoryへ作成した場合は、勝手に公開状態を放置せず、状況と実施可能な是正策をユーザーへ明確に報告する。

## 完了報告

次を簡潔に報告する。

- PRのタイトルとURL
- Draft / Ready状態
- base repositoryとbase branch
- head repositoryとhead branch
- セルフレビューと検証の結果
- 未実行または失敗した検証
