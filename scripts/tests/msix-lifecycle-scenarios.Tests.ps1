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

    $script:ifStatements = @($script:smokeAst.FindAll({
          param($node)
          $node -is [System.Management.Automation.Language.IfStatementAst]
        }, $true))
    $script:tryStatements = @($script:smokeAst.FindAll({
          param($node)
          $node -is [System.Management.Automation.Language.TryStatementAst]
        }, $true))
    $script:assignments = @($script:smokeAst.FindAll({
          param($node)
          $node -is [System.Management.Automation.Language.AssignmentStatementAst]
        }, $true))
    $script:functions = @($script:smokeAst.FindAll({
          param($node)
          $node -is [System.Management.Automation.Language.FunctionDefinitionAst]
        }, $true))
  }

  It "declares paired update and failed-update package inputs" {
    $parameterNames = @(
      $script:smokeAst.ParamBlock.Parameters |
        ForEach-Object { $_.Name.VariablePath.UserPath }
    )
    $pairGuard = @($script:ifStatements | Where-Object {
        $_.Clauses[0].Item1.Extent.Text -match '\$runLifecycleScenario' -and
        $_.Clauses[0].Item1.Extent.Text -match '\$UpdateMsixPath' -and
        $_.Clauses[0].Item1.Extent.Text -match '\$FailedUpdateMsixPath'
      })

    $parameterNames | Should -Contain "UpdateMsixPath"
    $parameterNames | Should -Contain "FailedUpdateMsixPath"
    $pairGuard | Should -HaveCount 1
    $pairGuard[0].Clauses[0].Item2.Extent.Text |
      Should -Match 'must be specified together'
  }

  It "refuses lifecycle scenarios when the target package is already installed" {
    $preGuard = @($script:ifStatements | Where-Object {
        $_.Clauses[0].Item1.Extent.Text.Trim() -eq '$pre' -and
        $_.Clauses[0].Item2.Extent.Text -match '\$runLifecycleScenario'
      })

    $preGuard | Should -HaveCount 1
    $preGuard[0].Clauses[0].Item2.Extent.Text | Should -Match 'require a clean VM'
  }

  It "requires an update to keep the package family and increase Version" {
    $updateGuard = @($script:ifStatements | Where-Object {
        $_.Clauses[0].Item1.Extent.Text -match
          '\$updatedPackage\.PackageFamilyName\s+-eq\s+\$baselineFamilyName' -and
        $_.Clauses[0].Item1.Extent.Text -match
          '\[version\]\$updatedPackage\.Version\s+-gt\s+\$baselineVersion'
      })

    $updateGuard | Should -HaveCount 1
    $script:smokeText | Should -Match '新版への update（上書き）が成功する'
  }

  It "requires the failure package to fail before checking rollback state" {
    $rollbackTry = @($script:tryStatements | Where-Object {
        $catchText = ($_.CatchClauses |
            ForEach-Object { $_.Extent.Text }) -join "`n"
        $_.Body.Extent.Text -match
          'Install-MsixPackage\s+-Path\s+\$FailedUpdateMsixPath' -and
        $catchText -match '\$failedAsExpected\s*=\s*\$true'
      })
    $rollbackStateRead = @($script:assignments | Where-Object {
        $_.Left.Extent.Text.Trim() -eq '$rollbackAfter'
      })

    $rollbackTry | Should -HaveCount 1
    $rollbackStateRead | Should -HaveCount 1
    ($rollbackTry[0].CatchClauses | ForEach-Object { $_.Extent.Text }) -join "`n" |
      Should -Match '\$failedAsExpected\s*=\s*\$true'
    ($rollbackTry[0].CatchClauses | ForEach-Object { $_.Extent.Text }) -join "`n" |
      Should -Match 'Get-ErrorDetail'
    $rollbackTry[0].Extent.EndOffset |
      Should -BeLessThan $rollbackStateRead[0].Extent.StartOffset
    $script:smokeText | Should -Match '失敗用 update がエラーになる'
  }

  It "sorts target package versions numerically" {
    $targetFunction = @($script:functions | Where-Object {
        $_.Name -eq "Get-TargetPackage"
      })

    $targetFunction | Should -HaveCount 1
    $targetFunction[0].Body.Extent.Text |
      Should -Match 'Sort-Object\s*\{\s*\[version\]\$_\.Version\s*\}'
  }

  It "compares both PackageFullName and Version after failed update" {
    $rollbackGuard = @($script:ifStatements | Where-Object {
        $_.Clauses[0].Item1.Extent.Text -match
          '\$rollbackAfter\.PackageFullName\s+-eq\s+\$rollbackBaseline\.PackageFullName' -and
        $_.Clauses[0].Item1.Extent.Text -match
          '\[version\]\$rollbackAfter\.Version\s+-eq\s+\[version\]\$rollbackBaseline\.Version'
      })

    $rollbackGuard | Should -HaveCount 1
    $script:smokeText |
      Should -Match '失敗した update 後も直前バージョンが保持される'
  }

  It "tracks packages within each Add-AppxPackage call" {
    $installFunction = @($script:functions | Where-Object {
        $_.Name -eq "Install-MsixPackage"
      })
    $installTry = @($installFunction[0].Body.FindAll({
          param($node)
          $node -is [System.Management.Automation.Language.TryStatementAst]
        }, $true))

    $installFunction | Should -HaveCount 1
    $installTry | Should -HaveCount 1
    $installTry[0].Finally.Extent.Text |
      Should -Match '\$script:IntroducedFullNames\.Add'
  }

  It "cleans up only tracked packages that remain after update attempts" {
    $cleanupTry = @($script:tryStatements | Where-Object {
        $_.Finally -and
        $_.Finally.Extent.Text -match 'Get-IntroducedPackage' -and
        $_.Finally.Extent.Text -match 'Remove-AppxPackage'
      })

    $cleanupTry | Should -HaveCount 1
    $cleanupTry[0].Finally.Extent.Text |
      Should -Not -Match 'BeforeFullNames'
  }

  It "writes report.json even when prerequisite validation throws" {
    $reportTry = @($script:tryStatements | Where-Object {
        $_.Finally -and $_.Finally.Extent.Text -match 'Write-Report' -and
        $_.Body.Extent.Text -match '\$runLifecycleScenario'
      })

    $reportTry | Should -HaveCount 1

    $reportDir = Join-Path $TestDrive "early-failure-report"
    {
      & $script:smokePath `
        -MsixPath (Join-Path $TestDrive "old.msix") `
        -UpdateMsixPath (Join-Path $TestDrive "update.msix") `
        -ReportDir $reportDir
    } | Should -Throw "*must be specified together*"

    $report = Get-Content -Raw -LiteralPath (Join-Path $reportDir "report.json") |
      ConvertFrom-Json
    $report.results.name | Should -Contain "update/rollback package を一組で指定する"
    ($report.results | Where-Object {
        $_.name -eq "update/rollback package を一組で指定する"
      }).status | Should -Be "FAIL"
  }

  It "records lifecycle package paths in report.json" {
    $script:smokeText | Should -Match 'updateMsixPath\s*=\s*\$UpdateMsixPath'
    $script:smokeText | Should -Match (
      'failedUpdateMsixPath\s*=\s*\$FailedUpdateMsixPath')
    $script:smokeText | Should -Match 'relatedIssues\s*=\s*@\("DEV-101", "DEV-586"\)'
  }
}
