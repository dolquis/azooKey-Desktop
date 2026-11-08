# コード署名の調達経路の選定

- 種別: 決定記録
- 日付: 2026-06（費用対効果の前提の更新は 2026-08）
- 関連 Issue: DEV-255、DEV-783、DEV-765
- 結論の正典: [`sideload-packaging-spec.md`](../sideload-packaging-spec.md) §2.0
- 移設元: `docs/sideload-packaging-spec.md` §2.0（commit `c8d9a5b` 時点）。原文をそのまま移設した

本文中の `§` は移設元の `docs/sideload-packaging-spec.md` の節を指す。

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
