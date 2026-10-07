Describe "VM package ETW resource freshness" {
  BeforeAll {
    . (Join-Path $PSScriptRoot "..\make-vm-verify-package.ps1")
  }

  BeforeEach {
    $script:repository = Join-Path $TestDrive ([guid]::NewGuid().ToString("N"))
    $script:build = Join-Path $script:repository "build"
    $script:generated = Join-Path $script:build "diagnostics\etw\generated"
    $script:dll = Join-Path $script:build "diagnostics\etw\azookey_etw_manifest.dll"
    $script:resourceObject = Join-Path $script:build "diagnostics\etw\CMakeFiles\azookey_etw_manifest.dir\generated\AzooKey.rc.res"
    foreach ($seedRelative in @(
        "diagnostics\etw\AzooKey.man", "diagnostics\etw\CMakeLists.txt",
        "build\CMakeCache.txt", "build\build.ninja",
        "build\diagnostics\etw\CMakeFiles\azookey_etw_manifest.dir\generated\AzooKey.rc.res",
        "build\diagnostics\etw\generated\AzooKey.rc",
        "build\diagnostics\etw\generated\MSG00001.bin",
        "build\diagnostics\etw\generated\AzooKeyTEMP.BIN",
        "build\diagnostics\etw\azookey_etw_manifest.dll")) {
      $path = Join-Path $script:repository $seedRelative
      New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null
      $seedRelative | Set-Content -LiteralPath $path
      (Get-Item -LiteralPath $path).LastWriteTimeUtc = [DateTime]::UtcNow.AddHours(-2)
    }
    @('1 11 "MSG00001.bin"', '1 WEVT_TEMPLATE "AzooKeyTEMP.BIN"') |
      Set-Content -LiteralPath (Join-Path $script:generated "AzooKey.rc")
    (Get-Item -LiteralPath (Join-Path $script:generated "AzooKey.rc")).LastWriteTimeUtc =
      [DateTime]::UtcNow.AddHours(-2)
    (Get-Item -LiteralPath $script:resourceObject).LastWriteTimeUtc =
      [DateTime]::UtcNow.AddMinutes(-90)
    (Get-Item -LiteralPath $script:dll).LastWriteTimeUtc = [DateTime]::UtcNow.AddHours(-1)
    $script:originalStatus = $env:NINJA_STATUS
    Mock cmake {
      $global:LASTEXITCODE = 0
      '[1/1] Linking CXX shared library diagnostics\etw\azookey_etw_manifest.dll'
    }
  }

  AfterEach {
    $env:NINJA_STATUS = $script:originalStatus
  }

  It "accepts only the expected resource DLL relink and restores Ninja status" {
    $env:NINJA_STATUS = '[%f/%t %es] '
    Mock cmake {
      $env:NINJA_STATUS | Should -BeExactly '[%f/%t] '
      $global:LASTEXITCODE = 0
      '[1/1] Linking CXX shared library diagnostics\etw\azookey_etw_manifest.dll'
    }
    Assert-VmVerifyDiagnosticsFresh -RepositoryRoot $script:repository `
      -BuildDirectory $script:build -Preset "windows-release"
    $env:NINJA_STATUS | Should -BeExactly '[%f/%t %es] '
    Should -Invoke cmake -Times 1 -Exactly -ParameterFilter {
      $args -contains 'azookey_etw_manifest' -and $args -contains '-n'
    }
  }

  It "accepts an idle graph or the same single relink with forward slashes: <Output>" -ForEach @(
    @{ Output = 'ninja: no work to do.' }
    @{ Output = '[1/1] Linking CXX shared library diagnostics/etw/azookey_etw_manifest.dll' }
  ) {
    Assert-VmVerifyDiagnosticsFresh -RepositoryRoot $script:repository `
      -BuildDirectory $script:build -Preset "windows-release" `
      -CommandResult ([pscustomobject]@{ ExitCode = 0; Output = $Output })
  }

  It "rejects a missing input or artifact: <Relative>" -ForEach @(
    @{ Relative = "diagnostics\etw\AzooKey.man" }
    @{ Relative = "diagnostics\etw\CMakeLists.txt" }
    @{ Relative = "build\CMakeCache.txt" }
    @{ Relative = "build\build.ninja" }
    @{ Relative = "build\diagnostics\etw\CMakeFiles\azookey_etw_manifest.dir\generated\AzooKey.rc.res" }
    @{ Relative = "build\diagnostics\etw\generated\AzooKey.rc" }
    @{ Relative = "build\diagnostics\etw\azookey_etw_manifest.dll" }
    @{ Relative = "build\diagnostics\etw\generated\MSG00001.bin" }
    @{ Relative = "build\diagnostics\etw\generated\AzooKeyTEMP.BIN" }
  ) {
    Remove-Item -LiteralPath (Join-Path $script:repository $Relative)
    {
      Assert-VmVerifyDiagnosticsFresh -RepositoryRoot $script:repository `
        -BuildDirectory $script:build -Preset "windows-release"
    } | Should -Throw "*diagnostics*missing*"
    Should -Invoke cmake -Times 0 -Exactly
  }

  It "rejects an input changed after the DLL despite an allowed relink: <Relative>" -ForEach @(
    @{ Relative = "diagnostics\etw\AzooKey.man" }
    @{ Relative = "diagnostics\etw\CMakeLists.txt" }
    @{ Relative = "build\CMakeCache.txt" }
    @{ Relative = "build\build.ninja" }
    @{ Relative = "build\diagnostics\etw\CMakeFiles\azookey_etw_manifest.dir\generated\AzooKey.rc.res" }
    @{ Relative = "build\diagnostics\etw\generated\AzooKey.rc" }
    @{ Relative = "build\diagnostics\etw\generated\MSG00001.bin" }
    @{ Relative = "build\diagnostics\etw\generated\AzooKeyTEMP.BIN" }
  ) {
    (Get-Item -LiteralPath (Join-Path $script:repository $Relative)).LastWriteTimeUtc = [DateTime]::UtcNow
    {
      Assert-VmVerifyDiagnosticsFresh -RepositoryRoot $script:repository `
        -BuildDirectory $script:build -Preset "windows-release"
    } | Should -Throw "*Diagnostics artifacts are stale*--target azookey_etw_manifest*"
  }

  It "rejects a resource changed after compilation even when the DLL was relinked: <Relative>" -ForEach @(
    @{ Relative = "AzooKey.rc" }
    @{ Relative = "MSG00001.bin" }
    @{ Relative = "AzooKeyTEMP.BIN" }
  ) {
    (Get-Item -LiteralPath (Join-Path $script:generated $Relative)).LastWriteTimeUtc =
      [DateTime]::UtcNow.AddMinutes(-75)
    {
      Assert-VmVerifyDiagnosticsFresh -RepositoryRoot $script:repository `
        -BuildDirectory $script:build -Preset "windows-release"
    } | Should -Throw "*changed after*AzooKey.rc.res*--clean-first*"
    Should -Invoke cmake -Times 0 -Exactly
  }

  It "refuses to guess when RC has no message-table reference" {
    "invalid rc" | Set-Content -LiteralPath (Join-Path $script:generated "AzooKey.rc")
    {
      Assert-VmVerifyDiagnosticsFresh -RepositoryRoot $script:repository `
        -BuildDirectory $script:build -Preset "windows-release"
    } | Should -Throw "*No resource input*"
  }

  It "rejects message tables outside this build's generated directory" {
    '1 11 "..\outside.bin"' | Set-Content -LiteralPath (Join-Path $script:generated "AzooKey.rc")
    "table" | Set-Content -LiteralPath (Join-Path $script:generated "..\outside.bin")
    {
      Assert-VmVerifyDiagnosticsFresh -RepositoryRoot $script:repository `
        -BuildDirectory $script:build -Preset "windows-release"
    } | Should -Throw "*outside the generated directory*"
  }

  It "checks every locale referenced by RC" {
    @('1 11 "MSG00001.bin"', '1 11 "MSG00002.bin"') |
      Set-Content -LiteralPath (Join-Path $script:generated "AzooKey.rc")
    {
      Assert-VmVerifyDiagnosticsFresh -RepositoryRoot $script:repository `
        -BuildDirectory $script:build -Preset "windows-release"
    } | Should -Throw "*resource is missing*MSG00002.bin*"
  }

  It "rejects additional work even with a recent DLL: <Output>" -ForEach @(
    @{ Output = '[1/1] Building RC object diagnostics/etw/AzooKey.rc.res' }
    @{ Output = "[1/2] Generating AzooKey.rc`n[2/2] Linking CXX shared library diagnostics/etw/azookey_etw_manifest.dll" }
    @{ Output = "[1/1] Linking CXX shared library diagnostics/etw/azookey_etw_manifest.dll`nunknown output" }
    @{ Output = '[1/1] Linking CXX shared library other.dll' }
    @{ Output = '' }
  ) {
    (Get-Item -LiteralPath $script:dll).LastWriteTimeUtc = [DateTime]::UtcNow
    {
      Assert-VmVerifyDiagnosticsFresh -RepositoryRoot $script:repository `
        -BuildDirectory $script:build -Preset "windows-release" `
        -CommandResult ([pscustomobject]@{ ExitCode = 0; Output = $Output })
    } | Should -Throw "*Diagnostics artifacts are stale*"
  }

  It "restores status after a failed command or invocation: <Throws>" -ForEach @(
    @{ Throws = $false }
    @{ Throws = $true }
  ) {
    $script:throws = $Throws
    $env:NINJA_STATUS = '[%f/%t %es] '
    Mock cmake {
      if ($script:throws) { throw 'invocation failed' }
      $global:LASTEXITCODE = 1
      'ninja: no work to do.'
    }
    {
      Assert-VmVerifyDiagnosticsFresh -RepositoryRoot $script:repository `
        -BuildDirectory $script:build -Preset "windows-release"
    } | Should -Throw
    $env:NINJA_STATUS | Should -BeExactly '[%f/%t %es] '
  }
}
