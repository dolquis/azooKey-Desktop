<#
.SYNOPSIS
  Option A identity package（sparse / external-location）を MakeAppx で組み立て、
  任意で署名する M28 PoC ビルドヘルパー（DEV-101 / M28）。

.DESCRIPTION
  pkg/msix/AppxManifest.xml（identity-only manifest）を MakeAppx.exe /nv でパッケージ化し、
  dev/prod 証明書があれば SignTool で署名する。VS の「Package with External Location」
  拡張に依存しない、tooling-independent な canonical 経路（Microsoft Learn 手動手順に準拠）。

  生成した .msix は DEV-267（Human Gate: MSIX 登録整合 VM smoke）で
  compat-test/msix_install_uninstall.ps1 が Add/Remove-AppxPackage する対象。
  identity package は「バイナリを含まない」ため、実行体（azookey_inference_host.exe 等）は
  別途 external location（§4 WiX/MSI の install ディレクトリ）に配置し、
  Add-AppxPackage -ExternalLocation <install-dir> で紐付ける（README.md 参照）。

  ⚠️ PoC 草案・未検証: 実 Windows SDK（MakeAppx.exe / SignTool.exe）と、登録は
     信頼された証明書（自己署名 dev cert は Cert:\...\TrustedPeople へ import 必須）が要る。
     schema validation・実登録の緑化は DEV-267（gate:human-required）。

.PARAMETER ManifestDir
  AppxManifest.xml を含むディレクトリ。既定は本スクリプトのディレクトリ（pkg/msix）。

.PARAMETER OutMsix
  出力 .msix パス。既定は <ManifestDir>\out\azooKey-identity-<Version>.msix。

.PARAMETER PfxPath
  署名に使う .pfx。未指定なら署名を skip（identity package は実登録前に必ず署名すること。
  README.md「署名」参照）。

.PARAMETER PfxPassword
  .pfx のパスワード（PoC 簡便のため平文。CI では Key Vault 等を使う）。

.EXAMPLE
  # 署名なしでパッケージだけ作る（schema/pack の確認用）
  pwsh -File .\build-identity-package.ps1

.EXAMPLE
  # dev cert で署名まで行う
  pwsh -File .\build-identity-package.ps1 -PfxPath .\azooKey-dev.pfx -PfxPassword 'devpw'

.NOTES
  Windows 専用。MakeAppx.exe / SignTool.exe は Windows SDK（既定
  C:\Program Files (x86)\Windows Kits\10\bin\<ver>\x64\）に含まれる。
#>
[CmdletBinding()]
param(
  [string]$ManifestDir = $PSScriptRoot,
  [string]$OutMsix = "",
  [string]$PfxPath = "",
  [string]$PfxPassword = ""
)

$ErrorActionPreference = "Stop"

function Find-SdkTool {
  param([string]$Name)
  # 1) PATH 上にあればそれを使う。
  $cmd = Get-Command $Name -ErrorAction SilentlyContinue
  if ($cmd) { return $cmd.Source }
  # 2) Windows SDK の bin から最新 ver の x64 を探す。
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
  throw "$Name が見つかりません。Windows SDK をインストールし、PATH を通すか SDK の bin\<ver>\x64 を確認してください。"
}

$manifestPath = Join-Path $ManifestDir "AppxManifest.xml"
if (-not (Test-Path $manifestPath)) {
  throw "AppxManifest.xml が見つかりません: $manifestPath"
}

# Version を manifest から拾って出力名に使う。
[xml]$manifestXml = Get-Content -Path $manifestPath -Raw
$version = $manifestXml.Package.Identity.Version
if (-not $version) { $version = "1.0.0.0" }

if (-not $OutMsix) {
  $outDir = Join-Path $ManifestDir "out"
  $null = New-Item -ItemType Directory -Path $outDir -Force
  $OutMsix = Join-Path $outDir "azooKey-identity-$version.msix"
}

$makeAppx = Find-SdkTool -Name "MakeAppx.exe"
Write-Host "MakeAppx: $makeAppx"
Write-Host "Manifest dir: $ManifestDir"
Write-Host "Output: $OutMsix"

# ---------------------------------------------------------------------------
# clean staging から pack する（秘密鍵・不要ファイルの同梱防止）
# ---------------------------------------------------------------------------
# MakeAppx pack /d <dir> は <dir> の中身を「丸ごと」パッケージ化する。$ManifestDir
# （pkg/msix）を直接指定すると、この .ps1 / Package.wapproj / README.md /
# .exe.manifest、さらに README の dev-cert 手順で生成される azookey-dev.pfx（秘密鍵）や
# out/ の .msix までもが .msix に同梱されてしまう（PFX 同梱は秘密鍵漏洩）。
# よって manifest と（あれば）Assets のみをクリーンな一時ディレクトリへ集めてから pack する。
$staging = Join-Path ([System.IO.Path]::GetTempPath()) ("azookey-msix-stage-" + [System.Guid]::NewGuid().ToString("N"))
$null = New-Item -ItemType Directory -Path $staging -Force
try {
  Copy-Item -LiteralPath $manifestPath -Destination (Join-Path $staging "AppxManifest.xml") -Force
  $assetsDir = Join-Path $ManifestDir "Assets"
  if (Test-Path $assetsDir) {
    Copy-Item -LiteralPath $assetsDir -Destination (Join-Path $staging "Assets") -Recurse -Force
  }
  # /nv: manifest 内の参照ファイルパス検証を bypass（identity package は外部コンテンツ参照のため必須）。
  # /o : 既存出力を上書き。
  & $makeAppx pack /o /d $staging /nv /p $OutMsix
  if ($LASTEXITCODE -ne 0) { throw "MakeAppx pack が失敗しました (exit $LASTEXITCODE)。" }
} finally {
  Remove-Item -LiteralPath $staging -Recurse -Force -ErrorAction SilentlyContinue
}
Write-Host "パッケージ生成: $OutMsix" -ForegroundColor Green

if ($PfxPath) {
  if (-not (Test-Path $PfxPath)) { throw ".pfx が見つかりません: $PfxPath" }
  $signTool = Find-SdkTool -Name "SignTool.exe"
  Write-Host "SignTool: $signTool"
  $signArgs = @("sign", "/fd", "SHA256", "/a", "/f", $PfxPath)
  if ($PfxPassword) { $signArgs += @("/p", $PfxPassword) }
  $signArgs += $OutMsix
  & $signTool @signArgs
  if ($LASTEXITCODE -ne 0) { throw "SignTool sign が失敗しました (exit $LASTEXITCODE)。" }
  Write-Host "署名完了: $OutMsix" -ForegroundColor Green
  Write-Host "自己署名 cert の場合、公開 .cer を Cert:\CurrentUser\TrustedPeople へ import してから登録すること（README.md 参照）。" -ForegroundColor Yellow
} else {
  Write-Host "署名を skip（-PfxPath 未指定）。実登録前に必ず署名すること。" -ForegroundColor Yellow
}

Write-Host ""
Write-Host "次の手順（外部ロケーション登録）:" -ForegroundColor Cyan
Write-Host "  Add-AppxPackage -Path `"$OutMsix`" -ExternalLocation `"<azookey install ディレクトリの絶対パス>`""
Write-Host "  詳細と smoke 手順は pkg/msix/README.md / compat-test/msix_install_uninstall.ps1 を参照。"
