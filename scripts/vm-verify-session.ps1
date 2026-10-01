#requires -Version 5.1
<#
.SYNOPSIS
  Bundles the Hyper-V host-side verification session operations (taking a
  checkpoint, transferring the package, restoring) into one command.

.DESCRIPTION
  Given a verification package produced by make-vm-verify-package.ps1, automates
  the following steps that docs/handoff/hyper-v-tip-verification.md defined as
  manual host-side work.

    -Prepare : takes the pre-verification checkpoint and transfers the zip to the VM.
    -Run     : runs bootstrap inside the guest over PowerShell Direct, runs
               azookey_diag.exe --json and compat_test.exe through a scheduled
               task of the interactive user, and collects the artifacts on the host
               (L1 in docs/handoff/hyper-v-vm-verification-plan.md section 5).
               The diag output is azookey-diag.json in the run directory and can
               be passed as-is to vm-verify-summary.ps1 -DiagJsonPath.
    -Restore : restores the given checkpoint.
    -Collect : MSI only. Checks the hash of the MSI in the guest and collects the
               msiexec /L*v logs.

  Passing an .msi to -PackagePath switches to lane 1 (MSI) (vm-verify-msi.ps1).
  -Prepare confirms the VM derives from the clean checkpoint named by
  -CleanCheckpointName, takes a checkpoint named from the MSI hash, and then
  transfers the MSI. A person runs msiexec, so -Run is not available for an MSI.

  -CompatCases / -CompatSkip of -Run are case IDs (C-001 format) passed to
  compat_test.exe --cases / --skip. C-013 suspends the Host and C-010 ends it, so
  run with -CompatSkip C-013,C-010 before the manual typing gates and run those
  cases after the gates.

  The checkpoint name is generated deterministically from the package's
  manifest.json (commit / preset), so rerunning against the same package always
  refers to the same name.

  Switching VMConnect to the basic session is interactive and is not automated.
  When -Prepare completes, the output states that IME verification is done in the
  basic session.

  -Run takes the guest credential from -Credential (a PSCredential; a secret stored
  as a PSCredential in SecretManagement can be passed as Get-Secret returns it) or,
  when omitted, from Get-Credential.
  It never takes a plain-text password as an argument and never writes one to logs.
#>
param(
  [switch]$Prepare,
  [switch]$Restore,
  [switch]$Run,
  [switch]$Collect,
  [string]$VMName = "",
  [string]$PackagePath = "",
  [string]$CheckpointName = "",
  [string]$CleanCheckpointName = "",
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
. (Join-Path $PSScriptRoot "vm-verify-msi.ps1")

function Get-VmVerifySessionAbsolutePath {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path
  )

  return $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path)
}

# Without an explicit path, pick the newest package from the default output
# directory. make-vm-verify-package.ps1 places the zip and manifest side by side
# under the same baseName.
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

# Generated deterministically from commit / preset. A rerun with the same package
# refers to the same name and is never confused with a verification of another
# commit or preset.
function Get-VmVerifySessionCheckpointName {
  param(
    [Parameter(Mandatory = $true)]
    $Manifest
  )

  $shortCommit = $Manifest.commit.Substring(0, 12)
  return "pre-azookey-$($Manifest.preset)-$shortCommit"
}

# Thin wrappers around the Hyper-V cmdlets from here on. Each call is enclosed in
# its own function so that Pester can replace it even without the Hyper-V module.
function Get-VmVerifySessionVM {
  param(
    [Parameter(Mandatory = $true)]
    [string]$VMName
  )

  return Get-VM -Name $VMName -ErrorAction Stop
}

# The component ID of Guest Service Interface. The display name (Name) is localized
# to the host language (Japanese Windows shows it in Japanese), so looking it up by
# display name always fails on such hosts. Select it by the language-independent ID.
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

# Copy-VMFile works only while Guest Service Interface is enabled.
# Otherwise point to an alternative and exit non-zero. Never skip the transfer
# silently.
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

  # Do not write the enable command with the display name either. On a localized
  # host it fails for the same reason, and following the guidance would not help.
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

  # Never silently overwrite an existing checkpoint of the same name. The VM may be
  # dirty from a previous verification, and saving that state as "before
  # verification" would lose the baseline.
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
# Host side of -Run. Each PowerShell Direct call is enclosed in its own function so
# that Pester can replace it.
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

# Accepts both "C-001,C-004" and @("C-001", "C-004") and normalizes them to the
# comma-separated form passed to compat_test.exe. The value is embedded in the
# task's command string, so anything but the case ID format is rejected here.
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

# compat_test.exe rejects IDs the target does not have, and a selection that ends
# up empty, with exit code 64. Check against the bundled target definitions before
# spending time on TIP registration and bootstrap.
function Assert-VmVerifySessionCaseSelection {
  param(
    [Parameter(Mandatory = $true)]
    [AllowEmptyCollection()]
    [object[]]$TargetCases,
    [string]$Cases = "",
    [string]$Skip = ""
  )

  $requested = @($Cases.Split(",", [StringSplitOptions]::RemoveEmptyEntries))
  $skipped = @($Skip.Split(",", [StringSplitOptions]::RemoveEmptyEntries))
  foreach ($target in @($TargetCases)) {
    $known = @($target.Cases)
    $unknown = @(@($requested) + @($skipped) | Where-Object { $known -cnotcontains $_ } |
        Select-Object -Unique)
    if ($unknown.Count -ne 0) {
      throw ("Target '$($target.Target)' does not define $($unknown -join ', '). " +
        "Its cases are $($known -join ', ').")
    }
    $selected = @($known | Where-Object {
        ($requested.Count -eq 0 -or $requested -ccontains $_) -and $skipped -cnotcontains $_
      })
    if ($selected.Count -eq 0) {
      throw "The case selection leaves no case to run for target '$($target.Target)'."
    }
  }
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
      "Invoke-VmVerifyGuestDiag",
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

  # compat_test.exe exit codes: 0 = all pass, 1 = includes a fail,
  # 2 = no fail but includes failing-skip (compat-test/README.md).
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
      Excluded = @($target.excluded | Where-Object { $_ } | ForEach-Object { [string]$_ })
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
  if ($Cases -or $Skip) {
    $targetCases = @(Invoke-VmVerifySessionGuestStep -Session $Session -Name "Get-VmVerifyGuestTargetCase" `
        -ArgumentList @($Paths.GuestPackageRoot))
    Assert-VmVerifySessionCaseSelection -TargetCases $targetCases -Cases $Cases -Skip $Skip
  }
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
  # Do not proceed to compat when the layer 1 bootstrap fails (plan section 3).
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
    Diag = [pscustomobject][ordered]@{
      Captured = [bool]$status.diagJson
      Error = [string]$status.diagError
    }
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

  # A failure before the run directory exists leaves nothing to collect. Keep that
  # apart from a collection failure so a secondary error does not bury the original
  # reason. Copying the azooKey logs is supplementary, so a failure there is only a
  # warning and collection of the bootstrap JSON and the compat report continues.
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
  # bootstrap registers the TIP machine-wide. Do not run it on a VM with no baseline
  # to restore to.
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
    # diag is supplementary information for the environment block, so failing to
    # collect it does not change the L1 verdict. vm-verify-summary.ps1 reports the
    # gap explicitly as missing.
    if ($outcome.Diag.Captured -and $collection.Collected) {
      Write-Host "azookey_diag --json: $(Join-Path $hostRunRoot 'azookey-diag.json')"
    } elseif (-not $outcome.Diag.Captured) {
      Write-Warning "azookey_diag --json was not captured in the interactive session: $($outcome.Diag.Error)"
    }
    foreach ($target in $outcome.Targets) {
      $partial = ""
      if (@($target.Excluded).Count -ne 0) {
        $partial = "; partial run, not run: $(@($target.Excluded) -join ', ')"
      }
      Write-Host "compat_test.exe $($target.Target): $($target.Outcome) (exit $($target.ExitCode)$partial)"
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
  if (@($Prepare, $Restore, $Run, $Collect | Where-Object { $_ }).Count -ne 1) {
    throw "Specify exactly one of -Prepare, -Restore, -Run, or -Collect."
  }
  if (-not $VMName) {
    throw "-VMName is required. Check the name with Get-VM."
  }
  if (-not $Run -and (@($CompatCases).Count -ne 0 -or @($CompatSkip).Count -ne 0)) {
    throw "-CompatCases and -CompatSkip apply only to -Run."
  }
  $isMsi = Test-VmVerifyMsiPackagePath -PackagePath $PackagePath
  if ($isMsi -and $Run) {
    throw ("-Run does not accept an MSI: a person runs msiexec in lane 1 " +
      "(hyper-v-vm-verification-plan.md section 4.5). Use -Prepare, install it, then -Collect.")
  }
  if ($Collect -and -not $isMsi) {
    throw "-Collect applies only to an MSI. Pass -PackagePath <file>.msi."
  }
  if ($CleanCheckpointName -and -not ($isMsi -and $Prepare)) {
    throw "-CleanCheckpointName applies only to -Prepare with an MSI."
  }

  $repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

  if ($isMsi -and $Prepare) {
    Invoke-VmVerifyMsiPrepare `
      -RepositoryRoot $repositoryRoot `
      -VMName $VMName `
      -PackagePath $PackagePath `
      -CleanCheckpointName $CleanCheckpointName `
      -CheckpointName $CheckpointName `
      -GuestDestination $GuestDestination `
      -ResultsDirectory $ResultsDirectory | Out-Null
  } elseif ($Collect) {
    Invoke-VmVerifyMsiCollect `
      -RepositoryRoot $repositoryRoot `
      -VMName $VMName `
      -PackagePath $PackagePath `
      -GuestDestination $GuestDestination `
      -Credential $Credential `
      -ResultsDirectory $ResultsDirectory | Out-Null
  } elseif ($isMsi -and $Restore) {
    $msiCheckpoint = $CheckpointName
    if (-not $msiCheckpoint) {
      $msiCheckpoint = (Get-VmVerifyMsiIdentity -PackagePath $PackagePath).CheckpointName
    }
    Invoke-VmVerifySessionRestore `
      -RepositoryRoot $repositoryRoot `
      -VMName $VMName `
      -CheckpointName $msiCheckpoint | Out-Null
  } elseif ($Prepare) {
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
