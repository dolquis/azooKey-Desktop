# 開発基盤・品質強化 仕様（Development Infrastructure & Quality）

本書は azooKey-Desktop（Windows ポート）の**開発基盤・ビルド再現性・CI 品質
ゲート・IPC/JSON 堅牢性・観測性・Host 可用性**に関する仕様を定める。対象は
Windows 版（C++ 移植: `core/` `ipc/` `learning/` `inference-host/` `tsf-tip/`
`bench/`）のみ。macOS 版 `legacy/` は対象外。

対応マイルストーン: `plans/windows-port-roadmap.md` M37〜M43（開発基盤・品質
強化トラック）。

本書は 2 通の第三者レビュー（開発環境・ライブラリ改善レビュー / プロジェクト
レビュー 2026-05-22）の指摘を評価・取捨選択した結果を仕様化したものである。
レビュー指摘のうち既存マイルストーン（M8/M11/M12/M28〜M34）と重複するもの、
本プロジェクトの方針と整合しないものは「§11 不採用とした提案」に理由とともに
記録する。

## 1. 目的と非目標

### 目的

MVP（Phase 1〜2 完了）から「実運用に耐える IME」へ進むにあたり、Phase 3
（Zenzai 統合）/ Phase 4（配布）着手前に開発基盤・品質の負債を解消する。
具体的には次の 4 点をボトルネックとみなし、マイルストーン化する。

1. ビルド再現性（手元・CI・AI エージェントでビルド入口が統一されていない）
2. IPC/JSON の境界堅牢性（自前 JSON パーサと Named Pipe 入力の事故耐性）
3. 観測性（遅延要因の切り分け、構造化ログ、エラーコード体系の不在）
4. Host 可用性（プロセス停止時の TIP 側再接続戦略が限定的）

### 非目標（本トラックでやらないこと）

- Zenzai 本実装の完成（M8 の範囲）
- CUDA / DirectML / NPU の実装（M24 の範囲）
- WinUI 設定アプリの完成（M11/M30 の範囲）
- MSIX/MSI 配布の完成（M11/M12/M28〜M32 の範囲）
- 自前 JSON パーサの即時全置換（§6・§11 参照）

### 横断する設計原則

- **オフラインビルド原則を維持する。** `cmake -S . -B build` がネットワーク
  アクセスなしで成功すること。ネットワーク取得は明示オプトイン
  （`AZOOKEY_FETCH_GOOGLETEST=ON` 等）に限る。本トラックの依存追加は
  この原則を崩してはならない（§3）。
- **依存最小主義。** header-only ライブラリを優先し、ビルドが必要な依存は
  導入価値を個別に評価する。
- **段階導入。** 警告強化・新ライブラリ導入は一括ではなく、新規コードから
  段階的に適用し、既存コードの大規模差分を避ける。

## 2. ビルド再現性（M37）

### 2.1 CMakePresets.json

ルートに `CMakePresets.json`（schema version 6）を追加し、configure /
build / test の入口を統一する。最低限のプリセット:

| プリセット | generator | `CMAKE_BUILD_TYPE` | 用途 |
|---|---|---|---|
| `windows-debug` | Ninja | Debug | 手元・CI Debug |
| `windows-release` | Ninja | Release | 手元・CI Release |

各 configure preset の `cacheVariables` は `AZOOKEY_BUILD_TESTS=ON`,
`AZOOKEY_BUILD_BENCH=ON` を既定とする。`AZOOKEY_FETCH_GOOGLETEST` は
**プリセット既定で ON にしない**（オフラインビルド原則。CI ジョブ側で
明示的に上書きする）。`binaryDir` は `${sourceDir}/build/<preset>`。

`CMakeUserPresets.json` は各自のローカル設定用とし、`.gitignore` に追加する
（§2.4）。

受け入れ条件:

```
cmake --preset windows-debug && cmake --build --preset windows-debug && ctest --preset windows-debug
cmake --preset windows-release && cmake --build --preset windows-release && ctest --preset windows-release
```

### 2.2 共通オプション INTERFACE target

ルート `CMakeLists.txt` で 2 つの `INTERFACE` ライブラリを定義し、コンパイル
オプションを一元管理する。

- `azookey_project_options` — C++ 標準（`cxx_std_17`）と MSVC ハードニング
  系オプション（`/utf-8` `/permissive-` `/EHsc` `/Zc:__cplusplus` `/sdl`）。
  各実体 target が `PUBLIC` でリンク。
- `azookey_project_warnings` — 警告レベル（MSVC `/W4`、非 MSVC
  `-Wall -Wextra -Wpedantic`）。各実体 target が `PRIVATE` でリンク。

`/W4` は既存コードで警告が大量に出る場合、target 単位で段階導入する
（新規モジュールから適用し、既存は警告解消とセットで切替）。TSF DLL / Host
/ IPC の致命的警告（C4715 制御パス、C4477 書式不一致 等）は本マイルストーン
内で解消する。

### 2.3 .clang-format

ルートに `.clang-format` を追加する。初期値は `BasedOnStyle: Google`,
`Standard: c++17`, `ColumnLimit: 100`, `IndentWidth: 2`,
`PointerAlignment: Left`, `DerivePointerAlignment: false`,
`SortIncludes: true`。

既存コードに大規模差分が出る場合、まず全体整形を独立 PR として分離し、
機能変更 PR と混ぜない。CI の `clang-format --dry-run --Werror` は全体整形
PR がマージされた後に有効化する（§4.3）。

### 2.4 .gitignore 整理

`.gitignore` から macOS/Xcode 専用エントリ（`*.dmg` `*.pkg` `*.app`
`DerivedData/` `*.xcodeproj/*` 等）は `legacy/` 保全のため削除しないが、
Windows/CMake 向けエントリを追記する:

```
CMakeUserPresets.json
out/
install/
compile_commands.json
.cache/
.idea/
*.ilk
*.pdb
*.exp
*.obj
*.tlog
```

`.pdb` は Git 管理しないが CI artifact としては保存する（§4.4）。`.vscode/`
はチーム共有設定の余地を残し、一括 ignore はしない。

### M37 受け入れ条件

- `cmake --preset windows-debug` / `windows-release` の configure→build→test
  が成功する
- 既存全 target が `azookey_project_options` / `azookey_project_warnings`
  をリンクした状態でビルドできる
- `.clang-format` がルートに存在し、`clang-format --dry-run` が新規追加
  コードに対して差分ゼロ
- `.gitignore` に Windows/CMake エントリが追加され、ビルド生成物が
  `git status` に現れない

## 3. 依存管理方針（M37 / 横断）

### 3.1 基本方針

レビューは `vcpkg.json` の一括導入を提案するが、本プロジェクトは
**オフラインビルド原則**（§1）を持つため、vcpkg を必須化しない。依存ごとに
次の優先順で導入手段を選ぶ。

1. header-only ライブラリ → submodule または `FetchContent`（オプトイン）
2. ビルドが必要な依存 → 導入価値を個別評価し、必要なら `FetchContent` を
   明示オプトインで

### 3.2 依存ごとの導入手段

| 依存 | 種別 | 導入手段 | 状態 |
|---|---|---|---|
| GoogleTest | テスト | `find_package` →（オプトイン時）`FetchContent` | 導入済み（現行方式を維持） |
| llama.cpp | 推論 | `FetchContent` / submodule、`AZOOKEY_BACKEND` で制御 | M8 で導入（本トラック対象外） |
| WIL | header-only | submodule または `FetchContent`（オプトイン） | M43 で導入（§9） |
| nlohmann-json | header-only | 任意・保留 | §6・§11 参照（即時導入しない） |
| spdlog | 要ビルド | 任意・保留 | §7・§11 参照（自前ロガーを優先評価） |

### 3.3 vcpkg.json の扱い

`vcpkg.json` は「vcpkg を使いたい開発者向けの利便ファイル」として将来追加
してよいが、**必須化しない**。追加する場合も CI の正系は preset ベースの
`FetchContent`/submodule 経路を維持し、README に「vcpkg は任意」と明記する。

## 4. CI 品質ゲート拡張（M38）

M38 着手前の `.github/workflows/windows.yml`（windows-latest + msvc-dev-cmd +
Ninja + Debug 単一構成、失敗時 PR コメント + test_report artifact）を基盤に、
以下を追加する。

### 4.1 Debug/Release マトリクス

```yaml
strategy:
  matrix:
    config: [Debug, Release]
```

runner は安定性のため `windows-2022` を明示する（`windows-latest` の暗黙
更新を避ける）。

### 4.2 preset 利用

CI の configure/build/test を §2.1 のプリセット経由に統一する。CI ジョブは
`AZOOKEY_FETCH_GOOGLETEST=ON` をジョブ側で明示指定する（プリセット既定は
OFF のまま）。

### 4.3 追加チェック

- `clang-format --dry-run --Werror`（§2.3 の全体整形 PR 後に有効化）
- Linux 補助ジョブ — `core` / `ipc` / `learning` / `inference-host` の
  非 Windows 依存部分のみをビルド・テストし、移植性回帰を早期検出する
- bench smoke — `azookey_bench` を CTest から exit=0 で実行（§4.5）
- `AZOOKEY_BUILD_TESTS=OFF` ビルドが壊れていないことの確認ジョブ

clang-tidy / CodeQL はオプション扱いとし、本マイルストーンの必須範囲には
含めない（導入コストが高く、段階導入とする）。

### 4.4 artifact 整理

configure / build / test の各ログと、Release ビルドの `.pdb` を artifact
として保存する。PR diagnostic コメントはマトリクスの config ごとの結果を
反映する。

### 4.5 bench smoke と回帰監視

`bench/azookey_bench` を CTest 登録し、CI で exit=0 と CPU `SimpleConverter`
経路の p95 < 50ms を確認する。p50/p95 レイテンシの推移監視（夜間ベンチ回帰）
は将来拡張とし、本マイルストーンでは smoke 実行までを範囲とする。

### M38 受け入れ条件

- Debug / Release 両構成が CI で緑
- CI が preset 経由で configure/build/test を実行する
- Linux 補助ジョブが非 Windows 部分のテストを実行する
- bench smoke が CTest 経由で実行され exit=0
- PR コメントが config ごとの結果に対応する

## 5. ユーザーデータ永続化の堅牢化（M39）

### 5.1 現状の問題

M39 着手前の `inference-host/src/main.cpp` は学習・辞書ファイルの既定パスを相対パス
（`azookey_learning.tsv` / `azookey_user_dict.json`）で持つため、Host の
起動元ディレクトリによって保存先が変わり、ユーザーデータが迷子になりやすい。

### 5.2 標準ディレクトリレイアウト

明示指定がない場合、`%LOCALAPPDATA%\azooKey\` 配下を既定とする:

```
%LOCALAPPDATA%\azooKey\
  config\   settings.json
  data\     learning.tsv / user_dict.json
  logs\     host-YYYYMMDD.jsonl / tip-YYYYMMDD.jsonl
  models\   zenzai\
```

`%LOCALAPPDATA%` は `SHGetKnownFolderPath(FOLDERID_LocalAppData, ...)` で
取得する（WIL 導入後は `wil::unique_cotaskmem_string` で受ける）。必要な
サブディレクトリは起動時に自動作成する。

### 5.3 パス解決規約

- `--learning` / `--user-dict` が指定された場合は、従来どおり**明示パスを
  優先**する（既存テスト・CI・開発フローを壊さない）。
- 指定がない場合のみ `%LOCALAPPDATA%\azooKey\data\...` を使う。
- 保存先決定ロジックは純粋関数として切り出し、unit test で
  「明示指定優先 / 既定パス生成 / 環境変数欠落時の挙動」を検証する。

### 5.4 原子的書き込み

学習・辞書ファイルの保存は「一時ファイルへ書き込み → flush →
`MoveFileEx`（`MOVEFILE_REPLACE_EXISTING` + `MOVEFILE_WRITE_THROUGH`）で
rename」で原子的に行い、書き込み中クラッシュによる破損を防ぐ。

### 5.5 本マイルストーンの範囲と将来課題

本マイルストーンの範囲は **保存先統一・ディレクトリ自動作成・原子的書き込み・
パス解決の unit test** まで。次の項目は将来課題として扱い、ここでは仕様化
しない（対応マイルストーンを明記）:

- ファイルロック（単一 writer 保証） — M39 で実装可だが、Host は単一
  プロセス前提のため多重起動検出として軽量に扱う
- 破損時のバックアップ世代・起動時検証 — 将来課題
- DPAPI 暗号化 — **M34** の範囲（重複実装しない）

### M39 受け入れ条件

- 明示指定なしで `%LOCALAPPDATA%\azooKey\data\learning.tsv` /
  `user_dict.json` が使われる
- `--learning` / `--user-dict` 指定時は指定パスが優先される
- 必要なディレクトリが自動作成される
- 保存が原子的（書き込み中クラッシュで既存ファイルが壊れない）
- 保存先決定ロジックの unit test が追加され緑
- 既存テストが回帰しない

## 6. IPC/JSON 堅牢化（M40）

### 6.1 方針

レビューは自前 JSON パーサ（`ipc/src/Json.cpp`）の `nlohmann-json` 置換
（提案 A）と、自前を残す場合の fuzz/malformed テスト追加（提案 B）を挙げる。
本仕様は**提案 B を主**とする。即時の全置換はしない（理由は §11）。自前
パーサを残したまま境界堅牢性を上げ、`nlohmann-json` 置換は将来判断とする。

### 6.2 JSON パーサ強化要件

`ipc/src/Json.cpp` に次の防御を追加する:

- **ネスト深度上限** — 配列/オブジェクトのネストに上限（例: 64）を設け、
  超過時はパース失敗を返す（スタック枯渇防止）
- **最大入力長** — パース対象バイト長に上限を設ける。length-prefix
  フレーミングの最大フレームサイズ（§6.4）と整合させる
- **サロゲートペア結合** — `\uXXXX` の上位/下位サロゲートを正しく結合し、
  単独サロゲートは不正として拒否する
- **不正 UTF-8 / 制御文字の拒否** — 文字列内の生の制御文字（U+0000〜
  U+001F）と不正 UTF-8 シーケンスを拒否する
- **末尾ゴミの拒否** — 値の後ろに非空白バイトが残る入力を拒否する
- **数値** — 現状 `std::stod` 経由。整数精度・巨大数の扱いを見直し、
  範囲外は安全に拒否する

### 6.3 追加テスト

`ipc/tests/` に malformed/fuzz 系テストを追加する:

- ランダムバイト列を `Parse` してもクラッシュせず失敗を返す
- 深すぎるネストを拒否する
- 巨大数・桁あふれを安全に拒否する
- 不正な Unicode escape（単独サロゲート等）を拒否する
- 文字列内の生制御文字を拒否する
- 末尾ゴミを拒否する
- 最大 payload 長超過入力を拒否する
- `Payloads.cpp` 側で期待外の型・必須キー欠損を安全に拒否する

### 6.4 Named Pipe セキュリティ強化

現行 `ipc/src/NamedPipeTransport.cpp`（per-user pipe 名 + DACL +
length-prefix フレーミング + `kMaxFrameSize`）を基盤に強化する:

- **6.4.1 Release で fail-closed** — SID 取得失敗時、現在は fallback pipe
  名 / 既定セキュリティに落ちる。Debug/test ではこの fallback を許可し、
  Release/production では SID 取得失敗時に Host 起動を失敗させる
  （ビルド構成で分岐）
- **6.4.2 接続インスタンス上限** — `PIPE_UNLIMITED_INSTANCES` をやめ、
  IME 用途に十分な上限（例: `kMaxPipeInstances = 4`）を設ける
- **6.4.3 最大フレームサイズ見直し** — M40 着手前は 16MB。IME の候補問い合わせ
  には大きすぎるため、用途に見合う上限（256KB〜1MB 程度）へ引き下げる。
  §6.2 の JSON 最大入力長と整合させる
- **6.4.4 Handshake トークン** — DACL は同一ユーザーまでしか絞れない。
  Host 起動時にランダムトークンを生成し、TIP との Handshake で検証する
  方式を導入し、同一ユーザー内の別プロセスからの接続を弾く
- **6.4.5 client cleanup** — 切断済み client が Stop まで保持される現状を
  見直し、切断検出時に解放する（長時間稼働でのリーク様の蓄積を防ぐ）

### M40 受け入れ条件

- 既存 `ipc_payloads_tests` / `ipc_named_pipe_transport_tests` が緑
- malformed JSON・ランダムバイト列でクラッシュしない
- ネスト深度・最大長超過を拒否する
- サロゲートペアを正しく結合し、単独サロゲートを拒否する
- Release ビルドで SID 取得失敗時に Host 起動が失敗する
- 複数接続・切断テストが追加され緑
- 切断済み client が解放される

## 7. 構造化ログと可観測性（M41）

### 7.1 現状

TIP は `OutputDebugStringA`（DebugView）、Host は stderr 中心。遅延要因
（TIP 側 / Pipe 側 / Host 側）の切り分けと、エラー分類の体系がない。

### 7.2 構造化ログ（JSON Lines）

TIP / Host とも JSON Lines 形式のログを出力する。出力先は §5.2 の
`%LOCALAPPDATA%\azooKey\logs\host-YYYYMMDD.jsonl` /
`tip-YYYYMMDD.jsonl`。

各行に最低限含める必須フィールド:

| フィールド | 内容 |
|---|---|
| `ts` | ISO 8601 タイムスタンプ |
| `level` | `info` / `warn` / `error` |
| `component` | `tip` / `host` / `ipc` |
| `request_id` | IPC リクエスト相関 ID（該当時） |
| `phase` | 処理フェーズ（§7.3） |
| `latency_ms` | 当該フェーズの所要時間（計測時） |
| `result` | `ok` / `error` / `cancelled` |
| `error_code` | §7.4 のエラーコード（error 時） |

記録対象イベント: Host 起動・終了、pipe listen 開始、model load 成否、
backend 選択、query latency、error、exception summary、learning/user-dict
の load/save 結果。

実装は自前の軽量 JSON Lines ロガーを第一候補とする（依存最小主義）。
`spdlog` 採用はビルド依存の増加に見合うかを評価したうえで判断し、本仕様の
必須範囲には含めない（§11）。

### 7.3 相関 ID とフェーズ

IPC リクエストに `request_id` を付与し（既存 `RequestScheduler` の連番を
流用）、TIP / IPC / Host のログを横断追跡できるようにする。フェーズは
`serialize` / `send` / `host_compute` / `recv` / `apply_ui` に分類し、
各フェーズの `latency_ms` を記録して遅延要因を切り分け可能にする。

### 7.4 エラーコード体系

エラーを 3 カテゴリの enum で固定する:

- `transport` — pipe 切断、接続失敗、フレーミング異常
- `protocol` — JSON パース失敗、未知 MessageType、payload 不整合
- `business` — モデル未ロード、変換失敗、辞書 I/O 失敗

### 7.5 タイムアウト規約

変換候補問い合わせのタイムアウトを定義する（例: ソフト 150ms / ハード
300ms）。ソフト超過はログに記録、ハード超過は当該リクエストを打ち切り
劣化モード（§8.3）へ移行する。

### 7.6 プライバシー配慮

IME である以上、入力本文・候補語をそのままログに出すとプライバシー
リスクがある。**本文・候補語のログ出力は Debug ビルド限定、または明示
設定 ON 時のみ**とする。既定（Release）では `request_id` ・長さ・結果
コードなどメタ情報のみを記録する。

### M41 受け入れ条件

- TIP / Host が JSON Lines ログを `%LOCALAPPDATA%\azooKey\logs\` に出力
- 各行に `request_id` / `phase` / `latency_ms` / `result` が含まれる
- エラーコードが 3 カテゴリ enum で固定される
- タイムアウト規約がコードとログに反映される
- Release ビルドで入力本文・候補語がログに出力されない

## 8. Host 可用性・再接続（M42）

### 8.1 現状の問題

Host が停止した際の TIP 側再接続戦略が限定的で、入力中に候補更新が停止する
恐れがある。常駐方法（Run キー）だけではアップデート時・クラッシュ時の復旧
が弱い。

### 8.2 接続状態機械

TIP 側 IPC ワーカー（`tsf-tip/src/TextService.cpp::IpcWorkerThread`）に
明示的な接続状態機械を導入する:

```
Disconnected ─→ Connecting ─→ Handshaking ─→ Ready
     ↑              │              │           │
     └──────────────┴──────────────┴───────────┘ (失敗・切断時)
Ready ─→ Degraded （ハードタイムアウト / 連続失敗時）
```

各状態遷移はログ（§7）に記録する。

### 8.3 再接続と劣化モード

- **Exponential backoff + jitter** で再接続を試行する（例:
  200ms→400ms→800ms→…→最大 5s、各値に jitter を加算）。
- Host から一定時間応答がない場合（§7.5 のハードタイムアウト、または
  ヘルス監視の無応答）、TIP は `Degraded` 状態へ移行し、`SimpleConverter`
  相当のローカルフォールバックで入力継続を保証する。Host 復帰後は
  `Ready` へ戻す。
- ヘルス監視は既存 `Health` メッセージを流用し、定期的に往復確認する。

### 8.4 本マイルストーンの範囲

接続状態機械・backoff 再接続・ヘルス監視・劣化フォールバックまでを範囲と
する。Host の Windows サービス化や監視ランチャ導入は将来課題とし、本仕様
では扱わない。

### M42 受け入れ条件

- TIP 側 IPC ワーカーが接続状態機械に基づいて動作する
- Host 停止 → 再起動で TIP が exponential backoff + jitter により自動
  再接続する
- Host 無応答時に TIP が劣化モードへ移行し、入力が止まらない
- 状態遷移がログに記録される
- Host 復帰後に `Ready` へ復帰する

## 9. WIL 段階導入（M43）

### 9.1 方針

Windows C++ では `HANDLE` / COM ポインタ / レジストリハンドル / `HRESULT`
/ `CoTaskMemFree` / `LocalFree` の管理が事故りやすい。Windows Implementation
Library（WIL、header-only）を導入し、RAII で安全化する。

WIL は header-only のため、vcpkg ではなく submodule または `FetchContent`
（オプトイン）で取り込む（§3.2）。

### 9.2 段階導入

- **新規コードは最初から WIL を使う**（`wil::unique_handle` /
  `wil::com_ptr` / `wil::unique_hlocal` / `wil::unique_cotaskmem_string`
  / `wil::unique_hkey` 等）。
- 既存の `NamedPipeTransport.cpp`（`SecurityDescriptor` 周り）、
  `tsf-tip` の COM/HANDLE/レジストリ処理は、一気に置き換えず段階的に
  置換する。M40（Named Pipe 強化）・M39（永続化）で当該ファイルを触る
  際に併せて WIL 化するのが効率的。

### M43 受け入れ条件

- WIL が header-only 依存として取り込まれ、オフラインビルドを壊さない
- 新規コードが WIL の RAII 型を使用する
- `tsf-tip` の COM / HANDLE / レジストリ処理の RAII 化が進む
- 既存テストが回帰しない

## 10. 成果測定 KPI

レビュー（プロジェクトレビュー 2026-05-22）が提案する KPI を、本トラックの
効果測定指標として記録する。これらは目標値であり、達成可否のゲートは各
マイルストーンの受け入れ条件側で判定する。

| KPI | 目標値 | 関連 |
|---|---|---|
| 候補表示レイテンシ | p50 < 80ms / p95 < 180ms | §4.5, §7.3 |
| Host 異常停止時の復旧時間 | 中央値 3 秒以内 | §8.3 |
| CI 成功率 | main 連続 14 日で > 95% | §4 |
| 学習永続化の破損率 | 0%（クラッシュ注入テストで確認） | §5.4 |

## 11. 不採用とした提案

レビュー指摘のうち、本トラックで採用しない / 修正して採用するものを理由と
ともに記録する。

### 11.1 vcpkg.json の一括導入 — 修正採用

レビューは `vcpkg.json` でビルド依存基盤を作ることを提案する。本プロジェクト
は**オフラインビルド原則**（§1）を持ち、`AZOOKEY_FETCH_GOOGLETEST` が意図的に
オプトインである。vcpkg の必須化はこの原則と衝突するため、必須化はしない。
依存ごとに header-only 優先・`FetchContent`/submodule オプトインで導入する
（§3）。`vcpkg.json` は任意の利便ファイルとしてのみ許容する。

### 11.2 自前 JSON パーサの即時全置換 — 修正採用

レビュー提案 A（`nlohmann-json` への即時全置換）は採らない。自前パーサは
軽量・低依存という利点があり、IPC の JSON は大きくない。提案 B（堅牢化 +
fuzz/malformed テスト）を主とし（§6）、置換は将来判断とする。

### 11.3 Zenzai runtime 抽象 `IModelRuntime` の新設 — 不採用

レビューは `IModelRuntime` インターフェース（`SimpleRuntime` /
`LlamaCpuRuntime` / `LlamaCudaRuntime` …）の新設を提案する。しかし
`core/IConverter` が既に同じ役割（変換バックエンドの差し替え可能境界）を
担い、M8 も Zenzai を `IConverter` 実装として `SimpleConverter` と差し替え
る方針が確定している。`IModelRuntime` 新設は二重抽象になるため不採用。
バックエンド別実装の内部構造は M8 / M24 の範囲で `IConverter` 実装として
整理する。

### 11.4 packaging / signing 足場の新設 — 不採用（重複）

レビューは packaging / signing の足場作りを提案するが、MSIX・WiX・署名 CI・
DPAPI 暗号化は既存マイルストーン M11 / M12 / M28〜M34 でカバー済みである。
重複するため本トラックでは新設しない。`scripts/register.ps1` /
`unregister.ps1` を「開発用」と明確化する小改善（命名分離の検討）は M28 の
残作業に追記する。

### 11.5 clang-tidy / CodeQL の必須化 — 見送り

CI への clang-tidy / CodeQL 追加は導入・調整コストが高い。本トラックでは
`clang-format` チェックまでを必須とし、clang-tidy / CodeQL は将来の任意
拡張とする（§4.3）。
