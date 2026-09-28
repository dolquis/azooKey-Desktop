#requires -Version 5.1
<#
.SYNOPSIS
  Host-side handling when vm-verify-session.ps1 receives an MSI as -PackagePath.

.DESCRIPTION
  Puts lane 1 of human-gate-batch-runbook.md (an MSI installed into a clean VM) under
  the same identity tracking as the verification zip. vm-verify-session.ps1
  dot-sources this file and the functions reuse its thin Hyper-V and PowerShell
  Direct wrappers. Nothing runs on its own.

    -Prepare : records the MSI SHA-256, confirms that the VM state derives from the
               clean checkpoint, copies the MSI into the guest, and then takes the
               MSI-specific checkpoint.
    -Collect : matches the hash of the MSI in the guest and collects the msiexec /L*v
               logs to the host.
    -Restore : restores the MSI-specific checkpoint (MSI copied, not installed).

  A person runs msiexec (hyper-v-vm-verification-plan.md section 4.5 keeps MSI
  install and uninstall out of agent scope), so -Run does not accept an MSI.

  Keep this file ASCII-only. vm-verify-session.ps1 supports Windows PowerShell 5.1,
  which reads a UTF-8 file without BOM in the ANSI code page; multibyte comments
  there can swallow line breaks and silently drop the following code.
#>

function Test-VmVerifyMsiPackagePath {
  param(
    [AllowEmptyString()]
    [string]$PackagePath = ""
  )

  return [bool]($PackagePath -and [System.IO.Path]::GetExtension($PackagePath) -ieq ".msi")
}

# The MSI identity is its content hash. A file name may only carry the version, so
# the checkpoint and the guest directory also derive from the hash; a rebuilt MSI
# with the same name is never mistaken for the previous one.
function Get-VmVerifyMsiIdentity {
  param(
    [Parameter(Mandatory = $true)]
    [string]$PackagePath
  )

  $msiPath = Get-VmVerifySessionAbsolutePath -Path $PackagePath
  if (-not (Test-Path -LiteralPath $msiPath -PathType Leaf)) {
    throw "MSI was not found: $msiPath"
  }
  $sha256 = (Get-FileHash -LiteralPath $msiPath -Algorithm SHA256).Hash.ToLowerInvariant()
  $shortHash = $sha256.Substring(0, 12)
  $baseName = [System.IO.Path]::GetFileNameWithoutExtension($msiPath)

  return [pscustomobject][ordered]@{
    MsiPath = $msiPath
    FileName = [System.IO.Path]::GetFileName($msiPath)
    Size = [int64](Get-Item -LiteralPath $msiPath).Length
    Sha256 = $sha256
    CheckpointName = "pre-azookey-msi-$shortHash"
    RunName = "$baseName-$shortHash"
  }
}

function Get-VmVerifyMsiGuestPath {
  param(
    [Parameter(Mandatory = $true)]
    [string]$GuestDestination,
    [Parameter(Mandatory = $true)]
    $Identity
  )

  $directory = Join-Path $GuestDestination "msi-$($Identity.Sha256.Substring(0, 12))"
  return [pscustomobject][ordered]@{
    Directory = $directory
    Msi = Join-Path $directory $Identity.FileName
  }
}

function Write-VmVerifyMsiRecord {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path,
    [Parameter(Mandatory = $true)]
    $Record
  )

  New-Item -ItemType Directory -Path (Split-Path -Parent $Path) -Force | Out-Null
  [System.IO.File]::WriteAllText($Path, ($Record | ConvertTo-Json -Depth 4),
    (New-Object System.Text.UTF8Encoding($false)))
}

function Get-VmVerifyMsiResultsRoot {
  param(
    [Parameter(Mandatory = $true)]
    [string]$RepositoryRoot,
    [AllowEmptyString()]
    [string]$ResultsDirectory = "",
    [Parameter(Mandatory = $true)]
    $Identity
  )

  if (-not $ResultsDirectory) {
    $ResultsDirectory = Join-Path $RepositoryRoot "build\vm-verify-results"
  }
  return Join-Path (Get-VmVerifySessionAbsolutePath -Path $ResultsDirectory) $Identity.RunName
}

# Runs in the guest. Returns the MSI hash and the msiexec logs in the same directory.
function Get-VmVerifyGuestMsiState {
  param(
    [Parameter(Mandatory = $true)]
    [string]$MsiPath,
    [Parameter(Mandatory = $true)]
    [string]$Directory
  )

  $sha256 = ""
  if (Test-Path -LiteralPath $MsiPath -PathType Leaf) {
    $sha256 = (Get-FileHash -LiteralPath $MsiPath -Algorithm SHA256).Hash.ToLowerInvariant()
  }
  $logs = @(Get-ChildItem -LiteralPath $Directory -Filter "*.log" -File -ErrorAction SilentlyContinue |
      Sort-Object Name | ForEach-Object { $_.Name })
  return [pscustomobject][ordered]@{
    Sha256 = $sha256
    Logs = $logs
  }
}

function Invoke-VmVerifyMsiPrepare {
  param(
    [Parameter(Mandatory = $true)]
    [string]$RepositoryRoot,
    [Parameter(Mandatory = $true)]
    [string]$VMName,
    [Parameter(Mandatory = $true)]
    [string]$PackagePath,
    [AllowEmptyString()]
    [string]$CleanCheckpointName = "",
    [AllowEmptyString()]
    [string]$CheckpointName = "",
    [Parameter(Mandatory = $true)]
    [string]$GuestDestination,
    [AllowEmptyString()]
    [string]$ResultsDirectory = ""
  )

  # Lane 1 starts from the clean checkpoint without vc_redist. The plan section 2
  # baseline already has vc_redist, so there is no default; the caller names it.
  if (-not $CleanCheckpointName) {
    throw ("-CleanCheckpointName is required for an MSI. Lane 1 starts from the clean checkpoint " +
      "without the VC++ Redistributable, which differs from the plan section 2 baseline.")
  }
  $identity = Get-VmVerifyMsiIdentity -PackagePath $PackagePath
  $checkpoint = $CheckpointName
  if (-not $checkpoint) {
    $checkpoint = $identity.CheckpointName
  }

  $vm = Assert-VmVerifySessionVMRunning -VMName $VMName
  Assert-VmVerifySessionGuestService -VMName $VMName
  if (-not (Get-VmVerifySessionCheckpoint -VMName $VMName -CheckpointName $CleanCheckpointName)) {
    throw ("Clean checkpoint '$CleanCheckpointName' was not found on '$VMName'. " +
      "List the checkpoints with Get-VMSnapshot -VMName '$VMName'.")
  }
  # Existence is not enough. Unless the current state derives from it, the MSI would
  # go on top of whatever a previous verification left behind.
  if ([string]$vm.ParentSnapshotName -ne $CleanCheckpointName) {
    throw (@(
      "The current state of '$VMName' is based on '$($vm.ParentSnapshotName)', not on the clean checkpoint '$CleanCheckpointName'."
      "Restore it first so that the MSI goes into a clean guest:"
      "  Restore-VMSnapshot -VMName '$VMName' -Name '$CleanCheckpointName' -Confirm:`$false; Start-VM -Name '$VMName'"
    ) -join [Environment]::NewLine)
  }
  if (Get-VmVerifySessionCheckpoint -VMName $VMName -CheckpointName $checkpoint) {
    throw (@(
      "Checkpoint '$checkpoint' already exists on '$VMName'."
      "It was created for this same MSI and already holds the copied MSI, so -Prepare is not needed again."
      "Use -Restore to return to it before a reinstall, or remove it to start over from the clean checkpoint:"
      "  Remove-VMSnapshot -VMName '$VMName' -Name '$checkpoint'"
    ) -join [Environment]::NewLine)
  }

  # Copy first, then take the checkpoint. Restoring it then returns to a guest that
  # holds the MSI but has not installed it, so a reinstall and -Collect still work.
  # A failed copy leaves no checkpoint behind, and -Prepare can simply run again.
  $guest = Get-VmVerifyMsiGuestPath -GuestDestination $GuestDestination -Identity $identity
  Write-Host "Copying $($identity.MsiPath) to '$VMName':$($guest.Msi) ..."
  Invoke-VmVerifySessionFileCopy -VMName $VMName -SourcePath $identity.MsiPath `
    -DestinationPath $guest.Msi | Out-Null
  Write-Host "Taking checkpoint '$checkpoint' on '$VMName'..."
  Invoke-VmVerifySessionCheckpoint -VMName $VMName -CheckpointName $checkpoint | Out-Null

  $resultsRoot = Get-VmVerifyMsiResultsRoot -RepositoryRoot $RepositoryRoot `
    -ResultsDirectory $ResultsDirectory -Identity $identity
  $recordPath = Join-Path $resultsRoot "msi-record.json"
  Write-VmVerifyMsiRecord -Path $recordPath -Record ([ordered]@{
      schemaVersion = 1
      kind = "msi"
      fileName = $identity.FileName
      size = $identity.Size
      sha256 = $identity.Sha256
      vmName = $VMName
      cleanCheckpoint = $CleanCheckpointName
      checkpoint = $checkpoint
      guestPath = $guest.Msi
      preparedAtUtc = [DateTimeOffset]::UtcNow.ToString("o")
    })

  $logPath = Join-Path $guest.Directory "install.log"
  Write-Host "Checkpoint: $checkpoint"
  Write-Host "MSI: $($identity.FileName) (SHA-256 $($identity.Sha256))"
  Write-Host "Record: $recordPath"
  Write-Host "Guest path: $($guest.Msi)"
  Write-Host ""
  Write-Host "Next: install the MSI yourself in the guest; this script does not run msiexec."
  Write-Host "Write every verbose log next to the MSI so that -Collect can retrieve it, for example:"
  Write-Host "  msiexec /i `"$($guest.Msi)`" /L*v `"$logPath`""
  Write-Host "Then run this script with -Collect and the same -PackagePath right after msiexec returns."

  return [pscustomobject][ordered]@{
    CheckpointName = $checkpoint
    MsiPath = $identity.MsiPath
    Sha256 = $identity.Sha256
    GuestPath = $guest.Msi
    RecordPath = $recordPath
  }
}

function Invoke-VmVerifyMsiCollect {
  param(
    [Parameter(Mandatory = $true)]
    [string]$RepositoryRoot,
    [Parameter(Mandatory = $true)]
    [string]$VMName,
    [Parameter(Mandatory = $true)]
    [string]$PackagePath,
    [Parameter(Mandatory = $true)]
    [string]$GuestDestination,
    [System.Management.Automation.PSCredential]$Credential = $null,
    [AllowEmptyString()]
    [string]$ResultsDirectory = ""
  )

  $identity = Get-VmVerifyMsiIdentity -PackagePath $PackagePath
  $guest = Get-VmVerifyMsiGuestPath -GuestDestination $GuestDestination -Identity $identity
  Assert-VmVerifySessionVMRunning -VMName $VMName | Out-Null
  if (-not $Credential) {
    try {
      $Credential = Get-VmVerifySessionCredential -VMName $VMName
    } catch {
      throw ("Guest credentials are required for PowerShell Direct. Pass -Credential when no prompt " +
        "is available. Original error: $($_.Exception.Message)")
    }
    if (-not $Credential) {
      throw "Guest credentials were not provided, so PowerShell Direct cannot be used."
    }
  }
  try {
    $session = Open-VmVerifySessionGuestSession -VMName $VMName -Credential $Credential
  } catch {
    throw ("Could not open a PowerShell Direct session to '$VMName'. If the guest is back at the " +
      "sign-in screen, stop the VM and mount its disk read-only with Mount-VHD to collect the log. " +
      "Original error: $($_.Exception.Message)")
  }

  try {
    $state = Invoke-VmVerifySessionGuestStep -Session $session -Name "Get-VmVerifyGuestMsiState" `
      -ArgumentList @($guest.Msi, $guest.Directory)
    if (-not $state.Sha256) {
      throw "The MSI is not in the guest at $($guest.Msi). Run -Prepare with the same -PackagePath first."
    }
    if ($state.Sha256 -ne $identity.Sha256) {
      throw ("The MSI in the guest ($($state.Sha256)) differs from $($identity.MsiPath) " +
        "($($identity.Sha256)), so its logs would be attributed to another build.")
    }
    $logs = @($state.Logs)
    if ($logs.Count -eq 0) {
      throw ("No msiexec log (*.log) was found in '$VMName':$($guest.Directory). " +
        "Pass /L*v with a path in that directory to msiexec.")
    }

    $resultsRoot = Get-VmVerifyMsiResultsRoot -RepositoryRoot $RepositoryRoot `
      -ResultsDirectory $ResultsDirectory -Identity $identity
    $destination = Join-Path $resultsRoot ("logs-" + [DateTime]::UtcNow.ToString("yyyyMMdd-HHmmss"))
    New-Item -ItemType Directory -Path $destination -Force | Out-Null
    foreach ($log in $logs) {
      Copy-VmVerifySessionGuestArtifact -Session $session -GuestPath (Join-Path $guest.Directory $log) `
        -Destination (Join-Path $destination $log)
    }
  } finally {
    Close-VmVerifySessionGuestSession -Session $session
  }

  Write-VmVerifyMsiRecord -Path (Join-Path $destination "msi-identity.json") -Record ([ordered]@{
      schemaVersion = 1
      kind = "msi"
      fileName = $identity.FileName
      sha256 = $identity.Sha256
      guestSha256 = $state.Sha256
      vmName = $VMName
      logs = $logs
      collectedAtUtc = [DateTimeOffset]::UtcNow.ToString("o")
    })
  Write-Host "msiexec logs: $destination ($($logs -join ', '))"

  return [pscustomobject][ordered]@{
    ResultsPath = $destination
    Logs = $logs
  }
}
