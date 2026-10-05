#requires -Version 5.1
<#
.SYNOPSIS
  Creates directories and files with non-ASCII names for the Windows CLI path gates.

.DESCRIPTION
  VM guest script for the human gates that pass non-ASCII paths to
  azookey_inference_host.exe (--learning, --user-dict, --model and
  model.selectedPath). It creates two sets under -Root:

    cp932    : names that the Japanese ANSI code page (CP932) can represent
    unicode  : names that CP932 cannot represent (a Hangul syllable in the BMP and
               a CJK Extension B ideograph, which is a surrogate pair in UTF-16)

  Each set gets a directory, a plaintext learning file with one synthetic record
  (reading U+306B U+307B U+3093 U+3054, surface U+65E5 U+672C U+8A9E) and the path
  where the CLI should create the user dictionary. The Host migrates the learning
  file to <name>.enc and <name>.bak the first time it reads it; the lookup CLI
  reads it without migrating. With -ModelPath the
  GGUF is copied into each directory under a non-ASCII file name and the copy is
  checked by SHA-256. The model itself is not part of this repository.

  The names are built from code points so that this file stays ASCII-only and no
  non-ASCII file name is committed. The result is written to paths.json (UTF-8)
  under -Root. Read it back with:

    $sets = (Get-Content -LiteralPath <Root>\paths.json -Raw -Encoding UTF8 | ConvertFrom-Json).sets

  -Root must not exist yet, or must be an empty directory. Nothing outside -Root
  is written. Delete -Root when the gate is done.

  Keep this file ASCII-only. It must run on Windows PowerShell 5.1 (the Windows 11
  default shell), which reads BOM-less UTF-8 as the ANSI code page.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File .\new-non-ascii-path-fixture.ps1 -Root C:\azookey-verify\non-ascii -ModelPath .\model.gguf
#>
param(
  [string]$Root = "",
  [string]$ModelPath = ""
)

$ErrorActionPreference = "Stop"

function ConvertFrom-NonAsciiFixtureCodePoint {
  param([int[]]$CodePoint)
  return -join ($CodePoint | ForEach-Object { [char]::ConvertFromUtf32($_) })
}

function Get-NonAsciiFixtureSet {
  # U+30E2 U+30C7 U+30EB U+691C U+8A3C : katakana "model" + kanji "verification"
  # U+5B66 U+7FD2 : "learning"   U+8F9E U+66F8 : "dictionary"
  # U+D55C : Hangul syllable, absent from CP932
  # U+20BB7 : CJK Extension B ideograph, absent from CP932 and a surrogate pair
  $model = ConvertFrom-NonAsciiFixtureCodePoint 0x30E2, 0x30C7, 0x30EB
  $verify = ConvertFrom-NonAsciiFixtureCodePoint 0x691C, 0x8A3C
  $learning = ConvertFrom-NonAsciiFixtureCodePoint 0x5B66, 0x7FD2
  $dictionary = ConvertFrom-NonAsciiFixtureCodePoint 0x8F9E, 0x66F8
  $outside = ConvertFrom-NonAsciiFixtureCodePoint 0xD55C, 0x20BB7
  return @(
    [pscustomobject]@{
      name = "cp932"
      directory = "$model$verify"
      learning = "$learning.tsv"
      userDict = "$dictionary.json"
      model = "$model.gguf"
    },
    [pscustomobject]@{
      name = "unicode"
      directory = "$verify-$outside"
      learning = "$learning-$outside.tsv"
      userDict = "$dictionary-$outside.json"
      model = "$model-$outside.gguf"
    }
  )
}

function Get-NonAsciiFixtureCodePointText {
  param([string]$Text)
  $points = @()
  for ($i = 0; $i -lt $Text.Length; $i++) {
    $point = [char]::ConvertToUtf32($Text, $i)
    if ([char]::IsHighSurrogate($Text[$i])) { $i++ }
    $points += "U+{0:X4}" -f $point
  }
  return $points -join " "
}

function Test-NonAsciiFixtureCp932 {
  # True when CP932 round-trips the text, i.e. the name survives an ANSI API on a Japanese system.
  param([string]$Text)
  $encoding = [System.Text.Encoding]::GetEncoding(932,
    [System.Text.EncoderFallback]::ExceptionFallback, [System.Text.DecoderFallback]::ExceptionFallback)
  try {
    return $encoding.GetString($encoding.GetBytes($Text)) -ceq $Text
  } catch [System.Text.EncoderFallbackException] {
    return $false
  }
}

function Write-NonAsciiPathFixture {
  param([string]$Root, [string]$ModelPath = "")
  $rootPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Root)
  if (Test-Path -LiteralPath $rootPath) {
    if (-not (Test-Path -LiteralPath $rootPath -PathType Container)) {
      throw "Root must be a directory."
    }
    if (Get-ChildItem -LiteralPath $rootPath -Force | Select-Object -First 1) {
      throw "Root must be empty. Use a new directory for each run."
    }
  }
  $modelSource = ""
  $modelHash = ""
  if ($ModelPath) {
    $modelSource = (Resolve-Path -LiteralPath $ModelPath).Path
    if (-not (Test-Path -LiteralPath $modelSource -PathType Leaf)) { throw "ModelPath must be a file." }
    $modelHash = (Get-FileHash -LiteralPath $modelSource -Algorithm SHA256).Hash
  }
  New-Item -ItemType Directory -Path $rootPath -Force | Out-Null

  # LF and no BOM: the store compares the header line byte for byte.
  $reading = ConvertFrom-NonAsciiFixtureCodePoint 0x306B, 0x307B, 0x3093, 0x3054
  $surface = ConvertFrom-NonAsciiFixtureCodePoint 0x65E5, 0x672C, 0x8A9E
  $learningText = "# azookey-learning-tsv escaped=1`n$reading`t$surface`t1 1790812800`n"
  $utf8 = New-Object System.Text.UTF8Encoding $false

  $sets = @()
  foreach ($set in @(Get-NonAsciiFixtureSet)) {
    $directory = Join-Path $rootPath $set.directory
    New-Item -ItemType Directory -Path $directory | Out-Null
    $learning = Join-Path $directory $set.learning
    [System.IO.File]::WriteAllText($learning, $learningText, $utf8)
    $model = ""
    if ($modelSource) {
      $model = Join-Path $directory $set.model
      Copy-Item -LiteralPath $modelSource -Destination $model
      if ((Get-FileHash -LiteralPath $model -Algorithm SHA256).Hash -ne $modelHash) {
        throw "Copied model does not match the source."
      }
    }
    $sets += [pscustomobject]@{
      name = $set.name
      representableInCp932 = (Test-NonAsciiFixtureCp932 -Text ($set.directory + $set.learning +
          $set.userDict + $set.model))
      directory = $directory
      directoryCodePoints = (Get-NonAsciiFixtureCodePointText -Text $set.directory)
      learningPath = $learning
      userDictPath = (Join-Path $directory $set.userDict)
      modelPath = $model
    }
  }

  $acp = (Get-ItemProperty -LiteralPath "HKLM:\SYSTEM\CurrentControlSet\Control\Nls\CodePage").ACP
  $result = [pscustomobject]@{
    schemaVersion = 1
    activeCodePage = [string]$acp
    modelSha256 = $modelHash
    sets = $sets
  }
  $json = $result | ConvertTo-Json -Depth 4
  [System.IO.File]::WriteAllText((Join-Path $rootPath "paths.json"), $json, $utf8)
  return $result
}

if ($MyInvocation.InvocationName -ne ".") {
  if (-not $Root) { throw "-Root is required." }
  $result = Write-NonAsciiPathFixture -Root $Root -ModelPath $ModelPath
  Write-Output "Active code page: $($result.activeCodePage)"
  foreach ($set in $result.sets) {
    Write-Output "Set $($set.name): CP932 representable = $($set.representableInCp932); $($set.directoryCodePoints)"
  }
  Write-Output "Paths: paths.json under -Root (UTF-8)"
}
