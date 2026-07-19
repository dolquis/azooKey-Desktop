#requires -Version 5.1
param(
  [switch]$SkipLint,
  [switch]$SkipTests
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$scriptAnalyzerSettings = Join-Path $repoRoot "PSScriptAnalyzerSettings.psd1"
$pesterTests = Join-Path $PSScriptRoot "tests"

$powerShellFiles = @(
  Join-Path $PSScriptRoot "doctor.ps1"
  Join-Path $PSScriptRoot "host-supervisor.ps1"
  Join-Path $PSScriptRoot "register-dev.ps1"
  Join-Path $PSScriptRoot "unregister-dev.ps1"
  Join-Path (Join-Path $repoRoot "compat-test") "msix_install_uninstall.ps1"
  Join-Path $PSScriptRoot "test-powershell-quality.ps1"
) | ForEach-Object { (Resolve-Path $_).Path }

$testFiles = Get-ChildItem -Path $pesterTests -Filter "*.Tests.ps1" -File |
  ForEach-Object { $_.FullName }
$powerShellFiles += $testFiles

function Get-RequiredModule {
  param(
    [Parameter(Mandatory=$true)]
    [string]$Name
  )

  $module = Get-Module -ListAvailable -Name $Name |
    Sort-Object Version -Descending |
    Select-Object -First 1
  if (-not $module) {
    throw "Required PowerShell module '$Name' is not installed. Install it locally (for example: Install-Module $Name -Scope CurrentUser) and retry."
  }

  return $module
}

if (-not $SkipLint) {
  $scriptAnalyzer = Get-RequiredModule -Name "PSScriptAnalyzer"
  Import-Module $scriptAnalyzer.Path -Force

  $diagnostics = foreach ($file in $powerShellFiles) {
    Invoke-ScriptAnalyzer -Path $file -Settings $scriptAnalyzerSettings
  }

  if ($diagnostics) {
    $diagnostics |
      Sort-Object ScriptName, Line, Column |
      Format-Table -AutoSize ScriptName, Line, Column, Severity, RuleName, Message |
      Out-String |
      Write-Output
    throw "PSScriptAnalyzer found $($diagnostics.Count) diagnostic(s)."
  }

  Write-Output "PSScriptAnalyzer passed for $($powerShellFiles.Count) PowerShell file(s)."
}

if (-not $SkipTests) {
  $pester = Get-RequiredModule -Name "Pester"
  Import-Module $pester.Path -Force

  if ($pester.Version.Major -ge 5) {
    $configuration = New-PesterConfiguration
    $configuration.Run.Path = $pesterTests
    $configuration.Run.PassThru = $true
    $configuration.TestRegistry.Enabled = $false
    $result = Invoke-Pester -Configuration $configuration
  } else {
    $result = Invoke-Pester -Path $pesterTests -PassThru
  }

  if ($result.FailedCount -gt 0) {
    throw "Pester reported $($result.FailedCount) failed test(s)."
  }

  Write-Output "Pester passed: $($result.PassedCount)/$($result.TotalCount) test(s)."
}
