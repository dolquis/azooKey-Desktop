Describe "development environment doctor" {
  BeforeAll {
    $repoRoot = (Resolve-Path (Join-Path (Join-Path $PSScriptRoot "..") "..")).Path
    $doctorPath = Join-Path (Join-Path $repoRoot "scripts") "doctor.ps1"
    $script:justfileText = Get-Content -Raw -LiteralPath (Join-Path $repoRoot "justfile")
    . $doctorPath
  }

  Context "command checks" {
    It "reports a required missing command as an error" {
      Mock Get-Command { $null } -ParameterFilter { $Name -eq "missing-tool.exe" }

      $check = Get-DoctorCommandCheck -Id "tool.missing" -Name "Missing tool" `
        -CommandName "missing-tool.exe" -Required $true -Hint "Install it."

      $check.status | Should -Be "error"
      $check.required | Should -BeTrue
      $check.hint | Should -Be "Install it."
    }

    It "reports an optional missing command as a warning" {
      Mock Get-Command { $null } -ParameterFilter { $Name -eq "optional-tool.exe" }

      $check = Get-DoctorCommandCheck -Id "tool.optional" -Name "Optional tool" `
        -CommandName "optional-tool.exe" -Required $false -Hint "Install it if needed."

      $check.status | Should -Be "warning"
      $check.required | Should -BeFalse
    }

    It "reports a present command and its version" {
      Mock Get-Command {
        [pscustomobject]@{ Source = "C:\tools\present-tool.exe" }
      } -ParameterFilter { $Name -eq "present-tool.exe" }
      Mock Get-DoctorCommandVersion { "present-tool 1.2.3" }

      $check = Get-DoctorCommandCheck -Id "tool.present" -Name "Present tool" `
        -CommandName "present-tool.exe" -Required $true -Hint "Install it."

      $check.status | Should -Be "ok"
      $check.version | Should -Be "present-tool 1.2.3"
      $check.details | Should -Be "C:\tools\present-tool.exe"
    }
  }

  Context "diagnostic tool checks" {
    It "accepts the first available application name" {
      Mock Get-Command {
        if ($Name -eq "second-tool.exe") {
          [pscustomobject]@{ Source = "C:\tools\second-tool.exe" }
        }
      }

      $check = Get-DoctorApplicationCheck -Id "tool.alternative" -Name "Alternative" `
        -CommandNames @("first-tool.exe", "second-tool.exe") -Required $false `
        -Hint "Install it."

      $check.status | Should -Be "ok"
      $check.details | Should -Be "C:\tools\second-tool.exe"
    }

    It "accepts an application in a known install location" {
      Mock Get-Command { $null }
      Mock Test-Path { $true } -ParameterFilter {
        $LiteralPath -eq "C:\kits\known-tool.exe"
      }

      $check = Get-DoctorApplicationCheck -Id "tool.known-path" -Name "Known path" `
        -CommandNames @("known-tool.exe") `
        -CandidatePaths @("C:\kits\known-tool.exe") -Required $false `
        -Hint "Install it."

      $check.status | Should -Be "ok"
      $check.details | Should -Be "C:\kits\known-tool.exe"
    }

    It "detects the packaged WinDbg application" {
      Mock Get-DoctorApplicationCheck {
        Get-DoctorCheck -Id "tool.windbg" -Name "WinDbg" -Status "warning" `
          -Required $false -Hint "Install WinDbg."
      }
      Mock Get-Command {
        [pscustomobject]@{ Name = "Get-AppxPackage" }
      } -ParameterFilter { $Name -eq "Get-AppxPackage" }
      Mock Get-AppxPackage {
        [pscustomobject]@{
          PackageFullName = "Microsoft.WinDbg_1.2.3.4_x64__8wekyb3d8bbwe"
          Version = [version]"1.2.3.4"
        }
      }

      $check = Get-WinDbgCheck

      $check.status | Should -Be "ok"
      $check.version | Should -Be "1.2.3.4"
      $check.details | Should -Match "^Appx: Microsoft\.WinDbg_"
    }

    It "publishes stable optional IDs for every diagnostic tool" {
      $scriptText = Get-Content -Raw -LiteralPath $doctorPath
      $expectedIds = @(
        "tool.windbg"
        "tool.cdb"
        "tool.gflags"
        "tool.wpr"
        "tool.wpa"
        "tool.procdump"
        "tool.procmon"
        "tool.appverifier"
        "tool.handle"
      )

      foreach ($id in $expectedIds) {
        $scriptText | Should -Match ([regex]::Escape("-Id `"$id`""))
      }
    }
  }

  Context "report schema" {
    It "uses the worst check status and emits stable JSON fields" {
      $checks = @(
        Get-DoctorCheck -Id "one" -Name "One" -Status "ok" -Required $true
        Get-DoctorCheck -Id "two" -Name "Two" -Status "warning" -Required $false
        Get-DoctorCheck -Id "three" -Name "Three" -Status "error" -Required $true
      )

      $report = Get-DoctorReport -RepoRoot "C:\repo" -Checks $checks
      $json = $report | ConvertTo-Json -Depth 6 | ConvertFrom-Json

      $json.schemaVersion | Should -Be 1
      $json.status | Should -Be "error"
      $json.summary.ok | Should -Be 1
      $json.summary.warning | Should -Be 1
      $json.summary.error | Should -Be 1
      $json.checks.Count | Should -Be 3
      $json.checks[0].id | Should -Be "one"
    }

    It "does not promote optional warnings to errors" {
      $checks = @(
        Get-DoctorCheck -Id "one" -Name "One" -Status "ok" -Required $true
        Get-DoctorCheck -Id "two" -Name "Two" -Status "warning" -Required $false
      )

      (Get-DoctorStatus -Checks $checks) | Should -Be "warning"
    }
  }

  Context "WPR profile" {
    It "defines a file-mode profile backed by the expected system provider" {
      $profilePath = Join-Path (Join-Path $repoRoot "diagnostics") `
        "azookey-diagnostics.wprp"
      [xml]$profile = Get-Content -Raw -LiteralPath $profilePath
      $profiles = $profile.WindowsPerformanceRecorder.Profiles
      $verboseFile = $profiles.Profile |
        Where-Object Id -eq "AzooKeyDiagnostics.Verbose.File"
      $systemProvider = $profiles.SystemProvider |
        Where-Object Id -eq "SystemProvider_AzooKeyDiagnostics"
      $keywords = @($systemProvider.Keywords.Keyword | ForEach-Object Value)

      $verboseFile.LoggingMode | Should -Be "File"
      $verboseFile.Collectors.SystemCollectorId.SystemProviderId.Value |
        Should -Be "SystemProvider_AzooKeyDiagnostics"
      $keywords | Should -Contain "ProcessThread"
      $keywords | Should -Contain "FileIO"
      $keywords | Should -Contain "Registry"
      $keywords | Should -Contain "NetworkTrace"
    }
  }

  Context "just integration" {
    It "forwards optional doctor arguments through pwsh file mode" {
      $script:justfileText | Should -Match '(?m)^doctor \*args:\r?$'
      $script:justfileText | Should -Match (
        '(?m)^\s+@pwsh -NoProfile -File \./scripts/doctor\.ps1 \{\{args\}\}\r?$'
      )
    }
  }
}
