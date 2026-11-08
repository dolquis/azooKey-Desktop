# 開発基盤トラックで採らなかった提案（§11.1〜§11.7）

- 種別: 決定記録
- 日付: 2026-07（§11.5〜§11.7 の原記録の日付。§11.1〜§11.4 は原記録に日付が無い）
- 関連 Issue: DEV-99、DEV-101
- 結論の正典: [`dev-infrastructure-spec.md`](../dev-infrastructure-spec.md) §11
- 移設元: `docs/dev-infrastructure-spec.md` §11 冒頭と §11.1〜§11.7（§11.2.1 を除く）（commit `c8d9a5b` 時点）。原文をそのまま移設した

本文中の `§` は移設元の `docs/dev-infrastructure-spec.md` の節を指す。

## 11. 不採用とした提案

レビュー指摘のうち、本トラックで採用しない / 修正して採用するものを理由と
ともに記録する。

### 11.1 vcpkg.json の一括導入 — 修正採用

レビューは `vcpkg.json` でビルド依存基盤を作ることを提案する。本プロジェクト
は**オフラインビルド原則**（§1）を持ち、`AZOOKEY_FETCH_GOOGLETEST` が意図的に
オプトインである。vcpkg の必須化はこの原則と衝突するため、必須化はしない。
依存ごとに header-only 優先・`FetchContent`/submodule オプトインで導入する
（§3）。`vcpkg.json` は任意の利便ファイルとしてのみ許容する。

### 11.2 自前 JSON パーサの即時全置換 — 修正採用

レビュー提案 A（`nlohmann-json` への即時全置換）は採らない。自前パーサは
軽量・低依存という利点があり、IPC の JSON は大きくない。提案 B（堅牢化 +
fuzz/malformed テスト）を主とする（§6）。先送りしていた置換の判断は §11.2.1 で確定した。

### 11.3 Zenzai runtime 抽象 `IModelRuntime` の新設 — 不採用

レビューは `IModelRuntime` インターフェース（`SimpleRuntime` /
`LlamaCpuRuntime` / `LlamaCudaRuntime` …）の新設を提案する。しかし
`core::IConverter` が既に同じ役割（変換バックエンドの差し替え可能境界）を
担い、M8 も Zenzai を `IConverter` 実装として `SimpleConverter` と差し替え
る方針が確定している。`IModelRuntime` 新設は二重抽象になるため不採用。
バックエンド別実装の内部構造は M8 / M24 の範囲で `IConverter` 実装として
整理する。

### 11.4 packaging / signing 足場の新設 — 不採用（重複）

レビューは packaging / signing の足場作りを提案するが、MSIX・WiX・署名 CI・
DPAPI 暗号化は既存マイルストーン M11 / M12 / M28〜M34 でカバー済みである。
重複するため本トラックでは新設しない。開発用 `regsvr32` スクリプトは
`scripts/register-dev.ps1` / `unregister-dev.ps1` へ命名分離済み（DEV-101 / M28）。

### 11.5 clang-tidy / CodeQL の必須化 — 見送り

CI への clang-tidy / CodeQL の**必須化**は導入・調整コストが高い。本トラックでは
`clang-format`（変更行ゲート）までを必須とし、clang-tidy / CodeQL は必須ゲートに
しない（§4.3）。変更 TU 全体の clang-tidy 解析は、既存負債で
PR が止まるのを避けつつ変更コード品質を上げる中間策として、`cpp-tidy` ジョブに
**advisory（非ブロッキング）**で導入済み。所見の可視化に留め、必須ゲート化・全体化
（全ソースへの拡大、CodeQL 追加）は将来判断とし Linear で追跡する（2026-07 開発基盤
ツール導入 第2弾）。

同じ段階導入方針を Release バイナリの BinSkim にも適用する。現時点では
`continue-on-error` の advisory とし、SARIF・job summary・artifact に所見を残す。
未署名に由来する `BA2022` だけを抑止し、他のルール結果を隠さない。必須ゲート化は
所見の基準線を整理し、ツール取得と解析の安定性を確認した後の将来判断とする。

### 11.6 IPC payload の Protobuf 即時移行 — 不採用

外部リサーチ提案（2026-07）は Named Pipe transport を維持したまま payload を
versioned Protobuf へ移行する案を挙げる。しかし本プロジェクトの IPC は同一
マシン内・単一実装・少数メッセージであり、schema 進化の恩恵よりも、コード生成
ツールチェーン追加がオフラインビルド原則（§1）へ与える影響と移行コストが上回る。
自前 JSON パーサを §6 の境界コーパス + 有界スモークで堅牢化する路線（§11.2 と整合）を
主とし、Protobuf 化は将来判断とする。設定アプリ実装・fuzz 整備の後に再評価する。

### 11.7 設定アプリの C#/.NET 9 化 — 不採用

外部リサーチ提案（2026-07）は設定アプリを C#/.NET 9 + WinUI 3 にする案を挙げる。
しかし設定アプリの UI フレームワークは **DEV-99 / D-03 で WinUI 3（C++/WinRT）に
確定済み**（`plans/windows-port-roadmap.md`「リスクと不確実性」/
`docs/sideload-packaging-spec.md` §3.0）であり、既存 C++/WinRT スタックとの親和性・
配布形態整合を根拠とする。TIP/Host と別プロセスで IPC 連携する点は C# でも成立するが、
確定済みの決定を覆すだけの決定的な差は現時点で無いため、C++/WinRT を維持する。
