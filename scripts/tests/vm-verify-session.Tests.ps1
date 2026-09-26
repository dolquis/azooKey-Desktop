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

  Context "-Run host orchestration" {
    BeforeAll {
      $secret = New-Object System.Security.SecureString
      foreach ($character in "s3cret-Guest-Pw".ToCharArray()) {
        $secret.AppendChar($character)
      }
      $testCredential = [System.Management.Automation.PSCredential]::new("VM\verifier", $secret)

      function Invoke-TestRun {
        param(
          [Parameter(Mandatory = $true)]
          [string]$Root,
          [switch]$WithoutCredential
        )

        $parameters = @{
          RepositoryRoot = $Root
          VMName = "azooKey-VM"
          GuestDestination = "C:\azookey-verify"
        }
        if (-not $WithoutCredential) {
          $parameters.Credential = $testCredential
        }
        return Invoke-VmVerifySessionRun @parameters
      }
    }

    BeforeEach {
      $script:guestUser = [pscustomobject]@{
        ConsoleUser = "VM\verifier"
        SessionUser = "VM\verifier"
        IsAdministrator = $true
        Locked = $false
      }
      $script:directBootstrap = '{"overallStatus":"pass","checks":[]}'
      $script:interactiveStatus = @{
        completed = $true
        error = ""
        targets = @(
          @{ target = "edge"; exitCode = 2; reportJson = $true }
          @{ target = "notepad"; exitCode = 0; reportJson = $true }
        )
      } | ConvertTo-Json -Depth 4

      Mock Get-VmVerifySessionVM { [pscustomobject]@{ Name = "azooKey-VM"; State = "Running" } }
      Mock Get-VmVerifySessionCheckpoint { [pscustomobject]@{ Name = "checkpoint" } }
      Mock Get-VmVerifySessionCredential { throw "Get-VmVerifySessionCredential must not prompt here." }
      Mock Open-VmVerifySessionGuestSession { [pscustomobject]@{ Id = 7 } }
      Mock Close-VmVerifySessionGuestSession {}
      Mock Copy-VmVerifySessionGuestArtifact {}
      Mock Write-Host {}
      Mock Invoke-VmVerifySessionGuestStep {
        switch ($Name) {
          "Initialize-VmVerifyGuestPackage" { return @("edge", "notepad") }
          "Get-VmVerifyGuestInteractiveUser" { return $script:guestUser }
          "Invoke-VmVerifyGuestBootstrap" { return 0 }
          "Read-VmVerifyGuestText" {
            if ($ArgumentList[0] -like "*bootstrap-direct.json") {
              return $script:directBootstrap
            }
            return $script:interactiveStatus
          }
          "Invoke-VmVerifyGuestSessionZeroHostShutdown" { return "stopped" }
          "Invoke-VmVerifyGuestInteractiveTask" { return 0 }
          default { return $null }
        }
      }
    }

    It "runs bootstrap over PowerShell Direct, then compat through the interactive task, and collects artifacts" {
      $root = Join-Path $TestDrive "run-happy"
      $package = Initialize-TestPackage -Root $root

      $result = Invoke-TestRun -Root $root

      $result.CheckpointName | Should -Be "pre-azookey-windows-release-0123456789ab"
      $result.ResultsPath | Should -BeLike (Join-Path $root "build\vm-verify-results\$($package.BaseName)-*")
      @($result.Targets | ForEach-Object { "$($_.Target)=$($_.Outcome)" }) |
        Should -Be @("edge=failing-skip", "notepad=pass")

      Should -Invoke Invoke-VmVerifySessionGuestStep -Times 1 -Exactly -ParameterFilter {
        $Name -eq "Invoke-VmVerifyGuestBootstrap" -and
        $ArgumentList[0] -eq "C:\azookey-verify\$($package.BaseName)" -and
        $ArgumentList[2] -eq $true
      }
      Should -Invoke Invoke-VmVerifySessionGuestStep -Times 1 -Exactly -ParameterFilter {
        $Name -eq "Invoke-VmVerifyGuestSessionZeroHostShutdown"
      }
      Should -Invoke Invoke-VmVerifySessionGuestStep -Times 1 -Exactly -ParameterFilter {
        $Name -eq "Invoke-VmVerifyGuestInteractiveTask" -and
        $ArgumentList[1] -eq "VM\verifier" -and
        $ArgumentList[3] -eq 45 * 60
      }
      Should -Invoke Copy-VmVerifySessionGuestArtifact -Times 1 -Exactly -ParameterFilter {
        $GuestPath -like "C:\azookey-verify\runs\$($package.BaseName)-*" -and
        $Destination -eq $result.ResultsPath
      }
      Should -Invoke Close-VmVerifySessionGuestSession -Times 1 -Exactly
      Should -Invoke Get-VmVerifySessionCredential -Times 0 -Exactly
    }

    It "prompts for guest credentials only when -Credential is omitted" {
      $root = Join-Path $TestDrive "run-prompt"
      Initialize-TestPackage -Root $root | Out-Null
      Mock Get-VmVerifySessionCredential { $testCredential }

      Invoke-TestRun -Root $root -WithoutCredential | Out-Null

      Should -Invoke Get-VmVerifySessionCredential -Times 1 -Exactly
    }

    It "never writes the guest password to output or to guest commands" {
      $root = Join-Path $TestDrive "run-secret"
      Initialize-TestPackage -Root $root | Out-Null

      Invoke-TestRun -Root $root | Out-Null

      Should -Invoke Write-Host -Times 0 -Exactly -ParameterFilter { "$Object" -match "s3cret" }
      Should -Invoke Invoke-VmVerifySessionGuestStep -Times 0 -Exactly -ParameterFilter {
        ($ArgumentList | Out-String) -match "s3cret"
      }
    }

    It "passes the compat case selection to the interactive runner" {
      $root = Join-Path $TestDrive "run-case-selection"
      Initialize-TestPackage -Root $root | Out-Null

      Invoke-VmVerifySessionRun -RepositoryRoot $root -VMName "azooKey-VM" `
        -GuestDestination "C:\azookey-verify" -Credential $testCredential `
        -CompatCases @("C-001,C-004", "C-010") -CompatSkip "C-010" | Out-Null

      Should -Invoke Invoke-VmVerifySessionGuestStep -Times 1 -Exactly -ParameterFilter {
        $Name -eq "Invoke-VmVerifyGuestInteractiveTask" -and
        [System.Text.Encoding]::Unicode.GetString([Convert]::FromBase64String(
            ($ArgumentList[2] -split " ")[-1])) -like
          "*Invoke-VmVerifyGuestCompatRun*-Cases 'C-001,C-004,C-010' -Skip 'C-010'"
      }
    }

    It "omits the case selection from the runner command when none is given" {
      $root = Join-Path $TestDrive "run-no-case-selection"
      Initialize-TestPackage -Root $root | Out-Null

      Invoke-TestRun -Root $root | Out-Null

      Should -Invoke Invoke-VmVerifySessionGuestStep -Times 1 -Exactly -ParameterFilter {
        $Name -eq "Invoke-VmVerifyGuestInteractiveTask" -and
        [System.Text.Encoding]::Unicode.GetString([Convert]::FromBase64String(
            ($ArgumentList[2] -split " ")[-1])) -notmatch "-Cases|-Skip"
      }
    }

    It "rejects <Case> before opening a guest session" -TestCases @(
      @{ Case = "a malformed case ID"; CaseList = @("C-1"); SkipList = @(); Message = "*-CompatCases*'C-1'*" }
      @{ Case = "an injected quote"; CaseList = @(); SkipList = @("C-010'; Stop-Computer"); Message = "*-CompatSkip*" }
      @{ Case = "a duplicate case ID"; CaseList = @("C-001,C-001"); SkipList = @(); Message = "*C-001 more than once*" }
    ) {
      $root = Join-Path $TestDrive ("run-bad-cases-" + [guid]::NewGuid().ToString("N"))
      Initialize-TestPackage -Root $root | Out-Null

      { Invoke-VmVerifySessionRun -RepositoryRoot $root -VMName "azooKey-VM" `
          -GuestDestination "C:\azookey-verify" -Credential $testCredential `
          -CompatCases $CaseList -CompatSkip $SkipList } | Should -Throw -ExpectedMessage $Message

      Should -Invoke Open-VmVerifySessionGuestSession -Times 0 -Exactly
    }

    It "explains how to pass credentials when no prompt is available" {
      $root = Join-Path $TestDrive "run-no-prompt"
      Initialize-TestPackage -Root $root | Out-Null

      { Invoke-TestRun -Root $root -WithoutCredential } |
        Should -Throw -ExpectedMessage "*Pass -Credential*"

      Should -Invoke Open-VmVerifySessionGuestSession -Times 0 -Exactly
    }

    It "refuses to run without the pre-verification checkpoint" {
      $root = Join-Path $TestDrive "run-no-checkpoint"
      Initialize-TestPackage -Root $root | Out-Null
      Mock Get-VmVerifySessionCheckpoint { $null }

      { Invoke-TestRun -Root $root } | Should -Throw -ExpectedMessage "*Run -Prepare first*"

      Should -Invoke Open-VmVerifySessionGuestSession -Times 0 -Exactly
    }

    It "fails with the reason when the PowerShell Direct session cannot be opened" {
      $root = Join-Path $TestDrive "run-no-session"
      Initialize-TestPackage -Root $root | Out-Null
      Mock Open-VmVerifySessionGuestSession { throw "The credential is invalid." }

      { Invoke-TestRun -Root $root } |
        Should -Throw -ExpectedMessage "*PowerShell Direct session*The credential is invalid.*"
    }

    It "stops before bootstrap when <Case>" -TestCases @(
      @{
        Case = "nobody is signed in to the console"
        User = @{ ConsoleUser = ""; SessionUser = "VM\verifier"; IsAdministrator = $true; Locked = $false }
        Expected = "*BASIC session*"
      }
      @{
        Case = "the console user differs from the PowerShell Direct account"
        User = @{ ConsoleUser = "VM\other"; SessionUser = "VM\verifier"; IsAdministrator = $true; Locked = $false }
        Expected = "*differs from the PowerShell Direct account*"
      }
      @{
        Case = "the console is locked"
        User = @{ ConsoleUser = "VM\verifier"; SessionUser = "VM\verifier"; IsAdministrator = $true; Locked = $true }
        Expected = "*locked*"
      }
      @{
        Case = "the account is not an administrator"
        User = @{ ConsoleUser = "VM\verifier"; SessionUser = "VM\verifier"; IsAdministrator = $false; Locked = $false }
        Expected = "*not a local administrator*"
      }
    ) {
      $root = Join-Path $TestDrive ("run-user-" + [guid]::NewGuid().ToString("N"))
      Initialize-TestPackage -Root $root | Out-Null
      $script:guestUser = [pscustomobject]$User

      { Invoke-TestRun -Root $root } | Should -Throw -ExpectedMessage $Expected

      Should -Invoke Invoke-VmVerifySessionGuestStep -Times 0 -Exactly -ParameterFilter {
        $Name -eq "Invoke-VmVerifyGuestBootstrap"
      }
      Should -Invoke Close-VmVerifySessionGuestSession -Times 1 -Exactly
    }

    It "does not start compat when bootstrap fails over PowerShell Direct, but still collects artifacts" {
      $root = Join-Path $TestDrive "run-bootstrap-fail"
      Initialize-TestPackage -Root $root | Out-Null
      $script:directBootstrap = @{
        overallStatus = "fail"
        checks = @(@{ id = "tipRegistration"; status = "fail"; message = "regsvr32 failed" })
      } | ConvertTo-Json -Depth 4

      { Invoke-TestRun -Root $root } |
        Should -Throw -ExpectedMessage "*overallStatus=fail*tipRegistration: regsvr32 failed*"

      Should -Invoke Invoke-VmVerifySessionGuestStep -Times 0 -Exactly -ParameterFilter {
        $Name -eq "Invoke-VmVerifyGuestInteractiveTask"
      }
      Should -Invoke Copy-VmVerifySessionGuestArtifact -Times 1 -Exactly
    }

    It "fails when bootstrap does not return JSON" {
      $root = Join-Path $TestDrive "run-bootstrap-garbage"
      Initialize-TestPackage -Root $root | Out-Null
      $script:directBootstrap = "WARNING: not json"

      { Invoke-TestRun -Root $root } | Should -Throw -ExpectedMessage "*not valid JSON*"
    }

    It "fails with the guest-side reason when the interactive run did not complete" {
      $root = Join-Path $TestDrive "run-incomplete"
      Initialize-TestPackage -Root $root | Out-Null
      $script:interactiveStatus = @{
        completed = $false
        error = "The inference host runs in session 0"
        targets = @()
      } | ConvertTo-Json -Depth 4

      { Invoke-TestRun -Root $root } |
        Should -Throw -ExpectedMessage "*did not complete*runs in session 0*"
    }

    It "exits non-zero when a compat target <Case>" -TestCases @(
      @{ Case = "fails"; ExitCode = 1; ReportJson = $true; Expected = "*'notepad' ended with fail (exit 1)*" }
      @{ Case = "crashes"; ExitCode = -1073741819; ReportJson = $false; Expected = "*'notepad' ended with error*" }
      @{ Case = "leaves no report.json"; ExitCode = 0; ReportJson = $false; Expected = "*'notepad' ended with error*" }
    ) {
      $root = Join-Path $TestDrive ("run-compat-" + [guid]::NewGuid().ToString("N"))
      Initialize-TestPackage -Root $root | Out-Null
      $script:interactiveStatus = @{
        completed = $true
        error = ""
        targets = @(@{ target = "notepad"; exitCode = $ExitCode; reportJson = $ReportJson })
      } | ConvertTo-Json -Depth 4

      { Invoke-TestRun -Root $root } | Should -Throw -ExpectedMessage $Expected
    }

    It "keeps the original reason when the guest failed before creating the run directory" {
      $root = Join-Path $TestDrive "run-no-run-root"
      Initialize-TestPackage -Root $root | Out-Null
      Mock Invoke-VmVerifySessionGuestStep -ParameterFilter { $Name -eq "Initialize-VmVerifyGuestPackage" } {
        throw "The package archive is not in the guest."
      }
      Mock Invoke-VmVerifySessionGuestStep -ParameterFilter { $Name -eq "Copy-VmVerifyGuestLog" } {
        "no-run-root"
      }

      $message = $null
      try {
        Invoke-TestRun -Root $root | Out-Null
      } catch {
        $message = $_.Exception.Message
      }

      $message | Should -Match "The package archive is not in the guest."
      $message | Should -Not -Match "could not be collected"
      Should -Invoke Copy-VmVerifySessionGuestArtifact -Times 0 -Exactly
    }

    It "reports a collection failure instead of claiming success" {
      $root = Join-Path $TestDrive "run-collect-fail"
      Initialize-TestPackage -Root $root | Out-Null
      Mock Copy-VmVerifySessionGuestArtifact { throw "Copy-Item failed." }

      { Invoke-TestRun -Root $root } |
        Should -Throw -ExpectedMessage "*could not be collected*Copy-Item failed.*"
    }

    It "still collects the run directory when copying the azooKey logs fails" {
      $root = Join-Path $TestDrive "run-log-copy-fail"
      Initialize-TestPackage -Root $root | Out-Null
      Mock Invoke-VmVerifySessionGuestStep -ParameterFilter { $Name -eq "Copy-VmVerifyGuestLog" } {
        throw "The process cannot access the file because it is being used by another process."
      }
      Mock Write-Warning {}

      $result = Invoke-TestRun -Root $root

      $result.ResultsPath | Should -Not -BeNullOrEmpty
      Should -Invoke Copy-VmVerifySessionGuestArtifact -Times 1 -Exactly
      Should -Invoke Write-Warning -Times 1 -Exactly -ParameterFilter { $Message -match "being used" }
    }
  }

  Context "-Run guest package preparation" {
    BeforeEach {
      $guestRoot = Join-Path $TestDrive ("guest-" + [guid]::NewGuid().ToString("N"))
      New-Item -ItemType Directory -Path $guestRoot -Force | Out-Null

      function Initialize-GuestArchive {
        param(
          [Parameter(Mandatory = $true)]
          [string]$Root,
          [switch]$WithoutCompat
        )

        $staging = Join-Path $Root "staging"
        New-Item -ItemType Directory -Path (Join-Path $staging "targets") -Force | Out-Null
        "{}" | Set-Content -LiteralPath (Join-Path $staging "manifest.json")
        "# bootstrap" | Set-Content -LiteralPath (Join-Path $staging "verify-bootstrap.ps1")
        if (-not $WithoutCompat) {
          "exe" | Set-Content -LiteralPath (Join-Path $staging "compat_test.exe")
          "{}" | Set-Content -LiteralPath (Join-Path $staging "targets\notepad.json")
          "{}" | Set-Content -LiteralPath (Join-Path $staging "targets\edge.json")
        }
        $zip = Join-Path $Root "azookey-verify-test.zip"
        Compress-Archive -Path (Join-Path $staging "*") -DestinationPath $zip -Force
        return $zip
      }
    }

    It "re-expands when a rebuilt archive of the same name replaces the old one" {
      $packageRoot = Join-Path $guestRoot "azookey-verify-test"
      $zip = Initialize-GuestArchive -Root $guestRoot -WithoutCompat
      { Initialize-VmVerifyGuestPackage -ZipPath $zip -PackageRoot $packageRoot `
          -RunRoot (Join-Path $guestRoot "runs\run-1") } | Should -Throw
      Remove-Item -LiteralPath (Join-Path $guestRoot "staging") -Recurse -Force

      $zip = Initialize-GuestArchive -Root $guestRoot
      $targets = Initialize-VmVerifyGuestPackage -ZipPath $zip -PackageRoot $packageRoot `
        -RunRoot (Join-Path $guestRoot "runs\run-2")

      $targets | Should -Be @("edge", "notepad")
      Join-Path $packageRoot "compat_test.exe" | Should -Exist
    }

    It "expands the archive to a stable path and returns the compat targets" {
      $zip = Initialize-GuestArchive -Root $guestRoot
      $packageRoot = Join-Path $guestRoot "azookey-verify-test"
      $runRoot = Join-Path $guestRoot "runs\run-1"

      $targets = Initialize-VmVerifyGuestPackage -ZipPath $zip -PackageRoot $packageRoot -RunRoot $runRoot

      $targets | Should -Be @("edge", "notepad")
      Join-Path $packageRoot "verify-bootstrap.ps1" | Should -Exist
      $runRoot | Should -Exist
    }

    It "fails when -Prepare has not placed the archive" {
      { Initialize-VmVerifyGuestPackage -ZipPath (Join-Path $guestRoot "missing.zip") `
          -PackageRoot (Join-Path $guestRoot "pkg") -RunRoot (Join-Path $guestRoot "run") } |
        Should -Throw -ExpectedMessage "*Run -Prepare first*"
    }

    It "fails when the package was built without compat_test.exe" {
      $zip = Initialize-GuestArchive -Root $guestRoot -WithoutCompat

      { Initialize-VmVerifyGuestPackage -ZipPath $zip `
          -PackageRoot (Join-Path $guestRoot "pkg") -RunRoot (Join-Path $guestRoot "run") } |
        Should -Throw -ExpectedMessage "*compat_test.exe, targets\*.json*-IncludeCompat*"
    }
  }

  Context "-Run guest bootstrap invocation" {
    It "keeps warnings out of the JSON output and returns the bootstrap exit code" -Skip:(
      -not (Get-Command powershell.exe -ErrorAction SilentlyContinue)) {
      $packageRoot = Join-Path $TestDrive "bootstrap dir's"
      New-Item -ItemType Directory -Path $packageRoot -Force | Out-Null
      @'
param([switch]$Json, [switch]$CheckpointConfirmed)
Write-Warning "noise that must not reach stdout"
@{ overallStatus = "fail"; json = [bool]$Json; confirmed = [bool]$CheckpointConfirmed } | ConvertTo-Json
exit 1
'@ | Set-Content -LiteralPath (Join-Path $packageRoot "verify-bootstrap.ps1")
      $outputPath = Join-Path $packageRoot "bootstrap-direct.json"

      $exitCode = Invoke-VmVerifyGuestBootstrap -PackageRoot $packageRoot `
        -OutputPath $outputPath -CheckpointConfirmed $true

      $exitCode | Should -Be 1
      $result = Get-Content -Raw -LiteralPath $outputPath | ConvertFrom-Json
      $result.overallStatus | Should -Be "fail"
      $result.json | Should -BeTrue
      $result.confirmed | Should -BeTrue
      Get-Content -Raw -LiteralPath (Join-Path $packageRoot "bootstrap-direct.warnings.log") |
        Should -Match "noise that must not reach stdout"
    }

    It "returns without waiting for the long-lived supervisor that bootstrap leaves running" -Skip:(
      -not (Get-Command powershell.exe -ErrorAction SilentlyContinue)) {
      $packageRoot = Join-Path $TestDrive "bootstrap-supervisor"
      New-Item -ItemType Directory -Path $packageRoot -Force | Out-Null
      @'
param([switch]$Json, [switch]$CheckpointConfirmed)
$child = Start-Process -FilePath "powershell.exe" -ArgumentList "-NoProfile -Command Start-Sleep -Seconds 120" `
  -WindowStyle Hidden -PassThru
Set-Content -LiteralPath (Join-Path $PSScriptRoot "child.pid") -Value $child.Id
@{ overallStatus = "pass" } | ConvertTo-Json
'@ | Set-Content -LiteralPath (Join-Path $packageRoot "verify-bootstrap.ps1")
      $outputPath = Join-Path $packageRoot "bootstrap-direct.json"

      try {
        $elapsed = Measure-Command {
          $script:exitCode = Invoke-VmVerifyGuestBootstrap -PackageRoot $packageRoot `
            -OutputPath $outputPath -TimeoutSeconds 60
        }

        $script:exitCode | Should -Be 0
        $elapsed.TotalSeconds | Should -BeLessThan 60
        (Get-Content -Raw -LiteralPath $outputPath | ConvertFrom-Json).overallStatus | Should -Be "pass"
      } finally {
        $pidPath = Join-Path $packageRoot "child.pid"
        if (Test-Path -LiteralPath $pidPath) {
          Stop-Process -Id ([int](Get-Content -LiteralPath $pidPath)) -Force -ErrorAction SilentlyContinue
        }
      }
    }

    It "records the error when bootstrap throws before producing JSON" -Skip:(
      -not (Get-Command powershell.exe -ErrorAction SilentlyContinue)) {
      $packageRoot = Join-Path $TestDrive "bootstrap-throws"
      New-Item -ItemType Directory -Path $packageRoot -Force | Out-Null
      'throw "VM verification package file is missing: x"' |
        Set-Content -LiteralPath (Join-Path $packageRoot "verify-bootstrap.ps1")
      $outputPath = Join-Path $packageRoot "bootstrap-direct.json"

      $exitCode = Invoke-VmVerifyGuestBootstrap -PackageRoot $packageRoot -OutputPath $outputPath

      $exitCode | Should -Be 1
      $outputPath | Should -Not -Exist
      Get-Content -Raw -LiteralPath (Join-Path $packageRoot "bootstrap-direct.error.log") |
        Should -Match "package file is missing"
    }
  }

  Context "-Run session 0 host shutdown" {
    BeforeEach {
      $packageRoot = Join-Path $TestDrive ("zero-" + [guid]::NewGuid().ToString("N"))
      New-Item -ItemType Directory -Path $packageRoot -Force | Out-Null
      # verify-bootstrap.ps1 の関数を差し替える偽物。挙動は state.json で切り替え、
      # 呼び出し順は calls.txt へ記録する。
      @'
$state = Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot "state.json") | ConvertFrom-Json
$callsPath = Join-Path $PSScriptRoot "calls.txt"
function Get-VmVerifyPipeName { "azookey-test" }
function Test-VmVerifyPipe { param($PipeName) [void]$PipeName; $state.pipeServing }
function Get-VmVerifyServingHostProcess { param($PipeName) [void]$PipeName; [pscustomobject]@{ Id = 4242; Path = "host.exe" } }
function Invoke-VmVerifyHostSupervisorShutdown { Add-Content -LiteralPath $callsPath -Value "shutdown" }
function Wait-VmVerifyHostSupervisorStopped { param($TimeoutSeconds) [void]$TimeoutSeconds; $state.supervisorStops }
function Invoke-VmVerifyHostProcessTermination { param($ProcessId) Add-Content -LiteralPath $callsPath -Value "kill:$ProcessId" }
function Wait-VmVerifyPipe { param($PipeName, $TimeoutSeconds, $ExpectedPresent) $true }
'@ | Set-Content -LiteralPath (Join-Path $packageRoot "verify-bootstrap.ps1")

      function Write-FakeHostState {
        param(
          [bool]$PipeServing = $true,
          [bool]$SupervisorStops = $true
        )

        @{ pipeServing = $PipeServing; supervisorStops = $SupervisorStops } | ConvertTo-Json |
          Set-Content -LiteralPath (Join-Path $packageRoot "state.json")
      }

      function Get-FakeHostCall {
        $callsPath = Join-Path $packageRoot "calls.txt"
        if (-not (Test-Path -LiteralPath $callsPath)) {
          return @()
        }
        return @(Get-Content -LiteralPath $callsPath)
      }

      Write-FakeHostState
      Mock Set-ExecutionPolicy {}
      Mock Get-Process { [pscustomobject]@{ Id = 4242; SessionId = 0 } }
    }

    It "stops the supervisor and the host that PowerShell Direct started in session 0" {
      Invoke-VmVerifyGuestSessionZeroHostShutdown -PackageRoot $packageRoot | Should -Be "stopped"

      Get-FakeHostCall | Should -Be @("shutdown", "kill:4242")
    }

    It "leaves a host that already serves an interactive session alone" {
      Mock Get-Process { [pscustomobject]@{ Id = 4242; SessionId = 1 } }

      Invoke-VmVerifyGuestSessionZeroHostShutdown -PackageRoot $packageRoot |
        Should -Be "interactive-session-1"
      Get-FakeHostCall | Should -BeNullOrEmpty
    }

    It "still stops a supervisor that is waiting to restart the host when no pipe is serving" {
      Write-FakeHostState -PipeServing $false

      Invoke-VmVerifyGuestSessionZeroHostShutdown -PackageRoot $packageRoot | Should -Be "not-serving"
      Get-FakeHostCall | Should -Be @("shutdown")
    }

    It "fails when a supervisor without a serving pipe does not stop" {
      Write-FakeHostState -PipeServing $false -SupervisorStops $false

      { Invoke-VmVerifyGuestSessionZeroHostShutdown -PackageRoot $packageRoot } |
        Should -Throw -ExpectedMessage "*did not stop*"
    }

    It "fails instead of killing the host when the supervisor does not stop" {
      Write-FakeHostState -SupervisorStops $false

      { Invoke-VmVerifyGuestSessionZeroHostShutdown -PackageRoot $packageRoot } |
        Should -Throw -ExpectedMessage "*did not stop*"
      Get-FakeHostCall | Should -Be @("shutdown")
    }
  }

  # New-ScheduledTask* はローカルに CIM オブジェクトを作るだけなので実物を使い、
  # タスクの登録・起動・状態取得だけを差し替える。
  Context "-Run interactive scheduled task" -Skip:(-not (Get-Command New-ScheduledTaskAction -ErrorAction SilentlyContinue)) {
    BeforeEach {
      $script:taskStates = @("Running", "Ready")
      $script:taskStateIndex = 0
      $script:taskRunTime = [DateTime]"1999-11-30"
      Mock Register-ScheduledTask {}
      Mock Start-ScheduledTask {}
      Mock Stop-ScheduledTask {}
      Mock Unregister-ScheduledTask {}
      Mock Start-Sleep {}
      Mock Get-ScheduledTask {
        $index = [Math]::Min($script:taskStateIndex, $script:taskStates.Count - 1)
        $script:taskStateIndex++
        [pscustomobject]@{ State = $script:taskStates[$index] }
      }
      Mock Get-ScheduledTaskInfo {
        [pscustomobject]@{ LastRunTime = $script:taskRunTime; LastTaskResult = 0 }
      }
    }

    It "runs the task as the console user with a limited, password-less interactive principal" {
      $result = Invoke-VmVerifyGuestInteractiveTask -TaskName "azooKey-vm-verify-test" `
        -UserName "VM\verifier" -Argument "-EncodedCommand AAAA" -TimeoutSeconds 60

      $result | Should -Be 0
      Should -Invoke Register-ScheduledTask -Times 1 -Exactly -ParameterFilter {
        $Principal.UserId -eq "VM\verifier" -and
        "$($Principal.LogonType)" -eq "Interactive" -and
        "$($Principal.RunLevel)" -eq "Limited" -and
        $Action.Execute -eq "powershell.exe"
      }
      Should -Invoke Unregister-ScheduledTask -Times 1 -Exactly
    }

    It "fails and unregisters the task when it never starts" {
      $script:taskStates = @("Ready")

      { Invoke-VmVerifyGuestInteractiveTask -TaskName "azooKey-vm-verify-test" -UserName "VM\verifier" `
          -Argument "x" -TimeoutSeconds 60 -StartTimeoutSeconds 1 } |
        Should -Throw -ExpectedMessage "*did not start*signed in to the console*"
      Should -Invoke Unregister-ScheduledTask -Times 1 -Exactly
    }

    It "stops the task and fails when it exceeds the timeout" {
      $script:taskStates = @("Running")

      { Invoke-VmVerifyGuestInteractiveTask -TaskName "azooKey-vm-verify-test" -UserName "VM\verifier" `
          -Argument "x" -TimeoutSeconds 1 } |
        Should -Throw -ExpectedMessage "*did not finish*"
      Should -Invoke Stop-ScheduledTask -Times 1 -Exactly
      Should -Invoke Unregister-ScheduledTask -Times 1 -Exactly
    }

    It "returns an HRESULT-sized task result without overflowing" {
      Mock Get-ScheduledTaskInfo {
        [pscustomobject]@{ LastRunTime = $script:taskRunTime; LastTaskResult = [uint32]3221225786 }
      }

      Invoke-VmVerifyGuestInteractiveTask -TaskName "azooKey-vm-verify-test" -UserName "VM\verifier" `
        -Argument "x" -TimeoutSeconds 60 | Should -Be 3221225786
    }

    It "names the task and user when registration fails" {
      Mock Register-ScheduledTask { throw "Access is denied." }

      { Invoke-VmVerifyGuestInteractiveTask -TaskName "azooKey-vm-verify-test" -UserName "VM\verifier" `
          -Argument "x" -TimeoutSeconds 60 } |
        Should -Throw -ExpectedMessage "*'azooKey-vm-verify-test' for 'VM\verifier'*Access is denied.*"
    }
  }

  Context "-Run interactive runner" {
    BeforeEach {
      $packageRoot = Join-Path $TestDrive ("runner-pkg-" + [guid]::NewGuid().ToString("N"))
      $runRoot = Join-Path $TestDrive ("runner-run-" + [guid]::NewGuid().ToString("N"))
      New-Item -ItemType Directory -Path (Join-Path $packageRoot "targets") -Force | Out-Null
      New-Item -ItemType Directory -Path $runRoot -Force | Out-Null
      "{}" | Set-Content -LiteralPath (Join-Path $packageRoot "targets\notepad.json")
      "{}" | Set-Content -LiteralPath (Join-Path $packageRoot "targets\edge.json")
      foreach ($name in @("edge", "notepad")) {
        $reportDirectory = Join-Path $runRoot "compat-report-$name"
        New-Item -ItemType Directory -Path $reportDirectory -Force | Out-Null
        "{}" | Set-Content -LiteralPath (Join-Path $reportDirectory "report.json")
      }
      $script:hostSessionId = 1
      Mock Invoke-VmVerifyGuestBootstrap {
        @{ overallStatus = "pass"; hostBinary = @{ processId = 4242; reason = "" } } |
          ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $OutputPath
        return 0
      }
      Mock Invoke-VmVerifyGuestInputMethodSelection { "switched" }
      Mock Get-Process { [pscustomobject]@{ SessionId = 1 } }
      Mock Get-Process -ParameterFilter { $Id -eq 4242 } {
        [pscustomobject]@{ SessionId = $script:hostSessionId }
      }
      Mock Start-Process {
        [pscustomobject]@{ ExitCode = 2; Handle = [IntPtr]::Zero } |
          Add-Member -MemberType ScriptMethod -Name WaitForExit -Value { } -PassThru
      }

      function Read-RunStatus {
        return Get-Content -Raw -LiteralPath (Join-Path $runRoot "interactive-status.json") |
          ConvertFrom-Json
      }
    }

    It "re-runs bootstrap in the interactive session and runs compat for every target" {
      Invoke-VmVerifyGuestCompatRun -PackageRoot $packageRoot -RunRoot $runRoot

      $status = Read-RunStatus
      $status.completed | Should -BeTrue
      $status.hostSessionId | Should -Be 1
      @($status.targets | ForEach-Object { "$($_.target)=$($_.exitCode)=$($_.reportJson)" }) |
        Should -Be @("edge=2=True", "notepad=2=True")
      Should -Invoke Invoke-VmVerifyGuestBootstrap -Times 1 -Exactly -ParameterFilter {
        $CheckpointConfirmed -eq $true
      }
      Should -Invoke Start-Process -Times 1 -Exactly -ParameterFilter {
        $ArgumentList -like "*--target*notepad.json*--output*compat-report-notepad*"
      }
    }

    It "passes the case selection to compat_test.exe for every target and records it" {
      Invoke-VmVerifyGuestCompatRun -PackageRoot $packageRoot -RunRoot $runRoot `
        -Cases "C-001,C-004" -Skip "C-010"

      $status = Read-RunStatus
      $status.completed | Should -BeTrue
      $status.cases | Should -Be "C-001,C-004"
      $status.skip | Should -Be "C-010"
      Should -Invoke Start-Process -Times 2 -Exactly -ParameterFilter {
        $ArgumentList -like "*--output*`" --cases C-001,C-004 --skip C-010"
      }
    }

    It "runs every case when no selection is given" {
      Invoke-VmVerifyGuestCompatRun -PackageRoot $packageRoot -RunRoot $runRoot

      Should -Invoke Start-Process -Times 0 -Exactly -ParameterFilter {
        $ArgumentList -match "--cases|--skip"
      }
    }

    It "refuses to run compat against a host in another session" {
      $script:hostSessionId = 0

      Invoke-VmVerifyGuestCompatRun -PackageRoot $packageRoot -RunRoot $runRoot

      $status = Read-RunStatus
      $status.completed | Should -BeFalse
      $status.error | Should -Match "runs in session 0, not in the interactive session 1"
      Should -Invoke Start-Process -Times 0 -Exactly
    }

    It "records a failing interactive bootstrap without running compat" {
      Mock Invoke-VmVerifyGuestBootstrap {
        @{ overallStatus = "fail"; hostBinary = @{ processId = 0; reason = "" } } |
          ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $OutputPath
        return 1
      }

      Invoke-VmVerifyGuestCompatRun -PackageRoot $packageRoot -RunRoot $runRoot

      $status = Read-RunStatus
      $status.completed | Should -BeFalse
      $status.bootstrapExitCode | Should -Be 1
      $status.error | Should -Match "overallStatus=fail in the interactive session"
      Should -Invoke Start-Process -Times 0 -Exactly
    }

    It "does not run compat when azooKey cannot be selected as the input method" {
      Mock Invoke-VmVerifyGuestInputMethodSelection {
        throw "azooKey could not be made the default input method of the console user."
      }

      Invoke-VmVerifyGuestCompatRun -PackageRoot $packageRoot -RunRoot $runRoot

      $status = Read-RunStatus
      $status.completed | Should -BeFalse
      $status.error | Should -Match "default input method"
      Should -Invoke Start-Process -Times 0 -Exactly
    }
  }

  Context "-Run input method selection" {
    BeforeEach {
      # International モジュールの cmdlet は実行ユーザーの設定を書き換えるため、
      # 引数の型を持たないスタブへ差し替えてから Mock する。
      function Get-WinDefaultInputMethodOverride { [CmdletBinding()] param() }
      function Get-WinUserLanguageList { [CmdletBinding()] param() }
      function Set-WinUserLanguageList {
        [CmdletBinding(SupportsShouldProcess = $true)]
        param($LanguageList, [switch]$Force)
        if ($PSCmdlet.ShouldProcess("$LanguageList", "Set $Force")) {
          throw "Set-WinUserLanguageList must be mocked in tests."
        }
      }
      function Set-WinDefaultInputMethodOverride {
        [CmdletBinding(SupportsShouldProcess = $true)]
        param($InputTip)
        if ($PSCmdlet.ShouldProcess($InputTip)) {
          throw "Set-WinDefaultInputMethodOverride must be mocked in tests."
        }
      }

      $script:azooKeyTip = "0411:{71EE04FA-B35D-4EB8-87A1-582D44A9A58C}{A8F74D91-8DF3-4DA1-B80B-01F7C73D4A90}"
      $microsoftImeTip = "0411:{03B5835F-F03C-411B-9CE2-AA23E1171E36}{A76C93D9-5523-4E90-AAFA-4DB112F9AC76}"
      $script:overrideTip = ""
      $script:languageList = @([pscustomobject]@{
          LanguageTag = "ja"
          InputMethodTips = [System.Collections.Generic.List[string]]::new([string[]]@($microsoftImeTip))
        })
      Mock Get-WinDefaultInputMethodOverride {
        if ($script:overrideTip) {
          [pscustomobject]@{ InputMethodTip = $script:overrideTip }
        }
      }
      Mock Get-WinUserLanguageList { $script:languageList }
      Mock Set-WinUserLanguageList {}
      Mock Set-WinDefaultInputMethodOverride { $script:overrideTip = $InputTip }
    }

    It "adds azooKey to the Japanese input methods and makes it the default" {
      Invoke-VmVerifyGuestInputMethodSelection | Should -Be "switched"

      $script:languageList[0].InputMethodTips | Should -Contain $script:azooKeyTip
      $script:languageList[0].InputMethodTips | Should -Contain $microsoftImeTip
      Should -Invoke Set-WinUserLanguageList -Times 1 -Exactly
      Should -Invoke Set-WinDefaultInputMethodOverride -Times 1 -Exactly -ParameterFilter {
        $InputTip -eq $script:azooKeyTip
      }
    }

    It "leaves the settings alone when azooKey is already the default" {
      $script:overrideTip = $script:azooKeyTip

      Invoke-VmVerifyGuestInputMethodSelection | Should -Be "already-default"

      Should -Invoke Set-WinUserLanguageList -Times 0 -Exactly
      Should -Invoke Set-WinDefaultInputMethodOverride -Times 0 -Exactly
    }

    It "fails when Japanese is not in the language list" {
      $script:languageList = @([pscustomobject]@{
          LanguageTag = "en-US"
          InputMethodTips = [System.Collections.Generic.List[string]]::new()
        })

      { Invoke-VmVerifyGuestInputMethodSelection } | Should -Throw -ExpectedMessage "*Japanese*"
    }

    It "fails when the override does not take effect" {
      Mock Set-WinDefaultInputMethodOverride {}

      { Invoke-VmVerifyGuestInputMethodSelection } |
        Should -Throw -ExpectedMessage "*could not be made the default input method*"
    }
  }

  Context "-Run guest script portability" {
    It "writes a runner script that defines the bootstrap helper and the compat run" {
      $runner = Get-VmVerifySessionRunnerScript
      $tokens = $null
      $errors = $null
      $ast = [System.Management.Automation.Language.Parser]::ParseInput($runner, [ref]$tokens, [ref]$errors)

      $errors | Should -BeNullOrEmpty
      @($ast.FindAll({ param($node) $node -is [System.Management.Automation.Language.FunctionDefinitionAst] }, $false) |
          ForEach-Object { $_.Name }) |
        Should -Be @(
          "Invoke-VmVerifyGuestBootstrap",
          "Invoke-VmVerifyGuestInputMethodSelection",
          "Invoke-VmVerifyGuestCompatRun")
    }

    It "encodes the task command so that quoted guest paths survive" {
      $command = ". 'C:\azookey-verify\runs\it''s\runner.ps1'; Invoke-VmVerifyGuestCompatRun"
      $argument = Get-VmVerifySessionEncodedArgument -Command $command

      $argument | Should -Match "-WindowStyle Hidden"
      $encoded = ($argument -split " ")[-1]
      [System.Text.Encoding]::Unicode.GetString([Convert]::FromBase64String($encoded)) |
        Should -Be $command
    }

    It "parses the guest functions with Windows PowerShell 5.1" -Skip:(
      -not (Get-Command powershell.exe -ErrorAction SilentlyContinue)) {
      $guestScript = Join-Path $repoRoot "scripts\vm-verify-guest.ps1"
      $probe = "`$e = `$null; [void][System.Management.Automation.Language.Parser]::ParseFile(" +
        "'$($guestScript.Replace("'", "''"))', [ref]`$null, [ref]`$e); `$e.Count"

      $errorCount = & powershell.exe -NoProfile -NonInteractive -Command $probe

      $errorCount | Should -Be "0"
    }
  }

  Context "script entrypoint" {
    It "exits non-zero when none of -Prepare, -Restore, or -Run is given" {
      $output = & pwsh -NoProfile -File $sessionScript -VMName "azooKey-VM" 2>&1
      $LASTEXITCODE | Should -Not -Be 0
      ($output | Out-String) | Should -Match "exactly one of -Prepare, -Restore, or -Run"
    }

    It "exits non-zero when both -Prepare and -Restore are given" {
      $output = & pwsh -NoProfile -File $sessionScript -Prepare -Restore -VMName "azooKey-VM" 2>&1
      $LASTEXITCODE | Should -Not -Be 0
      ($output | Out-String) | Should -Match "exactly one of -Prepare, -Restore, or -Run"
    }

    It "exits non-zero when -Run is combined with -Prepare" {
      $output = & pwsh -NoProfile -File $sessionScript -Prepare -Run -VMName "azooKey-VM" 2>&1
      $LASTEXITCODE | Should -Not -Be 0
      ($output | Out-String) | Should -Match "exactly one of -Prepare, -Restore, or -Run"
    }

    It "exits non-zero when a case selection is given without -Run" {
      $output = & pwsh -NoProfile -File $sessionScript -Restore -VMName "azooKey-VM" -CompatSkip "C-010" 2>&1
      $LASTEXITCODE | Should -Not -Be 0
      ($output | Out-String) | Should -Match "apply only to -Run"
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
