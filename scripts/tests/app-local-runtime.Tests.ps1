Describe "App-local MSVC runtime coverage check" {
  BeforeAll {
    $repoRoot = (Resolve-Path (Join-Path (Join-Path $PSScriptRoot "..") "..")).Path
    . (Join-Path $repoRoot "scripts\check-app-local-runtime.ps1")

    # dumpbin /dependents の実出力から、判定に関わる行構造だけを残したもの。
    function Get-DumpbinReport {
      param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [string[]]$Import
      )

      $lines = @(
        "Microsoft (R) COFF/PE Dumper Version 14.44.35211.0",
        "Copyright (C) Microsoft Corporation.  All rights reserved.",
        "",
        "Dump of file azookey_inference_host.exe",
        "",
        "File Type: EXECUTABLE IMAGE",
        "",
        "  Image has the following dependencies:",
        "")
      $lines += $Import | ForEach-Object { "    $_" }
      $lines += @("", "  Summary", "", "        1000 .data", "       12000 .text")
      return $lines
    }

    function Initialize-RuntimeDirectory {
      param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [string[]]$Name
      )

      New-Item -ItemType Directory -Path $Path -Force | Out-Null
      foreach ($file in $Name) {
        $file | Set-Content -LiteralPath (Join-Path $Path $file)
      }
      return $Path
    }
  }

  BeforeEach {
    $script:binary = Join-Path $TestDrive "azookey_inference_host.exe"
    "host" | Set-Content -LiteralPath $script:binary
    $script:crtDir = Initialize-RuntimeDirectory `
      -Path (Join-Path $TestDrive "crt") `
      -Name @("msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll")
    $script:openMpDir = Initialize-RuntimeDirectory `
      -Path (Join-Path $TestDrive "openmp") `
      -Name @("vcomp140.dll")
  }

  It "reads only the indented import names out of the dumpbin report" {
    $output = Get-DumpbinReport -Import @("MSVCP140.dll", "VCOMP140.DLL", "KERNEL32.dll")

    Get-AppLocalRuntimeImport -DependencyOutput $output |
      Should -Be @("MSVCP140.dll", "VCOMP140.DLL", "KERNEL32.dll")
  }

  It "ignores OS-provided imports and matches runtime names case-insensitively" {
    $imports = @("KERNEL32.dll", "ADVAPI32.dll", "MSVCP140.dll", "VCOMP140.DLL", "msvcp140.dll")

    Get-AppLocalRuntimeGap `
      -ImportName $imports `
      -RuntimeDirectory @($script:crtDir, $script:openMpDir) |
      Should -BeNullOrEmpty
  }

  It "reports a runtime import that no source directory provides" {
    $imports = @("KERNEL32.dll", "MSVCP140.dll", "VCOMP140.DLL", "msvcp140_1.dll")

    Get-AppLocalRuntimeGap -ImportName $imports -RuntimeDirectory @($script:crtDir) |
      Should -Be @("VCOMP140.DLL", "msvcp140_1.dll")
  }

  It "accepts a shipped binary whose runtime imports are all packaged" {
    $output = Get-DumpbinReport -Import @(
      "KERNEL32.dll", "MSVCP140.dll", "VCRUNTIME140.dll", "VCRUNTIME140_1.dll", "VCOMP140.DLL")

    $result = Assert-AppLocalRuntime `
      -BinaryPath @($script:binary) `
      -RuntimeDirectory @($script:crtDir, $script:openMpDir) `
      -DependencyOutput @{ $script:binary = $output }

    @($result).Count | Should -Be 1
    $result[0].path | Should -Be $script:binary
    $result[0].runtimeImports | Should -Be @(
      "MSVCP140.dll", "VCRUNTIME140.dll", "VCRUNTIME140_1.dll", "VCOMP140.DLL")
  }

  It "fails with an actionable message when the OpenMP runtime is not packaged" {
    # DEV-1137 の再発ガード。CRT だけを同梱した状態を再現する。
    $output = Get-DumpbinReport -Import @(
      "KERNEL32.dll", "MSVCP140.dll", "VCRUNTIME140.dll", "VCRUNTIME140_1.dll", "VCOMP140.DLL")

    {
      Assert-AppLocalRuntime `
        -BinaryPath @($script:binary) `
        -RuntimeDirectory @($script:crtDir) `
        -DependencyOutput @{ $script:binary = $output }
    } | Should -Throw "*pkg/msi/Package.wxs*imports VCOMP140.DLL*"
  }

  It "rejects a report with no import names instead of passing vacuously" {
    {
      Assert-AppLocalRuntime `
        -BinaryPath @($script:binary) `
        -RuntimeDirectory @($script:crtDir) `
        -DependencyOutput @{ $script:binary = @("Dump of file azookey_inference_host.exe") }
    } | Should -Throw "*No imported DLL names were found*"
  }

  It "rejects a missing binary or runtime directory" {
    {
      Assert-AppLocalRuntime `
        -BinaryPath @($script:binary) `
        -RuntimeDirectory @((Join-Path $TestDrive "absent")) `
        -DependencyOutput @{ $script:binary = @() }
    } | Should -Throw "*runtime directory does not exist*"

    {
      Assert-AppLocalRuntime `
        -BinaryPath @((Join-Path $TestDrive "absent.exe")) `
        -RuntimeDirectory @($script:crtDir) `
        -DependencyOutput @{}
    } | Should -Throw "*Binary to inspect does not exist*"
  }
}
