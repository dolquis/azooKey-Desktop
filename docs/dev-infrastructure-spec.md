---
kind: spec
milestones: [M37, M38, M39, M40, M41, M42, M43, M44, M47, M50, M51]
canonical_for: ["開発基盤の入口"]
---

# 開発基盤・品質強化 仕様（Development Infrastructure & Quality）

本書は azooKey-Desktop（Windows ポート）の**開発基盤・ビルド再現性・CI 品質
ゲート・IPC/JSON 堅牢性・観測性・Host 可用性・診断と互換性ハーネス**に関する仕様の入口である。
各節の本文は、下の表と末尾の転送表が示す文書が持つ。対象は Windows 版（C++ 移植: `core/` `ipc/` `learning/` `inference-host/` `tsf-tip/`
`bench/`）のみ。macOS 版 `legacy/` は対象外。

本書は 2 通の第三者レビュー（開発環境・ライブラリ改善レビュー / プロジェクト
レビュー）の指摘を評価・取捨選択した結果を仕様化したものである。
レビュー指摘のうち既存マイルストーン（M8/M11/M12/M28〜M34）と重複するもの、
本プロジェクトの方針と整合しないものは `docs/build-ci-spec.md` の「§11 不採用とした提案」に
理由とともに記録する。

本書の節は、節番号を保ったまま、次の文書へ分けた。分けた節の本文と正典範囲は移動先が持ち、
本書の転送表（末尾）が旧来の `dev-infrastructure-spec §N` の参照を移動先へ解決する。

| 群 | 節 | 文書 |
|---|---|---|
| CI・ビルド | §2（§2.6 を除く）、§3、§4、§9〜§11 | `docs/build-ci-spec.md` |
| 実機検証パッケージ | §2.6 | `docs/vm-verification-package-spec.md` |
| IPC・永続化の堅牢化 | §5、§6 | `docs/ipc-persistence-hardening-spec.md` |
| 観測性 | §7 | `docs/observability-spec.md` |
| Host 可用性 | §8 | `docs/host-availability-spec.md` |
| 診断・互換ハーネス | §12、§13 | `docs/diagnostics-compat-spec.md` |

## 0. 節とマイルストーンの対応

本書の節と、その範囲を定める `plans/windows-port-roadmap.md` のマイルストーンの対応を次に示す。

| 節 | マイルストーン | 文書 |
|---|---|---|
| §1 | 共通 | 本書 |
| §2〜§3（§2.6 を除く） | M37 | `docs/build-ci-spec.md` |
| §2.6 | M37 | `docs/vm-verification-package-spec.md` |
| §4 | M38 | `docs/build-ci-spec.md` |
| §5 | M39 | `docs/ipc-persistence-hardening-spec.md` |
| §6 | M40 | `docs/ipc-persistence-hardening-spec.md` |
| §7（§7.7 を除く） | M41 | `docs/observability-spec.md` |
| §7.7 | M51 | `docs/observability-spec.md` |
| §8（§8.5・§8.6 を除く） | M42 | `docs/host-availability-spec.md` |
| §8.5 | M47 | `docs/host-availability-spec.md` |
| §8.6 | M に属さない設計案（roadmap「リスクと不確実性」） | `docs/host-availability-spec.md` |
| §9 | M43 | `docs/build-ci-spec.md` |
| §10〜§11 | 共通 | `docs/build-ci-spec.md` |
| §12 | M44 | `docs/diagnostics-compat-spec.md` |
| §13 | M50 | `docs/diagnostics-compat-spec.md` |

## 1. 目的と非目標

### 目的

MVP（Phase 1〜2）から「実運用に耐える IME」へ進むにあたり、開発基盤・品質の
負債を解消する。
具体的には次の 4 点をボトルネックとみなし、マイルストーン化する。

1. ビルド再現性（手元・CI・AI エージェントでビルド入口が統一されていない）
2. IPC/JSON の境界堅牢性（自前 JSON パーサと Named Pipe 入力の事故耐性）
3. 観測性（遅延要因の切り分け、構造化ログ、エラーコード体系の不在）
4. Host 可用性（プロセス停止時の TIP 側再接続戦略が限定的）

### 非目標（本トラックでやらないこと）

- Zenzai 本実装の完成（M8 の範囲）
- CUDA / DirectML / NPU の実装（M24 の範囲）
- WinUI 設定アプリの完成（M11/M30 の範囲）
- MSIX/MSI 配布の完成（M11/M12/M28〜M32 の範囲）
- 自前 JSON パーサの即時全置換（`docs/ipc-persistence-hardening-spec.md` §6、`docs/build-ci-spec.md` §11 参照）

### 横断する設計原則

- **オフラインビルド原則を維持する。** `cmake -S . -B build` がネットワーク
  アクセスなしで成功すること。ネットワーク取得は明示オプトイン
  （`AZOOKEY_FETCH_GOOGLETEST=ON` 等）に限る。本トラックの依存追加は
  この原則を崩してはならない（`docs/build-ci-spec.md` §3）。
- **依存最小主義。** header-only ライブラリを優先し、ビルドが必要な依存は
  導入価値を個別に評価する。
- **段階導入。** 警告強化・新ライブラリ導入は一括ではなく、新規コードから
  段階的に適用し、既存コードの大規模差分を避ける。

## 転送表

分けた節は次の文書にある。節番号は保つ。

| 旧節 | 移動先 |
|---|---|
| §2、§3、§4、§9〜§11 | `docs/build-ci-spec.md` |
| §2.6 | `docs/vm-verification-package-spec.md` §1 |
| §5、§6 | `docs/ipc-persistence-hardening-spec.md` |
| §7 | `docs/observability-spec.md` |
| §8 | `docs/host-availability-spec.md` |
| §12、§13 | `docs/diagnostics-compat-spec.md` |

§2.6 は、番号を変える行（最長一致）で `docs/vm-verification-package-spec.md` の §1 へ解決する。

次の決定記録は、結論の節を転送表の移動先に持つ。

- `decisions/2026-07-dev-infrastructure-rejected-proposals.md`（結論の節は §11）
- `decisions/2026-07-sccache-action-cache-api-failure.md`（§4.7）
- `decisions/2026-08-json-parser-external-library.md`（§11.2.1）
- `decisions/2026-09-pch-unity-build-measurement.md`（§2.7・§11.8）
