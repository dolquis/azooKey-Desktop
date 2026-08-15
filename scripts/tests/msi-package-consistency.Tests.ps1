Describe "WiX MSI package consistency" {
  BeforeAll {
    $repoRoot = (Resolve-Path (Join-Path (Join-Path $PSScriptRoot "..") "..")).Path
    $projectPath = Join-Path $repoRoot "pkg\msi\azooKey.wixproj"
    $packagePath = Join-Path $repoRoot "pkg\msi\Package.wxs"
    $packageReadmePath = Join-Path $repoRoot "pkg\msi\README.md"
    $releaseWorkflowPath = Join-Path $repoRoot ".github\workflows\release.yml"
    $settingsProjectPath = Join-Path $repoRoot "settings-app\azookey_settings.vcxproj"
    $settingsCMakePath = Join-Path $repoRoot "settings-app\CMakeLists.txt"
    $thirdPartyLicensesPath = Join-Path $repoRoot "THIRD_PARTY_LICENSES"
    $rootReadmePath = Join-Path $repoRoot "README.md"

    $script:project = Get-Content -Raw $projectPath
    $script:package = Get-Content -Raw $packagePath
    $script:packageReadme = Get-Content -Raw $packageReadmePath
    $script:releaseWorkflow = Get-Content -Raw $releaseWorkflowPath
    $script:settingsProject = Get-Content -Raw $settingsProjectPath
    $script:settingsCMake = Get-Content -Raw $settingsCMakePath
    $script:thirdPartyLicenses = Get-Content -Raw $thirdPartyLicensesPath
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
    $script:project | Should -Match "Condition=`"!Exists\('\$\(SettingsPayloadDir\)'\)`""
    $script:project | Should -Match "Condition=`"!Exists\('\$\(SettingsExePath\)'\)`""
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
    $script:package | Should -Match 'Action="RegisterTip"[\s\S]*?After="UnregisterTipOnInstallRollback"'
    ([regex]::Matches($script:package, '\[System64Folder\]msiexec\.exe')).Count | Should -Be 4
    $script:package | Should -Not -Match '\[SystemFolder\]msiexec\.exe'
  }

  It "defines rollback actions for both registration directions" {
    $script:package | Should -Match 'Id="RegisterTipOnUninstallRollback"[\s\S]*?Execute="rollback"[\s\S]*?Return="ignore"'
    $script:package | Should -Match 'Id="UnregisterTipOnInstallRollback"[\s\S]*?Execute="rollback"[\s\S]*?Return="ignore"'
    $script:package | Should -Match 'Action="RegisterTipOnUninstallRollback"[\s\S]*?Before="UnregisterTip"'
    $script:package | Should -Match 'Action="UnregisterTipOnInstallRollback"[\s\S]*?After="InstallFiles"'
  }

  It "always installs the self-contained settings payload and shortcut" {
    $script:package | Should -Match 'Id="SettingsShortcut"[\s\S]*?Directory="ProgramMenuFolder"'
    $script:package | Should -Match 'Id="SettingsShortcutComponent"[\s\S]*?Root="HKCU"[\s\S]*?KeyPath="yes"'
    $script:project | Should -Match '<SuppressIces>ICE03</SuppressIces>'
    $script:package | Should -Match '<Files Include="\$\(SettingsPayloadDir\)\\\*\*" Directory="INSTALLFOLDER">'
    $script:package | Should -Match '<Exclude Files="\$\(SettingsExePath\)" />'
    $script:package | Should -Match '<Exclude Files="\$\(SettingsPayloadDir\)\\obj\\\*\*" />'
    $script:package | Should -Match '<Exclude Files="\$\(SettingsPayloadDir\)\\\*\.pdb" />'
    $script:project | Should -Match "Settings executable not found"
    $script:releaseWorkflow | Should -Match 'Build self-contained settings app'
    $script:releaseWorkflow | Should -Match '--target azookey_settings'
    $script:releaseWorkflow | Should -Match '"-p:SettingsPayloadDir='
    $script:releaseWorkflow | Should -Match '"-p:SettingsExePath='
  }

  It "records licenses for the redistributed settings runtime" {
    $script:thirdPartyLicenses | Should -Match 'Microsoft Windows App SDK'
    $script:thirdPartyLicenses | Should -Match 'Microsoft Windows C\+\+/WinRT'
    $script:thirdPartyLicenses | Should -Match 'Microsoft WebView2 loader'
  }

  It "builds the unpackaged WinUI settings app as self-contained" {
    $script:settingsProject | Should -Match '<UseWinUI>true</UseWinUI>'
    $script:settingsProject | Should -Match '<WindowsAppSDKSelfContained>true</WindowsAppSDKSelfContained>'
    $script:settingsProject | Should -Match '<AppxPackage>false</AppxPackage>'
    $script:settingsProject | Should -Match '<WindowsPackageType>None</WindowsPackageType>'
    $script:settingsProject | Should -Match '<TargetName>\$\(RootNamespace\)</TargetName>'
    $script:settingsCMake | Should -Match 'add_custom_target\(azookey_settings'
    $script:settingsProject | Should -Match '<PackageReference Include="Microsoft\.WindowsAppSDK\.Runtime" Version="2\.4\.0" />'
    $script:settingsProject | Should -Match '<PackageReference Include="Microsoft\.WindowsAppSDK\.WinUI"'
    $script:settingsProject | Should -Not -Match 'Microsoft\.WindowsAppSDK\.(AI|ML|Search|Widgets)'
    $script:settingsCMake | Should -Match '/restore'
  }

  It "keeps models and CUDA runtime outside the base MSI" {
    $script:package | Should -Not -Match '(?i)gguf|cudart|cublas|ggml-cuda'
    $script:packageReadme | Should -Match 'GGUF'
    $script:packageReadme | Should -Match 'CUDA'
    $script:packageReadme | Should -Match 'base MSI'
  }

  It "builds and uploads an unsigned MSI in the guarded release workflow" {
    $script:releaseWorkflow | Should -Match "vars\.RELEASE_ENABLED == 'true'"
    $script:releaseWorkflow | Should -Match 'dotnet build pkg\\msi\\azooKey\.wixproj'
    $script:releaseWorkflow | Should -Match 'Get-AuthenticodeSignature'
    $script:releaseWorkflow | Should -Match 'pkg\\msi\\bin\\Release\\\*\.msi'
    $script:releaseWorkflow | Should -Not -Match '(?m)^\s+(?:run:\s*)?.*signtool'
    $script:releaseWorkflow | Should -Not -Match '(?m)^\s+- name: .*MSIX'
    $script:releaseWorkflow | Should -Match 'REF_NAME: \$\{\{ github\.ref_name \}\}'
    $script:releaseWorkflow | Should -Match 'REF_TYPE: \$\{\{ github\.ref_type \}\}'
    $script:releaseWorkflow | Should -Match '\$env:REF_NAME\.Substring\(1\)'
    $script:releaseWorkflow | Should -Match '\$parts\[0\] -gt 255'
    $script:releaseWorkflow | Should -Match '\$parts\[1\] -gt 255'
    $script:releaseWorkflow | Should -Match '\$parts\[2\] -gt 65535'
  }

  It "documents the unsigned-publisher warning for users" {
    $script:rootReadme | Should -Match '未署名 MSI'
    $script:rootReadme | Should -Match 'SmartScreen'
    $script:rootReadme | Should -Match '不明な発行元'
  }
}
