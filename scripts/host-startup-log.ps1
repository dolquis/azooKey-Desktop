# Shared, best-effort diagnostics. Never make inference depend on a writable log.
function Write-HostStartupLog {
  param([string]$Path, [string]$EventName, [string]$Detail = "")
  if (-not $Path) { return }
  $record = [pscustomobject]@{
    time = [DateTimeOffset]::UtcNow.ToString("o")
    event = $EventName
    detail = $Detail.Substring(0, [Math]::Min($Detail.Length, 4096))
  } | ConvertTo-Json -Compress
  foreach ($target in @($Path, (Join-Path ([IO.Path]::GetTempPath()) "azooKey-host-startup.log"))) {
    try {
      New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
      if ((Test-Path -LiteralPath $target) -and (Get-Item -LiteralPath $target).Length -ge 1MB) {
        Move-Item -LiteralPath $target -Destination ($target + ".1") -Force
      }
      Add-Content -LiteralPath $target -Value $record -Encoding UTF8
      return
    } catch { continue }
  }
}

function Remove-OldHostLaunchLog {
  [CmdletBinding(SupportsShouldProcess)]
  param([string]$BasePath, [int]$Keep = 20)
  if (-not $BasePath) { return }
  $directory = Split-Path -Parent $BasePath
  $name = [regex]::Escape([IO.Path]::GetFileNameWithoutExtension($BasePath))
  $extension = [regex]::Escape([IO.Path]::GetExtension($BasePath))
  try {
    # Only completed per-launch files with our exact timestamp/counter format.
    $files = @(Get-ChildItem -LiteralPath $directory -File |
      Where-Object { $_.Name -match "^$name-\d{8}T\d{9}Z-\d+$extension$" } |
      Sort-Object Name -Descending | Select-Object -Skip $Keep)
    foreach ($file in $files) {
      if (-not $PSCmdlet.ShouldProcess($file.FullName, 'Remove expired startup log')) { continue }
      Remove-Item -LiteralPath $file.FullName -ErrorAction Stop
      $sidecar = $file.FullName + ".startup.json"
      if (Test-Path -LiteralPath $sidecar) { Remove-Item -LiteralPath $sidecar -ErrorAction Stop }
    }
  } catch { return }
}
