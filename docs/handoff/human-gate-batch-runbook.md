# 人間ゲート一括消化の VM セッション runbook

複数の `gate:human-required` 課題を 1 回の Hyper-V VM セッションでまとめて消化するための実行計画と記録様式を定める。
VM 構成、checkpoint 運用、bootstrap、TIP 登録の各手順は既存文書が正典であり、本書はそれらを再掲しない。

- VM 構成と 3 層フローと補助範囲：[`hyper-v-vm-verification-plan.md`](./hyper-v-vm-verification-plan.md)
- 登録、bootstrap、ログ取得、後始末の手順：[`hyper-v-tip-verification.md`](./hyper-v-tip-verification.md)
- 打鍵チェック項目 A1〜A8 と B1〜B7：[`dev32-verification-checklist.md`](./dev32-verification-checklist.md)
- compat runner の実行と出力レイアウト：[`../../compat-test/README.md`](../../compat-test/README.md)
- dump、ETW、Process Monitor の採取：[`windows-diagnostics-playbook.md`](./windows-diagnostics-playbook.md)

進捗と状態の正典は Linear（team `Dev` / project *azooKey Desktop / Windows IME MVP*）である。
本書は手順と記録様式だけを持ち、実走の履歴と個別課題の状態は持たない。
どの人間ゲートが実際に未消化か、どのゲートが何を待っているかは、実行前に Linear で確認する。
過去の実走で何がどこまで採れているかも、本書ではなく該当課題のコメントを読む。

## セッション前にホスト側で用意するもの

VM を起動する前に、2 種類の成果物をホストで作る。
レーン 1 とレーン 2 は前提とする VM 状態が異なるため、片方だけでは両方を走らせられない。

**MSI**：DEV-673 と DEV-767 が対象とする配布形態の成果物。両ゲートは同一の MSI を使う。
互換カテゴリ登録（DEV-766 / PR #271）と設定アプリ同梱（DEV-674 / PR #272）の**両方を含む
main** から MSI を作り、ファイル名と SHA-256 を検証メモへ記録する。
SHA-256 はレーン 1 の `vm-verify-session.ps1 -Prepare` が `msi-record.json` に書く値を使う。
どちらかを欠く MSI で走らせると、修正済みの欠陥を再観測することになる。

**検証 zip**：レーン 2 の全ゲートが使う開発登録用の成果物。

`AZOOKEY_FETCH_LLAMA_CPP` の既定は `OFF` で、`windows-release` preset もこれを ON にしない。
llama.cpp を含まない Host に対して `-ModelPath` を渡すと、`register-dev.ps1` の preflight が `llama_cpp=1` を検出できずに登録を拒否する。
実 GGUF を使う項目は Host の model preflight で止まるため、configure で明示的に ON にする。

```powershell
cmake --preset windows-release -DAZOOKEY_FETCH_GOOGLETEST=ON -DAZOOKEY_FETCH_LLAMA_CPP=ON
cmake --build --preset windows-release
cmake --build --preset windows-release --target compat_test
cmake --build --preset windows-release --target azookey_settings
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
  -IncludeSettings
```

依存ログが別の checkout のヘッダーを指していると、パッケージ生成は拒否する。
compiler cache（sccache）が別 checkout でコンパイルした結果を返した場合に起きる。
拒否メッセージの手順で cache を更新して clean 再ビルドし、生成をやり直す（`docs/debugging.md`）。

GGUF を使うゲートを同じ zip で検証する場合、`-ModelPath` は省略できない。
スクリプトは `-AllowNoModel` を明示しない限りモデル省略を拒否する。
DEV-1046 の local-zenzai 品質確認は実 GGUF での推論結果を見る。
GGUF なしでは `SimpleConverter` の静的辞書しか動かず、その判定は成立しない。

**compat runner の同梱**：`-IncludeCompat` で `compat_test.exe` と `targets/` 以下の全ファイルを
同じ検証 zip に追加する。各ファイルの SHA-256 は `manifest.json` に記録される。
DEV-716 で使うため、本セッションでは既定で無効のこのスイッチを明示する。

VM 側では `compat_test.exe` と `targets\` を同じディレクトリへ展開する。
runner は `--target` に渡したパスから target JSON を読むため、両者の相対関係を崩さない。

**設定アプリの同梱**：`-IncludeSettings` で `azookey_settings.exe` と self-contained ランタイムを
同じ検証 zip の `settings\` に追加し、各ファイルの SHA-256 を `manifest.json` に記録する。
レーン 2 の設定系ゲート（DEV-1160、DEV-761）は、展開したパッケージの
`settings\azookey_settings.exe` から設定を保存する。別の zip を手作業で作って持ち込まない。
同梱物の範囲は MSI と同じである。

持ち込むもののうち、パッケージ生成が拾わないものを別途 VM へ入れる。

- Sysinternals Suite（`procmon`、`handle`）：Store 入力が再検証で失敗した場合の境界確認に使う
- ゲスト側の 150% DPI 設定：DEV-716 の C-006 が要求する。ゲスト OS のスケーリング設定なので基本セッションのままで足りる
- 絵文字を含むユーザー辞書エントリ、または絵文字を返す辞書：DEV-716 の C-007 を TIP 経路で確認するため。取り込み用の TSV は下の合成 fixture にある
- DEV-153 の対象アプリ（Chrome、VS Code、Windows ターミナル、Office 365 Word）：未導入のアプリは測れないため、事前に入れるか対象外として記録する。VS Code は DEV-847 の対象でもある
- 第 2 のローカルユーザーアカウント：DEV-676 の項目 3（別ユーザー provisioning）が要求する
- デバッガ（WinDbg など）：DEV-905 で Application Verifier が停止したとき、接続していないと停止コードと stack が残らない
- ETW / WPR の資産：DEV-1092 と DEV-677 が要求する。検証 zip には含まれないため、`diagnostics/azookey-diagnostics.wprp`、`diagnostics/etw/AzooKey.man`、`azookey_etw_manifest` ターゲットで生成した `azookey_etw_manifest.dll` を同じ commit から持ち込む。登録と解除は `docs/sideload-packaging-spec.md` §7.4 の `wevtutil im` / `wevtutil um` に従い、パスはゲストへ置いた場所に読み替える
- 429 を返すループバックの stub：DEV-1411 の `RateLimit` と再試行待機を、外部 API を使わずに誘発するため（レーン 2 の手順 2）。stub は下の合成 fixture にある
- 合成 fixture（`scripts/vm-verify-fixtures/`）：検証 zip には含まれないため、検証 zip と同じ commit の checkout からディレクトリごとゲストへコピーする。収録物、コピー後の照合、ゲートごとのコマンドは [`scripts/vm-verify-fixtures/README.md`](../../scripts/vm-verify-fixtures/README.md) が正典であり、本書はコマンドを再掲しない

合成 fixture を使うゲートと、README の対応する節は次のとおり。

| ゲート | 使うもの | README の節 |
|---|---|---|
| DEV-716 の C-007 | 絵文字を表層に持つユーザー辞書の TSV | 「絵文字のユーザー辞書」 |
| DEV-1411 | 401、429 + `Retry-After`、遅延応答を返すループバック stub | 「AI 整文のループバック stub」 |
| DEV-1360 | カスタムローマ字 TSV（正常、差し替え用、空、不正行のみ） | 「カスタムローマ字 TSV」 |
| DEV-1446 | DPAPI 暗号化より前の形式の平文ストア | 「平文ストア」 |
| DEV-963 / DEV-1144 | 非 ASCII のディレクトリ名とファイル名を生成するスクリプト | 「非 ASCII パス」 |
| DEV-759 | `--stdio` の Host へ渡す Handshake と CommitObservation の行 | 「stdio の行」 |
| 検証メモの下書き | ゲート ID と課題の対応表 `gate-map.json` | 「gate map」 |

マルチディスプレイ構成は用意しない。
IME の打鍵検証は基本セッション必須であり（`hyper-v-tip-verification.md` の安全装置）、基本セッションは単一ディスプレイしか持てない。
C-005 はこの制約のため DEV-782 が扱い、本セッションの対象外である。

## Part A：Store 入力の再検証

**パッケージ化された UWP / Microsoft Store アプリ**での入力は **MVP の対象外**である
（`docs/sideload-packaging-spec.md` §0.1）。MVP が入力先として保証するのは Win32 デスクトップ
アプリに限る。除外の境界は AppContainer を使うかどうかではなく、パッケージ化された
UWP / Store アプリかどうかで引くため、**Edge は対象内**である。
署名は入力対象と独立した v1.0 のリリースゲートとして残り、MVP の未署名 MSI は評価配布に
限定する。したがって **DEV-673 の Store / UWP 入力項目は「未達」ではなく「スコープ外」**
として扱う。

本 Part は必須ゲートではない。実施するのは、互換カテゴリ登録の効果を確認したい場合と、
v1.0 以降の署名判断に使う証跡（記録先は DEV-783）を採りたい場合に限る。
時間が足りなければ Part B を優先してよい。

着手する前に DEV-765 と DEV-673 のコメントを読み、どこまでの境界が既に採れていて何が
未確認で残っているかを確かめる。同じ観測を採り直しても判定は動かない。

Store 入力が成立しない場合のロード前ゲートは 2 つあり、どちらも独立に効く。

- 互換カテゴリ `GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT` の登録（DEV-766 / PR #271）。
- TIP DLL の署名。Microsoft の IME 要件は第三者 IME への署名を求めている（[Input Method Editors (IME)](https://learn.microsoft.com/windows/apps/develop/input/input-method-editors#requirements-for-imes)）。

### 手順

セッション前に用意した MSI をクリーン VM へ入れ、同一ログオンセッションで Notepad と Microsoft Store の検索欄を対照する。
レーン 1 と同じ MSI を使う。互換カテゴリ登録（PR #271）はこれに含まれる。
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
過去の実走がどちらへ入り、何が未確認で残っているかは DEV-673 のコメントで確認する。

**Store で preedit が出る場合**：カテゴリ欠落が原因だったことが実機で裏付けられる。
DEV-673 の「Microsoft Store / UWP 入力」項目を、スコープ外ではなく Pass として記録する。
Store の検索候補フライアウトが前面に出ると azooKey の候補 UI が隠れるため、フライアウトを Esc で閉じてから Space を押し、候補 UI の描画を確認する。
DEV-555（pipe DACL の AppContainer capability ACE）は、preedit が出たうえで候補が来ないかどうかという別の問いなので、そこまで確認して結果を DEV-555 へ書く。

**Store で preedit が出ない場合**：署名要件が独立に効いている可能性が残る。
これは診断で解けない種類の残件で、署名済み成果物を用意するまで確定できない。
DEV-673 の当該項目は **スコープ外**（§0.1 / DEV-783）として記録し、次へ進む。
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
いずれの結果も **DEV-783** へ記録する。未確定としたときは、どの条件が欠けたかを併記する。
Store / UWP をスコープ外とする判断そのものを DEV-783 が扱い、v1.0 以降に署名を検討する際の材料もそこへ集める。

境界をもう一段だけ詰めるなら、DLL がロードされたかどうかまでを確認して止める。

```powershell
tasklist /m azookey_tsf_tip.dll
```

Store のホストプロセスが一覧に現れず Notepad が現れるなら、失敗はロード前で起きている。
ACL の観測だけで結論を出さない。
ACL が正常でありながら DLL が未ロードだった観測が DEV-765 にあるため、ACL の状態は原因候補にはなっても判定の根拠にはならない。

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

### レーン 1：クリーン VM に MSI を入れた状態

MSI 配布形態そのものを対象とするゲートを置く。
開発登録（`register-dev.ps1`）を先に走らせると、開発登録が付ける AppContainer ACL が MSI 側の継承 ACL と混ざり、ACL 由来かどうかの判別ができなくなる。
このレーンでは開発登録を一切行わない。

開始状態は **vc_redist 未導入のクリーン checkpoint** とする。
`hyper-v-vm-verification-plan.md` §2 のベースライン checkpoint は「クリーン + Redistributable + 設定」で vc_redist を導入済みであり、これとは別物である。
DEV-673 の第 1 項目は VC++ Redistributable 未導入の環境で MSI がインストールできること（CRT の app-local 同梱が効いていること）を見るものなので、ベースライン checkpoint から始めると前提が崩れたまま Pass になり、証跡の意味が失われる。
どちらの checkpoint を使ったかは、検証メモの環境ブロックの checkpoint 名で残す。

MSI の転送とログの回収は、検証 zip と同じ `vm-verify-session.ps1` に MSI を渡して行う。
`-Prepare` は、VM の現在の状態がクリーン checkpoint から派生していることを確かめる。
そのうえで MSI をゲストの `C:\azookey-verify\msi-<12桁>\` へ転送し、MSI の SHA-256 から
決まる checkpoint（`pre-azookey-msi-<12桁>`）を取って、ホストの結果ディレクトリへ
`msi-record.json` を書く。この checkpoint は MSI を置いただけで導入していない状態なので、
復元すればそのまま再導入でき、`-Prepare` をやり直す必要はない。
`msiexec` は実行しない。導入は人が行う（plan §4.5）。
`-Prepare`、`-Collect`、`-Restore` は、plan §4.5 の条件のもとでエージェントに任せられる。

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

1. DEV-673 のチェックリストを頭から実施する。
2. Microsoft Store / UWP 入力の項目は **スコープ外**として記録し、素通りする（§0.1 / DEV-783）。Part A を実施した場合のみ、その結果を併記する。
3. 続けて **DEV-767** のチェックリスト（設定アプリの起動・二重起動・アンインストール残留物）を実施する。同じクリーン VM 状態を使うため、DEV-673 のアンインストール確認と順序を合わせる。
4. **DEV-1385** は WiX 7 でビルドした MSI（DEV-1383 の成果物）と WiX 5 でビルドした MSI の 2 つを要する。クリーン checkpoint から WiX 7 版の新規インストールとアンインストールを確認し、クリーン checkpoint へ戻してから WiX 5 版を入れ、WiX 7 版へのアップグレードで旧版が残らないことを確認する。`pkg/msi/Package.wxs` の `MajorUpgrade` は同じ版を上位更新として扱わないため、WiX 7 版の ProductVersion が WiX 5 版より高いことを開始条件とする。2 つの MSI のファイル名、ProductVersion、SHA-256 を検証メモに記録する。

MSI を入れた状態でしか確認できない項目を持つ課題が、ほかに 2 つある。
レーン 2 以降では状態を作れないので、実施する場合はこのレーンで行い、実施しない場合は未実施と理由を各課題に記録する。

- DEV-676：ログオン時のコンソールウィンドウの表示と、ConstrainedLanguage または PowerShell 5.1 を無効にした環境での launcher の診断。どちらも MSI が登録するログオン経路（`start-installed-host.ps1`。`docs/sideload-packaging-spec.md`「MSI のログオン常駐」）のもので、開発登録は launcher を経由しない。後者は VM の状態を変えるので、checkpoint を取ってから行う。
- DEV-1092 の項目 4：clean VM での IME 動作と、アンインストール後の診断ファイルの残留。DEV-673 と DEV-767 のアンインストール確認と同じ機会に見る。

Store 入力の可否は DEV-673 の合否を左右しない。
MVP が入力先として保証するのは Win32 デスクトップアプリであり、Store / UWP は v1.0 以降へ送ることが確定しているためである。
除外の境界は AppContainer を使うかどうかではなく、パッケージ化された UWP / Store アプリかどうかで引く（§0.1）。
Edge は renderer のサンドボックス化に AppContainer を使うが、入力欄をホストするのは Win32 プロセスであり対象内である。通常どおり Pass / Fail で判定する。

DEV-673 の課題本文は設定アプリ同梱（PR #272）より前に書かれている。
`%ProgramFiles%\azooKey` の配置確認では、課題本文が挙げる TIP、Inference Host、MSVC runtime 3 DLL、ライセンスファイルに加えて、`azookey_settings.exe` と self-contained ランタイム、スタートメニューのショートカットが増えている。
DEV-673 では配置物として存在することの確認にとどめ、差分があった事実を検証メモへ書く。
設定アプリ側の起動とアンインストールは DEV-767 が扱う。
DEV-767 を検証済みとみなして飛ばさず、上記の手順 3 として同一レーンで実走する。
実走が要るかどうかは、実行前に DEV-767 の状態とコメントで確認する。

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
効いていなければ、Installer は `/qn` のまま再起動を開始して `1641` を返す（DEV-1139）。
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
レーン 1 が終わったら、レーン 2 の開始状態（plan §2 のベースライン checkpoint）へ復元する。
MSI の machine-wide 登録を残したままレーン 2 の開発登録を重ねると、どちらの登録が効いているか判別できなくなる。

### レーン 2：開発登録と検証 zip を入れた状態

`verify-bootstrap.ps1` で導入した状態で走らせるゲートを置く。
次の順に、可逆な設定と学習データ、通常の打鍵、VM 状態を変える操作を分ける。
前提実装と個別の合格条件は各課題と対応する spec で確認する。

DEV-1446（M34 DPAPI）の平文から暗号化への移行は、手順 1 より前に行う。
Host は最初の起動で平文ストアを暗号化するため（`docs/sideload-packaging-spec.md` §9.4・§9.5）、移行はその 1 回でしか観測できない。
手順 1 の `-Run` と `verify-bootstrap.ps1` は Host を起動するので、先に走らせると移行前の状態を作れない。
`-Prepare` の後、Host を一度も起動していない状態で合成の平文ストアを配置し、checkpoint を取って移行前を記録する。
最初の Host 起動は手順 1 の `-Run` に任せ、`-Run` の後に移行後を記録して比較する。
`-Run` は compat の打鍵も行うので、移行後の記録には compat による学習の書き込みも入る。移行の根拠は `Changed: yes` ではなく、平文の `removed`、`.enc` と `.bak` の `added`、`.bak` の SHA-256 が fixture と一致すること、README の件数で見る。
`-Prepare` は zip を転送するだけで展開しない。移行前の記録に使う `learning-data-snapshot.ps1` は、検証 zip から `-Run` の展開先（`C:\azookey-verify\<zip の basename>`）とは別のディレクトリへ取り出して使う。
`-Run` は展開先を自分で作り直すので、同じ場所へ手で展開して使用中のままにすると `-Run` が止まる。手で取り出した側から `register-dev.ps1` と `verify-bootstrap.ps1` を実行しない。
配置先、移行前後の記録、件数の確認は [`scripts/vm-verify-fixtures/README.md`](../../scripts/vm-verify-fixtures/README.md)「平文ストア」に従う。
以降の手順は暗号化済みの状態のまま進めてよい。
DEV-1446 の残りの項目は、設定アプリを操作する手順 2（API キーの `dpapi:` 保存と D-014）と、別ユーザーを使う手順 6（他ユーザーでの復号失敗）で確認する。

1. **先行自動判定**：`-Run -CompatSkip C-006,C-013,C-010` で Host 停止・DPI 変更を伴わない compat ケースを先に採る。`verify-bootstrap.ps1` が失敗したら、打鍵ゲートへ進まない。runner の結果は人の TIP 操作・目視判定を代替しない。
2. **設定反映と学習不変**：DEV-1160 を設定系ゲートの最初に置き、既存 TIP 接続への設定反映と新規アプリとの差を確認する。DEV-1160 が変える `batchRomajiConversion` / `batchConversionMode` / `batchAutoPunctuation` は設定アプリに UI が無い（`docs/sideload-packaging-spec.md` の設定アプリの UI 化の表で M58）ので、`%LOCALAPPDATA%\azooKey\config\settings.json` を直接書き換えて確かめる。`batchConversionMode` は `batchRomajiConversion=true` のときだけ効く。その後、DEV-1188（M46 secure 抑止・インジケータ）、DEV-1346（M14 の secure 抑止項目）、DEV-1046（M58-B/C の AI 整文で学習しない項目）を行う。各項目の直前・直後に学習データを記録して比較し、secure から通常入力へ戻る確認も記録する。DEV-1046 の実 API を使う場合は承認済みの接続先だけを使い、キーと入力本文を証跡へ載せない。DEV-1046 の後に DEV-1411（AI 整文のエラー分類通知）を行う。外部 API を使うかは人が決める。使わない場合は、分類ごとに次の手段で誘発する（分類の定義は `docs/ai-backend-spec.md` §7.2）。stub を使うときは `aiBackend=openai` とし、`openAiApiKey` に機密でない非空のダミーキー（`dpapi:` で始まらない平文）を設定する。キーが空だと HTTP 接続の前に `Auth` になり、ほかの分類を観測できない。`RateLimit` と再試行待機は HTTP 429 でしか起きないため、`openAiApiEndpoint` をゲスト内のループバック stub（`http://127.0.0.1:<port>/v1`）へ向け、429 と `Retry-After` を返させる。平文 HTTP が許されるのは `127.0.0.1` と `[::1]` の直書きだけで、`localhost` は拒否される。同じ stub に 401 を返させると `Auth`、応答を設定したタイムアウトより遅らせると `Timeout` になる。待ち受けていないポートを指すと `Network`、`dpapi:` キーを復号できない状態にすると `KeyReentry` になる。続けて DEV-1360（M17 カスタムローマ字）で `inputStyle=custom` と `customRomajiTablePath` の TSV を指定し、保存し直したときの反映と空・不正 TSV での内蔵表への復帰を確認する。確認後は `inputStyle` を元に戻してから次へ進む。
3. **通常の入力とアプリ巡回**：Notepad → VS Code → Edge を 1 巡し、下の統合チェックリストを使う。DEV-1266（M13、M3〜M10 回帰）を基準に DEV-1346（M14）、DEV-1350（M15）、DEV-760〜762（M61-A/B）を確認する。M14 と M15 は M13 の結果を前提にそれぞれ判定する。DEV-761 の per-app 設定切替は DEV-1160 の確認後に行う。DEV-153 と DEV-365 が要求する残りのアプリも、それぞれの課題で確認する。Notepad では DEV-1408（M20 再変換）の選択からの変換キー、選択直後の右クリック、選択や位置を変えた後に古い候補が出ないことも確認する。Office を導入した VM では同じ確認を Office でも行い、未導入なら未実施として記録する。
4. **中間 checkpoint と DPI**：巡回の証跡を回収して checkpoint を取る。ゲストの表示スケールを 150% に変えて再サインインし、DEV-716 の C-006 と DEV-365 の D-08（Notepad / VS Code / Edge）を確認する。D-08 は 200% へ変更して再サインインした状態でも 3 アプリを確認する。各スケールで OS の設定値、操作結果、runner の `report.json`、目視記録を復元前にホストへ回収する。最後に設定を戻すか checkpoint へ復元し、登録と Host の Ready を確認してから障害注入へ進む。
5. **無応答と Host kill**：TIP の info ログ、ETW、Host ログの採取を先に開始する。C-013 の一時停止と復帰を先に行い、DEV-1263（M42）の無応答・ローカル fallback・Ready 復帰を人が確認する。C-013 の実アプリ結果（Notepad / VS Code / Edge の `report.json` と TIP JSONL）は DEV-1395 に記録する。次に C-010 の Host kill を 1 回行い、DEV-716、DEV-1263、DEV-676 の項目 2 へ同じ実走を参照する。Host 不在時の DEV-760（M61-A）の基本ペア動作も確認する。C-013 と C-010 の実行中は、DEV-1398（M47）の `degraded_simple` の表示と消去も観察する。各課題の期待値と判定は別々に記録し、Host が Ready に戻ったことを確かめる。DEV-1398 の `degraded_model` は、モデルを指定せずに Host を起動し、存在しないパスか破損した GGUF をロードさせて誘発する。ロード済みのモデルがあるまま差し替えに失敗しても劣化として扱われない（`docs/dev-infrastructure-spec.md` §8.5.1）。復帰は正しいパスでの再ロードで確認し、検証 zip のモデル指定へ戻してから次へ進む。SafeMode は Host の連続クラッシュ（`docs/dev-infrastructure-spec.md` §8.5.3）で誘発する。SafeMode は手動解除まで残るため、誘発前に checkpoint を取り、確認後に復元してから次へ進む。
6. **ログオンとユーザー変更**：DEV-676 のログオン自動起動、別ユーザー provisioning、監督停止の各項目を行う。ログオフや別ユーザーへの切替は元の対話セッションを変えるので、通常の打鍵と障害注入の後に置く。監視失敗や連続失敗による supervisor の停止と、再ログオンまたは手動の再起動での復帰（`docs/sideload-packaging-spec.md`「MSI のログオン常駐」が定める停止条件。開発登録も同じ `host-supervisor.ps1` を使う）は VM の状態を変えるので、ほかの項目を終えて checkpoint を取ってから行う。DEV-676 の確認のうち、ログオン時のコンソールウィンドウの表示と launcher の診断は MSI のログオン経路のものであり、このレーンでは観測できない（レーン 1）。

| 訪問順 | 同じ訪問で確認する項目 | 記録先 |
|---|---|---|
| Notepad | M13 のキー回帰、M14 の live preedit、M15 の予測窓、M20 の再変換、M61 の基本・拡張挙動、G1〜G3、`pbShow`、DisplayAttribute | DEV-1266 / 1346 / 1350 / 1408 / 760〜762 / 847 / 153 / 365 |
| VS Code | M61 の per-app 有効範囲、G1〜G3、`pbShow`、DisplayAttribute | DEV-761 / 847 / 153 / 365 |
| Edge | M13 の UI-less 入力、G1〜G3、`pbShow`、DisplayAttribute | DEV-1266 / 847 / 153 / 365 |

各アプリで DEV-847 の G1〜G3 と DEV-153 の計測を続けてよい。
DEV-365 の対象アプリと省略できる項目は `compat-test/m3_display_attribute_checklist.md` §3・§3.1 に従う。
Notepad / Edge / VS Code でも描画属性 D-02 / D-03 / D-07 / D-08 / D-10 の目視は省略しない。
単一ディスプレイの基本セッションでは D-09 のモニタ跨ぎを実施できない。全画面での観察と区別して DEV-365 に D-09 未実施・環境制約を記録し、異なる DPI の複数モニタを使う別セッションで検証する。`compat-test/m3_display_attribute_checklist.md` §5 の完了条件は緩めず、D-09 の実走前に DEV-365 を完了扱いにしない。
巡回を統合しても検証メモは DEV-847、DEV-153、DEV-365 と新たに実走した各課題へ分けて残す。
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

比較結果の `Changed: yes` は、学習データのストア（`learning.tsv` などとその `.enc`、`.bak`）にハッシュの変化、ファイルの追加または削除があったことを示す。
各行の `size` と `lines` は直前の記録からの差分であり、ファイルのサイズではない。`size 0` はファイルが空であることを示さない。
更新時刻だけの変化は `touched` と表示し、変化に数えない。
Host が起動と終了で書き換える `host_run_state.txt` などは `not learning data` と付けて表示し、変化に数えない。
`Unverified` が出た場合は、ストアを読めなかったので、ラベルを変えて記録し直す。
`Warning: no learning store` が出た場合の `Changed: no` は証跡にならない。データディレクトリと、IME を使うユーザーのシェルで実行したかを確かめる。
Host が平文のストアを読み込むと `.enc` と `.bak` への移行が起き、追加と削除が出るので、基準のラベルは Host 起動後に取る。
記録するのはハッシュ、行数、サイズ、更新時刻だけで、学習データの本文は出力に含まれない。
JSON は他のゲスト出力と一緒にホストへ回収する。

DEV-676 の項目 3（別ユーザー provisioning）は、VM に第 2 のローカルユーザーが要る。
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
`-Run -CompatCases C-013` / `C-010` は全 target に障害を注入するため、1 回の C-010 を DEV-716 / DEV-1263 / DEV-676 で共有する場合は上の Notepad target だけを実行する。
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
追加された C-014 / C-016 はライブ変換 OFF、C-018 は ON の report で判定する。設定を切り替える場合は `dev32-verification-checklist.md` の compat 分担に従い、別の出力先で実行して設定と `case_selection` を記録する。反対の設定での `failing-skip` を Pass に読み替えない。C-015 / C-017 / C-018 / C-019 の自動判定は M13〜M15 の人の表示・操作判断を代替しない。
ゲスト内で直接実行した C-006 の出力は `-Run` と違って自動回収されない。150% と 200% の実走ごとに異なる出力ディレクトリを指定し、PowerShell Direct の `Copy-Item -FromSession` で `report.json` を含むディレクトリをホストの別々の回収先へコピーする。DEV-365 のスケール別目視記録・画像もホストへ保存し、ファイルの存在と対象スケールを確認してから checkpoint を復元する。回収方法の前提と資格情報の扱いは `hyper-v-tip-verification.md` の手順 3 に従う。

終了コードは、全件 pass が `0`、fail を含む場合が `1`、fail は無いが failing-skip を含む場合が `2` である。

C-007（サロゲートペアの確定）は、azooKey の候補から絵文字を確定する経路を要求する。
絵文字リライター（DEV-403）がその前提機能だが、ユーザー辞書の writer 経路（DEV-181 / DEV-790）
を使えば、リライターを待たずに同じ確定経路を作れる。
`UserDictionary` の登録内容は `SimpleConverter` の候補生成へ渡っており、surface に文字種の制限は無い。

```powershell
$exe = (Resolve-Path .\azookey_inference_host.exe).Path
& $exe userdict add --reading えもじ --surface 😀
```

登録したエントリを打鍵して候補から確定し、アプリ側の文字列でサロゲートペアが壊れないことを見る。
ZWJ でつないだ列を含む複数のエントリをまとめて取り込む場合は、`scripts/vm-verify-fixtures/README.md`「絵文字のユーザー辞書」の TSV と手順を使う。
候補に出ないか確定で壊れる場合は、DEV-403 を要する未達として DEV-716 へ記録する。

C-005（マルチディスプレイ端の候補クランプ）は本セッションの対象外で、DEV-782 が扱う。
基本セッションでは構成を作れないため `failing-skip` として残るが、runner は環境条件を満たせないケースを silent skip せず記録するので、誤って Pass にはならない。
したがって C-005 に起因する終了コード `2` は失敗ではなく、DEV-716 の判定には含めない。

### レーン 3：昇格と登録状態を変える検証

DEV-1211（昇格した登録・解除とロールバック）、DEV-1092（ETW とクラッシュ診断の実機設定・採取）、DEV-677（WPR profile の実採取と WPA での読込み）、DEV-905（Application Verifier）を置く。
DEV-677 は DEV-1092 と同じ管理者 PowerShell で、持ち込んだ ETW / WPR の資産を使って `docs/sideload-packaging-spec.md` §7.4 の手順で採取する。WPA で開けるだけでは合格にしない。Generic Events で provider `azooKey-Desktop` に絞り、同じ `client_id` / `request_id` の 3000 → 3003（FrameWrite）→ 4000 → 4002 → 4001 → 3003（FrameRead）→ 3001 が現れるかを人が判定する。ETL は Git へ入れず、採取後は `wevtutil um` で manifest の登録を解除する。
`AzooKeyDiagnostics` プロファイルの ETL は IPC のフレームと Health を含むため、十数分で GB 単位に育つ。
採取は確認する操作ごとに短く区切る。WPA を使わずに個々のイベントを読むときは、`tracerpt` で XML 化するとメモリが足りなくなるので、`Get-WinEvent -Path <ETL> -Oldest -FilterXPath` で provider `azooKey-Desktop` と必要なイベント ID に絞る。
管理者権限を使い、登録・診断設定や対象プロセスの状態を変えるので、レーン 2 の観察と証跡回収を終えた後に走らせる。
各課題が要求する権限、専用成果物、解除条件を課題本文と対応する診断手順で確認する。
DEV-1211 の失敗注入に Debug ビルドが必要なら、通常の検証 zip と混ぜず別パッケージとして用意し、保護 checkpoint から実施する。
登録失敗後の HKLM CLSID / TSF profile / category の残骸、再登録、解除を確認してから次へ進む。
DEV-1211 の解除後、DEV-1092 と DEV-905 のために開発登録済みの保護 checkpoint へ復元する。
DEV-1092 のトレースと dump は入力本文や秘密情報の混入を確認し、収集設定を元へ戻す。
DEV-1092 の項目 4（clean VM での IME 動作と、アンインストール後の診断ファイルの残留）は、開発登録のこのレーンでは確認できない。
レーン 1 で確認していなければ、未実施として理由を DEV-1092 に記録する。

DEV-905 はこのレーンの最後に走らせる。
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
検出があった場合は停止コードと stack を DEV-905 のコメントへ記録し、file:line と再現手順を添えて別課題を起こす。

### Windows CLI / コンソールの別枠

TIP 登録を必要としない DEV-963（非 ASCII argv）、DEV-1144（非 ASCII パスの GGUF 実ロード）、DEV-758（user_dict の 2 writer）、DEV-759（コンソール終了時 flush）は Windows CLI / コンソールの別枠で行う。
同じ VM を使う場合も対話 TIP レーンの進行条件にせず、各課題の前提と合格条件を確認して個別に記録する。
DEV-758 は稼働中 Host と offline CLI の同時実行を要するが、TIP の打鍵は要しない。
DEV-759 はコンソールを終了させるため、レーン 2 の Host と混同しない。

DEV-963、DEV-1144、DEV-759 のコマンドと fixture は [`scripts/vm-verify-fixtures/README.md`](../../scripts/vm-verify-fixtures/README.md) が持つ。
本節は、README に無い実行条件だけを定める。

| ゲート | README の節 | 実行条件 |
|---|---|---|
| DEV-963 | 「非 ASCII パス」 | VMConnect の基本セッションの対話コンソールから実行する。PowerShell Direct 経由の実行は、課題が求める通常コンソールの条件を満たさない |
| DEV-1144 | 「非 ASCII パス」 | 生成した `modelPath` を `--model` と `model.selectedPath` の両方で確認する。レーン 2 の Host と pipe を取り合うので、レーン 2 と 3 の証跡を回収した後に、supervisor ごと止めてから行う |
| DEV-759 | 「stdio の行」 | 終了経路ごとに新しいデータルートで `--stdio` の Host を起動し、最初に Handshake の行を送る |

DEV-963 と DEV-1144 は active code page で結果の意味が変わる。
課題が求める code page と異なる環境で実行した場合は、実行した code page での結果として記録し、求められた code page を未実施と明記する。
code page の値は fixture の生成スクリプトが `paths.json` に書く。

DEV-1144 では Host だけを止めても足りない。
モデルを指定して登録した開発登録の supervisor は、止まった Host をそのモデルを指す `--model` 付きで起動し直し、明示した `--model` は `model.selectedPath` より優先される。
`model.selectedPath` 側の確認は、supervisor を止めたうえで `--model` を付けずに pipe で起動した Host に対して行う。
`azookey_diag.exe --json` の D-008 は、pipe で応答している Host のロード済みパスを `model.selectedPath` と比べるので、この状態で採った D-007 と D-008 を記録する。
確認の後は checkpoint へ復元するか、登録をやり直して supervisor を戻す。

DEV-759 の stdio 手順は、Handshake を送ってから CommitObservation を送る。
Host が学習を受け付けるのは、Handshake で `secure_flag` を申告した接続だけである。
Handshake を省くと CommitObservation は `"ok":false` になり、学習データが作られないので、終了経路の保持と消失を判定できない。
学習データは暗号化されているため、保持の確認はファイルの行数ではなく README の `lookup` で行う。
貼り付けた行が Host に届かなかった場合は、その事実を検証メモに書く。

#### DEV-758 の 2 writer 実走

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
`settings.json` は対象外である。Host はこれを読むだけであり、設定アプリ側の保存経路は DEV-794 が扱う（`docs/windows-tsf-host-architecture.md`「共有ユーザーデータの writer 責務」）。

### 別環境・前提が必要なゲート

DEV-1248 は TIP 側の検出 2 トリガ、DEV-267 は Store 用 MSIX、DEV-909 は物理 ARM64 機、DEV-782 はマルチディスプレイ構成、DEV-265 は量子別 RSS 測定の前提をそれぞれの課題で確認してから別計画に入れる。
DEV-194 はホスト側のベンチと変換検証なので、VM の対話セッションと分ける。

## Part C：検証メモのひな形

Linear への記録様式を揃えておく。
各ゲートの完了条件は「検証メモを当該課題へコメントする」ことであり、記録が揃わないと Done へ遷移できない。

全ゲート共通で先頭に置く環境ブロック。
環境記入欄は本節が正典であり、`dev32-verification-checklist.md`「検証環境（記入）」から参照される。

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
bootstrap は未登録なら TIP を登録し、pipe が無ければ supervisor を起動するので、DEV-1211 の解除の後や DEV-676 のログオン確認の前に実行すると、ゲートが見たい状態を書き換える。
レーン 2 の手順 4 と 5 で compat を分けて回す `-Run` は、この制限に当たらない。
操作の後の状態を根拠にする場合は、読み取りだけを行う `azookey_diag.exe --json` を採り直してサマリを作り直す。

生成された `verification-summary.md` を検証メモの先頭に貼り、空欄を人が埋める。
サマリは観測値の集約であり、ゲートの合否は各課題の判定基準で人が決める。
入力の schema と分類は `docs/dev-infrastructure-spec.md` §2.6 を参照する。

課題ごとの転記下書きが必要な場合は、ゲート ID と課題の対応表を渡して次を実行する。
対応表は repo の `scripts/vm-verify-fixtures/gate-map.json` を使う。
bootstrap、diag、compat の ID を載せており、各対応の根拠と、対応を置かない ID は `scripts/vm-verify-fixtures/README.md`「gate map」にある。
対応表の各行は `source`（`bootstrap` / `diag` / `compat`）、`id`、`issueId` と、必要なら compat の `targetId` を指定する。
同じゲートを複数課題へ記録する場合は行を分ける（例: C-010 は DEV-716、DEV-1263、DEV-676）。
対応表は実施するゲートに合わせて確認し、下書きの出力後に Part C の課題別確認項目を人が埋める。
対象の課題を入れ替えるときは、`gate-map.json` と README の表を同じ変更で直す。

書式は次のとおり。

```json
{
  "schemaVersion": 1,
  "gates": [
    { "source": "bootstrap", "id": "inferenceHost", "issueId": "DEV-1263" },
    { "source": "diag", "id": "D-004", "issueId": "DEV-676" },
    { "source": "compat", "id": "C-010", "issueId": "DEV-716" },
    { "source": "compat", "id": "C-010", "issueId": "DEV-1263" },
    { "source": "compat", "id": "C-010", "issueId": "DEV-676" }
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
- 開始 checkpoint 名: ☐ vc_redist 未導入のクリーン（レーン 1） ☐ plan §2 のベースライン（レーン 2）/ 名前:
- バックエンド: ☐ CPU (SimpleConverter) ☐ zenz GGUF (ファイル名)
```

### Store 入力の再検証（DEV-673 の Store / UWP 項目へ記録）

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
☐ 依然として未成立（署名要件が残る。MVP スコープ外のため確定不要。§0.1 / DEV-783 参照）
☐ その他（観測内容を記載）

## 未成立だった場合の CIG 証跡（v1.0 以降の署名判断用。任意。記録先は DEV-783）
- 対象プロセスの署名ポリシー: ☐ MicrosoftSignedOnly ☐ StoreSignedOnly ☐ どちらも未設定 ☐ 未取得
- CodeIntegrity/Operational の 3033 / 3065: ☐ 該当あり ☐ 該当なし ☐ 未取得
- 相関（該当ありの場合のみ）: ☐ 時刻が基準時刻の直後 ☐ プロセスパスが一致 ☐ ファイルパスが `azookey_tsf_tip.dll`
- 判定: ☐ CIG 由来（上記 3 つがすべて揃った場合のみ。署名調達では解決しない）☐ 未確定（欠けた条件を記載）

## preedit が出た場合のみ
- 候補が返るか: ☐ 返る ☐ 返らない（→ DEV-555 へ記録）

## 診断中に緩和策を入れていないことの確認
☐ AppContainer ACE 追加なし ☐ peer 検証無効化なし ☐ token 露出なし
```

### DEV-673

課題本文のチェック項目をすべて含める。
項目を落とすと、実施しても Done 判定の証跡が残らない。

```md
## 検証手順ごとの結果
- 未署名であること、SmartScreen / UAC の「不明な発行元」表示: ☐ 確認 ☐ 未確認
- MSI インストール（クリーン Win11、VC++ Redist 未導入）: ☐ Pass ☐ Fail
- `%ProgramFiles%\azooKey` の配置物: ☐ TIP ☐ Inference Host ☐ MSVC runtime 3 DLL ☐ ライセンス
  - 課題本文以後に増えた配置物: ☐ azookey_settings.exe ☐ self-contained ランタイム ☐ スタートメニュー ショートカット
- 言語・入力設定に azooKey の IME プロファイルが出現: ☐ Pass ☐ Fail
- メモ帳で打鍵 → 変換 → 確定: ☐ Pass ☐ Fail
- `azookey_tsf_tip.dll` が ALL APPLICATION PACKAGES (S-1-15-2-1) の RX を継承: ☐ Pass ☐ Fail
- Edge（Win32 プロセスが入力欄をホストするため対象内）で打鍵 → 変換 → 確定: ☐ Pass ☐ Fail
- Microsoft Store / UWP で打鍵 → 変換 → 確定: ☐ Pass ☐ **スコープ外**（既定。§0.1 / DEV-783）
  - MVP の受け入れ条件ではない。未達として記録しない
  - 再検証を実施した場合の結果は上のブロックを参照
- サインアウト / 再起動後もプロファイルと入力が成立: ☐ Pass ☐ Fail ☐ 未実施
- アンインストールで COM / TSF 登録と `%ProgramFiles%\azooKey` の配置物が残らない: ☐ Pass ☐ Fail
  - 確認: HKLM CLSID / CTF\TIP（native / WOW6432Node）/ インストール先ディレクトリ

## 記録
- MSI の取得元 / バージョン / SHA-256:
- `msiexec /L*V` ログの保存先:
- Windows build / 使用アプリ / 再起動の有無:
```

### DEV-767

DEV-673 と同じ MSI・同じクリーン VM で実施する。
課題本文は PR #272 の成果物を対象と書いているが、レーン 1 で作る MSI が PR #272 を含む。
別の MSI を用意しない。

アンインストール確認は DEV-673 と対象が重なるが、確認対象が違う。
DEV-673 は TIP と COM 登録、本ゲートは設定 EXE・WinUI ランタイム payload・ショートカットである。
1 回のアンインストールで両方を見て、それぞれの記録へ書く。

```md
## 検証手順ごとの結果
- 対象の記録（バージョン / commit SHA / MSI SHA-256 / Windows build）: ☐ 記録した
- 開発ランタイム未導入のクリーン VM への MSI インストール: ☐ Pass ☐ Fail
  - 未導入であること: ☐ Visual Studio ☐ Windows App SDK runtime ☐ VC++ Redistributable
- Start Menu の `azooKey Settings` から設定アプリが起動: ☐ Pass ☐ Fail
- Windows 設定 > IME > Options から `ITfFnConfigure` 経由で起動し langid / profile context が渡る: ☐ Pass ☐ Fail
- 二重起動しても設定ウィンドウが 1 つに保たれる: ☐ Pass ☐ Fail
- machine-wide TIP 登録後に代表アプリから設定起動経路が成立: ☐ Pass ☐ Fail
  - MSI が machine-wide 登録を行うため、本レーンで `register-dev.ps1` は使わない
- アンインストールで設定 EXE・WinUI runtime payload・ショートカット・TIP 登録が残らない: ☐ Pass ☐ Fail

## 記録
- `msiexec /L*V` ログの保存先:
- スクリーンショットの保存先（Start Menu 起動 / Options 起動 / 二重起動）:
- 失敗項目があれば切り出した個別 Issue:
```

### 設定反映と学習不変（DEV-1160 / DEV-1188 / DEV-1346 / DEV-1046 / DEV-1411 / DEV-1360）

設定の切替を含む各課題に、同じ環境ブロックを付けて別々に記録する。
学習不変を判定する区間は、ほかの打鍵を挟まず直前・直後の snapshot ラベルと比較結果を添える。

```md
## DEV-1160 設定反映
- 同じ Notepad 接続で設定保存前後の変換と ETW 4002 phase / backend: ____
- 設定保存後に起動したアプリとの差: ____
- composition を保持したまま保存したときの表示と確定観測: ____

## DEV-1188 M46 secure
- secureApps / IS_PASSWORD の対象アプリ、通常入力と ai_cleanup 以外のバッチ入力: ____
- 学習不変の前後ラベル / Changed / Unverified: ____
- secure インジケータ、通常アプリへ戻した後の学習再開: ____

## DEV-1346 M14 secure 項目
- liveConversion の secure 入力先での抑止、通常入力先への復帰: ____
- 学習不変を確認した場合の前後ラベル / Changed / Unverified: ____

## DEV-1046 M58-B/C 非学習・安全入力
- AI 通信を抑止した password / PIN / scope 不明の入力先: ____
- AI 整文で学習しないことの前後ラベル / Changed / Unverified: ____
- 長文再選択、句読点 ON/OFF、local-zenzai と承認済み API の品質: ____
- API キーと入力本文を検証メモへ載せていない: ☐ 確認

## DEV-1411 AI 整文のエラー分類通知
- 分類ごとの誘発手段（ループバック stub の 401 / 429 + `Retry-After` / 遅延応答、待ち受けのないポート、復号失敗）と候補フッター文言: ____
- UI-less アプリで同じ文言が description として届くか: ____
- 失敗時の候補と確定結果が fallback と同じか、成功・キャンセル時に通知が出ないか: ____
- 再試行待機中の追加打鍵 / Esc による即時解除: ____
- 通知とログに入力本文・キー・応答本文が無い: ☐ 確認

## DEV-1360 M17 カスタムローマ字
- 使用した TSV の要点（規則数、変更した規則。私的な語は書かない）と保存手順: ____
- 保存し直した後の進行中 preedit の維持と、次の入力からの反映: ____
- 空ファイル・不正行のみの TSV で内蔵表へ戻るか: ____
```

### M13〜M15・M20・M61 の打鍵と表示（DEV-1266 / DEV-1346 / DEV-1350 / DEV-1408 / DEV-760〜762）

自動テストの結果だけで人の入力・視覚判断を Pass にしない。
アプリ、設定、操作、期待した表示と実際の表示を各課題のコメントへ分ける。

```md
## DEV-1266 M13
- M3〜M10 の候補巡回・確定・Esc・Backspace・記号・staleness・ショートカット: ____
- batch ON 時の文節移動、Notepad と UI-less アプリとの差: ____

## DEV-1346 M14
- liveConversion ON の高速入力と preedit、Enter・Backspace・Esc、Space 後の復帰: ____
- liveConversion OFF との対照と設定切替: ____

## DEV-1350 M15
- 予測窓のキャレット右側配置、画面端での反転と欠け: ____
- Tab / Shift+Tab / クリック / Esc と読みの残り: ____
- predictionEnabled OFF との対照: ____
- `privacy.custom.prediction=false` と secure 入力先（secure 入力欄、app profile の secure）で予測窓が出ない: ____

## DEV-760 / DEV-761 / DEV-762 M61（各課題へ別コメント）
- DEV-760 基本ペア・閉じ括弧のスキップ・Backspace・Host 不在時の挙動: ____
- DEV-760 VS Code / Edge で、対の後に続けて打った文字が対の内側へ入る: ____
- DEV-760 `bracketPairingTrigger=composition` の確定後にキャレットが対の内側にある（VS Code / Edge）: ____
- DEV-761 per-app denylist / allowlist と組み込みシード: ____
- DEV-761 Notepad を閉じずに denylist → allowlist を連続で保存し、IME を再有効化せずに切り替わる（設定ファイルの原子的置換でも同じか）: ____
- DEV-761 設定監視のログ `tip_settings_watch_*` の有無（`AZOOKEY_LOG=1` と `AZOOKEY_LOG_LEVEL=info` で採った場合）: ____
- DEV-762 対称デリミタ・選択囲みと Undo: ____

## DEV-1408 M20 再変換（Notepad / Office）
- 選択からの変換キーでの候補取得と置換: ____
- 選択直後の右クリックでの元表記の即時返却と、遅れて届いた候補の表示: ____
- 選択・文脈・位置を変えた後に古い候補が表示・適用されないか: ____
- Office が未導入なら未実施と記録: ☐ 該当
```

### 障害注入と診断（DEV-1263 / DEV-1395 / DEV-1398 / DEV-1211 / DEV-1092 / DEV-677 / DEV-905）

同じ C-013 / C-010 を複数課題の根拠へ使う場合も、課題ごとに期待結果と人の観察を分ける。
DEV-1092 は課題の 4 項目に分けて書く。項目 4 はレーン 1 の状態で確認した場合だけ結果を書く。

```md
## DEV-1263 M42
- C-013 中の degraded・かな/カタカナ fallback と再開後の Ready: ____
- C-010 中の Space / Enter と再起動後の漢字候補、遷移ログ・入力本文の非混入: ____

## DEV-1395 C-013 実アプリ
- Notepad / VS Code / Edge の `report.json` 結果（pass / fail / failing-skip）と回収先: ____
- `ipc_connection_state_transition` の `process_id` と `ready → degraded → ready`: ____
- Host の同一 PID での再開、watchdog 期限・runner 異常終了時に停止が残らないか: ____

## DEV-1398 M47 劣化表示
- `degraded_simple` の表示と 5 秒後の消去、復帰後の入力継続: ____
- `degraded_model` の [詳細] / [再試行]、再試行中の重複抑止、成功後の解消: ____
- SafeMode の通知（同一 Host 世代で 1 回、[再試行] なし）と、誘発前 checkpoint への復元: ____
- 候補ウィンドウ非表示中に変化した場合の次回表示、通常候補の選択を妨げないか: ____
- 表示中に文節移動で候補一覧を出し直しても 5 秒の残りの間消えないか: ____

## DEV-1211 登録ロールバック
- 昇格・Debug 失敗注入・登録 smoke の環境と結果: ____
- 失敗後の HKLM CLSID / TSF profile / category の残骸、再登録、解除: ____

## DEV-1092 診断
- 1 ETW で IME 入力 1 回の TIP → IPC → Host → converter/backend の相関、採取権限、manifest の登録と解除、トレースへの本文の非混入: ____
- 2 設定アプリの明示 off / local 保存、保存先の表示、保存できないときの表示、Host への反映: ____
- 3 Host / 設定アプリの障害でのメタデータ限定 dump の解析可否、off 時の未生成、OS 管理の WER との境界: ____
- 4 clean VM での IME 動作とアンインストール後の診断ファイルの残留: ☐ Pass ☐ Fail ☐ 未実施（レーン 1 の状態が要る。理由: ____）
- 採取設定を元へ戻した: ☐ 確認

## DEV-677 WPR 実採取
- `wevtutil im`、`wpr -start` / `wpr -stop`、`wevtutil um` の終了コードと、ETL のサイズ・SHA-256: ____
- WPA の版と、Generic Events での `azooKey-Desktop` イベント件数: ____
- 同じ `client_id` / `request_id` で 3000 → 3003（FrameWrite）→ 4000 → 4002 → 4001 → 3003（FrameRead）→ 3001 が揃ったか（揃わなければ欠けた ID）: ____

## DEV-905 Application Verifier
- 対象イメージ / 有効にした検査（Heaps / Handles / Locks）: ____
- 開始 checkpoint 名: ____
- 打鍵した動線（`dev32-verification-checklist.md` の A1〜A8）と結果: ____
- verifier の停止: ☐ なし ☐ あり（停止コード / stack の保存先 / 起票した課題: ____）
- デバッガの接続: ☐ 接続した ☐ 接続していない（停止時に情報が残らない）
- `appverif.exe -disable '*' -for notepad.exe` の実行: ☐ 済
- `appverif.exe -query '*' -for notepad.exe` に設定が残っていない: ☐ 確認
- 解除の確認後に checkpoint へ復元: ☐ 済
```

### DEV-365

DEV-365 の検証メモは `compat-test/m3_display_attribute_checklist.md` §7 の記録テンプレートを使い、本書はひな形を持たない。
Notepad / Edge / VS Code で省略する D-01 / D-04 / D-05 / D-06 には、根拠にした compat run（レーン 2 の手順 1 の `report.json`）を併記する。
D-08 はスケールごとに記録し、D-09 は未実施・環境制約として記録する（レーン 2）。

### DEV-847

アプリごとに G1〜G3 を記録する。
一部のアプリしか実施できなかった場合は、未実施のアプリを明記する。

```md
## アプリ別の結果（Notepad / Edge / VS Code）
各行: ☐ Pass ☐ Fail ☐ 未実施

- G1 候補選択と preedit の追従（↑↓ ごとの surface 切り替え、Enter と数字キーの確定一致、Esc / 追加入力 / Backspace での reading 復帰）
  - Notepad: / Edge: / VS Code:
- G2 明示句読点と `/`（preedit への `、` `。` 挿入、未変換強制確定が起きない、`/` 後の Backspace 整合、preedit 無しでのパススルー）
  - Notepad: / Edge: / VS Code:
  - 起票時は Notepad / Edge で発生し VS Code では再現しなかった。アプリ差が解消したか:
- G3 preedit 更新中のキャレット（追加入力ごとの末尾追従、Backspace 後の末尾移動、下線範囲の維持、確定後の末尾配置）
  - Notepad: / Edge: / VS Code:

## 未実施のアプリと理由

## 検出した不整合
- 再現条件を別 Issue へ切り出し、対応する実装課題へリンクした: ☐ 済 ☐ 該当なし
```

### DEV-153

```md
## アプリ別の実測（`nihongo` → Space）
各行: TIP activate / `pbShow` 戻り値 / 描画主体（自前 HWND か OS・アプリ側か）

- メモ帳:
- Edge:
- Chrome:
- VS Code:
- Windows ターミナル:
- Win11 スタート検索（activate + 入力までを確認。統合インライン表示は M21 スコープ）:
- Office 365 Word:

## 未導入で測れなかったアプリ

## `docs/legacy-parity-spec.md` §12 の合格条件を満たすか
☐ 満たす ☐ 満たさない（対象アプリと差分を記載）
```

### DEV-716

```md
## 自動 runner
- 先行実行: コマンド / 出力先 / 終了コード / `case_selection`: ____
- C-006（150% / 200% の各回）: コマンド / 出力先 / 終了コード / `case_selection` / ホスト回収先: ____
- C-013: コマンド / 出力先 / 終了コード / `case_selection`: ____
- C-010: コマンド / 出力先 / 終了コード / `case_selection`: ____
- 設定別の追加実行（C-014 / C-016: live OFF、C-018: live ON、C-019: 予測 ON）: 設定 / コマンド / 出力先 / 終了コード / `case_selection`: ____
- case ごとの結果 (C-001〜C-019、C-005 を除く): pass __ / fail __ / failing-skip __ / 除外・未実行 __
- C-013 の結果は DEV-1263 にも記録し、DEV-716 の C-001〜C-012 の合否と分ける: ☐ 記録
- C-005 は対象外（DEV-782）。failing-skip として残ることを確認: ☐ 確認

## runner が証明できない項目
- C-007 を azooKey の候補から絵文字確定してサロゲートペアが壊れない: ☐ Pass ☐ Fail ☐ 未実施（DEV-403 が要る場合）
  - 確認経路: ☐ ユーザー辞書へ絵文字 surface を登録 ☐ その他（記載）
- C-006 を 150% DPI で確認し、復元前に証跡を回収: ☐ Pass ☐ Fail / 回収先: ____
- C-010 で supervisor 稼働下の Host kill、DegradedSimple 継続、pipe 復帰: ☐ Pass ☐ Fail
- runner 実行の前後でクリップボードが全 format 保持される（遅延レンダリングを含む）: ☐ Pass ☐ Fail
- 実行前から開いていた Notepad を操作、終了しない: ☐ Pass ☐ Fail
- 複数 tab のうち runner 作成 tab だけに入力し、既存 tab と未保存文書を変更しない: ☐ Pass ☐ Fail
- failure artifact に入力本文と候補本文の画面ピクセルが残らない: ☐ Pass ☐ Fail

## クリップボードの私的内容は記録しない
```

### DEV-676

```md
- 1 ログオン自動起動（supervisor / host プロセス / `\\.\pipe\azookey-<SID>` の存在）: ☐ Pass ☐ Fail
- 2 実ホスト kill からの復帰: DEV-716 の C-010 実走結果を参照（結果: ____）
  - `inference-host-stderr.log` に launch ごとのログ: ☐ あり ☐ なし
- 3 別ユーザー provisioning が `windows-tsf-host-architecture.md` の記載どおり: ☐ Pass ☐ Fail
- 4 `unregister-dev.ps1` が稼働中ホストを落とさず監督のみ停止: ☐ Pass ☐ Fail
- MSI のログオン経路（レーン 1 の状態）でのコンソールウィンドウの一瞬の表示: ☐ 出ない ☐ 出る ☐ 未実施（理由: ____）
- MSI のログオン経路（レーン 1 の状態）で、ConstrainedLanguage または PowerShell 5.1 を無効にしたときに launcher 診断が残る範囲と回復方法: ____（☐ 未実施。理由: ____）
- 監視失敗（exit 3）または安定稼働前の 5 回連続失敗で supervisor が停止し、再ログオンか手動の再起動で復帰する: ☐ Pass ☐ Fail ☐ 未実施（理由: ____）
```

### Windows CLI / コンソール（DEV-963 / DEV-1144 / DEV-758 / DEV-759）

```md
## DEV-963 非 ASCII argv
- shell と版 / active code page（`paths.json` の `activeCodePage`）/ コンソールの種類: ____
- 組 cp932: `lookup` の結果が学習ファイルのレコードに一致: ☐ Pass ☐ Fail
- 組 cp932: `userdict add` / `list` / `remove` の往復: ☐ Pass ☐ Fail
- 組 cp932: `--learning` と `--user-dict` のディレクトリに残ったファイル名: ____（想定外の名前: ☐ なし ☐ あり）
- 組 unicode: `lookup` / `userdict` の往復 / 残ったファイル名: ☐ Pass ☐ Fail（内容: ____）
- 失敗時に別パスのファイルや文字化けした永続データが作られていない: ☐ 確認
- 課題が求める code page で未実施の場合、その code page: ____

## DEV-1144 非 ASCII パスの GGUF 実ロード
- OS build / active code page / Host のビルド（llama.cpp の有無）: ____
- GGUF のファイル名（元の名前）と SHA-256: ____
- 組 cp932: `--model` で起動したときのログの `model_load`（ok / error）と Zenzai 候補: ☐ Pass ☐ Fail
- 組 cp932: `model.selectedPath` での preload と `loaded_model_path` の一致（D-007 / D-008 の status）: ☐ Pass ☐ Fail
- 組 unicode: `--model` / `model.selectedPath`: ☐ Pass ☐ Fail（内容: ____）
- 失敗した場合の切り分け（Host の probe / llama.cpp の open / その他）と起票した課題: ____
- ログの抜粋に入力本文とユーザー名を含む絶対パスが無い: ☐ 確認

## DEV-758 同時更新
- user_dict.json: pipe 経由 `userdict add` と `--offline` を重ねて実行し、両方の entry が残る: ☐ Pass ☐ Fail
  - 試行回数 / 毎回両方が残ったか:
  - `userdict list` の出力（entry 数のみ。読みと表層は記録しない）:
- 修正前の消失再現を試みたか: ☐ 試みた（結果: ____） ☐ 試みていない

## DEV-759 コンソール終了時の flush
- 実行時点で DEV-791 の即時 flush が入っているか: ☐ 入っている ☐ 入っていない（commit: ____）
- 各経路で Handshake を先に送り、`"accepted":true` と CommitObservation の `"ok":true` を確かめた: ☐ 確認（行が Host に届かなかった経路: ____）
- 保証経路 stdio モードで Ctrl+C: ☐ 学習データ保持 ☐ 消失（消失なら Fail）
- best-effort 経路 単発 commit 直後に × 終了（classic conhost）: ☐ 保持 ☐ 消失
- best-effort 経路 単発 commit 直後に × 終了（Windows Terminal / ConPTY）: ☐ 保持 ☐ 消失
- 連続 commit の途中で × 終了したときの消失件数: ____ 件（上限 7 件以内かを記録）
- 対象外経路（タスクマネージャの強制終了、ログオフ）: ☐ 未実施（`docs/learning-data-management-spec.md` §11.1 で対象外。未達として記録しない）
- 確認方法（経路ごとの `lookup` の件数と、終了前後の `learning-data-snapshot.ps1` の比較結果を貼る）:
```

判定は `docs/learning-data-management-spec.md` §11 の区分に従う。
Ctrl+C は保証経路であり、消失すれば Fail である。
`×` 終了は best-effort 経路なので、消失そのものは Fail ではない。
DEV-791 の即時 flush が入った状態では、単発 commit 直後の `×` は終了経路によらず保持されるため、消失していれば実装が効いていない証拠として記録する。
連続 commit の途中で終了した場合は、消失が §11.3 の上限（既定で 7 件かつ 5 秒ぶん）に収まるかを見る。
タスクマネージャの強制終了とログオフは対象外であり、実施せず、未達としても記録しない。

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
