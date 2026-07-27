# MSIX Option A（external-location / identity package）の整合テスト（DEV-101 / M28）。
#
# 実機 VM を要する Add/Remove-AppxPackage smoke（compat-test/msix_install_uninstall.ps1、
# DEV-267 gate:human-required）とは別に、VM 無しで検証できる「宣言値の整合」を CI で守る。
# ここで守る不変条件が壊れると、症状は実機でしか現れない（identity 欠落 0x80073D54、
# 言語バーに死んだエントリが残る等）ため、静的に落とす価値が大きい。
#
# 正典: docs/sideload-packaging-spec.md §1.0（経路 3 候補）/ §1.1.2（Option A PoC）/ §1.5。
Describe "MSIX identity package consistency (DEV-101 / M28)" {
  BeforeAll {
    $repoRoot = (Resolve-Path (Join-Path (Join-Path $PSScriptRoot "..") "..")).Path
    $script:repoRoot = $repoRoot

    $script:appxManifestPath = Join-Path (Join-Path $repoRoot "pkg") "msix/AppxManifest.xml"
    $script:appManifestPath = Join-Path (Join-Path $repoRoot "pkg") "msix/azookey_inference_host.exe.manifest"
    $script:smokePath = Join-Path (Join-Path $repoRoot "compat-test") "msix_install_uninstall.ps1"
    $script:tipHeaderPath = Join-Path $repoRoot "tsf-tip/include/azookey/tsf/TextServiceFactory.h"
    $script:dllMainPath = Join-Path $repoRoot "tsf-tip/src/DllMain.cpp"
    $script:hostCMakePath = Join-Path (Join-Path $repoRoot "inference-host") "CMakeLists.txt"
    $script:rootCMakePath = Join-Path $repoRoot "CMakeLists.txt"
    $script:qualityPath = Join-Path (Join-Path $repoRoot "scripts") "test-powershell-quality.ps1"

    function Get-XmlNamespaceManager {
      param(
        [Parameter(Mandatory = $true)]
        [xml]$Document,
        [Parameter(Mandatory = $true)]
        [hashtable]$Namespace
      )

      $manager = [System.Xml.XmlNamespaceManager]::new($Document.NameTable)
      foreach ($prefix in $Namespace.Keys) {
        $manager.AddNamespace($prefix, $Namespace[$prefix])
      }
      # XmlNamespaceManager は IEnumerable なので、そのまま返すと prefix 文字列に
      # 展開されてしまう。単一要素配列で包んで返す。
      return , $manager
    }

    function Get-ParameterDefault {
      param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [Parameter(Mandatory = $true)]
        [string]$Name
      )

      $parseErrors = $null
      $ast = [System.Management.Automation.Language.Parser]::ParseFile(
        $Path, [ref]$null, [ref]$parseErrors)
      if ($parseErrors.Count -gt 0) {
        throw "PowerShell parse errors in $Path`: $($parseErrors.Message -join '; ')"
      }

      $parameter = $ast.ParamBlock.Parameters |
        Where-Object { $_.Name.VariablePath.UserPath -eq $Name } |
        Select-Object -First 1
      if (-not $parameter) {
        throw "$Path does not declare a -$Name parameter."
      }
      if (-not $parameter.DefaultValue) {
        throw "$Path declares -$Name without a default value."
      }

      return $parameter.DefaultValue.Value
    }

    function ConvertTo-GuidString {
      # C++ の GUID 初期化子（{0x…, 0x…, 0x…, {0x…, …}}）をレジストリ表記
      # {XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX} に変換する。
      param(
        [Parameter(Mandatory = $true)]
        [string]$Text,
        [Parameter(Mandatory = $true)]
        [string]$Name
      )

      $pattern = "(?s){0}\s*=\s*\{{(.*?)\}}\s*;" -f [regex]::Escape($Name)
      $match = [regex]::Match($Text, $pattern)
      if (-not $match.Success) {
        throw "GUID initializer for $Name not found."
      }

      $fields = @([regex]::Matches($match.Groups[1].Value, '0x[0-9a-fA-F]+') |
        ForEach-Object { [Convert]::ToUInt32(($_.Value -replace '^0x', ''), 16) })
      if ($fields.Count -ne 11) {
        throw "GUID initializer for $Name has $($fields.Count) fields (expected 11)."
      }

      $format = '{{{0:X8}-{1:X4}-{2:X4}-{3:X2}{4:X2}-{5:X2}{6:X2}{7:X2}{8:X2}{9:X2}{10:X2}}}'
      return $format -f $fields
    }

    $script:appx = [xml](Get-Content -Raw -LiteralPath $script:appxManifestPath)
    $script:appxNs = Get-XmlNamespaceManager -Document $script:appx -Namespace @{
      f      = "http://schemas.microsoft.com/appx/manifest/foundation/windows10"
      uap    = "http://schemas.microsoft.com/appx/manifest/uap/windows10"
      uap10  = "http://schemas.microsoft.com/appx/manifest/uap/windows10/10"
      rescap = "http://schemas.microsoft.com/appx/manifest/foundation/windows10/restrictedcapabilities"
    }

    $script:identity = $script:appx.SelectSingleNode("/f:Package/f:Identity", $script:appxNs)
    $script:applications = @($script:appx.SelectNodes("/f:Package/f:Applications/f:Application", $script:appxNs))
    $script:hostApplication = $script:applications |
      Where-Object { $_.GetAttribute("Executable") -eq "azookey_inference_host.exe" } |
      Select-Object -First 1

    $script:appManifest = [xml](Get-Content -Raw -LiteralPath $script:appManifestPath)
    $script:appManifestNs = Get-XmlNamespaceManager -Document $script:appManifest -Namespace @{
      asm  = "urn:schemas-microsoft-com:asm.v1"
      msix = "urn:schemas-microsoft-com:msix.v1"
    }
    $script:msixElement = $script:appManifest.SelectSingleNode("/asm:assembly/msix:msix", $script:appManifestNs)

    $script:tipHeaderText = Get-Content -Raw -LiteralPath $script:tipHeaderPath
    $script:dllMainText = Get-Content -Raw -LiteralPath $script:dllMainPath
    $script:hostCMakeText = Get-Content -Raw -LiteralPath $script:hostCMakePath
    $script:rootCMakeText = Get-Content -Raw -LiteralPath $script:rootCMakePath
    $script:qualityText = Get-Content -Raw -LiteralPath $script:qualityPath
  }

  Context "identity metadata (AppxManifest.xml and azookey_inference_host.exe.manifest)" {
    # 3 属性のいずれかが食い違うと Add-AppxPackage は成功するのに runtime で
    # identity が欠落する（0x80073D54）。実機でしか現れない失敗なので静的に守る。
    It "declares the host application in the identity package" {
      $script:hostApplication | Should -Not -BeNullOrEmpty -Because "AppxManifest.xml must declare an Application for azookey_inference_host.exe"
    }

    It "matches packageName with Identity@Name" {
      $script:msixElement.GetAttribute("packageName") |
        Should -BeExactly $script:identity.GetAttribute("Name")
    }

    It "matches publisher with Identity@Publisher" {
      $script:msixElement.GetAttribute("publisher") |
        Should -BeExactly $script:identity.GetAttribute("Publisher")
    }

    It "matches applicationId with Application@Id" {
      $script:msixElement.GetAttribute("applicationId") |
        Should -BeExactly $script:hostApplication.GetAttribute("Id")
    }

    It "names the side-by-side manifest after the declared executable" {
      # 命名規約 <executable>.manifest（Win32 application manifests）。
      [System.IO.Path]::GetFileName($script:appManifestPath) |
        Should -BeExactly ("{0}.manifest" -f $script:hostApplication.GetAttribute("Executable"))
    }

    It "keeps Publisher in certificate Subject form" {
      # Publisher は署名証明書の Subject と完全一致が必要（不一致は 0x8007000B）。
      $script:identity.GetAttribute("Publisher") | Should -Match '^CN='
    }
  }

  Context "Option A invariants (docs/sideload-packaging-spec.md 1.0 / 1.5)" {
    It "enables external content" {
      $node = $script:appx.SelectSingleNode(
        "/f:Package/f:Properties/uap10:AllowExternalContent", $script:appxNs)
      $node | Should -Not -BeNullOrEmpty
      $node.InnerText.Trim() | Should -BeExactly "true"
    }

    It "declares the Win32 compatibility capabilities" {
      $capabilities = @($script:appx.SelectNodes(
          "/f:Package/f:Capabilities/rescap:Capability", $script:appxNs) |
          ForEach-Object { $_.GetAttribute("Name") })
      $capabilities | Should -Contain "runFullTrust"
      $capabilities | Should -Contain "unvirtualizedResources"
    }

    It "keeps the host application Win32-compatible" {
      $script:hostApplication.GetAttribute("TrustLevel", "http://schemas.microsoft.com/appx/manifest/uap/windows10/10") |
        Should -BeExactly "mediumIL"
      $script:hostApplication.GetAttribute("RuntimeBehavior", "http://schemas.microsoft.com/appx/manifest/uap/windows10/10") |
        Should -BeExactly "win32App"
    }

    It "keeps MinVersion low enough for Windows 10 22H2" {
      # spec 1.5: Option A は Win10 22H2（build 19045）必須。com4 を使わないので
      # build 20348 要件を負わない。MinVersion を上げると受け入れ条件から外れる。
      $family = $script:appx.SelectSingleNode(
        "/f:Package/f:Dependencies/f:TargetDeviceFamily", $script:appxNs)
      $minVersion = [version]$family.GetAttribute("MinVersion")
      $minVersion | Should -BeGreaterOrEqual ([version]"10.0.19041.0") -Because "uap10:AllowExternalContent requires build 19041+"
      $minVersion | Should -BeLessOrEqual ([version]"10.0.19045.0") -Because "spec 1.5 requires Windows 10 22H2 (build 19045) support for Option A"
    }

    It "declares no COM server (Option A registers the TIP via regsvr32)" {
      # com4:Class / com4:Extension が入った時点で経路は Option B/C になり、
      # 登録先（HKLM か MSIX の OS 内部登録か）も smoke の合否定義も変わる。
      # spec 1.0 / 1.1.2 とハーネスを更新せずに混入するのを防ぐ。
      $comNodes = @($script:appx.SelectNodes("//*", $script:appxNs) |
          Where-Object { $_.NamespaceURI -like "http://schemas.microsoft.com/appx/manifest/com/*" })
      $comNodes.Count | Should -Be 0 -Because "Option A must not declare com4:ComServer; switching to Option B/C requires updating spec 1.0/1.1.2 and the smoke harness"
    }
  }

  Context "smoke harness defaults (compat-test/msix_install_uninstall.ps1)" {
    It "defaults -PackageName to Identity@Name" {
      Get-ParameterDefault -Path $script:smokePath -Name "PackageName" |
        Should -BeExactly $script:identity.GetAttribute("Name")
    }

    It "defaults -Clsid to kTextServiceClsid" {
      # 既定値がずれると、存在しないキーを見て「残骸 0」と誤 PASS する。
      $expected = ConvertTo-GuidString -Text $script:tipHeaderText -Name "kTextServiceClsid"
      $actual = Get-ParameterDefault -Path $script:smokePath -Name "Clsid"
      $actual | Should -Match '^\{[0-9A-F]{8}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{12}\}$'
      $actual | Should -BeExactly $expected
    }

    It "defaults -ProfileGuid to kTextServiceProfileGuid" {
      $expected = ConvertTo-GuidString -Text $script:tipHeaderText -Name "kTextServiceProfileGuid"
      $actual = Get-ParameterDefault -Path $script:smokePath -Name "ProfileGuid"
      $actual | Should -Match '^\{[0-9A-F]{8}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{12}\}$'
      $actual | Should -BeExactly $expected
    }

    It "defaults -LangId to kJapaneseLangId" {
      $match = [regex]::Match($script:dllMainText, 'kJapaneseLangId\s*=\s*(0x[0-9a-fA-F]+)')
      $match.Success | Should -BeTrue -Because "DllMain.cpp must define kJapaneseLangId"
      $expected = [Convert]::ToInt32(($match.Groups[1].Value -replace '^0x', ''), 16)
      $actual = Get-ParameterDefault -Path $script:smokePath -Name "LangId"
      [Convert]::ToInt32(($actual -replace '^0x', ''), 16) | Should -Be $expected
    }

    It "supports external-location registration" {
      # Option A では -ExternalLocation 無しの登録は host に identity を与えず
      # smoke が無意味になる（PR #214 の P2 指摘）。
      $smokeText = Get-Content -Raw -LiteralPath $script:smokePath
      $smokeText | Should -Match 'Add-AppxPackage\s+-Path\s+\$MsixPath\s+-ExternalLocation\s+\$absExternal'
    }
  }

  Context "build wiring (inference-host/CMakeLists.txt)" {
    BeforeAll {
      # コメント行は判定から外す。パスもフラグ名も解説コメントに書かれているため、
      # 素の文字列一致だと「配線が外れてもコメントだけで PASS する」偽陰性になる。
      $script:hostCMakeCode =
        ($script:hostCMakeText -split "`n" | Where-Object { $_ -notmatch '^\s*#' }) -join "`n"
      $script:rootCMakeCode =
        ($script:rootCMakeText -split "`n" | Where-Object { $_ -notmatch '^\s*#' }) -join "`n"
      # ガードされたブロック本体だけを取り出す（終端はインデント無しの endif()）。
      $script:embedBlock = [regex]::Match(
        $script:hostCMakeCode,
        '(?sm)^if\(WIN32 AND MSVC AND AZOOKEY_EMBED_MSIX_IDENTITY\)(.*?)^endif\(\)').Groups[1].Value
    }

    It "guards the embedding with the option" {
      # ブロックが取れない = ガード条件が変わったか配線ごと消えた。
      $script:embedBlock | Should -Not -BeNullOrEmpty
    }

    It "adds the identity manifest as a source of the host target" {
      # 埋め込みが外れると Add-AppxPackage -ExternalLocation で登録しても
      # Package.Current が null になり、DEV-267 の VM smoke が無意味になる。
      # .manifest ソースは CMake が mt.exe へ渡す（vs_link_exe --manifests）。
      # 変数定義と target_sources の結び付きを 1 単位で見る（別々の文字列一致だと
      # target_sources の引数を差し替えても素通りする）。
      $script:embedBlock | Should -Match 'set\(AZOOKEY_MSIX_APP_MANIFEST[\s\S]*?pkg/msix/azookey_inference_host\.exe\.manifest'
      $script:embedBlock | Should -Match 'target_sources\(\s*azookey_inference_host\s+PRIVATE\s+"\$\{AZOOKEY_MSIX_APP_MANIFEST\}"\s*\)'
    }

    It "does not hand the manifest to the linker directly" {
      # /MANIFEST:EMBED を渡すとリンカが埋め込んでしまい、CMake が続けて呼ぶ
      # mt.exe の入力が 0 になって c10100a7 でビルドが落ちる（実測）。
      $script:hostCMakeCode | Should -Not -Match '/MANIFEST'
    }

    It "defaults the embedding option to ON" {
      # 既定を OFF にすると if ブロックが実行されず、他の静的チェックは全て通るのに
      # exe から <msix> が消える。既定値まで含めて固定する。
      $script:rootCMakeCode | Should -Match '(?s)option\(\s*AZOOKEY_EMBED_MSIX_IDENTITY\s+"[^"]*"\s+ON\s*\)'
    }

    It "verifies the built executable in CI" {
      # 静的チェックはソースの記述しか見られない。実際に埋め込まれたかは
      # 成果物の RT_MANIFEST を見る CI ステップが担保する。
      $workflow = Get-Content -Raw -LiteralPath (
        Join-Path $script:repoRoot ".github/workflows/windows.yml")
      $workflow | Should -Match 'verify-msix-identity-embedding\.ps1'
      $workflow | Should -Match 'MsixIdentity\s*=\s*''\$\{\{\s*steps\.verify_identity\.outcome\s*\}\}'''
    }
  }

  Context "packaging scripts in the quality gate" {
    It "lints the identity package build script" {
      $script:qualityText | Should -Match 'build-identity-package\.ps1'
    }
  }
}
