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
    (Get-HostSelection -BasePath $script:basePath).Reason | Should -Be 'loader_probe_unavailable'
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

Describe "launcher diagnostics and ownership" {
  BeforeAll {
    $scripts = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
    . (Join-Path $scripts 'host-startup-log.ps1')
    $tokens = $null; $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile(
      (Join-Path $scripts 'start-installed-host.ps1'), [ref]$tokens, [ref]$errors)
    $fn = $ast.Find({ param($node)
      $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Invoke-InstalledHost'
    }, $false)
    . ([scriptblock]::Create($fn.Extent.Text))
    $supervisorAst = [Management.Automation.Language.Parser]::ParseFile(
      (Join-Path $scripts 'host-supervisor.ps1'), [ref]$tokens, [ref]$errors)
    $probeFn = $supervisorAst.Find({ param($node)
      $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Test-VulkanHost'
    }, $false)
    . ([scriptblock]::Create($probeFn.Extent.Text))
  }
  It "validates actual probe output, exit status, launch failure and timeout" {
    $probe = Join-Path $TestDrive 'probe.ps1'
    @'
param($OutputText, [int]$Code, [int]$Delay)
if ($Delay) { Start-Sleep -Seconds $Delay }
[Console]::Out.WriteLine($OutputText)
[Console]::Error.WriteLine('driver detail')
exit $Code
'@ | Set-Content $probe
    $shell = (Get-Process -Id $PID).Path
    foreach ($case in @(@('vulkan_devices=1', 0, $true), @('vulkan_devices=0', 1, $false),
        @('unexpected', 0, $false), @('vulkan_devices=1', 2, $false))) {
      Test-VulkanHost -Path $shell -Arguments "-NoProfile -File `"$probe`" -OutputText $($case[0]) -Code $($case[1])" |
        Should -Be $case[2]
      $script:selectionDetail | Should -Match 'driver detail'
    }
    Test-VulkanHost -Path $shell -Arguments "-NoProfile -File `"$probe`" -Delay 3" -TimeoutMilliseconds 100 |
      Should -BeFalse
    $script:probeReason | Should -Be 'probe_timeout'
    Test-VulkanHost -Path (Join-Path $TestDrive 'missing.exe') | Should -BeFalse
    $script:probeReason | Should -Be 'probe_launch_failed'
  }
  It "records registration mismatch without launching" {
    Mock Get-ItemProperty { $null }
    Invoke-InstalledHost -Directory $TestDrive -LocalData $TestDrive
    Get-Content (Join-Path $TestDrive 'azooKey/logs/host-launcher.log') | Should -Match 'registry_mismatch'
  }
  It "forwards process ownership and user logs and captures supervisor failures" {
    Mock Get-ItemProperty { [pscustomobject]@{ InstallDirectory = $TestDrive } }
    @'
param($HostExePath, $HostArguments, $StderrLogPath, $InstalledDirectory)
if ($HostArguments -ne "--pipe --supervisor-pid $PID") { throw 'PID missing' }
if ($InstalledDirectory -ne $PSScriptRoot) { throw 'ownership missing' }
if (-not $StderrLogPath.StartsWith($PSScriptRoot)) { throw 'wrong user log path' }
throw 'simulated supervisor failure'
'@ | Set-Content (Join-Path $TestDrive 'host-supervisor.ps1')
    { Invoke-InstalledHost -Directory $TestDrive -LocalData $TestDrive } | Should -Throw '*simulated supervisor failure*'
    Get-Content (Join-Path $TestDrive 'azooKey/logs/host-launcher.log') -Raw | Should -Match 'launcher_failed'
  }
  It "bounds diagnostics and tolerates unavailable storage" {
    $path = Join-Path $TestDrive 'diagnostics.log'
    ('x' * 1MB) | Set-Content $path
    Write-HostStartupLog -Path $path -EventName 'rotated'
    Test-Path ($path + '.1') | Should -BeTrue
    (Get-Item $path).Length | Should -BeLessThan 4096
    Mock Add-Content { throw 'disk full' }
    { Write-HostStartupLog -Path $path -EventName 'cannot_write' } | Should -Not -Throw
  }
  It "prunes only owned launch logs and preserves unrelated files" {
    1..23 | ForEach-Object { '' | Set-Content (Join-Path $TestDrive "host-20260908T120000000Z-$_.log") }
    '' | Set-Content (Join-Path $TestDrive 'unrelated.log')
    Remove-OldHostLaunchLog -BasePath (Join-Path $TestDrive 'host.log')
    @(Get-ChildItem $TestDrive -Filter 'host-*.log').Count | Should -Be 20
    Test-Path (Join-Path $TestDrive 'unrelated.log') | Should -BeTrue
  }
  It "never force-stops a data-bearing host" {
    $tokens = $null; $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile(
      (Join-Path $scripts 'host-supervisor.ps1'), [ref]$tokens, [ref]$errors)
    $kills = $ast.FindAll({ param($node)
      ($node -is [Management.Automation.Language.InvokeMemberExpressionAst] -and $node.Member.Value -eq 'Kill') -or
      ($node -is [Management.Automation.Language.CommandAst] -and $node.GetCommandName() -eq 'Stop-Process')
    }, $true)
    foreach ($kill in $kills) {
      $parent = $kill.Parent
      while ($parent -and $parent -isnot [Management.Automation.Language.FunctionDefinitionAst]) { $parent = $parent.Parent }
      $parent.Name | Should -Be 'Test-VulkanHost'
    }
  }
  It "stops terminal failures immediately and bounds repeated short crashes" {
    $child = Join-Path $TestDrive 'exit-host.ps1'
    @'
param($Counter, [int]$Code)
Add-Content -LiteralPath $Counter -Value 'started'
exit $Code
'@ | Set-Content $child
    foreach ($code in @(1, 2, 3)) {
      $counter = Join-Path $TestDrive "counter-$code.txt"
      & (Join-Path $scripts 'host-supervisor.ps1') -HostExePath (Get-Process -Id $PID).Path `
        -HostArguments "-NoProfile -File `"$child`" -Counter `"$counter`" -Code $code" `
        -PipeName "azookey-test-$([guid]::NewGuid())" -InstanceKey "test-$([guid]::NewGuid())" `
        -RestartDelayMinMs 1 -RestartDelayMaxMs 2 -StableRunSeconds 30
      @(Get-Content $counter).Count | Should -Be $(if ($code -eq 1) { 5 } else { 1 })
    }
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
