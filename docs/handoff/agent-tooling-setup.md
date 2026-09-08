# エージェント用ツールのセットアップと診断

本書は、Claude Code と Codex が azooKey-Desktop の調査、Windows ビルド、実機確認で使うホスト側ツールの恒常 runbook である。
本書は `AGENTS.md`「調査と実装」「ローカルサブエージェント」から参照される。
標準のビルド、CTest、bench、TIP 登録コマンドは `README.md`、失敗時の切り分けは `docs/debugging.md` に置き、本書ではエージェント固有の接続条件と調査手順を扱う。

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

## 調査対象とツールの選択

Context-Mode、CodeGraph、Serena はホスト側の登録と呼出可否を確認し、repo へ重複登録しない。

| 対象 | 手順 |
|---|---|
| 既知の文書、設定、文字列 | Read / `rg` で対象パスと節を絞る |
| C++ の構造や影響範囲 | 対象 checkout に `.codegraph/` があり利用可能なら CodeGraph を先に使う。`projectPath` は実際の checkout、初回の `maxFiles` は 1〜3 とする |
| 定義と参照 | Serena の絶対パスと言語を確認する。既知の symbol は `name_path_pattern` と `relative_path` で絞り、本文不要なら `include_body: false` とする。実際のツール schema が対応する場合だけ `max_matches: 1` を追加する |
| 大量の文書、diff、ログ | Context-Mode 内で解析し、必要な証拠だけ返す。対象 cwd または絶対パスを指定し、repo ごとに取得と検索の対象を区別する |

index 不在、ツール不在、対象不一致の場合は、限定した `rg`、実ファイル、関連検証へ戻る。
index は自動作成せず、作成はユーザー判断とする。別 checkout の index を対象ソースの代用にしない。
Serena の診断は C++ の build / test / lint の代替にしない。
一度取得した正確なソースは再利用し、鮮度警告、編集、未解決の疑問がある範囲を再取得する。

## 長い出力の扱い

- ツール探索は名前と短い説明から始め、選択したツールだけ詳細を取得する。
- connector の結果は必要なフィールドだけ返す。形式が予想と異なる場合はキー一覧を確認し、全文を自動で返さない。
- diff はレビュー範囲全体をファイルごとに確認する。stage / commit 後は包含範囲と変更分を照合し、同じ全文を繰り返し返さない。
- Windows ビルドでは終了コード、失敗した target / CTest 名、重要 warning、根拠のログ位置を抽出する。ログの先頭だけで判定せず、末尾の失敗も確認する。
- 通常の返却量は 4〜8 KiB を目安にする。重要な証拠が多ければ段階的に追加し、切捨て上限にはしない。

要約は候補抽出に使い、修正の判断は実ファイル、最新 diff、関連テストで行う。
診断ログと Human Gate の記録は分ける。

## ローカル分担の例

Host 実装と TIP / IPC 利用側の影響確認、学習処理と復旧テストの検討、独立した差分レビューを分担候補にする。
分担時の共通規約は `docs/linear-conventions.md` §2.1 に従う。azooKey 固有の引継ぎ対象には、実機入力や管理者操作の承認条件を含める。
