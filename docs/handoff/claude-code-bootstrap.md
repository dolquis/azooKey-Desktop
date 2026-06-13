# Claude Code 引き継ぎ：`.claude/` セットアップ（ハイブリッド版）

> このファイルは、`dolquis/azooKey-Desktop`（Windows版 IME、C++ / CMake / TSF）に
> Claude Code 用の共有設定を導入するための、Claude Code への作業指示書です。
>
> **方針：マーケット配布の MCP・プラグインを最大限活用し、自作スキルは
> プロジェクト固有でマーケットに代替が無い領域のみに限定する。**

---

## 0. 大前提（厳守）

- 本リポジトリは `azooKey/azooKey-Desktop` の **fork**。フォーク元には PR を出さない。
- PR は必ず **`dolquis/azooKey-Desktop` 宛・Draft** で作成する。
- `main` ブランチに直接 push しない。新規ブランチを切って作業する。
- `gh pr create` を使うときは `--repo dolquis/azooKey-Desktop --base main --draft` を必ず明示。
- `legacy/` 以下（旧 macOS 実装）は **絶対に編集しない**。参照のみ。

## 1. プロジェクト現状サマリー

- Windows 版に方針転換済み。macOS 実装は `legacy/` に保全（未保守）。
- 構成：
  - `tsf-tip/` — Text Services Framework TIP（in-proc COM DLL）
  - `inference-host/` — 推論ホスト（別プロセス、CPU、将来 CUDA）
  - `core/` — OS 非依存のかな漢字変換コア
  - `ipc/` — Named Pipe + JSON + length-prefix の IPC 定義
  - `learning/` — 頻度＋時間減衰の再ランキング
  - `bench/` — レイテンシ計測 CLI
  - `scripts/` — `register.ps1` / `unregister.ps1`(HKCU user-scope、elevation 不要)
- ビルド：Windows 10/11 + Visual Studio 2022(C++ デスクトップ)+ CMake ≥ 3.21 + Windows SDK
- テスト：CTest + GoogleTest(`-DAZOOKEY_FETCH_GOOGLETEST=ON` で FetchContent)
- 既存メタファイル：`CLAUDE.md`, `AGENTS.md` あり(役割を被らせない)

## 2. ゴール

リポジトリ直下に以下の3つを配置する。

```
.mcp.json                              ← マーケット/公開 MCP サーバー定義(共有)
.claude/
  settings.json                        ← 公式マーケット自動登録＋有効化プラグイン
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

**自作スキルを2個に絞り込んだ理由(前回案からの差分)**

| 前回案 | 今回どうするか | 理由 |
|---|---|---|
| `cmake-msvc-build` | **削除**。CLAUDE.md に標準コマンド3行追記で代替 | LSP(`clangd-lsp`)+ Bash で十分。マーケット品で代替可能 |
| `tsf-tip-development` | **残す**(自作)| TSF/COM/IUnknown はマーケットに該当スキルが存在しない |
| `tip-register-flow` | **削除**。PowerShell.MCP(共有コンソール)で代替 | 人間介在型の実行は MCP 側で安全に扱える |
| `tsf-ipc-protocol` | **残す**(自作)| Named Pipe + length-prefix の独自プロトコルはプロジェクト固有 |
| `bench-latency` | **削除**。CLAUDE.md に運用ルール3行追記で代替 | 通常の Bash 実行で十分、ナレッジ化するほどの手順は無い |

## 3. `.mcp.json` の作成

リポジトリ直下に作成し、コミットする。`${...}` で参照する環境変数は各開発者の
シェルで設定する運用とし、ファイル自体にシークレットは書かない。

```json
{
  "mcpServers": {
    "context7": {
      "type": "http",
      "url": "https://mcp.context7.com/mcp"
    },
    "powershell": {
      "type": "stdio",
      "command": "${POWERSHELL_MCP_PROXY}"
    },
    "windows-mcp": {
      "type": "stdio",
      "command": "uvx",
      "args": ["windows-mcp", "serve"]
    }
  }
}
```

**Claude Code が実装時に確認すべきこと：**

1. `powershell` / `windows-mcp` の `command` / `args` は、各 MCP サーバーの最新ドキュメントを
   `WebFetch` で確認して正確な起動方法に置き換えること：
   - PowerShell.MCP: <https://github.com/yotsuda/PowerShell.MCP>
   - Windows-MCP: <https://github.com/CursorTouch/Windows-MCP>
   - 上記サンプルでは PowerShell.MCP の `command` を環境変数
     `${POWERSHELL_MCP_PROXY}` で参照している。
     PowerShell.MCP は `pwsh -Command Start-McpServer` のような cmdlet ではなく
     `PowerShell.MCP.Proxy.exe` という proxy 実行ファイルを stdio で起動する仕様
     なので、各開発者は `Install-PSResource PowerShell.MCP` 後に
     `Get-MCPProxyPath` の戻り値を `POWERSHELL_MCP_PROXY` に永続化する。
2. macOS / Linux のメンテナがこのリポジトリを開いた場合、`powershell` / `windows-mcp` は
   起動失敗するが、それは想定動作(Windows 環境専用ツール)。`context7` だけは全 OS で動く。
3. シークレットを直接書かない。必要があれば `${ENV_VAR}` で参照。
4. 各 MCP サーバーは Windows ホスト側に別途インストールが必要。
   `README.md` の「推奨開発環境」セクションに以下の旨を追記する：
   - PowerShell 7+ と PowerShell.MCP のインストール
   - `uv`(`uvx` コマンド)のインストール
   - WSL から Claude Code を使う場合は `powershell.exe` 経由になる旨

## 4. `.claude/settings.json` の作成

公式マーケット(`claude-plugins-official`)を明示的に登録し、本プロジェクトで
使うプラグインを有効化リストとして列挙する。

```json
{
  "extraKnownMarketplaces": {
    "claude-plugins-official": {
      "source": {
        "source": "github",
        "repo": "anthropics/claude-plugins-official"
      }
    }
  },
  "enabledPlugins": {
    "clangd-lsp@claude-plugins-official": true,
    "github@claude-plugins-official": true,
    "commit-commands@claude-plugins-official": true,
    "pr-review-toolkit@claude-plugins-official": true
  }
}
```

**Claude Code が実装時に確認すべきこと：**

1. `enabledPlugins` の正確なスキーマ(キー名・配列要素の書式)を Claude Code 公式ドキュメント
   <https://code.claude.com/docs/en/plugin-marketplaces> で確認。本ドキュメントの記載は概念例なので、
   実フィールド名が `enabledPlugins` で正しいか・別名(`autoInstall` 等)が現行か必ず照合する。
2. 公式マーケットがデフォルトで利用可能な場合、`extraKnownMarketplaces` のエントリは
   省略可能なことがある。冗長になるなら省く。
3. 各プラグインが本リポジトリで有効に動くか、`/plugin install` 後に手動確認すること：
   - `clangd-lsp` … `core/` `tsf-tip/` 等で補完・エラー診断が出る
   - `github` … `gh` 経由の PR 作業がエージェントから可能
   - `commit-commands` … 規約に沿ったコミットメッセージ生成
   - `pr-review-toolkit` … PR レビュー補助
4. プラグインが提供する `clangd` 等はホスト側にインストールされている前提。Windows なら
   Visual Studio の C++ ワークロード or LLVM 公式インストーラ経由で `clangd.exe` を入れる旨を
   `README.md` の「推奨開発環境」セクションに追記すること。

## 4.5 clangd / Serena の C++ 解決（compile_commands.json）

`clangd-lsp` プラグインと Serena の cpp 言語サーバーは、いずれも内部で clangd を使う。
clangd は `compile_commands.json`（compile database）が無いと include パスを知らず、
`windows.h` / `msctf.h` / STL を解決できずに診断・シンボル解決が壊れる。本プロジェクトは
MSVC + Ninja だが、clangd 用には **clang-cl ベースの DB** を別ディレクトリに生成する。

```powershell
cmake -S . -B build/clangd -G Ninja `
  -DCMAKE_CXX_COMPILER=clang-cl `
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON `
  -DAZOOKEY_BUILD_TESTS=OFF -DAZOOKEY_BUILD_BENCH=OFF
```

- 通常の MSVC ビルド（`build/windows-debug`）とは別の `build/clangd` に出して互いを汚さない。
- コンパイラを **clang-cl** にする理由：clangd が MSVC / Windows SDK のシステムヘッダを
  自動検出でき、`--query-driver` 引数が不要になる（`cl.exe` 版 DB だと query-driver が必要で、
  Serena 経由では clangd に渡しづらい）。要 LLVM（`clang-cl` / `clangd`）。
- 配線はリポジトリ直下の **`.clangd`**（コミット済み）が担う：`CompilationDatabase: build/clangd`。
  `build/` は `.gitignore` 済みなので DB 実体は各開発者が上記コマンドで生成する。
- Serena/clangd-lsp がバンドルする clangd が古い（< Clang 20）場合、最新の MSVC STL（14.5x）が
  `STL1000` で弾くため、`.clangd` で `_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH` を定義し、併せて
  C++17 プロジェクトに無意味な `-Wc++98-compat` 系ノイズを抑制している（いずれも IntelliSense 限定で
  実ビルドには無関係）。

### Serena プロジェクト設定（`.serena/project.yml`、コミット済み）

Serena は `--project-from-cwd` で起動ディレクトリ（worktree を含む）を自動アクティブ化する。
このとき `.serena/project.yml` が無いと言語自動判定が `legacy/` の Swift 群（最多ファイル数）を
拾って cpp を見落とすため、設定をコミットして固定する。

- `languages: [cpp, json, markdown, powershell]` … swift は `legacy/` 専用なので含めない
  （SourceKit は Windows で不安定。除外で起動高速化＋孤児プロセス回避）。
- `ignored_paths: [legacy, .claude/worktrees]` … 非保守の Swift ツリーと入れ子 worktree を除外。
- `.serena/cache` と `project.local.yml` は `.serena/.gitignore` で無視（コミットしない）。

> 検証：`get_diagnostics_for_file tsf-tip/src/TextService.cpp` が空（エラー無し）になり、
> `tsf-tip/include/azookey/tsf/TextService.h` の `azookey/tsf/TextService` がメソッド付きで取得でき、
> `find_referencing_symbols` が他ファイル（例 `TextServiceFactory.cpp`）まで追えれば成功。

## 5. 自作スキル個別仕様

### 5.1 `tsf-tip-development`

**目的**：TSF Text Input Processor 実装の中核ルールと参照リソースを集約。
マーケットに COM/TSF 専用のスキルが存在しないため自作する。

````markdown
---
name: tsf-tip-development
description: tsf-tip/ 配下の C++ コード(ITfTextInputProcessor 実装、COM コンポーネント、TSF コールバック)を編集・追加・デバッグするときに使用する。COM / TSF / Win32 API の作業で自動起動する。
allowed-tools: Read, Edit, Grep, Glob, WebFetch
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

- 仕様確認：Context7 MCP 経由で <https://learn.microsoft.com/en-us/windows/win32/tsf/> を fetch
- 補完・診断：`clangd-lsp` プラグイン
- TIP登録/解除の検証：PowerShell.MCP(共有コンソール)で対象ユーザーとして実行
- 実アプリでの入力検証：Windows-MCP の UI Automation

## やってはいけない

- TIP の DLL から直接ファイル I/O を行う(ホスト側に委譲)。
- `MessageBox` 等のモーダル UI を TIP から出す。
- C++/WinRT を `tsf-tip/` に持ち込む(古い COM ベースの TSF と相性が悪い)。
- `legacy/` の Swift 実装の挙動を「正解」として参照する(仕様は `docs/*-spec.md` に従う)。

## 新規 ITf*** インターフェース実装時の手順

1. `docs/` 配下に該当する仕様 md があるか確認。無ければ先に仕様を起こす。
2. インターフェース ID(IID)と必要なメソッドを最新仕様で確認(Context7 経由)。
3. `tsf-tip/` にヘッダ＋実装を追加。`ComPtr` で受け取り、`HRESULT` で返す。
4. GoogleTest にユニットテスト(COM 境界を mock 化して呼び出し検証)。
5. 既存 TIP に `QueryInterface` 経路を追加。
````

**`references/` の埋め方(Claude Code 側で実施)：**

- `itf-interfaces.md` … `grep -r "ITf" tsf-tip/` で実装中のインターフェースを抽出し、責務を1行ずつ列挙
- `sample-projects.md` … 以下を転記
  - `https://github.com/chewing/windows-chewing-tsf`
  - `https://github.com/fkunn1326/azooKey-Windows`
  - `https://github.com/MicrosoftDocs/win32/tree/docs/desktop-src/TSF`
  - `https://learn.microsoft.com/en-us/windows/win32/tsf/text-services-framework`

### 5.2 `tsf-ipc-protocol`

**目的**：TIP ⇔ Inference Host の独自 IPC(Named Pipe + JSON length-prefix)の仕様と
変更時の同期ルールを明文化。独自プロトコルなのでマーケット代替不可。

````markdown
---
name: tsf-ipc-protocol
description: ipc/ 配下の IPC 定義、TIP と inference-host 間の Named Pipe / JSON プロトコル、メッセージスキーマ、ハンドシェイク、エラー回復を扱うときに使用する。
allowed-tools: Read, Edit, Grep, Glob
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

**`references/frame-format.md` の埋め方(Claude Code 側で実施)：**

- フレーム構造図(4-byte LE length + JSON body)
- 最大ペイロードサイズ(決まっていれば)
- 既知のメッセージ種別一覧(`ipc/` の実装を読んで抽出)
- バイトオーダーの根拠(Windows ネイティブが LE)

## 6. CLAUDE.md への追記(マーケット品で代替する内容)

前回案で自作スキルにしていた3項目は、CLAUDE.md に短く追記するだけで足りる。
**既存の CLAUDE.md を読み、矛盾しない場所に以下を追記する**：

````markdown
## ビルド(CMake + MSVC)

```bash
cmake --preset windows-debug -DAZOOKEY_FETCH_GOOGLETEST=ON
cmake --build --preset windows-debug
ctest --preset windows-debug --output-on-failure
```

`-DAZOOKEY_FETCH_GOOGLETEST=ON` は FetchContent で GoogleTest を取得する。
オフライン環境では `-OFF` でテストのみスキップしてビルドを通す。

## TIP 登録 / 解除(HKCU user-scope)

`scripts/register.ps1` / `unregister.ps1` は対象ユーザーの PowerShell で実行する。
**Claude Code が単独で実行を完了させてはならない**。PowerShell.MCP の共有コンソール経由で、
コマンド提示までに留めること。本リポジトリの `DllRegisterServer` と `register.ps1` は
user-scope (HKCU) に登録するため、失敗時は
`HKCU\Software\Classes\CLSID\{...}` と
`HKCU\Software\Microsoft\CTF\TIP\{...}` の登録状態を確認すること
(レビュー指摘で修正: 実装は HKLM ではなく HKCU を操作する)。

## レイテンシ計測

`core/` `learning/` `ipc/` を編集したら、Release ビルドで `build/windows-release/bench/azookey_bench.exe` を実行し、
変更前後の数値を PR 本文に貼る。Debug 数値は性能評価に使わない。
````

## 7. 作業フロー

1. ブランチを切る：`git switch -c chore/claude-bootstrap`
2. `.mcp.json` を作成(§3)
3. `.claude/settings.json` を作成(§4、スキーマを公式ドキュメントで再確認)
4. `.claude/skills/tsf-tip-development/` と `.claude/skills/tsf-ipc-protocol/` を作成(§5)
   - `references/` 配下は実コードを `grep` / `view` で読んで埋める
5. `CLAUDE.md` に §6 の3セクションを追記(既存と矛盾しないこと)
6. `.gitignore` に **追加不要**。`.mcp.json` `.claude/settings.json` `.claude/skills/` は
   全て共有が目的なのでコミット対象。
7. 動作確認：
   - `claude mcp list` で `context7`、`powershell`、`windows-mcp` の3つが見える
   - `/plugin marketplace list` で `claude-plugins-official` が見える
   - `/plugin list` で4プラグインが enabled になっている
8. PR 作成：
   ```bash
   gh pr create \
     --repo dolquis/azooKey-Desktop \
     --base main \
     --head chore/claude-bootstrap \
     --draft \
     --title "chore(claude): bootstrap MCP, plugins, and skills for Windows port" \
     --body "Hybrid setup: marketplace MCP/plugins + minimal custom skills (tsf-tip-development, tsf-ipc-protocol)."
   ```

## 8. 完了基準(DoD)

- [ ] `.mcp.json` がリポジトリ直下にあり、3 MCP サーバーが定義されている。
- [ ] `.mcp.json` にシークレットが含まれていない(`${ENV_VAR}` 参照のみ)。
- [ ] `.claude/settings.json` が公式マーケットを登録し、4プラグインを有効化している。
- [ ] `.claude/skills/tsf-tip-development/SKILL.md` と
      `.claude/skills/tsf-ipc-protocol/SKILL.md` が作成され、
      実コードに即した `references/` も整備されている。
- [ ] `CLAUDE.md` にビルド・TIP登録・bench の3セクションが追記されている。
- [ ] `claude mcp list` / `/plugin list` で全てが認識されている。
- [ ] Draft PR が `dolquis/azooKey-Desktop` 宛で作成されている。

## 9. 参考リソース

- Claude Code MCP: <https://code.claude.com/docs/en/mcp>
- Claude Code Plugin Marketplaces: <https://code.claude.com/docs/en/plugin-marketplaces>
- 公式マーケット: <https://github.com/anthropics/claude-plugins-official>
- コミュニティマーケット一覧: <https://claudemarketplaces.com/>
- PowerShell.MCP: <https://github.com/yotsuda/PowerShell.MCP>
- Windows-MCP: <https://github.com/CursorTouch/Windows-MCP>
- TSF 公式: <https://learn.microsoft.com/en-us/windows/win32/tsf/text-services-framework>
