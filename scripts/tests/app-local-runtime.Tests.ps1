Describe "App-local MSVC runtime coverage check" {
  BeforeAll {
    $script:repoRoot = (Resolve-Path (Join-Path (Join-Path $PSScriptRoot "..") "..")).Path
    . (Join-Path $script:repoRoot "scripts\check-app-local-runtime.ps1")

    # dumpbin /dependents の実出力から、判定に関わる行構造だけを残したもの。
    function Get-DumpbinReport {
      param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [string[]]$Import,
        [AllowEmptyCollection()]
        [string[]]$DelayLoadImport = @()
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
      if ($DelayLoadImport) {
        $lines += @("", "  Image has the following delay load dependencies:", "")
        $lines += $DelayLoadImport | ForEach-Object { "    $_" }
      }
      $lines += @("", "  Summary", "", "        1000 .data", "       12000 .text")
      return $lines
    }
  }

  BeforeEach {
    $script:binary = Join-Path $TestDrive "azookey_inference_host.exe"
    "host" | Set-Content -LiteralPath $script:binary
    # MSI が実際に同梱する CRT / OpenMP ランタイム。
    $script:shipped = @("msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll", "vcomp140.dll")
  }

  It "reads the indented import names out of both dependency sections" {
    $output = Get-DumpbinReport `
      -Import @("MSVCP140.dll", "VCOMP140.DLL", "KERNEL32.dll") `
      -DelayLoadImport @("SHELL32.dll", "msvcp140_1.dll")

    Get-AppLocalRuntimeImport -DependencyOutput $output |
      Should -Be @("MSVCP140.dll", "VCOMP140.DLL", "KERNEL32.dll", "SHELL32.dll", "msvcp140_1.dll")
  }

  It "ignores OS-provided imports and matches shipped names case-insensitively" {
    $imports = @("KERNEL32.dll", "ADVAPI32.dll", "MSVCP140.dll", "VCOMP140.DLL", "msvcp140.dll")

    Get-AppLocalRuntimeGap -ImportName $imports -ShippedName $script:shipped |
      Should -BeNullOrEmpty
  }

  It "reports runtime suffixes that are not plain version numbers" {
    # msvcp140_atomic_wait.dll などは redist に実在し、語尾が数字ではない。
    # 命名規則の判定から漏れると、同梱漏れが素通りする。
    $imports = @(
      "KERNEL32.dll", "msvcp140_atomic_wait.dll", "msvcp140_codecvt_ids.dll",
      "vcruntime140_threads.dll", "vccorlib140.dll", "concrt140.dll")

    Get-AppLocalRuntimeGap -ImportName $imports -ShippedName $script:shipped |
      Should -Be @(
        "msvcp140_atomic_wait.dll", "msvcp140_codecvt_ids.dll",
        "vcruntime140_threads.dll", "vccorlib140.dll", "concrt140.dll")
  }

  It "reports a runtime that the redist provides but the MSI does not ship" {
    # msvcp140_1.dll は Microsoft.VC*.CRT に実在するが Package.wxs は列挙しない。
    # 取得元ディレクトリと突き合わせると見逃す経路。
    Get-AppLocalRuntimeGap `
      -ImportName @("MSVCP140.dll", "msvcp140_1.dll") `
      -ShippedName $script:shipped |
      Should -Be @("msvcp140_1.dll")
  }

  It "accepts a shipped binary whose runtime imports are all packaged" {
    $output = Get-DumpbinReport -Import @(
      "KERNEL32.dll", "MSVCP140.dll", "VCRUNTIME140.dll", "VCRUNTIME140_1.dll", "VCOMP140.DLL")

    $result = Assert-AppLocalRuntime `
      -BinaryPath @($script:binary) `
      -ShippedName $script:shipped `
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
        -ShippedName @("msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll") `
        -DependencyOutput @{ $script:binary = $output }
    } | Should -Throw "*pkg/msi/Package.wxs*imports VCOMP140.DLL*"
  }

  It "rejects a report with no import names instead of passing vacuously" {
    {
      Assert-AppLocalRuntime `
        -BinaryPath @($script:binary) `
        -ShippedName $script:shipped `
        -DependencyOutput @{ $script:binary = @("Dump of file azookey_inference_host.exe") }
    } | Should -Throw "*No imported DLL names were found*"
  }

  It "rejects a missing binary" {
    {
      Assert-AppLocalRuntime `
        -BinaryPath @((Join-Path $TestDrive "absent.exe")) `
        -ShippedName $script:shipped `
        -DependencyOutput @{}
    } | Should -Throw "*Binary to inspect does not exist*"
  }

  Context "shipped file names" {
    It "reads the runtime payload out of the real Package.wxs" {
      $names = Get-MsiShippedFileName -PackagePath (
        Join-Path $script:repoRoot "pkg\msi\Package.wxs")

      foreach ($expected in $script:shipped) {
        $names | Should -Contain $expected
      }
      # Name 属性があるファイルはインストール後の名前で数える。
      $names | Should -Contain "azookey_tsf_tip.dll"
      $names | Should -Contain "azookey_inference_host.exe"
      # redist にあるが Package.wxs が列挙しないものは含まれない。
      $names | Should -Not -Contain "msvcp140_1.dll"
      $names | Should -Not -Contain "concrt140.dll"
    }

    It "collects harvested payload directories recursively" {
      $payload = Join-Path $TestDrive "payload"
      New-Item -ItemType Directory -Path (Join-Path $payload "nested") -Force | Out-Null
      "a" | Set-Content -LiteralPath (Join-Path $payload "azookey_settings.exe")
      "b" | Set-Content -LiteralPath (Join-Path $payload "nested\Microsoft.UI.Xaml.dll")

      Get-AppLocalPayloadFileName -PayloadDirectory @($payload) |
        Should -Be @("azookey_settings.exe", "Microsoft.UI.Xaml.dll")
    }

    It "rejects a missing package or payload directory" {
      { Get-MsiShippedFileName -PackagePath (Join-Path $TestDrive "absent.wxs") } |
        Should -Throw "*package authoring not found*"
      { Get-AppLocalPayloadFileName -PayloadDirectory @((Join-Path $TestDrive "absent")) } |
        Should -Throw "*payload directory does not exist*"
    }
  }
}
