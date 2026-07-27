<#
.SYNOPSIS
  ビルド済み azookey_inference_host.exe に MSIX identity metadata が実際に
  埋め込まれているかを成果物から検証する（DEV-101 / M28）。

.DESCRIPTION
  Option A（external-location / identity package）では、実行体に side-by-side
  manifest の <msix> 要素が埋め込まれていないと Add-AppxPackage -ExternalLocation
  で登録しても runtime に package identity が付かない（Package.Current が null）。
  この失敗は登録時にはエラーにならず、実機でしか症状が出ない。

  CMake 配線の静的チェック（scripts/tests/msix-identity-consistency.Tests.ps1）は
  「配線がソースに書かれているか」までしか見られないため、本スクリプトは
  ビルド成果物から RT_MANIFEST リソース（#1）を mt.exe で取り出し、
  <msix> の packageName / publisher / applicationId が pkg/msix/AppxManifest.xml と
  一致することを確認する。CMake / リンカ / mt.exe のどこで埋め込みが外れても落ちる。

  Windows 専用（mt.exe = Windows SDK）。CI の Windows build ジョブから実行する。

.PARAMETER ExePath
  検証対象の azookey_inference_host.exe。

.PARAMETER AppxManifestPath
  期待値の取得元。既定はリポジトリの pkg/msix/AppxManifest.xml。

.EXAMPLE
  pwsh -File .\scripts\verify-msix-identity-embedding.ps1 `
    -ExePath .\build\windows-debug\inference-host\azookey_inference_host.exe
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)]
  [string]$ExePath,

  [string]$AppxManifestPath = ""
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if (-not $AppxManifestPath) {
  $AppxManifestPath = Join-Path $repoRoot "pkg/msix/AppxManifest.xml"
}

if (-not (Test-Path -LiteralPath $ExePath)) {
  throw "実行体が見つかりません: $ExePath"
}
if (-not (Test-Path -LiteralPath $AppxManifestPath)) {
  throw "AppxManifest.xml が見つかりません: $AppxManifestPath"
}

function Find-SdkTool {
  param([Parameter(Mandatory = $true)][string]$Name)

  $cmd = Get-Command $Name -ErrorAction SilentlyContinue
  if ($cmd) { return $cmd.Source }

  $roots = @(
    "${env:ProgramFiles(x86)}\Windows Kits\10\bin",
    "${env:ProgramFiles}\Windows Kits\10\bin"
  ) | Where-Object { $_ -and (Test-Path $_) }
  foreach ($root in $roots) {
    $hit = Get-ChildItem -Path $root -Recurse -Filter $Name -ErrorAction SilentlyContinue |
      Where-Object { $_.FullName -match '\\x64\\' } |
      Sort-Object FullName -Descending | Select-Object -First 1
    if ($hit) { return $hit.FullName }
  }
  throw "$Name が見つかりません。Windows SDK をインストールし、PATH を通してください。"
}

$exeFullPath = (Resolve-Path -LiteralPath $ExePath).ProviderPath
$mt = Find-SdkTool -Name "mt.exe"
Write-Host "mt.exe: $mt"
Write-Host "対象: $exeFullPath"

# 実行体の RT_MANIFEST(#1) を取り出す。埋め込みが無ければ mt.exe が失敗する。
$extracted = Join-Path ([System.IO.Path]::GetTempPath()) ("azookey-embedded-manifest-" +
  [System.Guid]::NewGuid().ToString("N") + ".manifest")
try {
  & $mt -nologo "-inputresource:$exeFullPath;#1" "-out:$extracted"
  if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $extracted)) {
    throw ("$exeFullPath から RT_MANIFEST を取り出せませんでした (mt.exe exit $LASTEXITCODE)。" +
      "manifest が埋め込まれていない可能性があります（inference-host/CMakeLists.txt の " +
      "AZOOKEY_EMBED_MSIX_IDENTITY 配線を確認）。")
  }

  [xml]$embedded = Get-Content -Raw -LiteralPath $extracted
  $embeddedNs = [System.Xml.XmlNamespaceManager]::new($embedded.NameTable)
  $embeddedNs.AddNamespace("asm", "urn:schemas-microsoft-com:asm.v1")
  $embeddedNs.AddNamespace("msix", "urn:schemas-microsoft-com:msix.v1")

  $msixElement = $embedded.SelectSingleNode("/asm:assembly/msix:msix", $embeddedNs)
  if (-not $msixElement) {
    Write-Host "--- 埋め込み manifest ---"
    Get-Content -LiteralPath $extracted | Write-Host
    throw ("埋め込み manifest に <msix> 要素がありません。identity metadata が失われています " +
      "（Package.Current が null になり、DEV-267 の VM smoke が無意味になる）。")
  }

  [xml]$appx = Get-Content -Raw -LiteralPath $AppxManifestPath
  $appxNs = [System.Xml.XmlNamespaceManager]::new($appx.NameTable)
  $appxNs.AddNamespace("f", "http://schemas.microsoft.com/appx/manifest/foundation/windows10")
  $identity = $appx.SelectSingleNode("/f:Package/f:Identity", $appxNs)
  $application = $appx.SelectSingleNode(
    "/f:Package/f:Applications/f:Application[@Executable='azookey_inference_host.exe']", $appxNs)
  if (-not $identity -or -not $application) {
    throw "$AppxManifestPath から Identity / Application を読めません。"
  }

  # 不一致は 0x80073D54（登録は成功するが runtime に identity 欠落）。
  $expected = [ordered]@{
    packageName   = $identity.GetAttribute("Name")
    publisher     = $identity.GetAttribute("Publisher")
    applicationId = $application.GetAttribute("Id")
  }

  $mismatches = @()
  foreach ($key in $expected.Keys) {
    $actual = $msixElement.GetAttribute($key)
    if ($actual -cne $expected[$key]) {
      $mismatches += ("{0}: 埋め込み='{1}' / AppxManifest='{2}'" -f $key, $actual, $expected[$key])
    } else {
      Write-Host ("  OK  {0} = {1}" -f $key, $actual)
    }
  }

  if ($mismatches.Count -gt 0) {
    throw ("埋め込まれた <msix> が AppxManifest.xml と一致しません（0x80073D54 の原因）:`n  " +
      ($mismatches -join "`n  "))
  }

  Write-Host "MSIX identity metadata の埋め込みを確認しました。" -ForegroundColor Green
} finally {
  Remove-Item -LiteralPath $extracted -Force -ErrorAction SilentlyContinue
}
