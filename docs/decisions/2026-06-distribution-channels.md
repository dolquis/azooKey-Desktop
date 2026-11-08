# 配布チャネルと署名要否

- 種別: 決定記録
- 日付: 2026-06
- 関連 Issue: DEV-415、DEV-416、DEV-255
- 結論の正典: [`sideload-packaging-spec.md`](../sideload-packaging-spec.md) §0
- 移設元: `docs/sideload-packaging-spec.md` §0（commit `c8d9a5b` 時点）。原文をそのまま移設した

本文中の `§` は移設元の `docs/sideload-packaging-spec.md` の節を指す。

## 0. 配布方針（v1.0 MVP 確定 / 2026-06）

配布チャネルと署名要否を次のとおり確定する（Linear DEV-415 / DEV-416 / DEV-255）。

| チャネル | 形式 | 自前コード署名 | 位置づけ |
|---|---|---|---|
| **MVP 直接配布** | **MSI（WiX、未署名）** | 不要（任意） | MVP（評価配布）と v1.0 の既定形式。§4 が正典。GitHub Release で配布。TIP バイナリ署名は §0.1 |
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
