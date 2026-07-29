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
  }

  Context "make-vm-verify-package.ps1" {
    BeforeEach {
      $script:testRepository = Join-Path $TestDrive "repository"
      $script:testOutput = Join-Path $TestDrive "output"
      Initialize-TestRepository -Root $script:testRepository
      Mock Get-VmVerifyGitCommit { "0123456789abcdef0123456789abcdef01234567" }
      Mock Assert-VmVerifyWorktreeClean {}
      Mock Assert-VmVerifyBuildReady {}
    }

    It "creates a zip and a schema v1 manifest with required payload hashes" {
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
      @($manifest.files).Count | Should -Be 8
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
      Assert-MockCalled Assert-VmVerifyWorktreeClean -Times 1 -Exactly
      Assert-MockCalled Assert-VmVerifyBuildReady -Times 1 -Exactly
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
      Assert-MockCalled Invoke-VmVerifyRegistration -Times 1 -Exactly
      Assert-MockCalled Invoke-VmVerifyHostSupervisor -Times 0 -Exactly
    }

    It "starts one supervisor when registration exists but the host pipe is absent" {
      $script:bootstrapState.Registered = $true

      Invoke-VmVerifyBootstrap -PackageRoot $script:testPackageRoot -HasCheckpoint | Out-Null
      Invoke-VmVerifyBootstrap -PackageRoot $script:testPackageRoot -HasCheckpoint | Out-Null

      Assert-MockCalled Invoke-VmVerifyRegistration -Times 0 -Exactly
      Assert-MockCalled Invoke-VmVerifyHostSupervisor -Times 1 -Exactly
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

    It "suppresses registration script streams so JSON output stays clean" {
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

      $output = @(Invoke-VmVerifyRegistration `
        -RegisterScriptPath $registerScript `
        -TipDll "C:\package\azookey_tsf_tip.dll" `
        -HostExe "C:\package\azookey_inference_host.exe")

      $output.Count | Should -Be 0
    }
  }
}
