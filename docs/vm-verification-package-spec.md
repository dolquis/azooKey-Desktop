# 実機検証パッケージ 仕様（M37）

対象リポジトリ: dolquis/azooKey-Desktop
対応マイルストーン: M37
関連: `docs/dev-infrastructure-spec.md`（開発基盤の入口と転送表）、
      `docs/build-ci-spec.md`（ビルドと CI の仕様）、
      `docs/handoff/hyper-v-vm-verification-plan.md`（VM 検証資産の一覧）、
      `docs/handoff/hyper-v-tip-verification.md`（VM 検証の手順）、
      `docs/handoff/human-gate-batch-runbook.md`、
      `plans/windows-port-roadmap.md` M37

本書は `docs/dev-infrastructure-spec.md` から §2.6を切り出した文書である。節番号 §2.6 は旧 spec のまま保ち、旧 spec の転送表が§2.6を本書へ解決する。本書内で、本書にない節（旧 spec の他の節）を指すときは、文書名を付けて書く（例: `observability-spec.md` §7.6）。
旧 `dev-infrastructure-spec §2.6` は、旧 spec の転送表（番号を変える行）が本書の §1 へ解決する。

## 正典範囲

`scripts/make-vm-verify-package.ps1` が生成する実機検証パッケージ（Hyper-V の Windows 11 VM へ持ち込む zip）の入力、
同梱物、manifest、検査は本書が正典である。VM の構成と検証の手順は `docs/handoff/` の runbook が持つ。

## 節とマイルストーンの対応

| 節 | マイルストーン |
|---|---|
| §1 | M37 |

## 1. 実機検証パッケージ

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
（`diagnostics-compat-spec.md` §13.5、複数可）、VM の OS ビルド番号（`-OsBuild`）とする。
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

compat の `caseSelection` は `report.json` の `case_selection`（`diagnostics-compat-spec.md` §13.5）を写したもので、
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
