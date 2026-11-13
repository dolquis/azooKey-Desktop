---
kind: runbook
canonical_for: ["1 回の VM セッションでまとめるときのレーン設計、ゲート種別ごとの実行条件、検証メモの記録様式"]
---

# 人間ゲート一括消化の VM セッション runbook

複数の `gate:human-required` 課題を 1 回の Hyper-V VM セッションでまとめて消化するための実行計画と記録様式を定める。
VM 構成、checkpoint 運用、bootstrap、TIP 登録の各手順は既存文書が正典であり、本書はそれらを再掲しない。

- VM 構成、3 層フロー、エージェントへの委任の範囲、登録、bootstrap、ログ取得、後始末の手順：[`hyper-v-tip-verification.md`](./hyper-v-tip-verification.md)
- 打鍵チェック項目 A1〜A8 と B1〜B7：[`tip-manual-verification-checklist.md`](./tip-manual-verification-checklist.md)
- compat runner の実行：[`../../compat-test/README.md`](../../compat-test/README.md)。出力レイアウトは [`../dev-infrastructure-spec.md`](../dev-infrastructure-spec.md) §13.5
- dump、ETW、Process Monitor の採取：[`windows-diagnostics-playbook.md`](./windows-diagnostics-playbook.md)
- VM と開発機の振り分け基準、開発機での手順：[`dev-machine-verification.md`](./dev-machine-verification.md)

本書のレーンは VM で行う。
対象の選定では、Linear で `env:vm` が付いた課題を本書のレーンへ、`env:dev-machine` が付いた課題を開発機へ割り当てる。
`env:dev-machine` の課題を VM のレーンで行ってもよい。逆は行わない。

進捗と状態の正典は Linear（team `Dev` / project *azooKey Desktop / Windows IME MVP*）である。
本書はレーンの設計、ゲート種別ごとの実行条件、記録様式だけを持ち、実走の履歴と個別課題の状態は持たない。
どの課題をどのレーンで実施するかの割当と、課題ごとの確認項目は、Linear の tracking Issue と各課題の本文が持つ。
どの人間ゲートが実際に未消化か、どのゲートが何を待っているかは、実行前に Linear で確認する。
過去の実走で何がどこまで採れているかも、本書ではなく該当課題のコメントを読む。

## セッション前にホスト側で用意するもの

VM を起動する前に、2 種類の成果物をホストで作る。
レーン 1 とレーン 2 は前提とする VM 状態が異なるため、片方だけでは両方を走らせられない。

**MSI**：レーン 1 の MSI 配布形態のゲートが使う成果物。同じセッションのレーン 1 のゲートは同一の MSI を使う。
検証対象の修正を**すべて含む commit** から MSI を作り、ファイル名と SHA-256 を検証メモへ記録する。
SHA-256 はレーン 1 の `vm-verify-session.ps1 -Prepare` が `msi-record.json` に書く値を使う。
対象の修正を欠く MSI で走らせると、修正済みの欠陥を再観測することになる。

**検証 zip**：レーン 2 の全ゲートが使う開発登録用の成果物。

`AZOOKEY_FETCH_LLAMA_CPP` の既定は `OFF` で、`windows-release` preset もこれを ON にしない。
llama.cpp を含まない Host に対して `-ModelPath` を渡すと、`register-dev.ps1` の preflight が `llama_cpp=1` を検出できずに登録を拒否する。
実 GGUF を使う項目は Host の model preflight で止まるため、configure で明示的に ON にする。

```powershell
cmake --preset windows-release -DAZOOKEY_FETCH_GOOGLETEST=ON -DAZOOKEY_FETCH_LLAMA_CPP=ON
cmake --build --preset windows-release
cmake --build --preset windows-release --target compat_test
cmake --build --preset windows-release --target azookey_settings
cmake --build --preset windows-release --target azookey_etw_manifest
python dictbuild/build_bundled.py --output build/bundled-dictionaries --cache build/dictionary-sources
python dictbuild/check_bundle.py build/bundled-dictionaries --package pkg/msi/Package.wxs
```

パッケージ生成時に CMake cache の llama.cpp 構成を自動検査する。
`-ModelPath` を指定した場合は、`AZOOKEY_FETCH_LLAMA_CPP` または
`AZOOKEY_LLAMA_CPP_SOURCE_DIR` が有効でなければ生成を拒否する。
この検査は VM での `register-dev.ps1` の実行時 preflight を代替しない。

```powershell
.\scripts\make-vm-verify-package.ps1 `
  -Preset windows-release `
  -OutputDirectory .\build\vm-verify-packages `
  -RuntimeInstallerPath C:\path\to\vc_redist.x64.exe `
  -ModelPath C:\path\to\zenz-v3.gguf `
  -IncludeCompat `
  -IncludeSettings `
  -IncludeDiagnostics
```

依存ログが別の checkout のヘッダーを指していると、パッケージ生成は拒否する。
compiler cache（sccache）が別 checkout でコンパイルした結果を返した場合に起きる。
拒否メッセージの手順で cache を更新して clean 再ビルドし、生成をやり直す（`docs/debugging.md`）。

GGUF を使うゲートを同じ zip で検証する場合、`-ModelPath` は省略できない。
スクリプトは `-AllowNoModel` を明示しない限りモデル省略を拒否する。
local-zenzai の品質確認のように実 GGUF での推論結果を見るゲートは、
GGUF なしでは同梱辞書などの候補へフォールバックするため、その品質判定が成立しない。

静的辞書は `-BundledDictionaryDirectory`（既定 `build/bundled-dictionaries`）から
`dict/sudachi_lexicon.azdic`、`dict/technical_terms_lexicon.azdic` と
`ThirdPartyNotices.txt` を同梱し、各ファイルの SHA-256 を `manifest.json` に記録する。
生成入力と出力を結ぶ `bundle-provenance.json` も同梱する。パッケージ生成は形式・出所と現在の生成入力を照合するため、
古い生成結果で拒否された場合は上の辞書生成コマンドを実行し直す。manifestの `bundledDictionaries` には
既定/明示指定の区分とSudachi revisionを記録し、指定したディレクトリのパスは記録しない。
既定の辞書ディレクトリが無い場合だけ、`-AllowNoDictionary` の明示で省略できる。
明示したパスの不存在や、不完全な辞書セット・空の帰属表示は省略スイッチでも拒否する。
辞書を省略した zip は辞書層を前提とする再変換・候補合流・未知語判定のゲートに使わない。
展開後の Host ログでは `static_dictionary_load` の両層が `result=ok` であることを確認する。

**compat runner の同梱**：`-IncludeCompat` で `compat_test.exe` と `targets/` 以下の全ファイルを
同じ検証 zip に追加する。各ファイルの SHA-256 は `manifest.json` に記録される。
compat runner を使うゲートがあるセッションでは、既定で無効のこのスイッチを明示する。

VM 側では `compat_test.exe` と `targets\` を同じディレクトリへ展開する。
runner は `--target` に渡したパスから target JSON を読むため、両者の相対関係を崩さない。

**設定アプリの同梱**：`-IncludeSettings` で `azookey_settings.exe` と self-contained ランタイムを
同じ検証 zip のルート（`azookey_tsf_tip.dll` の隣）に追加し、各ファイルの SHA-256 を
`manifest.json` に記録する。TIP は自分の隣の `azookey_settings.exe` だけを起動するので、
MSI と同じ配置にしている。
レーン 2 の設定系ゲートは、展開したパッケージの
`azookey_settings.exe` から設定を保存する。別の zip を手作業で作って持ち込まない。
同梱物の範囲は MSI と同じである。

**診断資産の同梱**：`-IncludeDiagnostics` で WPR profile、ETW manifest と resource DLL を
`diagnostics\` 以下へ追加する。レーン 3 の ETW 診断と WPR の実採取は、この同じ zip の診断資産を使う。
各ファイルの role と SHA-256 は `manifest.json` に記録される。
DLL は `azookey_etw_manifest` をビルドしてから同梱する。
登録・採取・解除はレーン3で行い、手順は
[`windows-diagnostics-playbook.md`「IPC timeout の再現と採取」](./windows-diagnostics-playbook.md#ipc-timeout-の再現と採取) を参照する。

持ち込むもののうち、パッケージ生成が拾わないものを別途 VM へ入れる。

- Sysinternals Suite（`procmon`、`handle`）：Store 入力が再検証で失敗した場合の境界確認に使う
- ゲスト側の 150% DPI 設定：compat の C-006 が要求する。ゲスト OS のスケーリング設定なので基本セッションのままで足りる
- 絵文字を含むユーザー辞書エントリ、または絵文字を返す辞書：compat の C-007 を TIP 経路で確認するため。取り込み用の TSV は下の合成 fixture にある
- アプリ互換のゲートが対象とするアプリ（Chrome、VS Code、Windows ターミナル、Office 365 Word など、課題本文と `compat-test/` のチェックリストが挙げるもの）：未導入のアプリは測れないため、事前に入れるか対象外として記録する
- 第 2 のローカルユーザーアカウント：ログオン常駐の別ユーザー provisioning と、DPAPI の他ユーザーでの復号失敗の確認が要求する
- デバッガ（WinDbg など）：レーン 3 で Application Verifier が停止したとき、接続していないと停止コードと stack が残らない
- 429 を返すループバックの stub：AI 整文のエラー分類通知の `RateLimit` と再試行待機を、外部 API を使わずに誘発するため（レーン 2 の手順 2）。stub は下の合成 fixture にある
- 合成 fixture（`scripts/vm-verify-fixtures/`）：検証 zip には含まれないため、検証 zip と同じ commit の checkout からディレクトリごとゲストへコピーする。収録物、コピー後の照合、ゲートごとのコマンドは [`scripts/vm-verify-fixtures/README.md`](../../scripts/vm-verify-fixtures/README.md) が正典であり、本書はコマンドを再掲しない

合成 fixture を使うゲートと、README の対応する節は次のとおり。

| ゲート | 使うもの | README の節 |
|---|---|---|
| compat の C-007（TIP 経路の絵文字確定） | 絵文字を表層に持つユーザー辞書の TSV | 「絵文字のユーザー辞書」 |
| AI 整文のエラー分類通知 | 401、429 + `Retry-After`、遅延応答を返すループバック stub | 「AI 整文のループバック stub」 |
| カスタムローマ字（M17） | カスタムローマ字 TSV（正常、差し替え用、空、不正行のみ） | 「カスタムローマ字 TSV」 |
| DPAPI 移行（M34） | DPAPI 暗号化より前の形式の平文ストア | 「平文ストア」 |
| 非 ASCII argv、非 ASCII パスの GGUF 実ロード | 非 ASCII のディレクトリ名とファイル名を生成するスクリプト | 「非 ASCII パス」 |
| コンソール終了時の flush | `--stdio` の Host へ渡す Handshake と CommitObservation の行 | 「stdio の行」 |
| 検証メモの下書き | ゲート ID と課題の対応表 `gate-map.json` | 「gate map」 |

マルチディスプレイ構成は用意しない。
IME の打鍵検証は基本セッション必須であり（`hyper-v-tip-verification.md` の安全装置）、基本セッションは単一ディスプレイしか持てない。
C-005 はこの制約のため本セッションの対象外とし、マルチディスプレイ構成の別セッションで扱う。

## Part A：Store 入力の再検証

**パッケージ化された UWP / Microsoft Store アプリ**での入力は **MVP の対象外**である
（`docs/sideload-packaging-spec.md` §0.1）。MVP が入力先として保証するのは Win32 デスクトップ
アプリに限る。除外の境界は AppContainer を使うかどうかではなく、パッケージ化された
UWP / Store アプリかどうかで引くため、**Edge は対象内**である。
署名は入力対象と独立した v1.0 のリリースゲートとして残り、MVP の未署名 MSI は評価配布に
限定する。したがって **MSI 配布形態のゲートの Store / UWP 入力項目は「未達」ではなく「スコープ外」**
として扱う。

本 Part は必須ゲートではない。実施するのは、互換カテゴリ登録の効果を確認したい場合と、
v1.0 以降の署名判断に使う証跡を採りたい場合に限る。
時間が足りなければ Part B を優先してよい。

着手する前に、Store 入力と署名判断を扱う課題のコメントを読み、どこまでの境界が既に採れていて何が
未確認で残っているかを確かめる。同じ観測を採り直しても判定は動かない。

Store 入力が成立しない場合のロード前ゲートは 2 つあり、どちらも独立に効く。

- 互換カテゴリ `GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT` の登録。
- TIP DLL の署名。Microsoft の IME 要件は第三者 IME への署名を求めている（[Input Method Editors (IME)](https://learn.microsoft.com/windows/apps/develop/input/input-method-editors#requirements-for-imes)）。

### 手順

セッション前に用意した MSI をクリーン VM へ入れ、同一ログオンセッションで Notepad と Microsoft Store の検索欄を対照する。
レーン 1 と同じ MSI を使う。MSI は互換カテゴリ登録を含む commit から作る。
比較対象を同じセッションに揃えないと、プロファイル選択の取り違えと区別できない。

カテゴリ登録そのものは、ホスト側の COM smoke test が 4 カテゴリすべて（`GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT` を含む）を検証している。
MSI を作る前にこれを通しておき、VM 側で GUID を目視照合しない。
`GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT` の値は Microsoft のドキュメントにもリポジトリにも書かれておらず（`msctf.h` はシンボル宣言のみ）、レジストリに並ぶ GUID 文字列のどれが該当するかを実行者が判断できないためである。

```powershell
ctest --preset windows-release -L tsf-com
```

VM 側では、MSI が TIP 登録を行ったこと自体を確認する。
native と `WOW6432Node` の両方を見るのは、`compat-test/msix_install_uninstall.ps1` の残骸判定と揃えるためである。

```powershell
$clsid = '{71EE04FA-B35D-4EB8-87A1-582D44A9A58C}'
foreach ($root in 'HKLM:\Software\Microsoft\CTF\TIP',
                  'HKLM:\Software\WOW6432Node\Microsoft\CTF\TIP') {
  $key = Join-Path $root $clsid
  '{0}: {1}' -f $key, (Test-Path $key)
}
```

`Category` 配下の GUID 一覧は、証跡としてそのまま保存する。
分類は行わず、判定はこの後の打鍵結果で行う。

次に Notepad と Store の検索欄で `ni` を打鍵し、preedit の有無を記録する。

### 結果の扱い

打鍵の結果は次の 2 分岐のどちらかへ入る。
過去の実走がどちらへ入り、何が未確認で残っているかは対象課題のコメントで確認する。

**Store で preedit が出る場合**：カテゴリ欠落が原因だったことが実機で裏付けられる。
MSI 配布形態のゲートの「Microsoft Store / UWP 入力」項目を、スコープ外ではなく Pass として記録する。
Store の検索候補フライアウトが前面に出ると azooKey の候補 UI が隠れるため、フライアウトを Esc で閉じてから Space を押し、候補 UI の描画を確認する。
pipe DACL の AppContainer capability ACE は、preedit が出たうえで候補が来ないかどうかという別の問いなので、そこまで確認して結果をそれを扱う課題へ書く。

**Store で preedit が出ない場合**：署名要件が独立に効いている可能性が残る。
これは診断で解けない種類の残件で、署名済み成果物を用意するまで確定できない。
当該項目は **スコープ外**（`docs/sideload-packaging-spec.md` §0.1）として記録し、次へ進む。
未達として記録しない。MVP の受け入れ条件ではないためである。

このとき、v1.0 以降の署名判断に効く証跡を 2 つだけ採っておくと、後の判断が安くなる。
署名を調達しても Code Integrity Guard により入力が成立しない可能性があり、それを先に否定できれば
署名への投資判断が確実になるためである。

第一に、対象プロセスの署名ポリシーを読む。
`MicrosoftSignedOnly` または `StoreSignedOnly` が立っていれば、第三者証明書では解決しない。

これは `GetProcessMitigationPolicy(ProcessSignaturePolicy)` が返す値だが、PowerShell から素で呼べないため Process Explorer で代替する。
上の `Get-Process` で特定した Store のホストプロセスを選び、プロセスの Properties から Security タブを開いて Signature 系のポリシー表示を読む。
値をスクリーンショットで残し、Notepad の同じ画面も対照として撮る。

第二に、`CodeIntegrity - Operational` ログを Store へフォーカスした時刻で切って保存する。
CIG 由来の user-mode DLL ブロックはイベント 3033 / 3065 に出る。

打鍵の直前に基準時刻を控え、それ以降だけを取る。直近 N 件から絞る取り方だと、打鍵と無関係な過去のイベントを拾う。

```powershell
$since = Get-Date   # Store へフォーカスして打鍵する直前に実行する
# … ここで Store の検索欄にフォーカスし、`ni` を打鍵する …

Get-WinEvent -FilterHashtable @{
  LogName   = 'Microsoft-Windows-CodeIntegrity/Operational'
  Id        = 3033, 3065
  StartTime = $since
} -ErrorAction SilentlyContinue |
  Select-Object TimeCreated, Id, ActivityId, Message |
  Format-List
```

イベント ID が出たことだけを根拠に CIG と判定しない。
3033 は失効した署名や App Control のブロックでも出る。3065 は user-mode の DLL 署名ポリシー違反全般を表す汎用イベントであり、どちらも CIG に固有ではない。

CIG 由来と記録してよいのは、1 件のイベントで次の 3 つが揃った場合に限る。

1. **時刻**が、控えた基準時刻の直後にあること。
2. **プロセスパス**が、`Get-Process` で特定した Store のホストプロセスのパスと一致すること。
3. **ブロックされたファイルパス**が `azookey_tsf_tip.dll` を指すこと。

`Message` にプロセスパスやファイルパスが出ない場合は、同じ `ActivityId` を持つイベント（署名情報を載せる 3089 など）を併せて取り、対応関係を確かめる。

```powershell
Get-WinEvent -FilterHashtable @{
  LogName   = 'Microsoft-Windows-CodeIntegrity/Operational'
  StartTime = $since
} -ErrorAction SilentlyContinue |
  Where-Object { $_.ActivityId -eq '<相関させたい ActivityId>' } |
  Select-Object TimeCreated, Id, Message |
  Format-List
```

3 つが揃った場合にのみ、原因は CIG 側であり署名調達では解決しない、と記録する。
1 つでも欠ける場合は **未確定**とし、CIG と断定しない。
該当イベントが 1 件も出ない場合も未確定であり、IME 署名要件が効いていると結論づけない。
いずれの結果も、Store / UWP をスコープ外とする判断と、v1.0 以降に署名を検討する際の材料を集める課題へ記録する。未確定としたときは、どの条件が欠けたかを併記する。

境界をもう一段だけ詰めるなら、DLL がロードされたかどうかまでを確認して止める。

```powershell
tasklist /m azookey_tsf_tip.dll
```

Store のホストプロセスが一覧に現れず Notepad が現れるなら、失敗はロード前で起きている。
ACL の観測だけで結論を出さない。
ACL が正常なまま DLL がロードされない場合があるため、ACL の状態は原因候補にはなっても判定の根拠にはならない。

対象プロセスの package SID と integrity は、判定の前提となるので記録しておく。
プロセスの列挙は非昇格でも `Path` 付きで取れる。

```powershell
Get-Process |
  Where-Object { $_.Path -like '*\WindowsApps\*' } |
  Select-Object Id, ProcessName, Path
```

package SID と integrity は、対象プロセスのトークンを外から読む必要がある。
`whoami /groups` は自プロセスのトークンしか表示せず、Store のプロセス内で実行する手段が無いので使えない。
Process Explorer の Security タブか、Process Monitor のプロセス詳細から採る。
どちらも使えない場合は、パッケージ名（`Microsoft.WindowsStore_…_8wekyb3d8bbwe` の形）とプロセス名を記録して代える。

pipe ハンドルの有無を見る場合は、`handle` がカーネルドライバをロードするため**管理者 PowerShell** で実行する。
非昇格のまま実行すると、ここで採取が失敗する。

```powershell
& $handleExe -a -p <PID> | Select-String 'pipe|azookey'
```

`$handleExe` と `$procmonExe` は、診断プレイブック「ツールの準備」の解決方法で得る。
Sysinternals の実行ファイル名は配布形式によって 64-bit suffix の有無が異なるため、パスを決め打ちしない。

診断が終わるまで AppContainer 向けの ACE 追加、peer 検証の無効化、token の露出を行わない。
先に緩和すると、どの境界が失敗していたのかを確定できなくなる。

## Part B：VM 状態でレーンを分ける

人間ゲートは要求する VM 状態が異なる。
状態をまたぐたびに checkpoint 復元が要るため、同じ状態のゲートをまとめて走らせる。
実行対象は毎回 Linear の各課題と前提実装の状態で選び、ここに記した順は対象を選んだ後の実行順とする。
各レーンに置く課題の割当は Linear の tracking Issue が持つ。本節はレーンごとの開始状態と順序、ゲート種別ごとの実行条件を定める。

### レーン 1：クリーン VM に MSI を入れた状態

MSI 配布形態そのものを対象とするゲートを置く。
開発登録（`register-dev.ps1`）を先に走らせると、開発登録が付ける AppContainer ACL が MSI 側の継承 ACL と混ざり、ACL 由来かどうかの判別ができなくなる。
このレーンでは開発登録を一切行わない。

開始状態は **vc_redist 未導入のクリーン checkpoint** とする。
`hyper-v-tip-verification.md`「VM 構成」のベースライン checkpoint は「クリーン + Redistributable + 設定」で vc_redist を導入済みであり、これとは別物である。
MSI 配布形態のゲートは VC++ Redistributable 未導入の環境で MSI がインストールできること（CRT の app-local 同梱が効いていること）を見るので、ベースライン checkpoint から始めると前提が崩れたまま Pass になり、証跡の意味が失われる。
どちらの checkpoint を使ったかは、検証メモの環境ブロックの checkpoint 名で残す。

MSI の転送とログの回収は、検証 zip と同じ `vm-verify-session.ps1` に MSI を渡して行う。
`-Prepare` は、VM の現在の状態がクリーン checkpoint から派生していることを確かめる。
そのうえで MSI をゲストの `C:\azookey-verify\msi-<12桁>\` へ転送し、MSI の SHA-256 から
決まる checkpoint（`pre-azookey-msi-<12桁>`）を取って、ホストの結果ディレクトリへ
`msi-record.json` を書く。この checkpoint は MSI を置いただけで導入していない状態なので、
復元すればそのまま再導入でき、`-Prepare` をやり直す必要はない。
`msiexec` は実行しない。導入は人が行う（`hyper-v-tip-verification.md`「エージェントへの委任」）。
`-Prepare`、`-Collect`、`-Restore` は、同節の条件のもとでエージェントに任せられる。

```powershell
# ホスト側。VM をクリーン checkpoint へ復元し、起動した状態で実行する。
.\scripts\vm-verify-session.ps1 -Prepare -VMName "<VM名>" `
  -PackagePath <msi> -CleanCheckpointName "<クリーン checkpoint 名>"
# ゲスト側。-Prepare が表示したパスへ、MSI と同じディレクトリにログを出す。
#   msiexec /i C:\azookey-verify\msi-<12桁>\<msi 名> /L*v C:\azookey-verify\msi-<12桁>\install.log
# ホスト側。msiexec が返った直後に、ゲストの MSI の hash を照合してログを回収する。
.\scripts\vm-verify-session.ps1 -Collect -VMName "<VM名>" -PackagePath <msi>
```

`-Collect` は、MSI と同じディレクトリにある `*.log` を
`build\vm-verify-results\<msi 名>-<12桁>\logs-<UTC 時刻>\` へ回収し、同じ場所に
`msi-identity.json`（ホストとゲストの MSI の SHA-256）を書く。
ゲストの MSI の hash が `-PackagePath` と異なれば回収しない。

1. MSI 配布形態のゲート（インストール、配置物、IME プロファイル、打鍵、ACL の継承、アンインストール）のチェックリストを頭から実施する。
2. Microsoft Store / UWP 入力の項目は **スコープ外**として記録し、素通りする（`docs/sideload-packaging-spec.md` §0.1）。Part A を実施した場合のみ、その結果を併記する。
3. 続けて設定アプリのゲート（起動・二重起動・アンインストール残留物）を実施する。同じクリーン VM 状態を使うため、手順 1 のアンインストール確認と順序を合わせる。手順 1 は TIP と COM 登録、本手順は設定 EXE・WinUI ランタイム payload・ショートカットを見る。1 回のアンインストールで両方を見て、それぞれの記録へ書く。
4. MSI のアップグレードを見るゲートは、新旧 2 つの MSI を要する。クリーン checkpoint から新版の新規インストールとアンインストールを確認し、クリーン checkpoint へ戻してから旧版を入れ、新版へのアップグレードで旧版が残らないことを確認する。`pkg/msi/Package.wxs` の `MajorUpgrade` は同じ版を上位更新として扱わないため、新版の ProductVersion が旧版より高いことを開始条件とする。2 つの MSI のファイル名、ProductVersion、SHA-256 を検証メモに記録する。

次の確認は MSI を入れた状態でしか行えない。
レーン 2 以降では状態を作れないので、実施する場合はこのレーンで行い、実施しない場合は未実施と理由を各課題に記録する。

- ログオン常駐：ログオン時のコンソールウィンドウの表示と、ConstrainedLanguage または PowerShell 5.1 を無効にした環境での launcher の診断。どちらも MSI が登録するログオン経路（`start-installed-host.ps1`。`docs/sideload-packaging-spec.md`「MSI のログオン常駐」）のもので、開発登録は launcher を経由しない。後者は VM の状態を変えるので、checkpoint を取ってから行う。
- 診断：clean VM での IME 動作と、アンインストール後の診断ファイルの残留。手順 1 と手順 3 のアンインストール確認と同じ機会に見る。

Store 入力の可否は MSI 配布形態のゲートの合否を左右しない。
MVP が入力先として保証するのは Win32 デスクトップアプリであり、Store / UWP は MVP の対象外だからである。
除外の境界は AppContainer を使うかどうかではなく、パッケージ化された UWP / Store アプリかどうかで引く（§0.1）。
Edge は renderer のサンドボックス化に AppContainer を使うが、入力欄をホストするのは Win32 プロセスであり対象内である。通常どおり Pass / Fail で判定する。

課題本文が挙げる `%ProgramFiles%\azooKey` の配置物と、実際の配置物に差があることがある。
その場合は配置物として存在することの確認にとどめ、差分があった事実を検証メモへ書く。
設定アプリの起動とアンインストールは手順 3 のゲートが扱う。手順 1 で見たことを理由に手順 3 を飛ばさない。

#### MSI の保守操作（修復・再インストール）とログ回収

修復や再インストールは、レーン 1 のチェックリストを終え、保護
checkpoint を取ってから行う。

修復は `/f` 系ではなく、`/i` にプロパティを付けて実行する。

```powershell
msiexec /i <msi> REINSTALL=ALL REINSTALLMODE=amus REBOOT=ReallySuppress /qn /L*v <log>
```

`/f` は
[コマンドラインのプロパティ値を無視する](https://learn.microsoft.com/windows/win32/msi/command-line-options)。
`/norestart` は `REBOOT=ReallySuppress` と同じ意味なので、`/fa <msi> /qn /norestart`
では再起動の抑止が効かない。TIP DLL と同梱の CRT は TSF を使うプロセス（explorer、
メモ帳など）に読み込まれているため、修復では使用中ファイルの置換が起きる。抑止が
効いていなければ、Installer は `/qn` のまま再起動を開始して `1641` を返す。
上のコマンドは `/i` なのでプロパティが渡り、
[`REBOOT=ReallySuppress`](https://learn.microsoft.com/windows/win32/msi/reboot)
が使用中ファイルによる終了時の再起動を抑止する。同じ状況では `3010` が返る。

`msiexec` には `/L*v` で verbose ログを MSI と同じゲスト内のディレクトリへ出し、**操作が
返った直後に `-Collect` でホストへ回収する**。VM がサインイン前の画面へ戻ると PowerShell
Direct でのファイル取得経路が失われる。その場合は VM を停止したうえで、現行ディスクを
ホストの管理者 PowerShell から `Mount-VHD -ReadOnly` でマウントして回収する。
ゲストの OS ボリュームが BitLocker で保護されていて解錠できなければ、この経路は使えない。
回収してから次の操作へ進み、回収できていないログを前提に原因を推定しない。

終了コードは
[MSI error codes](https://learn.microsoft.com/en-us/windows/win32/msi/error-codes)
で分類する。`0` 以外がすべて失敗ではない。

| 終了コード | 意味 | 扱い |
|---|---|---|
| `1641` | `ERROR_SUCCESS_REBOOT_INITIATED` | 成功。Installer が再起動を開始した。`/f` 系の修復では `/norestart` が無視されるため、使用中ファイルがあればこれが返る（上記）。`REBOOT=ReallySuppress` を渡した実行でこれが返った場合は別の経路なので、verbose ログを回収して調べる |
| `3010` | `ERROR_SUCCESS_REBOOT_REQUIRED` | 成功。再起動は呼び出し側が行う。TIP を読み込んだプロセスがあれば、修復や更新ではこれが返る |
| `1638` | 同一製品の別バージョンが導入済み | 対象の版を確認する |

回収した verbose ログでは、サーバー側の `Command Line:` 行に `REBOOT=ReallySuppress`
が載っているか、`Info 1603`（日本語ログでは `情報 1603`。使用中のファイルと保持プロセス）、`ReplacedInUseFiles`、
`ScheduleReboot` / `ForceReboot` の有無、`MainEngineThread is returning` の値を見る。
併せてゲストの System イベントログから再起動関連イベントを採る。イベント 1074 は
再起動を開始したプロセスを示し、MSI 由来か別の更新由来かを切り分けられる。
再現は保護 checkpoint から分離して行う。

MSI を入れる前の状態へ戻すときは、`-Restore -PackagePath <msi>` で MSI の checkpoint へ復元する。
レーン 1 が終わったら、レーン 2 の開始状態（「VM 構成」のベースライン checkpoint）へ復元する。
MSI の machine-wide 登録を残したままレーン 2 の開発登録を重ねると、どちらの登録が効いているか判別できなくなる。

### レーン 2：開発登録と検証 zip を入れた状態

`verify-bootstrap.ps1` で導入した状態で走らせるゲートを置く。
次の順に、可逆な設定と学習データ、通常の打鍵、VM 状態を変える操作を分ける。
前提実装と個別の合格条件は各課題と対応する spec で確認する。

DPAPI 移行（M34）のゲートが見る平文から暗号化への移行は、手順 1 より前に行う。
Host は最初の起動で平文ストアを暗号化するため（`docs/sideload-packaging-spec.md` §9.4・§9.5）、移行はその 1 回でしか観測できない。
手順 1 の `-Run` と `verify-bootstrap.ps1` は Host を起動するので、先に走らせると移行前の状態を作れない。
`-Prepare` の後、Host を一度も起動していない状態で合成の平文ストアを配置し、checkpoint を取って移行前を記録する。
最初の Host 起動は手順 1 の `-Run` に任せ、`-Run` の後に移行後を記録して比較する。
`-Run` は compat の打鍵も行うので、移行後の記録には compat による学習の書き込みも入る。移行の根拠は `Changed: yes` ではなく、平文の `removed`、`.enc` と `.bak` の `added`、`.bak` の SHA-256 が fixture と一致すること、README の件数で見る。
`-Prepare` は zip を転送するだけで展開しない。移行前の記録に使う `learning-data-snapshot.ps1` は、検証 zip から `-Run` の展開先（`C:\azookey-verify\<zip の basename>`）とは別のディレクトリへ取り出して使う。
`-Run` は展開先を自分で作り直すので、同じ場所へ手で展開して使用中のままにすると `-Run` が止まる。手で取り出した側から `register-dev.ps1` と `verify-bootstrap.ps1` を実行しない。
配置先、移行前後の記録、件数の確認は [`scripts/vm-verify-fixtures/README.md`](../../scripts/vm-verify-fixtures/README.md)「平文ストア」に従う。
以降の手順は暗号化済みの状態のまま進めてよい。
同じゲートの残りの項目は、設定アプリを操作する手順 2（API キーの `dpapi:` 保存と D-014）と、別ユーザーを使う手順 6（他ユーザーでの復号失敗）で確認する。

1. **先行自動判定**：`-Run -CompatSkip C-006,C-013,C-010` で Host 停止・DPI 変更を伴わない compat ケースを先に採る。`verify-bootstrap.ps1` が失敗したら、打鍵ゲートへ進まない。runner の結果は人の TIP 操作・目視判定を代替しない。
2. **設定反映と学習不変**：設定反映のゲートを設定系ゲートの最初に置き、既存 TIP 接続への設定反映と新規アプリとの差を確認する。設定反映で変える `batchRomajiConversion` / `batchConversionMode` / `batchAutoPunctuation` は設定アプリに UI が無い（`docs/sideload-packaging-spec.md` の設定アプリの UI 化の表で M58）ので、`%LOCALAPPDATA%\azooKey\config\settings.json` を直接書き換えて確かめる。`batchConversionMode` は `batchRomajiConversion=true` のときだけ効く。その後、M46 の secure 抑止とインジケータ、M14 の secure 抑止の項目、M58-B/C の AI 整文で学習しない項目を行う。各項目の直前・直後に学習データを記録して比較し、secure から通常入力へ戻る確認も記録する。M58-B/C で実 API を使う場合は承認済みの接続先だけを使い、キーと入力本文を証跡へ載せない。M58-B/C の後に AI 整文のエラー分類通知を行う。外部 API を使うかは人が決める。使わない場合は、分類ごとに次の手段で誘発する（分類の定義は `docs/ai-backend-spec.md` §7.2）。stub を使うときは `aiBackend=openai` とし、`openAiApiKey` に機密でない非空のダミーキー（`dpapi:` で始まらない平文）を設定する。キーが空だと HTTP 接続の前に `Auth` になり、ほかの分類を観測できない。`RateLimit` と再試行待機は HTTP 429 でしか起きないため、`openAiApiEndpoint` をゲスト内のループバック stub（`http://127.0.0.1:<port>/v1`）へ向け、429 と `Retry-After` を返させる。平文 HTTP が許されるのは `127.0.0.1` と `[::1]` の直書きだけで、`localhost` は拒否される。同じ stub に 401 を返させると `Auth`、応答を設定したタイムアウトより遅らせると `Timeout` になる。待ち受けていないポートを指すと `Network`、`dpapi:` キーを復号できない状態にすると `KeyReentry` になる。続けて M17 のカスタムローマ字で `inputStyle=custom` と `customRomajiTablePath` の TSV を指定し、保存し直したときの反映と空・不正 TSV での内蔵表への復帰を確認する。確認後は `inputStyle` を元に戻してから次へ進む。
3. **通常の入力とアプリ巡回**：Notepad → VS Code → Edge を 1 巡し、下の統合表を使う。M13（M3〜M10 回帰）を基準に M14、M15、M61-A/B を確認する。M14 と M15 は M13 の結果を前提にそれぞれ判定する。M61 の per-app 設定切替は手順 2 の設定反映の確認後に行う。候補 UI の描画責務と表示属性のゲートが要求する残りのアプリも、それぞれの課題で確認する。Notepad では M20 再変換の選択からの変換キー、選択直後の右クリック、選択や位置を変えた後に古い候補が出ないことも確認する。Office を導入した VM では同じ確認を Office でも行い、未導入なら未実施として記録する。
4. **中間 checkpoint と DPI**：巡回の証跡を回収して checkpoint を取る。ゲストの表示スケールを 150% に変えて再サインインし、compat の C-006 と表示属性の D-08（Notepad / VS Code / Edge）を確認する。D-08 は 200% へ変更して再サインインした状態でも 3 アプリを確認する。各スケールで OS の設定値、操作結果、runner の `report.json`、目視記録を復元前にホストへ回収する。最後に設定を戻すか checkpoint へ復元し、登録と Host の Ready を確認してから障害注入へ進む。
5. **無応答と Host kill**：TIP の info ログ、ETW、Host ログの採取を先に開始する。C-013 の一時停止と復帰を先に行い、M42 の無応答・ローカル fallback・Ready 復帰を人が確認する。C-013 の実アプリ結果（Notepad / VS Code / Edge の `report.json` と TIP JSONL）は、C-013 の実アプリ確認を扱う課題に記録する。次に C-010 の Host kill を 1 回行い、compat、M42、ログオン常駐（実ホスト kill からの復帰）の各ゲートで同じ実走を参照する。Host 不在時の M61-A の基本ペア動作も確認する。C-013 と C-010 の実行中は、M47 の `degraded_simple` の表示と消去も観察する。各課題の期待値と判定は別々に記録し、Host が Ready に戻ったことを確かめる。M47 の `degraded_model` は、モデルを指定せずに Host を起動し、存在しないパスか破損した GGUF をロードさせて誘発する。ロード済みのモデルがあるまま差し替えに失敗しても劣化として扱われない（`docs/host-availability-spec.md` §8.5.1）。復帰は正しいパスでの再ロードで確認し、検証 zip のモデル指定へ戻してから次へ進む。SafeMode は Host の連続クラッシュ（`docs/host-availability-spec.md` §8.5.3）で誘発する。SafeMode は手動解除まで残るため、誘発前に checkpoint を取り、確認後に復元してから次へ進む。
6. **ログオンとユーザー変更**：ログオン常駐のゲートのうち、ログオン自動起動、別ユーザー provisioning、監督停止の各項目を行う。ログオフや別ユーザーへの切替は元の対話セッションを変えるので、通常の打鍵と障害注入の後に置く。監視失敗や連続失敗による supervisor の停止と、再ログオンまたは手動の再起動での復帰（`docs/sideload-packaging-spec.md`「MSI のログオン常駐」が定める停止条件。開発登録も同じ `host-supervisor.ps1` を使う）は VM の状態を変えるので、ほかの項目を終えて checkpoint を取ってから行う。ログオン常駐の確認のうち、ログオン時のコンソールウィンドウの表示と launcher の診断は MSI のログオン経路のものであり、このレーンでは観測できない（レーン 1）。

| 訪問順 | 同じ訪問で確認する項目 |
|---|---|
| Notepad | M13 のキー回帰、M14 の live preedit、M15 の予測窓、M20 の再変換、M61 の基本・拡張挙動、候補・句読点・キャレット、`pbShow`、DisplayAttribute |
| VS Code | M61 の per-app 有効範囲、候補・句読点・キャレット、`pbShow`、DisplayAttribute |
| Edge | M13 の UI-less 入力、候補・句読点・キャレット、`pbShow`、DisplayAttribute |

各アプリで候補・句読点・キャレットの確認と `pbShow` の計測（`compat-test/m5_uiless_pbshow_checklist.md`）を続けてよい。
表示属性の対象アプリと省略できる項目は `compat-test/m3_display_attribute_checklist.md` §3・§3.1 に従う。
Notepad / Edge / VS Code でも描画属性 D-02 / D-03 / D-07 / D-08 / D-10 の目視は省略しない。
単一ディスプレイの基本セッションでは D-09 のモニタ跨ぎを実施できない。全画面での観察と区別して表示属性の課題に D-09 未実施・環境制約を記録し、異なる DPI の複数モニタを使う別セッションで検証する。`compat-test/m3_display_attribute_checklist.md` §5 の完了条件は緩めず、D-09 の実走前に表示属性の課題を完了扱いにしない。
巡回を統合しても検証メモは課題ごとに分けて残す。
課題ごとの未実施項目と理由も、その課題のコメントに書く。

学習データの状態は、ゲストに展開した検証 zip の `learning-data-snapshot.ps1` で記録する。
学習データ不変のゲートは自由な打鍵より先に行う。順序を変える場合も、各ゲートの境目でラベルを付けて記録し、直前・直後の 2 つを比べる。

```powershell
$snapshot = "C:\azookey-verify\learning-snapshots.json"
powershell -ExecutionPolicy Bypass -File .\learning-data-snapshot.ps1 -Label before-secure -OutputPath $snapshot
# … ゲートの操作 …
powershell -ExecutionPolicy Bypass -File .\learning-data-snapshot.ps1 -Label after-secure -OutputPath $snapshot
powershell -ExecutionPolicy Bypass -File .\learning-data-snapshot.ps1 -From before-secure -To after-secure -OutputPath $snapshot
```

比較結果の `Changed: yes` は、学習データのストア（`learning.v2.tsv`、旧 `learning.tsv` などとその `.enc`、`.bak`）にハッシュの変化、ファイルの追加または削除があったことを示す。
各行の `size` と `lines` は直前の記録からの差分であり、ファイルのサイズではない。`size 0` はファイルが空であることを示さない。
更新時刻だけの変化は `touched` と表示し、変化に数えない。
Host が起動と終了で書き換える `host_run_state.txt` などは `not learning data` と付けて表示し、変化に数えない。
`Unverified` が出た場合は、読めなかったストアや未知ストアの警告を調べ、原因を解消してラベルを変えて記録し直す。
`Warning: no learning store` が出た場合の `Changed: no` は証跡にならない。データディレクトリと、IME を使うユーザーのシェルで実行したかを確かめる。
未知の `*.tsv`、`*.tsv.enc`、`*.json.enc` に対する警告が出た場合も、`Changed: no` だけで学習不変と判定しない。Host のストア名と記録スクリプトの対象を照合してから記録し直す。
Host が平文のストアを読み込むと `.enc` と `.bak` への移行が起き、追加と削除が出るので、基準のラベルは Host 起動後に取る。
記録するのはハッシュ、行数、サイズ、更新時刻だけで、学習データの本文は出力に含まれない。
JSON は他のゲスト出力と一緒にホストへ回収する。

ログオン常駐の別ユーザー provisioning は、VM に第 2 のローカルユーザーが要る。
セッション前に作っていない場合、この項目だけ実施できない。

compat の分割実行順は本レーンが正典であり、`hyper-v-tip-verification.md` の「ホストからの一括実行（`-Run`）」から参照される。
分割実行例を示す。共有する 1 回の Host kill は Notepad target で行い、障害注入前の自動判定とは分ける。
`--output` が既存の非空ディレクトリを指すと runner は実行を拒否するので、実行ごとに出力先を変える。

```powershell
.\compat_test.exe --target .\targets\notepad.json --skip C-006,C-013,C-010 --output C:\azookey-verify\compat-notepad-early
# 巡回後の checkpoint を取り、150% へ変更・再サインインして実行する
.\compat_test.exe --target .\targets\notepad.json --cases C-006 --output C:\azookey-verify\compat-notepad-dpi-150
# 150% の証跡をホストへ回収してから 200% へ変更・再サインインして実行する
.\compat_test.exe --target .\targets\notepad.json --cases C-006 --output C:\azookey-verify\compat-notepad-dpi-200
# TIP の info ログと ETW を開始し、打鍵を終えてから実行する
.\compat_test.exe --target .\targets\notepad.json --cases C-013 --output C:\azookey-verify\compat-notepad-hang
.\compat_test.exe --target .\targets\notepad.json --cases C-010 --output C:\azookey-verify\compat-notepad-kill
```

`vm-verify-session.ps1 -Run` の先行自動判定には `-CompatSkip C-006,C-013,C-010` を渡す。
DPI の後に全 target を判定する場合は `-CompatCases C-006` を渡せる。
`-Run -CompatCases C-013` / `C-010` は全 target に障害を注入するため、1 回の C-010 を複数のゲートで共有する場合は上の Notepad target だけを実行する。
全 target を `-Run` で分ける場合は次の順に実行する。

```powershell
# 1. Host を停止しないケースだけを先に回し、compat の自動判定を取る
#    表示スケールを途中で変える本レーンの先行自動判定では、-CompatSkip に C-006 も加える
.\scripts\vm-verify-session.ps1 -Run -VMName "<VM名>" -CompatSkip C-013,C-010
# 2. 打鍵確認（基本セッション）を行う
# 3. Host 無応答と復帰の C-013 を回す（対話タスクが TIP の info ログを有効にする）
.\scripts\vm-verify-session.ps1 -Run -VMName "<VM名>" -CompatCases C-013
# 4. 最後に Host kill と復帰の C-010 だけを回す
.\scripts\vm-verify-session.ps1 -Run -VMName "<VM名>" -CompatCases C-010
```

C-013 の直接実行前には `hyper-v-tip-verification.md` に従い、対象プロセスを起動し直して TIP の info ログを有効にする。
`--cases` と `-CompatCases` は必要な C-001 を自動で追加する。選択と除外の詳細は
`compat-test/README.md` と `hyper-v-tip-verification.md` に従う。
各回の `report.json` と `case_selection` を保存し、未実行ケースを Pass と数えない。
追加された C-014 / C-016 はライブ変換 OFF、C-018 は ON の report で判定する。設定を切り替える場合は `tip-manual-verification-checklist.md` の compat 分担に従い、別の出力先で実行して設定と `case_selection` を記録する。反対の設定での `failing-skip` を Pass に読み替えない。C-015 / C-017 / C-018 / C-019 の自動判定は M13〜M15 の人の表示・操作判断を代替しない。
ゲスト内で直接実行した C-006 の出力は `-Run` と違って自動回収されない。150% と 200% の実走ごとに異なる出力ディレクトリを指定し、PowerShell Direct の `Copy-Item -FromSession` で `report.json` を含むディレクトリをホストの別々の回収先へコピーする。表示属性のスケール別目視記録・画像もホストへ保存し、ファイルの存在と対象スケールを確認してから checkpoint を復元する。回収方法の前提と資格情報の扱いは `hyper-v-tip-verification.md` の手順 3 に従う。

終了コードは、全件 pass が `0`、fail を含む場合が `1`、fail は無いが failing-skip を含む場合が `2` である。

C-007（サロゲートペアの確定）は、azooKey の候補から絵文字を確定する経路を要求する。
絵文字リライターがその前提機能だが、ユーザー辞書の writer 経路
を使えば、リライターを待たずに同じ確定経路を作れる。
`UserDictionary` の登録内容は `SimpleConverter` の候補生成へ渡っており、surface に文字種の制限は無い。

```powershell
$exe = (Resolve-Path .\azookey_inference_host.exe).Path
& $exe userdict add --reading えもじ --surface 😀
```

登録したエントリを打鍵して候補から確定し、アプリ側の文字列でサロゲートペアが壊れないことを見る。
ZWJ でつないだ列を含む複数のエントリをまとめて取り込む場合は、`scripts/vm-verify-fixtures/README.md`「絵文字のユーザー辞書」の TSV と手順を使う。
候補に出ないか確定で壊れる場合は、絵文字リライターを要する未達として compat の課題へ記録する。

C-005（マルチディスプレイ端の候補クランプ）は本セッションの対象外で、マルチディスプレイ構成の別セッションで扱う。
基本セッションでは構成を作れないため `failing-skip` として残るが、runner は環境条件を満たせないケースを silent skip せず記録するので、誤って Pass にはならない。
したがって C-005 に起因する終了コード `2` は失敗ではなく、compat のゲートの判定には含めない。

### レーン 3：昇格と登録状態を変える検証

昇格した登録・解除とロールバック、ETW とクラッシュ診断の実機設定・採取、WPR profile の実採取と WPA での読込み、Application Verifier のゲートを置く。
WPR の実採取は ETW 診断と同じ管理者 PowerShell で、`-IncludeDiagnostics` 付きの検証 zip を
展開した診断資産を使い、[`windows-diagnostics-playbook.md`「IPC timeout の再現と採取」](./windows-diagnostics-playbook.md#ipc-timeout-の再現と採取)
の手順で manifest の hash 照合・登録・採取・解除を行う。WPA で開けるだけでは合格にしない。
Generic Events で provider `azooKey-Desktop` に絞り、同じ `client_id` / `request_id` の
3000 → 3003（FrameWrite）→ 4000 → 4002 → 4001 → 3003（FrameRead）→ 3001 が現れるかを人が判定する。
ETL は Git へ入れず、WPA または `Get-WinEvent -Path` での解析を終えてから、独立した手順で
`wevtutil um` により manifest の登録を解除する。解析を後で行う場合も、停止直後には解除しない。
`AzooKeyDiagnostics` プロファイルの ETL は IPC のフレームと Health を含むため、十数分で GB 単位に育つ。
採取は確認する操作ごとに短く区切る。WPA を使わずに個々のイベントを読むときは、`tracerpt` で XML 化するとメモリが足りなくなるので、`Get-WinEvent -Path <ETL> -Oldest -FilterXPath` で provider `azooKey-Desktop` と必要なイベント ID に絞る。
管理者権限を使い、登録・診断設定や対象プロセスの状態を変えるので、レーン 2 の観察と証跡回収を終えた後に走らせる。
各課題が要求する権限、専用成果物、解除条件を課題本文と対応する診断手順で確認する。
登録ロールバックの失敗注入に Debug ビルドが必要なら、通常の検証 zip と混ぜず別パッケージとして用意し、保護 checkpoint から実施する。
登録失敗後の HKLM CLSID / TSF profile / category の残骸、再登録、解除を確認してから次へ進む。
登録ロールバックの解除後、ETW 診断と Application Verifier のために開発登録済みの保護 checkpoint へ復元する。
ETW 診断のトレースと dump は入力本文や秘密情報の混入を確認し、収集設定を元へ戻す。
clean VM での IME 動作と、アンインストール後の診断ファイルの残留は、開発登録のこのレーンでは確認できない。
レーン 1 で確認していなければ、未実施として理由を課題に記録する。

Application Verifier はこのレーンの最後に走らせる。
Application Verifier は対象イメージの設定を registry（Image File Execution Options）へ書くため、有効なままでは以降のどのゲートも汚染された環境で走ることになる。

`windows-asan` の網が届くのは azooKey 側が生成するプロセスに限られ、実ホストアプリのプロセスへ in-proc ロードした状態は恒久的に対象外である（`docs/dev-infrastructure-spec.md` §4.6.1）。
このレーンはその範囲だけを埋める。

開始状態はレーン 2 の証跡を回収した後の保護 checkpoint とし、昇格操作ごとに必要なら復元する。

対象は `notepad.exe` に限定する。
`compat-test` が Win32 TSF ホストの基準として使っており、TIP の動線が最も素直に出るためである。
Edge と VS Code へは掛けない。

有効化から解除までを `try` / `finally` で囲み、途中で打鍵が失敗しても解除が走るようにする。
`appverif.exe` は管理者権限を要求する。

```powershell
$target = 'notepad.exe'
try {
  appverif.exe -enable Heaps Handles Locks -for $target
  # notepad を起動し、azooKey へ切り替えて A1〜A8 の打鍵を行う。
  # 停止したら debugger を接続し、verifier の停止コードと stack を採取する。
} finally {
  appverif.exe -disable '*' -for $target
}
appverif.exe -query '*' -for $target
```

`-query` の出力に設定が残っていないことを確認するまで、このレーンを終えない。
`finally` を置いても、PowerShell 自体が途中で落ちれば解除は走らない。

Application Verifier の停止はブレークであり、debugger を接続していないと対象プロセスがそのまま終了して情報が残らない。
検出があった場合は停止コードと stack を課題のコメントへ記録し、file:line と再現手順を添えて別課題を起こす。

### Windows CLI / コンソールの別枠

TIP 登録を必要としない非 ASCII argv、非 ASCII パスの GGUF 実ロード、user_dict の 2 writer、コンソール終了時の flush のゲートは、Windows CLI / コンソールの別枠で行う。
同じ VM を使う場合も対話 TIP レーンの進行条件にせず、各課題の前提と合格条件を確認して個別に記録する。
user_dict の 2 writer は稼働中 Host と offline CLI の同時実行を要するが、TIP の打鍵は要しない。
コンソール終了時の flush はコンソールを終了させるため、レーン 2 の Host と混同しない。

非 ASCII の 2 ゲートとコンソール終了時の flush のコマンドと fixture は [`scripts/vm-verify-fixtures/README.md`](../../scripts/vm-verify-fixtures/README.md) が持つ。
本節は、README に無い実行条件だけを定める。

| ゲート | README の節 | 実行条件 |
|---|---|---|
| 非 ASCII argv | 「非 ASCII パス」 | VMConnect の基本セッションの対話コンソールから実行する。PowerShell Direct 経由の実行は、課題が求める通常コンソールの条件を満たさない |
| 非 ASCII パスの GGUF 実ロード | 「非 ASCII パス」 | 生成した `modelPath` を `--model` と `model.selectedPath` の両方で確認する。レーン 2 の Host と pipe を取り合うので、レーン 2 と 3 の証跡を回収した後に、supervisor ごと止めてから行う |
| コンソール終了時の flush | 「stdio の行」 | 終了経路ごとに新しいデータルートで `--stdio` の Host を起動し、最初に Handshake の行を送る |

非 ASCII の 2 ゲートは active code page で結果の意味が変わる。
課題が求める code page と異なる環境で実行した場合は、実行した code page での結果として記録し、求められた code page を未実施と明記する。
code page の値は fixture の生成スクリプトが `paths.json` に書く。

非 ASCII パスの GGUF 実ロードでは Host だけを止めても足りない。
モデルを指定して登録した開発登録の supervisor は、止まった Host をそのモデルを指す `--model` 付きで起動し直し、明示した `--model` は `model.selectedPath` より優先される。
`model.selectedPath` 側の確認は、supervisor を止めたうえで `--model` を付けずに pipe で起動した Host に対して行う。
`azookey_diag.exe --json` の D-008 は、pipe で応答している Host のロード済みパスを `model.selectedPath` と比べるので、この状態で採った D-007 と D-008 を記録する。
確認の後は checkpoint へ復元するか、登録をやり直して supervisor を戻す。

コンソール終了時の flush の stdio 手順は、Handshake を送ってから CommitObservation を送る。
Host が学習を受け付けるのは、Handshake で `secure_flag` を申告した接続だけである。
Handshake を省くと CommitObservation は `"ok":false` になり、学習データが作られないので、終了経路の保持と消失を判定できない。
学習データは暗号化されているため、保持の確認はファイルの行数ではなく README の `lookup` で行う。
貼り付けた行が Host に届かなかった場合は、その事実を検証メモに書く。

判定は `docs/learning-data-management-spec.md` §11 の区分に従う。
Ctrl+C は保証経路であり、消失すれば Fail である。
`×` 終了は best-effort 経路なので、消失そのものは Fail ではない。
§11.2 の burst 先頭の同期 flush が入った状態では、単発 commit 直後の `×` は終了経路によらず保持されるため、消失していれば実装が効いていない証拠として記録する。
連続 commit の途中で終了した場合は、消失が §11.3 の上限（既定で 7 件かつ 5 秒ぶん）に収まるかを見る。
タスクマネージャの強制終了とログオフは対象外であり、実施せず、未達としても記録しない。

#### user_dict の 2 writer 実走

`userdict` CLI は既定で稼働中の Host へ IPC 経由でコマンドを送り、`--offline` を付けるとファイルを直接書く。
この 2 経路を別々の entry で重ねると、二つの書き手が同じ `user_dict.json` を read-modify-write する状況になる。
単発の `--offline` を 1 回実行するだけでは書き込みが重ならず、ロックの効きを確認できない。

`Start-Job` へ相対パスを渡さない。
Windows PowerShell 5.1 は子ランスペースをユーザーのホームで開始するため、`.\azookey_inference_host.exe` は解決に失敗する。

この手順は、Host と同じ対話セッションのコンソールか、コンソールユーザーのスケジュールタスク（LogonType Interactive）から実行する。
PowerShell Direct のセッション（Session 0）からは Host の per-user pipe に接続できず、IPC 経由の `add` が `failed to connect to running host` で失敗する。
その場合は `--offline` 側だけが書き込まれ、2 writer の確認にならない。

```powershell
$exe = (Resolve-Path .\azookey_inference_host.exe).Path
$viaPipe = Start-Job { & $using:exe userdict add --reading ぱいぷ --surface パイプ }
$viaFile = Start-Job { & $using:exe userdict add --reading ふぁいる --surface ファイル --offline }
Wait-Job $viaPipe, $viaFile | Out-Null
Receive-Job $viaPipe, $viaFile
& $exe userdict list --format json
```

`Receive-Job` で両ジョブが実際に実行されたことを確かめてから、`list` に両 entry が残るかを見る。
同じ操作を数回繰り返す。片方だけなら編集消失として記録する。
`settings.json` はこの userdict ゲートの対象外である。設定アプリ・Host の保存責務と共有ロックの契約は `docs/learning-data-management-spec.md` §14に従う。

### 別環境・前提が必要なゲート

次の前提を要するゲートは、前提をそれぞれの課題で確認してから別計画に入れる：TIP 側の検出トリガ、Store 用 MSIX、物理 ARM64 機、マルチディスプレイ構成、量子化別の RSS 測定。
ホスト側のベンチと変換検証は、VM の対話セッションと分ける。

## Part C：検証メモのひな形

Linear への記録様式を揃えておく。
各ゲートの完了条件は「検証メモを当該課題へコメントする」ことであり、記録が揃わないと Done へ遷移できない。

全ゲート共通で先頭に置く環境ブロック。
環境記入欄は本節が正典であり、`tip-manual-verification-checklist.md`「検証環境（記入）」から参照される。

環境ブロックの機械で埋まる欄と自動観測の件数は、VM から回収した出力からホスト側で生成できる。
`vm-verify-session.ps1 -Run` は、対話セッションで採取した `verify-bootstrap.ps1 -Json` の出力、`azookey_diag.exe --json` の出力、compat の `report.json` を 1 つの実行ディレクトリへ回収する。
`winver` の OS ビルド番号を控えてから、その実行ディレクトリを入力にして次を実行する。

```powershell
$run = ".\build\vm-verify-results\<zip basename>-<UTC 時刻>"
pwsh -File .\scripts\vm-verify-summary.ps1 `
  -ManifestPath .\build\vm-verify-packages\<zip basename>.manifest.json `
  -BootstrapJsonPath "$run\bootstrap-interactive.json" `
  -DiagJsonPath "$run\azookey-diag.json" `
  -CompatReportPath "$run\compat-report-notepad\report.json", "$run\compat-report-vscode\report.json" `
  -OsBuild <OS build> `
  -OutputDirectory .\build\vm-verify-summary
```

`-Run` を使わずに手で実行した場合は、同じ 3 種類の出力を VM 内で保存してホストへ回収し、それぞれのパスを渡す。

`verify-bootstrap.ps1 -Json` の出力は、セッション開始時の開始条件の観測としてだけ使う。
サマリの `-BootstrapJsonPath` には、最初の `-Run` の実行ディレクトリのものを渡す。
登録の状態や Host の起動を観測対象にするゲートでは、操作の後、状態を記録する前に `verify-bootstrap.ps1` を実行しない。`-Run` も bootstrap を実行するので同じ扱いとする。
bootstrap は未登録なら TIP を登録し、pipe が無ければ supervisor を起動するので、登録ロールバックの解除の後やログオン常駐の確認の前に実行すると、ゲートが見たい状態を書き換える。
レーン 2 の手順 4 と 5 で compat を分けて回す `-Run` は、この制限に当たらない。
操作の後の状態を根拠にする場合は、読み取りだけを行う `azookey_diag.exe --json` を採り直してサマリを作り直す。

生成された `verification-summary.md` を検証メモの先頭に貼り、空欄を人が埋める。
サマリは観測値の集約であり、ゲートの合否は各課題の判定基準で人が決める。
入力の schema と分類は `docs/dev-infrastructure-spec.md` §2.6 を参照する。

課題ごとの転記下書きが必要な場合は、ゲート ID と課題の対応表を渡して次を実行する。
対応表は repo の `scripts/vm-verify-fixtures/gate-map.json` を使う。
bootstrap、diag、compat の ID を載せており、各対応の根拠と、対応を置かない ID は `scripts/vm-verify-fixtures/README.md`「gate map」にある。
対応表の各行は `source`（`bootstrap` / `diag` / `compat`）、`id`、`issueId` と、必要なら compat の `targetId` を指定する。
同じゲートを複数課題へ記録する場合は行を分ける（例: 1 回の C-010 を複数の課題の根拠にする場合）。
対応表は実施するゲートに合わせて確認し、下書きの出力後に各課題の確認項目を人が埋める。
対象の課題を入れ替えるときは、`gate-map.json` と README の表を同じ変更で直す。

書式は次のとおり。

```json
{
  "schemaVersion": 1,
  "gates": [
    { "source": "bootstrap", "id": "inferenceHost", "issueId": "<課題 A>" },
    { "source": "diag", "id": "D-004", "issueId": "<課題 B>" },
    { "source": "compat", "id": "C-010", "issueId": "<課題 C>" },
    { "source": "compat", "id": "C-010", "issueId": "<課題 A>" },
    { "source": "compat", "id": "C-010", "issueId": "<課題 B>" }
  ]
}
```

```powershell
pwsh -File .\scripts\vm-verify-linear-drafts.ps1 `
  -SummaryPath .\build\vm-verify-summary\verification-summary.json `
  -GateMapPath .\scripts\vm-verify-fixtures\gate-map.json `
  -CheckpointName <checkpoint name> `
  -OutputDirectory .\build\vm-verify-drafts
```

`-CheckpointName` を省くと checkpoint 名は空欄になる。`DEV-番号.md` の各行は自動観測であり、
未取得は合否を意味しない。人間待ち・実機の観測・合否の欄は空欄のまま残る。
出力先は実行ごとに空のディレクトリを指定する。既存ファイルがある場合は書き込まずに止まる。
スクリプトは Linear に投稿しない。記録と合否判定は人が行う。

```md
## 検証環境
- 検証日 / 検証者:
- OS build (`winver`):
- source: branch / commit:
- 成果物: ☐ MSI (ファイル名 / SHA-256) ☐ 検証 zip (`manifest.json` の commit)
- VM: Hyper-V / セッション種別 = 基本セッション
- 開始 checkpoint 名: ☐ vc_redist 未導入のクリーン（レーン 1） ☐ 「VM 構成」のベースライン（レーン 2）/ 名前:
- バックエンド: ☐ CPU (SimpleConverter) ☐ zenz GGUF (ファイル名)
```

### Part A の記録様式（MSI 配布形態のゲートの Store / UWP 項目へ記録）

```md
## カテゴリ登録
- `GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT` が登録済み: ☐ はい ☐ いいえ
- MSI の commit / SHA-256:

## 同一セッション対照
- Notepad: preedit ☐ 出る ☐ 出ない / DLL ロード ☐ 済 ☐ 未
- Microsoft Store 検索欄: preedit ☐ 出る ☐ 出ない / DLL ロード ☐ 済 ☐ 未
  - 対象プロセス名 / PID / パッケージ名:

## 結論
☐ カテゴリ修正で Store 入力が成立した
☐ 依然として未成立（署名要件が残る。MVP スコープ外のため確定不要。`docs/sideload-packaging-spec.md` §0.1 参照）
☐ その他（観測内容を記載）

## 未成立だった場合の CIG 証跡（v1.0 以降の署名判断用。任意。記録先は署名判断を扱う課題）
- 対象プロセスの署名ポリシー: ☐ MicrosoftSignedOnly ☐ StoreSignedOnly ☐ どちらも未設定 ☐ 未取得
- CodeIntegrity/Operational の 3033 / 3065: ☐ 該当あり ☐ 該当なし ☐ 未取得
- 相関（該当ありの場合のみ）: ☐ 時刻が基準時刻の直後 ☐ プロセスパスが一致 ☐ ファイルパスが `azookey_tsf_tip.dll`
- 判定: ☐ CIG 由来（上記 3 つがすべて揃った場合のみ。署名調達では解決しない）☐ 未確定（欠けた条件を記載）

## preedit が出た場合のみ
- 候補が返るか: ☐ 返る ☐ 返らない（→ pipe DACL の AppContainer capability ACE を扱う課題へ記録）

## 診断中に緩和策を入れていないことの確認
☐ AppContainer ACE 追加なし ☐ peer 検証無効化なし ☐ token 露出なし
```

### 課題ごとの確認項目

課題ごとの確認項目は、課題本文と Linear の tracking Issue が持ち、本書はひな形を持たない。
環境ブロックの後に、課題本文のチェック項目をすべて含めて記録する。
項目を落とすと、実施しても Done 判定の証跡が残らない。

- 同じ実走（C-013、C-010 など）を複数の課題の根拠にする場合も、課題ごとに期待結果と人の観察を分ける。
- 自動テストと compat の結果だけで、人の入力・視覚判断を Pass にしない。アプリ、設定、操作、期待した表示と実際の表示を課題ごとに書く。
- 学習不変を判定する区間は、ほかの打鍵を挟まず、直前・直後の snapshot ラベルと比較結果を添える。
- 一部のアプリや項目しか実施できなかった場合は、未実施の対象と理由を明記する。
- 表示属性の記録は `compat-test/m3_display_attribute_checklist.md` §7 の記録テンプレートを使う。
- API キー、入力本文、クリップボードの私的内容、ユーザー名を含む絶対パスを記録に載せない。

## 中止条件と後始末

`verify-bootstrap.ps1 -Json` が `overallStatus=fail` を返した場合は、打鍵系のゲートへ進まない。
前段の失敗を後段の判定に持ち込むと、どのゲートの結果も信用できなくなる。

Part A で Store 入力が依然として成立しない場合、境界確認は DLL のロード有無と、Part A に挙げた CIG 証跡 2 点までで止める。
署名要件が独立に効いている可能性を、未署名の成果物だけで切り分けることはできない。
それ以上の probe を重ねても、署名済み成果物を用意するまで結論は変わらない。

セッション終了時に次を行う。

- 各ゲートの検証メモを Linear の該当課題へコメントする
- 新規に検出した問題を Linear へ起票する。ラベルは `repo:*` と `area:*` を必須とし、実機確認を要する人間専任タスクには `agent:*` の代わりに `gate:human-required` を付ける
- `AZOOKEY_LOG` と `AZOOKEY_LOG_LEVEL` を削除する
- Process Monitor と WPR の採取プロセスが残っていないことを確認する
- レーン 3 を実施した場合、`appverif.exe -query '*' -for notepad.exe` の出力に設定が残っていないことを確認する
- ベースライン checkpoint へ復元する。`-Restore` による復元と、復元せずに登録を解除する手順は `hyper-v-tip-verification.md`「5. 記録と後始末」が正典である

dump、ETL、PML、ログには変換中の本文と候補が含まれうる。
Linear へ添付する前に内容を確認し、入力本文とローカル絶対パスを残さない。
