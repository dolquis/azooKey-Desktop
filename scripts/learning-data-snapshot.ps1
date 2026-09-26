#requires -Version 5.1
<#
.SYNOPSIS
  Records labeled snapshots of the learning data directory and compares two labels.

.DESCRIPTION
  VM guest script for human gates whose pass condition is "the learning data does
  not change". Run it at each gate boundary to record the state in one command.

    -Label <name>             : record the current state and append it to -OutputPath
    -From <name> -To <name>   : compare two recorded labels file by file

  Targets every file under -DataDirectory (default %LOCALAPPDATA%\azooKey\data) plus
  the known stores (learning.tsv, user_dict.json, typo_corrections.tsv, auto_words.tsv
  and their .enc). Known stores are recorded with exists=false when missing.

  Only the relative path, existence, size, line count, last write time (UTC) and
  SHA-256 of each file are recorded. File contents are never written out. The
  absolute data directory contains the user name, so it is written only as
  %LOCALAPPDATA%\azooKey\data (default) or "custom". File names outside a fixed
  character set are replaced with a stand-in derived from the SHA-256 of the name.

  Line count is the number of LF (0x0A) bytes, plus one when a non-empty file does
  not end with LF. .enc files are DPAPI ciphertext: they are not decrypted, their
  ciphertext SHA-256 is compared, and their line count is null.

  Data files are opened read-only with ReadWrite and Delete sharing so that a
  running Host does not block the read. Nothing is written to the data directory.

  A comparison counts a changed SHA-256, an added file, or a removed file among the
  learning stores (the known stores and their .enc and .bak) as a change. Other files
  in the data directory, such as host_run_state.txt and the Host's temporary files,
  are listed but not counted. A file whose last write time alone changed ("touched")
  has the same contents and is not counted. A file that could not be read is recorded
  with its exception type only and compares as "unreadable". The two labels must come
  from the same data directory. The result is an observation; each gate's verdict
  follows that issue's criteria and is decided by a human.

  Keep this file ASCII-only. It must run on Windows PowerShell 5.1 (the Windows 11
  default shell), which reads BOM-less UTF-8 as the ANSI code page.
#>
param(
  [string]$Label = "",
  [string]$From = "",
  [string]$To = "",
  [string]$DataDirectory = "",
  [string]$OutputPath = "",
  [switch]$Json
)

$ErrorActionPreference = "Stop"

$script:LearningSnapshotKind = "azookey-learning-data-snapshots"
$script:LearningSnapshotSchemaVersion = 1
$script:LearningSnapshotNotice =
  "Hashes and counts only. File contents are never recorded. This file does not decide any human gate."
$script:LearningSnapshotDefaultDirectoryLabel = "%LOCALAPPDATA%\azooKey\data"
$script:LearningSnapshotKnownStores = @(
  "learning.tsv",
  "user_dict.json",
  "typo_corrections.tsv",
  "auto_words.tsv"
)
$script:LearningSnapshotLabelPattern = '^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$'
$script:LearningSnapshotPathPattern = '^[A-Za-z0-9][A-Za-z0-9._-]*(/[A-Za-z0-9][A-Za-z0-9._-]*)*$'
$script:LearningSnapshotMaxPathLength = 128

function Get-LearningSnapshotAbsolutePath {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path
  )

  return $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path)
}

function Get-LearningSnapshotDefaultDataDirectory {
  if (-not $env:LOCALAPPDATA) {
    throw "LOCALAPPDATA is not set. Pass -DataDirectory explicitly."
  }
  return Join-Path (Join-Path $env:LOCALAPPDATA "azooKey") "data"
}

function Test-LearningSnapshotLabel {
  param(
    [AllowEmptyString()]
    [string]$Value
  )

  return [bool]($Value -cmatch $script:LearningSnapshotLabelPattern)
}

function Get-LearningSnapshotTextHash {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Text
  )

  $sha = [System.Security.Cryptography.SHA256]::Create()
  try {
    $bytes = $sha.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($Text))
  } finally {
    $sha.Dispose()
  }
  return (($bytes | ForEach-Object { $_.ToString("x2") }) -join "")
}

# Relative paths are written as-is only within a fixed character set. Other names may
# contain user input, so they become a stable stand-in that still works as a diff key.
function ConvertTo-LearningSnapshotPathToken {
  param(
    [Parameter(Mandatory = $true)]
    [string]$RelativePath
  )

  if ($RelativePath.Length -le $script:LearningSnapshotMaxPathLength -and
      $RelativePath -cmatch $script:LearningSnapshotPathPattern) {
    return $RelativePath
  }
  $digest = Get-LearningSnapshotTextHash -Text $RelativePath
  return "unrecognized-name-" + $digest.Substring(0, 12)
}

# Contents feed only the SHA-256 and the LF count, both computed in a single read.
function Get-LearningSnapshotContentDigest {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path,
    [switch]$SkipLineCount
  )

  $share = [System.IO.FileShare]::ReadWrite -bor [System.IO.FileShare]::Delete
  $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, $share)
  $sha = [System.Security.Cryptography.SHA256]::Create()
  try {
    $buffer = New-Object byte[] 65536
    $lineFeeds = [long]0
    $total = [long]0
    $lastByte = -1
    while (($read = $stream.Read($buffer, 0, $buffer.Length)) -gt 0) {
      [void]$sha.TransformBlock($buffer, 0, $read, $null, 0)
      $total += $read
      $lastByte = $buffer[$read - 1]
      if (-not $SkipLineCount) {
        $index = [Array]::IndexOf($buffer, [byte]10, 0, $read)
        while ($index -ge 0) {
          $lineFeeds++
          $next = $index + 1
          if ($next -ge $read) {
            break
          }
          $index = [Array]::IndexOf($buffer, [byte]10, $next, $read - $next)
        }
      }
    }
    [void]$sha.TransformFinalBlock((New-Object byte[] 0), 0, 0)
    $hash = (($sha.Hash | ForEach-Object { $_.ToString("x2") }) -join "")
  } finally {
    $sha.Dispose()
    $stream.Dispose()
  }

  $lineCount = $null
  if (-not $SkipLineCount) {
    $lineCount = $lineFeeds
    if ($total -gt 0 -and $lastByte -ne 10) {
      $lineCount++
    }
  }
  return [pscustomobject]@{
    Sha256 = $hash
    LineCount = $lineCount
  }
}

# Learning stores are the known stores plus their .enc and .bak. Other files in the
# data directory (for example host_run_state.txt, which the Host rewrites on start
# and exit, and the Host's .tmp.* files) are reported but never count as a change.
function Test-LearningSnapshotStorePath {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path
  )

  foreach ($store in $script:LearningSnapshotKnownStores) {
    foreach ($name in @($store, "$store.enc", "$store.bak")) {
      if ([string]::Equals($Path, $name, [System.StringComparison]::OrdinalIgnoreCase)) {
        return $true
      }
    }
  }
  return $false
}

function Get-LearningSnapshotFileRecord {
  param(
    [Parameter(Mandatory = $true)]
    [string]$RelativePath,
    [AllowNull()]
    [System.IO.FileInfo]$File
  )

  $encrypted = $RelativePath.EndsWith(".enc", [System.StringComparison]::OrdinalIgnoreCase)
  $record = [ordered]@{
    path = ConvertTo-LearningSnapshotPathToken -RelativePath $RelativePath
    learningStore = Test-LearningSnapshotStorePath -Path $RelativePath
    exists = $false
    encrypted = $encrypted
    size = $null
    lineCount = $null
    lastWriteTimeUtc = $null
    sha256 = $null
    readError = $null
  }
  if ($null -eq $File) {
    return $record
  }

  # The Host replaces stores by renaming temporary files, so a listed file can vanish
  # or be locked before it is read. .NET IO exception messages carry the full path,
  # so only the exception type is recorded.
  try {
    $digest = Get-LearningSnapshotContentDigest -Path $File.FullName -SkipLineCount:$encrypted
    $File.Refresh()
    $size = [long]$File.Length
    $lastWrite = $File.LastWriteTimeUtc.ToString("o")
  } catch [System.IO.FileNotFoundException], [System.IO.DirectoryNotFoundException] {
    return $record
  } catch [System.IO.IOException], [System.UnauthorizedAccessException] {
    $record.exists = $true
    # PowerShell wraps .NET method exceptions; record the innermost type.
    $record.readError = $_.Exception.GetBaseException().GetType().Name
    return $record
  }
  $record.exists = $true
  $record.size = $size
  $record.lineCount = $digest.LineCount
  $record.lastWriteTimeUtc = $lastWrite
  $record.sha256 = $digest.Sha256
  return $record
}

function Get-LearningSnapshotRecord {
  param(
    [Parameter(Mandatory = $true)]
    [string]$SnapshotLabel,
    [Parameter(Mandatory = $true)]
    [string]$Directory,
    [Parameter(Mandatory = $true)]
    [string]$DirectoryLabel
  )

  $root = [System.IO.Path]::GetFullPath($Directory).TrimEnd("\", "/")
  # Windows file names are case-insensitive; so are @{} keys and the set below.
  $ignoreCase = [System.StringComparer]::OrdinalIgnoreCase
  $files = @{}
  $directoryExists = Test-Path -LiteralPath $root -PathType Container
  if ($directoryExists) {
    foreach ($file in Get-ChildItem -LiteralPath $root -File -Recurse -Force) {
      $relative = $file.FullName.Substring($root.Length).TrimStart("\", "/").Replace("\", "/")
      $files[$relative] = $file
    }
  }

  $names = New-Object System.Collections.Generic.List[string]
  $seen = New-Object 'System.Collections.Generic.HashSet[string]'($ignoreCase)
  foreach ($store in $script:LearningSnapshotKnownStores) {
    foreach ($name in @($store, "$store.enc")) {
      $names.Add($name)
      [void]$seen.Add($name)
    }
  }
  foreach ($relative in ($files.Keys | Sort-Object)) {
    if ($seen.Add($relative)) {
      $names.Add($relative)
    }
  }

  $records = foreach ($relative in $names) {
    $file = $null
    if ($files.ContainsKey($relative)) {
      $file = $files[$relative]
    }
    [pscustomobject](Get-LearningSnapshotFileRecord -RelativePath $relative -File $file)
  }

  return [pscustomobject][ordered]@{
    label = $SnapshotLabel
    capturedAtUtc = [DateTime]::UtcNow.ToString("o")
    dataDirectory = $DirectoryLabel
    dataDirectoryExists = [bool]$directoryExists
    files = @($records)
  }
}

function ConvertTo-LearningSnapshotTimestamp {
  param(
    [AllowNull()]
    $Value
  )

  if ($Value -is [DateTime]) {
    return $Value.ToUniversalTime().ToString("o")
  }
  return $Value
}

function Read-LearningSnapshotDocument {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path
  )

  if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
    return [pscustomobject][ordered]@{
      schemaVersion = $script:LearningSnapshotSchemaVersion
      kind = $script:LearningSnapshotKind
      notice = $script:LearningSnapshotNotice
      snapshots = @()
    }
  }

  $text = [System.IO.File]::ReadAllText($Path, [System.Text.Encoding]::UTF8)
  $document = $text | ConvertFrom-Json
  if ($document.kind -ne $script:LearningSnapshotKind -or
      $document.schemaVersion -ne $script:LearningSnapshotSchemaVersion) {
    throw ("'$Path' is not a learning data snapshot file (expected kind=" +
      "$($script:LearningSnapshotKind), schemaVersion=$($script:LearningSnapshotSchemaVersion)).")
  }
  # PowerShell 7 ConvertFrom-Json turns ISO 8601 strings into DateTime. Restore the
  # string form that 5.1 keeps before comparing and writing back.
  foreach ($snapshot in @($document.snapshots)) {
    $snapshot.capturedAtUtc = ConvertTo-LearningSnapshotTimestamp -Value $snapshot.capturedAtUtc
    foreach ($file in @($snapshot.files)) {
      $file.lastWriteTimeUtc = ConvertTo-LearningSnapshotTimestamp -Value $file.lastWriteTimeUtc
    }
  }
  return [pscustomobject][ordered]@{
    schemaVersion = $document.schemaVersion
    kind = $document.kind
    notice = $script:LearningSnapshotNotice
    snapshots = @($document.snapshots)
  }
}

# Write to a temporary file and swap it in so an interrupted run keeps earlier records.
# Set-Content -Encoding UTF8 on Windows PowerShell 5.1 adds a BOM, so it is not used.
function Write-LearningSnapshotDocument {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path,
    [Parameter(Mandatory = $true)]
    $Document
  )

  $directory = Split-Path -Parent $Path
  if ($directory -and -not (Test-Path -LiteralPath $directory -PathType Container)) {
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
  }
  $temporary = "$Path.tmp-$([guid]::NewGuid().ToString('N'))"
  $content = ($Document | ConvertTo-Json -Depth 8) + "`n"
  [System.IO.File]::WriteAllText($temporary, $content, (New-Object System.Text.UTF8Encoding($false)))
  try {
    if (Test-Path -LiteralPath $Path -PathType Leaf) {
      # $null reaches a string parameter as "", so request "no backup" with NullString.
      [System.IO.File]::Replace($temporary, $Path, [NullString]::Value)
    } else {
      [System.IO.File]::Move($temporary, $Path)
    }
  } finally {
    if (Test-Path -LiteralPath $temporary) {
      Remove-Item -LiteralPath $temporary -Force
    }
  }
}

function Test-LearningSnapshotPathUnder {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path,
    [Parameter(Mandatory = $true)]
    [string]$Directory
  )

  $root = [System.IO.Path]::GetFullPath($Directory).TrimEnd("\", "/") + "\"
  $candidate = [System.IO.Path]::GetFullPath($Path)
  return $candidate.StartsWith($root, [System.StringComparison]::OrdinalIgnoreCase)
}

function Resolve-LearningSnapshotLocation {
  param(
    [AllowEmptyString()]
    [string]$DataDirectory,
    [AllowEmptyString()]
    [string]$OutputPath
  )

  $directoryLabel = "custom"
  if ($DataDirectory) {
    $directory = Get-LearningSnapshotAbsolutePath -Path $DataDirectory
  } else {
    $directory = Get-LearningSnapshotDefaultDataDirectory
    $directoryLabel = $script:LearningSnapshotDefaultDirectoryLabel
  }
  if ($OutputPath) {
    $output = Get-LearningSnapshotAbsolutePath -Path $OutputPath
  } else {
    $output = Get-LearningSnapshotAbsolutePath -Path "learning-data-snapshots.json"
  }
  if (Test-LearningSnapshotPathUnder -Path $output -Directory $directory) {
    throw "-OutputPath must not be inside the learning data directory. The next snapshot would record it."
  }
  return [pscustomobject]@{
    DataDirectory = $directory
    DirectoryLabel = $directoryLabel
    OutputPath = $output
  }
}

function Add-LearningSnapshot {
  param(
    [Parameter(Mandatory = $true)]
    [string]$SnapshotLabel,
    [AllowEmptyString()]
    [string]$DataDirectory = "",
    [AllowEmptyString()]
    [string]$OutputPath = ""
  )

  if (-not (Test-LearningSnapshotLabel -Value $SnapshotLabel)) {
    throw "-Label must match $($script:LearningSnapshotLabelPattern)."
  }
  $location = Resolve-LearningSnapshotLocation -DataDirectory $DataDirectory -OutputPath $OutputPath
  $document = Read-LearningSnapshotDocument -Path $location.OutputPath
  if (@($document.snapshots | Where-Object { $_.label -ceq $SnapshotLabel }).Count -gt 0) {
    throw "Label '$SnapshotLabel' is already recorded in the snapshot file. Use a new label."
  }

  $snapshot = Get-LearningSnapshotRecord `
    -SnapshotLabel $SnapshotLabel `
    -Directory $location.DataDirectory `
    -DirectoryLabel $location.DirectoryLabel
  $document.snapshots = @($document.snapshots) + @($snapshot)
  Write-LearningSnapshotDocument -Path $location.OutputPath -Document $document

  return [pscustomobject]@{
    OutputPath = $location.OutputPath
    Snapshot = $snapshot
  }
}

function Get-LearningSnapshotFileStatus {
  param(
    [AllowNull()]
    $Before,
    [AllowNull()]
    $After
  )

  $beforeExists = ($null -ne $Before) -and [bool]$Before.exists
  $afterExists = ($null -ne $After) -and [bool]$After.exists
  if (-not $beforeExists -and -not $afterExists) {
    return "absent"
  }
  if (-not $beforeExists) {
    return "added"
  }
  if (-not $afterExists) {
    return "removed"
  }
  if ($Before.readError -or $After.readError) {
    return "unreadable"
  }
  if ($Before.sha256 -ne $After.sha256) {
    return "changed"
  }
  if ($Before.lastWriteTimeUtc -ne $After.lastWriteTimeUtc) {
    return "touched"
  }
  return "unchanged"
}

function Get-LearningSnapshotDelta {
  param(
    [AllowNull()]
    $Before,
    [AllowNull()]
    $After,
    [Parameter(Mandatory = $true)]
    [string]$Field
  )

  if ($null -eq $Before -or $null -eq $After) {
    return $null
  }
  if ($null -eq $Before.$Field -or $null -eq $After.$Field) {
    return $null
  }
  return [long]$After.$Field - [long]$Before.$Field
}

function Compare-LearningSnapshot {
  param(
    [Parameter(Mandatory = $true)]
    [string]$FromLabel,
    [Parameter(Mandatory = $true)]
    [string]$ToLabel,
    [AllowEmptyString()]
    [string]$OutputPath = ""
  )

  if ($OutputPath) {
    $path = Get-LearningSnapshotAbsolutePath -Path $OutputPath
  } else {
    $path = Get-LearningSnapshotAbsolutePath -Path "learning-data-snapshots.json"
  }
  if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
    throw "Snapshot file '$path' does not exist. Record snapshots with -Label first."
  }
  $document = Read-LearningSnapshotDocument -Path $path
  $snapshots = @{}
  foreach ($name in @($FromLabel, $ToLabel)) {
    $found = @($document.snapshots | Where-Object { $_.label -ceq $name })
    if ($found.Count -eq 0) {
      throw "Label '$name' is not recorded in the snapshot file."
    }
    $snapshots[$name] = $found[0]
  }
  if ($snapshots[$FromLabel].dataDirectory -ne $snapshots[$ToLabel].dataDirectory) {
    throw ("Labels '$FromLabel' and '$ToLabel' were recorded from different data directories " +
      "('$($snapshots[$FromLabel].dataDirectory)' and '$($snapshots[$ToLabel].dataDirectory)').")
  }

  $beforeFiles = @{}
  foreach ($file in @($snapshots[$FromLabel].files)) {
    $beforeFiles[$file.path] = $file
  }
  $afterFiles = @{}
  foreach ($file in @($snapshots[$ToLabel].files)) {
    $afterFiles[$file.path] = $file
  }
  $paths = @(@($beforeFiles.Keys) + @($afterFiles.Keys) | Sort-Object -Unique)

  $changed = $false
  $unreadable = 0
  $entries = foreach ($relative in $paths) {
    $before = $beforeFiles[$relative]
    $after = $afterFiles[$relative]
    $status = Get-LearningSnapshotFileStatus -Before $before -After $after
    $isStore = Test-LearningSnapshotStorePath -Path $relative
    if ($isStore -and $status -in @("added", "removed", "changed")) {
      $changed = $true
    }
    if ($isStore -and $status -eq "unreadable") {
      $unreadable++
    }
    [pscustomobject][ordered]@{
      path = $relative
      learningStore = $isStore
      status = $status
      sizeDelta = Get-LearningSnapshotDelta -Before $before -After $after -Field "size"
      lineCountDelta = Get-LearningSnapshotDelta -Before $before -After $after -Field "lineCount"
    }
  }

  return [pscustomobject][ordered]@{
    from = $FromLabel
    to = $ToLabel
    changed = $changed
    unreadableStores = $unreadable
    notice = "Observations only. Human gate verdicts follow each issue's criteria."
    files = @($entries)
  }
}

function Format-LearningSnapshotComparison {
  param(
    [Parameter(Mandatory = $true)]
    $Comparison
  )

  $lines = New-Object System.Collections.Generic.List[string]
  $lines.Add("Learning data: '$($Comparison.from)' -> '$($Comparison.to)'")
  foreach ($entry in $Comparison.files) {
    if ($entry.status -eq "absent") {
      continue
    }
    $detail = ""
    if ($null -ne $entry.sizeDelta) {
      $detail += " size {0:+#;-#;0}" -f $entry.sizeDelta
    }
    if ($null -ne $entry.lineCountDelta) {
      $detail += " lines {0:+#;-#;0}" -f $entry.lineCountDelta
    }
    if (-not $entry.learningStore) {
      $detail += " (not learning data; not counted)"
    }
    $lines.Add(("  {0,-10} {1}{2}" -f $entry.status, $entry.path, $detail))
  }
  if ($Comparison.changed) {
    $lines.Add("Changed: yes")
  } else {
    $lines.Add("Changed: no")
  }
  if ($Comparison.unreadableStores -gt 0) {
    $lines.Add("Unverified: $($Comparison.unreadableStores) learning store file(s) could not be read. Record again.")
  }
  $lines.Add($Comparison.notice)
  return $lines
}

if ($MyInvocation.InvocationName -ne ".") {
  $recordMode = [bool]$Label
  $compareMode = [bool]($From -or $To)
  if ($recordMode -eq $compareMode) {
    throw "Pass either -Label <name> to record, or -From <name> -To <name> to compare."
  }

  if ($recordMode) {
    $result = Add-LearningSnapshot -SnapshotLabel $Label -DataDirectory $DataDirectory -OutputPath $OutputPath
    $present = @($result.Snapshot.files | Where-Object { $_.exists }).Count
    if ($Json) {
      Write-Output ($result.Snapshot | ConvertTo-Json -Depth 8)
    } else {
      Write-Output "Recorded snapshot '$Label' ($present file(s) present) to $($result.OutputPath)"
    }
  } else {
    if (-not $From -or -not $To) {
      throw "Pass both -From <name> and -To <name>."
    }
    $comparison = Compare-LearningSnapshot -FromLabel $From -ToLabel $To -OutputPath $OutputPath
    if ($Json) {
      Write-Output ($comparison | ConvertTo-Json -Depth 8)
    } else {
      Format-LearningSnapshotComparison -Comparison $comparison | Write-Output
    }
  }
}
