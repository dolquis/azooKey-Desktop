---
name: japanese-doc-workflow
description: 日本語の技術文書、README、設計書、ADR、解説記事、書籍原稿を書く・推敲するときに使う統合ワークフロー。japanese-tech-writing、argument-gap-edit、doc-coauthoring、textlintを併用する。
allowed-tools: Read, Edit, Grep, Glob
---

# Japanese Doc Workflow

日本語の技術文書を書く、直す、レビューするときに使う。

## 使う規範

作業前に次を読む。

- `../japanese-tech-writing/SKILL.md`
- `../argument-gap-edit/SKILL.md`

新規文書を共同で作る場合は `../doc-coauthoring/SKILL.md` を使う。

## 作業手順

1. 文書の目的、読者、公開先、完成形を確認する。
2. 新規作成なら、まずアウトラインを作る。
3. 既存文書の推敲なら、段落ごとに論理の受け渡しを点検する。
4. japanese-tech-writing の規範に沿って文体を整える。
5. 可能なら `npx textlint` を実行し、警告を確認する。
6. textlint の警告は機械的に従わず、文脈に照らして採否を判断する。
7. 最終出力では、修正内容を「論理」「文体」「表記」「残課題」に分けて報告する。

## textlint の扱い

textlint は校正補助であり、最終判断者ではない。
誤検知や文体上必要な例外は残してよい。
ただし、長すぎる文、表記揺れ、二重否定、冗長表現は優先して確認する。
