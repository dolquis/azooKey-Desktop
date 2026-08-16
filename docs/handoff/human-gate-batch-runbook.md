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

**MSI**：DEV-765 と DEV-673 が対象とする配布形態の成果物。
再現性を保つため、既に実機検証済みの MSI と同一のものを使う。
DEV-765 は `azooKey-0.0.0-b417dc846738-x64.msi`（commit `b417dc84`、SHA-256 `7c18bb64…78aabb`）で症状を観測している。
別ビルドを使うと、観測済みの症状と切り分け結果が対応しなくなる。

**検証 zip**：レーン 2 の全ゲートが使う開発登録用の成果物。

```powershell
cmake --preset windows-release -DAZOOKEY_FETCH_GOOGLETEST=ON
cmake --build --preset windows-release
cmake --build --preset windows-release --target compat_test

.\scripts\make-vm-verify-package.ps1 `
  -Preset windows-release `
  -OutputDirectory .\build\vm-verify-packages `
  -RuntimeInstallerPath C:\path\to\vc_redist.x64.exe `
  -ModelPath C:\path\to\zenz-v3.gguf
```

`-ModelPath` は必須である。
DEV-225 の A5 判定は実 GGUF での推論結果を見るものであり、GGUF なしでは `SimpleConverter` の静的辞書しか動かず判定が成立しない。

持ち込むもののうち、パッケージ生成が拾わないものを別途 VM へ入れる。

- Sysinternals Suite（`procmon`、`handle`）：DEV-765 の段 3 と段 4 で使う
- 2 台目のモニター構成と 150% DPI 設定：DEV-716 の C-005 と C-006 が要求する
- 絵文字を含むユーザー辞書エントリ、または絵文字を返す辞書：DEV-716 の C-007 を TIP 経路で確認するため

## Part A：DEV-765 の切り分け階梯

DEV-765 は「Microsoft Store の検索欄で preedit が開始しない」という症状を、4 つの判定のいずれかへ絞り込む spike である。
症状が DEV-555 の予測（preedit は出るが候補が来ない）と食い違っているため、修正方針を決める前に失敗した境界を特定する。

課題本文が禁じているとおり、切り分けが終わるまで AppContainer 向けの ACE 追加、peer 検証の無効化、token の露出を行わない。
先に緩和すると、どの境界が失敗していたのかを永久に確定できなくなる。

段を上から順に実行し、期待どおりでない段が見つかった時点で止めて、その段が指す判定を結論とする。
各段は Notepad と Store の検索欄を同一ログオンセッションで対照して観測する。
Notepad 側が期待どおりで Store 側だけが外れることを確認して初めて、その段が AppContainer 固有の境界だと言える。

### 段 0：観測条件の固定

クリーン checkpoint から復元し、インストールログを残した状態で MSI を入れる。

```powershell
msiexec /i C:\azookey-verify\azooKey-0.0.0-b417dc846738-x64.msi /L*V C:\azookey-verify\msi-install.log
```

インストール後、ログ取得を有効にしてからサインアウトとサインインを行う。
TIP は各アプリのプロセス内で動くため、環境変数を設定した後に起動したプロセスにしかログ設定が効かない。

```powershell
[Environment]::SetEnvironmentVariable('AZOOKEY_LOG', '1', 'User')
[Environment]::SetEnvironmentVariable('AZOOKEY_LOG_LEVEL', 'info', 'User')
```

`AZOOKEY_LOG_LEVEL` が解釈するのは `warn` と `error` だけで、それ以外の値は Info へ落ちる。
`debug` を指定しても情報量は増えない。

### 段 1：TIP DLL 個別の ACL

8/14 の検証では親ディレクトリの `ALL APPLICATION PACKAGES` RX を目視で確認したが、DLL 自身が継承しているかは未確認である。
`%ProgramFiles%` は既定でこの ACE を持ち、MSI はそれを継承する前提で自前の ACL を持たない（`docs/sideload-packaging-spec.md` §4）。
継承が実際に効いているかをファイル単位で確認する。

```powershell
icacls "$env:ProgramFiles\azooKey\azookey_tsf_tip.dll"
Get-Acl "$env:ProgramFiles\azooKey\azookey_tsf_tip.dll" |
  Select-Object -ExpandProperty Access |
  Where-Object IdentityReference -like '*ALL APPLICATION PACKAGES*'
```

期待は、`S-1-15-2-1`（ALL APPLICATION PACKAGES）に対する ReadAndExecute の許可が継承 ACE として存在することである。
deny ACE が重なっていないことも見る。
Windows は deny を先に評価するため、部分的な deny でもロードを壊す。

ACE が無い、または deny がある場合は **判定 1（DLL 未ロード：MSI ACL 経路）** で確定する。
ACE がある場合も、それだけではロードの成功を意味しないため段 2 へ進む。

### 段 2：入力欄をホストしているプロセスの特定

Store の検索欄を実際にどのプロセスが持っているかを、記憶や推測ではなく実測で決める。
プロセス名を先に決め打ちすると、以降の段の観測がすべて空振りする。

UWP アプリのウィンドウは `ApplicationFrameHost.exe` がフレームを持ち、アプリ本体は別プロセスで動く。
フォアグラウンドウィンドウのハンドルから引いたプロセスは、この構成ではフレーム側を指すことがある。
TIP が読み込まれるのは入力欄を実際に持つ本体側なので、フレーム側の PID を対象にすると段 3 が誤った結論を出す。

Store を起動した状態で、パッケージアプリのプロセスを列挙する。

```powershell
Get-Process |
  Where-Object { $_.Path -like '*\WindowsApps\*' } |
  Select-Object Id, ProcessName, Path
```

以降で使う `$handleExe` と `$procmonExe` は、診断プレイブック「ツールの準備」の解決方法で得る。
Sysinternals の実行ファイル名は配布形式によって 64-bit suffix の有無が異なるため、パスを決め打ちしない。

列挙した各プロセスについて、pipe ハンドルの有無を記録する。

```powershell
& $handleExe -a -p <PID> | Select-String 'pipe|azookey'
```

Store の検索欄をホストしているプロセスを一意に決められない場合は、候補として挙げた PID 群をそのまま記録し、段 3 を候補すべてに対して実行する。
一つに絞れないこと自体は判定を妨げない。
段 3 でどれにも DLL が読み込まれていなければ、結論は同じである。

### 段 3：TIP DLL のロード有無

`azookey_tsf_tip.dll` を読み込んでいるプロセスを一覧し、段 2 の候補と対照する。
`tasklist /m` は DLL 側から引くため、段 2 で PID を一つに絞れていなくても結論を出せる。

```powershell
tasklist /m azookey_tsf_tip.dll
```

出力に段 2 で挙げた候補 PID が含まれるか、また Notepad が含まれるかを対照する。
Notepad に読み込まれていることを確認して初めて、Store 側の不在が AppContainer 固有だと言える。

Store 側だけロードされていない場合は **判定 1（DLL 未ロード）** で確定する。
段 1 で ACL が正常だったのにロードされていないなら、原因は ACL ではなく TSF 登録か AppContainer activation の側にある。
その区別まで進めるときは、Process Monitor で対象プロセスの `azookey_tsf_tip.dll` に対する `CreateFile` の結果コードを見る。
`ACCESS DENIED` なら ACL、`NAME NOT FOUND` なら登録パス、そもそも試行が無いなら TSF が TIP を候補に挙げていない。

```powershell
& $procmonExe -accepteula -backingfile C:\azookey-verify\dev765.pml -quiet -minimized
# Store の検索欄へ ni と打鍵してから
& $procmonExe -terminate -quiet
```

### 段 4：TIP の活性化と composition 開始

DLL がロードされている場合、TIP が活性化してキーイベントを受け取っているかを JSONL ログで見る。

```powershell
Get-Content "$env:LOCALAPPDATA\azooKey\logs\tip-$(Get-Date -Format yyyyMMdd).jsonl" -Tail 200
```

Store の検索欄へ打鍵した時刻の前後に、TIP のイベントが記録されているかを確認する。
イベントがまったく無い場合は、DLL はロードされているが `ITfKeyEventSink` までキーが届いていない。
イベントはあるが composition が始まっていない場合は、input scope による抑止か `ITfContext` 側の拒否を疑う。

いずれも **判定 2（DLL ロード済み、preedit なし）** で確定する。
判定 2 は DEV-555 の想定と異なる領域であり、修正の owner は TIP 側になる。

### 段 5：IPC と handshake

preedit が出ているのに候補が来ない場合だけ、この段に来る。
これが DEV-555 が予測していた症状である。

```powershell
Get-Content "$env:LOCALAPPDATA\azooKey\logs\host-$(Get-Date -Format yyyyMMdd).jsonl" -Tail 200
```

TIP 側の `ipc_connected` と `ipc_handshake_rejected` の有無、Host 側に query が届いているかを突き合わせる。
接続そのものが失敗しているなら pipe の DACL、接続後に拒否されているなら peer 検証か handshake token が原因である。
**判定 3（preedit あり、候補なし）** で確定し、DEV-555 へ引き継ぐ。

### 段が尽きた場合

上の 5 段のいずれにも当てはまらない挙動なら、**判定 4** として再現条件と次に打つ最小の probe を記録する。
根本原因を断定できないこと自体は失敗ではない。
課題の完了条件は「最後に成功した境界と最初に失敗した境界を特定すること」である。

## Part B：VM 状態でレーンを分ける

人間ゲートは要求する VM 状態が 2 種類に分かれる。
状態をまたぐたびに checkpoint 復元が要るため、同じ状態のゲートをまとめて走らせる。

### レーン 1：クリーン VM に MSI を入れた状態

MSI 配布形態そのものを対象とするゲートを置く。
開発登録（`register-dev.ps1`）を先に走らせると、開発登録が付ける AppContainer ACL が MSI 側の継承 ACL と混ざり、DEV-765 の段 1 が観測不能になる。
このレーンでは開発登録を一切行わない。

1. Part A（DEV-765）を実行する。
2. DEV-765 の判定が出た結果、Store 以外の受け入れ条件が満たせるなら DEV-673 を再走する。判定 1 から 3 のいずれかで Store 入力が未成立のままなら、DEV-673 はその 1 項目を明示的に未達として記録し、残りの項目（インストール、IME 出現、打鍵から確定、アンインストールでの登録解除）だけ判定する。

レーン 1 が終わったら、必ずベースライン checkpoint へ復元する。
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

DEV-758 の第二の書き手には `userdict` CLI を使う。
Host が稼働している状態で `--offline` を付けると、CLI が IPC を経由せず同じファイルを直接書くため、プロセス間ロックの効きを確認できる。

```powershell
.\azookey_inference_host.exe userdict add --reading てすと --surface テスト --offline
.\azookey_inference_host.exe userdict list --format json
```

`settings.json` 側は事情が異なる。
設定アプリは未実装であり（DEV-674 が実装対象として立っている）、`settings.json` を書く第二のプロセスが現時点で存在しない。
このゲートで再現できるのは `user_dict.json` の同時更新までである。
`settings.json` の同時更新は、テキストエディタからの手動書き込みを第二の書き手に代用するか、設定アプリ着地まで未実施として記録する。
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
- VM: Hyper-V / セッション種別 = 基本セッション / checkpoint 名:
- バックエンド: ☐ CPU (SimpleConverter) ☐ zenz GGUF (ファイル名)
```

### DEV-765

```md
## 判定
☐ 判定1 DLL未ロード ☐ 判定2 ロード済み・preeditなし ☐ 判定3 preeditあり・候補なし ☐ 判定4 上記以外

## 段ごとの観測
- 段1 DLL個別ACL: ALL APPLICATION PACKAGES = ☐ 継承Allow ☐ 明示Allow ☐ Deny ☐ 無し
- 段2 ホストプロセス: PID / プロセス名 / package SID:
- 段3 DLLロード: Store = ☐ ロード済 ☐ 未ロード / Notepad = ☐ ロード済 ☐ 未ロード
  - Procmon の CreateFile 結果コード（未ロード時のみ）:
- 段4 TIPイベント: 打鍵時刻前後の tip-*.jsonl 記録の有無:
- 段5 IPC: ipc_connected / handshake の結果（段5に到達した場合のみ）:

## 最後に成功した境界 / 最初に失敗した境界

## 修正対象の owner 領域と必要な spec 変更

## 診断中に緩和策を入れていないことの確認
☐ AppContainer ACE 追加なし ☐ peer 検証無効化なし ☐ token 露出なし
```

### DEV-673

```md
## 受け入れ条件ごとの結果
- MSI インストール（クリーン Win11、VC++ Redist 未導入）: ☐ Pass ☐ Fail
- IME がプロファイルに出現: ☐ Pass ☐ Fail
- 打鍵から変換、確定: ☐ Pass ☐ Fail
- AppContainer 入力（Microsoft Store 検索欄）: ☐ Pass ☐ Fail ☐ 未達（DEV-765 判定 __ を参照）
- アンインストールで TIP 登録が解除: ☐ Pass ☐ Fail
  - 確認: HKLM CLSID / CTF\TIP / インストール先ディレクトリの残骸
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
- C-011 前後で Notepad テキスト、CF_HDROP、delayed-rendering clipboard が保持: ☐ Pass ☐ Fail
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
- user_dict.json: Host 稼働中に `userdict add --offline` を実行し編集が消失しない: ☐ Pass ☐ Fail
- settings.json: 第二の書き手 = ☐ テキストエディタで代用 ☐ 未実施（設定アプリ未実装 / DEV-674）
- 修正前の消失再現を試みたか: ☐ 試みた（結果: ____） ☐ 試みていない

## DEV-759 コンソール終了時の flush
- stdio モードで × 終了: ☐ 学習データ保持 ☐ 消失
- stdio モードで Ctrl+C: ☐ 学習データ保持 ☐ 消失
- 確認方法（終了前後の学習データ差分の取り方）:
```

## 中止条件と後始末

`verify-bootstrap.ps1 -Json` が `overallStatus=fail` を返した場合は、打鍵系のゲートへ進まない。
前段の失敗を後段の判定に持ち込むと、どのゲートの結果も信用できなくなる。

Part A で判定 1 から 3 のいずれかが確定した時点で、DEV-765 はそこで終える。
残りの段を消化しても、修正 owner の特定という完了条件には何も足さない。

セッション終了時に次を行う。

- 各ゲートの検証メモを Linear の該当課題へコメントする
- 新規に検出した問題を Linear へ起票する。ラベルは `repo:*` と `area:*` を必須とし、実機確認を要する人間専任タスクには `agent:*` の代わりに `gate:human-required` を付ける
- `AZOOKEY_LOG` と `AZOOKEY_LOG_LEVEL` を削除する
- Process Monitor と WPR の採取プロセスが残っていないことを確認する
- ベースライン checkpoint へ復元する

dump、ETL、PML、ログには変換中の本文と候補が含まれうる。
Linear へ添付する前に内容を確認し、入力本文とローカル絶対パスを残さない。
