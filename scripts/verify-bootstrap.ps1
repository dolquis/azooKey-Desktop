#requires -Version 5.1
param(
  [switch]$Json,
  [string]$TipDllPath = "",
  [string]$HostExePath = "",
  [string]$RuntimeInstallerPath = "",
  [string]$ModelPath = "",
  [string]$BenchPath = "",
  [string]$MockDictionaryPath = "",
  [switch]$CheckpointConfirmed
)

$ErrorActionPreference = "Stop"
$script:VmVerifyTextServiceClsid = "{71EE04FA-B35D-4EB8-87A1-582D44A9A58C}"

if (-not ("AzooKey.VmVerifyNativeMethods" -as [type])) {
  Add-Type -TypeDefinition @'
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace AzooKey {
  public static class VmVerifyNativeMethods {
    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool GetNamedPipeServerProcessId(
      SafePipeHandle pipe,
      out uint serverProcessId);
  }
}
'@
}

function Get-VmVerifyBootstrapPath {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path
  )

  return $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path)
}

function Get-VmVerifyManifestPayloadPath {
  param(
    [Parameter(Mandatory = $true)]
    [string]$PackageRoot,
    [Parameter(Mandatory = $true)]
    [pscustomobject]$Manifest,
    [Parameter(Mandatory = $true)]
    [string]$Role
  )

  $payloadMatches = @($Manifest.files | Where-Object { $_.role -eq $Role })
  if ($payloadMatches.Count -eq 0) {
    return ""
  }
  if ($payloadMatches.Count -ne 1) {
    throw "VM verification manifest contains multiple '$Role' payloads."
  }

  $root = (Resolve-Path -LiteralPath $PackageRoot).Path.TrimEnd("\")
  $candidate = Get-VmVerifyBootstrapPath -Path (
    Join-Path $root ([string]$payloadMatches[0].path).Replace("/", "\"))
  $prefix = $root + [System.IO.Path]::DirectorySeparatorChar
  if (-not $candidate.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw "VM verification manifest payload escapes the package root: $($payloadMatches[0].path)"
  }
  return $candidate
}

function Get-VmVerifyCheck {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Id,
    [Parameter(Mandatory = $true)]
    [ValidateSet("pass", "fail", "manual_required", "not_applicable")]
    [string]$Status,
    [Parameter(Mandatory = $true)]
    [string]$Message
  )

  return [pscustomobject][ordered]@{
    id = $Id
    status = $Status
    message = $Message
  }
}

function Test-VmVerifyAdministrator {
  $principal = New-Object Security.Principal.WindowsPrincipal(
    [Security.Principal.WindowsIdentity]::GetCurrent())
  return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Test-VmVerifyVCRuntime {
  $systemDirectory = [Environment]::GetFolderPath("System")
  return (
    (Test-Path -LiteralPath (Join-Path $systemDirectory "vcruntime140.dll") -PathType Leaf) -and
    (Test-Path -LiteralPath (Join-Path $systemDirectory "msvcp140.dll") -PathType Leaf)
  )
}

function Install-VmVerifyVCRuntime {
  param(
    [Parameter(Mandatory = $true)]
    [string]$InstallerPath
  )

  $startParameters = @{
    FilePath = $InstallerPath
    ArgumentList = "/install /quiet /norestart"
    Wait = $true
    PassThru = $true
  }
  if (-not (Test-VmVerifyAdministrator)) {
    $startParameters.Verb = "RunAs"
  }
  $process = Start-Process @startParameters
  if ($process.ExitCode -notin @(0, 1638, 3010)) {
    throw "VC++ Redistributable installer failed with exit code $($process.ExitCode)."
  }
}

function Get-VmVerifyPipeName {
  $sid = ([Security.Principal.WindowsIdentity]::GetCurrent()).User.Value
  return "azookey-$sid"
}

function Test-VmVerifyPipe {
  param(
    [Parameter(Mandatory = $true)]
    [string]$PipeName
  )

  try {
    return [bool]([System.IO.Directory]::GetFiles("\\.\pipe\") |
      Where-Object { [System.IO.Path]::GetFileName($_) -eq $PipeName })
  } catch {
    return $false
  }
}

function Wait-VmVerifyPipe {
  param(
    [Parameter(Mandatory = $true)]
    [string]$PipeName,
    [ValidateRange(1, 120)]
    [int]$TimeoutSeconds = 15,
    [bool]$ExpectedPresent = $true
  )

  $deadline = [DateTimeOffset]::UtcNow.AddSeconds($TimeoutSeconds)
  do {
    if ((Test-VmVerifyPipe -PipeName $PipeName) -eq $ExpectedPresent) {
      return $true
    }
    Start-Sleep -Milliseconds 200
  } while ([DateTimeOffset]::UtcNow -lt $deadline)
  return $false
}

function Get-VmVerifyNamedPipeServerProcessId {
  param(
    [Parameter(Mandatory = $true)]
    [string]$PipeName,
    [int]$ConnectTimeoutMs = 1000
  )

  $pipe = [IO.Pipes.NamedPipeClientStream]::new(
    ".",
    $PipeName,
    [IO.Pipes.PipeDirection]::InOut,
    [IO.Pipes.PipeOptions]::None)
  try {
    $pipe.Connect($ConnectTimeoutMs)
    [uint32]$serverProcessId = 0
    if (-not [AzooKey.VmVerifyNativeMethods]::GetNamedPipeServerProcessId(
        $pipe.SafePipeHandle,
        [ref]$serverProcessId)) {
      $errorCode = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
      throw "GetNamedPipeServerProcessId failed with Win32 error $errorCode."
    }
    return [int]$serverProcessId
  } finally {
    $pipe.Dispose()
  }
}

function Get-VmVerifyServingHostProcess {
  param(
    [Parameter(Mandatory = $true)]
    [string]$PipeName
  )

  $serverProcessId = Get-VmVerifyNamedPipeServerProcessId -PipeName $PipeName
  $hostProcess = Get-Process -Id $serverProcessId -ErrorAction Stop
  $processPath = [string]$hostProcess.Path
  if (-not $processPath) {
    throw "Could not read the executable path for pipe server process $serverProcessId."
  }
  return [pscustomobject][ordered]@{
    Id = [int]$hostProcess.Id
    Path = $processPath
  }
}

function Get-VmVerifyServingHostBinary {
  param(
    [Parameter(Mandatory = $true)]
    [string]$PipeName,
    [Parameter(Mandatory = $true)]
    [string]$HostExe,
    [pscustomobject]$Manifest
  )

  $result = [pscustomobject][ordered]@{
    status = "unverified"
    reason = ""
    processId = 0
    runningPath = ""
    runningSha256 = ""
    expectedPath = $HostExe
    expectedSha256 = ""
  }

  if (-not (Test-VmVerifyPipe -PipeName $PipeName)) {
    $result.reason = "The inference host pipe is not serving."
    return $result
  }
  if (-not $Manifest) {
    $result.reason = "manifest.json is unavailable."
    return $result
  }

  $hostEntries = @($Manifest.files | Where-Object { $_.role -eq "inference-host" })
  if ($hostEntries.Count -ne 1 -or -not [string]$hostEntries[0].sha256) {
    $result.reason = "manifest.json does not contain one inference-host hash."
    return $result
  }

  try {
    $expectedHash = (Get-FileHash -LiteralPath $HostExe -Algorithm SHA256).Hash.ToLowerInvariant()
    $result.expectedSha256 = ([string]$hostEntries[0].sha256).ToLowerInvariant()
    if ($expectedHash -ne $result.expectedSha256) {
      $result.reason = "The bundled host hash does not match manifest.json."
      return $result
    }

    $hostProcess = Get-VmVerifyServingHostProcess -PipeName $PipeName
    $result.processId = [int]$hostProcess.Id
    $result.runningPath = [string]$hostProcess.Path
    $result.runningSha256 = (Get-FileHash `
        -LiteralPath $result.runningPath `
        -Algorithm SHA256).Hash.ToLowerInvariant()
    $runningPath = [IO.Path]::GetFullPath($result.runningPath)
    $expectedPath = [IO.Path]::GetFullPath($result.expectedPath)
    $samePath = [string]::Equals(
      $runningPath,
      $expectedPath,
      [StringComparison]::OrdinalIgnoreCase)
    if ($samePath -and $result.runningSha256 -eq $result.expectedSha256) {
      $result.status = "matched"
      $result.reason = "The serving host matches the bundled inference host."
    } else {
      $result.status = "mismatched"
      $result.reason = "The serving host path or hash differs from the bundled inference host."
    }
  } catch {
    $result.status = "unverified"
    $result.reason = $_.Exception.Message
  }
  return $result
}

function Invoke-VmVerifyHostSupervisorShutdown {
  $sid = ([Security.Principal.WindowsIdentity]::GetCurrent()).User.Value
  $eventName = "Global\azooKeyInferenceHostSupervisorStop-$sid"
  $stopEvent = $null
  try {
    $stopEvent = [System.Threading.EventWaitHandle]::OpenExisting($eventName)
    [void]$stopEvent.Set()
  } catch [System.Threading.WaitHandleCannotBeOpenedException] {
    # No supervisor currently owns the per-user instance.
    $stopEvent = $null
  } finally {
    if ($stopEvent) {
      $stopEvent.Dispose()
    }
  }
}

function Wait-VmVerifyHostSupervisorStopped {
  param(
    [ValidateRange(1, 120)]
    [int]$TimeoutSeconds = 5
  )

  $sid = ([Security.Principal.WindowsIdentity]::GetCurrent()).User.Value
  $mutexName = "Global\azooKeyInferenceHostSupervisor-$sid"
  $mutex = [System.Threading.Mutex]::new($false, $mutexName)
  $ownsMutex = $false
  try {
    try {
      $ownsMutex = $mutex.WaitOne([TimeSpan]::FromSeconds($TimeoutSeconds), $false)
    } catch [System.Threading.AbandonedMutexException] {
      $ownsMutex = $true
    }
    return $ownsMutex
  } finally {
    if ($ownsMutex) {
      $mutex.ReleaseMutex()
    }
    $mutex.Dispose()
  }
}

function Invoke-VmVerifyHostProcessTermination {
  param(
    [Parameter(Mandatory = $true)]
    [int]$ProcessId
  )

  $process = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
  if (-not $process) {
    return
  }
  $process.Kill()
  if (-not $process.WaitForExit(5000)) {
    throw "Inference host process $ProcessId did not stop within the timeout."
  }
}

function Test-VmVerifyRegistration {
  param(
    [Parameter(Mandatory = $true)]
    [string]$ExpectedTipDllPath
  )

  $registryPath = "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Classes\CLSID\" +
    "$script:VmVerifyTextServiceClsid\InprocServer32"
  try {
    $registeredPath = (Get-Item -LiteralPath $registryPath -ErrorAction Stop).
      GetValue($null, "", "DoNotExpandEnvironmentNames")
    if (-not $registeredPath) {
      return $false
    }
    return [string]::Equals(
      (Get-VmVerifyBootstrapPath -Path $registeredPath).TrimEnd("\"),
      (Get-VmVerifyBootstrapPath -Path $ExpectedTipDllPath).TrimEnd("\"),
      [StringComparison]::OrdinalIgnoreCase)
  } catch {
    return $false
  }
}

function Test-VmVerifyMicrosoftIme {
  $microsoftImeTip = "{03B5835F-F03C-411B-9CE2-AA23E1171E36}" +
    "{A76C93D9-5523-4E90-AAFA-4DB112F9AC76}"
  try {
    foreach ($language in @(Get-WinUserLanguageList)) {
      foreach ($tip in @($language.InputMethodTips)) {
        if ([string]$tip -match [regex]::Escape($microsoftImeTip)) {
          return $true
        }
      }
    }
  } catch {
    return $false
  }
  return $false
}

function Invoke-VmVerifyRegistration {
  param(
    [Parameter(Mandatory = $true)]
    [string]$RegisterScriptPath,
    [Parameter(Mandatory = $true)]
    [string]$TipDll,
    [Parameter(Mandatory = $true)]
    [string]$HostExe,
    [string]$Model = "",
    [string]$Bench = "",
    [string]$MockDictionary = ""
  )

  $parameters = @{
    TipDllPath = $TipDll
    HostExePath = $HostExe
  }
  if ($Model) {
    $parameters.ModelPath = $Model
    $parameters.BenchPath = $Bench
  } else {
    # windows-release intentionally uses the fallback converter unless a
    # llama.cpp-enabled preset and explicit GGUF model were packaged.
    $parameters.AllowMockHost = $true
  }
  if ($MockDictionary) {
    $parameters.MockDictionaryPath = $MockDictionary
  }

  # Capture all streams so -Json stdout remains machine-readable, while still
  # retaining enough context to diagnose non-zero registration exits.
  $global:LASTEXITCODE = 0
  $output = @(& $RegisterScriptPath @parameters *>&1)
  $exitCode = $LASTEXITCODE
  $outputText = (($output | ForEach-Object { "$_" }) |
    Select-Object -Last 12) -join [Environment]::NewLine
  if ($exitCode -ne 0) {
    throw ("register-dev.ps1 failed with exit code $exitCode." +
      $(if ($outputText) { " Output: $outputText" } else { "" }))
  }
  return $outputText
}

function Invoke-VmVerifyHostSupervisor {
  param(
    [Parameter(Mandatory = $true)]
    [string]$SupervisorScriptPath,
    [Parameter(Mandatory = $true)]
    [string]$HostExe,
    [Parameter(Mandatory = $true)]
    [string]$PipeName,
    [string]$Model = "",
    [string]$MockDictionary = ""
  )

  $logDirectory = Join-Path (Join-Path $env:LOCALAPPDATA "azooKey") "logs"
  New-Item -ItemType Directory -Path $logDirectory -Force | Out-Null
  $timestamp = [DateTimeOffset]::UtcNow.ToString("yyyyMMdd-HHmmss-fff")
  $hostLogPath = Join-Path $logDirectory "inference-host-stderr.log"
  $supervisorLogPath = Join-Path $logDirectory (
    "inference-host-supervisor-stderr-$timestamp.log")
  $powerShellExe = (Get-Process -Id $PID).Path
  $arguments = "-NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass " +
    "-File `"$SupervisorScriptPath`" -HostExePath `"$HostExe`" " +
    "-PipeName `"$PipeName`" -StderrLogPath `"$hostLogPath`""
  if ($Model) {
    $arguments += " -ModelPath `"$Model`""
  }
  if ($MockDictionary) {
    $arguments += " -MockDictionaryPath `"$MockDictionary`""
  }

  Start-Process `
    -FilePath $powerShellExe `
    -ArgumentList $arguments `
    -WindowStyle Hidden `
    -RedirectStandardError $supervisorLogPath | Out-Null
}

function Invoke-VmVerifyBootstrap {
  param(
    [Parameter(Mandatory = $true)]
    [string]$PackageRoot,
    [string]$TipDll = "",
    [string]$HostExe = "",
    [string]$RuntimeInstaller = "",
    [string]$Model = "",
    [string]$Bench = "",
    [string]$MockDictionary = "",
    [switch]$HasCheckpoint
  )

  $root = (Resolve-Path -LiteralPath $PackageRoot).Path
  if (-not $TipDll) {
    $TipDll = Join-Path $root "azookey_tsf_tip.dll"
  }
  if (-not $HostExe) {
    $HostExe = Join-Path $root "azookey_inference_host.exe"
  }
  if (-not $RuntimeInstaller) {
    $RuntimeInstaller = Join-Path $root "vc_redist.x64.exe"
  }
  $registerScript = Join-Path $root "register-dev.ps1"
  $supervisorScript = Join-Path $root "host-supervisor.ps1"
  $manifestPath = Join-Path $root "manifest.json"

  $package = [pscustomobject][ordered]@{
    commit = ""
    preset = ""
    buildType = ""
  }
  $manifest = $null
  if (Test-Path -LiteralPath $manifestPath -PathType Leaf) {
    $manifest = Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
    $package.commit = [string]$manifest.commit
    $package.preset = [string]$manifest.preset
    $package.buildType = [string]$manifest.buildType
  }

  if ($manifest -and -not $Model) {
    $Model = Get-VmVerifyManifestPayloadPath `
      -PackageRoot $root `
      -Manifest $manifest `
      -Role "gguf-model"
  }
  if ($manifest -and -not $Bench) {
    $Bench = Get-VmVerifyManifestPayloadPath `
      -PackageRoot $root `
      -Manifest $manifest `
      -Role "llama-preflight"
  }
  if ($manifest -and -not $MockDictionary) {
    $MockDictionary = Get-VmVerifyManifestPayloadPath `
      -PackageRoot $root `
      -Manifest $manifest `
      -Role "mock-dictionary"
  }

  $TipDll = Get-VmVerifyBootstrapPath -Path $TipDll
  $HostExe = Get-VmVerifyBootstrapPath -Path $HostExe
  $RuntimeInstaller = Get-VmVerifyBootstrapPath -Path $RuntimeInstaller
  if ($Model) {
    $Model = Get-VmVerifyBootstrapPath -Path $Model
  }
  if ($Bench) {
    $Bench = Get-VmVerifyBootstrapPath -Path $Bench
  }
  if ($MockDictionary) {
    $MockDictionary = Get-VmVerifyBootstrapPath -Path $MockDictionary
  }

  foreach ($requiredPath in @($TipDll, $HostExe, $registerScript, $supervisorScript)) {
    if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
      throw "VM verification package file is missing: $requiredPath"
    }
  }
  if ($Model -and -not (Test-Path -LiteralPath $Model -PathType Leaf)) {
    throw "GGUF model not found: $Model"
  }
  if ($Model -and -not (Test-Path -LiteralPath $Bench -PathType Leaf)) {
    throw "llama.cpp preflight tool not found: $Bench"
  }
  if ($MockDictionary -and
      -not (Test-Path -LiteralPath $MockDictionary -PathType Leaf)) {
    throw "Mock dictionary not found: $MockDictionary"
  }

  $actions = [ordered]@{
    vcRuntimeInstalled = $false
    tipRegistered = $false
    hostSupervisorStarted = $false
  }
  $hostBinary = [pscustomobject][ordered]@{
    status = "not_applicable"
    reason = "The inference host has not been inspected."
    processId = 0
    runningPath = ""
    runningSha256 = ""
    expectedPath = $HostExe
    expectedSha256 = ""
  }
  $checks = @()

  if (Test-VmVerifyVCRuntime) {
    $checks += Get-VmVerifyCheck `
      -Id "vcRuntime" `
      -Status "pass" `
      -Message "VC++ x64 runtime is already available."
  } elseif (Test-Path -LiteralPath $RuntimeInstaller -PathType Leaf) {
    try {
      Install-VmVerifyVCRuntime -InstallerPath $RuntimeInstaller
      $actions.vcRuntimeInstalled = $true
      if (-not (Test-VmVerifyVCRuntime)) {
        throw "Required runtime DLLs are still unavailable after installation."
      }
      $checks += Get-VmVerifyCheck `
        -Id "vcRuntime" `
        -Status "pass" `
        -Message "VC++ x64 runtime was installed from the package."
    } catch {
      $checks += Get-VmVerifyCheck `
        -Id "vcRuntime" `
        -Status "fail" `
        -Message $_.Exception.Message
    }
  } else {
    $checks += Get-VmVerifyCheck `
      -Id "vcRuntime" `
      -Status "fail" `
      -Message "VC++ x64 runtime is missing and vc_redist.x64.exe is not bundled."
  }

  $pipeName = Get-VmVerifyPipeName
  $hostWasServing = Test-VmVerifyPipe -PipeName $pipeName
  $staleHostStopped = $false
  if ($hostWasServing) {
    $preflightInspection = Get-VmVerifyServingHostBinary `
      -PipeName $pipeName `
      -HostExe $HostExe `
      -Manifest $manifest
    foreach ($property in $preflightInspection.PSObject.Properties | Where-Object {
        $_.Name -notin @("status", "reason")
      }) {
      $hostBinary.$($property.Name) = $property.Value
    }
    if ($preflightInspection.status -eq "mismatched") {
      try {
        Invoke-VmVerifyHostSupervisorShutdown
        if (-not (Wait-VmVerifyHostSupervisorStopped -TimeoutSeconds 5)) {
          throw "The existing inference host supervisor did not stop within the timeout."
        }
        Invoke-VmVerifyHostProcessTermination -ProcessId $preflightInspection.processId
        if (-not (Wait-VmVerifyPipe `
            -PipeName $pipeName `
            -TimeoutSeconds 15 `
            -ExpectedPresent:$false)) {
          throw "The stale inference host pipe did not disappear within the timeout."
        }
        $hostWasServing = $false
        $staleHostStopped = $true
      } catch {
        $hostBinary.status = "unverified"
        $hostBinary.reason = $_.Exception.Message
        $checks += Get-VmVerifyCheck `
          -Id "inferenceHost" `
          -Status "fail" `
          -Message $_.Exception.Message
      }
    }
  }

  $registered = Test-VmVerifyRegistration -ExpectedTipDllPath $TipDll
  if ($registered) {
    $checks += Get-VmVerifyCheck `
      -Id "tipRegistration" `
      -Status "pass" `
      -Message "The expected TIP DLL is already registered machine-wide."
  } else {
    try {
      $registrationOutput = Invoke-VmVerifyRegistration `
        -RegisterScriptPath $registerScript `
        -TipDll $TipDll `
        -HostExe $HostExe `
        -Model $Model `
        -Bench $Bench `
        -MockDictionary $MockDictionary
      $actions.tipRegistered = $true
      if (-not (Test-VmVerifyRegistration -ExpectedTipDllPath $TipDll)) {
        throw ("register-dev.ps1 completed, but the expected TIP registration was not found." +
          $(if ($registrationOutput) { " Output: $registrationOutput" } else { "" }))
      }
      $checks += Get-VmVerifyCheck `
        -Id "tipRegistration" `
        -Status "pass" `
        -Message "The TIP DLL was registered machine-wide."
      $registered = $true
    } catch {
      $checks += Get-VmVerifyCheck `
        -Id "tipRegistration" `
        -Status "fail" `
        -Message $_.Exception.Message
    }
  }

  if (-not $hostWasServing -and $registered) {
    try {
      if (-not (Wait-VmVerifyPipe -PipeName $pipeName -TimeoutSeconds 15)) {
        Invoke-VmVerifyHostSupervisor `
          -SupervisorScriptPath $supervisorScript `
          -HostExe $HostExe `
          -PipeName $pipeName `
          -Model $Model `
          -MockDictionary $MockDictionary
        $actions.hostSupervisorStarted = $true
      }
      if (-not (Wait-VmVerifyPipe -PipeName $pipeName -TimeoutSeconds 15)) {
        throw "Inference host did not expose \\.\pipe\$pipeName within the timeout."
      }
    } catch {
      $hostBinary.status = "unverified"
      $hostBinary.reason = $_.Exception.Message
      $checks += Get-VmVerifyCheck `
        -Id "inferenceHost" `
        -Status "fail" `
        -Message $_.Exception.Message
    }
  } elseif (-not $hostWasServing) {
    $hostBinary.status = "unverified"
    $hostBinary.reason = "Inference host startup was skipped because TIP registration failed."
    $checks += Get-VmVerifyCheck `
      -Id "inferenceHost" `
      -Status "fail" `
      -Message $hostBinary.reason
  }

  $hasInferenceHostFailure = @($checks | Where-Object {
      $_.id -eq "inferenceHost" -and $_.status -eq "fail"
    }).Count -ne 0
  if (-not $hasInferenceHostFailure -and -not (Test-VmVerifyPipe -PipeName $pipeName)) {
    $hostBinary.status = "unverified"
    $hostBinary.reason = "The inference host pipe stopped before final verification."
    $checks += Get-VmVerifyCheck `
      -Id "inferenceHost" `
      -Status "fail" `
      -Message $hostBinary.reason
  } elseif (-not $hasInferenceHostFailure) {
    $inspection = Get-VmVerifyServingHostBinary `
      -PipeName $pipeName `
      -HostExe $HostExe `
      -Manifest $manifest
    foreach ($property in $inspection.PSObject.Properties | Where-Object {
        $_.Name -notin @("status", "reason")
      }) {
      $hostBinary.$($property.Name) = $property.Value
    }

    if ($inspection.status -eq "mismatched") {
      $hostBinary.status = "unverified"
      $hostBinary.reason = "The serving host still differs from the bundled inference host after bootstrap."
      $checks += Get-VmVerifyCheck `
        -Id "inferenceHost" `
        -Status "fail" `
        -Message $hostBinary.reason
    } elseif ($inspection.status -eq "matched") {
      $hostBinary.status = $(if ($staleHostStopped) {
          "restarted"
        } elseif ($hostWasServing) {
          "reused"
        } else {
          "started"
        })
      $hostBinary.reason = $(if ($staleHostStopped) {
          "A stale host was stopped and replaced with the bundled inference host."
        } elseif ($hostWasServing) {
          "The existing host matches the bundled inference host and was reused."
        } else {
          "The bundled inference host was started and verified."
        })
      $checks += Get-VmVerifyCheck `
        -Id "inferenceHost" `
        -Status "pass" `
        -Message $hostBinary.reason
    } else {
      $hostBinary.status = "unverified"
      $hostBinary.reason = $inspection.reason
      Write-Warning "The serving inference host could not be verified: $($inspection.reason)"
      $checks += Get-VmVerifyCheck `
        -Id "inferenceHost" `
        -Status "manual_required" `
        -Message "The serving inference host could not be verified: $($inspection.reason)"
    }
  }

  if (Test-VmVerifyMicrosoftIme) {
    $checks += Get-VmVerifyCheck `
      -Id "microsoftIme" `
      -Status "pass" `
      -Message "Microsoft Japanese IME remains available for recovery."
  } else {
    $checks += Get-VmVerifyCheck `
      -Id "microsoftIme" `
      -Status "fail" `
      -Message "Microsoft Japanese IME was not found in the current user's language list."
  }

  if ($HasCheckpoint) {
    $checks += Get-VmVerifyCheck `
      -Id "vmCheckpoint" `
      -Status "pass" `
      -Message "The operator confirmed that a VM checkpoint exists."
  } else {
    $checks += Get-VmVerifyCheck `
      -Id "vmCheckpoint" `
      -Status "manual_required" `
      -Message "Confirm the Hyper-V checkpoint, then rerun with -CheckpointConfirmed."
  }

  if ($package.buildType -eq "Debug") {
    $checks += Get-VmVerifyCheck `
      -Id "debugView" `
      -Status "manual_required" `
      -Message "For Debug verification, enable Capture Global Win32 and filter [azooKey TIP]."
  } else {
    $checks += Get-VmVerifyCheck `
      -Id "debugView" `
      -Status "not_applicable" `
      -Message "DebugView setup is only required for Debug verification."
  }

  $overallStatus = "pass"
  if (@($checks | Where-Object { $_.status -eq "fail" }).Count -gt 0) {
    $overallStatus = "fail"
  } elseif (@($checks | Where-Object { $_.status -eq "manual_required" }).Count -gt 0) {
    $overallStatus = "warning"
  }

  return [pscustomobject][ordered]@{
    schemaVersion = 1
    generatedAtUtc = [DateTimeOffset]::UtcNow.ToString("o")
    package = $package
    overallStatus = $overallStatus
    actions = [pscustomobject]$actions
    hostBinary = $hostBinary
    checks = @($checks)
  }
}

if ($MyInvocation.InvocationName -ne ".") {
  $result = Invoke-VmVerifyBootstrap `
    -PackageRoot $PSScriptRoot `
    -TipDll $TipDllPath `
    -HostExe $HostExePath `
    -RuntimeInstaller $RuntimeInstallerPath `
    -Model $ModelPath `
    -Bench $BenchPath `
    -MockDictionary $MockDictionaryPath `
    -HasCheckpoint:$CheckpointConfirmed

  if ($Json) {
    $result | ConvertTo-Json -Depth 6
  } else {
    Write-Output "azooKey VM verification bootstrap: $($result.overallStatus)"
    foreach ($check in $result.checks) {
      Write-Output ("[{0}] {1}: {2}" -f $check.status, $check.id, $check.message)
    }
  }

  if ($result.overallStatus -eq "fail") {
    exit 1
  }
}
