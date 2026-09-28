Describe "VM verification MSI lane" {
  BeforeAll {
    $repoRoot = (Resolve-Path (Join-Path (Join-Path $PSScriptRoot "..") "..")).Path
    $sessionScript = Join-Path $repoRoot "scripts\vm-verify-session.ps1"
    . $sessionScript

    function Initialize-TestMsi {
      param(
        [Parameter(Mandatory = $true)]
        [string]$Root,
        [string]$Content = "msi-content"
      )

      New-Item -ItemType Directory -Path $Root -Force | Out-Null
      $path = Join-Path $Root "azooKey-0.9.0.msi"
      [System.IO.File]::WriteAllText($path, $Content)
      $sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
      return [pscustomobject]@{
        Path = $path
        Sha256 = $sha256
        ShortHash = $sha256.Substring(0, 12)
      }
    }

    $secret = New-Object System.Security.SecureString
    foreach ($character in "s3cret-Guest-Pw".ToCharArray()) {
      $secret.AppendChar($character)
    }
    $script:testCredential = [System.Management.Automation.PSCredential]::new("VM\verifier", $secret)
  }

  Context "identity" {
    It "derives the checkpoint and guest directory from the MSI content, not its name" {
      $first = Initialize-TestMsi -Root (Join-Path $TestDrive "identity-a") -Content "build-a"
      $second = Initialize-TestMsi -Root (Join-Path $TestDrive "identity-b") -Content "build-b"

      $a = Get-VmVerifyMsiIdentity -PackagePath $first.Path
      $b = Get-VmVerifyMsiIdentity -PackagePath $second.Path

      $a.CheckpointName | Should -Be "pre-azookey-msi-$($first.ShortHash)"
      $a.Sha256 | Should -Be $first.Sha256
      $a.FileName | Should -Be $b.FileName
      $a.CheckpointName | Should -Not -Be $b.CheckpointName
      (Get-VmVerifyMsiGuestPath -GuestDestination "C:\azookey-verify" -Identity $a).Msi |
        Should -Be "C:\azookey-verify\msi-$($first.ShortHash)\azooKey-0.9.0.msi"
    }

    It "treats only an .msi path as an MSI package" {
      Test-VmVerifyMsiPackagePath -PackagePath "C:\out\azooKey.MSI" | Should -BeTrue
      Test-VmVerifyMsiPackagePath -PackagePath "C:\out\azookey-verify-0123-windows-release.zip" | Should -BeFalse
      Test-VmVerifyMsiPackagePath -PackagePath "" | Should -BeFalse
    }
  }

  Context "-Prepare with an MSI" {
    BeforeEach {
      $script:parent = "clean-no-vcredist"
      $script:existing = @("clean-no-vcredist")
      Mock Get-VmVerifySessionVM {
        [pscustomobject]@{ Name = "azooKey-VM"; State = "Running"; ParentSnapshotName = $script:parent }
      }
      Mock Get-VmVerifySessionGuestServiceInterface { [pscustomobject]@{ Enabled = $true } }
      Mock Get-VmVerifySessionCheckpoint {
        if ($script:existing -contains $CheckpointName) { [pscustomobject]@{ Name = $CheckpointName } }
      }
      Mock Invoke-VmVerifySessionCheckpoint {}
      Mock Invoke-VmVerifySessionFileCopy {}
      Mock Write-Host {}
      $msi = Initialize-TestMsi -Root (Join-Path $TestDrive ("prepare-" + [guid]::NewGuid().ToString("N")))
      $results = Join-Path $TestDrive ("results-" + [guid]::NewGuid().ToString("N"))

      function Invoke-TestPrepare {
        param([string]$CleanCheckpointName = "clean-no-vcredist")
        Invoke-VmVerifyMsiPrepare -RepositoryRoot $TestDrive -VMName "azooKey-VM" -PackagePath $msi.Path `
          -CleanCheckpointName $CleanCheckpointName -GuestDestination "C:\azookey-verify" `
          -ResultsDirectory $results
      }
    }

    It "copies the MSI before taking its checkpoint so that a restore still holds it" {
      $script:order = @()
      Mock Invoke-VmVerifySessionFileCopy { $script:order += "copy" }
      Mock Invoke-VmVerifySessionCheckpoint { $script:order += "checkpoint" }

      Invoke-TestPrepare | Out-Null

      $script:order | Should -Be @("copy", "checkpoint")
    }

    It "leaves no checkpoint behind when the copy fails, so -Prepare can run again" {
      Mock Invoke-VmVerifySessionFileCopy { throw "Copy-VMFile failed." }

      { Invoke-TestPrepare } | Should -Throw -ExpectedMessage "*Copy-VMFile failed*"
      Should -Invoke Invoke-VmVerifySessionCheckpoint -Times 0 -Exactly
    }

    It "records the MSI hash, takes its checkpoint, and copies it next to the log directory" {
      $result = Invoke-TestPrepare

      $result.CheckpointName | Should -Be "pre-azookey-msi-$($msi.ShortHash)"
      $result.GuestPath | Should -Be "C:\azookey-verify\msi-$($msi.ShortHash)\azooKey-0.9.0.msi"
      Should -Invoke Invoke-VmVerifySessionCheckpoint -Times 1 -Exactly -ParameterFilter {
        $CheckpointName -eq "pre-azookey-msi-$($msi.ShortHash)"
      }
      Should -Invoke Invoke-VmVerifySessionFileCopy -Times 1 -Exactly -ParameterFilter {
        $SourcePath -eq $msi.Path -and $DestinationPath -eq $result.GuestPath
      }
      $record = Get-Content -Raw -LiteralPath $result.RecordPath | ConvertFrom-Json
      $record.kind | Should -Be "msi"
      $record.sha256 | Should -Be $msi.Sha256
      $record.cleanCheckpoint | Should -Be "clean-no-vcredist"
      $record.checkpoint | Should -Be $result.CheckpointName
      $result.RecordPath | Should -Be (Join-Path $results "azooKey-0.9.0-$($msi.ShortHash)\msi-record.json")
    }

    It "leaves msiexec to a person and names the log path -Collect retrieves" {
      Invoke-TestPrepare | Out-Null

      Should -Invoke Write-Host -Times 1 -Exactly -ParameterFilter {
        $Object -eq "  msiexec /i `"C:\azookey-verify\msi-$($msi.ShortHash)\azooKey-0.9.0.msi`" /L*v `"C:\azookey-verify\msi-$($msi.ShortHash)\install.log`""
      }
      Should -Invoke Write-Host -ParameterFilter { $Object -match "does not run msiexec" }
    }

    It "requires the clean checkpoint name instead of guessing the lane 1 start state" {
      { Invoke-TestPrepare -CleanCheckpointName "" } | Should -Throw -ExpectedMessage "*-CleanCheckpointName is required*"
      Should -Invoke Invoke-VmVerifySessionCheckpoint -Times 0 -Exactly
    }

    It "fails and copies nothing when the clean checkpoint does not exist" {
      $script:existing = @()

      { Invoke-TestPrepare } | Should -Throw -ExpectedMessage "*Clean checkpoint 'clean-no-vcredist' was not found*"
      Should -Invoke Invoke-VmVerifySessionFileCopy -Times 0 -Exactly
    }

    It "fails and copies nothing when the VM was not restored to the clean checkpoint" {
      $script:parent = "baseline-with-vcredist"

      { Invoke-TestPrepare } | Should -Throw -ExpectedMessage "*based on 'baseline-with-vcredist', not on the clean checkpoint*"
      Should -Invoke Invoke-VmVerifySessionCheckpoint -Times 0 -Exactly
      Should -Invoke Invoke-VmVerifySessionFileCopy -Times 0 -Exactly
    }

    It "does not overwrite the checkpoint of the same MSI" {
      $script:existing = @("clean-no-vcredist", "pre-azookey-msi-$($msi.ShortHash)")

      { Invoke-TestPrepare } | Should -Throw -ExpectedMessage "*already exists*"
      Should -Invoke Invoke-VmVerifySessionFileCopy -Times 0 -Exactly
    }
  }

  Context "-Collect" {
    BeforeEach {
      Mock Get-VmVerifySessionVM { [pscustomobject]@{ Name = "azooKey-VM"; State = "Running" } }
      Mock Get-VmVerifySessionCredential { throw "Get-VmVerifySessionCredential must not prompt here." }
      Mock Open-VmVerifySessionGuestSession { [pscustomobject]@{ Id = 7 } }
      Mock Close-VmVerifySessionGuestSession {}
      Mock Copy-VmVerifySessionGuestArtifact {}
      Mock Write-Host {}
      $msi = Initialize-TestMsi -Root (Join-Path $TestDrive ("collect-" + [guid]::NewGuid().ToString("N")))
      $results = Join-Path $TestDrive ("results-" + [guid]::NewGuid().ToString("N"))
      $script:guestState = [pscustomobject]@{ Sha256 = $msi.Sha256; Logs = @("install.log", "repair.log") }
      Mock Invoke-VmVerifySessionGuestStep { $script:guestState } -ParameterFilter { $Name -eq "Get-VmVerifyGuestMsiState" }

      function Invoke-TestCollect {
        Invoke-VmVerifyMsiCollect -RepositoryRoot $TestDrive -VMName "azooKey-VM" -PackagePath $msi.Path `
          -GuestDestination "C:\azookey-verify" -Credential $script:testCredential -ResultsDirectory $results
      }
    }

    It "collects every msiexec log after matching the guest MSI hash" {
      $result = Invoke-TestCollect

      $result.Logs | Should -Be @("install.log", "repair.log")
      $result.ResultsPath | Should -BeLike (Join-Path $results "azooKey-0.9.0-$($msi.ShortHash)\logs-*")
      Should -Invoke Invoke-VmVerifySessionGuestStep -Times 1 -Exactly -ParameterFilter {
        $ArgumentList[0] -eq "C:\azookey-verify\msi-$($msi.ShortHash)\azooKey-0.9.0.msi" -and
        $ArgumentList[1] -eq "C:\azookey-verify\msi-$($msi.ShortHash)"
      }
      Should -Invoke Copy-VmVerifySessionGuestArtifact -Times 1 -Exactly -ParameterFilter {
        $GuestPath -eq "C:\azookey-verify\msi-$($msi.ShortHash)\repair.log" -and
        $Destination -eq (Join-Path $result.ResultsPath "repair.log")
      }
      Should -Invoke Close-VmVerifySessionGuestSession -Times 1 -Exactly
      $identity = Get-Content -Raw -LiteralPath (Join-Path $result.ResultsPath "msi-identity.json") | ConvertFrom-Json
      $identity.sha256 | Should -Be $msi.Sha256
      $identity.guestSha256 | Should -Be $msi.Sha256
    }

    It "refuses logs of an MSI that differs from the one given" {
      $script:guestState = [pscustomobject]@{ Sha256 = ("0" * 64); Logs = @("install.log") }

      { Invoke-TestCollect } | Should -Throw -ExpectedMessage "*differs from*attributed to another build*"
      Should -Invoke Copy-VmVerifySessionGuestArtifact -Times 0 -Exactly
      Should -Invoke Close-VmVerifySessionGuestSession -Times 1 -Exactly
    }

    It "fails when -Prepare has not placed the MSI" {
      $script:guestState = [pscustomobject]@{ Sha256 = ""; Logs = @() }

      { Invoke-TestCollect } | Should -Throw -ExpectedMessage "*Run -Prepare with the same -PackagePath first*"
    }

    It "fails when msiexec wrote no log next to the MSI" {
      $script:guestState = [pscustomobject]@{ Sha256 = $msi.Sha256; Logs = @() }

      { Invoke-TestCollect } | Should -Throw -ExpectedMessage "*No msiexec log*/L*v*"
    }

    It "names the read-only disk route when the guest session cannot be opened" {
      Mock Open-VmVerifySessionGuestSession { throw "The credential is invalid." }

      { Invoke-TestCollect } | Should -Throw -ExpectedMessage "*Mount-VHD*"
    }
  }

  Context "guest state" {
    It "returns the MSI hash and the log names in the MSI directory" {
      $directory = Join-Path $TestDrive "guest-msi"
      $msi = Initialize-TestMsi -Root $directory
      "log" | Set-Content -LiteralPath (Join-Path $directory "install.log")

      $state = Get-VmVerifyGuestMsiState -MsiPath $msi.Path -Directory $directory

      $state.Sha256 | Should -Be $msi.Sha256
      $state.Logs | Should -Be @("install.log")
    }
  }

  Context "Windows PowerShell 5.1 portability" {
    It "keeps the MSI helpers ASCII-only so that 5.1 does not drop lines" {
      $bytes = [System.IO.File]::ReadAllBytes((Join-Path $repoRoot "scripts\vm-verify-msi.ps1"))
      @($bytes | Where-Object { $_ -gt 0x7F }).Count | Should -Be 0
    }

    It "parses the MSI helpers with Windows PowerShell 5.1" -Skip:(
      -not (Get-Command powershell.exe -ErrorAction SilentlyContinue)) {
      $script = Join-Path $repoRoot "scripts\vm-verify-msi.ps1"
      $probe = "`$e = `$null; [void][System.Management.Automation.Language.Parser]::ParseFile(" +
        "'$($script.Replace("'", "''"))', [ref]`$null, [ref]`$e); `$e.Count"

      & powershell.exe -NoProfile -NonInteractive -Command $probe | Should -Be "0"
    }
  }

  Context "script entrypoint" {
    It "rejects -Run with an MSI because a person runs msiexec" {
      $output = & pwsh -NoProfile -File $sessionScript -Run -VMName "azooKey-VM" -PackagePath "C:\out\azooKey.msi" 2>&1
      $LASTEXITCODE | Should -Not -Be 0
      ($output | Out-String) | Should -Match "-Run does not accept an MSI"
    }

    It "rejects -Collect for a verification zip" {
      $output = & pwsh -NoProfile -File $sessionScript -Collect -VMName "azooKey-VM" -PackagePath "C:\out\a.zip" 2>&1
      $LASTEXITCODE | Should -Not -Be 0
      ($output | Out-String) | Should -Match "-Collect applies only to an MSI"
    }

    It "rejects a clean checkpoint name outside MSI -Prepare" {
      $output = & pwsh -NoProfile -File $sessionScript -Restore -VMName "azooKey-VM" `
        -PackagePath "C:\out\azooKey.msi" -CleanCheckpointName "clean" 2>&1
      $LASTEXITCODE | Should -Not -Be 0
      ($output | Out-String) | Should -Match "-CleanCheckpointName applies only to -Prepare with an MSI"
    }
  }
}
