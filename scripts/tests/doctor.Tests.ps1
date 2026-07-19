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

  Context "just integration" {
    It "forwards optional doctor arguments through pwsh file mode" {
      $script:justfileText | Should -Match '(?m)^doctor \*args:\r?$'
      $script:justfileText | Should -Match (
        '(?m)^\s+@pwsh -NoProfile -File \./scripts/doctor\.ps1 \{\{args\}\}\r?$'
      )
    }
  }
}
