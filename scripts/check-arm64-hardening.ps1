#requires -Version 5.1
<#
.SYNOPSIS
  Verify ARM64 PE hardening and retain dumpbin evidence, including failed checks.
#>
param(
  [string[]]$BinaryPath = @(),
  [string]$OutputDirectory = "",
  [string]$DumpbinPath = "dumpbin"
)

$ErrorActionPreference = "Stop"

function Get-Arm64PeHexValue {
  param(
    [string]$Text,
    [string]$Label,
    [int]$MaxDigits = 16
  )

  $fields = [regex]::Matches($Text, ('(?mi)^\s*(\S+)[ \t]+' +
    [regex]::Escape($Label) + '[ \t]*\r?$'))
  if ($fields.Count -ne 1 -or
      $fields[0].Groups[1].Value -notmatch ('^[0-9a-fA-F]{1,' + $MaxDigits + '}$')) {
    throw "Missing, malformed or duplicate PE field: $Label"
  }
  return [Convert]::ToUInt64($fields[0].Groups[1].Value, 16)
}

function Assert-Arm64HardeningReport {
  param(
    [AllowEmptyString()]
    [string]$Text
  )

  if ($Text -notmatch '(?mi)^[ \t]*AA64 machine \(ARM64\)(?: \(ARM64X\))?[ \t]*\r?$') {
    throw "Expected AA64 machine (ARM64)."
  }
  $dllCharacteristics = Get-Arm64PeHexValue -Text $Text -Label "DLL characteristics" -MaxDigits 4
  # GUARD_CF | NX_COMPAT | DYNAMIC_BASE | HIGH_ENTROPY_VA must all be set.
  if (($dllCharacteristics -band 0x4160) -ne 0x4160 -or
      $Text -notmatch '(?mi)^[ \t]*Control Flow Guard[ \t]*\r?$') {
    throw "DLL characteristics must include CFG, NX compatibility, dynamic base and high entropy VA (0x4160)."
  }
  $guardFlags = Get-Arm64PeHexValue -Text $Text -Label "Guard Flags" -MaxDigits 8
  if (($guardFlags -band 0x500) -ne 0x500 -or
      $Text -notmatch '(?mi)^[ \t]*CF instrumented[ \t]*\r?$' -or
      $Text -notmatch '(?mi)^[ \t]*FID table present[ \t]*\r?$') {
    throw "Guard Flags must include CF instrumented and FID table present."
  }
  $table = Get-Arm64PeHexValue -Text $Text -Label "Guard CF function table"
  $count = Get-Arm64PeHexValue -Text $Text -Label "Guard CF function count"
  if ($table -eq 0 -or $count -eq 0) {
    throw "Guard CF function table and count must be nonzero."
  }
  $dependentLoadFlags = Get-Arm64PeHexValue -Text $Text -Label "Dependent Load Flag" -MaxDigits 4
  if ($dependentLoadFlags -ne 0x0B00) {
    throw "Expected Dependent Load Flag 0B00, got $($dependentLoadFlags.ToString('X4'))."
  }
}

function Invoke-Arm64Dumpbin {
  param(
    [string]$Path,
    [string]$DumpbinPath
  )

  # Capture native stderr on Windows PowerShell as well as PowerShell 7. A
  # nonzero exit is checked explicitly after all diagnostic output is captured.
  $ErrorActionPreference = "Continue"
  $PSNativeCommandUseErrorActionPreference = $false
  try {
    $output = @(& $DumpbinPath /nologo /headers /loadconfig $Path 2>&1)
    $completed = $?
    $exitCode = $LASTEXITCODE
  } catch {
    $output = @($_)
    $completed = $false
    $exitCode = -1
  }
  return [pscustomobject]@{
    Text = ($output | ForEach-Object { [string]$_ }) -join [Environment]::NewLine
    Succeeded = $completed -and $null -ne $exitCode -and $exitCode -eq 0
    ExitCode = $exitCode
  }
}

function Assert-Arm64Hardening {
  param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string[]]$BinaryPath,
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$OutputDirectory,
    [string]$DumpbinPath = "dumpbin"
  )

  New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
  $failures = @()
  $index = 0
  foreach ($binary in $BinaryPath) {
    $index++
    $reportPath = Join-Path $OutputDirectory (
      '{0:D2}-{1}.dumpbin.txt' -f $index, [IO.Path]::GetFileName($binary))
    try {
      if (-not (Test-Path -LiteralPath $binary -PathType Leaf)) {
        "Binary to inspect does not exist: $binary" | Set-Content -LiteralPath $reportPath -Encoding UTF8
        throw "Binary to inspect does not exist: $binary"
      }
      $dump = Invoke-Arm64Dumpbin -Path $binary -DumpbinPath $DumpbinPath
      $dump.Text | Set-Content -LiteralPath $reportPath -Encoding UTF8
      if (-not $dump.Succeeded) {
        throw "dumpbin failed (exit $($dump.ExitCode)); see $reportPath"
      }
      Assert-Arm64HardeningReport -Text $dump.Text
      Write-Output "ARM64 hardening passed: $binary (evidence: $reportPath)"
    } catch {
      $failures += "${binary}: $($_.Exception.Message)"
    }
  }
  if ($failures.Count -gt 0) {
    throw ("ARM64 hardening failed:`n" + ($failures -join "`n"))
  }
}

if ($MyInvocation.InvocationName -ne ".") {
  if (-not $BinaryPath -or -not $OutputDirectory) {
    throw "Specify -BinaryPath and -OutputDirectory."
  }
  Assert-Arm64Hardening -BinaryPath $BinaryPath -OutputDirectory $OutputDirectory -DumpbinPath $DumpbinPath
}
