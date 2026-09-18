#requires -Version 5.1
<#
.SYNOPSIS
  vm-verify-session.ps1 -Run がゲスト内で実行する関数群。

.DESCRIPTION
  vm-verify-session.ps1 が dot-source し、各関数の本体を PowerShell Direct の
  Invoke-Command へ送る。対話タスク用のスクリプトにも同じ本体を書き出す。
  単体では何も実行しない。
#>

# ---------------------------------------------------------------------------
# -Run のゲスト側関数。Invoke-Command -ScriptBlock へ関数本体だけを送るため、
# 他の関数やスクリプト変数に依存させず、ゲストの Windows PowerShell 5.1 で動く
# 構文だけを使う。対話タスク用スクリプトにも同じ本体を書き出す。
# ---------------------------------------------------------------------------

function Initialize-VmVerifyGuestPackage {
  param(
    [Parameter(Mandatory = $true)]
    [string]$ZipPath,
    [Parameter(Mandatory = $true)]
    [string]$PackageRoot,
    [Parameter(Mandatory = $true)]
    [string]$RunRoot
  )

  if (-not (Test-Path -LiteralPath $ZipPath -PathType Leaf)) {
    throw ("The package archive is not in the guest: $ZipPath. " +
      "Run -Prepare first with the same -GuestDestination.")
  }
  # 展開先は zip 名から決まる固定パスにする。TIP DLL のパスが実行ごとに変わると
  # bootstrap が登録済みと判定できず、毎回再登録が走るため。zip 名は commit と
  # preset しか含まないので、同名で送り直された zip は hash で見分けて展開し直す。
  $zipHash = (Get-FileHash -LiteralPath $ZipPath -Algorithm SHA256).Hash
  $markerPath = Join-Path $PackageRoot ".azookey-verify-archive.sha256"
  $expandedHash = ""
  if (Test-Path -LiteralPath $markerPath -PathType Leaf) {
    $expandedHash = [string](Get-Content -Raw -LiteralPath $markerPath)
  }
  if ($expandedHash.Trim() -ne $zipHash) {
    if (Test-Path -LiteralPath $PackageRoot) {
      Remove-Item -LiteralPath $PackageRoot -Recurse -Force
    }
    Expand-Archive -LiteralPath $ZipPath -DestinationPath $PackageRoot -Force
    Set-Content -LiteralPath $markerPath -Value $zipHash -Encoding ASCII
  }

  $missing = @()
  foreach ($name in @("verify-bootstrap.ps1", "compat_test.exe")) {
    if (-not (Test-Path -LiteralPath (Join-Path $PackageRoot $name) -PathType Leaf)) {
      $missing += $name
    }
  }
  $targets = @(Get-ChildItem -LiteralPath (Join-Path $PackageRoot "targets") `
      -Filter "*.json" -File -ErrorAction SilentlyContinue | Sort-Object Name)
  if ($targets.Count -eq 0) {
    $missing += "targets\*.json"
  }
  if ($missing.Count -ne 0) {
    throw ("The package in the guest lacks $($missing -join ', '). " +
      "Rebuild it with make-vm-verify-package.ps1 -IncludeCompat.")
  }

  New-Item -ItemType Directory -Path $RunRoot -Force | Out-Null
  return @($targets | ForEach-Object { $_.BaseName })
}

function Get-VmVerifyGuestInteractiveUser {
  $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
  $principal = New-Object Security.Principal.WindowsPrincipal($identity)
  # Win32_ComputerSystem.UserName はコンソール（VMConnect の基本セッション）に
  # サインインしているユーザーを返す。拡張セッション（RDP）だけの場合は空になる。
  return [pscustomobject][ordered]@{
    ConsoleUser = [string](Get-CimInstance -ClassName Win32_ComputerSystem).UserName
    SessionUser = [string]$identity.Name
    IsAdministrator = [bool]$principal.IsInRole(
      [Security.Principal.WindowsBuiltInRole]::Administrator)
    Locked = [bool](Get-Process -Name "LogonUI" -ErrorAction SilentlyContinue)
  }
}

function Invoke-VmVerifyGuestBootstrap {
  param(
    [Parameter(Mandatory = $true)]
    [string]$PackageRoot,
    [Parameter(Mandatory = $true)]
    [string]$OutputPath,
    [bool]$CheckpointConfirmed = $false,
    [int]$TimeoutSeconds = 900
  )

  # 子プロセスにするのは、bootstrap の exit でこの呼び出し元まで終わらせないため。
  # -Json の出力は子プロセス自身がファイルへ書く。標準出力をリダイレクトすると、
  # bootstrap が起動する常駐 supervisor がそのハンドルを継承しうるため。
  # Write-Warning（stream 3）は JSON を壊すので別ファイルへ分ける。
  $quote = { param($value) "'" + ([string]$value).Replace("'", "''") + "'" }
  $bootstrap = Join-Path $PackageRoot "verify-bootstrap.ps1"
  $warningPath = [System.IO.Path]::ChangeExtension($OutputPath, ".warnings.log")
  $errorPath = [System.IO.Path]::ChangeExtension($OutputPath, ".error.log")
  $switches = " -Json"
  if ($CheckpointConfirmed) {
    $switches += " -CheckpointConfirmed"
  }
  $command = "`$ErrorActionPreference = 'Stop'; try { `$global:LASTEXITCODE = 0; " +
    "`$output = & $(& $quote $bootstrap)$switches 3> $(& $quote $warningPath); " +
    "`$code = `$LASTEXITCODE; " +
    "`$output | Set-Content -LiteralPath $(& $quote $OutputPath) -Encoding UTF8; exit `$code } " +
    "catch { `$_ | Out-String | Set-Content -LiteralPath $(& $quote $errorPath) -Encoding UTF8; exit 1 }"
  $encoded = [Convert]::ToBase64String([System.Text.Encoding]::Unicode.GetBytes($command))

  # Windows PowerShell 5.1 の Start-Process -Wait は子孫プロセスの終了まで待つため、
  # 常駐 supervisor が残ると戻らない。本体だけを WaitForExit で待つ。
  $process = Start-Process -FilePath "powershell.exe" `
    -ArgumentList "-NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand $encoded" `
    -WindowStyle Hidden -PassThru
  $null = $process.Handle
  if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
    $process.Kill()
    throw "verify-bootstrap.ps1 did not finish within $TimeoutSeconds seconds and was stopped."
  }
  return [int]$process.ExitCode
}

# PowerShell Direct のセッションは Session 0 にあり、そこで起動した Host には
# 対話セッションの TIP が接続できない。bootstrap の pipe 判定と supervisor の
# mutex はセッションを区別しないため、Session 0 の Host が残っていると対話側の
# bootstrap がそれを再利用してしまう。対話タスクの前に止めておく。
function Invoke-VmVerifyGuestSessionZeroHostShutdown {
  param(
    [Parameter(Mandatory = $true)]
    [string]$PackageRoot
  )

  Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force
  . (Join-Path $PackageRoot "verify-bootstrap.ps1")

  $pipeName = Get-VmVerifyPipeName
  if (-not (Test-VmVerifyPipe -PipeName $pipeName)) {
    # supervisor が Host の再起動を待っている間は pipe が無い。残したままにすると
    # 対話側の supervisor が mutex を取れず、Session 0 の Host が後から立ち上がる。
    Invoke-VmVerifyHostSupervisorShutdown
    if (-not (Wait-VmVerifyHostSupervisorStopped -TimeoutSeconds 10)) {
      throw "The inference host supervisor did not stop within the timeout."
    }
    return "not-serving"
  }
  $servingHost = Get-VmVerifyServingHostProcess -PipeName $pipeName
  $sessionId = (Get-Process -Id $servingHost.Id -ErrorAction Stop).SessionId
  if ($sessionId -ne 0) {
    return "interactive-session-$sessionId"
  }

  Invoke-VmVerifyHostSupervisorShutdown
  if (-not (Wait-VmVerifyHostSupervisorStopped -TimeoutSeconds 10)) {
    throw "The inference host supervisor in session 0 did not stop within the timeout."
  }
  Invoke-VmVerifyHostProcessTermination -ProcessId $servingHost.Id
  if (-not (Wait-VmVerifyPipe -PipeName $pipeName -TimeoutSeconds 15 -ExpectedPresent:$false)) {
    throw "The inference host pipe in session 0 did not disappear within the timeout."
  }
  return "stopped"
}

function Write-VmVerifyGuestRunner {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path,
    [Parameter(Mandatory = $true)]
    [string]$Content
  )

  Set-Content -LiteralPath $Path -Value $Content -Encoding UTF8
}

function Read-VmVerifyGuestText {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path
  )

  if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
    return ""
  }
  return [string](Get-Content -Raw -LiteralPath $Path)
}

function Copy-VmVerifyGuestLog {
  param(
    [Parameter(Mandatory = $true)]
    [string]$RunRoot
  )

  if (-not (Test-Path -LiteralPath $RunRoot -PathType Container)) {
    return "no-run-root"
  }
  $logDirectory = Join-Path (Join-Path $env:LOCALAPPDATA "azooKey") "logs"
  if (-not (Test-Path -LiteralPath $logDirectory -PathType Container)) {
    return "no-logs"
  }
  Copy-Item -LiteralPath $logDirectory -Destination (Join-Path $RunRoot "azookey-logs") `
    -Recurse -Force
  return "copied"
}

# compat_test.exe は UI Automation と SendInput を使うため対話デスクトップが要る。
# PowerShell Direct のセッションは非対話なので、コンソールにサインインしている
# ユーザーのスケジュールタスクとして実行する。LogonType Interactive はそのユーザーが
# サインイン中のときだけ動き、パスワードをタスクへ保存しない。RunLevel Limited に
# するのは、手動手順と同じ非昇格トークンで Host と対象アプリを起動するため。
function Invoke-VmVerifyGuestInteractiveTask {
  param(
    [Parameter(Mandatory = $true)]
    [string]$TaskName,
    [Parameter(Mandatory = $true)]
    [string]$UserName,
    [Parameter(Mandatory = $true)]
    [string]$Argument,
    [Parameter(Mandatory = $true)]
    [int]$TimeoutSeconds,
    [int]$StartTimeoutSeconds = 60,
    [int]$PollSeconds = 5
  )

  $action = New-ScheduledTaskAction -Execute "powershell.exe" -Argument $Argument
  $principal = New-ScheduledTaskPrincipal -UserId $UserName -LogonType Interactive -RunLevel Limited
  $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -ExecutionTimeLimit ([TimeSpan]::FromSeconds($TimeoutSeconds)) -MultipleInstances IgnoreNew
  try {
    Register-ScheduledTask -TaskName $TaskName -Action $action -Principal $principal `
      -Settings $settings -Force -ErrorAction Stop | Out-Null
  } catch {
    throw ("Could not register the interactive scheduled task '$TaskName' for '$UserName'. " +
      "Original error: $($_.Exception.Message)")
  }

  try {
    $previousRun = (Get-ScheduledTaskInfo -TaskName $TaskName).LastRunTime
    Start-ScheduledTask -TaskName $TaskName -ErrorAction Stop
    $started = $false
    $deadline = [DateTime]::UtcNow.AddSeconds($StartTimeoutSeconds)
    do {
      $state = [string](Get-ScheduledTask -TaskName $TaskName).State
      if ($state -eq "Running" -or
          (Get-ScheduledTaskInfo -TaskName $TaskName).LastRunTime -ne $previousRun) {
        $started = $true
        break
      }
      Start-Sleep -Seconds 1
    } while ([DateTime]::UtcNow -lt $deadline)
    if (-not $started) {
      throw ("The interactive scheduled task '$TaskName' did not start within " +
        "$StartTimeoutSeconds seconds. It only runs while '$UserName' is signed in to the console.")
    }

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([string](Get-ScheduledTask -TaskName $TaskName).State -eq "Running") {
      if ([DateTime]::UtcNow -ge $deadline) {
        Stop-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
        throw ("The interactive scheduled task '$TaskName' did not finish within " +
          "$TimeoutSeconds seconds and was stopped.")
      }
      Start-Sleep -Seconds $PollSeconds
    }
    # LastTaskResult は uint32。0x8xxxxxxx の HRESULT を [int] に変換すると例外になる。
    return [int64](Get-ScheduledTaskInfo -TaskName $TaskName).LastTaskResult
  } finally {
    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false -ErrorAction SilentlyContinue
  }
}

# 対話タスクが実行する本体。ゲストへは Invoke-VmVerifyGuestBootstrap と一緒に
# スクリプトファイルとして書き出す。失敗しても状態ファイルは必ず書く。
function Invoke-VmVerifyGuestCompatRun {
  param(
    [Parameter(Mandatory = $true)]
    [string]$PackageRoot,
    [Parameter(Mandatory = $true)]
    [string]$RunRoot
  )

  $ErrorActionPreference = "Stop"
  $sessionId = (Get-Process -Id $PID).SessionId
  $status = [ordered]@{
    schemaVersion = 1
    completed = $false
    sessionId = $sessionId
    bootstrapExitCode = $null
    bootstrapStatus = ""
    hostProcessId = 0
    hostSessionId = $null
    targets = @()
    error = ""
  }
  try {
    # 登録は PowerShell Direct 側で済んでいるため、ここでの再実行は UAC 昇格に
    # 入らず、Host をこの対話セッションで起動して検証するだけになる。
    $bootstrapPath = Join-Path $RunRoot "bootstrap-interactive.json"
    $status.bootstrapExitCode = Invoke-VmVerifyGuestBootstrap `
      -PackageRoot $PackageRoot -OutputPath $bootstrapPath -CheckpointConfirmed $true
    $bootstrap = Get-Content -Raw -LiteralPath $bootstrapPath | ConvertFrom-Json
    $status.bootstrapStatus = [string]$bootstrap.overallStatus
    if ($status.bootstrapStatus -eq "fail") {
      throw "verify-bootstrap.ps1 reported overallStatus=fail in the interactive session."
    }
    $status.hostProcessId = [int]$bootstrap.hostBinary.processId
    if ($status.hostProcessId -le 0) {
      throw ("verify-bootstrap.ps1 did not identify the serving inference host: " +
        [string]$bootstrap.hostBinary.reason)
    }
    $status.hostSessionId = (Get-Process -Id $status.hostProcessId -ErrorAction Stop).SessionId
    if ($status.hostSessionId -ne $sessionId) {
      throw ("The inference host runs in session $($status.hostSessionId), not in the " +
        "interactive session $sessionId, so the TIP cannot connect to it.")
    }

    $compat = Join-Path $PackageRoot "compat_test.exe"
    $targets = @(Get-ChildItem -LiteralPath (Join-Path $PackageRoot "targets") `
        -Filter "*.json" -File | Sort-Object Name)
    foreach ($target in $targets) {
      $name = $target.BaseName
      $reportDirectory = Join-Path $RunRoot "compat-report-$name"
      $process = Start-Process -FilePath $compat `
        -ArgumentList "--target `"$($target.FullName)`" --output `"$reportDirectory`"" `
        -RedirectStandardOutput (Join-Path $RunRoot "compat-$name.stdout.log") `
        -RedirectStandardError (Join-Path $RunRoot "compat-$name.stderr.log") `
        -NoNewWindow -PassThru
      # -Wait は compat_test.exe が起動した対象アプリ（Edge の常駐プロセスなど）の
      # 終了まで待ってしまう。本体だけを待ち、全体の上限はタスクの制限時間に任せる。
      $null = $process.Handle
      $process.WaitForExit()
      $status.targets += [pscustomobject][ordered]@{
        target = $name
        exitCode = [int]$process.ExitCode
        reportJson = [bool](Test-Path -LiteralPath (Join-Path $reportDirectory "report.json") -PathType Leaf)
      }
    }
    $status.completed = $true
  } catch {
    $status.error = $_.Exception.Message
  } finally {
    [pscustomobject]$status | ConvertTo-Json -Depth 4 |
      Set-Content -LiteralPath (Join-Path $RunRoot "interactive-status.json") -Encoding UTF8
  }
}
