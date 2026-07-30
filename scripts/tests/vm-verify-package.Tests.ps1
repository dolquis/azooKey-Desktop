Describe "VM verification package automation" {
  BeforeAll {
    $repoRoot = (Resolve-Path (Join-Path (Join-Path $PSScriptRoot "..") "..")).Path
    . (Join-Path $repoRoot "scripts\make-vm-verify-package.ps1")
    . (Join-Path $repoRoot "scripts\verify-bootstrap.ps1")

    function Initialize-TestRepository {
      param(
        [Parameter(Mandatory = $true)]
        [string]$Root,
        [string]$BuildType = "Release",
        [switch]$SkipHost
      )

      New-Item -ItemType Directory -Path (Join-Path $Root "build\windows-release\tsf-tip") -Force | Out-Null
      New-Item -ItemType Directory -Path (Join-Path $Root "build\windows-release\inference-host") -Force | Out-Null
      New-Item -ItemType Directory -Path (Join-Path $Root "build\windows-release\diagnostics") -Force | Out-Null
      New-Item -ItemType Directory -Path (Join-Path $Root "build\windows-release\bench") -Force | Out-Null
      New-Item -ItemType Directory -Path (Join-Path $Root "scripts") -Force | Out-Null
      New-Item -ItemType Directory -Path (Join-Path $Root "docs\handoff") -Force | Out-Null

      @{
        version = 3
        configurePresets = @(
          @{
            name = "windows-release"
            binaryDir = '${sourceDir}/build/windows-release'
            cacheVariables = @{ CMAKE_BUILD_TYPE = "Release" }
          }
          @{
            name = "lower-priority-parent"
            binaryDir = '${sourceDir}/build/lower-priority'
            cacheVariables = @{
              CMAKE_BUILD_TYPE = "Debug"
              SHARED_VALUE = "lower"
            }
          }
          @{
            name = "higher-priority-parent"
            binaryDir = '${sourceDir}/build/higher-priority'
            cacheVariables = @{
              CMAKE_BUILD_TYPE = "RelWithDebInfo"
              SHARED_VALUE = "higher"
            }
          }
          @{
            name = "multi-parent"
            inherits = @("higher-priority-parent", "lower-priority-parent")
          }
        )
      } | ConvertTo-Json -Depth 5 |
        Set-Content -LiteralPath (Join-Path $Root "CMakePresets.json") -Encoding UTF8

      @(
        "CMAKE_BUILD_TYPE:STRING=$BuildType"
        "CMAKE_HOME_DIRECTORY:INTERNAL=$($Root.Replace('\', '/'))"
      ) | Set-Content `
        -LiteralPath (Join-Path $Root "build\windows-release\CMakeCache.txt") `
        -Encoding UTF8

      "tip" | Set-Content -LiteralPath (Join-Path $Root "build\windows-release\tsf-tip\azookey_tsf_tip.dll")
      if (-not $SkipHost) {
        "host" | Set-Content -LiteralPath (Join-Path $Root "build\windows-release\inference-host\azookey_inference_host.exe")
      }
      "diag" | Set-Content -LiteralPath (
        Join-Path $Root "build\windows-release\diagnostics\azookey_diag.exe")
      "bench" | Set-Content -LiteralPath (
        Join-Path $Root "build\windows-release\bench\azookey_zenzai_bench.exe")
      foreach ($scriptName in @(
          "register-dev.ps1",
          "unregister-dev.ps1",
          "host-supervisor.ps1",
          "AppContainerAcl.ps1",
          "verify-bootstrap.ps1")) {
        "# $scriptName" | Set-Content -LiteralPath (Join-Path $Root "scripts\$scriptName")
      }
      "# checklist" | Set-Content -LiteralPath (Join-Path $Root "docs\handoff\dev32-verification-checklist.md")
    }

    function Initialize-TestPackageRoot {
      param(
        [Parameter(Mandatory = $true)]
        [string]$Root
      )

      New-Item -ItemType Directory -Path $Root -Force | Out-Null
      foreach ($fileName in @(
          "azookey_tsf_tip.dll",
          "azookey_inference_host.exe",
          "register-dev.ps1",
          "host-supervisor.ps1")) {
        $fileName | Set-Content -LiteralPath (Join-Path $Root $fileName)
      }
      [ordered]@{
        schemaVersion = 1
        commit = ("a" * 40)
        preset = "windows-release"
        buildType = "Release"
        generatedAtUtc = "2026-07-29T00:00:00.0000000+00:00"
        files = @()
      } | ConvertTo-Json -Depth 5 |
        Set-Content -LiteralPath (Join-Path $Root "manifest.json") -Encoding UTF8
    }

    function Convert-TestGuidInitializer {
      param(
        [Parameter(Mandatory = $true)]
        [string]$Text,
        [Parameter(Mandatory = $true)]
        [string]$Name
      )

      $initializer = [regex]::Match(
        $Text,
        "(?s)$([regex]::Escape($Name))\s*=\s*\{(?<body>.*?)\};")
      if (-not $initializer.Success) {
        throw "GUID initializer not found: $Name"
      }
      $parts = @([regex]::Matches($initializer.Groups["body"].Value, "0x([0-9a-fA-F]+)") |
        ForEach-Object { [Convert]::ToUInt32($_.Groups[1].Value, 16) })
      if ($parts.Count -ne 11) {
        throw "Unexpected GUID initializer shape for $Name"
      }
      return ("{{{0:X8}-{1:X4}-{2:X4}-{3:X2}{4:X2}-{5:X2}{6:X2}{7:X2}{8:X2}{9:X2}{10:X2}}}" -f
        $parts)
    }
  }

  Context "make-vm-verify-package.ps1" {
    BeforeEach {
      $script:testRepository = Join-Path $TestDrive "repository"
      $script:testOutput = Join-Path $TestDrive "output"
      Initialize-TestRepository -Root $script:testRepository
    }

    It "creates a zip and a schema v1 manifest with required payload hashes" {
      Mock Get-VmVerifyGitCommit { "0123456789abcdef0123456789abcdef01234567" }
      Mock Assert-VmVerifyWorktreeClean {}
      Mock Assert-VmVerifyBuildReady {}

      $result = Export-VmVerifyPackage `
        -RepositoryRoot $script:testRepository `
        -PresetName "windows-release" `
        -DestinationDirectory $script:testOutput

      Test-Path -LiteralPath $result.ZipPath -PathType Leaf | Should -BeTrue
      Test-Path -LiteralPath $result.ManifestPath -PathType Leaf | Should -BeTrue

      $manifest = Get-Content -Raw -LiteralPath $result.ManifestPath | ConvertFrom-Json
      $manifest.schemaVersion | Should -Be 1
      $manifest.commit | Should -Be "0123456789abcdef0123456789abcdef01234567"
      $manifest.preset | Should -Be "windows-release"
      $manifest.buildType | Should -Be "Release"
      @($manifest.files).Count | Should -Be 9
      @($manifest.files | Where-Object { $_.role -eq "registration-dependency" }).Count |
        Should -Be 1
      foreach ($file in $manifest.files) {
        $file.sha256 | Should -Match "^[0-9a-f]{64}$"
        $file.size | Should -BeGreaterThan 0
      }

      $expanded = Join-Path $TestDrive "expanded"
      Expand-Archive -LiteralPath $result.ZipPath -DestinationPath $expanded
      Test-Path -LiteralPath (Join-Path $expanded "manifest.json") | Should -BeTrue
      Test-Path -LiteralPath (Join-Path $expanded "AppContainerAcl.ps1") | Should -BeTrue
      Test-Path -LiteralPath (Join-Path $expanded "verify-bootstrap.ps1") | Should -BeTrue
      Test-Path -LiteralPath (Join-Path $expanded "azookey_diag.exe") | Should -BeTrue
      $manifestBytes = [System.IO.File]::ReadAllBytes($result.ManifestPath)
      [BitConverter]::ToString($manifestBytes[0..2]) | Should -Not -Be "EF-BB-BF"
      Should -Invoke Assert-VmVerifyWorktreeClean -Times 1 -Exactly
      Should -Invoke Assert-VmVerifyBuildReady -Times 1 -Exactly
    }

    It "rejects a missing release artifact" {
      Remove-Item -LiteralPath (
        Join-Path $script:testRepository "build\windows-release\inference-host\azookey_inference_host.exe")

      {
        Export-VmVerifyPackage `
          -RepositoryRoot $script:testRepository `
          -PresetName "windows-release" `
          -DestinationDirectory $script:testOutput
      } | Should -Throw "*Required build artifact is missing*"
    }

    It "rejects a preset and CMake cache build type mismatch" {
      $cachePath = Join-Path $script:testRepository "build\windows-release\CMakeCache.txt"
      (Get-Content -Raw -LiteralPath $cachePath).Replace(
        "CMAKE_BUILD_TYPE:STRING=Release",
        "CMAKE_BUILD_TYPE:STRING=Debug") |
        Set-Content -LiteralPath $cachePath -Encoding UTF8

      {
        Export-VmVerifyPackage `
          -RepositoryRoot $script:testRepository `
          -PresetName "windows-release" `
          -DestinationDirectory $script:testOutput
      } | Should -Throw "*Preset/build mismatch*"
    }

    It "uses the first parent as the highest-precedence inherited preset" {
      $configuration = Get-VmVerifyPresetConfiguration `
        -PresetsPath (Join-Path $script:testRepository "CMakePresets.json") `
        -PresetName "multi-parent" `
        -RepositoryRoot $script:testRepository

      $configuration.BinaryDir | Should -Be (
        Join-Path $script:testRepository "build\higher-priority")
      $configuration.CacheVariables["CMAKE_BUILD_TYPE"] | Should -Be "RelWithDebInfo"
      $configuration.CacheVariables["SHARED_VALUE"] | Should -Be "higher"
    }

    It "rejects dry-run output that still contains build work" {
      {
        Assert-VmVerifyBuildReady `
          -BuildDirectory "C:\build" `
          -CommandResult ([pscustomobject]@{
              ExitCode = 0
              Output = "[1/2] Building CXX object"
            })
      } | Should -Throw "*Build artifacts are stale*"
    }

    It "rejects a dirty worktree result" {
      {
        Assert-VmVerifyWorktreeClean `
          -RepositoryRoot $script:testRepository `
          -CommandResult ([pscustomobject]@{
              ExitCode = 0
              Output = "?? package-output\old.zip"
            })
      } | Should -Throw "*worktree is not clean*"
    }

    It "normalizes a validated 40-character commit to lowercase" {
      Get-VmVerifyGitCommit `
        -RepositoryRoot $script:testRepository `
        -CommandResult ([pscustomobject]@{
            ExitCode = 0
            Output = "ABCDEF0123456789ABCDEF0123456789ABCDEF01"
          }) |
        Should -Be "abcdef0123456789abcdef0123456789abcdef01"
    }

    It "packages the llama preflight bench beside an optional GGUF model" {
      Mock Get-VmVerifyGitCommit { "0123456789abcdef0123456789abcdef01234567" }
      Mock Assert-VmVerifyWorktreeClean {}
      Mock Assert-VmVerifyBuildReady {}
      $model = Join-Path $TestDrive "model.gguf"
      "model" | Set-Content -LiteralPath $model

      $result = Export-VmVerifyPackage `
        -RepositoryRoot $script:testRepository `
        -PresetName "windows-release" `
        -DestinationDirectory $script:testOutput `
        -Model $model

      @($result.Manifest.files | Where-Object { $_.role -eq "gguf-model" }).Count |
        Should -Be 1
      @($result.Manifest.files | Where-Object { $_.role -eq "llama-preflight" }).Count |
        Should -Be 1
      Should -Invoke Assert-VmVerifyBuildReady `
        -Times 1 `
        -Exactly `
        -ParameterFilter { $IncludeBench }
    }
  }

  Context "verify-bootstrap.ps1" {
    BeforeEach {
      $script:testPackageRoot = Join-Path $TestDrive "package"
      Initialize-TestPackageRoot -Root $script:testPackageRoot
      $script:bootstrapState = @{
        Runtime = $true
        Registered = $false
        Pipe = $false
      }

      Mock Test-VmVerifyAdministrator { $true }
      Mock Test-VmVerifyVCRuntime { $script:bootstrapState.Runtime }
      Mock Test-VmVerifyRegistration { $script:bootstrapState.Registered }
      Mock Test-VmVerifyPipe { $script:bootstrapState.Pipe }
      Mock Test-VmVerifyMicrosoftIme { $true }
      Mock Invoke-VmVerifyRegistration {
        $script:bootstrapState.Registered = $true
        $script:bootstrapState.Pipe = $true
      }
      Mock Invoke-VmVerifyHostSupervisor {
        $script:bootstrapState.Pipe = $true
      }
      Mock Wait-VmVerifyPipe { $script:bootstrapState.Pipe }
      Mock Install-VmVerifyVCRuntime {
        $script:bootstrapState.Runtime = $true
      }
    }

    It "keeps registration and host startup idempotent across two runs" {
      $first = Invoke-VmVerifyBootstrap `
        -PackageRoot $script:testPackageRoot `
        -HasCheckpoint
      $second = Invoke-VmVerifyBootstrap `
        -PackageRoot $script:testPackageRoot `
        -HasCheckpoint

      $first.overallStatus | Should -Be "pass"
      $second.overallStatus | Should -Be "pass"
      Should -Invoke Invoke-VmVerifyRegistration -Times 1 -Exactly
      Should -Invoke Invoke-VmVerifyHostSupervisor -Times 0 -Exactly
    }

    It "starts one supervisor when registration exists but the host pipe is absent" {
      $script:bootstrapState.Registered = $true

      Invoke-VmVerifyBootstrap -PackageRoot $script:testPackageRoot -HasCheckpoint | Out-Null
      Invoke-VmVerifyBootstrap -PackageRoot $script:testPackageRoot -HasCheckpoint | Out-Null

      Should -Invoke Invoke-VmVerifyRegistration -Times 0 -Exactly
      Should -Invoke Invoke-VmVerifyHostSupervisor `
        -Times 1 `
        -Exactly `
        -ParameterFilter { $PipeName -match "^azookey-" }
    }

    It "returns the stable JSON schema and fixed check identifiers" {
      $script:bootstrapState.Registered = $true
      $script:bootstrapState.Pipe = $true

      $result = Invoke-VmVerifyBootstrap `
        -PackageRoot $script:testPackageRoot `
        -HasCheckpoint
      $json = $result | ConvertTo-Json -Depth 6 | ConvertFrom-Json

      $json.schemaVersion | Should -Be 1
      $json.generatedAtUtc | Should -Not -BeNullOrEmpty
      $json.package.commit | Should -Be ("a" * 40)
      $json.overallStatus | Should -Be "pass"
      @($json.actions.PSObject.Properties.Name) | Should -Be @(
        "vcRuntimeInstalled",
        "tipRegistered",
        "hostSupervisorStarted")
      @($json.checks.id) | Should -Be @(
        "vcRuntime",
        "tipRegistration",
        "inferenceHost",
        "microsoftIme",
        "vmCheckpoint",
        "debugView")
      @($json.checks.status | Where-Object {
          $_ -notin @("pass", "fail", "manual_required", "not_applicable")
        }).Count | Should -Be 0
    }

    It "keeps per-user registration in the interactive non-admin process" {
      Mock Test-VmVerifyAdministrator { $false }

      $result = Invoke-VmVerifyBootstrap `
        -PackageRoot $script:testPackageRoot `
        -HasCheckpoint

      $result.overallStatus | Should -Be "pass"
      Should -Invoke Invoke-VmVerifyRegistration -Times 1 -Exactly
    }

    It "auto-discovers model, bench, and mock dictionary payloads from the manifest" {
      $modelDirectory = Join-Path $script:testPackageRoot "models"
      $dataDirectory = Join-Path $script:testPackageRoot "data"
      New-Item -ItemType Directory -Path $modelDirectory, $dataDirectory -Force | Out-Null
      "model" | Set-Content -LiteralPath (Join-Path $modelDirectory "model.gguf")
      "bench" | Set-Content -LiteralPath (
        Join-Path $script:testPackageRoot "azookey_zenzai_bench.exe")
      "kana`tword" | Set-Content -LiteralPath (Join-Path $dataDirectory "mock.tsv")
      $manifestPath = Join-Path $script:testPackageRoot "manifest.json"
      $manifest = Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
      $manifest.files = @(
        [pscustomobject]@{ path = "models/model.gguf"; role = "gguf-model" }
        [pscustomobject]@{ path = "azookey_zenzai_bench.exe"; role = "llama-preflight" }
        [pscustomobject]@{ path = "data/mock.tsv"; role = "mock-dictionary" }
      )
      $manifest | ConvertTo-Json -Depth 5 |
        Set-Content -LiteralPath $manifestPath -Encoding UTF8

      Invoke-VmVerifyBootstrap -PackageRoot $script:testPackageRoot -HasCheckpoint | Out-Null

      Should -Invoke Invoke-VmVerifyRegistration `
        -Times 1 `
        -Exactly `
        -ParameterFilter {
          $Model -like "*model.gguf" -and
          $Bench -like "*azookey_zenzai_bench.exe" -and
          $MockDictionary -like "*mock.tsv"
        }
    }

    It "keeps the registration CLSID aligned with the C++ source of truth" {
      $header = Get-Content -Raw -LiteralPath (
        Join-Path $repoRoot "tsf-tip\include\azookey\tsf\TextServiceFactory.h")
      $expected = Convert-TestGuidInitializer `
        -Text $header `
        -Name "kTextServiceClsid"

      $script:VmVerifyTextServiceClsid | Should -BeExactly $expected
    }
  }

  Context "bootstrap command wrappers" {
    It "captures registration script streams so JSON output stays clean" {
      $registerScript = Join-Path $TestDrive "noisy-register.ps1"
      @'
param(
  [string]$TipDllPath,
  [string]$HostExePath,
  [switch]$AllowMockHost
)
Write-Host "host output"
Write-Warning "warning output"
Write-Output "pipeline output"
'@ | Set-Content -LiteralPath $registerScript -Encoding UTF8

      $output = Invoke-VmVerifyRegistration `
        -RegisterScriptPath $registerScript `
        -TipDll "C:\package\azookey_tsf_tip.dll" `
        -HostExe "C:\package\azookey_inference_host.exe"

      $output | Should -Match "host output"
      $output | Should -Match "warning output"
      $output | Should -Match "pipeline output"
    }

    It "reports registration exit codes with captured diagnostics" {
      $registerScript = Join-Path $TestDrive "failing-register.ps1"
      @'
param(
  [string]$TipDllPath,
  [string]$HostExePath,
  [switch]$AllowMockHost
)
Write-Output "diagnostic detail"
exit 23
'@ | Set-Content -LiteralPath $registerScript -Encoding UTF8

      {
        Invoke-VmVerifyRegistration `
          -RegisterScriptPath $registerScript `
          -TipDll "C:\package\azookey_tsf_tip.dll" `
          -HostExe "C:\package\azookey_inference_host.exe"
      } | Should -Throw "*exit code 23*diagnostic detail*"
    }

    It "elevates only a bundled VC runtime installer when needed" {
      Mock Test-VmVerifyAdministrator { $false }
      Mock Start-Process { [pscustomobject]@{ ExitCode = 0 } }

      Install-VmVerifyVCRuntime -InstallerPath "C:\package\vc_redist.x64.exe"

      Should -Invoke Start-Process `
        -Times 1 `
        -Exactly `
        -ParameterFilter { $Verb -eq "RunAs" }
    }
  }
}
