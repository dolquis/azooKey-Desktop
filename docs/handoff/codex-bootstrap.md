# Codex CLI 引き継ぎ：`.codex/` `.agents/` セットアップ

> このファイルは、`dolquis/azooKey-Desktop`(Windows版 IME、C++ / CMake / TSF)に
> **OpenAI Codex CLI** 用の共有設定を導入するための、Codex CLI への作業指示書です。
>
> **方針：マーケット配布の MCP を最大限活用し、自作スキルはプロジェクト固有で
> 代替が無い領域のみに限定する。Claude Code 版とは独立に維持する。**

---

## 0. 大前提(厳守)

- 本リポジトリは `azooKey/azooKey-Desktop` の **fork**。フォーク元には PR を出さない。
- PR は必ず **`dolquis/azooKey-Desktop` 宛・Draft** で作成する。
- `main` ブランチに直接 push しない。新規ブランチを切って作業する。
- `gh pr create` を使うときは `--repo dolquis/azooKey-Desktop --base main --draft` を必ず明示。
- `legacy/` 以下(旧 macOS 実装)は **絶対に編集しない**。参照のみ。
- 既存の Claude Code 用設定(`.claude/` `.mcp.json` `CLAUDE.md`)は **触らない**。Codex 用は独立で構築する。

## 1. プロジェクト現状サマリー

- Windows 版に方針転換済み。macOS 実装は `legacy/` に保全(未保守)。
- 構成：
  - `tsf-tip/` — Text Services Framework TIP(in-proc COM DLL)
  - `inference-host/` — 推論ホスト(別プロセス、CPU、将来 CUDA)
  - `core/` — OS 非依存のかな漢字変換コア
  - `ipc/` — Named Pipe + JSON + length-prefix の IPC 定義
  - `learning/` — 頻度＋時間減衰の再ランキング
  - `bench/` — レイテンシ計測 CLI
  - `scripts/` — `register-dev.ps1` / `unregister-dev.ps1`(machine-wide / HKLM、管理者権限が必要・非管理者なら自動昇格)
- ビルド：Windows 10/11 + Visual Studio 2022(C++ デスクトップ)+ CMake ≥ 3.21 + Windows SDK
- テスト：CTest + GoogleTest(`-DAZOOKEY_FETCH_GOOGLETEST=ON` で FetchContent)
- 既存メタファイル：`CLAUDE.md`(Claude Code 用)、`AGENTS.md`(Codex CLI および人間用)

## 2. Codex CLI と Claude Code の設定差(復習)

| 項目 | Claude Code | Codex CLI |
|---|---|---|
| プロジェクト命令 | `CLAUDE.md` | **`AGENTS.md`** |
| プロジェクト設定 | `.claude/settings.json`(JSON) | **`.codex/config.toml`**(TOML) |
| MCP 定義 | `.mcp.json`(独立、JSON) | **`.codex/config.toml` 内 `[mcp_servers.*]`**(統合) |
| スキル配置 | `.claude/skills/<name>/SKILL.md` | **`.agents/skills/<name>/SKILL.md`** |
| SKILL.md フォーマット | YAML frontmatter + 本文 | **完全に同じ**(クロス互換) |

## 3. ゴール

リポジトリ直下に以下を配置する。

```
AGENTS.md                              ← 新規(または既存追記)。Codex CLI が必ず読む
.codex/
  config.toml                          ← Codex プロジェクト設定 + MCP 定義(共有)
.agents/
  skills/                              ← 自作スキル(最小限のみ)
    tsf-tip-development/
      ├── SKILL.md
      └── references/
          ├── itf-interfaces.md
          └── sample-projects.md
    tsf-ipc-protocol/
      ├── SKILL.md
      └── references/
          └── frame-format.md
```

**自作スキルを2個に絞り込んだ理由**

| 項目 | どうするか | 理由 |
|---|---|---|
| `cmake-msvc-build` | AGENTS.md に標準コマンド3行追記で代替 | Codex の `shell` ツール + Bash で十分 |
| `tsf-tip-development` | **残す**(自作)| TSF/COM/IUnknown はマーケットに該当スキルが存在しない |
| `tip-register-flow` | AGENTS.md に注意事項を追記、PowerShell.MCP で実行 | 人間介在型の実行は MCP 側で安全に扱える |
| `tsf-ipc-protocol` | **残す**(自作)| Named Pipe + length-prefix の独自プロトコルはプロジェクト固有 |
| `bench-latency` | AGENTS.md に運用ルール3行追記で代替 | 通常の Bash 実行で十分 |

## 4. `.codex/config.toml` の作成

リポジトリ直下に `.codex/config.toml` を新規作成し、コミットする。

> **重要**: Codex CLI は `.codex/config.toml` を **trust された project でのみ** 読み込む。
> 各開発者が初回起動時に Codex の trust プロンプトに同意する必要がある。

> **TOML 文法上の注意**: トップレベルのスカラーキー (例:
> `sandbox_mode`, `approval_policy`) は **最初の `[テーブル]` 見出しより前** に
> 置かなければ、後続のテーブルに飲み込まれてしまう。下記サンプルは
> 「トップレベル → テーブル定義」の順に並べているのでこの順序を保つこと。

```toml
#:schema https://developers.openai.com/codex/config-schema.json

# ─────────────────────────────────────────────
# トップレベル設定 (サンドボックス / 承認ポリシー)
# ─────────────────────────────────────────────

# workspace-write: リポジトリ内の書き込みは許可、外部ネットワークは別途許可
sandbox_mode = "workspace-write"

# 重要操作はユーザー承認を仰ぐ
approval_policy = "on-request"

# ─────────────────────────────────────────────
# サンドボックス: workspace-write の挙動
# ─────────────────────────────────────────────

[sandbox_workspace_write]
# MCP HTTP(context7 等)と GoogleTest FetchContent のためにネットワーク許可
network_access = true

# ─────────────────────────────────────────────
# Windows ネイティブ実行時の設定
# ─────────────────────────────────────────────

# TIP の machine-wide 登録は管理者が別途実行する運用。Codex 自身は昇格しない（unelevated を維持）
[windows]
sandbox = "unelevated"
sandbox_private_desktop = true

# ─────────────────────────────────────────────
# MCP サーバー定義(共有)
# ─────────────────────────────────────────────

# Context7: TSF/COM/Win32 API のドキュメントを最新で参照
[mcp_servers.context7]
url = "https://mcp.context7.com/mcp"
startup_timeout_sec = 15

# PowerShell.MCP: register-dev.ps1 等の Windows 側コマンドを安全に提示
# PowerShell.MCP は PowerShell.MCP.Proxy.exe を stdio で起動する仕様。
# 各開発者は Install-PSResource PowerShell.MCP 後に
# `[Environment]::SetEnvironmentVariable('POWERSHELL_MCP_PROXY',
#   (Get-MCPProxyPath), 'User')` で環境変数にパスを永続化する。
[mcp_servers.powershell]
command = "${POWERSHELL_MCP_PROXY}"
startup_timeout_sec = 30

# Windows-MCP: UI Automation で TIP の実アプリ入力を検証
[mcp_servers.windows-mcp]
command = "uvx"
args = ["windows-mcp", "serve"]
startup_timeout_sec = 30

# ─────────────────────────────────────────────
# プロジェクトルート判定
# ─────────────────────────────────────────────

# Git ベースなので default(`.git`)で十分。明示しなくてもOK。
# project_root_markers = [".git"]

# ─────────────────────────────────────────────
# Web 検索 / モデル(任意。各開発者の好みに任せる場合は省く)
# ─────────────────────────────────────────────

# web_search = "cached"
# model = "gpt-5-codex"
```

**Codex CLI が実装時に確認すべきこと:**

1. `mcp_servers.<name>` のキー名はサーバー固有 ID。本ドキュメントの命名(`context7` `powershell` `windows-mcp`)に従う。
2. `PowerShell.MCP` / `Windows-MCP` の正確な起動コマンドを最新ドキュメントで照合：
   - PowerShell.MCP: <https://github.com/yotsuda/PowerShell.MCP>
   - Windows-MCP: <https://github.com/CursorTouch/Windows-MCP>
3. シークレットを TOML に直接書かない。必要な環境変数があれば `env` テーブルで参照：
   ```toml
   [mcp_servers.<name>.env]
   SOME_API_KEY = "${SOME_API_KEY}"
   ```
4. macOS/Linux のメンテナがこのリポジトリを開いた場合、`powershell` / `windows-mcp` は
   起動失敗するが想定動作。`context7` だけは全 OS で動く。失敗を許容するため
   `required = false`(既定)のままにし、`required = true` を付けない。
5. **`sandbox_mode = "workspace-write"`** と **`network_access = true`** の組み合わせは、
   GoogleTest の FetchContent と MCP HTTP のために必要。さらに強い制限が必要なら
   `[permissions.<name>]` で名前付きプロファイルを後から追加する余地がある。

## 5. `AGENTS.md` の作成(または既存への統合)

リポジトリ直下に `AGENTS.md` を新規作成、または既存に追記する。Codex CLI は起動時に
これを最初に読み、毎ターン参照する。Claude Code の `CLAUDE.md` とは**独立に維持**する
方針なので、内容が重複してもよいが、片方の更新を忘れない運用ルールを `README.md` に書く。

````markdown
# AGENTS.md — azooKey-Desktop(Windows 版)

このファイルは OpenAI Codex CLI、および本リポジトリで作業する人間のエージェント全員が
最初に読むべき規約です。

## 厳守事項

- 本リポジトリは `azooKey/azooKey-Desktop` の fork。フォーク元には PR を出さない。
- PR は必ず **`dolquis/azooKey-Desktop` 宛・Draft** で作成。
- `main` への直接 push 禁止。新規ブランチを切る。
- `gh pr create` は `--repo dolquis/azooKey-Desktop --base main --draft` を明示。
- `legacy/`(旧 macOS 実装)は編集しない。参照のみ。

## プロジェクト概要

Windows 版 IME。TSF Text Input Processor(in-proc COM DLL)+ 別プロセスの Inference Host
で構成。`tsf-tip/` `inference-host/` `core/` `ipc/` `learning/` `bench/` `scripts/`。

## ビルド(CMake + MSVC)

```bash
cmake --preset windows-debug -DAZOOKEY_FETCH_GOOGLETEST=ON
cmake --build --preset windows-debug
ctest --preset windows-debug --output-on-failure
```

`-DAZOOKEY_FETCH_GOOGLETEST=ON` は FetchContent で GoogleTest を取得する。
オフライン環境では `-OFF` でテストのみスキップしてビルドを通す。

## TIP 登録 / 解除(machine-wide / 管理者権限)

`scripts/register-dev.ps1` / `unregister-dev.ps1` は管理者 PowerShell で実行する（非管理者で
起動した場合は自動で UAC 昇格する）。**Codex CLI は単独で実行を完了させてはならない**。
PowerShell.MCP の共有コンソール経由で、コマンド提示までに留め、実行はユーザーが確定する。
`DllRegisterServer` と `register-dev.ps1` は machine-wide に登録するため、失敗時は
`HKLM\Software\Classes\CLSID\{...}`（COM in-proc）と
`HKLM\Software\Microsoft\CTF\TIP\{...}`（TSF プロファイル）の登録状態、および
プロセスが昇格しているかを確認すること。

## レイテンシ計測

`core/` `learning/` `ipc/` を編集したら、Release ビルドで `build/windows-release/bench/azookey_bench.exe` を
実行し、変更前後の数値を PR 本文に貼る。Debug 数値は性能評価に使わない。

## スキル(`.agents/skills/`)

- `tsf-tip-development` — TSF TIP 実装の中核ルールと参照リソース
- `tsf-ipc-protocol` — TIP ⇔ Host の独自 IPC プロトコル仕様

該当作業時は Codex が自動的に当該 SKILL.md を読み込む。

## MCP サーバー(`.codex/config.toml`)

- `context7` — TSF/COM/Win32 API のドキュメント参照
- `powershell` — Windows 側コマンドの安全な提示と実行(人間介在)
- `windows-mcp` — UI Automation で TIP の動作検証
````

Codex CLI が `AGENTS.md` に追記する場合、既存内容と矛盾しない場所に配置すること。
すでに `AGENTS.md` がある場合は、上記の各セクションを「Codex 用」とマークして
末尾に追加する形にする。

## 6. 自作スキル個別仕様

SKILL.md フォーマットは Claude Code 版と完全に同じ。**前回 Claude Code 用に作成したものと
本文は同一**で、配置先だけ `.claude/skills/` → `.agents/skills/` に変わる。

### 6.1 `.agents/skills/tsf-tip-development/SKILL.md`

````markdown
---
name: tsf-tip-development
description: tsf-tip/ 配下の C++ コード(ITfTextInputProcessor 実装、COM コンポーネント、TSF コールバック)を編集・追加・デバッグするときに使用する。COM / TSF / Win32 API の作業で自動起動する。
---

# TSF TIP 開発ガイド

## このスキルが扱う範囲

- `tsf-tip/` 配下のすべての C++ 実装
- COM クラス登録(CLSID)と TSF プロファイル登録(Profile GUID)
- `ITfTextInputProcessor`, `ITfThreadMgrEventSink`, `ITfCompositionSink`, `ITfKeyEventSink` 等の実装
- `DllRegisterServer` / `DllUnregisterServer` のエクスポート

## 守るべき原則

1. **TIP は arbitrary process にロードされる in-proc DLL**。
   - 重い処理を直接書かない。`ipc/` 経由で `inference-host` に投げる。
   - グローバル状態を持たない。スレッドセーフを徹底(`std::atomic`、`std::mutex`)。
   - C++ 例外を COM 境界の外に漏らさない(`HRESULT` で返す)。
2. **CLSID / Profile GUID は不変**。既存の GUID を変更しない。
3. **`IUnknown` の参照カウントは厳密に管理**。`Microsoft::WRL::ComPtr` を使う。
4. **すべての COM メソッドは `STDMETHODIMP` 戻り値、`E_POINTER` 等の防御チェックを入れる**。

## 参照リソース

- `references/itf-interfaces.md` — 本プロジェクトで実装している ITf*** インターフェースの一覧と責務
- `references/sample-projects.md` — 参考になる OSS TSF IME 実装の一覧

## 補助ツール(マーケット品を活用)

- 仕様確認：context7 MCP 経由で <https://learn.microsoft.com/en-us/windows/win32/tsf/> を fetch
- TIP登録/解除の検証：PowerShell.MCP(共有コンソール)で対象ユーザーとして実行
- 実アプリでの入力検証：Windows-MCP の UI Automation

## やってはいけない

- TIP の DLL から直接ファイル I/O を行う(ホスト側に委譲)。
- `MessageBox` 等のモーダル UI を TIP から出す。
- C++/WinRT を `tsf-tip/` に持ち込む(古い COM ベースの TSF と相性が悪い)。
- `legacy/` の Swift 実装の挙動を「正解」として参照する(仕様は `docs/*-spec.md` に従う)。

## 新規 ITf*** インターフェース実装時の手順

1. `docs/` 配下に該当する仕様 md があるか確認。無ければ先に仕様を起こす。
2. インターフェース ID(IID)と必要なメソッドを最新仕様で確認(context7 経由)。
3. `tsf-tip/` にヘッダ＋実装を追加。`ComPtr` で受け取り、`HRESULT` で返す。
4. GoogleTest にユニットテスト(COM 境界を mock 化して呼び出し検証)。
5. 既存 TIP に `QueryInterface` 経路を追加。
````

**Codex CLI による `references/` 埋め込みタスク:**

- `itf-interfaces.md` … `grep -r "ITf" tsf-tip/` で実装中のインターフェースを抽出し、責務を1行ずつ列挙
- `sample-projects.md` … 以下を転記
  - `https://github.com/chewing/windows-chewing-tsf`
  - `https://github.com/fkunn1326/azooKey-Windows`
  - `https://github.com/MicrosoftDocs/win32/tree/docs/desktop-src/TSF`
  - `https://learn.microsoft.com/en-us/windows/win32/tsf/text-services-framework`

### 6.2 `.agents/skills/tsf-ipc-protocol/SKILL.md`

````markdown
---
name: tsf-ipc-protocol
description: ipc/ 配下の IPC 定義、TIP と inference-host 間の Named Pipe / JSON プロトコル、メッセージスキーマ、ハンドシェイク、エラー回復を扱うときに使用する。
---

# IPC プロトコル運用ガイド

## 基本仕様

- トランスポート：Windows Named Pipe(`\\.\pipe\azookey-*`)
- フレーミング：**4-byte little-endian length prefix + UTF-8 JSON payload**
- 双方向：TIP → Host(リクエスト)、Host → TIP(レスポンス + 非同期イベント)

詳細は `references/frame-format.md` を参照。

## 変更時に必ず守ること

1. **後方互換を壊す変更を禁止**。フィールド追加は許容、削除・型変更は要バージョン bump。
2. JSON スキーマを変更したら：
   - `ipc/` のヘッダ／シリアライザを更新
   - TIP 側(`tsf-tip/`)の呼び出しコードを更新
   - Host 側(`inference-host/`)のハンドラを更新
   - GoogleTest を追加／更新
   - `docs/` の関連仕様 md を更新
3. プロトコルバージョンフィールド(あれば)を必ず確認。

## ハンドシェイク失敗時の挙動

- TIP は **無入力フォールバック** に切り替える(変換せずパススルー)。
- Host プロセスが落ちている場合、TIP は再起動を試行(リトライ上限あり)。
- ユーザーに見えるエラー UI は出さない(TIP からモーダル禁止)。

## やってはいけない

- 既存メッセージ ID(type フィールド)の意味を変更する。
- length prefix を含めずに JSON だけ送る実装を書く。
- 同期ブロッキング呼び出しを TIP のメインスレッドで行う(必ず非同期化)。
````

**Codex CLI による `references/frame-format.md` 埋め込みタスク:**

- フレーム構造図(4-byte LE length + JSON body)
- 最大ペイロードサイズ(決まっていれば)
- 既知のメッセージ種別一覧(`ipc/` の実装を読んで抽出)
- バイトオーダーの根拠(Windows ネイティブが LE)

## 7. 作業フロー

1. ブランチを切る：`git switch -c chore/codex-bootstrap`
2. `.codex/config.toml` を作成(§4)
3. `AGENTS.md` を作成または追記(§5)
4. `.agents/skills/` 配下に2スキルを作成(§6)
   - `references/` 配下は実コードを `grep` / `view` で読んで埋める
5. `.gitignore` に **追加不要**。`.codex/config.toml` `.agents/skills/` `AGENTS.md` は
   全て共有が目的なのでコミット対象。**ただし**、開発者個別の `~/.codex/config.toml` は
   元から git 管理外なので追加不要。
6. 動作確認：
   - `codex mcp list` で `context7` / `powershell` / `windows-mcp` の3つが見える
     (環境によって `powershell` / `windows-mcp` は起動失敗するが構成上は見える)
   - Codex CLI を起動し、`/skills`(or `$` メンション)で `tsf-tip-development` と
     `tsf-ipc-protocol` の2つが見える
   - 何か TSF 関連の質問をして `tsf-tip-development` が自動的に発動するか確認
7. PR 作成：
   ```bash
   gh pr create \
     --repo dolquis/azooKey-Desktop \
     --base main \
     --head chore/codex-bootstrap \
     --draft \
     --title "chore(codex): bootstrap .codex/, AGENTS.md, .agents/skills/ for Windows port" \
     --body "Codex CLI setup: MCP servers (context7, powershell, windows-mcp) + custom skills (tsf-tip-development, tsf-ipc-protocol)."
   ```

## 8. Claude Code 版との関係 / 二重管理について

- 本リポジトリには既に Claude Code 用設定(`.claude/` `.mcp.json` `CLAUDE.md` 等)が
  存在する想定。それらは **触らず独立に維持** する。
- ただし、両方を有意に保つために以下のルールを `README.md` に追記することを推奨：
  - `tsf-tip-development` / `tsf-ipc-protocol` の SKILL.md を変更した場合は、
    `.claude/skills/` と `.agents/skills/` の両方を更新する(または将来的に
    シンボリックリンク化を検討する)。
  - 同様に `CLAUDE.md` と `AGENTS.md` のビルド・登録・bench セクションも両方更新する。
- 将来的に統合したくなったら、ハイブリッド戦略(B)に移行可能。
  実体を `.agents/skills/` 側に置き、`.claude/skills/` から symlink。
  Windows でのシンボリックリンク利用は `git config core.symlinks true` と
  Developer Mode が必要なので、移行時に注意。

## 9. 完了基準(DoD)

- [ ] `.codex/config.toml` がリポジトリ直下にあり、3 MCP サーバーが定義されている。
- [ ] `.codex/config.toml` にシークレットが直書きされていない。
- [ ] `AGENTS.md` がリポジトリ直下にあり、PR 規約・ビルド・TIP 登録・bench が記載されている。
- [ ] `.agents/skills/tsf-tip-development/SKILL.md` と `.agents/skills/tsf-ipc-protocol/SKILL.md`
      が作成され、実コードに即した `references/` も整備されている。
- [ ] `codex mcp list` で 3 MCP サーバーが認識されている。
- [ ] Codex CLI 起動時に `AGENTS.md` が読み込まれる(`/init` で出力が確認できる)。
- [ ] スキルの自動発動が確認できる(TSF 関連の質問で `tsf-tip-development` が発動)。
- [ ] Draft PR が `dolquis/azooKey-Desktop` 宛で作成されている。
- [ ] Claude Code 用ファイル(`.claude/` `.mcp.json` `CLAUDE.md`)が変更されていない。

## 10. 参考リソース

- Codex 公式 — AGENTS.md: <https://developers.openai.com/codex/guides/agents-md>
- Codex 公式 — Config Reference: <https://developers.openai.com/codex/config-reference>
- Codex 公式 — Skills: <https://developers.openai.com/codex/skills>
- Codex 公式 — MCP: <https://developers.openai.com/codex/mcp>
- Codex 公式 — Plugins: <https://developers.openai.com/codex/plugins>
- PowerShell.MCP: <https://github.com/yotsuda/PowerShell.MCP>
- Windows-MCP: <https://github.com/CursorTouch/Windows-MCP>
- TSF 公式: <https://learn.microsoft.com/en-us/windows/win32/tsf/text-services-framework>
