# ドキュメント案内

azooKey-Desktop（Windows ポート）の設計・計画ドキュメントの一覧と正典マップ。
各ドキュメントの役割と「何の正典か」をここで把握できる。

> **状態・進捗・優先度・課題トラッキングの正典は Linear**（team `Dev` / project
> 「azooKey Desktop / Windows IME MVP」）。repo docs は仕様・構造・定義の正典であり、状態は持たない。
> 運用は `AGENTS.md`「Linear 運用（管制塔）」を参照。

## 計画ドキュメント（`plans/`）

| ドキュメント | 役割 | 正典範囲 |
|---|---|---|
| [`plans/windows-port-roadmap.md`](../plans/windows-port-roadmap.md) | 開発計画・マイルストーンロードマップ | **唯一の開発計画ドキュメント。M0〜M62 の定義・受け入れ条件（定義）・依存関係・Phase 1〜7・開発基盤トラック・プライバシー / モデル管理 / 学習データ UI トラック・変換品質トラック・v1.0 実行計画・テスト体系の正典。進捗・状態・優先度は持たず Linear を正典とする** |

## アーキテクチャ・設計

| ドキュメント | 役割 |
|---|---|
| [`windows-tsf-host-architecture.md`](windows-tsf-host-architecture.md) | TSF TIP + Inference Host 分離設計、IPC メッセージ、実装ルール（スレッドモデル・例外耐性・互換性）の正典 |
| [`windows-port-asset-audit.md`](windows-port-asset-audit.md) | M0 以前の初回調査。現行設計の正典ではない参考資料。参照先 macOS ソースは `legacy/` に保全 |
| [`zenzai-gpu-route.md`](zenzai-gpu-route.md) | Zenzai 推論形式の特定と GPU 化ルート決定 |

## 機能仕様（`*-spec.md`）

| ドキュメント | 対応フェーズ／マイルストーン |
|---|---|
| [`zenzai-inference-spec.md`](zenzai-inference-spec.md) | Phase 3（M8）Zenzai 推論コントラクト（プロンプト・制約デコード・n-best・多ソース候補統合・性能予算） |
| [`legacy-parity-spec.md`](legacy-parity-spec.md) | Phase 5（M13〜M19）レガシー parity 復元 |
| [`rich-features-spec.md`](rich-features-spec.md) | 横断テーマ X-1〜X-4（リッチ化）。M48 と統合 |
| [`tsf-deep-integration-spec.md`](tsf-deep-integration-spec.md) | Phase 6-A（M20〜M23）TSF 深部統合 |
| [`copilot-pc-backend-spec.md`](copilot-pc-backend-spec.md) | Phase 6-B（M24〜M27）Copilot+ PC / NPU バックエンド |
| [`native-ui-spec.md`](native-ui-spec.md) | Phase 6-C（M26）ネイティブ UI |
| [`sideload-packaging-spec.md`](sideload-packaging-spec.md) | Phase 7（M28〜M34）サイドロード配信 |
| [`dev-infrastructure-spec.md`](dev-infrastructure-spec.md) | 開発基盤・品質強化トラック（M37〜M43 + M44/M47/M50/M51）ビルド再現性・CI・IPC/JSON 堅牢化・観測性・Host 可用性・診断ウィザード・復旧 UX・互換性テスト・レイテンシトレーサ |
| [`typo-correction-learning-spec.md`](typo-correction-learning-spec.md) | 追加機能 M35（v1 基本タイプミス学習）+ 変換品質 M55（v2 統合補正エンジン） |
| [`auto-word-registration-spec.md`](auto-word-registration-spec.md) | 追加機能 M36-A / M36-B（新語自動取得）+ 変換品質 M53（§14 辞書層全体再設計 / §15 物理層: double-array trie・`.azdic` 直列化・マルチソースビルド） |
| [`model-management-spec.md`](model-management-spec.md) | プライバシー / モデル管理 / 学習データ UI トラック M45（Zenzai モデル管理 UI） |
| [`privacy-and-secure-input-spec.md`](privacy-and-secure-input-spec.md) | 同トラック M46（プライバシー / セーフ入力モード） |
| [`app-profile-spec.md`](app-profile-spec.md) | 追加機能 M48（アプリ別入力プロファイル） |
| [`learning-data-management-spec.md`](learning-data-management-spec.md) | 同トラック M49（学習データ可視化・バックアップ） |
| [`conversion-quality-benchmark-spec.md`](conversion-quality-benchmark-spec.md) | 変換品質トラック M52（評価ベンチ） |
| [`user-learning-enhancement-spec.md`](user-learning-enhancement-spec.md) | 同トラック M54（ユーザー学習強化）+ §14（学習ストアの reading-keyed 二層化・前方一致 lookup 契約・M15 予測候補への供給） |
| [`neural-reranker-spec.md`](neural-reranker-spec.md) | 同トラック M56（Track A: Tiny Neural Reranker）+ NllScorer トラック（Track B §B1〜§B12: ロード済み Zenzai で辞書由来候補を NLL リランク。M 番号なし・既定 OFF） |
| [`modernbert-ja-scoring-spec.md`](modernbert-ja-scoring-spec.md) | 同トラック M57（ModernBERT-Ja 候補スコアリング） |
| [`romaji-batch-conversion-spec.md`](romaji-batch-conversion-spec.md) | 追加機能 M58（ローマ字一括変換：M58-A コア / M58-B 長文・文節再変換 / M58-C AI 整文） |
| [`dynamic-punctuation-spec.md`](dynamic-punctuation-spec.md) | 追加機能 M59（動的自動句読点：ライブ変換中の句読点動的挿入・削除） |
| [`inline-english-candidate-spec.md`](inline-english-candidate-spec.md) | 追加機能 M60（ローマ字入力中インライン英単語候補：1 語単位の候補注入） |
| [`bracket-pairing-spec.md`](bracket-pairing-spec.md) | 追加機能 M61（自動カッコペアリング：開きカッコ自動補完 + カーソル内側配置・スキップ・空ペア削除。M61-A コア / M61-B 外部化・アプリ互換） |
| [`candidate-rewriter-spec.md`](candidate-rewriter-spec.md) | 追加機能 M62（候補リライター層：§1〜§5 M62-A 数字リライターと共通方針・注釈・ライセンス境界 / §6〜§17 M62-D 絵文字リライターのデータ形式・Host 配信境界と IPC・検索とランキング・候補窓 UX・`:trigger` 状態機械・確定時の学習の扱い / §18 M62-B カタカナリライターの純かな判定・半角写像と縮退・設定キー・既定 OFF の不変条件。M62-B の英字分は [`inline-english-candidate-spec.md`](inline-english-candidate-spec.md) が正典） |

## 運用

| ドキュメント | 役割 |
|---|---|
| [`debugging.md`](debugging.md) | ビルド・ベンチ・手動確認・ログ収集・CI・典型トラブル |
| [`licensing-policy.md`](licensing-policy.md) | 第三者資産 attribution 規約（三層 attribution・データヘッダ・採用ワークフロー）の正典。集約表記の実体はルート [`THIRD_PARTY_LICENSES`](../THIRD_PARTY_LICENSES) |
| Linear（外部・team `Dev`） | 課題・進捗・状態・優先度・サイクルの正典（管制塔）。`AGENTS.md`「Linear 運用（管制塔）」参照 |

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
| 追加機能 | 差別化機能（Phase 連番とは独立） | M35 / M36-A・M36-B / M48 / M58 / M59 / M60 / M61 / M62 |
| 開発基盤 | ビルド再現性・CI・IPC 堅牢化・観測性・可用性・診断・復旧 UX・互換性テスト・trace（Phase 連番とは独立） | M37〜M43 + M44/M47/M50/M51 |
| プライバシー / モデル管理 / 学習データ UI | Phase 5/6/7 の既存 M に依存する付加機能 | M45 / M46 / M49 |
| 変換品質 | 評価ベンチ・辞書・学習強化・打ち間違え統合・Tiny Reranker・ModernBERT スコアリング（Phase 連番とは独立） | M52〜M57 |
