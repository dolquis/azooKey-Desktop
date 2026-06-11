param(
  [string]$TipDllPath = ""
)

$ErrorActionPreference = "Continue"

if (-not $TipDllPath) {
  $TipDllPath = Join-Path $PSScriptRoot "..\build\windows-debug\tsf-tip\azookey_tsf_tip.dll"
}
$TipDllPath = [System.IO.Path]::GetFullPath($TipDllPath)

# Unregistration removes the machine-wide HKLM COM keys and the TSF profile, so
# it requires administrator rights. Relaunch elevated if needed.
$principal = New-Object Security.Principal.WindowsPrincipal(
  [Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
  Write-Host "Machine-wide unregistration requires elevation; relaunching as administrator..."
  Start-Process -FilePath "powershell.exe" -Verb RunAs -ArgumentList @(
    "-NoExit", "-ExecutionPolicy", "Bypass",
    "-File", $PSCommandPath,
    "-TipDllPath", $TipDllPath
  )
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

# Belt-and-suspenders: remove any leftover COM keys. DllUnregisterServer already
# removes the HKLM subtree; the HKCU key is cleaned for machines that still have
# the old (pre-machine-wide) user-scope registration from earlier builds.
$clsid = "{71EE04FA-B35D-4EB8-87A1-582D44A9A58C}"
foreach ($root in @("HKLM", "HKCU")) {
  $clsidKey = "${root}:\Software\Classes\CLSID\$clsid"
  if (Test-Path $clsidKey) {
    Remove-Item -Path $clsidKey -Recurse -Force -ErrorAction SilentlyContinue
  }
}

# The inference-host auto-start is per-user (HKCU).
$runKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run"
Remove-ItemProperty -Path $runKey -Name "azooKeyInferenceHost" -ErrorAction SilentlyContinue

Write-Host "TSF TIP unregistered (machine-wide)."
