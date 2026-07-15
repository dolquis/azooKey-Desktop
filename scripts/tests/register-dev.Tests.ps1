Describe "development registration scripts" {
  BeforeAll {
    $repoRoot = (Resolve-Path (Join-Path (Join-Path $PSScriptRoot "..") "..")).Path
    $registerPath = Join-Path (Join-Path $repoRoot "scripts") "register-dev.ps1"
    $unregisterPath = Join-Path (Join-Path $repoRoot "scripts") "unregister-dev.ps1"

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
    $script:registerParameters = Get-ParameterName -Ast $script:register.Ast
    $script:unregisterParameters = Get-ParameterName -Ast $script:unregister.Ast
  }

  Context "register-dev.ps1" {
    It "keeps explicit path parameters and elevated reentry separate" {
      Assert-Condition ($script:registerParameters -contains "TipDllPath") "register-dev.ps1 should expose TipDllPath."
      Assert-Condition ($script:registerParameters -contains "HostExePath") "register-dev.ps1 should expose HostExePath."
      Assert-Condition ($script:registerParameters -contains "AllowMockHost") "register-dev.ps1 should expose an explicit fallback-only override."
      Assert-Condition ($script:registerParameters -contains "ElevatedReentry") "register-dev.ps1 should expose ElevatedReentry."
    }

    It "defaults to the llama-enabled build and rejects an accidental mock host" {
      Assert-Condition ($script:register.Text -match [regex]::Escape('build\windows-llama-debug\tsf-tip\azookey_tsf_tip.dll')) "register-dev.ps1 should default to the llama-enabled TIP build."
      Assert-Condition ($script:register.Text -match [regex]::Escape('build\windows-llama-debug\inference-host\azookey_inference_host.exe')) "register-dev.ps1 should default to the llama-enabled host build."
      Assert-Condition ($script:register.Text -match 'azookey_zenzai_bench\.exe') "register-dev.ps1 should probe the Zenzai bench compiled with the host."
      Assert-Condition ($script:register.Text -match 'llama_cpp=1') "register-dev.ps1 should require a real llama.cpp runtime."
      Assert-Condition ($script:register.Text -match 'Assert-LlamaEnabledHost\s+-Path\s+\$HostExePath') "register-dev.ps1 should run the llama.cpp preflight before registration."
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
    }

    It "guards per-user HKCU cleanup from elevated reentry" {
      Assert-Condition ($script:unregister.Text -match 'if\s*\(\s*-not\s+\$ElevatedReentry\s*\)\s*\{') "unregister-dev.ps1 should guard HKCU cleanup with ElevatedReentry."
      Assert-Condition ($script:unregister.Text -match [regex]::Escape('HKCU:\Software\Microsoft\Windows\CurrentVersion\Run')) "unregister-dev.ps1 should clean the HKCU Run key."
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
}
