Describe "VM verification package automation" {
  BeforeAll {
    $repoRoot = (Resolve-Path (Join-Path (Join-Path $PSScriptRoot "..") "..")).Path
    . (Join-Path $repoRoot "scripts\make-vm-verify-package.ps1")
    . (Join-Path $repoRoot "scripts\verify-bootstrap.ps1")

    # settings-app/CMakeLists.txt の AZOOKEY_SETTINGS_OUTPUT_DIR を模した publish 出力。
    # 除外されるべき中間生成物と、保持されるべき入れ子ランタイムの双方を含める。
    function Initialize-SettingsAppPayload {
      param(
        [Parameter(Mandatory = $true)]
        [string]$Root,
        [string]$Configuration = "Release"
      )

      $payloadRoot = Join-Path $Root "build\windows-release\settings-app\$Configuration"
      $nativeDirectory = Join-Path $payloadRoot "runtimes\win-x64\native"
      $objDirectory = Join-Path $payloadRoot "obj"
      New-Item -ItemType Directory -Path $nativeDirectory, $objDirectory -Force | Out-Null

      "settings" | Set-Content -LiteralPath (Join-Path $payloadRoot "azookey_settings.exe")
      "managed" | Set-Content -LiteralPath (Join-Path $payloadRoot "azookey_settings.dll")
      "native" | Set-Content -LiteralPath (Join-Path $nativeDirectory "runtime.dll")
      "symbols" | Set-Content -LiteralPath (Join-Path $payloadRoot "azookey_settings.pdb")
      "import" | Set-Content -LiteralPath (Join-Path $payloadRoot "azookey_settings.lib")
      "exports" | Set-Content -LiteralPath (Join-Path $payloadRoot "azookey_settings.exp")
      "incremental" | Set-Content -LiteralPath (Join-Path $payloadRoot "azookey_settings.ilk")
      "intermediate" | Set-Content -LiteralPath (Join-Path $objDirectory "intermediate.txt")
    }

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
        files = @(
          [ordered]@{
            path = "azookey_inference_host.exe"
            role = "inference-host"
            size = (Get-Item -LiteralPath (
                Join-Path $Root "azookey_inference_host.exe")).Length
            sha256 = (Get-FileHash `
                -LiteralPath (Join-Path $Root "azookey_inference_host.exe") `
                -Algorithm SHA256).Hash.ToLowerInvariant()
          }
        )
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

    It "omits the settings app unless it is requested" {
      Mock Get-VmVerifyGitCommit { "0123456789abcdef0123456789abcdef01234567" }
      Mock Assert-VmVerifyWorktreeClean {}
      Mock Assert-VmVerifyBuildReady {}
      Initialize-SettingsAppPayload -Root $script:testRepository

      $result = Export-VmVerifyPackage `
        -RepositoryRoot $script:testRepository `
        -PresetName "windows-release" `
        -DestinationDirectory $script:testOutput

      $manifest = Get-Content -Raw -LiteralPath $result.ManifestPath | ConvertFrom-Json
      @($manifest.files | Where-Object { $_.role -eq "settings-app" }).Count | Should -Be 0
    }

    It "bundles the settings app payload and drops the MSI-excluded build outputs" {
      Mock Get-VmVerifyGitCommit { "0123456789abcdef0123456789abcdef01234567" }
      Mock Assert-VmVerifyWorktreeClean {}
      Mock Assert-VmVerifyBuildReady {}
      Initialize-SettingsAppPayload -Root $script:testRepository

      $result = Export-VmVerifyPackage `
        -RepositoryRoot $script:testRepository `
        -PresetName "windows-release" `
        -DestinationDirectory $script:testOutput `
        -SettingsApp

      $manifest = Get-Content -Raw -LiteralPath $result.ManifestPath | ConvertFrom-Json
      $settingsFiles = @($manifest.files | Where-Object { $_.role -eq "settings-app" })
      $settingsPaths = @($settingsFiles | ForEach-Object { $_.path })

      $settingsPaths | Should -Contain "settings-app/azookey_settings.exe"
      $settingsPaths | Should -Contain "settings-app/azookey_settings.dll"
      $settingsPaths | Should -Contain "settings-app/runtimes/win-x64/native/runtime.dll"
      # pkg/msi/Package.wxs の harvest 除外と揃っていること。
      $settingsPaths | Should -Not -Contain "settings-app/azookey_settings.pdb"
      $settingsPaths | Should -Not -Contain "settings-app/azookey_settings.lib"
      $settingsPaths | Should -Not -Contain "settings-app/azookey_settings.exp"
      $settingsPaths | Should -Not -Contain "settings-app/azookey_settings.ilk"
      $settingsPaths | Should -Not -Contain "settings-app/obj/intermediate.txt"
      foreach ($file in $settingsFiles) {
        $file.sha256 | Should -Match "^[0-9a-f]{64}$"
      }

      # zip の entry 名は OS の区切り文字に依存しないので、展開せず直接照合する。
      Add-Type -AssemblyName System.IO.Compression.FileSystem
      $archive = [System.IO.Compression.ZipFile]::OpenRead($result.ZipPath)
      try {
        $entryNames = @($archive.Entries | ForEach-Object { $_.FullName.TrimStart("/") })
      } finally {
        $archive.Dispose()
      }
      $entryNames | Should -Contain "settings-app/azookey_settings.exe"
      $entryNames | Should -Contain "settings-app/runtimes/win-x64/native/runtime.dll"
      $entryNames | Should -Not -Contain "settings-app/azookey_settings.pdb"
    }

    It "rejects a requested settings app that has not been built" {
      Mock Get-VmVerifyGitCommit { "0123456789abcdef0123456789abcdef01234567" }
      Mock Assert-VmVerifyWorktreeClean {}
      Mock Assert-VmVerifyBuildReady {}
      # Initialize-TestRepository は -Force で再作成するため、同じ TestDrive 配下では
      # 先行 It が置いた settings-app 出力が残る。専用の root で未ビルド状態を作る。
      $unbuilt = Join-Path $TestDrive "repository-without-settings-app"
      Initialize-TestRepository -Root $unbuilt

      {
        Export-VmVerifyPackage `
          -RepositoryRoot $unbuilt `
          -PresetName "windows-release" `
          -DestinationDirectory $script:testOutput `
          -SettingsApp
      } | Should -Throw "*Required VM verification payload is missing (settings-app)*"
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

  Context "serving host pipe ownership" {
    It "resolves the process that owns the named pipe" {
      Mock Get-VmVerifyNamedPipeServerProcessId { 4242 }
      Mock Get-Process {
        [pscustomobject]@{
          Id = 4242
          Path = "C:\package\azookey_inference_host.exe"
        }
      } -ParameterFilter { $Id -eq 4242 }

      $process = Get-VmVerifyServingHostProcess -PipeName "azookey-test"

      $process.Id | Should -Be 4242
      $process.Path | Should -BeExactly "C:\package\azookey_inference_host.exe"
      Should -Invoke Get-VmVerifyNamedPipeServerProcessId `
        -Times 1 `
        -Exactly `
        -ParameterFilter { $PipeName -eq "azookey-test" }
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
      Mock Get-VmVerifyServingHostProcess {
        [pscustomobject]@{
          Id = 4242
          Path = (Join-Path $script:testPackageRoot "azookey_inference_host.exe")
        }
      }
      Mock Invoke-VmVerifyHostSupervisorShutdown {}
      Mock Wait-VmVerifyHostSupervisorStopped { $true }
      Mock Invoke-VmVerifyHostProcessTermination { $script:bootstrapState.Pipe = $false }
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
      $json.hostBinary.status | Should -Be "reused"
      @($json.checks.id) | Should -Be @(
        "vcRuntime",
        "tipRegistration",
        "inferenceHost",
        "microsoftIme",
        "vmCheckpoint",
        "debugView")
      @($json.checks | Where-Object { $_.id -eq "inferenceHost" }).Count |
        Should -Be 1
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
      $hostEntry = @($manifest.files | Where-Object { $_.role -eq "inference-host" })[0]
      $manifest.files = @(
        $hostEntry
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

    It "compares the serving host path and hash with the bundled manifest" {
      $script:bootstrapState.Pipe = $true
      $script:servingHostPath = Join-Path $script:testPackageRoot "azookey_inference_host.exe"
      Mock Get-VmVerifyServingHostProcess {
        [pscustomobject]@{ Id = 4242; Path = $script:servingHostPath }
      }
      $manifest = Get-Content -Raw -LiteralPath (
        Join-Path $script:testPackageRoot "manifest.json") | ConvertFrom-Json

      $matched = Get-VmVerifyServingHostBinary `
        -PipeName "azookey-test" `
        -HostExe (Join-Path $script:testPackageRoot "azookey_inference_host.exe") `
        -Manifest $manifest
      $matched.status | Should -Be "matched"

      $script:servingHostPath = Join-Path $TestDrive "stale-host.exe"
      "stale" | Set-Content -LiteralPath $script:servingHostPath
      Mock Get-VmVerifyServingHostProcess {
        [pscustomobject]@{ Id = 4242; Path = $script:servingHostPath }
      }
      $mismatched = Get-VmVerifyServingHostBinary `
        -PipeName "azookey-test" `
        -HostExe (Join-Path $script:testPackageRoot "azookey_inference_host.exe") `
        -Manifest $manifest
      $mismatched.status | Should -Be "mismatched"

      $script:servingHostPath = Join-Path $TestDrive "copied-host.exe"
      Copy-Item `
        -LiteralPath (Join-Path $script:testPackageRoot "azookey_inference_host.exe") `
        -Destination $script:servingHostPath
      $differentPath = Get-VmVerifyServingHostBinary `
        -PipeName "azookey-test" `
        -HostExe (Join-Path $script:testPackageRoot "azookey_inference_host.exe") `
        -Manifest $manifest
      $differentPath.runningSha256 | Should -Be $differentPath.expectedSha256
      $differentPath.status | Should -Be "mismatched"
    }

    It "restarts a serving host when its hash differs from the bundled host" {
      $script:bootstrapState.Registered = $true
      $script:bootstrapState.Pipe = $true
      $script:hostInspectionCount = 0
      Mock Get-VmVerifyServingHostBinary {
        $script:hostInspectionCount++
        [pscustomobject][ordered]@{
          status = $(if ($script:hostInspectionCount -eq 1) { "mismatched" } else { "matched" })
          reason = "test inspection"
          processId = 4242
          runningPath = "C:\package\host.exe"
          runningSha256 = $(if ($script:hostInspectionCount -eq 1) { "a" * 64 } else { "b" * 64 })
          expectedPath = "C:\package\azookey_inference_host.exe"
          expectedSha256 = ("b" * 64)
        }
      }
      Mock Wait-VmVerifyPipe {
        param($PipeName, $TimeoutSeconds = 15, $ExpectedPresent = $true)
        $null = $PipeName
        $null = $TimeoutSeconds
        return $script:bootstrapState.Pipe -eq $ExpectedPresent
      }

      $result = Invoke-VmVerifyBootstrap `
        -PackageRoot $script:testPackageRoot `
        -HasCheckpoint

      $result.overallStatus | Should -Be "pass"
      $result.hostBinary.status | Should -Be "restarted"
      $script:hostInspectionCount | Should -Be 2
      @($result.checks | Where-Object { $_.id -eq "inferenceHost" }).Count |
        Should -Be 1
      Should -Invoke Invoke-VmVerifyHostSupervisorShutdown -Times 1 -Exactly
      Should -Invoke Invoke-VmVerifyHostProcessTermination `
        -Times 1 `
        -Exactly `
        -ParameterFilter { $ProcessId -eq 4242 }
      Should -Invoke Invoke-VmVerifyHostSupervisor -Times 1 -Exactly
    }

    It "records an unverified serving host as a warning instead of silently reusing it" {
      $script:bootstrapState.Registered = $true
      $script:bootstrapState.Pipe = $true
      Mock Get-VmVerifyServingHostBinary {
        [pscustomobject][ordered]@{
          status = "unverified"
          reason = "process path unavailable"
          processId = 0
          runningPath = ""
          runningSha256 = ""
          expectedPath = "C:\package\azookey_inference_host.exe"
          expectedSha256 = ("b" * 64)
        }
      }

      $result = Invoke-VmVerifyBootstrap `
        -PackageRoot $script:testPackageRoot `
        -HasCheckpoint `
        -WarningAction SilentlyContinue

      $result.overallStatus | Should -Be "warning"
      $result.hostBinary.status | Should -Be "unverified"
      @($result.checks | Where-Object { $_.id -eq "inferenceHost" }).status |
        Should -Be "manual_required"
      Should -Invoke Invoke-VmVerifyHostProcessTermination -Times 0 -Exactly
      Should -Invoke Invoke-VmVerifyHostSupervisor -Times 0 -Exactly
    }

    It "fails with one public inference host check when the pipe stops before final verification" {
      $script:bootstrapState.Registered = $true
      $script:bootstrapState.Pipe = $true
      $script:pipeProbeCount = 0
      Mock Test-VmVerifyPipe {
        $script:pipeProbeCount++
        return $script:pipeProbeCount -eq 1
      }
      Mock Get-VmVerifyServingHostBinary {
        [pscustomobject][ordered]@{
          status = "matched"
          reason = "internal preflight status"
          processId = 4242
          runningPath = "C:\package\azookey_inference_host.exe"
          runningSha256 = ("b" * 64)
          expectedPath = "C:\package\azookey_inference_host.exe"
          expectedSha256 = ("b" * 64)
        }
      }

      $result = Invoke-VmVerifyBootstrap `
        -PackageRoot $script:testPackageRoot `
        -HasCheckpoint

      $result.overallStatus | Should -Be "fail"
      $result.hostBinary.status | Should -Be "unverified"
      $result.hostBinary.status | Should -BeIn @(
        "reused", "restarted", "started", "unverified", "not_applicable")
      $inferenceHostChecks = @($result.checks | Where-Object {
          $_.id -eq "inferenceHost"
        })
      $inferenceHostChecks.Count | Should -Be 1
      $inferenceHostChecks[0].status | Should -Be "fail"
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
