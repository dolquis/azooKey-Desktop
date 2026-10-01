Describe "VM verification fixtures" {
  BeforeAll {
    $script:repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
    $script:fixtureRoot = Join-Path $script:repoRoot "scripts/vm-verify-fixtures"
    $script:dataRoot = Join-Path $script:fixtureRoot "data"
    $script:stubScript = Join-Path $script:fixtureRoot "ai-loopback-stub.ps1"
    $script:pathScript = Join-Path $script:fixtureRoot "new-non-ascii-path-fixture.ps1"
    . (Join-Path $script:repoRoot "scripts/vm-verify-linear-drafts.ps1")
    . $script:pathScript

    function Get-FixtureLine {
      param([string]$RelativePath)
      $text = [System.IO.File]::ReadAllText((Join-Path $script:dataRoot $RelativePath),
        [System.Text.UTF8Encoding]::new($false, $true))
      if ($text.Length -eq 0) { return @() }
      return @($text.TrimEnd("`n") -split "`n")
    }

    # core/src/CustomRomajiLoader.cpp の ParseLine と同じ判定。
    function Test-FixtureRomajiRule {
      param([string]$Line)
      $cells = $Line.Split("`t")
      if ($cells.Count -lt 2 -or $cells.Count -gt 3) { return $false }
      if ($cells[0] -cnotmatch '^[\x00-\x7F]{1,8}$' -or $cells[1].Length -gt 8) { return $false }
      if ($cells.Count -eq 3) {
        if ($cells[2] -cnotmatch '^[0-9]+$') { return $false }
        $consume = [int]$cells[2]
        if ($consume -lt 1 -or $consume -gt $cells[0].Length) { return $false }
      }
      return $true
    }

    function Get-FixtureRomajiRule {
      param([string]$RelativePath)
      return @(Get-FixtureLine $RelativePath | Where-Object { $_ -and -not $_.StartsWith("#") })
    }

    function Get-FixtureFreePort {
      $probe = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
      $probe.Start()
      try { return $probe.LocalEndpoint.Port } finally { $probe.Stop() }
    }

    function Invoke-FixtureStub {
      param([string]$Mode, [string]$LogPath, [string]$Body, [hashtable]$Headers)
      $port = Get-FixtureFreePort
      $stub = $script:stubScript
      $stubMode = $Mode
      $job = Start-Job -ScriptBlock {
        & $using:stub -Mode $using:stubMode -Port $using:port -RetryAfterSeconds 7 -DelaySeconds 1 `
          -MaxRequests 1 -LogPath $using:LogPath
      }
      try {
        $deadline = [DateTime]::UtcNow.AddSeconds(30)
        while (-not (Test-Path -LiteralPath $LogPath) -and [DateTime]::UtcNow -lt $deadline) {
          Start-Sleep -Milliseconds 100
        }
        Test-Path -LiteralPath $LogPath | Should -BeTrue
        $response = Invoke-WebRequest -Uri "http://127.0.0.1:$port/v1/chat/completions?probe=1" `
          -Method Post -Body $Body -Headers $Headers -ContentType "application/json" `
          -SkipHttpErrorCheck -TimeoutSec 20
        Wait-Job $job -Timeout 20 | Out-Null
        return $response
      } finally {
        Remove-Job $job -Force
      }
    }
  }

  Context "gate map" {
    BeforeAll {
      $script:gates = @(Get-VmVerifyDraftGate -Map (Get-VmVerifyDraftData `
            -Path (Join-Path $script:fixtureRoot "gate-map.json")))
    }

    It "passes the draft script's validation" {
      $script:gates.Count | Should -BeGreaterThan 0
      @($script:gates | Where-Object { $_.source -eq "bootstrap" }).Count | Should -BeGreaterThan 0
      @($script:gates | Where-Object { $_.source -eq "diag" }).Count | Should -BeGreaterThan 0
    }

    It "uses only check IDs that the implementations emit" {
      $bootstrap = Get-Content -Raw -LiteralPath (Join-Path $script:repoRoot "scripts/verify-bootstrap.ps1")
      $bootstrapIds = @([regex]::Matches($bootstrap, '-Id "([A-Za-z]+)"') |
          ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique)
      $diag = Get-Content -Raw -LiteralPath (Join-Path $script:repoRoot "diagnostics/Diagnostics.cpp")
      $diagIds = @([regex]::Matches($diag, 'AddCheck\(report, "(D-\d{3})"') |
          ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique)
      $compatIds = @(Get-ChildItem -LiteralPath (Join-Path $script:repoRoot "compat-test/cases") -Filter "C*.cpp" |
          Where-Object { $_.Name -match '^C(\d{3})_' } | ForEach-Object { "C-$($Matches[1])" })
      $bootstrapIds.Count | Should -BeGreaterThan 0
      $diagIds.Count | Should -BeGreaterThan 0
      $compatIds.Count | Should -BeGreaterThan 0

      foreach ($gate in $script:gates) {
        $known = switch ($gate.source) {
          "bootstrap" { $bootstrapIds }
          "diag" { $diagIds }
          "compat" { $compatIds }
        }
        $known | Should -Contain $gate.id -Because "$($gate.source) $($gate.id) → $($gate.issueId)"
      }
    }

    It "lists every mapped bootstrap and diag pair in the README rationale" {
      $readme = Get-Content -Raw -LiteralPath (Join-Path $script:fixtureRoot "README.md")
      foreach ($gate in @($script:gates | Where-Object { $_.source -ne "compat" })) {
        $pattern = '(?m)^\|[^|]*`' + [regex]::Escape($gate.id) + '`[^|]*\| ' + $gate.issueId + ' \|'
        $readme | Should -Match $pattern -Because "$($gate.id) → $($gate.issueId) needs a stated reason"
      }
    }
  }

  Context "data files" {
    It "keeps every data file LF-only and without a BOM" {
      $files = @(Get-ChildItem -LiteralPath $script:dataRoot -Recurse -File)
      $files.Count | Should -BeGreaterThan 0
      foreach ($file in $files) {
        $bytes = [System.IO.File]::ReadAllBytes($file.FullName)
        ($bytes -contains 13) | Should -BeFalse -Because "$($file.Name) must not contain CR"
        ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) |
          Should -BeFalse -Because "$($file.Name) must not start with a BOM"
        if ($bytes.Length -gt 0) { $bytes[-1] | Should -Be 10 -Because "$($file.Name) must end with LF" }
      }
    }

    It "has custom romaji tables that load or fall back as documented" {
      foreach ($name in "valid.tsv", "valid-updated.tsv") {
        $rules = Get-FixtureRomajiRule "custom-romaji/$name"
        $rules.Count | Should -BeGreaterThan 0
        foreach ($rule in $rules) { Test-FixtureRomajiRule $rule | Should -BeTrue -Because $rule }
      }
      (Get-FixtureRomajiRule "custom-romaji/valid.tsv") | Should -Contain "ka`tカ"
      (Get-FixtureRomajiRule "custom-romaji/valid-updated.tsv") | Should -Contain "ka`tｶ"

      (Get-Item -LiteralPath (Join-Path $script:dataRoot "custom-romaji/empty.tsv")).Length | Should -Be 0
      $invalid = Get-FixtureRomajiRule "custom-romaji/invalid-only.tsv"
      $invalid.Count | Should -BeGreaterThan 0
      foreach ($rule in $invalid) { Test-FixtureRomajiRule $rule | Should -BeFalse -Because $rule }
    }

    It "has an emoji import file whose surfaces are surrogate pairs" {
      $rows = Get-FixtureLine "user-dict/emoji.tsv"
      $rows.Count | Should -Be 3
      foreach ($row in $rows) {
        $cells = $row.Split("`t")
        $cells.Count | Should -Be 2
        $cells[0] | Should -Match '^\p{IsHiragana}+$'
        [char]::IsHighSurrogate($cells[1][0]) | Should -BeTrue
      }
    }

    It "has plaintext stores in the formats the Host migrates" {
      $learning = Get-FixtureLine "plaintext-stores/learning.tsv"
      $learning[0] | Should -BeExactly "# azookey-learning-tsv escaped=1"
      $learning.Count | Should -Be 4
      foreach ($row in $learning[1..3]) { $row | Should -Match "^[^\t]+\t[^\t]+\t\d+(\.\d+)? \d+$" }

      $typo = Get-FixtureLine "plaintext-stores/typo_corrections.tsv"
      $typo[0] | Should -BeExactly "# azookey-typo-correction-tsv escaped=1"
      $typo.Count | Should -Be 3
      foreach ($row in $typo[1..2]) { $row | Should -Match "^[^\t]+\t[^\t]+\t[1-9]\d* \d+$" }

      $auto = Get-FixtureLine "plaintext-stores/auto_words.tsv"
      $auto[0] | Should -BeExactly "# azookey-auto-word-store v1"
      $auto[1] | Should -Match "^# surface\t"
      $auto.Count | Should -Be 4
      foreach ($row in $auto[2..3]) {
        $row | Should -Match "^[^\t]+\t[^\t]+\t(mining|trending)\t(confirmed|rejected)\t\d+\t\d+\t\d+\t\d+(\.\d+)?$"
      }

      $dict = Get-Content -Raw -LiteralPath (Join-Path $script:dataRoot "plaintext-stores/user_dict.json") |
        ConvertFrom-Json
      $dict.version | Should -Be 1
      @($dict.entries).Count | Should -Be 1
      $dict.entries[0].word | Should -Not -BeNullOrEmpty
      $dict.entries[0].ruby | Should -Not -BeNullOrEmpty

      # 0 バイトの平文ストアは移行できない（DEV-1460）。
      foreach ($file in Get-ChildItem -LiteralPath (Join-Path $script:dataRoot "plaintext-stores") -File) {
        $file.Length | Should -BeGreaterThan 0
      }
    }

    It "has ASCII stdio lines that start with a handshake declaring secure_flag" {
      $handshake = @(Get-FixtureLine "stdio/handshake.jsonl")
      $handshake.Count | Should -Be 1
      $envelope = $handshake[0] | ConvertFrom-Json
      $envelope.type | Should -Be "Handshake"
      $envelope.version | Should -Be 1
      $envelope.payload.protocol_version | Should -Be 1
      $envelope.payload.capabilities | Should -Contain "secure_flag"

      $commits = @(Get-FixtureLine "stdio/commit-single.jsonl") + @(Get-FixtureLine "stdio/commit-burst.jsonl")
      $commits.Count | Should -Be 21
      $parsed = @($commits | ForEach-Object {
          $_ | Should -Match '^[\x20-\x7E]+$'
          $_ | ConvertFrom-Json
        })
      foreach ($commit in $parsed) {
        $commit.type | Should -Be "CommitObservation"
        $commit.payload.secure | Should -BeFalse
        $commit.payload.learning_allowed | Should -BeTrue
        $commit.payload.chosen.reading | Should -Be $commit.payload.reading
      }
      @($parsed.request_id | Sort-Object -Unique).Count | Should -Be 21
      @($parsed.payload.observation_id | Sort-Object -Unique).Count | Should -Be 21
      @($parsed.payload.reading | Sort-Object -Unique).Count | Should -Be 21
    }
  }

  Context "guest scripts" {
    It "are ASCII-only so that Windows PowerShell 5.1 reads them unchanged" {
      foreach ($path in $script:stubScript, $script:pathScript) {
        $bytes = [System.IO.File]::ReadAllBytes($path)
        @($bytes | Where-Object { $_ -gt 127 }).Count | Should -Be 0 -Because (Split-Path -Leaf $path)
      }
    }

    It "creates one CP932 set and one set that CP932 cannot represent" {
      $model = Join-Path $TestDrive "model.gguf"
      [System.IO.File]::WriteAllBytes($model, [byte[]](1..64))
      $root = Join-Path $TestDrive "non-ascii"
      $result = Write-NonAsciiPathFixture -Root $root -ModelPath $model

      @($result.sets).Count | Should -Be 2
      ($result.sets | Where-Object name -eq "cp932").representableInCp932 | Should -BeTrue
      ($result.sets | Where-Object name -eq "unicode").representableInCp932 | Should -BeFalse
      foreach ($set in $result.sets) {
        (Split-Path -Leaf $set.directory) | Should -Match '[^\x00-\x7F]'
        Test-Path -LiteralPath $set.userDictPath | Should -BeFalse
        $bytes = [System.IO.File]::ReadAllBytes($set.learningPath)
        $bytes.Length | Should -BeGreaterThan 0
        ($bytes -contains 13) | Should -BeFalse
        [System.Text.Encoding]::UTF8.GetString($bytes) |
          Should -Match "^# azookey-learning-tsv escaped=1\n[^\t\n]+\t[^\t\n]+\t1 \d+\n$"
        (Get-FileHash -LiteralPath $set.modelPath -Algorithm SHA256).Hash | Should -Be $result.modelSha256
      }
      ($result.sets | Where-Object name -eq "unicode").directoryCodePoints | Should -Match "U\+20BB7"

      $saved = Get-Content -Raw -Encoding UTF8 -LiteralPath (Join-Path $root "paths.json") | ConvertFrom-Json
      $saved.sets[1].learningPath | Should -Be $result.sets[1].learningPath
      { Write-NonAsciiPathFixture -Root $root } | Should -Throw "*must be empty*"
    }

    It "answers each stub mode without echoing or logging the request" {
      $marker = "secret-" + [guid]::NewGuid().ToString("N")
      $headers = @{ Authorization = "Bearer key-$marker" }
      $expected = @{ auth = 401; ratelimit = 429; delay = 504 }
      foreach ($mode in "auth", "ratelimit", "delay") {
        $log = Join-Path $TestDrive "stub-$mode.log"
        $response = Invoke-FixtureStub -Mode $mode -LogPath $log -Headers $headers `
          -Body "{`"input`":`"$marker`"}"

        $response.StatusCode | Should -Be $expected[$mode]
        if ($mode -eq "ratelimit") {
          [string]$response.Headers["Retry-After"] | Should -Be "7"
        } else {
          $response.Headers.ContainsKey("Retry-After") | Should -BeFalse
        }
        $response.Content | Should -Not -Match $marker
        $logText = Get-Content -Raw -LiteralPath $log
        $logText | Should -Match "POST /v1/chat/completions body=\d+ sent=$($expected[$mode])"
        $logText | Should -Not -Match "$marker|probe=1|Bearer"
      }
    }
  }
}
