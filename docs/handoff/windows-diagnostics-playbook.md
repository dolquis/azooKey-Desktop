# Windows 診断プレイブック

この文書は、azooKey TIP、inference-host、Named Pipe の crash、hang、IPC timeout、handle leak を開発環境で調査する手順を定める。
対象は再現可能な開発用ビルドであり、一般利用中のアプリや第三者プロセスへ verifier、page heap、debugger を設定しない。

## ツールの準備

最初にリポジトリ直下で doctor を実行する。
診断ツールは optional check なので、不足しているツールは `warning` になり、通常のビルド要件を `error` にしない。

```powershell
just doctor --json |
  ConvertFrom-Json |
  Select-Object -ExpandProperty checks |
  Where-Object id -like 'tool.*'
```

doctor は次の安定 ID を返す。

| ID | 用途 | 導入元 |
|---|---|---|
| `tool.windbg` | GUI debugger、dump 解析 | `winget install Microsoft.WinDbg` |
| `tool.cdb` | コンソール debugger、dump 解析 | Windows SDK の Debugging Tools for Windows |
| `tool.gflags` | page heap などのプロセス別 debug flag | Windows SDK の Debugging Tools for Windows |
| `tool.wpr` | ETW trace の採取 | Windows Performance Toolkit または Windows 組み込み |
| `tool.wpa` | ETW trace の解析 | Windows Performance Toolkit |
| `tool.procdump` | crash、hang、手動 dump の採取 | Sysinternals Suite |
| `tool.procmon` | file、registry、process、thread activity の採取 | Sysinternals Suite |
| `tool.appverifier` | heap、handle、lock の実行時検証 | Windows SDK の Application Verifier |
| `tool.handle` | process ごとの handle 集計 | Sysinternals Suite |

WinDbg は `WinDbgX.exe`、`windbg.exe`、既知の Windows SDK 配置、`Microsoft.WinDbg` Appx package の順で検出する。
CDB と GFlags は PATH に加えて Windows SDK の既定 x64 配置も検出する。

公式の導入手順は [WinDbg](https://learn.microsoft.com/windows-hardware/drivers/debugger/) と [Debugging Tools for Windows](https://learn.microsoft.com/windows-hardware/drivers/debugger/debugger-download-tools) を参照する。
WPR profile の構文と起動形式は [WPR command-line options](https://learn.microsoft.com/windows-hardware/test/wpt/wpr-command-line-options) を参照する。

Sysinternals の executable 名は配布形式によって 64-bit suffix の有無が異なる。
以降の手順では、doctor が検出する候補と同じ順序で executable を解決する。

```powershell
$procDumpExe = (Get-Command procdump64.exe, procdump.exe -ErrorAction Stop |
  Select-Object -First 1).Source
$procmonExe = (Get-Command procmon64.exe, procmon.exe -ErrorAction Stop |
  Select-Object -First 1).Source
$handleExe = (Get-Command handle64.exe, handle.exe -ErrorAction Stop |
  Select-Object -First 1).Source
```

## 成果物の保存先

dump、ETL、PML、debugger log には、変換中の本文、候補、ローカルパス、環境変数、API key が含まれる可能性がある。
採取先はリポジトリ外の一時ディレクトリに固定し、issue や PR へ添付する前に内容を確認する。

```powershell
$captureRoot = Join-Path $env:TEMP (
  'azookey-diagnostics-{0:yyyyMMdd-HHmmss}' -f (Get-Date)
)
New-Item -ItemType Directory -Path $captureRoot | Out-Null
```

リポジトリは `*.dmp`、`*.mdmp`、`*.etl`、`*.pml` と `diagnostics/captures/` を ignore する。
解析結果にローカルの絶対パスやユーザー入力本文が残っている場合も commit しない。

## 対象プロセスの確認

TIP の DLL は入力先アプリのプロセスへ読み込まれる。
そのため、TIP の問題は再現に使う入力先アプリを debugger または verifier の対象にし、`azookey_tsf_tip.dll` 自体を対象名にしない。

```powershell
Get-Process |
  Where-Object {
    $_.ProcessName -in @('azookey_inference_host', 'notepad')
  } |
  Select-Object Id, ProcessName, Path
```

Host の問題は `azookey_inference_host.exe` を対象にする。
Named Pipe の調査では TIP を読み込んだ入力先アプリと Host の両方を記録する。

## Crash の再現と採取

ProcDump を待機させてから、開発用ビルドで crash を再現する。
`-mp` は private memory を含むため、生成した dump は機密情報を含むものとして扱う。

```powershell
$procDump = Start-Process -FilePath $procDumpExe -PassThru -ArgumentList @(
  '-accepteula',
  '-mp',
  '-e',
  '-w',
  'azookey_inference_host.exe',
  $captureRoot
)
```

再現後は ProcDump を終了し、dump を CDB で解析する。

```powershell
if (-not $procDump.HasExited) {
  Stop-Process -Id $procDump.Id
}

$dump = Get-ChildItem -LiteralPath $captureRoot -Filter '*.dmp' |
  Sort-Object LastWriteTime -Descending |
  Select-Object -First 1
cdb.exe -z $dump.FullName -c '!analyze -v; ~* kb; lm; q' |
  Out-File (Join-Path $captureRoot 'crash-analysis.txt')
```

解析では exception code、faulting thread、最初の azooKey frame、読み込まれた module の build を確認する。
原因を確定できない場合は、推測で faulting module を決めず、再現条件と採取時点を記録する。

## Hang の再現と採取

ウィンドウを持つ入力先アプリの hang は ProcDump の `-h` trigger を使える。
Hidden 起動の Host や window message hang ではない deadlock は、再現中の PID を指定して手動 dump を採取する。

```powershell
$hostProcess = Get-Process azookey_inference_host -ErrorAction Stop |
  Select-Object -First 1
& $procDumpExe -accepteula -mp $hostProcess.Id $captureRoot
```

入力先アプリが UI message に応答しない場合は、次のように待機する。

```powershell
& $procDumpExe -accepteula -mp -h -w notepad.exe $captureRoot
```

CDB では全 thread の stack、待機 object、lock 所有者を確認する。

```powershell
$dump = Get-ChildItem -LiteralPath $captureRoot -Filter '*.dmp' |
  Sort-Object LastWriteTime -Descending |
  Select-Object -First 1
cdb.exe -z $dump.FullName -c '~* kb; !locks; !handle 0 3; q' |
  Out-File (Join-Path $captureRoot 'hang-analysis.txt')
```

採取後は待機中の ProcDump を終了し、検証用アプリと Host を通常の手順で再起動する。

## IPC timeout の再現と採取

WPR profile と Process Monitor を再現直前に開始し、再現直後に停止する。
無制限の file-mode trace と PML はディスクを使い続けるため、採取時間を短く保つ。
Kernel provider を使う WPR profile の開始は、管理者 PowerShell で実行する。

```powershell
$wprProfile = Join-Path $PWD 'diagnostics\azookey-diagnostics.wprp'
wpr.exe -start "${wprProfile}!AzooKeyDiagnostics.Verbose" -filemode

$procmon = Start-Process -FilePath $procmonExe -PassThru -ArgumentList @(
  '-accepteula',
  '-backingfile',
  (Join-Path $captureRoot 'ipc-timeout.pml'),
  '-quiet',
  '-minimized'
)
```

timeout を一度再現したら、先に WPR を保存し、Process Monitor を終了する。

```powershell
wpr.exe -stop (Join-Path $captureRoot 'ipc-timeout.etl') 'azooKey IPC timeout'
& $procmonExe -terminate -quiet
if (-not $procmon.HasExited) {
  $procmon.WaitForExit(10000)
}
```

WPA では Process と Thread activity、File I/O、Registry、Networking の順で時系列を確認する。
Process Monitor は採取後に `azookey_inference_host.exe` と入力先アプリで filter し、Named Pipe の connect、read、write、disconnect 周辺を確認する。

現在の profile は OS provider だけを収集する。
`docs/sideload-packaging-spec.md` §7.2 の azooKey 固有 provider は GUID が未確定であり、phase event の実装は DEV-604 の Work packet B で扱う。
固有 provider の導入後は、WPA の Generic Events で `request_id` を軸に `IpcRequest`、`IpcResponse`、`InferenceStart`、`InferenceEnd` を対応づける。

WPR の停止に失敗した場合は、保存を繰り返さずセッションを cancel する。

```powershell
wpr.exe -cancel
```

## COM と handle leak の再現と採取

Handle の集計を再現前後で取得し、増え続ける handle type と process を特定する。
`handle -c` はアプリや OS を不安定にするため、この手順では使わない。

```powershell
& $handleExe -s -p azookey_inference_host |
  Out-File (Join-Path $captureRoot 'handles-before.txt')

# 再現操作を一定回数だけ実行する。

& $handleExe -s -p azookey_inference_host |
  Out-File (Join-Path $captureRoot 'handles-after.txt')
```

Application Verifier は設定を registry に保持する。
有効化した test は `finally` で必ず解除し、再現対象を開発用 executable に限定する。

```powershell
$target = 'azookey_inference_host.exe'
try {
  appverif.exe -enable Heaps Handles Locks -for $target
  # debugger を接続して対象の再現テストを実行する。
} finally {
  appverif.exe -disable '*' -for $target
}
```

TIP の COM または handle leak は、専用の検証 executable に TIP を読み込ませて同じ手順を使う。
日常利用中のブラウザ、エディタ、Explorer に Application Verifier を設定しない。

Page heap が必要な場合は GFlags を対象 executable だけへ設定し、debugger を接続して再現する。

```powershell
$target = 'azookey_inference_host.exe'
try {
  gflags.exe /p /enable $target /full
  # 対象を再起動し、debugger 接続下で再現する。
} finally {
  gflags.exe /p /disable $target
}
```

Application Verifier の基本操作と解除方法は [Application Verifier testing applications](https://learn.microsoft.com/windows-hardware/drivers/devtest/application-verifier-testing-applications) を参照する。
Handle の `-c` による強制 close の警告は [Handle usage](https://learn.microsoft.com/sysinternals/downloads/handle#usage) を参照する。

## 後始末

診断を終える前に、WPR、ProcDump、Process Monitor の採取 process が残っていないことを確認する。
Application Verifier と GFlags の対象設定も解除する。

```powershell
wpr.exe -status
Get-Process procdump*, procmon* -ErrorAction SilentlyContinue
appverif.exe -query '*' -for azookey_inference_host.exe
gflags.exe /p
```

必要な解析結果だけを Linear へ記録し、dump、ETL、PML、ユーザー入力本文、ローカル絶対パスは添付しない。
成果物を共有する必要がある場合は、共有先、保持期間、redaction の確認結果を Linear のコメントへ残す。
