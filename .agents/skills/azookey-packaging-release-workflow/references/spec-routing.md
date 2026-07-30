# 配布とリリース変更のルーティング

## 最初に決めること

`docs/sideload-packaging-spec.md` §0 を読み、変更対象の配布チャネルを確定する。

| 配布チャネル | 現行の位置づけ | 最初に読む正典 |
|---|---|---|
| MVP 直接配布 | 未署名 WiX MSI。GitHub Release の既定経路 | `docs/sideload-packaging-spec.md` §4、`pkg/msi/README.md` |
| Microsoft Store | Store が再署名する MSIX。Store 用 identity を扱う | `docs/sideload-packaging-spec.md` §1 |
| スタンドアロン MSIX サイドロード | 自前署名が必要。現行方針では延期 | `docs/sideload-packaging-spec.md` §2 |
| external-location identity package | MSIX Option A の PoC。実機ゲートを残す | `docs/sideload-packaging-spec.md` §1.1.2、`pkg/msix/README.md` |

## 変更領域ごとの参照先

| 変更領域 | 主な実装と設定 | 優先検証 |
|---|---|---|
| WiX MSI の内容、custom action、VC runtime | `pkg/msi/Package.wxs`、`pkg/msi/azooKey.wixproj`、`pkg/msi/README.md` | `scripts/tests/msi-package-consistency.Tests.ps1`、README の MSI build 手順 |
| MSIX identity と external location | `pkg/msix/AppxManifest.xml`、`pkg/msix/azookey_inference_host.exe.manifest`、`pkg/msix/build-identity-package.ps1` | `scripts/tests/msix-identity-consistency.Tests.ps1`、`scripts/verify-msix-identity-embedding.ps1` |
| MSIX 更新、rollback、登録 lifecycle | `compat-test/msix_install_uninstall.ps1` | `scripts/tests/msix-lifecycle-scenarios.Tests.ps1`、許可された VM smoke |
| TIP 登録、解除、AppContainer ACL | `scripts/register-dev.ps1`、`scripts/unregister-dev.ps1`、`scripts/AppContainerAcl.ps1`、WiX custom action | `scripts/tests/register-dev.Tests.ps1`、`tsf-tip/tests/com_smoke_test.cpp` の opt-in smoke |
| VM 検証 package と bootstrap | `scripts/make-vm-verify-package.ps1`、`scripts/verify-bootstrap.ps1` | `scripts/tests/vm-verify-package.Tests.ps1`、`docs/handoff/hyper-v-tip-verification.md` |
| Release artifact、version、publish | `.github/workflows/release.yml`、`THIRD_PARTY_LICENSES` | `scripts/test-powershell-quality.ps1`、release workflow の build / artifact checks |
| package identity の runtime 利用 | `inference-host` の embedded manifest と package identity 判定、`tsf-tip` の登録分岐 | CodeGraph / Serena による参照確認、関連 C++ tests、Windows 実ビルド |

## 検証の段階

1. `scripts/test-powershell-quality.ps1` で PSScriptAnalyzer と Pester の既存 suite を確認する。
2. 変更対象に対応する個別 Pester test と静的 manifest / identity 検証を確認する。
3. 必要な target を Windows Headless CMake Build 手順で実ビルドする。
4. 正典に記載された canonical script で MSI または MSIX を生成する。
5. 署名、証明書ストア、package 登録、machine-wide TIP 登録、IME 実入力、rollback、uninstall は、許可された隔離 VM または人間の管理者セッションで確認する。

段階 1〜4 が成功しても段階 5 の実機結果を推測しない。
PoC、CI の静的検証、人間ゲートの状態を分けて記録する。
