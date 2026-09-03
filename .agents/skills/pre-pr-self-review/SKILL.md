---
name: pre-pr-self-review
description: リポジトリ内ファイルを変更した後のセルフレビューに使用する。最終報告、stage、commit、push、PR作成・更新の前に、baseとの差分、未追跡ファイル、検証、生成物、secret混入を確認する。読み取り専用の調査や説明だけには使用しない。
---

# Pre-PR Self Review

## 目的

今回の変更だけを対象に、実装上の問題、意図しない差分、検証漏れを人間のレビュー前に見つけて修正する。確認した事実と未確認事項を、PR本文または最終報告へ正確に引き継ぐ。

## 適用タイミング

コード、設定、ドキュメントなどのリポジトリ内ファイルを変更した作業で、次のタイミングに使用する。

1. 最終報告の前
2. 変更をstageする前
3. commitまたはpushの前
4. PRを作成または更新する前

同じ作業中に複数のタイミングが該当する場合は、以下の該当フェーズを順番に実行する。最後のレビュー後にファイルを変更した場合は、影響するフェーズを再実行する。

## 原則

- ユーザーの既存変更を自分の変更と混同しない。無関係な変更を戻す、整形する、stageする、commitする行為は禁止する。
- セルフレビュー中に発見した、今回の変更に起因する問題または変更範囲内で修正すべき問題は、可能な限り修正してから先へ進む。
- 今回の変更と無関係な既存問題は、タスク達成に必要でない限り勝手に修正せず、報告に留める。
- 要約ツールの結果だけで完了を判断しない。重要な判断は、現在のdiff、実ファイル、関連テスト、最新のPR情報で確認する。
- 検証結果を推測しない。実行していないチェックを成功扱いにしない。

## フェーズ1: レビュー対象の確定

1. `git status -sb` と `git status --short` を確認し、現在のbranch、変更、削除、未追跡ファイルを把握する。
2. PRのbase branchを次の優先順位で確定する。
   1. ユーザーの明示指定
   2. 既存PRのbase branch
   3. リポジトリ固有の指示
   4. `origin/main`
3. 既存PRの情報は、各ハーネスで利用可能なGitHub連携（connector、plugin、MCP）、または `gh pr list` / `gh pr view` などの読み取り操作で確認する。既存PRがない場合だけ、次の優先順位へ進む。
4. 選択したbase repositoryとbase branchに対応するremote-tracking refを `<base-ref>` として確定し、必要に応じてそのremoteをfetchする。fetchできなかった場合は、使用した参照が古い可能性を報告する。
5. `git merge-base HEAD <base-ref>` でmerge-baseを求める。以降、このcommitを `<merge-base>` と表記する。
6. base repository、base branch、`<base-ref>`、merge-baseのいずれかを確定できない場合は、誤った範囲をレビューしたふりをせず、理由を明記する。

## フェーズ2: stage前の差分レビュー

少なくとも次を確認する。コマンドは実行環境に合わせて同等の方法へ置き換えてよい。

```bash
git status --short
git diff --stat <merge-base>
git diff --name-status <merge-base>
git diff --check <merge-base>
git diff <merge-base>
git ls-files --others --exclude-standard
```

`git diff <merge-base>` は、merge-baseから現在の作業ツリーまでのcommit済み、stage済み、未stageの差分をまとめて表示する。未追跡ファイルの内容は表示しないため、`git status --short` と `git ls-files --others --exclude-standard` で列挙し、今回の変更に含める候補は個別に内容を確認する。

以下をレビューする。

- タスクの要求と変更内容が一致しているか
- 要求外の変更、無関係な整形、命名変更、refactorが混入していないか
- 実装の正しさ、回帰、境界条件、エラー処理、並行処理、リソース解放に問題がないか
- 公開API、schema、永続化、認証、権限、課金、通知、外部連携、安全設計への影響が見落とされていないか
- 関連テストが追加または更新され、変更した振る舞いを適切に検証しているか
- README、API仕様、設定例、コメントなどの関連ドキュメントと実装が整合しているか
- debug用コード、ログ、暫定フラグ、TODO、コメントアウトが残っていないか
- 生成物、cache、ローカル設定、大容量ファイル、不要なbinaryが混入していないか
- secret、credential、token、秘密鍵、`.env`、個人情報が混入していないか
- 削除、移動、renameが意図したものか

差分が長い場合は利用可能な要約・検索手段で整理してよいが、重要箇所は元のdiffと実ファイルでも確認する。

## フェーズ3: 検証

1. リポジトリの `AGENTS.md`、`CLAUDE.md`、README、CONTRIBUTING、CI設定、package scripts、Makefileなどから、変更範囲に必要な検証コマンドを特定する。
2. 変更に直接対応するテストを優先して実行し、その後、リポジトリで必須とされる範囲のtest、lint、format check、typecheck、buildを実行する。
3. 検証は、原則として最終的な変更後に実行する。
4. 自動formatを実行して差分が変わった場合は、フェーズ2の差分レビューを再実行する。
5. 問題を修正した場合は、修正の影響を受けるテストとチェックを再実行する。
6. ログが大量になる場合は利用可能な要約手段で整理してよい。ただし、終了コード、失敗箇所、重要なwarningは実ログで確認する。
7. 実行できなかった検証、失敗した検証、今回の変更と無関係な既存失敗を区別して記録する。

## フェーズ4: stage後の確認

変更をstageした場合は、commit前に少なくとも次を確認する。

```bash
git status -sb
git diff --cached --stat
git diff --cached --check
git diff --cached
```

- `git diff --cached` は次のcommitに入るstage済み差分の確認に使う。PR全体の差分確認の代用にはしない。
- stage済み差分が今回のcommitに必要な内容だけであることを確認する。
- ユーザー由来の無関係な変更、未完成の変更、生成物、secretがstageされている場合は、対象を壊さない方法でstageから外す。
- stage内容を修正した場合は、このフェーズを再実行する。

## フェーズ5: commit、push、PR前の最終確認

commit済みのPR差分は、少なくとも次の範囲で確認する。

```bash
git diff --stat <merge-base> HEAD
git diff --name-status <merge-base> HEAD
git diff --check <merge-base> HEAD
git diff <merge-base> HEAD
git status -sb
```

- `<merge-base>` から `HEAD` までの差分は、PRへ含まれるcommit済み差分の確認に使う。stage済み、未stage、未追跡の変更は含まれない。
- PRへ含まれるcommit済み差分が、意図した変更範囲だけであることを確認する。
- 未commit変更が残っている場合は、PRに含まれないことを認識し、意図的な残存か確認する。
- pushまたはPR作成前には、対象remote、branch、baseを確認する。
- 新規PRの作成には `create-draft-pr` を使用する。
- 変更起因または変更範囲内のP1/P2相当の問題を残したままpushまたはPR更新しない。修正できない場合は理由と影響を明示して停止する。

## 完了条件

次を満たすまでセルフレビューを完了扱いにしない。

- レビュー対象のbase repository、base branch、base ref、merge-baseを特定した、または特定できなかった理由を記録した
- 作業ツリーと未追跡ファイルを確認した
- 差分を内容までレビューした
- 必要な検証を最終変更後に実行した
- 発見した変更起因の問題を修正し、影響する確認を再実行した
- stageした場合はstage済み差分を確認した
- 実行結果と未実行事項を報告できる状態になった

## 報告形式

PR本文または最終報告には、簡潔に次を含める。

- レビュー対象: base repository、base branch、確認した差分範囲
- セルフレビュー: 発見して修正した問題、または問題なし
- 検証: 実行したコマンドと結果
- 未実行・失敗: 理由、既存失敗との区別、影響範囲
- 残存リスク: 人手確認が必要な事項がある場合のみ
