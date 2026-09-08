# MSI runs this after removing its startup registry values, before RemoveFiles.
# Never terminate a process by name: direct/dev hosts are not owned by the MSI.
param(
  [ValidateRange(1, 120)]
  [int]$TimeoutSeconds = 30
)
$ErrorActionPreference = "Stop"
function Wait-InstalledHostExit {
  param([string]$Directory, [int]$TimeoutSeconds)
  $paths = @(
    (Join-Path $Directory "azookey_inference_host.exe"),
    (Join-Path $Directory "azookey_inference_host_vulkan.exe")
  )
  $deadline = [DateTimeOffset]::UtcNow.AddSeconds($TimeoutSeconds)
  $launcherPattern = '(?i)(?:^|\s)-File\s+"' +
    [regex]::Escape((Join-Path $Directory "start-installed-host.ps1")) + '"(?:\s|$)'
  do {
    $running = @(Get-CimInstance Win32_Process -Filter `
      "Name = 'azookey_inference_host.exe' OR Name = 'azookey_inference_host_vulkan.exe' OR Name = 'powershell.exe'" |
      Where-Object { $_.ExecutablePath -in $paths -or $_.CommandLine -match $launcherPattern })
    if ($running.Count -eq 0) { return }
    Start-Sleep -Milliseconds 200
  } while ([DateTimeOffset]::UtcNow -lt $deadline)
  throw "An installed inference host did not exit. Close it and retry maintenance."
}

Wait-InstalledHostExit -Directory $PSScriptRoot -TimeoutSeconds $TimeoutSeconds
