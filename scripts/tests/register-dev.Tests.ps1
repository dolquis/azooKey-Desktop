Describe "development registration scripts" {
  BeforeAll {
    $repoRoot = (Resolve-Path (Join-Path (Join-Path $PSScriptRoot "..") "..")).Path
    $registerPath = Join-Path (Join-Path $repoRoot "scripts") "register-dev.ps1"
    $unregisterPath = Join-Path (Join-Path $repoRoot "scripts") "unregister-dev.ps1"
    $supervisorPath = Join-Path (Join-Path $repoRoot "scripts") "host-supervisor.ps1"
    $aclPath = Join-Path (Join-Path $repoRoot "scripts") "AppContainerAcl.ps1"
    $qualityPath = Join-Path (Join-Path $repoRoot "scripts") "test-powershell-quality.ps1"
    $justfilePath = Join-Path $repoRoot "justfile"

    function Assert-Condition {
      param(
        [Parameter(Mandatory=$true)]
        [bool]$Condition,
        [Parameter(Mandatory=$true)]
        [string]$Message
      )

      if (-not $Condition) {
        throw $Message
      }
    }

    function Get-ParsedScript {
      param(
        [Parameter(Mandatory=$true)]
        [string]$Path
      )

      $tokens = $null
      $parseErrors = $null
      $ast = [System.Management.Automation.Language.Parser]::ParseFile(
        $Path,
        [ref]$tokens,
        [ref]$parseErrors)

      if ($parseErrors.Count -gt 0) {
        throw "PowerShell parse errors in $Path`: $($parseErrors.Message -join '; ')"
      }

      return @{
        Ast = $ast
        Text = Get-Content -Raw $Path
      }
    }

    function Get-ParameterName {
      param(
        [Parameter(Mandatory=$true)]
        [System.Management.Automation.Language.ScriptBlockAst]$Ast
      )

      return @($Ast.ParamBlock.Parameters | ForEach-Object {
        $_.Name.VariablePath.UserPath
      })
    }

    function Get-CommandText {
      param(
        [Parameter(Mandatory=$true)]
        [System.Management.Automation.Language.ScriptBlockAst]$Ast,
        [Parameter(Mandatory=$true)]
        [string]$CommandName
      )

      $commands = @($Ast.FindAll({
        param($node)
        $node -is [System.Management.Automation.Language.CommandAst]
      }, $true))

      return @($commands | Where-Object { $_.GetCommandName() -eq $CommandName } |
        ForEach-Object { $_.Extent.Text })
    }

    $script:register = Get-ParsedScript -Path $registerPath
    $script:unregister = Get-ParsedScript -Path $unregisterPath
    $script:supervisor = Get-ParsedScript -Path $supervisorPath
    $script:acl = Get-ParsedScript -Path $aclPath
    $script:aclPath = $aclPath
    $script:supervisorPath = $supervisorPath
    $script:qualityText = Get-Content -Raw $qualityPath
    $script:justfileText = Get-Content -Raw $justfilePath
    $script:registerParameters = Get-ParameterName -Ast $script:register.Ast
    $script:unregisterParameters = Get-ParameterName -Ast $script:unregister.Ast
  }

  Context "register-dev.ps1" {
    It "keeps explicit path parameters and elevated reentry separate" {
      Assert-Condition ($script:registerParameters -contains "TipDllPath") "register-dev.ps1 should expose TipDllPath."
      Assert-Condition ($script:registerParameters -contains "HostExePath") "register-dev.ps1 should expose HostExePath."
      Assert-Condition ($script:registerParameters -contains "ModelPath") "register-dev.ps1 should expose an explicit Zenzai model path."
      Assert-Condition ($script:registerParameters -contains "BenchPath") "register-dev.ps1 should accept a packaged llama.cpp preflight tool."
      Assert-Condition ($script:registerParameters -contains "MockDictionaryPath") "register-dev.ps1 should accept a packaged mock dictionary."
      Assert-Condition ($script:registerParameters -contains "AllowMockHost") "register-dev.ps1 should expose an explicit fallback-only override."
      Assert-Condition ($script:registerParameters -contains "ElevatedReentry") "register-dev.ps1 should expose ElevatedReentry."
      Assert-Condition ($script:registerParameters -contains "SkipAppContainerAcl") "register-dev.ps1 should expose an explicit AppContainer ACL opt-out."
    }

    It "grants AppContainer access to the TIP DLL before registering it" {
      Assert-Condition ($script:register.Text -match [regex]::Escape('. (Join-Path $PSScriptRoot "AppContainerAcl.ps1")')) "register-dev.ps1 should share the AppContainer ACL helpers."
      $grantIndex = $script:register.Text.IndexOf('Grant-TipAppContainerAccess -TipDllPath $TipDllPath')
      $regsvrIndex = $script:register.Text.IndexOf('regsvr32.exe')
      Assert-Condition ($grantIndex -ge 0) "register-dev.ps1 should grant AppContainer access to the TIP DLL."
      Assert-Condition ($grantIndex -lt $regsvrIndex) "register-dev.ps1 should grant AppContainer access before regsvr32 advertises the profile."
      Assert-Condition ($script:register.Text -match 'if\s*\(\s*\$SkipAppContainerAcl\s*\)\s*\{') "register-dev.ps1 should honor the AppContainer ACL opt-out."
      Assert-Condition ($script:register.Text -match [regex]::Escape('$relaunchArgs += " -SkipAppContainerAcl"')) "register-dev.ps1 should forward the AppContainer ACL opt-out to the elevated reentry."
    }

    It "defaults to the llama-enabled build and rejects an accidental mock host" {
      Assert-Condition ($script:register.Text -match [regex]::Escape('build\windows-llama-debug\tsf-tip\azookey_tsf_tip.dll')) "register-dev.ps1 should default to the llama-enabled TIP build."
      Assert-Condition ($script:register.Text -match [regex]::Escape('build\windows-llama-debug\inference-host\azookey_inference_host.exe')) "register-dev.ps1 should default to the llama-enabled host build."
      Assert-Condition ($script:register.Text -match 'azookey_zenzai_bench\.exe') "register-dev.ps1 should probe the Zenzai bench compiled with the host."
      Assert-Condition ($script:register.Text -match 'llama_cpp=1') "register-dev.ps1 should require a real llama.cpp runtime."
      Assert-Condition ($script:register.Text -match 'Assert-LlamaEnabledHost[\s\S]{0,160}-Path\s+\$HostExePath') "register-dev.ps1 should run the llama.cpp preflight before registration."
    }

    It "warns when registration resolves to Debug CRT artifacts" {
      $warningFunction = $script:register.Ast.Find({
        param($node)
        $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
          $node.Name -eq "Write-DebugCrtWarning"
      }, $true)
      Assert-Condition ($null -ne $warningFunction) "register-dev.ps1 should define the Debug CRT warning check."
      . ([scriptblock]::Create($warningFunction.Extent.Text))

      $debugTip = "C:\repo\build\windows-llama-debug\tsf-tip\azookey_tsf_tip.dll"
      $debugHost = "C:\repo\build\WINDOWS-DEBUG\inference-host\azookey_inference_host.exe"
      $debugWarnings = @()
      Write-DebugCrtWarning `
        -Paths @($debugTip, $debugHost) `
        -WarningVariable debugWarnings `
        -WarningAction SilentlyContinue
      Assert-Condition ($debugWarnings.Count -eq 1) "Debug TIP/Host paths should emit one warning."
      Assert-Condition ($debugWarnings[0].Message.Contains($debugTip)) "The warning should identify the Debug TIP path."
      Assert-Condition ($debugWarnings[0].Message.Contains($debugHost)) "The warning should identify the Debug host path."

      $releaseWarnings = @()
      Write-DebugCrtWarning `
        -Paths @("C:\repo\build\windows-release\tsf-tip\azookey_tsf_tip.dll") `
        -WarningVariable releaseWarnings `
        -WarningAction SilentlyContinue
      Assert-Condition ($releaseWarnings.Count -eq 0) "Release artifacts should not emit a Debug CRT warning."

      $packagedWarnings = @()
      Write-DebugCrtWarning `
        -Paths @("C:\vm-verify\azookey_tsf_tip.dll") `
        -WarningVariable packagedWarnings `
        -WarningAction SilentlyContinue
      Assert-Condition ($packagedWarnings.Count -eq 0) "A copied artifact without build metadata should not be guessed as Debug."

      $asanBuild = Join-Path $TestDrive "build\windows-asan"
      $asanHostDir = Join-Path $asanBuild "inference-host"
      New-Item -ItemType Directory -Path $asanHostDir -Force | Out-Null
      "CMAKE_BUILD_TYPE:STRING=Debug" |
        Set-Content -LiteralPath (Join-Path $asanBuild "CMakeCache.txt") -Encoding UTF8
      $asanWarnings = @()
      Write-DebugCrtWarning `
        -Paths @((Join-Path $asanHostDir "azookey_inference_host.exe")) `
        -WarningVariable asanWarnings `
        -WarningAction SilentlyContinue
      Assert-Condition ($asanWarnings.Count -eq 1) "CMake Debug metadata should warn even when the build directory name omits debug."
    }

    It "warns before relaunching elevated in the original console" {
      $warnIndex = $script:register.Text.IndexOf('Write-DebugCrtWarning -Paths')
      $relaunchIndex = $script:register.Text.IndexOf('-Verb RunAs')
      Assert-Condition ($warnIndex -ge 0 -and $warnIndex -lt $relaunchIndex) "register-dev.ps1 should warn about Debug CRT before relaunching elevated."
      Assert-Condition ($script:register.Text -match 'if\s*\(\s*-not\s+\$ElevatedReentry\s*\)\s*\{\s*Write-DebugCrtWarning') "register-dev.ps1 should emit the warning only from the original non-elevated process."
    }

    It "runs the llama.cpp linkage probe without loading the configured Zenzai model" {
      Assert-Condition ($script:register.Text -match [regex]::Escape('[Environment]::GetEnvironmentVariable("AZOOKEY_ZENZAI_MODEL", "Process")')) "register-dev.ps1 should preserve the configured Zenzai model path."
      Assert-Condition ($script:register.Text -match [regex]::Escape('[Environment]::SetEnvironmentVariable("AZOOKEY_ZENZAI_MODEL", $null, "Process")')) "register-dev.ps1 should clear the Zenzai model path before probing linkage."
      Assert-Condition ($script:register.Text -match 'finally\s*\{[\s\S]*SetEnvironmentVariable\("AZOOKEY_ZENZAI_MODEL",\s*\$zenzaiModel,\s*"Process"\)') "register-dev.ps1 should restore the Zenzai model path after probing linkage."
    }

    It "passes an explicit GGUF model to both current-session and auto-start hosts" {
      Assert-Condition ($script:register.Text -match 'Test-Path\s+-LiteralPath\s+\$ModelPath\s+-PathType\s+Leaf') "register-dev.ps1 should require ModelPath to be an existing file."
      Assert-Condition ($script:register.Text -match 'GetExtension\(\$ModelPath\)\s+-ine\s+"\.gguf"') "register-dev.ps1 should reject non-GGUF model paths."
      Assert-Condition ($script:register.Text -match [regex]::Escape('-Value "`"$powerShellExe`" $supervisorArguments"')) "register-dev.ps1 should persist the supervisor command in the HKCU Run value."
      Assert-Condition ($script:register.Text -match 'FilePath\s*=\s*\$powerShellExe') "register-dev.ps1 should launch the supervisor with the current PowerShell executable."
      Assert-Condition ($script:register.Text -match 'ArgumentList\s*=\s*\$supervisorArguments') "register-dev.ps1 should pass the model-bearing arguments to the current-session supervisor."
      Assert-Condition ($script:register.Text -match [regex]::Escape('-ModelPath cannot be combined with -AllowMockHost')) "register-dev.ps1 should reject misleading real-model registration on a mock host."
      $existingHostGuardIndex = $script:register.Text.IndexOf('if ($ModelPath -and $hostServing)')
      $runRegistrationIndex = $script:register.Text.IndexOf('New-ItemProperty -Path $runKey')
      Assert-Condition ($existingHostGuardIndex -ge 0 -and $existingHostGuardIndex -lt $runRegistrationIndex) "register-dev.ps1 should reject an existing current-session host before changing the Run entry."
    }

    It "passes an explicit mock dictionary to the per-user supervisor" {
      Assert-Condition ($script:register.Text -match 'Test-Path\s+-LiteralPath\s+\$MockDictionaryPath\s+-PathType\s+Leaf') "register-dev.ps1 should require MockDictionaryPath to be an existing file."
      Assert-Condition ($script:register.Text -match 'GetExtension\(\$MockDictionaryPath\)\s+-ine\s+"\.tsv"') "register-dev.ps1 should reject non-TSV mock dictionaries."
      Assert-Condition ($script:register.Text -match [regex]::Escape('$supervisorArguments += " -MockDictionaryPath `"$MockDictionaryPath`""')) "register-dev.ps1 should pass the mock dictionary to the per-user supervisor."
    }

    It "registers and starts a per-user supervisor with host diagnostics" {
      Assert-Condition ($script:register.Text -match 'Join-Path\s+\(Join-Path\s+\$env:LOCALAPPDATA\s+"azooKey"\)\s+"logs"') "register-dev.ps1 should use a per-user log directory."
      Assert-Condition ($script:register.Text -match 'inference-host-stderr\.log') "register-dev.ps1 should name the inference host stderr log."
      Assert-Condition ($script:register.Text -match 'host-supervisor\.ps1') "register-dev.ps1 should use the host supervisor."
      Assert-Condition ($script:register.Text -match [regex]::Escape('-ModelPath `"$ModelPath`"')) "register-dev.ps1 should forward an explicit model path to the supervisor."
      Assert-Condition ($script:register.Text -match [regex]::Escape('-PipeName `"$myPipe`"')) "register-dev.ps1 should supervise the exact current-user pipe."
      Assert-Condition ($script:register.Text -match 'RedirectStandardError\s*=\s*\$supervisorStderrLog') "register-dev.ps1 should redirect hidden supervisor stderr."
      Assert-Condition ($script:register.Text -match 'Start-Process\s+@startParameters') "register-dev.ps1 should launch the host with the diagnostic redirection parameters."
    }

    It "keeps the just registration recipes on the llama-enabled preset" {
      Assert-Condition ($script:justfileText -match '(?m)^llama_preset := "windows-llama-debug"\r?$') "justfile should define the llama-enabled registration preset."
      Assert-Condition ($script:justfileText -match '(?m)^register preset=llama_preset:\r?$') "just register should default to the llama-enabled build."
      Assert-Condition ($script:justfileText -match '(?m)^unregister preset=llama_preset:\r?$') "just unregister should default to the same llama-enabled build."
    }

    It "guards per-user HKCU setup from elevated reentry" {
      Assert-Condition ($script:register.Text -match 'if\s*\(\s*-not\s+\$ElevatedReentry\s*\)\s*\{') "register-dev.ps1 should guard HKCU setup with ElevatedReentry."
      Assert-Condition ($script:register.Text -match [regex]::Escape('HKCU:\Software\Microsoft\Windows\CurrentVersion\Run')) "register-dev.ps1 should write the HKCU Run key only in the per-user step."
    }

    It "relaunches machine-wide registration with quoted absolute paths" {
      Assert-Condition ($script:register.Text -match 'Start-Process\s+-FilePath\s+"powershell\.exe"\s+-Verb\s+RunAs') "register-dev.ps1 should relaunch with RunAs."
      Assert-Condition ($script:register.Text -match [regex]::Escape('-TipDllPath `"$TipDllPath`"')) "register-dev.ps1 should forward a quoted TipDllPath."
      Assert-Condition ($script:register.Text -match [regex]::Escape('-HostExePath `"$HostExePath`"')) "register-dev.ps1 should forward a quoted HostExePath."
      Assert-Condition ($script:register.Text -match "-ElevatedReentry") "register-dev.ps1 should set ElevatedReentry on relaunch."
    }

    It "waits for elevated registration and propagates its exit code" {
      $elevationBranch = @($script:register.Ast.FindAll({
        param($node)
        $node -is [System.Management.Automation.Language.IfStatementAst] -and
          $node.Extent.Text -match 'Start-Process[\s\S]*-Verb\s+RunAs'
      }, $true))
      Assert-Condition ($elevationBranch.Count -eq 1) "register-dev.ps1 should have exactly one RunAs elevation branch."

      $elevationText = $elevationBranch[0].Extent.Text
      Assert-Condition ($elevationText -match 'Start-Process[\s\S]*-Wait[\s\S]*-PassThru') "register-dev.ps1 should wait for the elevated registration process."
      Assert-Condition ($elevationText -notmatch '-NoExit') "register-dev.ps1 should let the elevated registration process exit."
      Assert-Condition ($elevationText -match '\$elevatedProcess\s*=\s*Start-Process') "register-dev.ps1 should capture the elevated registration process."
      Assert-Condition ($elevationText -match 'exit\s+\$elevatedProcess\.ExitCode') "register-dev.ps1 should propagate the elevated registration exit code."
    }

    It "uses silent regsvr32 and fails on registration errors" {
      $commands = (Get-CommandText -Ast $script:register.Ast -CommandName "Start-Process") -join "`n"
      Assert-Condition ($commands -match 'regsvr32\.exe') "register-dev.ps1 should call regsvr32.exe."
      Assert-Condition ($commands -match '/s') "register-dev.ps1 should call regsvr32 silently."
      Assert-Condition ($script:register.Text -match 'ExitCode\s+-ne\s+0') "register-dev.ps1 should check regsvr32 exit code."
      Assert-Condition ($script:register.Text -match 'throw\s+"regsvr32 failed') "register-dev.ps1 should throw on regsvr32 failure."
    }
  }

  Context "unregister-dev.ps1" {
    It "keeps explicit path and elevated reentry parameters" {
      Assert-Condition ($script:unregisterParameters -contains "TipDllPath") "unregister-dev.ps1 should expose TipDllPath."
      Assert-Condition ($script:unregisterParameters -contains "ElevatedReentry") "unregister-dev.ps1 should expose ElevatedReentry."
      Assert-Condition ($script:unregisterParameters -contains "SkipAppContainerAcl") "unregister-dev.ps1 should expose an explicit AppContainer ACL opt-out."
    }

    It "takes the AppContainer grant back off the TIP DLL" {
      Assert-Condition ($script:unregister.Text -match [regex]::Escape('. (Join-Path $PSScriptRoot "AppContainerAcl.ps1")')) "unregister-dev.ps1 should share the AppContainer ACL helpers."
      Assert-Condition ($script:unregister.Text -match [regex]::Escape('Revoke-TipAppContainerAccess -TipDllPath $TipDllPath')) "unregister-dev.ps1 should revoke the AppContainer grant."
      Assert-Condition ($script:unregister.Text -match 'if\s*\(\s*-not\s+\$SkipAppContainerAcl\s*\)\s*\{') "unregister-dev.ps1 should honor the AppContainer ACL opt-out."
      Assert-Condition ($script:unregister.Text -match [regex]::Escape('$relaunchArgs += " -SkipAppContainerAcl"')) "unregister-dev.ps1 should forward the AppContainer ACL opt-out to the elevated reentry."
    }

    It "guards per-user HKCU cleanup from elevated reentry" {
      Assert-Condition ($script:unregister.Text -match 'if\s*\(\s*-not\s+\$ElevatedReentry\s*\)\s*\{') "unregister-dev.ps1 should guard HKCU cleanup with ElevatedReentry."
      Assert-Condition ($script:unregister.Text -match [regex]::Escape('HKCU:\Software\Microsoft\Windows\CurrentVersion\Run')) "unregister-dev.ps1 should clean the HKCU Run key."
      Assert-Condition ($script:unregister.Text -match 'Global\\azooKeyInferenceHostSupervisorStop-\$mySid') "unregister-dev.ps1 should signal the current-user supervisor across sessions."
      Assert-Condition ($script:unregister.Text -match [regex]::Escape('HKCU:\Software\Classes\CLSID\$clsid')) "unregister-dev.ps1 should clean legacy HKCU CLSID entries."
    }

    It "relaunches machine-wide cleanup with quoted absolute paths" {
      Assert-Condition ($script:unregister.Text -match 'Start-Process\s+-FilePath\s+"powershell\.exe"\s+-Verb\s+RunAs') "unregister-dev.ps1 should relaunch with RunAs."
      Assert-Condition ($script:unregister.Text -match [regex]::Escape('-TipDllPath `"$TipDllPath`"')) "unregister-dev.ps1 should forward a quoted TipDllPath."
      Assert-Condition ($script:unregister.Text -match "-ElevatedReentry") "unregister-dev.ps1 should set ElevatedReentry on relaunch."
    }

    It "uses silent regsvr32 unregister and removes HKLM TSF leftovers" {
      $commands = (Get-CommandText -Ast $script:unregister.Ast -CommandName "Start-Process") -join "`n"
      Assert-Condition ($commands -match 'regsvr32\.exe') "unregister-dev.ps1 should call regsvr32.exe."
      Assert-Condition ($commands -match '/u /s') "unregister-dev.ps1 should call regsvr32 unregister silently."
      Assert-Condition ($script:unregister.Text -match [regex]::Escape('HKLM:\Software\Microsoft\CTF\TIP\$clsid')) "unregister-dev.ps1 should clean HKLM CTF TIP leftovers."
      Assert-Condition ($script:unregister.Text -match [regex]::Escape('HKLM:\Software\WOW6432Node\Microsoft\CTF\TIP\$clsid')) "unregister-dev.ps1 should clean WOW6432Node CTF TIP leftovers."
    }
  }

  Context "AppContainerAcl.ps1" {
    BeforeAll {
      . $script:aclPath
      $script:allApplicationPackages = "S-1-15-2-1"
      $script:onWindows = ($PSVersionTable.PSEdition -eq "Desktop") -or $IsWindows
      $script:readAndExecute = [System.Security.AccessControl.FileSystemRights]::ReadAndExecute

      function Get-AppContainerRule {
        param(
          [Parameter(Mandatory=$true)]
          [string]$Path,
          [switch]$IncludeInherited
        )

        return @((Get-Acl -LiteralPath $Path).GetAccessRules(
            $true,
            [bool]$IncludeInherited,
            [Security.Principal.SecurityIdentifier]) |
          Where-Object { $_.IdentityReference.Value -eq $script:allApplicationPackages })
      }

      function Initialize-AclSandbox {
        $directory = Join-Path $TestDrive ([guid]::NewGuid().ToString("N"))
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
        return $directory
      }

      # The grant ledger lives under HKLM in production. Tests point it at a
      # throwaway HKCU key so they neither need elevation nor touch machine state.
      function Initialize-TestLedgerKey {
        return "HKCU:\Software\azooKey\Tests\$([guid]::NewGuid().ToString('N'))"
      }

      function Add-ExplicitAppContainerRule {
        param(
          [Parameter(Mandatory=$true)]
          [string]$Path,
          [Parameter(Mandatory=$true)]
          [System.Security.AccessControl.FileSystemRights]$Rights,
          [System.Security.AccessControl.InheritanceFlags]$Inheritance =
            [System.Security.AccessControl.InheritanceFlags]::None
        )

        $acl = Get-Acl -LiteralPath $Path
        $acl.AddAccessRule((New-Object System.Security.AccessControl.FileSystemAccessRule(
          (New-Object Security.Principal.SecurityIdentifier($script:allApplicationPackages)),
          $Rights,
          $Inheritance,
          [System.Security.AccessControl.PropagationFlags]::None,
          [System.Security.AccessControl.AccessControlType]::Allow)))
        Set-Acl -LiteralPath $Path -AclObject $acl
      }
    }

    AfterAll {
      $testRoot = "HKCU:\Software\azooKey\Tests"
      if ((($PSVersionTable.PSEdition -eq "Desktop") -or $IsWindows) -and
          (Test-Path -LiteralPath $testRoot)) {
        Remove-Item -LiteralPath $testRoot -Recurse -Force -ErrorAction SilentlyContinue
      }
    }

    It "is included in the canonical PowerShell quality gate" {
      Assert-Condition ($script:qualityText -match 'Join-Path\s+\$PSScriptRoot\s+"AppContainerAcl\.ps1"') "test-powershell-quality.ps1 should lint the AppContainer ACL helpers."
    }

    It "targets the well-known ALL APPLICATION PACKAGES SID with read+execute only" {
      Assert-Condition ($script:acl.Text -match [regex]::Escape('SecurityIdentifier("S-1-15-2-1")')) "AppContainerAcl.ps1 should use the well-known ALL APPLICATION PACKAGES SID."
      Assert-Condition ($script:acl.Text -match 'FileSystemRights\]::ReadAndExecute') "AppContainerAcl.ps1 should grant read+execute."
      Assert-Condition ($script:acl.Text -notmatch 'FileSystemRights\]::(Write|Modify|FullControl)') "AppContainerAcl.ps1 should never grant write access to AppContainer apps."
      Assert-Condition ($script:acl.Text -match 'RemoveAccessRuleSpecific') "AppContainerAcl.ps1 should remove only the exact ACE a grant installs."
      Assert-Condition ($script:acl.Text -match 'function Revoke-TipAppContainerAccess[\s\S]*?Get-AppContainerGrantLedger') "Revoke-TipAppContainerAccess should consult the grant ledger before touching any DACL."
    }

    It "grants read+execute idempotently and revokes it again" {
      if (-not $script:onWindows) {
        Set-ItResult -Skipped -Because "file ACLs are a Windows concept"
      }

      $dllPath = Join-Path (Initialize-AclSandbox) "azookey_tsf_tip.dll"
      Set-Content -LiteralPath $dllPath -Value "stand-in for the TIP DLL"

      Assert-Condition ((Grant-AppContainerReadExecute -Path $dllPath) -eq $true) "The first grant should rewrite the DACL."
      Assert-Condition ((Grant-AppContainerReadExecute -Path $dllPath) -eq $false) "A repeated grant should report no change."

      $rules = Get-AppContainerRule -Path $dllPath
      Assert-Condition ($rules.Count -eq 1) "Exactly one explicit ALL APPLICATION PACKAGES ACE should remain."
      Assert-Condition (([int]$rules[0].FileSystemRights -band [int]$script:readAndExecute) -eq [int]$script:readAndExecute) "The ACE should grant read+execute."
      Assert-Condition (([int]$rules[0].FileSystemRights -band [int][System.Security.AccessControl.FileSystemRights]::Write) -eq 0) "The ACE should not grant write access."
      Assert-Condition ($rules[0].AccessControlType -eq [System.Security.AccessControl.AccessControlType]::Allow) "The ACE should be an allow entry."

      Assert-Condition ((Revoke-AppContainerReadExecute -Path $dllPath) -eq $true) "Revoke should remove the explicit ACE."
      Assert-Condition ((Get-AppContainerRule -Path $dllPath).Count -eq 0) "No explicit ALL APPLICATION PACKAGES ACE should survive the revoke."
      Assert-Condition ((Revoke-AppContainerReadExecute -Path $dllPath) -eq $false) "A repeated revoke should report no change."
    }

    It "makes the directory grant inheritable so a rebuilt DLL keeps access" {
      if (-not $script:onWindows) {
        Set-ItResult -Skipped -Because "file ACLs are a Windows concept"
      }

      $directory = Initialize-AclSandbox
      Assert-Condition ((Grant-AppContainerReadExecute -Path $directory) -eq $true) "The directory grant should rewrite the DACL."

      # The DLL is written *after* the grant, standing in for a rebuild that
      # replaces the file: it must pick the ACE up by inheritance.
      $dllPath = Join-Path $directory "azookey_tsf_tip.dll"
      Set-Content -LiteralPath $dllPath -Value "rebuilt TIP DLL"

      Assert-Condition ((Get-AppContainerRule -Path $dllPath -IncludeInherited).Count -ge 1) "A rebuilt DLL should inherit the AppContainer grant."
      Assert-Condition ((Grant-AppContainerReadExecute -Path $dllPath) -eq $false) "An inherited grant should count as already effective."
    }

    It "round-trips the registration grant over the DLL and its directory" {
      if (-not $script:onWindows) {
        Set-ItResult -Skipped -Because "file ACLs are a Windows concept"
      }

      $directory = Initialize-AclSandbox
      $dllPath = Join-Path $directory "azookey_tsf_tip.dll"
      Set-Content -LiteralPath $dllPath -Value "stand-in for the TIP DLL"
      $ledgerKey = Initialize-TestLedgerKey

      # Asserted through the effective state rather than the ACE count: adding an
      # inheritable ACE to the directory propagates to the existing DLL, so the
      # DLL may end up covered by inheritance instead of an explicit entry.
      Grant-TipAppContainerAccess -TipDllPath $dllPath -LedgerKey $ledgerKey
      Assert-Condition ((Get-AppContainerRule -Path $directory).Count -eq 1) "Registration should grant the containing directory."
      Assert-Condition ((Get-AppContainerAccessState -Acl (Get-Acl -LiteralPath $dllPath) -Rights $script:readAndExecute) -eq "Allowed") "Registration should leave the DLL readable by AppContainer apps."
      Assert-Condition ((Get-AppContainerGrantLedger -LedgerKey $ledgerKey).Count -ge 1) "Registration should record what it granted."

      Revoke-TipAppContainerAccess -TipDllPath $dllPath -LedgerKey $ledgerKey
      Assert-Condition ((Get-AppContainerRule -Path $directory -IncludeInherited).Count -eq 0) "Unregistration should leave no grant on the directory."
      Assert-Condition ((Get-AppContainerAccessState -Acl (Get-Acl -LiteralPath $dllPath) -Rights $script:readAndExecute) -eq "Missing") "Unregistration should leave no grant on the DLL."
      Assert-Condition ((Get-AppContainerGrantLedger -LedgerKey $ledgerKey).Count -eq 0) "Unregistration should clear the ledger."
      Assert-Condition (-not (Test-Path -LiteralPath $ledgerKey)) "An emptied ledger key should not be left behind."
    }

    It "keeps the ledger entry when a revoke fails" {
      if (-not $script:onWindows) {
        Set-ItResult -Skipped -Because "file ACLs are a Windows concept"
      }

      $directory = Initialize-AclSandbox
      $dllPath = Join-Path $directory "azookey_tsf_tip.dll"
      Set-Content -LiteralPath $dllPath -Value "stand-in for the TIP DLL"
      $ledgerKey = Initialize-TestLedgerKey

      Grant-TipAppContainerAccess -TipDllPath $dllPath -LedgerKey $ledgerKey
      $before = @(Get-AppContainerGrantLedger -LedgerKey $ledgerKey)
      Assert-Condition ($before.Count -ge 1) "Registration should record what it granted."

      # Shadow the per-path revoke so every attempt fails the way a locked or
      # permission-denied DACL write would. Scoped to this It block.
      function Revoke-AppContainerReadExecute {
        param(
          [Parameter(Mandatory=$true)]
          [string]$Path
        )

        throw "simulated DACL write failure on $Path"
      }

      Revoke-TipAppContainerAccess -TipDllPath $dllPath -LedgerKey $ledgerKey -WarningAction SilentlyContinue

      $after = @(Get-AppContainerGrantLedger -LedgerKey $ledgerKey)
      Assert-Condition ($after.Count -eq $before.Count) "A failed revoke should keep the ledger entry so unregistration can retry."
      Assert-Condition ((Get-AppContainerRule -Path $directory).Count -eq 1) "The ACE should still be present after a failed revoke."
    }

    It "adds the inheritable ACE even when the directory already has a non-inheritable one" {
      if (-not $script:onWindows) {
        Set-ItResult -Skipped -Because "file ACLs are a Windows concept"
      }

      # Effective read+execute on the directory says nothing about what a file
      # created later inherits, so a non-inheritable ACE must not be mistaken for
      # coverage — the next rebuild would otherwise lose access.
      $directory = Initialize-AclSandbox
      Add-ExplicitAppContainerRule -Path $directory -Rights $script:readAndExecute
      Assert-Condition ((Get-AppContainerAccessState -Acl (Get-Acl -LiteralPath $directory) -Rights $script:readAndExecute) -eq "Allowed") "The directory should already be readable."

      Assert-Condition ((Grant-AppContainerReadExecute -Path $directory) -eq $true) "A non-inheritable ACE should not count as coverage for future rebuilds."

      $dllPath = Join-Path $directory "azookey_tsf_tip.dll"
      Set-Content -LiteralPath $dllPath -Value "rebuilt TIP DLL"
      Assert-Condition ((Get-AppContainerAccessState -Acl (Get-Acl -LiteralPath $dllPath) -Rights $script:readAndExecute) -eq "Allowed") "A DLL created after the grant should inherit the access."
    }

    It "keeps ACEs that predate registration" {
      if (-not $script:onWindows) {
        Set-ItResult -Skipped -Because "file ACLs are a Windows concept"
      }

      $directory = Initialize-AclSandbox
      $dllPath = Join-Path $directory "azookey_tsf_tip.dll"
      Set-Content -LiteralPath $dllPath -Value "stand-in for the TIP DLL"
      $ledgerKey = Initialize-TestLedgerKey

      # Both paths are covered before registration runs, so registration adds —
      # and records — nothing. Unregistration must then leave both alone.
      Add-ExplicitAppContainerRule -Path $directory -Rights $script:readAndExecute `
        -Inheritance ([System.Security.AccessControl.InheritanceFlags]::ContainerInherit -bor
          [System.Security.AccessControl.InheritanceFlags]::ObjectInherit)
      Add-ExplicitAppContainerRule -Path $dllPath -Rights $script:readAndExecute

      Grant-TipAppContainerAccess -TipDllPath $dllPath -LedgerKey $ledgerKey
      Assert-Condition ((Get-AppContainerGrantLedger -LedgerKey $ledgerKey).Count -eq 0) "A no-op registration should record nothing."

      Revoke-TipAppContainerAccess -TipDllPath $dllPath -LedgerKey $ledgerKey
      Assert-Condition ((Get-AppContainerRule -Path $directory).Count -eq 1) "A pre-existing directory ACE should survive unregistration."
      Assert-Condition ((Get-AppContainerRule -Path $dllPath).Count -eq 1) "A pre-existing DLL ACE should survive unregistration."
    }

    It "removes only the exact ACE a grant installs" {
      if (-not $script:onWindows) {
        Set-ItResult -Skipped -Because "file ACLs are a Windows concept"
      }

      # A directory, so the granted ACE (ContainerInherit|ObjectInherit) and the
      # hand-set one (no inheritance) stay distinct entries instead of being
      # merged into a single ACE by AddAccessRule.
      $directory = Initialize-AclSandbox
      Assert-Condition ((Grant-AppContainerReadExecute -Path $directory) -eq $true) "The grant should install its ACE."
      Add-ExplicitAppContainerRule -Path $directory `
        -Rights ([System.Security.AccessControl.FileSystemRights]::Read)
      Assert-Condition ((Get-AppContainerRule -Path $directory).Count -eq 2) "The hand-set ACE should be a separate entry."

      Assert-Condition ((Revoke-AppContainerReadExecute -Path $directory) -eq $true) "Revoke should remove the granted ACE."
      $survivors = Get-AppContainerRule -Path $directory
      Assert-Condition ($survivors.Count -eq 1) "A hand-set ACE with other rights should survive the revoke."
      Assert-Condition (([int]$survivors[0].FileSystemRights -band [int][System.Security.AccessControl.FileSystemRights]::ExecuteFile) -eq 0) "The surviving ACE should be the hand-set read-only one."
      Assert-Condition ($survivors[0].InheritanceFlags -eq [System.Security.AccessControl.InheritanceFlags]::None) "The surviving ACE should keep its own inheritance flags."
    }

    It "never rewrites ACLs under protected system directories" {
      if (-not $script:onWindows) {
        Set-ItResult -Skipped -Because "the protected roots are Windows paths"
      }

      Assert-Condition (Test-ProtectedSystemPath -Path (Join-Path $env:ProgramFiles "azooKey\azookey_tsf_tip.dll")) "An MSI install location should be protected."
      Assert-Condition (Test-ProtectedSystemPath -Path $env:SystemRoot) "%SystemRoot% should be protected."
      Assert-Condition (-not (Test-ProtectedSystemPath -Path (Initialize-AclSandbox))) "A development build tree should not be protected."
      # Asserted statically rather than by calling Revoke against a real system
      # path: a regression here would rewrite %SystemRoot% on the test machine.
      Assert-Condition ($script:acl.Text -match 'function Revoke-AppContainerReadExecute[\s\S]*?Test-ProtectedSystemPath[\s\S]*?Get-Acl') "Revoke-AppContainerReadExecute should refuse protected system paths before touching the DACL."
    }

    It "refuses to grant access to a missing path" {
      if (-not $script:onWindows) {
        Set-ItResult -Skipped -Because "file ACLs are a Windows concept"
      }

      $missing = Join-Path (Initialize-AclSandbox) "absent.dll"
      $threw = $false
      try {
        Grant-AppContainerReadExecute -Path $missing | Out-Null
      } catch {
        $threw = $true
      }
      Assert-Condition $threw "Granting a missing path should throw instead of silently succeeding."
    }
  }

  Context "host-supervisor.ps1" {
    It "is included in the canonical PowerShell quality gate" {
      Assert-Condition ($script:qualityText -match 'Join-Path\s+\$PSScriptRoot\s+"host-supervisor\.ps1"') "test-powershell-quality.ps1 should lint the supervisor."
    }

    It "serializes supervision per user and uses a bounded restart backoff" {
      Assert-Condition ($script:supervisor.Text -match 'Global\\azooKeyInferenceHostSupervisor-\$InstanceKey') "host-supervisor.ps1 should use a cross-session per-user named mutex."
      Assert-Condition ($script:supervisor.Text -match 'Global\\azooKeyInferenceHostSupervisorStop-\$InstanceKey') "host-supervisor.ps1 should expose a cross-session per-user stop event."
      Assert-Condition ($script:supervisor.Text -match '\[Math\]::Min\(\$restartDelayMs \* 2,\s*\$RestartDelayMaxMs\)') "host-supervisor.ps1 should bound exponential restart backoff."
      Assert-Condition ($script:supervisor.Text -match 'Test-PerUserPipe\s+-Name\s+\$PipeName') "host-supervisor.ps1 should wait for an existing per-user pipe before launching."
      Assert-Condition ($script:supervisor.Text -match '\$hostProcess\.Dispose\(\)') "host-supervisor.ps1 should release each child process handle."
    }

    It "restarts a failed host process" {
      $childPath = Join-Path $TestDrive "fake-host.ps1"
      $counterPath = Join-Path $TestDrive "launch-count.txt"
      $stderrLogBase = Join-Path $TestDrive "host-stderr.log"
      @'
param([string]$CounterPath)
Add-Content -LiteralPath $CounterPath -Value "launched"
[Console]::Error.WriteLine("simulated crash")
exit 3
'@ | Set-Content -LiteralPath $childPath

      $powerShellPath = (Get-Process -Id $PID).Path
      $hostArguments = "-NoLogo -NoProfile -NonInteractive -File `"$childPath`" -CounterPath `"$counterPath`""
      & $script:supervisorPath `
        -HostExePath $powerShellPath `
        -HostArguments $hostArguments `
        -PipeName "azookey-supervisor-test-$([guid]::NewGuid().ToString('N'))" `
        -StderrLogPath $stderrLogBase `
        -InstanceKey "test-$([guid]::NewGuid().ToString('N'))" `
        -RestartDelayMinMs 10 `
        -RestartDelayMaxMs 20 `
        -StableRunSeconds 1 `
        -MaxLaunchCount 2

      Assert-Condition ((Get-Content -LiteralPath $counterPath).Count -eq 2) "host-supervisor.ps1 should relaunch a failed host."
      $stderrLogs = @(Get-ChildItem -LiteralPath $TestDrive -Filter "host-stderr-*.log")
      Assert-Condition ($stderrLogs.Count -eq 2) "host-supervisor.ps1 should preserve stderr for each launch."
      foreach ($stderrLog in $stderrLogs) {
        Assert-Condition ((Get-Content -Raw -LiteralPath $stderrLog.FullName) -match "simulated crash") "Each launch log should contain the host stderr."
      }
    }
  }
}
