# 配布・パッケージング 仕様（Phase 7）

本書は azooKey-Desktop Windows 版の配布形態と署名・更新・観測仕様を定める。
`plans/windows-port-roadmap.md` の Phase 7 の M28〜M34 が本書を参照する。

## 0. 配布方針（v1.0 MVP 確定 / 2026-06）

配布チャネルと署名要否を次のとおり確定する（Linear DEV-415 / DEV-416 / DEV-255）。

| チャネル | 形式 | 自前コード署名 | 位置づけ |
|---|---|---|---|
| **MVP 直接配布** | **MSI（WiX、未署名）** | 不要（任意） | v1.0 の既定。§4 が正典。GitHub Release で配布 |
| **Microsoft Store** | **MSIX** | **不要**（Microsoft が再署名） | 並行準備。§1 を Store 用 Identity で構成（DEV-416） |
| **スタンドアロン MSIX サイドロード** | MSIX | **必須**（有料 OV/EV） | **当面延期**。§2 の署名ルート（経路 B 確定済み）は本チャネル着手時に実施（DEV-255） |

判断根拠:

- MSIX は署名がインストールの前提条件だが、MSI/EXE は署名が任意である。未署名 MSI も
  インストール可能で、SmartScreen 警告 + UAC「不明な発行元」は出るが reputation building で
  許容する（出典: [Create an unsigned MSIX package](https://learn.microsoft.com/windows/msix/package/unsigned-package) /
  [Code signing options](https://learn.microsoft.com/windows/apps/package-and-deploy/code-signing-options)）。
- Microsoft Store 提出 MSIX は Microsoft が再署名するため、開発者側の有料証明書が不要
  （出典: [Publish your first Windows app](https://learn.microsoft.com/windows/apps/package-and-deploy/publish-first-app)）。
- 有料コード署名証明書を要するのはスタンドアロン MSIX サイドロードのみ。MVP では不要のため延期する。
- 先行実装（fkunn1326/azooKey-Windows = Inno Setup EXE、CorvusSKK = WiX/Inno EXE）も MSIX では
  なく MSI/EXE インストーラで配布している。

上表の「自前コード署名」欄は**インストーラの署名要否**を示す。第三者 IME として TIP バイナリに
求められる署名要件は別の境界であり、§0.1 で扱う。

以降の §1（MSIX）・§2（署名）は上表に従って読むこと。**§1 は MS Store 用 MSIX の構成**、
**§2 は延期されたスタンドアロン MSIX サイドロード向け**であり、MVP の直接配布は §4（WiX/MSI）が正典となる。

### 0.1 MVP の入力対象スコープと署名ゲート（DEV-783 で確定 / 2026-08）

MVP が入力先として保証するのは **Win32 デスクトップアプリに限る**。
**パッケージ化された UWP / Microsoft Store アプリ**での入力は MVP の対象外とし、v1.0 以降へ送る。

除外の境界は「AppContainer を使うか」ではなく「パッケージ化された UWP / Store アプリか」で引く。
Edge は renderer のサンドボックス化に AppContainer を使うが、入力欄をホストするのは Win32 プロセスであり、2026-08-14 の実機検証でも入力が成立している。
したがって Edge は対象内とする。

#### 32 bit (x86) プロセスを MVP 対象外とする理由（DEV-160 で確定 / 2026-08）

入力対象にはもう一つ、対象プロセスの bitness という境界がある。
Win32 か UWP かという上記の軸とは独立しており、MVP が保証するのは **Win32 デスクトップアプリであり、かつ x64 プロセス**である。
32 bit (x86) プロセスでの入力は v1.0 の対象外とし、既知の制限として扱う。

TSF と ctfmon は対象プロセスの bitness に一致する TIP をロードする。
本リポジトリのビルドは x64 のみであり、`tsf-tip/src/DllMain.cpp` の `DllRegisterServer` が書くのも 64 bit ビューの `HKLM\Software\Classes\CLSID` と、そこから `ITfInputProcessorProfiles` が登録する `CTF\TIP` だけである。
`scripts/register-dev.ps1` が呼ぶのも 64 bit の `regsvr32` である。
したがって 32 bit プロセスからは TIP が解決されず、azooKey を選べない。
古い Office や一部のレガシーアプリ、組み込みアプリがこれに該当する。

64 bit アプリ（Notepad、Chrome、VS Code、最新 Office、設定アプリ）は影響を受けない。
DEV-32 の実機検証動線も 64 bit アプリで成立するため、この制限は v1.0 のリリースゲートを塞がない。
`docs/handoff/dev32-verification-checklist.md` の B7 が 32 bit アプリを「既知(DEV-160)」として扱うのも、この境界に沿う。

将来 32 bit 対応を入れる場合に必要になる作業は次のとおりである。

* `tsf-tip/` に x86 ターゲットを追加し、x86 TIP DLL をビルドする
* 32 bit の `regsvr32` から `DllRegisterServer` を呼び、レジストリの 32 bit ビュー（`WOW6432Node`）へ CLSID と `CTF\TIP` を登録する
* §1.7 の `ALL APPLICATION PACKAGES` RX 付与規則を x86 DLL にも適用する（同節の付与対象は現時点で x64 TIP DLL 1 個を前提としている）
* MSIX（§1）と WiX/MSI（§4）へ x86 成果物を同梱し、アンインストール時に 32 bit ビューの残骸も消えることを確認する

再評価のトリガは 2 つある。
実利用者から 32 bit プロセスでの入力要望が挙がること、または M27（ARM64 ビルド）で複数アーキテクチャのビルドと登録の経路が整備され、x86 追加の限界コストが下がることである。
それまでは本節を既知の制限の正典とする。

#### 署名は入力対象と独立した v1.0 リリースゲート

第三者 IME へのデジタル署名要件は第三者 IME 全般を対象とし、Win32 向けの例外は文書化されていない。
[Input Method Editors (IME)](https://learn.microsoft.com/windows/apps/develop/input/input-method-editors#requirements-for-imes) は要件を満たさない第三者 IME を "blocked from running" と定めており、この記述に配布形態や入力先による限定はない。

2026-08-14 の実機検証では、未署名 MSI でも Notepad と Edge で打鍵から確定まで成立した。
ただしこれは 1 環境で観測された挙動であり、**未署名の Win32 配布がサポートされることの根拠にはならない**。
観測された動作と、文書化されたサポート条件は別のものである。

したがって次の 2 点を分けて扱う。

* **署名は入力対象にかかわらず v1.0 のリリースゲートとして残す。** Store / UWP を対象外にしたことは、署名義務を免除する理由にならない。署名ルートの選定は §2.0 が正典。
* **MVP の未署名 MSI は評価配布に限定する。** 開発とゲート検証のための配布であり、一般利用者向けの正式リリースとしては扱わない。配布時は未署名であることと評価目的であることを明示する。

#### Store / UWP を MVP 対象外とする理由

署名を調達しても Store 入力が成立するとは限らないためである。

プロセスが Code Integrity Guard を有効化している場合、`PROCESS_MITIGATION_BINARY_SIGNATURE_POLICY` の `MicrosoftSignedOnly` または `StoreSignedOnly` により、Microsoft 署名でも Store 署名でもない DLL は memory manager が `STATUS_INVALID_IMAGE_HASH` を返して map 自体を拒否する（[Code integrity guard](https://learn.microsoft.com/defender-endpoint/exploit-protection-reference#code-integrity-guard)）。
この経路が働いているなら、OV 証明書を取得しても Store 入力は成立しない。
DEV-765 のスパイクは Store 検索欄での失敗段階を「DLL 未ロード」と判定したが、それが IME 署名要件によるものか CIG によるものかは未切り分けであり、切り分けは同課題の追加 probe に残っている。

つまり Store 入力は、署名を調達しても解決しない可能性を抱えたままである。
MVP の期間内に決着させる見込みが立たないため、入力対象から外して v1.0 以降のトラックへ送る。

この決定により、DEV-673 の受け入れ条件からパッケージ化された UWP / Microsoft Store アプリでの入力成立を外す。
TIP DLL が `ALL APPLICATION PACKAGES` の RX を継承していることの確認（§1.7）は、将来の対応に備えた回帰防止として残す。

## 1. MSIX パッケージング（MS Store 向け・M28）

> **スコープ注記（§0 配布方針）**: 本節の MSIX は **MS Store 配布（DEV-416）の構成**として読む。
> Store 提出パッケージは Microsoft が再署名するため §2 の自前署名は不要。有料署名を要する
> スタンドアロン MSIX サイドロードは当面延期（DEV-255）。MVP の直接配布は §4（WiX/MSI、DEV-415）が正典。

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
/ `kTextServiceProfileGuid` と一致させる。

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
* CI / ローカル開発で MSIX と `regsvr32` を併用する場合は、
  `compat-test/msix_install_uninstall.ps1` の smoke ハーネスを使う。
  clean install / uninstall では Add-AppxPackage → 登録確認 → Remove-AppxPackage →
  残骸 0 を確認する。
  update / rollback では旧版を `-MsixPath`、新版を `-UpdateMsixPath`、失敗が期待される
  同一 package family のより新しい package を `-FailedUpdateMsixPath` に指定する。
  失敗用 package は validation / staging を通過した後、依存 framework の不足や登録内容の
  不整合によって deployment 中に失敗するものを使う。署名不正など validation 開始前に
  拒否される package は rollback 経路を通らないため、この検証には使わない。
  ハーネスは新版の Version 増加を確認した後、失敗する更新を実行し、直前の
  `PackageFullName` と `Version` が保持されることを確認する。失敗時の HRESULT と
  `FullyQualifiedErrorId` は `report.json` に記録し、実機検証で失敗段階を識別できるようにする。
  update package 2 件は常に同時に指定し、片方だけのシナリオ実行を認めない。
  実機 VM での実行は `gate:human-required` とする。
  COM 登録ラウンドトリップ自体は CTest
  `tsf_tip_com_smoke_tests::TsfTipRegistrationSmokeTest` が担い、本ハーネスは
  MSIX パッケージング層を補完する。

### 1.1.2 Option A の具体 PoC（`pkg/msix/`）

§1.1 は Option B（通常 MSIX + `com4:InProcessServer`）の参考例だった。DEV-101 の M28 PoC
として **Option A（external-location packaging / sparse package）の具体化**を `pkg/msix/`
に置く（`pkg/msix/README.md` が正典手順）。

* `pkg/msix/AppxManifest.xml` — identity package manifest。`uap10:AllowExternalContent=true`
  （Namespace `.../uap/windows10/10`、Win10 build 19041+）、`ProcessorArchitecture="neutral"`、
  `runFullTrust` + `unvirtualizedResources`、`Application` は `uap10:TrustLevel="mediumIL"` /
  `uap10:RuntimeBehavior="win32App"` + `VisualElements AppListEntry="none"`。
  **`com4:Class` / CLSID を書かない**のが Option B/C との最大の違い（TIP は外部配置 +
  `regsvr32` 登録のため）。出典: [Grant package identity by packaging with external location](https://learn.microsoft.com/windows/apps/desktop/modernize/grant-identity-to-nonpackaged-apps)。
* `pkg/msix/azookey_inference_host.exe.manifest` — app 側 side-by-side manifest。`<msix>`
  要素の `packageName` / `publisher` / `applicationId` を identity manifest の
  `Identity@Name` / `Identity@Publisher` / `Application@Id` と一致させ、exe を package
  identity へ紐付ける（不一致は登録自体は成功するが runtime で identity 欠落 = 0x80073D54）。
  この manifest は `inference-host/CMakeLists.txt` が `azookey_inference_host` の
  `.manifest` ソースとして追加し、MSVC ビルドで CMake（`cmake -E vs_link_exe --manifests`）が
  `mt.exe` へ渡してリンカ生成 manifest とマージ・埋め込みする（CMake オプション
  `AZOOKEY_EMBED_MSIX_IDENTITY`、既定 ON）。リンカへ `/MANIFEST:EMBED` を直接渡す方法は
  使えない（CMake が続けて呼ぶ `mt.exe` の入力が 0 になり `c10100a7` でビルドが落ちる）。
  埋め込みが無いと `-ExternalLocation` 付きで登録しても `Package.Current` が null になり、
  §1.5 の受け入れ条件を満たせない。identity
  package 未登録の環境では `<msix>` 要素は無視されるため、開発ビルドの挙動は変わらない。
* `pkg/msix/build-identity-package.ps1` — MakeAppx `/nv` + 任意署名の canonical ビルド経路
  （VS 拡張非依存）。`pkg/msix/Package.wapproj` は VS-IDE 向け convenience（要「Package with
  External Location」拡張）。

上記のうち **実機 VM を要さない静的整合**は `scripts/tests/msix-identity-consistency.Tests.ps1`
（Pester、CI の PowerShell lint/test ジョブ）が検証する。検証対象は (a) `<msix>` 3 属性と
identity manifest の一致、(b) Option A の不変条件（`uap10:AllowExternalContent=true`、
`runFullTrust` / `unvirtualizedResources`、`mediumIL` / `win32App`、`TargetDeviceFamily`
`MinVersion` が Win10 22H2 = build 19045 を切らないこと、`com4` 宣言を持たないこと）、
(c) `compat-test/msix_install_uninstall.ps1` の既定 `-PackageName` / `-Clsid` /
`-ProfileGuid` / `-LangId` が `Identity@Name` および `kTextServiceClsid` /
`kTextServiceProfileGuid` / `kJapaneseLangId` と一致すること、(d) 上記のビルド埋め込み配線が
残っていること。`com4` 宣言を足す（= Option B/C へ移る）場合は本節と §1.0、smoke ハーネスの
合否定義を同時に更新する必要があり、更新漏れは (b) が落として検出する。

登録は `Add-AppxPackage -Path <pkg>.msix -ExternalLocation <install-dir>`。TIP DLL / 実行体は
external location（§4 WiX/MSI の install ディレクトリ）に置き、COM 登録は
`scripts/register-dev.ps1` の `regsvr32` 経路で行う。**本 PoC は Option A を具体化するが経路の
最終確定ではない**（確定は DEV-267 の実機 smoke 結果を待つ）。identity package は MS Store
提出物ではなくサイドロード登録専用で、§0 の配布方針（MVP=MSI/WiX §4、MS Store MSIX=DEV-416、
スタンドアロン MSIX サイドロード延期）を変更しない。

### 1.2 Package.wapproj

> **Option A では本項の「同梱物」は同梱しない**: 以下は Option B/C（バイナリ同梱の通常 MSIX）
> 向けの記述。Option A（sparse / external-location）ではバイナリを MSIX に含めず external
> location へ置くため、`pkg/msix/AppxManifest.xml`（identity のみ）+ `build-identity-package.ps1`
> を使う（§1.1.2）。`pkg/msix/Package.wapproj` は Option A では identity のみをパッケージ化する。

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
依存する。VCLibs だけではクリーン VM で起動しないため、次のいずれかで同梱する。ビルド側は
`settings-app/azookey_settings.vcxproj` に Windows App SDK の component packages を
`PackageReference` で宣言する。採用 package と版の正典は同プロジェクトと
`THIRD_PARTY_LICENSES` とし、本 spec へ重複記載しない。

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
R2 向けの行は R2 再開時に適用し、R2 保留中は R1 向け構成だけを配布する。

| 構成要素 | 配布形態 | 理由 |
|---|---|---|
| llama.cpp（R1）CPU ランタイム | **base MSIX に同梱** | v1.0 既定エンジン。バイナリは小さい |
| zenz-v3 GGUF モデル本体 | **MSIX 非同梱**（初回起動時 DL。配信元は §1.6.1） | サイズ過大。§1.2 の既存方針に従う。v1.0 最小取得経路の確定は §1.6.1（DEV-202 の結論＝暫定 CC-BY-SA 保守・上流 HF から取得。§1.6.2） |
| Windows ML bootstrap（R2 用 ORT GenAI WinML） | **base MSIX に同梱（薄い）** | EP 本体は含めない |
| Windows ML EP（QNN / OpenVINO / VitisAI / NvTensorRtRtx 等） | **非バンドル（Windows Update 配信）** | Microsoft 推奨。MSIX 肥大回避・自動更新 |
| ggml-cuda（R1 CUDA, NVIDIA） | **optional add-on / 別パッケージ**（base に含めない。cudart/cublas を同梱・再配布） | CUDA ランタイムが大きく NVIDIA 環境限定。再配布は CUDA Toolkit EULA Attachment A 準拠（著作権表示保持＋条項 pass-down。§1.6.2） |
| zenz-v3 ONNX 変換モデル（R2） | **R2 保留中は配布対象外**。再開時は optional モデルパッケージ（同 §1.2 同様に非同梱・DL） | 現行 builder では変換不可。将来、検証済み変換経路を確立した場合に再評価 |

NPU / HW EP は Win11 24H2 (build 26100)+ を要するため、未満環境は R1 CPU に
フォールバックする（同 §4.3 項6）。モデル本体（GGUF / 将来の ONNX）はいずれも MSIX に同梱せず
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
| (a) MSIX 同梱 | ✗ 不採用 | base MSIX が GGUF 分（数百 MB〜）肥大する（§1.2）。加えてモデル同梱は再配布ライセンスリスクが最も高く、暫定確定（保守・CC-BY-SA、§1.6.2）でも不採用を維持する |
| (b) 初回起動時オンデマンド DL | ✅ **v1.0 目標既定（M32 後）** | M32 で切り出す共有 `HttpDownloader` + SHA256 検証基盤（§6.3 / M36-B が再利用 / `docs/auto-word-registration-spec.md` §5）を再利用。サイズ問題を回避し、配信元を DEV-202 結論で差し替えられる。M32 前（M28 時点）は (c) が operative |
| (c) 手動配置 | ✅ **常時併存（M28 の operative default 兼 恒久フォールバック）** | オフライン / 企業環境 / DL 失敗時の確実な経路。Phase 3 検証の既存前提（M8 受け入れ条件「未配置時も Host が落ちない」）をそのまま恒久サポートする |

**確定**: v1.0 の取得方式は **(b) 初回起動時 DL を目標既定、(c) 手動配置を常時
フォールバック**とし、(a) は採らない。(b) と (c) は同一の配置レイアウト（後述）に
収束するため、Host / M45 から見た「モデルがそこに在る」状態は取得方式に依存しない。

**マイルストーン順序の制約（初回パッケージングを M32 の DL 基盤に依存させない）**: (b) は共有
ヘルパ `HttpDownloader`（M32 で切り出し、M36-B が再利用）に依存する。MVP 直接配布は
**M11/M12（MVP MSI 構築・公開）→ M32** の順（延期した M29 は前提でない。§0 / roadmap 依存グラフ）で、初回
パッケージング時点では `HttpDownloader` が未だ存在しない。したがって **初回出荷時の operative
default は (c) 手動配置**とし、(b) は **M32 の共有 `HttpDownloader` が揃った時点で既定化する fast-follow** として扱う
（初回パッケージングで one-off の重複ダウンローダを書かない＝二重実装回避）。この (b) の実装は
**roadmap M32 のスコープに計上**し（`plans/windows-port-roadmap.md` M32 実装範囲の注記）、
宙に浮かせない。(b) を v1.0 ローンチまでに既定化したい場合は、ダウンローダ基盤の切り出しを
初回パッケージング（M11/M12）の前提として前倒しする（roadmap 側で M32 の該当スコープを M11/M12 前へ移す）
必要があり、これは roadmap 更新を伴う別判断とする。

##### ライセンス分岐（DEV-202 連動。暫定確定＝配信元は上流 HF に固定。§1.6.2）

再配布可否は DEV-202（`gate:human-required`）の結論に従う。**現時点は暫定確定
（保守・CC-BY-SA、§1.6.2）であり、配信元は上流 HuggingFace に固定（再ホストしない）。
GitHub Release への再ホスト最適化のみ著者確認後に解禁する。** ここで重要な
のは、**取得*機構*（HttpDownloader + SHA256 検証 + 原子的配置）は結論に依存せず
同一**で、分岐するのは **配信元 URL と同梱可否だけ**である点。プロジェクトの
GitHub Release への再ホスト自体が再配布に当たるため、DEV-202 は (a) だけでなく
(b) の配信元選択も律する。

| DEV-202 結論 | (b) の配信元 | (a) 同梱 |
|---|---|---|
| 再配布可 | プロジェクトの GitHub Release に再ホストして DL | サイズ理由で引き続き不採用 |
| 再配布不可 / 条件付き | 再ホストせず上流 HuggingFace の**`expected.json` が定める repo / ファイル**（出所は `Miwa-Keita/zenz-v3.2-small-gguf`）から DL。取得物は「期待版のピン」（下記）の SHA256 と一致する＝同一アーティファクトなので 404 や版ズレを起こさない。帰属・条項は `ThirdPartyNotices.txt` と UI に明示 | 不可 |
| 暫定確定（保守・CC-BY-SA 前提。§1.6.2） | 上流 HuggingFace（`expected.json` ピンが定める repo / ファイル）から DL し、当面プロジェクト側へ**再ホストしない**。(c) 手動配置を確実な既定経路として案内する。帰属・改変明示を `ThirdPartyNotices.txt` と UI に記載 | 不採用（サイズ＋保守方針） |

CUDA ランタイムの同梱可否（DEV-202 で併せて確認）は本経路と独立した判断である
（モデルではなくランタイム。§1.6 の optional add-on 行で扱う）。結論は §1.6.2 参照。

##### 配置パスとバージョニング

- **配置先**: `%LOCALAPPDATA%\azooKey\models\zenzai\<file>.gguf`（§3.4、および
  `model-management-spec.md` §3.1 の 1 階層スキャンと整合）。
- **取得の原子性**: DL は `<file>.gguf.part` へ書き、SHA256 検証通過後に最終名へ
  rename する（`learning/src/AtomicFile.h` と同じ temp→rename 規律）。検証前の
  ファイルをロード対象に入れない。
- **中断と再試行**: `.part` があれば Range GET で続きから再開する。
  サーバーが Range を無視した場合は `.part` を切り詰めて全量取得へ戻す。
  ネットワーク失敗または SHA256 不一致では既存の最終名を変更せず、手動配置済みの
  モデルを継続利用できる状態に保つ。
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

#### 1.6.2 配布ライセンス結論（DEV-202・v1.0 確定）

DEV-202（`gate:human-required`）の調査結論を固定する。**本節は法的助言ではなく、
一次情報の整理と運用方針である**。**v1.0 は下記の保守運用（CC-BY-SA-4.0 として扱い、
上流 HuggingFace から取得・再ホストしない）で確定**する（DEV-202 は Done）。著者確認を
要するのは「再ホスト解禁」の任意最適化のみで、その進捗・状態は Linear DEV-497 が正典
（本 spec には書かない）。

##### モデル（zenz GGUF）

| 項目 | 内容 |
|---|---|
| ピン対象 | `Miwa-Keita/zenz-v3.2-small-gguf`（§1.6.1 の `expected.json` が正） |
| HuggingFace タグ | `apache-2.0`（ただし README 空・ベース未記載） |
| 矛盾兆候 | zenz 一族（v1 / v2 / v2.5 / v3 / v3.1、safetensors 版含む）とベース `ku-nlp/gpt2-small-japanese-char` はすべて `cc-by-sa-4.0`（`zenz-v2.5-small` は `base_model: ku-nlp/gpt2-small-japanese-char` を明示。HF API 確認 2026-07）。apache-2.0 は一族で v3.2-small-gguf のみ。CC-BY-SA-4.0 は ShareAlike を持ち、Apache-2.0 は CC-BY-SA-4.0 の一方向互換リストに含まれない。v3.2 が同ベース由来なら apache タグは誤りの可能性（README 空・ベース未記載・照合できる safetensors 版なし） |
| **当面の扱い（確定）** | **保守側に倒し CC-BY-SA-4.0 として設計**する。CC-BY-SA で成立する運用は Apache でも成立するため、どちらに確定しても手戻りが出ない |
| 帰属・改変明示 | BY（帰属）は両ライセンス共通で必須。GGUF 量子化は「改変」に当たるため「量子化派生である」旨も明示（`ThirdPartyNotices.txt` / インストーラ NOTICE / 設定アプリ About。モデル名・著者 `Miwa-Keita`・出所 URL・ライセンス・量子化改変の 5 点） |
| 配信 | 当面**再ホストせず**上流 HuggingFace から取得（§1.6.1 表）。CC-BY-SA-4.0 でも再配布自体は帰属＋SA＋改変明示で可能だが、著者確認までは再ホストしない運用でリスクを最小化する |
| 商用 | Apache-2.0 / CC-BY-SA-4.0 とも商用可。ブロッカーではない |

##### CUDA ランタイム（optional add-on として同梱・再配布する）

- `ggml-cuda`（R1 CUDA, NVIDIA）は §1.6 のとおり base 非同梱の optional add-on。**この add-on に `cudart64_*.dll` / `cublas64_*.dll` を同梱・再配布する**。
- 根拠: NVIDIA CUDA Toolkit EULA の Attachment A（redistributable 一覧）が CUDA Runtime（cudart）・cuBLAS（cublas）等の再配布を許可する。
- 遵守条件: 配布物に **NVIDIA の著作権表示を保持**し、利用者へ **EULA と整合する条項を pass-down** する（`ThirdPartyNotices.txt` に NVIDIA CUDA Toolkit EULA の該当条項と著作権表示を記載）。
- **配置制約（app-only）**: 再配布する DLL は**アプリ専用（private）ディレクトリに配置し、本アプリからのみアクセスされる**ようにする。CUDA Toolkit EULA は redistributable な SDK 部分を「アプリからのみアクセスされる」ことを条件とするため（§2.6 が cudart/cublas を redistributable と定める一方、§1.1.2 がアクセス主体をアプリに限定）、共有 `PATH` / システムディレクトリ（`System32` 等）へ設置して他アプリから参照可能にしない。DLL 探索は app-local ディレクトリに限定する（例: add-on の配置フォルダを `SetDllDirectory` / manifest で明示し、グローバル `PATH` へ注入しない）。
- 版・ファイル名はビルドで固定し、`ThirdPartyNotices.txt` に列挙する。

##### Vulkan（最小リスク）

- `ggml-vulkan` 自体は llama.cpp（MIT）のビルド成果物であり再配布に制約は薄い。
- **Vulkan ローダ / ドライバは GPU ベンダのドライバが提供**し、こちらで同梱・再配布しない → Vulkan 経路に固有の再配布義務は無い。
- 先行実装 fkunn1326/azooKey-Windows でも実働実証済み（`docs/zenzai-gpu-route.md` 参考節）。

##### 再ホスト解禁の条件（恒久ルール）

GitHub Release 等への再ホスト（§1.6.1 表「再配布可」行）への移行は、**上流モデルの
ライセンスが Apache-2.0 と確認できたときに限る**。確認が取れるまでは本節の保守運用
（上流 HuggingFace 取得・帰属・量子化改変明示）を維持する。この著者確認・再ホスト解禁
タスクの起票・進捗・検証メモ・状態は **Linear DEV-497 が正典**（`gate:human-required`。
本 spec には状態を書かない。運用規約は `docs/linear-conventions.md` を参照する）。

### 1.7 Windows アプリでの TIP ロード前提と常駐起動

UWP / Microsoft Store / AppContainer 実行の Windows アプリで第三者 TIP をロードするには、
次の条件をすべて満たす。

1. TIP DLL と親ディレクトリを AppContainer から読み取り、実行できること（下記 ACL）。
2. `DllRegisterServer` が `ITfCategoryMgr::RegisterCategory` で
   `GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT` を登録していること（[Custom input method editor requirements](https://learn.microsoft.com/windows/apps/develop/input/input-method-editor-requirements)）。
3. 第三者 IME がデジタル署名されていること（[Input Method Editors (IME)](https://learn.microsoft.com/windows/apps/develop/input/input-method-editors)）。

Microsoft Learn の署名要件は第三者 IME 全般を対象とする。本節ではその原文のスコープを
明示した上で、Windows アプリでの TIP ロード前提として整理する。

配布パッケージの署名要否（§0 / §2）と TIP バイナリに対する第三者 IME の署名要件は
別の境界である。Microsoft Store が提出 MSIX を再署名することだけを、TIP DLL の署名要件を
満たした証拠として扱わない。

> **MVP でのスコープ（§0.1 / DEV-783）**: **パッケージ化された UWP / Microsoft Store アプリ**での
> 入力は MVP の対象外であり、要件 1・2 を扱う本節は v1.0 以降で当該対応に着手するときの前提整理として読む。
> 要件 3（署名）はこのスコープ判断と独立しており、入力対象にかかわらず **v1.0 のリリースゲートとして残る**
> （§0.1）。MVP の未署名 MSI は評価配布に限定する。署名ルートの選定は §2.0 が正典
> （所有課題だった DEV-255 は Canceled、スコープ判断は DEV-783 で確定済み）。

**AppContainer DLL ACL**: UWP / Microsoft Store / AppContainer 実行のアプリで TIP を
有効化するには、TIP DLL に `ALL APPLICATION PACKAGES`（SID `S-1-15-2-1`）への
読み取り+実行 (RX) が必要である。AppContainer プロセスの実効アクセスは、ユーザー /
グループ SID による判定と AppContainer SID（package SID・capability SID）による判定の
積で決まる。対話ユーザーが読める DLL でも、DACL が AppContainer 側の principal を
含まなければアクセスは成立しない（[Launch an AppContainer](https://learn.microsoft.com/windows/win32/secauthz/implementing-an-appcontainer#appcontainer-overview)）。
ctfmon は対象アプリのプロセス内へ TIP DLL を in-proc ロードするため、この ACE が無いと
当該アプリで日本語入力ができない。

#### 付与対象・タイミング・権限（DEV-204 確定）

| 配布経路 | 付与主体 | 付与対象 | タイミング |
|---|---|---|---|
| §4 WiX/MSI（MVP 既定） | Windows（`%ProgramFiles%` からの継承） | インストール先配下 | インストール時。**MSI は独自 ACE を追加しない** |
| 開発用 `regsvr32` 登録 | `scripts/register-dev.ps1` | 登録する TIP DLL と**その親ディレクトリ** | **登録時**（`regsvr32` 実行の直前） |
| Option A（external-location MSIX） | external location の配置先に準ずる（上 2 行のいずれか） | 同上 | 同上 |
| Option B/C（通常 MSIX） | パッケージ側で解決 | — | 不要 |

* **権限は RX のみ**。AppContainer アプリへ書き込み権限を与えない。
* **登録時のみに一本化し、ビルド時フック（post_build `icacls`）は持たない**。親ディレクトリへ
  継承付き ACE を置くので、再ビルドで置き換わった TIP DLL が ACE を継承する。付与点が
  登録スクリプト 1 箇所に閉じ、付与漏れの探索範囲が広がらない。先行実装
  fkunn1326/azooKey-Windows はビルド時とインストール時に二重付与するが、本リポジトリは
  継承 ACE が再ビルドを吸収するため二重化しない。
* **付与対象の DLL**: 本リポジトリのビルドは x64 のみである（`CMakePresets.json`）。したがって
  現時点の対象は x64 TIP DLL 1 個。32 bit プロセスへの入力は §0.1 のとおり v1.0 の対象外であり、
  将来 x86 TIP DLL を追加する場合は、同じ規則を当該 DLL にも適用する
  （`-TipDllPath` を x86 成果物へ向けて登録スクリプトを再実行する）。
* **既存 ACE の判定**: ディレクトリでは「実効 RX があるか」ではなく「`ObjectInherit` を持つ
  allow ACE があるか」を見る。非継承の明示 ACE を「設定済み」と扱うと、再ビルドで作り直された
  DLL が継承元を持たず AppContainer から読めなくなるため。
* **解除**: `scripts/unregister-dev.ps1` は**登録処理が実際に追加した ACE のみ**を除去する。
  登録と解除は別プロセスであり、DACL だけからは「手で置いた ACE」と自前の ACE を区別できない。
  そこで付与したパスを machine-wide 台帳（`HKLM\Software\azooKey\DevRegistration` の
  `AppContainerAclGrants`）に記録し、解除は台帳にあるパスだけを対象とする。台帳から
  エントリを落とすのは、ACE を除去できたか、既に存在しないことを確認できた場合に限る。
  DACL 書き込みが失敗したパスはエントリを残し、次回の解除で再試行する（消してしまうと
  ACE が残ったまま追跡不能になるため）。台帳は最後のエントリと同時に削除する。除去は `RemoveAccessRuleSpecific` による完全一致
  （権限・継承フラグ・伝播フラグが一致する非継承 allow ACE）で行い、権限や継承が異なる
  手動設定の ACE には触れない。継承 ACE と `%ProgramFiles%` / `%SystemRoot%` 配下も対象外とし、
  MSI 導入先の ACL を壊さない。
  なお、同一 SID・同一フラグの ACE は `AddAccessRule` の時点で 1 件へ統合されるため、
  自前 ACE が後付けの広い権限へ吸収された場合は完全一致せず、解除は何もしない。
  他者が広げた権限を削るより残す側に倒す判断である。
* **opt-out**: 両スクリプトの `-SkipAppContainerAcl`。ビルドツリーを AppContainer から到達可能に
  したくないホスト向けで、指定時は UWP / Microsoft Store アプリで TIP がロードされない。
* **実装と検証**: 共通ヘルパは `scripts/AppContainerAcl.ps1`。実機を要さない検証は
  `scripts/tests/register-dev.Tests.ps1`（Pester、CI の PowerShell lint/test ジョブ）が担い、
  付与 → 冪等 → 解除のラウンドトリップ、再ビルド相当の継承、非継承 ACE がある場合も継承 ACE を
  追加すること、登録前から存在した ACE が解除後も残ること、権限の異なる手動 ACE を消さないこと、
  解除失敗時に台帳エントリが残ること、書き込み権限を付与しないこと、保護対象パスを
  書き換えないことを確認する。
* **祖先ディレクトリの traverse は付与しない**。既定構成の Windows では bypass traverse
  checking（`SeChangeNotifyPrivilege`）により最終要素の DACL だけが効くという前提を置く。
  グループポリシーで同特権を外した環境ではビルドツリーの祖先に traverse が別途必要になる。
  この前提は実機ゲートで確認する。

実機検証は DEV-673（MSI インストール先の継承 ACL）で行う。DEV-271（開発用登録経路での
サンドボックスアプリ入力成立）は DEV-673 に統合され Duplicate としてクローズ済み。
パッケージ化された UWP / Store アプリでの入力成立そのものは §0.1 により MVP の受け入れ条件から
外れており、DEV-673 で確認するのは継承 ACL までとなる。MSIX 側の制限は DEV-101（com4:ComServer ACL 制限）と
連動する。

**常駐起動（参考）**: Host / launcher のログオン常駐を Run キーでなく **Task Scheduler
（LogonTrigger + RunLevel=HighestAvailable）+ VBS 非表示起動**で実現し、アンインストール
時に `schtasks /Delete` する方式がある（UAC プロンプト無しの常駐）。
`RunLevel=HighestAvailable` は UAC 構成依存である点に注意。MSIX 配布では startup task /
app execution alias の利用を優先する。

## 2. EV/OV コード署名（M29）

> **スコープ注記（§0 配布方針 / §0.1）**: 本節（自前コード署名）が要るのは 2 つの文脈である。
> 1 つは**スタンドアロン MSIX サイドロード**で、これは §0 の配布方針により当面延期する。
> もう 1 つは**第三者 IME としての TIP バイナリ署名**（§1.7 の要件 3）で、こちらは入力対象の
> スコープと独立に **v1.0 のリリースゲート**として残る（§0.1 / DEV-783）。
> MVP 期間の MSI 直接配布は未署名（§4 / DEV-415）だが、これは評価配布としての扱いであり、
> 署名義務が免除されたことを意味しない。MS Store の MSIX は Microsoft が再署名するため、
> Store チャネルに限れば本節の自前署名を要しない。
>
> 所有課題だった DEV-255 は Canceled であり、本節を再開するときは新規に課題を立てる。
> 署名鍵の保管要件は CA/Browser Forum の HSM 必須化（2023-06-01 施行）によって変わっており、
> 新規取得の証明書では PFX を入手できない。既定の署名経路は §2.2 を参照する。

### 2.0 署名経路の選定

署名証明書の調達ルートは v1.0 / v1.x で 3 候補ある。Microsoft Learn の現行ガイ
ダンスは **Azure Artifact Signing（旧 Trusted Signing）** を非ストア配布の推奨
として提示する（[Code signing options](https://learn.microsoft.com/windows/apps/package-and-deploy/code-signing-options)）。

| 経路 | 推奨度 | 価格 | CI 統合 | SmartScreen 信頼 | 制約 |
|---|---|---|---|---|---|
| A. **Azure Artifact Signing** | 推奨 | ≈$10/月 | ◎（GitHub Actions / Azure DevOps） | reputation building（OV と同等） | 組織: 米/カナダ/EU/英国のみ。個人: 米/カナダのみ |
| B. **Azure Key Vault + [AzureSignTool](https://learn.microsoft.com/windows/msix/desktop/cicd-keyvault)** | 個人向け次善 | Key Vault 料金 + OV cert | ◎ | reputation building | コミュニティ製 .NET ツール（[vcsjones/AzureSignTool](https://github.com/vcsjones/AzureSignTool)） |
| C. **伝統的 OV/EV cert + PFX を GitHub Secrets** | 旧証明書のみ | OV: 数万円/年 / EV: 10 万円超/年 + HSM | △（EV の HSM 物理トークンは不可） | reputation building（EV の即時信頼は 2024 年に廃止） | **新規取得では PFX 不可**（2023-06-01 の HSM 必須化）。2023-06-01 以前に発行され PFX を保持している証明書に限り成立する。PFX 漏えいリスク、CI でのキー回転が煩雑 |

**判定（DEV-255 で確定）**: 署名主体は日本の個人開発者であり、経路 A（Azure Artifact
Signing）は地域制限で不可（個人 = 米/加のみ）。reputation building 許容のため EV（経路 C）は
不採用。新規 OV は HSM 必須化で PFX を入手できないため、CI 署名を保てる **経路 B（Azure Key
Vault + AzureSignTool）/ B'（CA クラウド署名: SSL.com eSigner / DigiCert KeyLocker 等）** を
採用する。ただし本署名はスタンドアロン MSIX サイドロードと Windows アプリ入力対応の 2 用途に
限られ、§0 / §0.1 のとおりいずれも MVP の対象外である
（MVP の MSI / MS Store の MSIX はいずれも自前署名不要）。

上記の経路判定（2026-06 時点）は調達ルートの比較として有効だが、費用対効果の前提は
2026-08 に更新されている。EV 証明書の「即時信頼」は 2024 年に廃止され、EV も OV と同じ
reputation building になった（[Code signing options](https://learn.microsoft.com/windows/apps/package-and-deploy/code-signing-options)）。
経路 C の EV を選ぶ理由は SmartScreen 対策としては残っていない。
また §0.1 のとおり、署名を調達しても Code Integrity Guard により Store 入力が成立しない
可能性がある。着手前にその切り分け（DEV-765）を済ませる。

証明書調達・申請手順は所在地と組織化状況に依存する人間判断であり、再開時に
`gate:human-required` 課題を新規に立てて扱う（従来の DEV-255 は Canceled、スコープ判断は
[Linear DEV-783](https://linear.app/dolquis/issue/DEV-783) で確定済み）。

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

**新規取得の証明書では PFX 経路を採らない。** CA/Browser Forum の Code Signing
Baseline Requirements 改定（**2023-06-01 施行**）により、新規発行されるコード署名
証明書は OV / EV を問わず FIPS 140-2 Level 2 / Common Criteria EAL4+ 相当の HSM 上
での鍵生成・非エクスポータブル保管が必須となった。結果として、**秘密鍵を PFX
ファイルとしてエクスポートできない**（[SSL.com](https://www.ssl.com/article/code-signing-key-storage-requirements-will-change-on-june-1-2023/) /
[DigiCert](https://knowledge.digicert.com/general-information/new-private-key-storage-requirement-for-standard-code-signing-certificates-november-2022)）。

したがって新規取得時の既定は次のいずれかとする。

1. **§2.2.B Azure Key Vault + AzureSignTool** — §2.0 の判定で採用した経路 B。
   Key Vault の HSM に鍵を置いたまま CI から署名する。
2. **CA クラウド署名（経路 B'）** — SSL.com eSigner / DigiCert KeyLocker /
   Sectigo cloud signing 等。CA 側の HSM に鍵を置いたまま CI から署名する。

§2.2.A（Azure Artifact Signing）は Windows が既定で信頼する点で最も強いが、
個人契約は米国・カナダに限られ、日本の個人開発者は利用できない（§2.0）。

#### legacy: PFX を GitHub Secrets に格納する経路

**2023-06-01 以前に発行され、手元に PFX を保持している証明書に限り**成立する。
新規取得ではこの経路を選べないため、新規メンテナはここから始めないこと。

| Secret 名 | 内容 |
|---|---|
| `WINDOWS_PFX_BASE64` | PFX ファイルを base64 エンコードしたもの |
| `WINDOWS_PFX_PASSWORD` | PFX のパスワード |
| `WINDOWS_CERT_THUMBPRINT` | 証明書のフィンガープリント |

現行の `.github/workflows/release.yml` は未署名 MSI をビルドする経路であり、PFX
import ステップを持たない（§2.3）。署名を再開する場合も、この legacy 経路を CI へ
復活させない。

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
* **個人名義の OV では CN が法的氏名になる。** 個人開発者が OV を取得すると
  Subject CN は本人確認で用いた法的氏名で発行され、ハンドル名にはならない。
  §1.1 manifest 例の `Publisher="CN=dolquis"` はそのままでは通らないため、証明書
  取得後に `Identity@Publisher` を実際の Subject DN 全文へ差し替える。ハンドル名を
  残せるのは `DisplayName` / `PublisherDisplayName` の側であり、完全一致が要求
  されるのは `Identity@Publisher` と証明書 Subject DN だけである。

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

現行の `.github/workflows/release.yml` は、§4 の未署名 MSI をビルドする経路である。
job 全体を repository variable `RELEASE_ENABLED == 'true'` でガードし、`v*` タグでは
MSI を Draft Release に添付する。`workflow_dispatch` は同じガード下でビルド確認に使い、
タグ以外では Release を作成しない。

スタンドアロン MSIX と自前コード署名は DEV-255 で延期しており、この workflow へ
署名処理を混在させない。Store 用 MSIX の CI は DEV-416 で別経路として設計する。
署名経路を再開する場合は、§2.0 で経路を確定してから workflow と本節を同時に更新する。

### 2.4 ローカル署名検証手順

署名経路を再開した場合は、CI で署名した MSIX を手元で確認する。
Windows SDK の `signtool` で:

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
3. **配布形態との整合** — v1.0 MVP の配布形態（§0 / §4 の未署名 MSI）に、設定アプリ
   `azookey_settings.exe` を **self-contained 配置（§1.3・§4 の同梱必須注記）**でペイロード同梱できる。
   MS Store 用 MSIX（§1）では packaged EXE として同梱する。いずれも WinUI 3 の unpackaged /
   packaged 両対応（下記事実更新）で成立し、単一 UI スタックのまま両チャネルに載る。

> **事実更新（Microsoft Learn, 2026-06 時点。旧記述の訂正）**
> - WinUI 3 がサポートする言語は **C# と C++/WinRT のみ**（C++/CX は非推奨）。
> - WinUI 3 / Windows App SDK の対応 OS は **Windows 10 バージョン 1809（build 17763）以降**。
>   ただしこれは**設定アプリ（WinUI 3）単体の下限**であり、配布パッケージ全体の最小 OS では
>   ない（下記）。
> - **unpackaged（MSIX なし）配布もサポートされる**（Windows App SDK 1.0 以降）。旧記述
>   「WinUI 3 デスクトップは MSIX 必須・unpackaged 不可」は**誤り**。本プロジェクトは §0 の
>   とおり **MVP を未署名 MSI（設定アプリは unpackaged / self-contained 同梱）**で配布し、
>   **MS Store 向けには packaged（MSIX）**を用いる。WinUI 3 は両形態に対応するため単一 UI スタックで両立する。
> - **配布パッケージ全体の最小 OS は TIP の配布経路で決まり、設定アプリの 1809 より高い**:
>   §1 の経路 A（external-location packaging）は **Win10 2004 / build 19041 以上**、経路 B
>   （通常 MSIX + `com4:InProcessServer`）は **build 20348 以上**（`MinVersion="10.0.20348.0"`、
>   §1.1）。設定アプリの WinUI 3（1809+）はこのパッケージ下限に内包されるため、MSIX の
>   `TargetDeviceFamily` には 1809 ではなく §1 の経路別下限を設定する。

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
- フレームワーク: WinUI 3（採用 component packages と版は
  `settings-app/azookey_settings.vcxproj` / `THIRD_PARTY_LICENSES` を正典とする。更新時は
  self-contained 出力と MSI の回収結果を再検証する）
- 配布: MVP は MSI にペイロード同梱（別 EXE `azookey_settings.exe`。self-contained 配置、§1.3 / §4）。
  MS Store 用 MSIX（§1）では packaged EXE として同梱（配布方針は §0）

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
> を持ち、参考側（cpu/cuda/vulkan）より広い。詳細は DEV-120 のコメント参照。**この能力検出は、当該
> バックエンドをリンクしたビルドを配布してから導入する**（DEV-854 再確定）。DLL 存在判定が答えるのは
> 機器側の可否であって、ビルドが当該バックエンドを含むかではないため、未リンクのバックエンドを
> DLL 判定で出し分けると実効のない選択肢を有効化してしまう。v1.0 UI の縮小と露出条件は §3.7 を正典とする。

### 3.3 Host との IPC

設定アプリは `%LOCALAPPDATA%\azooKey\config\settings.json` を更新した後、Host に
payload 空の `UpdateConfig` メッセージを送信して再読込を促す。設定オブジェクトは
IPC schema に二重定義しない。`settings.json` を保存するのは設定アプリだけだが、Host も
parse に失敗した `settings.json` を `.invalid*` へ rename するため mutator である。保存側と
Host の read から rename までは同一の named mutex 下で直列化する（writer 責務とこの排他の
正典は `docs/windows-tsf-host-architecture.md`「共有ユーザーデータの writer 責務」）。

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
  `azookey_settings.exe` に渡し、該当言語プロファイルの設定ページを初期表示する。設定アプリは
  `AppInstance::FindOrRegisterForKey` で単一インスタンスを登録し、後続起動は
  `RedirectActivationToAsync` で新しい引数を既存インスタンスへ転送する。既存ウィンドウは転送後に
  前面化し、ウィンドウタイトルによるプロセス間検索は行わない。受信側は LANGID を16進値、
  profile を GUID として検証し、未指定と不正値を区別する。
- **正典実装**: 具体コード（`ITfFnConfigure`・`ShellExecuteExW` による起動・
  インストールパス解決）は
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
| `backendPreference` | enum `auto`/`cpu`/`cuda`/`vulkan`/`winml`/`directml`/`npu` | `auto` | 詳細※ | M24（device 選択 UI は `model.backendPreference` にバインド〔下記※〕。v1.0 UI は `auto`/`cpu` に縮小、§3.7） | モデル再ロード | `copilot-pc-backend-spec.md` §4 |
| `epPreference` | enum `auto`/`npu`/`gpu`/`cpu` | `auto` | 詳細※ | M24（EP 選択 UI は `model.epPreference` にバインド〔下記※〕。root は back-compat のみ・UI 非バインド） | モデル再ロード | `copilot-pc-backend-spec.md` §4.4 |
| `powerProfile` | enum `auto`/`performance`/`battery_saver` | `auto` | 詳細 | M25 | 即時 | `copilot-pc-backend-spec.md` §5–§6 |
| `logLevel` | enum `error`/`warn`/`info`/`debug` | `info` | 詳細 | Phase 5（基本） | 即時 | schema / §7 |
| `model` | object（`model-management-spec.md` §7 が下位フィールドを定義） | — | モデル / 一般 | M45（`enabled`/`selectedPath`/`backendPreference` の 3 フィールドは v1.0=M11 で先行露出、§3.7） | モデル再ロード | `model-management-spec.md` §5/§7 |
| `autoUpdate` | object（`enabled`/`channel`/`checkIntervalHours`） | — | 一般 | M32 | 即時 | 本書 §6 |

> オブジェクト型キー（`model` / `autoUpdate`）の下位フィールドは「正典」列の spec が確定形を
> 持つ。本表で再掲せず、ネスト構造の単一情報源を維持する。

> **※ device 選択 UI のバインド先（§3.7）**: `backendPreference` / `epPreference` の **root tier は後方互換用の
> 下位レイヤ**であり（解決順 `model.*` ＞ root、`model-management-spec.md` §5.2）、設定アプリの device 選択 UI は
> **v1.0 / M24 とも `model.backendPreference` / `model.epPreference`（`model.*` tier）にバインド**する。**root tier は
> UI にバインドしない** — M24 で root を再露出すると、`model.*` を設定済みのユーザーでは編集しても実効値が変わらず
> （`model.*` ＞ root）「selector が壊れて見える」二重ソース問題に戻るため。root tier は `settings.json` 直書きの
> 後方互換としてのみ残し、§3.7 の保存規律（device 保存時に root `backendPreference` を削除して `model.*` を単一
> 実効ソースにする）と整合させる。

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
  - `privacy`（`docs/privacy-and-secure-input-spec.md` §7 が schema fragment を正典とする）。
    object 全体（`mode` / `custom` / secure 各軸）の統合は **M46** だが、**`crashReportConsent`
    subfield のみ M33（ETW/WER）で先行導入**する。M33 受け入れ条件が
    `privacy.crashReportConsent = off` を要求するため（§8.3）、M33 統合 PR で `privacy` object に
    `crashReportConsent` を既定値付きで追加し（schema とコードを同一 PR で）、残り subfield は M46 で
    加算する（加算的拡張は本節「拡張方針」）。subfield ごとの導入 M は privacy spec §7 を正典とする。
  - `profilesByApp`（M48。前面アプリ別プロファイル。`docs/app-profile-spec.md` §4 が schema fragment を
    正典とする。既存 `promptPrefixByApp` との統合は同 §6）。
- **§3.2 ナビゲーションとの整合**: §3.2 はペイン割り当ての概観であり、キーの正典一覧は本節とする。
  §3.2 に挙げる「ETW プロバイダ」は固定 GUID（§7.1）であって設定キーではない（ログ詳細度は `logLevel`
  が制御する）。LearningStore / Persona 表示はキーではなく読み取り専用ビューである。

### 3.7 v1.0 設定 UI の最小機能セット（M11）と v1.0 / v1.x 境界

§3.6 は schema の最終形（全 top-level キー）を確定する索引である。本節はそのうち
**v1.0（M11）出荷時に設定アプリへ UI として出す最小サブセット**を確定し、残りを v1.x（M30
フル UI）へ送る境界を引く。狙いは 2 つ: D-03（WinUI 3 UI フレームワークスパイク）の実装
ボリュームを確定させ、v1.0 を遅らせないこと。本節は新キーを追加しない —— v1.0 UI は
`settings/mvp-settings.schema.json`（§3.6 の正典 schema）の**部分集合**を描画するだけで、
schema 自体は superset のまま変えない。

> **設計判断（DEV-107）**: 起票時の推奨案は `zenzaiEnabled` / `modelPath` / `userDictionaryPath`
> という新キーを提案していたが、§3.6 で schema 正典が確定した後はこれらを新設しない。M11 の目的
> （`plans/windows-port-roadmap.md` M11「Zenzai ON/OFF・辞書管理・デバイス選択」）を**既存の正典
> キーへ写像**して実現する: Zenzai ON/OFF → `model.enabled`、デバイス選択 → `model.backendPreference`
> （v1.0 は enum を縮小）、モデル選択 → `model.selectedPath`。ユーザー辞書は設定 `settings.json` の
> キーではなく `%LOCALAPPDATA%\azooKey\data\user_dict.json`（§3.4）の内容であり、v1.0 はキーではなく
> 「編集」アクション（下記ボタン）で扱う。

#### v1.0（M11）で UI 化するキー（最小サブセット）

| キー | v1.0 UI の型 / 値域 | UI ペイン | 裏づけ M | 備考 |
|---|---|---|---|---|
| `model.enabled` | bool（「Zenzai を使う」トグル） | 一般 | M8 | false で SimpleConverter 固定（`model-management-spec.md` §7） |
| `model.backendPreference` | enum **`auto` / `cpu` に縮小** | 一般 | M8 | `auto` は Host 起動時の既定バックエンド（`AZOOKEY_BACKEND` / `--backend`）に従い、`cpu` は CPU に固定する。`cuda` を含む残り enum と `epPreference` の解禁条件は下記「v1.0 / v1.x 境界表」に置く（判断の根拠は「デバイス選択の enum 縮小（DEV-854 再確定）」） |
| `model.selectedPath` | string（モデルの絶対パス。**空＝モデル未選択**） | 一般 | M8 | 空は「ピン既定へ自動解決」ではない。Host は `selectedPath` をそのまま `autoLoadOnHostStart` でロードし、空なら何もロードせず SimpleConverter（M8 受け入れ「未配置時も落ちない」）。下記「probe-then-commit」を参照 |
| `logLevel` | enum `error`/`warn`/`info`/`debug` | 詳細 | M2〜 | 診断用。ログ詳細度のみ（ETW プロバイダ GUID は設定キーではない、§3.6） |

- **デバイス選択の enum 縮小（DEV-854 再確定）**: v1.0 UI は `model.backendPreference` を
  `auto` / `cpu` の 2 値だけ描画する。**`cuda` は v1.0 UI に出さない**。CUDA backend の実リンク
  （DEV-223）は中止され、`cuda` 要求は `docs/copilot-pc-backend-spec.md` §4.4 のとおり成功 LoadModel の
  まま CPU へ降格するため、選択肢として出すと実効のない選択になる。ランタイム DLL の存在判定による
  disable + Tooltip（§3.2 参考実装注）はここでは採らない。DLL 判定が答えるのは「この機器に CUDA
  ランタイムがあるか」であって、実際の制約は「配布ビルドが CUDA をリンクしていないか」だからである。
  CUDA ランタイムを備えた NVIDIA 機ではむしろ選択肢が有効化され、実効のない選択がそのまま残る。
  能力検出は CUDA リンク済みビルドを配布してから導入する。
- **v1.0 の 2 値が持つ意味**: `auto` は Host 起動時の既定バックエンド（ビルド時 `AZOOKEY_BACKEND`
  または `--backend`）に従い、`cpu` は CPU に固定する（解決順は `model-management-spec.md` §5.2、
  `auto` のときに root `backendPreference` へ委ねる挙動を含む）。CPU 推論のみを含む v1.0 配布ビルドでは
  どちらも R1 CPU に解決するが、保存される意図は異なる。`auto` はアクセラレータを選ぶ経路が配線された
  ビルドを配布したときにそれへ追随し、`cpu` は追随せず CPU に留まる。ここでいう配線とは、配布ビルドの
  既定 backend をそのアクセラレータへ切り替えるか、`model-management-spec.md` §5.1 の auto 推奨ロジックを
  実装して実行時に選ばせるかのいずれかである。`auto` はこの既定に委ねるだけで自ら列挙・選択しないため、
  アクセラレータをリンクしただけでは追随しない。M11 の目的「デバイス選択」はこの 2 値で満たし、
  UI にはアクセラレーションの有無を誤解させないため「v1.0 の配布ビルドは CPU 推論のみを含む」という
  静的な補足文を添える。
- **`vulkan` の UI 露出条件を R1 側へ切り離す**: `vulkan`（R1 ggml-vulkan）の UI 露出は、これまで
  v1.x（M24）としていたが、M24 は R2 / Windows ML のマイルストーンであり `docs/copilot-pc-backend-spec.md`
  §4.3 のとおり R2 は保留中である。R1 に属する `vulkan` を保留中の R2 に従属させないため、**露出条件を
  「ggml-vulkan ビルドを配布に含め、かつ実行時にそれを選ぶ経路が配線された時点」へ改める**（R1 側の
  条件であり、M24 の進捗に依存しない）。**リンクだけでは足りない**。上記のとおり `auto` は既定 backend に
  委ねるだけなので、配線がなければ `vulkan` を選んでも `auto` との差が生まれず、`cuda` と同じ実効のない
  選択になる。現行の CMake は `AZOOKEY_BACKEND` が `cpu` / `cuda` のみで ggml-vulkan ビルドを持たず、
  §1.6 の base MSIX も GPU ランタイムを含まないため、**v1.0 UI には出さない**。ビルドと配線は DEV-944 で追う。
- **降格の可視化**: Host 側の Health 判定は §4.4 / `docs/zenzai-inference-spec.md` §9.2.1 のまま変えない。
  `cuda` 降格はモデルをロードでき変換も動くため `Health=ok` であり、degraded は実エラーで降格した場合に
  限る。v1.0 UI から `cuda` を外すことで UI 由来の実効のない選択は消え、残るのは `settings.json` 直書きの
  `cuda` だけになる。この値は下記の「UI が描画しない schema 有効値」と同じ扱いとし、UI は選択なしの状態で
  現在値と「現在の配布ビルドでは CPU 実行へ降格する」旨を表示し、保存時にその値を温存する。
- **UI が描画しない schema 有効値の扱い**: `settings.json` に v1.0 UI が出さない schema 有効値
  （`cuda` / `vulkan` / `winml` / `directml` / `npu`）が直書きされていても、Host は schema 上受理し、
  UI は選択肢として描画しないだけで破棄しない。schema 正典（§3.6）は完全 enum を保持する
  （enum 縮小は §3.6 拡張方針で破壊的変更に当たるため、UI 露出だけを変え schema には手を入れない）。
- **`model.*` の UI 露出を M11 へ前倒し**: §3.6 の「導入」列は `model` を M45 と記すが、これは**フル
  モデル管理 UI**（`epPreference` / `nGpuLayers` / `benchmark*` / `autoLoadOnHostStart` / `fallbackToSimpleConverter`
  等の全フィールド）の導入時期である。v1.0 はそのうち `enabled` / `backendPreference`（縮小）/ `selectedPath`
  の 3 フィールドのみを露出する。キーは schema に既存のため schema 変更は不要で、変わるのは UI 露出の時期だけ。
  実効値の解決順（`model.*` ＞ root 同名キー、§3.6 永続化 / `model-management-spec.md` §5.2）は v1.0 でも同一。
- **モデルパスの probe-then-commit（既定 autoselect と明示選択を分ける）**: 空 `model.selectedPath` は
  「ピン既定モデルを使う」を意味しない（Host は空を未選択として扱い SimpleConverter へ落ちる、
  `model-management-spec.md` §7 / `autoLoadOnHostStart`）。よって v1.0 UI は**空を既定ファイルへ暗黙解決しない**。
  `selectedPath` 確定の規律は §1.6.1「配置パスとバージョニング」の取得経路定義に従い、**2 経路を混同しない**:
    - **既定モデルの autoselect（ピン依存、§1.6.1 (b) DL / (c1)）**: ピン定義（`models\zenzai\expected.json`）の
      モデルファイルが**実在し SHA256 がピン一致することを probe してから**、その絶対パスを `model.selectedPath`
      へ commit する（commit 後に「既定モデル使用中」と表示）。ピン未投入 / 不一致なら autoselect しない。
    - **ユーザー / 管理者の明示選択（ピン非依存、§1.6.1 (c2)）**: 設定 UI のモデル選択（ファイルピッカ）や
      `settings.json` 手編集で選ばれた**任意の実在 GGUF パス**を probe-load して `model.selectedPath` へ commit する。
      **ピン未投入でも成立し、ピン一致を要求しない**（§1.6.1 (c2)「ピン非依存の唯一の手動経路」を v1.0 UI が塞がない。
      非ピンのローカル GGUF を管理者が選ぶ M28/M11 経路を degraded に落とさない）。
  いずれの経路でも、パス未確定の間は `selectedPath` を空のまま残し、UI は「モデル未配置（SimpleConverter 動作）」を
  表示する。これにより「既定モデルと表示しているのに Host にパスが渡らず fallback のまま」と「明示選択した非ピン
  モデルが commit されず degraded のまま」の両方の不整合を防ぐ。
- **root `backendPreference` を v1.0 UI に二重表示せず、保存時に UI 選択値で上書きする**: デバイス選択は
  `model.backendPreference` 一本に統一する（`model-management-spec.md` §5.2 の解決順は `model.backendPreference`
  → root `backendPreference` → 既定 `auto`）。root を非表示のまま write-back で温存すると、ユーザーが直書きした
  root `cuda` 等が UI の「auto」選択を上書きし続け、UI からバックエンドを auto へ戻せない。これを防ぐため、**v1.0 UI が
  デバイス選択を保存するとき、UI で選んだ値（`auto` を含む）を `model.backendPreference` に書き、root `backendPreference`
  を削除する**。**root の隠れ値を `model.*` へ「移行」してはならない** —— 移行すると root `cuda` が
  `model.backendPreference` に複製され、UI の `auto` 選択を上書きし続けてリセット不能になる（root 値を持ち越さず、
  常に現在の UI 選択で上書きする）。これにより `model.backendPreference` が単一の実効ソースになる。この root
  `backendPreference` 削除は後述 write-back 規則の例外（保持対象外）とする。`epPreference` は v1.0 UI が露出せず
  device 選択にも従属しないため、root / `model.epPreference` とも温存する（直書き intent を壊さない。v1.x で管理）。
  **`model.backendPreference` が v1.0 UI の描画しない schema 有効値（`cuda` 等）のときは上書き対象から外し、
  その値を温存する**（DEV-854 再確定）。このとき UI はデバイス選択を未選択の状態にし、現在値と降格の注記を
  表示する。上書きは UI が選択を持つときの規律であり、UI が選べない値をその 2 値へ丸めると直書き intent を
  壊すためである。root `backendPreference` の削除はこの場合も行い、実効ソースを `model.backendPreference` に
  一本化する規律は変えない。

#### v1.0（M11）の UI アクション（設定キーではないボタン）

- **「ユーザー辞書を編集」** → ユーザー辞書（M9、`user_dict.json`）の追加 / 削除を行う CLI / デバッグ
  probe を起動する（D-09 と接続。`AddUserWord` / `RemoveUserWord` 経路は roadmap M9）。v1.0 は専用 GUI を
  作らず、最小の編集導線のみを提供する。フル辞書管理 GUI は v1.x（M30）。設定アプリは v1.0 / v1.x とも
  `user_dict.json` を直接開かず、CLI probe または Host への IPC を経由する
  （`docs/windows-tsf-host-architecture.md`「共有ユーザーデータの writer 責務」）。
- **「ログ出力先を開く」** → `%LOCALAPPDATA%\azooKey\logs\`（§3.4）を Explorer で開く。

#### v1.0 / v1.x 境界表

「v1.0 で UI 化」しないキーは、v1.0 では `settings.json` 直書き（＋ Host hot-reload、§3.3）か
debug probe で操作し、v1.x（M30 フル UI / 各機能の UI 化マイルストーン）で UI 露出する。下表は
§3.6 の全キーに対する v1.0 / v1.x の UI 露出区分である（意味論の正典は §3.6「正典」列に従う）。

| キー | v1.0 UI | v1.x で UI 化（暫定: schema 直書き / probe） |
|---|---|---|
| `model.enabled` | ◯（一般） | — |
| `model.backendPreference` | ◯（一般、`auto`/`cpu` のみ） | `vulkan` = ggml-vulkan ビルド配布 + 実行時選択経路の配線（DEV-944） / `cuda` = CUDA リンク済みビルド配布 + 同配線 / `winml`・`directml`・`npu` = M24（`winml` 統合先。§5.1） |
| `model.selectedPath` | ◯（一般） | — |
| `logLevel` | ◯（詳細） | — |
| `model.*` の残りフィールド（`epPreference`/`nGpuLayers`/`benchmark*`/`autoLoadOnHostStart`/`fallbackToSimpleConverter` 等） | — | M45（モデル管理 UI） |
| `epPreference`（root） / `powerProfile` | — | M24 / M25 |
| `inputMode` | —（実行時にキー操作で切替） | M30（任意） |
| `inputStyle` / `customRomajiTablePath` | — | M17 |
| `liveConversion` | — | M14 |
| `predictionEnabled` | — | M15 |
| `llmMagicConversion` / `aiBackend` / `openAiApiKey` / `openAiApiEndpoint` / `openAiModel` / `includeContextInAITransform` | — | M16（鍵は §9 DPAPI、M34） |
| `promptPrefixByApp` | — | rich X-2-6 / M48（`profilesByApp` 統合） |
| `contextReselection` / `postCommitLint` / `retroactiveRecompute` / `sentenceCompletion`（実験） | — | rich（M30 以降。実験フラグ） |
| `batchRomajiConversion` / `batchRomajiPreviewStyle` / `batchConversionMode` / `batchAutoPunctuation` | — | M58 |
| `autoUpdate.*` | — | M32（v1.0＝M11/M12 より後。一般ペインに UI 化） |
| 予定済み拡張: `privacy.*` / `profilesByApp` | — | M46 / M48（schema 統合は §3.6 拡張方針） |

> v1.0 で UI 化しないキーも schema 正典（§3.6）には残り、`settings.json` 直書きと Host hot-reload で
> 機能自体は動く。v1.0 設定アプリは未露出キーを**消さない**（下記バリデーションの write-back 規則）。

#### 設定アプリ起動時の schema バリデーション

- 設定アプリは起動時に `settings.json` を `settings/mvp-settings.schema.json`（JSON Schema draft 2020-12）で
  検証する。**未知キー / 型不正 / enum 外の値は警告ログ（`logLevel`）に記録して skip し、当該キーは既定値
  （§3.6 拡張方針「欠落キーは schema 既定で補完」）で扱う。**破損キー 1 つで設定アプリやランタイムを
  停止させない（fail-safe）。
- **write-back 規則**: 設定アプリが `settings.json` を保存するとき、**schema 上有効で UI に露出していないキー**
  （v1.x キー・ユーザーが直書きした有効値）を**保持して書き戻す**。v1.0 UI が知らないだけの有効キーを silently に
  消去しない（直書きワークフローを壊さないため）。一方、**起動時バリデーションで弾いた未知キー / 型不正 / enum 外の
  エントリは write-back で温存しない**（quarantine して書き戻さない）。これらを温存すると、`additionalProperties: false`
  の固定オブジェクト（§3.6）に対して Host 再読込が `ok=false`（§3.3）になり、UI 側の有効な変更まで Host に拒否されるため。
  すなわち保持対象は「schema-known かつ UI-hidden」に限る。書き込みは破損耐性のため atomic write（一時ファイル → rename、§3.6 永続化）。
- Host 側の再読込時バリデーション（無効なら `UpdateConfigResponse.ok=false` + `error`、runtime 設定維持）は
  §3.3 を正典とする。本節は**設定アプリ側の起動時検証**を補い、二重定義しない。

## 4. WiX / MSI インストーラ（MVP 既定 / 旧 M31）

> **スコープ注記（§0 配布方針）**: 本節の **WiX MSI が v1.0 MVP の既定配布形態**（未署名、DEV-415）。
> 当初は MSIX 不可環境（Win10 LTSC, 法人ポリシーで AppX 無効）向けの代替として位置づけていたが、
> 配布方針転換により MVP の主経路へ格上げした。MSIX は §1（MS Store 用）に回す。MSI は署名が任意で、
> 未署名でもインストール可能（SmartScreen 警告 + UAC「不明な発行元」は出る）。

MSIX 不可環境（Win10 LTSC, 法人ポリシーで AppX 無効）にも本経路で対応する。

> **設定アプリと WinUI 3 ランタイムの同梱**:
> `azookey_settings` target は unpackaged の設定アプリを self-contained でビルドする。
> MVP MSI は、その出力フォルダから `azookey_settings.exe` と WinUI 3 ランタイムを回収する。
> MSIX フレームワーク依存が使えない LTSC 等では、設定アプリを
> **self-contained 配置（§1.3）でビルドし、その出力フォルダ一式を配置する**。
> self-contained を採らない場合は `WindowsAppRuntimeInstall.exe` をペイロードに含めて
> インストール時に実行する。TIP DLL / Host は WinUI 3 非依存（§1.3）。

### 4.1 WiX 構成

`pkg/msi/Package.wxs` と `pkg/msi/azooKey.wixproj` を正典とする。
WiX Toolset は MSBuild SDK の 5.0.2 に固定し、x64 の per-machine MSI を生成する。
配置先は `%ProgramFiles%\azooKey` とする。

```xml
<?xml version="1.0" encoding="UTF-8"?>
<Wix xmlns="http://wixtoolset.org/schemas/v4/wxs">
  <Package Name="azooKey"
           Manufacturer="dolquis"
           Version="$(ProductVersion)"
           UpgradeCode="..."
           Scope="perMachine">
    <StandardDirectory Id="ProgramFiles64Folder">
      <Directory Id="INSTALLFOLDER" Name="azooKey">
        <Component Id="TipComponent" Guid="...">
          <File Id="TipDll"
                Source="$(TipDllPath)"
                Name="azookey_tsf_tip.dll"
                KeyPath="yes" />
        </Component>
        <Component Id="HostComponent" Guid="...">
          <File Source="$(HostExePath)"
                Name="azookey_inference_host.exe"
                KeyPath="yes" />
        </Component>
      </Directory>
    </StandardDirectory>

    <CustomAction Id="RegisterTip"
                  Directory="INSTALLFOLDER"
                  ExeCommand="&quot;[System64Folder]msiexec.exe&quot; /y &quot;[#TipDll]&quot;"
                  Execute="deferred"
                  Impersonate="no"
                  Return="check" />
    <CustomAction Id="UnregisterTip"
                  Directory="INSTALLFOLDER"
                  ExeCommand="&quot;[System64Folder]msiexec.exe&quot; /z &quot;[#TipDll]&quot;"
                  Execute="deferred"
                  Impersonate="no"
                  Return="check" />
  </Package>
</Wix>
```

ファイル配置後に `msiexec /y [#TipDll]` を実行し、インストール済み DLL の
`DllRegisterServer` に COM クラス、TSF プロファイル、カテゴリの登録を委譲する。
Binary table に格納した DLL を直接呼ぶ方式は採らない。
`DllRegisterServer` が `GetModuleFileNameW` で自身のパスを登録するため、一時展開した
custom-action DLL を呼ぶと `InprocServer32` が一時パスを指してしまうからである。

登録と解除は、いずれも deferred、`Impersonate="no"`、`Return="check"` とする。
インストール時は `InstallFiles` 後に登録し、失敗時は rollback action で解除する。
アンインストール時は `RemoveFiles` 前に解除し、失敗時は rollback action で再登録する。
Major Upgrade は `afterInstallInitialize` で旧版を先に削除し、旧版の解除後に新版を登録する。

base MSI には TIP、Inference Host、`LICENSE`、`THIRD_PARTY_LICENSES` に加え、
両バイナリが直接依存する
`msvcp140.dll`、`vcruntime140.dll`、`vcruntime140_1.dll` を app-local で同梱する。
これらはビルドに使用した MSVC toolset の x64 `Microsoft.VC*.CRT` から取得し、
TIP と Host と同じ `%ProgramFiles%\azooKey` へ配置する。これにより、
VC++ Redistributable が未導入のクリーンな Windows 11 でも起動可能にする。
GGUF モデルは初回取得、CUDA runtime は optional add-on とし、base MSI へ含めない。
設定アプリは `SettingsPayloadDir` を必須入力として self-contained 出力一式を同梱する。
`SettingsExePath` は実行ファイルの取得元パスであり、既定値は
`$(SettingsPayloadDir)\azookey_settings.exe` とする。インストール後の名前は常に
`INSTALLFOLDER\azookey_settings.exe` で、スタートメニューのショートカットはこの固定パスを
指す。ショートカットは `SettingsExe` の advertised shortcut とし、per-machine のファイルを
component KeyPath に保つ。release workflow は `azookey_settings` target をビルドしてから両
property を MSI ビルドへ渡す。

### 4.2 アンインストール時

`RemoveFiles` の前に `msiexec /z [#TipDll]` で `DllUnregisterServer` を呼ぶ。
TIP DLL を削除した後では解除できないため、この順序を変更しない。

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

### 4.4 SBOM と build provenance（M38 供給網固定）

`release.yml` はタグ push 時に、MSI へ次の 2 つを付与する。

| 生成物 | 手段 | 添付先 |
|---|---|---|
| SBOM（SPDX JSON） | syft の出力を `scripts/complete-release-sbom.py` で補完し `actions/attest` の `sbom-path` で署名 | Draft Release の資産 `azooKey.spdx.json` + SBOM attestation |
| build provenance | `actions/attest`（`sbom-path` も predicate も渡さない既定モード） | GitHub の attestation ストア |

利用者は `gh attestation verify <msi> --repo dolquis/azooKey-Desktop` で、その MSI が
本リポジトリの `release.yml` から生成されたことを検証できる。

`actions/attest-build-provenance` と `actions/attest-sbom` は v4 で `actions/attest` の
wrapper になっており、新規実装は `actions/attest` を使う。本 workflow もそれに従う。

#### この SBOM が保証する範囲

syft の検出結果に、MSI の SHA256 と次のビルド入力を追加する。各依存は MSI package
から `DEPENDS_ON` で参照する。これは canonical Release ビルドの依存宣言であり、
バイナリ内部の全コンポーネントを自動検出した結果ではない。

- llama.cpp / WIL: `CMakeLists.txt` のフル SHA を読み、CMake cache と取得した Git
  checkout の HEAD・追跡ファイルの変更有無を照合する。
- MSVC runtime: MSI へ渡す 3 DLL それぞれの数値ファイルバージョンと SHA256 を記録する。
  runner の Visual Studio 更新に追随するため、固定バージョンを別の台帳へ転記せず、
  そのビルドで選択したファイルをハッシュで同定する。
- Windows App SDK、C++/WinRT、WebView2 loader: `settings-app/packages.lock.json`
  の解決バージョンと NuGet package の SHA512 を使う。restore による lock の変更は拒否する。

依存と SPDX ライセンス識別子の対応は `THIRD_PARTY_LICENSES` の `sbom` 注記から生成する。
NuGet lock の未分類依存、対応する attribution の欠落、pin や CRT ハッシュの不一致は
生成エラーとする。CI は正常系と不一致時の拒否を回帰テストする。`LicenseRef` の本文は
同ファイルの attribution 節であり、Microsoft のライセンス条項そのものを置き換えない。

GoogleTest はテスト専用、Windows SDK BuildTools はビルド専用として配布依存から除外し、
理由を SPDX annotation に残す。GGUF モデルと CUDA runtime も基本 MSI には同梱しない。

同梱・依存する第三者資産を列挙する正典は、引き続きルート `THIRD_PARTY_LICENSES`
（運用規約は `docs/licensing-policy.md`）である。SBOM をもって attribution の確認を
代替しない。本節は `docs/licensing-policy.md` の SBOM 案内から参照される。

#### attestation を dry-run で作らない理由

attestation はリポジトリに残る公開記録である。`workflow_dispatch` によるビルド確認
（§2.3）で attestation を作ると、配布していないビルドに対する証明が残り、検証者に
誤解を与える。このため attest ステップは `github.ref_type == 'tag'` で限定する。
SBOM 生成自体は dry-run でも実行し、ステップの破損を早期に検出する。

## 5. WinGet マニフェスト（M32）

> **スコープ注記（§0 配布方針）**: WinGet が配布するのは **MVP の未署名 MSI**（DEV-415）。
> Release に MSIX は含まれないため `InstallerType` は `msi`。MS Store 配布（DEV-416）は Store が
> 更新を担うため、本節の WinGet / §6 自動更新は Store チャネルには適用しない。

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
InstallerType: msi
Installers:
  - Architecture: x64
    InstallerUrl: https://github.com/dolquis/azooKey-Desktop/releases/download/v1.0.0/azooKey-1.0.0-x64.msi
    InstallerSha256: <SHA256>
  - Architecture: arm64
    InstallerUrl: https://github.com/dolquis/azooKey-Desktop/releases/download/v1.0.0/azooKey-1.0.0-arm64.msi
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

1. GitHub Release で MSI を公開
2. SHA256 を計算
3. winget-pkgs リポジトリへ PR
4. マージ後 `winget install dolquis.azooKey` で利用可能

`wingetcreate` で半自動化：

```powershell
wingetcreate update --urls `
  "https://github.com/dolquis/azooKey-Desktop/releases/download/v$ver/azooKey-$ver-x64.msi" `
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
        std::string url;        // MSI installer ダウンロード URL
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
3. MSI を `%TEMP%` にダウンロード（`WinHttpReadData`）
4. SHA256 検証
5. `msiexec /i <msi> /qn /norestart` で更新（同一 `UpgradeCode` により in-place アップグレード。要昇格）。
   使用中の TIP DLL 置換で再起動要求が起き得るため、`/norestart` で自動再起動を抑止する
   （[Standard Installer Command-Line Options](https://learn.microsoft.com/windows/win32/msi/standard-installer-command-line-options)）
6. 終了コード `3010`（`ERROR_SUCCESS_REBOOT_REQUIRED`）の場合は、自動再起動せず「再起動が必要」を
   ユーザーに通知して同意を得てから再起動する

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
| 5000 | LearningObserve | reading_len, surface_len |
| 5001 | LearningForget | reading_len, surface_len |
| 9000 | Error | source, error_code, hr |

> 上表のフィールドは長さ・件数・enum・数値・GUID・ID のみで構成し、入力本文を
> 含めない。本文を載せない理由と禁止対象は §7.2.1 を正典とする。

#### 7.2.1 本文を ETW に載せない（redaction 規律）

ETW は `docs/dev-infrastructure-spec.md` §7.6 の redaction ポリシー正典が適用
される観測経路である。ETW トレース（`.etl`）は WPA で誰でも閲覧でき、secure /
`privacy.redactLogs` の状態に応じて**事後にフィルタできない**（消費側に redaction
ゲートが無い）。そのため本文系フィールド（`reading` / `surface` / `candidate.text` /
確定文字列 / Magic Conversion prompt / typo の `raw_keys` 等）は **build / env / mode に
よらず一切 ETW へ書き込まない**。構造化ログ（`logs/*.jsonl`）が §7.6 優先順位 4 の
opt-in で本文を出し得るのとは異なり、ETW は本文出力経路を持たない。

- 旧 §7.2 の `5000/5001 LearningObserve/Forget` は `reading, surface`（= 入力本文）を
  載せていたが §7.6 違反のため廃止する。代わりに本文長のみを `reading_len` /
  `surface_len`（整数）で記録し、内容は復元できないようにする。
- `9000 Error` は自由文 `message` を載せず、`source`（`Module` enum で符号化したモジュール
  識別子）/ `error_code`（`docs/dev-infrastructure-spec.md` §7.4 の 3 カテゴリ enum）/
  `hr`（HRESULT）のみとする。例外メッセージ等の自由文は §7.6 を適用済みの構造化ログ側へ
  出し、ETW には載せない。
- `EtwLogger`（§7.3）は本文型の引数を受ける API を**持たない**。長さ・件数・enum・
  数値・GUID・ID のみを受ける型シグネチャに限定し、本文混入をコンパイル時に防ぐ。
  `source` も `const char*` ではなく `Module` enum とし、呼び出し側が任意 / ユーザー由来の
  文字列を渡せる引数を一切残さない。
- ETW は本文を含まないメタ情報のみのため、レイテンシ trace（§7.6 redact 対象外）と
  同じく追加の同意を要さない。WPR トレース（`.etl`）の採取・共有はユーザー / 開発者の
  明示操作（§7.4）であり、その成果物にも本文は含まれない。

### 7.3 ラッパ

`core/src/EtwLogger.cpp`（新規）：

```cpp
// Module / ErrorCode は閉じた enum。文字列引数を一切持たせず、本文混入を
// コンパイル時に防ぐ（§7.2.1）。ErrorCode は dev-infrastructure-spec.md §7.4 の
// 3 カテゴリ（transport / protocol / business）。§7.2 9000 Error と同一。
enum class Module { Tip, Host, Settings };
enum class ErrorCode { Transport, Protocol, Business };

class EtwLogger {
public:
    static void Register();
    static void Unregister();
    static void LogActivate(...);
    static void LogIpcRequest(...);
    // 自由文字列は受けない。source は Module enum、code は ErrorCode enum、
    // hr は HRESULT。例外メッセージ等の自由文は ETW に載せず、§7.6 適用済みの
    // 構造化ログへ出す（§7.2.1）。
    static void LogError(Module source, ErrorCode code, HRESULT hr);
};
```

`EventRegister` + manifest 定義イベントの `EventWrite`（または TraceLogging）で
**型付きフィールド**を書き込む薄いラッパ。任意文字列を書く `EventWriteString` は
本文混入経路になるため**使わない**（§7.2.1）。各 `Log*` は §7.2 表のフィールド
（長さ・件数・enum・数値・GUID・ID）のみを型付き引数で受け、本文型引数を持たない。
全モジュール（TIP/Host/Settings）から呼べるよう `core/` 配下に置く。

### 7.4 観測

`wpr -start GeneralProfile -filemode` + `wpa.exe` で開発者がトレース可能。
本番ユーザーは ETW を意識せず、問題報告時に WPR トレースを取ってもらう。

## 8. WER（Windows Error Reporting、M33）

### 8.1 MiniDumpWriteDump

```cpp
// ダンプ種別: 本文（入力バッファ・候補・学習データ・API キー）を取り込みやすい
// データセグメント / フルメモリ / ヒープは含めず、原因解析に要る最小集合に限定する。
// 詳細な根拠と禁止フラグは §8.3 を正典とする。
constexpr MINIDUMP_TYPE kAzooKeyDumpType = static_cast<MINIDUMP_TYPE>(
    MiniDumpNormal |                  // スタック + モジュール一覧（最小）
    MiniDumpWithThreadInfo |          // スレッド状態（本文を含まない）
    MiniDumpWithUnloadedModules |     // アンロード済みモジュール
    MiniDumpIgnoreInaccessibleMemory);

LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS* ep) {
    // crashReportConsent=off: azooKey のダンプを書かず OS 既定処理（既定の
    // UnhandledExceptionFilter = WER 等）へ委ねる。次フィルタ / 既定へ進める
    // 戻り値は EXCEPTION_CONTINUE_SEARCH（§8.3）。
    if (!CrashReporting::DumpAllowed()) return EXCEPTION_CONTINUE_SEARCH;

    // 保存先は %LOCALAPPDATA%\azooKey\crashes\ に統一（§8.2）。GetTempPath は使わない。
    // NextDumpPath() はディレクトリの存在を保証してからパスを返す（起動時にも作成済み。
    // 新規プロファイル / アンインストール後の掃除で親ディレクトリが無くても dump を失わない。§8.2）。
    std::wstring path = CrashReporting::NextDumpPath();  // azookey-<module>-<UTCstamp>-<pid>.dmp

    bool wrote = false;
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{
            GetCurrentThreadId(), ep, FALSE
        };
        wrote = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
                                  hFile,
                                  kAzooKeyDumpType,
                                  &mei, nullptr, nullptr);
        CloseHandle(hFile);
    }
    CrashReporting::RotateDumps();  // 保持上限を適用（§8.2）
    // 自前 dump を書けたときのみ EXCEPTION_EXECUTE_HANDLER（OS 既定 WER の二重ダンプを避け
    // プロセス終了へ）。ディレクトリ欠落・書き込み失敗で dump を残せなかったときは OS 既定
    // フォールバックを潰さないよう EXCEPTION_CONTINUE_SEARCH を返す（§8.2）。
    return wrote ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}

// main で
SetUnhandledExceptionFilter(UnhandledFilter);
```

Host / Settings は明示的に `SetUnhandledExceptionFilter` で設定。
TIP DLL は in-proc なのでアプリ側を巻き込まないよう **設定しない**。
代わりに `__try`/`__except` で IPC ループ等のスコープを囲む。

### 8.2 保存先・保持・削除運用

- **保存先**: `%LOCALAPPDATA%\azooKey\crashes\`。ファイル名は
  `azookey-<module>-<UTC: yyyyMMddTHHmmssZ>-<pid>.dmp`（`<module>` は `host` /
  `settings`）。`GetTempPath` 直下への書き込み（旧 §8.1）は廃止し、保存先を一本化する。
- **ディレクトリ作成（必須）**: クラッシュディレクトリは **起動時**（`CrashReporting` 初期化 /
  `SetUnhandledExceptionFilter` 設定時）に作成し、`NextDumpPath()` も呼び出しのたびに存在を
  冪等に保証する（`SHCreateDirectoryEx` 等で再帰作成）。新規プロファイルやアンインストール後の
  掃除で親ディレクトリが無い状態でも初回 dump を失わないため。ディレクトリ欠落・書き込み失敗で
  自前 dump を残せなかった場合、`UnhandledFilter` は `EXCEPTION_CONTINUE_SEARCH` を返して OS 既定
  フォールバックを潰さない（§8.1。自前 dump 取得時のみ `EXCEPTION_EXECUTE_HANDLER`）。
- **保持上限（ローテーション）**: 既定で **最新 5 個 / 合計 50 MB / 30 日**を上限とし、
  いずれかを超えた古いダンプから削除する。`module` をまたいだ全体集合に対して適用する。
- **適用タイミング**: Host / Settings 起動時に一度、および各ダンプ書き込み直後
  （`RotateDumps()`）に実行する。クラッシュ時の `UnhandledFilter` 内処理は最小限に保つ。
- **アンインストール時**: MSIX（§1.4）・手動アンインストール経路ともに当該ディレクトリを
  削除し、ダンプを残骸として残さない（`compat-test` の残骸 0 smoke 対象に含める）。

### 8.3 ダンプ内容の最小化と同意（redaction / consent）

クラッシュダンプはメモリ断片を含むため、入力本文・候補・学習データ（`reading` /
`surface`）・OpenAI API キーを意図せず取り込み得る。これを設計原則
（`docs/privacy-and-secure-input-spec.md` §2「ローカル完結」「明示同意なしにクラウド
送信しない」「fail closed」）に沿って最小化する。

- **ダンプ種別の最小化（§8.1）**: `MiniDumpWithDataSegs`（データセグメント = グローバル /
  静的バッファ）・`MiniDumpWithFullMemory`・`MiniDumpWithProcessThreadData`・
  `MiniDumpWithHandleData` は**使わない**。これらは入力バッファ・学習データ・API キーを
  取り込む可能性が高い。スタックメモリには確定前の入力断片が残り得るが、原因解析に必須の
  ため許容し、データセグメント / ヒープ / フルメモリの全面取り込みは行わないことで露出面を
  抑える。
- **同意キー** `privacy.crashReportConsent`（enum、既定 `local`。schema 正典は
  `docs/privacy-and-secure-input-spec.md` §7）:
  - `off` — azooKey 管理のダンプ（§8.2 の `%LOCALAPPDATA%\azooKey\crashes\`）を書かない。
    `UnhandledFilter` は `DumpAllowed()==false` のとき **`EXCEPTION_CONTINUE_SEARCH`** を返して
    既定の `UnhandledExceptionFilter` へ進める（`EXCEPTION_EXECUTE_HANDLER` は WER を迂回して
    プロセス終了へ進む値であり、IME がマシンの障害処理を上書きするのは不適切なため用いない）。
    なお自前 dump を書いた経路は二重ダンプ回避のため `EXCEPTION_EXECUTE_HANDLER` を返す（§8.1）。
    - **同意スコープの明確化**: `crashReportConsent` は **azooKey 管理ダンプの生成可否のみ**を
      制御する。OS の WER / LocalDumps はマシン全体のポリシー（管理者 / ユーザーの OS 設定）で
      あり azooKey の同意スコープ外とする。azooKey は **自身向けの WER LocalDumps 登録を行わず**
      （`HKLM/HKCU\...\Windows Error Reporting\LocalDumps\<exe>` を作らない）、OS 既定を有効化も
      強制無効化もしない。したがって `off` でも、既にマシンに WER / LocalDumps ポリシーが
      設定されていれば OS 側ダンプは生じ得るが、それは azooKey 管理外であり本同意の対象外。
      `off` が保証するのは「azooKey が自前ダンプを書かない／自前の WER 収集を仕込まない」こと
      である。
  - `local` — `%LOCALAPPDATA%` 配下に保存のみ。自動送信は一切しない（Phase 7 の実装範囲・
    既定）。
  - **送信（upload）は M33 schema に含めない**。送信経路は Phase 7 では未実装で、
    `crashReportConsent` の enum は `off` / `local` のみとする。送信機能は将来 M で実装する際に、
    **バージョン / タイムスタンプ付きの明示同意レコード**として別途追加する（bare な enum 値
    `"upload"` を先行して永続化しない）。理由: M33 で `upload` を enum に入れて永続化可能にすると、
    設定 UI / 管理者 / `settings.json` 直書きで送信経路実装前に `crashReportConsent: "upload"` が
    残り、将来の送信実装時に「新規の明示同意」と区別できず one-shot 再同意保証を迂回し得るため。
    ローダは未知値を `local` に正規化する（前方互換は §3.6 拡張方針）。
- **送信は常に明示操作**: 自動アップロード・GitHub Issue 自動起票は行わない。ユーザーは
  設定アプリ「詳細 → クラッシュレポート」から、対象ダンプを選んで手動でアーカイブ・添付
  する（§8.2 の保存ディレクトリを開く）。
- **要約の redaction**: M44 診断 ZIP の `crash-summary.txt` は
  `docs/dev-infrastructure-spec.md` §12.5 のとおり **WER ダンプの要約のみ**を含め、
  ダンプ本体・スタック上の文字列バッファを含めない。本書 §8 と §12.5 は同一方針とする。

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
| MSIX install/update/rollback/uninstall | `compat-test/msix_install_uninstall.ps1` | Windows 限定・実機 VM（`gate:human-required`）。旧版 install → CLSID / TSF プロファイル登録の存在確認 → 新版 update → deployment 中に失敗する update 後の直前バージョン保持 → uninstall → 残骸 0 を半自動検証（DEV-101 / DEV-586 / M28） |
| MSIX identity 宣言の整合 | `scripts/tests/msix-identity-consistency.Tests.ps1` | OS 非依存（CI の PowerShell lint/test ジョブ）。identity manifest と app 側 `<msix>` 3 属性の一致、Option A の不変条件、smoke ハーネス既定値と `kTextServiceClsid` / `kTextServiceProfileGuid` / `kJapaneseLangId` の一致、ビルド埋め込み配線を検証（§1.1.2） |
| MSIX update / rollback ハーネス契約 | `scripts/tests/msix-lifecycle-scenarios.Tests.ps1` | OS 非依存（CI の PowerShell lint/test ジョブ）。update package のペア指定、clean VM 前提、package family / Version 比較、失敗 update と rollback 判定の順序、Add ごとの導入 package 追跡、早期中断時の report 出力を AST で検証（§1.1.1） |
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
5. Draft Release が作成される（MVP: 未署名 MSI 添付。配布方針は §0。MS Store 配布は Partner Center 経由で別手順 = DEV-416。スタンドアロン MSIX 署名は §2、当面延期）
6. SBOM（`azooKey.spdx.json`）が Release 資産に添付され、`gh attestation verify <msi> --repo dolquis/azooKey-Desktop` が成功することを確認（§4.4）
7. 動作確認（クリーン VM でインストール → 入力 → 確定 → アンインストール）
8. Draft → Publish
9. winget-pkgs に PR（`wingetcreate update`）

## 12. 参照

- MSIX SDK: <https://learn.microsoft.com/windows/msix/desktop/source-code-overview>
- signtool: <https://learn.microsoft.com/windows/win32/seccrypto/signtool>
- DPAPI: <https://learn.microsoft.com/windows/win32/api/dpapi/>
- ETW: <https://learn.microsoft.com/windows/win32/etw/>
- WiX v4: <https://wixtoolset.org/docs/intro/>
- WinGet manifest: <https://github.com/microsoft/winget-cli/blob/master/doc/ManifestSpecv1.5.md>
- 既存：`scripts/register-dev.ps1` / `scripts/unregister-dev.ps1`（`regsvr32` 開発用経路）
- ベース：`docs/windows-tsf-host-architecture.md`
