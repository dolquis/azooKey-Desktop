# MSIX Option A identity package（M28 PoC）

DEV-101（D-05 M28: MSIX `com4:ComServer` と TIP CLSID/Profile 登録の整合 smoke）の
**配布経路 Option A（external-location packaging / sparse package）**の PoC 一式。

仕様の正典は [`docs/sideload-packaging-spec.md`](../../docs/sideload-packaging-spec.md) §1
（特に §1.0 経路 3 候補・§1.1 Option B 参考例・§1.5 受け入れ条件）。状態・進捗の正典は
Linear（DEV-101 / 実機検証は DEV-267）。

> **⚠️ PoC 草案・未検証**: 本ディレクトリのファイルは実 Windows ビルド / 実機 VM で
> schema validation・登録検証していない。緑化は **DEV-267（`gate:human-required`）** の担当。

## Option A とは（なぜ sparse か）

Option A は **バイナリを同梱しない identity package** を登録する軽量経路。

- TIP DLL（`azookey_tsf_tip.dll`）や `azookey_inference_host.exe` は **MSIX の外**
  （external location = §4 WiX/MSI が配置する install ディレクトリ）に置く。
- TIP の COM 登録は従来どおり [`scripts/register-dev.ps1`](../../scripts/register-dev.ps1) の
  `regsvr32` 経由（machine-wide HKLM `InprocServer32` + TSF プロファイル）で行う。
- したがって manifest に `com4:Class` / CLSID を書かない。これが Option B/C
  （`com4:InProcessServer` の install-location ACL 制限）を回避し、`com4` の
  build 20348 要件を負わず **Win10 22H2（build 19045）でも動く**理由（spec §1.0 / §1.5）。

identity package は **MS Store 提出物ではない**（サイドロード登録専用）。§0 の配布方針
（MVP 直接配布 = MSI/WiX §4、MS Store MSIX = DEV-416、スタンドアロン MSIX サイドロードは
当面延期）は本 PoC で変更しない。本 PoC は DEV-267 のローカル VM smoke 用。

## ファイル

| ファイル | 役割 |
|---|---|
| `AppxManifest.xml` | identity package manifest（`uap10:AllowExternalContent=true`、`ProcessorArchitecture=neutral`、`runFullTrust` + `unvirtualizedResources`）。 |
| `azookey_inference_host.exe.manifest` | app 側 side-by-side manifest。`<msix>` 要素で exe を package identity に紐付ける（ビルド時に埋め込み済み、下記）。 |
| `build-identity-package.ps1` | MakeAppx `/nv` でパッケージ化＋任意署名する **canonical** ビルド経路（VS 拡張非依存）。 |
| `Package.wapproj` | VS-IDE 向け convenience 経路（「Package with External Location」拡張が必要）。 |

## ビルド → 署名 → 登録 → smoke

### 1. パッケージ化

```powershell
pwsh -File .\build-identity-package.ps1
# → pkg\msix\out\azooKey-identity-1.0.0.0.msix
```

`/nv`（参照ファイルパス検証の bypass）は external-content manifest に必須。

### 2. 署名（dev cert 例）

identity package は登録前に**信頼された証明書での署名が必須**。自己署名 dev cert の例:

> **⚠️ 秘密鍵（.pfx）は `pkg/msix` の外に置く**: `build-identity-package.ps1` は clean staging
> から pack するため `pkg/msix` 内の `.pfx` を .msix へ同梱しないが、二重の安全策として鍵は
> パッケージ対象ツリー外（下記 `$certDir`）へ出力する。`pkg/msix/.gitignore` も `*.pfx` / `*.cer` /
> `*.msix` / `out/` を無視する。

```powershell
# 鍵はパッケージ対象ツリー外へ（例: リポジトリ外の一時ディレクトリ）。
$certDir = Join-Path $env:TEMP "azooKey-dev-certs"
New-Item -ItemType Directory -Path $certDir -Force | Out-Null

# dev 証明書を作成（例）。Subject は AppxManifest.xml の <Identity Publisher> と一致必須。
$cert = New-SelfSignedCertificate -Type Custom -Subject "CN=dolquis" `
  -KeyUsage DigitalSignature -FriendlyName "azooKey dev" `
  -CertStoreLocation "Cert:\CurrentUser\My" `
  -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3", "2.5.29.19={text}")
# .pfx へエクスポートして署名（.pfx はツリー外）。パスワードは SecureString で扱い、
# コマンド履歴やスクリプトに平文を残さない。
$pw = Read-Host "pfx password" -AsSecureString
Export-PfxCertificate -Cert $cert -FilePath (Join-Path $certDir "azooKey-dev.pfx") -Password $pw | Out-Null
pwsh -File .\build-identity-package.ps1 -PfxPath (Join-Path $certDir "azooKey-dev.pfx") -PfxPassword $pw

# 公開 .cer を TrustedPeople へ import（未実施だと 0x800B0109 / CERT_E_UNTRUSTEDROOT）
Export-Certificate -Cert $cert -FilePath (Join-Path $certDir "azooKey-dev.cer") | Out-Null
Import-Certificate -FilePath (Join-Path $certDir "azooKey-dev.cer") -CertStoreLocation Cert:\CurrentUser\TrustedPeople
```

本番は OV/EV 証明書 or Azure Trusted Signing（spec §2）。`Publisher` と cert Subject の
整合は spec §2.2（不一致は `Add-AppxPackage` 0x8007000B）。

### 3. 登録（external location 紐付け）

```powershell
# <install-dir> は実行体（azookey_inference_host.exe 等）が実際に置かれる絶対パス。
Add-AppxPackage -Path .\out\azooKey-identity-1.0.0.0.msix -ExternalLocation "C:\Program Files\azooKey"
```

`-ExternalLocation` が実行体の実ディレクトリと一致しないと runtime に identity が付かない
（症状: `Package.Current` が null）。per-machine は `-Stage` + `Add-AppxProvisionedPackage`
（Microsoft Learn「Register the identity package」参照）。

### 4. smoke（DEV-267）

登録／解除の整合と残骸 0 は [`compat-test/msix_install_uninstall.ps1`](../../compat-test/msix_install_uninstall.ps1)
で検証する（既定 `-PackageName dolquis.azooKey` は本 manifest の `Identity@Name` と一致）。
**Option A（sparse）では `-ExternalLocation` が必須**（未指定だと host に package identity が
付かず smoke が無意味）。external location は実行体の実 install ディレクトリの絶対パス:

```powershell
pwsh -File ..\..\compat-test\msix_install_uninstall.ps1 `
  -MsixPath .\out\azooKey-identity-1.0.0.0.msix `
  -ExternalLocation "C:\Program Files\azooKey"
```

COM 登録ラウンドトリップ自体は CTest `tsf_tip_com_smoke_tests::TsfTipRegistrationSmokeTest`
が担い、本 PoC はその上の MSIX パッケージング層を補完する。

## app 側 identity metadata（ビルド埋め込み）

`azookey_inference_host.exe.manifest` の `<msix>` 3 属性（`packageName` / `publisher` /
`applicationId`）は `AppxManifest.xml` の `Identity@Name` / `Identity@Publisher` /
`Application@Id` と一致必須（不一致は `Add-AppxPackage` は成功するのに runtime で identity が
欠落する 0x80073D54）。

この side-by-side manifest は `inference-host/CMakeLists.txt` が `azookey_inference_host` の
`.manifest` ソースとして追加し、MSVC ビルドで CMake が `mt.exe` 経由でリンカ生成 manifest と
マージして `azookey_inference_host.exe` へ埋め込む。埋め込みが
無いと `-ExternalLocation` 付きで登録しても host に package identity が付かない
（`Package.Current` が null）。identity package を登録していない環境では `<msix>` 要素は
無視されるだけなので、通常の開発ビルドの挙動は変わらない。埋め込みを外すには CMake の
`-DAZOOKEY_EMBED_MSIX_IDENTITY=OFF`。

3 属性の整合・Option A の不変条件（`com4` 宣言を持たないこと等）・smoke ハーネスの既定値と
`kTextServiceClsid` / `kTextServiceProfileGuid` の一致は、
[`scripts/tests/msix-identity-consistency.Tests.ps1`](../../scripts/tests/msix-identity-consistency.Tests.ps1)
が CI（PowerShell lint/test ジョブ）で検証する。実機 VM を要さない静的整合はここで落とす。
署名 cert 確定時は両 manifest の `Publisher` を同時に差し替える。

## 既知の未確定・フォローアップ

- `azookey_settings.exe` は未実装（M11/M30）。実装後に `AppxManifest.xml` へ同形式の
  `Application` と、対応する `.exe.manifest` + そのビルド埋め込みを追加する。
- 経路確定（Option A/B/C）とハーネスの経路別アサーション強化は spec §1.0 / 雛形の TODO。
  本 PoC は Option A を具体化するが、最終確定は DEV-267 の実機 smoke 結果を待つ。
