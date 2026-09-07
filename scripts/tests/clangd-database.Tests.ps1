Describe "strict clangd database validation" {
  BeforeAll {
    . (Join-Path $PSScriptRoot "../clangd-database.ps1")
  }
  BeforeEach {
    $root = Join-Path $TestDrive ([guid]::NewGuid().ToString())
    $build = Join-Path $root "build/clangd"
    $null = New-Item -ItemType Directory -Path $build -Force
    $source = Join-Path $root "sample.cpp"
    Set-Content -LiteralPath $source -Value 'int main() {}'
    $database = Join-Path $build "compile_commands.json"
    $entry = @{ directory = $build; file = $source; command = "clang-cl /c sample.cpp" }
    ConvertTo-Json -InputObject @($entry) | Set-Content -LiteralPath $database
  }
  It "accepts this checkout and resolves relative file names" {
    $entry.file = '../../sample.cpp'
    ConvertTo-Json -InputObject @($entry) | Set-Content -LiteralPath $database
    (Test-ClangdDatabase -RepoRoot $root -RequiredSources sample.cpp).ok | Should -BeTrue
  }
  It "rejects missing, empty, malformed and non-array databases" {
    foreach ($content in @('[]', '{', '{}', '[{}]')) {
      Set-Content -LiteralPath $database -Value $content
      (Test-ClangdDatabase -RepoRoot $root -RequiredSources sample.cpp).ok | Should -BeFalse
    }
    Remove-Item -LiteralPath $database
    (Test-ClangdDatabase -RepoRoot $root -RequiredSources sample.cpp).ok | Should -BeFalse
  }
  It "rejects a database copied from a different checkout even if sources exist" {
    $entry.directory = $TestDrive
    ConvertTo-Json -InputObject @($entry) | Set-Content -LiteralPath $database
    (Test-ClangdDatabase -RepoRoot $root -RequiredSources sample.cpp).ok | Should -BeFalse
  }
  It "rejects missing required translation units" {
    (Test-ClangdDatabase -RepoRoot $root -RequiredSources absent.cpp).ok | Should -BeFalse
  }
  It "rejects deleted source files" {
    Remove-Item -LiteralPath $source
    (Test-ClangdDatabase -RepoRoot $root -RequiredSources sample.cpp).ok | Should -BeFalse
  }
  It "rejects entries without a compile command" {
    $entry.Remove('command')
    ConvertTo-Json -InputObject @($entry) | Set-Content -LiteralPath $database
    (Test-ClangdDatabase -RepoRoot $root -RequiredSources sample.cpp).ok | Should -BeFalse
  }
  It "rejects newer build configuration" {
    $config = Join-Path $root "CMakeLists.txt"
    Set-Content -LiteralPath $config -Value 'project(test)'
    (Get-Item -LiteralPath $config).LastWriteTimeUtc = (Get-Item -LiteralPath $database).LastWriteTimeUtc.AddSeconds(10)
    (Test-ClangdDatabase -RepoRoot $root -RequiredSources sample.cpp).ok | Should -BeFalse
  }
  It "returns a nonzero CLI exit and structured error for an unprepared checkout" {
    $scripts = Join-Path $root "scripts"
    $null = New-Item -ItemType Directory -Path $scripts
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot "../doctor.ps1") -Destination $scripts
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot "../clangd-database.ps1") -Destination $scripts
    $output = & (Join-Path $PSHOME "pwsh.exe") -NoProfile -File (Join-Path $scripts "doctor.ps1") -Clangd -AsJson
    $LASTEXITCODE | Should -Be 1
    $report = $output | ConvertFrom-Json
    $report.status | Should -Be 'error'
    $report.checks[0].required | Should -BeTrue
  }
}
