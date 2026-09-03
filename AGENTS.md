# AGENTS.md instructions

原則として日本語で回答してください。

## 最優先の安全規則

- ユーザーの未コミット変更を勝手に戻さない。同じファイルで交差する場合は差分を確認し、最小限の編集にする。
- 作業開始時、進捗確認時、PR 前に `git status -sb` を確認する。進捗確認では `git fetch origin` 後に `main` と `origin/main` を比較し、作業中ブランチは不用意に切り替えない。
- secret、credential、token、`.env`、ローカル設定、生成物、Context-Mode の DB / cache、CodeGraph の index を編集、index、commit しない。
- `main` へ直接 push しない。新規 PR は Draft とし、通常は `dolquis/<repository-name>` の `main` 向けに作成する。base / head の repository と branch、compare 範囲を確認し、同じ head の PR を重複作成しない。
- フォーク元や upstream への PR、`legacy/` の変更は、実行前にユーザーへ確認する。
- 実機確認、管理者権限、署名などの Human Gate を CI の成功で代替しない。

## 対象と正典

- 開発対象は Windows 版 azooKey-Desktop。主な保守領域は `tsf-tip/`、`inference-host/`、`core/`、`ipc/`、`learning/`、`settings/`。
- `legacy/` は macOS / Swift の参照資産であり、Windows 版の仕様判断には `docs/*-spec.md` を優先する。
- 状態、進捗、優先度、担当、サイクル、課題追跡の正典は Linear（workspace `dolquis` / team `Dev`）。GitHub Issues は mirror とする。
- 機能仕様の正典は対応する `docs/*-spec.md`、マイルストーン定義、依存関係、受け入れ条件の定義、リスクは `plans/windows-port-roadmap.md`、ビルドとテストの標準手順は `README.md` とする。
- Linear のラベル、状態遷移、GitHub 連携、週次監査は `docs/linear-conventions.md` を参照する。repo 固有差分は同文書の Project Delta に置く。

## 調査と実装

- 不明点が結果を大きく変える場合は確認する。軽微な判断は既存実装、仕様、リポジトリの慣習に従う。
- 非自明な変更では、編集前にスコープ、予定ファイル、検証方法、主なリスクを短く整理する。
- 最小差分を基本とし、無関係なリファクタリング、整形、rename、メタデータ更新を混ぜない。
- 小さな単一ファイル修正や文字列検索には Read / Edit / `rg` を使う。範囲が広い場合は利用可能な調査ツールを選び、大量の検索結果、diff、log をそのまま会話へ流さない。
- `.codegraph/` があり CodeGraph が利用可能なら、構造、呼び出し関係、影響範囲、関連テストの候補出しに使う。対象シンボルが既知なら Serena で宣言、実装、参照を確認し、開始前に active project と languages を確認する。
- Context-Mode が利用可能なら、長い文書、検索結果、diff、build / test / CI log の整理に使う。要約だけで完了を判断せず、重要箇所は実ファイル、最新 diff、関連テストで確認する。
- 公開 API、schema、永続化、認証、権限、安全設計、データ削除、課金、通知、外部連携では、構造と参照元と関連文書を確認してから変更する。
- GitHub 操作は Codex GitHub 連携を優先し、必要に応じて `gh` CLI を使う。

## ドキュメント

- README、AGENTS、CLAUDE、`docs/`、`plans/` を変更するときは `$azookey-doc-governance` を使う。
- コード変更で仕様、責務境界、fallback、ログ、設定、ユーザー可視挙動が変わる場合は対応する spec を更新する。マイルストーン定義やリスクが変わる場合は roadmap を更新する。
- 状態や進捗を README、docs、roadmap、PR 本文へ複製しない。状態語、行番号付きコード参照、変動するテスト件数を恒常文書へ書かない。
- 新規または rename した文書は `docs/README.md` に索引する。恒常 runbook は `docs/handoff/`、一回限りの記録は完了基準を満たしたら `docs/archive/` に置く。
- PR 本文の Documentation impact は `.github/PULL_REQUEST_TEMPLATE.md` を使う。docs 変更時は `python3 scripts/docs-lint.py` を実行し、新規 warning を確認する。
- 日本語の技術文書を新規作成または大きく推敲するときは `$japanese-doc-workflow` を入口にする。

## セルフレビューと PR

- リポジトリ内ファイルを変更したら、最終報告、stage、commit、push、PR 作成または更新の前に `$pre-pr-self-review` を使う。
- レビュー範囲は通常 `origin/main` との merge-base から作業ツリーまで。既存 PR やユーザーが別 base を指定した場合はそれに従う。
- 変更起因または変更範囲内の問題は修正して検証を再実行する。無関係な既存問題は勝手に直さず、必要なら Linear へ記録する。
- 実行できなかった検証、失敗、既存失敗は、理由と影響範囲を最終報告と PR 本文に記載する。
- 新規 PR は `$create-draft-pr` を使う。`gh pr create` を使う場合は `--repo dolquis/<repository-name> --base main --head <branch-name> --draft` を明示する。
- 通常のマージ方法はノーマルマージとする。Ready 状態の既存 PR を無理に Draft へ戻さない。

## Linear とレビュー指摘

- 当該作業で修正しないレビューまたは監査の問題は、Linear の該当 Project に起票し、file / symbol、現象、影響、推奨修正、Priority、`repo:*`、`area:*`、担当 `agent:*` を記録する。人間専任課題では `gate:human-required` を付け、`agent:*` を省略できる。
- ブランチ名は `dolquis/dev-<番号>-<slug>` とする。Draft PR 作成時に Linear を In Review、マージ時に Merged とし、検証メモを記載してから Done にする。
- PR 本文から対応する DEV 課題と GitHub mirror を相互参照する。Human Gate や検証メモ待ちの課題には `Fixes` を使わない。
- Codex Cloud の assign、delegate、mention は、人間の明示許可なしに行わない。

## Windows ビルドと実機操作

- Windows CMake / Ninja / MSVC の実ビルドと実テストには Windows Headless CMake Build を使い、実 Ninja build / test は `sandbox_permissions=require_escalated` で実行する。
- Full CTest は `cmake --build --preset windows-debug --target azookey_check` を優先する。詳細と停止時の切り分けは `README.md` と `docs/debugging.md` を参照する。
- `.ninja_lock` は関連する `ninja`、`cmake`、`cl`、`link`、`ctest` のプロセスを確認するまで削除しない。
- `CreateProcessAsUserW failed: 5` は、同じ最小 probe を適切な elevated 経路で再試行してからツール不在と判断する。
- TIP の machine-wide 登録はユーザーが管理者 PowerShell で完了する。エージェント単独で Human Gate を完了扱いにしない。
- エージェント用 MCP、プラグイン、ホスト前提、doctor の手順は `docs/handoff/agent-tooling-setup.md` を参照する。

## Skill の配置

- Claude Code は `.claude/skills/`、Codex は `.agents/skills/` を読む。共有 Skill は `dolquis/agent-ops` を正典とし、product repo では直接改訂せずベンダリングする。
- 両ツリーは本文と `references/` を同期し、ハーネス固有 frontmatter だけ差異を許す。`doc-coauthoring` などの Claude 専用 Skill は `.claude/skills/` のみに置く。
