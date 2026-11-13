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
| [`plans/AGENTS.md`](../plans/AGENTS.md) | `plans/` の roadmap の規約 |
| [`plans/CLAUDE.md`](../plans/CLAUDE.md) | `plans/AGENTS.md` を Claude Code へ届ける import 専用 |

## 計画ドキュメント（`plans/`）

| ドキュメント | 役割 | 正典範囲 |
|---|---|---|
| [`plans/windows-port-roadmap.md`](../plans/windows-port-roadmap.md) | 開発計画・マイルストーンロードマップ | **マイルストーン定義、依存関係、受け入れ条件、リスクの正典。進捗と状態は Linear を参照** |

## アーキテクチャ・設計

| ドキュメント | 役割 |
|---|---|
| [`windows-tsf-host-architecture.md`](windows-tsf-host-architecture.md) | TSF TIP + Inference Host 分離設計、実装ルール（スレッドモデル・例外耐性・互換性）の正典。IPC の型から正典 spec の節への索引を持つ（payload の正典は `ipc/include/azookey/ipc/Payloads.h`） |
| [`windows-port-asset-audit.md`](windows-port-asset-audit.md) | M0 以前の初回調査。現行設計の正典ではない参考資料。参照先 macOS ソースは `legacy/` に保全 |
| [`karukan-comparison-report.md`](karukan-comparison-report.md) | karukan（`togatoga/karukan`）との比較調査。更新しない参考スナップショットであり正典ではない。開発計画は roadmap、取り込み状態は Linear を参照 |
| [`macos-input-parity-audit-report.md`](macos-input-parity-audit-report.md) | 入力体験（キー操作・状態遷移・表示）を macOS 版（`legacy/`）と比べた監査。更新しない参考スナップショットであり正典ではない。仕様は各 spec、対応の状態は Linear（DEV-1810 と子課題）を参照 |
| [`mixed-english-input-feasibility-report.md`](mixed-english-input-feasibility-report.md) | 日英混在ローマ字入力の自動変換（M63）の実現可能性と先行事例の調査。更新しない参考スナップショットであり正典ではない。定義と受け入れ条件は roadmap の M63、機能仕様は [`mixed-english-auto-conversion-spec.md`](mixed-english-auto-conversion-spec.md) を参照 |
| [`zenzai-gpu-route.md`](zenzai-gpu-route.md) | Zenzai の重み形式と推論エンジン経路の結論、実装構成（`AZOOKEY_BACKEND`、llama.cpp 接続）、計測ゲートの正典。決定の根拠は [`decisions/2026-05-20-zenzai-engine-route.md`](decisions/2026-05-20-zenzai-engine-route.md) |

## 機能仕様（`*-spec.md`）

| ドキュメント | 対応フェーズ／マイルストーン |
|---|---|
| [`zenzai-inference-spec.md`](zenzai-inference-spec.md) | Phase 3（M8）Zenzai 推論コントラクト（プロンプト・制約デコード・n-best・多ソース候補統合・性能予算） |
| [`ai-backend-spec.md`](ai-backend-spec.md) | M16 / M58-C / X-3-3 が共有する `AiBackend` 契約 |
| [`legacy-parity-spec.md`](legacy-parity-spec.md) | Phase 5（M13〜M19）のレガシー parity。§1.2・§1.4 の字種指定確定（F6〜F10）は M64 |
| [`rich-features-spec.md`](rich-features-spec.md) | 横断テーマ X-1〜X-4（リッチ化）。M48 と統合 |
| [`tsf-deep-integration-spec.md`](tsf-deep-integration-spec.md) | Phase 6-A（M20〜M23）TSF 深部統合。M3（§5.6・§5.7 の単一 DisplayAttribute）、M5（§2.0・§2.8〜§2.11 の UI-less 最小契約）、M64（§3.6 の字種指定確定〔F6〜F10〕）も扱う。読みが無いときの Space の幅（`fullWidthSpace`）は §3.7、入力インジケーターとその右クリックのメニューは §4.5 |
| [`copilot-pc-backend-spec.md`](copilot-pc-backend-spec.md) | Phase 6-B（M24〜M27）Copilot+ PC / NPU バックエンド。§4 の推論エンジン選定は M8 と共有 |
| [`native-ui-spec.md`](native-ui-spec.md) | Phase 6-C（M26）ネイティブ UI。§4.4 は M16 の Magic Conversion プロンプト、§4.5 は M34 の API キー入力、§4.6 は M53 の設定アプリの辞書ライセンス導線 |
| [`sideload-packaging-spec.md`](sideload-packaging-spec.md) | MVP の配布方針と MSI・設定アプリ（M11 / M12）、Phase 7（M28〜M30、M32〜M34）の配布・署名・更新・観測。設定アプリのナビゲーション（ペインの構成と保存バー）と表示文字列の表記規則は §3.2 |
| [`dev-infrastructure-spec.md`](dev-infrastructure-spec.md) | M37〜M44・M47・M50・M51 のビルド、CI、IPC、観測性、Host 可用性、診断基盤。§12 は診断ウィザード（M44）、§13 はアプリ互換性ハーネス（M50） |
| [`typo-correction-learning-spec.md`](typo-correction-learning-spec.md) | 追加機能 M35（v1 基本タイプミス学習）+ 変換品質 M55（v2 統合補正エンジン） |
| [`auto-word-registration-spec.md`](auto-word-registration-spec.md) | M36 新語自動取得と M53 辞書層再設計（§14 論理層、§15 物理層） |
| [`model-management-spec.md`](model-management-spec.md) | M45 Zenzai モデル管理 UI（§3.1.1 カタログ、§3.1.2 GGUF 取得） |
| [`privacy-and-secure-input-spec.md`](privacy-and-secure-input-spec.md) | 同トラック M46（プライバシー / セーフ入力モード）。§7 の `crashReportConsent` は M33 |
| [`app-profile-spec.md`](app-profile-spec.md) | 追加機能 M48（アプリ別入力プロファイル） |
| [`learning-data-management-spec.md`](learning-data-management-spec.md) | M49 学習データ可視化とバックアップ。§11 は終了時の flush 保証境界、§12 は CommitObservation の配送保証 |
| [`conversion-quality-benchmark-spec.md`](conversion-quality-benchmark-spec.md) | 変換品質トラック M52（評価ベンチ） |
| [`user-learning-enhancement-spec.md`](user-learning-enhancement-spec.md) | M54 ユーザー学習強化。§14 は reading-keyed lookup と M15 予測候補への供給 |
| [`neural-reranker-spec.md`](neural-reranker-spec.md) | M56 Tiny Neural Reranker（Track A） |
| [`neural-reranker-nll-spec.md`](neural-reranker-nll-spec.md) | NllScorer 型ニューラルリランク（Track B） |
| [`modernbert-ja-scoring-spec.md`](modernbert-ja-scoring-spec.md) | 同トラック M57（ModernBERT-Ja 候補スコアリング） |
| [`romaji-batch-conversion-spec.md`](romaji-batch-conversion-spec.md) | 追加機能 M58（ローマ字一括変換：M58-A コア / M58-B 長文・文節再変換 / M58-C AI 整文） |
| [`dynamic-punctuation-spec.md`](dynamic-punctuation-spec.md) | 追加機能 M59（動的自動句読点：ライブ変換中の句読点動的挿入・削除） |
| [`inline-english-candidate-spec.md`](inline-english-candidate-spec.md) | 追加機能 M60（ローマ字入力中インライン英単語候補：1 語単位の候補注入）。§3・§4.3 は M62-B の英字分の正典 |
| [`bracket-pairing-spec.md`](bracket-pairing-spec.md) | M61-A 自動カッコペアリングと M61-B 外部化・アプリ互換 |
| [`mixed-english-auto-conversion-spec.md`](mixed-english-auto-conversion-spec.md) | 追加機能 M63（日英混在ローマ字入力の自動変換：M63-A 1 語 / M63-B 文中混在。分割器・IPC・順位・学習・設定） |
| [`candidate-rewriter-spec.md`](candidate-rewriter-spec.md) | M62-A 数字（§1〜5）、M62-D 絵文字（§6〜17）、M62-B カタカナ（§18）、M62-C 記号（§19）。英字分は [`inline-english-candidate-spec.md`](inline-english-candidate-spec.md) が正典 |

## 運用

| ドキュメント | 役割 |
|---|---|
| [`debugging.md`](debugging.md) | ビルドの差分（Release / Linux）・ベンチ・手動確認・ログ収集・CI・典型トラブル |
| [`test-inventory.md`](test-inventory.md) | CTest 登録テストと CTest 以外の自動検査の一覧の正典。`scripts/check_test_inventory.py` が CMake の登録、CI ワークフローのジョブ、`scripts/tests/` との一致を検査する。未解消のカバレッジギャップは roadmap「テスト体系」 |
| [`licensing-policy.md`](licensing-policy.md) | 第三者資産 attribution と採用手順、ビルドツールの利用条件の正典。集約表記は [`THIRD_PARTY_LICENSES`](../THIRD_PARTY_LICENSES) |
| [`linear-conventions.md`](linear-conventions.md) | Linear のラベル、状態遷移、監査、Project Delta（§13）の正典 |
| Linear（外部・team `Dev`） | 課題・進捗・状態・優先度・サイクルの正典（管制塔） |

### 恒常 runbook（`docs/handoff/`）

一回限りの引き継ぎ作業記録は完了後に [`docs/archive/`](archive/) へ移す（本表には載せない）。
以下は継続的に使う実機検証・診断手順(一部は対応する spec が正典として参照する)。

| ドキュメント | 役割 |
|---|---|
| [`handoff/windows-diagnostics-playbook.md`](handoff/windows-diagnostics-playbook.md) | `dev-infrastructure-spec.md` §12 の診断ウィザードと §4.6.2 の Application Verifier runbook |
| [`handoff/trending-asset-publication.md`](handoff/trending-asset-publication.md) | trending アセットの入力検証、ローカル HTTP 確認、手動公開・更新・ロールバック |
| [`handoff/human-gate-batch-runbook.md`](handoff/human-gate-batch-runbook.md) | `gate:human-required` の実機検証を 1 回の VM セッションでまとめるときのレーン設計、ゲート種別ごとの実行条件、検証メモの記録様式。課題の割当と課題別の確認項目は Linear が持つ |
| [`handoff/dev-machine-verification.md`](handoff/dev-machine-verification.md) | 実機検証を VM と開発機のどちらで行うかの判定基準（Linear の `env:vm` / `env:dev-machine`）と、開発機での前提・手順・後始末 |
| [`handoff/hyper-v-tip-verification.md`](handoff/hyper-v-tip-verification.md) | Hyper-V VM 上での TIP 登録・入力確認の実機検証手順。VM 構成、3 層の検証フロー、VM 内の TIP 登録をエージェントへ委任する条件と範囲の正典 |
| [`handoff/hyper-v-vm-verification-plan.md`](handoff/hyper-v-vm-verification-plan.md) | VM 検証資産の一覧と、エージェントが Hyper-V VM 操作を補助する前提（ホスト側の権限）とリスク |
| [`handoff/tip-manual-verification-checklist.md`](handoff/tip-manual-verification-checklist.md) | TIP の打鍵確認（コア A1〜A8、拡張 B1〜B7）と compat runner との分担。VM 検証パッケージ（`make-vm-verify-package.ps1`）に同梱する |
| [`handoff/agent-tooling-setup.md`](handoff/agent-tooling-setup.md) | Claude Code / Codex の MCP、ホスト前提、doctor、Human Gate の接続手順、Claude Code on the web の SessionStart hook |
| [`handoff/agent-orchestration.md`](handoff/agent-orchestration.md) | Claude Code / Codex でローカル agent を分担させるときの分割レベル、touched path からの route、snapshot 契約、返却形式、ロールバック、分担の例と、管制塔セッションと子セッションの役割分担 |
| [`handoff/parallel-worktree-runbook.md`](handoff/parallel-worktree-runbook.md) | 複数 worktree で同時に build / test / PR 作業をするときに共有される資源（sccache、`%TEMP%`、ミューテックス、Host のパイプ、TIP 登録）と回避策、main の取り込み |
| [`handoff/docs-audit-runbook.md`](handoff/docs-audit-runbook.md) | repo docs を read-only で読み直し、機械検査が拾えない陳腐化・矛盾・重複を 6 領域に分けて所見として起票する手順。手動で起動する |

### 一回限りの引き継ぎ（`docs/handoff/`）

完了基準を満たしたら [`docs/archive/`](archive/) へ移し、本表から外す。

| ドキュメント | 役割 |
|---|---|
| [`handoff/docs-refactoring-plan.md`](handoff/docs-refactoring-plan.md) | repo docs の陳腐化・矛盾・重複を解消し、再発を防ぐ仕組みを入れる整理計画。方針の決定、段階、再発防止策、完了基準 |

## 決定記録と測定記録（`docs/decisions/`）

日付、DEV 番号、経緯、実測値を持つ凍結された記録である。正典ではなく、結論と条件は各行の spec が持つ。
命名、書式、追記のみの規則、spec からの参照の仕方は [`decisions/README.md`](decisions/README.md) が持つ。

| ドキュメント | 種別 | 結論の正典 |
|---|---|---|
| [`decisions/2026-05-20-model-load-and-userdict-cli.md`](decisions/2026-05-20-model-load-and-userdict-cli.md) | 決定記録 | [`plans/windows-port-roadmap.md`](../plans/windows-port-roadmap.md) M8・M9、[`copilot-pc-backend-spec.md`](copilot-pc-backend-spec.md) §4.3 |
| [`decisions/2026-05-20-zenzai-engine-route.md`](decisions/2026-05-20-zenzai-engine-route.md) | 決定記録 | [`zenzai-gpu-route.md`](zenzai-gpu-route.md)「決定（ADR 要旨）」、[`copilot-pc-backend-spec.md`](copilot-pc-backend-spec.md) §4.3 |
| [`decisions/2026-06-11-candidate-ui-coexistence.md`](decisions/2026-06-11-candidate-ui-coexistence.md) | 決定記録 | [`tsf-deep-integration-spec.md`](tsf-deep-integration-spec.md) §2.8〜§2.11、[`plans/windows-port-roadmap.md`](../plans/windows-port-roadmap.md) M5 |
| [`decisions/2026-06-distribution-channels.md`](decisions/2026-06-distribution-channels.md) | 決定記録 | [`sideload-packaging-spec.md`](sideload-packaging-spec.md) §0 |
| [`decisions/2026-06-settings-app-ui-framework.md`](decisions/2026-06-settings-app-ui-framework.md) | 決定記録 | [`sideload-packaging-spec.md`](sideload-packaging-spec.md) §3.0 |
| [`decisions/2026-06-signing-route-selection.md`](decisions/2026-06-signing-route-selection.md) | 決定記録 | [`sideload-packaging-spec.md`](sideload-packaging-spec.md) §2.0 |
| [`decisions/2026-07-dev-infrastructure-rejected-proposals.md`](decisions/2026-07-dev-infrastructure-rejected-proposals.md) | 決定記録 | [`dev-infrastructure-spec.md`](dev-infrastructure-spec.md) §11 |
| [`decisions/2026-07-sccache-action-cache-api-failure.md`](decisions/2026-07-sccache-action-cache-api-failure.md) | 決定記録 | [`dev-infrastructure-spec.md`](dev-infrastructure-spec.md) §4.7 |
| [`decisions/2026-08-04-cuda-real-link-cancellation.md`](decisions/2026-08-04-cuda-real-link-cancellation.md) | 決定記録 | [`zenzai-inference-spec.md`](zenzai-inference-spec.md) §7.4、[`copilot-pc-backend-spec.md`](copilot-pc-backend-spec.md) §4.3、[`sideload-packaging-spec.md`](sideload-packaging-spec.md) §3.7、[`plans/windows-port-roadmap.md`](../plans/windows-port-roadmap.md) M8 |
| [`decisions/2026-08-23-r1-cpu-baseline.md`](decisions/2026-08-23-r1-cpu-baseline.md) | 測定記録 | [`copilot-pc-backend-spec.md`](copilot-pc-backend-spec.md) §4.2.1・§4.2.1.1 |
| [`decisions/2026-08-23-zenz-v3-r2-conversion.md`](decisions/2026-08-23-zenz-v3-r2-conversion.md) | 決定記録 | [`copilot-pc-backend-spec.md`](copilot-pc-backend-spec.md) §4.2.2 |
| [`decisions/2026-08-json-parser-external-library.md`](decisions/2026-08-json-parser-external-library.md) | 決定記録 | [`dev-infrastructure-spec.md`](dev-infrastructure-spec.md) §11.2.1 |
| [`decisions/2026-08-mvp-input-scope-and-signing-gate.md`](decisions/2026-08-mvp-input-scope-and-signing-gate.md) | 決定記録 | [`sideload-packaging-spec.md`](sideload-packaging-spec.md) §0.1 |
| [`decisions/2026-08-x86-process-out-of-scope.md`](decisions/2026-08-x86-process-out-of-scope.md) | 決定記録 | [`sideload-packaging-spec.md`](sideload-packaging-spec.md) §0.1 |
| [`decisions/2026-09-08-nll-release-measurement.md`](decisions/2026-09-08-nll-release-measurement.md) | 測定記録 | [`neural-reranker-spec.md`](neural-reranker-spec.md) Track B（正典の節は `neural-reranker-nll-spec.md` の B12） |
| [`decisions/2026-09-arm64-hardware-start-condition.md`](decisions/2026-09-arm64-hardware-start-condition.md) | 決定記録 | [`plans/windows-port-roadmap.md`](../plans/windows-port-roadmap.md) M27、[`copilot-pc-backend-spec.md`](copilot-pc-backend-spec.md) §8 |
| [`decisions/2026-09-pch-unity-build-measurement.md`](decisions/2026-09-pch-unity-build-measurement.md) | 測定記録・決定記録 | [`dev-infrastructure-spec.md`](dev-infrastructure-spec.md) §2.7・§11.8 |
| [`decisions/2026-10-25-kanakanji-edge-r2-conversion.md`](decisions/2026-10-25-kanakanji-edge-r2-conversion.md) | 測定記録 | [`copilot-pc-backend-spec.md`](copilot-pc-backend-spec.md) §4.2.3 |

## Phase 一覧

Phase の構成（Phase 1〜7 + 追加機能 + 開発基盤 + プライバシー / モデル管理 /
学習データ UI + 変換品質）と、各 Phase が対象とするマイルストーン番号は
[`plans/windows-port-roadmap.md`](../plans/windows-port-roadmap.md) を正典とする。
v1.0 までの Phase 1〜4 は同ファイル「v1.0 までの実行計画（Phase 1〜4）」の索引表、
Phase 5 以降は各 Phase 章の見出しが対象 M 範囲を示す。本 README は Phase 表を
複製しない（roadmap 側の更新に追随できなくなるため）。
