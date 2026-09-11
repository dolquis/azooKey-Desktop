Describe "VM package benchmark freshness" {
  BeforeAll {
    . (Join-Path $PSScriptRoot "..\make-vm-verify-package.ps1")
  }

  BeforeEach {
    $script:originalNinjaStatus = $env:NINJA_STATUS
    $script:build = Join-Path $TestDrive "build"
    New-Item -ItemType Directory -Path "$script:build\bench\generated" -Force | Out-Null
    @(
      "CMAKE_HOME_DIRECTORY:INTERNAL=$TestDrive"
      "AZOOKEY_BENCH_GIT_COMMIT:STRING="
    ) | Set-Content "$script:build\CMakeCache.txt"
    Mock Get-VmVerifyGitCommit { "a" * 40 }
    [System.IO.File]::WriteAllText("$script:build\bench\generated\BenchmarkCommit.h",
      "#pragma once`n#define AZOOKEY_BENCH_COMMIT `"$('a' * 40)`"`n")
  }

  AfterEach {
    $env:NINJA_STATUS = $script:originalNinjaStatus
  }

  It "normalizes Ninja status and restores the caller's value: <Status>" -TestCases @(
    @{ Status = '[%f/%t %es] ' }
    @{ Status = $null }
  ) {
    param($Status)
    $env:NINJA_STATUS = $Status
    Mock cmake {
      $env:NINJA_STATUS | Should -BeExactly '[%f/%t] '
      $global:LASTEXITCODE = 0
      '[1/1] Refreshing benchmark commit header'
    }
    Assert-VmVerifyBuildReady -BuildDirectory $script:build -IncludeBench
    $env:NINJA_STATUS | Should -BeExactly $Status
    Should -Invoke cmake -Times 1 -Exactly
  }

  It "restores Ninja status when the command fails: <Throws>" -TestCases @(
    @{ Throws = $false }
    @{ Throws = $true }
  ) {
    param($Throws)
    $script:commandThrows = $Throws
    $env:NINJA_STATUS = '[%f/%t %es] '
    Mock cmake {
      $env:NINJA_STATUS | Should -BeExactly '[%f/%t] '
      if ($script:commandThrows) { throw 'command invocation failed' }
      $global:LASTEXITCODE = 1
      'ninja: no work to do.'
    }
    { Assert-VmVerifyBuildReady -BuildDirectory $script:build -IncludeBench } | Should -Throw
    $env:NINJA_STATUS | Should -BeExactly '[%f/%t %es] '
    Should -Invoke cmake -Times 1 -Exactly
  }

  It "accepts only the known no-op generator with the current commit header" {
    { Assert-VmVerifyBuildReady -BuildDirectory $script:build -IncludeBench `
        -CommandResult ([pscustomobject]@{ ExitCode = 0; Output = '[1/1] Refreshing benchmark commit header' }) } |
      Should -Not -Throw
  }

  It "rejects a header from the previous commit" {
    Mock Get-VmVerifyGitCommit { "b" * 40 }
    { Assert-VmVerifyBuildReady -BuildDirectory $script:build -IncludeBench `
        -CommandResult ([pscustomobject]@{ ExitCode = 0; Output = '[1/1] Refreshing benchmark commit header' }) } |
      Should -Throw '*Build artifacts are stale*'
  }

  It "accepts the Windows line endings written by CMake" {
    $header = "$script:build\bench\generated\BenchmarkCommit.h"
    [System.IO.File]::WriteAllText($header,
      [System.IO.File]::ReadAllText($header).Replace("`n", "`r`n"))
    { Assert-VmVerifyBuildReady -BuildDirectory $script:build -IncludeBench `
        -CommandResult ([pscustomobject]@{ ExitCode = 0; Output = '[1/1] Refreshing benchmark commit header' }) } |
      Should -Not -Throw
  }

  It "rejects a missing generated header" {
    Remove-Item "$script:build\bench\generated\BenchmarkCommit.h"
    { Assert-VmVerifyBuildReady -BuildDirectory $script:build -IncludeBench `
        -CommandResult ([pscustomobject]@{ ExitCode = 0; Output = '[1/1] Refreshing benchmark commit header' }) } |
      Should -Throw '*Build artifacts are stale*'
  }

  It "respects a configured benchmark commit override" {
    Add-Content "$script:build\CMakeCache.txt" 'UNRELATED:STRING=value'
    (Get-Content "$script:build\CMakeCache.txt").Replace(
      'AZOOKEY_BENCH_GIT_COMMIT:STRING=', 'AZOOKEY_BENCH_GIT_COMMIT:STRING=custom') |
      Set-Content "$script:build\CMakeCache.txt"
    [System.IO.File]::WriteAllText("$script:build\bench\generated\BenchmarkCommit.h",
      "#pragma once`n#define AZOOKEY_BENCH_COMMIT `"custom`"`n")
    { Assert-VmVerifyBuildReady -BuildDirectory $script:build -IncludeBench `
        -CommandResult ([pscustomobject]@{ ExitCode = 0; Output = '[1/1] Refreshing benchmark commit header' }) } |
      Should -Not -Throw
    Should -Invoke Get-VmVerifyGitCommit -Times 0 -Exactly
  }

  It "rejects additional work or ambiguous output: <Output>" -TestCases @(
    @{ Output = "[1/2] Refreshing benchmark commit header`n[2/2] Building CXX object" }
    @{ Output = "[1/1] Building CXX object" }
    @{ Output = "[1/1] Refreshing benchmark commit header`nunknown command" }
    @{ Output = "ninja: no work to do.`n[1/1] Building CXX object" }
    @{ Output = "" }
  ) {
    param($Output)
    $null = $Output
    { Assert-VmVerifyBuildReady -BuildDirectory $script:build -IncludeBench `
        -CommandResult ([pscustomobject]@{ ExitCode = 0; Output = $Output }) } |
      Should -Throw '*Build artifacts are stale*'
  }

  It "rejects a failed command even when its output looks clean" {
    { Assert-VmVerifyBuildReady -BuildDirectory $script:build -IncludeBench `
        -CommandResult ([pscustomobject]@{ ExitCode = 1; Output = 'ninja: no work to do.' }) } |
      Should -Throw '*freshness check failed*'
  }

  It "checks diagnostics and optionally compat along with the main artifacts" {
    Mock cmake { $global:LASTEXITCODE = 0; 'ninja: no work to do.' }
    Assert-VmVerifyBuildReady -BuildDirectory $script:build -IncludeCompat -IncludeBench
    Should -Invoke cmake -Times 1 -Exactly -ParameterFilter {
      $arguments = $args | ForEach-Object { $_ }
      $arguments -contains 'azookey_diag' -and $arguments -contains 'compat_test' -and
      $arguments -contains 'azookey_zenzai_bench' -and $arguments -contains '-n'
    }
  }
}
