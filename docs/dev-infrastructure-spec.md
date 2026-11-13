# 開発基盤・品質強化 仕様（Development Infrastructure & Quality）

本書は azooKey-Desktop（Windows ポート）の**開発基盤・ビルド再現性・CI 品質
ゲート**に関する仕様の入口である。IPC/JSON 堅牢性・観測性・Host 可用性は、
下の表と末尾の転送表が示す文書が持つ。対象は Windows 版（C++ 移植: `core/` `ipc/` `learning/` `inference-host/` `tsf-tip/`
`bench/`）のみ。macOS 版 `legacy/` は対象外。

本書は 2 通の第三者レビュー（開発環境・ライブラリ改善レビュー / プロジェクト
レビュー）の指摘を評価・取捨選択した結果を仕様化したものである。
レビュー指摘のうち既存マイルストーン（M8/M11/M12/M28〜M34）と重複するもの、
本プロジェクトの方針と整合しないものは「§11 不採用とした提案」に理由とともに
記録する。

本書は、節番号を保ったまま、節ごとに次の文書へ分けていく。分けた節の本文と正典範囲は移動先が持ち、
本書の転送表（末尾）が旧来の `dev-infrastructure-spec §N` の参照を移動先へ解決する。

| 群 | 節 | 文書 |
|---|---|---|
| IPC・永続化の堅牢化 | §5、§6 | `docs/ipc-persistence-hardening-spec.md` |
| 観測性 | §7 | `docs/observability-spec.md` |
| Host 可用性 | §8 | `docs/host-availability-spec.md` |

## 0. 節とマイルストーンの対応

本書の節と、その範囲を定める `plans/windows-port-roadmap.md` のマイルストーンの対応を次に示す。

| 節 | マイルストーン | 文書 |
|---|---|---|
| §1 | 共通 | 本書 |
| §2〜§3 | M37 | 本書 |
| §4 | M38 | 本書 |
| §5 | M39 | `docs/ipc-persistence-hardening-spec.md` |
| §6 | M40 | `docs/ipc-persistence-hardening-spec.md` |
| §7（§7.7 を除く） | M41 | `docs/observability-spec.md` |
| §7.7 | M51 | `docs/observability-spec.md` |
| §8（§8.5・§8.6 を除く） | M42 | `docs/host-availability-spec.md` |
| §8.5 | M47 | `docs/host-availability-spec.md` |
| §8.6 | M に属さない設計案（roadmap「リスクと不確実性」） | `docs/host-availability-spec.md` |
| §9 | M43 | 本書 |
| §10〜§11 | 共通 | 本書 |
| §12 | M44 | 本書 |
| §13 | M50 | 本書 |

## 1. 目的と非目標

### 目的

MVP（Phase 1〜2）から「実運用に耐える IME」へ進むにあたり、開発基盤・品質の
負債を解消する。
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

### 2.6 実機検証パッケージ

`scripts/make-vm-verify-package.ps1` は Hyper-V Win11 VM へ持ち込む実機検証用
zip を生成する。
入力は CMake configure preset と出力先で、preset の既定値は
`windows-release` とする。
`-ModelPath` の省略には `-AllowNoModel` の明示を必須とする。
`-BundledDictionaryDirectory` は `dictbuild/build_bundled.py` の出力先を受け取り、
既定を `build/bundled-dictionaries` とする。同梱対象は
`dict/sudachi_lexicon.azdic`、`dict/technical_terms_lexicon.azdic`、
`ThirdPartyNotices.txt`、`bundle-provenance.json` で、zip 内でも同じ相対パスを保つ。role は辞書2層が
`bundled-dictionary`、帰属表示が `dictionary-notices`、生成入力の記録が `dictionary-provenance` であり、SHA-256 を他の同梱物と
同様に manifest へ記録する。配布可否と帰属の正典は
`auto-word-registration-spec.md` §14.9 / §14.10 とする。
MSI の `RewriterDataDir` と同じ `data/english-words.tsv`、`data/symbol.tsv`、`data/emoji.tsv` も role `rewriter-data` として同梱する。
既定ディレクトリが存在しない場合だけ、`-AllowNoDictionary` の明示で辞書省略を許可し、
警告を出す。明示したディレクトリの不存在、2層の欠落・空ファイル、帰属表示の欠落・白紙は
このスイッチでも許可しない。辞書省略時は辞書層を前提とする Human Gate の証跡に使わない。
辞書を同梱する場合は Python 3.10 以降で `dictbuild/check_bundle.py --vm-package-json` を実行し、
既存ビルダと同じ `.azdic` 形式検査、層ID、METAの出所・帰属と、生成入力の記録を検査する。
生成時の入力SHA-256・Sudachi revision・取得アーカイブのpinを現在のcheckoutと照合し、
記録の欠落、古い生成器・seed・metadata・notice、出力の不一致を拒否する。
生成入力が同一なら無関係なcommit更新だけで再生成を要求しない。既存のclean判定と組み合わせ、
manifestのcommitに含まれる生成入力と同梱辞書を結び付ける。コピー後の辞書・帰属表示・記録のSHA-256も
検証時の値と一致しなければzipを生成しない。
モデルを指定する場合は CMake cache の `AZOOKEY_FETCH_LLAMA_CPP` または
`AZOOKEY_LLAMA_CPP_SOURCE_DIR` が有効であることを生成時に検査する。
これは登録時の実行時 preflight や実 GGUF 推論の Human Gate を代替しない。

スクリプトは `CMakePresets.json` の `binaryDir` と `CMAKE_BUILD_TYPE` を解決し、
実 build directory の `CMakeCache.txt` と照合する。
さらに `azookey_tsf_tip`、`azookey_inference_host`、`azookey_diag` の Ninja dry-run が
`no work to do` になることを確認する。`-ModelPath` 指定時は
`azookey_zenzai_bench`、`-IncludeCompat` 指定時は `compat_test` も対象にする。
dry-run の間だけ `NINJA_STATUS` を `[%f/%t] ` に固定し、終了時には元の値へ戻す。
bench の commit ヘッダーは Git の HEAD・参照先と override の変更を依存として追跡し、
ビルド直後に変更がなければ、ヘッダー更新や consumer の再ビルドを要求しない。
branch 切替、detached HEAD、linked worktree、packed ref からの更新でも、
次のビルドで記録する commit を更新する。
bench を含む場合に限り、出力が `[1/1] Refreshing benchmark commit header` の
1 行だけで、build directory の既存の `bench/generated/BenchmarkCommit.h` が現在の HEAD または
有効な `AZOOKEY_BENCH_GIT_COMMIT` override と一致する場合も許可する。
ヘッダーの欠落・不一致や、コンパイル・リンクなどの追加作業があれば拒否する。
dry-run の前に `ninja -t deps` の依存ログを走査し、この checkout と build directory の
外にあって、追跡ファイルと同じ相対パスで終わるヘッダーが 1 件でもあれば拒否する。
sccache はヒット時に MSVC の `/showIncludes` 出力を保存時のまま返し、キャッシュキーに
checkout のパスを含めないため、別 checkout でコンパイルした結果の依存が記録されうる。
その依存が指すヘッダーが無ければ dry-run が毎回 stale を報告し、残っていればこの
checkout のヘッダー変更が再コンパイルを起こさない。後者は dry-run では検出できない。
拒否時は `SCCACHE_RECACHE=1` での clean 再ビルドを案内する（`docs/debugging.md`）。
依存ログは同じドライブのパスを build directory からの相対で記録するため、絶対パスへ
戻してから判定する。repository の下にあっても追跡ファイルそのものでないパスは、
`.claude/worktrees/` のような入れ子の checkout とみなして同じ判定にかける。
見出しが `STALE` の記録は Ninja が使わずに再ビルドするため判定から除く。
依存ログは `ninja -n -t deps` で読み、build directory へ書き込まない。
manifest の commit が同梱スクリプトと文書も一意に指すよう、tracked/untracked を
含む作業ツリーが clean であることも要求する。
成果物の欠落、build type の不一致、別 checkout の CMake cache、別 checkout を指す
依存ログ、stale target、未コミット変更のいずれかを検出した場合は、zip を生成せず非ゼロ終了する。
出力先は worktree 外、または `.gitignore` 対象の `build/` 配下とする。
それ以外の worktree 内へ出力すると、次回実行時の clean 判定が前回の成果物を
未追跡ファイルとして検出する。

zip のルートには次を置く。

- `azookey_tsf_tip.dll`
- `azookey_inference_host.exe`
- `azookey_diag.exe`
- `register-dev.ps1`
- `unregister-dev.ps1`
- `host-supervisor.ps1`
- `host-startup-log.ps1`
- `AppContainerAcl.ps1`
- `verify-bootstrap.ps1`
- `learning-data-snapshot.ps1`
- `tip-manual-verification-checklist.md`
- `dict/`（`sudachi_lexicon.azdic`、`technical_terms_lexicon.azdic`。辞書省略時は含めない）
- `ThirdPartyNotices.txt`（辞書省略時は含めない）
- `bundle-provenance.json`（辞書省略時は含めない）
- `manifest.json`

`AppContainerAcl.ps1` は `register-dev.ps1` と `unregister-dev.ps1` が、
`host-startup-log.ps1` は `host-supervisor.ps1` が dot-source する実行時依存で
あるため、いずれも参照元スクリプトと同じディレクトリへ置く。
payload の一覧は手書きなので、zip 生成時に各 PowerShell payload の dot-source を
静的に走査し、参照先が同じ zip に無ければ zip を書かずに失敗する。走査は
`. (Join-Path $PSScriptRoot "x.ps1")` を行頭でも入れ子の block 内でも拾い、行頭の
dot-source でこの形に解決できないものは見逃さずに失敗として扱う。
`verify-bootstrap.ps1` は、既存 Host を停止する前にこれらの実行時依存の実在を
確認する。停止したあとに `register-dev.ps1` と supervisor を起動するので、起動に
失敗すると入力できない VM が残るためである。
`learning-data-snapshot.ps1` は、学習データを不変条件とする Human Gate の境目で
`%LOCALAPPDATA%\azooKey\data` の状態をラベル付きで JSON へ追記し、2 つのラベルを比べる。
記録するのはファイルごとの相対パス、存在、サイズ、行数、更新時刻、SHA-256 だけで、
本文とデータディレクトリの絶対パスは出さない。`.enc` は復号せず暗号文のハッシュで比べる。
比較は既知のストア（`learning.v2.tsv`、旧 `learning.tsv` を含む）とその `.enc`、`.bak` について
SHA-256 の変化、追加、削除を変化とし、
更新時刻だけの変化は変化に数えない。`host_run_state.txt` や Host の一時ファイルなど、
データディレクトリにある学習データ以外のファイルは一覧に出すが変化には数えない。
未知の `*.tsv`、`*.tsv.enc`、`*.json.enc` は警告し、対象ストアの見落としを示す。
この警告がある比較の `Changed: no` だけでは学習不変の証跡としない。
読めなかったファイルは例外の型だけを記録し、比較では `unreadable` として示す。
データディレクトリは絶対パスの代わりに、記録ファイルごとのランダムな salt を付けたハッシュで識別し、
識別子の違う 2 つのラベルは比較しない。ラベルは大文字小文字を区別せずに一意とする。
ゲストの既定シェルである Windows PowerShell 5.1 で動かすため、スクリプトは ASCII だけで書く。
`-MockDictionaryPath` の TSV は `data/`、`-ModelPath` の GGUF は `models/` に
追加する。
GGUF を追加する場合は、同じ build directory の `azookey_zenzai_bench.exe` も
zip ルートへ追加する。
生成前の Ninja dry-run では bench target も鮮度確認の対象にする。
`-IncludeCompat`（既定は無効）を指定すると、同じ build directory の
`compat_test.exe` と C-013 用の `compat_host_hang_watchdog.exe` を zip ルートへ、
`compat-test/targets/` 以下の全ファイルを相対階層を保って `targets/` へ追加し、
compat target も鮮度確認の対象にする。manifest の role はそれぞれ
`compat-runner`、`compat-host-hang-watchdog`、`compat-targets` とし、各ファイルの
SHA-256 を記録する。
`-IncludeSettings`（既定は無効）を指定すると、`settings-app/<Debug|Release>/` の
設定アプリを相対階層を保って zip のルート（`azookey_tsf_tip.dll` の隣）へ追加する。
TIP は自分の隣の `azookey_settings.exe` だけを起動する（`SettingsLauncher.cpp`）ので、
MSI と同じ配置にそろえる。ほかの payload と同じ archive path になるファイルがあれば
zip を作らず失敗する。範囲は MSI（`pkg/msi/Package.wxs`）と
同じで、`obj/` 以下と直下の `*.exp`、`*.lib`、`*.pdb`、`*.ilk` を除く全ファイルとする。
self-contained の Windows App SDK ランタイム、`resources.pri`、`*.xbf` は exe と同じ
ディレクトリに無いと起動しないためである。manifest の role は `azookey_settings.exe` が
`settings-app`、それ以外が `settings-runtime` である。
`azookey_settings` は MSBuild を呼ぶ custom target で Ninja の dry-run が常に作業ありを
返すため、鮮度は dry-run ではなく更新時刻で確認する。比べる入力は、MSBuild が中間
ディレクトリ（`settings-app/obj/<Debug|Release>/`）に残す `*.read.*.tlog` に載った
repository の追跡ファイルだけである。exe がそのいずれより古ければ拒否する。
MSBuild が読まないファイル（`settings-app/Assets/` の README など）は入力に含めない。
含めると、target を再ビルドしても exe の時刻が動かず、拒否が解けないためである。MSBuild の増分判定と同じ入力を見るため、拒否は
target の再ビルドで解消する。tlog が無ければ入力を決められないので拒否する。
`-IncludeDiagnostics`（既定は無効）を指定すると、WPR profile
`diagnostics/azookey-diagnostics.wprp`、ETW manifest `diagnostics/etw/AzooKey.man`、
同じ build directory の `diagnostics/etw/azookey_etw_manifest.dll` を、それぞれ同じ
相対パスで zip に追加する。role は `wpr-profile`、`etw-manifest`、`etw-resource-dll`
とし、他の同梱物と同様にコピー後の SHA-256 を記録する。
ETW DLL の鮮度は専用に検査する。DLL、manifest、ETW の CMake 定義、CMake cache、
`build.ninja`、生成 `AzooKey.rc` と RC object（`AzooKey.rc.res`）、RC が参照する全 locale の message table と
`WEVT_TEMPLATE` resource が存在し、DLL がこれらの入力より古くないことを要求する。
RC object も生成 RC と参照する全 resource より古くないことを要求する。
DLL だけが再リンクされ、古い RC object が残った場合も拒否する。
resource の更新が Ninja に追跡されない場合は、`--clean-first` で再コンパイルする。
resource は同じ build directory の
生成ディレクトリ内に限る。さらに `azookey_etw_manifest` の Ninja dry-run が idle か、
`[1/1] Linking CXX shared library diagnostics/etw/azookey_etw_manifest.dll` の1行だけで
あることを確認する（パス区切りは `/` または `\`）。resource-only DLL は export を持たず
import `.lib` が生成されないため、この再リンクだけを許可する。
MC による生成、RC コンパイル、追加作業、失敗したコマンド、不明な出力は拒否する。
dry-run の間だけ `NINJA_STATUS` を `[%f/%t] ` に固定し、終了時に元へ戻す。
この検査は build の鮮度確認であり、manifest の hash はコピーしたファイルの同一性を確認する。
ETW 登録と WPR の開始・停止は package 生成や bootstrap では行わず、人間が診断時に
明示的に実行する（[`handoff/windows-diagnostics-playbook.md`](./handoff/windows-diagnostics-playbook.md)）。
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
| `buildPrerequisites` | object | cache の `AZOOKEY_FETCH_LLAMA_CPP` と `AZOOKEY_LLAMA_CPP_SOURCE_DIR` を同名キーの文字列で保持 |
| `bundledDictionaries` | object | `selection` は `default` / `explicit` / `omitted`。辞書同梱時は `sudachiRevision`、`inputFingerprint`、`provenanceSha256` も記録する。選択したディレクトリのパスは記録しない |
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

`scripts/vm-verify-summary.ps1` は、VM から回収した機械可読な成果物を
ホスト側で 1 つの検証サマリへ束ねる。
PowerShell 7 で実行し、zip には同梱しない。
入力はパッケージの manifest（`-ManifestPath`、必須）、`verify-bootstrap.ps1 -Json`
の出力、`azookey_diag.exe --json` の出力、target ごとの compat `report.json`
（§13.5、複数可）、VM の OS ビルド番号（`-OsBuild`）とする。
OS ビルド番号はホストの値と取り違えないよう自動取得しない。
`vm-verify-session.ps1 -Run` の実行ディレクトリには、対話セッションで採取した
`bootstrap-interactive.json`、`azookey-diag.json`、`compat-report-<target>/report.json` が
入り、それぞれをそのまま入力に渡せる。
`-OutputDirectory` へ `verification-summary.json` と `verification-summary.md` を
BOM なし UTF-8 で書き出す。
Markdown は `docs/handoff/human-gate-batch-runbook.md` Part C の環境ブロックに沿い、
機械で埋まらない欄は空欄のまま残す。

サマリは観測値の集約であり、人間ゲートの合否を表さない。全体の合否欄は持たない。
各入力の status は次の分類へ写し、分類ごとに数える。
未知の status は `unrecognized` として数え、`pass` へ丸めない。

| 分類 | bootstrap | diag | compat |
|---|---|---|---|
| `pass` | `pass` | `ok` | `pass` |
| `fail` | `fail` | `error` | `fail` |
| `failingSkip` | — | — | `failing-skip` |
| `manualRequired` | `manual_required` | — | — |
| `warning` | — | `warning` | — |
| `notApplicable` | `not_applicable` | — | — |

compat の件数は `report.json` の `summary` ではなく `results` から数え直し、
`summary` との一致を `reportedSummaryMatches` に記録する。
未指定・不在・解析不能の入力は `inputs[].state` の `missing` / `invalid` と
`missingInputs` に記録し、省かない。
系統ごとの必須キー（bootstrap は `overallStatus` と `checks`、diag は `status` と
`checks`、compat は `target` と `results`）を欠く JSON も `invalid` とする。
manifest の commit が 40 桁でない場合と preset が無い場合は、サマリを書かずに失敗する。
bootstrap の `package.commit` と manifest の commit の一致は
`bootstrap.packageCommitVsManifest`（`match`、`mismatch`、`unknown`）として記録する。
`mismatch` の bootstrap の件数は `bootstrap.counts` に残し、合計の `counts` へは加えない。

redaction は許可リストで行う。識別子（check ID、status、target ID、reason code、
モデルのファイル名）は英数字と `._-` だけの値を通し、外れた値は `redacted` に置き換える。
自由文の `message` は 1 行へ畳み、パスを `[path]` へ置換して 200 文字で切る。
ユーザー名は空白を含みうるため、引用符内のパスは引用符内全体を、`\Users\` と
`\Documents and Settings\` 以降は空白を越えて置換する。
ドライブ付きパス・UNC パス・`\\?\` パス・区切り文字 `\` を含む語も置換する。
`azookey_diag` の `details`、bootstrap の `hostBinary` のパスと process ID は拾わない。
compat の `artifact` は出力ディレクトリ相対のパスだけを通す。

`verification-summary.json` の schema v1 は次の top-level field を持つ。

| field | 型 | 内容 |
|---|---|---|
| `schemaVersion` | integer | 固定値 `1` |
| `generatedAtUtc` | string | ISO 8601 UTC 生成時刻 |
| `notice` | string | 人間ゲートの合否を表さない旨の固定文 |
| `package` | object | manifest の `commit`、`preset`、`buildType`、`generatedAtUtc`、同梱モデルのファイル名 `bundledModel`、compat runner 同梱の有無 `compatBundled` |
| `os` | object | `build`（未取得・不正なら空文字列） |
| `inputs` | array | 入力ごとの `name`、`state`（`present`、`missing`、`invalid`）、`reason` |
| `missingInputs` | array | `state` が `present` でない入力の `name` |
| `counts` | object | 全系統を合算した分類ごとの件数と `missingInputs` の件数 |
| `bootstrap` | object / null | `overallStatus`、`packageCommitVsManifest`、`hostBinary` の status と SHA-256、分類件数、`checks` |
| `diag` | object / null | `status`、分類件数、`checks` |
| `compat` | array | target ごとの ID、表示名、automation level、`reportedSummaryMatches`、`caseSelection`、分類件数、`results` |

compat の `caseSelection` は `report.json` の `case_selection`（§13.5）を写したもので、
`excluded`、`prerequisitesAdded`、`baselineCaseExcluded` を持つ。
`case_selection` を持たない report では `null` とする。
Markdown では、未実行のケースがある target と C-001 を除外した target の行に、
部分実行であることを明記する。

`scripts/vm-verify-linear-drafts.ps1` は schema v1 のサマリ JSON とゲート ID 対応表を
読み、`issueId` ごとに Markdown の下書きを出す。対応表は `schemaVersion: 1` と
`gates` 配列を持ち、各行に `source`（bootstrap / diag / compat）、`id`、
`issueId`（DEV-番号）、必要に応じて compat の `targetId` を指定する。
同じゲートを複数課題へ対応付けてもよい。対応表にあるがサマリにない ID は
「未取得」とし、compat の `caseSelection.excluded` にある ID は「未実行」とする。
自由文・入力本文・ローカル絶対パスは下書きへ転記しない。
checkpoint 名は summary JSON にないため任意の明示指定とし、未指定なら空欄にする。
下書きは人間ゲートの合否判定も Linear への投稿も行わない。
bootstrap の package commit が manifest と不一致の場合、その行の status は `excluded` とし、
元の観測 status を転記しない。出力先に既存ファイルがある場合は、古い下書きを残したまま
新しい下書きを混在させないよう書き込みを拒否する。

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

| 依存 | 種別 | 導入手段 | 採否 |
|---|---|---|---|
| GoogleTest | テスト | `find_package` →（オプトイン時）`FetchContent` | 採用（本トラックで変更しない） |
| llama.cpp | 推論 | `FetchContent` / submodule、`AZOOKEY_BACKEND` で制御 | 採用（M8 の範囲。本トラック対象外） |
| WIL | header-only | submodule または `FetchContent`（オプトイン） | 採用（M43 の範囲。§9） |
| nlohmann-json | header-only | 任意・保留 | §6・§11 参照（即時導入しない） |
| spdlog | 要ビルド | 任意・保留 | §7・§11 参照（自前ロガーを優先評価） |
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
| 候補表示レイテンシ | p50 < 80ms / p95 < 180ms | §4.5, §7.3 |
| Host 異常停止時の復旧時間 | 中央値 3 秒以内 | §8.3 |
| CI 成功率 | main 連続 14 日で > 95% | §4 |
| Portable C++ 行カバレッジ | 80%（初期段階は informational） | §4.3 |
| 学習永続化の破損率 | 0%（クラッシュ注入テストで確認） | §5.4 |

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

vcpkg を必須にしない。オフラインビルド原則（§1）と衝突するためである。依存ごとに
header-only 優先・`FetchContent`/submodule オプトインで導入し（§3）、`vcpkg.json` は
任意の利便ファイルとしてのみ許容する。

### 11.2 自前 JSON パーサの即時全置換 — 修正採用

`nlohmann-json` への即時全置換は採らず、自前パーサの堅牢化と fuzz/malformed テストを主とする（§6）。
外部ライブラリへの移行の方針は §11.2.1 が持つ。

#### 11.2.1 外部ライブラリ移行の方針

**v1.0 の範囲では自前パーサを維持し、外部ライブラリへは移行しない。** 移行の動機だった
自前実装に起因するバグクラスは、§6.2 のパーサ強化で閉じている。一方で移行のコストは残る。
自前パーサは §6.2 の上限値と wire contract を spec と `static_assert` で結び付けており、
置換すればこの対応関係を作り直すことになる。TIP は任意アプリへ in-proc ロードされる DLL であり、
依存追加はコンパイル時間とバイナリサイズに直接効く。no-egress 方針のもとでは FetchContent の
ピン留めと `AZOOKEY_FETCH_*=OFF` 既定も前提として維持しなければならない。

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
恩恵よりも、コード生成ツールチェーンの追加がオフラインビルド原則（§1）へ与える影響と移行コストが
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

## 12. IME 診断・修復ウィザード（M44）

### 12.1 目的

azooKey が動作しない、候補が出ない、Zenzai が使われていない、学習が
反映されない、IME 登録が壊れているといった問題をユーザー自身または
開発者が短時間で切り分けられるようにする。IME は「入力できない」時点で
UX が即死しやすいため、本機能は配布前（Phase 4 ゲート）に優先実装する。

#### 12.1.1 M44 実装スコープ

M44 は `azookey_diag.exe --json`、`--repair`、`--collect` を提供し、
D-001〜D-015 を診断する。
Host が停止している場合もローカルで判定できる項目を継続し、全項目を
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

設定アプリの「プライバシー」ペインの障害診断の節は follow-up とする。

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
| D-009 | fallback 状態 | Zenzai / SimpleConverter / degraded を表示 | モデルロード再試行。`safe_mode` は `safeMode.enabled=false` に変更し、Host を再起動するか `UpdateConfig` を送る（§8.5.3） |
| D-010 | learning store | 読み込み可能か、破損していないか、v2 と暗号化 M7 の併存 | 保存による M7 cleanup の再試行、破損時はバックアップ後に初期化 |
| D-011 | user dict | JSON 読み込み可能か | バックアップ後に修復 |
| D-012 | settings | schema validation 成功 | 不正値のリセット |
| D-013 | logs | `%LOCALAPPDATA%\azooKey\logs\` 書き込み可能 | ディレクトリ作成 |
| D-014 | DPAPI | 暗号化データを復号できるか | 再認証 / 再入力を促す |
| D-015 | app compatibility | 前面ウィンドウのプロセスアーキテクチャと AppContainer 状態から既知の非対応条件を判定する。対象アプリ内の TSF context 取得可否は CLI からは観測しない | 互換性情報表示（M50 result） |

D-013 は実効ログディレクトリ内に一時ファイルを作成して削除し、実際の書き込み可否を
判定する。プロセス終了などで残った診断用一時ファイルは、次回診断時に 24 時間を
超えていれば削除する。Host を一度も起動していない環境ではディレクトリが存在せず
`error` になりうる。この場合の `--repair` はログディレクトリを作成して再診断する。

D-012 の schema 正典は `settings/mvp-settings.schema.json` とし、CI / pre-commit は
同 schema の meta-schema 妥当性と代表サンプルの適合性を静的に検証する（§4.3）。

### 12.2.1 判定基準（status 決定ルール）

各項目の `status`（§12.3 の `ok` / `warning` / `error`）は以下の閾値で確定する。
閾値は §8.5.2 timeout 表・§12.6 `fallback_state` enum・§13.2 自動化レベルと
同一値を参照し、二重定義しない。`--repair` 列は roadmap M44 の受け入れ条件の
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
| D-008 | `model.enabled=false`、またはモデル未選択（`model.selectedPath` 空 = 検証対象なし。該当なしとし、未選択の warning は D-007 が担う）、または enabled かつ**選択済み**モデルが `valid` **かつ loaded**（R1=GGUF magic / version、R2=`genai_config.json` パース + 参照 ONNX 実在。model-management-spec §3.3 の format 別 `valid` を使う） | enabled かつ選択済みモデルが `valid` だが未ロード（fallback 動作中）、または Host の実効ランタイムが `mock`（§12.6 `engine`） | enabled かつ**選択済み**モデルの形式別検証に失敗（R1: magic 不一致 / version 非対応 / 破損、R2: config 不正 / 参照 ONNX 欠落） | ✗ |
| D-009 | `fallback_state == healthy`、または（`safe_mode` でない）`model.enabled=false`（SimpleConverter 固定が意図された設定） | `degraded_simple` / `degraded_model`（enabled 時の非意図的劣化）。Host に到達できず `QueryDiagnostics` を得られない場合は、`model.enabled` なら `degraded_simple`、そうでなければ `healthy` とみなす（§8.5.1） | `safe_mode`（`model.enabled` に関わらず最優先） | ✗（復旧は D-005 / D-008 修復経由。`safe_mode` の解除は §8.5.3） |
| D-010 | 読み込み成功・schema 妥当（空 / 新規を含む）かつ暗号化 M7 の cleanup 待ちでない | 旧 schema だが migration 可能、または可読な v2 と `learning.tsv.enc` が併存 | 読み込み不可 / 破損 / cleanup 対象の存在確認不可 | ✗（バックアップ後の初期化は手動確認） |
| D-011 | JSON 読み込み成功（空 / 新規・欠損ファイルは空として正常） | — | パース不可 / 破損 | ✗（バックアップ後の修復は手動確認） |
| D-012 | schema validation 成功、または `settings.json` が無い（既定値で動作） | 本節の登録済み旧形式に一致し、登録された移行経路で移行できる | 読み込み不可、JSON 不正、または validation 失敗（旧ベンチマーク履歴の例外を含む） | ✗（不正値リセットは確認後） |
| D-013 | logs ディレクトリ書き込み可 | — | ディレクトリ未作成、または書き込み不可 | ✓ ディレクトリ作成 |
| D-014 | OpenAI 鍵を要求する**実効バックエンド**が無い（global `aiBackend` と全 `profilesByApp.*` の app-profile §4.2 解決後の実効値がいずれも `none` / `local-zenzai`）、または OpenAI を要求する実効バックエンドがあり `openAiApiKey` が非空で有効（plaintext〔M16–M34 移行期。schema が plaintext を許容〕はそのまま有効、`dpapi:` prefix 付きは復号成功） | OpenAI を要求する実効バックエンド（global もしくは**いずれかの** `profilesByApp.*` が §4.2 解決後に `openai`）があるが `openAiApiKey` が空（資格情報未設定で認証不可）、または settings の読み込み・schema 検証の失敗や DPAPI を使えないことで判定できない（`details.state` が `unavailable`。settings の不備そのものは D-012 が担う） | `dpapi:` prefix 付きの暗号化値が復号失敗 | ✗（再認証 / 再入力を促す） |
| D-015 | —（CLI は対象アプリ内の TSF context を直接観測しない） | x64 の通常プロセス、または前面ウィンドウ・プロセス情報・アーキテクチャ・トークンの取得不能で、既知の非対応条件を確定できない | x86、x64 以外のアーキテクチャ、または AppContainer プロセスを確認 | ✗（§13 互換性情報へ） |

D-002 の `warning` は任意の表示名だけが欠けた状態であり、`--repair` は登録を再実行しない。
必須登録に不備がある `error` の場合だけ再登録し、再診断で `warning` まで改善した場合は修復成功とする。

D-010 の `details.legacy_retained` は、v2 の検出時に `learning.tsv.enc` の存在を
確認した結果を真偽値で返す。存在確認に失敗した場合は `error` とする。
v2 が可読なら併存を cleanup 待ちの `warning` とし、v2 が読めない場合の `error` を
優先する。v2 がなく、可読で非空のヘッダなし旧 TSV を読んだ場合は migration の `warning` とする。
`escaped=1` の M7 ファイルだけがある場合は、それだけでは `warning` にしない。
診断はファイルを削除せず、v2 との併存判定では legacy の本文を復号・出力しない。削除と再試行の条件は
`docs/user-learning-enhancement-spec.md` §3.1 に従う。平文 M7、`.bak`、`.corrupt-*` の
残存だけでは cleanup 待ちと判定しない。

D-012 が `warning` とする旧形式は、本節に登録したものに限る。`settings.json` は版識別子を持たず、
`settings/mvp-settings.schema.json` は加算的に拡張する（`docs/settings-app-spec.md` §3.6
「拡張方針」）。破壊的変更は下記の登録を伴うため、登録済みの旧形式が無い限り、過去の schema に
適合した `settings.json` は現行 schema にも適合し、`ok` になる。ただし下記のベンチマーク履歴の例外を除く。
後方互換として受理する値形式、すなわち平文の `openAiApiKey`（D-014 が扱う）、root tier の
`backendPreference` / `epPreference`（`docs/model-management-spec.md` §5.2）、
`backendPreference` の `directml` / `npu`（同 §5.1）、`promptPrefixByApp`（`docs/app-profile-spec.md` §6）も
schema に適合するため D-012 では `ok` とし、非推奨であることを D-012 では案内しない。

旧形式の登録は、キーの改名・削除・enum 縮小を伴う schema 変更と同じ PR で行い、
旧形式の識別方法と、Host の `SettingsStore` または設定アプリの `settings-app/SettingsDocument.*` に
実装した移行経路を併記する。`docs/app-profile-spec.md` §6 が予定する `promptPrefixByApp` の削除や、
`directml` / `npu` を enum から外す変更は、この登録の対象になる。

厳密な履歴 schema を導入した `model.benchmarkHistory` は、上記の後方互換保証の例外とする。
旧設定アプリが読んだ `model` / `model_path` 形式、`completedAt` などの必須フィールド不足、
nullable な旧計測値を持つ要素は、過去の schema が履歴を配列として受理していても、
現行 schema に適合しなければ D-012 を `error` とする。旧応答の追加フィールドを持つ場合も同じで、
診断用コピーから旧行を除いて `warning` に緩和しない。
整理経路は `model-management-spec.md` §7.1 の Host 追記時整理であり、
利用者が次回ベンチマークを実行した際に不適合な旧要素を除き、新しい結果を追記する。
設定アプリの通常保存と診断は旧要素を保持し、診断も `--repair` もこの整理を実行しない。

登録済みの旧形式に一致しない schema 非適合は、旧形式らしく見えても（`schemaVersion` などの
未定義キーを含む場合も）旧形式と推定せず `error` とする。D-012 の判定は `settings.json` を
読むだけで書き換えない。移行と不正値のリセットは `--repair` の対象外とし、
上記の移行経路または利用者の確認を経て行う。

ARM64 ホストでは `IsWow64Process2` の結果だけで x64 エミュレーションと ARM64
ネイティブを区別できないため、`GetProcessInformation(ProcessMachineTypeInfo)` で
対象プロセスのアーキテクチャを確認する。同情報を取得できない場合は
`architecture_unknown` の `warning` とし、ARM64 を非対応と推定しない。
確認できた ARM64 ネイティブは、`docs/sideload-packaging-spec.md` §0.1 の
x64 入力対象スコープに従い
`unsupported_architecture` とする。

D-015 の `details.reason` は `context_unverified`、`no_foreground_window`、
`process_unavailable`、`architecture_unknown`、`container_unknown`、`x86`、
`unsupported_architecture`、`app_container` の固定 enum とする。`warning` は
「対応を確認済み」ではなく、プロセス条件からは TSF context の可否を決められないことを
意味する。アプリ別の実入力と既知の劣化は §13 の互換性ハーネスで判定する。
ウィンドウタイトル、プロセス名、入力本文は D-015 の JSON に含めない。

全体 `status` は §12.4 の規約どおり `checks[].status` の最悪値
（`error` > `warning` > `ok`）とする。通常の `warning` は「縮退しているが入力は
継続できる」を意味する。D-015 の `warning` だけは TSF context が未確認であり、
入力の成立を主張しない。`error` は「当該機能が成立しない」を意味し、UI（§12.7）の
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
解決規則（`auto` を global へ展開 → プライバシー判定による降格を適用）で評価した実効値の
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
      "message": "Selected model is valid but is not loaded",
      "details": {
        "valid": true,
        "loaded": false,
        "engine": "llama_cpp"
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
└── crash-summary.txt         (azooKey 管理ダンプの一覧のみ、ダンプ本体は含めない)
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
  文字列 field は §12.5 の `host-health.json` 行の規則で処理してから保存し、
  生の payload を**そのまま書き出す経路は持たない**。
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
| `host-health.json` | §12.6 `QueryDiagnostics` payload。構造化 field（bool / enum / 数値）はそのまま保存するが、文字列の `loaded_model_path` / `last_error` / `ep_last_error` は `core::RedactFreeText`（§7.6）でパス正規化（`%LOCALAPPDATA%` 等）と資格情報の伏せ字を通してから保存し、生のユーザーパスを残さない。自由文字列から本文を後で検出することはできないため、Host は `last_error` / `ep_last_error` を作る時点で入力本文・候補・prompt を入れず、固定の理由語・HRESULT・backend や HTTP のステータスだけで構成する |
| `ipc-ping.json` | RTT / 成否のみ |
| `logs/*.jsonl*` | §7.6 の共通 field 判定を用いてログとローテート世代を収集（Release 既定で本文なし）。Debug かつ `AZOOKEY_LOG_BODY=1` の本文入りログは収集時に再 redact する。新しいファイルから 1 ファイル末尾 1 MiB、合計 8 MiB まで収集し、切り詰めまたは除外は `logs/README.txt` に記録する |
| `environment.txt` | OS バージョン / CPU / RAM / GPU のみ。ホスト名・ユーザー名・シリアル番号を含めない |
| `crash-summary.txt` | azooKey 管理ダンプ（`docs/sideload-packaging-spec.md` §8.2 の保存先と命名規則に合う `.dmp`）の件数・合計サイズと、各ファイルのファイル名・サイズ・更新時刻だけを記録する。ファイル名は `<module>` / UTC 時刻 / PID だけから成り、ディレクトリ部分は書かない。ダンプを開かず、ダンプ本体・スタック上の文字列バッファを含めない。同意が `off` で保存先が無い場合と、ダンプが 0 件の場合はその旨を 1 行で記す。WER（OS）が保存したダンプは対象外とする |

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
      "backend_selection": {                  (optional, 起動時の選択結果)
        "source": "cli" | "settings" | "auto",
        "auto_backend": str, "reason": str,
        "adapters_known": bool,
        "hw_gpu_count": uint64, "hw_ml_only_count": uint64,
        "vulkan_loader": bool, "cuda_runtime": bool,
        "os_build": uint32, "winml_os_eligible": bool, "on_battery": bool
      },
      "prompt_format": "zenz-v3" | "kanakanji-edge-v1"   (optional, ロード中モデルの値),
      "ep": str (optional, R2/winml 時の選択 EP 名),
      "ep_state": str (optional, "NotPresent"|"NotReady"|"Ready"|"Registered"|"Failed"),
      "ep_last_error": str (optional, EP 取得・登録失敗の HRESULT/診断文; §4.6),
      "learning_entries": int, "user_dict_entries": int,
      "fallback_state": "healthy" | "degraded_simple" |
                        "degraded_model" | "safe_mode",
      "last_error": str (optional),
      "neologd_layer": {                       (optional)
        "state": "not_requested" | "loading" | "ready" | "missing_pack" | "error",
        "reason": str (optional, state="error" のときの固定の分類名)
      }
    } }
```

v1 の reader は `rss_mb`、`learning_entries`、`user_dict_entries` の欠落を `0` として扱う。
`engine`、`backend`、`fallback_state` は必須であり、欠落時は payload 全体を不正とする。
`neologd_layer` は protocol v1 の追加フィールドで、欠落と未知の `state` はどちらも「無し」として
扱い、payload 全体は不正にしない。値の意味は `docs/azdic-physical-layer-spec.md` §15.14 が定める。
`reason` は `neologd_pack_load` ログと同じ固定の分類名だけで、パスやサーバの応答を含まない。

`backend_selection` は protocol v1 への省略可能な追加フィールドである。欠落、型違い、
内部フィールドの欠落や数値範囲外は「無し」として扱い、payload 全体は不正にしない。
未知の内部キーは無視する。値は Host 起動時の BackendSelector の結果で、選択規則と
`reason` の固定分類名は `docs/copilot-pc-backend-spec.md` §4.4 に従う。
`source` は起動時の要求元、`auto_backend` は一次フィルタの候補であり、明示指定や
ロード失敗後の実 backend は従来どおり `backend` が示す。Health の意味も変えない。
電源変化や `UpdateConfig` の後もこの起動時の値を保持する。
`adapters_known=false` は列挙不能または SafeMode による probe 省略、`os_build=0` は
OS build 不明を表す。SafeMode の probe 省略時は件数と bool も 0 / false になる。
アダプタ名・識別子・パス・自由文は含めず、分類名と非識別の事実のみを返す。
診断 CLI の JSON は `D-008` の `details.backend_selection` に同じ値を載せる。

`prompt_format` は protocol v1 への省略可能な追加フィールドで、ロード中モデルが使う
プロンプト形式（catalog の宣言、または語彙からの推定。`docs/zenzai-inference-spec.md`
§2.4.2）を返す。モデル未ロード、Zenzai 以外の変換器、mock runtime（`runtime_tier` が `mock`）、古い Host では省略する。
reader は欠落と未知の値を「無し」として扱い、payload 全体は不正にしない。値は
2 つの固定名だけで、パスや利用者の入力を含まない。診断 CLI の JSON は `D-008` の
`details.prompt_format` に同じ値を載せる（省略時は載せない）。

`--collect` 時はこの IPC で取得した値を `host-health.json` に保存する。
`last_error` / `ep_last_error` は失敗時にユーザーパスを含みうるため、保存前に
§12.5 の `host-health.json` 行の規則で処理する。Host がこれらに入力本文・候補・prompt を
入れないことも同行が定める。

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

受け入れ条件の定義の正典は [`plans/windows-port-roadmap.md`](../plans/windows-port-roadmap.md)
の M44 節とする。本書は §12 で診断項目・判定基準・診断 ZIP の redaction を定義し、
受け入れ条件を複製しない。

診断 ZIP に秘密情報が含まれないことは snapshot テストで検証する。

## 13. アプリ互換性テストハーネス（M50）

### 13.1 目的

主要アプリで TSF composition / 候補ウィンドウ位置 / 確定 / キャンセル /
Unicode / 絵文字 / Undo / Redo が壊れないことを半自動で検証し、
アプリ互換性のベースラインを確保する。

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
| C-013 | supervisor 配下の Host を一時停止して入力し、再開する | TIP の `Degraded` 中もかな候補で入力を続け、再開後 `Ready` と通常変換へ戻る |
| C-014 | ライブ変換 OFF で `kitto` / `syatu` / `siro` / `nn` を入力 | 各かなを preedit / commit できる |
| C-015 | 固定 2 候補で ↑↓ / Space / Shift+Space / Ctrl+N/P、数字 2、Esc 1 回 | 選択 surface、確定文字列、Esc 1 回で候補窓と preedit が消えることを確認する |
| C-016 | ライブ変換 OFF で `nihongo` + 即 Space を一括送出し、確定後も入力する | 候補・確定・後続入力を UIA で確認する |
| C-017 | preedit 中に Ctrl+H を押す | 1 単位が削除される |
| C-018 | ライブ変換 ON で入力・編集・確定する | UIA で preedit と確定文字列の遷移を確認する |
| C-019 | 予測候補を表示し Tab / Shift+Tab / Esc を押す | 候補窓の予測モード（窓プロパティ `azooKey.CandidateWindowMode` で候補と区別する）の位置、受理後の文字列変化、Esc 1 回で窓と preedit が消えることを確認する。Shift+Tab は予測モードに 2 件以上表示された場合に判定し、1 件の場合は failing-skip とする。表示候補の index は文字列から判定しない |

C-001〜C-012 は `full` アプリ（§13.2）で UI Automation により自動判定できる。
C-014〜C-019 は UI Automation と対象 process の HWND 列挙により自動判定する。
C-013 は UI Automation と TIP の構造化ログを組み合わせて判定する。
C-013 は DEV-1364 の追加ケースであり、M50 の C-001〜C-012 受け入れ条件は変えない。
予測候補の文字そのものと描画品質は UIA で判定しない。
ライブ変換が ON の C-014 / C-016、ライブ変換が OFF の C-018、予測候補が返らない場合、
固定 2 候補の入力配列が利用できない場合は
該当ケースを `failing-skip` として残す。
ただし M50 完了ゲートの**必須対象は Notepad / VS Code / Edge**（M50 受け入れ
条件）であり、その他の `full` アプリ（Chrome / Windows Terminal /
Windows Settings 等）は実行可能な範囲で自動判定する best-effort 対象とする。
`best-effort` / `recorder` アプリでは自動化できないケースを §13.3.1 の
手動チェックリストで補完する。
TSF の描画責務を見る実機の手動チェックリスト（M3 の表示属性、M5 の `pbShow`）は `compat-test/` に置き、
一覧は `compat-test/README.md`「手動チェックリスト」が持つ。

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
| UWP / WinUI（Store apps） | AppContainer ACL、Windows アプリ互換カテゴリ、第三者 IME の署名がロード前提 | `ALL APPLICATION PACKAGES`（S-1-15-2-1）RX、`GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT` 登録、署名済み TIP を揃える（正典は `docs/msix-packaging-spec.md` §1.7）。本ハーネスは Windows Settings を `full`、その他 Store apps を `best-effort` とし、署名済み成果物の確認は Human Gate で行う |

### 13.4 実装

ハーネスは `compat-test/` に置く。ディレクトリ構成とファイルの役割は
`compat-test/README.md`「ディレクトリ構成」が正典である。

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
C-013 は同じ supervisor 配下の Host を終了させずに一時停止する。別プロセスの
`compat_host_hang_watchdog.exe` が再開を担当し、runner の異常終了か 8 秒の期限でも
再開する。検証 zip には runner と同じディレクトリに watchdog を同梱する。
停止中の入力継続と再開後の通常変換を UI Automation で確認し、TIP の構造化ログで
`Degraded` → `Ready` の遷移を確認する。対象アプリが runner 起動プロセス自身なら
ログ環境変数の継承を確認できるため、遷移の欠落を fail とする。既存プロセスへの
ウィンドウ委譲などでログ設定を確認できず、対象 PID の遷移が無い場合は failing-skip とし、
観測条件を満たせない状態を pass にしない。watchdog が期限切れで Host を再開した
場合は再開失敗と区別して期限切れとして fail にする。

runner / C-001〜C-019 / unit test はトップ `CMakeLists.txt` から
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
3 target とも C-013、C-010 の順に最後に実行し、Host の停止を後続ケースへ波及させない。

### 13.5 出力

```
compat-report-YYYYMMDD-HHMMSS/
├── report.md         # 人間向けサマリ
├── report.json       # CI artifact 用
├── screenshots/
│   └── notepad_C-001_fail.png
└── failures/
    └── notepad_C-001_fail/
        ├── failure.log    # 固定 reason code・矩形・本文を含まない読み取り診断値のみ
        └── screenshot.png
```

スクリーンショットは失敗時だけ生成する。デスクトップ画素は取得せず、空の画像に
対象ウィンドウ・キャレット・候補ウィンドウの矩形だけを描画する。
`report.md` は target 固有の HTML コメント marker、全体結果、件数表、折りたたみ可能な
case 詳細表を持ち、そのまま PR コメント本文として使える形式にする。

runner は `--cases C-001,C-004` で実行するケースを選び、`--skip C-010` で除外できる。
両方を指定した場合は、`--cases` の選択から `--skip` を除く。
形式が `C-NNN` でない ID、重複、target の `cases` に無い ID、空になる選択は終了コード
`64` で拒否し、出力ディレクトリを作らない。
実行順は選択の順ではなく target の `cases` の順に従い、C-010 を最後に置く順序を保つ。
C-001 を前提とするケースを選び C-001 を選ばなかった場合は、C-001 を前提として追加する。
`--skip C-001` で明示的に除外した場合は追加せず、前提を持つケースを実行せずに
`baseline-case-excluded` の `failing-skip` とする。
除外したケースは `results` と件数に含めず、終了コードは実行したケースだけで決まる。

`report.json` は `schema_version` 1 のまま、次の `case_selection` を持つ。

| フィールド | 内容 |
|---|---|
| `executed` | `results` に載るケース。target の順 |
| `excluded` | target の `cases` のうち実行しなかったケース。target の順 |
| `prerequisites_added` | 前提として追加したケース（C-001） |
| `baseline_case_excluded` | C-001 を除外したまま前提を持つケースを選んだ場合に `true` |

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

受け入れ条件の定義の正典は [`plans/windows-port-roadmap.md`](../plans/windows-port-roadmap.md)
の M50 節とする。本書は §13 で対象アプリ・テストケース・レポート形式を定義し、受け入れ条件を複製しない。

## 転送表

分けた節は次の文書にある。節番号は保つ。

| 旧節 | 移動先 |
|---|---|
| §5、§6 | `docs/ipc-persistence-hardening-spec.md` |
| §7 | `docs/observability-spec.md` |
| §8 | `docs/host-availability-spec.md` |
