Describe "installed host startup contracts" {
  BeforeAll {
    $repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
    $supervisorPath = Join-Path $repoRoot "scripts/host-supervisor.ps1"
    $tokens = $null
    $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile($supervisorPath, [ref]$tokens, [ref]$errors)
    if ($errors.Count) { throw $errors[0] }
    foreach ($name in @("Test-InstallationActive", "Test-VulkanLoader", "Test-VulkanHost", "Get-HostSelection")) {
      $function = $ast.Find({ param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name
      }, $false)
      . ([scriptblock]::Create($function.Extent.Text))
    }
    $script:basePath = Join-Path $TestDrive "azookey_inference_host.exe"
    $script:addonPath = Join-Path $TestDrive "azookey_inference_host_vulkan.exe"
  }

  BeforeEach {
    $script:InstalledDirectory = "C:\Program Files\azooKey"
    Mock Test-Path { $true }
    Mock Test-VulkanLoader { $true }
    Mock Test-VulkanHost { $true }
  }

  It "leaves development registration independent of MSI" {
    $script:InstalledDirectory = ""
    Mock Get-ItemProperty { throw "Must not read MSI registry for development" }
    Test-InstallationActive -Directory $script:InstalledDirectory | Should -BeTrue
  }

  It "accepts only the matching machine registration" {
    Mock Get-ItemProperty { [pscustomobject]@{ InstallDirectory = 'c:\program files\azooKey\' } }
    Test-InstallationActive -Directory $script:InstalledDirectory | Should -BeTrue
    Mock Get-ItemProperty { [pscustomobject]@{ InstallDirectory = 'C:\Other' } }
    Test-InstallationActive -Directory $script:InstalledDirectory | Should -BeFalse
    Mock Get-ItemProperty { $null }
    Test-InstallationActive -Directory $script:InstalledDirectory | Should -BeFalse
  }

  It "selects base without probing when the add-on is absent" {
    Mock Test-Path { $false }
    $selection = Get-HostSelection -BasePath $script:basePath
    $selection.Path | Should -Be $script:basePath
    $selection.Reason | Should -Be "addon_absent"
    Should -Invoke Test-VulkanLoader -Times 0 -Exactly
    Should -Invoke Test-VulkanHost -Times 0 -Exactly
  }

  It "does not launch the probe without a loadable system loader" {
    Mock Test-VulkanLoader { $false }
    (Get-HostSelection -BasePath $script:basePath).Reason | Should -Be "loader_unavailable"
    Should -Invoke Test-VulkanHost -Times 0 -Exactly
  }

  It "uses base if loading the native helper fails" {
    Mock Test-VulkanLoader { throw "Restricted PowerShell" }
    (Get-HostSelection -BasePath $script:basePath).Path | Should -Be $script:basePath
    Should -Invoke Test-VulkanHost -Times 0 -Exactly
  }

  It "uses base after a failed device probe" {
    Mock Test-VulkanHost { $false }
    $selection = Get-HostSelection -BasePath $script:basePath
    $selection.Path | Should -Be $script:basePath
    $selection.Reason | Should -Be "probe_failed"
  }

  It "selects the add-on only after both checks and rechecks on each launch" {
    $selection = Get-HostSelection -BasePath $script:basePath
    $selection.Path | Should -Be $script:addonPath
    $selection.Reason | Should -Be "vulkan_devices_available"
    Mock Test-VulkanHost { $false }
    (Get-HostSelection -BasePath $script:basePath).Path | Should -Be $script:basePath
    Should -Invoke Test-VulkanLoader -Times 2 -Exactly
    Should -Invoke Test-VulkanHost -Times 2 -Exactly
  }
}

Describe "MSI shutdown waiter" {
  BeforeAll {
    $script:scripts = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
    $script:waiter = Join-Path $scripts "wait-installed-host.ps1"
    $script:installedHost = Join-Path $scripts "azookey_inference_host.exe"
    $script:launcherCommand = 'powershell.exe -NoProfile -File "' +
      (Join-Path $scripts "start-installed-host.ps1") + '"'
    $tokens = $null
    $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile($script:waiter, [ref]$tokens, [ref]$errors)
    $function = $ast.Find({ param($node)
      $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq "Wait-InstalledHostExit"
    }, $false)
    . ([scriptblock]::Create($function.Extent.Text))
  }

  It "ignores development processes outside the installed directory" {
    Mock Get-CimInstance {
      [pscustomobject]@{ ExecutablePath = 'C:\dev\azookey_inference_host.exe'; CommandLine = '' }
    }
    Mock Start-Sleep { throw "Must not wait for an unrelated host" }
    { Wait-InstalledHostExit -Directory $script:scripts -TimeoutSeconds 2 } | Should -Not -Throw
  }

  It "waits for the installed launcher even before it creates a host" {
    $script:pollCount = 0
    Mock Get-CimInstance {
      $script:pollCount++
      if ($script:pollCount -eq 1) {
        [pscustomobject]@{ ExecutablePath = 'C:\Windows\powershell.exe'; CommandLine = $script:launcherCommand }
      }
    }
    Mock Start-Sleep { }
    Wait-InstalledHostExit -Directory $script:scripts -TimeoutSeconds 2
    Should -Invoke Start-Sleep -Times 1 -Exactly
    Should -Invoke Get-CimInstance -Times 2 -Exactly
  }

  It "fails maintenance instead of killing a host that remains alive" {
    Mock Get-CimInstance {
      [pscustomobject]@{ ExecutablePath = $script:installedHost; CommandLine = '' }
    }
    Mock Stop-Process { throw "Must not kill the host" }
    { Wait-InstalledHostExit -Directory $script:scripts -TimeoutSeconds 1 } | Should -Throw '*did not exit*'
    Should -Invoke Stop-Process -Times 0 -Exactly
  }
}
