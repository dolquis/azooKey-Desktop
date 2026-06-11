param(
  [string]$TipDllPath = "",
  [string]$HostExePath = ""
)

$ErrorActionPreference = "Stop"

# Resolve default paths relative to the script location, then make them absolute
# *before* any elevation relaunch: the elevated process starts in a different
# working directory, so relative paths would otherwise resolve incorrectly.
if (-not $TipDllPath) {
  $TipDllPath = Join-Path $PSScriptRoot "..\build\windows-debug\tsf-tip\azookey_tsf_tip.dll"
}
if (-not $HostExePath) {
  $HostExePath = Join-Path $PSScriptRoot "..\build\windows-debug\inference-host\azookey_inference_host.exe"
}

$TipDllPath  = [System.IO.Path]::GetFullPath($TipDllPath)
$HostExePath = [System.IO.Path]::GetFullPath($HostExePath)

# Machine-wide TSF registration writes the COM CLSID under HKLM and the TSF
# profile under HKLM\...\CTF\TIP, both of which require administrator rights.
# TSF has no persistent per-user TIP registration (see
# docs/sideload-packaging-spec.md §1), so elevation is mandatory. Relaunch
# elevated if needed, forwarding the already-resolved (absolute) arguments.
$principal = New-Object Security.Principal.WindowsPrincipal(
  [Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
  Write-Host "Machine-wide registration requires elevation; relaunching as administrator..."
  Start-Process -FilePath "powershell.exe" -Verb RunAs -ArgumentList @(
    "-NoExit", "-ExecutionPolicy", "Bypass",
    "-File", $PSCommandPath,
    "-TipDllPath", $TipDllPath,
    "-HostExePath", $HostExePath
  )
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

# Register inference host for auto-start (best-effort). This is intentionally
# per-user (HKCU Run): the host runs in the interactive user's session. On a
# multi-user machine each user re-runs this step; machine-wide host auto-start
# is deferred to the installer / MSIX work.
if (Test-Path $HostExePath) {
  $runKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run"
  try {
    New-ItemProperty -Path $runKey -Name "azooKeyInferenceHost" `
      -Value "`"$HostExePath`" --pipe" -PropertyType String -Force | Out-Null
    Write-Host "Inference host auto-start registered: $HostExePath"
  } catch {
    Write-Warning "Could not register inference host auto-start: $_"
  }
} else {
  Write-Warning "Inference host not found, skipping auto-start registration: $HostExePath"
}

Write-Host "TSF TIP registration complete (machine-wide)."
