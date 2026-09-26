#requires -Version 5.1
<#
.SYNOPSIS
  Hyper-V ホスト側の検証セッション操作（チェックポイント取得・パッケージ転送・復元）を
  1 コマンドへまとめる。

.DESCRIPTION
  make-vm-verify-package.ps1 が生成した検証パッケージを前提に、
  docs/handoff/hyper-v-tip-verification.md がホスト側の手作業として規定していた
  次を自動化する。

    -Prepare : 検証前チェックポイントを取得し、zip を VM へ転送する。
    -Run     : PowerShell Direct でゲスト内の bootstrap を実行し、対話ユーザーの
               スケジュールタスク経由で compat_test.exe を実行して、成果物をホストへ
               回収する（docs/handoff/hyper-v-vm-verification-plan.md §5 の L1）。
    -Restore : 指定チェックポイントへ復元する。

  -Run の -CompatCases / -CompatSkip は compat_test.exe の --cases / --skip へ渡す
  case ID（C-001 形式）。C-010 は Host を停止するため、手動の打鍵ゲートより前に
  -CompatSkip C-010 で回し、ゲートの後に -CompatCases C-010 で回せる。

  チェックポイント名はパッケージの manifest.json（commit / preset）から決定的に
  生成するため、同じパッケージに対する再実行は常に同じ名前を指す。

  VMConnect の基本セッションへの切り替えは対話操作のため自動化しない。
  -Prepare の完了時に、IME 検証は基本セッションで行うことを出力で明示する。

  -Run のゲスト資格情報は -Credential（PSCredential。SecretManagement に
  PSCredential として保管した secret なら Get-Secret の戻り値をそのまま渡せる）か、
  省略時の Get-Credential で受け取る。
  平文のパスワードを引数に取らず、ログにも書かない。
#>
param(
  [switch]$Prepare,
  [switch]$Restore,
  [switch]$Run,
  [string]$VMName = "",
  [string]$PackagePath = "",
  [string]$CheckpointName = "",
  [string]$GuestDestination = "C:\azookey-verify",
  [System.Management.Automation.PSCredential]$Credential = $null,
  [string]$ResultsDirectory = "",
  [ValidateRange(1, 240)]
  [int]$TimeoutMinutes = 45,
  [string[]]$CompatCases = @(),
  [string[]]$CompatSkip = @()
)

$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "vm-verify-guest.ps1")

function Get-VmVerifySessionAbsolutePath {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path
  )

  return $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path)
}

# 明示指定が無ければ、既定の出力ディレクトリから最新のパッケージを選ぶ。
# zip と manifest は make-vm-verify-package.ps1 が同じ baseName で並べて置く。
function Resolve-VmVerifySessionPackage {
  param(
    [Parameter(Mandatory = $true)]
    [string]$RepositoryRoot,
    [string]$PackagePath = ""
  )

  if ($PackagePath) {
    $zipPath = Get-VmVerifySessionAbsolutePath -Path $PackagePath
    if (-not (Test-Path -LiteralPath $zipPath -PathType Leaf)) {
      throw "VM verification package was not found: $zipPath"
    }
  } else {
    $searchRoot = Join-Path $RepositoryRoot "build\vm-verify-packages"
    if (-not (Test-Path -LiteralPath $searchRoot -PathType Container)) {
      throw ("No VM verification package directory at $searchRoot. " +
        "Run scripts\make-vm-verify-package.ps1 first, or pass -PackagePath.")
    }

    $candidate = Get-ChildItem -LiteralPath $searchRoot -Filter "azookey-verify-*.zip" -File |
      Sort-Object LastWriteTimeUtc -Descending |
      Select-Object -First 1
    if (-not $candidate) {
      throw ("No VM verification package under $searchRoot. " +
        "Run scripts\make-vm-verify-package.ps1 first, or pass -PackagePath.")
    }
    $zipPath = $candidate.FullName
  }

  $baseName = [System.IO.Path]::GetFileNameWithoutExtension($zipPath)
  $manifestPath = Join-Path ([System.IO.Path]::GetDirectoryName($zipPath)) "$baseName.manifest.json"
  if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    throw ("Package manifest was not found next to the archive: $manifestPath. " +
      "The checkpoint name is derived from the manifest, so the package cannot be used without it.")
  }

  return [pscustomobject][ordered]@{
    ZipPath = $zipPath
    ManifestPath = $manifestPath
  }
}

function Get-VmVerifySessionManifest {
  param(
    [Parameter(Mandatory = $true)]
    [string]$ManifestPath
  )

  $manifest = Get-Content -Raw -LiteralPath $ManifestPath | ConvertFrom-Json
  if (-not $manifest.commit -or $manifest.commit -notmatch '^[0-9a-f]{40}$') {
    throw "Package manifest does not carry a full commit hash: $ManifestPath"
  }
  if (-not $manifest.preset) {
    throw "Package manifest does not carry a preset name: $ManifestPath"
  }

  return $manifest
}

# commit / preset から決定的に生成する。同一パッケージでの再実行が同名を指し、
# 別コミット・別 preset の検証と取り違えない。
function Get-VmVerifySessionCheckpointName {
  param(
    [Parameter(Mandatory = $true)]
    $Manifest
  )

  $shortCommit = $Manifest.commit.Substring(0, 12)
  return "pre-azookey-$($Manifest.preset)-$shortCommit"
}

# 以降は Hyper-V コマンドレットの薄いラッパ。Hyper-V モジュールが無い環境でも
# Pester から差し替えられるよう、呼び出しを 1 箇所ずつ関数へ閉じ込める。
function Get-VmVerifySessionVM {
  param(
    [Parameter(Mandatory = $true)]
    [string]$VMName
  )

  return Get-VM -Name $VMName -ErrorAction Stop
}

# Guest Service Interface の component ID。表示名（Name）はホストの言語で
# ローカライズされ、日本語 Windows では「ゲスト サービス インターフェイス」に
# なる。表示名で引くと該当環境で必ず取り違えるため、言語非依存の ID で選ぶ。
function Get-VmVerifySessionGuestServiceInterface {
  param(
    [Parameter(Mandatory = $true)]
    [string]$VMName
  )

  return Get-VMIntegrationService -VMName $VMName -ErrorAction Stop |
    Where-Object { $_.Id -like "*6C09BB55-D683-4DA0-8931-C9BF705F6480" } |
    Select-Object -First 1
}

function Get-VmVerifySessionCheckpoint {
  param(
    [Parameter(Mandatory = $true)]
    [string]$VMName,
    [Parameter(Mandatory = $true)]
    [string]$CheckpointName
  )

  return Get-VMSnapshot -VMName $VMName -Name $CheckpointName -ErrorAction SilentlyContinue
}

function Invoke-VmVerifySessionCheckpoint {
  param(
    [Parameter(Mandatory = $true)]
    [string]$VMName,
    [Parameter(Mandatory = $true)]
    [string]$CheckpointName
  )

  Checkpoint-VM -Name $VMName -SnapshotName $CheckpointName -ErrorAction Stop
}

function Invoke-VmVerifySessionFileCopy {
  param(
    [Parameter(Mandatory = $true)]
    [string]$VMName,
    [Parameter(Mandatory = $true)]
    [string]$SourcePath,
    [Parameter(Mandatory = $true)]
    [string]$DestinationPath
  )

  Copy-VMFile -Name $VMName -SourcePath $SourcePath -DestinationPath $DestinationPath `
    -FileSource Host -CreateFullPath -Force -ErrorAction Stop
}

function Invoke-VmVerifySessionCheckpointRestore {
  param(
    [Parameter(Mandatory = $true)]
    [string]$VMName,
    [Parameter(Mandatory = $true)]
    [string]$CheckpointName
  )

  Restore-VMSnapshot -VMName $VMName -Name $CheckpointName -Confirm:$false -ErrorAction Stop
}

function Assert-VmVerifySessionVMRunning {
  param(
    [Parameter(Mandatory = $true)]
    [string]$VMName
  )

  try {
    $vm = Get-VmVerifySessionVM -VMName $VMName
  } catch {
    throw ("Hyper-V virtual machine '$VMName' was not found on this host. " +
      "Check the name with Get-VM. Original error: $($_.Exception.Message)")
  }
  if (-not $vm) {
    throw "Hyper-V virtual machine '$VMName' was not found on this host. Check the name with Get-VM."
  }
  if ($vm.State -ne "Running") {
    throw ("Hyper-V virtual machine '$VMName' is '$($vm.State)', but it must be Running " +
      "to take a checkpoint and copy the package. Start it with Start-VM -Name '$VMName'.")
  }

  return $vm
}

# Copy-VMFile は Guest Service Interface が有効なときだけ使える。
# 使えない場合は代替手段を案内して非ゼロ終了する。黙って転送をスキップしない。
function Assert-VmVerifySessionGuestService {
  param(
    [Parameter(Mandatory = $true)]
    [string]$VMName
  )

  $service = $null
  try {
    $service = Get-VmVerifySessionGuestServiceInterface -VMName $VMName
  } catch {
    $service = $null
  }

  if ($service -and $service.Enabled) {
    return
  }

  # 有効化コマンドも表示名で書かない。ローカライズされたホストでは同じ理由で
  # 失敗し、案内どおりに実行しても解決しないため。
  throw (@(
    "Guest Service Interface is not enabled on '$VMName', so Copy-VMFile cannot transfer the package."
    "Enable it on the host and retry (the display name is localized, so select by component ID):"
    "  Get-VMIntegrationService -VMName '$VMName' |"
    "    Where-Object { `$_.Id -like '*6C09BB55-D683-4DA0-8931-C9BF705F6480' } |"
    "    Enable-VMIntegrationService"
    "If the guest does not support it, copy the archive manually instead:"
    "  - open VMConnect in an enhanced session and copy the zip into the guest, or"
    "  - share a host folder with the guest and copy it from there."
    "The package was NOT transferred."
  ) -join [Environment]::NewLine)
}

function Invoke-VmVerifySessionPrepare {
  param(
    [Parameter(Mandatory = $true)]
    [string]$RepositoryRoot,
    [Parameter(Mandatory = $true)]
    [string]$VMName,
    [string]$PackagePath = "",
    [string]$CheckpointName = "",
    [Parameter(Mandatory = $true)]
    [string]$GuestDestination
  )

  $package = Resolve-VmVerifySessionPackage -RepositoryRoot $RepositoryRoot -PackagePath $PackagePath
  $manifest = Get-VmVerifySessionManifest -ManifestPath $package.ManifestPath
  $checkpoint = $CheckpointName
  if (-not $checkpoint) {
    $checkpoint = Get-VmVerifySessionCheckpointName -Manifest $manifest
  }

  Assert-VmVerifySessionVMRunning -VMName $VMName | Out-Null
  Assert-VmVerifySessionGuestService -VMName $VMName

  # 既存の同名チェックポイントは黙って上書きしない。前回の検証で VM が汚れている
  # 可能性があり、その状態を「検証前」として保存すると基準を失う。
  if (Get-VmVerifySessionCheckpoint -VMName $VMName -CheckpointName $checkpoint) {
    throw (@(
      "Checkpoint '$checkpoint' already exists on '$VMName'."
      "It was created for this same package, so the guest may already be dirty."
      "Restore it first (-Restore), or remove it with:"
      "  Remove-VMSnapshot -VMName '$VMName' -Name '$checkpoint'"
    ) -join [Environment]::NewLine)
  }

  Write-Host "Taking checkpoint '$checkpoint' on '$VMName'..."
  Invoke-VmVerifySessionCheckpoint -VMName $VMName -CheckpointName $checkpoint | Out-Null

  $destinationPath = Join-Path $GuestDestination ([System.IO.Path]::GetFileName($package.ZipPath))
  Write-Host "Copying $($package.ZipPath) to '$VMName':$destinationPath ..."
  Invoke-VmVerifySessionFileCopy `
    -VMName $VMName `
    -SourcePath $package.ZipPath `
    -DestinationPath $destinationPath | Out-Null

  Write-Host "Checkpoint: $checkpoint"
  Write-Host "Guest path: $destinationPath"
  Write-Host ""
  Write-Host "Next: expand the archive in the guest and run verify-bootstrap.ps1,"
  Write-Host "or run this script with -Run to execute bootstrap and compat_test.exe from the host."
  Write-Host "IME verification must run in a BASIC session. Enhanced sessions (VMConnect over RDP)"
  Write-Host "redirect input, so TIP behaviour cannot be judged there. Switch VMConnect to a basic"
  Write-Host "session before typing. This step is interactive and is not automated."

  return [pscustomobject][ordered]@{
    CheckpointName = $checkpoint
    ZipPath = $package.ZipPath
    GuestPath = $destinationPath
  }
}

function Invoke-VmVerifySessionRestore {
  param(
    [Parameter(Mandatory = $true)]
    [string]$RepositoryRoot,
    [Parameter(Mandatory = $true)]
    [string]$VMName,
    [string]$PackagePath = "",
    [string]$CheckpointName = ""
  )

  $checkpoint = $CheckpointName
  if (-not $checkpoint) {
    $package = Resolve-VmVerifySessionPackage -RepositoryRoot $RepositoryRoot -PackagePath $PackagePath
    $manifest = Get-VmVerifySessionManifest -ManifestPath $package.ManifestPath
    $checkpoint = Get-VmVerifySessionCheckpointName -Manifest $manifest
  }

  try {
    Get-VmVerifySessionVM -VMName $VMName | Out-Null
  } catch {
    throw ("Hyper-V virtual machine '$VMName' was not found on this host. " +
      "Check the name with Get-VM. Original error: $($_.Exception.Message)")
  }

  if (-not (Get-VmVerifySessionCheckpoint -VMName $VMName -CheckpointName $checkpoint)) {
    throw ("Checkpoint '$checkpoint' was not found on '$VMName'. " +
      "List the available checkpoints with Get-VMSnapshot -VMName '$VMName'.")
  }

  Write-Host "Restoring '$VMName' to checkpoint '$checkpoint'..."
  Invoke-VmVerifySessionCheckpointRestore -VMName $VMName -CheckpointName $checkpoint | Out-Null

  Write-Host "Restored: $checkpoint"

  return [pscustomobject][ordered]@{
    CheckpointName = $checkpoint
  }
}

# ---------------------------------------------------------------------------
# -Run のホスト側。PowerShell Direct の呼び出しは Pester から差し替えられるよう
# 1 箇所ずつ関数へ閉じ込める。
# ---------------------------------------------------------------------------

function Get-VmVerifySessionCredential {
  param(
    [Parameter(Mandatory = $true)]
    [string]$VMName
  )

  return Get-Credential -Message (
    "Local administrator of '$VMName' who is signed in to its console (basic session)")
}

function Open-VmVerifySessionGuestSession {
  param(
    [Parameter(Mandatory = $true)]
    [string]$VMName,
    [Parameter(Mandatory = $true)]
    [System.Management.Automation.PSCredential]$Credential
  )

  return New-PSSession -VMName $VMName -Credential $Credential -ErrorAction Stop
}

function Close-VmVerifySessionGuestSession {
  param(
    [Parameter(Mandatory = $true)]
    $Session
  )

  Remove-PSSession -Session $Session -ErrorAction SilentlyContinue
}

function Invoke-VmVerifySessionGuestStep {
  param(
    [Parameter(Mandatory = $true)]
    $Session,
    [Parameter(Mandatory = $true)]
    [string]$Name,
    [object[]]$ArgumentList = @()
  )

  $definition = (Get-Command -Name $Name -CommandType Function -ErrorAction Stop).ScriptBlock
  return Invoke-Command -Session $Session -ScriptBlock $definition `
    -ArgumentList $ArgumentList -ErrorAction Stop
}

function Copy-VmVerifySessionGuestArtifact {
  param(
    [Parameter(Mandatory = $true)]
    $Session,
    [Parameter(Mandatory = $true)]
    [string]$GuestPath,
    [Parameter(Mandatory = $true)]
    [string]$Destination
  )

  Copy-Item -FromSession $Session -Path $GuestPath -Destination $Destination `
    -Recurse -Force -ErrorAction Stop
}

function ConvertFrom-VmVerifySessionJson {
  param(
    [AllowEmptyString()]
    [string]$Text,
    [Parameter(Mandatory = $true)]
    [string]$Description
  )

  if (-not $Text -or -not $Text.Trim()) {
    throw "$Description produced no output."
  }
  try {
    return $Text | ConvertFrom-Json
  } catch {
    throw "$Description is not valid JSON: $($_.Exception.Message)"
  }
}

function Assert-VmVerifySessionInteractiveUser {
  param(
    [Parameter(Mandatory = $true)]
    $Info,
    [Parameter(Mandatory = $true)]
    [string]$VMName
  )

  if (-not $Info.IsAdministrator) {
    throw ("The PowerShell Direct account '$($Info.SessionUser)' is not a local administrator " +
      "of '$VMName'. verify-bootstrap.ps1 registers the TIP machine-wide and cannot show a UAC " +
      "prompt in a non-interactive session.")
  }
  if (-not $Info.ConsoleUser) {
    throw (@(
      "Nobody is signed in to the console of '$VMName', so compat_test.exe has no interactive desktop."
      "Sign in through VMConnect in a BASIC session (an enhanced session is RDP and does not count),"
      "or configure auto sign-in on the verification VM."
    ) -join [Environment]::NewLine)
  }
  if ($Info.ConsoleUser -ne $Info.SessionUser) {
    throw ("The console user '$($Info.ConsoleUser)' differs from the PowerShell Direct account " +
      "'$($Info.SessionUser)'. The inference host pipe and its auto-start are per user; " +
      "pass the credentials of the console user.")
  }
  if ($Info.Locked) {
    throw ("The console of '$VMName' is locked or at the sign-in screen. UI Automation and " +
      "SendInput fail on a locked desktop; unlock it and disable the lock screen on the verification VM.")
  }
}

# "C-001,C-004" と @("C-001", "C-004") のどちらも受け、compat_test.exe へ渡す
# カンマ区切りへ正規化する。値はタスクのコマンド文字列へ埋め込むため、
# case ID の形式以外はここで拒否する。
function ConvertTo-VmVerifySessionCaseList {
  param(
    [string[]]$Value = @(),
    [Parameter(Mandatory = $true)]
    [string]$ParameterName
  )

  $ids = @()
  foreach ($item in @($Value)) {
    foreach ($id in ([string]$item).Split(",")) {
      $id = $id.Trim()
      if ($id -cnotmatch '^C-[0-9]{3}$') {
        throw "-$ParameterName takes case IDs such as C-001 or C-001,C-004; '$id' is not one."
      }
      if ($ids -contains $id) {
        throw "-$ParameterName lists $id more than once."
      }
      $ids += $id
    }
  }
  return ($ids -join ",")
}

function Get-VmVerifySessionEncodedArgument {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Command
  )

  $encoded = [Convert]::ToBase64String([System.Text.Encoding]::Unicode.GetBytes($Command))
  return "-NoLogo -NoProfile -NonInteractive -WindowStyle Hidden -ExecutionPolicy Bypass -EncodedCommand $encoded"
}

function Get-VmVerifySessionRunnerScript {
  $definitions = foreach ($name in @(
      "Invoke-VmVerifyGuestBootstrap",
      "Invoke-VmVerifyGuestInputMethodSelection",
      "Invoke-VmVerifyGuestCompatRun")) {
    $body = (Get-Command -Name $name -CommandType Function -ErrorAction Stop).ScriptBlock.ToString()
    "function $name {$body}"
  }
  return $definitions -join [Environment]::NewLine
}

function Get-VmVerifySessionTargetSummary {
  param(
    [Parameter(Mandatory = $true)]
    $Status
  )

  # compat_test.exe の終了コード: 0 = 全件 pass、1 = fail を含む、
  # 2 = fail は無いが failing-skip を含む（compat-test/README.md）。
  foreach ($target in @($Status.targets)) {
    $outcome = switch ([int]$target.exitCode) {
      0 { "pass" }
      1 { "fail" }
      2 { "failing-skip" }
      default { "error" }
    }
    if ($outcome -ne "error" -and -not $target.reportJson) {
      $outcome = "error"
    }
    [pscustomobject][ordered]@{
      Target = [string]$target.target
      ExitCode = [int]$target.exitCode
      Outcome = $outcome
    }
  }
}

function Invoke-VmVerifySessionGuestRun {
  param(
    [Parameter(Mandatory = $true)]
    $Session,
    [Parameter(Mandatory = $true)]
    [string]$VMName,
    [Parameter(Mandatory = $true)]
    $Paths,
    [Parameter(Mandatory = $true)]
    [int]$TimeoutSeconds,
    [string]$Cases = "",
    [string]$Skip = ""
  )

  $targets = @(Invoke-VmVerifySessionGuestStep -Session $Session -Name "Initialize-VmVerifyGuestPackage" `
      -ArgumentList @($Paths.GuestZip, $Paths.GuestPackageRoot, $Paths.GuestRunRoot))
  $user = Invoke-VmVerifySessionGuestStep -Session $Session -Name "Get-VmVerifyGuestInteractiveUser"
  Assert-VmVerifySessionInteractiveUser -Info $user -VMName $VMName

  Write-Host "Running verify-bootstrap.ps1 -Json in '$VMName' over PowerShell Direct..."
  $directPath = Join-Path $Paths.GuestRunRoot "bootstrap-direct.json"
  $directExitCode = Invoke-VmVerifySessionGuestStep -Session $Session -Name "Invoke-VmVerifyGuestBootstrap" `
    -ArgumentList @($Paths.GuestPackageRoot, $directPath, $true)
  $directText = Invoke-VmVerifySessionGuestStep -Session $Session -Name "Read-VmVerifyGuestText" `
    -ArgumentList @($directPath)
  $direct = ConvertFrom-VmVerifySessionJson -Text $directText `
    -Description ("verify-bootstrap.ps1 -Json over PowerShell Direct (exit $directExitCode; " +
      "see bootstrap-direct.error.log in the artifacts)")
  Write-Host "Bootstrap over PowerShell Direct: $($direct.overallStatus)"
  # 層 1 の bootstrap が fail なら compat へ進まない（plan §3）。
  if ($direct.overallStatus -eq "fail") {
    $failed = @($direct.checks | Where-Object { $_.status -eq "fail" } |
        ForEach-Object { "$($_.id): $($_.message)" })
    throw ("verify-bootstrap.ps1 reported overallStatus=fail over PowerShell Direct, " +
      "so compat_test.exe was not run. " + ($failed -join "; "))
  }

  $hostState = Invoke-VmVerifySessionGuestStep -Session $Session `
    -Name "Invoke-VmVerifyGuestSessionZeroHostShutdown" -ArgumentList @($Paths.GuestPackageRoot)
  Write-Host "Session 0 inference host shutdown: $hostState"

  $runnerPath = Join-Path $Paths.GuestRunRoot "interactive-runner.ps1"
  Invoke-VmVerifySessionGuestStep -Session $Session -Name "Write-VmVerifyGuestRunner" `
    -ArgumentList @($runnerPath, (Get-VmVerifySessionRunnerScript)) | Out-Null
  $command = ". '" + $runnerPath.Replace("'", "''") + "'; Invoke-VmVerifyGuestCompatRun" +
    " -PackageRoot '" + $Paths.GuestPackageRoot.Replace("'", "''") + "'" +
    " -RunRoot '" + $Paths.GuestRunRoot.Replace("'", "''") + "'"
  if ($Cases) {
    $command += " -Cases '$Cases'"
  }
  if ($Skip) {
    $command += " -Skip '$Skip'"
  }
  $taskName = "azooKey-vm-verify-" + (Split-Path -Leaf $Paths.GuestRunRoot)

  Write-Host ("Running compat_test.exe ($($targets -join ', ')) as '$($user.ConsoleUser)' " +
    "through an interactive scheduled task...")
  if ($Cases -or $Skip) {
    Write-Host "Compat case selection: cases=$(if ($Cases) { $Cases } else { 'all' }) skip=$(if ($Skip) { $Skip } else { 'none' })"
  }
  $taskExitCode = Invoke-VmVerifySessionGuestStep -Session $Session -Name "Invoke-VmVerifyGuestInteractiveTask" `
    -ArgumentList @($taskName, $user.ConsoleUser, (Get-VmVerifySessionEncodedArgument -Command $command), $TimeoutSeconds)
  $taskResult = "0x{0:X8}" -f [int64]$taskExitCode
  $statusText = Invoke-VmVerifySessionGuestStep -Session $Session -Name "Read-VmVerifyGuestText" `
    -ArgumentList @((Join-Path $Paths.GuestRunRoot "interactive-status.json"))
  $status = ConvertFrom-VmVerifySessionJson -Text $statusText `
    -Description "The interactive run status (task result $taskResult)"
  if (-not $status.completed) {
    throw "The interactive run did not complete (task result $taskResult): $($status.error)"
  }

  return [pscustomobject][ordered]@{
    DirectBootstrap = $direct
    InteractiveStatus = $status
    Targets = @(Get-VmVerifySessionTargetSummary -Status $status)
  }
}

function Receive-VmVerifySessionArtifact {
  param(
    [Parameter(Mandatory = $true)]
    $Session,
    [Parameter(Mandatory = $true)]
    [string]$GuestRunRoot,
    [Parameter(Mandatory = $true)]
    [string]$HostRunRoot
  )

  # 実行ディレクトリを作る前に失敗した場合は回収対象が無い。回収失敗と区別し、
  # 元の失敗理由を二次エラーで埋もれさせない。azooKey ログの複製は付随作業なので、
  # 失敗しても警告に留め、bootstrap の JSON と compat の report は回収を続ける。
  $logState = ""
  try {
    $logState = Invoke-VmVerifySessionGuestStep -Session $Session -Name "Copy-VmVerifyGuestLog" `
      -ArgumentList @($GuestRunRoot)
  } catch {
    Write-Warning "azooKey logs could not be copied into the run directory: $($_.Exception.Message)"
  }
  if ($logState -eq "no-run-root") {
    return [pscustomobject]@{ Collected = $false; Error = "" }
  }
  try {
    New-Item -ItemType Directory -Path (Split-Path -Parent $HostRunRoot) -Force | Out-Null
    Copy-VmVerifySessionGuestArtifact -Session $Session -GuestPath $GuestRunRoot -Destination $HostRunRoot
    return [pscustomobject]@{ Collected = $true; Error = "" }
  } catch {
    return [pscustomobject]@{ Collected = $false; Error = $_.Exception.Message }
  }
}

function Invoke-VmVerifySessionRun {
  param(
    [Parameter(Mandatory = $true)]
    [string]$RepositoryRoot,
    [Parameter(Mandatory = $true)]
    [string]$VMName,
    [string]$PackagePath = "",
    [string]$CheckpointName = "",
    [Parameter(Mandatory = $true)]
    [string]$GuestDestination,
    [System.Management.Automation.PSCredential]$Credential = $null,
    [string]$ResultsDirectory = "",
    [int]$TimeoutMinutes = 45,
    [string[]]$CompatCases = @(),
    [string[]]$CompatSkip = @()
  )

  $cases = ConvertTo-VmVerifySessionCaseList -Value $CompatCases -ParameterName "CompatCases"
  $skip = ConvertTo-VmVerifySessionCaseList -Value $CompatSkip -ParameterName "CompatSkip"
  $package = Resolve-VmVerifySessionPackage -RepositoryRoot $RepositoryRoot -PackagePath $PackagePath
  $manifest = Get-VmVerifySessionManifest -ManifestPath $package.ManifestPath
  $checkpoint = $CheckpointName
  if (-not $checkpoint) {
    $checkpoint = Get-VmVerifySessionCheckpointName -Manifest $manifest
  }

  Assert-VmVerifySessionVMRunning -VMName $VMName | Out-Null
  # bootstrap は TIP を machine-wide に登録する。戻す基準の無い VM では実行しない。
  if (-not (Get-VmVerifySessionCheckpoint -VMName $VMName -CheckpointName $checkpoint)) {
    throw ("Checkpoint '$checkpoint' was not found on '$VMName'. Run -Prepare first: " +
      "-Run registers the TIP machine-wide and needs a checkpoint to restore afterwards.")
  }

  $baseName = [System.IO.Path]::GetFileNameWithoutExtension($package.ZipPath)
  $runName = "$baseName-" + [DateTime]::UtcNow.ToString("yyyyMMdd-HHmmss")
  $paths = [pscustomobject][ordered]@{
    GuestZip = Join-Path $GuestDestination "$baseName.zip"
    GuestPackageRoot = Join-Path $GuestDestination $baseName
    GuestRunRoot = Join-Path (Join-Path $GuestDestination "runs") $runName
  }
  if (-not $ResultsDirectory) {
    $ResultsDirectory = Join-Path $RepositoryRoot "build\vm-verify-results"
  }
  $hostRunRoot = Join-Path (Get-VmVerifySessionAbsolutePath -Path $ResultsDirectory) $runName

  if (-not $Credential) {
    try {
      $Credential = Get-VmVerifySessionCredential -VMName $VMName
    } catch {
      throw ("Guest credentials are required for PowerShell Direct. Pass -Credential " +
        "(for example -Credential (Get-Secret -Name <name>)) when no prompt is available. " +
        "Original error: $($_.Exception.Message)")
    }
    if (-not $Credential) {
      throw "Guest credentials were not provided, so PowerShell Direct cannot be used."
    }
  }

  try {
    $session = Open-VmVerifySessionGuestSession -VMName $VMName -Credential $Credential
  } catch {
    throw ("Could not open a PowerShell Direct session to '$VMName'. Check the guest " +
      "credentials, that the guest has finished booting, and that the PowerShell Direct " +
      "integration service is running. Original error: $($_.Exception.Message)")
  }

  $outcome = $null
  $failures = @()
  try {
    $outcome = Invoke-VmVerifySessionGuestRun -Session $session -VMName $VMName `
      -Paths $paths -TimeoutSeconds ($TimeoutMinutes * 60) -Cases $cases -Skip $skip
  } catch {
    $failures += $_.Exception.Message
  } finally {
    $collection = Receive-VmVerifySessionArtifact -Session $session `
      -GuestRunRoot $paths.GuestRunRoot -HostRunRoot $hostRunRoot
    Close-VmVerifySessionGuestSession -Session $session
  }

  if ($collection.Error) {
    $failures += "Artifacts could not be collected from '$VMName':$($paths.GuestRunRoot): $($collection.Error)"
  } elseif ($collection.Collected) {
    Write-Host "Artifacts: $hostRunRoot"
  } else {
    Write-Host "No artifacts were produced: the guest run directory was not created."
  }
  if ($outcome) {
    foreach ($target in $outcome.Targets) {
      Write-Host "compat_test.exe $($target.Target): $($target.Outcome) (exit $($target.ExitCode))"
      if ($target.Outcome -in @("fail", "error")) {
        $failures += "compat_test.exe target '$($target.Target)' ended with $($target.Outcome) (exit $($target.ExitCode))."
      }
    }
    if (@($outcome.Targets).Count -eq 0) {
      $failures += "compat_test.exe ran no targets."
    }
  }
  if ($failures.Count -ne 0) {
    throw ((@("L1 guest verification did not pass on '$VMName'.") + $failures) -join [Environment]::NewLine)
  }

  Write-Host ("L1 guest verification finished without fail. Review failing-skip cases in each report.md; " +
    "this result does not replace the basic-session checks or the human gate.")
  return [pscustomobject][ordered]@{
    CheckpointName = $checkpoint
    ResultsPath = $hostRunRoot
    Targets = $outcome.Targets
  }
}

if ($MyInvocation.InvocationName -ne ".") {
  if (@($Prepare, $Restore, $Run | Where-Object { $_ }).Count -ne 1) {
    throw "Specify exactly one of -Prepare, -Restore, or -Run."
  }
  if (-not $VMName) {
    throw "-VMName is required. Check the name with Get-VM."
  }
  if (-not $Run -and (@($CompatCases).Count -ne 0 -or @($CompatSkip).Count -ne 0)) {
    throw "-CompatCases and -CompatSkip apply only to -Run."
  }

  $repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

  if ($Prepare) {
    Invoke-VmVerifySessionPrepare `
      -RepositoryRoot $repositoryRoot `
      -VMName $VMName `
      -PackagePath $PackagePath `
      -CheckpointName $CheckpointName `
      -GuestDestination $GuestDestination | Out-Null
  } elseif ($Run) {
    Invoke-VmVerifySessionRun `
      -RepositoryRoot $repositoryRoot `
      -VMName $VMName `
      -PackagePath $PackagePath `
      -CheckpointName $CheckpointName `
      -GuestDestination $GuestDestination `
      -Credential $Credential `
      -ResultsDirectory $ResultsDirectory `
      -TimeoutMinutes $TimeoutMinutes `
      -CompatCases $CompatCases `
      -CompatSkip $CompatSkip | Out-Null
  } else {
    Invoke-VmVerifySessionRestore `
      -RepositoryRoot $repositoryRoot `
      -VMName $VMName `
      -PackagePath $PackagePath `
      -CheckpointName $CheckpointName | Out-Null
  }
}
