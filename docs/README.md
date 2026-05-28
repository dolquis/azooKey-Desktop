# ドキュメント案内

azooKey-Desktop（Windows ポート）の設計・計画ドキュメントの一覧と正典マップ。
各ドキュメントの役割と「何の正典か」をここで把握できる。

## 計画ドキュメント（`plans/`）

| ドキュメント | 役割 | 正典範囲 |
|---|---|---|
| [`plans/windows-port-roadmap.md`](../plans/windows-port-roadmap.md) | 開発計画・マイルストーンロードマップ | **唯一の開発計画ドキュメント。M0〜M57 の定義・受け入れ条件・依存関係・Phase 1〜7・開発基盤トラック・プライバシー / モデル管理 / 学習データ UI トラック・変換品質トラック・v1.0 実行計画・テスト体系の正典** |

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
| [`rich-features-spec.md`](rich-features-spec.md) | 横断テーマ X-1〜X-4（リッチ化）。M48 と統合 |
| [`tsf-deep-integration-spec.md`](tsf-deep-integration-spec.md) | Phase 6-A（M20〜M23）TSF 深部統合 |
| [`copilot-pc-backend-spec.md`](copilot-pc-backend-spec.md) | Phase 6-B（M24〜M27）Copilot+ PC / NPU バックエンド |
| [`native-ui-spec.md`](native-ui-spec.md) | Phase 6-C（M26）ネイティブ UI |
| [`sideload-packaging-spec.md`](sideload-packaging-spec.md) | Phase 7（M28〜M34）サイドロード配信 |
| [`dev-infrastructure-spec.md`](dev-infrastructure-spec.md) | 開発基盤・品質強化トラック（M37〜M43 + M44/M47/M50/M51）ビルド再現性・CI・IPC/JSON 堅牢化・観測性・Host 可用性・診断ウィザード・復旧 UX・互換性テスト・レイテンシトレーサ |
| [`typo-correction-learning-spec.md`](typo-correction-learning-spec.md) | 追加機能 M35（v1 基本タイプミス学習）+ 変換品質 M55（v2 統合補正エンジン） |
| [`auto-word-registration-spec.md`](auto-word-registration-spec.md) | 追加機能 M36-A / M36-B（新語自動取得）+ 変換品質 M53（辞書層全体再設計） |
| [`model-management-spec.md`](model-management-spec.md) | プライバシー / モデル管理 / 学習データ UI トラック M45（Zenzai モデル管理 UI） |
| [`privacy-and-secure-input-spec.md`](privacy-and-secure-input-spec.md) | 同トラック M46（プライバシー / セーフ入力モード） |
| [`app-profile-spec.md`](app-profile-spec.md) | 追加機能 M48（アプリ別入力プロファイル） |
| [`learning-data-management-spec.md`](learning-data-management-spec.md) | 同トラック M49（学習データ可視化・バックアップ） |
| [`conversion-quality-benchmark-spec.md`](conversion-quality-benchmark-spec.md) | 変換品質トラック M52（評価ベンチ） |
| [`user-learning-enhancement-spec.md`](user-learning-enhancement-spec.md) | 同トラック M54（ユーザー学習強化） |
| [`neural-reranker-spec.md`](neural-reranker-spec.md) | 同トラック M56（Tiny Neural Reranker） |
| [`modernbert-ja-scoring-spec.md`](modernbert-ja-scoring-spec.md) | 同トラック M57（ModernBERT-Ja 候補スコアリング） |

## 運用

| ドキュメント | 役割 |
|---|---|
| [`debugging.md`](debugging.md) | ビルド・ベンチ・手動確認・ログ収集・CI・典型トラブル |

## Phase 一覧

`plans/windows-port-roadmap.md` の Phase は通し連番（Phase 1〜7）+ 追加機能 +
独立トラックで構成される。

| Phase | スコープ | M 範囲 |
|---|---|---|
| Phase 1 | TIP 基盤完成 | M1〜M4 |
| Phase 2 | 候補選択と確定動線 | M5/M6/M10 |
| Phase 3 | 実 Zenzai と辞書 UI | M8/M9 |
| Phase 4 | 配布可能化 v1.0 | M11/M12 |
| Phase 5 | レガシー parity 復元 | M13〜M19 |
| Phase 6 | Windows ネイティブ深耕 | M20〜M27 |
| Phase 7 | サイドロード配信 | M28〜M34 |
| 追加機能 | 差別化機能（Phase 連番とは独立） | M35 / M36-A・M36-B / M48 |
| 開発基盤 | ビルド再現性・CI・IPC 堅牢化・観測性・可用性・診断・復旧 UX・互換性テスト・trace（Phase 連番とは独立） | M37〜M43 + M44/M47/M50/M51 |
| プライバシー / モデル管理 / 学習データ UI | Phase 5/6/7 の既存 M に依存する付加機能 | M45 / M46 / M49 |
| 変換品質 | 評価ベンチ・辞書・学習強化・打ち間違え統合・Tiny Reranker・ModernBERT スコアリング（Phase 連番とは独立） | M52〜M57 |
