Describe "WiX MSI package consistency" {
  BeforeAll {
    $repoRoot = (Resolve-Path (Join-Path (Join-Path $PSScriptRoot "..") "..")).Path
    $projectPath = Join-Path $repoRoot "pkg\msi\azooKey.wixproj"
    $packagePath = Join-Path $repoRoot "pkg\msi\Package.wxs"
    $packageReadmePath = Join-Path $repoRoot "pkg\msi\README.md"
    $releaseWorkflowPath = Join-Path $repoRoot ".github\workflows\release.yml"
    $rootReadmePath = Join-Path $repoRoot "README.md"

    $script:project = Get-Content -Raw $projectPath
    $script:package = Get-Content -Raw $packagePath
    $script:packageReadme = Get-Content -Raw $packageReadmePath
    $script:releaseWorkflow = Get-Content -Raw $releaseWorkflowPath
    $script:rootReadme = Get-Content -Raw $rootReadmePath
  }

  It "pins the WiX SDK and builds an x64 package" {
    $script:project | Should -Match 'Project Sdk="WixToolset\.Sdk/5\.0\.2"'
    $script:project | Should -Match '<InstallerPlatform>x64</InstallerPlatform>'
    $script:project | Should -Match '<TreatWarningsAsErrors>true</TreatWarningsAsErrors>'
  }

  It "fails before packaging when required release payloads are absent" {
    $script:project | Should -Match "Condition=`"!Exists\('\$\(TipDllPath\)'\)`""
    $script:project | Should -Match "Condition=`"!Exists\('\$\(HostExePath\)'\)`""
    $script:project | Should -Match "MSVC runtime not found"
    $script:project | Should -Match "Condition=`"!Exists\('\$\(LicensePath\)'\)`""
    $script:project | Should -Match "Third-party licenses file not found"
  }

  It "installs the TIP and host per-machine under Program Files" {
    $script:package | Should -Match 'Scope="perMachine"'
    $script:package | Should -Match 'StandardDirectory Id="ProgramFiles64Folder"'
    $script:package | Should -Match 'Id="TipDll"[\s\S]*?Name="azookey_tsf_tip\.dll"'
    $script:package | Should -Match 'Id="HostExe"[\s\S]*?Name="azookey_inference_host\.exe"'
    $script:package | Should -Match 'Id="ThirdPartyLicensesFile"[\s\S]*?Name="THIRD_PARTY_LICENSES\.txt"'
  }

  It "deploys the required MSVC runtime beside the TIP and host" {
    $script:package | Should -Match 'Source="\$\(VCRuntimeDir\)\\msvcp140\.dll"'
    $script:package | Should -Match 'Source="\$\(VCRuntimeDir\)\\vcruntime140\.dll"'
    $script:package | Should -Match 'Source="\$\(VCRuntimeDir\)\\vcruntime140_1\.dll"'
    $script:releaseWorkflow | Should -Match 'Resolve app-local MSVC runtime'
    $script:releaseWorkflow | Should -Match 'Microsoft\.VisualStudio\.Component\.VC\.Tools\.x86\.x64'
    $script:releaseWorkflow | Should -Match '"-p:VCRuntimeDir=\$\{\{ steps\.vc-runtime\.outputs\.dir \}\}"'
  }

  It "registers and unregisters the installed TIP with elevated deferred actions" {
    $script:package | Should -Match 'Id="RegisterTip"[\s\S]*?/y &quot;\[#TipDll\]&quot;[\s\S]*?Execute="deferred"[\s\S]*?Impersonate="no"'
    $script:package | Should -Match 'Id="UnregisterTip"[\s\S]*?/z &quot;\[#TipDll\]&quot;[\s\S]*?Execute="deferred"[\s\S]*?Impersonate="no"'
    $script:package | Should -Match 'Action="UnregisterTip"[\s\S]*?Before="RemoveFiles"'
    $script:package | Should -Match 'Action="RegisterTip"[\s\S]*?After="RollbackUnregisterTip"'
  }

  It "defines rollback actions for both registration directions" {
    $script:package | Should -Match 'Id="RollbackRegisterTip"[\s\S]*?Execute="rollback"'
    $script:package | Should -Match 'Id="RollbackUnregisterTip"[\s\S]*?Execute="rollback"'
    $script:package | Should -Match 'Action="RollbackRegisterTip"[\s\S]*?Before="UnregisterTip"'
    $script:package | Should -Match 'Action="RollbackUnregisterTip"[\s\S]*?After="InstallFiles"'
  }

  It "supports a settings shortcut only when a settings executable is supplied" {
    $script:package | Should -Match '<\?if \$\(SettingsExePath\) != "__NOT_PROVIDED__" \?>'
    $script:package | Should -Match 'Id="SettingsShortcut"[\s\S]*?Directory="ProgramMenuFolder"'
    $script:project | Should -Match "Settings executable not found"
  }

  It "keeps models and CUDA runtime outside the base MSI" {
    $script:package | Should -Not -Match '(?i)gguf|cudart|cublas|ggml-cuda'
    $script:packageReadme | Should -Match 'GGUF モデルと CUDA ランタイムは base MSI に同梱しません'
  }

  It "builds and uploads an unsigned MSI in the guarded release workflow" {
    $script:releaseWorkflow | Should -Match "vars\.RELEASE_ENABLED == 'true'"
    $script:releaseWorkflow | Should -Match 'dotnet build pkg\\msi\\azooKey\.wixproj'
    $script:releaseWorkflow | Should -Match 'Get-AuthenticodeSignature'
    $script:releaseWorkflow | Should -Match 'pkg\\msi\\bin\\Release\\\*\.msi'
    $script:releaseWorkflow | Should -Not -Match '(?m)^\s+(?:run:\s*)?.*signtool'
    $script:releaseWorkflow | Should -Not -Match '(?m)^\s+- name: .*MSIX'
  }

  It "documents the unsigned-publisher warning for users" {
    $script:rootReadme | Should -Match '未署名 MSI'
    $script:rootReadme | Should -Match 'SmartScreen'
    $script:rootReadme | Should -Match '不明な発行元'
  }
}
