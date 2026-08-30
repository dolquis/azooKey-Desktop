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
    -Restore : 指定チェックポイントへ復元する。

  チェックポイント名はパッケージの manifest.json（commit / preset）から決定的に
  生成するため、同じパッケージに対する再実行は常に同じ名前を指す。

  VMConnect の基本セッションへの切り替えは対話操作のため自動化しない。
  -Prepare の完了時に、IME 検証は基本セッションで行うことを出力で明示する。
#>
param(
  [switch]$Prepare,
  [switch]$Restore,
  [string]$VMName = "",
  [string]$PackagePath = "",
  [string]$CheckpointName = "",
  [string]$GuestDestination = "C:\azookey-verify"
)

$ErrorActionPreference = "Stop"

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

function Get-VmVerifySessionGuestServiceInterface {
  param(
    [Parameter(Mandatory = $true)]
    [string]$VMName
  )

  return Get-VMIntegrationService -VMName $VMName -Name "Guest Service Interface" -ErrorAction Stop
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

  throw (@(
    "Guest Service Interface is not enabled on '$VMName', so Copy-VMFile cannot transfer the package."
    "Enable it on the host and retry:"
    "  Enable-VMIntegrationService -VMName '$VMName' -Name 'Guest Service Interface'"
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
  Write-Host "Next: expand the archive in the guest and run verify-bootstrap.ps1."
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

if ($MyInvocation.InvocationName -ne ".") {
  if ($Prepare -eq $Restore) {
    throw "Specify exactly one of -Prepare or -Restore."
  }
  if (-not $VMName) {
    throw "-VMName is required. Check the name with Get-VM."
  }

  $repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

  if ($Prepare) {
    Invoke-VmVerifySessionPrepare `
      -RepositoryRoot $repositoryRoot `
      -VMName $VMName `
      -PackagePath $PackagePath `
      -CheckpointName $CheckpointName `
      -GuestDestination $GuestDestination | Out-Null
  } else {
    Invoke-VmVerifySessionRestore `
      -RepositoryRoot $repositoryRoot `
      -VMName $VMName `
      -PackagePath $PackagePath `
      -CheckpointName $CheckpointName | Out-Null
  }
}
