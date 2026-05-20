# ドキュメント案内

azooKey-Desktop（Windows ポート）の設計・計画ドキュメントの一覧と正典マップ。
各ドキュメントの役割と「何の正典か」をここで把握できる。

## 計画ドキュメント（`plans/`）

| ドキュメント | 役割 | 正典範囲 |
|---|---|---|
| [`plans/windows-port-roadmap.md`](../plans/windows-port-roadmap.md) | 開発計画・マイルストーンロードマップ | **唯一の開発計画ドキュメント。M0〜M36 の定義・受け入れ条件・依存関係・Phase 1〜7・v1.0 実行計画・テスト体系の正典** |

## アーキテクチャ・設計

| ドキュメント | 役割 |
|---|---|
| [`windows-tsf-host-architecture.md`](windows-tsf-host-architecture.md) | TSF TIP + Inference Host 分離設計、IPC メッセージ、実装ルール（スレッドモデル・例外耐性・互換性）の正典 |
| [`windows-port-asset-audit.md`](windows-port-asset-audit.md) | M0 以前の初回調査。現行設計の正典ではない参考資料。参照先 macOS ソースは `legacy/` に保全 |
| [`zenzai-gpu-route.md`](zenzai-gpu-route.md) | Zenzai 推論形式の特定と GPU 化ルート決定 |

## 機能仕様（`*-spec.md`）

| ドキュメント | 対応フェーズ／マイルストーン |
|---|---|
| [`legacy-parity-spec.md`](legacy-parity-spec.md) | Phase 5（M13〜M19）レガシー parity 復元 |
| [`rich-features-spec.md`](rich-features-spec.md) | 横断テーマ X-1〜X-4（リッチ化） |
| [`tsf-deep-integration-spec.md`](tsf-deep-integration-spec.md) | Phase 6-A（M20〜M23）TSF 深部統合 |
| [`copilot-pc-backend-spec.md`](copilot-pc-backend-spec.md) | Phase 6-B（M24〜M27）Copilot+ PC / NPU バックエンド |
| [`native-ui-spec.md`](native-ui-spec.md) | Phase 6-C（M26）ネイティブ UI |
| [`sideload-packaging-spec.md`](sideload-packaging-spec.md) | Phase 7（M28〜M34）サイドロード配信 |

## 運用

| ドキュメント | 役割 |
|---|---|
| [`debugging.md`](debugging.md) | ビルド・ベンチ・手動確認・ログ収集・CI・典型トラブル |

## Phase 一覧

`plans/windows-port-roadmap.md` の Phase は通し連番（Phase 1〜7）+ 追加機能で
構成される。

| Phase | スコープ | M 範囲 |
|---|---|---|
| Phase 1 | TIP 基盤完成 | M1〜M4 |
| Phase 2 | 候補選択と確定動線 | M5/M6/M10 |
| Phase 3 | 実 Zenzai と辞書 UI | M8/M9 |
| Phase 4 | 配布可能化 v1.0 | M11/M12 |
| Phase 5 | レガシー parity 復元 | M13〜M19 |
| Phase 6 | Windows ネイティブ深耕 | M20〜M27 |
| Phase 7 | サイドロード配信 | M28〜M34 |
| 追加機能 | 差別化機能（Phase 連番とは独立） | M35 / M36-A・M36-B |
