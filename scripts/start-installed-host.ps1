# The MSI owns the machine-wide Run entry; this script runs as the logon user.
$ErrorActionPreference = "Stop"
$registration = Get-ItemProperty -LiteralPath "HKLM:\SOFTWARE\azooKey\HostStartup" `
  -Name "InstallDirectory" -ErrorAction SilentlyContinue
if ($null -eq $registration -or
    $registration.InstallDirectory.TrimEnd('\') -ine $PSScriptRoot.TrimEnd('\')) { return }
$logDirectory = Join-Path $env:LOCALAPPDATA "azooKey\logs"
New-Item -ItemType Directory -Path $logDirectory -Force | Out-Null
$logPath = Join-Path $logDirectory "inference-host-stderr.log"
& (Join-Path $PSScriptRoot "host-supervisor.ps1") `
  -HostExePath (Join-Path $PSScriptRoot "azookey_inference_host.exe") `
  -HostArguments "--pipe --supervisor-pid $PID" `
  -StderrLogPath $logPath `
  -InstalledDirectory $PSScriptRoot
