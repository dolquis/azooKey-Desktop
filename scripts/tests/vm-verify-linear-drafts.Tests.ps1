Describe "VM verification Linear drafts" {
  BeforeAll {
    $repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
    $draftScript = Join-Path $repoRoot "scripts/vm-verify-linear-drafts.ps1"
    . $draftScript
    $script:commit = "0123456789abcdef0123456789abcdef01234567"

    function Write-DraftTestJson {
      param([string]$Path, $Value)
      $Value | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $Path -Encoding utf8
      return $Path
    }

    function Write-DraftTestSummary {
      param([string]$Root)
      return Write-DraftTestJson -Path (Join-Path $Root "summary.json") -Value @{
        schemaVersion = 1
        package = @{ commit = $script:commit; preset = "windows-release" }
        os = @{ build = "26100.4061" }
        bootstrap = @{
          packageCommitVsManifest = "match"
          checks = @(
            @{ id = "vmCheckpoint"; status = "manual_required"; message = "private input C:\Users\alice\log.txt" }
          )
        }
        diag = @{ checks = @(@{ id = "D-004"; status = "error"; message = "private input" }) }
        compat = @(
          @{ targetId = "notepad"; results = @(
              @{ id = "C-001"; status = "pass"; reasonCode = "none" }
              @{ id = "C-010"; status = "failing-skip"; reasonCode = "runner-prerequisite" }
            ) },
          @{ targetId = "vscode"; caseSelection = @{ excluded = @("C-010") }; results = @(
              @{ id = "C-001"; status = "fail"; reasonCode = "private input" }
            ) }
        )
      }
    }
  }

  It "writes issue-specific drafts with distinct observations and blank human results" {
    $root = Join-Path $TestDrive "normal"
    New-Item -ItemType Directory -Path $root -Force | Out-Null
    $summaryPath = Write-DraftTestSummary -Root $root
    $mapPath = Write-DraftTestJson -Path (Join-Path $root "map.json") -Value @{
      schemaVersion = 1
      gates = @(
        @{ source = "compat"; id = "C-001"; issueId = "DEV-716" }
        @{ source = "compat"; id = "C-010"; issueId = "DEV-716" }
        @{ source = "compat"; id = "C-010"; issueId = "DEV-676" }
        @{ source = "bootstrap"; id = "vmCheckpoint"; issueId = "DEV-716" }
        @{ source = "diag"; id = "D-004"; issueId = "DEV-676" }
        @{ source = "diag"; id = "D-999"; issueId = "DEV-676" }
      )
    }
    $output = Join-Path $root "out"
    $issues = @(Invoke-VmVerifyLinearDraft -SummaryPath $summaryPath -GateMapPath $mapPath `
        -OutputDirectory $output -CheckpointName "Baseline 1")

    $issues | Should -Be @("DEV-676", "DEV-716")
    $gate716 = Get-Content -Raw -LiteralPath (Join-Path $output "DEV-716.md")
    $gate676 = Get-Content -Raw -LiteralPath (Join-Path $output "DEV-676.md")
    $gate716 | Should -Match "commit:  / $script:commit"
    $gate716 | Should -Match "26100.4061"
    $gate716 | Should -Match "開始 checkpoint 名: Baseline 1"
    $gate716 | Should -Match "\| compat \| C-001 \| notepad \| pass \|"
    $gate716 | Should -Match "\| compat \| C-001 \| vscode \| fail \|"
    $gate716 | Should -Match "\| bootstrap \| vmCheckpoint \|  \| manual_required \|"
    $gate716 | Should -Match "- bootstrap vmCheckpoint の人間確認:\n"
    $gate716 | Should -Not -Match "D-004"
    $gate676 | Should -Match "\| compat \| C-010 \| notepad \| failing-skip \|"
    $gate676 | Should -Match "\| compat \| C-010 \| vscode \| 未実行 \|"
    $gate676 | Should -Match "\| diag \| D-999 \|  \| 未取得 \|"
    $gate676 | Should -Not -Match "C-001"
    foreach ($draft in @($gate716, $gate676)) {
      $draft | Should -Match "人間ゲートの合否:\n"
      $draft | Should -Not -Match "private input|alice|(?i)[a-z]:[\\/]"
    }
    $bytes = [System.IO.File]::ReadAllBytes((Join-Path $output "DEV-716.md"))
    ($bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB) | Should -BeFalse
  }

  It "rejects unsafe map identifiers and checkpoint text before writing a draft" {
    $root = Join-Path $TestDrive "unsafe"
    New-Item -ItemType Directory -Path $root -Force | Out-Null
    $summaryPath = Write-DraftTestSummary -Root $root
    $mapPath = Write-DraftTestJson -Path (Join-Path $root "map.json") -Value @{
      schemaVersion = 1
      gates = @(@{ source = "compat"; id = "C-001"; issueId = "DEV-716/path" })
    }
    { Invoke-VmVerifyLinearDraft -SummaryPath $summaryPath -GateMapPath $mapPath `
        -OutputDirectory (Join-Path $root "out") } | Should -Throw "*invalid entry*"
    Test-Path -LiteralPath (Join-Path $root "out") | Should -BeFalse

    $mapPath = Write-DraftTestJson -Path $mapPath -Value @{
      schemaVersion = 1
      gates = @(@{ source = "compat"; id = "C-001"; issueId = "DEV-716" })
    }
    { Invoke-VmVerifyLinearDraft -SummaryPath $summaryPath -GateMapPath $mapPath `
        -OutputDirectory (Join-Path $root "out") -CheckpointName "C:\Users\alice" } |
      Should -Throw "*Checkpoint name*"
    Test-Path -LiteralPath (Join-Path $root "out") | Should -BeFalse
  }

  It "rejects scalar and array JSON roots without combining their gates" {
    $root = Join-Path $TestDrive "root-shapes"
    New-Item -ItemType Directory -Path $root -Force | Out-Null
    $summaryPath = Write-DraftTestSummary -Root $root
    $mapPath = Join-Path $root "map.json"
    $entry = @{ schemaVersion = 1; gates = @(
        @{ source = "compat"; id = "C-001"; issueId = "DEV-716" }) }
    $invalidRoots = @(
      '"text"'
      (ConvertTo-Json -InputObject @($entry) -Depth 5 -Compress)
      (ConvertTo-Json -InputObject @($entry, $entry) -Depth 5 -Compress)
    )
    foreach ($json in $invalidRoots) {
      Set-Content -LiteralPath $mapPath -Value $json -Encoding utf8
      { Invoke-VmVerifyLinearDraft -SummaryPath $summaryPath -GateMapPath $mapPath `
          -OutputDirectory (Join-Path $root "out") } |
        Should -Throw "*root must be an object*"
      Test-Path -LiteralPath (Join-Path $root "out") | Should -BeFalse
    }
    $validMap = Write-DraftTestJson -Path $mapPath -Value $entry
    Set-Content -LiteralPath $summaryPath -Value (ConvertTo-Json -InputObject @(
        (Get-Content -Raw -LiteralPath $summaryPath | ConvertFrom-Json)) -Depth 10) -Encoding utf8
    { Invoke-VmVerifyLinearDraft -SummaryPath $summaryPath -GateMapPath $validMap `
        -OutputDirectory (Join-Path $root "out") } |
      Should -Throw "*root must be an object*"
  }

  It "excludes bootstrap statuses from a different package commit" {
    $root = Join-Path $TestDrive "mismatch"
    New-Item -ItemType Directory -Path $root -Force | Out-Null
    $summaryPath = Write-DraftTestSummary -Root $root
    $summary = Get-Content -Raw -LiteralPath $summaryPath | ConvertFrom-Json
    $summary.bootstrap.packageCommitVsManifest = "mismatch"
    $summary.bootstrap.checks += [pscustomobject]@{
      id = "inferenceHost"; status = "pass"; message = "unrelated package"
    }
    $null = Write-DraftTestJson -Path $summaryPath -Value $summary
    $mapPath = Write-DraftTestJson -Path (Join-Path $root "map.json") -Value @{
      schemaVersion = 1
      gates = @(
        @{ source = "bootstrap"; id = "vmCheckpoint"; issueId = "DEV-716" }
        @{ source = "bootstrap"; id = "inferenceHost"; issueId = "DEV-716" }
      )
    }
    $output = Join-Path $root "out"
    $null = Invoke-VmVerifyLinearDraft -SummaryPath $summaryPath -GateMapPath $mapPath `
      -OutputDirectory $output
    $draft = Get-Content -Raw -LiteralPath (Join-Path $output "DEV-716.md")
    $draft | Should -Match "\| bootstrap \| vmCheckpoint \|  \| excluded \|"
    $draft | Should -Match "\| bootstrap \| inferenceHost \|  \| excluded \|"
    $draft | Should -Not -Match "\| bootstrap \|[^\n]*\| (pass|manual_required) \|"
    $draft | Should -Not -Match "- bootstrap vmCheckpoint の人間確認:"
    $draft | Should -Match "commit は manifest と不一致"
  }

  It "refuses a rerun into a directory holding drafts from an earlier map" {
    $root = Join-Path $TestDrive "rerun"
    New-Item -ItemType Directory -Path $root -Force | Out-Null
    $summaryPath = Write-DraftTestSummary -Root $root
    $mapPath = Write-DraftTestJson -Path (Join-Path $root "map.json") -Value @{
      schemaVersion = 1
      gates = @(
        @{ source = "compat"; id = "C-001"; issueId = "DEV-716" }
        @{ source = "compat"; id = "C-010"; issueId = "DEV-676" }
      )
    }
    $output = Join-Path $root "out"
    $null = Invoke-VmVerifyLinearDraft -SummaryPath $summaryPath -GateMapPath $mapPath `
      -OutputDirectory $output
    $old716 = Get-Content -Raw -LiteralPath (Join-Path $output "DEV-716.md")
    $old676 = Get-Content -Raw -LiteralPath (Join-Path $output "DEV-676.md")
    $null = Write-DraftTestJson -Path $mapPath -Value @{
      schemaVersion = 1
      gates = @(@{ source = "compat"; id = "C-001"; issueId = "DEV-716" })
    }
    { Invoke-VmVerifyLinearDraft -SummaryPath $summaryPath -GateMapPath $mapPath `
        -OutputDirectory $output } | Should -Throw "*must be empty*"
    (Get-Content -Raw -LiteralPath (Join-Path $output "DEV-716.md")) | Should -BeExactly $old716
    (Get-Content -Raw -LiteralPath (Join-Path $output "DEV-676.md")) | Should -BeExactly $old676
  }

  It "does not expose absolute output paths in the script entry point" {
    $root = Join-Path $TestDrive "entry"
    New-Item -ItemType Directory -Path $root -Force | Out-Null
    $summaryPath = Write-DraftTestSummary -Root $root
    $mapPath = Write-DraftTestJson -Path (Join-Path $root "map.json") -Value @{
      schemaVersion = 1
      gates = @(@{ source = "compat"; id = "C-001"; issueId = "DEV-716" })
    }
    $lines = & $draftScript -SummaryPath $summaryPath -GateMapPath $mapPath `
      -OutputDirectory (Join-Path $root "out")
    ($lines -join "`n") | Should -Match "Draft: DEV-716.md"
    ($lines -join "`n") | Should -Not -Match "(?i)[a-z]:[\\/]"
  }
}
