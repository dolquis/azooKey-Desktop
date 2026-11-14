---
kind: spec
milestones: [M28]
canonical_for: ["MSIX パッケージング"]
---

# MSIX パッケージング 仕様（MS Store 向け・M28）

対象リポジトリ: dolquis/azooKey-Desktop
対応マイルストーン: M28（§1.6 は M24 と連動）
関連: `docs/sideload-packaging-spec.md`（配布方針 §0、署名 §2、WiX / MSI §4）、
      `pkg/msix/README.md`（Option A の identity package の手順）、
      `plans/windows-port-roadmap.md` M28

本書は `docs/sideload-packaging-spec.md` から §1（MSIX パッケージング）を切り出した文書である。
節番号 §1.x は旧 spec のまま保ち、旧 spec の転送表が `§1` を本書へ解決する。
本書内で配布方針・署名・WiX / MSI などの旧 spec の節を指すときは `sideload-packaging-spec §N` と書く。

## 正典範囲

TIP の配布経路の 3 候補（Option A / B / C）、AppxManifest の要件、`pkg/msix/` の identity package、
VCRT 同梱、インストールとアンインストール、経路ごとの対象 OS、推論バックエンドとモデルの配布方針（§1.6）、
TIP のロード前提と Host の常駐起動（§1.7）は本書が正典である。配布チャネルの選定と署名は
`docs/sideload-packaging-spec.md` が定める。

## 節とマイルストーンの対応

| 節 | マイルストーン |
|---|---|
| §1.0〜§1.5 | M28 |
| §1.6 | M28（M24 と連動） |
| §1.7 | M28 |

## 1. MSIX パッケージング（MS Store 向け・M28）

**現行の `pkg/msix/` の位置づけ**: `pkg/msix/` が持つのは Option A（external-location の identity package）だけである。
`AppxManifest.xml` は `uap10:AllowExternalContent=true`・`com4` 宣言なし・`TargetDeviceFamily@MinVersion` 10.0.19041.0 で、
`Application` は `azooKeyHost`（`azookey_inference_host.exe`）の 1 件だけを宣言する。`Package.wapproj` も identity のみをパッケージ化する。
Option B / C（バイナリ同梱の通常 MSIX）は設計候補であり、`pkg/` に manifest も wapproj も無い。§1.1 と §1.2 の同梱物の記述は B / C 向けである。

> **スコープ注記（`sideload-packaging-spec` §0 配布方針）**: 本節の MSIX は **MS Store 配布（DEV-416）の構成**として読む。
> Store 提出パッケージは Microsoft が再署名するため `sideload-packaging-spec` §2 の自前署名は不要。有料署名を要する
> スタンドアロン MSIX サイドロードは延期する（`sideload-packaging-spec` §0。DEV-255）。MVP の直接配布は `sideload-packaging-spec` §4（WiX/MSI、DEV-415）が正典。

> **⚠️ OPEN ISSUE — M28 着手時に PoC 必須**: MSIX に TIP DLL を同梱する経路は、
> Microsoft 公式仕様上**機能制限あり**の領域である。`com4:InProcessServer`
> は「外部位置（external location / sparse package）向け」と明記されており、
> 通常の `.msix` では install location ACL により **外部クライアント（ctfmon
> 等）が TIP DLL を読み込めない**（[com4:Extension Examples](https://learn.microsoft.com/uwp/schemas/appxpackage/uapmanifestschema/element-com4-extension#examples)）。本書 §1.1 の `com4:InProcessServer` 構成は
> 参考であり、M28 は下表のいずれかを選ぶ。選定は Linear
> [DEV-101](https://linear.app/dolquis/issue/DEV-101) で追跡する。

### 1.0 TIP 配布経路 3 候補

| 経路 | DLL ロード可否 | 配布形態 | OS 要件 | 評価 |
|---|---|---|---|---|
| A. **external-location packaging (sparse package)** + 既存 `regsvr32` 経路 | ◎ 外部から in-proc ロード可 | sparse manifest + 自前 installer（or xcopy） | Win10 2004 (19041) / Win11 21H2+ | 推奨候補。TIP DLL を MSIX 外に置けば従来の `InprocServer32` レジストリで動く |
| B. **通常 `.msix` + `com4:InProcessServer`** | ✗ ACL でブロックされる可能性大 | フル MSIX | Win10 21H2 server (20348+) / Win11 21H2+ | Microsoft が「外部位置向け」と明記。通常 MSIX では動かない事例あり |
| C. **通常 `.msix` + `com4:SurrogateServer`**（[ClassReference Remarks](https://learn.microsoft.com/uwp/schemas/appxpackage/uapmanifestschema/element-com4-inprocessserverclassreference#remarks) が推奨） | △ out-of-proc 経路。TIP の標準的 in-proc 活性化と semantics が異なる | フル MSIX | Win10 21H2 server / Win11 21H2+ | runFullTrust 下で COM 活性化は確実だが、TIP 動作と整合するか要検証 |

> 補足: D ルートとして **WiX/MSI installer** で従来の `regsvr32` 経路（`sideload-packaging-spec` §4）を
> 使う選択肢もある。MSIX 不可環境（LTSC 等）向けには結局 `sideload-packaging-spec` §4 を持つので、
> v1.0 は `sideload-packaging-spec` §4 / WiX で確定 + §1 は M28 PoC へ送る選択もあり得る。

### 1.1 AppxManifest.xml（Option B の参考例）

Option B（通常 MSIX + `com4:InProcessServer`）を採る場合の manifest 要件を、schema
validation の観点でまとめる。**この構成だけではアクティベーションが成立しない可能性**を
§1.0 で示したため、M28 着手時に Option A への切替を含めた PoC で確定する。Option A の
具体（`pkg/msix/AppxManifest.xml` が現物）は §1.1.2 にある。本項は manifest 全文を持たない。

- `com4:Extension` / `com4:InProcessServer` は **Win10 build 20348+** を要求するので、
  `TargetDeviceFamily@MinVersion` を `10.0.20348.0` まで引き上げる。Win10 22H2
  （build 19045）では schema validation で reject される。
- `IgnorableNamespaces` に `com4` を追加する（namespace は
  `http://schemas.microsoft.com/appx/manifest/com/windows10/4`）。
- `com4:Class` の `Virtualization` は**必須属性**である（`enabled` / `disabled`。classic COM
  互換のため `disabled`）。`com4:Class` に `Path` 属性は無く、DLL パスは
  `com4:InProcessServerDll`（`Path` + `ProcessorArchitecture` とも必須）で与える。
- `com4:Class` の `ThreadingModel` の許容値は `Both` / `STA` / `MTA` / `MainSTA` / `Neutral`
  である（`Apartment` はクラシックレジストリ値で MSIX schema では invalid）。TIP は TSF の
  standard STA で動作する。
- `Application` は `runFullTrust` + `EntryPoint="Windows.FullTrustApplication"` とし、Host の
  起動は `uap3:AppExecutionAlias` で与える。
- TSF Profile 登録は MSIX manifest では扱わない（`windows.inputMethod` は `uap3:Extension` の
  許容 Category に存在しない）。runtime に `ITfInputProcessorProfiles::Register` を呼ぶ。
  詳細は本節下記「TSF Profile 登録のライフサイクル」を参照。

`com4:Class@Id`（CLSID）と Profile GUID は `tsf-tip/src/DllMain.cpp` の `kTextServiceClsid` /
`kTextServiceProfileGuid` と一致させる。

> **署名 cert を入れたら `Publisher` を必ず差し替える**: dev cert 用の値
> （`Publisher="CN=dolquis"`）のままだと、OV/EV cert で署名した場合に
> `Identity@Publisher` が署名証明書の Subject DN と一致せず `Add-AppxPackage` が
> `0x8007000B` で失敗する。具体手順は `sideload-packaging-spec` §2.2「Publisher と証明書 Subject の整合（必須）」を参照。

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
  契機が無く、roadmap M28 の受け入れ条件（アンインストールで登録の残骸が消え、言語バーに
  死んだエントリが残らないこと）を自動で満たせない。M28 PoC で次のいずれかを確定する:
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
  roadmap M28 の受け入れ条件を満たせない。identity
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
external location（`sideload-packaging-spec` §4 WiX/MSI の install ディレクトリ）に置き、COM 登録は
`scripts/register-dev.ps1` の `regsvr32` 経路で行う。**本 PoC は Option A を具体化するが経路の
最終確定ではない**（確定は DEV-267 の実機 smoke 結果を待つ）。identity package は MS Store
提出物ではなくサイドロード登録専用で、`sideload-packaging-spec` §0 の配布方針（MVP=MSI/WiX `sideload-packaging-spec` §4、MS Store MSIX=DEV-416、
スタンドアロン MSIX サイドロード延期）を変更しない。

### 1.2 Package.wapproj

> **Option A では本項の「同梱物」は同梱しない**: 以下は Option B/C（バイナリ同梱の通常 MSIX）
> 向けの記述。Option A（sparse / external-location）ではバイナリを MSIX に含めず external
> location へ置くため、`pkg/msix/AppxManifest.xml`（identity のみ）+ `build-identity-package.ps1`
> を使う（§1.1.2）。`pkg/msix/Package.wapproj` は Option A では identity のみをパッケージ化する。

`pkg/msix/Package.wapproj`（WAP プロジェクト）：

- ターゲット: x64 / arm64
- 同梱物:
  - `azookey_tsf_tip.dll`
  - `azookey_inference_host.exe`
  - `azookey_settings.exe`（M11 で最小版を同梱 / M30 でフル UI 化。`settings-app-spec` §3.0）
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

設定アプリ `azookey_settings.exe` は WinUI 3（`settings-app-spec` §3.0）のため **Windows App SDK ランタイム**に
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
  の配信をインストーラ手順（`sideload-packaging-spec` §4 / §5）に含める。
  ```xml
  <!-- framework package family は major 版に揃う（WASDK 2.0 SemVer 移行）。NuGet/版が
       2.2.0 でも family は Microsoft.WindowsAppRuntime.2.0 のまま（".2.2" は存在せず、
       clean VM で解決失敗する）。MinVersion は採用する WASDK 版（`settings-app-spec` §3.1）に対応する
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

解除前の入力方式の切替（`azookey_ime_switch.exe`）は MSI と開発登録の解除手順に含め、
MSIX の `Remove-AppxPackage` には含めない。手順は `sideload-packaging-spec` §4.2 に従う。

### 1.5 経路ごとの対象 OS

受け入れ条件の定義の正典は [`plans/windows-port-roadmap.md`](../plans/windows-port-roadmap.md)
の M28 節とする。本節は受け入れで検証する OS を経路ごとに定義し、受け入れ条件を複製しない。

OS ターゲットは §1.0 で選定する経路に依存する。同 PoC で 1 経路に確定したら、
当該経路の行の OS で roadmap M28 の受け入れ条件を満たす。

| 経路 | Win11 23H2 | Win10 22H2 (build 19045) | Win Server 2022 (build 20348+) |
|---|---|---|---|
| A. external-location packaging | ✅ 必須 | ✅ 必須（`com4:InProcessServer` を使わないため build 19045 で動く） | （対象外） |
| B. 通常 MSIX + `com4:InProcessServer` | ✅ 必須 | ✗ schema reject（com4 が build 20348+ を要求） | ✅ |
| C. 通常 MSIX + `com4:SurrogateServer` | ✅ 必須 | ✗ 同上 | ✅ |

> Win10 22H2 必須を維持するなら **Option A 確定が前提**。Option B/C を選ぶ場合
> は上表から Win10 22H2 を外し、Win Server 2022 ベースに置換する。

### 1.6 推論バックエンド / EP / モデルの配布方針（M24 連動）

推論バックエンドの選定は `docs/copilot-pc-backend-spec.md` §4 が正典。配布形態の
決定のみを以下に固定する（同 §4.5 から参照）。
R2 向けの行は R2 再開時（`docs/copilot-pc-backend-spec.md` §4.3 項 2）に適用し、再開前は R1 向け構成だけを配布する。

| 構成要素 | 配布形態 | 理由 |
|---|---|---|
| llama.cpp（R1）CPU ランタイム | **base MSIX に同梱** | v1.0 既定エンジン。バイナリは小さい |
| zenz-v3 GGUF モデル本体 | **MSIX 非同梱**（初回起動時 DL。配信元は §1.6.1） | サイズ過大。§1.2 の既存方針に従う。v1.0 最小取得経路は §1.6.1（CC-BY-SA-4.0 として扱う保守運用・上流 HF から取得。§1.6.2） |
| kanakanji-edge-33m Q8_0 GGUF（選べるモデル。既定ではない） | **MSIX 非同梱**（取得元は上流 HF の固定した revision で、再ホストしない。ピンは §1.6.2）。既定の catalog（§1.6.1）に選べる entry として載り、`azookey_inference_host models fetch kanakanji-edge-33m` で取得できる。取得後は `model.selectedPath` で選び、プロンプト形式は語彙から推定する（`docs/zenzai-inference-spec.md` §2.4.2） | 既定は zenz-v3 のまま（`docs/zenzai-inference-spec.md` §2.4.3）。ライセンスの結論は §1.6.2（DEV-1721） |
| Windows ML bootstrap（R2 用 ORT GenAI WinML） | **base MSIX に同梱（薄い）** | EP 本体は含めない |
| Windows ML EP（QNN / OpenVINO / VitisAI / NvTensorRtRtx 等） | **非バンドル（Windows Update 配信）** | Microsoft 推奨。MSIX 肥大回避・自動更新 |
| ggml-cuda（R1 CUDA, NVIDIA） | **base に含めない**。採否は `docs/copilot-pc-backend-spec.md` §4.3 項 7 が正典で、同項の再検討の条件を満たして配る場合に限り optional add-on / 別パッケージとし、cudart/cublas を同梱する | CUDA ランタイムが大きく NVIDIA 環境限定。配る場合の再配布は CUDA Toolkit EULA Attachment A 準拠（著作権表示保持＋条項 pass-down。§1.6.2） |
| ggml-vulkan（R1 Vulkan, ベンダ横断 GPU） | **optional add-on / 別パッケージ**（base に含めない。別 MSI `azooKey-vulkan-addon-<version>-x64.msi` として配る。Vulkan loader / ドライバーは同梱しない。構成、起動する Host の決定、導入と更新は §1.6.3） | Vulkan 版 Host は Vulkan loader を暗黙インポートし、loader が無い環境では起動しない。base に混ぜると base の起動条件が上がる。loader とドライバーは GPU ベンダーが提供する（§1.6.2） |
| R2 の ONNX 変換モデル（R1 と同じモデル。対象は `kanakanji-edge-33m`） | **R2 の再開前は配布対象外**。再開時は optional モデルパッケージ（同 §1.2 同様に非同梱・DL。R2 を使えない機器では取得しない） | zenz-v3（GPT-2）は builder で変換できない。Llama 構造の edge は変換できる（`docs/copilot-pc-backend-spec.md` §4.2.2・§4.2.3） |

NPU / HW EP は Win11 24H2 (build 26100)+ を要するため、未満環境は R1 CPU に
フォールバックする（同 §4.3 項6）。モデル本体（GGUF / 将来の ONNX）はいずれも MSIX に同梱せず
初回起動時 DL とする方針で §1.2 と一貫させる。

#### 1.6.1 v1.0 における Zenzai GGUF の最小取得経路（M8 / M28）

§1.6 はモデルを「MSIX 非同梱・初回取得」と定める。本節はその **v1.0 最小実装**を
確定する。対象は **zenz-v3 系 GGUF 1 種**（既定配置
`%LOCALAPPDATA%\azooKey\models\zenzai\<file>.gguf`）。**v1.0 が対象とする
モデル（repo / revision / ファイル / サイズ / SHA256）は Windows 側が所有するピン定義を
唯一の正とし、(b) の配信元・(c) の手動配置先・「期待版のピン」の SHA256 はすべてこの同一
アーティファクトを指す**。ピン定義は、ビルド時に Host へ埋め込む既定の model catalog
（`settings/default-model-catalog.json`、`docs/model-management-spec.md` §3.1.1「既定の
catalog」）の `defaultModelId` の entry である。

> **legacy submodule を Windows の正にしない（AGENTS.md 準拠）**: 上流の出所は
> HuggingFace `Miwa-Keita/zenz-v3.2-small-gguf`（legacy macOS ビルドが
> `.gitmodules` / `legacy/azooKeyMac/Resources/gguf` で参照するのと同じモデル）
> だが、`legacy/` は保全された参照資産であり Windows 版の source of truth では
> ない。legacy submodule の更新・削除で Windows v1.0 のアーティファクトや
> 期待 SHA が暗黙に変わらないよう、**Windows のピンは `.gitmodules` を参照せず
> 既定の catalog に独立に固定する**（`.gitmodules` は出所の参考としてのみ引く）。
> `settings-app-spec` §3.4 のモデルパス例や roadmap M8 受け入れの版表記も、本ピン（上流
> `Miwa-Keita/zenz-v3.2-small-gguf` = v3.2）を正として揃える。版番号は各 doc に
> 直書きせず本ピンを参照し、版の更新は既定の catalog の entry を書き換えて行う。

M45 のフル管理 UI（`docs/model-management-spec.md`）が乗る土台を、二重実装を
避けて先に敷くことを目的とする。

##### 取得方式の選定

| 方式 | v1.0 採否 | 理由 |
|---|---|---|
| (a) MSIX 同梱 | ✗ 不採用 | base MSIX が GGUF 分（数百 MB〜）肥大する（§1.2）。加えてモデル同梱は再配布ライセンスリスクが最も高く、CC-BY-SA-4.0 として扱う保守運用（§1.6.2）でも不採用とする |
| (b) 初回起動時オンデマンド DL | **将来の判断（auto-DL の承認後）** | M32 で切り出す共有 `HttpDownloader` + SHA256 検証基盤（`sideload-packaging-spec` §6.3 / M36-B が再利用 / `docs/auto-word-registration-spec.md` §5）を再利用する。サイズ問題を回避し、配信元を DEV-202 結論で差し替えられる。起動時に利用者の操作なしでネットワークへ出る挙動なので、入れるかは別に判断する（下記「初回取得後の `selectedPath` コミット」）。それまでの取得は、利用者の操作による取得（設定アプリの取得ボタンと CLI の `models fetch`）が担う |
| (c) 手動配置 | ✅ **常時併存（M28 の operative default 兼 恒久フォールバック）** | オフライン / 企業環境 / DL 失敗時の確実な経路。Phase 3 検証の既存前提（M8 受け入れ条件「未配置時も Host が落ちない」）をそのまま恒久サポートする |

**確定**: v1.0 の取得方式は **利用者の操作による取得と (c) 手動配置**とし、(a) は採らない。
(b) 初回起動時 DL は auto-DL の承認後に入れるかを判断する。取得のどの経路も同一の配置
レイアウト（後述）に収束するため、Host / M45 から見た「モデルがそこに在る」状態は取得方式に
依存しない。

**マイルストーン順序の制約（初回パッケージングを M32 の DL 基盤に依存させない）**: (b) は共有
ヘルパ `HttpDownloader`（M32 で切り出し、M36-B が再利用）に依存する。MVP 直接配布は
**M11/M12（MVP MSI 構築・公開）→ M32** の順（M29 のうち延期したスタンドアロン MSIX 署名部分は前提でない。`sideload-packaging-spec` §0 / roadmap 依存グラフ）で、初回
パッケージング時点では `HttpDownloader` が未だ存在しない。したがって **初回出荷時の operative
default は (c) 手動配置**とする（初回パッケージングで one-off の重複ダウンローダを書かない＝二重
実装回避）。M32 の共有 `HttpDownloader` の上には、利用者の操作による取得を載せる。(b) を既定に
するかは auto-DL の承認後の将来の判断とし、入れる場合は **roadmap M32 のスコープ**
（`plans/windows-port-roadmap.md` M32 の受け入れ条件の auto-DL 承認の分岐）で扱う。

##### ライセンス分岐（配信元は上流 HF に固定。§1.6.2）

再配布可否は §1.6.2 の配布ライセンス結論に従う。**zenz GGUF は CC-BY-SA-4.0 として扱い
（保守側。§1.6.2）、配信元は上流 HuggingFace に固定する（再ホストしない）。
GitHub Release への再ホスト最適化だけを著者確認後に解禁する。** ここで重要な
のは、**取得*機構*（HttpDownloader + SHA256 検証 + 原子的配置）は結論に依存せず
同一**で、分岐するのは **配信元 URL と同梱可否だけ**である点。プロジェクトの
GitHub Release への再ホスト自体が再配布に当たるため、DEV-202 は (a) だけでなく
(b) の配信元選択も律する。

| 配布ライセンスの結論 | (b) の配信元 | (a) 同梱 |
|---|---|---|
| 再配布可 | プロジェクトの GitHub Release に再ホストして DL | サイズ理由で引き続き不採用 |
| 再配布不可 / 条件付き | 再ホストせず上流 HuggingFace の**既定の catalog が定める repo / revision / ファイル**（出所は `Miwa-Keita/zenz-v3.2-small-gguf`）から DL。取得物は「期待版のピン」（下記）の SHA256 と一致する＝同一アーティファクトなので 404 や版ズレを起こさない。帰属・条項は `ThirdPartyNotices.txt` と UI に明示 | 不可 |
| CC-BY-SA-4.0 として扱う（保守側。§1.6.2 の採用値） | 上流 HuggingFace（既定の catalog のピンが定める repo / revision / ファイル）から DL し、プロジェクト側へ**再ホストしない**。(c) 手動配置を確実な既定経路として案内する。帰属・改変明示を `ThirdPartyNotices.txt` と UI に記載 | 不採用（サイズ＋保守方針） |

CUDA ランタイムの同梱可否（DEV-202 で併せて確認）は本経路と独立した判断である
（モデルではなくランタイム。§1.6 の ggml-cuda 行で扱う）。採否の正典は
`docs/copilot-pc-backend-spec.md` §4.3 項 7、配る場合の再配布条件は §1.6.2 参照。

##### 配置パスとバージョニング

- **配置先**: `%LOCALAPPDATA%\azooKey\models\zenzai\<file>.gguf`（`settings-app-spec` §3.4、および
  `model-management-spec.md` §3.1 の 1 階層スキャンと整合）。
- **取得の原子性・中断と再試行**: `.part` への書き込み、Range による再開、SHA256 検証、
  確定パスへの原子置換は `docs/model-management-spec.md` §3.1.2 が正典。検証前のファイルは
  確定パスに置かれないので、ロード対象に入らない。
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
      いずれか不成立なら DL せず手動配置に委ねる（offline でも (c2) 明示選択は
      network なしで成立）。ガードを満たす場合、
      ダウンローダが rename 後の確定パスをプローブロードし、成功して初めて
      `model.selectedPath` へコミットする（更新時と同じ probe→commit 規律）。
      起動時に利用者の操作なしでネットワークへ出る挙動の変更なので、(b) を Host に
      入れるかは将来の判断とし、入れるときに本項の前提ガードを適用する。それまでの
      取得の入口は、次の利用者の操作による取得と CLI の `models fetch` だけである。
    - **利用者の操作による取得（設定アプリ）**: 設定アプリの「AI 変換」ペインは既定の
      catalog の entry を一覧し、利用者が押した取得ボタンから Host に取得させる
      （IPC は `model-management-spec.md` §4.4、取得機構は同 §3.1.2）。`privacy.mode=offline`
      では接続しない。取得時の SHA-256 のピン一致と `settings-app-spec` §3.3 の R1 検証を probe とみなし、
      成功したら確定パスを同じペインの「使うモデル」へ入れる。commit は (c2) と同じく
      保存バーが `model.selectedPath` へ書くときであり、保存後の再ロードが実際のロードを
      行う。取得しただけでは `selectedPath` を変えない。
    - **(c) 手動配置**: 次の 2 経路を定める。
        - **(c1) Host 起動時の default-path autoselect（ピン依存。将来の判断）**:
          上記のとおり Host は起動時に既定ディレクトリを自動スキャンしないので、(c1) は
          採っていない。(b) と同じく利用者の操作なしに起動時の挙動を変えるため、入れるかは
          将来の判断とし、入れる場合は次の条件を適用する。
          **`model.enabled` かつ `model.autoLoadOnHostStart=true`
          （`model-management-spec.md` §7）かつ `selectedPath` が空 / 未設定**の
          場合に限り、`models\zenzai\` の中から **SHA256 が既定の catalog の
          ピンと一致するファイル**を選び（単に format-valid な GGUF では不可。
          **ピン一致を*前提条件*とし**、licensing / 版ゲートを迂回しない。
          `model-management-spec.md` §3.1/§3.3 の形式検証も併せて満たすこと）、
          プローブロード成功時に `selectedPath` へコミットする。**ピン一致
          ファイルが無い場合は autoselect しない**（任意の有効
          GGUF を勝手に選ばない）＝ (c1) 適用外で、(c2) か劣化モード（下記）に
          委ねる。次も autoselect の対象外: (i) `autoLoadOnHostStart=false`
          （起動時ロードを抑止する設定を尊重し、何もしない）、(ii) `selectedPath`
          が**非空だが不在**（`diagnostics-compat-spec.md` D-007 が *error* 扱い
          する「設定済みパス不在」。silently 切り替えず error / M45 モデル選択
          誘導に委ねる）、(iii) `selectedPath` が非空かつ実在（下記 (c2) の明示
          選択。autoselect は上書きしない）。
        - **(c2) ユーザーによる明示選択（ピン非依存）**: ユーザー / 管理者が
          `model.selectedPath` を直接設定する経路（`settings.json` 手編集、または
          設定 UI / M45 の「モデルを追加」「選択モデルをロード」）。設定された
          実在パスを probe-load して使う。**ピンと一致しないファイルでも成立
          する唯一の手動経路**であり、(c1) を採らない間は手動配置したファイルを使う唯一の
          経路である。ファイルを `models\zenzai\` に置くだけでは不十分で、必ず
          `selectedPath` を明示設定する（空のままでは degraded）。
- **期待版のピン**: 対象の Hugging Face repo / revision / ファイル名 / サイズ / SHA256 は、
  既定の catalog（`settings/default-model-catalog.json`）の `defaultModelId` の entry の
  `artifacts["llama.cpp"]` に固定し、DL / 検証の照合に使う（legacy `.gitmodules` は参照
  しない。上記注記参照）。形式（`fileName`・`sha256`・`sizeBytes`・`source`）は
  `docs/model-management-spec.md` §3.1.1 の catalog v2 が定め、値は §1.6.2 の固定値と
  同じである。(b) の配信元と (c) の手動配置先は、いずれもこのピンと同一アーティファクトを
  指す。catalog はビルド時に Host へ埋め込み、MSI と検証用 zip には catalog のファイルを
  置かない。版を変えるときは既定の catalog の entry を書き換える。revision はコミット ID に
  固定するので、上流がブランチを更新しても取得物は変わらない。(b) の取得機構は
  `docs/model-management-spec.md` §3.1.2「catalog の entry の取得」が持つ。
- **更新時の置換（無停止更新）**: 新版を別ファイル名で DL → SHA256 検証 →
  **新版をプローブロード（実際にロード成功を確認）** → 成功して初めて
  `model.selectedPath`（`model-management-spec.md` §7）を新版へコミット →
  旧版を削除する。SHA256 一致でもロード非互換だったり途中でクラッシュした場合に
  備え、`selectedPath` のコミットはロード成功後に限定する（`autoLoadOnHostStart`
  が次回起動で参照するのはコミット済みの値のみ）。いずれかの段で失敗したら
  `selectedPath` は旧版のまま据え置き、新版ファイルは破棄して切替しない。これに
  より「失敗時は旧版を残す」を原子的に保証する。
- **未配置 / 破損時の劣化モード**: ロード失敗時の fallback と Health は
  `docs/zenzai-inference-spec.md` §7.4 が正典で、M47 の状態機械（`DegradedModel` /
  `SafeMode`）と UI 通知に従う（`⚠️ … 簡易変換で継続`）。部分 DL の `.part` は確定パスに
  昇格しないのでロード対象にならず、次の取得の再開に使う（`docs/model-management-spec.md` §3.1.2）。

##### M45（フル管理 UI）への橋渡し

`docs/model-management-spec.md` §2 は任意の URL から取得する汎用ダウンロード機能を M45 の
非目標とする。本 v1.0 最小取得経路は既定の catalog の entry に限った取得であり、次の責務分担で
二重実装を避ける:

- **v1.0（本節）が敷く土台**: 配置レイアウト（`models\zenzai\`）・取得機構
  （HttpDownloader + SHA256 + 原子置換）・期待版ピン。対象は既知の zenz-v3 1 種に
  限定し、汎用ダウンロードマネージャ UI は持たない。
- **M45 が上に足す**: 検出 / 検証（`ListModels`、GGUF magic）・backend 選択・
  ベンチマーク・管理 UI（`model-management-spec.md` §6）。M45 はこの配置レイアウト
  と SHA256 値域をそのまま再利用し、独自の取得 / 配置スキームを作らない。
- **モデル DL の UI**: 設定アプリの「AI 変換」ペインの取得ボタン（上記「利用者の操作による
  取得」）が本節の取得機構の UI であり、既定の catalog の entry だけを取得する。上表の
  配信元分岐を引き継ぎ、任意の URL を取得する汎用ダウンロードマネージャは持たない。

#### 1.6.2 配布ライセンス結論

配布ライセンスの調査結論（DEV-202、`gate:human-required`）を固定する。**本節は法的助言ではなく、
一次情報の整理と運用方針である**。**v1.0 の zenz GGUF は下記の zenz 節の保守運用（CC-BY-SA-4.0 として扱い、
上流 HuggingFace から取得・再ホストしない）とする**。著者確認を
要するのは「再ホスト解禁」の任意最適化のみで、その進捗・状態は Linear DEV-497 が正典
（本 spec には書かない）。
本節は `docs/zenzai-gpu-route.md`「配布ライセンス」から参照される。

##### モデル（zenz GGUF）

| 項目 | 内容 |
|---|---|
| ピン対象 | `Miwa-Keita/zenz-v3.2-small-gguf` の revision `c67e03e07d215c869f591b274c1631170d3e11fe`、`ggml-model-Q5_K_M.gguf`（73,871,936 バイト、SHA-256 `29c223d4c23327b80fd13ebb5ab2555057a46317997d5da391584ffbef0db673`）。既定の catalog の既定 entry（§1.6.1） |
| HuggingFace タグ | `apache-2.0`（ただし README 空・ベース未記載） |
| 矛盾兆候 | zenz 一族（v1 / v2 / v2.5 / v3 / v3.1、safetensors 版含む）とベース `ku-nlp/gpt2-small-japanese-char` はすべて `cc-by-sa-4.0`（`zenz-v2.5-small` は `base_model: ku-nlp/gpt2-small-japanese-char` を明示。HF API 確認 2026-07）。apache-2.0 は一族で v3.2-small-gguf のみ。CC-BY-SA-4.0 は ShareAlike を持ち、Apache-2.0 は CC-BY-SA-4.0 の一方向互換リストに含まれない。v3.2 が同ベース由来なら apache タグは誤りの可能性（README 空・ベース未記載・照合できる safetensors 版なし） |
| **扱い** | **保守側に倒し CC-BY-SA-4.0 として設計**する。CC-BY-SA で成立する運用は Apache でも成立するため、どちらに確定しても手戻りが出ない |
| 帰属・改変明示 | BY（帰属）は両ライセンス共通で必須。GGUF 量子化は「改変」に当たるため「量子化派生である」旨も明示（`ThirdPartyNotices.txt` / インストーラ NOTICE / 設定アプリ About。モデル名・著者 `Miwa-Keita`・出所 URL・ライセンス・量子化改変の 5 点） |
| 配信 | **再ホストせず**上流 HuggingFace から取得（§1.6.1 表）。CC-BY-SA-4.0 でも再配布自体は帰属＋SA＋改変明示で可能だが、著者確認までは再ホストしない運用でリスクを最小化する |
| 商用 | Apache-2.0 / CC-BY-SA-4.0 とも商用可。ブロッカーではない |

##### モデル（kanakanji-edge-33m GGUF）

上の zenz 節の保守運用（全体を CC-BY-SA-4.0 として扱う）は zenz に限る。選べるモデルとする
edge の扱いは、人間 lead の判断（DEV-1721）で次のとおり固定する。edge は既定の catalog
（§1.6.1）に選べる entry として載り、`azookey_inference_host models fetch kanakanji-edge-33m`
で取得できる（`docs/model-management-spec.md` §3.1.2）。取得したファイルは `model.selectedPath`
で選び、プロンプト形式は語彙から推定する（`docs/zenzai-inference-spec.md` §2.4.2）。下表の
固定値は既定の catalog の entry と同じ値である。

| 項目 | 内容 |
|---|---|
| ピン対象 | `yuuki14202028/kanakanji-edge-33m` の revision `da588df2db17e5ab69316986823806ec5f911450`、`kanakanji-edge-33m-Q8_0.gguf`（35,806,592 バイト、SHA-256 `924b1a0e2b95a17208366048071b9fd259364f24fbc6090f0e4fd680f8bdb146`）。Q8_0 だけを使う（DEV-1766 の範囲） |
| 一次情報 | 同じ revision のモデルカード（`README.md`「ライセンス」節）と `LICENSE`（Apache License 2.0） |
| 重み | Apache-2.0。モデルカードは、ゼロから学習し他モデルの重みを含まないと記載する。Q8_0 は上流が配布する GGUF をそのまま使い、こちらで量子化しない |
| 語彙 | `ku-nlp/gpt2-small-japanese-char`（京都大学 黒橋研究室）の byte-level BPE 語彙をそのまま使い、CC-BY-SA-4.0。GGUF に埋め込まれている。上流が追加した制御トークン 3 つ（id 6000〜6002）は Apache-2.0 |
| 学習データ | `Miwa-Keita/zenz-v2.5-dataset`（CC-BY-SA-4.0 の Wikipedia 由来部分と ODC-BY の llm-jp-corpus-v3 由来部分） |
| **扱い（確定）** | 重みは Apache-2.0 として扱う。語彙と学習データには CC-BY-SA-4.0 の帰属表記を置く（`ThirdPartyNotices.txt` / インストーラ NOTICE / 設定アプリ About）。帰属表記に載せるのは、モデル名、著者 `yuuki14202028`、出所 URL（`https://huggingface.co/yuuki14202028/kanakanji-edge-33m`）、ライセンスと URL（Apache-2.0 `https://www.apache.org/licenses/LICENSE-2.0`、CC-BY-SA-4.0 `https://creativecommons.org/licenses/by-sa/4.0/`）、語彙の帰属先 ku-nlp（`https://huggingface.co/ku-nlp/gpt2-small-japanese-char`）、改変の有無である。改変の有無は「上流の GGUF をそのまま使い、こちらでは改変しない。語彙の制御トークン 6000〜6002 は上流が追加したもの」と書く |
| 配信 | 再ホストせず、上流 HuggingFace の固定した revision から取得する |

##### CUDA ランタイム（add-on として配る場合の再配布条件）

- CUDA の扱いは `docs/copilot-pc-backend-spec.md` §4.3 項 7 を正典とする。R1 CUDA（ggml-cuda の実リンク）は同項の再検討の条件を満たすまで行わないので、CUDA の add-on は配らない。本節は、再検討の結果 add-on を配る場合の再配布条件を定める。
- `ggml-cuda`（R1 CUDA, NVIDIA）を配る場合は §1.6 のとおり base 非同梱の optional add-on とし、**この add-on に `cudart64_*.dll` / `cublas64_*.dll` を同梱・再配布する**。
- 根拠: NVIDIA CUDA Toolkit EULA の Attachment A（redistributable 一覧）が CUDA Runtime（cudart）・cuBLAS（cublas）等の再配布を許可する。
- 遵守条件: 配布物に **NVIDIA の著作権表示を保持**し、利用者へ **EULA と整合する条項を pass-down** する（`ThirdPartyNotices.txt` に NVIDIA CUDA Toolkit EULA の該当条項と著作権表示を記載）。
- **配置制約（app-only）**: 再配布する DLL は**アプリ専用（private）ディレクトリに配置し、本アプリからのみアクセスされる**ようにする。CUDA Toolkit EULA は redistributable な SDK 部分を「アプリからのみアクセスされる」ことを条件とするため（`sideload-packaging-spec` §2.6 が cudart/cublas を redistributable と定める一方、§1.1.2 がアクセス主体をアプリに限定）、共有 `PATH` / システムディレクトリ（`System32` 等）へ設置して他アプリから参照可能にしない。DLL 探索は app-local ディレクトリに限定する（例: add-on の配置フォルダを `SetDllDirectory` / manifest で明示し、グローバル `PATH` へ注入しない）。 <!-- doc-refs:allow -->
- 版・ファイル名はビルドで固定し、`ThirdPartyNotices.txt` に列挙する。

##### Vulkan（最小リスク）

- `ggml-vulkan` 自体は llama.cpp（MIT）のビルド成果物であり再配布に制約は薄い。
- **Vulkan ローダ / ドライバは GPU ベンダのドライバが提供**し、こちらで同梱・再配布しない → Vulkan 経路に固有の再配布義務は無い。
- 先行実装 fkunn1326/azooKey-Windows でも実働実証済み（`docs/decisions/2026-05-20-zenzai-engine-route.md`「参考: 先行 Windows 実装」）。

##### 再ホスト解禁の条件（恒久ルール）

GitHub Release 等への再ホスト（§1.6.1 表「再配布可」行）への移行は、**上流モデルの
ライセンスが Apache-2.0 と確認できたときに限る**。確認が取れるまでは本節の保守運用
（上流 HuggingFace 取得・帰属・量子化改変明示）を維持する。この著者確認・再ホスト解禁
タスクの起票・進捗・検証メモ・状態は **Linear DEV-497 が正典**（`gate:human-required`。
本 spec には状態を書かない。運用規約は `docs/linear-conventions.md` を参照する）。

#### 1.6.3 Vulkan add-on の構成と起動する Host の決定（R1）

§1.6 の `ggml-vulkan` 行を具体化し、配布形態と「どの Host 実行体が起動するか」の決定規則を
定義する。ライセンス上の扱いは §1.6.2 の Vulkan 節、backend 選択の意味論は
`docs/copilot-pc-backend-spec.md` §4 が正典。

##### 2 実行体構成を採る理由

`AZOOKEY_BACKEND=vulkan` でビルドした Host は、ggml-vulkan がインポートライブラリ経由で
Vulkan loader（`vulkan-1.dll`）を参照するため、**loader が無い環境ではプロセスが起動しない**。
ggml のバックエンドレジストリは Vulkan を有効にしたビルドでは初期化時に Vulkan バックエンドを
登録するので、この要求は明示 `cpu` 起動やモデル初期化の CPU 降格
（`docs/copilot-pc-backend-spec.md` §4.5）にも及ぶ。つまり **Vulkan 版 Host 単体では、loader
不在環境で CPU 経路も成立しない**。

したがって配布は次の 2 実行体構成とする。

| 実行体 | 配布物 | ビルド | Vulkan loader への依存 |
|---|---|---|---|
| `azookey_inference_host.exe` | base（`sideload-packaging-spec` §4 の MSI） | `windows-release` preset（`AZOOKEY_BACKEND=cpu`） | 無し |
| `azookey_inference_host_vulkan.exe` | Vulkan add-on | `windows-vulkan-release` preset（`AZOOKEY_BACKEND=vulkan`） | 有り（暗黙インポート） |

どちらの preset もビルドツリーへは `azookey_inference_host.exe` を出力する。add-on の実行体名は
パッケージ生成時に `azookey_inference_host_vulkan.exe` へ改名して確定させ、add-on の
`pkg/msi-vulkan/azooKey.VulkanAddon.wixproj` はこの名前以外の入力を拒否する。

**MSIX 経路での package identity**: external-location MSIX（§1.1.2）は `Application@Executable` に
宣言した実行体へ identity を与える。`pkg/msix/AppxManifest.xml` が持つ `Application` は base の
Host 1 つなので、この経路で add-on を配る場合は **add-on の実行体にも `Application` 宣言と、
`Application@Id` に一致する app 側 side-by-side manifest（`<msix>` の 3 属性）、および両者の
静的整合検証を用意する**。用意しなければ add-on 側の Host は `-ExternalLocation` 付きで登録しても
`Package.Current` が null になり、roadmap M28 の受け入れ条件を満たさない。MVP 既定の MSI 経路（§4）は
package identity を使わないため、この要求は MSIX 経路にだけ掛かる。MSIX の配布は延期中
（`sideload-packaging-spec` §2.0、DEV-255）であり、MSIX 経路の add-on はその再開まで配らない。

add-on は実行体を 1 つ追加するだけで base の Host を置き換えないため、base の起動条件は
add-on の導入有無で変わらない。Vulkan loader と GPU ドライバーは同梱せず、GPU ベンダーの
ドライバーが提供するものを使う（§1.6.2）。

##### add-on MSI の導入・更新・削除

add-on は base とは別の MSI（`pkg/msi-vulkan/VulkanAddon.wxs`、独自の UpgradeCode、per-machine、
未署名は base と同じ扱い）である。

| 項目 | 定義 |
|---|---|
| 中身 | `azookey_inference_host_vulkan.exe` と、帰属表示 `THIRD_PARTY_LICENSES-vulkan-addon.txt`。MSVC runtime と OpenMP runtime は base の同梱物を使う |
| 配置先 | base が `HKLM\Software\azooKey\HostStartup` の `InstallDirectory` に記録した base の `INSTALLFOLDER`。supervisor は base の Host と同じディレクトリでしか add-on を探さない |
| 導入の前提 | base が導入済みで、base の `ProductVersion`（同じキー）が add-on の版と一致すること。満たさない場合は LaunchCondition で導入を止める |
| 版の記録 | base は `HKLM\Software\azooKey\HostStartup`、add-on は `HKLM\Software\azooKey\VulkanAddon` の `ProductVersion` に自分の版を書く |
| 更新 | add-on は base と同じ版番号で出す。base だけを更新すると版が食い違い、supervisor は add-on を選ばない。add-on を同じ版へ更新すると Vulkan 版 Host の選択に戻る |
| 削除 | add-on を削除しても base は残る。base だけを削除すると Run キーと base の版の記録が消え、supervisor は起動しない。残った add-on は、base を同じ版で入れ直すと再び選ばれ、違う版で入れ直すと版の照合で選ばれない。不要なら利用者が「アプリと機能」から削除する |

稼働中の Host が add-on の実行体を使っているときの削除・更新は、ファイル使用中として
Windows Installer が再起動を求めうる。base と同じく Restart Manager による停止は無効にしている。

##### 起動する Host の決定

Host のプロセス管理は TIP の外側にある（`docs/windows-tsf-host-architecture.md`「Host の起動と
再起動」）。どちらの実行体を起動するかは、その Host 起動経路が次の順で決定する。

1. Vulkan add-on が導入済みで、MSI 経由の導入では add-on と base の `ProductVersion` が一致すること
   （開発登録には MSI の版の記録が無いため、この照合を行わない）。
2. `vulkan-1.dll` をロードできること。
3. Vulkan 版 Host の probe 起動が成功し、Vulkan デバイスを 1 つ以上列挙できること。
4. 1 から 3 がすべて成立する場合に限り Vulkan 版 Host を起動し、それ以外は base の CPU 版 Host を
   起動する。

版を照合するのは、base の更新後に古い add-on が残ると、IPC と設定の契約が違う Host を
起動してしまうためである。2 を 3 より先に判定するのは、loader が無い状態で Vulkan 版 Host を
起動するとプロセス生成そのものが失敗し、失敗理由が Host 側のログに残らないためである。3 は
loader はあるがドライバーが無い、または非対応の環境を Host 起動前に CPU 版へ寄せる。probe は Host 起動ごとに評価し、前回の
判定結果を持ち越さない。

起動経路（`scripts/host-supervisor.ps1`）は、選んだ実行体と判定理由（`addon_absent`、
`addon_version_mismatch`、`loader_unavailable`、`probe_failed` など）を診断可能な形で記録する。実際に動いている Host が
どちらかを、Health の backend 値（`docs/copilot-pc-backend-spec.md` §4.4）だけでなく起動側からも
特定できるようにする。

##### backend 解決との関係

`model.backendPreference` の解決は Host 内で完結する（`settings-app-spec` §3.7、`docs/model-management-spec.md` §5.1）。
本節が決めるのは起動する実行体であり、実行体ごとの解決は次になる。

| 起動した実行体 | `auto` | 明示 `cpu` | Vulkan 初期化失敗時 |
|---|---|---|---|
| Vulkan 版 Host | Vulkan | CPU | CPU へ降格し Health を `degraded` にする（`docs/zenzai-inference-spec.md` §7.4、段位は `docs/copilot-pc-backend-spec.md` §4.5） |
| CPU 版 Host | CPU | CPU | — |

##### ライセンス表示と SBOM

- Vulkan 版 Host は llama.cpp の成果物に加え、Vulkan-Headers / Vulkan-Hpp / SPIRV-Headers を
  ビルド時に使う。ライセンスは依存ごとに異なるため、SPDX 識別子と帰属文の正典はルートの
  `THIRD_PARTY_LICENSES` とし、本節では列挙しない。同ファイルの Vulkan add-on 向けの節は、
  固定した llama.cpp が読むヘッダを Release workflow で固定した Vulkan SDK の実ファイルで確かめて
  書く。add-on の MSI も同ファイルを `THIRD_PARTY_LICENSES-vulkan-addon.txt` として同梱する。
- Vulkan loader とドライバーを再配布しないため、CUDA add-on（§1.6.2）のような再配布条項の
  pass-down は生じない。
- add-on は独立した配布物なので、SBOM と build provenance（`sideload-packaging-spec` §4.4）を add-on 成果物に対しても
  生成し、base の SBOM に Vulkan 版 Host の依存を混ぜない。`scripts/complete-release-sbom.py` は
  `--package vulkan-addon` で add-on の SBOM を補完する。対象は `THIRD_PARTY_LICENSES` の `sbom` 注記の
  `packages` に `vulkan-addon` を持つ依存で、Vulkan SDK のヘッダは CMake cache の
  `Vulkan_INCLUDE_DIR` が渡した SDK を指すことを確かめてから、SDK の版とヘッダの SHA256 で記録する。

##### 検証の段階

| 段階 | 対象 | 実施場所 |
|---|---|---|
| compile / link | 固定 llama.cpp と ggml-vulkan のビルド、`vulkan-1.dll` 暗黙インポートの維持 | CI（GPU 不要。Vulkan SDK を導入する Windows ジョブ） |
| GPU 実行 | 実 GGUF のロードと推論、`auto` の Vulkan 解決、明示 `cpu` | Vulkan 対応 GPU のある実機 |
| 起動保証 | loader 不在、ドライバー不在・非対応、GPU 初期化失敗での CPU 経路 | GPU ドライバーの無い clean 環境（Human Gate） |
| インストール | add-on の導入・削除と base との共存 | 実機インストール（Human Gate） |

CI の compile / link 成功を、GPU 実行・起動保証・インストールの成功として読み替えない。

### 1.7 Windows アプリでの TIP ロード前提と常駐起動

UWP / Microsoft Store / AppContainer 実行の Windows アプリで第三者 TIP をロードするには、
次の条件をすべて満たす。

1. TIP DLL と親ディレクトリを AppContainer から読み取り、実行できること（下記 ACL）。
2. `DllRegisterServer` が `ITfCategoryMgr::RegisterCategory` で
   `GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT` を登録していること（[Custom input method editor requirements](https://learn.microsoft.com/windows/apps/develop/input/input-method-editor-requirements)）。
3. 第三者 IME がデジタル署名されていること（[Input Method Editors (IME)](https://learn.microsoft.com/windows/apps/develop/input/input-method-editors)）。

Microsoft Learn の署名要件は第三者 IME 全般を対象とする。本節ではその原文のスコープを
明示した上で、Windows アプリでの TIP ロード前提として整理する。

配布パッケージの署名要否（`sideload-packaging-spec` §0 / §2）と TIP バイナリに対する第三者 IME の署名要件は
別の境界である。Microsoft Store が提出 MSIX を再署名することだけを、TIP DLL の署名要件を
満たした証拠として扱わない。

> **MVP でのスコープ（`sideload-packaging-spec` §0.1 / DEV-783）**: **パッケージ化された UWP / Microsoft Store アプリ**での
> 入力は MVP の対象外であり、要件 1・2 を扱う本節は v1.0 以降で当該対応に着手するときの前提整理として読む。
> 要件 3（署名）はこのスコープ判断と独立しており、入力対象にかかわらず **v1.0 のリリースゲートとして残る**
> （`sideload-packaging-spec` §0.1）。MVP の未署名 MSI は評価配布に限定する。署名ルートの選定は `sideload-packaging-spec` §2.0 が正典
> （所有課題だった DEV-255 は Canceled、スコープ判断は DEV-783 で確定済み）。

**AppContainer DLL ACL**: UWP / Microsoft Store / AppContainer 実行のアプリで TIP を
有効化するには、TIP DLL に `ALL APPLICATION PACKAGES`（SID `S-1-15-2-1`）への
読み取り+実行 (RX) が必要である。AppContainer プロセスの実効アクセスは、ユーザー /
グループ SID による判定と AppContainer SID（package SID・capability SID）による判定の
積で決まる。対話ユーザーが読める DLL でも、DACL が AppContainer 側の principal を
含まなければアクセスは成立しない（[Launch an AppContainer](https://learn.microsoft.com/windows/win32/secauthz/implementing-an-appcontainer#appcontainer-overview)）。
ctfmon は対象アプリのプロセス内へ TIP DLL を in-proc ロードするため、この ACE が無いと
当該アプリで日本語入力ができない。

#### 付与対象・タイミング・権限

| 配布経路 | 付与主体 | 付与対象 | タイミング |
|---|---|---|---|
| `sideload-packaging-spec` §4 WiX/MSI（MVP 既定） | Windows（`%ProgramFiles%` からの継承） | インストール先配下 | インストール時。**MSI は独自 ACE を追加しない** |
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
  対象は x64 TIP DLL 1 個。32 bit プロセスへの入力は `sideload-packaging-spec` §0.1 のとおり v1.0 の対象外であり、
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
パッケージ化された UWP / Store アプリでの入力成立そのものは `sideload-packaging-spec` §0.1 により MVP の受け入れ条件から
外れており、DEV-673 で確認するのは継承 ACL までとなる。MSIX 側の制限は DEV-101（com4:ComServer ACL 制限）と
連動する。

#### MSI のログオン常駐

MVP の MSI は 64-bit の `HKLM\Software\Microsoft\Windows\CurrentVersion\Run` に
`azooKeyHost` を登録する。各ユーザーのログオン時に Windows PowerShell へ `-WindowStyle Hidden` を指定し、
インストール先の `start-installed-host.ps1` を実行する。
実行権限はログオンユーザーの通常権限であり、昇格する Task Scheduler は採用しない。
MSIX の startup task / app execution alias は本節の MSI 経路とは別に扱う。

登録は MSI の machine-wide component が所有する。各ユーザーの HKCU へのコピーや
管理者による別ユーザーのプロファイル初期化は行わない。インストール後に追加したユーザーも
同じログオン経路を使い、Host がそのユーザーのデータ領域を初期化する。
GGUF と設定値はユーザーごとのモデル管理規則に従う。インストール操作だけでは、
ログオン中の各セッションで Host を起動しない。利用開始にはサインアウトして再ログオンする。
Run の実行は Windows により遅延され得るため、ログオン直後の即時起動は保証しない
（[Run and RunOnce Registry Keys](https://learn.microsoft.com/windows/win32/setupapi/run-and-runonce-registry-keys)）。

起動の監視には開発登録と同じ `scripts/host-supervisor.ps1` を使い、実行体の選択は
§1.6.3 に従う。Vulkan probe は `--probe-vulkan` を単独で指定し、モデル、IPC、ユーザーデータを
初期化せず、列挙したデバイス数を `vulkan_devices=<数>` として出力する。成功は終了コード 0 と
正のデバイス数の両方で判定し、タイムアウトや異常終了は CPU へ戻す。選択した実行体と理由は
各ユーザーのログディレクトリで `inference-host-stderr.log.supervisor.jsonl` へ記録する。
probe のタイムアウト、起動失敗、終了コード、デバイス 0、出力不正、loader 検査不能を区別し、
Win32 エラーと probe stderr の先頭 2048 文字を診断用に残す。
launcher 自身の失敗・登録不一致は `host-launcher.log` に記録する。
両診断ログは各 1 MiB で 1 世代ローテーションし、書き込み不能時はユーザーの一時ディレクトリの
`azooKey-host-startup.log`（同じ上限）へ試行する。診断ログの失敗だけでは Host 起動を止めない。
stderr は直近 20 起動分を保持する。ログ保持は過去の起動ファイル数の制限であり、稼働中の stderr の
サイズ上限ではない。終了コード 2（引数）・3（supervisor 監視失敗）、または安定稼働前の
5 回連続失敗で再起動を停止し、次回ログオンまたは手動再起動で再試行する。
開発用 Host が同じユーザーの pipe を所有する場合は、その終了を待つ。

MSI はアンインストールと major upgrade で Run 値と
`HKLM\Software\azooKey\HostStartup` の `InstallDirectory` 値と `ProductVersion` 値を削除する。
配布 supervisor は登録の削除を検知して終了し、Host は `--supervisor-pid` で保持した
プロセスハンドルの終了を検知して通常の終了・保存処理を行う。MSI は登録値の削除後、
ファイル削除前に配布 launcher とインストール先の Host の終了を待つ。
期限内に終了しなければメンテナンスを失敗させ、強制終了しない。rollback では MSI が
登録を復元し、次回ログオンで常駐を再開する。upgrade 後も再ログオンで再開する。
ユーザーデータと開発登録の HKCU は削除しない。

ログオン起動、別ユーザーの初回起動と復帰は DEV-676、MSI の削除・upgrade・rollback と
登録値の後始末は DEV-673 の実機ゲートで確認する。静的テストや MSI 生成だけでは
これらの成功を判定しない。

`InstallValidate` の files-in-use 判定は登録値の削除より早い。MSI は
`MSIRESTARTMANAGERCONTROL=DisableShutdown` と `MSIDISABLERMRESTART=1` を指定し、
Restart Manager による自動終了・再起動と supervisor の競合を抑止する。
files-in-use の検出や再起動要求そのものは抑止しない。repair／同一版再インストールでは
削除用 waiter は動かないため、稼働中ファイルの置換には Windows の再起動が必要になり得る。
DEV-673 では稼働中の削除・repair・同一版再インストールについて、full／reduced UI の
ダイアログ、終了コード 3010、再起動後の実行版を確認する。
`-WindowStyle Hidden` はコンソール割り当て前の非表示を保証しないため、ログオン時に一瞬の表示が
起きないことは保証しない。DEV-676 で実機の見え方も確認する。
