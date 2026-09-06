# 開発基盤・品質強化 仕様（Development Infrastructure & Quality）

本書は azooKey-Desktop（Windows ポート）の**開発基盤・ビルド再現性・CI 品質
ゲート・IPC/JSON 堅牢性・観測性・Host 可用性**に関する仕様を定める。対象は
Windows 版（C++ 移植: `core/` `ipc/` `learning/` `inference-host/` `tsf-tip/`
`bench/`）のみ。macOS 版 `legacy/` は対象外。

対応マイルストーン: `plans/windows-port-roadmap.md` M37〜M43（開発基盤・品質
強化トラック） + M44/M47/M50/M51（同トラックの自然な延長）。

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
| `windows-clangd` | Ninja | Debug | clangd / Serena 用 compilation database 生成 |
| `windows-release` | Ninja | Release | 手元・CI Release |
| `windows-asan` | Ninja | Debug | Windows ASan 手動・定期実行 |
| `linux-asan-ubsan` | Ninja | Debug | Linux ASan + UBSan 手動・定期実行 |

各 configure preset の `cacheVariables` は `AZOOKEY_BUILD_TESTS=ON`,
`AZOOKEY_BUILD_BENCH=ON` を既定とする。`AZOOKEY_FETCH_GOOGLETEST` は
**プリセット既定で ON にしない**（オフラインビルド原則。CI ジョブ側で
明示的に上書きする）。`binaryDir` は `${sourceDir}/build/<preset>`。

`windows-clangd` は configure 専用の例外とする。
コミット済みの `.clangd` が参照するパスに合わせて `binaryDir` を `build/clangd` とし、
製品バイナリを生成しないため tests / bench を既定で OFF にする。
`just tidy windows-clangd` も同じ `build/clangd` の compilation database を使う。

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

- `azookey_project_options` — C++ 標準（`cxx_std_20`）と MSVC ハードニング
  系オプション（`/utf-8` `/permissive-` `/EHsc` `/Zc:__cplusplus` `/sdl`
  `/guard:cf` `/Qspectre`）。各実体 target が `PUBLIC` でリンクし、最終バイナリへ
  入る project 管理下の static library object にも CFG instrumentation を適用する。
  llama.cpp / ggml はこの target を参照しないため、Windows MSVC で取り込む
  subdirectory の `CMAKE_C_FLAGS` / `CMAKE_CXX_FLAGS` に同じ `/guard:cf` `/Qspectre` を
  スコープ限定で追加し、Host へ入る third-party object も計装する。
- `azookey_project_warnings` — 警告レベル（MSVC `/W4`、非 MSVC
  `-Wall -Wextra -Wpedantic`）。各実体 target が `PRIVATE` でリンク。
- `azookey_binary_hardening` — `azookey_tsf_tip.dll` と
  `azookey_inference_host.exe` だけが `PRIVATE` でリンクする。MSVC linker に
  `/GUARD:CF` `/DYNAMICBASE` `/NXCOMPAT` を渡し、x64 では加えて
  `/HIGHENTROPYVA` `/CETCOMPAT` を渡す。`/DEPENDENTLOADFLAG:0xB00` により静的 import の
  探索を DLL load directory、application directory、System32 に限定する。これは current
  directory と `PATH` を除外しつつ、MSI が TIP / Host と同じ `INSTALLFOLDER` に配置する
  app-local VC runtime、および MSVC ASan の隣接 runtime DLL を解決できる構成である。

`/Qspectre` は 2026-08-31 に MSVC Release の `azookey_bench` を各 30 回交互実行して
採用した。p95 中央値は 0.0018 ms から 0.0023 ms（+0.0005 ms）、p99 中央値は
0.0037 ms から 0.00405 ms（+0.00035 ms）であり、§4.5 の絶対ノイズ下限 0.05 ms を
下回った。

`/W4` は既存コードで警告が大量に出る場合、target 単位で段階導入する
（新規モジュールから適用し、既存は警告解消とセットで切替）。TSF DLL / Host
/ IPC の致命的警告（C4715 制御パス、C4477 書式不一致 等）は本マイルストーン
内で解消する。

### 2.3 .clang-format

ルートに `.clang-format` を追加する。初期値は `BasedOnStyle: Google`,
`Standard: c++20`, `ColumnLimit: 100`, `IndentWidth: 2`,
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

### 2.5 開発環境 doctor（`scripts/doctor.ps1`）

開発者・AI エージェントの環境差分（不足ツール・未初期化の MSVC dev shell・
未取得の依存など）を、ビルド失敗の原因調査より前に一覧化する診断スクリプトを
`scripts/doctor.ps1` として定義する。検査対象は PowerShell 7 / git / gh /
Visual Studio 2022 + MSVC dev shell / CMake / Ninja / Windows SDK / LLVM
（clang-format / clang-tidy / clangd）/ pre-commit 系ツール / `.mcp.json`・
`.codex/config.toml` などのエージェント設定 / CMakePresets・compile_commands の
有無とする。人間向け表形式に加え `--json` 出力を持つ。
compile_commands は `.clangd` が参照する `build/clangd/compile_commands.json` を検査し、
別のビルドディレクトリにある DB を代用しない。

これは**開発環境の診断であり、§12 の `azookey_diag.exe`（エンドユーザー向けの
IME ランタイム診断・修復ウィザード）とは別物**である。両者を混同しないこと。
本項はオンボーディング短縮・エージェント初動ミス削減を目的とする補助ツールで、
M37 受け入れ条件には含めず、導入は Linear で追跡する（2026-07 開発基盤ツール
導入 第2弾）。

### 2.6 実機検証パッケージ

`scripts/make-vm-verify-package.ps1` は Hyper-V Win11 VM へ持ち込む実機検証用
zip を生成する。
入力は CMake configure preset と出力先で、preset の既定値は
`windows-release` とする。

スクリプトは `CMakePresets.json` の `binaryDir` と `CMAKE_BUILD_TYPE` を解決し、
実 build directory の `CMakeCache.txt` と照合する。
さらに `azookey_tsf_tip` と `azookey_inference_host` の Ninja dry-run が
`no work to do` になることを確認する。
manifest の commit が同梱スクリプトと文書も一意に指すよう、tracked/untracked を
含む作業ツリーが clean であることも要求する。
成果物の欠落、build type の不一致、別 checkout の CMake cache、stale target、
未コミット変更のいずれかを検出した場合は、zip を生成せず非ゼロ終了する。
出力先は worktree 外、または `.gitignore` 対象の `build/` 配下とする。
それ以外の worktree 内へ出力すると、次回実行時の clean 判定が前回の成果物を
未追跡ファイルとして検出する。

zip のルートには次を置く。

- `azookey_tsf_tip.dll`
- `azookey_inference_host.exe`
- `register-dev.ps1`
- `unregister-dev.ps1`
- `host-supervisor.ps1`
- `AppContainerAcl.ps1`
- `verify-bootstrap.ps1`
- `dev32-verification-checklist.md`
- `manifest.json`

`AppContainerAcl.ps1` は `register-dev.ps1` と `unregister-dev.ps1` が dot-source
する実行時依存であるため、登録スクリプトと同じディレクトリへ置く。
`-MockDictionaryPath` の TSV は `data/`、`-ModelPath` の GGUF は `models/` に
追加する。
GGUF を追加する場合は、同じ build directory の `azookey_zenzai_bench.exe` も
zip ルートへ追加する。
生成前の Ninja dry-run では bench target も鮮度確認の対象にする。
`vc_redist.x64.exe` は `-RuntimeInstallerPath` が指定された場合だけ同梱する。
生成スクリプトは依存ファイルをネットワークから取得しない。

zip 名は `azookey-verify-<12桁 commit>-<preset>.zip` とする。
同じ出力ディレクトリへ `<zip basename>.manifest.json` も書き出す。
zip 内の `manifest.json` と外部 manifest の内容は同一で、schema v1 は次の
top-level field を持つ。

| field | 型 | 内容 |
|---|---|---|
| `schemaVersion` | integer | 固定値 `1` |
| `commit` | string | 40 桁の Git commit |
| `preset` | string | 入力した configure preset 名 |
| `buildType` | string | preset と cache で一致した `CMAKE_BUILD_TYPE` |
| `generatedAtUtc` | string | ISO 8601 UTC 生成時刻 |
| `files` | array | 同梱ファイルの `path`、`role`、`size`、`sha256` |

`files` は `manifest.json` 自身を含めない。
自己参照 hash を避け、同梱物だけを検証対象にするためである。
manifest は BOM なし UTF-8 で書き出す。
GGUF は再圧縮せず zip entry とし、2 GB を超える entry に対応できる
`System.IO.Compression` を使用する。

VM 側の `scripts/verify-bootstrap.ps1` は対話ユーザーの PowerShell で実行する。
同梱された場合だけ VC++ Redistributable を導入し、期待する TIP DLL の
machine-wide 登録、per-user host pipe、Microsoft Japanese IME の残存を確認する。
VC++ Redistributable と machine-wide 登録だけを UAC で昇格し、HKCU の自動起動設定と
per-user host は昇格前の対話ユーザーで構成する。
登録済みの DLL と稼働中の pipe を検出した場合は再登録と supervisor 起動を省く。
manifest の `gguf-model`、`llama-preflight`、`mock-dictionary` role は自動検出して
登録と host 起動へ渡す。

`-Json` 出力の schema v1 は次の top-level field を持つ。

| field | 型 | 内容 |
|---|---|---|
| `schemaVersion` | integer | 固定値 `1` |
| `generatedAtUtc` | string | ISO 8601 UTC 実行時刻 |
| `package` | object | manifest から読んだ `commit`、`preset`、`buildType` |
| `overallStatus` | string | `pass`、`warning`、`fail` |
| `actions` | object | runtime 導入、TIP 登録、supervisor 起動の実施有無 |
| `checks` | array | 固定 ID の事前確認結果 |

`checks[].status` は `pass`、`fail`、`manual_required`、
`not_applicable` のいずれかとする。
固定 ID は `vcRuntime`、`tipRegistration`、`inferenceHost`、
`microsoftIme`、`vmCheckpoint`、`debugView` である。
Hyper-V checkpoint の作成と DebugView の capture 設定は guest から自動化せず、
人間の確認結果を表す。

### 2.7 ビルド時間の内訳（実測）

本節は M37 の受け入れ条件ではなく、ビルド時間短縮策の当てどころを決めるための計測
記録である。短縮策としては §4.7 の sccache のみが入っており、コンパイル 1 回あたりの
コストがどこに出ているかは未計測だった。

#### 測定条件

| 項目 | 値 |
|---|---|
| コンパイラ | clang-cl（LLVM 23.1.0、`/clang:-ftime-trace`） |
| 構成 | Debug、Ninja、`-j 4` |
| 対象 | 本体ターゲットのみ（`AZOOKEY_BUILD_TESTS=OFF` / `AZOOKEY_BUILD_BENCH=OFF`）、55 TU |
| キャッシュ | `AZOOKEY_USE_COMPILER_CACHE=OFF`（実コンパイルを測るため） |

再現手順は、MSVC 環境を初期化したうえで次を実行し、生成される TU ごとの
`*.cpp.json` を集計する。

```powershell
cmake -S . -B build/timetrace -G Ninja -DCMAKE_BUILD_TYPE=Debug `
  -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl `
  "-DCMAKE_CXX_FLAGS=/DWIN32 /D_WINDOWS /GR /EHsc /clang:-ftime-trace" `
  -DAZOOKEY_USE_COMPILER_CACHE=OFF -DAZOOKEY_BUILD_TESTS=OFF `
  -DAZOOKEY_BUILD_BENCH=OFF -DAZOOKEY_FETCH_WIL=ON
cmake --build build/timetrace
```

`CMAKE_CXX_FLAGS` を指定すると CMake の MSVC 既定フラグを上書きするため、
`/DWIN32 /D_WINDOWS /GR /EHsc` を明示的に含める。省くと例外が無効化され
`cannot use 'try' with exceptions disabled` でビルドが落ちる。

集計には clang が出力する `Total Source`（`.cpp` の直接 include をルートとする解析
時間。TU あたり 0〜15 件）と `Total InstantiateFunction` / `Total InstantiateClass` を
使う。個別の `Source` イベントは `-ftime-trace-granularity` の既定 500us で出力から
落ちるため、`Total` 側を読む。

**この 2 つは排他ではなく、重なる累積タイマーである。** テンプレート実体化は
ヘッダの解析中に起きるため、実体化の時間はヘッダ展開の時間にも含まれる。したがって
以下の「ヘッダ展開」「テンプレート実体化」はいずれも **inclusive** な値であり、
足しても total にならない（下表の `C011_shortcut_routing.cpp` は 84.8% + 26.2% =
111.0%）。内訳の分割ではなく、それぞれ独立に「どれだけの時間がそこを通ったか」を
表す指標として読むこと。

`Source` 同士の入れ子による二重計上は無い。55 TU すべてで `Total Source` が
`Total Frontend` を超えないこと（最大比 0.990）、`Total Source` の count が TU あたり
0〜15 件（`.cpp` の直接 include 数）に留まることを確認している。

#### 上位 10 TU

| TU | total (ms) | frontend | backend | ヘッダ展開 (incl.) | テンプレート実体化 (incl.) | ヘッダ% | テンプレート% |
|---|---|---|---|---|---|---|---|
| `NamedPipeTransport.cpp` | 5315 | 5128 | 112 | 4479 | 533 | 84.3% | 10.0% |
| `Diagnostics.cpp` | 2711 | 2213 | 310 | 1236 | 630 | 45.6% | 23.2% |
| `TextService.cpp` | 2360 | 2140 | 138 | 1651 | 530 | 69.9% | 22.5% |
| `main.cpp` | 1961 | 1702 | 155 | 1255 | 516 | 64.0% | 26.3% |
| `CompatRunner.cpp` | 1960 | 1710 | 158 | 1310 | 496 | 66.8% | 25.3% |
| `ReportWriter.cpp` | 1812 | 1540 | 164 | 1377 | 446 | 76.0% | 24.6% |
| `Dispatcher.cpp` | 1811 | 1700 | 66 | 1170 | 424 | 64.6% | 23.4% |
| `UserDictCli.cpp` | 1790 | 1517 | 153 | 1299 | 474 | 72.5% | 26.5% |
| `C011_shortcut_routing.cpp` | 1763 | 1676 | 44 | 1494 | 462 | 84.8% | 26.2% |
| `TargetConfigLoader.cpp` | 1756 | 1577 | 102 | 1375 | 445 | 78.3% | 25.3% |

TU 別コンパイル時間の総和は 74.2 秒（並列実行のため wall-clock とは異なる）。
上位 10 TU が総和の 31.3% を占める。

#### 全体の内訳と読み取り

ビルド全体（55 TU）では **frontend 91.2%、ヘッダ展開 76.4%（inclusive）、
テンプレート実体化 22.2%（inclusive）**。後の 2 つは重なるため、合計は内訳にならない。
排他な分割は frontend と backend の間にだけ成り立つ。

* **コストはヘッダ展開に偏っている。** backend（最適化・コード生成）は 1 割に満たず、
  frontend の中でもヘッダの解析が支配的である。`NamedPipeTransport.cpp` は単一 TU で
  最も重く、その 84.3% がヘッダ展開である。
* したがって**当てどころは PCH** である。PCH はまさにヘッダ展開の繰り返しを削る手段で、
  この分布と一致する。
* **unity build の採否は、この計測だけでは決まらない（未確定）。** unity build が削るのは
  TU 数に比例する固定費だけではない。複数の `.cpp` を同じ TU へまとめると、include guard
  や `#pragma once` の効く共通ヘッダは unity batch 内で 1 度しか解析されない。つまり
  ここで支配項と測った「TU ごとに繰り返すヘッダ展開」そのものが削減対象になる。
  内訳から不採用を導くことはできないため、unity on/off の実測と、無名 namespace /
  マクロ衝突の保守コストとの比較が済むまで採否を保留する。
* 重量 TU は `tsf-tip` に限らない。`ipc` / `diagnostics` / `compat-test` にも同程度の
  ヘッダ展開コストが出ており、PCH の適用対象を `tsf-tip` と `settings-app` に限定する
  前提は、実測では支持されない。

#### この計測が示さないこと

* **絶対値を MSVC へ一般化できない。** 実際の CI と開発ループが使うのは `cl.exe` で
  あり、本計測は clang-cl のもの。内訳の傾向（ヘッダ展開が支配的）は構造的なもので
  移りやすいが、短縮幅の見積もりには使えない。
* **PCH の採否はこの計測だけでは決まらない。** §4.7 の sccache と PCH は相互作用し、
  MSVC の PCH（`/Yc` `/Yu`）を挟むとキャッシュのヒット率が落ちて正味で悪化しうる。
  採否は「PCH 有無 × sccache hit/miss」の 4 条件を MSVC で実測して決める（別課題）。
* **ヘッダ展開のうち「純粋な解析」と「実体化」の比率は出せない。** 上記のとおり
  両者は重なる指標であり、exclusive time は取っていない（個別イベントが粒度で
  落ちているため、算出には粒度を下げた再計測が要る）。「ヘッダ展開が支配的」と
  いう結論はこの制約の影響を受けないが、内訳をさらに割る用途には使えない。
* テスト・bench ターゲットと `settings-app` は対象外。Debug 構成のみ。

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
3. 外部から取得する prebuilt バイナリ / モデル（llama.cpp ランタイム・GGUF 等）は
   **供給源を固定し SHA256 等でハッシュ pin** する。先行実装 fkunn1326/azooKey-Windows が
   CI で個人 gist / 個人 fork の成果物をチェックサム無しで取得しているのは**反面教師**とし、
   自分側は採らない（サプライチェーン耐性）。

### 3.2 依存ごとの導入手段

| 依存 | 種別 | 導入手段 | 状態 |
|---|---|---|---|
| GoogleTest | テスト | `find_package` →（オプトイン時）`FetchContent` | 導入済み（現行方式を維持） |
| llama.cpp | 推論 | `FetchContent` / submodule、`AZOOKEY_BACKEND` で制御 | M8 で導入（本トラック対象外） |
| WIL | header-only | submodule または `FetchContent`（オプトイン） | M43 で導入（§9） |
| nlohmann-json | header-only | 任意・保留 | §6・§11 参照（即時導入しない） |
| spdlog | 要ビルド | 任意・保留 | §7・§11 参照（自前ロガーを優先評価） |
| miniz | 要ビルド（C ソース 2 ファイル） | vendored（既定） →（オプトイン時）`FetchContent`（`-DAZOOKEY_FETCH_MINIZ=ON`） | M49 で導入予定（`docs/learning-data-management-spec.md` §7 の ZIP backup で使用） |

### 3.3 vcpkg.json の扱い

`vcpkg.json` は「vcpkg を使いたい開発者向けの利便ファイル」として将来追加
してよいが、**必須化しない**。追加する場合も CI の正系は preset ベースの
`FetchContent`/submodule 経路を維持し、README に「vcpkg は任意」と明記する。

### 3.4 pin 済み依存の更新手順

§3.1 の pin は「固定して放置する」ことを意味しない。上流の修正を取り込む機会を
定期的に設けたうえで、取り込みの可否を毎回判断する。pin と放置を区別するのが
本節の目的である。

#### 対象と pin の所在

| 依存 | pin の所在 | pin の形式 |
|---|---|---|
| llama.cpp | `CMakeLists.txt` の `AZOOKEY_LLAMA_CPP_GIT_TAG` | フル SHA |
| WIL | `CMakeLists.txt` の `AZOOKEY_WIL_GIT_TAG` | フル SHA |
| GoogleTest | `CMakeLists.txt` の `FetchContent_Declare(googletest)` | リリースタグ |
| prebuilt バイナリ / GGUF モデル | 各取得経路（§3.1 の 3） | SHA256 |

**GoogleTest も本手順の対象**である。テスト専用の依存でも、更新を思いつきで
行えば CI が壊れた原因の切り分けが難しくなる。

pin 値の正典は `CMakeLists.txt` と各取得経路であり、本 spec や README へ**転記
しない**。転記すると二重管理になり、片側が必ず古くなる。

#### 更新 cadence

**四半期に 1 回**、上表のすべてを対象に上流の差分を確認する。加えて、次のいずれかが
起きた時点では cadence を待たずに確認する。

* 上流でセキュリティ修正が公表された
* 現行 pin で再現する不具合を追っており、上流に修正が入っている
* 新しいモデル形式・量子化形式のサポートが必要になった

確認の結果が「更新しない」であることも正当な結論とする。その場合も次項の比較
結果を残し、次回の確認が同じ調査のやり直しにならないようにする。

#### 比較項目

推論に関わる依存（llama.cpp / GGUF モデル）は、ビルドが通ることだけでは判断
しない。更新前後で次を比較する。

| 項目 | 測り方 | 判断 |
|---|---|---|
| モデルロード | 既存 GGUF が現行 loader でロードできるか | 失敗する場合は更新を保留するか、変換側の対応とセットで扱う |
| 代表プロンプト出力 | `docs/conversion-quality-benchmark-spec.md` のベンチ | 変換品質に回帰が無いこと |
| レイテンシ | bench の JSON 出力（§4.5 の回帰監視） | 既存のしきい値内に収まること |
| メモリ | 同 bench のピーク常駐 | 明確な増加があれば理由を特定する |
| ライセンス | 上流 LICENSE と `THIRD_PARTY_LICENSES` の差分 | 変化があれば `docs/licensing-policy.md` を同時に更新する |

GoogleTest と WIL は推論品質に影響しないため、比較は「CTest 全件が緑」と
「ライセンス差分の確認」に留めてよい。cadence と次項の独立 PR 規約は同じく適用する。

#### 独立 PR の規約と記録先

依存更新は**機能変更と混ぜず、依存ごとに独立した PR** として出す。回帰したときに
revert 対象を一意に決められるようにするためで、複数依存の更新を 1 本にまとめない。

**1 つ以上の依存を更新する回は、pin SHA・上流日付・モデル互換性の記録先を
その更新 PR の本文**とする。専用の台帳ファイルは作らない（pin 値そのものは
`CMakeLists.txt` が正典であり、台帳は必ず実体から乖離するため）。PR 本文には
最低限、次を記載する。

* 更新前後の pin 値（フル SHA またはタグ）
* 上流コミットの日付と、取り込む主な変更。セキュリティ修正の有無を明示する
* 上表の比較結果。推論依存では実測値を数値で載せる
* モデル互換性。既存 GGUF がそのまま使えるか、再変換が必要か

**どの依存も更新しない回（no-op）は、Linear の定期課題
`[Recurring] pin 済み依存の四半期レビュー — azooKey` へコメントする。** この回には
差分も更新 PR も存在せず、PR 本文を記録先にできない。書き残せなければ次回の確認が
同じ調査のやり直しになり、本節が避けようとしている「pin＝放置」に戻る。コメントには
確認日と対象ごとの現行 pin・上流の最新 commit / タグとその日付、更新しないと判断した
根拠を書く。

| 結果 | 記録先 |
|---|---|
| 1 つ以上の依存を更新する | その依存の更新 PR 本文 |
| どの依存も更新しない（no-op） | 上記の Linear 定期課題へコメント |

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

- Agent 指示予算 — root `AGENTS.md` の UTF-8 byte size は 12 KiB を目標、
  16 KiB を上限とする。目標超過は GitHub Actions annotation で可視化し、
  上限超過は非 0 終了で PR を停止する。機械利用時の `--json` は単一行を保つ。
  実装は `scripts/check_agent_instruction_size.py`。`dolquis/agent-ops` から
  ベンダリングしたコピーであり、閾値・終了コード・出力形式の正典は origin にある。
  この repo では編集せず、変更は origin で行って配布し直す。本節は azooKey が
  この検査を CI のどのジョブで走らせるかを定義する。
- `clang-format` 変更行ゲート（`cpp-format` ジョブ、`git-clang-format`）—
  PR の**変更行のみ**を必須チェックする。M37 受け入れ条件「`clang-format
  --dry-run` が新規追加コードに対して差分ゼロ」に対応する。既存負債（未整形の
  既存行）では PR を止めない。全体整形 PR とツリー全体の
  `clang-format --dry-run --Werror` 必須化は、既存差分が大きいため別作業として
  分離する（§2.3）
- Linux 補助ジョブ — `core` / `ipc` / `learning` / `inference-host` の
  非 Windows 依存部分のみをビルド・テストし、移植性回帰を早期検出する
- Linux coverage ジョブ — Clang 18 の source-based coverage で portable subset
  （`core` / `ipc` / `learning` / `inference-host`）の行・分岐カバレッジを測る。
  tests / `third_party` / `build` は集計から除外し、HTML・LCOV・JSON summary を
  `linux-llvm-coverage` artifact として 14 日保持する。初期段階では数値を可視化する
  だけで、目標値を下回ってもジョブを失敗させない。一方、profile が生成されない、
  またはレポート生成に失敗するなど計測基盤が壊れた場合はジョブを失敗させる。
  Windows 専用コードと Cobertura は Phase 2 の OpenCppCoverage 導入時に追加する
- bench smoke — `azookey_bench` を CTest から exit=0 で実行（§4.5）
- `AZOOKEY_BUILD_TESTS=OFF` ビルドが壊れていないことの確認ジョブ — Linux の
  移植対象に加え、Windows では `diagnostics` / `compat-test` / `settings-app` / `tsf-tip` を
  含む非テスト target を `windows-no-tests` preset で継続検証する
- dependency review — PR で追加・更新された依存だけを対象にし、既知の脆弱性が
  High または Critical の場合は必須チェックを失敗させる。結果は job summary に残し、
  PR コメントは投稿しないため `pull-requests: write` 権限を付与しない。GitHub の
  Dependency Graph と repository variable `DEPENDENCY_REVIEW_ENABLED=true` の有効化は
  Human Gate とする。未完了の間は job summary に前提不足を警告し、全 PR を失敗させない。
  有効化後は High / Critical の検出を必須ゲートとして扱う
- BinSkim binary hardening analysis — Windows Release の
  `azookey_tsf_tip.dll`、`azookey_inference_host.exe`、`azookey_settings.exe` を
  固定版 BinSkim で検査し、Pass を含む SARIF、`dumpbin /loadconfig`、要約を artifact と
  job summary に残す。TIP / Host は BinSkim `BA2008` の CFG Pass と
  `Dependent Load Flag 0B00` を確認する。BinSkim には `DependentLoadFlags` のルールが
  ないため、後者は `dumpbin` で機械判定する。署名は CI packaging 後に行うため
  `BA2022.SignSecurely` だけを設定ファイルで無効化し、他のルールは既定のまま維持する
- GitHub Actions supply-chain pin — 外部 Action の `uses:` はフル 40 桁 commit SHA へ
  固定し、対応するリリースタグを行末コメントに残す。Dependabot の
  `github-actions` ecosystem を週次実行し、更新をまとめた PR で SHA を追従する
- settings JSON Schema — `check-jsonschema==0.37.3` で
  `settings/mvp-settings.schema.json` の meta-schema 妥当性と、
  `settings/default-settings.sample.json` の schema 適合性を確認する
- PowerShell quality gate — `scripts/test-powershell-quality.ps1` から
  `PSScriptAnalyzer` と `Pester` を実行し、`scripts/register-dev.ps1` /
  `scripts/unregister-dev.ps1` / `compat-test/*.ps1` の静的解析と、実
  HKLM 書き込みを伴わない分岐・引数構築テストを行う。pre-commit は
  `language: system` でローカル導入済みモジュールを使い、CI は pinned
  module version を導入して同じ wrapper を呼ぶ

`settings-schema`、`powershell-quality`、`cpp-tidy` は、先行する `changes` ジョブが
対象ファイルの変更を検出した場合だけ起動する。
`settings-schema` は `settings/`、`powershell-quality` は PowerShell の script / module /
manifest、`cpp-tidy` は portable subset の C++ source を対象とする。
手動実行と `.github/workflows/windows.yml` 自体の変更では、変更判定を含む構成を検証するため
三つの専門ジョブをすべて起動する。

clang-tidy / CodeQL は**必須ゲートには含めない**（導入コストが高く、段階導入と
する）。ただし変更行 clang-tidy は `cpp-tidy` ジョブで advisory（`continue-on-error`、
非ブロッキング）として実行し、変更 C++ ソースの静的解析所見を可視化する。
所見の有無で PR を止めない（§11.5）。CodeQL は将来拡張のまま据え置く。
なお `clang-format`（必須）と `clang-tidy`（advisory）は独立ジョブ
（`cpp-format` / `cpp-tidy`）に分割し、GitHub Actions 上で並行実行して CI 全体の
所要時間を短縮する。加えてワークフロー全体に `concurrency` グループを設定し、
同一 PR で新しい push が来たら進行中の run を畳む（`main` への push は畳まない）。

BinSkim も既存所見を可視化する段階では **advisory**（`continue-on-error`、
非ブロッキング）とし、所見や解析基盤の一時障害で PR を止めない。必須ゲート化は
`azookey_settings.exe` を含む所見の整理と運用実績を得てから別途判断する（§11.5）。

### 4.4 artifact 整理

configure / build / test の各ログ、Release ビルドの `.pdb`、Release binary hardening の
SARIF・`dumpbin` 出力・要約、Linux coverage の HTML・LCOV・JSON summary を
artifact として保存する。PR diagnostic コメントは
マトリクスの config ごとの結果を反映する。Release 用 artifact の保持期間は 14 日間とする。

### 4.5 bench smoke と回帰監視

`bench/azookey_bench` を CTest 登録し、CI で exit=0 と CPU `SimpleConverter`
経路の p95 < 50ms を確認する。
`azookey_zenzai_bench` は llama.cpp 無効構成の mock smoke と、pin 済み実モデルを使う
llama.cpp 構成の smoke を CTest へ登録する。

両 bench は既存の人間向け text 出力を既定とし、次の共通オプションを追加する。

| option | 動作 |
|---|---|
| `--json` | 人間向けログを混ぜず、schema v1 の JSON だけを stdout へ出力する |
| `--output <path>` | stdout の既定形式を変えず、schema v1 の JSON を指定ファイルへ書き込む |
| `--baseline <path>` | 同じ bench と build config の schema v1 結果を読み、p95 と p99 を比較する |

`--output` の open または write に失敗した場合は、対象パスを含むエラーを stderr へ出し、
exit code `2` で終了する。
`azookey_zenzai_bench` で model が未指定のまま `--json` または `--output` を要求した場合も、
結果を生成できないことを stderr へ明示し、exit code `2` で終了する。
出力要求がない従来の text 実行は、引き続き `status=skipped` と exit code `0` を維持する。
`--max-p95-ms` の閾値超過は従来どおり exit code `1` とし、baseline warning は exit code を
変更しない。

schema v1 のトップレベル契約は次のとおりとする。

| field | 型 | 内容 |
|---|---|---|
| `schemaVersion` | integer | 固定値 `1` |
| `bench` | string | `azookey_bench` または `azookey_zenzai_bench` |
| `commit` | string | 計測対象の 40 桁 Git commit。ローカル build は build 時に HEAD を生成ヘッダーへ反映し、CI は configure 引数で SHA を固定する。取得不能時は `unknown` |
| `config` | string | CMake build config（`Debug` または `Release`）。取得不能時は `unknown` |
| `iterations` | integer | 計測回数 |
| `latencyMs` | object | millisecond 単位の `p50`、`p95`、`p99`、`max` |
| `threshold` | object | `maxP95Ms`（未指定時は `null`）と `passed` |
| `baseline` | object | 比較状態、比較元 commit、p95/p99 変化率、`warningPercent`、`minimumChangeMs`、warning 判定と理由 |
| `deadlineCutoffs` | object（任意） | `azookey_zenzai_bench` の各 iteration が deadline で打ち切られたかを表す `samples`、打ち切り回数 `count`、打ち切り率 `rate` |
| `decodePhases` | object（任意） | `azookey_zenzai_bench` の実モデル推論で取得した `prompt` と `beam` の decode 内訳 |

`deadlineCutoffs.samples` は計測 iteration ごとの boolean 配列で、deadline により
best-so-far で正常完了した iteration を `true` とする。
`count` は `true` の数、`rate` は `count` を `samples` の要素数で割った値である。

`decodePhases.prompt` と `decodePhases.beam` は、それぞれ `samples`、`tokens`、
`latencyMs` を持つ。`decodePhases.prompt` は、変換間の prompt 接頭辞キャッシュから
再利用した token 数の全標本合計 `reusedTokens` も持つ。
`samples` は phase 時間の標本数で、正常完了した計測 iteration 数と一致する。
`tokens` は全標本の decode token 数の合計である。
`latencyMs` は iteration ごとの phase 合計時間から求めた `p50`、`p95`、`p99`、`max` とする。
`decodePhases.beam.evaluations` は全標本の beam 評価回数の合計であり、`samples` とは母数が
異なる。beam 評価は `llama_decode` 呼び出しと 1 対 1 に対応しない（step 0 の空 token beam は
decode を呼ばず、複数 beam の評価が 1 回の decode にまとまる構成もある。
`zenzai-inference-spec` §9.2.3）。したがって `latencyMs` を `evaluations` で割って
1 decode あたりの時間を求めてはならない。
llama.cpp 無効構成の mock runtime と `azookey_bench` は phase 内訳を取得できないため、
`decodePhases` を出力しない。

baseline 比較は schema version、bench 名、build config が一致し、比較元の p95/p99 が
有限の正数である場合だけ行う。
`--baseline` 未指定時は `baseline.status=not_provided` とする。
比較不能な場合は `baseline.status` を `unavailable` または `incompatible` とし、
`baseline.warning=false` を維持する。
比較可能な場合は `baseline.status=compared` とする。
p95 または p99 が比較元から 10% を超えて悪化し、かつ絶対差が 0.05ms を超えたときに
`baseline.warning=true` とする。
`baseline.minimumChangeMs` はこの絶対ノイズ床を表し、短時間処理の timer / scheduler 揺らぎを
性能回帰として扱わないために用いる。
CTest は mock runtime と llama.cpp 実モデルの両構成で JSON schema を smoke 検証し、
model 未指定の出力要求がファイルを生成せず失敗することも確認する。

`.github/workflows/benchmarks.yml` は main push、日次 schedule、手動実行で Linux Release の
両 bench を実行する。
workflow は直前の成功 run から `benchmark-results` artifact を取得できる場合だけ baseline
比較を行い、悪化を GitHub Actions warning として表示する。
今回の結果は両 bench の JSON を含む `benchmark-results` artifact として 90 日保持する。
bench が閾値超過などで非ゼロ終了した場合も、生成済みの JSON を先に artifact として保存し、
その後で job を失敗させる。
初回実行、artifact 期限切れ、run 一覧取得または download の失敗は regression と判定せず、
当該 run の計測を継続する。

### 4.6 Sanitizer プリセットと定期実行（M38 範囲外）

`HANDLE` / COM / Named Pipe / UTF-8↔UTF-16 / 自前 JSON パーサ / 非同期 I/O を
扱う本実装では、通常の unit test では use-after-free・バッファ超過・未定義動作・
境界外アクセスを見落としやすい。これを早期検出するため、次の sanitizer プリセット
を定義する（本マイルストーン M38 の必須範囲には**含めない**）。
`.github/workflows/sanitizers.yml` の手動 dispatch と週次 schedule で実行し、
通常の PR Build workflow からは分離する。安定後の required 化は別途判断する。

| プリセット | 対象 | 内容 |
|---|---|---|
| `linux-asan-ubsan` | Linux ビルド可能サブセット | Clang AddressSanitizer + UndefinedBehaviorSanitizer + `ipc_json_fuzzer` |
| `windows-asan` | Windows でビルドできる全コンポーネント（`tsf-tip` を含む） | MSVC AddressSanitizer |

Linux preset の対象は §4.3 の Linux 補助ジョブと同じ `core` / `ipc` / `learning` /
`inference-host` である（`tsf-tip` は Windows 専用のため Linux では構成できない）。
Windows preset はこれに `tsf-tip` を加えた全体を対象とする（適用範囲は §4.6.1）。PR
では通常 Build のみを required とし、sanitizer は schedule / manual dispatch で
段階導入する。各ジョブは configure / build / CTest のログと
`Testing/Temporary` の診断ログを、成功・失敗にかかわらず artifact として 14 日間
保持する。Windows preset は x64 のみを対象とし、MSVC ASan と非互換な `/RTC1` と
incremental link を sanitizer 設定内で無効化する。
GoogleTest は同じ ASan オプションでソースからビルドし、テスト検出前に ASan runtime
DLL を各実行ファイルのディレクトリへ配置する。Linux preset は Clang を使い、leak
detection と未定義動作検出を fail-fast で有効化する。CTest の後に `ipc/testdata/` を
seed corpus として `ipc_json_fuzzer` を 60 秒実行し、クラッシュ検体を診断 artifact に
含める。この実行は週次 schedule と手動 dispatch に限り、PR の required check にはしない。

#### 4.6.1 `tsf-tip` に対する ASan の適用範囲

`tsf-tip` は MSVC ASan の対象に含める（`AZOOKEY_BUILD_TSF_TIP=ON`）。
検査が届くのは次の 2 形態であり、どちらもプロセスを azooKey 側が生成するため、ASan
runtime（`clang_rt.asan_dynamic-x86_64.dll`）の探索経路を保証できる。

- `tsf-tip/tests/` の単体テスト実行ファイル。`add_tsf_tip_unit_test` が TIP の実装
  ソースをテスト実行ファイルへ直接取り込むため、DLL のロードを経ずに TIP のロジック
  が ASan 計装下で動く。
- `tsf_tip_com_smoke_tests`。ASan 計装済みの `azookey_tsf_tip.dll` を、同じく ASan
  計装済みのテスト実行ファイルへ `LoadLibraryW` で in-proc ロードする。実 DLL の
  `DllGetClassObject` から `TextService` の生成と解放までが検査対象になる。

**azooKey が制御しないホストアプリケーションのプロセスは、ASan の対象にしない。**
ASan を有効にした DLL は ASan runtime の解決を要求するが、ロード側プロセスの探索
経路をこちらから保証できない。加えて配布する成果物は非 ASan ビルドであり、実ホスト
上に載るバイナリが ASan 計装済みになることはない。これは段階導入の途中経過ではなく
恒久的な境界であり、この範囲は §4.6.2 の手段で埋める。

#### 4.6.2 実ホストプロセスでの実行時検証

実ホストアプリケーションへ in-proc ロードした状態のヒープ、ハンドル、ロックの検証
には Application Verifier を用いる。有効化と解除の手順は
`docs/handoff/windows-diagnostics-playbook.md` が正典である。

これを CI や `compat-test` の定常実行へ組み込むことはしない。同 playbook は一般利用
中のアプリや第三者プロセスへ verifier、page heap、debugger を設定しないと定めており、
`compat-test` が駆動するのは Notepad / Edge / VS Code だからである。実施は使い捨て
VM 上の人間ゲートセッションに限り、対象アプリと解除手順は
`docs/handoff/human-gate-batch-runbook.md` に置く。

### 4.7 コンパイラキャッシュ（sccache）

CI のコンパイルジョブ（`windows-build` Debug/Release、`windows-llama-build`、
`linux-build`）は `mozilla-actions/sccache-action` で sccache を導入し、GitHub
Actions のキャッシュサービス（`SCCACHE_GHA_ENABLED`）でオブジェクトを run 間再利用
する。ルート `CMakeLists.txt` が sccache を PATH 上で自動検出して compiler launcher
に設定するため（`AZOOKEY_USE_COMPILER_CACHE` 既定 ON）、ワークフロー側の追加設定は
不要。MSVC ではキャッシュ互換のため Debug / RelWithDebInfo のデバッグ情報を `/Z7`
（Embedded）へ切り替える。この切り替えは Release を対象外とするため、Release の
`.pdb` artifact（§4.4）は影響を受けない。各ジョブ末尾で `sccache --show-stats` で
ヒット率を可視化する。

Windows の Ninja build では、ルート `CMakeLists.txt` が compiler launcher の先頭に
`cmake -E env VSLANG=1033` を追加する。
CMake が生成する英語の `/showIncludes` prefix と MSVC の出力言語を一致させ、Ninja が
header dependency を記録できるようにするためである。
この環境指定は compiler cache の有効・無効にかかわらず適用し、sccache などの既存
launcher は環境指定の後段へ保持する。

**action のバージョンに注意（過去の失敗の教訓）。** 初回導入時に
`sccache-action@v0.0.6` を pin したところ、全ビルドジョブが失敗した（2026-07）。
このタグは旧 (v1) Actions Cache API 用の env（`ACTIONS_CACHE_URL`）しか渡さないが、
GitHub は v1 を **2025-04-15 に完全停止**しており、廃止済みエンドポイントが HTTP 400
（`Our services aren't available right now`）を返して sccache サーバ起動が失敗、
compiler launcher に設定された sccache が全コンパイルを道連れにした。現行 sccache は
v2 サービスの env（`ACTIONS_RESULTS_URL`）を要求するため、**action は v2 を繋ぐ
`v0.0.9` 以降（本 repo は `v0.0.10`）を pin する**こと。あわせて
`SCCACHE_IGNORE_SERVER_IO_ERROR=1` を設定し、実行時にキャッシュ backend が失敗しても
ローカルコンパイルへ fallback してビルドを緑に保つ（キャッシュはビルドを壊しては
ならない）。

### 4.8 CTest の並列実行

通常の Debug / Release CTest は 4 ケースを上限に並列実行する。
`CMakePresets.json` の通常 test preset を並列度の正典とし、`just test` と CI は
test preset の設定をそのまま引き継ぐ。
`azookey_check` は `AZOOKEY_CTEST_PARALLEL_JOBS`（既定値 4）を使う。
固定値にするのは、ローカルと CI の論理プロセッサ数が異なっても同じ負荷条件で
テストできるようにするためである。
ASan / UBSan preset はメモリ負荷を実測していないため、当面は並列設定を継承せず
直列で実行する。

共有状態を使うテストだけは `RESOURCE_LOCK` で相互排他にする。
RuntimeLogger のテストは component 名で決まる OS mutex を共有するため、
`azookey-runtime-log` を使う。
TSF/COM registration smoke は登録状態を変更するため、
`azookey-tsf-com-registration` を使う。
ケースごとに一意な一時パスや PID 付き Named Pipe を使うテストは直列化しない。
並列実行で timeout が生じてもケース単位の `TIMEOUT` は緩和せず、共有状態の有無と
並列度を先に見直す。

### 4.9 ブランチ保護と必須チェック

`main` は GitHub の branch ruleset で保護する。`AGENTS.md` が運用ルールとして定める
「`main` へ直接 push しない」「PR は Draft から始める」のうち、前者を設定として担保する。

ruleset の対象は既定ブランチ（`main`）とし、bypass list は置かない。適用する規則は次のとおり。

| 規則 | 設定 |
|---|---|
| Restrict deletions | 有効 |
| Block force pushes | 有効 |
| Require a pull request before merging | 有効。required approvals は 0 |
| Require status checks to pass | 有効。`Require branches to be up to date` は無効 |
| Require linear history | 無効 |
| Require signed commits | 無効 |

required approvals を 0 とするのは、単独オーナーが自分の PR を承認できないためである。
1 以上にすると全 PR が merge 不能になる。したがって本 ruleset が強制するのは「PR を経由すること」
までであり、人間の承認そのものは強制しない。

`Require branches to be up to date` を無効とするのは、Windows ビルドが 45 分を要し、
`main` へ merge するたびに全 PR の再実行が発生するためである。

`Require linear history` を無効とするのは、`AGENTS.md` が通常のマージ方法をノーマルマージと
定めているためである。

`Require signed commits` を無効とするのは、有効化するとローカルおよびエージェント経由の push に
GPG / SSH 署名の設定が必要になるためである。GitHub の web merge が生成するコミットは GitHub の鍵で
署名され Verified として扱われるので、web merge 経由の履歴について署名の欠落は生じない。

#### required checks の選定基準

**required とするジョブは、ワークフローの `paths` / `paths-ignore` によらず、すべての PR で
必ず check run を生成するものに限る。** 発火しないワークフローのジョブは check run 自体が
生成されず、required に指定した PR は待機状態のまま merge できなくなる。`if:` で skip された
ジョブは skipped として報告されるため、この問題は起きない。

この基準を満たす required checks は次のとおり。

| check 名 | ワークフロー |
|---|---|
| `Secret scan` | `.github/workflows/secret-scan.yml` |

`.github/workflows/windows.yml` はワークフローレベルの `paths-ignore` で docs 変更を除外するため、
その配下のジョブ（`Windows Debug` / `Windows Release` / `Pre-commit` / `C++ format (changed-lines gate)` /
`Dependency review`）はこの基準を満たさない。同ファイル冒頭のコメントが定めるとおり、
required checks へ加えるには `paths-ignore` を `changes` ジョブの出力による条件分岐へ移し、
`if: always()` で全 `needs` の結果を集約する終端ジョブを設ける必要がある（DEV-1000）。
`.github/workflows/docs.yml` と `.github/workflows/sbom.yml` も `paths` で絞られるため対象外とする。
`.github/workflows/compat.yml` の `Notepad / VS Code / Edge` はラベル付与時のみ実行する
対話ジョブであり、required にしても判定に寄与しない。

### M38 受け入れ条件

- Debug / Release 両構成が CI で緑
- CI が preset 経由で configure/build/test を実行する
- Linux 補助ジョブが非 Windows 部分のテストを実行する
- bench smoke が CTest 経由で実行され exit=0
- PR コメントが config ごとの結果に対応する

## 5. ユーザーデータ永続化の堅牢化（M39）

### 5.1 解決する問題

M39 着手前の `inference-host/src/main.cpp` は学習・辞書ファイルの既定パスを相対パス
（`azookey_learning.tsv` / `azookey_user_dict.json`）で持つため、Host の
起動元ディレクトリによって保存先が変わり、ユーザーデータが迷子になりやすい。

### 5.2 標準ディレクトリレイアウト

明示指定がない場合、`%LOCALAPPDATA%\azooKey\` 配下を既定とする:

```
%LOCALAPPDATA%\azooKey\
  config\   settings.json
  data\     learning.tsv / user_dict.json / typo_corrections.tsv（M35）/
            auto_words.tsv（M36-A）
  logs\     host-YYYYMMDD.jsonl / tip-YYYYMMDD.jsonl
  models\   zenzai\
```

注: `typo_corrections.tsv`（M35）と `auto_words.tsv`（M36-A）は
`learning.tsv` / `user_dict.json` と同じ `data\` 配下に置く（`UserDataPaths`
の `data_dir` 規約に合わせる）。M49 backup（`docs/learning-data-management-spec.md`
§2）の対象範囲は本レイアウトを正典とする。

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

Host は SIGINT / SIGTERM と Windows の CTRL+C / CTRL+Break / コンソール終了を
停止要求として処理し、`InferenceEngine` のデストラクタが未保存の学習観測を
flush してから終了する。
Windows のコンソール終了ハンドラは停止要求だけを通知し、通常の終了処理が
完了するまで OS の制限時間内で待機する。
コンソールウィンドウの終了（`CTRL_CLOSE_EVENT`）は OS の制限時間が有限で
ConPTY では更に短くなりうるため、この経路の最終 flush は best-effort である。
保証する終了経路と失われうる最大量の正典は
`docs/learning-data-management-spec.md` §11 に置く。

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
パーサを残したまま境界堅牢性を上げる。外部ライブラリへの移行は v1.0 の範囲では
行わないことを §11.2.1 で確定した。再評価の条件も同節が正典とする。

### 6.2 JSON パーサ強化要件

`ipc/src/Json.cpp` は次の防御を備える。各上限値は**本節（spec）を正典**とする
（AGENTS.md「対象と正典」: IPC payload / JSON schema / 挙動の決定は
`docs/*-spec.md`）。`ipc/include/azookey/ipc/Limits.h` の定数は本節の値に
一致させ、齟齬が出た場合は spec を正として Limits.h を修正する。コード側の
上限変更は wire contract と受け入れ条件に影響するため、spec 更新（レビュー）と
セットで行うこと。定数と spec 値の同期は `static_assert` / テストで担保する
（code-only な変更で wire contract が無断で変わる drift を防ぐ）。

- **ネスト深度上限 = 64**（`kMaxJsonNestDepth`） — 配列/オブジェクトのネストが
  64 を超えたらパース失敗を返す（再帰によるスタック枯渇防止）。`ParseValue` は
  depth を持ち回り、`ParseObject` / `ParseArray` が超過を検査する。
- **最大入力長 = 1 MiB**（`kMaxJsonInputBytes = 1024 * 1024`） — `ParseDocument`
  冒頭で入力バイト長を検査し、超過は即失敗。§6.4.3 の最大フレームサイズと同値で
  揃える（フレームを読み切れた時点で入力長上限も満たされる）。
- **サロゲートペア結合** — `\uXXXX` の上位（U+D800–U+DBFF）に続く下位
  （U+DC00–U+DFFF）を結合し、単独サロゲート（上位のみ / 下位のみ）は不正として
  拒否する。
- **不正 UTF-8 / 制御文字の拒否** — 文字列内の生の制御文字（U+0000–U+001F）を
  拒否し、生 UTF-8 バイト列は長さ・継続バイト・overlong（C0/C1、E0<A0、F0<90）・
  範囲外（F4>8F、> U+10FFFF）を検査して不正シーケンスを拒否する。
- **末尾ゴミの拒否** — 値の後ろに空白以外が残る入力を拒否する
  （`ParseDocument` が `pos_ == size` を要求）。
- **数値の安全な扱い** — `0` 始まりの多桁・小数点後桁なし・指数部桁なし等の
  不正形を拒否し、`1e9999` 等は `std::isfinite` で弾く。
- **uint64 精度** — **plain 整数 token**（符号なし数字のみ）の抽出
  （`GetInt` / `GetUInt`）は元トークン文字列から `std::stoll` /
  `std::stoull` で復元し、double 経由の精度欠落を避ける。
  `uint64_t` 全域 `18446744073709551615` まで正しく取り出せ、桁あふれは
  `nullopt` とする。小数・指数形などの非 plain 数値形は double 経路に入るため、
  `int64_t` / `uint64_t` の変換前に exclusive upper bound を検査し、範囲外 double
  を整数へキャストしてはならない。直列化側は `request_id` / `Ping.nonce` /
  `t_ms` / `Cancel.target_request_id` / `CommitObservation.timestamp_ms` 等の
  uint64 フィールドを token 保持版 `Value(uint64_t)` に渡し、2^53 超の値でも
  wire 上で丸めない。
- **数値 codec の locale 非依存** — 数値の parse / stringify は
  `std::from_chars` / `std::to_chars` で行い、ホストプロセス（TIP は任意アプリ内
  in-proc、Host も CRT locale を変える可能性）が非 C ロケール（小数点が `,` 等）
  を設定しても wire 表現を壊さない。overflow は `nullopt` で拒否し、
  underflow は JSON 数値として受け入れて 0.0（符号付きゼロを含む）へ丸める。

### 6.3 追加テストと協定外メッセージの扱い

`ipc/tests/` に境界・malformed テストを置く。JSON パーサ単体の境界は
`json_test.cpp` の `JsonTest` スイートで扱い、Envelope と length-prefix framing は
`messages_test.cpp` に置く。v1.0 の堅牢性バーは「**決定的な境界コーパス** + **有界な擬似乱数
スモーク**」で満たす。

探索的検証には `ipc_json_fuzzer` を使う。`AZOOKEY_BUILD_FUZZERS` は既定 `OFF` とし、
Clang と libFuzzer を利用できる構成でのみ有効化する。ハーネスは任意バイト列を
`ipc::json::Parse` へ渡し、`linux-asan-ubsan` の週次 schedule と手動 dispatch で
`ipc/testdata/` を seed corpus として 60 秒実行する。通常のオフラインビルド、MSVC
ビルド、PR の required check には含めない。

クラッシュ検体は sanitizer workflow の artifact に保存する。検体を再現して最小化した後、
JSON parser の入力なら `ipc/tests/json_test.cpp`、Envelope または frame の入力なら
`ipc/tests/messages_test.cpp` へ決定的な回帰ケースとして追加する。回帰テストが単独で失敗し、
修正後に通ることを確認してから、同じ検体を seed corpus に残すかを判断する。

- 深すぎるネスト・最大長超過を拒否する（`kMaxJsonNestDepth + 1` /
  `kMaxJsonInputBytes + 1`）
- 末尾ゴミ・`0` 始まり・指数部欠落を拒否する
- 巨大数（`1e9999`）・uint64 桁あふれ（`...616`）を安全に拒否する
- 不正 Unicode escape（単独サロゲート `\uD800` / `\uDC00`）を拒否する
- 文字列内の生制御文字（`\x01`）・overlong UTF-8（`C0 AF`、`E0 80 80`）を拒否する
- 擬似乱数バイト列を `Parse` してもクラッシュせず失敗を返す（有界回数スモーク）
- 自前 JSON パーサの直接単体テスト（int64/uint64 精度・深度・unicode escape・
  round-trip）を `ipc_json_tests`（`json_test.cpp`）に置く
- 最大フレーム超過の length-prefix を `Decode/EncodeLengthPrefixed` が拒否する
- `Payloads.cpp` 側で必須キー欠損・型不一致を安全に拒否する

**enum 予約のみで未配線の MessageType**（`QueryPredictions` / `QueryCorrections` /
`CommitCorrection` / `UpdateUserWord`。`docs/windows-tsf-host-architecture.md` の
⚠️ 項）を受信した際、Dispatcher は黙って無視せず**明示的なエラー応答**を返す。
応答待ちの blocking client をハングさせないため、既存ワイヤ形式の envelope に
「未対応 type」を表す `{"ok":false,"error":"unsupported_message_type"}` を載せ、
request の `type`、`request_id`、`trace_id` を応答 envelope に維持する。
`Cancel` など fire-and-forget の type のみ無応答を許す。
認証済み Dispatcher の未配線 type と `Unknown` はこの応答へ統一する（DEV-266）。
併せて MessageType 列挙 ↔ `Payloads` codec の網羅
整合をテスト / CI で検査し、列挙追加時の codec 取りこぼしを防ぐ。

### 6.4 Named Pipe セキュリティ強化

現行 `ipc/src/NamedPipeTransport.cpp`（per-user pipe 名 + DACL +
length-prefix フレーミング + `kMaxFrameSize`）を基盤に強化する:

- **6.4.1 Release で fail-closed** — SID 取得失敗時、現在は fallback pipe
  名 / 既定セキュリティに落ちる。Debug/test ではこの fallback を許可し、
  Release/production では SID 取得失敗時に Host 起動を失敗させる
  （ビルド構成で分岐）。Debug/test の restricted-token 実行環境では current-user
  の logon SID ACE だけでは同一プロセス client の write open が拒否されるため、
  Release 以外に限り restricted-token 互換 ACE を追加して transport tests を
  実行可能にする
- **6.4.1a remote client rejection** — Named Pipe はローカル IME ↔ Host 専用
  であり、server 作成時に `PIPE_REJECT_REMOTE_CLIENTS` を指定して remote client
  接続を OS レベルで拒否する。これは DACL / per-user pipe 名の補助防御であり、
  本項は remote client 拒否に限る。**同一ユーザー内の別プロセスを秘密だけで
  区別する認証は不可能**であり（§6.4.4）、Handshake トークンは同一ユーザー
  なりすまし対策の認証ではなく多層防御として §6.4.4 で扱う
- **6.4.1b logon 境界と pipe 名の占有** — DACL の許可先には user SID ではなく
  logon SID を使い、同じ Windows アカウントの別ログオンセッションを拒否する。
  許可権限は `GENERIC_ALL` ではなく、duplex pipe の read/write と属性操作、
  複数 instance 作成に必要な個別権限へ限定する。`FILE_APPEND_DATA` と
  `FILE_CREATE_PIPE_INSTANCE` は同じ値であり、現行の複数 instance server では
  後者を除外できない。この残余権限に対して、最初の instance に
  `FILE_FLAG_FIRST_PIPE_INSTANCE` を指定し、先行プロセスが同名 pipe を作成済みなら
  Host 起動を失敗させる。接続後は server と client の双方が OS の pipe peer PID
  から user SID と logon SID の一致を確認し、不一致ならその接続を閉じる。
  client は `SECURITY_IDENTIFICATION` を指定して pipe を開き、偽 server が
  client のセキュリティコンテキストを impersonate できないようにする
- **6.4.1c pipe DACL の対応アプリ範囲（AppContainer 非対応）** — pipe DACL
  （`ipc/src/NamedPipeTransport.cpp`）には capability ACE
  （`ALL APPLICATION PACKAGES` / パッケージ SID）を**意図的に含めない**。
  AppContainer トークンは dual-principal であり、許可は user / logon SID と
  Package / Capability SID の intersection で決まる。logon SID しか許可しない
  現行 DACL では intersection が空になり、**AppContainer プロセスに in-proc
  ロードされた TIP は pipe へ接続できない**（`ERROR_ACCESS_DENIED`）。preedit は
  TIP 内ローカルで出るが、Host 経由の候補は来ない。
  これは実装の欠落ではなく**スコープ判断**である。MVP が入力先として保証するのは
  Win32 デスクトップアプリであり、パッケージ化された UWP / Microsoft Store アプリは
  対象外と確定している（`docs/sideload-packaging-spec.md` §0.1 / DEV-783）。
  除外の境界は AppContainer を使うかどうかではなくパッケージ化された UWP / Store
  アプリかどうかで引くため、**Edge は対象内**である（renderer は AppContainer だが、
  入力欄をホストするのは Win32 プロセス）。
  capability ACE の設計と、それに伴う接続元の信頼境界の見直しは、Store / UWP 入力を
  実装する v1.0 以降のトラックで扱う（DEV-555）。**MVP では追加しない。**
  なお §6.4.4 の Handshake トークンも、格納先 `%LOCALAPPDATA%` を読めない
  AppContainer プロセスを結果的に排除する。本項の DACL による排除とは独立した
  別レイヤであり、どちらか一方を緩めても AppContainer 入力は成立しない
- **6.4.2 接続インスタンス上限** — `PIPE_UNLIMITED_INSTANCES` を使わず、
  TIP と設定 UI などの同時接続を許容する有界な上限
  (`kMaxPipeInstances = 32`) を設ける
- **6.4.3 最大フレームサイズ = 1 MiB** — `kMaxFrameSize`（`= kMaxJsonInputBytes`、
  `Limits.h`）。4-byte little-endian length-prefix の値が 0 または本上限超過の
  フレームは `ReadEnvelope` / `DecodeLengthPrefixed` が拒否する。候補問い合わせ
  （数 KB）には十分で、長文一括変換（5,000 文字 ≒ 15 KB）も収まる。5 万文字級の
  超長文は分割前提（M58-B、`docs/romaji-batch-conversion-spec.md` と整合）。
  §6.2 の JSON 最大入力長と**同値に固定**し、片方だけ広げない
- **6.4.4 Handshake トークン** — Handshake で共有秘密トークンを検証する
  （`HandshakeRequest.handshake_token` ↔ Host の
  `DispatcherConfig.handshake_token`）。トークンが設定されている場合、Host は
  Handshake 成立まで他メッセージを未認証として type 別エラーで弾く
  （`Dispatcher::RequiresAuthenticatedSession`）。

  **脅威モデルと到達限界（重要）**: 本トークンは**多層防御（defense-in-depth）**で
  あり、**悪意ある同一ユーザープロセスからのなりすましを防ぐ認証ではない**。
  TIP は TSF DLL として任意のアプリプロセスに in-proc ロードされるため、Host と
  「正規の TIP プロセス」を結ぶ事前の信頼チャネルが存在しない。共有秘密を
  どのチャネルで配っても、同一ユーザー権限で動く悪意あるプロセスは正規 TIP と
  同じ経路でその秘密を入手でき（ファイルなら読み取り、env なら環境継承）、同一
  ユーザー内のプロセス同士を秘密だけで区別することは Windows のセキュリティ
  モデル上できない。**同一ユーザー境界の主防御は per-user pipe ACL + SID
  fail-closed（§6.4.1）+ remote 拒否（§6.4.1a）**であり、「どのプロセスが正規 TIP か」
  の信頼境界は TSF 自体が担う。トークンに認証を期待しない。

  **トークンが実際に与える価値**:
  - 非悪意の誤接続（バージョン違い / 別ビルドの Host・TIP / 残存プロセス）を弾く
  - トークン格納先（`%LOCALAPPDATA%`）を**読めない**同一ユーザープロセス、
    具体的には **capability 分離された AppContainer プロセス**（ユーザー通常 SID で
    `%LOCALAPPDATA%` を読めず、パッケージ SID / capability で隔離。DEV-204 関連）を
    結果的に排除する。**低 IL（low integrity）プロセスは排除できない** — Windows の
    整合性レベルは通常 write-up のみ制限し read-up は許すため、低 IL でも同一ユーザー
    DACL のトークンファイルを読めてしまう。この価値は AppContainer 等の capability
    隔離ケースに限る
  - 攻撃の手数を一段増やす speed bump

  **トークンの配布チャネル（v1.0 決定）**: env 事前共有（`AZOOKEY_IPC_HANDSHAKE_TOKEN`）
  は in-proc TIP が Host 起動時の環境を共有しないため production では成立しない。
  そこで Host が起動時に暗号論的乱数 16 byte（hex 32 文字）を生成し、現在ユーザー
  のみが読める `%LOCALAPPDATA%\azooKey\config\ipc-token`（NTFS ACL 継承で
  current-user RX）へ §5.4 の write-then-rename で原子的に書き出す。TIP は Handshake
  直前に同ファイルを読みトークンを得る。Host 再起動時はファイルを新トークンで
  上書きし、TIP は**再 Handshake のたびにファイルを読み直す**（M42 再接続時も同様）。
  環境変数 `AZOOKEY_IPC_HANDSHAKE_TOKEN` は開発・テスト用の上書き経路として残す。
  本チャネルは上記の defense-in-depth 価値に見合うものであり、認証の保証は与えない。

  **トークン未設定時の縮退**: ファイルも環境変数も無い場合は per-logon pipe ACL
  のみに依拠し、Host は warn ログを出す（現行 `inference-host/src/main.cpp` の
  挙動）。Release では §6.4.1 の SID fail-closed により pipe 自体が current logon
  に限定されるため、トークン未設定でも remote / 別ユーザー / 別 logon session は
  到達しない。
  トークンが設定されている場合の比較は、値の一致位置を処理時間へ反映しない
  定数時間比較を使う。
- **6.4.5 client cleanup** — 切断済み client が Stop まで保持される現状を
  見直し、切断検出時に解放する（長時間稼働でのリーク様の蓄積を防ぐ）
- **6.4.6 length-prefix read/write hardening** — Named Pipe は
  `PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT` を使う。server は
  `FILE_FLAG_OVERLAPPED` 付きで instance を作成し、`ConnectNamedPipe` の
  pending accept を `OVERLAPPED` event で待つ。`Stop()` 時は accept / client I/O
  を cancel して、listen 中の thread が無期限に残らないようにする。4-byte
  little-endian length-prefix によってフレーム境界を復元し、`ERROR_MORE_DATA`
  を扱って指定長まで読み切る。フレーム途中の一時的な `ERROR_NO_DATA` は bounded
  retry に留め、フレーム開始前の no-data / zero-byte read は切断扱いにする。
  write 側も `ERROR_PIPE_BUSY` / zero-byte write を bounded retry に留め、
  読まない peer による client thread 滞留を防ぐため blocking flush に依存しない。
  64KiB を超えるフレームが pipe write 単位で分割されても往復できる。
  ただし retry 回数の上限だけでは、pend したまま進まない I/O を縛れない。
  時間の上限は §6.4.7 が担う
- **6.4.7 フレームデッドライン** — retry 回数とは別に、**転送が始まった 1
  フレーム**に monotonic な時間上限を課す。

  **計時の開始点**: 接続が要求と要求の間で idle している間は計時しない
  （TIP は打鍵の合間に分単位で idle し得るため、idle に上限を置くと正常な接続を
  誤って切る）。最初の 1 chunk が 1 byte 以上を転送した時点で arm し、以降は
  ヘッダと本文を通じた 1 つの絶対デッドラインとして働く。chunk より細かい粒度は
  観測できない（pending I/O の内部進行は取得できない）が、絶対デッドラインで
  あるため本文を細切れに送り続ける peer も縛れる。

  **値**: ソフト 500ms。ハードは read 2000ms / write 5000ms。write を緩くするのは、
  64KiB の pipe buffer が埋まった後は読まない peer が正当に write を止め得るため
  （TIP がモーダルループ中・デバッガ attach 中など、悪意なしに起こる）。ローカル
  Named Pipe の 1 MiB 転送は健全な peer なら ms オーダーであり、いずれも十分な余裕を持つ。

  **ハード超過時**: 当該接続のみを切断する。部分データからの再開は行わない
  （キャンセルした message-mode read の残りは pipe に残留し、フレーム同期を
  回復する手段がないため）。他のクライアントには波及しない。

  **ソフト超過時**: フレームは破棄せず、遅い接続として計上するだけに留める
  （`NamedPipeServer::SoftDeadlineExceededCount()`）。M41 の構造化ログへ配線するまでの
  暫定形であり、閾値に観測可能な効果を持たせて回帰テスト可能にすることが目的。

  **§8.5.2 の timeout 表との関係**: 別レイヤであり、値を一致させない。
  §8.5.2 は「要求を出してから応答が返るまで」（推論時間を含む）を縛る request
  レイヤの規約で、本項は「動き始めた 1 フレームを転送し切る」ことだけを縛る
  transport レイヤの規約である。推論に要した時間は本デッドラインに算入されない
  （write の計時は handler が応答を返した後に始まる）。ただし**両者が同時に効く
  場面（client の受信）では短いほうが勝つ**。本項のデッドラインが §8.5.2 の
  wall-clock deadline を延長することはない（優先関係は §6.4.8）。

  **適用範囲**: server 側と `NamedPipeClient` の双方に適用する。どちらも
  `FILE_FLAG_OVERLAPPED` 付きのハンドルを使い、本項の計時規約（idle は計時せず、
  最初の 1 byte で arm し、ハード超過で当該接続を切る）を共有する。client 側は
  受信 API が引数でタイムアウトを取るため、その引数と本項のデッドラインの
  役割分担を §6.4.8 で定める。

  **残余リスクと評価結果（DEV-644 で確定 / 2026-08）**: 何も送らない idle 接続を
  `kMaxPipeInstances`（32）本張って枯渇させる DoS は本項では防げない。評価の結論は
  **対処しない**である。理由は 2 つある。第一に、これは同一ユーザー権限で動くプロセスを
  前提とし、§6.4.4 の脅威モデルでは per-user pipe ACL の主防御対象外に置いた範囲そのもの
  である。第二に、idle に上限を置かない判断は本項の計時規約と同じ根拠に立つ。TIP は打鍵の
  合間に分単位で idle し得るため、idle を縛る keepalive デッドラインは正常接続の誤切断
  リスクだけを増やす。

  将来この判断を覆すなら、**接続後 Handshake 未了の猶予**（例 5s で切断）を第一候補とする。
  §6.4.4 のトークンが未設定でも Handshake 自体は必須であり、idle 全体ではなく「まだ
  Handshake していない接続」だけを縛れるため、正常な idle を誤って切らない。
  一方、`kMaxPipeInstances` の一定数を Handshake 済み接続に予約する案は採らない。
  未 Handshake 枠が埋まれば正規 TIP の新規接続も同じように阻まれ、枯渇の窓が狭まるだけで
  塞がらないためである。
#### 6.4.8 client 受信の二相セマンティクスと deadline の優先関係

`NamedPipeClient` の受信は性質の異なる 2 つの待ちを 1 回の呼び出しに含む。
相ごとに上限を分けるが、**どちらの相も request レイヤ（§8.5.2）の
wall-clock deadline を超えて待ってはならない**。

**スライスと request deadline は別物**: TIP の応答待ちループは
50ms 程度のスライスで受信 API を繰り返し呼びながら、その外側で
150ms（QueryCandidates fast）のような絶対 deadline を持つ
（`tsf-tip/src/TextService.cpp`）。スライス幅は Cancel 転送や停止確認を
差し挟むための刻みであって、要求 1 件の許容時間ではない。したがって
受信 API は **スライス幅と request の残デッドラインを別の引数として受け取る**
（1 引数形は「両者が等しい単発呼び出し」と定義する。実装形は DEV-744）。
呼び出し側が絶対 deadline を持たない場合は「なし」を渡せるものとし、その場合の
相 B は read ハードのみで縛る。§8.5.2 に値を持つ要求（Ping・QueryCandidates・
Live・Heavy）と M58-B の `T_sub` は、いずれも request deadline として渡す。

**相 A（フレーム到着待ち）**: 1 byte も届いていない間の待ち。上限はスライス幅。
超過時は `nullopt` を返し、接続は維持し、pipe からは何も取り出さない。
呼び出し側は同じ応答を次の呼び出しで受け取れる。相 A のタイムアウトは異常では
なく通常の制御フローであり、`docs/romaji-batch-conversion-spec.md` の
stale 応答 drain も「タイムアウトは応答を消費しない」というこの性質に依存する。

**相 B（フレーム転送中）**: 最初の 1 byte が到着した時点で相 B に入る。上限は
**`min(request の残デッドライン, §6.4.7 の read ハード)`**。スライス幅では
縛らない — 1 MiB までのフレーム（`kMaxFrameSize`）が 64KiB の chunk
（`kPipeBufferSize`）に分割されて届き得るため、スライス境界とフレーム境界は
一致せず、スライスで打ち切ると健全な応答を運んでいる接続を切ることになる。
一方 request の残デッドラインで縛るのは打ち切って正しい: その時刻を過ぎた
応答は §8.5.2 の契約上すでに失敗であり、呼び出し側は Cancel と fallback へ
進む。read ハードは「壊れた peer に対する上限」であって request deadline の
延長を許す根拠にはならない。

**相 B の超過**: §6.4.7 と同じく当該接続を切断して `nullopt` を返す
（以後 `IsConnected()` は false）。部分的に読み出したフレームからは同期を
回復できないため、接続を維持したままの再開はしない。**したがって request
deadline 超過のうち、フレームが転送途中だった場合だけは接続断（→ M42 の
再接続 / 劣化モード）を伴う。** 送信が始まってもいない遅い Host は相 A で
タイムアウトするため接続は維持され、従来どおり stale drain で処理できる。

**有界性**: 受信 1 回の所要は `min(request の残デッドライン, read ハード)` を
上限に有界。request deadline を渡さない呼び出しでも read ハードで有界。

**タイムアウトを取らない API**: `Receive()` は相 A を無期限に待ち（idle な
接続は正常）、相 B は §6.4.7 の read ハードデッドラインで縛る。`Send()` は
同項の write ハードデッドラインで縛る。

### M40 受け入れ条件

- 既存 `ipc_payloads_tests` / `ipc_named_pipe_transport_tests` が緑
- malformed JSON・ランダムバイト列でクラッシュしない
- ネスト深度・最大長超過を拒否する
- サロゲートペアを正しく結合し、単独サロゲートを拒否する
- Release ビルドで SID 取得失敗時に Host 起動が失敗する
- 複数接続・切断テストが追加され緑
- 切断済み client が解放される
- 64KiB を超える length-prefix フレームが往復できる
- ヘッダだけを送って沈黙する peer に対し、`ReceiveWithTimeout(N)` が概ね N ms で
  `nullopt` を返し、当該接続を切断する。request deadline を渡す呼び出しでは、
  受信が `min(request の残デッドライン, read ハード)` を超えて戻らない（§6.4.8。
  相 A のタイムアウトでは接続を維持し、応答を消費しない）
- 未配線 MessageType に明示エラー応答が返り、blocking client がハングしない。
  MessageType 列挙 ↔ `Payloads` codec の網羅整合が検査される（DEV-162）
- 数値の parse / stringify が locale 非依存（C ロケール固定）で round-trip する（DEV-163）
- uint64 フィールド（`request_id` / `Ping` / `Cancel.target_request_id` /
  `CommitObservation.timestamp_ms`）が 2^53 超でも丸めず全域 round-trip する（DEV-163）
- 非 plain 数値形の桁あふれ（例 `18446744073709551616.0`）を `nullopt` で拒否する（DEV-163 / DEV-188）
- Handshake トークンが per-user ファイルチャネルで配布され、未設定時は
  ACL 縮退 + warn ログとなる（§6.4.4）

> 注: 本節は受け入れ条件の「定義」のみを持ち、各項目の達成状態は持たない
> （正典は Linear）。協定外メッセージ（DEV-162）・数値 codec の locale 非依存
> （DEV-163）・直接パーサ単体テスト拡充（DEV-188）・トークン配布チャネル
> （§6.4.4）の進捗は対応する Linear 課題で追跡する。

## 7. 構造化ログと可観測性（M41）

### 7.1 解決する問題

TIP は `OutputDebugStringA`（DebugView）、Host は stderr 中心。遅延要因
（TIP 側 / Pipe 側 / Host 側）の切り分けと、エラー分類の体系がない。

#### 7.1.1 M41 v1 スコープ

M41 v1 は、Release ビルドで障害の一次情報を採取するためのファイルロガーを
TIP と Host に導入する。§7.2 以降が定める最終形のうち、相関 ID と phase
計測は M51 へ残し、次の契約を実装する。

- `AZOOKEY_LOG=1` のときだけ有効化する。未設定または `1` 以外では、
  Debug / Release のどちらもファイルを作成しない。
- `AZOOKEY_LOG_LEVEL` は `info`、`warn`、`error` のいずれかを受け付ける。
  未設定または不正値では `info` を使う。
- 出力先とファイル名は §5.2 の
  `%LOCALAPPDATA%\azooKey\logs\host-YYYYMMDD.jsonl` と
  `tip-YYYYMMDD.jsonl` とする。
- 各行の必須フィールドは `ts`、`component`、`level`、`event` とする。
  `request_id`、`result`、`error_code` など、既存の呼び出し位置で得られる
  メタ情報は構造化フィールドとして追加してよい。
- 現行ファイルが 5 MiB を超える前にローテーションし、同じ日付の
  `.1` から `.3` まで 3 世代を保持する。`.1` が直前の世代である。
- UTC 日付単位で当日を含む直近 7 日分を保持し、それより古い TIP / Host の
  日次ファイルとローテーション世代は、各プロセスの初回書き込み時に削除する。
- 複数プロセスから同じ component のファイルへ書く場合は名前付き mutex で直列化する。
  待機は最大 200 ms とし、取得できなければ入力処理を妨げず、その行を破棄する。
- ディレクトリ作成、ローテーション、書き込みの失敗はロガー内で処理する。
  ログ失敗を TIP / Host の終了、例外、入力経路の失敗へ波及させない。
- Release ビルドの構造化フィールドはメタ情報に限定し、入力本文、候補本文、
  prompt、`raw_keys`、window title の生値を API へ渡さない。ロガーも §7.6 の
  本文系フィールド名を検出した場合は値を `***redacted***` に置換し、
  `window_title` フィールドは出力しない。
- 文字列フィールドは `RuntimeLogSafeText` で明示的に opt-in した値だけを受け付ける。
  本文を示しうるキーを部分一致で検出した場合は redact する。

M41 v1 では `trace_id` の生成と伝播、phase 別計測、ETW provider、WER / local
dump を実装しない。

### 7.2 構造化ログ（JSON Lines）

TIP / Host とも JSON Lines 形式のログを出力する。出力先は §5.2 の
`%LOCALAPPDATA%\azooKey\logs\host-YYYYMMDD.jsonl` /
`tip-YYYYMMDD.jsonl`。
Windows の `%LOCALAPPDATA%` 解決は TIP / Host とも
`SHGetKnownFolderPath(FOLDERID_LocalAppData)` を共通利用する。

各行に最低限含める必須フィールド:

| フィールド | 内容 |
|---|---|
| `ts` | ISO 8601 タイムスタンプ |
| `level` | `info` / `warn` / `error` |
| `component` | `tip` / `host` / `ipc` |
| `event` | 機械可読なイベント名 |
| `request_id` | IPC リクエスト相関 ID（該当時。§7.3） |
| `trace_id` | ユーザー 1 アクション相関 ID（該当時。§7.3 / §7.7.1） |
| `phase` | 処理フェーズ。正典 enum は `core/include/azookey/logging/Phase.h`（§7.3 / §7.7.2） |
| `latency_ms` | 当該フェーズの所要時間 ms（計測時）。本フィールド名を全コンポーネント共通の正典とする |
| `result` | `ok` / `error` / `cancelled` / `blocked`（secure 抑止時。`docs/privacy-and-secure-input-spec.md` §5.1） |
| `error_code` | §7.4 のエラーコード（error 時） |

`phase` の取り得る値は §7.7.2 の 11 値に固定し、`core/include/azookey/logging/Phase.h`
の `azookey::logging::Phase` を全コンポーネント（tip / ipc / host）の正典とする。
所要時間フィールドは `latency_ms` を正典名とし、`duration_ms` 等の別名は使わない。
phase 別の絶対オフセット（key_down=0 起点）が必要な場合のみ任意で `t_ms` を併記する。

記録対象イベント: Host 起動・終了、pipe listen 開始、model load 成否、
backend 選択、query latency、error、exception summary、learning/user-dict
の load/save 結果。

実装は自前の軽量 JSON Lines ロガーを第一候補とする（依存最小主義）。
`spdlog` 採用はビルド依存の増加に見合うかを評価したうえで判断し、本仕様の
必須範囲には含めない（§11）。

### 7.3 相関 ID とフェーズ

相関 ID は **2 軸のみ** とし、3 つ目の概念（`correlation_id` 等）は導入しない。

| ID | 型 | 粒度 | 発番元 |
|---|---|---|---|
| `request_id` | uint64（≥1, allocator ごとに単調増加） | IPC リクエスト 1 往復 | **TIP（client）採番**。`QueryCandidates`/staleness は `ipc_pending_id_`、送信キュー（`CommitObservation` / `Cancel`）は接続ローカル連番（§7.3 注記）。Host は `req.request_id` を echo（`Dispatcher::MakeResponse`） |
| `trace_id` | string（UUIDv7。**全 envelope 必須**） | 1 論理操作（キー押下 → UI 更新、または 1 lifecycle / settings IPC） | 操作の発行側が採番（キー駆動は TIP の `OnKeyDown`、§7.7.1） |

- 横断追跡のキーは **`(trace_id, request_id)` の組** とする。`trace_id` が
  1 アクション内の複数 IPC を束ね、`request_id` がその中の個々の往復を識別する。
- 両 ID は既存 IPC エンベロープ `{version, request_id, type, trace_id, payload}`
  にそのまま乗る（§12.6 / `docs/privacy-and-secure-input-spec.md` §9）。新フィールドは追加しない。
- `trace_id` は **全 IPC envelope に必須**（`ipc::Deserialize` は `trace_id` 欠落を
  reject、`ipc/src/Messages.cpp`）。発番単位は「1 論理操作」:
  - キー駆動操作 — TIP が `OnKeyDown` で 1 `trace_id` を発番し、その操作の複数 IPC
    （`QueryCandidates` 等）を束ねる。
  - 非キー（lifecycle / settings）IPC — `Handshake` / `Ping` / `LoadModel` /
    `QueryDiagnostics`（§12.6）/ `UpdatePrivacyMode`（privacy §9）等は、その発行側
    （TIP / 設定アプリ）が操作開始時に UUIDv7 を採番する。単発操作は 1 envelope = 1 `trace_id`。
- `request_id` は **すべて TIP（client）側で採番**し、Host は応答で echo するのみ。
  TIP には 2 系統の allocator があり、実装はこの分担を維持する（新たに統合しない）:
  - `ipc_pending_id_`（TIP メンバ）— `QueryCandidates` の応答相関と staleness 判定
    （M10、`req_id == ipc_pending_id_`）・`Cancel.target_request_id` に用いる「最新リクエスト」id。
  - 接続ローカル連番（`next_id`、Handshake=1 の後 2 から開始）— 送信キュー
    （`ipc_send_queue_`）に積む `CommitObservation` / `Cancel` の wire id。このキューは
    応答有無が混在する: **`CommitObservation` は応答あり**（TIP が `expects_response=true`
    で送り、`CommitObservationResponse` を待つ。`tsf-tip/src/TextService.cpp` /
    `inference-host/src/Dispatcher.cpp`）、**`Cancel` のみ応答なし**。
  どちらも `tsf-tip/src/TextService.cpp`。ログ相関では `(trace_id, request_id)` に加え
  MessageType で系統を区別する。`CommitObservation` の応答は**必ず受信・相関する**
  （読み飛ばすと次の受信が stale 応答を拾い pipe ストリームが desync するため、
  「応答なし」扱いにできるのは `Cancel` のみ）。
- `Cancel` は primary 接続とは別の短命な control 接続から送ってよい。
  この場合も control 接続は Handshake を完了してから `Cancel` を送る。
  `Cancel` envelope の `request_id` は control 接続ローカルの wire id であり、キャンセル
  対象は payload の `target_request_id` で指定する。
  Host 側では全接続の `Dispatcher` が共有する `RequestScheduler` に
  `target_request_id` が登録されるため、primary 接続で in-flight の `QueryCandidates`
  にもキャンセルが届く。
- Host の `RequestScheduler`（`inference-host/src/RequestScheduler.cpp`）は wire
  `request_id` を **発番しない**。TIP 由来の id をキーに cancellation / latest 追跡を
  行う側であり、`NextRequestId()` は wire ID の採番元ではない。

フェーズは §7.7.2 の **11 値に固定** し、正典 enum は
`core/include/azookey/logging/Phase.h`（`azookey::logging::Phase`）とする。
M41 では粗い部分集合（例: `romaji_convert` / `model_inference` / `total`）のみ記録し、
M51 で全 11 phase を記録する。phase 体系は 1 つだけで、M41 用と M51 用の別系統は持たない。
各フェーズの所要時間は §7.2 の `latency_ms` フィールドに記録して遅延要因を切り分ける。

### 7.4 エラーコード体系

エラーを 3 カテゴリの enum で固定する:

- `transport` — pipe 切断、接続失敗、フレーミング異常
- `protocol` — JSON パース失敗、未知 MessageType、payload 不整合
- `business` — モデル未ロード、変換失敗、辞書 I/O 失敗

### 7.5 タイムアウト規約

変換候補問い合わせのタイムアウトを定義する（例: ソフト 150ms / ハード
300ms）。ソフト超過はログに記録、ハード超過は当該リクエストを打ち切り
劣化モード（§8.3）へ移行する。

本節が定めるのは request レイヤ（要求送信から応答受信まで。推論時間を含む）の
規約であり、transport レイヤのフレームデッドライン（§6.4.7）とは別物である。
両者は独立に働き、値を一致させない。フレームデッドラインのソフト超過も本節と
同じくログ記録に留める（配線は M41）。

### 7.6 プライバシー配慮（redaction ポリシー正典）

IME である以上、入力本文・候補語をそのままログに出すとプライバシー
リスクがある。本文系フィールド（`reading` / `surface` / `candidate.text` /
確定文字列 / Magic Conversion prompt / typo の `raw_keys` 等）の出力可否は、
以下の **優先順位** で 1 つに定める。上位が下位を常に上書きする。

| 優先 | 条件 | 本文系フィールドの扱い |
|---|---|---|
| 1（最優先） | secure 中（`PrivacyGate::IsSecure()==true`） | **常に redact**。Debug でも `AZOOKEY_LOG_BODY=1` でも出力しない |
| 2 | プライバシー設定が詳細ログ不許可（`PrivacyGate::DetailedLoggingAllowed()==false`。`privacy.redactLogs=true`〔既定〕、または mode が `private`／`secure`） | **常に redact**。build / env を無視 |
| 3 | Release ビルド（既定） | **常に redact**。`request_id` / `trace_id` / 長さ / `result` / `latency_ms` 等のメタ情報のみ |
| 4 | Debug ビルド かつ `AZOOKEY_LOG_BODY=1` | 本文を出力（opt-in。開発時のみ） |
| 5 | Debug ビルド（既定、env 未設定） | redact（メタ情報のみ） |

等価な単一条件として、本文出力は
**`Debug ∧ AZOOKEY_LOG_BODY=1 ∧ ¬IsSecure() ∧ DetailedLoggingAllowed()`** が成り立つ
ときのみ。いずれか 1 つでも偽なら redact する。

- `PrivacyGate::DetailedLoggingAllowed()`（`docs/privacy-and-secure-input-spec.md` §5.1）が
  mode（§3）と `privacy.redactLogs`（同 §7 schema, 既定 `true`）を集約した正典クエリであり、
  本表 優先 2 はそれを参照するだけで重複ロジックを持たない。`redactLogs` の既定が `true` の
  ため、**設定未変更のユーザーは Debug + `AZOOKEY_LOG_BODY=1` でも本文が出ない**。
- redact 時は値を `***redacted***` に置換し、`window_title` は `window_title_hash`
  のみに置換する（`docs/privacy-and-secure-input-spec.md` §8 と同一規約）。
- 本ポリシーの実装は M44 診断 ZIP（§12.5）と secure redaction（同 §5 / §8）で
  **共通の redaction 関数** を用い、二重定義・不整合を作らない。
- レイテンシ trace（§7.7）は本文を含まないメタ情報であり、本ポリシーの
  redact 対象外（phase 別 `latency_ms` は Release でも記録してよい）。
- 互換性テスト（§13）でも、Release 既定で本文がログ・成果物に残らないことを
  確認対象に含める。

### M41 受け入れ条件

- TIP / Host が JSON Lines ログを `%LOCALAPPDATA%\azooKey\logs\` に出力
- 各行に `request_id` / `phase` / `latency_ms` / `result` が含まれる
- エラーコードが 3 カテゴリ enum で固定される
- タイムアウト規約がコードとログに反映される
- Release ビルドで入力本文・候補語がログに出力されない

### 7.7 M51 拡張: レイテンシ内訳トレーサ

M41 の構造化ログ基盤を発展させ、キー押下から候補表示までの全 phase を
1 リクエスト単位で追跡可能にする。Zenzai 最適化（M24 / M25 / M57）・
Tiny Reranker（M56）・ModernBERT スコアリング（M57）の効果測定の前提と
なる。

#### 7.7.1 trace_id

`trace_id` は §7.3 の wire format `{version, request_id, type, trace_id, payload}` に
**既に存在するフィールド**であり（`ipc::Deserialize` が欠落を reject）、M51 は wire format を
変更せず、この既存フィールドへ **UUIDv7 値を生成・伝播**するのみ。キー駆動操作では TIP が
`OnKeyDown` で生成し、対応する全 IPC 往復に付与する。非キー（lifecycle / settings）
IPC は §7.3 のとおり発行側が操作単位で採番する（全 envelope 必須は §7.3 参照）。
既存 `request_id` は IPC リクエスト単位のままとし、`trace_id` は
「ユーザーの 1 アクション（キー押下 → UI 更新）」単位で複数 IPC を
束ねる。

```json
{
  "type": "QueryCandidates",
  "request_id": 123,
  "trace_id": "018fd2c2-2a3e-7c9a-b8e1-7f3a92d4c5e2"
}
```

#### 7.7.2 計測フェーズ

| フェーズ | 説明 | 計測コンポーネント |
|---|---|---|
| `key_down` | TIP がキーを受け取った時刻 | tsf-tip |
| `romaji_convert` | ローマ字かな変換 | core / tsf-tip |
| `ipc_serialize` | payload 生成（JSON 化） | ipc / tsf-tip |
| `pipe_send` | Named Pipe 送信 | ipc |
| `host_queue_wait` | Host scheduler 待ち | inference-host |
| `model_inference` | SimpleConverter / Zenzai 推論 | inference-host |
| `rerank` | 学習 / user dict / tag boost / Tiny / BERT | inference-host |
| `pipe_recv` | 応答受信 | ipc / tsf-tip |
| `staleness_check` | request_id 確認 | tsf-tip |
| `ui_apply` | CandidateWindow / Preedit 更新 | tsf-tip |
| `total` | key_down → ui_apply の通算 | tsf-tip |

この 11 値が phase の正典であり、`core/include/azookey/logging/Phase.h` の
`azookey::logging::Phase` と 1:1 で対応する（wire 名は `PhaseName()`）。
値の **追加は後方互換（patch）**、**削除 / 改名は破壊的変更（major）** とする
（読み手は未知 phase を無視。集計ツールは wire 名に依存するため改名不可）。

各 phase は §7.2 の構造化ログ行として記録する（既存 schema 互換）。所要時間は
`latency_ms`（正典名）に入れ、`key_down` 等の絶対オフセット（key_down=0 起点）が
必要なときのみ任意で `t_ms` を併記する。
`model_inference` 行に以下を付与する（M24 `docs/copilot-pc-backend-spec.md` §4 整合）:

- `engine`: `"llama_cpp" | "winml"`
- `backend`: R1 アクセラレータ `cpu` / `cuda` / `vulkan`（旧 `directml` / `npu` は
  非推奨。R2 では `winml`）
- R2(`winml`) 時のみ `ep`（選択 EP 名。例 `QNNExecutionProvider`）と `ep_state`
  （`NotPresent` / `NotReady` / `Ready` / `Registered` / `Failed`、§4.6）。EP 取得・登録
  失敗をトレースで切り分け可能にする。

```json
{"ts":"2026-05-27T10:00:00.000Z","trace_id":"abc","component":"tip","phase":"key_down","t_ms":0.0,"level":"info","result":"ok"}
{"ts":"2026-05-27T10:00:00.001Z","trace_id":"abc","component":"core","phase":"romaji_convert","latency_ms":0.04,"level":"info","result":"ok"}
{"ts":"2026-05-27T10:00:00.015Z","trace_id":"abc","component":"host","phase":"model_inference","latency_ms":14.5,"backend":"cuda","level":"info","result":"ok"}
{"ts":"2026-05-27T10:00:00.018Z","trace_id":"abc","component":"tip","phase":"total","latency_ms":18.3,"level":"info","result":"ok"}
```

#### 7.7.3 trace_viewer CLI

`bench/azookey_trace_viewer.cpp` を新設し、JSONL ログを集計する:

```powershell
azookey_bench.exe --trace --backend cuda --model zenzai-small.gguf
azookey_trace_viewer.exe trace.jsonl --summary
```

summary 出力例:

```
QueryCandidates latency summary (N=10000)
  total_p50: 18.3 ms
  total_p95: 42.1 ms
  total_p99: 78.4 ms
  model_inference_p95: 30.4 ms
  pipe_p95: 1.2 ms
  ui_apply_p95: 3.8 ms
```

#### 7.7.4 オーバーヘッド制御

通常利用時は §7.6 のプライバシー方針に従い、Release では phase 別
`latency_ms`（メタ情報のみ。本文は含まない）を記録する。詳細な per-key
trace は以下のいずれかで明示有効化する:

- `bench/azookey_bench --trace`（ベンチ実行時）
- `settings.latencyTracing.enabled = true`（設定 GUI から）
- 環境変数 `AZOOKEY_TRACE=1`（開発時）

サンプリングレート（`latencyTracing.sampleRate`、既定 0.01）で本番でも
低コストで取得可能にする。

`settings.latencyTracing.*` を設定 GUI から扱うため、M51 実装時に
`settings/mvp-settings.schema.json`（root が `additionalProperties: false` のため
未定義トップレベルキーは reject される）へ以下を追加する。スキーマ追加と読み書き
パスを同じ PR / コミットで揃え、永続化されない不整合状態を作らない（§8.5.3
`safeMode` と同じ方針）:

```json
"latencyTracing": {
  "type": "object",
  "description": "M51: レイテンシ内訳トレースの取得設定",
  "properties": {
    "enabled": { "type": "boolean", "default": false },
    "sampleRate": { "type": "number", "default": 0.01, "minimum": 0.0, "maximum": 1.0 }
  },
  "additionalProperties": false
}
```

### M51 受け入れ条件

- 全 IPC envelope に `trace_id` フィールドが追加され、JSON parse 後
  伝播する
- 1 リクエスト単位で TIP / IPC / Host / UI の各 phase 時間を JSONL に
  出力できる
- `azookey_trace_viewer --summary` で p50 / p95 / p99 を出力できる
- Zenzai backend 比較（R1: cpu / cuda / vulkan、R2: winml〔EP 自動選択〕）に使える
- 通常利用時（trace 無効）の追加 overhead が p95 で +1ms 未満

## 8. Host 可用性・再接続（M42）

### 8.1 解決する問題

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

Handshake 応答には protocol v1 の省略可能フィールド
`host_generation_id: string` を含める。Host はプロセス起動時にランダムな UUID を
1 回生成し、同じ Host プロセスが受け付ける primary / control を含む全接続へ同じ値を返す。
旧 Host がこのフィールドを省略した場合、TIP は空文字として扱い、接続自体は拒否しない。

TIP は初めて非空の世代 ID を観測したときは保存だけを行う。同じ世代への再接続では
保留中の要求を維持する。既知の世代 ID から別の値（省略を含む）へ変化した場合は、
`Handshaking` から `Ready` へ遷移する前に request ID を更新して旧世代の in-flight 応答を
stale 化する。未送信または接続断後に再武装された pending 要求と候補表示待機状態は維持し、
新世代へ新しい request ID で再送する。候補キャッシュは世代変更時に破棄する。
現行の IPC worker は Handshake と応答処理を同一スレッドで直列化しているため、世代変更と
応答反映は並行せず、request ID の不一致が旧世代応答の実効的な破棄経路になる。
世代の初回観測と変化は構造化ログへ記録する。

### 8.3 再接続と劣化モード

- **Exponential backoff + jitter** で再接続を試行する（例:
  200ms→400ms→800ms→…→最大 5s、各値に jitter を加算）。
- Host から一定時間応答がない場合（§7.5 のハードタイムアウト、または
  ヘルス監視の無応答）、TIP は `Degraded` 状態へ移行し、`SimpleConverter`
  相当のローカルフォールバックで入力継続を保証する。Host 復帰後は
  `Ready` へ戻す。
- 「Host から一定時間応答がない場合」には、pipe 接続自体は維持されているが
  `QueryCandidates` / `Health` の有効応答が deadline 内に返らない
  connected-but-silent 状態を含める。TIP は pipe 切断を待たず、処理種別ごとの
  deadline で劣化判定する。
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
- Host 再起動をまたぐ再接続で `host_generation_id` の変化を検知・記録し、
  旧世代の in-flight 応答を破棄し、pending 要求を新しい request ID で再送する

### 8.5 M47 拡張: ユーザー可視復旧 UX

M42 は IPC transport 層の状態機械と劣化フォールバックまでを範囲とした。
M47 はその上に乗る**ユーザー可視 UX レイヤ**を扱う。Zenzai モデル単位の
劣化（モデル未配置 / ロード失敗 / 推論 timeout）と、連続クラッシュ時の
SafeMode を導入する。

#### 8.5.1 拡張状態機械

M42 の `Ready` / `Degraded` を更に細分化する:

```
Healthy
  ↓ Host no response (M42 transport)
DegradedSimple   ← SimpleConverter で継続
  ↓ reconnect success
Recovering
  ↓ health ok
Healthy

Healthy
  ↓ model load failed / inference timeout
DegradedModel    ← Host は健康だが Zenzai 無効、辞書 + 学習 + Simple
  ↓ reload success
Healthy

Any
  ↓ repeated crash (Host process が N 回連続クラッシュ)
SafeMode         ← AI / 学習 / 外部 API を全停止、最小限の入力のみ
```

| 状態 | 説明 | ユーザー影響 |
|---|---|---|
| `Healthy` | Host + Zenzai 正常 | 通常 |
| `DegradedSimple` | Host またはモデル不調。SimpleConverter で継続 | 変換品質低下 |
| `DegradedModel` | Host は正常だが Zenzai 無効 | 変換品質低下 |
| `Recovering` | 再接続 / 再ロード中 | 一時的に候補更新遅延 |
| `SafeMode` | 連続クラッシュにより AI / 学習を停止 | 安定優先 |

遷移トリガと駆動 timeout（§8.5.2）を一覧化する。`request_id` / `trace_id`
（§7.3）で staleness を判定し、状態遷移は §7 のログに記録する。

| From | To | トリガ | 駆動 timeout / 閾値 |
|---|---|---|---|
| `Healthy` | `DegradedSimple` | Host 無応答（pipe 切断 or connected-but-silent） | Ping 500ms / QueryCandidates fast 150ms / Live 80ms / Heavy 800ms 超過 |
| `Healthy` | `DegradedModel` | Zenzai モデル load 失敗 or 推論 timeout | Model load 30s / 推論 deadline 超過 |
| `DegradedSimple` | `Recovering`（transport 復旧） | 再接続成功（pipe 再確立 + Handshake） | exponential backoff（§8.3） |
| `DegradedModel` | `Recovering`（model 復旧） | `LoadModel` 再ロード**受理**（完了ではない） | Model load 30s |
| `Recovering`（transport 復旧） | `Healthy` | Ping 往復成功（pipe + Handshake Ready） | Ping 500ms 以内 |
| `Recovering`（model 復旧） | `Healthy` | `LoadModelResponse.ok==true`（受理）**かつ後続 `Health.model_loaded==true`**（`model_loaded` は `HealthPayload`／`HandshakeResponse` の field。`LoadModelResponse` は `ok`/`error` のみ）。Ping や ok だけでは遷移しない | Model load 30s |
| `Recovering`（transport 復旧） | `DegradedSimple` | 再接続失敗 / 再 timeout | 同上 timeout 再超過 |
| `Recovering`（model 復旧） | `DegradedModel` | 再ロード失敗 / timeout / `model_loaded==false` | Model load 30s 超過 |
| `Any` | `SafeMode` | Host プロセスが 60s 以内に 3 回連続クラッシュ（§8.5.3） | crash カウンタ ≥3 / 60s |
| `SafeMode` | `Healthy` | ユーザーが手動解除（`settings.safeMode.enabled=false`） | 手動のみ（自動復帰しない） |

`Recovering` は突入元（transport / model）を保持し、退出条件は元の劣化種別に対応する
ものだけを満たす。model 復旧中は pipe が生きていても（Ping OK）モデル未ロードのうちは
`DegradedModel` に留め、`degraded-model` UI（§8.5.4）を消さない。

#### 8.5.2 timeout 表

M42 §7.5 のソフト/ハードを処理種別ごとに具体化する:

| 処理 | timeout |
|---|---:|
| IPC Ping | 500ms |
| QueryCandidates fast | 150ms |
| QueryLiveConversion | 80ms |
| Heavy inference（ローカル Zenzai 変換等） | 800ms |
| 外部 AI 呼び出し（M16 Magic Conversion / M58-C ai-cleanup の openai backend） | `openAiTimeoutMs`（既定 30s）+ 余裕。正典は `docs/ai-backend-spec.md` §7.1 |
| ModernBERT scoring（M57） | 30〜50ms |
| Model load | 30s |

timeout は request 送信または backend 処理開始からの wall-clock deadline として扱う。
connected-but-silent Host でも、pipe 切断や blocking read の解除を待たない。

本表は request レイヤの値であり、transport のフレームデッドライン（§6.4.7。
read 2000ms / write 5000ms）とは別レイヤである。本表の値を変えても §6.4.7 は
追従せず、逆も同様。フレームデッドラインには推論時間が算入されないため、
Heavy inference 800ms や Model load 30s と比較して短いことは矛盾しない。
値は独立だが**優先関係はある**: client の受信で両者が同時に効く場合、本表の
deadline がフレームデッドラインより短ければ本表が勝つ（§6.4.8）。上記の
「blocking read の解除を待たない」を transport 側が破らないための規約である。

timeout 時は Cancel を送信し、古い結果は staleness check（M10）で
破棄する。`request_id` と `trace_id`（M51）で staleness を判定する。
Cancel / deadline は Dispatcher の応答抑止だけでなく、converter / reranker /
backend 推論処理まで伝播させる。重い処理が deadline 後も継続し、IME 側の
fallback や次リクエスト処理を妨げる設計は不可とする。

#### 8.5.3 SafeMode 突入条件

直近 60 秒以内に Host プロセスが 3 回連続でクラッシュした場合、TIP は
`SafeMode` に入り、設定で AI / 学習を一時的に強制 OFF にする。次回起動
時にユーザー通知で復旧手順を案内する。M47 v1（M30 未完了時）は TIP の
候補ウィンドウ下部バナーで通知する。設定アプリの通知バナーへの統合は
M30 完了後の follow-up とし、M30 を M47 v1 の前提にはしない。
SafeMode は `settings.safeMode.enabled = true` フラグとして永続化し、
ユーザーが手動で解除するまで継続する。M47 実装時に
`settings/mvp-settings.schema.json`（既存スキーマは
`additionalProperties: false` のため、未定義キーは現状 reject される）に
以下を追加する:

```json
"safeMode": {
  "type": "object",
  "description": "M47: Host 連続クラッシュ時の安全モード状態",
  "properties": {
    "enabled": { "type": "boolean", "default": false },
    "entered_at": { "type": "string", "description": "RFC 3339 タイムスタンプ" },
    "last_crash_count": { "type": "integer", "default": 0 }
  },
  "additionalProperties": false
}
```

スキーマ追加と Host 側の読み書きパスを同じ PR / コミットで揃え、永続化
されない不整合状態を作らない。

#### 8.5.4 UI 通知

候補ウィンドウ下部の控えめインジケータで状態を表示する:

```
⚠️ Zenzai が応答しないため、簡易変換で継続しています [詳細] [再試行]
```

毎回ラベルを出すと邪魔になるため:

- 状態遷移直後のみ 1 回表示（5 秒で自動消滅）
- 詳細クリックの遷移先は M30 / M44 の進捗で分岐する。M47 v1（M30 未完了
  かつ M44 v1 完了時）は `azookey_diag.exe` の起動方法を案内する TIP 内
  ポップアップを表示する。M30 完了と M44 §12.7 の診断タブ統合（M44
  follow-up）の両方が揃った後は、設定アプリの診断タブを直接開く動作に
  切り替える
- 「再試行」クリックで `LoadModel` IPC を送る（現在選択中のモデル・backend で再ロード）

#### 8.5.5 適応モデル切替の再解釈（DEV-411 で確定 / 2026-08）

karukan は `max_latency_ms` を超えたら main（90M）から light（26M）へ切り替える適応
モデル切替を持つ（`docs/karukan-comparison-report.md` §6）。azooKey はこれを
**取り込まない**。2 モデル常駐を前提とする設計であり、単一 Zenzai モデル構成と衝突する
ためである。ParallelBeam も同じ理由で採らない。

その上で、この機構が解こうとしている問題、すなわち「遅いときは軽い経路へ落として応答を
返す」ことは、**M47 の縮退経路が既に実現している**。§8.5.1 の `Healthy` → `DegradedModel`
は推論 deadline 超過を遷移トリガに持ち、その閾値は §8.5.2 の Heavy inference 800ms で
ある。これが karukan の `max_latency_ms` に対応する。落ちる先は軽量モデルではなく、
Zenzai を無効化した辞書 + 学習 + SimpleConverter の経路であり、モデルをもう 1 つ常駐
させずに同じ目的を果たす。backend の切り替えもモデル差し替えとは別の軸にあり、ロード失敗時に
CPU backend へ再試行し、それも失敗したら SimpleConverter へ落とす経路は
`docs/model-management-spec.md` §5.3 が定めている。

したがって本項の結論は「再解釈済み。追加機構は設けない」である。v1.0 後にレイテンシ起因の
切替を新たに検討する余地が残るのは、モデルとメモリ予算が確定し、**軽量モデルを常駐させる
費用が正当化できると分かった場合**に限る。その検討は §8.5 の状態機械への追加として新規課題
で扱い、本項の判断を暗黙に覆さない。

### M47 受け入れ条件

- Host を手動 kill しても入力中のアプリが固まらない（M42 と同じ）
- Host 再起動後に自動復帰する（M42 と同じ）
- Zenzai モデルロード失敗時に `DegradedModel` 状態が UI に明示される
- 連続クラッシュ時は `SafeMode` に入り、次回起動時に通知する
- 各処理の timeout が §8.5.2 の表通りに動作する

### 8.6 推論 Worker プロセス分離（Broker/Worker, MVP後）

現状の inference-host は TIP から見て out-of-proc だが、`Dispatcher` /
`InferenceEngine` / `SettingsStore` / `UserDataPaths` を同一プロセス・同一
ユニットに同居させている。この構成では、モデル runtime（llama.cpp / ggml /
将来の NPU runtime / driver）起因のメモリ破損・例外が、設定・辞書・IPC 認証まで
巻き込む blast radius を持つ。

将来の堅牢化として、inference-host を **Broker（設定・辞書・学習・IPC 認証・
Worker 監視/再起動）** と **Worker（モデル runtime のみ）** に分離し、モデル
backend のクラッシュを「落としてよい側」へ封じ込める方向を定義する。これは
v1.0 の必須範囲ではなく **MVP 後の投資**であり、着手時は §7 の相関 ID・§8 の
再接続状態機械を土台に、`request_id` を軸とした Cancel / staleness / sequencing
の E2E を最優先で担保する（分離後に取りこぼしが出やすい箇所のため）。設計スパイクは
Linear で追跡する（2026-07 開発基盤ツール導入 第2弾）。`plans/windows-port-roadmap.md`
の「リスクと不確実性」と相互参照する。

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
| Portable C++ 行カバレッジ | 80%（初期段階は informational） | §4.3 |
| 学習永続化の破損率 | 0%（クラッシュ注入テストで確認） | §5.4 |

Portable C++ 行カバレッジは Linux coverage ジョブの `summary.txt` と `summary.json` で
継続観測する。
80% は目標値であり、初期段階では PR の合否条件にしない。
Windows 専用コードを含む全体値は OpenCppCoverage 導入後に別系列として扱い、
portable subset の履歴へ混在させない。

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
fuzz/malformed テスト）を主とする（§6）。先送りしていた置換の判断は §11.2.1 で確定した。

#### 11.2.1 外部ライブラリ移行の方針（DEV-390 で確定 / 2026-08）

「将来判断」として残していた RapidJSON / nlohmann-json への移行方針を、次のとおり確定
する。**v1.0 の範囲では自前パーサを維持し、外部ライブラリへは移行しない。**

移行を推す論拠は、自前実装に起因するバグクラスを構造的に解消できることにあった。その
バグクラスは §6.2 のパーサ強化で既に解消済みである。数値 codec の locale 依存
（DEV-163）は `std::from_chars` / `std::to_chars` への置換で閉じ、int64/uint64 精度・
深度上限・unicode escape・round-trip の直接単体テスト（DEV-188）は `json_test.cpp` に
入った。残る DEV-211 は IPC メッセージ毎の冗長な JSON 往復を減らす性能課題であり、
パーサ実装の出自とは独立している。したがって移行の主たる動機は既に失われている。

対して移行のコストは残ったままである。自前パーサは §6.2 の上限値と wire contract を
spec と `static_assert` で結び付けており、置換すればこの対応関係を作り直すことになる。
TIP は任意アプリへ in-proc ロードされる DLL であり、依存追加はコンパイル時間と
バイナリサイズに直接効く。no-egress 方針のもとでは FetchContent のピン留めと
`AZOOKEY_FETCH_*=OFF` 既定も前提として維持しなければならない。

再評価は次のいずれかが起きたときに行う。

* §6.2 の範囲を超える JSON 要件が生じる（streaming parse、JSON Pointer、
  schema validation など、自前実装への追加が大きくなる機能）
* IPC schema の大規模な作り直しが決まり、payload codec を書き直す機会が生じる
  （このとき RapidJSON を新規ペイロードだけに部分導入し、性能・コンパイル時間・
  バイナリサイズを実測する）
* DEV-211 の bench で JSON parse / stringify 自体がレイテンシのボトルネックだと
  示される（往復回数の削減で足りるなら移行の根拠にはならない）

いずれも v1.0 のリリースゲートではない。設定ファイル側（`SettingsStore` /
`UserDictionary`）はホットパスでないため、部分導入を試すなら先行させる候補になる。

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
重複するため本トラックでは新設しない。開発用 `regsvr32` スクリプトは
`scripts/register-dev.ps1` / `unregister-dev.ps1` へ命名分離済み（DEV-101 / M28）。

### 11.5 clang-tidy / CodeQL の必須化 — 見送り

CI への clang-tidy / CodeQL の**必須化**は導入・調整コストが高い。本トラックでは
`clang-format`（変更行ゲート）までを必須とし、clang-tidy / CodeQL は必須ゲートに
しない（§4.3）。changed-lines clang-tidy（差分行のみの静的解析）は、既存負債で
PR が止まるのを避けつつ新規コード品質を上げる中間策として、`cpp-tidy` ジョブに
**advisory（非ブロッキング）**で導入済み。所見の可視化に留め、必須ゲート化・全体化
（全ソースへの拡大、CodeQL 追加）は将来判断とし Linear で追跡する（2026-07 開発基盤
ツール導入 第2弾）。

同じ段階導入方針を Release バイナリの BinSkim にも適用する。現時点では
`continue-on-error` の advisory とし、SARIF・job summary・artifact に所見を残す。
未署名に由来する `BA2022` だけを抑止し、他のルール結果を隠さない。必須ゲート化は
所見の基準線を整理し、ツール取得と解析の安定性を確認した後の将来判断とする。

### 11.6 IPC payload の Protobuf 即時移行 — 不採用

外部リサーチ提案（2026-07）は Named Pipe transport を維持したまま payload を
versioned Protobuf へ移行する案を挙げる。しかし本プロジェクトの IPC は同一
マシン内・単一実装・少数メッセージであり、schema 進化の恩恵よりも、コード生成
ツールチェーン追加がオフラインビルド原則（§1）へ与える影響と移行コストが上回る。
自前 JSON パーサを §6 の境界コーパス + 有界スモークで堅牢化する路線（§11.2 と整合）を
主とし、Protobuf 化は将来判断とする。設定アプリ実装・fuzz 整備の後に再評価する。

### 11.7 設定アプリの C#/.NET 9 化 — 不採用

外部リサーチ提案（2026-07）は設定アプリを C#/.NET 9 + WinUI 3 にする案を挙げる。
しかし設定アプリの UI フレームワークは **DEV-99 / D-03 で WinUI 3（C++/WinRT）に
確定済み**（`plans/windows-port-roadmap.md`「リスクと不確実性」/
`docs/sideload-packaging-spec.md` §3.0）であり、既存 C++/WinRT スタックとの親和性・
配布形態整合を根拠とする。TIP/Host と別プロセスで IPC 連携する点は C# でも成立するが、
確定済みの決定を覆すだけの決定的な差は現時点で無いため、C++/WinRT を維持する。

## 12. IME 診断・修復ウィザード（M44）

### 12.1 目的

azooKey が動作しない、候補が出ない、Zenzai が使われていない、学習が
反映されない、IME 登録が壊れているといった問題をユーザー自身または
開発者が短時間で切り分けられるようにする。IME は「入力できない」時点で
UX が即死しやすいため、本機能は配布前（Phase 4 ゲート）に優先実装する。

#### 12.1.1 M44 実装スコープ

M44 は `azookey_diag.exe --json`、`--repair`、`--collect` を提供し、
D-001〜D-013 を診断する。
Host が停止している場合もローカルで判定できる項目を継続し、13 項目すべてを
stable schema の `checks` 配列へ出力する。

Host の診断は §12.6 `QueryDiagnostics` を使う。`engine` は実効ランタイム tier
（R1 の `llama_cpp` / `mock`。R2 追加後は `winml`）を表し、`model_loaded` と
`loaded_model_path` は実際にロードされたモデルだけを表す。設定済みパスの存在だけで
モデルロード成功と判定してはならない。

診断 ZIP は `diag.json`、`settings.redacted.json`、`host-health.json`、
`ipc-ping.json`、`environment.txt`、`crash-summary.txt` で構成する。Release
ランタイムログが存在する場合は、直近 7 日の `host-*.jsonl` と `tip-*.jsonl` を
ローテート世代（`.jsonl.N`）も含め、redaction 後に `logs/` へ収集する。
収集は新しいファイルを優先し、1 ファイルあたり末尾 1 MiB、合計 8 MiB を上限とする。
切り詰めまたは除外が発生した場合は `logs/README.txt` に件数と上限を記録する。

D-014、D-015、設定アプリの診断タブは follow-up とする。

### 12.2 診断項目

| ID | 項目 | チェック内容 | 失敗時の推奨修復 |
|---|---|---|---|
| D-001 | TIP DLL 存在 | 登録済み DLL パスが存在するか | 再登録を促す |
| D-002 | COM 登録 | CLSID / InprocServer32 / Profile GUID が正しいか | `scripts/register-dev.ps1` または MSIX 修復 |
| D-003 | 言語プロファイル | 日本語 `0x0411` の Profile があるか | Profile 再登録 |
| D-004 | Host 起動 | 現在のログオンセッションに Host プロセスが存在するか | Host 起動 |
| D-005 | IPC Handshake | Named Pipe 接続 + Handshake 成功 | Host 再起動 |
| D-006 | IPC Ping | Ping 往復 latency 測定 | pipe / firewall / Host 状態確認 |
| D-007 | モデルパス | 設定上のモデル（R1=`.gguf` ファイル / R2=ONNX GenAI ディレクトリ）が存在するか | モデル選択 UI（M45）へ誘導 |
| D-008 | モデル検証 | 形式別検証（R1=GGUF magic / version、R2=`genai_config.json` + 参照 ONNX） | 破損モデル扱い |
| D-009 | fallback 状態 | Zenzai / SimpleConverter / degraded を表示 | モデルロード再試行 |
| D-010 | learning store | 読み込み可能か、破損していないか | バックアップ後に初期化 |
| D-011 | user dict | JSON 読み込み可能か | バックアップ後に修復 |
| D-012 | settings | schema validation 成功 | 不正値のリセット |
| D-013 | logs | `%LOCALAPPDATA%\azooKey\logs\` 書き込み可能 | ディレクトリ作成 |
| D-014 | DPAPI | 暗号化データを復号できるか | 再認証 / 再入力を促す |
| D-015 | app compatibility | 現在の前面アプリで TSF context が取得できるか | 互換性情報表示（M50 result） |

D-013 は実効ログディレクトリ内に一時ファイルを作成して削除し、実際の書き込み可否を
判定する。プロセス終了などで残った診断用一時ファイルは、次回診断時に 24 時間を
超えていれば削除する。Host を一度も起動していない環境ではディレクトリが存在せず
`error` になりうる。この場合の `--repair` はログディレクトリを作成して再診断する。

D-012 の schema 正典は `settings/mvp-settings.schema.json` とし、CI / pre-commit は
同 schema の meta-schema 妥当性と代表サンプルの適合性を静的に検証する（§4.3）。

### 12.2.1 判定基準（status 決定ルール）

各項目の `status`（§12.3 の `ok` / `warning` / `error`）は以下の閾値で確定する。
閾値は §8.5.2 timeout 表・§12.6 `fallback_state` enum・§13.2 自動化レベルと
同一値を参照し、二重定義しない。`--repair` 列は「M44 受け入れ条件」の
自動修復対象（D-001 / D-002 / D-003 / D-013）と一致する。

**評価順序（重複時の優先）**: 各行の `ok` / `warning` / `error` 述語が重複し
うる場合は、**`error` → `warning` → `ok` の順に評価し、最初に一致した（最も
厳しい）status を採用する**。これにより各行の status は一意に定まり、緩い
`ok` 述語が厳しい状態を隠さない。

| ID | ok | warning | error | `--repair` |
|---|---|---|---|---|
| D-001 | DLL パス存在かつ呼び出し元 bitness と一致 | — | パス不在 or bitness 不一致 | ✓ 再登録 |
| D-002 | CLSID / InprocServer32 / Profile GUID が全一致 | 任意キー欠落（動作には影響しない） | 必須キー欠落 or 値不一致 | ✓ `scripts/register-dev.ps1` / MSIX 修復 |
| D-003 | `0x0411` Profile 登録済み | — | 未登録 | ✓ Profile 再登録 |
| D-004 | Host プロセス存在 | — | プロセス不在 | ✗（起動案内のみ。自動起動は M42 / 起動経路の責務） |
| D-005 | 接続 + Handshake Ready | — | 接続不可 or Handshake 失敗 | ✗ |
| D-006 | Ping RTT ≤ 100ms | 100ms < RTT ≤ 500ms | RTT > 500ms or 無応答（§8.5.2 Ping timeout） | ✗ |
| D-007 | `model.enabled=false`（Zenzai 無効 = 該当なし）、または enabled かつ `model.selectedPath` が存在（R1=`.gguf` ファイル / R2=`genai_config.json` を含む ONNX GenAI ディレクトリ） | enabled だがパス未設定（Zenzai ON だがモデル未選択） | enabled かつ設定済みパスが不在 | ✗（M45 モデル選択へ誘導） |
| D-008 | `model.enabled=false`、またはモデル未選択（`model.selectedPath` 空 = 検証対象なし。該当なしとし、未選択の warning は D-007 が担う）、または enabled かつ**選択済み**モデルが `valid` **かつ loaded**（R1=GGUF magic / version、R2=`genai_config.json` パース + 参照 ONNX 実在。model-management-spec §3.3 の format 別 `valid` を使う） | enabled かつ選択済みモデルが `valid` だが未ロード（fallback 動作中） | enabled かつ**選択済み**モデルの形式別検証に失敗（R1: magic 不一致 / version 非対応 / 破損、R2: config 不正 / 参照 ONNX 欠落） | ✗ |
| D-009 | `fallback_state == healthy`、または（`safe_mode` でない）`model.enabled=false`（SimpleConverter 固定が意図された設定） | `degraded_simple` / `degraded_model`（enabled 時の非意図的劣化） | `safe_mode`（`model.enabled` に関わらず最優先） | ✗（復旧は D-005 / D-008 修復経由） |
| D-010 | 読み込み成功・schema 妥当（空 / 新規を含む） | 旧 schema だが migration 可能 | 読み込み不可 / 破損 | ✗（バックアップ後の初期化は手動確認） |
| D-011 | JSON 読み込み成功（空 / 新規・欠損ファイルは空として正常） | — | パース不可 / 破損 | ✗（バックアップ後の修復は手動確認） |
| D-012 | schema validation 成功 | 旧 schema だが migration 可能 | validation 失敗 | ✗（不正値リセットは確認後） |
| D-013 | logs ディレクトリ書き込み可 | — | ディレクトリ未作成、または書き込み不可 | ✓ ディレクトリ作成 |
| D-014 | OpenAI 鍵を要求する**実効バックエンド**が無い（global `aiBackend` と全 `profilesByApp.*` の app-profile §4.2 解決後の実効値がいずれも `none` / `local-zenzai`）、または OpenAI を要求する実効バックエンドがあり `openAiApiKey` が非空で有効（plaintext〔M16–M34 移行期。schema が plaintext を許容〕はそのまま有効、`dpapi:` prefix 付きは復号成功） | OpenAI を要求する実効バックエンド（global もしくは**いずれかの** `profilesByApp.*` が §4.2 解決後に `openai`）があるが `openAiApiKey` が空（資格情報未設定で認証不可） | `dpapi:` prefix 付きの暗号化値が復号失敗 | ✗（再認証 / 再入力を促す） |
| D-015 | 前面アプリで TSF context 取得可**かつ §13.3.2 の既知の劣化 / workaround なし**（§13.2 の自動化レベルに関わらず、context が取れれば automation level では warning にしない） | TSF context は取得できるが既知の product workaround / 部分的劣化がある（§13.3.2） | TSF context 取得不可 | ✗（§13 互換性情報へ） |

全体 `status` は §12.4 の規約どおり `checks[].status` の最悪値
（`error` > `warning` > `ok`）とする。`warning` は「縮退しているが入力は
継続できる」、`error` は「当該機能が成立しない」を意味し、UI（§12.7）の
アイコン（✅ / ⚠️ / ❌）に対応させる。任意データ・任意機密の「未設定」は
正常系として扱い、`warning` を出さない。具体的には、クリーンインストール
直後で学習・辞書が空（`UserDictionary::Load()` は欠損ファイルを空の成功
ロードとして扱う）の D-010 / D-011 と、OpenAI 鍵を要求する**実効バックエンド**が
無い構成（global および全 app-profile の §4.2 実効 `aiBackend` が
`none` / `local-zenzai`）で復号対象の機密が無い D-014 は
`ok` とする。ただし global もしくはいずれかの app-profile の実効バックエンドが
`openai`（§4.2 解決後）で `openAiApiKey` が空の場合は、
当該 AI backend が認証できないため D-014 を `warning` とする
（機能を選択したのに資格情報が欠落している状態）。`warning` / `error` は
migration 要・読み込み不可・破損・**選択中機能の資格情報欠落**、または
**設定済みの値が期待どおり復号 / 読込できない**場合に限る。

**D-014 の「実効バックエンド」の定義**: global `settings.aiBackend` と、設定された
全 `profilesByApp.*` プロファイルの `aiBackend` を `docs/app-profile-spec.md` §4.2 の
解決規則（`auto` を global へ展開 → `PrivacyGate` による降格を適用）で評価した実効値の
集合を指す。診断は前面アプリに依存しない**静的構成チェック**のため、その集合の
**いずれか 1 つでも** `openai` に解決されれば「OpenAI 鍵を要求する」とみなす。逆に
プロファイルが `aiBackend=openai` を宣言しても、privacy（`secure`、または `private` /
`offline` / `custom` の外部 AI 無効）で §4.2 step 2–3 により `local-zenzai` / `none` へ
降格する実効値は鍵を要求しない（D-014 の判定対象に含めない）。`profilesByApp` が未導入
（M48 前）の構成では実効集合は global のみとなり、従来の D-014（global `aiBackend` のみで
判定）と等価で後方互換である。
同様に、`model.enabled=false`（SimpleConverter 固定。model-management-spec
§モデル設定）は Zenzai を使わない意図的構成のため、D-007 / D-008 / D-009 は
`ok`（該当なし）とし、モデル未選択・未ロード・`degraded_model` を `warning`
扱いしない。これらが `warning` になるのは `model.enabled=true` のときのみ。
ただし `safe_mode`（連続クラッシュによる安全モード）は `model.enabled` に
関わらず D-009 を `error` とし、この「該当なし」ショートカットより優先する。

### 12.3 `azookey_diag.exe` CLI

CLI は 3 サブコマンドを持つ。

```powershell
azookey_diag.exe --json                                  # 全項目を JSON で出力
azookey_diag.exe --repair                                # 自動修復可能なものを実行
azookey_diag.exe --collect --output azookey-diag.zip     # 診断 ZIP 生成
```

`--json` 出力例:

```json
{
  "status": "warning",
  "timestamp_ms": 1780000000000,
  "checks": [
    {
      "id": "D-008",
      "name": "model_validation",
      "status": "warning",
      "message": "Model is valid but not loaded yet. SimpleConverter fallback is active.",
      "details": {
        "configured_path": "%LOCALAPPDATA%\\azooKey\\models\\zenzai.gguf",
        "exists": true,
        "valid": true,
        "loaded": false,
        "backend": "cpu"
      }
    }
  ]
}
```

`status` は `ok` / `warning` / `error` の 3 値。`--json` はテスト可能な
stable schema として固定する。
`--repair` は D-001、D-002、D-003、D-013 を修復した後に全項目を再診断し、
同じ root object へ次の `repairs` 配列を追加する。

```json
{
  "probe_failed": false,
  "repairs": [
    {
      "id": "D-001",
      "status": "succeeded | failed | not_needed | permission_denied",
      "before_status": "ok | warning | error",
      "after_status": "ok | warning | error | null",
      "message": "string"
    }
  ]
}
```

D-001〜D-003 の machine-wide COM / TSF 再登録は 1 回の操作にまとめる。
非昇格プロセスでは UAC を自動起動せず、`permission_denied` と昇格して再実行する手順を返す。
修復対象が `ok` の場合は操作せず `not_needed` を返す。
post-probe を実行できない場合は `probe_failed=true` とし、`after_status` を `null` にする。
修復前に `ok` だった項目は `not_needed` のまま保持し、修復を試みた項目は `failed` とする。
D-001〜D-003 の直接修復は AppContainer ACL を変更しないため、UWP または Microsoft Store
アプリで検証する場合は `scripts/register-dev.ps1` を実行して ACL を付与する。
再登録が失敗すると DLL 側の rollback により修復前の登録も失われる場合がある。
post-probe で状態悪化を検出した場合は `failed` とし、`scripts/register-dev.ps1` または
MSIX 修復を案内する。
診断の実行と JSON または ZIP の生成に成功した場合、`status` が `error` でも
プロセスの exit code は `0` とする。
引数の不正、probe の実行失敗、ZIP の生成失敗では exit code `2` を返す。
`--repair` で `failed` または `permission_denied` が残った場合は exit code `3` を返す。
自動化側は exit code ではなく、出力された `status` と `checks` で診断結果を判定する。

### 12.4 診断 ZIP 構成

```
azookey-diagnostics-YYYYMMDD-HHMMSS.zip
├── diag.json
├── settings.redacted.json
├── host-health.json
├── ipc-ping.json
├── logs/
│   ├── host-YYYYMMDD.jsonl   (直近 7 日)
│   ├── host-YYYYMMDD.jsonl.N (ローテート世代)
│   ├── tip-YYYYMMDD.jsonl    (直近 7 日)
│   ├── tip-YYYYMMDD.jsonl.N  (ローテート世代)
│   └── README.txt            (切り詰めまたは除外時のみ)
├── environment.txt           (OS バージョン / CPU / RAM / GPU)
└── crash-summary.txt         (WER ダンプの要約のみ、ダンプ本体は含めない)
```

`diag.json` は §12.3 `--json` 出力と同一の **stable schema** とし、以下を正典とする
（snapshot テストで固定。本文系フィールドは持たない）:

```json
{
  "status": "ok | warning | error",
  "timestamp_ms": 0,
  "checks": [
    {
      "id": "D-001 .. D-015",
      "name": "string (安定 ID。例 model_validation)",
      "status": "ok | warning | error",
      "message": "string (人間向け。本文・候補・prompt を含めない)",
      "details": { "...": "check 固有のメタ情報のみ（パス / bool / 件数 / hash / mtime）" }
    }
  ]
}
```

- `status` は `checks[].status` の最悪値（error > warning > ok）。
- 各 ZIP メンバの突き合わせは §7.3 の相関 ID では行わず、診断は単発スナップショット。
  `host-health.json` は §12.6 `QueryDiagnostics` の payload を保存する。ただし
  自由文字列の `last_error` / `ep_last_error` はパス正規化 + 本文 redaction を
  通してから保存し（§12.5 / §12.6）、生の payload を**そのまま書き出す経路は持たない**。
- 新しい check は `D-0NN` を末尾追加（後方互換）。既存 ID の `name` 改名は破壊的変更扱い。

### 12.5 機密情報の取り扱い

診断 ZIP には以下を**含めない**:

- OpenAI API key（DPAPI 暗号化済みでも除外）
- 入力本文・候補本文
- Magic Conversion の prompt
- 変換前後の全文
- ユーザー辞書の実データ
- 学習 TSV / typo_corrections.tsv / auto_words.tsv の本文

必要な場合でも、**件数・サイズ・hash・mtime のみ**を記録する。
`settings.redacted.json` は API key 等の機密 field を `***redacted***`
に置換した copy。

ZIP メンバごとの redaction ルールを以下に固定する。本文系フィールドの
出力可否は §7.6 redaction ポリシーを正典とし、診断 ZIP は §7.6 と
**共通の redaction 関数**を用いて二重定義を作らない。

| ZIP メンバ | redaction ルール |
|---|---|
| `diag.json` | §12.4 stable schema の制約に従い `message` / `details` に本文・候補・prompt を含めない。パス中のユーザー名は `%LOCALAPPDATA%` 等の環境変数表記へ正規化する |
| `settings.redacted.json` | API key 等の機密 field に加え、Magic Conversion prompt 系 field（`promptPrefixByApp` の各値、および移行後の `profilesByApp.*.promptPrefix`〔`profilesByApp` は process / window class をキーとする map。配列ではなく全エントリ値が対象〕）を `***redacted***` に置換。さらに path 系 field（`model.selectedPath` / `model.directory` / `customRomajiTablePath` 等の絶対パス）は `diag.json` / `host-health.json` と同じくユーザー名を含む生パス（`C:\Users\...`）を残さないよう `%LOCALAPPDATA%` 等の環境変数表記へ正規化する。§7.6 はログ本文の正典で settings の prompt / path field を対象に含めないため、診断 ZIP では本欄で明示的に処理し、§12.5 の「prompt を含めない」方針と整合させる |
| `host-health.json` | §12.6 `QueryDiagnostics` payload。構造化 field（bool / enum / 数値）はそのまま保存するが、自由文字列の `last_error` / `ep_last_error` はパス正規化（`%LOCALAPPDATA%` 等）+ 本文 redaction を通してから保存し、生のユーザーパス・backend / API 診断文を残さない |
| `ipc-ping.json` | RTT / 成否のみ |
| `logs/*.jsonl*` | §7.6 の共通 field 判定を用いてログとローテート世代を収集（Release 既定で本文なし）。Debug かつ `AZOOKEY_LOG_BODY=1` の本文入りログは収集時に再 redact する。新しいファイルから 1 ファイル末尾 1 MiB、合計 8 MiB まで収集し、切り詰めまたは除外は `logs/README.txt` に記録する |
| `environment.txt` | OS バージョン / CPU / RAM / GPU のみ。ホスト名・ユーザー名・シリアル番号を含めない |
| `crash-summary.txt` | WER ダンプの要約のみ。ダンプ本体・スタック上の文字列バッファを含めない |

### 12.6 IPC: QueryDiagnostics

Host 側の状態を取得する新規 IPC。MessageType enum 末尾に追加する
（M40 互換性ルール）。

エンベロープは既存 wire format `{version, request_id, type, trace_id, payload}`
に従う。

```
Request:
  { "version": 1, "request_id": 1, "type": "QueryDiagnostics",
    "trace_id": "...", "payload": {} }

Response:
  { "version": 1, "request_id": 1, "type": "QueryDiagnostics",
    "trace_id": "...",
    "payload": {
      "model_loaded": bool,
      "loaded_model_path": str (optional, model_loaded=true のときだけ存在),
      "engine": "llama_cpp" | "mock" | "winml",
      "backend": str, "rss_mb": int,
      "ep": str (optional, R2/winml 時の選択 EP 名),
      "ep_state": str (optional, "NotPresent"|"NotReady"|"Ready"|"Registered"|"Failed"),
      "ep_last_error": str (optional, EP 取得・登録失敗の HRESULT/診断文; §4.6),
      "learning_entries": int, "user_dict_entries": int,
      "fallback_state": "healthy" | "degraded_simple" |
                        "degraded_model" | "safe_mode",
      "last_error": str (optional)
    } }
```

v1 の reader は `rss_mb`、`learning_entries`、`user_dict_entries` の欠落を `0` として扱う。
`engine`、`backend`、`fallback_state` は必須であり、欠落時は payload 全体を不正とする。

`--collect` 時はこの IPC で取得した値を `host-health.json` に保存する。
自由文字列の `last_error` / `ep_last_error` は失敗時にユーザーパスや backend /
API 診断文を含みうるため、保存前にパス正規化 + 本文 redaction を通す（§12.5）。

### 12.7 UI

M44 v1（Phase 4 ゲート）は **CLI（`azookey_diag.exe`）+ 診断 ZIP** のみで
完結させる。設定アプリ `診断` タブは M30 完了後の follow-up として
v1.x で追加する位置づけ（M30 を M44 v1 の前提にはしない）。下記の GUI
要件は v1.x（M30 完了後）に適用する。Host 未起動でも GUI 単体で項目
D-001〜D-003 / D-007 / D-008 / D-011〜D-013 までは実行可能とする。

```
[azooKey 診断]

状態: 一部問題があります

[1] TIP 登録状態             ✅ 正常
[2] Host 起動状態            ✅ 起動中
[3] IPC 接続                 ✅ Ping 12ms
[4] Zenzai モデル            ⚠️ 未ロード。SimpleConverter fallback 中
[5] 学習データ               ✅ 正常
[6] ユーザー辞書             ✅ 正常
[7] 設定ファイル             ✅ 正常
[8] ログ出力                 ✅ 有効
[9] セキュリティ設定         ✅ API キーは DPAPI 暗号化済み

[再チェック] [自動修復] [診断 ZIP を作成] [ログフォルダを開く]
```

### M44 受け入れ条件

- クリーン環境で全項目チェックが実行できる
- Host 未起動でも診断アプリがクラッシュしない
- Zenzai 有効でモデル未選択（`model.selectedPath` 空）時に `warning`、
  設定済みパスが不在の場合は `error` として fallback 状態を表示する（§12.2.1 D-007 / D-008）
- 診断 ZIP に秘密情報が含まれない（snapshot テストで保証）。各 ZIP メンバが
  §12.5 の redaction ルール表どおりに処理される
- `--json` 出力が stable schema としてテストされる
- 各診断項目が §12.2.1 の判定基準どおりに `ok` / `warning` / `error` を返す
- `--repair` で D-001 / D-002 / D-003 / D-013 の自動修復が動く

## 13. アプリ互換性テストハーネス（M50）

### 13.1 目的

主要アプリで TSF composition / 候補ウィンドウ位置 / 確定 / キャンセル /
Unicode / 絵文字 / Undo / Redo が壊れないことを半自動で検証する。
M28（MSIX サイドロード）着手前にアプリ互換性のベースラインを確保する。

### 13.2 対象アプリ

| 種別 | アプリ | 自動化レベル |
|---|---|---|
| 標準 | Notepad | full |
| 標準 | WordPad / Notepad 後継 | best-effort |
| ブラウザ | Edge | full |
| ブラウザ | Chrome | full |
| ブラウザ | Firefox | best-effort |
| Electron | VS Code | full |
| Electron | Discord | best-effort |
| Electron | Slack | best-effort |
| Office | Word | recorder |
| Office | Excel | recorder |
| Office | Outlook | recorder |
| Terminal | Windows Terminal | full |
| Terminal | PowerShell ISE | best-effort |
| UWP / WinUI | Windows Settings | full |
| UWP / WinUI | Store apps | best-effort |

`full` = UI Automation + SendInput + screenshot による完全自動。
`best-effort` = 自動化可能な範囲のみ、残りは checklist。
`recorder` = キーボード操作の記録・再生（Office は UI Automation の
信頼性が低い）。

M50 の自動ゲートは次の target 定義を使う。
runner は target ごとに新規ウィンドウと一時文書を作り、既存の文書やタブを操作しない。

| target | AppId | トップレベル window class | 優先する UI Automation control |
|---|---|---|---|
| Notepad | `Microsoft.WindowsNotepad_8wekyb3d8bbwe!App` | `Notepad` | Document |
| Edge | `MSEdge` | `Chrome_WidgetWin_1` | Edit + Name `azooKey compatibility editor` |
| VS Code | `Microsoft.VisualStudioCode` | `Chrome_WidgetWin_1` | Document |

Edge は ControlType だけで Edit を探索するとアドレスバーへ一致し得るため、テスト HTML の
`aria-label` が公開する UIA Name との AND 条件で textarea を特定する。

### 13.3 テストケース

| ID | ケース | 期待動作 |
|---|---|---|
| C-001 | `nihongo` → Space → Enter で確定 | 「日本語」が確定される |
| C-002 | Backspace で preedit が戻る | 1 文字分戻る |
| C-003 | ESC で composition 破棄 | preedit が消える |
| C-004 | 候補ウィンドウがキャレット付近に出る | キャレット下に出る |
| C-005 | マルチディスプレイ端で候補が画面外に出ない | 画面内にクランプ |
| C-006 | DPI 150% で候補位置がずれない | 正しくスケール |
| C-007 | 絵文字 / サロゲートペア入力 | 正しく入力される |
| C-008 | Undo / Redo が破綻しない | 元に戻る |
| C-009 | フォーカス移動時に composition が安全に処理される | crash しない |
| C-010 | Host kill 中も入力が固まらない | DegradedSimple で継続 |
| C-011 | `Ctrl+A/C/V/L/S`、Alt メニュー、Win キー併用を押す | TIP が食わずアプリ / OS へ通る |
| C-012 | `ja` / `ju` / `jo` / `jya` / `jyu` / `jyo` を入力 | 「じゃ」「じゅ」「じょ」として preedit / commit できる |

C-001〜C-012 は `full` アプリ（§13.2）で UI Automation により自動判定できる。
ただし M50 完了ゲートの**必須対象は Notepad / VS Code / Edge**（M50 受け入れ
条件）であり、その他の `full` アプリ（Chrome / Windows Terminal /
Windows Settings 等）は実行可能な範囲で自動判定する best-effort 対象とする。
`best-effort` / `recorder` アプリでは自動化できないケースを §13.3.1 の
手動チェックリストで補完する。

### 13.3.1 Office 手動チェックリスト（recorder 補完）

Office（Word / Excel / Outlook）は UI Automation の TSF テキストパターン
信頼性が低く（§13.2 `recorder`）、キー操作の記録・再生 + 目視チェック
リストで代替する。各行は「対象」列のアプリでのみ確認し、対象外アプリでは
N/A（`report.md` に `N/A` と記録し pass/fail 判定に含めない）とする。pass /
fail を手動記録して `report.md` の Office セクション（§13.5）へ転記する。

| ID | 対象 | チェック項目 | 期待 |
|---|---|---|---|
| O-01 | Word / Excel / Outlook | 本文（Word 段落 / Excel セル / Outlook 本文）で `nihongo`→Space→Enter | 「日本語」確定、文字化けなし |
| O-02 | Word / Excel / Outlook | 候補ウィンドウがキャレット付近に出る | キャレット下に出て、セル / 行移動に追従 |
| O-03 | Excel | セル編集（F2）と数式バーの双方で composition が成立 | 双方で確定できる |
| O-04 | Excel | 未確定中の矢印キーが preedit 内移動になる | preedit 内で動く（確定後はセル移動） |
| O-05 | Word | オートコレクト / オートフォーマットが composition を破壊しない | preedit 中は介入しない |
| O-06 | Outlook | 宛先（To / Cc）欄と本文の双方で入力できる | 双方で確定できる |
| O-07 | Word / Excel / Outlook | 絵文字 / サロゲートペア確定後の表示（C-007 相当） | 正しく表示される |
| O-08 | Word / Excel / Outlook | ESC で composition 破棄 / Backspace で 1 文字戻る（C-002 / C-003 相当） | preedit が消える / 1 文字戻る |
| O-09 | Word / Excel / Outlook | Host kill 中の入力（C-010 相当） | 固まらず DegradedSimple で継続 |
| O-10 | Word / Excel / Outlook | Release 既定で本文がログ / 成果物に残らない（§7.6） | 残らない |

recorder スクリプトは各アプリの対象行（O-01〜O-09 のうち対象列に該当する
もの）のキー列を記録・再生し、結果は目視 + screenshot で判定する。

### 13.3.2 アプリ別 workaround / 既知の差異

`full` 自動化が成立しない、または TSF 実装差で挙動が分かれるアプリの
既知差異と対処方針を固定する。実装（§13.4）の target JSON にこの方針を
反映する。

| アプリ / 種別 | 既知の差異 | workaround |
|---|---|---|
| Chrome / Edge（Chromium） | display attribute（下線）が完全反映されない場合がある | 位置取得は `ITfContextView::GetTextExt` を優先。下線は IME 既定描画にフォールバック |
| Firefox | TSF サポートが best-effort。`GetTextExt` が空矩形を返すことがある | 空矩形時は直近 caret rect のキャッシュへフォールバック |
| VS Code / Electron | Monaco が composition 中に独自補完を出す | preedit 中は IME 候補を優先し、Electron 側 IME イベント順序に依存しない |
| Discord / Slack（Electron） | `contenteditable` で `GetTextExt` 精度が低い | best-effort。位置ずれ時はキャレット位置 fallback |
| Word / Excel / Outlook | UI Automation TextPattern の信頼性が低い | recorder + §13.3.1 チェックリスト。Excel はセル編集と数式バーで context が切り替わる点に注意 |
| Windows Terminal / PowerShell | conhost 系で TSF level 3 非対応の場面がある | level 1（最小 composition）で確定を優先、候補位置はキャレット概算 |
| UWP / WinUI（Store apps） | AppContainer ACL、Windows アプリ互換カテゴリ、第三者 IME の署名がロード前提 | `ALL APPLICATION PACKAGES`（S-1-15-2-1）RX、`GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT` 登録、署名済み TIP を揃える（正典は `docs/sideload-packaging-spec.md` §1.7）。本ハーネスは Windows Settings を `full`、その他 Store apps を `best-effort` とし、署名済み成果物の確認は Human Gate で行う |

### 13.4 実装

`compat-test/` ディレクトリを新設:

```
compat-test/
├── CMakeLists.txt
├── runner/
│   ├── CompatRunner.cpp        # UI Automation + SendInput
│   ├── CaseSupport.cpp         # 物理 screen 座標の共通判定
│   ├── ClipboardIsolation.cpp  # C-011 の退避・固定データ置換・復元
│   ├── ScreenshotCapture.cpp   # 矩形だけを描く WIC PNG
│   └── ReportWriter.cpp
├── cases/
│   ├── C001_basic_input.cpp
│   ├── C002_backspace.cpp
│   ├── C003_escape.cpp
│   ├── C004_candidate_position.cpp
│   ├── C005_monitor_clamp.cpp
│   ├── C006_dpi_scaling.cpp
│   ├── C007_surrogate_pair.cpp
│   ├── C008_undo_redo.cpp
│   ├── C009_focus_transition.cpp
│   ├── C010_host_recovery.cpp
│   ├── C011_shortcut_routing.cpp
│   └── C012_romanization.cpp
└── targets/
    ├── notepad.json
    ├── edge.json
    ├── vscode.json
    └── ...
```

各 target JSON は AppId / window class / 自動化レベルを定義する。

**クリップボード安全化（C-011）**: C-011 は `Ctrl+V` 貼り付けの透過を検証する
ため、実機・self-hosted runner の実クリップボードに依存すると利用者の私的な
クリップボード内容を対象アプリに貼り付け、§13.5 の screenshot / log artifact
として保存してしまうおそれがある。ハーネスは **C-011 実行直前に決定的な
テストデータでクリップボードを置換し、実行後に元の内容を復元する**こと
（復元失敗時も元データを artifact に残さない）。**クリップボード退避は
M50 ゲート対象（Notepad / VS Code / Edge）では前提条件**であり、退避が
不可能な runner では C-011 を pass にできない（gate 未達の failing skip 扱い。
自動ゲートから silent に除外しない）。手動確認（§13.3.1 相当）への振り替えは
best-effort / 非ゲート対象に限る。

退避時は `EmptyClipboard` より前に全formatを列挙し、遅延レンダリングを含む各handleを
即時複製する。復元後は退避した全formatが存在し、比較可能な `HGLOBAL` データのsizeと
hashが一致することを確認する。複製または一致確認ができない場合は復元成功として扱わない。

C-007 の `KEYEVENTF_UNICODE` によるサロゲートペア注入はTSFを通らないため、対象アプリの
UTF-16保持確認に限定する。保持できても `surrogate-pair-tip-path-unverified` の
`failing-skip` とし、azooKey候補からの絵文字確定は `gate:human-required` の実機検証で
補完する。

C-010 はPowerShell supervisorが起動したHostだけを停止対象とし、再起動プロセスに加えて
per-user named pipeへの接続を確認してから復帰と判定する。runnerはHostを代替起動しない。
Notepad targetでは、再接続待ちを後続ケースへ波及させないためC-010を最後に実行する。

runner / C-001〜C-012 / unit test はトップ `CMakeLists.txt` から
`add_subdirectory(compat-test)` で Windows ビルドへ配線する。

runner は起動前後のトップレベルウィンドウを比較し、起動後に増えた対象 class の
ウィンドウだけを runner 所有として扱う。
この判定では、`CreateProcessW` が返した PID のウィンドウを優先し、対象 executable と
同名の既存 process が新しいウィンドウを生成した場合も、その新規ウィンドウを選択する。
runner 所有の新規ウィンドウは `WM_CLOSE` の対象にできるが、既存 process 自体は終了しない。
一時 document を編集した場合は、保存結果を確認できたときだけ新規ウィンドウを閉じる。

Modern Notepad は設定に応じて、指定された一時 document を既存ウィンドウの新規 tab として
開く場合がある。
Notepad target は `use_temporary_document` と
`allow_reused_window_for_temporary_document` を有効にし、UI Automation の選択中 `TabItem`
または一意な一時ファイル名を含む対象ウィンドウ title で今回の document を識別する。
識別できた場合、runner が所有するのは一時 document の tab だけであり、既存ウィンドウと
既存 process は所有しない。
既存ウィンドウを再利用する場合、editor探索は非表示でない候補が一つに定まる場合だけ成功とする。
入力前後にも一時 document が選択中であることを確認し、別tabへ切り替わった場合は入力を中止する。
この非表示判定は既存ウィンドウ内のtab再利用に限り、EdgeとVS Codeを含む新規ウィンドウの
editor探索には適用しない。

終了時は一時 document が選択中であることを再確認する。
`Ctrl+S` の入力注入だけでは保存成功としない。
一時ファイルの更新時刻が変化し、内容が空になったことを確認できた場合だけ `Ctrl+W` でそのtabを閉じる。
再利用を許可する target は `save_before_close` を無効にできない。
`require_new_window` と既存ウィンドウ再利用を同時に指定したtarget設定は無効とする。
所有権、保存、tabの終了を確認できない場合は既存ウィンドウを閉じず、一時ファイルを残して
そのパスを診断へ出力する。

runner が `TerminateProcess` の対象にできるのは、自身の `CreateProcessW` が返した process
handle に限る。
対象ウィンドウの PID が起動 PID と異なる場合、その対象 process を終了してはならない。
対象 class のウィンドウが生成されなければ `new-target-window-not-found`、既存ウィンドウを
対象 executable の既存ウィンドウを検出しても一時 document の所有権を一意に証明できなければ
`target-document-ownership-not-established` の `failing-skip` とする。
回帰テストは「起動 process の新規ウィンドウ」「既存 process が生成した新規ウィンドウ」
「既存ウィンドウ内の新規 tab」「ウィンドウ未生成」を区別する。

Edge と VS Code は `Chrome_WidgetWin_1` の新規ウィンドウだけを操作する。
Edge は外部通信を行わない一時 HTML の textarea、VS Code は拡張機能を無効にした
一時テキストファイルを使う。
3 target とも C-010 を最後に実行し、Host の再接続待ちを後続ケースへ波及させない。

### 13.5 出力

```
compat-report-YYYYMMDD-HHMMSS/
├── report.md         # 人間向けサマリ
├── report.json       # CI artifact 用
├── screenshots/
│   └── notepad_C-001_fail.png
└── failures/
    └── notepad_C-001_fail/
        ├── failure.log    # 固定 reason code と矩形のみ
        └── screenshot.png
```

スクリーンショットは失敗時だけ生成する。デスクトップ画素は取得せず、空の画像に
対象ウィンドウ・キャレット・候補ウィンドウの矩形だけを描画する。
`report.md` は target 固有の HTML コメント marker、全体結果、件数表、折りたたみ可能な
case 詳細表を持ち、そのまま PR コメント本文として使える形式にする。

### 13.6 CI 連携

`.github/workflows/compat.yml` に optional な `compat` ジョブを置く。
この workflow は `compat-test` ラベル付き PR の open / reopen / synchronize / labeled
イベント、または手動 dispatch でだけ runner を起動する。
通常の PR ではジョブが skip されるため、Windows runner 時間を消費しない。

```yaml
jobs:
  compat:
    if: >-
      github.event_name == 'workflow_dispatch' ||
      contains(github.event.pull_request.labels.*.name, 'compat-test')
```

ジョブは Release の `compat_test` を一度だけビルドし、Notepad、VS Code、Edge の
3 target を順に実行する。
各 target の非 0 終了を記録しても残りの target を続行し、`compat-report-*/`、
`compat-run-*.log`、`compat-summary.json` を
`compat-report-<run-id>-<run-attempt>` artifact としてアップロードする。
実機条件を満たせない target は `failing-skip` を report に残す。

GitHub-hosted runner では azooKey TIP を登録も選択もしない。
このジョブは runner のビルド、対象アプリの起動、診断レポートの収集経路を確認するため、
target の非 0 終了だけではジョブを失敗させない。
ジョブが成功しても M50 の互換性テストが通ったことを意味せず、M50 ゲートは azooKey TIP を
登録・選択した対話環境の `report.json` で判定する。

### M50 受け入れ条件

- Notepad / VS Code / Edge で C-001〜C-012 の自動テストが通る
- Office（Word / Excel / Outlook）が §13.3.1 の手動チェックリストで検証され、
  結果が `report.md` の Office セクションに記録される
- 失敗時にスクリーンショットとログが `failures/` に保存される
- `report.json` が CI artifact としてアップロードできる
- `report.md` が PR コメント用に整形されている
