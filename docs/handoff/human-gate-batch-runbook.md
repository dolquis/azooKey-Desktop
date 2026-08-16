# 人間ゲート一括消化の VM セッション runbook

複数の `gate:human-required` 課題を 1 回の Hyper-V VM セッションでまとめて消化するための実行計画と記録様式を定める。
VM 構成、checkpoint 運用、bootstrap、TIP 登録の各手順は既存文書が正典であり、本書はそれらを再掲しない。

- VM 構成と 3 層フローと補助範囲：[`hyper-v-vm-verification-plan.md`](./hyper-v-vm-verification-plan.md)
- 登録、bootstrap、ログ取得、後始末の手順：[`hyper-v-tip-verification.md`](./hyper-v-tip-verification.md)
- 打鍵チェック項目 A1〜A8 と B1〜B7：[`dev32-verification-checklist.md`](./dev32-verification-checklist.md)
- compat runner の実行と出力レイアウト：[`../../compat-test/README.md`](../../compat-test/README.md)
- dump、ETW、Process Monitor の採取：[`windows-diagnostics-playbook.md`](./windows-diagnostics-playbook.md)

進捗と状態の正典は Linear（team `Dev` / project *azooKey Desktop / Windows IME MVP*）である。
本書は状態を持たない。
どの人間ゲートが実際に未消化かは、実行前に Linear で確認する。

## セッション前にホスト側で用意するもの

VM を起動する前に、2 種類の成果物をホストで作る。
レーン 1 とレーン 2 は前提とする VM 状態が異なるため、片方だけでは両方を走らせられない。

**MSI**：DEV-673 が対象とする配布形態の成果物。
DEV-765 のスパイクで使った `b417dc84` の MSI は使わない。
その MSI は互換カテゴリ登録（DEV-766 / PR #271）と設定アプリ同梱（DEV-674 / PR #272）のどちらも含まないため、既に修正済みの欠陥を再観測することになる。
`260b665` 以降の main から MSI を作り、ファイル名と SHA-256 を検証メモへ記録する。

**検証 zip**：レーン 2 の全ゲートが使う開発登録用の成果物。

`AZOOKEY_FETCH_LLAMA_CPP` の既定は `OFF` で、`windows-release` preset もこれを ON にしない。
llama.cpp を含まない Host に対して `-ModelPath` を渡すと、`register-dev.ps1` の preflight が `llama_cpp=1` を検出できずに登録を拒否する。
レーン 2 は DEV-225 に到達する前に停止するため、configure で明示的に ON にする。

```powershell
cmake --preset windows-release -DAZOOKEY_FETCH_GOOGLETEST=ON -DAZOOKEY_FETCH_LLAMA_CPP=ON
cmake --build --preset windows-release
cmake --build --preset windows-release --target compat_test
```

パッケージを作る前に、preflight が通る状態かをホストで確認する。

```powershell
.\build\windows-release\bench\azookey_zenzai_bench.exe
```

出力に `llama_cpp=1` が含まれない場合は、この時点で configure をやり直す。
VM へ持ち込んでから登録で弾かれると、checkpoint 復元からやり直しになる。

```powershell
.\scripts\make-vm-verify-package.ps1 `
  -Preset windows-release `
  -OutputDirectory .\build\vm-verify-packages `
  -RuntimeInstallerPath C:\path\to\vc_redist.x64.exe `
  -ModelPath C:\path\to\zenz-v3.gguf
```

`-ModelPath` は本セッションでは省略できない。
スクリプト側は既定 `""` で省略を許し、モデルと bench を含まないパッケージを正常に生成するため、指定漏れはエラーにならない。
一方 DEV-225 の A5 判定は実 GGUF での推論結果を見るものであり、GGUF なしでは `SimpleConverter` の静的辞書しか動かず判定が成立しない。
上の preflight 確認と同じく、VM へ持ち込む前に指定したかどうかを見る。

**compat runner の同伴バンドル**：`make-vm-verify-package.ps1` の payload は TIP、Host、diag、登録スクリプト、bootstrap、GGUF 関連だけで、`compat_test.exe` と target JSON を含まない。
DEV-716 は zip だけでは実行できないので、別に固めて持ち込む。

```powershell
$bundle = ".\build\vm-verify-packages\compat-bundle"
New-Item -ItemType Directory -Path $bundle -Force | Out-Null
Copy-Item .\build\windows-release\compat-test\compat_test.exe $bundle
Copy-Item .\compat-test\targets -Destination $bundle -Recurse -Force
Compress-Archive -Path "$bundle\*" -DestinationPath "$bundle.zip" -Force
```

VM 側では `compat_test.exe` と `targets\` を同じディレクトリへ展開する。
runner は `--target` に渡したパスから target JSON を読むため、両者の相対関係を崩さない。

持ち込むもののうち、パッケージ生成が拾わないものを別途 VM へ入れる。

- Sysinternals Suite（`procmon`、`handle`）：Store 入力が再検証で失敗した場合の境界確認に使う
- 2 台目のモニター構成と 150% DPI 設定：DEV-716 の C-005 と C-006 が要求する
- 絵文字を含むユーザー辞書エントリ、または絵文字を返す辞書：DEV-716 の C-007 を TIP 経路で確認するため

## Part A：Store 入力の再検証

DEV-765 のスパイクは 2026-08-14 に実走済みで、判定は **1（DLL 未ロード）** に確定している。
Microsoft Store の `WinStore.App.exe` に TIP DLL がロードされておらず、pipe や handshake には到達していなかった。
TIP DLL の ACL は `ALL APPLICATION PACKAGES` と `ALL RESTRICTED APPLICATION PACKAGES` の RX を継承しており、ACL 不足ではなかった。

ロード前ゲートとして 2 つの独立した要件違反が挙がっている。

- 互換カテゴリ `GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT` の未登録（DEV-766）。PR #271 で登録済みになった。
- TIP DLL が未署名であること。Microsoft の IME 要件は第三者 IME への署名を求めている。署名ルートは DEV-255 で未決のままである。

したがって本セッションで行うのは切り分けではなく、カテゴリ修正後の再検証である。
切り分け階梯を再走しても、既に判定済みの結論を作り直すだけになる。

### 手順

`260b665` 以降の main から作った MSI をクリーン VM へ入れ、同一ログオンセッションで Notepad と Microsoft Store の検索欄を対照する。
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

**Store で preedit が出る場合**：カテゴリ欠落が原因だったことが実機で裏付けられる。
DEV-673 の AppContainer 入力項目を Pass として記録する。
DEV-555（pipe DACL の AppContainer capability ACE）は、preedit が出たうえで候補が来ないかどうかという別の問いなので、そこまで確認して結果を DEV-555 へ書く。

**Store で preedit が出ない場合**：未署名要件が独立に効いている可能性が残る。
これは診断で解けない種類の残件で、署名済み成果物を用意するまで確定できない。
署名は DEV-255 の人間判断待ちであり、本セッションで解消できないため、DEV-673 の当該項目は未達として記録して次へ進む。

境界をもう一段だけ詰めるなら、DLL がロードされたかどうかまでを確認して止める。

```powershell
tasklist /m azookey_tsf_tip.dll
```

Store のホストプロセスが一覧に現れず Notepad が現れるなら、失敗は依然としてロード前である。
ACL の観測だけで結論を出さない。
2026-08-14 の実走では ACL が正常でありながら DLL は未ロードだったため、ACL の状態は原因候補にはなっても判定の根拠にはならない。

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
どちらも使えない場合は、パッケージ名（2026-08-14 の実走では `Microsoft.WindowsStore_…_8wekyb3d8bbwe`）とプロセス名を記録して代える。

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

人間ゲートは要求する VM 状態が 2 種類に分かれる。
状態をまたぐたびに checkpoint 復元が要るため、同じ状態のゲートをまとめて走らせる。

### レーン 1：クリーン VM に MSI を入れた状態

MSI 配布形態そのものを対象とするゲートを置く。
開発登録（`register-dev.ps1`）を先に走らせると、開発登録が付ける AppContainer ACL が MSI 側の継承 ACL と混ざり、ACL 由来かどうかの判別ができなくなる。
このレーンでは開発登録を一切行わない。

開始状態は **vc_redist 未導入のクリーン checkpoint** とする。
`hyper-v-vm-verification-plan.md` §2 のベースライン checkpoint は「クリーン + Redistributable + 設定」で vc_redist を導入済みであり、これとは別物である。
DEV-673 の第 1 項目は VC++ Redistributable 未導入の環境で MSI がインストールできること（CRT の app-local 同梱が効いていること）を見るものなので、ベースライン checkpoint から始めると前提が崩れたまま Pass になり、証跡の意味が失われる。
どちらの checkpoint を使ったかは、検証メモの環境ブロックの checkpoint 名で残す。

1. DEV-673 のチェックリストを頭から実施する。
2. AppContainer 入力の項目に来たら Part A の再検証を行い、その結果を当該項目へ記録する。

Store 入力が未成立のままでも、DEV-673 の残りの項目は判定できる。
未署名要件（DEV-255）は本セッションで解消できないため、当該項目の未達は想定内の結果であり、他の項目の判定を止める理由にはしない。

DEV-673 の課題本文は設定アプリ同梱（PR #272）より前に書かれている。
`%ProgramFiles%\azooKey` の配置確認では、課題本文が挙げる TIP、Inference Host、MSVC runtime 3 DLL、ライセンスファイルに加えて、`azookey_settings.exe` と self-contained ランタイム、スタートメニューのショートカットが増えている。
設定アプリ側の起動とアンインストールは DEV-767 で個別に検証済みなので、本ゲートでは配置物として存在することの確認にとどめ、差分があった事実を検証メモへ書く。

レーン 1 が終わったら、レーン 2 の開始状態（plan §2 のベースライン checkpoint）へ復元する。
MSI の machine-wide 登録を残したままレーン 2 の開発登録を重ねると、どちらの登録が効いているか判別できなくなる。

### レーン 2：開発登録と検証 zip を入れた状態

`verify-bootstrap.ps1` で導入した状態で走らせるゲートを置く。
順序は、状態を壊さないものから壊すものへ並べる。
Host を kill する検証を先に走らせると、以降のゲートが供給側の不安定な状態を引きずる。

1. **DEV-225**（Zenzai 漢字変換）。GGUF が要る唯一のゲートであり、判定基準が最も厳しいので、環境が最も素直な段階で走らせる。
2. **DEV-757**（ローマ字一括変換）。打鍵のみで、状態を壊さない。
3. **DEV-716**（Notepad C-001〜C-012）。`compat_test.exe` の自動実行と、runner が証明できない TIP 経路の手動確認を行う。C-010 が Host kill を含むため、レーン 2 の打鍵系はここまでで終える。
4. **DEV-676**（supervisor 復帰）。項目 2 は DEV-716 の C-010 実走結果を参照して記録し、再打鍵しない。残る項目 1（ログオン自動起動）と項目 3（別ユーザー provisioning）と項目 4（unregister が監督のみ停止）を実施する。項目 1 はログオフとログオンを挟むため、打鍵系の後に置く。
5. **DEV-758**（同時更新での編集消失）と **DEV-759**（コンソール終了時の flush）。どちらも Host のプロセス寿命を操作するため最後に置く。

DEV-716 の実行例を示す。
`--output` が既存の非空ディレクトリを指すと runner は実行を拒否するので、実行ごとに出力先を変える。

```powershell
.\compat_test.exe --target .\targets\notepad.json --output C:\azookey-verify\compat-notepad
```

終了コードは、全件 pass が `0`、fail を含む場合が `1`、fail は無いが failing-skip を含む場合が `2` である。

DEV-758 は `user_dict.json` への同時更新を作る。
`userdict` CLI は既定で稼働中の Host へ IPC 経由でコマンドを送り、`--offline` を付けるとファイルを直接書く。
この 2 経路を別々の entry で重ねると、二つの書き手が同じファイルを read-modify-write する状況になる。

単発の `--offline` を 1 回実行するだけでは足りない。
書き込みが時間的に重ならないため、ロックが無くても通ってしまい、ロックの効きを確認したことにならない。

`Start-Job` へ相対パスを渡さない。
Windows 11 の既定シェルである Windows PowerShell 5.1 は、子ランスペースを呼び出し元のカレントディレクトリではなくユーザーのホームで開始するため、`.\azookey_inference_host.exe` は解決に失敗する。
両ジョブが即座に失敗しても `list` は何も変わらないので、「両方残った」と読み違える。

```powershell
$exe = (Resolve-Path .\azookey_inference_host.exe).Path
$viaPipe = Start-Job { & $using:exe userdict add --reading ぱいぷ --surface パイプ }
$viaFile = Start-Job { & $using:exe userdict add --reading ふぁいる --surface ファイル --offline }
Wait-Job $viaPipe, $viaFile | Out-Null
Receive-Job $viaPipe, $viaFile
& $exe userdict list --format json
```

`Receive-Job` の出力を必ず読み、両ジョブが実際にコマンドを実行したことを先に確かめる。
これを飛ばすと、失敗を成功と取り違える。

判定は、`list` の出力に両方の entry が残っていることである。
片方だけが残る場合は、後勝ちの上書きで編集が消失している。
重なりを確実にするため、同じ操作を数回繰り返して毎回両方が残ることを見る。

`settings.json` 側は事情が異なる。
設定アプリは MSI へ同梱されたが（DEV-674、PR #272）、現行の実装に設定を保存する UI は無く、`settings.json` を書く第二のプロセスは今も存在しない。
保存機能を持つ実装課題は本書の作成時点で見当たらないため、未起票として扱う。
このゲートで再現できるのは `user_dict.json` の同時更新までである。
`settings.json` の同時更新は、テキストエディタからの手動書き込みを第二の書き手に代用するか、未実施として記録する。
どちらを選んだかを検証メモに明記し、代用で済ませた範囲を曖昧にしない。

### このセッションの対象外

DEV-194（推論バックエンド実機ベンチと ONNX Runtime GenAI 変換可否スパイク）は、TIP の実機動線ではなくホスト側のベンチと変換検証である。
VM の対話セッションを必要としないため、本セッションに混ぜず別に扱う。

## Part C：検証メモのひな形

Linear への記録様式を揃えておく。
各ゲートの完了条件は「検証メモを当該課題へコメントする」ことであり、記録が揃わないと Done へ遷移できない。

全ゲート共通で先頭に置く環境ブロック。

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

### Store 入力の再検証（DEV-673 の AppContainer 項目へ記録）

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
☐ 依然として未成立（未署名要件 DEV-255 が残るため本セッションでは確定不可）
☐ その他（観測内容を記載）

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
- AppContainer アプリ（Microsoft Store / Edge）で打鍵 → 変換 → 確定: ☐ Pass ☐ Fail ☐ 未達
  - 未達の場合の理由: ☐ 未署名要件（DEV-255）待ち ☐ その他
  - Store 入力の再検証結果は上のブロックを参照
- サインアウト / 再起動後もプロファイルと入力が成立: ☐ Pass ☐ Fail ☐ 未実施
- アンインストールで COM / TSF 登録と `%ProgramFiles%\azooKey` の配置物が残らない: ☐ Pass ☐ Fail
  - 確認: HKLM CLSID / CTF\TIP（native / WOW6432Node）/ インストール先ディレクトリ

## 記録
- MSI の取得元 / バージョン / SHA-256:
- `msiexec /L*V` ログの保存先:
- Windows build / 使用アプリ / 再起動の有無:
```

### DEV-225

```md
## A5 判定（最上位候補の完全一致で判定する。「含む」では不可）
- `nihongo` → Space の最上位候補: 「____」 / ☐ `日本語` に完全一致 ☐ 不一致
- `わたしはがくせいです` → 「____」 / ☐ `私は学生です` ☐ 不一致
- `top_debug_info` に `utf8-prefix-trimmed`: ☐ 出ない ☐ 出る
- 候補が Host / Zenzai 由来である証跡（`Candidate.debug_info` / host ログ）:
- GGUF 削除時に SimpleConverter へ劣化して候補が継続: ☐ Pass ☐ Fail
- （A3）`siro` / `tu` / `nn` の preedit 即時表示: ☐ Pass ☐ Fail
- （caret）確定後キャレットが末尾: ☐ Pass ☐ Fail
```

### DEV-716

```md
## 自動 runner
- 実行コマンド / 出力先 / 終了コード:
- case ごとの結果 (C-001〜C-012): pass __ / fail __ / failing-skip __

## runner が証明できない項目
- C-007 を azooKey の候補から絵文字確定してサロゲートペアが壊れない: ☐ Pass ☐ Fail
- C-005 を複数モニター構成で確認: ☐ Pass ☐ Fail
- C-006 を 150% DPI モニターで確認: ☐ Pass ☐ Fail
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
```

### DEV-757、DEV-758、DEV-759

```md
## DEV-757 ローマ字一括変換 end-to-end
- 打鍵 → かな preview → 変換 → 確定: ☐ Pass ☐ Fail
- 手順と結果:

## DEV-758 同時更新
- user_dict.json: pipe 経由 `userdict add` と `--offline` を重ねて実行し、両方の entry が残る: ☐ Pass ☐ Fail
  - 試行回数 / 毎回両方が残ったか:
  - `userdict list` の出力（entry 数のみ。読みと表層は記録しない）:
- settings.json: 第二の書き手 = ☐ テキストエディタで代用 ☐ 未実施
  - 設定アプリは同梱済みだが保存 UI が無く、保存機能の実装課題は未起票
- 修正前の消失再現を試みたか: ☐ 試みた（結果: ____） ☐ 試みていない

## DEV-759 コンソール終了時の flush
- stdio モードで × 終了: ☐ 学習データ保持 ☐ 消失
- stdio モードで Ctrl+C: ☐ 学習データ保持 ☐ 消失
- 確認方法（終了前後の学習データ差分の取り方）:
```

## 中止条件と後始末

`verify-bootstrap.ps1 -Json` が `overallStatus=fail` を返した場合は、打鍵系のゲートへ進まない。
前段の失敗を後段の判定に持ち込むと、どのゲートの結果も信用できなくなる。

Part A で Store 入力が依然として成立しない場合、境界確認は DLL のロード有無までで止める。
未署名要件が独立に効いている可能性を、未署名の成果物だけで切り分けることはできない。
それ以上の probe を重ねても、DEV-255 の署名ルートが決まるまで結論は変わらない。

セッション終了時に次を行う。

- 各ゲートの検証メモを Linear の該当課題へコメントする
- 新規に検出した問題を Linear へ起票する。ラベルは `repo:*` と `area:*` を必須とし、実機確認を要する人間専任タスクには `agent:*` の代わりに `gate:human-required` を付ける
- `AZOOKEY_LOG` と `AZOOKEY_LOG_LEVEL` を削除する
- Process Monitor と WPR の採取プロセスが残っていないことを確認する
- ベースライン checkpoint へ復元する

dump、ETL、PML、ログには変換中の本文と候補が含まれうる。
Linear へ添付する前に内容を確認し、入力本文とローカル絶対パスを残さない。
