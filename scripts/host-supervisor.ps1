param(
  [Parameter(Mandatory=$true)]
  [string]$HostExePath,
  [string]$HostArguments = "",
  [string]$ModelPath = "",
  [string]$PipeName = "",
  [string]$StderrLogPath = "",
  [ValidateRange(1, 60000)]
  [int]$RestartDelayMinMs = 250,
  [ValidateRange(1, 60000)]
  [int]$RestartDelayMaxMs = 3000,
  [ValidateRange(1, 3600)]
  [int]$StableRunSeconds = 30,
  # Test seam: zero means unlimited supervision.
  [ValidateRange(0, 100)]
  [int]$MaxLaunchCount = 0,
  # Test seam: production uses the current-user SID.
  [ValidatePattern('^[A-Za-z0-9._-]*$')]
  [string]$InstanceKey = ""
)

$ErrorActionPreference = "Stop"

function Test-PerUserPipe {
  param(
    [Parameter(Mandatory=$true)]
    [string]$Name
  )

  try {
    return [bool]([System.IO.Directory]::GetFiles("\\.\pipe\") |
      Where-Object { [System.IO.Path]::GetFileName($_) -eq $Name })
  } catch {
    return $false
  }
}

if (-not (Test-Path -LiteralPath $HostExePath -PathType Leaf)) {
  throw "Inference host not found: $HostExePath"
}
if ($RestartDelayMinMs -gt $RestartDelayMaxMs) {
  throw "RestartDelayMinMs must not exceed RestartDelayMaxMs."
}
if (-not $HostArguments) {
  $HostArguments = "--pipe"
  if ($ModelPath) {
    $HostArguments += " --model `"$ModelPath`""
  }
}

$currentSid = ([Security.Principal.WindowsIdentity]::GetCurrent()).User.Value
if (-not $PipeName) {
  $PipeName = "azookey-$currentSid"
}
if (-not $InstanceKey) {
  $InstanceKey = $currentSid
}

$mutexName = "Local\azooKeyInferenceHostSupervisor-$InstanceKey"
$stopEventName = "Local\azooKeyInferenceHostSupervisorStop-$InstanceKey"
$mutex = [System.Threading.Mutex]::new($false, $mutexName)
$ownsMutex = $false
$stopEvent = $null

try {
  try {
    $ownsMutex = $mutex.WaitOne(0, $false)
  } catch [System.Threading.AbandonedMutexException] {
    $ownsMutex = $true
  }
  if (-not $ownsMutex) {
    return
  }

  $eventCreated = $false
  $stopEvent = [System.Threading.EventWaitHandle]::new(
    $false,
    [System.Threading.EventResetMode]::ManualReset,
    $stopEventName,
    [ref]$eventCreated)

  $restartDelayMs = $RestartDelayMinMs
  $launchCount = 0

  while (-not $stopEvent.WaitOne(0)) {
    # A host started before this supervisor may already own the per-user pipe.
    # Wait for it to disappear, then take over future restarts.
    while ((Test-PerUserPipe -Name $PipeName) -and -not $stopEvent.WaitOne(500)) {
      # Intentionally empty.
    }
    if ($stopEvent.WaitOne(0)) {
      break
    }

    $startParameters = @{
      FilePath = $HostExePath
      ArgumentList = $HostArguments
      PassThru = $true
      WindowStyle = "Hidden"
    }
    if ($StderrLogPath) {
      $logDirectory = Split-Path -Parent $StderrLogPath
      if ($logDirectory) {
        New-Item -ItemType Directory -Path $logDirectory -Force | Out-Null
      }
      $startParameters.RedirectStandardError = $StderrLogPath
    }

    $startedAt = [DateTimeOffset]::UtcNow
    try {
      $hostProcess = Start-Process @startParameters
      $launchCount++
    } catch {
      Write-Warning "Could not start inference host: $_"
      if ($stopEvent.WaitOne($restartDelayMs)) {
        break
      }
      $restartDelayMs = [Math]::Min($restartDelayMs * 2, $RestartDelayMaxMs)
      continue
    }

    try {
      while (-not $hostProcess.HasExited) {
        if ($stopEvent.WaitOne(500)) {
          # Unregistration stops supervision but preserves the current host,
          # matching the previous unregister-dev.ps1 behavior.
          return
        }
        $hostProcess.Refresh()
      }
    } finally {
      $hostProcess.Dispose()
    }

    if ($MaxLaunchCount -gt 0 -and $launchCount -ge $MaxLaunchCount) {
      break
    }

    $runtime = [DateTimeOffset]::UtcNow - $startedAt
    if ($runtime.TotalSeconds -ge $StableRunSeconds) {
      $restartDelayMs = $RestartDelayMinMs
    } else {
      $restartDelayMs = [Math]::Min($restartDelayMs * 2, $RestartDelayMaxMs)
    }

    if ($stopEvent.WaitOne($restartDelayMs)) {
      break
    }
  }
} finally {
  if ($stopEvent) {
    $stopEvent.Dispose()
  }
  if ($ownsMutex) {
    $mutex.ReleaseMutex()
  }
  $mutex.Dispose()
}
