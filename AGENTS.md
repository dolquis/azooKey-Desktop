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

## README 編集ルール

`README.md` はフォーク元 (ensan-hcl/azooKey) のオリジナル版に近い、
簡潔な紹介ドキュメントとして保つ。以下を厳守すること：

- 詳細な実装プラン・進捗状況・マイルストーン履歴を README に書かない。
  - 実装プラン・マイルストーン定義・現状 → `plans/windows-port-roadmap.md`
    （開発計画は本ファイル 1 本に一本化）
  - 機能ごとの仕様・ロジック → `docs/*-spec.md`
- README に新セクションを足す前に、その内容が `plans/` か `docs/` に
  収まらないかを必ず先に検討する。
- 「## 状態」「## 進捗」「## TODO」のような時系列で陳腐化するセクションを
  README に追加しない。該当情報は `plans/windows-port-roadmap.md` の
  各マイルストーン「現状」欄に書く。
- ロードマップへのリンクは README に置いてよいが、ロードマップの中身を
  README にコピーしない（リンクのみ、要約は 1〜2 行まで）。
- 例外的に README を肥大化させる必要があるときは、ユーザーに事前確認する。

## エージェントツール構成 (.claude/ .codex/ .agents/)

本リポジトリには Claude Code と Codex CLI 双方のための共有設定がコミット
されている。ビルド・CTest・bench 実行・TIP 登録の標準手順は `README.md` を
参照する (重複記述はしない)。本セクションはエージェント固有の構成と運用ルール
のみを扱う。再導入や横展開のための詳細手順書は `docs/handoff/` にある。

### MCP サーバー (`.mcp.json` / `.codex/config.toml`)

- `context7` … TSF / COM / Win32 API の公式ドキュメント参照 (全 OS で動作)。
- `powershell` … PowerShell.MCP。`scripts/register.ps1` 等の管理者コマンドを
  共有コンソール経由で提示する用途。**実行はユーザが管理者 PowerShell で
  完了させること**。エージェントが単独で TIP 登録を完了させてはならない。
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
**両ディレクトリは同一内容を維持する運用とし**、SKILL.md または `references/`
配下を変更した際は両方を同時に更新する (将来的に symlink 統合の余地あり)。

### 動作要件 (各メンテナのホスト側に必要)

- PowerShell 7+ と PowerShell.MCP (`Install-PSResource PowerShell.MCP`)
- `uv` (`uvx` コマンド) のインストール — `windows-mcp` の起動に必要
- `clangd.exe` — `clangd-lsp` プラグイン用 (VS C++ ワークロードまたは LLVM
  公式インストーラ経由)
- WSL から Claude Code を使う場合は `powershell.exe` 経由で Windows 側を駆動
