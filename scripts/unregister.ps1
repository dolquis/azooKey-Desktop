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

if (-not $TipDllPath) {
  $TipDllPath = Join-Path $PSScriptRoot "..\build\windows-debug\tsf-tip\azookey_tsf_tip.dll"
}
$TipDllPath = [System.IO.Path]::GetFullPath($TipDllPath)

$clsid = "{71EE04FA-B35D-4EB8-87A1-582D44A9A58C}"

# Per-user step (HKCU, no elevation): remove the inference-host auto-start from
# the *interactive* user's hive, and clean any leftover user-scope CLSID from
# older (pre-machine-wide) builds. Done in the original process so a relaunch
# under a different administrator account cannot target the wrong profile.
if (-not $ElevatedReentry) {
  $runKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run"
  Remove-ItemProperty -Path $runKey -Name "azooKeyInferenceHost" -ErrorAction SilentlyContinue

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

# Belt-and-suspenders: remove any leftover machine-wide COM keys (HKLM).
# DllUnregisterServer already removes this subtree; kept for the case where the
# DLL is missing and regsvr32 /u could not run.
$clsidHklm = "HKLM:\Software\Classes\CLSID\$clsid"
if (Test-Path $clsidHklm) {
  Remove-Item -Path $clsidHklm -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host "TSF TIP unregistered (machine-wide)."
