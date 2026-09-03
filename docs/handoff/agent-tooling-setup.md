# エージェント用ツールのセットアップと診断

本書は、Claude Code と Codex が azooKey-Desktop の調査、Windows ビルド、実機確認で使うホスト側ツールの恒常 runbook である。
標準のビルド、CTest、bench、TIP 登録コマンドは `README.md`、失敗時の切り分けは `docs/debugging.md` に置き、本書ではエージェント固有の接続条件だけを扱う。

## 設定の正典

- Claude Code の repo MCP は `.mcp.json`、プラグインは `.claude/settings.json` が定義する。
- Codex の repo 固有 sandbox と MCP は `.codex/config.toml` が定義する。
- Context7 は各開発者が Claude Code と Codex のユーザー連携で有効にする。repo 設定には重複登録しない。Context7 が利用できない場合、Microsoft API は Microsoft Learn の公式リファレンスを優先する。
- Skill の配置とミラー規則は `AGENTS.md`「Skill の配置」に従う。

設定ファイル自体がサーバー名、引数、プラグイン一覧の正典である。
本書へ一覧を転記せず、接続失敗時は設定ファイルとユーザー連携の両方を確認する。

## ホスト側の前提

- PowerShell 7+
- PowerShell.MCP（machine-wide 操作を共有コンソールへ提示するため）
- `uv` と `uvx`（`windows-mcp` の起動に必要）
- `clangd.exe`（Claude Code の `clangd-lsp` プラグインに必要）
- Windows CMake / Ninja / MSVC を使う Visual Studio C++ toolchain
- 任意の補助コマンドとして `just`

PowerShell.MCP は PowerShell 7 で次のように導入し、`Get-MCPProxyPath` が返す proxy の絶対パスをユーザー環境変数 `POWERSHELL_MCP_PROXY` に設定する。

```powershell
Install-PSResource PowerShell.MCP
[Environment]::SetEnvironmentVariable(
  'POWERSHELL_MCP_PROXY', (Get-MCPProxyPath), 'User')
```

WSL から Claude Code を使う場合、Windows CMake / Ninja / MSVC と PowerShell.MCP は `powershell.exe` 経由で Windows 側を駆動する。Codex で Windows Headless CMake Build が利用可能な場合はその手順を使い、実 build / test はホスト実行に必要な権限で起動する。どちらも README の preset と受け入れ条件を変更しない。

Windows 以外のホストでは PowerShell.MCP と Windows UI Automation を前提にせず、該当する実機確認を Human Gate として引き継ぐ。

## 診断の入口

環境起因の失敗を調べるときは、最初に次を実行する。

```powershell
just doctor --fix-hints
```

機械可読な結果が必要な場合は、次を使う。

```powershell
just doctor --json
```

結果を読むときは、repo 設定、ユーザー連携、実行ファイルの有無、環境変数、Windows 固有権限を分けて確認する。
`CreateProcessAsUserW failed: 5` が出た場合は、同じ最小 probe を elevated 経路で再試行する。

## 管理者権限と実機確認

PowerShell.MCP は、`scripts/register-dev.ps1` などの machine-wide 操作を共有コンソールへ提示するために使う。
TIP 登録はユーザーが管理者 PowerShell で完了し、エージェントは単独で成功扱いにしない。

UI Automation は TIP の実アプリ挙動を確認する補助である。
登録、署名、実機入力などの Human Gate は、自動テストや UI Automation の成功だけでは完了しない。

## 長い出力の扱い

CodeGraph は構造と影響範囲、Serena はシンボルと参照、Context-Mode は長い文書、diff、log の整理に使う。
接続できないツールがある場合は、対象を狭めた `rg`、ファイル読み取り、build / test log の直接確認へ切り替える。
要約結果は候補抽出に使い、修正完了の判断は実ファイル、最新 diff、関連テストで行う。
