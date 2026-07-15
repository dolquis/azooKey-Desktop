param(
  [string]$TipDllPath = "",
  [string]$HostExePath = "",
  [switch]$AllowMockHost,
  # Internal: set when the script relaunches itself elevated. The per-user
  # (HKCU) inference-host auto-start is written in the original user's process
  # *before* elevation, so the elevated reentry must skip it — otherwise, when a
  # standard user elevates with a *separate* administrator account, the Run value
  # would land in that administrator's hive instead of the interactive user's.
  [switch]$ElevatedReentry
)

$ErrorActionPreference = "Stop"

function Resolve-DevPath {
  param(
    [Parameter(Mandatory=$true)]
    [string]$Path
  )

  return $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path)
}

function Assert-LlamaEnabledHost {
  param(
    [Parameter(Mandatory=$true)]
    [string]$Path,
    [switch]$AllowMock
  )

  if ($AllowMock) {
    Write-Warning "Skipping llama.cpp preflight because -AllowMockHost was specified. Zenzai candidates will use the fallback converter."
    return
  }

  if (-not (Test-Path $Path)) {
    throw "Inference host not found: $Path"
  }

  $buildDir = Split-Path -Parent (Split-Path -Parent $Path)
  $benchPath = Join-Path $buildDir "bench\azookey_zenzai_bench.exe"
  if (-not (Test-Path $benchPath)) {
    throw "llama.cpp preflight tool not found: $benchPath. Build the windows-llama-debug preset before registration."
  }

  $output = & $benchPath 2>&1
  $exitCode = $LASTEXITCODE
  $outputText = ($output | Out-String).Trim()
  if ($exitCode -ne 0 -or $outputText -notmatch '(?:^|\s)llama_cpp=1(?:\s|$)') {
    throw "Inference host is not linked with llama.cpp. Build the windows-llama-debug preset, or use -AllowMockHost only for fallback-only TIP tests. Preflight output: $outputText"
  }

  Write-Host "llama.cpp host preflight passed: $benchPath"
}

# Resolve default paths relative to the script location, then make them absolute
# *before* any elevation relaunch: the elevated process starts in a different
# working directory, so relative paths would otherwise resolve incorrectly. Use
# PowerShell's location resolver rather than .NET's process current directory:
# Developer PowerShell sessions can leave those two out of sync after cd.
if (-not $TipDllPath) {
  $TipDllPath = Join-Path $PSScriptRoot "..\build\windows-llama-debug\tsf-tip\azookey_tsf_tip.dll"
}
if (-not $HostExePath) {
  $HostExePath = Join-Path $PSScriptRoot "..\build\windows-llama-debug\inference-host\azookey_inference_host.exe"
}

$TipDllPath  = Resolve-DevPath $TipDllPath
$HostExePath = Resolve-DevPath $HostExePath

if (-not $ElevatedReentry) {
  Assert-LlamaEnabledHost -Path $HostExePath -AllowMock:$AllowMockHost
}

# Per-user step (HKCU, no elevation needed): register the inference host for
# auto-start in the *interactive* user's hive. Done in the original process so a
# relaunch under a different administrator account cannot write it to the wrong
# profile. Best-effort. Skipped on the elevated reentry.
if (-not $ElevatedReentry) {
  if (Test-Path $HostExePath) {
    $runKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run"
    try {
      New-ItemProperty -Path $runKey -Name "azooKeyInferenceHost" `
        -Value "`"$HostExePath`" --pipe" -PropertyType String -Force | Out-Null
      Write-Host "Inference host auto-start registered (current user): $HostExePath"
    } catch {
      Write-Warning "Could not register inference host auto-start: $_"
    }

    # Start the host for the *current* session as well. The HKCU Run entry above
    # only takes effect at the next logon, so without this the first verification
    # right after registration sees a live TIP (preedit works) but no candidates
    # (Space returns nothing) because nothing is serving the per-user pipe yet.
    # Started here, in the original (non-elevated) process, so it runs as the
    # interactive user — same rationale as the Run entry. Best-effort; uses the
    # same `--pipe` arguments so the in-session host matches the auto-start one.
    #
    # Skip only when *this user's* pipe is already being served. The pipe name is
    # per-user — DefaultPipeName() is \\.\pipe\azookey-<current user SID> — so a
    # global process-name match would, on RDP / Fast User Switching, find another
    # account's host (listening on a different pipe) and wrongly suppress starting
    # one for the interactive user. Probe the exact per-user pipe instead. On
    # enumeration failure, fall through and start (best-effort: a redundant start
    # is preferable to leaving the just-registered TIP with no candidate server).
    $mySid = ([Security.Principal.WindowsIdentity]::GetCurrent()).User.Value
    $myPipe = "azookey-$mySid"
    $hostServing = $false
    try {
      $hostServing = [bool]([System.IO.Directory]::GetFiles("\\.\pipe\") |
        Where-Object { [System.IO.Path]::GetFileName($_) -eq $myPipe })
    } catch {
      $hostServing = $false
    }
    if ($hostServing) {
      Write-Host "Inference host already serving this user's pipe ($myPipe); not starting another."
    } else {
      try {
        Start-Process -FilePath $HostExePath -ArgumentList "--pipe" -WindowStyle Hidden
        Write-Host "Inference host started for current session: $HostExePath"
      } catch {
        Write-Warning "Could not start inference host for current session: $_"
      }
    }
  } else {
    Write-Warning "Inference host not found, skipping auto-start registration: $HostExePath"
  }
}

# Machine-wide step (HKLM COM + TSF profile under CTF\TIP): requires elevation.
# TSF has no persistent per-user TIP registration (see
# docs/sideload-packaging-spec.md §1). Relaunch elevated if needed. The argument
# list is built as a single string with each path wrapped in escaped double
# quotes so values containing spaces survive the relaunch (Start-Process does not
# re-quote array elements).
$principal = New-Object Security.Principal.WindowsPrincipal(
  [Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
  Write-Host "Machine-wide registration requires elevation; relaunching as administrator..."
  $relaunchArgs = "-NoExit -ExecutionPolicy Bypass -File `"$PSCommandPath`" " +
                  "-TipDllPath `"$TipDllPath`" -HostExePath `"$HostExePath`" -ElevatedReentry"
  Start-Process -FilePath "powershell.exe" -Verb RunAs -ArgumentList $relaunchArgs
  return
}

if (!(Test-Path $TipDllPath)) {
  throw "TIP DLL not found: $TipDllPath"
}

Write-Host "Registering TIP DLL (machine-wide): $TipDllPath"

# regsvr32 calls DllRegisterServer, which performs the machine-wide COM
# registration, the TSF profile registration (ITfInputProcessorProfileMgr::
# RegisterProfile) and the keyboard / display-attribute / UI-element category
# registration. There is no manual registry fallback: the previous hand-written
# CLSID\...\Profiles keys were never read by TSF and have been removed.
$result = Start-Process -FilePath "regsvr32.exe" -ArgumentList "/s `"$TipDllPath`"" `
  -Wait -PassThru -NoNewWindow
if ($result.ExitCode -ne 0) {
  throw "regsvr32 failed with exit code $($result.ExitCode). Confirm this is running elevated (administrator)."
}

Write-Host "TSF TIP registration complete (machine-wide)."
