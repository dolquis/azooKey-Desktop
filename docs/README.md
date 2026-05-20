# ドキュメント案内

azooKey-Desktop（Windows ポート）の設計・計画ドキュメントの一覧と正典マップ。
各ドキュメントの役割と「何の正典か」をここで把握できる。

## 計画ドキュメント（`plans/`）

| ドキュメント | 役割 | 正典範囲 |
|---|---|---|
| [`plans/windows-port-roadmap.md`](../plans/windows-port-roadmap.md) | マイルストーン・ロードマップ | **M0〜M34 の定義・受け入れ条件・依存関係・Phase 1/2/3・テスト体系の正典** |
| [`plans/development-plan.md`](../plans/development-plan.md) | v1.0 までの実行計画 | Phase A〜D の実行順・直近タスク・検証手順。マイルストーンの状態は roadmap を正典とする |

## アーキテクチャ・設計

| ドキュメント | 役割 |
|---|---|
| [`windows-tsf-host-architecture.md`](windows-tsf-host-architecture.md) | TSF TIP + Inference Host 分離設計、IPC メッセージ、実装ルール（スレッドモデル・例外耐性・互換性）の正典 |
| [`windows-port-asset-audit.md`](windows-port-asset-audit.md) | M0 以前の macOS 資産の流用可否棚卸し（初回調査・参考） |
| [`zenzai-gpu-route.md`](zenzai-gpu-route.md) | Zenzai 推論形式の特定と GPU 化ルート決定 |
| [`segment-edit-upstream.md`](segment-edit-upstream.md) | 文節エディットの上流計画（macOS 向け、Windows MVP 対象外・参考） |

## 機能仕様（`*-spec.md`）

| ドキュメント | 対応フェーズ／マイルストーン |
|---|---|
| [`legacy-parity-spec.md`](legacy-parity-spec.md) | Phase 1（M13〜M19）レガシー parity 復元 |
| [`rich-features-spec.md`](rich-features-spec.md) | 横断テーマ X-1〜X-4（リッチ化） |
| [`tsf-deep-integration-spec.md`](tsf-deep-integration-spec.md) | Phase 2-A（M20〜M23）TSF 深部統合 |
| [`copilot-pc-backend-spec.md`](copilot-pc-backend-spec.md) | Phase 2-B（M24〜M27）Copilot+ PC / NPU バックエンド |
| [`native-ui-spec.md`](native-ui-spec.md) | Phase 2-C（M26）ネイティブ UI |
| [`sideload-packaging-spec.md`](sideload-packaging-spec.md) | Phase 3（M28〜M34）サイドロード配信 |

## 運用

| ドキュメント | 役割 |
|---|---|
| [`debugging.md`](debugging.md) | ビルド・ベンチ・手動確認・ログ収集・CI・典型トラブル |

## Phase 命名の対応

`plans/` の 2 つの計画ドキュメントは異なる Phase 命名を用いる。両者は別スコープを
指すため、以下の対応で読み分ける。

| development-plan.md | windows-port-roadmap.md | スコープ |
|---|---|---|
| Phase A〜D | M0〜M12 | v1.0（MSIX 配布可能な最小 IME）までの段階 |
| Phase E ≒ Phase 1 | M13〜M19 | v1.0 以降: レガシー parity 復元 |
| Phase F ≒ Phase 2 | M20〜M27 | v1.0 以降: Windows ネイティブ深耕 |
| Phase G ≒ Phase 3 | M28〜M34 | v1.0 以降: サイドロード配信 |
