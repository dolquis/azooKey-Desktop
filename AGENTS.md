# AIエージェント向け GitHub作業・PR作成ルール

このリポジトリで作業するAIエージェントは、以下のルールを必ず守ること。
本ファイルがエージェント規約の正典であり、`CLAUDE.md` は本ファイルを参照する。

## 最重要方針

- PRは必ずDraft PRとして作成する。
- フォーク元リポジトリにPRを出さない。
- 通常作業のPR作成先は、必ず `dolquis/<repository-name>` にする。
- `gh pr create` を使う場合は、必ず `--repo dolquis/<repository-name>` を明示する。
- `main` ブランチへ直接pushしない。
- フォーク元リポジトリへPRを出す必要がある場合は、作成前に必ずユーザーへ確認する。
- base repository / base branch / head repository / compare branch を確認せずにPRを作成してはいけない。

## 現行対象と legacy の扱い

- 現行の開発対象は **Windows 版 azooKey-Desktop** である。保守対象は `tsf-tip/`
  `inference-host/` `core/` `ipc/` `learning/` `settings/` などの Windows 実装。
- `legacy/` 配下の macOS / Swift 実装は保全された参照資産であり、保守対象ではない。
- `legacy/` の挙動を、そのまま Windows 版の正解として扱ってはいけない。参考にする場合も
  Windows 版としての仕様判断は別途行う。
- 仕様判断は `docs/*-spec.md`、開発順序・マイルストーン定義は
  `plans/windows-port-roadmap.md` を正典とする。進捗・状態・優先度の正典は Linear
  （「Linear 運用（管制塔）」参照）。`legacy/` と Windows 仕様が食い違う場合は後者を優先する。
- `legacy/` を変更対象に含める必要がある場合は、作業前に必ずユーザーへ確認する。

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
- バグや P1・P2 レベルの問題（クラッシュ・データ破損・機能不全・
  セキュリティリスクなど影響度の高い欠陥）が混入していないかを点検し、
  検出した場合は push 前に必ず修正する。
- セルフレビューで問題が見つかった場合は、push前に修正する。

## レビュー指摘事項の追跡・修正方針

レビュー・監査・セルフレビューで検出した問題は、以下の方針で必ず追跡・修正する。

- **当該セッション内で修正しない問題は、必ず Linear（team `Dev` / 該当 Project）に
  起票する。** 以後の修正は Linear 課題を起点に行い、PR 提出で In Review に、レビュー合格・
  マージ・検証メモ記載後に Done に遷移させる（状態遷移は「Linear 運用（管制塔）」の
  状態ライフサイクルに従う）。GitHub Issues は mirror であり、Linear 課題には対応する
  GitHub Issue / PR リンクを必須付与する。
- 課題本文には最低限、次を記載する: 該当 file:line / 現象 / 影響 / 推奨修正 /
  関連 Linear milestone (あれば)。重要度は Linear の **Priority**（Urgent・High・Medium・Low）で
  表し、`repo:*` / `area:*` / `agent:*` ラベルを付与する。
- 既存ロードマップ (`plans/windows-port-roadmap.md`) に該当節（「既知のテストギャップ」
  「リスク」、対応する M 番号など）があれば相互参照する。進捗・状態は roadmap に書かず Linear に置く。
- 問題一覧を README や `docs/`・roadmap に「TODO」「進捗」「状態」セクションとして
  書かない (時系列で陳腐化するため。トラッカーは Linear に一本化する)。
- セルフレビューで検出した P1・P2 は push 前に修正する。修正しないものは
  本方針に従って Linear 課題化し、放置しない。
- 例外: Linear が利用できない場合に限り、暫定的に該当 PR / GitHub Issue 本文に記載し、
  復旧後に Linear へ移す。
- 詳細な運用（正典マトリクス・ラベル・状態ライフサイクル・週次監査）は
  「Linear 運用（管制塔）」を参照。

## Linear 運用（管制塔）

開発の進捗・状態・優先度・トリアージは Linear で管理する（「管制塔」）。GitHub は
コード・PR・ドキュメントのホストであり、課題トラッキングの正典は Linear に置く。

### 正典マトリクス

| 情報種別 | 正典 |
|---|---|
| 状態・進捗・優先度・担当・サイクル | **Linear** |
| 課題トラッキング（バグ・タスクの起票/状態/優先度） | **Linear**（GitHub Issues は mirror） |
| 機能仕様（IPC payload・JSON schema・永続化形式・責務境界・fallback・設定項目・ユーザー可視挙動） | `docs/*-spec.md` |
| マイルストーン定義・依存関係・受け入れ条件の「定義」・スコープ・リスク | `plans/windows-port-roadmap.md`（達成状態は持たない） |
| コード・ビルド/テスト手順 | リポジトリ（`README.md`） |

進捗表・状態表を README / `docs/` / roadmap / Issue / PR 本文に作らない（陳腐化するため）。
状態は Linear のみが持つ。受け入れ条件は「定義」を roadmap、「達成状態」を Linear に置く。

### ワークスペース構成

- ワークスペース `dolquis` / team `Dev`（key `DEV`）。
- リポジトリごとに Project を分ける（`azooKey Desktop / Windows IME MVP` と
  `Shift Alarm / Calender_Aralrm` を分離済み）。複数リポジトリを 1 Project に混在させない。

### ラベル規約（起票時に必須付与）

- `repo:*` … リポジトリ識別（例 `repo:azooKey-Desktop`）。
- `area:*` … 技術領域（例 `area:tsf-tip` `area:ipc` `area:learning` `area:inference-host`
  `area:converter-core` `area:settings`（設定アプリ / schema）`area:build`（CMake / CTest /
  CI / コード署名 / MSIX）`area:docs`（ドキュメント・ガバナンス、repo 横断共有））。
- `agent:*` … 担当エージェント（`agent:claude-design` `agent:claude-review`
  `agent:codex-impl` `agent:codex-pr-review`）。ただし `gate:human-required` を付与した
  人間専任タスク（実機検証など）は AI 担当が存在しないため `agent:*` を免除する。
- `gate:human-required` … 完了前に人間の検証が必須な作業（実機確認・署名値設定など）。
  本ラベルを付与した課題は `agent:*` を省略してよい（上記 `agent:*` 参照）。
- `Migrated` … GitHub から移行した課題（対応する GitHub Issue リンクを必須付与）。
- **Codex 実行ポリシー** … `agent:codex-*` は候補（ルーティング）ラベルであり Codex 実行許可ではない。Codex Cloud の起動（assign / delegate / mention）は人間の明示許可があるときのみで、Claude は行わない。正典は `docs/linear-conventions.md` §2.1 Codex Execution Policy。

### 状態ライフサイクル

| status | 意味 | 遷移トリガ | 操作者 |
|---|---|---|---|
| Backlog | 未 triage | 起票直後 | 起票者 |
| Todo | triage 済・着手可（Priority 確定） | triage | Claude / 人間 |
| In Progress | 実装中 | ブランチ作成・着手 | Codex |
| In Review | Draft PR 提出済み | PR 作成 | Codex / Claude |
| Done | レビュー合格 + マージ + **検証メモ記載** | 人間マージ後 | Claude |
| Canceled / Duplicate | 中止 / 重複 | 随時 | Claude |

- Done への遷移時は、どのテスト / 実機確認で確認したかの検証メモを Linear にコメントする。
- 役割分担: Claude = 設計・レビュー（triage・起票・Priority・依存・status → Done）、
  Codex = 実装（`agent:codex-impl` 課題の着手 → 実装 → Draft PR）、人間 = 舵取り
  （Priority / Cycle 決定・マージ）。

### GitHub との対応

- ブランチ命名は Linear の自動命名に合わせ **`dolquis/dev-<番号>-<slug>`** とする。
- PR 本文に対応課題（`DEV-<番号>` / GitHub ミラーは `Fixes #<番号>`）を記載し、Linear と相互リンクする。ただし **`gate:human-required` / 検証メモ待ちの Issue では `Fixes #` を使わず `Refs #<番号>` 等の非クローズ参照にする**（GitHub Issue クローズ → Linear 同期で Done 化し In Review を迂回するため。§7.1.3）。
- PR オープン → 該当 Linear 課題を In Review。**PR マージは In Review までで止め（自動 Done にしない）**、Done は検証メモ記載後に Claude / 人間が明示遷移する（`docs/linear-conventions.md` §7.1.3）。
- `agent:claude-design` / `claude-review` と `gate:human-required` が両方絡む課題は、設計 Issue と人間ゲート Issue（`Human Gate: …`）に分離する（分離テストと規格は `docs/linear-conventions.md` §7.1）。

### 週次 control tower audit

Linear の定期監査課題（`[Recurring] Linear control tower audit`）で次を点検する:
Project / `repo:*` / `area:*` / `agent:*` ラベルの欠落（`gate:human-required` の人間専任
タスクは `agent:*` 免除）、`Migrated` の GitHub リンク欠落、人間検証作業の
`gate:human-required` 欠落、Tracking 課題の子未リンク、Done の検証メモ欠落。加えて
Codex safety checks（無許可の Codex delegate / mention、放置された delegate 済み課題など。
`docs/linear-conventions.md` §11）も点検する。
点検は Linear のルーティング衛生のみを対象とし、仕様・設計の正典は引き続き repo docs に置く。

## README 編集ルール

`README.md` はフォーク元 (ensan-hcl/azooKey) のオリジナル版に近い、
簡潔な紹介ドキュメントとして保つ。以下を厳守すること：

- 詳細な実装プラン・進捗状況・マイルストーン履歴を README に書かない。
  - 実装プラン・マイルストーン定義 → `plans/windows-port-roadmap.md`
    （開発計画は本ファイル 1 本に一本化）
  - 進捗・状態・優先度 → Linear（「Linear 運用（管制塔）」参照）
  - 機能ごとの仕様・ロジック → `docs/*-spec.md`
- README に新セクションを足す前に、その内容が `plans/` か `docs/` に
  収まらないかを必ず先に検討する。
- 「## 状態」「## 進捗」「## TODO」のような時系列で陳腐化するセクションを
  README に追加しない。進捗・状態は Linear に置き、README にも roadmap にも書かない。
- ロードマップへのリンクは README に置いてよいが、ロードマップの中身を
  README にコピーしない（リンクのみ、要約は 1〜2 行まで）。
- 開発計画・マイルストーン定義の正典は `plans/windows-port-roadmap.md`、進捗・状態の正典は
  Linear に一本化する。README へ転記せず、リンクと 1〜2 行の短い案内に留める。
- 進捗表・状態表を README / `docs/` / Issue / PR 本文に重複作成しない。
  トラッカー（進捗・課題）は Linear に一本化する。
- 例外的に README を肥大化させる必要があるときは、ユーザーに事前確認する。

## 実装変更とドキュメント更新の同期

コードを変更したら、影響範囲に応じて roadmap / docs / Issue / PR 本文を同時に更新する。
既存記述が不正確になる場合、ドキュメント更新は任意ではなく必須とする。

- **roadmap (`plans/windows-port-roadmap.md`)**: マイルストーン定義・受け入れ条件（定義）・
  依存関係・既知のテストギャップ・リスクの内容が変わる場合に更新する。進捗・完了状態・残作業は
  roadmap に書かず Linear に置く（roadmap は状態を持たない）。README へ転記しない。
- **仕様ドキュメント (`docs/*-spec.md`)**: IPC payload・JSON schema・永続化形式・
  TIP / Host / learning / settings 間の責務境界・fallback・ログ・設定項目・
  ユーザーから見える挙動が変わる場合に、対応する spec を更新する。
- **Issue / PR 本文**: 追跡中の問題や受け入れ条件に影響する場合は該当 Issue / PR を更新する
  （Issue 起票・追跡の詳細は「レビュー指摘事項の追跡・修正方針」に従う）。
- typo・コメントのみ・挙動を変えない小規模リファクタリング・テスト内部の整理など、
  ドキュメント記述に影響しない変更では roadmap / docs の更新は不要でよい。
- README は原則として更新先にしない。更新する場合も簡潔な案内・リンク追加に留める
  （「README 編集ルール」参照）。
- 上記の判断結果は PR 本文の「Documentation impact」欄に必ず記載し、更新しなかった
  場合はその理由も書く。

### PR 本文テンプレート: Documentation impact

PR 本文には次のチェック項目を含め、該当するものにチェックを入れる。

```md
## Documentation impact

- [ ] Roadmap updated
- [ ] Spec docs updated
- [ ] README update not needed
- [ ] No documentation impact

Reason:
```

## エージェントツール構成 (.claude/ .codex/ .agents/)

本リポジトリには Claude Code と Codex CLI 双方のための共有設定がコミット
されている。ビルド・CTest・bench 実行・TIP 登録の標準手順は `README.md` を
参照する (重複記述はしない)。本セクションはエージェント固有の構成と運用ルール
のみを扱う。再導入や横展開のための詳細手順書は `docs/handoff/` にある。

### MCP サーバー (`.mcp.json` / `.codex/config.toml`)

- `context7` … TSF / COM / Win32 API の公式ドキュメント参照 (全 OS で動作)。
- `powershell` … PowerShell.MCP。`scripts/register-dev.ps1` 等の machine-wide (HKLM)
  登録コマンドを共有コンソール経由で提示する用途。**実行はユーザが管理者 PowerShell で
  完了させること**（`register-dev.ps1` は machine-wide 登録のため管理者権限が必要。非管理者で
  起動した場合は自動で UAC 昇格する）。エージェントが単独で TIP 登録を完了させてはならない。
- `windows-mcp` … UI Automation で TIP の実アプリ動作を検証 (Windows 専用)。

`powershell` MCP の `command` には環境変数 `POWERSHELL_MCP_PROXY` を参照させて
いる。Windows 開発者は `Install-PSResource PowerShell.MCP` 後に以下を一度実行
して PowerShell.MCP の Proxy パスを永続化すること:

```powershell
[Environment]::SetEnvironmentVariable(
  'POWERSHELL_MCP_PROXY', (Get-MCPProxyPath), 'User')
```

macOS / Linux のメンテナがリポジトリを開いた場合、`powershell` /
`windows-mcp` は起動失敗するが想定動作 (`context7` のみ全 OS で動く)。

### Claude Code 公式マーケットプラグイン (`.claude/settings.json`)

`enabledPlugins` で以下を有効化している (公式マーケット `claude-plugins-official`
は自動的に利用可能):

- `clangd-lsp` … `core/` `tsf-tip/` 等で補完・型診断 (ホスト側に
  `clangd.exe` のインストールが必要)。
- `github` … `gh` 経由の PR 作業をエージェントから可能にする。
- `commit-commands` … 規約に沿ったコミットメッセージ生成。
- `pr-review-toolkit` … PR レビュー補助。

### カスタムスキル (`.claude/skills/` `.agents/skills/`)

- `tsf-tip-development` … TSF TIP 実装の中核ルールと参照リソース。
- `tsf-ipc-protocol` … TIP ⇔ Inference Host の独自 IPC プロトコル仕様。

Claude Code は `.claude/skills/`、Codex CLI は `.agents/skills/` を読む。
**両ツリーは同一の skill 群と同一の実質ガイダンス（SKILL.md 本文・`references/` 配下）を
維持する。** 片方の本文または参照を変更したら、もう片方も同時に更新する。ただし
Claude Code 固有の `allowed-tools:` などのフロントマターは各ハーネス向けに差異が
あってよく、ミラー対象外とする（将来的に symlink 統合の余地あり）。

### 動作要件 (各メンテナのホスト側に必要)

- PowerShell 7+ と PowerShell.MCP (`Install-PSResource PowerShell.MCP`)
- `uv` (`uvx` コマンド) のインストール — `windows-mcp` の起動に必要
- `clangd.exe` — `clangd-lsp` プラグイン用 (VS C++ ワークロードまたは LLVM
  公式インストーラ経由)
- WSL から Claude Code を使う場合は `powershell.exe` 経由で Windows 側を駆動
