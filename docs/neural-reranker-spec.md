# ニューラルリランカー仕様

対象リポジトリ: dolquis/azooKey-Desktop
対応マイルストーン: M56（変換品質トラック）+ NllScorer トラック（DEV-413）
関連: `plans/windows-port-roadmap.md` M52 / M53 / M54 / M55 / M57、
      `docs/conversion-quality-benchmark-spec.md`（M52）、
      `docs/dev-infrastructure-spec.md` §7.7（M51 trace）、
      `docs/modernbert-ja-scoring-spec.md`（M57）、
      `docs/zenzai-inference-spec.md`（Track B の推論基盤）
作成日: 2026-05-27
更新日: 2026-08-09（DEV-556: Track B〈NllScorer 型ニューラルリランク〉の NLL 定義・
        コンテキスト再利用・責務境界・合成・fallback・有効化フラグを確定）
位置づけ: 変換品質トラック（M53 / M54 / M55 のすべてが完了し、M52 ベンチで
baseline 固定後）

## 0. 本書が扱う 2 トラック

本書は「候補を並べ替える（生成しない）」層の正典であり、独立した 2 つの
リランカートラックを収容する。両者は**別モデル・別実装・別スケジュール**であり、
一方の完了を他方の前提にしない。

| トラック | 章 | 対象 | スコア源 | 前提 |
|---|---|---|---|---|
| **Track A: Tiny Neural Reranker**（M56） | §1〜§14 | 全ソースの候補 | ONNX MLP + 手作り特徴量 | M52 + M53 + M54 + M55 |
| **Track B: NllScorer 型ニューラルリランク**（DEV-413） | §B1〜§B12 | 辞書由来候補のみ | ロード済み Zenzai の系列 NLL | M8（Zenzai 配線）のみ |

- 章番号は分離する。他ドキュメントからの既存参照（`neural-reranker-spec` §4 /
  §7 / §9 等）はすべて Track A を指す。Track B は `§B<n>` で参照する。
- 両トラックの接点は**スコア統合レイヤのみ**（§B6）。Track B は Track A の
  `features_v1` 契約（§5.2）を変更しない。

---

## 1. 目的（Track A）

Zenzai / 辞書 / ユーザー辞書 / 補正候補から得た**候補を、文脈と各種
特徴量で並べ替える**。生成は行わず、候補選択に専念する。M52 ベンチで
top1 を baseline 比 +3% 以上 / p95 latency 悪化 +10ms 以内を目標。

## 2. 設計原則

- **生成しない**: 既存 Zenzai / 辞書出力を入力として、score を出すのみ
- **軽量**: 推論 timeout 10〜20ms、CPU 優先
- **fallback**: 失敗時は reranker なしで返す（IME 入力を止めない）
- **学習データはローカル**: 確定 / 訂正履歴を使う、クラウド送信なし
- **embedding 源を実行時条件に従属させない**: v1 は embedding を入力に
  使わず手作り特徴量で完結させる（§4.1 で決定）。embedder を M57
  ModernBERT に依存させない（CPU-only / 未配置でも reranker が同一 shape で
  動く）

## 3. アーキテクチャ

```
CandidateGenerator output (最大 30 候補)
        ↓
FeatureExtractor
        ↓
TinyNeuralReranker (MLP, ONNX)
        ↓
final_score を更新
        ↓
ScorePipeline へ
```

## 4. 入力特徴量

| 特徴 | 型 | 例 | 出所 |
|---|---|---|---|
| `left_context_embedding` | vector<128> | 文脈 embedding | §4.1 |
| `reading_embedding` | vector<128> | こうしょうする | §4.1 |
| `candidate_embedding` | vector<128> | 交渉する | §4.1 |
| `zenzai_score` | float | -1.82 | Zenzai 出力 |
| `dictionary_score` | float | 0.64 | M53 |
| `user_frequency` | float | 0.25 | M54 |
| `recency_score` | float | 0.41 | M54 |
| `typo_confidence` | float | 0.82 | M55 |
| `app_profile_score` | float | 0.08 | M48（未完了時は 0.0 既定） |
| `candidate_length` | int | 4 | length |
| `segment_count` | int | 2 | M55 segments |
| `is_named_entity` | bool | false | M53 category |
| `is_user_dict` | bool | false | M9 |
| `is_neologism` | bool | false | M53 category |
| `is_typo_corrected` | bool | true | M55 |

合計入力次元: 128 × 3（embeddings）+ scalar 8（zenzai / dictionary /
user_frequency / recency / typo_confidence / app_profile /
candidate_length / segment_count）+ flag 4（bool を 0/1 で符号化）=
**396 次元**。これは embedding を含めた **v2 契約**であり、ONNX 入力
shape は `features_v2 = [batch, 396]`。

v1（§5.2 Option C 採用）は 3 種類の embedding（合計 384 次元）を
**入力に含めず**、scalar 8 + flag 4 = 12 次元の `features_v1` を入力
shape とする（`[batch, 12]`）。FeatureExtractor は ONNX メタ情報
`model_version` で v1 / v2 を切り替える（§5.2 と整合）。

**M48（AppProfile）が未完了の環境への fallback**: M56 の hard prerequisite
は M52 + M53 + M54 + M55 の 4 つで、M48 は含まれない（roadmap M56 §前提と
整合）。M48 未完了 / 未配置の環境では、FeatureExtractor が
`app_profile_score` を **0.0（neutral）** で埋める。これにより v1 ONNX の
入力 shape `[batch, 12]` は M48 の完了状況によらず常に確定する。M48 が
後から有効化されても feature の物理位置・dtype は変えず、値の供給元のみ
切り替わる。

### 4.1 Embedding 供給元（決定）

**決定（DEV-112 で固定）**: M56 の範囲（v0 → v1）では **embedding を入力に
含めない**。3 種類の embedding（`left_context` / `reading` / `candidate`）の
供給方式は **Option C（手作り特徴量で代替）を v1 で採用**し、**Option A
（独立小型 encoder）を v2 以降の拡張として段階導入**する。**Option B
（M57 ModernBERT 共用）は embedder としては不採用**とする。

検討した選択肢:

**Option A**: 独立小型 encoder
- 自前の char-level CNN または BERT-tiny（〜10MB ONNX）
- 学習データから fine-tune、ロード時間短、独立に最適化可能
- → **v2 以降で採用**（§10）

**Option B**: ModernBERT 共用（M57 と並存）
- M57 の ModernBERT-Ja を embedder として再利用
- → **不採用**（理由は下記）

**Option C**: 計算により回避（手作り特徴量）
- embedding ベクトルを使わず、§4 / §5.2 の既存 **12 次元 `features_v1`**
  （scalar 8 + flag 4）のみで構成する。これらは `candidate_length` /
  `segment_count` / `typo_confidence` など**それ自体が手作り特徴量**であり、
  v1 はこの 12 次元に固定する（embedding 代替の追加次元は導入しない）。
- char n-gram / reading-candidate edit distance などの追加手作り特徴量は
  **v1 範囲外**。導入する場合は `features_v1.x` として次元を versioning し、
  ONNX schema（input 名・shape）を更新する（後方互換は §5.2 の version 切替に従う）。
- ONNX は最小 MLP のみ（embedder 不要）、推論軽量、CPU で完結
- → **v1 で採用**（§5.2 / §10）。v1 の入力は 12 次元固定

**Option B（ModernBERT 共用）を不採用とする理由**（M57 spec と整合）:

1. **CPU で動作しない**: M57 は ModernBERT を `backend in {winml, cuda,
   vulkan}` でのみロードし、CPU backend ではロードすらしない
   （`docs/modernbert-ja-scoring-spec.md` §4.0 順 3 / §6.1）。M56 reranker は
   §2 のとおり **CPU 優先で常時動作**する前提のため、embedder を ModernBERT に
   従属させると CPU-only 環境で embedding 特徴が常に欠損する。
2. **常時ロードされない**: M57 は ambiguity ゲート・RSS ロードゲート・circuit
   breaker により**条件付きにしか起動しない**（§4.0 / §6.1）。embedder の
   可用性が実行時条件に左右されると、M56 の特徴量パイプラインが非決定的になり、
   M52 ベンチの再現性（baseline +3%）を測れない。
3. **配置フェーズが異なる**: M57 は v1.0 後トラックで bundle / DL 方針も
   Phase 7 で決定する（§6）。M56 を M57 配置に従属させると M56 単独で acceptance を
   満たせない（roadmap M56 の前提は M52 / M53 / M54 / M55 のみで M57 を含まない）。
4. **IME の embedder として過剰**: 70M BERT を rerank の embedding 供給に常用する
   のは latency / メモリの面で IME に不釣り合い。

この決定により、**M56 の embedding 源は実行時条件（backend / RSS / M57 配置）に
依存しない**。v1 は §5.2 の 12 次元 `features_v1` のみで成立し、ModernBERT の
有無に関わらず同一の入力 shape で動く。v2 で Option A の独立小型 encoder を導入する
際も、ModernBERT とは独立した自前 encoder を用い、M57 とは結合しない（§10 / §14）。

> M57 との関係は**スコア統合のみ**（§9 の `modernbert_score` weight）に留める。
> M57 は final_score の 1 項であって M56 の embedding 供給元ではない。2 段 rerank
> （M57 が ambiguous 候補だけ精査）は §14 の将来拡張であり、embedder 共用とは別物。

段階導入の詳細は §10 のモデル変遷で扱う。

## 5. モデル

### 5.1 候補

| 案 | 内容 | 段階 |
|---|---|---|
| LightGBM | 特徴量ベースの GBDT、CI で再現性高い | v0（baseline 用） |
| MLP reranker | 軽量 NN、ONNX 化 | v1（採用） |
| Mini Transformer | 文脈・読み・候補を直接入力 | v2（将来） |
| ModernBERT fine-tuned reranker | 高品質だが重い | M57 と統合 |

### 5.2 v1 MLP 構造

- 入力: §4 から **128 次元 embedding 3 つを除いた** 12 次元
  （scalar 8 + flag 4 = 12）。Option C を採用するため §4 の 396 次元
  契約とは別の v1 専用 input shape を持つ
- ONNX 入力名: `features_v1`（shape `[batch, 12]`）。`features_v2`
  （shape `[batch, 396]`、embedding 込み）は v2 以降で導入
- 隠れ層: 2 層、各 64 ユニット、GELU
- 出力: 1 unit（rerank_score）
- パラメータ数: 約 6 KB（量子化なし）

runtime FeatureExtractor は v1 / v2 で別ビルダーを持ち、ONNX のメタ情報
`model_version`（`v1` / `v2`）から入力形を切り替える。両者を同時に
ロードする場合は version ごとに別 session を保持する。

### 5.3 ONNX export

PyTorch / scikit-learn で学習 → ONNX export。配置先:

```
%LOCALAPPDATA%\azooKey\models\tiny_reranker.onnx
```

または bundled として `models/` 配下に同梱（M28 MSIX 同梱可）。
モデルパスは `settings.reranker.tinyNeuralModelPath` で上書き可能。

### 5.4 特徴量の重要度と重み付け（決定）

§4 の 12 次元（v1）/ 396 次元（v2）の特徴量について、**個々の重みは手動で
固定しない**。決定方針を以下に固定する:

- **v0（LightGBM）を feature importance の基準器とする**: v0 baseline で
  gain / split ベースの feature importance を算出し、低寄与特徴の除外候補・
  正規化方針の根拠とする。importance は M52 ベンチ成果物として残す
  （§12 の v0 / v1 比較レポートに含める）。
- **v1（MLP）の重みは学習で決定する**: 隠れ 2 層 × 64 unit の重みは学習データ
  （§6）から学習し、手動調整しない。特徴の寄与は学習後の ablation（特徴を
  1 つ落として再評価）で確認し、§12 レポートに記す。
- **特徴量の正規化を ONNX に焼き込む**: scalar 特徴（`zenzai_score` /
  `dictionary_score` / `user_frequency` / `recency_score` / `typo_confidence` /
  `app_profile_score`）は学習データ統計から推定した per-feature 標準化
  （z-score または min-max）を ONNX グラフ内に持たせ、runtime では生値を入力する。
  正規化統計はモデルと一緒に versioning する。flag / int 特徴
  （`candidate_length` / `segment_count` / 4 つの bool）は正規化せず素の値
  （bool は 0 / 1）を入力する。
- **欠損特徴の既定値を固定する**: M48 未完了時の `app_profile_score`=0.0（§4）、
  タグ未整備時の `is_named_entity` / `is_neologism`=false、M55 非適用時の
  `typo_confidence`=0.0 / `is_typo_corrected`=false を既定とし、欠損が score を
  不当に押し上げない安全側に倒す。
- **score 統合の重み（§9）は別レイヤ**: §9 の `final_score` 係数は reranker
  内部重みとは別に、LightGBM / logistic regression で学習する。M56 reranker は
  `tiny_score` を出力するのみで、`final_score` 係数の校正は M52 ベンチの責務。

## 6. 学習データ

正例はユーザーが確定した候補、負例は表示されたが選ばれなかった候補。

```json
{
  "left_context": "明日の会議では価格について",
  "input": "こうしょうする",
  "candidate": "交渉する",
  "features": { ... },
  "label": 1
}
```

訂正イベントは強い負例として扱う:

```json
{
  "left_context": "明日の会議では価格について",
  "input": "こうしょうする",
  "rejected_candidate": "校章する",
  "accepted_candidate": "交渉する",
  "label_strength": "strong_negative"
}
```

### 6.1 データ収集

- **露出トレース（負例・正例の源、確定時に記録）**: 負例（§6.2）は確定時に
  表示されていた候補集合を必要とする。これは IPC `CommitObservation(reading,
  chosen, shown, left_context, timestamp_ms)` の `shown[]` に存在するが、既存の
  集約学習ストア `learning.tsv`（M54、`user-learning-enhancement-spec.md` §8）には
  **永続化されない**（同ストアは reading / surface / weight / context_hash 等のみ）。
  そのため reranker 学習では、確定時に次を記録する**専用の露出トレース**を
  ローカルへ永続化する（`learning.tsv` とは別ファイル。例:
  `%LOCALAPPDATA%\azooKey\data\reranker_trace.jsonl`）。各レコードのフィールド:
  - `left_context_hash`（§8.1 と同方式。raw 文脈は持たない）/ `reading` /
    `chosen_surface` / `shown_surfaces[]`（露出した候補集合）/ 各候補の `features`
  - `shown_at_ms`（候補ウィンドウ表示時刻）と `commit_ts`（確定時刻）。両者から
    `dwell_ms = commit_ts - shown_at_ms` を算出でき、§6.2 の `minDwellMs` 判定に使う
    （`CommitObservation.timestamp_ms` は確定時刻のみで dwell を出せないため、表示
    時刻を TIP 側で付与してトレースに含める）
  - 訂正レコード（**強負例の源**）: 訂正発生時に `event_type`（`correction_reject`
    / `correction_accept`）・`rejected_surface` / `accepted_surface`・`correction_ts`
    を同トレースに記録する。これにより `correctionWindowMs` を `correction_ts` と
    確定時刻の差で判定でき、ペアリングもトレース内で完結する。
  - secure モード中は記録しない（§13）。`shown[]` が利用できる確定時にのみ書く。
  - 露出トレースが**正例・負例・強負例ラベルの正典**。`learning.tsv` は scalar
    特徴（`user_frequency` / `recency_score`）の供給に使うが、露出・dwell・訂正
    ペアの情報は持たない。
  - プライバシー方針（§13）を `learning.tsv` と揃える（local 限定・secure 除外）。
- **強負例は露出トレースの訂正レコードを源とする**: M54 v1 の集約 TSV は単一
  `surface` + `event_type` + `context_hash` のみで、`rejected_surface` /
  `accepted_surface` のペアや event timestamp を durable に持たない（ペア保持は
  将来の SQLite オプション、`user-learning-enhancement-spec.md` §8）。そのため強負例
  と `correctionWindowMs` は上記トレースの訂正レコードから生成する（M54 集約 TSV に
  依存しない）。M54 SQLite オプションが有効な環境ではそのペアテーブルを併用してよい。
- 個人ユーザーの学習データを **送信せず**、ローカルで個人 fine-tune（v2 で検討）。
- bundled モデルは公開コーパスから生成（青空文庫 / CC0・MIT 互換の IME 公開
  データセット）。M52 §11 と同じく CC BY-SA / GFDL 系（Wikipedia 等）は採用しない。
  bundled 生成では露出（chosen / shown）を公開コーパスの正解 + distractor 候補から
  **合成的に構成**する（実ユーザーの露出トレースは使わない）。

### 6.2 ラベリングプロトコル（決定）

正例・負例・強負例の定義と採否条件を以下に固定する。曖昧な収集はノイズになり
M52 ベンチの精度評価を歪めるため、採否条件まで spec で固定する。

| ラベル | 定義 | 採否条件 |
|---|---|---|
| 正例（`label`=1） | 同一変換機会でユーザーが確定した候補 | 候補ウィンドウに**表示された**こと。確定が即時取り消し（直後の undo / 再変換）された場合は除外 |
| 負例（`label`=0） | 同一機会で表示されたが選ばれなかった候補 | **露出トレース（§6.1）の `shown[]` を源とする**（集約 `learning.tsv` には露出情報が無いため負例は生成不可）。表示された候補のみを対象（表示されなかった候補は負例にしない＝露出バイアスを入れない）。1 機会あたり負例は上位 N（既定 `trainNegativesPerSample`=8）まで |
| 強負例（`label_strength`=`strong_negative`） | 訂正イベントで reject され別候補へ訂正された候補 | **露出トレース（§6.1）の訂正レコード**（`rejected_surface` / `accepted_surface` / `correction_ts`）を源とする。`correctionWindowMs`（既定 10000）は `correction_ts` と確定時刻の差で判定。M54 集約 TSV はペア・event timestamp を durable に持たないため使わない（SQLite オプションがあれば併用可） |

追加の採否・整形ルール:

- **重複排除**: dedup キーは文脈源によって使い分ける。
  - **個人（M54 由来）データ**: M54 / M46 は raw `left_context` を永続化せず
    `context_hash` のみを保持する（`user-learning-enhancement-spec.md` §8.1、
    SHA-256 上位 4 bytes・復元不能）。したがって dedup キーは
    `(context_hash, input, candidate)` とする。`context_hash` は衝突を許容する
    弱い 4-byte シグネチャ（同 §8.1）のため、集約は**近似的**であることを許容する。
  - **bundled コーパス生成（§6.1）**: 公開コーパスから生成するため raw 文脈が
    得られる。`(normalize(left_context), input, candidate)` を使う。
  - 集約後、label を多数決でまとめ、出現回数を frequency 系特徴の補強に使う。
- **滞留時間ゲート**: 候補ウィンドウ表示から確定までが極端に短い（誤確定の疑い、
  既定 `minDwellMs`=120 未満）サンプルは正例から除外する。判定には露出トレース
  （§6.1）の `dwell_ms = commit_ts - shown_at_ms` を用いる（`shown_at_ms` を
  持たない bundled 合成データでは本ゲートを適用しない）。
- **セッション境界**: bundled コーパス生成では `left_context` をその確定時点の
  **確定済みテキスト**とし、後続編集の影響を混ぜない（§7 推論時の context 定義と
  一致させる）。個人データでは M54 が確定時点の `context_hash` を保持するため
  同等の境界が保たれる（raw 文脈は使わない）。
- **クラス不均衡**: 正例 1 に対し負例が多くなるため、学習時に負例をサブサンプル
  （または class weight）で調整する。比率は M52 ベンチで校正する。
- **強負例の loss 重み**: 強負例は通常負例より大きい loss 重み（既定 2.0）を与える。
  値は M52 ベンチで校正する。
- **secure 除外**: §13 のとおり M46 secure 中は学習データを**収集しない**
  （正例・負例・訂正イベントすべて）。
- **bundled と個人の分離**: bundled モデルは §6.1 の公開コーパス由来のみ。個人の
  `correction_events` 由来データはローカル fine-tune（v2 で検討）の範囲に留め、
  bundled 学習には混ぜない。

> 上記の `trainNegativesPerSample` / `correctionWindowMs` / `minDwellMs` /
> 強負例 loss 重み・class weight は**オフライン学習パイプラインのパラメータ**
> であり、runtime 設定（§8 の `settings/mvp-settings.schema.json`）には登録しない。

## 7. 推論仕様

| 項目 | 仕様 |
|---|---|
| 入力候補数 | 最大 30 |
| 出力 | rerank_score per candidate |
| timeout | 既定 15ms（範囲 10〜20ms、`tinyNeuralTimeoutMs`、範囲外は clamp） |
| fallback | 失敗時は reranker なしで返す（candidate 順序維持） |
| 実行頻度 | 通常変換ごと |
| backend | ONNX Runtime CPU 優先、GPU optional |
| バッチ | 全候補を 1 forward で処理 |

### 7.1 失敗 fallback

- ONNX load 失敗 → reranker 無効化、log 記録、続行
- 推論 timeout → 当該リクエストの rerank skip、log 記録、続行
- 入力 NaN/Inf 検出 → 当該候補だけ skip、他は継続

M47 `Recovering` / `DegradedModel` 状態と整合（Host は落ちない）。

### 7.2 timeout / 失敗の閾値（決定）

- **timeout 既定値**: `tinyNeuralTimeoutMs` = **15ms**（範囲 10〜20ms、範囲外は
  clamp）。v1 MLP（12 次元・隠れ 64×2）の 30 候補一括 forward は CPU でも通常
  1〜2ms で完了するため、timeout は**病的な stall（ONNX session の一時的ハング等）
  に対する安全弁**であり、通常 path の p95 を支配しない。これにより §12 の
  「p95 latency 悪化 +10ms 以内」を脅かさない。
- **ロード失敗は即時無効化（breaker とは別経路）**: ONNX load 失敗時は §7.1 の
  とおり当該セッションで reranker を**即時無効化**し、`reason=load_failed` で
  fallback する。使えるセッションが残らず後続のリクエスト失敗を生まないため、
  circuit breaker の連続カウントには**載せない**。次回モデル再ロード（設定変更・
  Host 再起動）で再評価する。
- **連続失敗の circuit breaker**: **runtime の失敗**（推論例外 `infer_error` /
  timeout `timeout`）が**連続 `tinyNeuralFailureThreshold`=3 回**に達したら、当該
  セッションで reranker を**無効化**し（runtime 状態）、以降は fallback（candidate
  順序維持）で返す。次回モデル再ロード / セッション再初期化で再有効化する。これは
  M57（`docs/modernbert-ja-scoring-spec.md` §5.4）の circuit breaker と同じ runtime
  安全パターンであり、ユーザー設定 `tinyNeuralEnabled` とは独立した内部フラグである。
  `load_failed`（上記）と `invalid_input`（下記、入力起因）は breaker に載せない。
- **部分適用**: 入力 NaN / Inf を含む候補は当該候補のみ skip し、他候補には rerank を
  適用する（§7.1）。全候補が skip された場合は `reason=invalid_input` で順序維持して
  返す（circuit breaker の連続失敗にはカウントしない＝入力データ起因のため）。
- **fallback は失敗ではなく既定パス**: 上記いずれの fallback も
  `tiny_used=false, reason=<load_failed | timeout | infer_error | invalid_input | secure_mode | circuit_open | disabled>`
  を構造化ログに記録し、M52 ベンチの `fallback_rate`
  （`docs/conversion-quality-benchmark-spec.md`）集計に使う。各 reason の意味:
  - `load_failed`: ONNX ロード失敗による即時無効化（上記）
  - `timeout` / `infer_error`: runtime 失敗（circuit breaker にカウント）。
    `infer_error` は breaker が開く前の単発推論例外にも用いる（M57 §5.4 と整合）
  - `invalid_input`: 全候補が NaN / Inf で skip（入力起因、breaker 対象外）
  - `secure_mode`: M46 secure による抑止（§13。ユーザー設定 off の `disabled` とは
    区別し、プライバシー監査・fallback 分析で識別可能にする。M57 §9 の
    `secure_mode` と整合）
  - `circuit_open`: circuit breaker 作動中（runtime 状態）
  - `disabled`: ユーザー設定 `tinyNeuralEnabled=false`

## 8. 設定スキーマ

`settings/mvp-settings.schema.json` に追加:

```json
{
  "reranker": {
    "tinyNeuralEnabled": false,
    "tinyNeuralModelPath": "",
    "tinyNeuralTimeoutMs": 15,
    "tinyNeuralFailureThreshold": 3,
    "modernbertEnabled": "auto",
    "modernbertTimeoutMs": 30
  }
}
```

- `tinyNeuralEnabled`: 既定は **OFF**。Track B の `nllRerankEnabled`（§B9）と同じく、
  リランク層は §12 の受け入れ条件（M52 ベンチで top1 +3% 以上 / p95 悪化 +10ms 以内）を
  満たしたことを確認したうえで、既定 ON への切替を別途判断する。**既定 OFF のとき
  ONNX セッションを生成せず**、候補列・`final_score`・`debug_info`・レイテンシは Track A
  導入前と等価であること（§11 の回帰対象）。ユーザー設定 OFF による非適用は
  `reason=disabled` で記録する（§7.2）。
- `tinyNeuralTimeoutMs`: 範囲 10〜20（§7.2）。範囲外は clamp。
- `tinyNeuralFailureThreshold`: §7.2 の circuit breaker 閾値（既定 3）。
- `modernbertEnabled` / `modernbertTimeoutMs` は M57 が使う（本 spec では枠のみ
  予約）。M57 側の正典は `docs/modernbert-ja-scoring-spec.md` §7。
- §6.2 の学習パイプラインパラメータ（`trainNegativesPerSample` /
  `correctionWindowMs` / `minDwellMs` 等）はオフライン用であり、本 runtime schema
  には登録しない。
- 本ブロックは M56 実装着手時に `settings/mvp-settings.schema.json` へ正式登録する
  （M56 / M57 は v1.0 後トラックのため現状未登録）。

## 9. スコア統合

M56 の rerank_score は M52 ベンチで定義する `final_score` の構成要素
の 1 つ。初期重みは以下を提案するが、最終的に M52 ベンチで校正する:

```
final_score =
  normalize(zenzai_score)       * 0.35
+ normalize(dictionary_score)   * 0.15
+ normalize(user_score)         * 0.20
+ normalize(typo_score)         * 0.10
+ normalize(tiny_score)         * 0.15
+ normalize(modernbert_score)   * 0.05
+ hard_bonus
- hard_penalty
```

これらの重みは **手動値ではなく LightGBM / logistic regression で
学習**することを推奨する（評価は M52 で実施）。

## 10. モデル変遷（段階導入）

| 段階 | モデル | embedding | M52 KPI 目標 |
|---|---|---|---|
| v0 | LightGBM（手作り特徴量） | なし | baseline 比 +1% |
| v1 | MLP（手作り特徴量） | なし | baseline 比 +3% |
| v2 | MLP + 独立小型 encoder | Option A | baseline 比 +4% |
| v3 | Mini Transformer | Option A | baseline 比 +5% 以上 |

本 M56 は **v0 → v1** までを範囲とし、v2 以降は将来 M に分離する。embedding を
使う v2 / v3 でも embedder は §4.1 の決定どおり **Option A（独立小型 encoder）**を
用い、M57 ModernBERT 共用（Option B）は採らない。

## 11. テスト

- unit: 特徴量抽出（NaN / 欠損値）
- unit: 既定 OFF（`tinyNeuralEnabled=false`）で ONNX セッションを生成せず、候補列・
  `final_score` が Track A 導入前と等価（§8）
- unit: ONNX load 失敗時の fallback
- unit: timeout 時の fallback
- unit: 連続失敗で circuit breaker が開き、再ロードで閉じる（§7.2）
- integration: M52 ベンチで top1 +3% / p95 latency +10ms 以内
- integration: M55 typo 候補に対する正しい boost / penalty
- snapshot: ONNX schema 固定（input / output 名）

## 12. M56 受け入れ条件

- M52 ベンチで `top1_accuracy` が baseline 比 +3% 以上
- M52 ベンチで `latency_p95_ms` 悪化が +10ms 以内
- timeout / load 失敗時に reranker なしで候補が返る（IME が止まらない）
- 連続失敗（既定 3 回）で circuit breaker が開き fallback する（§7.2）
- v0（LightGBM baseline）と v1（MLP）の比較レポート（feature importance /
  ablation を含む、§5.4）が残る
- embedding 供給方針（§4.1）が**決定**され spec に明記されている
  （v1=Option C / v2 以降=Option A、ModernBERT 共用は不採用）
- 学習データが §6.2 のラベリングプロトコルに従って生成されている

## 13. プライバシー

- M46 secure 中は reranker を無効化（学習データへの影響を避ける）。fallback は
  `reason=secure_mode` で記録し（§7.2）、ユーザー設定 off の `disabled` と区別する
- 学習データは個人ローカルでのみ収集（クラウド送信なし）
- bundled モデルは公開コーパス由来のみ

## 14. 将来拡張（M56 範囲外）

- v2 以降の embedding 強化
- Mini Transformer 化
- 個人 fine-tune（プライバシーを保ったまま）
- M57 ModernBERT との 2 段 rerank（M57 が ambiguous 候補だけ精査）

---

# Track B: NllScorer 型ニューラルリランク

対応課題: DEV-413（実装）/ DEV-556（本章の設計確定）
参考実装（**設計思想のみ**。逐語移植しない）: karukan
`karukan-engine/src/kanji/llamacpp.rs`（`NllScorer`。コンテキストを
再利用して (reading, surface) ペアの文字あたり NLL を算出する）。本章の算出式・
正規化・合成規則は azooKey 側で独自に定めたものであり、参考実装の写しではない。

## B1. 目的とスコープ

**目的**: すでにロード済みの Zenzai モデルを使い、**辞書由来候補**の並び順を
系列尤度（NLL）で整える。生成は行わない。

- **入力**: 1 リクエスト内で確定した (左文脈, 読み) と、マージ済み候補列。
- **出力**: 対象候補への加算スコア（§B6）。候補の追加・削除・表層の書き換えは
  行わない。
- **前提**: M8（Zenzai 配線、`docs/zenzai-inference-spec.md`）のみ。M52 ベンチ・
  M53〜M55 を前提にしない（Track A と独立に着手できる）。
- **実装配置**: `inference-host/` 内で完結する。`ipc/` と `tsf-tip/` は変更しない
  （§B4.4）。
- **適用経路は `QueryCandidates` のみ**。`QueryPredictions`（次語予測）と
  `QueryCorrections`（訂正候補）には適用しない。前者は読みが確定していないため
  §B2.1 のプロンプト（`[IN]<input_katakana>[OUT]`）が成立せず、後者は候補集合が
  小さくレイテンシに対する利得が薄い。両者への拡張は将来課題とする。
- **`QueryCandidates` の中でも `live=true`（ライブ変換）は対象外**。ライブ変換は
  打鍵ごとに走る最小レイテンシ経路で top-1 のみを使う
  （`docs/zenzai-inference-spec.md` §6.1）ため、並べ替えの利得が無いまま
  §B7 の予算（既定 20ms）を毎打鍵に上乗せすることになる。NllScorer は
  **候補ウィンドウ要求（`live=false`）にのみ**適用する。

### B1.1 Track A（M56）との関係

- **排他ではない**。Track A は全ソース候補を手作り特徴量 + ONNX MLP で並べ替える層、
  Track B は辞書由来候補だけを LLM 尤度で整える層で、評価対象も信号源も異なる。
- 両方が有効な環境では、Track B の出力は Track A より**手前**に効く（§B6 の適用点）。
- Track B の `nll_per_char` を Track A の特徴量（§4）や `final_score`（§9）へ
  取り込むかは **M56 実装時の判断**とし、本章では決めない。取り込む場合も
  `features_v1` の 12 次元契約（§5.2）は Track B からは変更しない。

### B1.2 既存 Zenzai 生成との関係

Zenzai が生成した候補（`CandidateSource::Model`）は生成時点で系列 logprob を持ち、
`docs/zenzai-inference-spec.md` §6.5 で [0.3, 1.4] 帯へ写像済みである。これを
NllScorer で再評価すると**同じ信号を二重に掛ける**ため、生成候補は対象外とする
（§B5）。Track B が埋めるのは「Zenzai を通っていない辞書候補には尤度の情報が
一切無い」という空白である。

## B2. NLL の定義

### B2.1 プロンプトと候補トークン列

プロンプト `P` は `docs/zenzai-inference-spec.md` §3.2 の zenz-v3 フォーマットと
**完全に同一**とする（別フォーマットを作らない）。

```
P = [CTX]<clean_left_context>[IN]<input_katakana>[OUT]
```

- 前処理（カタカナ化・左文脈 30 文字切り詰め）も同 §3.1 と同一。生成経路と
  同じ `BuildZenzaiPrompt` を共有する。
- 候補表層 `s` は `llama_tokenize` でトークン列 `t_1 … t_m` にする。
  **BOS を付与しない**（BOS は `P` 側のみ。同 §5 の `add_bos_token` 契約に従う）。
- **EOS をトークン列に含めない**（決定）。理由:
  1. GGUF が宣言する `tokenizer.ggml.eos_token_id` は pin モデルでずれており、
     `docs/zenzai-inference-spec.md` §9.2 の override 契約に依存する（DEV-743）。
     EOS 項を NLL に入れると、
     スコアが override の有無というモデルメタデータ都合に左右される。
  2. EOS の logprob は「ここで文が終わる自然さ」を測る別の量であり、読みが同一で
     表層長の異なる候補間で短い表層を系統的に有利にする。
  3. 辞書候補は**完結した表層**として与えられ、終端の判断は不要。

### B2.2 算出式

`log p(t_i | …)` は、`t_1 … t_{i-1}` まで decode した位置の logits に
log-softmax を掛けた値とする（数値安定化は生成経路の `CollectTokenChoices` と
同じく最大値減算で行う。top-K への絞り込みは行わず、対象トークン id の値のみ使う）。

```
NLL_total(s) = - Σ_{i=1..m} log p(t_i | P, t_1 … t_{i-1})
nll_per_char(s) = NLL_total(s) / max(1, C(s))
```

- `C(s)` = 表層 `s` の **UTF-8 コードポイント数**。
- 累算は `double`。

### B2.3 正規化単位をコードポイントとする理由（決定）

分母に**トークン数ではなくコードポイント数**を採る。

- トークン平均（`NLL_total / m`）は語彙依存が強い。同一読みの候補でも、語彙に
  1 トークンとして載っている表層（例「日本語」）と複数トークンに割れる表層
  （例「二本語」）でスケールが変わり、**語彙頻度バイアスがそのまま順位に乗る**。
- コードポイント数は候補固有かつモデル非依存の量で、同一読みの候補集合では
  ほぼ等長になる。したがって候補間比較は実質的に `NLL_total` の比較に近い意味を
  保ちつつ、長さの異なる候補（送りがな違い等）でも破綻しない。
- 書記素クラスタ単位（結合文字・異体字セレクタを 1 文字に畳む）には**しない**。
  決定性と実装単純性を優先する。日本語表層では両者の差はほぼ生じず、差が問題に
  なる表層が現れた時点で別課題として再検討する。

### B2.4 非有限値の扱い

`log p` に NaN / Inf が 1 つでも現れた候補は**スコア不能**として扱い、その候補
だけを対象集合から外す（元の score と順位を保つ）。他候補の評価は継続する。
全対象候補がスコア不能なら §B8 の `invalid_score` として何も適用しない。

## B3. コンテキスト再利用（prefix KV 共有）

1 リクエスト内で `P` は全候補に共通である。これを候補ごとに decode し直すと
コストが候補数に比例して跳ね上がるため、**prefix の KV を 1 回だけ作って
候補ごとにロールバックする**。

```
1. KV をクリアし、P を decode する（prefix KV 確立、prefix_len トークン）
1'. prefix 最終位置の logits を**スナップショットへ複製**する（§B3.1）
2. 各候補 s について:
   a. t_1 の logprob は 1'. のスナップショットから読む
   b. t_1 … t_{m-1} を decode し、t_2 … t_m の logprob を読む
   c. KV を prefix_len へロールバックする
3. 予算 / cancel は 1.・2.a の前と 2.c の後で確認する（§B7）
```

### B3.1 prefix logits のスナップショット契約

**prefix 最終位置の logits は、prefix decode の直後に複製して全候補で再利用する。**

`llama_get_logits*` が返すのは**直近の `llama_decode()` の出力**である。KV を
`prefix_len` へロールバックしても logits バッファは巻き戻らないため、候補 1 の
decode 後に読める「最終位置の logits」は prefix のものではなく**候補 1 の最終
出力**である。スナップショットを取らずに §B3 の 2.a を実装すると、2 候補目以降の
`t_1` の logprob が誤る（あるいは候補ごとに prefix を decode し直すことになり、
本節の共有契約そのものが失われる）。

- 複製するのは prefix 最終位置の logits 行（`vocab_size` 個）。候補ごとに `t_1` が
  異なるため、特定トークンの値だけでは足りない。
- log-softmax の分母（§B2.2 の最大値減算を含む正規化項）は**スナップショットから
  1 回だけ算出**し、全候補で使い回す。
- スナップショットの寿命は 1 リクエスト内。次リクエストでは prefix が変わるため
  必ず取り直す。

回帰テストで固定する（§B11）: 候補を 2 件以上与え、候補 2 の `t_1` の logprob が
**prefix 由来**であること（候補 1 の出力由来になっていないこと）。

### B3.2 off-by-one 契約

**`t_i` の logprob は `t_1 … t_{i-1}` まで decode した位置の logits から読む。**
したがって:

- `t_1` の logprob の出所は**候補トークンではなく prefix の最終位置**である。
- `t_m` 自身を decode する必要は無い（`t_m` の出力 logits は使わない）。
  候補あたり実際に decode するのは `m-1` トークンである。

この 1 ずれは実装で最も壊れやすい箇所であり、テストで固定する（§B11）。

### B3.3 既存 `DecodeTokens` を流用しない

`inference-host/src/ZenzaiModelConverter.cpp` の `DecodeTokens` は
NllScorer にそのまま使えない。理由は 2 つで、いずれも生成経路には正しい挙動である。

1. 先頭で `llama_kv_self_clear` を呼び、**毎回 KV を捨てる**。prefix 再利用と
   両立しない。
2. `llama_batch_get_one` で組んだ batch は**最終位置の logits しか出さない**。
   NLL には位置ごとの logits が要る。

NllScorer 専用の decode ヘルパを設け、(a) KV をクリアしない、(b) 位置ごとに
logits 出力を要求する batch を組む、の 2 点を満たす。

> **実装時に確認する API 名**: 位置ごとの logits 要求（`llama_batch` の
> `logits[]` を立てる形）と seq 単位の KV 削除は、pin 中の llama.cpp
> （`AZOOKEY_LLAMA_CPP_GIT_TAG` = `05f6ac6…`）のヘッダで名前を確認すること。
> 既存コードが使う `llama_kv_self_*` 系はリネームが進行中の層であり、本書では
> 関数名を確定させない（契約は「prefix 長より後ろの KV を落とす」ことのみ）。

### B3.4 `llama_context` の共有とスレッド

- 生成経路と**同一の `llama_context`** を使う。モデルを 2 つ常駐させない。
- NllScorer は `InferenceEngine::QueryCandidates` が `state_mutex_` を保持した
  区間の中、`Convert` の**完了後**に走る。したがって context への同時アクセスは
  起きない。
- `Convert` の beam decode が KV を触るため、NllScorer は prefix を必ず 1 回
  decode し直す前提で予算を組む（§B12）。
- NllScorer を別スレッドへ出さない（`llama_context` は共有できない）。

## B4. 責務境界

### B4.1 IPC 責務境界（決定: 新規メッセージ型を作らない）

**Track B は IPC を一切変更しない。** 新しい `MessageType` を追加せず、
`QueryCandidates` の request / response payload（`ipc/include/azookey/ipc/Payloads.h`）
も変更しない。

理由:

- 候補集合は **TIP から送られてこない**。現行アーキテクチャでは
  `InferenceEngine::QueryCandidates` が user_dict + converter の出力を Host 内部で
  合成し（`docs/zenzai-inference-spec.md` §7.1）、TIP は読みと文脈を送るだけである。
  「TIP からリランク要求として候補集合を送る」形は現行の責務分割に無い。
- リランクは Zenzai の `llama_context` を要する。これは Host が単独で所有する資源で、
  プロセス境界をまたがせる理由が無い。
- 追加ラウンドトリップは、1 変換あたりのレイテンシ予算
  （`docs/zenzai-inference-spec.md` §8）を理由なく食う。

したがって NllScorer は `QueryCandidates` の内部フェーズであり、**TIP からは
「候補の並びが変わる」以外に観測されない**。

### B4.2 外部から観測できるもの

| 経路 | 内容 |
|---|---|
| `Candidate::debug_info` | 対象候補に `nll=<nll_per_char>;nlld=<bonus>` を追記（既存の `dup:` 併記と同じ流儀。表層・読みは含めない） |
| `Health` | **runtime 失敗のみ** `model_runtime_error_` へ `nll-scorer:<reason>` をミラーし、既存の `last_error` フィールドで運ばれる（§B8）。payload の形は変わらない |
| 構造化ログ（`azookey::logging::RuntimeLogger`） | 全 `reason`（正常な `disabled` / `no_target` を含む）と対象候補数。適用時は `nll_applied=<件数>` |

**`QueryDiagnostics` へは出さない（決定）。** 現行 `QueryDiagnosticsPayload`
（`ipc/include/azookey/ipc/Payloads.h`）は `model_loaded` / `engine` / `backend` /
`rss_mb` / EP 情報 / entry 数 / `fallback_state` / `last_error` のみを持ち、
`reason`（正常系を含む）や対象候補数を表すフィールドが無い。ここに出す要件を残すと
optional field 追加が必要になり、§B4.1 の「IPC を変更しない」と両立しない。

正常系を含む `reason` の観測先は**構造化ログに一本化する**。これは Track A が
fallback reason を構造化ログへ出し、M52 ベンチの `fallback_rate` 集計に使う設計
（§7.2）と同じ流儀であり、IPC を触らずに `fallback_rate` 相当の集計ができる。
`QueryDiagnostics` に載せたくなった場合は、後方互換な optional field 追加として
**別課題で IPC 互換性を含めて設計する**（Track B の範囲外）。

### B4.3 モジュール境界

- `core::IConverter` は変更しない。NLL 評価は変換ではなく、全 converter が
  持てるインターフェースではない。
- NllScorer は `ZenzaiModelConverter` が公開する評価 API（`llama_context` を
  持つ側のメンバ）として実装し、`InferenceEngine` は
  `MirrorModelRuntimeErrorLocked` と同じく `dynamic_cast<ZenzaiModelConverter*>` で
  到達する。Zenzai 以外が active な間は `model_not_loaded` で no-op になる。

### B4.4 変更対象ファイル（DEV-413 の見込み）

`inference-host/src/ZenzaiModelConverter.cpp`（NLL 評価 API・専用 decode ヘルパ）、
`inference-host/include/azookey/host/ZenzaiModelConverter.h`、
`inference-host/src/InferenceEngine.cpp`（適用点の挿入）、
`inference-host/include/azookey/host/InferenceEngine.h`（`EngineConfig` へ設定追加）、
`inference-host/tests/`、`settings/mvp-settings.schema.json`（§B9）。
`ipc/` `tsf-tip/` `core/` `learning/` は**変更しない**。

## B5. 対象候補集合

| `CandidateSource` | 対象 | 理由 |
|---|---|---|
| `SystemDictionary` / `Heuristic` | **対象** | 尤度の情報を持たない辞書・ヒューリスティック候補。Track B が埋める空白 |
| `UserDictionary` | 対象外 | ユーザーが登録した語の順位を LLM 尤度で動かさない（`docs/zenzai-inference-spec.md` §7.3 / §7.6 のソース優先と整合） |
| `Model` | 対象外 | Zenzai 生成候補。生成時の系列 logprob と二重評価になる（§B1.2） |
| `Llm` | 対象外 | M16 クラウド整文用の予約。性質が異なる |

- 上限 `nllTopK`（既定 **8**、範囲 1〜16）。適用直前の score 降順で上位から採る。
  候補あたり 1 回の decode が要るためコストは `K` に線形で、予算（§B7）は `K` で
  決まる。
- 対象が 0 件のときは何もしない（§B8 の `no_target`。失敗ではない）。

### B5.1 選択集合の外に対する保証

対象外ソースの候補と、`nllTopK` からあふれた候補について保証するのは
**`score` と `debug_info` が不変であること**に限る。**相対順位の不変は保証しない。**

最終順位は候補集合全体に対する score 降順の `stable_sort`（§B6.1）で決まるため、
選択集合内の候補が減点されれば、選択集合外の候補が自分の score を変えないまま
相対的に浮上しうる。例: `nllTopK=2` で対象 A=1.00 / B=0.95、選択集合外 C=0.94 の
とき、B に -0.30 が適用されれば C は B を追い越す。これは全体ソートを持つ以上
避けられず、**並べ替えという目的そのもの**でもある。

ただし §B6.2 が減点のみであることから、次の一方向の性質は保証される:

> **選択集合外の候補が、NLL 補正によって順位を下げることはない。** 補正は
> 選択集合内の候補を下げるだけなので、集合外の候補の相対位置は同じか上がるかの
> いずれかである。

したがって `UserDictionary` 候補（§B5 で対象外）は、NLL によって順位を落とされない
という保証を持つ。§B11 のテストもこの粒度で書く。

## B6. スコア合成（既存 `Reranker` との関係）

### B6.1 適用点

```
QueryCandidates:
  user_dict lookup → converter Convert → merge → DedupMergedCandidates
    → ★ NllScorer（本章）
    → ApplyRerankerOrRaw（learning::Reranker: 学習加点 + score 降順 stable_sort）
```

`DedupMergedCandidates` の**後**、`ApplyRerankerOrRaw` の**前**に置く。

この順序が DEV-413 の「既存 `Reranker` とは排他でなく補完」の具体化である。
学習加点はユーザー個人の確定履歴という最も強い個別化信号であり、最後に載せて
最終順位を支配できるようにする。NLL は辞書候補の**事前順位**を整えるだけの層で、
学習された選好を上書きしない。

`learning::Reranker::Apply` の実装（加算 → `stable_sort`）と最終順位規則
（`docs/zenzai-inference-spec.md` §7.3）は**変更しない**。

### B6.2 合成形（減点のみの相対補正）

score を**置換せず加算項**として合成する。対象集合内の最尤候補を基準にする。

```
nll_min   = min over 対象候補 i of nll_per_char_i
delta_i   = nll_min - nll_per_char_i                  // ≤ 0
bonus_i   = max(delta_i, -kNllSpan) * nllWeight       // ≤ 0
score_i  += bonus_i
```

- `kNllSpan` = **2.0**（nats/char。コード内定数）、`nllWeight` 既定 **0.15**。
  したがって 1 候補あたりの補正幅は **[-0.30, 0]** に収まる。
- **加点しない（減点のみ）**のは意図的である。最尤候補は 0 のまま据え置き、
  劣る候補だけが尤度差に応じて下がる。これにより:
  - 対象集合内の並びは「元の score − 尤度ギャップ」で決まり、目的である
    辞書候補の並べ替えは達成される。
  - `docs/zenzai-inference-spec.md` §7.2 の **score 帯を上向きに壊さない**。
    辞書候補が user_dict 帯（1.5）や Zenzai 帯上限（1.4）を NLL 由来で
    追い越すことが構造的に起こらない。
  - 帯からの下方向のはみ出しは `kNllSpan * nllWeight`（既定 0.30）に上限が
    あり、帯幅を超えて沈むことはない。
- 同点時は `stable_sort` により従前の順序（挿入順）が保たれる。NLL は独自の
  タイブレーク規則を導入しない。
- `debug_info` へ `nll=` / `nlld=` を追記する（§B4.2）。

### B6.3 Track A `final_score` との関係（インターフェース注記）

Track A §9 の `final_score` へ NLL を載せる場合は、`normalize(nll_score)` の
1 項として扱い、重みは M52 ベンチで校正する（§9 の他項と同じ扱い）。本章は
Track A の重み表を変更しない。§B6.2 の加算合成は、Track A が未導入の環境で
Track B が単独で成立するための形式である。

## B7. 予算・キャンセル・打ち切り

- リクエスト単位の予算 `nllBudgetMs`（既定 **20ms**、範囲 5〜60、範囲外は clamp）。
- 既存の deadline / cancel plumbing（`kModelConversionBudget`、
  `ConversionContext` の cancel、`docs/zenzai-inference-spec.md` §9.2.2）に載せる。
  NLL 用の別スレッド・別タイマーを作らない。
- **all-or-nothing**（決定）: 予算超過を検出した時点で、そのリクエストの NLL 適用を
  **全面的に破棄**し、NLL 適用前の score のまま続行する。部分適用はしない。
  - 理由: 部分適用では「先に評価された候補だけが減点される」ため、同じ入力でも
    実行時のゆらぎで順位が変わる。IME の決定性を優先する（Track A §7.2 が
    timeout に対して whole-request skip を選ぶのと同じ方針）。
  - 実装上は `Candidate::score` を直接書き換えず、bonus を別配列に貯め、
    **完走したときだけ**適用する。
- cancel は prefix decode の前、各候補の decode の前後で確認する。
- **cancel は失敗ではなく中断**（決定）: 現行 `QueryCandidates` は cancel 観測時に
  候補列そのものを返さない（空を返す）。したがって cancel 時の NllScorer は
  「fallback で順序を保つ」対象ですらなく、単に処理を放棄して cancel 経路へ抜ける。
  §B8 の `reason` を記録せず、circuit breaker にも計上しない（下記）。

## B8. fallback と失敗理由

いずれの場合も**候補列は NLL 未適用のまま返し**、`learning::Reranker` は通常どおり
走る。これが DEV-413 の「既存 `Reranker` のみへ縮退する経路」である。

| `reason` | 条件 | breaker 計上 |
|---|---|---|
| `disabled` | `nllRerankEnabled=false`（既定） | なし |
| `model_not_loaded` | Zenzai 未ロード / active converter が Zenzai でない（degraded 中を含む） | なし |
| `no_target` | 対象候補 0 件（no-op。失敗ではない） | なし |
| `secure_mode` | M46 secure 中（§B10） | なし |
| `budget_exceeded` | §B7 の予算超過 | **する** |
| （cancel） | ユーザー操作による中断。`reason` を記録しない（§B7） | なし |
| `infer_error` | decode 例外・logits 取得失敗・トークナイズ失敗 | **する** |
| `invalid_score` | 全対象候補が非有限 NLL（§B2.4） | なし（入力起因） |
| `circuit_open` | breaker 作動中 | なし |

- **例外を `QueryCandidates` の外へ出さない**。`ApplyRerankerOrRaw` と同じ流儀で
  握り潰し、Zenzai 由来の失敗として `model_runtime_error_` に
  `nll-scorer:<reason>` を記録する（汎用 `last_error_` には混ぜない。
  `docs/zenzai-inference-spec.md` §9.2.1 と整合）。
- **連続失敗の circuit breaker**: runtime 失敗（`budget_exceeded` / `infer_error`）が
  連続 `nllFailureThreshold`（既定 **3**）回に達したらセッション内で NllScorer を
  無効化し、以降は `circuit_open` で no-op にする。モデル再ロード / 設定変更 /
  Host 再起動で再有効化する。ユーザー設定 `nllRerankEnabled` とは独立した内部
  フラグである（Track A §7.2・M57 §5.4 と同じ runtime パターン）。
- `model_not_loaded` と `no_target` は正常な非適用であり、失敗として記録しない
  （degraded / 短い候補列で breaker が無意味に開くのを防ぐ）。
- **cancel を breaker に計上しない**。cancel の発生率はスコアラの健全性ではなく
  ユーザーの打鍵速度に相関するため、速く打つユーザーが連続 cancel しただけで
  breaker が開き、セッション中 NLL が黙って無効化されてしまう。予算超過
  （`budget_exceeded`）は計上する — 遅いマシンで慢性的に予算を超えるなら無効化が
  正しい応答であり、Track A が timeout を計上するのと同じ理由による。

## B9. 有効化フラグと既定 OFF 時の挙動不変

`settings/mvp-settings.schema.json` の `reranker` ブロック（§8）へ追加する:

```json
{
  "reranker": {
    "nllRerankEnabled": false,
    "nllTopK": 8,
    "nllWeight": 0.15,
    "nllBudgetMs": 20,
    "nllFailureThreshold": 3
  }
}
```

- 既定は **OFF**（`nllRerankEnabled=false`）。
- **既定 OFF のとき、prefix decode を含め Zenzai への追加呼び出しを一切行わない。**
  候補列・score・`debug_info`・ログ出力・レイテンシは Track B 導入前と等価であること
  （回帰テストで固定する。§B11）。
- 範囲: `nllTopK` 1〜16、`nllWeight` 0.0〜1.0、`nllBudgetMs` 5〜60、
  `nllFailureThreshold` 1〜10。範囲外は clamp（Track A §8 と同じ扱い）。
- schema への正式登録は DEV-413 の実装時に行う（Track A §8 と同じ運用）。
- 既定値（特に `nllWeight` / `nllTopK`）は M52 ベンチで校正する（§B12）。

## B10. プライバシー

- **M46 secure 中は NllScorer を実行しない**（`reason=secure_mode`）。
  `docs/privacy-and-secure-input-spec.md` §5 は secure 中の候補生成を
  「内蔵変換 + 既存辞書のみ」に限定しており、LLM を追加で走らせる本層はその契約に
  反する。ユーザー設定 OFF の `disabled` とは区別して記録し、プライバシー監査で
  識別できるようにする。M46（`PrivacyGate`）未実装の環境では判定手段が無いため
  本条は適用されない。M46 完了後に Host 側 gate を参照する配線を入れる
  （Track B の実装が M46 より先行しても、本条は M46 側の統合作業として扱う）。
- NllScorer は**学習データを書かない**。`LearningStore` にも Track A の露出トレース
  （§6.1）にも寄与しない。読み取り専用の評価層である。
- ログに `reading` / `surface` を出さない。`debug_info` に載せるのは数値のみで、
  redaction 方針（`docs/dev-infrastructure-spec.md` §7.6）に抵触しない。
- 外部送信は無い（ローカルモデルのみ）。

## B11. テスト契約

**unit（決定的）**

NLL の集約・正規化（§B2.2）と合成（§B6.2）、対象集合の選別（§B5）は
**llama.cpp に依存しない純関数へ切り出す**。これらは `AZOOKEY_WITH_LLAMA_CPP=OFF`
のビルドでも、固定 logprob 表を入力とする mock フィクスチャで検証できる。
decode 経路そのもの（§B3）は llama 有りビルドの test に置く。

- `NLL_total` と `nll_per_char` の算出: 固定 logprob 表を与え、期待値と一致する。
- **off-by-one**（§B3.2）: 位置ごとに異なる固定 logits を与え、`t_1` の logprob が
  prefix 最終位置由来であることを検証する。1 ずれれば落ちるフィクスチャにする。
- EOS を含めない（§B2.1）: 候補トークン列に EOS が混入していないこと。
- 合成（§B6.2）: 最尤候補の bonus が 0、他が負、絶対値が `kNllSpan * nllWeight` を
  超えない。
- **prefix logits スナップショット**（§B3.1）: 候補を 2 件以上与え、候補 2 の
  `t_1` の logprob が prefix 由来であること（候補 1 の出力由来だと落ちる
  フィクスチャにする）。
- 選択集合外の保証（§B5.1）: 対象外ソース（`UserDictionary` / `Model` / `Llm`）と
  `nllTopK` からあふれた候補の **`score` と `debug_info` が不変**であること。
  併せて、それらの候補が補正によって**順位を下げない**こと（減点のみの帰結）。
  順位そのものの不変は検証しない（全体ソートを持つ以上成立しないため）。
- `live=true` のリクエストで NllScorer が走らない（§B1）。
- fallback: §B8 の各 `reason` で、候補列が NLL 未適用の結果と完全一致する。
- all-or-nothing（§B7）: 途中で予算超過させたとき、部分的な減点が残らない。
- circuit breaker: 連続 3 失敗で無効化され、モデル再ロードで復帰する。
- **既定 OFF の回帰ゲート**: `nllRerankEnabled=false` で候補列が Track B 導入前と
  完全一致する（§B9）。

**integration（real model。pin モデルを要する `gate:human-required` 相当）**

- DEV-743 で締めた流儀に合わせ、合格条件は代表入力の **top candidate 完全一致**
  とする（「含む」では壊れた出力が通過するため）。
- 同音異義の代表ケース（左文脈あり / なし）で、NLL 適用前後の 1 位が期待どおりに
  入れ替わること、および対象外ソースの順位が動かないこと。

**bench**

- `bench/` に `K = nllTopK` での追加レイテンシを計測する case を足し、§B7 の予算内に
  収まることを示す。prefix decode の再実行コスト（§B3.4）を内訳として分離する。

## B12. 未確定事項（DEV-413 実装時に確定する）

- pin 中の llama.cpp における「位置ごとの logits を出す batch の組み方」と
  「seq 単位の KV 削除 API」の正確な名前（§B3.3）。
- `Convert` が KV を消すため NllScorer が prefix を必ず 1 回 decode し直す前提が、
  §B7 の予算（既定 20ms）に収まるか。収まらない場合は `Convert` と NllScorer で
  prefix decode を共有する最適化を別課題として起票する（本章の契約は変えずに
  実装内部で吸収できる範囲）。
- `nllWeight` / `nllTopK` / `nllBudgetMs` の既定値校正。M52 ベンチ
  （`docs/conversion-quality-benchmark-spec.md`）が整備された時点で実測に置き換える。
- 構造化ログに出す `reason` / 対象候補数のフィールド名と粒度（§B4.2。ログ形式の
  正典は `docs/dev-infrastructure-spec.md` §7 に従う）。
