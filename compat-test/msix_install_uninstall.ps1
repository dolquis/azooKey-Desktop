<#
.SYNOPSIS
  MSIX install / uninstall residue smoke harness (DEV-101, M28).

.DESCRIPTION
  クリーン Win11 23H2 / Win10 22H2 / Win Server 2022 VM 上で azooKey の MSIX
  パッケージを Add-AppxPackage → 言語プロファイル / CLSID 登録の存在確認 →
  Remove-AppxPackage → 残骸 0 確認まで半自動でチェックする「雛形」スクリプト。

  本スクリプトは `docs/sideload-packaging-spec.md` §1（MSIX サイドロード）と
  同 §1.5（受け入れ条件）を検証対象とし、`plans/windows-port-roadmap.md`
  「既知のテストギャップ」#5（MSIX manifest と DllRegisterServer の整合）を
  解消するためのハーネスである。状態・進捗の正典は Linear（DEV-101）。

  TIP DLL の COM 登録ラウンドトリップ（DllRegisterServer →
  InprocServer32 / TSF プロファイル → DllUnregisterServer 消滅）自体は CTest
  `tsf_tip_com_smoke_tests::TsfTipRegistrationSmokeTest`
  （`tsf-tip/tests/com_smoke_test.cpp`、env `AZOOKEY_RUN_REGISTRATION_SMOKE` +
  昇格 opt-in）が担う。本ハーネスは CTest で再現できない MSIX
  パッケージング層（Add-AppxPackage / Remove-AppxPackage の OS 内部登録と
  残骸）を実機 VM で検証する補完であり、両者は重複しない。

  ⚠️ 雛形（skeleton）であること:
    - 配布経路（spec §1.0 の Option A: external-location / B: com4:InProcessServer
      / C: com4:SurrogateServer）が M28 PoC で未確定のため、経路別の登録先
      （HKLM machine-wide か MSIX OS 内部登録か）の最終アサーションは PoC 確定後に
      埋める。現状は存在/消滅を観測してレポートに残すところまでを行う。
    - 言語バー UI の実操作（azooKey を選んで入力できるか）は UI Automation /
      windows-mcp で別途検証する。本スクリプトは登録の有無を registry / TSF API
      レベルで確認するに留める。
    - `gate:human-required`: 実機 VM での緑化（スクショ / ログ）が Done の条件。

.PARAMETER MsixPath
  インストールする .msix / .msixbundle へのパス。

.PARAMETER PackageName
  Get-AppxPackage -Name に渡すパッケージ名（AppxManifest.xml の Identity@Name）。
  既定は spec §1.1 の例に合わせ "dolquis.azooKey"。

.PARAMETER Clsid
  TIP の CLSID。既定は tsf-tip/include/azookey/tsf/TextServiceFactory.h の
  kTextServiceClsid と一致。

.PARAMETER ProfileGuid
  TSF 言語プロファイル GUID。既定は kTextServiceProfileGuid と一致。

.PARAMETER ReportDir
  レポート出力先。既定はカレントに compat-report-YYYYMMDD-HHMMSS/ を作る
  （compat-test/README.md の出力レイアウトに準拠）。

.PARAMETER ExternalLocation
  Option A（external-location / sparse identity package）の登録に必須の外部ロケーション。
  実行体（azookey_inference_host.exe 等）が実際に置かれる install ディレクトリの絶対パス
  （末尾に exe 名は付けない）。指定すると Add-AppxPackage -ExternalLocation で登録する。
  Option A では未指定だと host に package identity が付かず smoke が無意味になるため
  警告を出す。Option B/C（バイナリ同梱の通常 MSIX）では不要（未指定でよい）。
  出典: https://learn.microsoft.com/windows/apps/desktop/modernize/grant-identity-to-nonpackaged-apps#register-the-identity-package-in-your-installer

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File .\msix_install_uninstall.ps1 `
    -MsixPath .\azooKey-1.0.0-x64.msix

.EXAMPLE
  # Option A（external-location）: 外部 install ディレクトリを紐付けて登録する
  powershell -ExecutionPolicy Bypass -File .\msix_install_uninstall.ps1 `
    -MsixPath .\out\azooKey-identity-1.0.0.0.msix `
    -ExternalLocation "C:\Program Files\azooKey"

.NOTES
  Windows 専用。Add-AppxPackage / Remove-AppxPackage を伴うため、クリーンな
  使い捨て VM（チェックポイント復元前提）で実行すること。
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)]
  [string]$MsixPath,

  [string]$PackageName = "dolquis.azooKey",

  # tsf-tip/include/azookey/tsf/TextServiceFactory.h::kTextServiceClsid と一致。
  [string]$Clsid = "{71EE04FA-B35D-4EB8-87A1-582D44A9A58C}",

  # 同 kTextServiceProfileGuid と一致。
  [string]$ProfileGuid = "{A8F74D91-8DF3-4DA1-B80B-01F7C73D4A90}",

  # 日本語 LANGID 0x0411。spec §1 / DllMain.cpp の kJapaneseLangId と一致。
  [string]$LangId = "0x0411",

  # Option A（external-location / sparse）の外部 install ディレクトリ絶対パス。
  # 指定すると Add-AppxPackage -ExternalLocation で登録する（Option A 必須）。
  [string]$ExternalLocation = "",

  [string]$ReportDir = ""
)

$ErrorActionPreference = "Stop"

# ---------------------------------------------------------------------------
# レポート基盤（compat-test/README.md の出力レイアウトに準拠）
# ---------------------------------------------------------------------------
if (-not $ReportDir) {
  $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
  $ReportDir = Join-Path (Get-Location) "compat-report-$stamp"
}
$null = New-Item -ItemType Directory -Path $ReportDir -Force
$logPath = Join-Path $ReportDir "msix_install_uninstall.log"
$jsonPath = Join-Path $ReportDir "report.json"

$script:Results = [System.Collections.Generic.List[object]]::new()

function Write-Step {
  param([string]$Message)
  $line = "[{0}] {1}" -f (Get-Date -Format "HH:mm:ss"), $Message
  Write-Host $line
  Add-Content -Path $logPath -Value $line
}

function Add-Check {
  param(
    [string]$Name,
    [ValidateSet("PASS", "FAIL", "SKIP", "INFO")]
    [string]$Status,
    [string]$Detail = ""
  )
  $script:Results.Add([ordered]@{ name = $Name; status = $Status; detail = $Detail })
  $color = switch ($Status) {
    "PASS" { "Green" }
    "FAIL" { "Red" }
    "SKIP" { "Yellow" }
    default { "Gray" }
  }
  Write-Host ("  {0,-5} {1}" -f $Status, $Name) -ForegroundColor $color
  if ($Detail) { Write-Host ("        {0}" -f $Detail) -ForegroundColor DarkGray }
  Add-Content -Path $logPath -Value ("  {0,-5} {1} {2}" -f $Status, $Name, $Detail)
}

# ---------------------------------------------------------------------------
# 登録状態プローブ（経路非依存の観測。経路別の合否判定は PoC 確定後に強化）
# ---------------------------------------------------------------------------
function Test-ClsidRegistered {
  # Option A（external-location / regsvr32 機械全体登録）は HKLM に書く。
  # Option B/C（通常 MSIX comServer）は MSIX が OS 内部 CLSID 登録を行い、
  # 必ずしも素の HKLM/HKCU キーとして観測できない（manifest 由来の仮想登録）。
  # ここでは観測可能な registry view を best-effort で確認する。
  $paths = @(
    "HKLM:\Software\Classes\CLSID\$Clsid",
    "HKCU:\Software\Classes\CLSID\$Clsid",
    "HKLM:\Software\WOW6432Node\Classes\CLSID\$Clsid"
  )
  foreach ($p in $paths) {
    if (Test-Path $p) { return $p }
  }
  return $null
}

function Get-LangProfileKey {
  # CTF\TIP の LanguageProfile キーは LANGID を 8 桁 16 進（例 0x00000411）で持つ。
  # 入力 $LangId は "0x0411" / "0x00000411" / "411" いずれでも受ける。
  $val = [Convert]::ToInt32(($LangId -replace '^0x', ''), 16)
  return ('0x{0:x8}' -f $val)
}

function Test-TsfRegistrationKey {
  # テキストサービス本体（カテゴリ含む）の登録キー CTF\TIP\{CLSID}。
  # アンインストール後の「残骸 0」判定に使う（言語プロファイル単体だけでなく
  # トップレベルキーの残存も検出するため）。
  $paths = @(
    "HKLM:\Software\Microsoft\CTF\TIP\$Clsid",
    "HKLM:\Software\WOW6432Node\Microsoft\CTF\TIP\$Clsid"
  )
  foreach ($p in $paths) {
    if (Test-Path $p) { return $p }
  }
  return $null
}

function Test-TsfLanguageProfileRegistered {
  # 具体的な ja-JP 言語プロファイル
  # CTF\TIP\{CLSID}\LanguageProfile\{LANGID8}\{ProfileGUID} を確認する
  # （DllMain.cpp の RegisterProfile 経由、machine-wide）。トップレベルの
  # CTF\TIP\{CLSID} の存在だけでは、テキストサービスは登録済みでも ja-JP
  # 言語プロファイルが未登録という言語バー回帰を見逃すため、LANGID /
  # ProfileGUID まで含めた具体キーを判定する。
  $langKey = Get-LangProfileKey
  $paths = @(
    "HKLM:\Software\Microsoft\CTF\TIP\$Clsid\LanguageProfile\$langKey\$ProfileGuid",
    "HKLM:\Software\WOW6432Node\Microsoft\CTF\TIP\$Clsid\LanguageProfile\$langKey\$ProfileGuid"
  )
  foreach ($p in $paths) {
    if (Test-Path $p) { return $p }
  }
  return $null
}

# ---------------------------------------------------------------------------
# 本体
# ---------------------------------------------------------------------------
Write-Step "MSIX install/uninstall smoke harness 開始 (DEV-101 / M28)"
Write-Step "MsixPath=$MsixPath PackageName=$PackageName"
Write-Step "Clsid=$Clsid ProfileGuid=$ProfileGuid LangId=$LangId"
Write-Step "ReportDir=$ReportDir"

if (-not (Test-Path $MsixPath)) {
  Add-Check -Name "MSIX ファイルが存在する" -Status "FAIL" -Detail "not found: $MsixPath"
  throw "MSIX package not found: $MsixPath"
}
Add-Check -Name "MSIX ファイルが存在する" -Status "PASS" -Detail $MsixPath

# 事前状態: 同名パッケージが残っていないこと（クリーン VM 前提の確認）
$pre = Get-AppxPackage -Name $PackageName -ErrorAction SilentlyContinue
if ($pre) {
  Add-Check -Name "事前にパッケージが未インストール" -Status "INFO" `
    -Detail "既存インストールを検出: $($pre.PackageFullName)。クリーン VM での実行を推奨。"
} else {
  Add-Check -Name "事前にパッケージが未インストール" -Status "PASS"
}

$installed = $false
# Add 時に実際に追加されたパッケージの full name。-PackageName が stale でも
# before/after 差分で確定し、finally で確実にクリーンアップするための真実源。
$addedFullNames = @()
try {
  # --- インストール ---
  Write-Step "Add-AppxPackage 実行"
  # 名前検索に頼らず、Add 前後のパッケージ集合差分で「実際に追加された」ものを
  # 特定する。-PackageName が AppxManifest の Identity@Name と食い違っても
  # 取り逃さず、クリーン VM にパッケージを置き去りにしない（残骸検証の前提を保つ）。
  $before = @(Get-AppxPackage | ForEach-Object { $_.PackageFullName })
  try {
    if ($ExternalLocation) {
      # Option A（external-location / sparse identity package）: 実行体は MSIX の外に
      # 置かれるため、-ExternalLocation で install ディレクトリを紐付けないと host に
      # package identity が付かず smoke が無意味になる。
      if (-not (Test-Path $ExternalLocation)) {
        Add-Check -Name "ExternalLocation が存在する" -Status "FAIL" `
          -Detail "指定された外部 install ディレクトリが存在しない: $ExternalLocation"
        throw "ExternalLocation not found: $ExternalLocation"
      }
      $absExternal = (Resolve-Path -LiteralPath $ExternalLocation).ProviderPath
      Write-Step "external-location 登録: $absExternal"
      Add-AppxPackage -Path $MsixPath -ExternalLocation $absExternal
    } else {
      # Option B/C（バイナリ同梱の通常 MSIX）: 外部ロケーション不要。
      # ただし identity package（Option A）を -ExternalLocation 無しで登録すると
      # host に identity が付かないため、Option A 検証では -ExternalLocation を渡すこと。
      Add-Check -Name "ExternalLocation の指定" -Status "INFO" `
        -Detail "-ExternalLocation 未指定。Option A（sparse identity package）を検証する場合は必須（未指定だと host に package identity が付かず smoke が無意味）。Option B/C なら不要。"
      Add-AppxPackage -Path $MsixPath
    }
    $added = @(Get-AppxPackage | Where-Object { $before -notcontains $_.PackageFullName })
    $addedFullNames = @($added | ForEach-Object { $_.PackageFullName })
    if ($addedFullNames.Count -gt 0) {
      $installed = $true
      Add-Check -Name "Add-AppxPackage 成功" -Status "PASS" -Detail ($addedFullNames -join ", ")
      # -PackageName が実 Identity と一致するかを交差確認（不一致だと name ベースの
      # 事前チェックが意味を持たないため明示的に FAIL で気付かせる）。
      if (-not (Get-AppxPackage -Name $PackageName -ErrorAction SilentlyContinue)) {
        Add-Check -Name "PackageName が実 Identity と一致する" -Status "FAIL" `
          -Detail "追加された $($addedFullNames -join ', ') が -PackageName '$PackageName' に一致しない。AppxManifest の Identity@Name に合わせること。"
      }
    } else {
      Add-Check -Name "Add-AppxPackage 成功" -Status "FAIL" `
        -Detail "Add-AppxPackage はエラーを返さなかったが新規パッケージを検出できない"
    }
  } catch {
    # Publisher 不一致 (0x8007000B) は spec §2.2 を参照。
    Add-Check -Name "Add-AppxPackage 成功" -Status "FAIL" -Detail $_.Exception.Message
  }

  # --- インストール後: 登録の存在確認 ---
  if ($installed) {
    $clsidPath = Test-ClsidRegistered
    if ($clsidPath) {
      Add-Check -Name "インストール後に CLSID 登録が存在する" -Status "PASS" -Detail $clsidPath
    } else {
      # Option B/C では素のレジストリに現れない場合があるため FAIL ではなく INFO。
      # 経路確定後に Option A は PASS 必須へ昇格する（TODO）。
      Add-Check -Name "インストール後に CLSID 登録が存在する" -Status "INFO" `
        -Detail "観測可能な HKLM/HKCU CLSID キー無し。Option B/C の OS 内部登録の可能性（spec §1.0）。"
    }

    $tsfProfilePath = Test-TsfLanguageProfileRegistered
    $tsfTopKey = Test-TsfRegistrationKey
    if ($tsfProfilePath) {
      Add-Check -Name "インストール後に ja-JP TSF 言語プロファイル登録が存在する" -Status "PASS" -Detail $tsfProfilePath
    } elseif ($tsfTopKey) {
      # テキストサービス本体は登録されているのに ja-JP 言語プロファイルが無い =
      # 言語バーに出ない部分登録。本ハーネスが捕まえるべき回帰なので FAIL。
      Add-Check -Name "インストール後に ja-JP TSF 言語プロファイル登録が存在する" -Status "FAIL" `
        -Detail "テキストサービスキー ($tsfTopKey) は存在するが LanguageProfile\$(Get-LangProfileKey)\$ProfileGuid が無い（ja-JP 言語プロファイル未登録＝言語バー回帰）。"
    } else {
      Add-Check -Name "インストール後に ja-JP TSF 言語プロファイル登録が存在する" -Status "INFO" `
        -Detail "CTF\TIP に machine-wide 登録無し。MSIX 経路では host 初回起動時の ITfInputProcessorProfiles::Register が契機（spec §1.1 TSF Profile 登録のライフサイクル）。"
    }

    # 言語バーに azooKey が出るかは UI Automation / windows-mcp で別途確認。
    Add-Check -Name "言語バーから azooKey を選択できる" -Status "SKIP" `
      -Detail "UI 操作は windows-mcp / UI Automation で別途検証（本ハーネスは登録レベルのみ）。"
  }

  # --- アンインストール ---
  if ($installed) {
    Write-Step "Remove-AppxPackage 実行"
    try {
      foreach ($fn in $addedFullNames) { Remove-AppxPackage -Package $fn }
      $still = @(Get-AppxPackage | Where-Object { $addedFullNames -contains $_.PackageFullName })
      if ($still.Count -gt 0) {
        Add-Check -Name "Remove-AppxPackage 成功" -Status "FAIL" `
          -Detail "まだ残存: $(($still | ForEach-Object { $_.PackageFullName }) -join ', ')"
      } else {
        $installed = $false
        Add-Check -Name "Remove-AppxPackage 成功" -Status "PASS"
      }
    } catch {
      Add-Check -Name "Remove-AppxPackage 成功" -Status "FAIL" -Detail $_.Exception.Message
    }
  }

  # --- アンインストール後: 残骸 0 確認 ---
  $clsidAfter = Test-ClsidRegistered
  if ($clsidAfter) {
    Add-Check -Name "アンインストール後に CLSID 残骸が無い" -Status "FAIL" `
      -Detail "残存キー: $clsidAfter（spec §1.5『残骸なく消える』違反 / リリース blocker）"
  } else {
    Add-Check -Name "アンインストール後に CLSID 残骸が無い" -Status "PASS"
  }

  # 残骸はトップレベルキー（テキストサービス本体）の存在で判定する。具体的な
  # 言語プロファイルだけが消えてトップレベルキーが残ると言語バーに死んだ
  # エントリが残るため、LanguageProfile 単体ではなく CTF\TIP\{CLSID} を見る。
  $tsfAfter = Test-TsfRegistrationKey
  if ($tsfAfter) {
    Add-Check -Name "アンインストール後に TSF 登録残骸が無い" -Status "FAIL" `
      -Detail "残存キー: $tsfAfter（言語バーに死んだエントリが残る / リリース blocker）"
  } else {
    Add-Check -Name "アンインストール後に TSF 登録残骸が無い" -Status "PASS"
  }
} finally {
  # 後始末: アサーション失敗で抜けても、テストで入れたパッケージは必ず剥がす。
  # 名前検索ではなく Add 時に確定した full name で剥がすので、-PackageName が
  # stale でも置き去りにならない。
  if ($installed -and $addedFullNames.Count -gt 0) {
    Write-Step "クリーンアップ: 残存パッケージを Remove-AppxPackage"
    foreach ($fn in $addedFullNames) {
      try {
        Remove-AppxPackage -Package $fn -ErrorAction SilentlyContinue
      } catch {
        Write-Warning "クリーンアップに失敗 ($fn): $_"
      }
    }
  }
}

# ---------------------------------------------------------------------------
# レポート出力
# ---------------------------------------------------------------------------
$summary = [ordered]@{
  tool        = "msix_install_uninstall.ps1"
  issue       = "DEV-101"
  msixPath    = $MsixPath
  packageName = $PackageName
  externalLocation = $ExternalLocation
  clsid       = $Clsid
  profileGuid = $ProfileGuid
  langId      = $LangId
  timestamp   = (Get-Date -Format "o")
  results     = $script:Results
}
$summary | ConvertTo-Json -Depth 6 | Set-Content -Path $jsonPath -Encoding UTF8

$fail = ($script:Results | Where-Object { $_.status -eq "FAIL" }).Count
$pass = ($script:Results | Where-Object { $_.status -eq "PASS" }).Count
$skip = ($script:Results | Where-Object { $_.status -eq "SKIP" }).Count
Write-Step ("完了: PASS={0} FAIL={1} SKIP={2} -> {3}" -f $pass, $fail, $skip, $jsonPath)

if ($fail -gt 0) {
  Write-Host "FAIL が存在します。レポートを確認してください: $jsonPath" -ForegroundColor Red
  exit 1
}
exit 0
