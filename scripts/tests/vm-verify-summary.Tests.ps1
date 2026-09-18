Describe "VM verification summary" {
  BeforeAll {
    $repoRoot = (Resolve-Path (Join-Path (Join-Path $PSScriptRoot "..") "..")).Path
    $summaryScript = Join-Path $repoRoot "scripts\vm-verify-summary.ps1"
    . $summaryScript

    $script:testCommit = "0123456789abcdef0123456789abcdef01234567"

    function Write-TestJson {
      param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [Parameter(Mandatory = $true)]
        $Value
      )

      $Value | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $Path -Encoding UTF8
      return $Path
    }

    function Initialize-TestManifest {
      param(
        [Parameter(Mandatory = $true)]
        [string]$Root,
        [string]$Commit = $script:testCommit,
        [switch]$WithModel
      )

      $files = @(
        @{ path = "azookey_tsf_tip.dll"; role = "tip-dll"; size = 1; sha256 = ("a" * 64) }
      )
      if ($WithModel) {
        $files += @{ path = "models/zenz-v3.1-small.gguf"; role = "gguf-model"; size = 1; sha256 = ("b" * 64) }
      }
      return Write-TestJson -Path (Join-Path $Root "manifest.json") -Value @{
        schemaVersion = 1
        commit = $Commit
        preset = "windows-release"
        buildType = "Release"
        generatedAtUtc = "2026-01-01T00:00:00.0000000+00:00"
        files = $files
      }
    }

    function Initialize-TestBootstrap {
      param(
        [Parameter(Mandatory = $true)]
        [string]$Root,
        [string]$Commit = $script:testCommit
      )

      return Write-TestJson -Path (Join-Path $Root "bootstrap.json") -Value ([ordered]@{
          schemaVersion = 1
          generatedAtUtc = "2026-01-01T00:00:00.0000000+00:00"
          package = @{ commit = $Commit; preset = "windows-release"; buildType = "Release" }
          overallStatus = "fail"
          actions = @{ vcRuntimeInstalled = $false; tipRegistered = $true; hostSupervisorStarted = $true }
          hostBinary = [ordered]@{
            status = "matched"
            reason = "ok"
            processId = 4242
            runningPath = "C:\Users\alice\azookey-verify\azookey_inference_host.exe"
            runningSha256 = ("C" * 64)
            expectedPath = "C:\Users\alice\azookey-verify\azookey_inference_host.exe"
            expectedSha256 = ("c" * 64)
          }
          checks = @(
            @{ id = "vcRuntime"; status = "pass"; message = "VC++ x64 runtime is already available." }
            @{ id = "tipRegistration"; status = "fail"; message = "Failed at C:\Users\alice\azookey-verify\register-dev.ps1`nline 2 of log" }
            @{ id = "inferenceHost"; status = "pass"; message = "Host at \\fileserver\share\alice is serving." }
            @{ id = "profileQuoted"; status = "pass"; message = "Cannot find path 'C:\Users\John Smith\AppData\Local\x.dll' because it does not exist." }
            @{ id = "profileBare"; status = "pass"; message = "Stale host C:\Users\Mary Ann\azookey\host.exe stopped; also saw \Users\zqxuser\x and rel\carol\y" }
            @{ id = "microsoftIme"; status = "pass"; message = "Microsoft Japanese IME remains available for recovery." }
            @{ id = "vmCheckpoint"; status = "manual_required"; message = "Confirm the Hyper-V checkpoint." }
            @{ id = "debugView"; status = "not_applicable"; message = "DebugView setup is only required for Debug verification." }
          )
        })
    }

    function Initialize-TestDiag {
      param(
        [Parameter(Mandatory = $true)]
        [string]$Root
      )

      return Write-TestJson -Path (Join-Path $Root "diag.json") -Value ([ordered]@{
          checks = @(
            @{ id = "D-001"; name = "tip_dll"; status = "ok"; message = "TIP DLL is present"; details = @{} }
            @{ id = "D-007"; name = "model_path"; status = "warning"; message = "Model is enabled but no model is selected"; details = @{ path = "C:\Users\alice\models\secret.gguf" } }
            @{ id = "D-004"; name = "host_process"; status = "error"; message = "Host process is not running"; details = @{} }
          )
          status = "error"
          timestamp_ms = 1
        })
    }

    function Initialize-TestCompat {
      param(
        [Parameter(Mandatory = $true)]
        [string]$Root,
        [string]$Target = "notepad"
      )

      $directory = Join-Path $Root "compat-$Target"
      New-Item -ItemType Directory -Path $directory -Force | Out-Null
      return Write-TestJson -Path (Join-Path $directory "report.json") -Value ([ordered]@{
          schema_version = 1
          target = @{ id = $Target; display_name = "Notepad"; app_id = "notepad.exe"; automation_level = "full" }
          # 意図的に results と食い違わせ、数え直していることを確かめる。
          summary = @{ pass = 3; fail = 0; failing_skip = 0 }
          results = @(
            @{ id = "C-001"; status = "pass"; reason_code = "none"; duration_ms = 10 }
            @{ id = "C-002"; status = "fail"; reason_code = "candidate-missing"; duration_ms = 20; artifact = "failures/notepad_C-002_fail" }
            @{ id = "C-003"; status = "failing-skip"; reason_code = "C:\Users\alice\trace.log"; duration_ms = 0; artifact = "C:/Users/alice/failures/x" }
          )
        })
    }
  }

  It "aggregates all inputs into distinct counts without rounding to pass" {
    $root = Join-Path $TestDrive "full"
    New-Item -ItemType Directory -Path $root -Force | Out-Null

    $summary = Get-VmVerifySummary `
      -ManifestPath (Initialize-TestManifest -Root $root -WithModel) `
      -BootstrapJsonPath (Initialize-TestBootstrap -Root $root) `
      -DiagJsonPath (Initialize-TestDiag -Root $root) `
      -CompatReportPath @(Initialize-TestCompat -Root $root) `
      -OsBuild "26100.4061"

    $summary.package.commit | Should -Be $script:testCommit
    $summary.package.bundledModel | Should -Be "zenz-v3.1-small.gguf"
    $summary.package.generatedAtUtc | Should -Be "2026-01-01T00:00:00.0000000Z"
    $summary.os.build | Should -Be "26100.4061"
    $summary.missingInputs.Count | Should -Be 0

    $summary.bootstrap.counts.pass | Should -Be 5
    $summary.bootstrap.counts.fail | Should -Be 1
    $summary.bootstrap.counts.manualRequired | Should -Be 1
    $summary.bootstrap.counts.notApplicable | Should -Be 1
    $summary.bootstrap.packageCommitVsManifest | Should -Be "match"

    $summary.diag.counts.pass | Should -Be 1
    $summary.diag.counts.warning | Should -Be 1
    $summary.diag.counts.fail | Should -Be 1

    $summary.compat[0].counts.pass | Should -Be 1
    $summary.compat[0].counts.fail | Should -Be 1
    $summary.compat[0].counts.failingSkip | Should -Be 1
    $summary.compat[0].reportedSummaryMatches | Should -BeFalse

    $summary.counts.pass | Should -Be 7
    $summary.counts.fail | Should -Be 3
    $summary.counts.failingSkip | Should -Be 1
    $summary.counts.manualRequired | Should -Be 1
    $summary.counts.warning | Should -Be 1
    $summary.counts.notApplicable | Should -Be 1
    $summary.counts.unrecognized | Should -Be 0
    $summary.counts.missingInputs | Should -Be 0
  }

  It "records each absent input as missing instead of omitting it" {
    $root = Join-Path $TestDrive "missing"
    New-Item -ItemType Directory -Path $root -Force | Out-Null
    $manifest = Initialize-TestManifest -Root $root

    $summary = Get-VmVerifySummary `
      -ManifestPath $manifest `
      -BootstrapJsonPath (Join-Path $root "absent-bootstrap.json")

    $summary.missingInputs | Should -Be @("osBuild", "bootstrap", "diag", "compat")
    $summary.counts.missingInputs | Should -Be 4
    $summary.counts.pass | Should -Be 0
    $summary.bootstrap | Should -BeNullOrEmpty
    ($summary.inputs | Where-Object { $_.name -eq "bootstrap" }).reason | Should -Be "file not found"
    ($summary.inputs | Where-Object { $_.name -eq "diag" }).reason | Should -Be "not provided"

    $markdown = ConvertTo-VmVerifySummaryMarkdown -Summary $summary
    $markdown | Should -Match "OS build \(``winver``\): （未取得）"
    $markdown | Should -Match "\| diag \(``azookey_diag.exe --json``\) \| 未取得 \(not provided\) \| - \|"
    $markdown | Should -Match "欠落した入力: osBuild \(missing: not provided\), bootstrap \(missing: file not found\)"
  }

  It "marks unreadable JSON and unknown statuses separately" {
    $root = Join-Path $TestDrive "invalid"
    New-Item -ItemType Directory -Path $root -Force | Out-Null
    $badDiag = Join-Path $root "diag.json"
    "not json {" | Set-Content -LiteralPath $badDiag -Encoding UTF8
    $bootstrap = Write-TestJson -Path (Join-Path $root "bootstrap.json") -Value @{
      overallStatus = "pass"
      checks = @(@{ id = "vcRuntime"; status = "skipped"; message = "?" })
    }

    $summary = Get-VmVerifySummary `
      -ManifestPath (Initialize-TestManifest -Root $root) `
      -BootstrapJsonPath $bootstrap `
      -DiagJsonPath $badDiag `
      -OsBuild "not-a-build"

    ($summary.inputs | Where-Object { $_.name -eq "diag" }).state | Should -Be "invalid"
    ($summary.inputs | Where-Object { $_.name -eq "osBuild" }).state | Should -Be "invalid"
    $summary.os.build | Should -Be ""
    $summary.bootstrap.counts.unrecognized | Should -Be 1
    $summary.bootstrap.counts.pass | Should -Be 0
    $summary.bootstrap.packageCommitVsManifest | Should -Be "unknown"
  }

  It "treats JSON of an unexpected shape as invalid instead of an empty present input" {
    $root = Join-Path $TestDrive "shape"
    New-Item -ItemType Directory -Path $root -Force | Out-Null
    $noResults = Write-TestJson -Path (Join-Path $root "compat.json") -Value @{
      schema_version = 1
      target = @{ id = "notepad" }
    }
    $wrapped = Join-Path $root "wrapped.json"
    '[{"status":"ok","checks":[]}]' | Set-Content -LiteralPath $wrapped -Encoding UTF8
    $crossed = Initialize-TestDiag -Root $root

    $summary = Get-VmVerifySummary `
      -ManifestPath (Initialize-TestManifest -Root $root) `
      -BootstrapJsonPath $crossed `
      -DiagJsonPath $wrapped `
      -CompatReportPath @($noResults)

    foreach ($name in @("bootstrap", "diag", "compat[0]")) {
      ($summary.inputs | Where-Object { $_.name -eq $name }).state | Should -Be "invalid"
    }
    ($summary.inputs | Where-Object { $_.name -eq "bootstrap" }).reason | Should -Be "unexpected shape"
    ($summary.inputs | Where-Object { $_.name -eq "diag" }).reason | Should -Be "not a JSON object"
    $summary.missingInputs | Should -Contain "compat[0]"
    @($summary.compat).Count | Should -Be 0
  }

  It "flags a bootstrap result taken from a different package commit" {
    $root = Join-Path $TestDrive "mismatch"
    New-Item -ItemType Directory -Path $root -Force | Out-Null

    $summary = Get-VmVerifySummary `
      -ManifestPath (Initialize-TestManifest -Root $root) `
      -BootstrapJsonPath (Initialize-TestBootstrap -Root $root -Commit ("f" * 40))

    $summary.bootstrap.packageCommitVsManifest | Should -Be "mismatch"
    $summary.bootstrap.counts.fail | Should -Be 1
    $summary.counts.pass | Should -Be 0
    $summary.counts.fail | Should -Be 0
    $markdown = ConvertTo-VmVerifySummaryMarkdown -Summary $summary
    $markdown | Should -Match "\*\*不一致\*\*"
    $markdown | Should -Match "commit 不一致のため合計に含めない"
  }

  It "keeps absolute paths, user names and log bodies out of both outputs" {
    $root = Join-Path $TestDrive "redaction"
    New-Item -ItemType Directory -Path $root -Force | Out-Null
    $output = Join-Path $root "out"

    $result = Invoke-VmVerifySummary `
      -ManifestPath (Initialize-TestManifest -Root $root) `
      -BootstrapJsonPath (Initialize-TestBootstrap -Root $root) `
      -DiagJsonPath (Initialize-TestDiag -Root $root) `
      -CompatReportPath @(Initialize-TestCompat -Root $root) `
      -OsBuild "26100" `
      -OutputDirectory $output

    foreach ($path in @($result.JsonPath, $result.MarkdownPath)) {
      $text = [System.IO.File]::ReadAllText($path)
      $text | Should -Not -Match "alice"
      $text | Should -Not -Match "(?i)[a-z]:[\\/]"
      $text | Should -Not -Match "fileserver"
      $text | Should -Not -MatchExactly "Smith|Mary|zqxuser|carol"
      $text | Should -Not -Match "secret\.gguf"
      $text | Should -Not -Match "processId"
    }

    $summary = $result.Summary
    ($summary.bootstrap.checks | Where-Object { $_.id -eq "tipRegistration" }).message |
      Should -Be "Failed at <path>"
    ($summary.bootstrap.checks | Where-Object { $_.id -eq "profileQuoted" }).message |
      Should -Be "Cannot find path '<path>' because it does not exist."
    $summary.bootstrap.hostBinary.runningSha256 | Should -Be ("c" * 64)
    $summary.compat[0].results[1].artifact | Should -Be "failures/notepad_C-002_fail"
    $summary.compat[0].results[2].reasonCode | Should -Be "redacted"
    $summary.compat[0].results[2].artifact | Should -Be ""
  }

  It "writes UTF-8 without BOM and carries no pass/fail verdict" {
    $root = Join-Path $TestDrive "files"
    New-Item -ItemType Directory -Path $root -Force | Out-Null

    $result = Invoke-VmVerifySummary `
      -ManifestPath (Initialize-TestManifest -Root $root) `
      -CompatReportPath @(Initialize-TestCompat -Root $root -Target "notepad"; Initialize-TestCompat -Root $root -Target "vscode") `
      -OsBuild "26100.1" `
      -OutputDirectory (Join-Path $root "out")

    $bytes = [System.IO.File]::ReadAllBytes($result.JsonPath)
    ($bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB) | Should -BeFalse

    $json = Get-Content -Raw -LiteralPath $result.JsonPath | ConvertFrom-Json
    $json.schemaVersion | Should -Be 1
    $json.notice | Should -Match "does not decide any human gate"
    @($json.compat).Count | Should -Be 2
    $json.counts.failingSkip | Should -Be 2
    $json.PSObject.Properties.Name | Should -Not -Contain "overallStatus"
    $json.PSObject.Properties.Name | Should -Not -Contain "outcome"

    $markdown = Get-Content -Raw -LiteralPath $result.MarkdownPath
    $markdown | Should -Match "## 検証環境"
    $markdown | Should -Match "- 検証日 / 検証者:\n"
    $markdown | Should -Match "人間ゲートの合否を表さない"
    $markdown | Should -Match "\| compat ``vscode``（report.json の summary と results が食い違う） \| 取得 \| 1 \| 1 \| 1 \|"
    $markdown | Should -Not -Match "Outcome"
  }

  It "rejects a manifest without a full commit hash" {
    $root = Join-Path $TestDrive "bad-manifest"
    New-Item -ItemType Directory -Path $root -Force | Out-Null
    $manifest = Initialize-TestManifest -Root $root -Commit "abc123"

    { Get-VmVerifySummary -ManifestPath $manifest } | Should -Throw "*full commit hash*"
  }

  It "runs as a script entry point" {
    $root = Join-Path $TestDrive "entry"
    New-Item -ItemType Directory -Path $root -Force | Out-Null
    $output = Join-Path $root "out"

    $lines = & $summaryScript -ManifestPath (Initialize-TestManifest -Root $root) -OsBuild "26100" -OutputDirectory $output

    Test-Path -LiteralPath (Join-Path $output "verification-summary.json") | Should -BeTrue
    Test-Path -LiteralPath (Join-Path $output "verification-summary.md") | Should -BeTrue
    ($lines -join "`n") | Should -Match "Missing inputs: bootstrap, diag, compat"
  }
}
