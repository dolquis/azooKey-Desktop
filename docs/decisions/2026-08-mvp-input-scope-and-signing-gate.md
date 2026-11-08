# MVP の入力対象スコープと署名ゲート

- 種別: 決定記録
- 日付: 2026-08（実機検証は 2026-08-14）
- 関連 Issue: DEV-783、DEV-765、DEV-673
- 結論の正典: [`sideload-packaging-spec.md`](../sideload-packaging-spec.md) §0.1
- 移設元: `docs/sideload-packaging-spec.md` §0.1（32 bit の小節を除く）（commit `c8d9a5b` 時点）。原文をそのまま移設した

本文中の `§` は移設元の `docs/sideload-packaging-spec.md` の節を指す。

### 0.1 MVP の入力対象スコープと署名ゲート（DEV-783 で確定 / 2026-08）

MVP が入力先として保証するのは **Win32 デスクトップアプリに限る**。
**パッケージ化された UWP / Microsoft Store アプリ**での入力は MVP の対象外とし、v1.0 以降へ送る。

除外の境界は「AppContainer を使うか」ではなく「パッケージ化された UWP / Store アプリか」で引く。
Edge は renderer のサンドボックス化に AppContainer を使うが、入力欄をホストするのは Win32 プロセスであり、2026-08-14 の実機検証でも入力が成立している。
したがって Edge は対象内とする。

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
