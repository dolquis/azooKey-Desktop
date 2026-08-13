#requires -Version 5.1
[CmdletBinding()]
param(
  [Alias("json")]
  [switch]$AsJson,
  [Alias("fix-hints")]
  [switch]$FixHints
)

$ErrorActionPreference = "Stop"

function Get-DoctorCheck {
  param(
    [Parameter(Mandatory=$true)]
    [string]$Id,
    [Parameter(Mandatory=$true)]
    [string]$Name,
    [Parameter(Mandatory=$true)]
    [ValidateSet("ok", "warning", "error")]
    [string]$Status,
    [Parameter(Mandatory=$true)]
    [bool]$Required,
    [AllowEmptyString()]
    [string]$Version = "",
    [AllowEmptyString()]
    [string]$Details = "",
    [AllowEmptyString()]
    [string]$Hint = ""
  )

  [pscustomobject][ordered]@{
    id = $Id
    name = $Name
    status = $Status
    required = $Required
    version = $Version
    details = $Details
    hint = $Hint
  }
}

function Get-DoctorCommandVersion {
  param(
    [Parameter(Mandatory=$true)]
    [string]$CommandPath,
    [string[]]$VersionArguments = @("--version")
  )

  try {
    $versionOutput = @(& $CommandPath @VersionArguments 2>&1)
    $firstLine = $versionOutput |
      ForEach-Object { $_.ToString().Trim() } |
      Where-Object { $_ } |
      Select-Object -First 1
    if ($firstLine) {
      return $firstLine
    }
  } catch {
    return ""
  }

  return ""
}

function Get-DoctorCommandCheck {
  param(
    [Parameter(Mandatory=$true)]
    [string]$Id,
    [Parameter(Mandatory=$true)]
    [string]$Name,
    [Parameter(Mandatory=$true)]
    [string]$CommandName,
    [Parameter(Mandatory=$true)]
    [bool]$Required,
    [string[]]$VersionArguments = @("--version"),
    [Parameter(Mandatory=$true)]
    [string]$Hint
  )

  $command = Get-Command -Name $CommandName -CommandType Application -ErrorAction SilentlyContinue |
    Select-Object -First 1
  if (-not $command) {
    $status = if ($Required) { "error" } else { "warning" }
    return Get-DoctorCheck -Id $Id -Name $Name -Status $status -Required $Required `
      -Details "$CommandName was not found on PATH." -Hint $Hint
  }

  $version = Get-DoctorCommandVersion -CommandPath $command.Source -VersionArguments $VersionArguments
  return Get-DoctorCheck -Id $Id -Name $Name -Status "ok" -Required $Required `
    -Version $version -Details $command.Source
}

function Get-DoctorApplicationCheck {
  param(
    [Parameter(Mandatory=$true)]
    [string]$Id,
    [Parameter(Mandatory=$true)]
    [string]$Name,
    [Parameter(Mandatory=$true)]
    [string[]]$CommandNames,
    [string[]]$CandidatePaths = @(),
    [Parameter(Mandatory=$true)]
    [bool]$Required,
    [Parameter(Mandatory=$true)]
    [string]$Hint
  )

  foreach ($commandName in $CommandNames) {
    $command = Get-Command -Name $commandName -CommandType Application -ErrorAction SilentlyContinue |
      Select-Object -First 1
    if ($command) {
      return Get-DoctorCheck -Id $Id -Name $Name -Status "ok" -Required $Required `
        -Details $command.Source
    }
  }

  foreach ($candidatePath in $CandidatePaths) {
    if (-not [string]::IsNullOrWhiteSpace($candidatePath) -and
        (Test-Path -LiteralPath $candidatePath -PathType Leaf)) {
      return Get-DoctorCheck -Id $Id -Name $Name -Status "ok" -Required $Required `
        -Details $candidatePath
    }
  }

  $status = if ($Required) { "error" } else { "warning" }
  $commandList = $CommandNames -join ", "
  return Get-DoctorCheck -Id $Id -Name $Name -Status $status -Required $Required `
    -Details "No matching command was found on PATH or in known install locations: $commandList." `
    -Hint $Hint
}

function Get-WinDbgCheck {
  param(
    [string[]]$CandidatePaths = @()
  )

  $hint = "Install WinDbg with: winget install Microsoft.WinDbg"
  $pathCheck = Get-DoctorApplicationCheck -Id "tool.windbg" -Name "WinDbg" `
    -CommandNames @("WinDbgX.exe", "windbg.exe") -CandidatePaths $CandidatePaths `
    -Required $false -Hint $hint
  if ($pathCheck.status -eq "ok") {
    return $pathCheck
  }

  $getAppxPackage = Get-Command -Name "Get-AppxPackage" -CommandType Cmdlet `
    -ErrorAction SilentlyContinue
  if ($getAppxPackage) {
    try {
      $package = Get-AppxPackage -Name "Microsoft.WinDbg" -ErrorAction SilentlyContinue |
        Select-Object -First 1
    } catch {
      $package = $null
    }

    if ($package) {
      $details = if ($package.PackageFullName) {
        "Appx: $($package.PackageFullName)"
      } else {
        "Appx: Microsoft.WinDbg"
      }
      return Get-DoctorCheck -Id "tool.windbg" -Name "WinDbg" -Status "ok" `
        -Required $false -Version $package.Version.ToString() -Details $details
    }
  }

  return $pathCheck
}

function Get-DoctorPathCheck {
  param(
    [Parameter(Mandatory=$true)]
    [string]$Id,
    [Parameter(Mandatory=$true)]
    [string]$Name,
    [Parameter(Mandatory=$true)]
    [string]$Path,
    [Parameter(Mandatory=$true)]
    [bool]$Required,
    [Parameter(Mandatory=$true)]
    [string]$Hint,
    [ValidateSet("Any", "Container", "Leaf")]
    [string]$PathType = "Any"
  )

  $exists = if ($PathType -eq "Any") {
    Test-Path -LiteralPath $Path
  } else {
    Test-Path -LiteralPath $Path -PathType $PathType
  }

  if ($exists) {
    return Get-DoctorCheck -Id $Id -Name $Name -Status "ok" -Required $Required `
      -Details $Path
  }

  $status = if ($Required) { "error" } else { "warning" }
  return Get-DoctorCheck -Id $Id -Name $Name -Status $status -Required $Required `
    -Details "Not found: $Path" -Hint $Hint
}

function Get-VisualStudioCheck {
  $installationPath = $env:VSINSTALLDIR
  if (-not $installationPath) {
    $vswhereCandidates = @(
      (Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe")
      (Join-Path $env:ProgramFiles "Microsoft Visual Studio\Installer\vswhere.exe")
    ) | Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Leaf) }

    $vswhere = $vswhereCandidates | Select-Object -First 1
    if ($vswhere) {
      $installationPath = @(
        & $vswhere -latest -products * `
          -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
          -property installationPath
      ) | Select-Object -First 1
    }
  }

  if ($installationPath -and (Test-Path -LiteralPath $installationPath -PathType Container)) {
    return Get-DoctorCheck -Id "tool.vs2022" -Name "Visual Studio 2022 C++" `
      -Status "ok" -Required $true -Details $installationPath
  }

  return Get-DoctorCheck -Id "tool.vs2022" -Name "Visual Studio 2022 C++" `
    -Status "error" -Required $true `
    -Details "A Visual Studio installation with the MSVC x64 tools was not found." `
    -Hint "Install the Visual Studio 2022 Desktop development with C++ workload."
}

function Get-MsvcShellCheck {
  $cl = Get-Command -Name "cl.exe" -CommandType Application -ErrorAction SilentlyContinue |
    Select-Object -First 1
  if ($cl -and $env:VSCMD_VER) {
    return Get-DoctorCheck -Id "env.msvc-shell" -Name "MSVC developer shell" `
      -Status "ok" -Required $true -Version $env:VSCMD_VER -Details $cl.Source
  }

  return Get-DoctorCheck -Id "env.msvc-shell" -Name "MSVC developer shell" `
    -Status "error" -Required $true `
    -Details "cl.exe and VSCMD_VER are not both available in this process." `
    -Hint "Run from a VS 2022 Developer PowerShell, or use C:\Codex\codex-build.ps1."
}

function Get-WindowsSdkCheck {
  $sdkRoot = $env:WindowsSdkDir
  if (-not $sdkRoot) {
    $kitsRoot = Get-ItemProperty `
      -Path "HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots" `
      -Name "KitsRoot10" -ErrorAction SilentlyContinue
    if ($kitsRoot) {
      $sdkRoot = $kitsRoot.KitsRoot10
    }
  }

  if ($sdkRoot -and (Test-Path -LiteralPath $sdkRoot -PathType Container)) {
    $includeRoot = Join-Path $sdkRoot "Include"
    $version = Get-ChildItem -LiteralPath $includeRoot -Directory -ErrorAction SilentlyContinue |
      Sort-Object Name -Descending |
      Select-Object -First 1 -ExpandProperty Name
    return Get-DoctorCheck -Id "tool.windows-sdk" -Name "Windows SDK" `
      -Status "ok" -Required $true -Version $version -Details $sdkRoot
  }

  return Get-DoctorCheck -Id "tool.windows-sdk" -Name "Windows SDK" `
    -Status "error" -Required $true -Details "Windows 10/11 SDK was not found." `
    -Hint "Add a Windows SDK through Visual Studio Installer."
}

function Get-CompileCommandsCheck {
  param(
    [Parameter(Mandatory=$true)]
    [string]$RepoRoot
  )

  $buildRoot = Join-Path $RepoRoot "build"
  $clangdBuild = Join-Path $buildRoot "clangd"
  $compileCommands = Join-Path $clangdBuild "compile_commands.json"

  if (Test-Path -LiteralPath $compileCommands -PathType Leaf) {
    return Get-DoctorCheck -Id "file.compile-commands" -Name "compile_commands.json" `
      -Status "ok" -Required $false -Details $compileCommands
  }

  return Get-DoctorCheck -Id "file.compile-commands" -Name "compile_commands.json" `
    -Status "warning" -Required $false `
    -Details "The clangd compilation database was not found at $compileCommands." `
    -Hint "Run: cmake --preset windows-clangd"
}

function Get-DoctorCheckSet {
  param(
    [Parameter(Mandatory=$true)]
    [string]$RepoRoot
  )

  $powerShellStatus = if ($PSVersionTable.PSVersion.Major -ge 7) { "ok" } else { "error" }
  $powerShellHint = if ($powerShellStatus -eq "error") {
    "Install PowerShell 7 and run this script with pwsh."
  } else {
    ""
  }
  $programFilesX86 = [Environment]::GetFolderPath(
    [Environment+SpecialFolder]::ProgramFilesX86
  )
  $windowsDirectory = [Environment]::GetFolderPath(
    [Environment+SpecialFolder]::Windows
  )
  $debuggerX64 = if ($programFilesX86) {
    Join-Path $programFilesX86 "Windows Kits\10\Debuggers\x64"
  } else {
    ""
  }
  $wptRoot = if ($programFilesX86) {
    Join-Path $programFilesX86 "Windows Kits\10\Windows Performance Toolkit"
  } else {
    ""
  }
  $system32 = if ($windowsDirectory) {
    Join-Path $windowsDirectory "System32"
  } else {
    ""
  }

  @(
    Get-DoctorCheck -Id "tool.pwsh" -Name "PowerShell 7" -Status $powerShellStatus `
      -Required $true -Version $PSVersionTable.PSVersion.ToString() `
      -Details $PSHOME -Hint $powerShellHint
    Get-DoctorCommandCheck -Id "tool.git" -Name "Git" -CommandName "git.exe" `
      -Required $true -Hint "Install Git for Windows and add it to PATH."
    Get-DoctorCommandCheck -Id "tool.gh" -Name "GitHub CLI" -CommandName "gh.exe" `
      -Required $false -Hint "Install GitHub CLI: winget install GitHub.cli"
    Get-VisualStudioCheck
    Get-MsvcShellCheck
    Get-DoctorCommandCheck -Id "tool.cmake" -Name "CMake" -CommandName "cmake.exe" `
      -Required $true -Hint "Install CMake and add it to PATH."
    Get-DoctorCommandCheck -Id "tool.ninja" -Name "Ninja" -CommandName "ninja.exe" `
      -Required $true -Hint "Install Ninja and add it to PATH."
    Get-WindowsSdkCheck
    Get-DoctorCommandCheck -Id "tool.clang-format" -Name "clang-format" `
      -CommandName "clang-format.exe" -Required $false `
      -Hint "Install LLVM or the Visual Studio Clang tools."
    Get-DoctorCommandCheck -Id "tool.clang-tidy" -Name "clang-tidy" `
      -CommandName "clang-tidy.exe" -Required $false `
      -Hint "Install LLVM or the Visual Studio Clang tools."
    Get-DoctorCommandCheck -Id "tool.clangd" -Name "clangd" -CommandName "clangd.exe" `
      -Required $false -Hint "Install LLVM and add clangd.exe to PATH."
    Get-DoctorCommandCheck -Id "tool.pre-commit" -Name "pre-commit" `
      -CommandName "pre-commit.exe" -Required $false `
      -Hint "Install pre-commit: pipx install pre-commit"
    Get-DoctorCommandCheck -Id "tool.just" -Name "just" -CommandName "just.exe" `
      -Required $false -Hint "Install just: winget install Casey.Just"
    Get-WinDbgCheck -CandidatePaths @(
      $(if ($debuggerX64) { Join-Path $debuggerX64 "windbg.exe" })
    )
    Get-DoctorApplicationCheck -Id "tool.cdb" -Name "CDB" `
      -CommandNames @("cdb.exe") -CandidatePaths @(
        $(if ($debuggerX64) { Join-Path $debuggerX64 "cdb.exe" })
      ) -Required $false `
      -Hint "Install the Windows SDK Debugging Tools for Windows component."
    Get-DoctorApplicationCheck -Id "tool.gflags" -Name "GFlags" `
      -CommandNames @("gflags.exe") -CandidatePaths @(
        $(if ($debuggerX64) { Join-Path $debuggerX64 "gflags.exe" })
      ) -Required $false `
      -Hint "Install the Windows SDK Debugging Tools for Windows component."
    Get-DoctorApplicationCheck -Id "tool.wpr" -Name "Windows Performance Recorder" `
      -CommandNames @("wpr.exe") -CandidatePaths @(
        $(if ($system32) { Join-Path $system32 "wpr.exe" })
        $(if ($wptRoot) { Join-Path $wptRoot "wpr.exe" })
      ) -Required $false `
      -Hint "Install the Windows Performance Toolkit from the Windows ADK."
    Get-DoctorApplicationCheck -Id "tool.wpa" -Name "Windows Performance Analyzer" `
      -CommandNames @("wpa.exe") -CandidatePaths @(
        $(if ($wptRoot) { Join-Path $wptRoot "wpa.exe" })
      ) -Required $false `
      -Hint "Install the Windows Performance Toolkit from the Windows ADK."
    Get-DoctorApplicationCheck -Id "tool.procdump" -Name "ProcDump" `
      -CommandNames @("procdump64.exe", "procdump.exe") -Required $false `
      -Hint "Install the Sysinternals Suite and add it to PATH."
    Get-DoctorApplicationCheck -Id "tool.procmon" -Name "Process Monitor" `
      -CommandNames @("procmon64.exe", "procmon.exe") -Required $false `
      -Hint "Install the Sysinternals Suite and add it to PATH."
    Get-DoctorApplicationCheck -Id "tool.appverifier" -Name "Application Verifier" `
      -CommandNames @("appverif.exe") -CandidatePaths @(
        $(if ($system32) { Join-Path $system32 "appverif.exe" })
      ) -Required $false `
      -Hint "Install Application Verifier from the Windows SDK."
    Get-DoctorApplicationCheck -Id "tool.handle" -Name "Sysinternals Handle" `
      -CommandNames @("handle64.exe", "handle.exe") -Required $false `
      -Hint "Install the Sysinternals Suite and add it to PATH."
    Get-DoctorPathCheck -Id "file.mcp-config" -Name ".mcp.json" `
      -Path (Join-Path $RepoRoot ".mcp.json") -Required $false `
      -Hint "Restore the repository's .mcp.json agent configuration." -PathType Leaf
    Get-DoctorPathCheck -Id "file.codex-config" -Name ".codex/config.toml" `
      -Path (Join-Path $RepoRoot ".codex\config.toml") -Required $false `
      -Hint "Restore the repository's .codex/config.toml agent configuration." -PathType Leaf
    Get-DoctorPathCheck -Id "file.cmake-presets" -Name "CMakePresets.json" `
      -Path (Join-Path $RepoRoot "CMakePresets.json") -Required $true `
      -Hint "Restore CMakePresets.json from the repository." -PathType Leaf
    Get-DoctorPathCheck -Id "dependency.wil" -Name "WIL submodule" `
      -Path (Join-Path $RepoRoot "third_party\wil\CMakeLists.txt") -Required $true `
      -Hint "Run: git submodule update --init third_party/wil" -PathType Leaf
    Get-CompileCommandsCheck -RepoRoot $RepoRoot
  )
}

function Get-DoctorStatus {
  param(
    [Parameter(Mandatory=$true)]
    [object[]]$Checks
  )

  if ($Checks.status -contains "error") {
    return "error"
  }
  if ($Checks.status -contains "warning") {
    return "warning"
  }
  return "ok"
}

function Get-DoctorReport {
  param(
    [Parameter(Mandatory=$true)]
    [string]$RepoRoot,
    [Parameter(Mandatory=$true)]
    [object[]]$Checks
  )

  [pscustomobject][ordered]@{
    schemaVersion = 1
    status = Get-DoctorStatus -Checks $Checks
    repoRoot = $RepoRoot
    generatedAt = [DateTimeOffset]::UtcNow.ToString("o")
    summary = [pscustomobject][ordered]@{
      ok = @($Checks | Where-Object status -eq "ok").Count
      warning = @($Checks | Where-Object status -eq "warning").Count
      error = @($Checks | Where-Object status -eq "error").Count
    }
    checks = @($Checks)
  }
}

function Show-DoctorReport {
  param(
    [Parameter(Mandatory=$true)]
    [pscustomobject]$Report,
    [switch]$IncludeFixHints
  )

  $table = $Report.checks |
    Select-Object @{
      Name = "Status"
      Expression = { $_.status.ToUpperInvariant() }
    }, @{
      Name = "Required"
      Expression = { if ($_.required) { "yes" } else { "no" } }
    }, name, version, details |
    Format-Table -AutoSize |
    Out-String
  Write-Host $table.TrimEnd()
  Write-Host ""
  Write-Host (
    "Overall: {0} (ok={1}, warning={2}, error={3})" -f `
      $Report.status.ToUpperInvariant(),
      $Report.summary.ok,
      $Report.summary.warning,
      $Report.summary.error
  )

  if ($IncludeFixHints) {
    $checksWithHints = @($Report.checks | Where-Object {
      $_.status -ne "ok" -and $_.hint
    })
    if ($checksWithHints.Count -gt 0) {
      Write-Host ""
      Write-Host "Fix hints:"
      foreach ($check in $checksWithHints) {
        Write-Host ("- [{0}] {1}" -f $check.id, $check.hint)
      }
    }
  }
}

function Invoke-DoctorMain {
  param(
    [switch]$OutputJson,
    [switch]$IncludeFixHints
  )

  $repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
  $checks = @(Get-DoctorCheckSet -RepoRoot $repoRoot)
  $report = Get-DoctorReport -RepoRoot $repoRoot -Checks $checks

  if ($OutputJson) {
    Write-Output ($report | ConvertTo-Json -Depth 6)
  } else {
    Show-DoctorReport -Report $report -IncludeFixHints:$IncludeFixHints
  }

  if ($report.status -eq "error") {
    exit 1
  }
}

if ($MyInvocation.InvocationName -ne ".") {
  Invoke-DoctorMain -OutputJson:$AsJson -IncludeFixHints:$FixHints
}
