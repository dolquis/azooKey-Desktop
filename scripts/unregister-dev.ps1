param(
  [string]$TipDllPath = "",
  # Internal: set when the script relaunches itself elevated. Per-user (HKCU)
  # cleanup runs in the original user's process *before* elevation, so the
  # elevated reentry skips it — otherwise a relaunch under a separate
  # administrator account would clean that administrator's hive, not the
  # interactive user's.
  [switch]$ElevatedReentry
)

$ErrorActionPreference = "Continue"

function Resolve-DevPath {
  param(
    [Parameter(Mandatory=$true)]
    [string]$Path
  )

  return $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path)
}

if (-not $TipDllPath) {
  $TipDllPath = Join-Path $PSScriptRoot "..\build\windows-debug\tsf-tip\azookey_tsf_tip.dll"
}
$TipDllPath = Resolve-DevPath $TipDllPath

$clsid = "{71EE04FA-B35D-4EB8-87A1-582D44A9A58C}"

# Per-user step (HKCU, no elevation): remove the inference-host auto-start from
# the *interactive* user's hive, and clean any leftover user-scope CLSID from
# older (pre-machine-wide) builds. Done in the original process so a relaunch
# under a different administrator account cannot target the wrong profile.
if (-not $ElevatedReentry) {
  $runKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run"
  Remove-ItemProperty -Path $runKey -Name "azooKeyInferenceHost" -ErrorAction SilentlyContinue

  # Stop restart supervision for this user without terminating the currently
  # running host. The host keeps its existing lifetime, matching historical
  # unregister behavior, but will no longer be respawned after it exits.
  $mySid = ([Security.Principal.WindowsIdentity]::GetCurrent()).User.Value
  $stopEventName = "Local\azooKeyInferenceHostSupervisorStop-$mySid"
  try {
    $stopEvent = [System.Threading.EventWaitHandle]::OpenExisting($stopEventName)
    $stopEvent.Set() | Out-Null
    $stopEvent.Dispose()
  } catch [System.Threading.WaitHandleCannotBeOpenedException] {
    Write-Verbose "No inference host supervisor is running for the current user."
  }

  $clsidHkcu = "HKCU:\Software\Classes\CLSID\$clsid"
  if (Test-Path $clsidHkcu) {
    Remove-Item -Path $clsidHkcu -Recurse -Force -ErrorAction SilentlyContinue
  }
}

# Machine-wide step (HKLM COM + TSF profile): requires elevation. Relaunch
# elevated if needed, forwarding the absolute path as a single quoted argument
# string so spaces survive the relaunch.
$principal = New-Object Security.Principal.WindowsPrincipal(
  [Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
  Write-Host "Machine-wide unregistration requires elevation; relaunching as administrator..."
  $relaunchArgs = "-NoExit -ExecutionPolicy Bypass -File `"$PSCommandPath`" " +
                  "-TipDllPath `"$TipDllPath`" -ElevatedReentry"
  Start-Process -FilePath "powershell.exe" -Verb RunAs -ArgumentList $relaunchArgs
  return
}

# regsvr32 /u calls DllUnregisterServer, which unregisters the TSF profile and
# categories and removes the machine-wide HKLM CLSID subtree.
if (Test-Path $TipDllPath) {
  Write-Host "Unregistering TIP DLL (machine-wide): $TipDllPath"
  $result = Start-Process -FilePath "regsvr32.exe" -ArgumentList "/u /s `"$TipDllPath`"" `
    -Wait -PassThru -NoNewWindow
  if ($result.ExitCode -ne 0) {
    Write-Warning "regsvr32 /u returned exit code $($result.ExitCode)."
  }
} else {
  Write-Warning "TIP DLL not found, skipping regsvr32 /u: $TipDllPath"
}

# Belt-and-suspenders: remove leftover machine-wide registration directly.
# DllUnregisterServer already removes these via the TSF APIs, but when the DLL is
# missing (build cleaned / path changed) regsvr32 /u cannot run — so also delete
# the TSF profile + category registration under CTF\TIP, not just the COM CLSID
# subtree. Otherwise the language profile is orphaned and Windows keeps showing /
# resolving a broken azooKey input method. Both the native and WOW6432Node views
# are cleaned; Remove-Item tolerates absent keys.
$leftovers = @(
  "HKLM:\Software\Classes\CLSID\$clsid",
  "HKLM:\Software\Microsoft\CTF\TIP\$clsid",
  "HKLM:\Software\WOW6432Node\Classes\CLSID\$clsid",
  "HKLM:\Software\WOW6432Node\Microsoft\CTF\TIP\$clsid"
)
foreach ($key in $leftovers) {
  if (Test-Path $key) {
    Remove-Item -Path $key -Recurse -Force -ErrorAction SilentlyContinue
  }
}

Write-Host "TSF TIP unregistered (machine-wide)."
