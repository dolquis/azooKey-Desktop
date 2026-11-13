# 配布・パッケージング 仕様（Phase 7）

本書は azooKey-Desktop Windows 版の配布形態と署名・更新・観測仕様を定める。
`plans/windows-port-roadmap.md` の Phase 7 の M28〜M34 が本書を参照する。

## 読み方

本書は配布チャネルごとに節が分かれる。全文を順に読む必要はない。

- **MVP（未署名 MSI）の実装者**: §0 で対象チャネルを確認し、§4（WiX / MSI）→ §3（設定アプリ）
  の順に読む。
- **Microsoft Store 提出**: §1（MSIX）。TIP 配布経路の 3 候補は §1.0、Option A の PoC 具体は
  §1.1.2（経路はいずれも M28 の PoC で確定する）。
- **コード署名**: §2。
- **リリース後の運用（更新・観測・学習データ保護）**: §6〜§9。

次の表は、各節の内容と、その範囲を定める `plans/windows-port-roadmap.md` のマイルストーンを示す。

| 節 | 内容 | マイルストーン |
|---|---|---|
| §0 | 配布方針（チャネルと署名要否の確定） | M11・M12 |
| §1 | MSIX パッケージング（MS Store 向け） | M28（§1.6 は M24 と連動） |
| §2 | EV/OV コード署名 | M29 |
| §3 | 設定アプリ（`docs/settings-app-spec.md` へ移動。転送表を参照） | M11（最小）・M30（フル） |
| §4 | WiX / MSI インストーラ（MVP 既定） | M11・M12（§4.4 は M38） |
| §5 | WinGet マニフェスト | M32 |
| §6 | 自動更新 | M32 |
| §7 | ETW | M33 |
| §8 | WER | M33 |
| §9 | DPAPI 学習データ暗号化 | M34 |
| §10 | テスト | 共通 |
| §11 | リリース手順チェックリスト | 共通 |
| §12 | 参照 | 共通 |

## 0. 配布方針

配布チャネルと署名要否を次のとおりとする。

| チャネル | 形式 | 自前コード署名 | 位置づけ |
|---|---|---|---|
| **MVP 直接配布** | **MSI（WiX、未署名）** | 不要（任意） | MVP（評価配布）と v1.0 の既定形式。§4 が正典。GitHub Release で配布。TIP バイナリ署名は §0.1 |
| **Microsoft Store** | **MSIX** | **不要**（Microsoft が再署名） | 並行準備。§1 を Store 用 Identity で構成 |
| **スタンドアロン MSIX サイドロード** | MSIX | **必須**（有料 OV/EV） | 延期。§2 の署名ルート（§2.0 の経路 B）は本チャネルの着手時に実施する |

MSIX は署名がインストールの前提条件だが、MSI は署名が任意である。未署名 MSI では
SmartScreen 警告と UAC「不明な発行元」が出るが、reputation building で許容する。
判断根拠と、比べた先行実装は
[`decisions/2026-06-distribution-channels.md`](decisions/2026-06-distribution-channels.md) にある。

上表の「自前コード署名」欄は**インストーラの署名要否**を示す。第三者 IME として TIP バイナリに
求められる署名要件は別の境界であり、§0.1 で扱う。

以降の §1（MSIX）・§2（署名）は上表に従って読むこと。**§1 は MS Store 用 MSIX の構成**、
**§2 は延期されたスタンドアロン MSIX サイドロード向け**であり、MVP の直接配布は §4（WiX/MSI）が正典となる。

### 0.1 MVP の入力対象スコープと署名ゲート

MVP が入力先として保証するのは **Win32 デスクトップアプリに限る**。
**パッケージ化された UWP / Microsoft Store アプリ**での入力は MVP の対象外とし、v1.0 以降へ送る。

除外の境界は「AppContainer を使うか」ではなく「パッケージ化された UWP / Store アプリか」で引く。
Edge は renderer のサンドボックス化に AppContainer を使うが、入力欄をホストするのは Win32 プロセスである。
したがって Edge は対象内とする。

決定の経緯と、根拠にした実機検証は
[`decisions/2026-08-mvp-input-scope-and-signing-gate.md`](decisions/2026-08-mvp-input-scope-and-signing-gate.md) にある。

#### 32 bit (x86) プロセスを MVP 対象外とする理由

入力対象にはもう一つ、対象プロセスの bitness という境界がある。
Win32 か UWP かという上記の軸とは独立しており、MVP が保証するのは **Win32 デスクトップアプリであり、かつ x64 プロセス**である。
32 bit (x86) プロセスでの入力は v1.0 の対象外とし、既知の制限として扱う。

TSF と ctfmon は対象プロセスの bitness に一致する TIP をロードする。
本リポジトリのビルドは x64 のみであり、`tsf-tip/src/DllMain.cpp` の `DllRegisterServer` が書くのも 64 bit ビューの `HKLM\Software\Classes\CLSID` と、そこから `ITfInputProcessorProfiles` が登録する `CTF\TIP` だけである。
`scripts/register-dev.ps1` が呼ぶのも 64 bit の `regsvr32` である。
したがって 32 bit プロセスからは TIP が解決されず、azooKey を選べない。
古い Office や一部のレガシーアプリ、組み込みアプリがこれに該当する。
64 bit アプリ（Notepad、Chrome、VS Code、最新 Office、設定アプリ）は影響を受けず、この制限は v1.0 のリリースゲートを塞がない。
`docs/handoff/tip-manual-verification-checklist.md` の B7 が 32 bit アプリを `☐ 既知` として扱うのも、この境界に沿う。

将来 32 bit 対応を入れる場合に必要になる作業は次のとおりである。

* `tsf-tip/` に x86 ターゲットを追加し、x86 TIP DLL をビルドする
* 32 bit の `regsvr32` から `DllRegisterServer` を呼び、レジストリの 32 bit ビュー（`WOW6432Node`）へ CLSID と `CTF\TIP` を登録する
* §1.7 の `ALL APPLICATION PACKAGES` RX 付与規則を x86 DLL にも適用する（同節の付与対象は x64 TIP DLL 1 個を前提としている）
* MSIX（§1）と WiX/MSI（§4）へ x86 成果物を同梱し、アンインストール時に 32 bit ビューの残骸も消えることを確認する

再評価のトリガは 2 つある。
実利用者から 32 bit プロセスでの入力要望が挙がること、または M27（ARM64 ビルド）で複数アーキテクチャのビルドと登録の経路が整備され、x86 追加の限界コストが下がることである。
それまでは本節を既知の制限の正典とする。
決定の経緯と、実機検証の動線との関係は
[`decisions/2026-08-x86-process-out-of-scope.md`](decisions/2026-08-x86-process-out-of-scope.md) にある。

#### 署名は入力対象と独立した v1.0 リリースゲート

第三者 IME へのデジタル署名要件は第三者 IME 全般を対象とし、Win32 向けの例外は文書化されていない。
[Input Method Editors (IME)](https://learn.microsoft.com/windows/apps/develop/input/input-method-editors#requirements-for-imes) は要件を満たさない第三者 IME を "blocked from running" と定めており、この記述に配布形態や入力先による限定はない。
未署名の TIP が動く環境があっても、それは観測された挙動であり、文書化されたサポート条件ではない。

したがって次の 2 点を分けて扱う。

* **署名は入力対象にかかわらず v1.0 のリリースゲートとして残す。** Store / UWP を対象外にしたことは、署名義務を免除する理由にならない。署名ルートの選定は §2.0 が正典。
* **MVP の未署名 MSI は評価配布に限定する。** 開発とゲート検証のための配布であり、一般利用者向けの正式リリースとしては扱わない。配布時は未署名であることと評価目的であることを明示する。

#### Store / UWP を MVP 対象外とする理由

署名を調達しても Store 入力が成立するとは限らないためである。

プロセスが Code Integrity Guard を有効化している場合、`PROCESS_MITIGATION_BINARY_SIGNATURE_POLICY` の `MicrosoftSignedOnly` または `StoreSignedOnly` により、Microsoft 署名でも Store 署名でもない DLL は memory manager が `STATUS_INVALID_IMAGE_HASH` を返して map 自体を拒否する（[Code integrity guard](https://learn.microsoft.com/defender-endpoint/exploit-protection-reference#code-integrity-guard)）。
この経路が働いているなら、OV 証明書を取得しても Store 入力は成立しない。
Store 入力の失敗が IME 署名要件と CIG のどちらによるものかは切り分けておらず、MVP の期間内に決着させる見込みが立たない。
そのため入力対象から外して v1.0 以降のトラックへ送り、切り分けもそのトラックで行う。

TIP DLL が `ALL APPLICATION PACKAGES` の RX を継承していることの確認（§1.7）は、将来の対応に備えた回帰防止として残す。

## 1. MSIX パッケージング（MS Store 向け・M28）

> **スコープ注記（§0 配布方針）**: 本節の MSIX は **MS Store 配布（DEV-416）の構成**として読む。
> Store 提出パッケージは Microsoft が再署名するため §2 の自前署名は不要。有料署名を要する
> スタンドアロン MSIX サイドロードは延期する（§0。DEV-255）。MVP の直接配布は §4（WiX/MSI、DEV-415）が正典。

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

> 補足: D ルートとして **WiX/MSI installer** で従来の `regsvr32` 経路（§4）を
> 使う選択肢もある。MSIX 不可環境（LTSC 等）向けには結局 §4 を持つので、
> v1.0 は §4 / WiX で確定 + §1 は M28 PoC へ送る選択もあり得る。

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
> `0x8007000B` で失敗する。具体手順は §2.2「Publisher と証明書 Subject の整合（必須）」を参照。

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

`pkg/msix/Package.wapproj`（WAP プロジェクト）：

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

解除前の入力方式の切替（`azookey_ime_switch.exe`）は MSI と開発登録の解除手順に含め、
MSIX の `Remove-AppxPackage` には含めない。手順は §4.2 に従う。

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
> §3.4 のモデルパス例や roadmap M8 受け入れの版表記も、本ピン（上流
> `Miwa-Keita/zenz-v3.2-small-gguf` = v3.2）を正として揃える。版番号は各 doc に
> 直書きせず本ピンを参照し、版の更新は既定の catalog の entry を書き換えて行う。

M45 のフル管理 UI（`docs/model-management-spec.md`）が乗る土台を、二重実装を
避けて先に敷くことを目的とする。

##### 取得方式の選定

| 方式 | v1.0 採否 | 理由 |
|---|---|---|
| (a) MSIX 同梱 | ✗ 不採用 | base MSIX が GGUF 分（数百 MB〜）肥大する（§1.2）。加えてモデル同梱は再配布ライセンスリスクが最も高く、CC-BY-SA-4.0 として扱う保守運用（§1.6.2）でも不採用とする |
| (b) 初回起動時オンデマンド DL | **将来の判断（auto-DL の承認後）** | M32 で切り出す共有 `HttpDownloader` + SHA256 検証基盤（§6.3 / M36-B が再利用 / `docs/auto-word-registration-spec.md` §5）を再利用する。サイズ問題を回避し、配信元を DEV-202 結論で差し替えられる。起動時に利用者の操作なしでネットワークへ出る挙動なので、入れるかは別に判断する（下記「初回取得後の `selectedPath` コミット」）。それまでの取得は、利用者の操作による取得（設定アプリの取得ボタンと CLI の `models fetch`）が担う |
| (c) 手動配置 | ✅ **常時併存（M28 の operative default 兼 恒久フォールバック）** | オフライン / 企業環境 / DL 失敗時の確実な経路。Phase 3 検証の既存前提（M8 受け入れ条件「未配置時も Host が落ちない」）をそのまま恒久サポートする |

**確定**: v1.0 の取得方式は **利用者の操作による取得と (c) 手動配置**とし、(a) は採らない。
(b) 初回起動時 DL は auto-DL の承認後に入れるかを判断する。取得のどの経路も同一の配置
レイアウト（後述）に収束するため、Host / M45 から見た「モデルがそこに在る」状態は取得方式に
依存しない。

**マイルストーン順序の制約（初回パッケージングを M32 の DL 基盤に依存させない）**: (b) は共有
ヘルパ `HttpDownloader`（M32 で切り出し、M36-B が再利用）に依存する。MVP 直接配布は
**M11/M12（MVP MSI 構築・公開）→ M32** の順（M29 のうち延期したスタンドアロン MSIX 署名部分は前提でない。§0 / roadmap 依存グラフ）で、初回
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

- **配置先**: `%LOCALAPPDATA%\azooKey\models\zenzai\<file>.gguf`（§3.4、および
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
      では接続しない。取得時の SHA-256 のピン一致と §3.3 の R1 検証を probe とみなし、
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
          が**非空だが不在**（`dev-infrastructure-spec.md` D-007 が *error* 扱い
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
- **配置制約（app-only）**: 再配布する DLL は**アプリ専用（private）ディレクトリに配置し、本アプリからのみアクセスされる**ようにする。CUDA Toolkit EULA は redistributable な SDK 部分を「アプリからのみアクセスされる」ことを条件とするため（§2.6 が cudart/cublas を redistributable と定める一方、§1.1.2 がアクセス主体をアプリに限定）、共有 `PATH` / システムディレクトリ（`System32` 等）へ設置して他アプリから参照可能にしない。DLL 探索は app-local ディレクトリに限定する（例: add-on の配置フォルダを `SetDllDirectory` / manifest で明示し、グローバル `PATH` へ注入しない）。 <!-- doc-refs:allow -->
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
| `azookey_inference_host.exe` | base（§4 の MSI） | `windows-release` preset（`AZOOKEY_BACKEND=cpu`） | 無し |
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
（§2.0、DEV-255）であり、MSIX 経路の add-on はその再開まで配らない。

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

`model.backendPreference` の解決は Host 内で完結する（§3.7、`docs/model-management-spec.md` §5.1）。
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
- add-on は独立した配布物なので、SBOM と build provenance（§4.4）を add-on 成果物に対しても
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

#### 付与対象・タイミング・権限

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
  対象は x64 TIP DLL 1 個。32 bit プロセスへの入力は §0.1 のとおり v1.0 の対象外であり、
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

**採用経路**: 経路 B（Azure Key Vault + AzureSignTool）、または B'（CA クラウド署名:
SSL.com eSigner / DigiCert KeyLocker 等）とする。署名主体は日本の個人開発者であり、経路 A は
地域制限で使えない（個人 = 米/加のみ）。EV（経路 C）は即時信頼が廃止され OV と同じ
reputation building になったため、SmartScreen 対策として選ぶ理由が無い。新規 OV は HSM 必須化で
PFX を入手できないため、CI 署名を保てる B / B' を採る。

本署名の用途は、スタンドアロン MSIX サイドロードと、第三者 IME としての TIP バイナリ署名である。
前者は §0 のとおり延期し、後者は §0.1 のとおり v1.0 のリリースゲートとして残る
（MVP の MSI と MS Store の MSIX はいずれも自前署名を要しない）。
§0.1 のとおり、署名を調達しても Code Integrity Guard により Store 入力が成立しない可能性がある。
本節に着手する前に、その切り分けを済ませる。

証明書の調達と申請は所在地と組織化状況に依存する人間判断であり、再開時に
`gate:human-required` 課題を新規に立てて扱う。
経路判定の経緯と、費用対効果の前提の更新は
[`decisions/2026-06-signing-route-selection.md`](decisions/2026-06-signing-route-selection.md) にある。

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
ネイティブ成果物と設定アプリは `all azookey_settings --parallel 2` の一回の build で
構築する。ネイティブコンパイルには Release 専用の sccache を使う
（`docs/dev-infrastructure-spec.md` §4.7）。設定アプリの MSBuild はキャッシュ対象外である。

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

## 4. WiX / MSI インストーラ（MVP 既定 / 旧 M31）

MSI の最小 OS は Windows 10 version 1903（build 18362）とする。
Host が静的リンクする [OS 付属 ICU](https://learn.microsoft.com/en-us/windows/win32/intl/international-components-for-unicode--icu-)
の下限に合わせ、MSI はレジストリの `CurrentBuildNumber` を検査して旧 OS への新規インストールを拒否する。
既存インストールの削除はこの条件で妨げない。リライターが既定 OFF でも ICU のロード要件は変わらない。

> **スコープ注記（§0 配布方針）**: 本節の **WiX MSI が MVP と v1.0 の既定配布形態**である。MVP の MSI は未署名の評価配布（DEV-415）、
> v1.0 の MSI は TIP バイナリ署名を含む正式リリースとする（§0.1）。
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
WiX Toolset は MSBuild SDK の 7.0.0 に固定し、x64 の per-machine MSI を生成する。
WiX 7 のビルドに必要な OSMF EULA への同意は `azooKey.wixproj` の `AcceptEula` に記録する
（条件は [`licensing-policy.md`](licensing-policy.md#ビルドツールの利用条件)）。
配置先は `%ProgramFiles%\azooKey` とする。
Vulkan add-on は別の MSI（`pkg/msi-vulkan/VulkanAddon.wxs` と `pkg/msi-vulkan/azooKey.VulkanAddon.wixproj`、
同じ WiX SDK）で、base の `INSTALLFOLDER` へ実行体を 1 つ足す。構成と導入の条件は §1.6.3 が正典。

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
配布バイナリが直接依存する MSVC ランタイムを app-local で同梱する。
`msvcp140.dll`、`vcruntime140.dll`、`vcruntime140_1.dll` はビルドに使用した MSVC
toolset の x64 `Microsoft.VC*.CRT` から取得する。llama.cpp を組み込んだ Host は
ggml の OpenMP 経路により `vcomp140.dll` も暗黙インポートするため、これを
x64 `Microsoft.VC*.OpenMP` から取得して同じく同梱する。OpenMP は CRT とは別の
redist ディレクトリであり、`VCRuntimeDir` と `VCOpenMPDir` の 2 つの MSBuild
property で取得元を渡す。いずれも TIP と Host と同じ `%ProgramFiles%\azooKey` へ
配置する。これにより、VC++ Redistributable が未導入のクリーンな Windows 11 でも
起動可能にする。同梱漏れは、配布先で
`STATUS_DLL_NOT_FOUND`（`0xC0000135`）による即時終了として表面化する。

同梱一覧は `Package.wxs` の手書き `<File>` なので、ビルド構成の変更が増やした
ランタイム依存は静かに漏れうる。`scripts/check-app-local-runtime.ps1` は配布する PE の
import を `dumpbin /dependents`（delay load を含む）で読み、MSVC ランタイムの命名規則
（`msvcp` / `msvcr` / `vcruntime` / `vcomp` / `concrt` / `vccorlib` で始まり、
`msvcp140_atomic_wait` のような非数値の接尾辞も含む）に一致する名前が、MSI の同梱一覧に
あるかを検査する。Release workflow は MSI をビルドする前にこれを実行する。
Build workflow の `windows-llama-build` も、Release / CPU / llama.cpp 構成の TIP・Host・
self-contained 設定アプリに同じ検査を実行する（実行条件は `dev-infrastructure-spec.md` §4.3）。
本節は同 spec の app-local MSVC runtime 検査から参照される。

base MSI は静的辞書層の `.azdic` を `%ProgramFiles%\azooKey\dict\` へ、その帰属を
`%ProgramFiles%\azooKey\ThirdPartyNotices.txt` へ配置する。Host はこの `dict` だけを
探索する。同梱する層、配布判定、取得元とサイズ予算の正典は
`docs/auto-word-registration-spec.md` §14.9 / §14.10 と `docs/azdic-physical-layer-spec.md` §15.9 / §15.12 であり、本節は
配置先だけを持つ。辞書は release workflow が `dictbuild/build_bundled.py` で SHA256 固定の
上流とリポジトリ内の自作seedから生成し、`BundledDictionaryDir` property で MSI へ渡す。
2回生成の再現性検査は `azdic-physical-layer-spec.md` §15.12 を正典とする。`dictbuild/check_bundle.py` は
MSI をビルドする前に、standalone の `neologd_lexicon` pack の混入、層 ID とファイル名の不一致、
`ThirdPartyNotices.txt` の帰属欠落、`Package.wxs` の同梱一覧と生成物の差を検出して失敗させる。
本節の帰属文書の配置先は、設定アプリのライセンス導線（`native-ui-spec.md` §4.6）から参照される。

照合先は取得元の redist ディレクトリではなく MSI の同梱一覧とする。redist
ディレクトリは `msvcp140_1.dll` や `concrt140.dll` のように MSI が同梱しないファイルも
持つため、そちらと突き合わせると「取得元にはあるが MSI に入らない」依存を見逃す。
同梱一覧は `Package.wxs` が個別に列挙する `<File>` と、`<Files Include="...\**">` で
ツリーごと harvest する設定アプリ payload の実ファイルから組み立てる。OS が常に提供する
DLL は対象にせず、allowlist を持たない。設定アプリは Hybrid CRT で静的リンクするため、
この検査では MSVC ランタイムの import を持たない側として通る。

`Package.wxs`、release workflow のランタイム解決、SBOM の `RUNTIME_FILES` は同じ
ファイル集合を別々に手書きするため、`scripts/tests/msi-package-consistency.Tests.ps1`
が三者の一致を検査する。片方だけ増やすと SBOM が出荷物を過少報告する。
GGUF モデルは初回取得、CUDA runtime は optional add-on とし、base MSI へ含めない。
設定アプリは `SettingsPayloadDir` を必須入力として self-contained 出力一式を同梱する。
`SettingsExePath` は実行ファイルの取得元パスであり、既定値は
`$(SettingsPayloadDir)\azookey_settings.exe` とする。インストール後の名前は常に
`INSTALLFOLDER\azookey_settings.exe` で、スタートメニューのショートカットはこの固定パスを
指す。ショートカットは `SettingsExe` の advertised shortcut とし、per-machine のファイルを
component KeyPath に保つ。release workflow は `azookey_settings` target をビルドしてから両
property を MSI ビルドへ渡す。

#### 設定アプリの製品メタデータ

設定アプリの EXE と MSI の advertised ショートカットは、同じ
`settings-app/Assets/azookey-settings.ico` を使用する。EXE は RC resource として、
MSI は Icon table の `SettingsIcon.exe` stream として格納する。
実際のスタートメニューでのアイコン表示は DEV-673 の実機確認に含める。

Release の FileVersion / ProductVersion は、タグ `vMAJOR.MINOR.PATCH` から
得た MSI 製品バージョンに `.0` を付けた値とする。通常の開発ビルドの既定値は
`0.0.0.0` とし、CMake の `AZOOKEY_PRODUCT_VERSION` で指定できる。
Release workflow は設定アプリをビルドした直後に EXE の両バージョンを読み取り、
期待値との不一致をエラーにする。
`azooKey.wixproj` も、`SettingsExePath` の FileVersion が `ProductVersion` に `.0` を付けた値と
違う場合はビルドを失敗させる。`SettingsExe` は版を持つ KeyPath なので、旧版より低い
FileVersion をメジャーアップグレードで配ると、costing が component の導入を見送った後に
`RemoveExistingProducts` が旧版のファイルを消し、アップグレード後に設定アプリが残らない。

### 4.2 アンインストール時

`RemoveFiles` の前に `msiexec /z [#TipDll]` で `DllUnregisterServer` を呼ぶ。
TIP DLL を削除した後では解除できないため、この順序を変更しない。

azooKey を選択中のアプリが開いたまま登録だけが消えると、そのアプリの入力状態が壊れる。
`DllUnregisterServer` は昇格したプロセス（MSI では SYSTEM の deferred CA）で動き、
対話ユーザーの入力方式を切り替えられない。そのため解除の前に、対話ユーザーの文脈で
`azookey_ime_switch.exe` を実行する。

- helper は `ITfInputProcessorProfileMgr::ActivateProfile`（`TF_IPPMF_FORSESSION`）で、
  日本語の別の有効な入力方式（Microsoft IME、他の TIP、キーボードレイアウトの順）へセッションを切り替える。
  別の TIP が既に選択中なら何もしない。キーボードレイアウトの照会結果は、フォーカスを持たない helper のスレッドからは当てにならないので、切替の要否に使わない。切替先は keyboard カテゴリの TIP とレイアウトに限る。azooKey のプロファイルは無効化しない。
  無効化はユーザー単位（HKCU）に残り、再登録後も azooKey が選べない状態になりうるためである。
- 切替先が無い、または `ActivateProfile` が失敗したときは、何も変えずに終了コード 1 で終わり、
  失敗の種類を `%LOCALAPPDATA%\azooKey\logs` の `ime_switch` ログに残す。解除は止めない。
- `unregister-dev.ps1` は昇格前のユーザー側の処理で helper を TIP DLL と同じディレクトリから呼ぶ。
  helper が無いときは警告して解除を続ける。
- MSI は immediate CA `SwitchAwayFromTip`（`Return="ignore"`）を `RegisterTipOnUninstallRollback` の前に置く。
  条件は `$TipComponent = 2 AND REMOVE="ALL" AND NOT UPGRADINGPRODUCTCODE` で、修復とメジャーアップグレードでは走らない。
  deferred / rollback の CA の順序は変えない。
- 切替の対象は、アンインストールを実行したユーザーのセッションだけである。他のユーザーのセッションと、
  SYSTEM が実行する配布は対象外とする。

### 4.3 Inno Setup（代替）

WiX に習熟がない場合は Inno Setup でも代替できる。MVP の正典は §4.1 の WiX 構成であり、
Inno Setup 版の成果物はリポジトリに持たない。代替を採る場合も次の条件は WiX 版と揃える。

- ユーザースコープでインストールする（`DefaultDirName={localappdata}\azooKey`、
  `PrivilegesRequired=lowest`）。管理者権限を要求しない。
- TIP DLL の登録・解除は `[Run]` / `[UninstallRun]` から `regsvr32`（`/s`、解除は `/u /s`）を
  `waituntilterminated` で呼ぶ。
- 設定アプリの WinUI 3 ランタイムの扱いは §1.3 と同じ判断に従う。self-contained 出力なら
  一式を同梱し、framework-dependent なら `WindowsAppRuntimeInstall.exe` を同梱して
  インストール時に実行する。

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

syft が `documentDescribes` または `DESCRIBES` で示す MSI package を再利用し、
`versionInfo` に Release の MSI バージョン、`checksums` に実ファイルの SHA256 を設定する。
root が曖昧、別のファイル名、またはハッシュが不一致の場合は生成を拒否する。
次の各依存は同じ MSI package から `DEPENDS_ON` で参照する。
これは canonical Release ビルドの依存宣言であり、
バイナリ内部の全コンポーネントを自動検出した結果ではない。

- llama.cpp / WIL: `CMakeLists.txt` のフル SHA と、それを参照する `FetchContent_Declare`
  を読み、CMake cache と取得した Git checkout の HEAD・追跡ファイルの変更有無を照合する。
  改行の比較は checkout の Git 属性に従い、stage 済み・未 stage の両方を HEAD と比較する。
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
Vulkan add-on の MSI は base と別の SBOM と attestation を持ち、`--package vulkan-addon` で
補完する（対象の依存と Vulkan SDK ヘッダの照合は §1.6.3）。

同梱・依存する第三者資産を列挙する正典は、引き続きルート `THIRD_PARTY_LICENSES`
（運用規約は `docs/licensing-policy.md`）である。SBOM をもって attribution の確認を
代替しない。本節は `docs/licensing-policy.md` の SBOM 案内から参照される。

#### attestation を dry-run で作らない理由

attestation はリポジトリに残る公開記録である。`workflow_dispatch` によるビルド確認
（§2.3）で attestation を作ると、配布していないビルドに対する証明が残り、検証者に
誤解を与える。このため attest ステップは `github.ref_type == 'tag'` で限定する。
SBOM 生成自体は dry-run でも実行し、ステップの破損を早期に検出する。

#### タグ release の draft と asset の確認

タグの選定・push、`RELEASE_ENABLED` の有効化、Release の公開判断は人間が行う。
エージェントは run と draft Release の読み取り、asset の照合、検証メモの作成を補助する。
branch を指定した `workflow_dispatch` は MSI と SBOM の生成確認に使える。
tag ref では attestation と draft 添付も実行されるため、dry-run には branch を指定する。

1. 人間が選んだ `vMAJOR.MINOR.PATCH` タグに対する Release run を開く。
   `RELEASE_ENABLED` が文字列 `true` であり、`release` job が skip されず成功したことを確認する。
   `Attest build provenance`、`Attest SBOM`、`Upload Release asset (draft)` の実行結果も確認する。
   失敗時は job 名、step 名、該当ログを検証メモに残す。
2. 同じタグの Release が draft であることを確認する。添付ファイルは
   `azooKey-<MAJOR.MINOR.PATCH>-x64.msi` と `azooKey.spdx.json` の 2 点とする。
   `gh release view <tag> --repo dolquis/azooKey-Desktop --json tagName,isDraft,assets,url`
   でもタグ、draft 状態、asset 名とバイト数を読み取れる。
3. run summary の `Release assets before upload` 表と、Release asset の名前・バイト数を照合する。
   ダウンロードした各 asset の SHA256 も同じ表と照合する。空ファイル、追加・欠落、
   別タグの MSI、サイズ・ハッシュの不一致は確認失敗として扱う。
   添付は `softprops/action-gh-release` が同じ job のファイルを直接読む経路であり、
   `actions/download-artifact` / `actions/upload-artifact` による受け渡しはこの workflow にはない。

検証メモは Linear の該当課題へ、タグ名・commit SHA・run URL・draft Release URL、
job と上記 step の結果、各 asset の名前・バイト数・SHA256 と照合結果、人間の判断を残す。
branch dry-run の成功は、タグ時の attestation・draft 作成・asset 添付の確認を代替しない。

## 5. WinGet マニフェスト（M32）

> **スコープ注記（§0 配布方針 / §0.1）**: WinGet が配布するのは **v1.0 の正式リリースの MSI**
> （TIP バイナリ署名済み。M29 が前提）である。MVP の未署名 MSI は評価配布に限るため（§0.1）、
> WinGet には載せない。Release に MSIX は含まれないため `InstallerType` は `msi`。MS Store 配布（DEV-416）は Store が
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

本節の範囲は更新の確認（新しいリリースを見つけて利用者に示すこと）である。MSI のダウンロードと
適用（§6.3）は別の課題とする。

### 6.1 チェック経路

`inference-host/src/UpdateChecker.cpp`（`azookey::host::UpdateChecker`）が、常駐 Host の中で
確認を担う。

- `autoUpdate.enabled` が `true` で、`privacy.mode` が `offline` でも `secure` でもないときだけ
  確認する（§6.2）。設定の再読込で無効になると、実行中の確認を取り消す（ダウンローダの
  取り消しは受信の待ちの間にも効く。`docs/model-management-spec.md` §3.1.2）。
- `settings.json` が読めない・不正な間も確認しない。不正な再読込は設定を適用しないが、Host が
  publish するプライバシーの判定が secure に倒れる（`docs/privacy-and-secure-input-spec.md` §2 の
  fail closed）ので、それを見て止める。
- 確認は Host の起動時と、`checkIntervalHours` ごとに行う（8760 を超える値は 8760 として
  扱う）。Host を再起動しても、同じ `channel` と同じ製品のバージョンの前回の確認から
  `checkIntervalHours` が経つまでは確認しない。更新を入れた後の最初の起動では、すぐに確認して
  記録を書き換える。失敗した確認は、次の周期で再び試す（即時の再試行はしない）。
- 結果は `settings.json` と同じディレクトリの `update-status.json` に atomic write で書く。
  形式は次のとおりで、`latestVersion` と `releaseUrl` は確認に成功したときだけ置く。
  `releaseUrl` は `https://github.com/dolquis/azooKey-Desktop/releases/` で始まる URL だけを
  書く。取り消した確認は何も書かない。

```json
{
  "version": 1,
  "checkedAtEpoch": 1794370000,
  "channel": "stable",
  "result": "update_available",
  "currentVersion": "1.0.0",
  "latestVersion": "1.1.0",
  "releaseUrl": "https://github.com/dolquis/azooKey-Desktop/releases/tag/v1.1.0"
}
```

`result` は `update_available` / `up_to_date` / `failed` のいずれかである。設定アプリの
「バージョン」はこのファイルを読んで表示し（§3.2）、IPC は使わない。版は `v` を付けて
「v1.1.0」と表示する。記録の `channel` か `currentVersion` が設定アプリの設定と製品のバージョンに
合わない記録は、結果として表示しない。Host は各確認の結果を
RuntimeLog の `update_check` イベントに、固定の `reason`（`up_to_date` / `update_available` /
`failed` / `cancelled` / `disabled`）だけで記録する。URL と応答本文は記録しない。

### 6.2 API と送る情報

```
stable: GET https://api.github.com/repos/dolquis/azooKey-Desktop/releases/latest
beta:   GET https://api.github.com/repos/dolquis/azooKey-Desktop/releases?per_page=20
User-Agent: azooKey-Desktop/<製品のバージョン>
```

`stable` は最新の正式リリース（draft と pre-release を除く）を、`beta` は一覧のうち draft を除いた
最も新しいリリース（pre-release を含む）を採る。`tag_name`（`v` 接頭辞を許す）を SemVer 2.0.0 の
優先順位で現在の製品のバージョン（ビルド時の `AZOOKEY_PRODUCT_VERSION`）と比べ、新しければ
`update_available` とする。応答は 1 MiB を上限とし、プロキシは WinHTTP の automatic proxy に従う。

送る情報は、接続元の IP アドレス（通信の性質上 GitHub に見える）と、User-Agent に入る製品の
バージョンだけである。端末やインストールの識別子、設定値、利用状況、Cookie は送らない。確認の
結果を外部へ送り返すこともなく、テレメトリにはならない。止め方は `autoUpdate.enabled` を
`false` にする（既定）か、`privacy.mode` を `offline` か `secure` にすることである。opt-in の
うえでも、利用者が最も強い保護（`secure`）を選んだときは外部へ通信しない。`custom` の
`externalAi` は AI バックエンドの可否であり、更新の確認とは連動させない。

§1.6.1 は Host が起動時に利用者の操作なしでネットワークへ出ることを制限する。更新の確認は
既定で無効（opt-in）であり、利用者が `autoUpdate.enabled` をオンにすることを、起動時と周期の
確認を許す利用者の操作として扱う。

### 6.3 ダウンロード → 適用

本節は将来の課題であり、更新の確認（§6.1）とは別に実装する。新バージョンを検出したら：

1. 通知（トースト）「新しいバージョン v1.1.0 が利用可能」
2. ユーザーが「インストール」をクリック
3. MSI を `%TEMP%` にダウンロード（`WinHttpReadData`）
4. SHA256 検証
5. `msiexec /i <msi> /qn /norestart` で更新（同一 `UpgradeCode` により in-place アップグレード。要昇格）。
   使用中の TIP DLL 置換で再起動要求が起き得るため、`/norestart` で自動再起動を抑止する
   （[Standard Installer Command-Line Options](https://learn.microsoft.com/windows/win32/msi/standard-installer-command-line-options)）
6. 終了コード `3010`（`ERROR_SUCCESS_REBOOT_REQUIRED`）の場合は、自動再起動せず「再起動が必要」を
   ユーザーに通知して同意を得てから再起動する

更新の確認の結果は、設定アプリの「バージョン」に表示するだけで、トースト通知はしない。非パッケージの
MSI 配布でトーストを出すには AUMID の登録が要るため、トースト通知は別の課題とする。

### 6.4 WinSparkle 互換

WinSparkle 互換の `appcast.xml` フィードも `gh-pages` ブランチで提供
（外部ツールからの参照用）。Phase 7 では実装スコープ外、将来課題。

### 6.5 設定

```json
{
  "autoUpdate": {
    "enabled": false,       // default: false（opt-in。§6.2）
    "channel": "stable",    // "stable" | "beta"
    "checkIntervalHours": 24
  }
}
```

## 7. ETW（M33）

### 7.1 Provider GUID

```cpp
// {89553f1c-7d18-4a6d-a720-534921c8b730}
constexpr GUID kAzooKeyEtwProvider = {
    0x89553f1c, 0x7d18, 0x4a6d, {0xa7, 0x20, 0x53, 0x49, 0x21, 0xc8, 0xb7, 0x30}};
```

Provider 名は `azooKey-Desktop`。GUID はバージョン間で固定する。

### 7.2 Event ID 表

| Event ID | 名称 | フィールド |
|---|---|---|
| 1000 | Activate | client_id, profile_guid |
| 1001 | Deactivate | client_id |
| 2000 | CompositionStart | length |
| 2001 | CompositionEnd | length, committed |
| 3000 | IpcRequest | request_id, message_type, payload_size, client_id |
| 3001 | IpcResponse | request_id, latency_ms, client_id, result |
| 3002 | IpcCancel | target_request_id, client_id |
| 3003 | IpcPhase | request_id, phase, latency_ms, client_id, result |
| 4000 | InferenceStart | request_id, backend, kana_len, client_id |
| 4001 | InferenceEnd | request_id, n_candidates, latency_ms, client_id, result |
| 4002 | InferencePhase | request_id, phase, backend, latency_ms, client_id, result |
| 5000 | LearningObserve | reading_len, surface_len |
| 5001 | LearningForget | reading_len, surface_len |
| 9000 | Error | source, error_code, hr |

> 上表のフィールドは長さ・件数・enum・数値・GUID・ID のみで構成し、入力本文を
> 含めない。本文を載せない理由と禁止対象は §7.2.1 を正典とする。

相関キーは `client_id`（GUID）と `request_id` の組とする。GUID 形式でない旧クライアントの
ID はゼロ GUID とし、本文や任意文字列に置き換えない。`result` は Success=0、Timeout=1、
Disconnected=2、Failed=3、Cancelled=4、`backend` は Unknown=0、Kana=1、Neural=2、Ai=3。
`phase` は Converter=0、AiTransform=1、FrameWrite=2、FrameRead=3 とする。この 4 値は
構造化ログの phase（`docs/dev-infrastructure-spec.md` §7.7.2 の 11 値）の粗い射影であり、
対応表と `trace_id` を載せない相関の規則は同 §7.3 を正典とする。
converter の失敗後に代替変換が成功した場合、4002 は Failed、4001 は Success を記録する。
`kana_len`、`reading_len`、`surface_len`、`payload_size` は UTF-8 バイト数、
Composition の `length` は UTF-16 コード単位数、`latency_ms` は単調時計によるミリ秒。

バイナリレイアウトは `diagnostics/etw/AzooKey.man` の template で定義する。
各イベントは UInt64 3 個、Double 1 個、GUID 1 個、UInt64 1 個の 56 バイトとし、
イベントが使わない `reserved*` フィールドはゼロにする。末尾の UInt64 は結果分類、
GUID は 1000 では profile GUID、要求イベントでは client GUID を表す。

#### 7.2.1 本文を ETW に載せない（redaction 規律）

ETW は `docs/dev-infrastructure-spec.md` §7.6 の redaction ポリシー正典が適用
される観測経路である。ETW トレース（`.etl`）は WPA で誰でも閲覧でき、secure /
`privacy.redactLogs` の状態に応じて**事後にフィルタできない**（消費側に redaction
ゲートが無い）。そのため本文系フィールド（`reading` / `surface` / `candidate.text` /
確定文字列 / Magic Conversion prompt / typo の `raw_keys` 等）は **build / env / mode に
よらず一切 ETW へ書き込まない**。構造化ログ（`logs/*.jsonl`）が `docs/dev-infrastructure-spec.md` §7.6 優先順位 4 の
opt-in で本文を出し得るのとは異なり、ETW は本文出力経路を持たない。

- 旧 §7.2 の `5000/5001 LearningObserve/Forget` は `reading, surface`（= 入力本文）を
  載せていたが `docs/dev-infrastructure-spec.md` §7.6 違反のため廃止する。代わりに本文長のみを `reading_len` /
  `surface_len`（整数）で記録し、内容は復元できないようにする。
- `9000 Error` は自由文 `message` を載せず、`source`（`EtwModule` enum で符号化したモジュール
  識別子）/ `error_code`（`docs/dev-infrastructure-spec.md` §7.4 の 3 カテゴリ enum）/
  `hr`（HRESULT）のみとする。例外メッセージ等の自由文は `docs/dev-infrastructure-spec.md` §7.6 を適用済みの構造化ログ側へ
  出し、ETW には載せない。
- `EtwLogger`（§7.3）は本文型の引数を受ける API を**持たない**。長さ・件数・enum・
  数値・GUID・ID のみを受ける型シグネチャに限定し、本文混入をコンパイル時に防ぐ。
  `source` も `const char*` ではなく `EtwModule` enum とし、呼び出し側が任意 / ユーザー由来の
  文字列を渡せる引数を一切残さない。
- ETW は本文を含まないメタ情報のみのため、レイテンシ trace（`docs/dev-infrastructure-spec.md` §7.6 redact 対象外）と
  同じく追加の同意を要さない。WPR トレース（`.etl`）の採取・共有はユーザー / 開発者の
  明示操作（§7.4）であり、その成果物にも本文は含まれない。

### 7.3 ラッパ

`core/src/EtwLogger.cpp`：

```cpp
// EtwModule / EtwErrorCode は閉じた enum。文字列引数を一切持たせず、本文混入を
// コンパイル時に防ぐ（§7.2.1）。EtwErrorCode は dev-infrastructure-spec.md §7.4 の
// 3 カテゴリ（transport / protocol / business）。§7.2 9000 Error と同一。
enum class EtwModule { Tip, Host, Settings, Ipc };
enum class EtwErrorCode { Transport, Protocol, Business };

class EtwLogger {
public:
    static void Register();
    static void Unregister();
    static void LogActivate(...);
    static void LogIpcRequest(...);
    // 自由文字列は受けない。source は EtwModule enum、code は EtwErrorCode enum、
    // hr は HRESULT。例外メッセージ等の自由文は ETW に載せず、§7.6 適用済みの
    // 構造化ログへ出す（§7.2.1）。
    static void LogError(EtwModule source, EtwErrorCode code, std::int32_t hr);
};
```

`EventRegister` + manifest 定義イベントの `EventWrite`（または TraceLogging）で
**型付きフィールド**を書き込む薄いラッパ。任意文字列を書く `EventWriteString` は
本文混入経路になるため**使わない**（§7.2.1）。各 `Log*` は §7.2 表のフィールド
（長さ・件数・enum・数値・GUID・ID）のみを型付き引数で受け、本文型引数を持たない。
全モジュール（TIP/Host/Settings）から呼べるよう `core/` 配下に置く。

### 7.4 観測

`diagnostics/azookey-diagnostics.wprp` は azooKey provider と OS の CSwitch / DiskIO を
同じトレースへ収集する。`GeneralProfile` だけでは azooKey のイベントは有効にならない。
manifest のリソース DLL は `azookey_etw_manifest` ターゲットで生成する。

開発者はビルド後、リポジトリルートの管理者 PowerShell から以下を実行する。
manifest 登録と WPR の開始・終了は診断時の明示操作であり、IME の起動・インストールでは行わない。

```powershell
$manifest = (Resolve-Path diagnostics/etw/AzooKey.man).Path
$resources = (Resolve-Path build/windows-debug/diagnostics/etw/azookey_etw_manifest.dll).Path
wevtutil im $manifest /rf:$resources /mf:$resources
wpr -profiles diagnostics/azookey-diagnostics.wprp
wpr -start diagnostics/azookey-diagnostics.wprp!AzooKeyDiagnostics -filemode
# IME を有効にして一度変換する。
wpr -stop azookey.etl
wpa.exe azookey.etl
# 診断が終了したら開発用 manifest 登録を解除する。
wevtutil um $manifest
```

WPA の Generic Events で provider `azooKey-Desktop` を絞り、同一の
`client_id` / `request_id` の 3000 → 3003（FrameWrite）→ 4000 → 4002 → 4001 → 3003（FrameRead）→ 3001 を確認する。
3003 の FrameRead と FrameWrite は読み書きの区間、4002 は backend の区間を表す。
タイムアウト・切断・converter 障害は `result` で分類し、CSwitch / DiskIO と時間軸を比較する。
WPR / WPA の実機確認は、自動テストによる provider 登録・採取の確認とは別に行う。

## 8. WER（Windows Error Reporting、M33）

### 8.1 許可したメタデータだけを保存する minidump

Host / Settings は `CrashReporting` を初期化し、明示的に
`SetUnhandledExceptionFilter` を設定する。TIP DLL は利用アプリ内で動くため、
プロセス全体の例外フィルタを設定しない。TIP の IPC 処理は局所的な
`__try` / `__except` の局所境界で数値の例外コードだけを記録し、
`EXCEPTION_CONTINUE_SEARCH` で利用アプリ / OS の障害処理へ伝播する。
`/EHsc` では非同期例外時の C++ オブジェクト解放を保証できないため、
アクセス違反を握りつぶして IPC worker を再開する復旧は行わない。

ダンプはプロセスメモリを走査せず、次の 4 ストリームだけを明示的に構成する。
`MiniDumpWriteDump` の通常ダンプに含まれるスタックやレジスタは収集しない。

| ストリーム | 保存する情報 |
|---|---|
| `SystemInfoStream` | システムのアーキテクチャと固定のプラットフォーム識別値 |
| `ExceptionStream` | 数値の例外コード・例外アドレス・障害スレッド ID |
| `ThreadListStream` | 障害スレッド ID のみ。スタック・コンテキスト・TEB は含めない |
| `ModuleListStream` | Host または Settings の固定 exe 名、起動時に取得したモジュールのベースアドレスとサイズ |

ストリームと未使用フィールドはゼロ初期化する。例外パラメータ、レジスタ、
スタック、ヒープ、データセグメント、ユーザーパス、任意の文字列は保存しない。
この形式では例外位置の照合はできるが、呼び出し履歴や変数の復元はできない。

`local` の有効化時に書き込み worker を用意し、例外フィルタは数値フィールドを
渡して最大 1,000 ms 待つ。ディレクトリ作成、ファイル I/O、保持処理は worker が行う。
無効時、要求が競合した場合、保存失敗時、待機が期限に達した場合は以前のフィルタまたは
OS 既定処理へ委ねる。自前ダンプの保存成功を確認できた場合だけ
`EXCEPTION_EXECUTE_HANDLER` を返す。プロセス内の障害であるため、worker による
保存も保証はしない。

スタックオーバーフロー時のフィルタ実行余地として、Host / Settings の main thread と
Host が管理する worker の各開始点で `SetThreadStackGuarantee` により 64 KiB を要求する。
この予約は呼び出したスレッドまたは fiber にのみ適用され、プロセス全体には波及しない。
予約または fiber ごとの成功記録に失敗したスレッドでのスタックオーバーフローは
azooKey 管理ダンプを試みず、OS の障害処理へ委ねる。WinRT threadpool と外部ライブラリが
管理するスレッドには予約を保証せず、同じ fallback を適用する。
実際のスタック枯渇は隔離した子プロセスの main / worker で検証し、
`off` では成果物を作らない。スタックやメモリ内容の収集範囲は増やさない。

### 8.2 保存先・保持・削除運用

- **保存先**: `%LOCALAPPDATA%\azooKey\crashes\`。ファイル名は
  `azookey-<module>-<UTC: yyyyMMddTHHmmssZ>-<pid>.dmp`（`<module>` は `host` /
  `settings`）。同名ファイルを上書きしない。
- **ディレクトリ作成**: `local` の有効化時に作成し、保存時にも存在を確認して、
  削除されていれば再作成する。`off` では収集用ディレクトリを作らない。
  作成・書き込みに失敗しても OS の障害処理を妨げない。
- **保持上限**: 既定で **最新 5 個 / 合計 50 MB / 30 日**を上限とし、
  いずれかを超えた古いダンプから削除する。Host / Settings をまたいだ集合に適用する。
  対象は所定の名前を持つ通常ファイルのみとし、シンボリックリンク、reparse point、
  サブディレクトリや無関係なファイルを辿らない。
- **適用タイミング**: `local` の有効化時に適用し、書き込み前にも新規 1 個分の枠を確保する。
  Host / Settings の同時書き込みは同一セッション内の名前付き mutex で直列化する。
  書き込み成功後は完了を通知してから再度適用するが、プロセス終了によって中断され得る。
  `off` への切替は新規収集を停止し、既存ファイルの手動削除は保存先から行う。
- **アンインストール時**: MSIX（§1.4）・手動アンインストール経路では当該ディレクトリも
  削除対象とする（`compat-test` の残骸 0 smoke 対象）。

### 8.3 ダンプ内容の最小化と同意（redaction / consent）

本節は `docs/privacy-and-secure-input-spec.md` §7 から参照される収集・保持方針の正典とする。
本文を後から検出・マスクする方式ではなく、§8.1 の許可したフィールドだけを保存する。
入力本文、候補、学習データ（`reading` / `surface`）、API キーを含み得る
メモリ断片は取り込まない。

- **同意キー** `privacy.crashReportConsent`（enum、既定 `off`。schema の定義は
  `docs/privacy-and-secure-input-spec.md` §7）:
  - `off` — azooKey 管理のダンプを書かない。キー欠落、未知値、型不正、
    設定ファイルの読み取り・解析失敗も `off` として扱う。
  - `local` — ユーザーが明示的に選択した場合のみ、§8.1 のメタデータを
    `%LOCALAPPDATA%` 配下へ保存する。自動送信しない。
- **OS 側との境界**: 本キーが制御するのは azooKey 管理ダンプの新規生成のみとする。
  azooKey は WER LocalDumps のレジストリ登録を作成せず、OS の WER / LocalDumps 設定を
  有効化も無効化もしない。`off` でも、管理者やユーザーが設定した OS 側の収集は
  行われ得る。その内容は本キーの制御対象外とする。
- **同意操作**: 設定アプリのクラッシュレポート設定で `local` を選び、保存すると適用する。
  保存先を開く操作を用意する。自動アップロード・GitHub Issue 自動起票は行わず、
  外部への添付はユーザーの明示操作とする。
- **送信同意は別契約**: M33 の enum は `off` / `local` のみとし、`upload` を含めない。
  送信機能を追加する場合は、バージョンとタイムスタンプを持つ明示同意レコードを
  別途導入する。既存の `local` や未知値を送信への同意に読み替えない。
- **診断 ZIP**: M44 の `crash-summary.txt` は
  `docs/dev-infrastructure-spec.md` §12.5 に従い、ダンプの要約だけを含める。
  ダンプ本体やメモリ上の文字列バッファを同梱しない。

## 9. DPAPI 学習データ暗号化（M34）

### 9.1 目的

`learning.tsv` に含まれる確定履歴（個人情報を含み得る）をユーザースコープで
暗号化。OS ユーザーが変わると復号できない。

### 9.2 実装

`learning/src/DpapiCrypto.cpp` の `DpapiByteCrypto` が現在の Windows ユーザーの
DPAPI で暗号化・復号する。`ReadProtectedText` は `.enc` を優先して復号し、存在しない
場合に限って旧平文ファイルを読む。復号失敗時は平文へフォールバックしない。
`WriteProtectedText` は暗号化結果を `.enc` へ原子的に保存する。

### 9.3 LearningStore との統合

`LearningStore::Load` は `ReadProtectedText` で得た内容を TSV として解析し、旧平文を
読み込んだ場合は `MigratePlaintextFile` で `.bak` を保全してから `.enc` を作る。
`LearningStore::Save` は `WriteProtectedText` を使い、移行前の平文や読み込み失敗が
残る状態では上書きしない。暗号化ファイルと旧平文が共存するときは、復号が成功し、
旧平文が保全済みの `.bak` と一致する場合だけ旧平文を削除して保存を続ける。
入力を変更しない `LearningStore::LoadReadOnly`（`lookup` CLI が使う）は、旧平文を同じ形式で
解析し、移行しない。
ユーザー辞書、誤字補正、新語のストアも同じ保護境界に従う。

### 9.4 移行

既存の平文 TSV から暗号化形式への 1 回限り移行：

- 起動時、`learning.tsv`（平文）と `learning.tsv.enc`（暗号化）の両方を check
- 平文があれば読み込んで暗号化形式に書き直し、平文を削除（バックアップは
  `learning.tsv.bak` に残す）
- `.enc` が既にある場合は暗号化ファイルを優先し、復号に失敗しても平文へ
  フォールバックしない。移行では元ファイルを `.bak` に残し、暗号化書き込みが
  成功するまでは元ファイルを維持する。`typo_corrections.tsv` と
  `auto_words.tsv` にも同じ境界を適用する。
- 0 バイトの平文 TSV はレコードの無いストアとして読み込み、同じ手順で移行する
  （0 バイトの `.bak` を残し、空の内容を暗号化した `.enc` を作り、平文を削除する）。

### 9.5 ユーザー辞書

`user_dict.json` も同様に暗号化（M34 範囲）。明示的な辞書エクスポートは、
利用者が指定した出力先へ平文 JSON を書く操作として区別する。
0 バイトの `user_dict.json` は JSON を解釈できないファイルとして隔離せず、
エントリの無い辞書として読み込み、§9.4 の 0 バイトの平文 TSV と同じ手順で移行する。
復号した内容が空の `user_dict.json.enc` もエントリの無い辞書として読み込む。
入力を変更しない読み込み（`lookup` CLI）は、0 バイトの平文を移行せずに空の辞書として返す。

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
| 署名検証 | `pkg/tests/signature_test.ps1`（M29 で新設する予定。署名経路〔§2〕と同時に追加する） | signtool /verify で成功するか |
| UpdateChecker | `inference-host/tests/update_checker_test.cpp`、`inference-host/tests/http_downloader_test.cpp`（`UpdateCheckerHttpIntegrationTest`） | 版の比較、channel ごとのリリースの選択、無効・失敗・取り消し時の扱い、周期と前回の確認による間隔、ループバックの Releases API を通した確認と送る情報 |
| ETW provider | `core/tests/etw_logger_test.cpp` | Windows 限定。Register/Unregister + Write |
| DPAPI | `learning/tests/dpapi_crypto_test.cpp` | Windows 限定。Encrypt → Decrypt round trip、他ユーザでは失敗 |
| WinGet manifest | CI で `winget validate` | YAML が valid |

## 11. リリース手順チェックリスト

1. 変更点をまとめる（repo は `CHANGELOG.md` を持たない。`release.yml` は Release の本文を生成しないので、手順 5 の Draft Release の本文にリリースノートを書く）
2. バージョンタグを打つ (`git tag v1.0.0`)
3. `git push --tags`
4. `.github/workflows/release.yml` が自動実行
5. Draft Release が作成される（MVP: 未署名 MSI 添付。配布方針は §0。MS Store 配布は Partner Center 経由で別手順 = DEV-416。スタンドアロン MSIX 署名は §2。延期は §0）
6. SBOM（`azooKey.spdx.json`）が Release 資産に添付され、`gh attestation verify <msi> --repo dolquis/azooKey-Desktop` が成功することを確認（§4.4）
7. 動作確認（クリーン VM でインストール → 入力 → 確定 → アンインストール）
8. Draft → Publish
9. winget-pkgs に PR（`wingetcreate update`）

## 12. 参照

- MSIX SDK: <https://learn.microsoft.com/windows/msix/desktop/source-code-overview>
- signtool: <https://learn.microsoft.com/windows/win32/seccrypto/signtool>
- DPAPI: <https://learn.microsoft.com/windows/win32/api/dpapi/>
- ETW: <https://learn.microsoft.com/windows/win32/etw/>
- WiX Toolset: <https://wixtoolset.org/docs/intro/>（版は `pkg/msi/azooKey.wixproj` の `WixToolset.Sdk` が定める）
- WinGet manifest: <https://github.com/microsoft/winget-cli/blob/master/doc/ManifestSpecv1.5.md>
- 既存：`scripts/register-dev.ps1` / `scripts/unregister-dev.ps1`（`regsvr32` 開発用経路）
- ベース：`docs/windows-tsf-host-architecture.md`
- CUDA backend の実リンクの中止に合わせて、§3.7 の v1.0 設定 UI のデバイス選択を `auto` / `cpu` の 2 値にした経緯：[`decisions/2026-08-04-cuda-real-link-cancellation.md`](decisions/2026-08-04-cuda-real-link-cancellation.md)

## 転送表

§3（設定アプリ）は `docs/settings-app-spec.md` へ移した。節番号は保つ。

| 旧節 | 移動先 |
|---|---|
| §3 | `docs/settings-app-spec.md` |

設定アプリの UI フレームワーク選定の経緯は `decisions/2026-06-settings-app-ui-framework.md` にある。結論の節は `docs/settings-app-spec.md` §3.0 である。
