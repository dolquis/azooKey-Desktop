#requires -Version 5.1
<#
.SYNOPSIS
  Functions that vm-verify-session.ps1 -Run executes inside the guest.

.DESCRIPTION
  vm-verify-session.ps1 dot-sources this file and sends each function body to
  PowerShell Direct Invoke-Command. The same bodies are also written into the
  script for the interactive task. On its own this file executes nothing.
#>

# ---------------------------------------------------------------------------
# Guest-side functions for -Run. Only the function body is sent to
# Invoke-Command -ScriptBlock, so do not depend on other functions or script
# variables, and use only syntax that runs on the guest's Windows PowerShell 5.1.
# The same bodies are also written into the script for the interactive task.
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
  # Extract to a fixed path derived from the zip name. If the TIP DLL path changed
  # on every run, bootstrap could not tell it is already registered and would
  # re-register each time. The zip name only carries the commit and preset, so a
  # zip re-sent under the same name is told apart by its hash and extracted again.
  $zipHash = (Get-FileHash -LiteralPath $ZipPath -Algorithm SHA256).Hash
  $markerPath = Join-Path $PackageRoot ".azookey-verify-archive.sha256"
  $expandedHash = ""
  if (Test-Path -LiteralPath $markerPath -PathType Leaf) {
    $expandedHash = [string](Get-Content -Raw -LiteralPath $markerPath)
  }
  if ($expandedHash.Trim() -ne $zipHash) {
    if (Test-Path -LiteralPath $PackageRoot) {
      try {
        Remove-Item -LiteralPath $PackageRoot -Recurse -Force
      } catch {
        throw ("The previously expanded package is in use and cannot be replaced: $PackageRoot. " +
          "Restore the checkpoint (-Restore), run -Prepare again, then retry. " +
          "Original error: $($_.Exception.Message)")
      }
    }
    Expand-Archive -LiteralPath $ZipPath -DestinationPath $PackageRoot -Force
    Set-Content -LiteralPath $markerPath -Value $zipHash -Encoding ASCII
  }

  $missing = @()
  foreach ($name in @("verify-bootstrap.ps1", "compat_test.exe", "compat_host_hang_watchdog.exe")) {
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

# Returns the case IDs of each bundled target. compat_test.exe stops with exit code
# 64 when -CompatCases / -CompatSkip names an ID the target does not have, so the
# host checks them before bootstrap.
function Get-VmVerifyGuestTargetCase {
  param(
    [Parameter(Mandatory = $true)]
    [string]$PackageRoot
  )

  $targets = @(Get-ChildItem -LiteralPath (Join-Path $PackageRoot "targets") `
      -Filter "*.json" -File | Sort-Object Name)
  foreach ($target in $targets) {
    $config = Get-Content -Raw -Encoding UTF8 -LiteralPath $target.FullName | ConvertFrom-Json
    [pscustomobject][ordered]@{
      Target = $target.BaseName
      Cases = @($config.cases | ForEach-Object { [string]$_ })
    }
  }
}

function Get-VmVerifyGuestInteractiveUser {
  $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
  $principal = New-Object Security.Principal.WindowsPrincipal($identity)
  # Win32_ComputerSystem.UserName returns the user signed in to the console (the
  # VMConnect basic session). It is empty when only an enhanced session (RDP) exists.
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

  # Run as a child process so that bootstrap's exit does not end this caller too.
  # The child writes the -Json output to a file itself, because redirecting
  # stdout could let the resident supervisor that bootstrap starts inherit the
  # handle. Write-Warning (stream 3) would corrupt the JSON, so it goes to a
  # separate file.
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

  # Start-Process -Wait in Windows PowerShell 5.1 waits for all descendant
  # processes, so it never returns while the resident supervisor is alive. Wait
  # only for the child itself with WaitForExit.
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

# A PowerShell Direct session lives in Session 0, and the TIP in the interactive
# session cannot connect to a Host started there. bootstrap's pipe check and the
# supervisor mutex do not distinguish sessions, so a leftover Session 0 Host would
# be reused by the interactive bootstrap. Stop it before the interactive task.
function Invoke-VmVerifyGuestSessionZeroHostShutdown {
  param(
    [Parameter(Mandatory = $true)]
    [string]$PackageRoot
  )

  Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force
  . (Join-Path $PackageRoot "verify-bootstrap.ps1")

  $pipeName = Get-VmVerifyPipeName
  if (-not (Test-VmVerifyPipe -PipeName $pipeName)) {
    # While the supervisor waits to restart the Host there is no pipe. If it were
    # left running, the interactive supervisor could not take the mutex and a
    # Session 0 Host would come up later.
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

# compat_test.exe uses UI Automation and SendInput, so it needs the interactive
# desktop. A PowerShell Direct session is non-interactive, so run it as a scheduled
# task of the user signed in to the console. LogonType Interactive runs only while
# that user is signed in and does not store a password in the task. RunLevel
# Limited starts the Host and the target apps with the same non-elevated token as
# the manual procedure.
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
    # LastTaskResult is a uint32. Converting a 0x8xxxxxxx HRESULT to [int] throws.
    return [int64](Get-ScheduledTaskInfo -TaskName $TaskName).LastTaskResult
  } finally {
    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false -ErrorAction SilentlyContinue
  }
}

# compat_test.exe does not select azooKey (compat-test/README.md). Registering the
# TIP only makes the input method selectable, so make azooKey the interactive
# user's default input method. This is a per-user setting and is undone by
# restoring the checkpoint after verification.
function Invoke-VmVerifyGuestInputMethodSelection {
  $tip = "0411:{71EE04FA-B35D-4EB8-87A1-582D44A9A58C}{A8F74D91-8DF3-4DA1-B80B-01F7C73D4A90}"
  $current = Get-WinDefaultInputMethodOverride
  if ($current -and [string]$current.InputMethodTip -eq $tip) {
    return "already-default"
  }

  $languages = Get-WinUserLanguageList
  $japanese = @($languages | Where-Object { $_.LanguageTag -like "ja*" }) | Select-Object -First 1
  if (-not $japanese) {
    throw "Japanese is not in the console user's language list, so azooKey cannot be selected."
  }
  if (@($japanese.InputMethodTips) -notcontains $tip) {
    $japanese.InputMethodTips.Add($tip)
    Set-WinUserLanguageList -LanguageList $languages -Force
  }
  Set-WinDefaultInputMethodOverride -InputTip $tip

  $selected = Get-WinDefaultInputMethodOverride
  if (-not $selected -or [string]$selected.InputMethodTip -ne $tip) {
    throw "azooKey could not be made the default input method of the console user."
  }
  return "switched"
}

# Set-WinDefaultInputMethodOverride changes only the default for later sign-ins. A
# session that is already signed in keeps its current input method (Microsoft IME
# on a fresh sign-in), and the apps compat_test.exe starts inherit it. So activate
# the azooKey profile for the whole desktop with TSF, then read the active keyboard
# profile back. TF_IPPMF_FORSESSION acts on the current desktop, so this must run
# in the interactive task, not over PowerShell Direct (session 0). The profile must
# already be enabled for the user, which Invoke-VmVerifyGuestInputMethodSelection
# does; otherwise ActivateProfile returns S_FALSE. -QueryOnly only reads it back.
# The result uses the InputMethodTip format of Get-WinUserLanguageList
# ("0411:{CLSID}{PROFILE}"); a keyboard layout is "0411:HKL:<hex>".
function Invoke-VmVerifyGuestInputMethodActivation {
  param(
    [string]$Tip = "0411:{71EE04FA-B35D-4EB8-87A1-582D44A9A58C}{A8F74D91-8DF3-4DA1-B80B-01F7C73D4A90}",
    [switch]$QueryOnly,
    [int]$TimeoutSeconds = 10
  )

  if (-not ([System.Management.Automation.PSTypeName]"VmVerifyTsfProfile").Type) {
    Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;

public static class VmVerifyTsfProfile {
  [StructLayout(LayoutKind.Sequential)]
  struct TF_INPUTPROCESSORPROFILE {
    public uint dwProfileType;
    public ushort langid;
    public Guid clsid;
    public Guid guidProfile;
    public Guid catid;
    public IntPtr hklSubstitute;
    public uint dwCaps;
    public IntPtr hkl;
    public uint dwFlags;
  }

  // Methods are declared in vtable order (msctf.h).
  [ComImport, Guid("71c6e74c-0f28-11d8-a82a-00065b84435c"),
   InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  interface ITfInputProcessorProfileMgr {
    [PreserveSig] int ActivateProfile(uint dwProfileType, ushort langid, ref Guid clsid,
        ref Guid guidProfile, IntPtr hkl, uint dwFlags);
    [PreserveSig] int DeactivateProfile();
    [PreserveSig] int GetProfile();
    [PreserveSig] int EnumProfiles();
    [PreserveSig] int ReleaseInputProcessor();
    [PreserveSig] int RegisterProfile();
    [PreserveSig] int UnregisterProfile();
    [PreserveSig] int GetActiveProfile(ref Guid catid, out TF_INPUTPROCESSORPROFILE profile);
  }

  const uint TF_PROFILETYPE_INPUTPROCESSOR = 1;
  const uint TF_IPPMF_FORSESSION = 0x20000000;
  static readonly Guid CLSID_TF_InputProcessorProfiles =
      new Guid("33C53A50-F456-4884-B049-85FD643ECFED");
  static readonly Guid GUID_TFCAT_TIP_KEYBOARD =
      new Guid("34745C63-B2F0-4784-8B67-5E12C8701A31");

  static ITfInputProcessorProfileMgr Create() {
    return (ITfInputProcessorProfileMgr)Activator.CreateInstance(
        Type.GetTypeFromCLSID(CLSID_TF_InputProcessorProfiles));
  }

  public static int Activate(ushort langid, Guid clsid, Guid guidProfile) {
    return Create().ActivateProfile(TF_PROFILETYPE_INPUTPROCESSOR, langid, ref clsid,
        ref guidProfile, IntPtr.Zero, TF_IPPMF_FORSESSION);
  }

  public static string GetActive() {
    Guid catid = GUID_TFCAT_TIP_KEYBOARD;
    TF_INPUTPROCESSORPROFILE profile;
    int hr = Create().GetActiveProfile(ref catid, out profile);
    if (hr != 0) {
      throw new COMException("GetActiveProfile failed.", hr);
    }
    if (profile.dwProfileType == TF_PROFILETYPE_INPUTPROCESSOR) {
      return string.Format("{0:X4}:{1}{2}", profile.langid,
          profile.clsid.ToString("B").ToUpperInvariant(),
          profile.guidProfile.ToString("B").ToUpperInvariant());
    }
    return string.Format("{0:X4}:HKL:{1:X}", profile.langid, profile.hkl.ToInt64());
  }
}
"@
  }

  if ($QueryOnly) {
    return [pscustomobject][ordered]@{
      Activation = "query-only"
      ActiveInputMethod = [VmVerifyTsfProfile]::GetActive()
    }
  }
  if ($Tip -notmatch "^([0-9A-Fa-f]{4}):(\{[0-9A-Fa-f-]{36}\})(\{[0-9A-Fa-f-]{36}\})$") {
    throw "Not a text service input method tip: $Tip"
  }
  $langid = [Convert]::ToUInt16($Matches[1], 16)
  $clsid = [Guid]$Matches[2]
  $guidProfile = [Guid]$Matches[3]

  $active = [VmVerifyTsfProfile]::GetActive()
  if ($active -eq $Tip) {
    return [pscustomobject][ordered]@{ Activation = "already-active"; ActiveInputMethod = $active }
  }
  $hr = [VmVerifyTsfProfile]::Activate($langid, $clsid, $guidProfile)
  if ($hr -ne 0) {
    # S_FALSE (1) means the profile is not enabled for the user, which is a failure too.
    throw ("ITfInputProcessorProfileMgr::ActivateProfile returned 0x{0:X8} for {1}; " -f $hr, $Tip) +
      "the active input method stays $active."
  }
  $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
  while ($true) {
    $active = [VmVerifyTsfProfile]::GetActive()
    if ($active -eq $Tip -or [DateTime]::UtcNow -ge $deadline) {
      break
    }
    Start-Sleep -Milliseconds 500
  }
  return [pscustomobject][ordered]@{ Activation = "activated"; ActiveInputMethod = $active }
}

# Runs azookey_diag.exe --json in the interactive session and writes stdout to a
# file as-is. The Host pipe and settings are per user, so collect it as the same
# interactive user as compat rather than from PowerShell Direct (Session 0).
# --json always exits with 0; the verdict lives in the status inside the JSON.
# Passing the JSON through a PowerShell string can corrupt it under the 5.1 default
# encoding, so stdout is redirected straight from the process to the file.
function Invoke-VmVerifyGuestDiag {
  param(
    [Parameter(Mandatory = $true)]
    [string]$PackageRoot,
    [Parameter(Mandatory = $true)]
    [string]$OutputPath,
    [int]$TimeoutSeconds = 120
  )

  $diag = Join-Path $PackageRoot "azookey_diag.exe"
  if (-not (Test-Path -LiteralPath $diag -PathType Leaf)) {
    throw "azookey_diag.exe is not in the package: $diag"
  }
  $process = Start-Process -FilePath $diag -ArgumentList "--json" `
    -RedirectStandardOutput $OutputPath `
    -RedirectStandardError ([System.IO.Path]::ChangeExtension($OutputPath, ".stderr.log")) `
    -NoNewWindow -PassThru
  $null = $process.Handle
  if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
    $process.Kill()
    throw "azookey_diag.exe --json did not finish within $TimeoutSeconds seconds and was stopped."
  }
  return [int]$process.ExitCode
}

# The body the interactive task executes. It is written to the guest as a script
# file together with Invoke-VmVerifyGuestBootstrap,
# Invoke-VmVerifyGuestInputMethodSelection, Invoke-VmVerifyGuestInputMethodActivation
# and Invoke-VmVerifyGuestDiag. The state file is always written, even on failure.
function Invoke-VmVerifyGuestCompatRun {
  param(
    [Parameter(Mandatory = $true)]
    [string]$PackageRoot,
    [Parameter(Mandatory = $true)]
    [string]$RunRoot,
    # Comma-separated case IDs passed as-is to compat_test.exe --cases / --skip.
    # When empty, no argument is added and every case of the target runs.
    [string]$Cases = "",
    [string]$Skip = ""
  )

  $ErrorActionPreference = "Stop"
  $sessionId = (Get-Process -Id $PID).SessionId
  $status = [ordered]@{
    schemaVersion = 1
    completed = $false
    cases = $Cases
    skip = $Skip
    sessionId = $sessionId
    bootstrapExitCode = $null
    bootstrapStatus = ""
    hostProcessId = 0
    hostSessionId = $null
    inputMethod = ""
    inputMethodActivation = ""
    activeInputMethod = ""
    diagExitCode = $null
    diagJson = $false
    diagError = ""
    targets = @()
    error = ""
  }
  $originalLog = [Environment]::GetEnvironmentVariable("AZOOKEY_LOG", "Process")
  $originalLogLevel = [Environment]::GetEnvironmentVariable("AZOOKEY_LOG_LEVEL", "Process")
  try {
    # Registration is already done on the PowerShell Direct side, so this rerun
    # does not go through UAC elevation; it only starts and checks the Host in this
    # interactive session.
    $bootstrapPath = Join-Path $RunRoot "bootstrap-interactive.json"
    $status.bootstrapExitCode = Invoke-VmVerifyGuestBootstrap `
      -PackageRoot $PackageRoot -OutputPath $bootstrapPath -CheckpointConfirmed $true
    $bootstrap = Get-Content -Raw -Encoding UTF8 -LiteralPath $bootstrapPath | ConvertFrom-Json
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
    $status.inputMethod = Invoke-VmVerifyGuestInputMethodSelection
    $azooKeyTip = "0411:{71EE04FA-B35D-4EB8-87A1-582D44A9A58C}{A8F74D91-8DF3-4DA1-B80B-01F7C73D4A90}"
    $activation = Invoke-VmVerifyGuestInputMethodActivation -Tip $azooKeyTip
    $status.inputMethodActivation = [string]$activation.Activation
    $status.activeInputMethod = [string]$activation.ActiveInputMethod
    if ($status.activeInputMethod -ne $azooKeyTip) {
      throw ("The active input method of the interactive session is " +
        "'$($status.activeInputMethod)', not azooKey ($azooKeyTip), so compat_test.exe was not run.")
    }

    # Collect before compat. C-010 ends the Host and C-013 suspends it, so after
    # compat the state no longer reflects the moment right after bootstrap. diag is
    # supplementary, so on failure record the reason and continue to compat.
    $diagPath = Join-Path $RunRoot "azookey-diag.json"
    try {
      $status.diagExitCode = Invoke-VmVerifyGuestDiag -PackageRoot $PackageRoot -OutputPath $diagPath
      $status.diagJson = [bool]((Test-Path -LiteralPath $diagPath -PathType Leaf) -and
        (Get-Item -LiteralPath $diagPath).Length -gt 0)
      if (-not $status.diagJson) {
        $status.diagError = "azookey_diag.exe --json produced no output (exit $($status.diagExitCode))."
      }
    } catch {
      $status.diagError = $_.Exception.Message
    }

    $compat = Join-Path $PackageRoot "compat_test.exe"
    $selection = ""
    if ($Cases) {
      $selection += " --cases $Cases"
    }
    if ($Skip) {
      $selection += " --skip $Skip"
    }
    $runHostHang = (-not $Cases -or $Cases.Split(',') -ccontains 'C-013') -and
      ($Skip.Split(',') -cnotcontains 'C-013')
    if ($runHostHang) {
      # Only newly started processes inherit this. When launching is delegated to an
      # existing browser / VS Code, C-013 reports the unverified log as failing-skip.
      [Environment]::SetEnvironmentVariable("AZOOKEY_LOG", "1", "Process")
      [Environment]::SetEnvironmentVariable("AZOOKEY_LOG_LEVEL", "info", "Process")
    }
    $targets = @(Get-ChildItem -LiteralPath (Join-Path $PackageRoot "targets") `
        -Filter "*.json" -File | Sort-Object Name)
    foreach ($target in $targets) {
      $name = $target.BaseName
      $reportDirectory = Join-Path $RunRoot "compat-report-$name"
      $process = Start-Process -FilePath $compat `
        -ArgumentList "--target `"$($target.FullName)`" --output `"$reportDirectory`"$selection" `
        -RedirectStandardOutput (Join-Path $RunRoot "compat-$name.stdout.log") `
        -RedirectStandardError (Join-Path $RunRoot "compat-$name.stderr.log") `
        -NoNewWindow -PassThru
      # -Wait would also wait for the target apps compat_test.exe started (such as
      # Edge's resident processes) to exit. Wait only for compat_test.exe itself and
      # leave the overall limit to the task's time limit.
      $null = $process.Handle
      $process.WaitForExit()
      $reportPath = Join-Path $reportDirectory "report.json"
      $reportJson = [bool](Test-Path -LiteralPath $reportPath -PathType Leaf)
      # Copy the cases that did not run into the state so that a partial run is not
      # mistaken for an all-pass.
      $excluded = @()
      if ($reportJson) {
        try {
          $report = Get-Content -Raw -Encoding UTF8 -LiteralPath $reportPath | ConvertFrom-Json
          if ($report.case_selection) {
            $excluded = @($report.case_selection.excluded | ForEach-Object { [string]$_ })
          }
        } catch {
          # vm-verify-summary.ps1 counts a broken report.json as invalid. Continue
          # here as if nothing was excluded so the remaining targets are not stopped.
          $excluded = @()
        }
      }
      $status.targets += [pscustomobject][ordered]@{
        target = $name
        exitCode = [int]$process.ExitCode
        reportJson = $reportJson
        excluded = $excluded
      }
    }
    $status.completed = $true
  } catch {
    $status.error = $_.Exception.Message
  } finally {
    [Environment]::SetEnvironmentVariable("AZOOKEY_LOG", $originalLog, "Process")
    [Environment]::SetEnvironmentVariable("AZOOKEY_LOG_LEVEL", $originalLogLevel, "Process")
    [pscustomobject]$status | ConvertTo-Json -Depth 4 |
      Set-Content -LiteralPath (Join-Path $RunRoot "interactive-status.json") -Encoding UTF8
  }
}
