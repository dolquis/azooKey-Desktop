param(
  [Parameter(Mandatory=$true)]
  [string]$HostExePath,
  [string]$HostArguments = "",
  [string]$ModelPath = "",
  [string]$MockDictionaryPath = "",
  [string]$PipeName = "",
  [string]$StderrLogPath = "",
  # Empty for development registration. MSI removes this registration on uninstall.
  [string]$InstalledDirectory = "",
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

function Test-InstallationActive {
  param([string]$Directory)
  if (-not $Directory) { return $true }
  $registration = Get-ItemProperty -LiteralPath "HKLM:\SOFTWARE\azooKey\HostStartup" `
    -Name "InstallDirectory" -ErrorAction SilentlyContinue
  return $null -ne $registration -and
    $registration.InstallDirectory.TrimEnd('\') -ieq $Directory.TrimEnd('\')
}

function Test-VulkanLoader {
  if (-not ("AzooKey.SupervisorLoader" -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace AzooKey {
  public static class SupervisorLoader {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr LoadLibraryExW(string path, IntPtr file, uint flags);
    [DllImport("kernel32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool FreeLibrary(IntPtr module);
  }
}
'@
  }
  # Drivers install the loader in System32. Never search the current directory.
  $module = [AzooKey.SupervisorLoader]::LoadLibraryExW("vulkan-1.dll", [IntPtr]::Zero, 0x800)
  if ($module -eq [IntPtr]::Zero) { return $false }
  [void][AzooKey.SupervisorLoader]::FreeLibrary($module)
  return $true
}

function Test-VulkanHost {
  param([string]$Path)
  $probe = [Diagnostics.Process]::new()
  try {
    $probe.StartInfo = [Diagnostics.ProcessStartInfo]@{
      FileName = $Path
      Arguments = "--probe-vulkan"
      UseShellExecute = $false
      CreateNoWindow = $true
      RedirectStandardOutput = $true
      RedirectStandardError = $true
    }
    [void]$probe.Start()
    $stdout = $probe.StandardOutput.ReadToEndAsync()
    $stderr = $probe.StandardError.ReadToEndAsync()
    if (-not $probe.WaitForExit(10000)) {
      $probe.Kill()
      $probe.WaitForExit()
      return $false
    }
    $output = $stdout.GetAwaiter().GetResult()
    [void]$stderr.GetAwaiter().GetResult()
    return $probe.ExitCode -eq 0 -and $output.Trim() -match '^vulkan_devices=[1-9][0-9]*$'
  } catch {
    return $false
  } finally {
    $probe.Dispose()
  }
}

function Get-HostSelection {
  param([string]$BasePath)
  $addon = Join-Path (Split-Path -Parent $BasePath) "azookey_inference_host_vulkan.exe"
  $reason = "addon_absent"
  if (Test-Path -LiteralPath $addon -PathType Leaf) {
    $reason = "loader_unavailable"
    $loaderAvailable = $false
    try { $loaderAvailable = Test-VulkanLoader } catch { $loaderAvailable = $false }
    if ($loaderAvailable) {
      $reason = "probe_failed"
      if (Test-VulkanHost -Path $addon) {
        return [pscustomobject]@{ Path = $addon; Reason = "vulkan_devices_available" }
      }
    }
  }
  return [pscustomobject]@{ Path = $BasePath; Reason = $reason }
}

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

function Get-LaunchLogPath {
  param(
    [Parameter(Mandatory=$true)]
    [string]$BasePath,
    [Parameter(Mandatory=$true)]
    [int]$LaunchAttempt
  )

  $directory = Split-Path -Parent $BasePath
  $baseName = [System.IO.Path]::GetFileNameWithoutExtension($BasePath)
  $extension = [System.IO.Path]::GetExtension($BasePath)
  $timestamp = [DateTimeOffset]::UtcNow.ToString(
    "yyyyMMddTHHmmssfffZ",
    [Globalization.CultureInfo]::InvariantCulture)
  $fileName = "$baseName-$timestamp-$LaunchAttempt$extension"
  if ($directory) {
    return Join-Path $directory $fileName
  }
  return $fileName
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
  if ($MockDictionaryPath) {
    $HostArguments += " --mock-dict `"$MockDictionaryPath`""
  }
}

$currentSid = ([Security.Principal.WindowsIdentity]::GetCurrent()).User.Value
if (-not $PipeName) {
  $PipeName = "azookey-$currentSid"
}
if (-not $InstanceKey) {
  $InstanceKey = $currentSid
}

$mutexName = "Global\azooKeyInferenceHostSupervisor-$InstanceKey"
$stopEventName = "Global\azooKeyInferenceHostSupervisorStop-$InstanceKey"
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
  $launchAttemptCount = 0

  while (-not $stopEvent.WaitOne(0) -and (Test-InstallationActive -Directory $InstalledDirectory)) {
    # A host started before this supervisor may already own the per-user pipe.
    # Wait for it to disappear, then take over future restarts.
    while ((Test-PerUserPipe -Name $PipeName) -and -not $stopEvent.WaitOne(500) -and
        (Test-InstallationActive -Directory $InstalledDirectory)) {
      # Intentionally empty.
    }
    if ($stopEvent.WaitOne(0) -or -not (Test-InstallationActive -Directory $InstalledDirectory)) {
      break
    }

    $selection = Get-HostSelection -BasePath $HostExePath
    # Probe can take time. Do not launch after MSI has removed the registration.
    if (-not (Test-InstallationActive -Directory $InstalledDirectory)) { break }
    $startParameters = @{
      FilePath = $selection.Path
      ArgumentList = $HostArguments
      PassThru = $true
      WindowStyle = "Hidden"
    }
    if ($StderrLogPath) {
      $launchAttemptCount++
      $logDirectory = Split-Path -Parent $StderrLogPath
      if ($logDirectory) {
        New-Item -ItemType Directory -Path $logDirectory -Force | Out-Null
      }
      $startParameters.RedirectStandardError = Get-LaunchLogPath `
        -BasePath $StderrLogPath `
        -LaunchAttempt $launchAttemptCount
      # Per-launch sibling records the exact selected executable, including failed starts.
      [pscustomobject]@{
        time = [DateTimeOffset]::UtcNow.ToString("o")
        executable = $selection.Path
        reason = $selection.Reason
      } | ConvertTo-Json -Compress | Set-Content `
        -LiteralPath ($startParameters.RedirectStandardError + ".startup.json") -Encoding UTF8
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
        if ($stopEvent.WaitOne(500) -or -not (Test-InstallationActive -Directory $InstalledDirectory)) {
          # Development keeps the host alive. Installed hosts observe this
          # supervisor's process exit and perform their normal shutdown.
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
