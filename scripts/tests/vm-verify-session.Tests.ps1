Describe "VM verification session automation" {
  BeforeAll {
    $repoRoot = (Resolve-Path (Join-Path (Join-Path $PSScriptRoot "..") "..")).Path
    $sessionScript = Join-Path $repoRoot "scripts\vm-verify-session.ps1"
    . $sessionScript

    function Initialize-TestPackage {
      param(
        [Parameter(Mandatory = $true)]
        [string]$Root,
        [string]$Commit = "0123456789abcdef0123456789abcdef01234567",
        [string]$Preset = "windows-release",
        [switch]$SkipManifest
      )

      $packageDir = Join-Path $Root "build\vm-verify-packages"
      New-Item -ItemType Directory -Path $packageDir -Force | Out-Null

      $baseName = "azookey-verify-$($Commit.Substring(0, 12))-$Preset"
      $zipPath = Join-Path $packageDir "$baseName.zip"
      "zip" | Set-Content -LiteralPath $zipPath -Encoding UTF8

      if (-not $SkipManifest) {
        @{
          schemaVersion = 1
          commit = $Commit
          preset = $Preset
          buildType = "Release"
          generatedAtUtc = "2026-01-01T00:00:00.0000000+00:00"
          files = @()
        } | ConvertTo-Json -Depth 5 |
          Set-Content -LiteralPath (Join-Path $packageDir "$baseName.manifest.json") -Encoding UTF8
      }

      return [pscustomobject]@{
        ZipPath = $zipPath
        BaseName = $baseName
      }
    }
  }

  Context "checkpoint naming" {
    It "derives the checkpoint name from commit and preset" {
      $manifest = [pscustomobject]@{
        commit = "0123456789abcdef0123456789abcdef01234567"
        preset = "windows-release"
      }

      Get-VmVerifySessionCheckpointName -Manifest $manifest |
        Should -Be "pre-azookey-windows-release-0123456789ab"
    }

    It "returns the same name for repeated calls on the same package" {
      $manifest = [pscustomobject]@{
        commit = "0123456789abcdef0123456789abcdef01234567"
        preset = "windows-release"
      }

      $first = Get-VmVerifySessionCheckpointName -Manifest $manifest
      $second = Get-VmVerifySessionCheckpointName -Manifest $manifest
      $second | Should -Be $first
    }

    It "distinguishes different commits and presets" {
      $a = Get-VmVerifySessionCheckpointName -Manifest ([pscustomobject]@{
          commit = "0123456789abcdef0123456789abcdef01234567"
          preset = "windows-release"
        })
      $b = Get-VmVerifySessionCheckpointName -Manifest ([pscustomobject]@{
          commit = "fedcba9876543210fedcba9876543210fedcba98"
          preset = "windows-release"
        })
      $c = Get-VmVerifySessionCheckpointName -Manifest ([pscustomobject]@{
          commit = "0123456789abcdef0123456789abcdef01234567"
          preset = "windows-llama-debug"
        })

      $a | Should -Not -Be $b
      $a | Should -Not -Be $c
    }
  }

  Context "package resolution" {
    It "selects the newest package when no path is given" {
      $root = Join-Path $TestDrive "resolve-newest"
      $older = Initialize-TestPackage -Root $root -Commit "1111111111111111111111111111111111111111"
      $newer = Initialize-TestPackage -Root $root -Commit "2222222222222222222222222222222222222222"
      (Get-Item -LiteralPath $older.ZipPath).LastWriteTimeUtc = [DateTime]::UtcNow.AddHours(-2)
      (Get-Item -LiteralPath $newer.ZipPath).LastWriteTimeUtc = [DateTime]::UtcNow

      $resolved = Resolve-VmVerifySessionPackage -RepositoryRoot $root
      $resolved.ZipPath | Should -Be $newer.ZipPath
      $resolved.ManifestPath | Should -Exist
    }

    It "fails when the manifest is missing next to the archive" {
      $root = Join-Path $TestDrive "resolve-no-manifest"
      Initialize-TestPackage -Root $root -SkipManifest | Out-Null

      { Resolve-VmVerifySessionPackage -RepositoryRoot $root } |
        Should -Throw -ExpectedMessage "*manifest was not found*"
    }

    It "fails when no package directory exists" {
      $root = Join-Path $TestDrive "resolve-empty"
      New-Item -ItemType Directory -Path $root -Force | Out-Null

      { Resolve-VmVerifySessionPackage -RepositoryRoot $root } |
        Should -Throw -ExpectedMessage "*make-vm-verify-package.ps1*"
    }

    It "rejects a manifest without a full commit hash" {
      $root = Join-Path $TestDrive "resolve-bad-commit"
      $package = Initialize-TestPackage -Root $root
      $manifestPath = Join-Path (Split-Path -Parent $package.ZipPath) "$($package.BaseName).manifest.json"
      @{ schemaVersion = 1; commit = "abc"; preset = "windows-release" } |
        ConvertTo-Json | Set-Content -LiteralPath $manifestPath -Encoding UTF8

      { Get-VmVerifySessionManifest -ManifestPath $manifestPath } |
        Should -Throw -ExpectedMessage "*full commit hash*"
    }
  }

  Context "Guest Service Interface lookup" {
    BeforeEach {
      # Hyper-V モジュールが無いホスト（CI ランナー等）でも Mock を張れるよう
      # スタブを定義してから差し替える。実機がある環境でも同じ経路を通す。
      function Get-VMIntegrationService {
        # CmdletBinding が無いと -ErrorAction 等の共通パラメータを受け取れず、
        # 実装側の呼び出しがパラメータ束縛で落ちる。
        [CmdletBinding()]
        param([string]$VMName)
        throw "Get-VMIntegrationService must be mocked in tests (VMName=$VMName)."
      }
      Mock Get-VMIntegrationService {
        @(
          [pscustomobject]@{
            Name = "ハートビート"
            Enabled = $true
            Id = "Microsoft:6482F90F-339A-4998-B4F8-9AAC9CF7A580\84EAAE65-2F2E-45F5-9BB5-0E857DC8EB47"
          }
          [pscustomobject]@{
            Name = "ゲスト サービス インターフェイス"
            Enabled = $true
            Id = "Microsoft:6482F90F-339A-4998-B4F8-9AAC9CF7A580\6C09BB55-D683-4DA0-8931-C9BF705F6480"
          }
          [pscustomobject]@{
            Name = "VSS"
            Enabled = $true
            Id = "Microsoft:6482F90F-339A-4998-B4F8-9AAC9CF7A580\5CED1297-4598-4915-A5FC-AD21BB4D02A4"
          }
        )
      }
    }

    It "finds the service on a localized host, where the display name is not English" {
      $service = Get-VmVerifySessionGuestServiceInterface -VMName "azooKey-VM"

      $service | Should -Not -BeNullOrEmpty
      $service.Name | Should -Be "ゲスト サービス インターフェイス"
      $service.Enabled | Should -BeTrue
    }

    It "accepts a localized host in -Prepare instead of stopping the transfer" {
      $root = Join-Path $TestDrive "guest-service-localized"
      Initialize-TestPackage -Root $root | Out-Null
      Mock Get-VmVerifySessionVM { [pscustomobject]@{ Name = "azooKey-VM"; State = "Running" } }
      Mock Get-VmVerifySessionCheckpoint { $null }
      Mock Invoke-VmVerifySessionCheckpoint {}
      Mock Invoke-VmVerifySessionFileCopy {}
      Mock Write-Host {}

      { Invoke-VmVerifySessionPrepare -RepositoryRoot $root -VMName "azooKey-VM" `
          -GuestDestination "C:\azookey-verify" } | Should -Not -Throw

      Should -Invoke Invoke-VmVerifySessionFileCopy -Times 1 -Exactly
    }

    It "reports not-enabled when the component is present but disabled" {
      Mock Get-VMIntegrationService {
        @([pscustomobject]@{
            Name = "ゲスト サービス インターフェイス"
            Enabled = $false
            Id = "Microsoft:6482F90F-339A-4998-B4F8-9AAC9CF7A580\6C09BB55-D683-4DA0-8931-C9BF705F6480"
          })
      }

      { Assert-VmVerifySessionGuestService -VMName "azooKey-VM" } |
        Should -Throw -ExpectedMessage "*NOT transferred*"
    }

    It "reports not-enabled when the component is absent entirely" {
      Mock Get-VMIntegrationService {
        @([pscustomobject]@{
            Name = "ハートビート"
            Enabled = $true
            Id = "Microsoft:6482F90F-339A-4998-B4F8-9AAC9CF7A580\84EAAE65-2F2E-45F5-9BB5-0E857DC8EB47"
          })
      }

      { Assert-VmVerifySessionGuestService -VMName "azooKey-VM" } |
        Should -Throw -ExpectedMessage "*NOT transferred*"
    }

    It "does not tell the operator to enable the service by its English display name" {
      Mock Get-VMIntegrationService { @() }

      $message = $null
      try {
        Assert-VmVerifySessionGuestService -VMName "azooKey-VM"
      } catch {
        $message = $_.Exception.Message
      }

      $message | Should -Not -BeNullOrEmpty
      $message | Should -Not -Match "-Name 'Guest Service Interface'"
      $message | Should -Match "6C09BB55-D683-4DA0-8931-C9BF705F6480"
    }
  }

  Context "-Prepare" {
    BeforeEach {
      Mock Get-VmVerifySessionVM { [pscustomobject]@{ Name = "azooKey-VM"; State = "Running" } }
      Mock Get-VmVerifySessionGuestServiceInterface { [pscustomobject]@{ Enabled = $true } }
      Mock Get-VmVerifySessionCheckpoint { $null }
      Mock Invoke-VmVerifySessionCheckpoint {}
      Mock Invoke-VmVerifySessionFileCopy {}
      Mock Write-Host {}
    }

    It "takes a checkpoint and copies the package into the guest" {
      $root = Join-Path $TestDrive "prepare-happy"
      $package = Initialize-TestPackage -Root $root

      $result = Invoke-VmVerifySessionPrepare `
        -RepositoryRoot $root `
        -VMName "azooKey-VM" `
        -GuestDestination "C:\azookey-verify"

      $result.CheckpointName | Should -Be "pre-azookey-windows-release-0123456789ab"
      $result.ZipPath | Should -Be $package.ZipPath
      $result.GuestPath | Should -Be "C:\azookey-verify\$($package.BaseName).zip"

      Should -Invoke Invoke-VmVerifySessionCheckpoint -Times 1 -Exactly
      Should -Invoke Invoke-VmVerifySessionFileCopy -Times 1 -Exactly
    }

    It "tells the operator that IME verification needs a basic session" {
      $root = Join-Path $TestDrive "prepare-basic-session"
      Initialize-TestPackage -Root $root | Out-Null

      Invoke-VmVerifySessionPrepare `
        -RepositoryRoot $root `
        -VMName "azooKey-VM" `
        -GuestDestination "C:\azookey-verify" | Out-Null

      Should -Invoke Write-Host -ParameterFilter { $Object -match "BASIC session" }
    }

    It "fails and copies nothing when the VM does not exist" {
      $root = Join-Path $TestDrive "prepare-no-vm"
      Initialize-TestPackage -Root $root | Out-Null
      Mock Get-VmVerifySessionVM { throw "The operation failed because of a virtual machine not found." }

      { Invoke-VmVerifySessionPrepare -RepositoryRoot $root -VMName "missing-vm" `
          -GuestDestination "C:\azookey-verify" } |
        Should -Throw -ExpectedMessage "*was not found on this host*"

      Should -Invoke Invoke-VmVerifySessionCheckpoint -Times 0 -Exactly
      Should -Invoke Invoke-VmVerifySessionFileCopy -Times 0 -Exactly
    }

    It "fails and copies nothing when the VM is not running" {
      $root = Join-Path $TestDrive "prepare-vm-off"
      Initialize-TestPackage -Root $root | Out-Null
      Mock Get-VmVerifySessionVM { [pscustomobject]@{ Name = "azooKey-VM"; State = "Off" } }

      { Invoke-VmVerifySessionPrepare -RepositoryRoot $root -VMName "azooKey-VM" `
          -GuestDestination "C:\azookey-verify" } |
        Should -Throw -ExpectedMessage "*must be Running*"

      Should -Invoke Invoke-VmVerifySessionCheckpoint -Times 0 -Exactly
      Should -Invoke Invoke-VmVerifySessionFileCopy -Times 0 -Exactly
    }

    It "refuses to continue silently when Guest Service Interface is disabled" {
      $root = Join-Path $TestDrive "prepare-no-guest-services"
      Initialize-TestPackage -Root $root | Out-Null
      Mock Get-VmVerifySessionGuestServiceInterface { [pscustomobject]@{ Enabled = $false } }

      { Invoke-VmVerifySessionPrepare -RepositoryRoot $root -VMName "azooKey-VM" `
          -GuestDestination "C:\azookey-verify" } |
        Should -Throw -ExpectedMessage "*NOT transferred*"

      Should -Invoke Invoke-VmVerifySessionFileCopy -Times 0 -Exactly
    }

    It "names the fallback transfer routes when Guest Service Interface is unavailable" {
      $root = Join-Path $TestDrive "prepare-guest-services-missing"
      Initialize-TestPackage -Root $root | Out-Null
      Mock Get-VmVerifySessionGuestServiceInterface { throw "Hyper-V module is unavailable." }

      { Invoke-VmVerifySessionPrepare -RepositoryRoot $root -VMName "azooKey-VM" `
          -GuestDestination "C:\azookey-verify" } |
        Should -Throw -ExpectedMessage "*share a host folder*"
    }

    It "does not overwrite an existing checkpoint of the same name" {
      $root = Join-Path $TestDrive "prepare-existing-checkpoint"
      Initialize-TestPackage -Root $root | Out-Null
      Mock Get-VmVerifySessionCheckpoint {
        [pscustomobject]@{ Name = "pre-azookey-windows-release-0123456789ab" }
      }

      { Invoke-VmVerifySessionPrepare -RepositoryRoot $root -VMName "azooKey-VM" `
          -GuestDestination "C:\azookey-verify" } |
        Should -Throw -ExpectedMessage "*already exists*"

      Should -Invoke Invoke-VmVerifySessionCheckpoint -Times 0 -Exactly
      Should -Invoke Invoke-VmVerifySessionFileCopy -Times 0 -Exactly
    }
  }

  Context "-Restore" {
    BeforeEach {
      Mock Get-VmVerifySessionVM { [pscustomobject]@{ Name = "azooKey-VM"; State = "Off" } }
      Mock Invoke-VmVerifySessionCheckpointRestore {}
      Mock Write-Host {}
    }

    It "restores the checkpoint derived from the package" {
      $root = Join-Path $TestDrive "restore-happy"
      Initialize-TestPackage -Root $root | Out-Null
      Mock Get-VmVerifySessionCheckpoint { [pscustomobject]@{ Name = "checkpoint" } }

      $result = Invoke-VmVerifySessionRestore -RepositoryRoot $root -VMName "azooKey-VM"

      $result.CheckpointName | Should -Be "pre-azookey-windows-release-0123456789ab"
      Should -Invoke Invoke-VmVerifySessionCheckpointRestore -Times 1 -Exactly
    }

    It "honours an explicit checkpoint name without needing a package" {
      $root = Join-Path $TestDrive "restore-explicit"
      New-Item -ItemType Directory -Path $root -Force | Out-Null
      Mock Get-VmVerifySessionCheckpoint { [pscustomobject]@{ Name = "checkpoint" } }

      $result = Invoke-VmVerifySessionRestore -RepositoryRoot $root -VMName "azooKey-VM" `
        -CheckpointName "pre-azookey-manual"

      $result.CheckpointName | Should -Be "pre-azookey-manual"
    }

    It "fails when the checkpoint does not exist" {
      $root = Join-Path $TestDrive "restore-missing"
      Initialize-TestPackage -Root $root | Out-Null
      Mock Get-VmVerifySessionCheckpoint { $null }

      { Invoke-VmVerifySessionRestore -RepositoryRoot $root -VMName "azooKey-VM" } |
        Should -Throw -ExpectedMessage "*was not found on*"

      Should -Invoke Invoke-VmVerifySessionCheckpointRestore -Times 0 -Exactly
    }
  }

  Context "script entrypoint" {
    It "exits non-zero when neither -Prepare nor -Restore is given" {
      $output = & pwsh -NoProfile -File $sessionScript -VMName "azooKey-VM" 2>&1
      $LASTEXITCODE | Should -Not -Be 0
      ($output | Out-String) | Should -Match "exactly one of -Prepare or -Restore"
    }

    It "exits non-zero when both -Prepare and -Restore are given" {
      $output = & pwsh -NoProfile -File $sessionScript -Prepare -Restore -VMName "azooKey-VM" 2>&1
      $LASTEXITCODE | Should -Not -Be 0
      ($output | Out-String) | Should -Match "exactly one of -Prepare or -Restore"
    }

    It "exits non-zero when -VMName is missing" {
      $output = & pwsh -NoProfile -File $sessionScript -Restore 2>&1
      $LASTEXITCODE | Should -Not -Be 0
      ($output | Out-String) | Should -Match "-VMName is required"
    }

    It "exits non-zero when the target VM cannot be resolved" {
      # Hyper-V が無いホストでは Get-VM 自体が解決できない。いずれの場合も
      # 「VM を特定できない」ことを明示して非ゼロ終了する必要がある。
      $output = & pwsh -NoProfile -File $sessionScript -Restore `
        -VMName "azookey-nonexistent-vm" -CheckpointName "pre-azookey-manual" 2>&1
      $LASTEXITCODE | Should -Not -Be 0
      ($output | Out-String) | Should -Match "azookey-nonexistent-vm"
    }
  }
}
