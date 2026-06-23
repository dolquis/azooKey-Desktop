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

> **署名 cert を入れたら `Publisher` を必ず差し替える**: 上記 `Publisher="CN=dolquis"`
> は自己署名 dev cert 用の暫定値である。OV/EV cert で署名する場合、`Identity@Publisher`
> は署名証明書の Subject DN 全体と完全一致しないと `Add-AppxPackage` が `0x8007000B`
> で失敗する。具体手順は §2.2「Publisher と証明書 Subject の整合（必須）」を参照。

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

TIP 側のレジストリ登録（`DllRegisterServer` 内の machine-wide HKLM
`Software\Classes\CLSID\...` と TSF プロファイル `...\CTF\TIP\...`）は MSIX 経路では
使われない。MSIX manifest が registry を上書きする形になるため、TIP 側の自己登録
ロジックは MSIX 同梱時に動かないことを前提に書く。
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

`scripts/register-dev.ps1` / `unregister-dev.ps1` は `regsvr32` 経由で machine-wide
(HKLM) 登録を行う管理者向け開発用スクリプトであり、MSIX 経路と衝突する。両者を
取り違えると、片方の登録解除が漏れて言語バーに古いエントリが残る (M28 設計メモ)。
本書では以下を運用ルールとして固定:

* `scripts/register-dev.ps1` / `unregister-dev.ps1`（接尾辞 `-dev` を必須化。
  DEV-101 でリネーム済み）。`regsvr32` 開発用経路であることを名前で明示する
* MSIX 同梱の TIP は HKCU 自己登録ロジックを skip（上記 `IsRunningInMsixContext`
  で分岐）
* CI / ローカル開発で MSIX と `regsvr32` を併用する場合は、`compat-test/msix_install_uninstall.ps1`
  の smoke ハーネスで Add-AppxPackage → 登録確認 → Remove-AppxPackage → 残骸 0 を
  確認する（実機 VM で実行する `gate:human-required`。COM 登録ラウンドトリップ自体は
  CTest `tsf_tip_com_smoke_tests::TsfTipRegistrationSmokeTest` が担い、本ハーネスは
  MSIX パッケージング層を補完する）

### 1.2 Package.wapproj

`pkg/msix/Package.wapproj`（新規、WAP プロジェクト）：

- ターゲット: x64 / arm64
- 同梱物:
  - `azookey_tsf_tip.dll`
  - `azookey_inference_host.exe`
  - `azookey_settings.exe`（M11 で最小版を同梱 / M30 でフル UI 化。§3.0）
  - `Assets/*.png`
  - `models/`（gguf）は **MSIX に含めない**（サイズ過大）→ 初回起動時に DL
    （v1.0 の最小取得経路と配信元の分岐は §1.6.1）

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

#### Windows App SDK ランタイム（設定アプリ = WinUI 3 の依存）

設定アプリ `azookey_settings.exe` は WinUI 3（§3.0）のため **Windows App SDK ランタイム**に
依存する。VCLibs だけではクリーン VM で起動しないため、次のいずれかで同梱する（ビルド側は
`Microsoft.WindowsAppSDK` NuGet の `PackageReference` が必須。版は §3.1 の採用 WASDK に揃える）:

- **self-contained 配置（推奨・サイドロード向け）**:
  `<WindowsAppSDKSelfContained>true</WindowsAppSDKSelfContained>` を **設定アプリ本体の
  プロジェクト（`azookey_settings`）に設定し、さらに WAP パッケージングプロジェクト
  （`pkg/msix/Package.wapproj`）にも同プロパティを設定する**（Microsoft の self-contained 配置
  ガイド: アプリプロジェクトに必須、WAP 利用時はパッケージングプロジェクトにも追加。WAP のみだと
  ランタイムファイルが設定アプリ出力に展開されず起動失敗し得る）。これによりランタイムを
  アプリへバンドルし、フレームワークパッケージ依存が無くなり、外部配信に依存せずクリーン VM で
  確実に起動する（配布サイズは増える）。
- **framework-dependent 配置**: manifest に Windows App SDK のフレームワーク依存を宣言し、
  `Microsoft.WindowsAppRuntime` フレームワークパッケージ（または `WindowsAppRuntimeInstall.exe`）
  の配信をインストーラ手順（§4 / §5）に含める。
  ```xml
  <!-- framework package family は major 版に揃う（WASDK 2.0 SemVer 移行）。NuGet/版が
       2.2.0 でも family は Microsoft.WindowsAppRuntime.2.0 のまま（".2.2" は存在せず、
       clean VM で解決失敗する）。MinVersion は採用する WASDK 版（§3.1）に対応する
       ランタイムパッケージ版に合わせる。 -->
  <PackageDependency Name="Microsoft.WindowsAppRuntime.2.0"
                     MinVersion="..."
                     Publisher="CN=Microsoft Corporation, O=Microsoft Corporation, L=Redmond, S=Washington, C=US" />
  ```

TIP DLL（`tsf-tip/`）と Host（`inference-host/`）は WinUI 3 非依存のため、本ランタイムは
**設定アプリのみ**が必要（TIP/Host パッケージには不要）。

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
- アンインストールで `HKLM\Software\Classes\CLSID\...`（A: `regsvr32` 経路、machine-wide）/
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

### 1.6 推論バックエンド / EP / モデルの配布方針（M24 連動）

推論バックエンドの選定は `docs/copilot-pc-backend-spec.md` §4 が正典。配布形態の
決定のみを以下に固定する（同 §4.5 から参照）。

| 構成要素 | 配布形態 | 理由 |
|---|---|---|
| llama.cpp（R1）CPU ランタイム | **base MSIX に同梱** | v1.0 既定エンジン。バイナリは小さい |
| zenz-v3 GGUF モデル本体 | **MSIX 非同梱**（初回起動時 DL。配信元は §1.6.1） | サイズ過大。§1.2 の既存方針に従う。v1.0 最小取得経路の確定は §1.6.1（DEV-202 のライセンス結論で配信元が分岐） |
| Windows ML bootstrap（R2 用 ORT GenAI WinML） | **base MSIX に同梱（薄い）** | EP 本体は含めない |
| Windows ML EP（QNN / OpenVINO / VitisAI / NvTensorRtRtx 等） | **非バンドル（Windows Update 配信）** | Microsoft 推奨。MSIX 肥大回避・自動更新 |
| ggml-cuda（R1 CUDA, NVIDIA） | **optional add-on / 別パッケージ**（base に含めない） | CUDA ランタイムが大きく NVIDIA 環境限定 |
| zenz-v3 ONNX 変換モデル（R2, 変換スパイク成功時） | **optional モデルパッケージ**（同 §1.2 同様に非同梱・DL） | 変換可否が未確定・対象環境限定 |

NPU / HW EP は Win11 24H2 (build 26100)+ を要するため、未満環境は R1 CPU に
フォールバックする（同 §4.3-5）。モデル本体（GGUF / ONNX）はいずれも MSIX に同梱せず
初回起動時 DL とする方針で §1.2 と一貫させる。

#### 1.6.1 v1.0 における Zenzai GGUF の最小取得経路（M8 / M28）

§1.6 はモデルを「MSIX 非同梱・初回取得」と定める。本節はその **v1.0 最小実装**を
確定する。対象は **zenz-v3 系 GGUF 1 種**（既定配置
`%LOCALAPPDATA%\azooKey\models\zenzai\<file>.gguf`）。**v1.0 が対象とする
モデル（repo / ファイル / version / 量子化 / SHA256）は Windows 側が所有する
ピン定義 `models\zenzai\expected.json`（または同等のビルド定数）を唯一の正とし、
(b) の配信元・(c) の手動配置先・「期待版のピン」の SHA256 はすべてこの同一
アーティファクトを指す**。

> **legacy submodule を Windows の正にしない（AGENTS.md 準拠）**: 上流の出所は
> HuggingFace `Miwa-Keita/zenz-v3.2-small-gguf`（legacy macOS ビルドが
> `.gitmodules` / `legacy/azooKeyMac/Resources/gguf` で参照するのと同じモデル）
> だが、`legacy/` は保全された参照資産であり Windows 版の source of truth では
> ない。legacy submodule の更新・削除で Windows v1.0 のアーティファクトや
> 期待 SHA が暗黙に変わらないよう、**Windows のピンは `.gitmodules` を参照せず
> `expected.json` に独立に固定する**（`.gitmodules` は出所の参考としてのみ引く）。
> §3.4 のモデルパス例や roadmap M8 受け入れの版表記も、本ピン（上流
> `Miwa-Keita/zenz-v3.2-small-gguf` = v3.2）を正として揃える。版番号は各 doc に
> 直書きせず本ピンを参照し、版の更新は版の決定権を持つ DEV-219〔M8 統合〕が
> `expected.json` で行う。

M45 のフル管理 UI（`docs/model-management-spec.md`）が乗る土台を、二重実装を
避けて先に敷くことを目的とする。

##### 取得方式の選定

| 方式 | v1.0 採否 | 理由 |
|---|---|---|
| (a) MSIX 同梱 | ✗ 不採用 | base MSIX が GGUF 分（数百 MB〜）肥大する（§1.2）。加えて再配布可否が DEV-202 未確定で、同梱は最もライセンスリスクが高い |
| (b) 初回起動時オンデマンド DL | ✅ **v1.0 目標既定（M32 後）** | M32 で切り出す共有 `HttpDownloader` + SHA256 検証基盤（§6.3 / M36-B が再利用 / `docs/auto-word-registration-spec.md` §5）を再利用。サイズ問題を回避し、配信元を DEV-202 結論で差し替えられる。M32 前（M28 時点）は (c) が operative |
| (c) 手動配置 | ✅ **常時併存（M28 の operative default 兼 恒久フォールバック）** | オフライン / 企業環境 / DL 失敗時の確実な経路。Phase 3 検証の既存前提（M8 受け入れ条件「未配置時も Host が落ちない」）をそのまま恒久サポートする |

**確定**: v1.0 の取得方式は **(b) 初回起動時 DL を目標既定、(c) 手動配置を常時
フォールバック**とし、(a) は採らない。(b) と (c) は同一の配置レイアウト（後述）に
収束するため、Host / M45 から見た「モデルがそこに在る」状態は取得方式に依存しない。

**マイルストーン順序の制約（M28 を M32 の DL 基盤に依存させない）**: (b) は共有
ヘルパ `HttpDownloader`（M32 で切り出し、M36-B が再利用）に依存する。roadmap は
**M28（§1 全体を実装）→ M29 → M32** の順で、M28 時点では `HttpDownloader` が未だ
存在しない。したがって **M28 出荷時の operative default は (c) 手動配置**とし、(b)
は **M32 の共有 `HttpDownloader` が揃った時点で既定化する fast-follow** として扱う
（M28 で one-off の重複ダウンローダを書かない＝二重実装回避）。この (b) の実装は
**roadmap M32 のスコープに計上**し（`plans/windows-port-roadmap.md` M32 / M28 実装
範囲の注記）、宙に浮かせない。(b) を v1.0 ローンチ
までに既定化したい場合は、ダウンローダ基盤の切り出しを M28 の前提として前倒しする
（roadmap 側で M32 の該当スコープを M28 前へ移す）必要があり、これは roadmap 更新を
伴う別判断とする。

##### ライセンス分岐（DEV-202 連動。確定までは「配信元 保留」で設計）

再配布可否は DEV-202（`gate:human-required`、未確定）の結論に従う。ここで重要な
のは、**取得*機構*（HttpDownloader + SHA256 検証 + 原子的配置）は結論に依存せず
同一**で、分岐するのは **配信元 URL と同梱可否だけ**である点。プロジェクトの
GitHub Release への再ホスト自体が再配布に当たるため、DEV-202 は (a) だけでなく
(b) の配信元選択も律する。

| DEV-202 結論 | (b) の配信元 | (a) 同梱 |
|---|---|---|
| 再配布可 | プロジェクトの GitHub Release に再ホストして DL | サイズ理由で引き続き不採用 |
| 再配布不可 / 条件付き | 再ホストせず上流 HuggingFace の**`expected.json` が定める repo / ファイル**（出所は `Miwa-Keita/zenz-v3.2-small-gguf`）から DL。取得物は「期待版のピン」（下記）の SHA256 と一致する＝同一アーティファクトなので 404 や版ズレを起こさない。帰属・条項は `ThirdPartyNotices.txt` と UI に明示 | 不可 |
| 未確定（現状） | 配信元 URL を設定 / ビルド定数の間接参照にしておき、(c) 手動配置を確実な既定経路として案内する | 保留 |

CUDA ランタイムの同梱可否（DEV-202 で併せて確認）は本経路と独立した判断である
（モデルではなくランタイム。§1.6 の optional add-on 行で扱う）。

##### 配置パスとバージョニング

- **配置先**: `%LOCALAPPDATA%\azooKey\models\zenzai\<file>.gguf`（§3.4、および
  `model-management-spec.md` §3.1 の 1 階層スキャンと整合）。
- **取得の原子性**: DL は `<file>.gguf.part` へ書き、SHA256 検証通過後に最終名へ
  rename する（`learning/src/AtomicFile.h` と同じ temp→rename 規律）。検証前の
  ファイルをロード対象に入れない。
- **初回取得後の `selectedPath` コミット（必須）**: Host は起動時に既定
  ディレクトリを自動スキャン**しない** —
  `inference-host/src/SettingsStore.cpp::ApplyRuntimeSettingsToEngineConfig` は
  `model.selectedPath` を `config.model_path` へコピーするのみで、空 path は
  「モデル未選択 → SimpleConverter」になる。よって**ファイルを配置しただけでは
  再起動後も degraded のまま**になり得る。これを防ぐため、取得方式ごとに次で
  `selectedPath` を確定させる:
    - **(b) 初回 DL**: headless 取得のため、(c1) と同じ前提ガードを
      **ネットワーク I/O・probe-load の前に**評価し、満たす場合のみ起動する —
      **`model.enabled` かつ `model.autoLoadOnHostStart=true`
      （`model-management-spec.md` §7）かつ `privacy.mode` がネットワーク禁止
      （`offline`。M46 / `privacy-and-secure-input-spec.md`）でない**こと。
      いずれか不成立なら DL せず手動配置に委ねる（offline でも (c1) ローカル
      autoselect / (c2) 明示選択は network なしで成立）。ガードを満たす場合、
      ダウンローダが rename 後の確定パスをプローブロードし、成功して初めて
      `model.selectedPath` へコミットする（更新時と同じ probe→commit 規律）。
    - **(c) 手動配置**: 2 経路を持つ。
        - **(c1) Host 起動時の default-path autoselect（ピン依存）**:
          **`model.enabled` かつ `model.autoLoadOnHostStart=true`
          （`model-management-spec.md` §7）かつ `selectedPath` が空 / 未設定**の
          場合に限り、`models\zenzai\` の中から **SHA256 が `expected.json` の
          ピンと一致するファイル**を選び（単に format-valid な GGUF では不可。
          **ピン一致を*前提条件*とし**、licensing / 版ゲートを迂回しない。
          `model-management-spec.md` §3.1/§3.3 の形式検証も併せて満たすこと）、
          プローブロード成功時に `selectedPath` へコミットする。**ピン一致
          ファイルが無い / ピン未投入の場合は autoselect しない**（任意の有効
          GGUF を勝手に選ばない）＝ (c1) 適用外で、(c2) か劣化モード（下記）に
          委ねる。次も autoselect の対象外: (i) `autoLoadOnHostStart=false`
          （起動時ロードを抑止する設定を尊重し、何もしない）、(ii) `selectedPath`
          が**非空だが不在**（`dev-infrastructure-spec.md` D-007 が *error* 扱い
          する「設定済みパス不在」。silently 切り替えず error / M45 モデル選択
          誘導に委ねる）、(iii) `selectedPath` が非空かつ実在（下記 (c2) の明示
          選択。autoselect は上書きしない）。
        - **(c2) ユーザーによる明示選択（ピン非依存）**: ユーザー / 管理者が
          `model.selectedPath` を直接設定する経路（`settings.json` 手編集、または
          設定 UI / M45 の「モデルを追加」「選択モデルをロード」）。設定された
          実在パスを probe-load して使う。**`expected.json` ピンが未投入でも成立
          する唯一の手動経路**であり、autoselect が無効な状況（ピン未投入等）の
          受け皿になる。ファイルを `models\zenzai\` に置くだけでは不十分で、必ず
          `selectedPath` を明示設定する（空のままでは degraded）。
- **期待版のピン**: 対象の repo / ファイル名 / version / 量子化 / SHA256 は
  Windows 側が所有する `models\zenzai\expected.json`（または同等のビルド定数）に
  独立に固定し、DL / 検証の照合に使う（legacy `.gitmodules` は参照しない。上記
  注記参照）。(b) の配信元と (c) の手動配置先は、いずれもこのピンと同一
  アーティファクトを指す。SHA256 の値域は M45 の `ModelCatalogEntry.sha256`
  （`model-management-spec.md` §3.2）と同一。本書はこのピンの**契約（schema）を
  定義**し、**具体値（正確なファイル名と SHA256）は M8/M28 実装時に版の決定権を
  持つ DEV-219 が投入する**（docs は値ファイルを未コミット。値の確定 = DEV-219）:

  ```jsonc
  // models\zenzai\expected.json（Windows 所有のピン。値は M8/M28 で投入）
  {
    "repo": "Miwa-Keita/zenz-v3.2-small-gguf",   // 出所（上流）。再ホスト時も artifact は同一
    "file": "<exact-file-name>.gguf",             // 例: ggml-model-Q5_K_M.gguf（DEV-219 で確定）
    "version": "v3.2-small",
    "quantization": "Q5_K_M",
    "sha256": "<64-hex>",                          // 検証の基準。DEV-219 で実値投入
    "size_bytes": 0                                // 任意（部分 DL 早期検出用）
  }
  ```

  **ピン未投入時の挙動（明示）**: `expected.json`（または定数）が未投入の間は、
  照合すべき正確な artifact / SHA256 が無いため **(b) DL は発火させない**（誤った
  v3.1/v3.2 を引いたり検証不能になるのを防ぐ）。この期間の operative path は
  (c) 手動配置のみで、未配置なら M8/M47 の劣化モード（下記）に従う。これにより
  「ピンが正、未投入なら DL せず degraded」が決定的になる。
- **更新時の置換（無停止更新）**: 新版を別ファイル名で DL → SHA256 検証 →
  **新版をプローブロード（実際にロード成功を確認）** → 成功して初めて
  `model.selectedPath`（`model-management-spec.md` §7）を新版へコミット →
  旧版を削除する。SHA256 一致でもロード非互換だったり途中でクラッシュした場合に
  備え、`selectedPath` のコミットはロード成功後に限定する（`autoLoadOnHostStart`
  が次回起動で参照するのはコミット済みの値のみ）。いずれかの段で失敗したら
  `selectedPath` は旧版のまま据え置き、新版ファイルは破棄して切替しない。これに
  より「失敗時は旧版を残す」を原子的に保証する。
- **未配置 / 破損時の劣化モード**: ロード境界は M8 のとおり Host を落とさず
  `SimpleConverter` にフォールバックし、M47 の状態機械（`DegradedModel` /
  `SafeMode`）と UI 通知に従う（`⚠️ … 簡易変換で継続`）。破損 / 部分 DL
  （`.part` 残骸）は破棄し、直前の有効モデルか SimpleConverter を維持する。

##### M45（フル管理 UI）への橋渡し

`docs/model-management-spec.md` §2 はモデル DL を M45 の非目標（将来 M へ分離）と
する。本 v1.0 最小取得経路がその「将来 M」の最小サブセットであり、次の責務分担で
二重実装を避ける:

- **v1.0（本節）が敷く土台**: 配置レイアウト（`models\zenzai\`）・取得機構
  （HttpDownloader + SHA256 + 原子置換）・期待版ピン。対象は既知の zenz-v3 1 種に
  限定し、汎用ダウンロードマネージャ UI は持たない。
- **M45 が上に足す**: 検出 / 検証（`ListModels`、GGUF magic）・backend 選択・
  ベンチマーク・管理 UI（`model-management-spec.md` §6）。M45 はこの配置レイアウト
  と SHA256 値域をそのまま再利用し、独自の取得 / 配置スキームを作らない。
- **将来のモデル DL UI（M45 後続 M）**: 本節の取得機構をそのまま UI 化し、上表の
  配信元分岐を引き継ぐ。

### 1.7 AppContainer DLL ACL と常駐起動（参考: 先行 Windows 実装）

**AppContainer DLL ACL**: UWP / Microsoft Store / AppContainer 実行のアプリで TIP を
有効化するには、TIP DLL に `ALL APPLICATION PACKAGES`（SID `S-1-15-2-1`）への RX 付与が
要る。Option A（external-location / `regsvr32` 機械全体登録）や開発用
`scripts/register-dev.ps1` 経路ではこれが既定で付かないため、
`icacls <dll> /grant "*S-1-15-2-1:(RX)"` 相当を登録ステップに加える（x64 / x86 両 DLL、
ビルド時・登録時）。先行実装 fkunn1326/azooKey-Windows はビルド時+インストール時に
二重付与している。Option B/C（通常 MSIX）はパッケージ側で解決されるため不要。
設計・実機検証は DEV-204、DEV-101（com4:ComServer ACL 制限）と連動。

**常駐起動（参考）**: Host / launcher のログオン常駐を Run キーでなく **Task Scheduler
（LogonTrigger + RunLevel=HighestAvailable）+ VBS 非表示起動**で実現し、アンインストール
時に `schtasks /Delete` する方式がある（UAC プロンプト無しの常駐）。
`RunLevel=HighestAvailable` は UAC 構成依存である点に注意。MSIX 配布では startup task /
app execution alias の利用を優先する。

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
的。組織化済みで該当地域なら A を強く推奨。ルートの最終選定・証明書調達・申請手順は
人間判断が必須のため、`gate:human-required` 課題
（[Linear DEV-255](https://linear.app/dolquis/issue/DEV-255)）で確定する。

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

`.github/workflows/release.yml` を**雛形として作成済み**（DEV-100）。本ファイルが
実装の正典であり、本節の YAML はその設計の写しである。雛形は以下の前提で**既定無効**:

* job 全体を repository variable `RELEASE_ENABLED == 'true'` でガードし、誤発火を防ぐ。
* MSIX ビルド（`pkg/msix/Package.wapproj`）は M28 PoC（DEV-101）確定後に作成する。
* 署名ステップは経路 A/B/C（§2.0）の 3 ルートをコメントで併記し、採用ルートは
  人間ゲート課題 D-04-A（`gate:human-required`）で確定してから 1 つだけ有効化する。
* 各ルートの Secrets スキーマは §2.2 / §2.2.A / §2.2.B の表を正典とする。

有効化手順（M28/M29 完了・証明書手当て済み後に人間が実施）は同ファイル冒頭コメントに記す。

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

## 3. 設定アプリ（M11 最小 / M30 フル）

### 3.0 UI フレームワーク選定（採用: WinUI 3 / C++/WinRT）

設定アプリの UI フレームワークは、**WinUI 3（Windows App SDK, C++/WinRT）を第一候補と
して確定する**。対象は **v1.0 スコープの M11（最小設定アプリ。Phase 4 リリースゲート）** と、
それを **post-v1.0 で本格化する M30（フル設定 UI）**、および同一 UI スタックに乗る後続機能
（モデル管理 UI = M45、学習データ可視化 UI = M49）。同一フレームワークに揃えて後続の
作り直しを避ける（v1.0 では M11 の最小機能セットのみを出荷し、M30 のフル UI / 横断機能は
v1.0 に引き込まない）。根拠は次の 3 点:

1. **C++/WinRT 親和性** — TIP（`tsf-tip/`）・Inference Host（`inference-host/`）が
   すでに C++ / C++/WinRT スタックであり、IPC クライアント（Named Pipe + 4-byte
   length-prefix + JSON、§3.3）の型・実装・ビルド系を設定アプリと共有できる。別言語
   ランタイム（.NET / Rust）を追加で持ち込まずに済む。
2. **Fluent Design 標準対応** — Mica / Acrylic backdrop（`Window.SystemBackdrop` に
   `MicaBackdrop` / `DesktopAcrylicBackdrop`）・Light/Dark・PerMonitorV2 DPI・
   アクセントカラーが追加実装なしで得られ、M26（Win11 ネイティブ体験）と整合する。
3. **MSIX サイドロード整合** — v1.0 の配布形態（§1 MSIX サイドロード）と一致し、
   設定アプリ EXE をパッケージ内同梱として配布できる（§1.1 の `comServer` は TIP の
   in-proc サーバ宣言であり、設定アプリは通常の packaged EXE として同梱する）。

> **事実更新（Microsoft Learn, 2026-06 時点。旧記述の訂正）**
> - WinUI 3 がサポートする言語は **C# と C++/WinRT のみ**（C++/CX は非推奨）。
> - WinUI 3 / Windows App SDK の対応 OS は **Windows 10 バージョン 1809（build 17763）以降**。
>   ただしこれは**設定アプリ（WinUI 3）単体の下限**であり、配布パッケージ全体の最小 OS では
>   ない（下記）。
> - **unpackaged（MSIX なし）配布もサポートされる**（Windows App SDK 1.0 以降）。旧記述
>   「WinUI 3 デスクトップは MSIX 必須・unpackaged 不可」は**誤り**。ただし本プロジェクトは
>   TIP の CLSID/Profile 登録と MSIX サイドロード配布の都合で **packaged（MSIX）を採用**する。
> - **配布パッケージ全体の最小 OS は TIP の配布経路で決まり、設定アプリの 1809 より高い**:
>   §1 の経路 A（external-location packaging）は **Win10 2004 / build 19041 以上**、経路 B
>   （通常 MSIX + `com4:InProcessServer`）は **build 20348 以上**（`MinVersion="10.0.20348.0"`、
>   §1.1）。設定アプリの WinUI 3（1809+）はこのパッケージ下限に内包されるため、MSIX の
>   `TargetDeviceFamily` には 1809 ではなく §1 の経路別下限を設定する。
> - 現行安定版は **Windows App SDK 2.2.0（2026-06-09）**（バージョン系列は 2.x）。版は
>   固定せず実装時の最新安定版に追従する（§3.1）。

#### フレームワーク比較

| 軸 | **WinUI 3 (C++/WinRT) ★採用** | WPF (.NET 9+) | Tauri (Rust + WebView2) |
|---|---|---|---|
| 言語 | C++/WinRT（既存スタックと同一） | C#（.NET ランタイム追加） | Rust + JS/TS |
| Fluent / Mica | 標準（`SystemBackdrop`） | .NET 9 で Fluent テーマ標準化（Mica は DWM API 併用） | WebView2 任せ（CSS 自前実装） |
| DPI / Dark | 標準（PerMonitorV2） | 対応（Fluent テーマ・`ThemeMode`） | WebView2 / 自前 |
| 配布 | packaged / unpackaged 両対応 | unpackaged 可・MSIX 可 | 最小バイナリ・WebView2 ランタイム前提 |
| C++ IPC 連携 | 同一言語で interop 不要 | C#↔C++ interop 層が必要 | Rust↔C++ FFI（または既存 Named Pipe を再利用） |
| MS サポート | first-party | first-party | 非サポート（OSS） |
| MSIX 整合 | ◎ | ○ | △（WebView2 依存） |

**代替案メモ**:
- **WPF (.NET 9+)** — .NET 9 で Fluent テーマ（Light/Dark/アクセント、`ThemeMode`、
  `PresentationFramework.Fluent`）が標準化され UI 体験差は縮小した（旧記述「Fluent /
  Mica は非標準」は .NET 9 以降では不正確）。ただし設定アプリに .NET ランタイム依存が
  増え、C++ IPC クライアントとの interop 層が必要になる。**v1.0 では非採用**。
- **Tauri** — 配布サイズ最小だが Microsoft 非サポートで、C++/WinRT との境界が増える
  （既存 Named Pipe IPC を再利用すれば緩和可能）。**v1.x の差別化リッチ UI（M30 拡張）で
  WebView ベースへ切り替える余地を残す**が、v1.0 では非採用。

**仮決定の確証（v1.0 着手前、`gate:human-required`）**: Windows 実機で WinUI 3 / WPF の
最小サンプル（チェックボックス + テキスト入力 + リストボックス + IPC 往復 1 経路）を作り、
(a) 同梱配布サイズ増分、(b) 初回起動時間、(c) C++/WinRT IPC 連携コード行数 の 3 指標を
計測してこの選定を確証する（実測値は Linear 課題に記録し、本 spec には決定と設計のみ残す）。

### 3.1 構成

- 言語: C++/WinRT（TIP / Host と統一）
- フレームワーク: WinUI 3（Windows App SDK 2.x。2026-06 時点の最新安定版は 2.2.0。版は固定せず実装時の最新安定版に追従）
- 配布: MSIX 内同梱（別 EXE `azookey_settings.exe`）

### 3.2 ナビゲーション

| ペイン | 内容 |
|---|---|
| 一般 | 起動時に IME を有効化 / 自動更新 ON/OFF |
| 入力 | inputStyle / customRomajiTablePath / liveConversion / predictionEnabled |
| AI | aiBackend / openAiApiKey / openAiApiEndpoint / promptPrefixByApp |
| 学習 | LearningStore 表示 / エクスポート / リセット / Persona 表示 |
| 詳細 | backendPreference / epPreference / powerProfile / logLevel |
| 校正 | （M30 後半）バッチ訂正ビュー |
| バージョン | バージョン情報 / 更新確認 / ライセンス |

> 本表はペイン割り当ての概観である。設定キーの正典一覧（全 top-level キー・型・既定・永続化・
> 反映方法・拡張方針）は §3.6 を参照する。

> **参考（fkunn1326/azooKey-Windows, MIT）**: 「詳細」ペインの `backendPreference` 選択は、
> 各バックエンドのランタイム DLL 存在判定（`cudart64_12.dll`+`cublas64_12.dll` / `vulkan-1.dll`）
> で**未対応バックエンドを disable + 理由 Tooltip 表示**する能力検出 UX を採れる（先行実装の
> `check_capability` 相当）。一次フィルタ扱いとし、実 EP 選択は `docs/copilot-pc-backend-spec.md`
> §3-§4 に委ねる。`backendPreference` は自分側 schema が `cpu/cuda/vulkan/winml/directml/npu`
> を持ち、参考側（cpu/cuda/vulkan）より広い。詳細は DEV-120 のコメント参照。

### 3.3 Host との IPC

設定アプリは `%LOCALAPPDATA%\azooKey\config\settings.json` を更新した後、Host に
payload 空の `UpdateConfig` メッセージを送信して再読込を促す。設定オブジェクトは
IPC schema に二重定義しない。

```
UpdateConfigRequest:
  request_id

UpdateConfigResponse:
  request_id
  ok: bool
  error: optional<string>
```

Host は `settings.json` を再読込し、即時反映可能なものを適用する。破損などで
再読込結果が invalid の場合、`ok=false` と `error` を返し、現在の runtime 設定は
維持する。再起動が必要な設定は M30 の UI 本格化時に settings app 側の表示で扱う。

### 3.4 設定ファイルパス

| 用途 | パス |
|---|---|
| 設定 | `%LOCALAPPDATA%\azooKey\config\settings.json` |
| カスタムローマ字 | `%LOCALAPPDATA%\azooKey\config\custom-romaji.tsv` |
| 学習データ | `%LOCALAPPDATA%\azooKey\data\learning.tsv`（DPAPI 暗号化、M34） |
| ユーザー辞書 | `%LOCALAPPDATA%\azooKey\data\user_dict.json` |
| モデル | `%LOCALAPPDATA%\azooKey\models\zenzai\<file>.gguf`（`<file>` は §1.6.1 の `expected.json` ピンが定める実ファイル名。出所は上流 `Miwa-Keita/zenz-v3.2-small-gguf`） |
| ログ | `%LOCALAPPDATA%\azooKey\logs\*.jsonl` |

### 3.5 `ITfFnConfigure` 連携（Windows 設定「詳細設定」からの起動）

Windows の「設定 > 時刻と言語 > 言語と地域 > 日本語 > キーボード」や従来の Text Services
コントロールパネルから本 IME の「プロパティ / オプション」を開くと、TSF マネージャは TIP の
CLSID を `CoCreateInstance` し `IID_ITfFnConfigure` を要求して
`ITfFnConfigure::Show(hwndParent, langid, rguidProfile)` を呼ぶ（M30 受け入れ条件
「Windows 設定からの『詳細設定』起動で開く」に対応）。

- **実装場所**: `ITfFnConfigure` は **TIP DLL（`tsf-tip/`）側**が実装する（設定アプリ EXE
  ではない）。TSF が `DllRegisterServer`（§1）で登録した CLSID に対して直接
  `CoCreateInstance` するため。語句登録の `ITfFnConfigureRegisterWord` が
  `ITfFunctionProvider::GetFunction` 経由で取得されるのと異なり、`ITfFnConfigure` は
  **CLSID 直 `CoCreateInstance`** で取得される点に注意 — すなわち
  `CoCreateInstance(kTextServiceClsid, IID_ITfFnConfigure)` であり、**TextService オブジェクト
  自身が `QueryInterface(IID_ITfFnConfigure)` で応答する**必要がある（別クラスで実装する場合も
  TextService の QI 経由で到達可能にする。さもないと `E_NOINTERFACE` で「詳細設定」が開かない。
  `docs/tsf-deep-integration-spec.md` §6.2）。
- **呼び出し元**: `Show` は **Windows の言語/IME 設定（Text Services コントロールパネル
  相当）が TIP の CLSID を `CoCreateInstance` したプロセス上**で同期的に呼ばれ、
  `hwndParent` はその設定 UI のウィンドウである（入力先のメモ帳・ブラウザ等ではない）。
- **方式（非同期起動）**: 設定 UI は WinUI 3 の独立した**長命**プロセス
  （`azookey_settings.exe`）であり、`Show` を設定アプリ終了までブロックすると呼び出し元
  （言語/IME 設定 UI）をその間フリーズさせる。よって **`Show` は設定アプリを起動
  （既存インスタンスがあれば前面化、single-instance）したうえで `S_OK` を即時返す**。
  `ITfFnConfigure::Show` の Remarks「ダイアログを閉じるまで return しない」は短命なモーダル
  プロパティ シートを想定した記述であり、別プロセスの設定アプリを採る本実装では非同期起動と
  する（設定値の反映はプロパティ シートの OK/Apply ではなく §3.3 の `UpdateConfig` IPC で
  行う）。WinUI 3 / Windows App SDK ランタイムを設定 UI ホストプロセスへ load しない利点も保つ。
- **引数受け渡し**: `langid` / `rguidProfile` は起動コマンドライン引数として
  `azookey_settings.exe` に渡し、該当言語プロファイルの設定ページを初期表示する。
- **正典実装**: 具体コード（`ConfigureFunction`・`ShellExecuteExW` による起動・カテゴリ登録
  `GUID_TFCAT_TIP_PROPERTY_UI_TEXT_SERVICE`・インストールパス解決）は
  `docs/tsf-deep-integration-spec.md` §6 を正典とする。本節は配布・プロセス境界の観点を補う。

### 3.6 設定スキーマの確定（キー一覧・永続化・拡張方針）

設定キーが各機能 spec に分散したまま実装に入ると、設定アプリと Host の整合が崩れる。
本節は schema の最終形（全 top-level キー）と永続化・拡張方針を一箇所に確定する索引で
あり、M30 設定アプリ・後続 UI（M45 モデル管理 / M49 学習データ可視化）の共通基盤とする。

**正典アーティファクトと責務境界**:

- **スキーマ正典**: `settings/mvp-settings.schema.json`（JSON Schema draft 2020-12）。
  **固定形オブジェクト**（root、および `model` / `autoUpdate` のように既知フィールド集合を持つもの）は
  `additionalProperties: false` を維持し、未知キーを拒否する。一方、**マップ形オブジェクト**は値の型を
  制約した `additionalProperties`（`false` ではない）で任意キーを許容する設計であり、本規則の対象外とする
  （例: `promptPrefixByApp` = `additionalProperties: {type: string}`、予定済みの `profilesByApp` および
  プロファイル内 `candidateTagBoosts` も同様のマップ）。
- **永続インスタンス**: `%LOCALAPPDATA%\azooKey\config\settings.json`（§3.4）。設定アプリが
  書き込み、Host が読み込んで適用する（往復は §3.3 の `UpdateConfig`）。
- **意味論の正典は各機能 spec**: 各キーの挙動・enum 値の意味・閾値は下表「正典」列の spec が
  定義する。本節はそれらを **再定義せず**、設定アプリ統合の観点（UI ペイン / 導入マイルストーン /
  反映方法）を一覧化する。v1.0 出荷時に同梱する**最小サブセット**の選定は DEV-107（M11 schema
  サブセット）が扱い、本節は schema 最終形と永続化・拡張方針を確定する。

#### キー一覧（top-level）

| キー | 型 / enum | 既定 | UI ペイン | 導入 | 反映 | 正典 |
|---|---|---|---|---|---|---|
| `inputMode` | enum `hiragana`/`alnum_half`/`alnum_full` | `hiragana` | 入力 | Phase 5（基本） | 即時 | schema |
| `inputStyle` | enum `default`/`custom` | `default` | 入力 | M17 | 即時 | roadmap M17 |
| `customRomajiTablePath` | string（パス） | `…\custom-romaji.tsv` | 入力 | M17 | 即時（ホットリロード） | roadmap M17 |
| `liveConversion` | bool | `false` | 入力 | M14 | 即時 | roadmap M14 |
| `predictionEnabled` | bool | `true` | 入力 | M15 | 即時 | roadmap M15 |
| `batchRomajiConversion` | bool | `false` | 入力 | M58-A | 即時 | `romaji-batch-conversion-spec.md` |
| `batchRomajiPreviewStyle` | enum `kana`/`romaji` | `kana` | 入力 | M58-A | 即時 | `romaji-batch-conversion-spec.md` |
| `batchConversionMode` | enum `neural`/`ai-cleanup` | `neural` | 入力 | M58-A/C | 即時 | `romaji-batch-conversion-spec.md` |
| `batchAutoPunctuation` | bool | `false` | 入力 | M58-C | 即時 | `romaji-batch-conversion-spec.md` |
| `llmMagicConversion` | bool | `false` | AI | M16 | 即時 | roadmap M16 |
| `aiBackend` | enum `none`/`openai`/`local-zenzai` | `none` | AI | M16 | 即時 | roadmap M16 |
| `openAiApiKey` | string（機微・§9 DPAPI） | `""` | AI | M16 / M34 | 即時 | roadmap M16 / §9 |
| `openAiApiEndpoint` | string（URL） | `https://api.openai.com/v1` | AI | M16 | 即時 | roadmap M16 |
| `openAiModel` | string | `gpt-4o-mini` | AI | M16 | 即時 | roadmap M16 |
| `includeContextInAITransform` | bool | `true` | AI | M16 | 即時 | roadmap M16 |
| `promptPrefixByApp` | map<string,string> | `{}` | AI | rich X-2-6 | 即時 | `rich-features-spec.md` X-2-6 / `app-profile-spec.md` §6 |
| `contextReselection` | bool（実験） | `false` | 詳細 | rich X-3-2 | 即時 | `rich-features-spec.md` X-3-2 |
| `postCommitLint` | bool（実験） | `false` | 詳細 | rich X-3-3 | 即時 | `rich-features-spec.md` X-3-3 |
| `retroactiveRecompute` | bool（実験） | `false` | 詳細 | rich X-1-3 | 即時 | `rich-features-spec.md` X-1-3 |
| `sentenceCompletion` | bool（実験） | `false` | 詳細 | rich X-2-1 | 即時 | `rich-features-spec.md` X-2-1 |
| `backendPreference` | enum `auto`/`cpu`/`cuda`/`vulkan`/`winml`/`directml`/`npu` | `auto` | 詳細 | M24 | モデル再ロード | `copilot-pc-backend-spec.md` §4 |
| `epPreference` | enum `auto`/`npu`/`gpu`/`cpu` | `auto` | 詳細 | M24 | モデル再ロード | `copilot-pc-backend-spec.md` §4.4 |
| `powerProfile` | enum `auto`/`performance`/`battery_saver` | `auto` | 詳細 | M25 | 即時 | `copilot-pc-backend-spec.md` §5–§6 |
| `logLevel` | enum `error`/`warn`/`info`/`debug` | `info` | 詳細 | Phase 5（基本） | 即時 | schema / §7 |
| `model` | object（`model-management-spec.md` §7 が下位フィールドを定義） | — | モデル | M45 | モデル再ロード | `model-management-spec.md` §5/§7 |
| `autoUpdate` | object（`enabled`/`channel`/`checkIntervalHours`） | — | 一般 | M32 | 即時 | 本書 §6 |

> オブジェクト型キー（`model` / `autoUpdate`）の下位フィールドは「正典」列の spec が確定形を
> 持つ。本表で再掲せず、ネスト構造の単一情報源を維持する。

#### 永続化形式と適用順

- 単一の `settings.json`（UTF-8）に top-level キーを格納する。書き込みは破損耐性のため
  atomic write（一時ファイル → rename）で行う。
- **レイヤリング / 優先順位**: per-app `profilesByApp`（M48）＞ `model.*`（同名 root キーを上書き、
  `model-management-spec.md` §5.2）＞ root。実効値の解決順は `app-profile-spec.md` §5 を正典とする。
- **反映方法の確定（v1.0 キー）**: Host は `settings.json` を hot-reload し、**プロセス再起動を要する
  キーを持たない**。モデルロードに影響するキー（`backendPreference` / `epPreference` / `model.*`）は
  Host 内部の推論エンジン再ロードで反映する（`model-management-spec.md` §5.3、`model.autoLoadOnHostStart`）。
  それ以外は次回変換から即時反映する。§3.3 の「再起動が必要な設定」は v1.0 では発生せず、将来キーで
  必要になった場合のみ M30 設定アプリが再起動指示を表示する。
- **機微値**: `openAiApiKey` は M34 で DPAPI 暗号化し `dpapi:` プレフィックス付きで保存する（§9）。
  平文保存は移行期のみ許容し、Host は両形式を受理する。

#### 拡張方針（`additionalProperties: false` 下でのキー追加）

- **追加は加算的**: 新規キーは既定値付きで追加し、旧 `settings.json` は欠落キーを schema 既定で
  補完して前方互換を保つ。リネーム・削除・enum 縮小は破壊的変更とし、`SettingsManager` に移行処理を
  実装してから schema を変更する。
- **schema とコードの同時更新**: 新規 top-level キーの schema 追加と Host 側読み書き実装は同一 PR で
  行い、schema 不在のままキーを書き込む不整合を作らない（`privacy-and-secure-input-spec.md` §7 /
  `app-profile-spec.md` §4 と同方針）。
- **予定済み top-level 拡張**（現行 `mvp-settings.schema.json` には未統合。各 spec の fragment が確定形で、
  統合 PR で本ファイルへマージする）:
  - `privacy`（M46。プライバシー / secure 各軸。`docs/privacy-and-secure-input-spec.md` §7 が schema
    fragment を正典とする）。
  - `profilesByApp`（M48。前面アプリ別プロファイル。`docs/app-profile-spec.md` §4 が schema fragment を
    正典とする。既存 `promptPrefixByApp` との統合は同 §6）。
- **§3.2 ナビゲーションとの整合**: §3.2 はペイン割り当ての概観であり、キーの正典一覧は本節とする。
  §3.2 に挙げる「ETW プロバイダ」は固定 GUID（§7.1）であって設定キーではない（ログ詳細度は `logLevel`
  が制御する）。LearningStore / Persona 表示はキーではなく読み取り専用ビューである。

## 4. WiX / Inno Setup インストーラ（M31）

MSIX 不可環境（Win10 LTSC, 法人ポリシーで AppX 無効）向け。

> **設定アプリと WinUI 3 ランタイムの同梱（必須）**: TIP の `ITfFnConfigure`（§3.5）は
> `azookey_settings.exe` を起動するため、本経路でも **設定アプリ本体と WinUI 3 ランタイムを
> ペイロードに含める**。MSIX フレームワーク依存が使えない LTSC 等では、設定アプリを
> **self-contained 配置（§1.3）でビルドし、その出力フォルダ一式（ランタイム同梱）を配置する**
> のが最も確実。self-contained を採らない場合は `WindowsAppRuntimeInstall.exe` をペイロードに
> 含めてインストール時に実行する。TIP DLL / Host は WinUI 3 非依存（§1.3）。

### 4.1 WiX 構成

`pkg/wix/Product.wxs`（新規）：

```xml
<?xml version="1.0" encoding="UTF-8"?>
<Wix xmlns="http://wixtoolset.org/schemas/v4/wxs">
  <Package Name="azooKey" Manufacturer="dolquis" Version="1.0.0" UpgradeCode="...">
    <Feature Id="Main" Title="azooKey">
      <ComponentGroupRef Id="Files" />
      <ComponentRef Id="RegisterTip" />
      <ComponentRef Id="SettingsApp" />
    </Feature>

    <Component Id="RegisterTip" Directory="INSTALLFOLDER" Guid="...">
      <File Source="$(var.TipDll)" />
      <RegistryKey Root="HKCU"
                   Key="Software\Classes\CLSID\{kTextServiceClsid}">
        <RegistryValue Type="string" Value="azooKey TIP" />
      </RegistryKey>
      <!-- ... profile / category 登録 ... -->
    </Component>

    <!-- 設定アプリ本体 + WinUI 3 ランタイム。ITfFnConfigure(§3.5) の起動先。
         self-contained 配置（§1.3）では azookey_settings 出力の WASDK ランタイム DLL 群も
         同梱する（ComponentGroup へ harvest）。framework-dependent の場合は
         WindowsAppRuntimeInstall.exe を実行する CustomAction を別途追加する。 -->
    <Component Id="SettingsApp" Directory="INSTALLFOLDER" Guid="...">
      <File Source="$(var.SettingsExe)" />
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
; 設定アプリ本体 + WinUI 3 ランタイム（self-contained 出力一式を recurse で同梱）
Source: "build\settings-app\Release\*"; DestDir: "{app}"; Flags: recursesubdirs
; framework-dependent の場合のみ: ランタイムインストーラを同梱して [Run] で実行
; Source: "redist\WindowsAppRuntimeInstall.exe"; DestDir: "{tmp}"

[Run]
Filename: "regsvr32"; Parameters: "/s ""{app}\azookey_tsf_tip.dll"""; \
    Flags: runhidden waituntilterminated
; framework-dependent の場合のみ: WinUI 3 ランタイムをインストール（self-contained 時は不要）
; Filename: "{tmp}\WindowsAppRuntimeInstall.exe"; Parameters: "--quiet"; \
;     Flags: runhidden waituntilterminated

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
| TIP 登録ラウンドトリップ | `tsf-tip/tests/com_smoke_test.cpp`（`TsfTipRegistrationSmokeTest`） | Windows 限定。`DllRegisterServer` 後に HKLM InprocServer32 + TSF プロファイルが存在し、`DllUnregisterServer` で消えるかを検証。env `AZOOKEY_RUN_REGISTRATION_SMOKE` + 昇格 opt-in（CI 非実行） |
| MSIX install/uninstall 残骸 | `compat-test/msix_install_uninstall.ps1` | Windows 限定・実機 VM（`gate:human-required`）。`Add-AppxPackage` → CLSID / TSF プロファイル登録の存在確認 → `Remove-AppxPackage` → 残骸 0 を半自動検証（DEV-101 / M28） |
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
- 既存：`scripts/register-dev.ps1` / `scripts/unregister-dev.ps1`（`regsvr32` 開発用経路）
- ベース：`docs/windows-tsf-host-architecture.md`
