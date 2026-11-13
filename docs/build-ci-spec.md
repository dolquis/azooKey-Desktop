# ビルド再現性・依存管理・CI 品質ゲート 仕様（M37・M38・M43）

対象リポジトリ: dolquis/azooKey-Desktop
対応マイルストーン: M37（§2、§3）、M38（§4）、M43（§9）。§10・§11 は共通
関連: `docs/dev-infrastructure-spec.md`（開発基盤の入口と転送表）、
      `docs/vm-verification-package-spec.md`（旧 §2.6 の実機検証パッケージ）、
      `docs/test-inventory.md`、`README.md`（preset と標準手順）、
      `plans/windows-port-roadmap.md` M37 / M38 / M43

本書は `docs/dev-infrastructure-spec.md` から §2〜§4・§9〜§11（§2.6 を除く）を切り出した文書である。節番号 §2.x〜§4.x・§9.x〜§11.x は旧 spec のまま保ち、旧 spec の転送表がこれらの節を本書へ解決する。本書内で、本書にない節（旧 spec の他の節）を指すときは、文書名を付けて書く（例: `observability-spec.md` §7.6）。

## 正典範囲

CMake preset と共通オプション、`.clang-format`、`.gitignore`、開発環境 doctor、ビルド時間の内訳（§2）、依存管理方針（§3）、
CI のジョブ構成と品質ゲート、bench smoke、Sanitizer、sccache、CTest の並列実行、ブランチ保護と必須チェック（§4）、
WIL の段階導入（§9）、成果測定 KPI（§10）、不採用とした提案（§11）は本書が正典である。

## 節とマイルストーンの対応

| 節 | マイルストーン |
|---|---|
| §2〜§3 | M37 |
| §4 | M38 |
| §9 | M43 |
| §10〜§11 | 共通 |

## 2. ビルド再現性（M37）

### 2.1 CMakePresets.json

ルートに `CMakePresets.json` を追加し、configure /
build / test の入口を統一する。configure preset の用途は次のとおり。generator、build type、
cache 変数、`binaryDir`、継承関係など各 preset の値は `CMakePresets.json` が正典であり、本表には写さない。

| プリセット | 用途 |
|---|---|
| `windows-debug` | 手元・CI の Debug。no-egress の mock テスト |
| `windows-llama-debug` | pin 済みの llama.cpp を取得し、実 Zenzai モデルを読み込める開発用成果物 |
| `windows-clangd` | clangd / Serena 用 compilation database の生成（configure 専用） |
| `windows-release` | 手元・CI の Release。VM 検証パッケージと Release workflow |
| `windows-vulkan-release` | ggml-vulkan を有効にした Release ビルド（`docs/zenzai-gpu-route.md`）。CI は Host のコンパイル・リンク検証に使う |
| `windows-release-arm64` | x64 ホストから Windows on Arm へのクロスビルド（`docs/copilot-pc-backend-spec.md` §8.1） |
| `windows-no-tests` | `AZOOKEY_BUILD_TESTS=OFF` の非テスト target が壊れていないことの検証（§4.3） |
| `linux-debug` | Linux の portable subset の手元・CI ビルドとテスト |
| `linux-coverage` | Linux の portable subset のカバレッジ計測（§4.3） |
| `windows-asan` | Windows ASan 手動・定期実行（§4.6） |
| `linux-asan-ubsan` | Linux ASan + UBSan 手動・定期実行（§4.6） |

基本の configure preset（`windows-debug`、`windows-release`、`linux-debug`）の
`cacheVariables` は `AZOOKEY_BUILD_TESTS=ON`, `AZOOKEY_BUILD_BENCH=ON` を既定とする。
`AZOOKEY_FETCH_GOOGLETEST` は基本 preset の
**既定で ON にしない**（オフラインビルド原則。CI ジョブ側で
明示的に上書きする）。`binaryDir` は `${sourceDir}/build/<preset>` を原則とする。
目的別の preset はこれらを上書きしてよい（bench を OFF にする sanitizer・coverage、tests と bench を
OFF にする ARM64・`windows-no-tests`、GoogleTest を取得し ggml-vulkan の shader compiler のパス長のために
`build/vk` を使う `windows-vulkan-release` など）。各 preset の値は `CMakePresets.json` を正典とする。

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
  MSVC ABI の clang-cl（compiler ID `Clang`、simulate ID `MSVC`）には
  `/utf-8` `/EHsc` `/guard:cf` を適用し、llama.cpp / ggml にも `/guard:cf` を渡す。
  clang-cl に非対応の `/sdl` `/Qspectre` は渡さない。
- `azookey_project_warnings` — 警告レベル（MSVC `/W4`、非 MSVC
  `-Wall -Wextra -Wpedantic`）。各実体 target が `PRIVATE` でリンク。
- `azookey_binary_hardening` — `azookey_tsf_tip.dll` と
  `azookey_inference_host.exe` だけが `PRIVATE` でリンクする。MSVC linker に
  `/GUARD:CF` `/DYNAMICBASE` `/NXCOMPAT` を渡し、x64 では加えて
  `/HIGHENTROPYVA` `/CETCOMPAT` を渡す。`/DEPENDENTLOADFLAG:0xB00` により静的 import の
  探索を DLL load directory、application directory、System32 に限定する。これは current
  directory と `PATH` を除外しつつ、MSI が TIP / Host と同じ `INSTALLFOLDER` に配置する
  app-local VC runtime、および MSVC ASan の隣接 runtime DLL を解決できる構成である。
  clang-cl にも `/GUARD:CF` `/DYNAMICBASE` `/NXCOMPAT` と同じ
  `/DEPENDENTLOADFLAG:0xB00` を適用する。64-bit clang-cl には ARM64 も含め
  `/HIGHENTROPYVA` を明示する。`/CETCOMPAT` は x64 専用のため ARM64 には渡さない。

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

実機検証パッケージ（旧 §2.6）の契約は `docs/vm-verification-package-spec.md` が持つ。

### 2.7 ビルド時間の内訳（実測）

本節は M37 の受け入れ条件ではなく、ビルド時間短縮策の当てどころを決めるための計測の結論である。

clang-cl の `-ftime-trace` で本体ターゲットの Debug ビルドを測ると、コンパイル時間の大半は
frontend にあり、なかでも TU ごとに繰り返すヘッダ展開が支配的である。backend（最適化・コード生成）は
1 割に満たない。重いヘッダ展開は `tsf-tip` に限らず、`ipc`、`diagnostics`、`compat-test` にも出る。
この内訳から当てどころを PCH と unity build に絞り、MSVC での実測で採否を決めた（§11.8）。
clang-cl での内訳は傾向を示すものであり、MSVC での短縮幅の見積もりには使わない。

測定条件、再現手順、上位 TU の表、計測が示さないことは
[`decisions/2026-09-pch-unity-build-measurement.md`](decisions/2026-09-pch-unity-build-measurement.md) にある。

### M37 受け入れ条件

受け入れ条件の定義の正典は [`plans/windows-port-roadmap.md`](../plans/windows-port-roadmap.md)
の M37 節とする。本書は §2・§3 でビルド入口・共通オプション・依存更新手順を定義し、受け入れ条件を複製しない。

## 3. 依存管理方針（M37 / 横断）

### 3.1 基本方針

レビューは `vcpkg.json` の一括導入を提案するが、本プロジェクトは
**オフラインビルド原則**（`dev-infrastructure-spec.md` §1）を持つため、vcpkg を必須化しない。依存ごとに
次の優先順で導入手段を選ぶ。

1. header-only ライブラリ → submodule または `FetchContent`（オプトイン）
2. ビルドが必要な依存 → 導入価値を個別評価し、必要なら `FetchContent` を
   明示オプトインで
3. 外部から取得する prebuilt バイナリ / モデル（llama.cpp ランタイム・GGUF 等）は
   **供給源を固定し SHA256 等でハッシュ pin** する。先行実装 fkunn1326/azooKey-Windows が
   CI で個人 gist / 個人 fork の成果物をチェックサム無しで取得しているのは**反面教師**とし、
   自分側は採らない（サプライチェーン耐性）。

### 3.2 依存ごとの導入手段

| 依存 | 種別 | 導入手段 | 採否 |
|---|---|---|---|
| GoogleTest | テスト | `find_package` →（オプトイン時）`FetchContent` | 採用（本トラックで変更しない） |
| llama.cpp | 推論 | `FetchContent` / submodule、`AZOOKEY_BACKEND` で制御 | 採用（M8 の範囲。本トラック対象外） |
| WIL | header-only | submodule または `FetchContent`（オプトイン） | 採用（M43 の範囲。§9） |
| nlohmann-json | header-only | 任意・保留 | `ipc-persistence-hardening-spec.md` §6・§11 参照（即時導入しない） |
| spdlog | 要ビルド | 任意・保留 | `observability-spec.md` §7・§11 参照（自前ロガーを優先評価） |
| miniz | 要ビルド（C ソース 2 ファイル） | 導入しない | M49 の ZIP backup は STORE 方式だけの自前実装で足りるため採用しない（`docs/learning-data-management-spec.md` §7） |

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

#### llama.cpp pin 更新時の再測定手順

上表の比較を llama.cpp で行うときの不変条件と手順を定める。数値は本節に書かず、
更新 PR の本文に載せる。

不変条件:

* 前後のビルドは**同じ azooKey ソース**から作り、違いを
  `AZOOKEY_LLAMA_CPP_SOURCE_DIR` だけにする。前後の llama.cpp は、
  リポジトリ外に置いた上流 clone の `git worktree` で、旧 pin と新 pin を checkout する。
  別の checkout の既存ビルドは、azooKey 側のソースが違うので基準にしない。
* 両方とも `windows-release` preset（Release、CPU、`AZOOKEY_BUILD_BENCH=ON`）で、
  `-B <旧 pin 用・新 pin 用の別ディレクトリ>` と `-DAZOOKEY_ZENZAI_TEST_MODEL=<GGUF>` を付けて構成する。
  preset の既定の build directory を共有すると、後の configure が前の構成を上書きする。
  モデルの SHA256 を記録する。
* 計測の直前と直後に、同じマシンでコンパイラ・リンカ・ビルドツールのプロセス
  （`cl`、`link`、`ninja`、`MSBuild`、`clang-cl`、`cargo`、`rustc`）が
  動いていないことを確かめて記録する。
* 前後を交互（ABAB）に実行し、各条件を同じ回数・同じ引数で測る。
  スレッド数は `zenzai-inference-spec` §8.1.1 の上限値 8 に固定する。
* OS のファイルキャッシュは消せない。「cold」は、プロセス起動直後の 1 回目
  （モデルを再マップした直後）の値と定義して明記する。

手順と判断基準:

| 比較項目 | コマンド | 回帰と見なす条件（出典） |
|---|---|---|
| モデルロード | `ctest -L zenzai-real-model`。prompt token ids は、同ラベルのうち `--expected-prompt-token-ids` を指定した smoke で照合する | 1 件でも失敗 |
| ロード時間 | 下記レイテンシ行の `azookey_zenzai_bench` の text 出力にある `load_ms` を記録する | 計測のみでゲートしない（`zenzai-inference-spec` §8）。増減があれば llama.cpp のロードログで内訳を示す |
| 代表プロンプト出力 | `azookey_bench --eval bench/data/kana_kanji_eval.jsonl --model <GGUF> --per-case <path> --output <path>` を前後 2 回ずつ。per-case の候補列も前後で比べる | `top1_accuracy` が −0.5pp を超えて悪化（`conversion-quality-benchmark-spec` §14.3） |
| レイテンシ | `azookey_zenzai_bench --model <GGUF> --input <読み> --threads 8 --warmup 5 --iterations 50 --output <path>` を短い読みと長い読みで、前後 3 セットずつ。text 出力の `load_ms` と `top_debug_info` も残す。新 pin の計測で旧 pin の JSON を `--baseline` に渡すと、右欄の §4.5 判定が出力に入る | p95 / p99 が 10% 超かつ 0.05ms 超の悪化（§4.5）。読み 8 文字以下の p50 / p95 目標と打ち切り率（`zenzai-inference-spec` §8.1、§8.2） |
| メモリ | 上記 bench プロセスの `PeakWorkingSet64` と、品質ベンチ出力の `memory_peak_mb` | 明確な増加があれば理由を特定する（上表） |

生成スコア（`top_debug_info` の `lp`）と per-case の候補列は前後で比べ、差があれば PR 本文に記録する。

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

本節は CI の品質ゲートの方針（何をどの経路で検査し、何を必須、何を advisory にするか）を持つ。
ジョブの一覧と起動条件は `docs/test-inventory.md`「CTest 以外の自動検査」が正典で、
`scripts/check_test_inventory.py` が workflow と突き合わせる。runner image、action とツールの版、
artifact の保持期間、schedule の値は `.github/workflows/` が正典であり、本書には写さない。

### 4.1 Debug/Release マトリクス

```yaml
strategy:
  matrix:
    config: [Debug, Release]
```

runner は安定性のため、Windows と Linux のどちらもすべての workflow で版を明示した image を使い、
`windows-latest` / `ubuntu-latest` の暗黙更新を避ける（SBOM は matrix の値として指定する）。

### 4.2 preset 利用

CI の configure/build/test を §2.1 のプリセット経由に統一する。CI ジョブは
`AZOOKEY_FETCH_GOOGLETEST=ON` をジョブ側で明示指定する（プリセット既定は
OFF のまま）。

### 4.3 追加チェック

- 英単語辞書 porter — `linux-build` は CTest の前に
  `scripts/tests/test_english_words_porter.py` を実行し、porter の決定性、
  同梱 `data/english-words.tsv` の形式と件数、`THIRD_PARTY_LICENSES` の全文集約を検証する。
  同じジョブで `scripts/tests/test_generate_mixed_english_eval.py` を実行し、
  評価セット生成の決定性と同梱 fixture の鮮度を検証する。
- trending アセットの公開前 validator — Build workflow の `quality`（表示名 `Pre-commit`）
  ジョブで `python scripts/tests/test_validate_trending_asset.py` を実行する。
  runtime の入力形式・サイズ・Unicode 制約、元バイト列の SHA256 と checksum 出力の保持を検証する。
  実データの出典・利用許諾・内容のレビューと公開操作は
  [`handoff/trending-asset-publication.md`](handoff/trending-asset-publication.md) に従う。
- 配布バイナリの app-local MSVC runtime — `windows-llama-build` は Release / CPU / llama.cpp
  構成で TIP、Host、self-contained 設定アプリをビルドし、MSVC 開発者環境から
  `scripts/check-app-local-runtime.ps1` を実行する。`Package.wxs` の個別ファイルと設定アプリの
  harvest payload を照合先にして、MSI に入らないランタイムの import はジョブを失敗させる。
  build 対象の PR、`main` push、手動 dispatch で実行し、結果を `runtime-llama.log` として
  build log artifact に残す。同梱契約は `sideload-packaging-spec.md` §4.1 が正典であり、
  Release workflow の MSI 生成前の検査も維持する。
- Agent 指示予算 — root `AGENTS.md` の UTF-8 byte size は 12 KiB を目標、
  16 KiB を上限とする。目標超過は GitHub Actions annotation で可視化し、
  上限超過は非 0 終了で PR を停止する。機械利用時の `--json` は単一行を保つ。
  実装は `scripts/check_agent_instruction_size.py`。`dolquis/agent-ops` から
  ベンダリングしたコピーであり、閾値・終了コード・出力形式の正典は origin にある。
  この repo では編集せず、変更は origin で行って配布し直す。本節は azooKey が
  この検査を CI のどのジョブで走らせるかを定義する。
- agent 定義の突合 — `scripts/check_agent_definitions.toml` に宣言したカスタム agent（shared を含む）は、
  `.claude/agents/<name>.md` と `.codex/agents/<name>.toml` で `name`・`description`・本文を
  一致させ、設定の `write_boundary`（`read-only` / `build-write`）を検査上の書込み境界として扱う。
  Claude 側は `disallowedTools` と read-only の `hooks.PreToolUse` で制限し、Codex 側は
  親の sandbox と承認設定を継承する。`scripts/check_agent_definitions.py` が `docs-lint`
  ジョブで本文・型・既知の無効な override と MANIFEST の agent 名集合を検査する。全 schema や実効権限の検査ではない。
  `[agents].enabled = false` は親による代替実行を許す有効な設定とする。
  Claude の `tools` が `Agent` / `Task`（`Agent(...)` / `Task(...)` を含む）を含まないことも同じ script が見る。分担の規約は `docs/handoff/agent-orchestration.md`。
- Codex の限定 push — `scripts/codex_push.py` は指定 branch と送信先を検査し、明示した
  commit と宛先 ref だけを送る。利用条件は `docs/handoff/agent-tooling-setup.md` が定義する。
  `docs-lint` ジョブで `scripts/tests/test_codex_push.py` を実行し、保護対象や曖昧な送信先の拒否、
  送信範囲の固定を外部 push なしで検査する。
- `clang-format` 変更行ゲート（`cpp-format` ジョブ、`git-clang-format`）—
  PR の**変更行のみ**を必須チェックする。roadmap M37 の受け入れ条件「`clang-format
  --dry-run` が新規追加コードに対して差分ゼロ」に対応する。既存負債（未整形の
  既存行）では PR を止めない。全体整形 PR とツリー全体の
  `clang-format --dry-run --Werror` 必須化は、既存差分が大きいため別作業として
  分離する（§2.3）
- Linux 補助ジョブ — `core` / `ipc` / `learning` / `inference-host` の
  非 Windows 依存部分のみをビルド・テストし、移植性回帰を早期検出する
- Linux coverage ジョブ — Clang の source-based coverage で portable subset
  （`core` / `ipc` / `learning` / `inference-host`）の行・分岐カバレッジを測る。
  tests / `third_party` / `build` は集計から除外し、HTML・LCOV・JSON summary を
  `linux-llvm-coverage` artifact として残す。初期段階では数値を可視化する
  だけで、目標値を下回ってもジョブを失敗させない。一方、profile が生成されない、
  またはレポート生成に失敗するなど計測基盤が壊れた場合はジョブを失敗させる。
- Windows coverage ジョブ — 版を固定し SHA256 を検証した OpenCppCoverage の installer を使い、
  Windows Debug の CTest 子プロセスを 2 台の runner に分割して計測する。
  除外適用後の一覧を名前順に交互配分し、manifest と JUnit で各 shard の実行集合と
  全 shard の重複・欠落がないことを検証する。各 shard の native coverage を統合してから
  HTML と Cobertura を生成する。tests / `third_party` / `build` を
  集計から除き、HTML と Cobertura を `windows-opencppcoverage` artifact として
  残す。`NamedPipeTransport.cpp` を含む Windows 専用コードも対象とし、
  デバッガ下で挙動が変わるクラッシュ注入テストと、計測時に時間制限を超える
  大量ログ境界テストは計測 CTest から除く。通常の Windows Debug ジョブでは実行する。
  Linux の portable subset とは別系列で可視化する。数値閾値は設けない。
  計測・レポート生成の失敗はジョブと `advisory-summary` に表示する。
  `ci-gate` はこのジョブの完了を待たず、結果を合否判定に含めない
- bench smoke — `azookey_bench` を CTest から exit=0 で実行（§4.5）
- M63 の分割器の品質ゲート — CTest `azookey_mixed_english_quality_gate` が
  `bench/data/mixed_english_eval.jsonl` と同梱の英単語辞書で、`docs/mixed-english-auto-conversion-spec.md`
  §15.2 の分割器の指標を確かめる（モデルは使わない）。期待する表層は辞書の最頻の表層に従うので、
  `data/english-words.tsv` を更新したら `python3 scripts/generate_mixed_english_eval.py` で評価セットを
  作り直す
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
- ARM64 binary hardening — ARM64 クロスビルドの TIP / Host を
  `scripts/check-arm64-hardening.ps1` で検査し、CFG、ASLR、NX、High Entropy VA と `DependentLoadFlags=0xB00` を
  必須ゲートにする。検査項目と実行経路は `copilot-pc-backend-spec.md` §8.1 を参照する。
  x64 Release の BinSkim advisory とは独立して扱う。
- GitHub Actions supply-chain pin — 外部 Action の `uses:` はフル 40 桁 commit SHA へ
  固定し、対応するリリースタグを行末コメントに残す。Dependabot の
  `github-actions` ecosystem を週次実行し、更新をまとめた PR で SHA を追従する
- settings JSON Schema — 版を固定した `check-jsonschema` で
  `settings/mvp-settings.schema.json` と `settings/model-catalog.schema.json` の meta-schema 妥当性と、
  `settings/default-settings.sample.json` と `settings/default-model-catalog.json` の schema 適合性を確認する
- PowerShell quality gate — `scripts/test-powershell-quality.ps1` から
  `PSScriptAnalyzer` と `Pester` を実行し、`scripts/register-dev.ps1` /
  `scripts/unregister-dev.ps1` / `compat-test/*.ps1` の静的解析と、実
  HKLM 書き込みを伴わない分岐・引数構築テストを行う。pre-commit は
  `language: system` でローカル導入済みモジュールを使う。CI は Pester の版を
  job の `PESTER_VERSION` で指定し、同じ wrapper に `-PesterVersion` で渡す。
  wrapper は指定版を3要素に正規化して比較し、プレリリース版を除外する。
  wrapper は指定版がなければ停止し、実行する版をログへ出す。
  版の指定がないローカル実行では、導入済みの最新版を使う。
- クラッシュ収集の期限付き probe は `RUN_SERIAL` で登録し、並列 CTest の負荷と分離する。

専門ジョブ（`settings-schema`、`powershell-quality`、`cpp-tidy`、`windows-coverage` など）は、先行する
`changes` ジョブが検査対象の入力の変更を検出した場合だけ起動する。各ジョブの起動条件は
`docs/test-inventory.md` の「実行系統」列、入力の分類は `windows.yml` の `changes` ジョブの分類コメントが持つ。
Markdown・`docs/`・`plans/` だけの変更ではこれらを起動しない。
手動実行と `.github/workflows/windows.yml` 自体の変更では、変更判定を含む構成を検証するため
これらの専門ジョブをすべて起動する。

clang-tidy / CodeQL は**必須ゲートには含めない**（導入コストが高く、段階導入と
する）。ただし変更 TU の clang-tidy は `cpp-tidy` ジョブで advisory（`continue-on-error`、
非ブロッキング）として実行し、変更 C++ ソースの静的解析所見を可視化する。
所見の有無で PR を止めない（§11.5）。CodeQL は将来拡張のまま据え置く。
offline configure は GoogleTest を取得せず、変更した portable subset の TU を
`compile_commands.json` の登録と照合する。DB 外の TU（取得を省いたテストなど）は
解析せず skip とし、job summary に成功・skip・解析失敗の件数、skip と失敗の path・
理由を表示する。configure や compile DB の取得に失敗した場合も summary に記録し、
古い DB で解析を続行しない。advisory の成功は全変更 TU の解析完了を意味しない。
なお `clang-format`（必須）と `clang-tidy`（advisory）は独立ジョブ
（`cpp-format` / `cpp-tidy`）に分割し、GitHub Actions 上で並行実行して CI 全体の
所要時間を短縮する。加えてワークフロー全体に `concurrency` グループを設定し、
同一 PR で新しい push が来たら進行中の run を畳む（`main` への push は畳まない）。

BinSkim も既存所見を可視化する段階では **advisory**（`continue-on-error`、
非ブロッキング）とし、所見や解析基盤の一時障害で PR を止めない。必須ゲート化は
`azookey_settings.exe` を含む所見の整理と運用実績を得てから別途判断する（§11.5）。

### 4.4 artifact 整理

configure / build / test の各ログ、Release ビルドの `.pdb`、Release binary hardening の
SARIF・`dumpbin` 出力・要約、Linux coverage の HTML・LCOV・JSON summary、
Windows coverage の HTML・Cobertura を
artifact として保存する。PR diagnostic コメントは
マトリクスの config ごとの結果を反映する。保持期間は workflow の `retention-days` が定める。

### 4.5 bench smoke と回帰監視

target `azookey_bench` を CTest（`azookey_bench_smoke`）に登録し、CI で exit=0 と CPU `SimpleConverter`
経路の p95 < 50ms を確認する。
`azookey_zenzai_bench` は llama.cpp 無効構成の mock smoke と、pin 済み実モデルを使う
llama.cpp 構成の smoke を CTest へ登録する。
実モデルの `zenzai-real-model` ラベルには、非ライブ複数候補の最上位で
`けいやくこうしんのじょうけんをきょうぎする` → `契約更新の条件を協議する` を
厳密一致で検証する読み末尾欠けの回帰テストを含める。

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
| `ipcPhases` | object（任意） | `azookey_bench --ipc` の codec、試験用 pipe 往復、実 Dispatcher 経由の送信 / Host / 受信の計測 |

`azookey_bench --ipc` は固定の10候補 payload を使い、20回の warmup 後に200回計測する。
`ipcPhases` は `samples`、`payloadBytes` と、`serializeMs`、`framingMs`、
`deserializeMs`、`pipeRoundTripMs` の各 `p50` / `p95` / `p99` / `max` を持つ。
framing は length-prefix の encode と decode の合計、pipe 往復は Windows の
同一プロセス内 echo server に対する Send / Receive の合計で、両端の codec を含む。
接続確立、payload の Build / 型別 Parse、推論はこれらの計測に含めない。
非 Windows では `pipeRoundTripMs=null` とし、ゼロ時間として扱わない。

`ipcPhases` は、同じ warmup と標本数で実 `Dispatcher`（モデル無しの `InferenceEngine`）へ
`QueryCandidates` を往復させた `sendMs`、`hostComputeMs`、`receiveMs` も持つ。
各 field は `p50` / `p95` / `p99` / `max` を持ち、1 回の往復を次の 3 区間に分けて
同じ `steady_clock` で測る。3 区間の和は往復全体に一致する。

| field | 区間 |
|---|---|
| `sendMs` | client の `Send` 開始から、Host の handler 入口まで。client の書込と server の読取・起床を含む |
| `hostComputeMs` | Host の handler 入口から出口まで（`Dispatcher::Dispatch`） |
| `receiveMs` | Host の handler 出口から、client が応答を読み終えるまで。server の書込と client の読取を含む |

読み方の注意: bench は Host と client を同一プロセスに置くため、区間の境界を同じ時計で
測れる。一方で `sendMs` と `receiveMs` は転送だけでなく待機と起床を含む。
`hostComputeMs` は `Dispatcher::Dispatch` の呼び出し全体で、推論と rerank に加え、要求の parse、
要求登録、応答の JSON 生成も含むため、`latencyMs`（変換器の呼び出しだけ）より大きい。
Host 側の `InferenceEngine` は `--ipc` のために別に作り、変換器は `SimpleConverter`、
model path は空、trace の出力先は無しとする。`--model` や `--trace` を併用しても
`hostComputeMs` は実モデルの推論を含まず、`--trace` の JSONL にも入らない。
推論のフェーズ別の内訳は、`azookey_bench --trace` の通常の計測（`--ipc` の外）を
`azookey_trace_viewer` で集計する。
これらも非 Windows では出力しない（`pipeRoundTripMs=null` と同じ理由）。

UI 反映（`ui_apply`）は `TextService` の中で計時され、実機の `AZOOKEY_LOG` と
`azookey_trace_viewer` で集計する。bench は TSF の編集セッションと候補ウィンドウを持たず測れないため、
`ipcPhases` の対象外とする。
既定の `latencyMs` と baseline 比較は推論時間のままとし、IPC 時間を加算しない。
`--ipc` は `--eval` と併用せず、pipe の作成・通信失敗時は非ゼロ終了する。

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
コンパイルには §4.7 の sccache を使い、bench の計測条件は変更しない。
workflow は直前の成功 run から `benchmark-results` artifact を取得できる場合だけ baseline
比較を行い、悪化を GitHub Actions warning として表示する。
今回の結果は両 bench の JSON を含む `benchmark-results` artifact として保存する。
bench が閾値超過などで非ゼロ終了した場合も、生成済みの JSON を先に artifact として保存し、
その後で job を失敗させる。
初回実行、artifact 期限切れ、run 一覧取得または download の失敗は regression と判定せず、
当該 run の計測を継続する。

独立した `quality-benchmarks.yml` の `quality-benchmark` ジョブは Windows hosted runner で、モデルなしの CPU
経路を使い `kana_kanji_eval.jsonl` と `typo_eval.jsonl` を `--eval` で採取する。
SHA-256 と NFKC の Windows API を使うため Windows で採取する。baseline は各 workflow の成功 run
から選び、品質 job とレイテンシ job の成否が互いの baseline 更新に影響しないようにする。
`scripts/run-quality-bench.py` は評価 JSON の config と品質 workflow の直前の main 成功 run の
`conversion-quality-results` artifact を照合し、互換な baseline にだけ `--baseline` を渡す。
互換キーと指標の正典は `conversion-quality-benchmark-spec.md` §8 / §14.1 とする。
baseline 不在、破損、version / config 不一致、指標欠落は比較スキップとし、回帰 warning を出さない。
top1_accuracy / exact_match_rate の低下と CER の増加は、カテゴリ別を含め warning として通知する。
top5_accuracy、NFKC 系とレイテンシはこの品質 warning の対象にしない。
品質 JSON と比較状態・理由・差分を含む `comparison.json` は
`conversion-quality-results` artifact として保存する。
品質悪化だけでは終了コードを変更せず、CLI の非ゼロ終了は artifact 保存後に job を失敗させる。
定期 workflow は PR で起動せず、通常 PR の required check に追加しない。
reporting の Python テストは `windows.yml` の `quality` ジョブで実行し、
既存の `changes` 分類により docs-only PR では省略する。

このモデルなし採取は、評価 JSON の schema と artifact 保存・比較経路を確認する smoke である。
SimpleConverter の静的辞書では当該データセットの top1 / top5 / exact が 0 となるため、
精度低下の検知を実証できない。CER の差分は記録できるが、実辞書または固定モデルを用いた
変換品質回帰の受入検証は別途必要とする。smoke の成功を実品質回帰検知の達成とみなさない。

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
`Testing/Temporary` の診断ログを、成功・失敗にかかわらず artifact として
保存する。Windows preset は x64 のみを対象とし、MSVC ASan と非互換な `/RTC1` と
incremental link を sanitizer 設定内で無効化する。
GoogleTest は同じ ASan オプションでソースからビルドし、テスト検出前に ASan runtime
DLL を各実行ファイルのディレクトリへ配置する。Linux preset は Clang を使い、leak
detection と未定義動作検出を fail-fast で有効化する。CTest の後に `ipc/testdata/` を
seed corpus として `ipc_json_fuzzer` を 60 秒実行し、クラッシュ検体を診断 artifact に
含める。この実行は週次 schedule と手動 dispatch に限り、PR の required check にはしない。
CI では build を 4 並列、CTest を 2 並列で実行し、compiler cache は無効にする。

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

CI のコンパイルジョブは `mozilla-actions/sccache-action` で sccache を導入し（導入するジョブは各 workflow の
step が正典。`AZOOKEY_USE_COMPILER_CACHE=OFF` の preset を使う sanitizer と Linux coverage は使わない）、GitHub
Actions のキャッシュサービス（`SCCACHE_GHA_ENABLED`）でオブジェクトを run 間再利用
する。ルート `CMakeLists.txt` が sccache を PATH 上で自動検出して compiler launcher
に設定するため（`AZOOKEY_USE_COMPILER_CACHE` 既定 ON）、ワークフロー側の追加設定は
不要。MSVC ではキャッシュ互換のため Debug / RelWithDebInfo のデバッグ情報を `/Z7`
（Embedded）へ切り替える。この切り替えは Release を対象外とするため、Release の
`.pdb` artifact（§4.4）は影響を受けない。各ジョブ末尾で `sccache --show-stats` で
ヒット率を可視化する。

`windows-llama-build` と `windows-vulkan-compile` はビルドを 2 並列で実行する。
Windows coverage はキャッシュを使用しても最終リンクで PDB を生成し、
OpenCppCoverage の計測対象と除外条件を維持する。計測 CTest は各 shard 内で直列に実行する。
Release workflow は `SCCACHE_GHA_VERSION=release-v1` でキャッシュを分離する。
設定アプリの MSBuild はこの compiler cache の対象外とする。

設定アプリの NuGet パッケージは別に扱う。`windows.yml` で `azookey_settings` を
ビルドするジョブは、NuGet の global packages folder（`~/.nuget/packages`）を
`actions/cache` で run 間再利用する。キーは `settings-app/packages.lock.json` の hash で、
restore-keys は置かない。lock ファイルが変われば新しいキーで取得し直し、古いパッケージを
持ち越さない。キャッシュが無いときは MSBuild の restore が nuget.org から取得するため、
ビルドの成否には影響しない。Release workflow はこのキャッシュを使わず、毎回 nuget.org から
restore する。

`quality` ジョブは actionlint / yamlfmt / taplo の固定版バイナリを、runner の OS image・
architecture と各ツールのバージョンを含むキーでキャッシュする。
完全一致するキャッシュが無い場合は固定版ソースから構築し、復元後もツールを実行してから
pre-commit の既存 hook を検証する。pre-commit 自体は固定版を毎回導入する。

Windows の Ninja build では、ルート `CMakeLists.txt` が compiler launcher の先頭に
`cmake -E env VSLANG=1033` を追加する。
CMake が生成する英語の `/showIncludes` prefix と MSVC の出力言語を一致させ、Ninja が
header dependency を記録できるようにするためである。
この環境指定は compiler cache の有効・無効にかかわらず適用し、sccache などの既存
launcher は環境指定の後段へ保持する。

**sccache-action は v2 Cache API に対応する `v0.0.9` 以降を pin する。** 現行 sccache は
v2 サービスの env（`ACTIONS_RESULTS_URL`）を要求し、旧 (v1) API の env だけを渡すタグでは
sccache サーバの起動が失敗して、compiler launcher 経由で全コンパイルが失敗する。あわせて
`SCCACHE_IGNORE_SERVER_IO_ERROR=1` を設定し、実行時にキャッシュ backend が失敗しても
ローカルコンパイルへ fallback してビルドを緑に保つ（キャッシュはビルドを壊してはならない）。
この規則を決めた失敗の経緯は
[`decisions/2026-07-sccache-action-cache-api-failure.md`](decisions/2026-07-sccache-action-cache-api-failure.md) にある。

### 4.8 CTest の並列実行

通常の Debug / Release CTest は 4 ケースを上限に並列実行する。
`CMakePresets.json` の通常 test preset を並列度の正典とし、`just test` と CI は
test preset の設定をそのまま引き継ぐ。
`azookey_check` は `AZOOKEY_CTEST_PARALLEL_JOBS`（既定値 4）を使う。
固定値にするのは、ローカルと CI の論理プロセッサ数が異なっても同じ負荷条件で
テストできるようにするためである。
ASan / UBSan preset は並列設定を継承せず、ローカルでは直列で実行する。
定期 CI は §4.6 のとおり CTest の並列度を 2 に上書きする。

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
| `CI gate` | `.github/workflows/windows.yml` |

`.github/workflows/windows.yml` はワークフローレベルの `paths` / `paths-ignore` を持たず、
すべての PR で発火する。ドキュメント・計画・エージェント設定・`legacy/` だけの変更で重量ジョブを
省く判定は `changes` ジョブの `build` 出力による `if:` 条件が担い、スキップされたジョブは
`skipped` として報告される。`build` の判定は base commit を取得できない場合と
`workflow_dispatch` で `true` へ倒し、偽陰性スキップを作らない。

終端の `ci-gate`（表示名 `CI gate`）は `if: always()` で必ず実行され、`needs` に列挙した
全ジョブの結果を集約する。`failure` または `cancelled` が 1 件でもあれば fail し、
`success` と `skipped` だけなら pass する。`windows.yml` にジョブを追加するときは
判定対象なら `ci-gate` の `needs` にも加える。加えなければそのジョブの失敗が required check を素通りする。

例外は advisory ジョブの `cpp-tidy`（§4.3 / §11.5）と
`windows-coverage`（§4.3 / §10）で、`ci-gate` の `needs` に含めず、完了も待たない。
両ジョブの結果は `if: always()` の `advisory-summary` がログと job summary に表示する。
解析・依存導入・configure の失敗は元ジョブに残し、必須チェックの合否には含めない。
`advisory-summary` 自体も `ci-gate` の判定対象にしない。

`.github/workflows/sbom.yml` は `paths` で絞られるため対象外とする。
`.github/workflows/docs.yml` は `paths` を持たずすべての PR で発火するので、`docs-lint` と `docs-impact`（push と Dependabot の PR では `if:` で skipped になる）は本節の基準を満たす。required に加えるかは ruleset の変更として別に判断する。
`.github/workflows/compat.yml` の `Notepad / VS Code / Edge` はラベル付与時のみ実行する
対話ジョブであり、required にしても判定に寄与しない。

### M38 受け入れ条件

受け入れ条件の定義の正典は [`plans/windows-port-roadmap.md`](../plans/windows-port-roadmap.md)
の M38 節とする。本書は §4 で CI のジョブ構成と品質ゲートを定義し、受け入れ条件を複製しない。

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

受け入れ条件の定義の正典は [`plans/windows-port-roadmap.md`](../plans/windows-port-roadmap.md)
の M43 節とする。本書は §9 で WIL の取り込み方と適用範囲を定義し、受け入れ条件を複製しない。

## 10. 成果測定 KPI

レビュー（プロジェクトレビュー 2026-05-22）が提案する KPI を、本トラックの
効果測定指標として記録する。これらは目標値であり、達成可否のゲートは各
マイルストーンの受け入れ条件側で判定する。

| KPI | 目標値 | 関連 |
|---|---|---|
| 候補表示レイテンシ | p50 < 80ms / p95 < 180ms | §4.5, `observability-spec.md` §7.3 |
| Host 異常停止時の復旧時間 | 中央値 3 秒以内 | `host-availability-spec.md` §8.3 |
| CI 成功率 | main 連続 14 日で > 95% | §4 |
| Portable C++ 行カバレッジ | 80%（初期段階は informational） | §4.3 |
| 学習永続化の破損率 | 0%（クラッシュ注入テストで確認） | `ipc-persistence-hardening-spec.md` §5.4 |

Portable C++ 行カバレッジは Linux coverage ジョブの `summary.txt` と `summary.json` で
継続観測する。
80% は目標値であり、初期段階では PR の合否条件にしない。
Windows 専用コードを含む値は OpenCppCoverage の Cobertura と HTML で別系列として
観測し、portable subset の履歴へ混在させない。Windows 系列に 80% の目標値は
適用せず、閾値ゲート化は別途判断する。

## 11. 不採用とした提案

レビューと外部リサーチの提案のうち、本トラックで採用しない / 修正して採用するものの結論と
再評価の条件を置く。§11.1〜§11.7 の提案ごとの理由は
[`decisions/2026-07-dev-infrastructure-rejected-proposals.md`](decisions/2026-07-dev-infrastructure-rejected-proposals.md) にある。
§11.2.1 と §11.8 の経緯は各節のリンク先にある。

### 11.1 vcpkg.json の一括導入 — 修正採用

vcpkg を必須にしない。オフラインビルド原則（`dev-infrastructure-spec.md` §1）と衝突するためである。依存ごとに
header-only 優先・`FetchContent`/submodule オプトインで導入し（§3）、`vcpkg.json` は
任意の利便ファイルとしてのみ許容する。

### 11.2 自前 JSON パーサの即時全置換 — 修正採用

`nlohmann-json` への即時全置換は採らず、自前パーサの堅牢化と fuzz/malformed テストを主とする（`ipc-persistence-hardening-spec.md` §6）。
外部ライブラリへの移行の方針は §11.2.1 が持つ。

#### 11.2.1 外部ライブラリ移行の方針

**v1.0 の範囲では自前パーサを維持し、外部ライブラリへは移行しない。** 移行の動機だった
自前実装に起因するバグクラスは、§6.2 のパーサ強化で閉じている。一方で移行のコストは残る。
自前パーサは `ipc-persistence-hardening-spec.md` §6.2 の上限値と wire contract を spec と `static_assert` で結び付けており、
置換すればこの対応関係を作り直すことになる。TIP は任意アプリへ in-proc ロードされる DLL であり、
依存追加はコンパイル時間とバイナリサイズに直接効く。no-egress 方針のもとでは FetchContent の
ピン留めと `AZOOKEY_FETCH_*=OFF` 既定も前提として維持しなければならない。

再評価は次のいずれかが起きたときに行う。

* `ipc-persistence-hardening-spec.md` §6.2 の範囲を超える JSON 要件が生じる（streaming parse、JSON Pointer、
  schema validation など、自前実装への追加が大きくなる機能）
* IPC schema の大規模な作り直しが決まり、payload codec を書き直す機会が生じる
  （このとき RapidJSON を新規ペイロードだけに部分導入し、性能・コンパイル時間・
  バイナリサイズを実測する）
* DEV-211 の bench で JSON parse / stringify 自体がレイテンシのボトルネックだと
  示される（往復回数の削減で足りるなら移行の根拠にはならない）

いずれも v1.0 のリリースゲートではない。設定ファイル側（`SettingsStore` /
`UserDictionary`）はホットパスでないため、部分導入を試すなら先行させる候補になる。
決定の経緯は
[`decisions/2026-08-json-parser-external-library.md`](decisions/2026-08-json-parser-external-library.md) にある。

### 11.3 Zenzai runtime 抽象 `IModelRuntime` の新設 — 不採用

`IModelRuntime` を新設しない。`core::IConverter` が変換バックエンドの差し替え可能境界を既に担い、
M8 も Zenzai を `IConverter` 実装として `SimpleConverter` と差し替えるため、二重抽象になる。
バックエンド別実装の内部構造は M8 / M24 の範囲で `IConverter` 実装として整理する。

### 11.4 packaging / signing 足場の新設 — 不採用（重複）

本トラックでは packaging / signing の足場を新設しない。MSIX・WiX・署名 CI・DPAPI 暗号化は
M11 / M12 / M28〜M34 が扱う。開発用の登録スクリプトは `scripts/register-dev.ps1` /
`scripts/unregister-dev.ps1` である。

### 11.5 clang-tidy / CodeQL の必須化 — 見送り

clang-tidy / CodeQL は必須ゲートにしない。必須とするのは `clang-format`（変更行ゲート）までである
（§4.3）。変更 TU 全体の clang-tidy 解析は、既存負債で PR が止まるのを避けつつ変更コードの品質を
上げる中間策として、`cpp-tidy` ジョブで **advisory（非ブロッキング）**として走らせ、所見の可視化に
留める。必須ゲート化・全体化（全ソースへの拡大、CodeQL 追加）は将来判断とし Linear で追跡する。

同じ段階導入方針を Release バイナリの BinSkim にも適用する。`continue-on-error` の advisory とし、
SARIF・job summary・artifact に所見を残す。未署名に由来する `BA2022` だけを抑止し、他のルール結果を
隠さない。必須ゲート化は、所見の基準線を整理し、ツール取得と解析の安定性を確認した後に判断する。

### 11.6 IPC payload の Protobuf 即時移行 — 不採用

Named Pipe の payload は JSON のまま維持し、§6 の境界コーパスと有界スモークで自前パーサを
堅牢化する（§11.2 と整合）。同一マシン内・単一実装・少数メッセージの IPC では、schema 進化の
恩恵よりも、コード生成ツールチェーンの追加がオフラインビルド原則（`dev-infrastructure-spec.md` §1）へ与える影響と移行コストが
上回る。Protobuf 化は将来判断とし、設定アプリの実装と fuzz の整備の後に再評価する。

### 11.7 設定アプリの C#/.NET 9 化 — 不採用

設定アプリは WinUI 3（C++/WinRT）を維持する。UI フレームワークの選定と再評価の条件は
`docs/settings-app-spec.md` §3.0 が正典である。

### 11.8 PCH — 不採用 / unity build — 見送り

§2.7 の内訳を受けて、PCH と unity build を MSVC で「有無 × sccache の cold / warm」の 4 条件で
実測した。**PCH は不採用、unity build は見送りとする。**

- **PCH**: cold は短縮するが warm は悪化する。sccache は PCH 関連フラグ（`/Fp`、`/Yc`）を持つ
  コンパイルをキャッシュ対象から外すため、PCH を使う TU は cache が warm でも毎回コンパイルされる。
  CI（§4.7）と日常の開発ループでは warm 条件が支配的である。PCH をローカル専用 preset に限る構成も、
  ビルド構成が二重化して CI が通らない経路ができ、ブランチ切り替え後のフルビルドでは warm 条件の
  悪化を受けるため採らない。PCH の適用には DEV-1086 の解消も前提となる。
- **unity build**: warm でも改善し、全 TU がキャッシュ可能なまま（hit 率 100%）である。それでも
  見送るのは順序の問題による。削減量のおよそ半分はテスト target が同じソースを重複コンパイルしている
  分であり、unity build の意味論的リスクを負わずに構造的に除ける（DEV-1087）。また unity batch は
  ソースの並び順で決まるため、ソースを 1 本足すと無関係な batch の内容が変わり、通っていた組み合わせが
  壊れうる。単一 `.cpp` の変更に対する増分ビルドは batch 全体の再コンパイルで遅くなる。

再評価は本節と同じ 4 条件で行う。CMake の再評価用対策として、Win32 `max` マクロとの
衝突を core target の `NOMINMAX` で防ぐ。無名 namespace の同名 helper を持つ
学習ストア、Host CLI / モデル関連ソース、core / Host のテストソースは、該当 target の
`CMakeLists.txt` で衝突する組から必要なソースだけを unity batch から除外する。
同じ契約の helper は内部共通 helper へ集約し、意味が異なる helper はソース固有の名前にする。
これで衝突を解消できる場合は、unity batch からの除外を不要とする。
`CrashReporting.cpp` と `ThreadStackGuarantee.cpp` も別 object にして、TIP DLL が
クラッシュフィルタ本体を取り込まないリンク境界を保つ。これらは unity build の既定採用を
意味しない。
`UNITY_BUILD_BATCH_SIZE` は既定値のまま測っており、調整の余地を残している。

`settings-app` は 4 条件の対象外である。MSBuild が `azookey_settings.vcxproj` を
ビルドする独立経路にあり、CMake の compiler launcher が及ばないため sccache とは
相互作用しない。PCH は `pch.h` と `/Yu` で既に使っている。
`tsf-tip` 以外にも重いヘッダ展開が出ている点は §2.7 のとおりである。

測定条件、4 条件の結果、sccache のヒット率、再現手順（計測用の CMake 足場の commit を含む）は
[`decisions/2026-09-pch-unity-build-measurement.md`](decisions/2026-09-pch-unity-build-measurement.md) にある。
