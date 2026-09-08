# The MSI owns the machine-wide Run entry; this script runs as the logon user.
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "host-startup-log.ps1")
function Invoke-InstalledHost {
  param([string]$Directory, [string]$LocalData)
  $logPath = Join-Path $LocalData "azooKey\logs\host-launcher.log"
  try {
    $registration = Get-ItemProperty -LiteralPath "HKLM:\SOFTWARE\azooKey\HostStartup" `
      -Name "InstallDirectory" -ErrorAction SilentlyContinue
    if ($null -eq $registration -or
        $registration.InstallDirectory.TrimEnd('\') -ine $Directory.TrimEnd('\')) {
      Write-HostStartupLog -Path $logPath -EventName "registry_mismatch"
      return
    }
    Write-HostStartupLog -Path $logPath -EventName "launcher_started"
    & (Join-Path $Directory "host-supervisor.ps1") `
      -HostExePath (Join-Path $Directory "azookey_inference_host.exe") `
      -HostArguments "--pipe --supervisor-pid $PID" `
      -StderrLogPath (Join-Path (Split-Path -Parent $logPath) "inference-host-stderr.log") `
      -InstalledDirectory $Directory *>&1 | ForEach-Object {
        Write-HostStartupLog -Path $logPath -EventName "supervisor_output" -Detail "$_"
      }
  } catch {
    Write-HostStartupLog -Path $logPath -EventName "launcher_failed" -Detail "$($_.Exception.GetType().FullName): $_"
    throw
  }
}
Invoke-InstalledHost -Directory $PSScriptRoot -LocalData ([Environment]::GetFolderPath('LocalApplicationData'))
