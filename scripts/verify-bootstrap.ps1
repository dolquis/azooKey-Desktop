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
    [int]$TimeoutSeconds = 15
  )

  $deadline = [DateTimeOffset]::UtcNow.AddSeconds($TimeoutSeconds)
  do {
    if (Test-VmVerifyPipe -PipeName $PipeName) {
      return $true
    }
    Start-Sleep -Milliseconds 200
  } while ([DateTimeOffset]::UtcNow -lt $deadline)
  return $false
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

  $pipeName = Get-VmVerifyPipeName
  if (Test-VmVerifyPipe -PipeName $pipeName) {
    $checks += Get-VmVerifyCheck `
      -Id "inferenceHost" `
      -Status "pass" `
      -Message "The inference host is already serving \\.\pipe\$pipeName."
  } elseif ($registered) {
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
      $checks += Get-VmVerifyCheck `
        -Id "inferenceHost" `
        -Status "pass" `
        -Message "The inference host is serving \\.\pipe\$pipeName."
    } catch {
      $checks += Get-VmVerifyCheck `
        -Id "inferenceHost" `
        -Status "fail" `
        -Message $_.Exception.Message
    }
  } else {
    $checks += Get-VmVerifyCheck `
      -Id "inferenceHost" `
      -Status "fail" `
      -Message "Inference host startup was skipped because TIP registration failed."
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
