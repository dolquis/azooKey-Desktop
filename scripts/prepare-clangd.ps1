#requires -Version 7.0
<#
.SYNOPSIS
Prepare and validate this checkout's Serena/clangd compilation database.
.DESCRIPTION
Run after clone/worktree creation, build directory cleanup, or CMake changes.
Uses an existing VS developer shell or initializes Visual Studio in this process.
Configure only; no product build or dependency downloads. Repeated runs are safe.
After success, reload the language server and verify symbols/references through MCP.
.EXAMPLE
pwsh -NoProfile -File scripts/prepare-clangd.ps1
#>
[CmdletBinding()]
param()
$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
. (Join-Path $PSScriptRoot "clangd-database.ps1")
if (-not $env:VSCMD_VER) {
  $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio/Installer/vswhere.exe"
  $install = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  if ($LASTEXITCODE -ne 0 -or -not $install) { throw "Visual Studio C++ tools were not found." }
  Import-Module (Join-Path $install "Common7/Tools/Microsoft.VisualStudio.DevShell.dll")
  Enter-VsDevShell -VsInstallPath $install -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
}
Push-Location $repoRoot
try {
  & cmake --preset windows-clangd -DAZOOKEY_FETCH_GOOGLETEST=OFF -DAZOOKEY_FETCH_LLAMA_CPP=OFF -DAZOOKEY_FETCH_WIL=OFF
  if ($LASTEXITCODE -ne 0) { throw "clangd configure failed (exit $LASTEXITCODE)." }
  $result = Test-ClangdDatabase -RepoRoot $repoRoot
  if (-not $result.ok) { throw $result.details }
  Write-Output $result.details
  Write-Output "Reload Serena/clangd, then verify C++ symbols, cross-file references and diagnostics."
} finally {
  Pop-Location
}
