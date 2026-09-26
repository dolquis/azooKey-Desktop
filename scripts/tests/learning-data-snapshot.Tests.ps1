Describe "Learning data snapshot" {
  BeforeAll {
    $repoRoot = (Resolve-Path (Join-Path (Join-Path $PSScriptRoot "..") "..")).Path
    $script:snapshotScript = Join-Path $repoRoot "scripts\learning-data-snapshot.ps1"
    . $script:snapshotScript

    # 出力へ漏れてはならない本文。GUID と日本語の両方を置く。
    $script:secretMarker = "secret-" + [guid]::NewGuid().ToString("N")
    $script:secretJapanese = "ひみつのにゅうりょく"

    function Write-TestFile {
      param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [AllowEmptyString()]
        [string]$Text
      )

      $parent = Split-Path -Parent $Path
      New-Item -ItemType Directory -Path $parent -Force | Out-Null
      [System.IO.File]::WriteAllText($Path, $Text, (New-Object System.Text.UTF8Encoding($false)))
    }

    function Initialize-TestDataDirectory {
      param(
        [Parameter(Mandatory = $true)]
        [string]$Root
      )

      $data = Join-Path $Root "data"
      Write-TestFile -Path (Join-Path $data "learning.tsv") `
        -Text "$($script:secretJapanese)`t$($script:secretMarker)`n2`t3`n"
      Write-TestFile -Path (Join-Path $data "user_dict.json.enc") -Text "ciphertext $($script:secretMarker)`n`n"
      Write-TestFile -Path (Join-Path $data "user_dict.json.bak") -Text "{`"$($script:secretMarker)`": 1}"
      return $data
    }

    function Get-TestFileState {
      param(
        [Parameter(Mandatory = $true)]
        [string]$Directory
      )

      return @(Get-ChildItem -LiteralPath $Directory -File -Recurse -Force | Sort-Object FullName | ForEach-Object {
          "{0}|{1}|{2}" -f $_.FullName, $_.LastWriteTimeUtc.Ticks,
            (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
        })
    }

    function Get-TestRecord {
      param(
        [Parameter(Mandatory = $true)]
        $Snapshot,
        [Parameter(Mandatory = $true)]
        [string]$Path
      )

      return @($Snapshot.files | Where-Object { $_.path -eq $Path })[0]
    }
  }

  BeforeEach {
    # TestDrive は Context 内の It で共有されるので、テストごとに別のルートを使う。
    $root = Join-Path $TestDrive ([guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Path $root | Out-Null
  }

  Context "recording" {
    It "records hashes, sizes, and line counts without file contents or absolute paths" {
      $data = Initialize-TestDataDirectory -Root $root
      $output = Join-Path $root "out\snapshots.json"

      $result = Add-LearningSnapshot -SnapshotLabel "before-gate" -DataDirectory $data -OutputPath $output

      $text = [System.IO.File]::ReadAllText($output)
      $text | Should -Not -Match ([regex]::Escape($script:secretMarker))
      $text | Should -Not -Match ([regex]::Escape($script:secretJapanese))
      $text | Should -Not -Match ([regex]::Escape($root))
      $text | Should -Not -Match ([regex]::Escape($root.Replace("\", "\\")))
      ($result | ConvertTo-Json -Depth 8) | Should -Not -Match ([regex]::Escape($script:secretMarker))

      $document = $text | ConvertFrom-Json
      $document.kind | Should -BeExactly "azookey-learning-data-snapshots"
      $document.schemaVersion | Should -Be 1
      $snapshot = @($document.snapshots)[0]
      $snapshot.label | Should -BeExactly "before-gate"
      $snapshot.dataDirectory | Should -BeExactly "custom"
      $snapshot.dataDirectoryExists | Should -BeTrue

      $learning = Get-TestRecord -Snapshot $snapshot -Path "learning.tsv"
      $learning.exists | Should -BeTrue
      $learning.encrypted | Should -BeFalse
      $learning.lineCount | Should -Be 2
      $learning.size | Should -Be (Get-Item -LiteralPath (Join-Path $data "learning.tsv")).Length
      $learning.sha256 | Should -BeExactly (
        Get-FileHash -LiteralPath (Join-Path $data "learning.tsv") -Algorithm SHA256).Hash.ToLowerInvariant()
    }

    It "hashes encrypted stores without a line count and records backups" {
      $data = Initialize-TestDataDirectory -Root $root
      $output = Join-Path $root "snapshots.json"

      $snapshot = (Add-LearningSnapshot -SnapshotLabel "a" -DataDirectory $data -OutputPath $output).Snapshot

      $encrypted = Get-TestRecord -Snapshot $snapshot -Path "user_dict.json.enc"
      $encrypted.exists | Should -BeTrue
      $encrypted.encrypted | Should -BeTrue
      $encrypted.lineCount | Should -BeNullOrEmpty
      $encrypted.sha256 | Should -Match "^[0-9a-f]{64}$"
      (Get-TestRecord -Snapshot $snapshot -Path "user_dict.json.bak").exists | Should -BeTrue
    }

    It "lists every known store, including missing ones" {
      $data = Initialize-TestDataDirectory -Root $root
      $output = Join-Path $root "snapshots.json"

      $snapshot = (Add-LearningSnapshot -SnapshotLabel "a" -DataDirectory $data -OutputPath $output).Snapshot

      foreach ($store in @("learning.tsv", "user_dict.json", "typo_corrections.tsv", "auto_words.tsv")) {
        @($snapshot.files | Where-Object { $_.path -eq $store }).Count | Should -Be 1
        @($snapshot.files | Where-Object { $_.path -eq "$store.enc" }).Count | Should -Be 1
      }
      $missing = Get-TestRecord -Snapshot $snapshot -Path "auto_words.tsv"
      $missing.exists | Should -BeFalse
      $missing.sha256 | Should -BeNullOrEmpty
    }

    It "records a missing data directory as absent stores" {
      $output = Join-Path $root "snapshots.json"

      $snapshot = (Add-LearningSnapshot -SnapshotLabel "fresh" `
          -DataDirectory (Join-Path $root "no-such-dir") -OutputPath $output).Snapshot

      $snapshot.dataDirectoryExists | Should -BeFalse
      @($snapshot.files | Where-Object { $_.exists }).Count | Should -Be 0
    }

    It "does not modify or add files in the data directory" {
      $data = Initialize-TestDataDirectory -Root $root
      $before = Get-TestFileState -Directory $data

      Add-LearningSnapshot -SnapshotLabel "a" -DataDirectory $data -OutputPath (Join-Path $root "s.json") |
        Out-Null

      Get-TestFileState -Directory $data | Should -Be $before
    }

    It "reads a store that another process holds open for writing" {
      $data = Initialize-TestDataDirectory -Root $root
      $held = [System.IO.File]::Open((Join-Path $data "learning.tsv"), [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::ReadWrite)
      try {
        $snapshot = (Add-LearningSnapshot -SnapshotLabel "a" -DataDirectory $data `
            -OutputPath (Join-Path $root "s.json")).Snapshot
      } finally {
        $held.Dispose()
      }

      (Get-TestRecord -Snapshot $snapshot -Path "learning.tsv").exists | Should -BeTrue
    }

    It "counts lines as LF plus an unterminated last line" -ForEach @(
      @{ Text = ""; Expected = 0 }
      @{ Text = "a"; Expected = 1 }
      @{ Text = "a`n"; Expected = 1 }
      @{ Text = "a`nb"; Expected = 2 }
      @{ Text = "a`r`nb`r`n"; Expected = 2 }
      @{ Text = "`n`n"; Expected = 2 }
    ) {
      $path = Join-Path $root "lines.tsv"
      Write-TestFile -Path $path -Text $Text

      (Get-LearningSnapshotContentDigest -Path $path).LineCount | Should -Be $Expected
    }

    It "replaces file names outside the token pattern with a stable stand-in" {
      $data = Initialize-TestDataDirectory -Root $root
      Write-TestFile -Path (Join-Path $data "$($script:secretJapanese).txt") -Text "x"
      $output = Join-Path $root "s.json"

      Add-LearningSnapshot -SnapshotLabel "a" -DataDirectory $data -OutputPath $output | Out-Null
      Add-LearningSnapshot -SnapshotLabel "b" -DataDirectory $data -OutputPath $output | Out-Null

      $text = [System.IO.File]::ReadAllText($output)
      $text | Should -Not -Match ([regex]::Escape($script:secretJapanese))
      $text | Should -Match "unrecognized-name-[0-9a-f]{12}"
      (Compare-LearningSnapshot -FromLabel "a" -ToLabel "b" -OutputPath $output).changed | Should -BeFalse
    }

    It "records nested files with forward-slash relative paths" {
      $data = Initialize-TestDataDirectory -Root $root
      Write-TestFile -Path (Join-Path (Join-Path $data "sub") "extra.tsv") -Text "x`n"

      $snapshot = (Add-LearningSnapshot -SnapshotLabel "a" -DataDirectory $data `
          -OutputPath (Join-Path $root "s.json")).Snapshot

      (Get-TestRecord -Snapshot $snapshot -Path "sub/extra.tsv").exists | Should -BeTrue
    }

    It "matches known stores regardless of case" {
      $data = Join-Path $root "data"
      Write-TestFile -Path (Join-Path $data "Learning.TSV") -Text "a`n"

      $snapshot = (Add-LearningSnapshot -SnapshotLabel "a" -DataDirectory $data `
          -OutputPath (Join-Path $root "s.json")).Snapshot

      @($snapshot.files | Where-Object { $_.path -eq "learning.tsv" }).Count | Should -Be 1
      (Get-TestRecord -Snapshot $snapshot -Path "learning.tsv").exists | Should -BeTrue
    }

    It "rejects invalid and duplicate labels" {
      $data = Initialize-TestDataDirectory -Root $root
      $output = Join-Path $root "s.json"

      { Add-LearningSnapshot -SnapshotLabel "has space" -DataDirectory $data -OutputPath $output } |
        Should -Throw "*-Label must match*"
      Add-LearningSnapshot -SnapshotLabel "a" -DataDirectory $data -OutputPath $output | Out-Null
      { Add-LearningSnapshot -SnapshotLabel "a" -DataDirectory $data -OutputPath $output } |
        Should -Throw "*already recorded*"
      @(([System.IO.File]::ReadAllText($output) | ConvertFrom-Json).snapshots).Count | Should -Be 1
    }

    It "rejects an output path inside the data directory" {
      $data = Initialize-TestDataDirectory -Root $root

      { Add-LearningSnapshot -SnapshotLabel "a" -DataDirectory $data -OutputPath (Join-Path $data "s.json") } |
        Should -Throw "*must not be inside*"
      Test-Path -LiteralPath (Join-Path $data "s.json") | Should -BeFalse
    }

    It "refuses to append to a file that is not a snapshot file" {
      $data = Initialize-TestDataDirectory -Root $root
      $output = Join-Path $root "other.json"
      '{"kind":"something-else","schemaVersion":1}' | Set-Content -LiteralPath $output

      { Add-LearningSnapshot -SnapshotLabel "a" -DataDirectory $data -OutputPath $output } |
        Should -Throw "*not a learning data snapshot file*"
    }
  }

  Context "comparison" {
    BeforeEach {
      $script:data = Initialize-TestDataDirectory -Root $root
      $script:output = Join-Path $root "snapshots.json"
      Add-LearningSnapshot -SnapshotLabel "before" -DataDirectory $script:data -OutputPath $script:output |
        Out-Null
    }

    It "reports no change when nothing changed" {
      Add-LearningSnapshot -SnapshotLabel "after" -DataDirectory $script:data -OutputPath $script:output |
        Out-Null

      $comparison = Compare-LearningSnapshot -FromLabel "before" -ToLabel "after" -OutputPath $script:output

      $comparison.changed | Should -BeFalse
      @($comparison.files | Where-Object { $_.status -notin @("unchanged", "absent") }).Count | Should -Be 0
    }

    It "does not count a timestamp-only update as a change" {
      $learning = Join-Path $script:data "learning.tsv"
      (Get-Item -LiteralPath $learning).LastWriteTimeUtc = [DateTime]::UtcNow.AddMinutes(5)
      Add-LearningSnapshot -SnapshotLabel "after" -DataDirectory $script:data -OutputPath $script:output |
        Out-Null

      $comparison = Compare-LearningSnapshot -FromLabel "before" -ToLabel "after" -OutputPath $script:output

      $comparison.changed | Should -BeFalse
      (@($comparison.files | Where-Object { $_.path -eq "learning.tsv" })[0]).status | Should -BeExactly "touched"
    }

    It "classifies changed, added, and removed files with deltas" {
      Write-TestFile -Path (Join-Path $script:data "learning.tsv") `
        -Text "$($script:secretJapanese)`t$($script:secretMarker)`n2`t3`n4`t5`n"
      Write-TestFile -Path (Join-Path $script:data "auto_words.tsv") -Text "x`n"
      Remove-Item -LiteralPath (Join-Path $script:data "user_dict.json.bak")
      Add-LearningSnapshot -SnapshotLabel "after" -DataDirectory $script:data -OutputPath $script:output |
        Out-Null

      $comparison = Compare-LearningSnapshot -FromLabel "before" -ToLabel "after" -OutputPath $script:output

      $comparison.changed | Should -BeTrue
      $byPath = @{}
      foreach ($entry in $comparison.files) {
        $byPath[$entry.path] = $entry
      }
      $byPath["learning.tsv"].status | Should -BeExactly "changed"
      $byPath["learning.tsv"].lineCountDelta | Should -Be 1
      $byPath["learning.tsv"].sizeDelta | Should -Be 4
      $byPath["auto_words.tsv"].status | Should -BeExactly "added"
      $byPath["user_dict.json.bak"].status | Should -BeExactly "removed"
      $byPath["user_dict.json.enc"].status | Should -BeExactly "unchanged"
      $byPath["typo_corrections.tsv"].status | Should -BeExactly "absent"
      ($comparison | ConvertTo-Json -Depth 8) | Should -Not -Match ([regex]::Escape($script:secretMarker))
    }

    It "lists but does not count files that are not learning stores" {
      Write-TestFile -Path (Join-Path $script:data "host_run_state.txt") -Text "clean`n"
      Add-LearningSnapshot -SnapshotLabel "mid" -DataDirectory $script:data -OutputPath $script:output | Out-Null
      Write-TestFile -Path (Join-Path $script:data "host_run_state.txt") -Text "running`n"
      Write-TestFile -Path (Join-Path $script:data "learning.tsv.tmp.1.2.3") -Text "x`n"
      Add-LearningSnapshot -SnapshotLabel "after" -DataDirectory $script:data -OutputPath $script:output |
        Out-Null

      $comparison = Compare-LearningSnapshot -FromLabel "mid" -ToLabel "after" -OutputPath $script:output

      $comparison.changed | Should -BeFalse
      $state = @($comparison.files | Where-Object { $_.path -eq "host_run_state.txt" })[0]
      $state.status | Should -BeExactly "changed"
      $state.learningStore | Should -BeFalse
      (@($comparison.files | Where-Object { $_.path -eq "learning.tsv.tmp.1.2.3" })[0]).status |
        Should -BeExactly "added"
      (Format-LearningSnapshotComparison -Comparison $comparison) -join "`n" |
        Should -Match "not learning data"
    }

    It "records an unreadable store by exception type only and flags the comparison" {
      $learning = Join-Path $script:data "learning.tsv"
      $held = [System.IO.File]::Open($learning, [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
      try {
        Add-LearningSnapshot -SnapshotLabel "after" -DataDirectory $script:data -OutputPath $script:output |
          Out-Null
      } finally {
        $held.Dispose()
      }

      $text = [System.IO.File]::ReadAllText($script:output)
      $text | Should -Not -Match ([regex]::Escape($root))
      $text | Should -Not -Match ([regex]::Escape($root.Replace("\", "\\")))
      $snapshot = @(($text | ConvertFrom-Json).snapshots)[1]
      $record = Get-TestRecord -Snapshot $snapshot -Path "learning.tsv"
      $record.exists | Should -BeTrue
      $record.readError | Should -BeExactly "IOException"
      $record.sha256 | Should -BeNullOrEmpty
      $comparison = Compare-LearningSnapshot -FromLabel "before" -ToLabel "after" -OutputPath $script:output
      $comparison.unreadableStores | Should -Be 1
      (@($comparison.files | Where-Object { $_.path -eq "learning.tsv" })[0]).status |
        Should -BeExactly "unreadable"
      (Format-LearningSnapshotComparison -Comparison $comparison) -join "`n" | Should -Match "Unverified: 1"
    }

    It "rejects comparing snapshots from different data directories" {
      Add-LearningSnapshot -SnapshotLabel "elsewhere" -DataDirectory $script:data -OutputPath $script:output |
        Out-Null
      $document = [System.IO.File]::ReadAllText($script:output) | ConvertFrom-Json
      @($document.snapshots)[1].dataDirectory = "%LOCALAPPDATA%\azooKey\data"
      [System.IO.File]::WriteAllText($script:output, ($document | ConvertTo-Json -Depth 8))

      { Compare-LearningSnapshot -FromLabel "before" -ToLabel "elsewhere" -OutputPath $script:output } |
        Should -Throw "*different data directories*"
    }

    It "warns when no learning store exists in either snapshot" {
      $empty = Join-Path $root "empty"
      $output = Join-Path $root "empty.json"
      Add-LearningSnapshot -SnapshotLabel "a" -DataDirectory $empty -OutputPath $output | Out-Null
      Add-LearningSnapshot -SnapshotLabel "b" -DataDirectory $empty -OutputPath $output | Out-Null

      $comparison = Compare-LearningSnapshot -FromLabel "a" -ToLabel "b" -OutputPath $output

      $comparison.storesPresent | Should -Be 0
      (Format-LearningSnapshotComparison -Comparison $comparison) -join "`n" |
        Should -Match "Warning: no learning store"
      $withData = Compare-LearningSnapshot -FromLabel "before" -ToLabel "before" -OutputPath $script:output
      (Format-LearningSnapshotComparison -Comparison $withData) -join "`n" | Should -Not -Match "Warning:"
    }

    It "fails clearly for an unknown label or a missing file" {
      { Compare-LearningSnapshot -FromLabel "before" -ToLabel "nope" -OutputPath $script:output } |
        Should -Throw "*'nope' is not recorded*"
      { Compare-LearningSnapshot -FromLabel "a" -ToLabel "b" -OutputPath (Join-Path $root "missing.json") } |
        Should -Throw "*does not exist*"
    }
  }

  Context "command line" {
    It "keeps the script ASCII-only for Windows PowerShell 5.1 in the guest" {
      $bytes = [System.IO.File]::ReadAllBytes($script:snapshotScript)

      @($bytes | Where-Object { $_ -gt 0x7F }).Count | Should -Be 0
    }

    It "records and compares without printing file contents" {
      $data = Initialize-TestDataDirectory -Root $root
      $output = Join-Path $root "cli.json"
      $shell = (Get-Process -Id $PID).Path

      & $shell -NoProfile -File $script:snapshotScript -Label before -DataDirectory $data -OutputPath $output |
        Out-Null
      $LASTEXITCODE | Should -Be 0
      Write-TestFile -Path (Join-Path $data "learning.tsv") -Text "$($script:secretMarker)`n"
      & $shell -NoProfile -File $script:snapshotScript -Label after -DataDirectory $data -OutputPath $output |
        Out-Null
      $text = & $shell -NoProfile -File $script:snapshotScript -From before -To after -OutputPath $output
      $LASTEXITCODE | Should -Be 0

      ($text -join "`n") | Should -Match "changed\s+learning\.tsv"
      ($text -join "`n") | Should -Match "Changed: yes"
      ($text -join "`n") | Should -Not -Match ([regex]::Escape($script:secretMarker))
    }

    It "rejects mixing record and compare modes" {
      $shell = (Get-Process -Id $PID).Path

      & $shell -NoProfile -File $script:snapshotScript -Label a -From a -To b 2>&1 | Out-Null

      $LASTEXITCODE | Should -Not -Be 0
    }
  }
}
