Describe "PowerShell quality module selection" {
  BeforeAll {
    $qualityPath = Join-Path (Join-Path $PSScriptRoot "..") "test-powershell-quality.ps1"
    $tokens = $null
    $parseErrors = $null
    $ast = [System.Management.Automation.Language.Parser]::ParseFile(
      $qualityPath, [ref]$tokens, [ref]$parseErrors)
    if ($parseErrors.Count) { throw "Cannot parse PowerShell quality wrapper." }
    $definition = $ast.Find({ param($node)
      $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
      $node.Name -eq "Get-RequiredModule"
    }, $true)
    . ([scriptblock]::Create($definition.Extent.Text))
  }

  BeforeEach {
    $script:availableModules = @(
      [pscustomobject]@{ Version = [version]"6.2.0"; PrivateData = @{ PSData = @{ Prerelease = "rc1" } }; Path = "preview" }
      [pscustomobject]@{ Version = [version]"6.2.0"; PrivateData = @{ PSData = @{} }; Path = "stable" }
      [pscustomobject]@{ Version = [version]"7.0.0"; PrivateData = @{}; Path = "newest" }
    )
    Mock Get-Module { $script:availableModules }
  }

  It "selects the stable exact version alongside a preview and a newer version" {
    (Get-RequiredModule Pester -RequiredVersion 6.2.0).Path | Should -Be "stable"
  }

  It "normalizes a two-part requested version" {
    (Get-RequiredModule Pester -RequiredVersion 6.2).Path | Should -Be "stable"
  }

  It "rejects a preview when the requested stable version is absent" {
    $script:availableModules = @($script:availableModules | Where-Object Path -ne "stable")
    { Get-RequiredModule Pester -RequiredVersion 6.2.0 } | Should -Throw "*6.2.0*not installed*"
  }

  It "keeps the newest-version default for local Pester and Analyzer" {
    (Get-RequiredModule Pester).Path | Should -Be "newest"
    (Get-RequiredModule PSScriptAnalyzer).Path | Should -Be "newest"
  }
}
