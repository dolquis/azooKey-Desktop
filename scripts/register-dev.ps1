param(
  [string]$TipDllPath = "",
  [string]$HostExePath = "",
  # Per-user host argument only. Elevated reentry skips HKCU setup and host start.
  [string]$ModelPath = "",
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

  $zenzaiModel = [Environment]::GetEnvironmentVariable("AZOOKEY_ZENZAI_MODEL", "Process")
  try {
    [Environment]::SetEnvironmentVariable("AZOOKEY_ZENZAI_MODEL", $null, "Process")
    $output = & $benchPath 2>&1
    $exitCode = $LASTEXITCODE
  } finally {
    [Environment]::SetEnvironmentVariable("AZOOKEY_ZENZAI_MODEL", $zenzaiModel, "Process")
  }
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
if ($ModelPath) {
  $ModelPath = Resolve-DevPath $ModelPath
  if (-not (Test-Path -LiteralPath $ModelPath -PathType Leaf)) {
    throw "Zenzai model not found: $ModelPath"
  }
  if ([System.IO.Path]::GetExtension($ModelPath) -ine ".gguf") {
    throw "Zenzai model must be a GGUF file: $ModelPath"
  }
  if ($AllowMockHost) {
    throw "-ModelPath cannot be combined with -AllowMockHost because a mock host cannot produce real Zenzai candidates."
  }
}

if (-not $ElevatedReentry) {
  Assert-LlamaEnabledHost -Path $HostExePath -AllowMock:$AllowMockHost
}

$supervisorScriptPath = Resolve-DevPath (Join-Path $PSScriptRoot "host-supervisor.ps1")
if (-not (Test-Path -LiteralPath $supervisorScriptPath -PathType Leaf)) {
  throw "Inference host supervisor not found: $supervisorScriptPath"
}

# Per-user step (HKCU, no elevation needed): register the inference host for
# auto-start in the *interactive* user's hive. Done in the original process so a
# relaunch under a different administrator account cannot write it to the wrong
# profile. Best-effort. Skipped on the elevated reentry.
if (-not $ElevatedReentry) {
  if (Test-Path $HostExePath) {
    # Probe the exact per-user pipe before changing the Run entry. If a model was
    # requested, continuing with an already-running host would leave the current
    # session on its previous model configuration while reporting registration
    # success. Require the caller to stop that host and retry instead.
    $mySid = ([Security.Principal.WindowsIdentity]::GetCurrent()).User.Value
    $myPipe = "azookey-$mySid"
    $hostServing = $false
    try {
      $hostServing = [bool]([System.IO.Directory]::GetFiles("\\.\pipe\") |
        Where-Object { [System.IO.Path]::GetFileName($_) -eq $myPipe })
    } catch {
      $hostServing = $false
    }
    if ($ModelPath -and $hostServing) {
      throw "Inference host already serving this user's pipe ($myPipe). Stop it and rerun with -ModelPath so the current session uses the requested model."
    }

    $hostStderrLog = $null
    $supervisorStderrLog = $null
    try {
      $hostLogDir = Join-Path (Join-Path $env:LOCALAPPDATA "azooKey") "logs"
      $hostStderrLog = Join-Path $hostLogDir "inference-host-stderr.log"
      $supervisorStderrLog = Join-Path $hostLogDir "inference-host-supervisor-stderr.log"
      New-Item -ItemType Directory -Path $hostLogDir -Force | Out-Null
    } catch {
      Write-Warning "Could not create inference host log directory: $_"
    }

    $powerShellExe = (Get-Process -Id $PID).Path
    $supervisorArguments = "-NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass " +
      "-File `"$supervisorScriptPath`" -HostExePath `"$HostExePath`" " +
      "-PipeName `"$myPipe`""
    if ($ModelPath) {
      $supervisorArguments += " -ModelPath `"$ModelPath`""
    }
    if ($hostStderrLog) {
      $supervisorArguments += " -StderrLogPath `"$hostStderrLog`""
    }

    $runKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run"
    try {
      New-ItemProperty -Path $runKey -Name "azooKeyInferenceHost" `
        -Value "`"$powerShellExe`" $supervisorArguments" -PropertyType String -Force | Out-Null
      Write-Host "Inference host supervisor auto-start registered (current user): $supervisorScriptPath"
    } catch {
      Write-Warning "Could not register inference host supervisor auto-start: $_"
    }

    # Start the supervisor for the *current* session as well. The HKCU Run entry above
    # only takes effect at the next logon, so without this the first verification
    # right after registration sees a live TIP (preedit works) but no candidates
    # (Space returns nothing) because nothing is serving the per-user pipe yet.
    # Started here, in the original (non-elevated) process, so it runs as the
    # interactive user — same rationale as the Run entry. The supervisor owns
    # host restart responsibility and waits for an already-serving host before
    # taking over, so registration never creates a duplicate pipe server.
    #
    try {
      $startParameters = @{
        FilePath = $powerShellExe
        ArgumentList = $supervisorArguments
        WindowStyle = "Hidden"
      }
      if ($supervisorStderrLog) {
        $startParameters.RedirectStandardError = $supervisorStderrLog
      }
      Start-Process @startParameters
      Write-Host "Inference host supervisor started for current session: $supervisorScriptPath"
      if ($hostServing) {
        Write-Host "Inference host already serving this user's pipe ($myPipe); supervisor will take over after it exits."
      }
      if ($hostStderrLog) {
        Write-Host "Inference host stderr log: $hostStderrLog"
      }
    } catch {
      Write-Warning "Could not start inference host supervisor for current session: $_"
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
