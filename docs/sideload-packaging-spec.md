---
kind: spec
milestones: [M11, M12, M29, M32, M33, M34]
canonical_for: ["配布・署名・更新・観測"]
---

# 配布・パッケージング 仕様（Phase 7）

本書は azooKey-Desktop Windows 版の配布形態と署名・更新・観測仕様を定める。
`plans/windows-port-roadmap.md` の M11・M12 と Phase 7 の M29・M32〜M34 が本書を参照する。
M28（MSIX）は `docs/msix-packaging-spec.md`、M30（設定アプリ）は `docs/settings-app-spec.md` が定める。

## 読み方

本書は配布チャネルごとに節が分かれる。全文を順に読む必要はない。

- **MVP（未署名 MSI）の実装者**: §0 で対象チャネルを確認し、§4（WiX / MSI）→ §3（設定アプリ）
  の順に読む。
- **Microsoft Store 提出**: `docs/msix-packaging-spec.md`（旧 §1）。TIP 配布経路の 3 候補は §1.0、Option A の PoC 具体は
  §1.1.2（経路はいずれも M28 の PoC で確定する）。
- **コード署名**: §2。
- **リリース後の運用（更新・観測・学習データ保護）**: §6〜§9。

次の表は、各節の内容と、その範囲を定める `plans/windows-port-roadmap.md` のマイルストーンを示す。

| 節 | 内容 | マイルストーン |
|---|---|---|
| §0 | 配布方針（チャネルと署名要否の確定） | M11・M12 |
| §1 | MSIX パッケージング（MS Store 向け。`docs/msix-packaging-spec.md` へ移動。転送表を参照） | M28（移動先が定める。§1.6 は M24 と連動） |
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
| **Microsoft Store** | **MSIX** | **不要**（Microsoft が再署名） | 並行準備。`msix-packaging-spec` §1 を Store 用 Identity で構成 |
| **スタンドアロン MSIX サイドロード** | MSIX | **必須**（有料 OV/EV） | 延期。§2 の署名ルート（§2.0 の経路 B）は本チャネルの着手時に実施する |

MSIX は署名がインストールの前提条件だが、MSI は署名が任意である。未署名 MSI では
SmartScreen 警告と UAC「不明な発行元」が出るが、reputation building で許容する。
判断根拠と、比べた先行実装は
[`decisions/2026-06-distribution-channels.md`](decisions/2026-06-distribution-channels.md) にある。

上表の「自前コード署名」欄は**インストーラの署名要否**を示す。第三者 IME として TIP バイナリに
求められる署名要件は別の境界であり、§0.1 で扱う。

以降の `msix-packaging-spec` §1（MSIX）・§2（署名）は上表に従って読むこと。**`msix-packaging-spec` §1 は MS Store 用 MSIX の構成**、
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
* `msix-packaging-spec` §1.7 の `ALL APPLICATION PACKAGES` RX 付与規則を x86 DLL にも適用する（同節の付与対象は x64 TIP DLL 1 個を前提としている）
* MSIX（`msix-packaging-spec` §1）と WiX/MSI（§4）へ x86 成果物を同梱し、アンインストール時に 32 bit ビューの残骸も消えることを確認する

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

TIP DLL が `ALL APPLICATION PACKAGES` の RX を継承していることの確認（`msix-packaging-spec` §1.7）は、将来の対応に備えた回帰防止として残す。

## 2. EV/OV コード署名（M29）

> **スコープ注記（§0 配布方針 / §0.1）**: 本節（自前コード署名）が要るのは 2 つの文脈である。
> 1 つは**スタンドアロン MSIX サイドロード**で、これは §0 の配布方針により当面延期する。
> もう 1 つは**第三者 IME としての TIP バイナリ署名**（`msix-packaging-spec` §1.7 の要件 3）で、こちらは入力対象の
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

* `msix-packaging-spec` §1.1 manifest 例の `Publisher="CN=dolquis"` は **`CN=dolquis` のみの Subject**
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
  `msix-packaging-spec` §1.1 manifest 例の `Publisher="CN=dolquis"` はそのままでは通らないため、証明書
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
（`docs/build-ci-spec.md` §4.7）。設定アプリの MSBuild はキャッシュ対象外である。

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
> 配布方針転換により MVP の主経路へ格上げした。MSIX は `msix-packaging-spec` §1（MS Store 用）に回す。MSI は署名が任意で、
> 未署名でもインストール可能（SmartScreen 警告 + UAC「不明な発行元」は出る）。

MSIX 不可環境（Win10 LTSC, 法人ポリシーで AppX 無効）にも本経路で対応する。

> **設定アプリと WinUI 3 ランタイムの同梱**:
> `azookey_settings` target は unpackaged の設定アプリを self-contained でビルドする。
> MVP MSI は、その出力フォルダから `azookey_settings.exe` と WinUI 3 ランタイムを回収する。
> MSIX フレームワーク依存が使えない LTSC 等では、設定アプリを
> **self-contained 配置（`msix-packaging-spec` §1.3）でビルドし、その出力フォルダ一式を配置する**。
> self-contained を採らない場合は `WindowsAppRuntimeInstall.exe` をペイロードに含めて
> インストール時に実行する。TIP DLL / Host は WinUI 3 非依存（`msix-packaging-spec` §1.3）。

### 4.1 WiX 構成

`pkg/msi/Package.wxs` と `pkg/msi/azooKey.wixproj` を正典とする。
WiX Toolset は MSBuild SDK の 7.0.0 に固定し、x64 の per-machine MSI を生成する。
WiX 7 のビルドに必要な OSMF EULA への同意は `azooKey.wixproj` の `AcceptEula` に記録する
（条件は [`licensing-policy.md`](licensing-policy.md#ビルドツールの利用条件)）。
配置先は `%ProgramFiles%\azooKey` とする。
Vulkan add-on は別の MSI（`pkg/msi-vulkan/VulkanAddon.wxs` と `pkg/msi-vulkan/azooKey.VulkanAddon.wixproj`、
同じ WiX SDK）で、base の `INSTALLFOLDER` へ実行体を 1 つ足す。構成と導入の条件は `msix-packaging-spec` §1.6.3 が正典。

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
self-contained 設定アプリに同じ検査を実行する（実行条件は `build-ci-spec.md` §4.3）。
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
- 設定アプリの WinUI 3 ランタイムの扱いは `msix-packaging-spec` §1.3 と同じ判断に従う。self-contained 出力なら
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
補完する（対象の依存と Vulkan SDK ヘッダの照合は `msix-packaging-spec` §1.6.3）。

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
「バージョン」はこのファイルを読んで表示し（`settings-app-spec` §3.2）、IPC は使わない。版は `v` を付けて
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

`msix-packaging-spec` §1.6.1 は Host が起動時に利用者の操作なしでネットワークへ出ることを制限する。更新の確認は
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
構造化ログの phase（`docs/observability-spec.md` §7.7.2 の 11 値）の粗い射影であり、
対応表と `trace_id` を載せない相関の規則は同 §7.3 を正典とする。
converter の失敗後に代替変換が成功した場合、4002 は Failed、4001 は Success を記録する。
`kana_len`、`reading_len`、`surface_len`、`payload_size` は UTF-8 バイト数、
Composition の `length` は UTF-16 コード単位数、`latency_ms` は単調時計によるミリ秒。

バイナリレイアウトは `diagnostics/etw/AzooKey.man` の template で定義する。
各イベントは UInt64 3 個、Double 1 個、GUID 1 個、UInt64 1 個の 56 バイトとし、
イベントが使わない `reserved*` フィールドはゼロにする。末尾の UInt64 は結果分類、
GUID は 1000 では profile GUID、要求イベントでは client GUID を表す。

#### 7.2.1 本文を ETW に載せない（redaction 規律）

ETW は `docs/observability-spec.md` §7.6 の redaction ポリシー正典が適用
される観測経路である。ETW トレース（`.etl`）は WPA で誰でも閲覧でき、secure /
`privacy.redactLogs` の状態に応じて**事後にフィルタできない**（消費側に redaction
ゲートが無い）。そのため本文系フィールド（`reading` / `surface` / `candidate.text` /
確定文字列 / Magic Conversion prompt / typo の `raw_keys` 等）は **build / env / mode に
よらず一切 ETW へ書き込まない**。構造化ログ（`logs/*.jsonl`）が `docs/observability-spec.md` §7.6 優先順位 4 の
opt-in で本文を出し得るのとは異なり、ETW は本文出力経路を持たない。

- 旧 §7.2 の `5000/5001 LearningObserve/Forget` は `reading, surface`（= 入力本文）を
  載せていたが `docs/observability-spec.md` §7.6 違反のため廃止する。代わりに本文長のみを `reading_len` /
  `surface_len`（整数）で記録し、内容は復元できないようにする。
- `9000 Error` は自由文 `message` を載せず、`source`（`EtwModule` enum で符号化したモジュール
  識別子）/ `error_code`（`docs/observability-spec.md` §7.4 の 3 カテゴリ enum）/
  `hr`（HRESULT）のみとする。例外メッセージ等の自由文は `docs/observability-spec.md` §7.6 を適用済みの構造化ログ側へ
  出し、ETW には載せない。
- `EtwLogger`（§7.3）は本文型の引数を受ける API を**持たない**。長さ・件数・enum・
  数値・GUID・ID のみを受ける型シグネチャに限定し、本文混入をコンパイル時に防ぐ。
  `source` も `const char*` ではなく `EtwModule` enum とし、呼び出し側が任意 / ユーザー由来の
  文字列を渡せる引数を一切残さない。
- ETW は本文を含まないメタ情報のみのため、レイテンシ trace（`docs/observability-spec.md` §7.6 redact 対象外）と
  同じく追加の同意を要さない。WPR トレース（`.etl`）の採取・共有はユーザー / 開発者の
  明示操作（§7.4）であり、その成果物にも本文は含まれない。

### 7.3 ラッパ

`core/src/EtwLogger.cpp`：

```cpp
// EtwModule / EtwErrorCode は閉じた enum。文字列引数を一切持たせず、本文混入を
// コンパイル時に防ぐ（§7.2.1）。EtwErrorCode は observability-spec.md §7.4 の
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
- **アンインストール時**: MSIX（`msix-packaging-spec` §1.4）・手動アンインストール経路では当該ディレクトリも
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
  `docs/diagnostics-compat-spec.md` §12.5 に従い、ダンプの要約だけを含める。
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

§3（設定アプリ）は `docs/settings-app-spec.md` へ、§1（MSIX パッケージング）は `docs/msix-packaging-spec.md` へ移した。節番号は保つ。

| 旧節 | 移動先 |
|---|---|
| §3 | `docs/settings-app-spec.md` |
| §1 | `docs/msix-packaging-spec.md` |

設定アプリの UI フレームワーク選定の経緯は `decisions/2026-06-settings-app-ui-framework.md` にある。結論の節は `docs/settings-app-spec.md` §3.0 である。
