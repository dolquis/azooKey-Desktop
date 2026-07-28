Describe "MSIX update and rollback smoke scenarios (DEV-586 / M28)" {
  BeforeAll {
    $repoRoot = (Resolve-Path (Join-Path (Join-Path $PSScriptRoot "..") "..")).Path
    $script:smokePath = Join-Path (Join-Path $repoRoot "compat-test") `
      "msix_install_uninstall.ps1"
    $script:smokeText = Get-Content -Raw -LiteralPath $script:smokePath

    $parseErrors = $null
    $script:smokeAst = [System.Management.Automation.Language.Parser]::ParseFile(
      $script:smokePath, [ref]$null, [ref]$parseErrors)
    if ($parseErrors.Count -gt 0) {
      throw "PowerShell parse errors in $($script:smokePath): $($parseErrors.Message -join '; ')"
    }
  }

  It "declares paired update and failed-update package inputs" {
    $parameterNames = @(
      $script:smokeAst.ParamBlock.Parameters |
        ForEach-Object { $_.Name.VariablePath.UserPath }
    )

    $parameterNames | Should -Contain "UpdateMsixPath"
    $parameterNames | Should -Contain "FailedUpdateMsixPath"
    $script:smokeText | Should -Match (
      '(?s)\$runLifecycleScenario.*\$UpdateMsixPath.*\$FailedUpdateMsixPath.*' +
      'must be specified together')
  }

  It "refuses lifecycle scenarios when the target package is already installed" {
    $script:smokeText | Should -Match (
      '(?s)if\s*\(\$pre\).*if\s*\(\$runLifecycleScenario\).*' +
      'require a clean VM')
  }

  It "requires an update to keep the package family and increase Version" {
    $script:smokeText | Should -Match (
      '(?s)\$updatedPackage\.PackageFamilyName\s+-eq\s+\$baselineFamilyName.*' +
      '\[version\]\$updatedPackage\.Version\s+-gt\s+\$baselineVersion')
    $script:smokeText | Should -Match '新版への update（上書き）が成功する'
  }

  It "requires the failure package to fail before checking rollback state" {
    $script:smokeText | Should -Match (
      '(?s)Install-MsixPackage\s+-Path\s+\$FailedUpdateMsixPath.*' +
      '\$failedAsExpected\s*=\s*\$true')
    $script:smokeText | Should -Match '失敗用 update がエラーになる'
  }

  It "compares both PackageFullName and Version after failed update" {
    $script:smokeText | Should -Match (
      '(?s)\$rollbackAfter\.PackageFullName\s+-eq\s+' +
      '\$rollbackBaseline\.PackageFullName.*' +
      '\[version\]\$rollbackAfter\.Version\s+-eq\s+' +
      '\[version\]\$rollbackBaseline\.Version')
    $script:smokeText | Should -Match '失敗した update 後も直前バージョンが保持される'
  }

  It "cleans up the package that remains after update attempts" {
    $script:smokeText | Should -Match (
      '(?s)finally\s*\{.*Get-IntroducedPackage\s+' +
      '-BeforeFullNames\s+\$beforeFullNames.*Remove-AppxPackage')
  }

  It "records lifecycle package paths in report.json" {
    $script:smokeText | Should -Match 'updateMsixPath\s*=\s*\$UpdateMsixPath'
    $script:smokeText | Should -Match (
      'failedUpdateMsixPath\s*=\s*\$FailedUpdateMsixPath')
    $script:smokeText | Should -Match 'relatedIssues\s*=\s*@\("DEV-101", "DEV-586"\)'
  }
}
