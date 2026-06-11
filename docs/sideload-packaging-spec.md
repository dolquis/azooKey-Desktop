# サイドロード配信 仕様（Phase 7）

本書は azooKey-Desktop Windows 版の配布形態と署名・更新・観測仕様を定める。
`plans/windows-port-roadmap.md` の Phase 7 の M28〜M34 が本書を参照する。

**Microsoft Store 配信は対象外** とし、サイドロード（自己署名 + EV/OV 証明書
配布）に専念する。

## 1. MSIX サイドロード（M28）

> **⚠️ OPEN ISSUE — M28 着手時に PoC 必須**: MSIX に TIP DLL を同梱する経路は、
> Microsoft 公式仕様の現状で**機能制限あり**の領域である。`com4:InProcessServer`
> は「外部位置（external location / sparse package）向け」と明記されており、
> 通常の `.msix` では install location ACL により **外部クライアント（ctfmon
> 等）が TIP DLL を読み込めない**（[com4:Extension Examples](https://learn.microsoft.com/uwp/schemas/appxpackage/uapmanifestschema/element-com4-extension#examples)）。本書 §1.1 の `com4:InProcessServer` 例は
> 暫定参考であり、M28 着手前に下表のいずれかへ確定する必要がある。Linear
> [DEV-101](https://linear.app/dolquis/issue/DEV-101) で追跡。

### 1.0 TIP 配布経路 3 候補

| 経路 | DLL ロード可否 | 配布形態 | OS 要件 | 評価 |
|---|---|---|---|---|
| A. **external-location packaging (sparse package)** + 既存 `regsvr32` 経路 | ◎ 外部から in-proc ロード可 | sparse manifest + 自前 installer（or xcopy） | Win10 2004 (19041) / Win11 21H2+ | 推奨候補。TIP DLL を MSIX 外に置けば従来の `InprocServer32` レジストリで動く |
| B. **通常 `.msix` + `com4:InProcessServer`** | ✗ ACL でブロックされる可能性大 | フル MSIX | Win10 21H2 server (20348+) / Win11 21H2+ | Microsoft が「外部位置向け」と明記。通常 MSIX では動かない事例あり |
| C. **通常 `.msix` + `com4:SurrogateServer`**（[ClassReference Remarks](https://learn.microsoft.com/uwp/schemas/appxpackage/uapmanifestschema/element-com4-inprocessserverclassreference#remarks) が推奨） | △ out-of-proc 経路。TIP の標準的 in-proc 活性化と semantics が異なる | フル MSIX | Win10 21H2 server / Win11 21H2+ | runFullTrust 下で COM 活性化は確実だが、TIP 動作と整合するか要検証 |

> 補足: D ルートとして **WiX/MSI installer** で従来の `regsvr32` 経路（§4）を
> 使う選択肢もある。MSIX 不可環境（LTSC 等）向けには結局 §4 を持つので、
> v1.0 は §4 / WiX で確定 + §1 は M28 PoC へ送る選択もあり得る。

### 1.1 AppxManifest.xml（Option B の参考例。M28 PoC で要確定）

> 以下は Option B（通常 MSIX + `com4:InProcessServer`）の schema-valid な
> 雛形である。**この XML 単体ではアクティベーションが成立しない可能性**を
> 上記 §1.0 で示したため、M28 着手時に Option A への切替を含めた PoC で
> 確定すること。schema validation の観点では以下 4 点を満たす:
>
> * `com4:Extension` / `com4:InProcessServer` は **Win10 build 20348+** を要求
>   するので、`MinVersion="10.0.20348.0"` まで引き上げる
> * `IgnorableNamespaces` に `com4` を追加
> * `com4:Class` の `Virtualization` は **必須属性**（`enabled` or `disabled`）
> * `com4:InProcessServerDll` は **必須**（`Path` + `ProcessorArchitecture`）

`pkg/msix/AppxManifest.xml`（新規）：

```xml
<?xml version="1.0" encoding="utf-8"?>
<Package
    xmlns="http://schemas.microsoft.com/appx/manifest/foundation/windows10"
    xmlns:uap="http://schemas.microsoft.com/appx/manifest/uap/windows10"
    xmlns:uap3="http://schemas.microsoft.com/appx/manifest/uap/windows10/3"
    xmlns:com4="http://schemas.microsoft.com/appx/manifest/com/windows10/4"
    xmlns:rescap="http://schemas.microsoft.com/appx/manifest/foundation/windows10/restrictedcapabilities"
    IgnorableNamespaces="uap uap3 com4 rescap">

  <Identity Name="dolquis.azooKey"
            Publisher="CN=dolquis"
            Version="1.0.0.0"
            ProcessorArchitecture="x64" />

  <Properties>
    <DisplayName>azooKey</DisplayName>
    <PublisherDisplayName>dolquis</PublisherDisplayName>
    <Logo>Assets\StoreLogo.png</Logo>
  </Properties>

  <Dependencies>
    <!-- com4:Extension / com4:InProcessServer は Windows 10 build 20348 以上を
         要求するため MinVersion を引き上げる。Win10 22H2 (build 19045) では
         schema validation で reject される。 -->
    <TargetDeviceFamily Name="Windows.Desktop"
                       MinVersion="10.0.20348.0"
                       MaxVersionTested="10.0.22631.0" />
  </Dependencies>

  <Applications>
    <Application Id="azooKey" Executable="azookey_inference_host.exe"
                 EntryPoint="Windows.FullTrustApplication">
      <uap:VisualElements DisplayName="azooKey"
                          Description="azooKey 日本語入力"
                          BackgroundColor="transparent"
                          Square150x150Logo="Assets\Square150x150Logo.png"
                          Square44x44Logo="Assets\Square44x44Logo.png" />
      <Extensions>
        <!-- 起動時に Host を立ち上げ -->
        <uap3:Extension Category="windows.appExecutionAlias">
          <uap3:AppExecutionAlias>
            <uap3:ExecutionAlias Alias="azookey-host.exe" />
          </uap3:AppExecutionAlias>
        </uap3:Extension>

        <!-- TIP の COM 登録（Option B 参考例。§1.0 の制限を再確認のこと）。 -->
        <com4:Extension Category="windows.comServer">
          <com4:ComServer>
            <com4:InProcessServer>
              <!-- `com4:Class` には Path 属性が無い。DLL パスは
                   `com4:InProcessServerDll` 子要素で明示する（Path +
                   ProcessorArchitecture とも必須）。 -->
              <com4:InProcessServerDll Path="azookey_tsf_tip.dll"
                                       ProcessorArchitecture="x64" />
              <!-- ThreadingModel の許容値は Both / STA / MTA / MainSTA / Neutral
                   ("Apartment" はクラシックレジストリ値で MSIX schema では invalid)。
                   TIP は TSF の standard STA で動作する。Virtualization は
                   `com4:Class` で必須属性、classic COM 互換のため "disabled"。 -->
              <com4:Class Id="{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}"
                          ThreadingModel="STA"
                          Virtualization="disabled" />
            </com4:InProcessServer>
          </com4:ComServer>
        </com4:Extension>

        <!-- TSF Profile 登録は MSIX manifest では扱わない（windows.inputMethod
             は uap3:Extension の許容 Category に存在しない）。runtime に
             `ITfInputProcessorProfiles::Register` を呼ぶ。詳細は本節下記
             「TSF Profile 登録のライフサイクル」を参照。 -->
      </Extensions>
    </Application>
  </Applications>

  <Capabilities>
    <rescap:Capability Name="runFullTrust" />
  </Capabilities>
</Package>
```

`Class Id` と `Profile GUID` は `tsf-tip/src/DllMain.cpp` の `kTextServiceClsid`
/ `kProfileGuid` と一致させる。

#### MSIX `comServer` の AAP（Activate As Package）挙動と既知制限

MSIX に同梱した COM サーバは、`regsvr32` で登録した classic な COM サーバと異な
り **Activate As Package (AAP)** で活性化される。具体的には:

* package 識別子と app identity 込みの user session token で実行される
* `runFullTrust` capability を宣言した本パッケージでは追加の制限なく動作する
* `RunAs` 等の代替活性化挙動は **サポートされない**（[`com4:ComServer` Remarks](https://learn.microsoft.com/uwp/schemas/appxpackage/uapmanifestschema/element-com4-comserver#remarks)）

**ただし in-proc サーバの DLL ロードに既知制限**:

* `com4:InProcessServer` は「**packages with external location** での利用が
  想定されており、通常 MSIX では install location の ACL により外部クライアント
  が DLL をロードできない場合がある」（[com4:Extension Examples](https://learn.microsoft.com/uwp/schemas/appxpackage/uapmanifestschema/element-com4-extension#examples)）
* TSF / ctfmon は TIP DLL を外部クライアントとしてロードするので、この制限に
  直撃する可能性が高い。Option A（sparse package + 外部 DLL 配置）への切替を
  M28 着手時に PoC で検証する
* [`com4:InProcessServerClassReference` Remarks](https://learn.microsoft.com/uwp/schemas/appxpackage/uapmanifestschema/element-com4-inprocessserverclassreference#remarks) は packaged app では
  runFullTrust + SurrogateServer のみの登録（Option C）を推奨。ただし TIP は
  in-proc 活性化が前提なので surrogate にすると挙動が変わる可能性がある

TIP 側のレジストリ登録（`DllRegisterServer` 内の HKCU `Software\Classes\CLSID\...`）
は MSIX 経路では使われない。MSIX manifest が registry を上書きする形になるた
め、TIP 側の自己登録ロジックは MSIX 同梱時に動かないことを前提に書く。
**ただし Option A（external location）を採用する場合は逆に従来の `regsvr32`
経路で `InprocServer32` を登録するため、TIP 側自己登録ロジックがそのまま使える**。

#### TSF Profile 登録のライフサイクル

TSF Profile（Language Profile）の登録には **MSIX manifest の専用拡張は存在しない**。
`uap3:Extension` の許容 `Category` は `windows.appExecutionAlias` /
`windows.protocol` / `windows.fileTypeAssociation` / `windows.appExtension` 系
のみで、`windows.inputMethod` カテゴリは無い（[uap3:Extension 仕様](https://learn.microsoft.com/uwp/schemas/appxpackage/uapmanifestschema/element-uap3-extension)）。
よって TSF Profile 登録は **runtime に `ITfInputProcessorProfiles::Register` /
`Unregister` を呼ぶ**しか方法がない。Microsoft の TSF ドキュメント [Text Service
Registration](https://learn.microsoft.com/windows/win32/tsf/text-service-registration) も同じ規約。

* MSIX 経路 (Option B/C) — **登録**: `DllMain.cpp::DllRegisterServer` は MSIX
  経由では呼ばれないため、`Application` の `Executable`（本書では
  `azookey_inference_host.exe`）に Profile 登録ロジックを組み込み、初回起動 /
  再活性時に `ITfInputProcessorProfiles::Register` を idempotent に呼ぶ。
* MSIX 経路 (Option B/C) — **解除の既知制限**: MSIX には標準の **pre-uninstall
  hook が存在しない**（`Remove-AppxPackage` / 設定アプリのアンインストールは
  package を atomic に削除し、host プロセスを kill するだけ）。したがって
  `Remove-AppxPackage` だけでは `ITfInputProcessorProfiles::Unregister` を呼ぶ
  契機が無く、§1.5 受け入れ条件「言語バーから死んだエントリが消える」を
  自動で満たせない。M28 PoC で次のいずれかを確定する:
    1. **Companion uninstall script を README で案内**: `Remove-AppxPackage` の
       前にユーザーに `azookey-host.exe userdict unregister-profile` を実行させ、
       明示的に `Unregister` を呼ぶ。
    2. **External-location（Option A）への変更**: 従来の `regsvr32 /u` 経路で
       `DllUnregisterServer` を呼べるので、本制限は発生しない。
    3. **PackageCatalog observer process**: `PackageCatalog.PackageStatusChanged`
       を独立 worker / Windows service で監視し、自パッケージの uninstall を
       検出して `Unregister` を呼ぶ。実装コストが高く、v1.0 範囲外と想定。

    本制限は MSIX 同梱 TIP の本質的な課題であり、Option A の優位性として記録する。
* Option A（external location）: 従来の `regsvr32 azookey_tsf_tip.dll` 経路で
  `DllRegisterServer` / `DllUnregisterServer` が呼ばれるので、その中で
  `ITfInputProcessorProfiles::Register` / `Unregister` を従来通り呼んで完結する。
* `DllRegisterServer` / `DllUnregisterServer` を MSIX context で誤って呼ぶと
  HKCU `\Software\Classes\CLSID\...` に二重エントリが残るので、TIP 内では
  `IsRunningInMsixContext()`（Win32 API `GetCurrentPackageFamilyName` の戻り値
  で判定）で MSIX context での自己登録を skip させる。

### 1.1.1 HKCU 開発用登録 vs MSIX 登録の取り違え事故防止

`scripts/register.ps1` / `unregister.ps1` は HKCU を直接書く開発用スクリプトで
あり、MSIX 経路と衝突する。両者を取り違えると、片方の登録解除が漏れて言語バー
に古いエントリが残る (M28 設計メモ)。本書では以下を運用ルールとして固定:

* `scripts/register-dev.ps1` / `unregister-dev.ps1` にリネーム（接尾辞 `-dev`
  を必須化）
* MSIX 同梱の TIP は HKCU 自己登録ロジックを skip（上記 `IsRunningInMsixContext`
  で分岐）
* CI / ローカル開発で MSIX と `regsvr32` を併用する場合は、`compat-test/
  msix_install_uninstall.ps1` の smoke ハーネスで残骸 0 を確認

### 1.2 Package.wapproj

`pkg/msix/Package.wapproj`（新規、WAP プロジェクト）：

- ターゲット: x64 / arm64
- 同梱物:
  - `azookey_tsf_tip.dll`
  - `azookey_inference_host.exe`
  - `azookey_settings.exe`（M30）
  - `Assets/*.png`
  - `models/`（gguf）は **MSIX に含めない**（サイズ過大）→ 初回起動時に
    GitHub Release から DL

### 1.3 Microsoft.VCRTForwarders 同梱

C++ ランタイム依存（`msvcp140.dll` 等）を MSIX に同梱：

```xml
<Dependencies>
  <PackageDependency Name="Microsoft.VCLibs.140.00.UWPDesktop"
                     MinVersion="14.0.32530.0"
                     Publisher="CN=Microsoft Corporation, O=Microsoft Corporation, L=Redmond, S=Washington, C=US" />
</Dependencies>
```

これにより、ユーザー環境に VCRedist が無くても動作する。

### 1.4 インストール / アンインストール

```powershell
# インストール
Add-AppxPackage -Path .\azooKey-1.0.0.msix

# アンインストール
Get-AppxPackage -Name dolquis.azooKey | Remove-AppxPackage
```

### 1.5 受け入れ条件

OS ターゲットは §1.0 で選定する経路に依存する。同 PoC で 1 経路に確定したら、
当該経路の受け入れ条件を満たす。

| 経路 | Win11 23H2 | Win10 22H2 (build 19045) | Win Server 2022 (build 20348+) |
|---|---|---|---|
| A. external-location packaging | ✅ 必須 | ✅ 必須（`com4:InProcessServer` を使わないため build 19045 で動く） | （対象外） |
| B. 通常 MSIX + `com4:InProcessServer` | ✅ 必須 | ✗ schema reject（com4 が build 20348+ を要求） | ✅ |
| C. 通常 MSIX + `com4:SurrogateServer` | ✅ 必須 | ✗ 同上 | ✅ |

共通の受け入れ条件:

- クリーン VM で `Add-AppxPackage`（A は sparse 登録）成功
- 言語バーから azooKey が選べる
- アンインストールで `HKCU\Software\Classes\CLSID\...`（A: `regsvr32` 経路）/
  package manifest 由来の OS 内部 CLSID 登録（B/C）が残骸なく消える
- 同梱辞書アセットの第三者ライセンス（Apache-2.0 / BSD-3-Clause / CC0 /
  CC-BY-4.0 / 権利主張なし（日本郵便 郵便番号データ等のパブリックドメイン
  相当））の `ThirdPartyNotices.txt` 同梱と帰属表示（GeoNames=CC-BY-4.0 は
  帰属必須。BSD-3-Clause は SudachiDict 内包の UniDic 由来で著作権表示必須）は
  `docs/auto-word-registration-spec.md` §14.10 に従う。standalone NEologd 単体
  パックは同梱しない（同 §14.9。ただし SudachiDict 内包の NEologd 由来データの
  帰属は §14.10 に従い ThirdPartyNotices に含める）

> Win10 22H2 必須を維持するなら **Option A 確定が前提**。Option B/C を選ぶ場合
> は本受け入れ条件から Win10 22H2 を外し、Win Server 2022 ベースに置換する。

## 2. EV/OV コード署名（M29）

### 2.0 署名経路の選定

署名証明書の調達ルートは v1.0 / v1.x で 3 候補ある。Microsoft Learn の現行ガイ
ダンスは **Azure Artifact Signing（旧 Trusted Signing）** を非ストア配布の推奨
として提示する（[Code signing options](https://learn.microsoft.com/windows/apps/package-and-deploy/code-signing-options)）。

| 経路 | 推奨度 | 価格 | CI 統合 | SmartScreen 信頼 | 制約 |
|---|---|---|---|---|---|
| A. **Azure Artifact Signing** | 推奨 | ≈$10/月 | ◎（GitHub Actions / Azure DevOps） | reputation building（OV と同等） | 組織: 米/カナダ/EU/英国のみ。個人: 米/カナダのみ |
| B. **Azure Key Vault + [AzureSignTool](https://learn.microsoft.com/windows/msix/desktop/cicd-keyvault)** | 個人向け次善 | Key Vault 料金 + OV cert | ◎ | reputation building | コミュニティ製 .NET ツール（[vcsjones/AzureSignTool](https://github.com/vcsjones/AzureSignTool)） |
| C. **伝統的 OV/EV cert + PFX を GitHub Secrets** | 既存 §2.3 経路 | OV: 数万円/年 / EV: 10 万円超/年 + HSM | △（EV の HSM 物理トークンは不可） | EV のみ即時信頼 | PFX 漏えいリスク、CI でのキー回転が煩雑 |

**v1.0 の判定**: 開発者の所在地・組織化状況に応じて A or B or C を選ぶ。日本の
個人開発者で組織化していない場合は B（Azure Key Vault + AzureSignTool）が現実
的。組織化済みで該当地域なら A を強く推奨。詳細運用と申請手順は別途調査タスク
（[Linear DEV-100](https://linear.app/dolquis/issue/DEV-100)）で確定する。

### 2.1 signtool 引数

```powershell
signtool sign `
    /fd SHA256 `
    /tr http://timestamp.digicert.com `
    /td SHA256 `
    /sha1 <thumbprint> `
    /d "azooKey" `
    /du "https://github.com/dolquis/azooKey-Desktop" `
    azookey_tsf_tip.dll `
    azookey_inference_host.exe `
    azookey_settings.exe

# MSIX 自体にも署名
signtool sign /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 `
    /sha1 <thumbprint> azooKey-1.0.0.msix
```

タイムスタンプサーバは複数指定可能（フェイルオーバ）：
- `http://timestamp.digicert.com`
- `http://timestamp.sectigo.com`
- `http://timestamp.globalsign.com/tsa/r6advanced1`

### 2.2 証明書管理

EV/OV 証明書（PFX）を GitHub Secrets に格納：

| Secret 名 | 内容 |
|---|---|
| `WINDOWS_PFX_BASE64` | PFX ファイルを base64 エンコードしたもの |
| `WINDOWS_PFX_PASSWORD` | PFX のパスワード |
| `WINDOWS_CERT_THUMBPRINT` | 証明書のフィンガープリント |

#### Publisher と証明書 Subject の整合（必須）

`AppxManifest.xml` の `Identity@Publisher` 属性は、署名証明書の **Subject
distinguished name (DN) 全体と完全一致**（フィールド順含む）でなければならない。
CN だけでなく `O=` / `OU=` / `L=` / `S=` / `C=` 等のすべての RDN が含まれる。
不一致だと `Add-AppxPackage` が `0x8007000B` ([Publisher name mismatch](https://learn.microsoft.com/windows/msix/msix-troubleshooting-guide#publisher-name-mismatch-0x8007000b-event-id-150)) で失敗する。

* §1.1 manifest 例の `Publisher="CN=dolquis"` は **`CN=dolquis` のみの Subject**
  を持つ自己署名 dev cert で動作する想定（OV/EV cert ではほぼ通らない）
* OV/EV cert 取得後は **certutil / openssl で Subject を取得**し、その全文を
  `Publisher` に貼る。例:
  * 証明書 Subject: `CN=dolquis, O="Sample LLC", L=Tokyo, S=Tokyo, C=JP`
  * manifest: `Publisher="CN=dolquis, O=&quot;Sample LLC&quot;, L=Tokyo, S=Tokyo, C=JP"`
    （XML 属性内のダブルクォートは `&quot;` でエスケープ）
* RDN の順序とスペース・カンマの形式まで完全一致。`Get-PfxCertificate` /
  `signtool /pa /v` で Subject 表記を必ず確認してから manifest 側へ貼る
* CI で署名失敗時は AppxPackagingOM operational log の Event ID 150 を確認

### 2.2.A Azure Artifact Signing 経路（推奨）

Microsoft Identity Verification Root CA 配下で発行されるため、Windows 10 1809+ /
Windows 11 で **既定で信頼される**（追加の Trusted Root インストール不要）。

GitHub Actions ワークフローでの最小構成（詳細は別途公式ドキュメント参照）:

```yaml
- name: Azure CLI login
  uses: azure/login@v2
  with:
    creds: ${{ secrets.AZURE_CREDENTIALS }}

- name: Sign MSIX with Azure Artifact Signing
  uses: azure/trusted-signing-action@v0
  with:
    azure-tenant-id: ${{ secrets.AZURE_TENANT_ID }}
    azure-client-id: ${{ secrets.AZURE_CLIENT_ID }}
    azure-client-secret: ${{ secrets.AZURE_CLIENT_SECRET }}
    endpoint: https://eus.codesigning.azure.net/
    trusted-signing-account-name: <account>
    certificate-profile-name: <profile>
    files-folder: pkg\msix\AppPackages\Package_1.0.0_Test
    files-folder-filter: msix
    file-digest: SHA256
    timestamp-rfc3161: http://timestamp.acs.microsoft.com
```

### 2.2.B Azure Key Vault + AzureSignTool 経路

Key Vault に OV cert を import し、AzureSignTool（.NET global tool）で署名する。
Microsoft Learn の [MSIX and CI/CD Pipeline signing with Azure Key Vault](https://learn.microsoft.com/windows/msix/desktop/cicd-keyvault) に詳細手順あり。

| Secret 名 | 内容 |
|---|---|
| `AZURE_KEY_VAULT_URL` | Key Vault URL |
| `AZURE_KEY_VAULT_TENANT_ID` | Azure AD テナント ID（service principal 認証で必須） |
| `AZURE_KEY_VAULT_CLIENT_ID` | Azure AD アプリ ID |
| `AZURE_KEY_VAULT_CLIENT_SECRET` | クライアントシークレット |
| `AZURE_KEY_VAULT_CERT_NAME` | 証明書フレンドリ名 |

```yaml
- name: Install AzureSignTool
  run: dotnet tool install --global AzureSignTool

- name: Sign MSIX with AzureSignTool
  run: |
    AzureSignTool sign `
      -kvu ${{ secrets.AZURE_KEY_VAULT_URL }} `
      -kvt ${{ secrets.AZURE_KEY_VAULT_TENANT_ID }} `
      -kvi ${{ secrets.AZURE_KEY_VAULT_CLIENT_ID }} `
      -kvs ${{ secrets.AZURE_KEY_VAULT_CLIENT_SECRET }} `
      -kvc ${{ secrets.AZURE_KEY_VAULT_CERT_NAME }} `
      -tr http://timestamp.digicert.com `
      -v pkg\msix\AppPackages\Package_1.0.0_Test\Package_1.0.0_x64.msix
```

`-kvt`（テナント ID）は client ID / secret 認証時に必須。Managed Identity /
アクセストークン認証を使う場合は `-kvt` 不要だが、本書では明示性のため
service principal 経路の例を示す。

物理 USB トークン不要、HSM 連携を Key Vault 内で完結できる利点がある。

### 2.3 CI ステップ

`.github/workflows/release.yml`（新規）：

```yaml
name: Release
on:
  push:
    tags: ['v*']
jobs:
  build:
    runs-on: windows-2022
    steps:
      - uses: actions/checkout@v4
        with: { submodules: recursive }

      - name: Set up MSVC environment
        uses: ilammy/msvc-dev-cmd@v1

      - name: Configure
        run: cmake --preset windows-release -DAZOOKEY_FETCH_GOOGLETEST=ON

      - name: Build
        run: cmake --build --preset windows-release

      - name: Test
        run: ctest --preset windows-release --no-tests=error

      - name: Import certificate
        run: |
          $bytes = [Convert]::FromBase64String("${{ secrets.WINDOWS_PFX_BASE64 }}")
          [IO.File]::WriteAllBytes("$env:TEMP\cert.pfx", $bytes)
          $pwd = ConvertTo-SecureString -String "${{ secrets.WINDOWS_PFX_PASSWORD }}" -AsPlainText -Force
          Import-PfxCertificate -FilePath $env:TEMP\cert.pfx -Password $pwd -CertStoreLocation Cert:\CurrentUser\My

      - name: Sign binaries
        run: |
          $tools = "${env:ProgramFiles(x86)}\Windows Kits\10\bin\10.0.22621.0\x64"
          & "$tools\signtool.exe" sign /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 `
              /sha1 ${{ secrets.WINDOWS_CERT_THUMBPRINT }} `
              build\tsf-tip\Release\azookey_tsf_tip.dll `
              build\inference-host\Release\azookey_inference_host.exe

      - name: Build MSIX
        run: msbuild pkg\msix\Package.wapproj /p:Configuration=Release /p:Platform=x64

      - name: Sign MSIX
        run: |
          & signtool sign /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 `
              /sha1 ${{ secrets.WINDOWS_CERT_THUMBPRINT }} `
              pkg\msix\AppPackages\Package_1.0.0_Test\Package_1.0.0_x64.msix

      - name: Upload Release asset
        uses: softprops/action-gh-release@v2
        with:
          files: pkg\msix\AppPackages\Package_1.0.0_Test\Package_1.0.0_x64.msix
          draft: true
```

### 2.4 ローカル署名検証手順

CI で署名した MSIX を手元で確認する。Windows SDK の `signtool` で:

```powershell
# 署名チェーンを検証（exit 0 で成功）
& signtool verify /pa /v azooKey-1.0.0.msix

# 詳細をテキストファイルへ
& signtool verify /pa /v /all azooKey-1.0.0.msix > sign-verify.log
```

`/pa` は default authentication policy で検証する。`/v` は verbose。chain が
途中の Trusted Root にしか繋がっていない場合は警告が出るので、Azure Artifact
Signing 利用時は問題なし（Microsoft Identity Verification Root が Windows に
組み込まれているため）。

開発者が Azure Key Vault 経路を使う場合は、署名後に `Get-AuthenticodeSignature`
で chain を確認:

```powershell
$sig = Get-AuthenticodeSignature -FilePath .\azooKey-1.0.0.msix
$sig.Status                                  # Valid を期待
$sig.SignerCertificate | Format-List Subject, Issuer, NotAfter
```

## 3. WinUI 3 設定アプリ（M30）

### 3.1 構成

- 言語: C++/WinRT（TIP / Host と統一）
- フレームワーク: WinUI 3 (Windows App SDK 1.5+)
- 配布: MSIX 内同梱（別 EXE `azookey_settings.exe`）

### 3.2 ナビゲーション

| ペイン | 内容 |
|---|---|
| 一般 | 起動時に IME を有効化 / 自動更新 ON/OFF |
| 入力 | inputStyle / customRomajiTablePath / liveConversion / predictionEnabled |
| AI | aiBackend / openAiApiKey / openAiApiEndpoint / promptPrefixByApp |
| 学習 | LearningStore 表示 / エクスポート / リセット / Persona 表示 |
| 詳細 | backendPreference / powerProfile / logLevel / etwProvider |
| 校正 | （M30 後半）バッチ訂正ビュー |
| バージョン | バージョン情報 / 更新確認 / ライセンス |

### 3.3 Host との IPC

設定変更時は Host に `UpdateSettings` メッセージを送信：

```
UpdateSettingsRequest:
  request_id
  settings_json: string   // JSON schema 適用済み

UpdateSettingsResponse:
  request_id
  ok: bool
  error: optional<string>
```

Host は受信した設定を `%LOCALAPPDATA%\azooKey\config\settings.json` に保存し、
即時反映可能なものは適用、再起動が必要なものは `restart_required: bool` を返す。

### 3.4 設定ファイルパス

| 用途 | パス |
|---|---|
| 設定 | `%LOCALAPPDATA%\azooKey\config\settings.json` |
| カスタムローマ字 | `%LOCALAPPDATA%\azooKey\config\custom-romaji.tsv` |
| 学習データ | `%LOCALAPPDATA%\azooKey\data\learning.tsv`（DPAPI 暗号化、M34） |
| ユーザー辞書 | `%LOCALAPPDATA%\azooKey\data\user_dict.json` |
| モデル | `%LOCALAPPDATA%\azooKey\models\zenzai\zenz-v3.1-small-Q5_K_M.gguf` |
| ログ | `%LOCALAPPDATA%\azooKey\logs\*.jsonl` |

## 4. WiX / Inno Setup インストーラ（M31）

MSIX 不可環境（Win10 LTSC, 法人ポリシーで AppX 無効）向け。

### 4.1 WiX 構成

`pkg/wix/Product.wxs`（新規）：

```xml
<?xml version="1.0" encoding="UTF-8"?>
<Wix xmlns="http://wixtoolset.org/schemas/v4/wxs">
  <Package Name="azooKey" Manufacturer="dolquis" Version="1.0.0" UpgradeCode="...">
    <Feature Id="Main" Title="azooKey">
      <ComponentGroupRef Id="Files" />
      <ComponentRef Id="RegisterTip" />
    </Feature>

    <Component Id="RegisterTip" Directory="INSTALLFOLDER" Guid="...">
      <File Source="$(var.TipDll)" />
      <RegistryKey Root="HKCU"
                   Key="Software\Classes\CLSID\{kTextServiceClsid}">
        <RegistryValue Type="string" Value="azooKey TIP" />
      </RegistryKey>
      <!-- ... profile / category 登録 ... -->
    </Component>

    <CustomAction Id="RegisterTipDll"
                  BinaryRef="RegSvr"
                  ExeCommand="/s [INSTALLFOLDER]azookey_tsf_tip.dll"
                  Execute="deferred"
                  Impersonate="no"
                  Return="check" />
  </Package>
</Wix>
```

カスタムアクションで `regsvr32 /s azookey_tsf_tip.dll` を呼び、
`DllRegisterServer` に処理を委譲する（M2 で実装済み）。

### 4.2 アンインストール時

`regsvr32 /u /s azookey_tsf_tip.dll` で `DllUnregisterServer` を呼ぶ。
WiX ScheduleService の `InstallFinalize` で実行。

### 4.3 Inno Setup（代替）

WiX に習熟がない場合 Inno Setup でも可。`pkg/inno/setup.iss`：

```pascal
[Setup]
AppName=azooKey
AppVersion=1.0.0
DefaultDirName={localappdata}\azooKey
DisableDirPage=yes
PrivilegesRequired=lowest

[Files]
Source: "build\tsf-tip\Release\azookey_tsf_tip.dll"; DestDir: "{app}"
Source: "build\inference-host\Release\azookey_inference_host.exe"; DestDir: "{app}"

[Run]
Filename: "regsvr32"; Parameters: "/s ""{app}\azookey_tsf_tip.dll"""; \
    Flags: runhidden waituntilterminated

[UninstallRun]
Filename: "regsvr32"; Parameters: "/u /s ""{app}\azookey_tsf_tip.dll"""; \
    Flags: runhidden waituntilterminated
```

ユーザースコープ（`{localappdata}`、`PrivilegesRequired=lowest`）で
管理者権限なしインストール。

## 5. WinGet マニフェスト（M32）

### 5.1 構成

`manifests/d/dolquis/azooKey/<version>/`（winget-pkgs リポジトリへの PR で配布）：

```
dolquis.azooKey.locale.ja-JP.yaml
dolquis.azooKey.installer.yaml
dolquis.azooKey.yaml
```

#### dolquis.azooKey.installer.yaml

```yaml
PackageIdentifier: dolquis.azooKey
PackageVersion: 1.0.0
Installers:
  - Architecture: x64
    InstallerType: msix
    InstallerUrl: https://github.com/dolquis/azooKey-Desktop/releases/download/v1.0.0/azooKey-1.0.0-x64.msix
    InstallerSha256: <SHA256>
    SignatureSha256: <SignatureSHA256>
  - Architecture: arm64
    InstallerType: msix
    InstallerUrl: https://github.com/dolquis/azooKey-Desktop/releases/download/v1.0.0/azooKey-1.0.0-arm64.msix
    InstallerSha256: <SHA256>
ManifestType: installer
ManifestVersion: 1.5.0
```

#### dolquis.azooKey.locale.ja-JP.yaml

```yaml
PackageIdentifier: dolquis.azooKey
PackageVersion: 1.0.0
PackageLocale: ja-JP
Publisher: dolquis
PackageName: azooKey
License: Apache-2.0
ShortDescription: azooKey 日本語入力 for Windows
Tags: [ime, japanese, ai]
ManifestType: defaultLocale
ManifestVersion: 1.5.0
```

### 5.2 リリースフロー

1. GitHub Release で MSIX を公開
2. SHA256 を計算
3. winget-pkgs リポジトリへ PR
4. マージ後 `winget install dolquis.azooKey` で利用可能

`wingetcreate` で半自動化：

```powershell
wingetcreate update --urls `
  "https://github.com/dolquis/azooKey-Desktop/releases/download/v$ver/azooKey-$ver-x64.msix" `
  --version $ver dolquis.azooKey
```

## 6. 自動更新（M32）

### 6.1 チェック経路

`inference-host/src/UpdateChecker.cpp`（新規）：

```cpp
class UpdateChecker {
public:
    struct Release {
        std::string version;
        std::string url;        // MSIX ダウンロード URL
        std::string sha256;
        std::string body;       // changelog
    };

    // GitHub Releases API を叩く
    std::optional<Release> CheckLatest();

    // 起動時 + 24h タイマで呼ぶ
    void StartPeriodicCheck();
};
```

### 6.2 API

```
GET https://api.github.com/repos/dolquis/azooKey-Desktop/releases/latest
Accept: application/vnd.github+json
User-Agent: azooKey/1.0.0 (Windows)
```

レスポンスから `tag_name` をパースし、現バージョンと比較。

### 6.3 ダウンロード → 適用

新バージョンを検出したら：

1. 通知（トースト）「新しいバージョン v1.1.0 が利用可能」
2. ユーザーが「インストール」をクリック
3. MSIX を `%TEMP%` にダウンロード（`WinHttpReadData`）
4. SHA256 検証
5. `Add-AppxPackage -Path` で更新（既存パッケージは自動アンインストール）

### 6.4 WinSparkle 互換

WinSparkle 互換の `appcast.xml` フィードも `gh-pages` ブランチで提供
（外部ツールからの参照用）。Phase 7 では実装スコープ外、将来課題。

### 6.5 設定

```json
{
  "autoUpdate": {
    "enabled": true,        // default: true
    "channel": "stable",    // "stable" | "beta"
    "checkIntervalHours": 24
  }
}
```

## 7. ETW（M33）

### 7.1 Provider GUID

```cpp
// {ZZZZZZZZ-ZZZZ-ZZZZ-ZZZZ-ZZZZZZZZZZZZ}
constexpr GUID kAzooKeyEtwProvider = { ... };
```

GUID 実値は M33 着手時に `uuidgen` で確定。

### 7.2 Event ID 表

| Event ID | 名称 | フィールド |
|---|---|---|
| 1000 | Activate | client_id, profile_guid |
| 1001 | Deactivate | client_id |
| 2000 | CompositionStart | length |
| 2001 | CompositionEnd | length, committed |
| 3000 | IpcRequest | request_id, message_type, payload_size |
| 3001 | IpcResponse | request_id, latency_ms |
| 3002 | IpcCancel | target_request_id |
| 4000 | InferenceStart | request_id, backend, kana_len |
| 4001 | InferenceEnd | request_id, n_candidates, latency_ms |
| 5000 | LearningObserve | reading, surface |
| 5001 | LearningForget | reading, surface |
| 9000 | Error | source, message, hr |

### 7.3 ラッパ

`core/src/EtwLogger.cpp`（新規）：

```cpp
class EtwLogger {
public:
    static void Register();
    static void Unregister();
    static void LogActivate(...);
    static void LogIpcRequest(...);
    static void LogError(const char* source, const char* msg, HRESULT hr);
};
```

`EventRegister`/`EventWriteString` の薄いラッパ。
全モジュール（TIP/Host/Settings）から呼べるよう `core/` 配下に置く。

### 7.4 観測

`wpr -start GeneralProfile -filemode` + `wpa.exe` で開発者がトレース可能。
本番ユーザーは ETW を意識せず、問題報告時に WPR トレースを取ってもらう。

## 8. WER（Windows Error Reporting、M33）

### 8.1 MiniDumpWriteDump

```cpp
LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS* ep) {
    wchar_t path[MAX_PATH];
    GetTempPathW(MAX_PATH, path);
    wcscat_s(path, L"azookey-crash.dmp");

    HANDLE hFile = CreateFileW(path, GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{
            GetCurrentThreadId(), ep, FALSE
        };
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
                          hFile,
                          MiniDumpWithDataSegs,
                          &mei, nullptr, nullptr);
        CloseHandle(hFile);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

// main で
SetUnhandledExceptionFilter(UnhandledFilter);
```

Host / Settings は明示的に `SetUnhandledExceptionFilter` で設定。
TIP DLL は in-proc なのでアプリ側を巻き込まないよう **設定しない**。
代わりに `__try`/`__except` で IPC ループ等のスコープを囲む。

### 8.2 クラッシュレポート送信

Phase 7 では `%LOCALAPPDATA%\azooKey\crashes\` に保存のみ。
送信先 UI は将来課題（GitHub Issue 自動起票はプライバシ懸念で見送り）。

設定アプリ「詳細 → クラッシュレポート」から手動でアーカイブして添付。

## 9. DPAPI 学習データ暗号化（M34）

### 9.1 目的

`learning.tsv` に含まれる確定履歴（個人情報を含み得る）をユーザースコープで
暗号化。OS ユーザーが変わると復号できない。

### 9.2 実装

`learning/src/DpapiCrypto.cpp`（新規）：

```cpp
#include <dpapi.h>

bool EncryptToFile(const std::vector<uint8_t>& plain,
                   const std::wstring& path) {
    DATA_BLOB in{ static_cast<DWORD>(plain.size()),
                  const_cast<BYTE*>(plain.data()) };
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"azooKey-learning",
                          nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        return false;
    }
    bool ok = WriteAllBytes(path, out.pbData, out.cbData);
    LocalFree(out.pbData);
    return ok;
}

bool DecryptFromFile(const std::wstring& path,
                     std::vector<uint8_t>& plain) {
    std::vector<uint8_t> cipher;
    if (!ReadAllBytes(path, cipher)) return false;
    DATA_BLOB in{ static_cast<DWORD>(cipher.size()), cipher.data() };
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        return false;
    }
    plain.assign(out.pbData, out.pbData + out.cbData);
    LocalFree(out.pbData);
    return true;
}
```

### 9.3 LearningStore との統合

`LearningStore::Load`/`Save` でラップ：

```cpp
bool LearningStore::Save() {
    std::vector<uint8_t> tsv = SerializeToTsv();
    return EncryptToFile(tsv, path_);
}

bool LearningStore::Load() {
    std::vector<uint8_t> tsv;
    if (!DecryptFromFile(path_, tsv)) return false;
    return DeserializeFromTsv(tsv);
}
```

### 9.4 移行

既存の平文 TSV から暗号化形式への 1 回限り移行：

- 起動時、`learning.tsv`（平文）と `learning.tsv.enc`（暗号化）の両方を check
- 平文があれば読み込んで暗号化形式に書き直し、平文を削除（バックアップは
  `learning.tsv.bak` に残す）

### 9.5 ユーザー辞書

`user_dict.json` も同様に暗号化（M34 範囲）。

設定 JSON（`settings.json`）は **暗号化しない**（API キー以外は機密性低い）。
ただし `openAiApiKey` は **個別に DPAPI 暗号化**：

```json
{
  "openAiApiKey": "dpapi:base64..."   // "dpapi:" prefix で暗号化済みを示す
}
```

設定アプリで API キーを入力した時点で暗号化、表示時に復号して伏字。

## 10. テスト

| テスト | 場所 | 内容 |
|---|---|---|
| MSIX 登録 | `tsf-tip/tests/msix_smoke_test.cpp` | Windows 限定。MSIX 内に CLSID が登録されているか |
| 署名検証 | `pkg/tests/signature_test.ps1` | signtool /verify で成功するか |
| UpdateChecker | `inference-host/tests/update_checker_test.cpp` | GitHub API モック、バージョン比較 |
| ETW provider | `core/tests/etw_logger_test.cpp` | Windows 限定。Register/Unregister + Write |
| DPAPI | `learning/tests/dpapi_crypto_test.cpp` | Windows 限定。Encrypt → Decrypt round trip、他ユーザでは失敗 |
| WinGet manifest | CI で `winget validate` | YAML が valid |

## 11. リリース手順チェックリスト

1. `CHANGELOG.md` を更新
2. バージョンタグを打つ (`git tag v1.0.0`)
3. `git push --tags`
4. `.github/workflows/release.yml` が自動実行
5. Draft Release が作成される（MSIX 添付済み、署名済み）
6. 動作確認（クリーン VM でインストール → 入力 → 確定 → アンインストール）
7. Draft → Publish
8. winget-pkgs に PR（`wingetcreate update`）

## 12. 参照

- MSIX SDK: <https://learn.microsoft.com/windows/msix/desktop/source-code-overview>
- signtool: <https://learn.microsoft.com/windows/win32/seccrypto/signtool>
- DPAPI: <https://learn.microsoft.com/windows/win32/api/dpapi/>
- ETW: <https://learn.microsoft.com/windows/win32/etw/>
- WiX v4: <https://wixtoolset.org/docs/intro/>
- WinGet manifest: <https://github.com/microsoft/winget-cli/blob/master/doc/ManifestSpecv1.5.md>
- 既存：`scripts/register.ps1` / `scripts/unregister.ps1`
- ベース：`docs/windows-tsf-host-architecture.md`
