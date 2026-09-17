---
name: azookey-linear-issue-ops
description: azooKey Desktop の作業で Linear（workspace dolquis / team Dev）の Issue を起票、更新、状態遷移、コメント、監査するときに使う。レビューや監査で見つけたが今回直さない問題の起票、Draft PR 作成時の In Review 遷移、マージ時の Merged 遷移、検証メモを書いて Done にする操作、Human Gate の分割、週次監査の実施が対象。「Linear に起票して」「課題を Done にして」「この指摘を追跡して」のような依頼では必ずこのスキルを使う。
---

# azooKey Linear 課題運用

状態、進捗、優先度、担当の正典は Linear であり、repo docs には書かない。
このスキルは規約を写さず、手順と使うツールだけを持つ。ラベル、状態、Issue の記述形式、
週次監査の項目は `docs/linear-conventions.md` が正典で、azooKey 固有の値は同 §13 Project Delta にある。

## 使うツール

Linear MCP を優先する。起票と更新は `save_issue`、コメントは `save_comment`、
重複確認は `list_issues`、ラベルと状態の実名は `list_issue_labels` と `list_issue_statuses` で
毎回取得する。ラベル名や状態名を記憶や規約文からの推測で書かない。

## 手順

### 起票

1. `list_issues` で同じ file / symbol、同じ現象の Issue が無いか確認する。あれば新規作成せず、
   コメントで追記する。
2. 所属 Project を `docs/linear-conventions.md` §13 の `PROJECT_NAME` から確定する。
3. 本文は AGENTS.md「Linear とレビュー指摘」の項目（file / symbol、現象、影響、推奨修正）と
   §5 Agent Task Format に沿って書く。Priority、`repo:*`、`area:*`、`kind:*`、`type:*`、
   担当 `agent:*` を付ける。人間専任なら `gate:human-required` を付け、`agent:*` を省略できる。
4. 実装 Issue と人間ゲートは §7.1 の分離テストで分け、同居させない。

### PR と連動する状態遷移

1. Draft PR を作成したら Issue を In Review にし、PR URL をコメントに残す。
   PR 本文からは DEV 番号と GitHub mirror を相互参照する（`create-draft-pr` の手順内で行う）。
2. マージされたら Merged にする。検証メモが書けるまで Done にしない。
3. Done にするときは §6 Agent Run Report Format で検証メモ（実行した検証、未実行、次アクション）を
   コメントに残してから遷移する。Human Gate が残る Issue は Done にしない。
4. PR 本文の `Fixes` は GitHub mirror を自動 close するため、Human Gate や検証メモ待ちの Issue には
   使わず `Refs` にする。

### 監査

週次監査は機械判定から始める。機械判定の正典は origin `dolquis/agent-ops` が持つ
`<agent-ops>/scripts/linear-audit.py` で、この repo へベンダリングしない。team 横断のツールであり、
product repo 側に複製を置くと silent fork になる。実行のたびに origin を取得する。

```sh
git clone --depth 1 https://github.com/dolquis/agent-ops <path>
LINEAR_API_KEY=<非空文字列> python3 <path>/scripts/linear-audit.py --team Dev
```

読み取り専用で Linear へ書き込まない。`CONFIRMED` が 1 件でもあると exit 1 になる。
`api.linear.app` への認証をインジェクトするハーネスでは実鍵を渡さない（`LINEAR_API_KEY` は
スクリプト側の非空チェックを通すためだけに要る）。インジェクトが無い環境では実鍵を環境変数で
渡し、コマンド履歴と Issue 本文に残さない。TLS 検証が通らない場合はハーネスの CA bundle を
`SSL_CERT_FILE` で指す。

`CONFIRMED` を先に処置する。`REVIEW` はヒューリスティックで過検出を前提とするため、採否を
判断して棄却する場合は理由を当該 Issue に残す。そのうえで §11 の共通項目と §13 の repo 固有項目
（spec-first、アーカイブ衛生、保留理由の失効、実装課題と人間ゲートの分離、一括誤 Done、
Linear と main の突合）を順に確認する。機械判定が持たない項目はこちらにしかない。
結果は Project の Status Update に追記する。

## 必須ガードレール

- Codex Cloud の assign、delegate、mention は人間の明示許可なしに行わない。
- 未完了の Issue をアーカイブしない。完了扱いは Done / Canceled / Duplicate の明示遷移だけにする。
- 仕様の本文を Linear にコピーしない。Issue からは `docs/*-spec.md` の節へリンクする。
- Issue 本文やコメントに secret、ユーザー入力の本文、ローカルの絶対パスを書かない。
- 他人の Issue の Priority や担当を、依頼なしに変えない。

## 完了条件

- 作成・更新した Issue の識別子、状態、付けたラベル、所属 Project を報告する。
- Done にした場合は検証メモの所在を、保留にした場合は保留理由と失効条件を報告する。
