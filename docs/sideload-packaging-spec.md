# サイドロード配信 仕様（Phase 7）

本書は azooKey-Desktop Windows 版の配布形態と署名・更新・観測仕様を定める。
`plans/windows-port-roadmap.md` の Phase 7 の M28〜M34 が本書を参照する。

**Microsoft Store 配信は対象外** とし、サイドロード（自己署名 + EV/OV 証明書
配布）に専念する。

## 1. MSIX サイドロード（M28）

### 1.1 AppxManifest.xml

`pkg/msix/AppxManifest.xml`（新規）：

```xml
<?xml version="1.0" encoding="utf-8"?>
<Package
    xmlns="http://schemas.microsoft.com/appx/manifest/foundation/windows10"
    xmlns:uap="http://schemas.microsoft.com/appx/manifest/uap/windows10"
    xmlns:uap3="http://schemas.microsoft.com/appx/manifest/uap/windows10/3"
    xmlns:rescap="http://schemas.microsoft.com/appx/manifest/foundation/windows10/restrictedcapabilities"
    IgnorableNamespaces="uap uap3 rescap">

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
    <TargetDeviceFamily Name="Windows.Desktop"
                       MinVersion="10.0.19041.0"
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

        <!-- TIP の COM 登録 -->
        <com:Extension Category="windows.comServer"
                       xmlns:com="http://schemas.microsoft.com/appx/manifest/com/windows10">
          <com:ComServer>
            <com:SurrogateServer>
              <com:Class Id="{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}"
                         Path="azookey_tsf_tip.dll" />
            </com:SurrogateServer>
          </com:ComServer>
        </com:Extension>

        <!-- TSF Profile 登録（Windows 11 でも有効） -->
        <uap3:Extension Category="windows.inputMethod">
          <uap3:InputMethod>
            <uap3:Profile InputProfileGuid="{YYYYYYYY-YYYY-YYYY-YYYY-YYYYYYYYYYYY}"
                          LanguageTag="ja-JP" />
          </uap3:InputMethod>
        </uap3:Extension>
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

- クリーンな Win10 22H2 / Win11 23H2 VM で `Add-AppxPackage` 成功
- 言語バーから azooKey が選べる
- アンインストールで `HKCU\Software\Classes\CLSID\...` が消える

## 2. EV/OV コード署名（M29）

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

### 2.3 CI ステップ

`.github/workflows/release.yml`（新規）：

```yaml
name: Release
on:
  push:
    tags: ['v*']
jobs:
  build:
    runs-on: windows-latest
    steps:
      - uses: actions/checkout@v4
        with: { submodules: recursive }

      - name: Setup MSBuild
        uses: microsoft/setup-msbuild@v2

      - name: Configure
        run: cmake -S . -B build -G "Visual Studio 17 2022" -A x64

      - name: Build
        run: cmake --build build --config Release

      - name: Test
        run: ctest --test-dir build -C Release --output-on-failure

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

Host は受信した設定を `%LOCALAPPDATA%\azooKey\settings.json` に保存し、
即時反映可能なものは適用、再起動が必要なものは `restart_required: bool` を返す。

### 3.4 設定ファイルパス

| 用途 | パス |
|---|---|
| 設定 | `%LOCALAPPDATA%\azooKey\settings.json` |
| カスタムローマ字 | `%LOCALAPPDATA%\azooKey\custom-romaji.tsv` |
| 学習データ | `%LOCALAPPDATA%\azooKey\learning.tsv`（DPAPI 暗号化、M34） |
| ユーザー辞書 | `%LOCALAPPDATA%\azooKey\user-dict.json` |
| モデル | `%LOCALAPPDATA%\azooKey\models\zenz-v3.1-small-Q5_K_M.gguf` |
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

`user-dict.json` も同様に暗号化（M34 範囲）。

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
