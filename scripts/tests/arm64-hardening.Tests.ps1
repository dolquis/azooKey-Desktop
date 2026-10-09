Describe "ARM64 binary hardening" {
  BeforeAll {
    $script:checker = Join-Path (Join-Path $PSScriptRoot "..") "check-arm64-hardening.ps1"
    . $script:checker

    # Field spelling and hexadecimal count match dumpbin 14.51 /headers
    # /loadconfig on an installed ARM64 CRT. Values describe the required policy.
    $script:validReport = @'
FILE HEADER VALUES
            AA64 machine (ARM64)
OPTIONAL HEADER VALUES
            4160 DLL characteristics
                   Control Flow Guard
  Section contains the following load config:
                0B00 Dependent Load Flag
    000000018002F11C Guard CF function table
                 11D Guard CF function count
            10417500 Guard Flags
                       CF instrumented
                       FID table present
'@
    function Invoke-TestDumpbin {
      param([Parameter(ValueFromRemainingArguments = $true)][string[]]$ArgumentList)
      throw "Unmocked test dumpbin: $ArgumentList"
    }
  }

  BeforeEach {
    $script:binary = Join-Path $TestDrive "azookey_tsf_tip.dll"
    $script:hostBinary = Join-Path $TestDrive "azookey_inference_host.exe"
    $script:evidence = Join-Path $TestDrive ([Guid]::NewGuid().ToString("N"))
    "test fixture" | Set-Content -LiteralPath $script:binary
    "test fixture" | Set-Content -LiteralPath $script:hostBinary
    $script:nativeReport = $script:validReport
    $script:nativeExitCode = 0
    Mock Invoke-TestDumpbin {
      $global:LASTEXITCODE = $script:nativeExitCode
      $script:nativeReport
    }
  }

  It "checks both binaries and retains their raw reports" {
    Assert-Arm64Hardening -BinaryPath @($script:binary, $script:hostBinary) `
      -OutputDirectory $script:evidence -DumpbinPath "Invoke-TestDumpbin"

    Should -Invoke Invoke-TestDumpbin -Times 2 -Exactly
    Should -Invoke Invoke-TestDumpbin -Times 1 -Exactly -ParameterFilter {
      ($ArgumentList -join '|') -eq "/nologo|/headers|/loadconfig|$script:binary"
    }
    Should -Invoke Invoke-TestDumpbin -Times 1 -Exactly -ParameterFilter {
      ($ArgumentList -join '|') -eq "/nologo|/headers|/loadconfig|$script:hostBinary"
    }
    $reports = @(Get-ChildItem -LiteralPath $script:evidence -Filter "*.dumpbin.txt")
    $reports.Count | Should -Be 2
    foreach ($report in $reports) {
      (Get-Content -Raw -LiteralPath $report.FullName).Trim() | Should -Be $script:validReport.Trim()
    }
  }

  It "accepts the ARM64X annotation and hexadecimal function count" {
    { Assert-Arm64HardeningReport -Text ($script:validReport.Replace(
        "machine (ARM64)", "machine (ARM64) (ARM64X)")) } | Should -Not -Throw
  }

  It "rejects omitted <Label>" -TestCases @(
    @{ Label = "AA64 machine (ARM64)" }
    @{ Label = "4160 DLL characteristics" }
    @{ Label = "Control Flow Guard" }
    @{ Label = "10417500 Guard Flags" }
    @{ Label = "CF instrumented" }
    @{ Label = "FID table present" }
    @{ Label = "000000018002F11C Guard CF function table" }
    @{ Label = "11D Guard CF function count" }
    @{ Label = "0B00 Dependent Load Flag" }
  ) {
    param($Label)
    $report = $script:validReport.Replace($Label, "")
    { Assert-Arm64HardeningReport -Text $report } |
      Should -Throw
  }

  It "rejects invalid <Field>" -TestCases @(
    @{ Field = "AA64 machine (ARM64)"; Replacement = "8664 machine (x64)" }
    @{ Field = "4160 DLL characteristics"; Replacement = "0160 DLL characteristics" }
    @{ Field = "4160 DLL characteristics"; Replacement = "4120 DLL characteristics" }
    @{ Field = "4160 DLL characteristics"; Replacement = "4060 DLL characteristics" }
    @{ Field = "4160 DLL characteristics"; Replacement = "4140 DLL characteristics" }
    @{ Field = "4160 DLL characteristics"; Replacement = "4100 DLL characteristics" }
    @{ Field = "4160 DLL characteristics"; Replacement = "nope DLL characteristics" }
    @{ Field = "4160 DLL characteristics"; Replacement = "100004160 DLL characteristics" }
    @{ Field = "10417500 Guard Flags"; Replacement = "10417400 Guard Flags" }
    @{ Field = "10417500 Guard Flags"; Replacement = "10417100 Guard Flags" }
    @{ Field = "10417500 Guard Flags"; Replacement = "nope Guard Flags" }
    @{ Field = "10417500 Guard Flags"; Replacement = "1000010417500 Guard Flags" }
    @{ Field = "000000018002F11C Guard CF function table"; Replacement = "0000000000000000 Guard CF function table" }
    @{ Field = "000000018002F11C Guard CF function table"; Replacement = "nope Guard CF function table" }
    @{ Field = "11D Guard CF function count"; Replacement = "0 Guard CF function count" }
    @{ Field = "11D Guard CF function count"; Replacement = "-1 Guard CF function count" }
    @{ Field = "11D Guard CF function count"; Replacement = "10000000000000000 Guard CF function count" }
    @{ Field = "0B00 Dependent Load Flag"; Replacement = "0000 Dependent Load Flag" }
    @{ Field = "0B00 Dependent Load Flag"; Replacement = "0F00 Dependent Load Flag" }
    @{ Field = "0B00 Dependent Load Flag"; Replacement = "0B00junk Dependent Load Flag" }
    @{ Field = "0B00 Dependent Load Flag"; Replacement = "00000B00 Dependent Load Flag" }
  ) {
    param($Field, $Replacement)
    $report = $script:validReport.Replace($Field, $Replacement)
    { Assert-Arm64HardeningReport -Text $report } |
      Should -Throw
  }

  It "rejects duplicate dependent-load fields" {
    { Assert-Arm64HardeningReport -Text ($script:validReport + "`n  0B00 Dependent Load Flag") } |
      Should -Throw "*duplicate PE field*"
  }

  It "rejects empty output" {
    { Assert-Arm64HardeningReport -Text "" } | Should -Throw
  }

  It "fails a native nonzero exit even when the output looks valid and retains evidence" {
    $script:nativeExitCode = 7
    {
      Assert-Arm64Hardening -BinaryPath @($script:binary, $script:hostBinary) `
        -OutputDirectory $script:evidence -DumpbinPath "Invoke-TestDumpbin"
    } | Should -Throw "*dumpbin failed (exit 7)*"
    Should -Invoke Invoke-TestDumpbin -Times 2 -Exactly
    @(Get-ChildItem -LiteralPath $script:evidence).Count | Should -Be 2
    Get-Content -Raw -LiteralPath (Join-Path $script:evidence "01-azookey_tsf_tip.dll.dumpbin.txt") |
      Should -Match "CF instrumented"
  }

  It "retains malformed output and still inspects the second binary" {
    $script:nativeReport = "dumpbin diagnostic: bad image"
    {
      Assert-Arm64Hardening -BinaryPath @($script:binary, $script:hostBinary) `
        -OutputDirectory $script:evidence -DumpbinPath "Invoke-TestDumpbin"
    } | Should -Throw "*Expected AA64*"
    Should -Invoke Invoke-TestDumpbin -Times 2 -Exactly
    Get-Content -Raw -LiteralPath (Join-Path $script:evidence "01-azookey_tsf_tip.dll.dumpbin.txt") |
      Should -Match "bad image"
  }

  It "rejects a missing binary but still inspects the next binary" {
    {
      Assert-Arm64Hardening -BinaryPath @((Join-Path $TestDrive "absent.dll"), $script:hostBinary) `
        -OutputDirectory $script:evidence -DumpbinPath "Invoke-TestDumpbin"
    } | Should -Throw "*Binary to inspect does not exist*"
    Should -Invoke Invoke-TestDumpbin -Times 1 -Exactly
    @(Get-ChildItem -LiteralPath $script:evidence).Count | Should -Be 2
  }

  It "rejects a missing dumpbin command and retains its diagnostic" {
    {
      Assert-Arm64Hardening -BinaryPath @($script:binary) -OutputDirectory $script:evidence `
        -DumpbinPath (Join-Path $TestDrive "missing-dumpbin.exe")
    } | Should -Throw "*dumpbin failed*"
    Get-Content -Raw -LiteralPath (Join-Path $script:evidence "01-azookey_tsf_tip.dll.dumpbin.txt") |
      Should -Match "missing-dumpbin"
  }

  It "rejects an empty binary list" {
    { Assert-Arm64Hardening -BinaryPath @() -OutputDirectory $script:evidence } | Should -Throw
  }
}
