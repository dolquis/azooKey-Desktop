# ドキュメント案内

azooKey-Desktop（Windows ポート）の設計・計画ドキュメントの一覧と正典マップ。
各ドキュメントの役割と「何の正典か」をここで把握できる。

> **状態・進捗・優先度・課題トラッキングの正典は Linear**（team `Dev` / project
> 「azooKey Desktop / Windows IME MVP」）。repo docs は仕様・構造・定義の正典であり、状態は持たない。
> 運用規約は [`linear-conventions.md`](linear-conventions.md) を参照。

## エージェント規約

| ドキュメント | 適用範囲 |
|---|---|
| [`docs/AGENTS.md`](AGENTS.md) | `docs/` の正典、索引、lint 規約 |
| [`docs/CLAUDE.md`](CLAUDE.md) | `docs/AGENTS.md` を Claude Code へ届ける import 専用 |
| [`plans/AGENTS.md`](../plans/AGENTS.md) | `plans/` の roadmap と調査資料の規約 |
| [`plans/CLAUDE.md`](../plans/CLAUDE.md) | `plans/AGENTS.md` を Claude Code へ届ける import 専用 |

## 計画ドキュメント（`plans/`）

| ドキュメント | 役割 | 正典範囲 |
|---|---|---|
| [`plans/windows-port-roadmap.md`](../plans/windows-port-roadmap.md) | 開発計画・マイルストーンロードマップ | **マイルストーン定義、依存関係、受け入れ条件、リスクの正典。進捗と状態は Linear を参照** |
| [`plans/karukan-comparison-report.md`](../plans/karukan-comparison-report.md) | karukan 比較レポート | 更新しない参考スナップショット。開発計画は roadmap を参照 |

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
| [`ai-backend-spec.md`](ai-backend-spec.md) | M16 / M58-C / X-3-3 が共有する `AiBackend` 契約 |
| [`legacy-parity-spec.md`](legacy-parity-spec.md) | Phase 5（M13〜M19）のレガシー parity。§12 は M5 の UI-less / `pbShow` 実機確認 |
| [`rich-features-spec.md`](rich-features-spec.md) | 横断テーマ X-1〜X-4（リッチ化）。M48 と統合 |
| [`tsf-deep-integration-spec.md`](tsf-deep-integration-spec.md) | Phase 6-A（M20〜M23）TSF 深部統合 |
| [`copilot-pc-backend-spec.md`](copilot-pc-backend-spec.md) | Phase 6-B（M24〜M27）Copilot+ PC / NPU バックエンド |
| [`native-ui-spec.md`](native-ui-spec.md) | Phase 6-C（M26）ネイティブ UI |
| [`sideload-packaging-spec.md`](sideload-packaging-spec.md) | Phase 7（M28〜M34）サイドロード配信 |
| [`dev-infrastructure-spec.md`](dev-infrastructure-spec.md) | M37〜M51 のビルド、CI、IPC、診断基盤。§12 は診断ウィザード |
| [`typo-correction-learning-spec.md`](typo-correction-learning-spec.md) | 追加機能 M35（v1 基本タイプミス学習）+ 変換品質 M55（v2 統合補正エンジン） |
| [`auto-word-registration-spec.md`](auto-word-registration-spec.md) | M36 新語自動取得と M53 辞書層再設計（§14 論理層、§15 物理層） |
| [`model-management-spec.md`](model-management-spec.md) | M45 Zenzai モデル管理 UI（§3.1.1 カタログ、§3.1.2 GGUF 取得） |
| [`privacy-and-secure-input-spec.md`](privacy-and-secure-input-spec.md) | 同トラック M46（プライバシー / セーフ入力モード） |
| [`app-profile-spec.md`](app-profile-spec.md) | 追加機能 M48（アプリ別入力プロファイル） |
| [`learning-data-management-spec.md`](learning-data-management-spec.md) | M49 学習データ可視化とバックアップ。§11 は終了時の flush 保証境界 |
| [`conversion-quality-benchmark-spec.md`](conversion-quality-benchmark-spec.md) | 変換品質トラック M52（評価ベンチ） |
| [`user-learning-enhancement-spec.md`](user-learning-enhancement-spec.md) | M54 ユーザー学習強化。§14 は reading-keyed lookup と M15 予測候補への供給 |
| [`neural-reranker-spec.md`](neural-reranker-spec.md) | M56 Tiny Neural Reranker と NllScorer 契約 |
| [`modernbert-ja-scoring-spec.md`](modernbert-ja-scoring-spec.md) | 同トラック M57（ModernBERT-Ja 候補スコアリング） |
| [`romaji-batch-conversion-spec.md`](romaji-batch-conversion-spec.md) | 追加機能 M58（ローマ字一括変換：M58-A コア / M58-B 長文・文節再変換 / M58-C AI 整文） |
| [`dynamic-punctuation-spec.md`](dynamic-punctuation-spec.md) | 追加機能 M59（動的自動句読点：ライブ変換中の句読点動的挿入・削除） |
| [`inline-english-candidate-spec.md`](inline-english-candidate-spec.md) | 追加機能 M60（ローマ字入力中インライン英単語候補：1 語単位の候補注入） |
| [`bracket-pairing-spec.md`](bracket-pairing-spec.md) | M61-A 自動カッコペアリングと M61-B 外部化・アプリ互換 |
| [`candidate-rewriter-spec.md`](candidate-rewriter-spec.md) | M62-A 数字（§1〜5）、M62-D 絵文字（§6〜17）、M62-B カタカナ（§18）、M62-C 記号（§19）。英字分は [`inline-english-candidate-spec.md`](inline-english-candidate-spec.md) が正典 |

## 運用

| ドキュメント | 役割 |
|---|---|
| [`debugging.md`](debugging.md) | ビルドの差分（Release / Linux）・ベンチ・手動確認・ログ収集・CI・典型トラブル |
| [`licensing-policy.md`](licensing-policy.md) | 第三者資産 attribution と採用手順の正典。集約表記は [`THIRD_PARTY_LICENSES`](../THIRD_PARTY_LICENSES) |
| [`linear-conventions.md`](linear-conventions.md) | Linear のラベル、状態遷移、監査、Project Delta（§13）の正典 |
| Linear（外部・team `Dev`） | 課題・進捗・状態・優先度・サイクルの正典（管制塔） |

### 恒常 runbook（`docs/handoff/`）

一回限りの引き継ぎ作業記録は完了後に [`docs/archive/`](archive/) へ移す（本表には載せない）。
以下は継続的に使う実機検証・診断手順(一部は対応する spec が正典として参照する)。

| ドキュメント | 役割 |
|---|---|
| [`handoff/windows-diagnostics-playbook.md`](handoff/windows-diagnostics-playbook.md) | `dev-infrastructure-spec.md` §12 の診断ウィザードと §4.6.2 の Application Verifier runbook |
| [`handoff/human-gate-batch-runbook.md`](handoff/human-gate-batch-runbook.md) | `gate:human-required` の実機検証をまとめる runbook |
| [`handoff/hyper-v-tip-verification.md`](handoff/hyper-v-tip-verification.md) | Hyper-V VM 上での TIP 登録・入力確認の実機検証手順 |
| [`handoff/hyper-v-vm-verification-plan.md`](handoff/hyper-v-vm-verification-plan.md) | Hyper-V VM 検証環境そのものの構築・スパイク計画 |
| [`handoff/dev32-verification-checklist.md`](handoff/dev32-verification-checklist.md) | VM 検証パッケージ（`make-vm-verify-package.ps1`）に同梱する汎用チェックリスト |
| [`handoff/claude-code-web-setup.md`](handoff/claude-code-web-setup.md) | Claude Code on the web のセットアップ手順 |
| [`handoff/agent-tooling-setup.md`](handoff/agent-tooling-setup.md) | Claude Code / Codex の MCP、ホスト前提、doctor、Human Gate の接続手順 |

## Phase 一覧

Phase の構成（Phase 1〜7 + 追加機能 + 開発基盤 + プライバシー / モデル管理 /
学習データ UI + 変換品質）と、各 Phase が対象とするマイルストーン番号は
[`plans/windows-port-roadmap.md`](../plans/windows-port-roadmap.md) を正典とする。
v1.0 までの Phase 1〜4 は同ファイル「v1.0 までの実行計画（Phase 1〜4）」の索引表、
Phase 5 以降は各 Phase 章の見出しが対象 M 範囲を示す。本 README は Phase 表を
複製しない（roadmap 側の更新に追随できなくなるため）。
