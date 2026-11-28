---
kind: spec
milestones: [M56]
canonical_for: ["Tiny Neural Reranker"]
---

# ニューラルリランカー仕様

対象リポジトリ: dolquis/azooKey-Desktop
関連: `plans/windows-port-roadmap.md` M52 / M53 / M54 / M55 / M57、
      `docs/conversion-quality-benchmark-spec.md`（M52）、
      `docs/observability-spec.md` §7.7（M51 trace）、
      `docs/modernbert-ja-scoring-spec.md`（M57）、
      `docs/neural-reranker-nll-spec.md`（Track B）、
      `docs/zenzai-inference-spec.md`

## 0. 2 トラックと本書の範囲

「候補を並べ替える（生成しない）」層には、独立した 2 つのリランカートラックがある。
本書は Track A を定め、Track B は `docs/neural-reranker-nll-spec.md` が定める。
両者は**別モデル・別実装・別スケジュール**であり、一方の完了を他方の前提にしない。

| トラック | 章 | 対象 | スコア源 | 前提 |
|---|---|---|---|---|
| **Track A: Tiny Neural Reranker**（M56） | §1〜§14 | 全ソースの候補 | ONNX MLP + 手作り特徴量 | M52 + M53 + M54 + M55 |
| **Track B: NllScorer 型ニューラルリランク**（DEV-413。`docs/neural-reranker-nll-spec.md`） | §B1〜§B12（別文書） | 辞書由来候補のみ | ロード済み Zenzai の系列 NLL | M8（Zenzai 配線）のみ |

- 章番号は分離する。他ドキュメントからの既存参照（`neural-reranker-spec` §4 /
  §7 / §9 等）はすべて Track A を指す。Track B は `docs/neural-reranker-nll-spec.md` の
  `§B<n>` で参照する。
- Track B の Release 測定記録は `docs/decisions/2026-09-08-nll-release-measurement.md` にある
  （結論の正典は `docs/neural-reranker-nll-spec.md` §B12）。
- 両トラックの接点は**スコア統合レイヤのみ**（`docs/neural-reranker-nll-spec.md` §B6）。Track B は Track A の
  `features_v1` 契約（§5.2）を変更しない。

## 0.1 節とマイルストーンの対応

本書の節と、その範囲を定める `plans/windows-port-roadmap.md` のマイルストーンの対応を次に示す。

| 節 | マイルストーン |
|---|---|
| §0 | 共通（2 トラックの構成） |
| §1〜§11 | Track A（M56） |
| §12 | Track A（M56）。受け入れ条件の正典は roadmap の M56 節 |
| §13〜§14 | Track A（M56） |
| §B1〜§B12 | `docs/neural-reranker-nll-spec.md`（Track B。節→M 対応は同書） |

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

候補生成と重複除去の後、NLL を適用する経路では NLL の後、`ApplyRerankerOrRaw` の前に
TinyNeuralReranker を適用する。`ExtractFeaturesV1` が最大 30 候補の生信号を §4 の
順に並べ、MLP が候補ごとのスコアを返す。既存の学習加点は `ApplyRerankerOrRaw` で行う。

同じ表層を重複除去する際は、既存の順位規則で代表候補を選ぶ。生の辞書スコアと
Zenzai の平均 logprob は別々に保持する。各スコア、typo confidence、ラティスの
文節数は、代表候補に値があればその値を使い、欠損していればもう一方の値を引き継ぐ。
flag は論理和を取る。重複除去後の `candidate_length` は代表候補の表層から算出する。

## 4. 入力特徴量

| 特徴 | 型 | 例 | 出所 |
|---|---|---|---|
| `left_context_embedding` | vector<128> | 文脈 embedding | §4.1 |
| `reading_embedding` | vector<128> | こうしょうする | §4.1 |
| `candidate_embedding` | vector<128> | 交渉する | §4.1 |
| `zenzai_score` | float | -1.82 | Zenzai の生の平均 logprob |
| `dictionary_score` | float | 0.64 | 辞書検索のスコア（§5.4） |
| `user_frequency` | float | 0.25 | M54 |
| `recency_score` | float | 0.41 | M54 |
| `typo_confidence` | float | 0.82 | M55 |
| `app_profile_score` | float | 0.0 | v1 は 0.0 固定 |
| `candidate_length` | int | 4 | 表層の Unicode コードポイント数 |
| `segment_count` | int | 2 | ラティスの文節数、それ以外は 1 |
| `is_named_entity` | bool | false | 辞書カテゴリ bit 1、2、3、4、7 のいずれか |
| `is_user_dict` | bool | false | 出所に `LayerId::User` を含む |
| `is_neologism` | bool | false | 辞書カテゴリ bit 9 |
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

v1 の `app_profile_score` は **0.0** に固定する。M48 のタグ加点は
`ApplyRerankerOrRaw` の後に行われるため、特徴抽出の時点では供給されない。

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

1. **CPU で動作するとは限らない**: M57 は ModernBERT を許可した実行 EP でのみロードし、
   CPU EP は計測ゲートに合格するまで許さない
   （`docs/modernbert-ja-scoring-spec.md` §4.0 順 3 / §6.3）。M56 reranker は
   §2 のとおり **CPU 優先で常時動作**する前提のため、embedder を ModernBERT に
   従属させると CPU-only 環境で embedding 特徴が常に欠損する。
2. **常時ロードされない**: M57 は ambiguity ゲート・RSS ロードゲート・circuit
   breaker により**条件付きにしか起動しない**（`docs/modernbert-ja-scoring-spec.md` §4.0 / §6.1）。embedder の
   可用性が実行時条件に左右されると、M56 の特徴量パイプラインが非決定的になり、
   M52 ベンチの再現性（baseline +3%）を測れない。
3. **配置フェーズが異なる**: M57 は v1.0 後トラックで、モデルを同梱せずダウンロードで
   配置する（`docs/modernbert-ja-scoring-spec.md` §6）。M56 を M57 配置に従属させると M56 単独で acceptance を
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
- ONNX 入力名: `features_v1`（fp32、shape `[batch, 12]`）。`features_v2`
  （shape `[batch, 396]`、embedding 込み）は v2 以降で導入
- 隠れ層: 2 層、各 64 ユニット、GELU
- ONNX 出力名: `rerank_score`（fp32、shape `[batch, 1]`）
- パラメータ数: 5,057（バイアス込み）。fp32 で約 20 KB

runtime FeatureExtractor は v1 / v2 で別ビルダーを持ち、ONNX のメタ情報
`model_version`（`v1` / `v2`）から入力形を切り替える。両者を同時に
ロードする場合は version ごとに別 session を保持する。

### 5.3 ONNX export

PyTorch / scikit-learn で学習したモデルを ONNX に export する。Host は
`reranker.tinyNeuralModelPath` の明示パス、`%LOCALAPPDATA%\azooKey\models\tiny_reranker.onnx`、
Host 実行ファイルに隣接する `models/tiny_reranker.onnx` の順に解決する。
明示パスは絶対パスに限る。無効な場合は他のパスを探索せず `load_failed` とする。
明示パスが空の場合は、既定の候補のうち最初に見つかる通常ファイルを使う。

### 5.4 特徴量の重要度と重み付け（決定）

§4 の 12 次元（v1）/ 396 次元（v2）の特徴量について、**個々の重みは手動で
固定しない**。決定方針を以下に固定する:

- **v0（LightGBM）を feature importance の基準器とする**: v0 baseline で
  gain / split ベースの feature importance を算出し、低寄与特徴の除外候補・
  正規化方針の根拠とする。importance は M52 ベンチ成果物として残す
  （roadmap M56 の v0 / v1 比較レポートに含める）。
  v0 はオフライン評価で使用し、Host の推論は v1 を使う。
- **v1（MLP）の重みは学習で決定する**: 隠れ 2 層 × 64 unit の重みは学習データ
  （§6）から学習し、手動調整しない。特徴の寄与は学習後の ablation（特徴を
  1 つ落として再評価）で確認し、roadmap M56 の比較レポートに記す。
- **特徴量の正規化を ONNX に焼き込む**: scalar 特徴（`zenzai_score` /
  `dictionary_score` / `user_frequency` / `recency_score` / `typo_confidence` /
  `app_profile_score`）は学習データ統計から推定した per-feature 標準化
  （z-score または min-max）を ONNX グラフ内に持たせ、runtime では生値を入力する。
  学習データで標準偏差が 0 の特徴には平均 0、スケール 1 を使い、入力値をそのまま通す。
  正規化統計はモデルと一緒に versioning する。flag / int 特徴
  （`candidate_length` / `segment_count` / 4 つの bool）は正規化せず素の値
  （bool は 0 / 1）を入力する。
- **生スコアの尺度**: `zenzai_score` は生成時の総 logprob を対象トークン数で
  割った値。`dictionary_score` は `DictionaryCandidates` が受け取る
  `DictionaryEntry::score` の値。どちらも Host で標準化や再スケールは行わない。
  Engine が直接作るユーザー辞書候補の `dictionary_score` には、語の明示スコアを使い、
  省略時は `user_word_default_score` を使う。
  `user_frequency` と `recency_score` は Host が供給する `UserSignals` の値を使う。
- **欠損特徴の既定値**: Zenzai の平均 logprob が無い場合や有限でない場合は
  `zenzai_score` を −10.0 とする。
  `dictionary_score`、`user_frequency`、`recency_score`、`typo_confidence` は
  欠損時に 0.0。文節数と flag の既定値はそれぞれ 1 と false。
  `app_profile_score` は §4 のとおり 0.0 に固定する。
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
  - `left_context_hash`（`user-learning-enhancement-spec.md` §8.1 と同方式。raw 文脈は持たない）/ `reading` /
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
  - v1 の `typo_confidence` と `is_typo_corrected` は、補正候補が適用点より後に加わるため、
    推論時も学習時もそれぞれ既定値の 0.0 と false を使う。
- **強負例は露出トレースの訂正レコードを源とする**: M54 v1 の集約 TSV は単一
  `surface` + `event_type` + `context_hash` のみで、`rejected_surface` /
  `accepted_surface` のペアや event timestamp を durable に持たない（ペア保持は
  将来の SQLite オプション、`user-learning-enhancement-spec.md` §8）。そのため強負例
  と `correctionWindowMs` は上記トレースの訂正レコードから生成する（M54 集約 TSV に
  依存しない）。M54 SQLite オプションが有効な環境ではそのペアテーブルを併用してよい。
- 個人ユーザーの学習データを **送信せず**、ローカルで reranker を個人 fine-tune（v2 で検討）。
  Zenzai 本体の個人 fine-tune は採らない（`docs/zenzai-inference-spec.md` §9.4）。
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
  `correction_events` 由来データは reranker のローカル fine-tune（v2 で検討）の範囲に留め、
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
| 適用経路 | 通常・ライブ・混在変換、予測、訂正（secure 要求を除く） |
| backend | ONNX Runtime の CPU EP（§7.3） |
| バッチ | 適用直前のスコア順で最大 30 件を選び、1 forward で処理。同点は入力順で決め、31 件目以降の加算は 0 |

### 7.1 失敗 fallback

- ONNX load 失敗 → reranker 無効化、log 記録、続行
- 推論 timeout → 当該リクエストの rerank skip、log 記録、続行
- 入力 NaN/Inf 検出 → 当該候補だけ skip、他は継続

上記の失敗はいずれも**要求単位の fallback**であり、M47 の健康状態機械
（`docs/host-availability-spec.md` §8.5.1）は遷移させない。reranker の失敗で
`HealthEvent::ModelFailed` を発火せず、`DegradedModel` / `RecoveringModel` にも入らない。
`ModelFailed` を発火するのは Zenzai モデルのロード失敗だけである。Host は落ちない。

`Health` への現れ方は次のとおり。

- reranker の失敗は状態機械の状態を変えない。`QueryDiagnostics.fallback_state`
  （状態機械の状態と設定から決まる）と §8.5.4 の劣化表示（Zenzai を使えない旨）は
  reranker の失敗では出ない。
- 既存の `learning::Reranker` 経路（`ApplyRerankerOrRaw`）が例外を握り潰して raw 候補を
  返すときは、汎用の `last_error_` に固定の理由語を記録する。`Health.last_error` は
  `last_error_` を `model_runtime_error_` より優先して返すため、この値は Health の
  `last_error` に出る。`HealthPayload.status` は `last_error` があればモデルがロード済みまたは
  パス設定済みのとき `degraded`、モデル未ロードかつパス未設定なら `error` になる。
  これは `last_error` から決まる粗い表示値であり、状態機械の遷移ではない。
- Zenzai 由来の NLL スコアラの runtime 失敗だけは、`docs/neural-reranker-nll-spec.md` §B8 のとおり `model_runtime_error_` へ
  `nll-scorer:<reason>` をミラーする（`nll-scorer:infer_error` など。§B8 の `reason`）。
  これに対し、Track A（tiny reranker）の §7.2 の `reason` 群（`timeout` / `infer_error` など）は
  構造化ログと `fallback_rate` 集計に使い、Health へはミラーしない。

### 7.2 timeout / 失敗の閾値（決定）

- **timeout 既定値**: `tinyNeuralTimeoutMs` = **15ms**（範囲 10〜20ms、範囲外は
  clamp）。v1 MLP（12 次元・隠れ 64×2）の 30 候補一括 forward は CPU でも通常
  1〜2ms で完了するため、timeout は**病的な stall（ONNX session の一時的ハング等）
  に対する安全弁**であり、通常 path の p95 を支配しない。これにより roadmap M56 の
  「p95 latency 悪化 +10ms 以内」を脅かさない。
  開始時に要求の deadline までの残り時間が tiny の timeout より短ければ、forward を始めずにスキップする。
  watchdog は期限後の出力を破棄し、実行中の forward に停止を通知する。
  要求の deadline が先に来るか同時ならスキップし、tiny 自身の timeout が先に来た場合に限って
  `timeout` として数える。前の forward がまだ実行中なら新しい forward は始めない。
  前の要求が tiny 自身の timeout で終わった場合は新しい要求も `timeout` として数え、
  取り消しや要求の deadline で終わった場合は新しい要求もスキップする。スキップは失敗回数に含めない。
  このスキップは下記の fallback reason にも含めない。
- **ロード失敗は即時無効化（breaker とは別経路）**: ONNX load 失敗時は §7.1 の
  とおり当該セッションで reranker を**即時無効化**し、`reason=load_failed` で
  fallback する。使えるセッションが残らず後続のリクエスト失敗を生まないため、
  circuit breaker の連続カウントには**載せない**。次回モデル再ロード（設定変更・
  Host 再起動）で再評価する。
- **連続失敗の circuit breaker**: **runtime の失敗**（推論例外 `infer_error` /
  timeout `timeout`）が**連続 `tinyNeuralFailureThreshold` 回**に達したら、当該
  セッションで reranker を**無効化**し（runtime 状態）、以降は fallback（candidate
  順序維持）で返す。次回モデル再ロード / セッション再初期化で再有効化する。これは
  M57（`docs/modernbert-ja-scoring-spec.md` §5.4）の circuit breaker と同じ runtime
  安全パターンであり、ユーザー設定 `tinyNeuralEnabled` とは独立した内部フラグである。
  連続失敗は要求単位で数え、予測の最初のバッチが失敗したら次のバッチを実行せず、どちらかが失敗した要求では両方の加算を破棄する。
  `load_failed`（上記）と `invalid_input`（下記、入力起因）は breaker に載せない。
  閾値は既定 3、範囲 1〜10 とし、範囲外は clamp する。
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

### 7.3 推論ランタイム（決定）

M56 の `tiny_reranker.onnx` と M57 の `modernbert-ja-70m` の ONNX モデルは、同じ推論ランタイムで Host が動かす。
本節を両者の正典とし、M57 固有の実行 EP の条件は `docs/modernbert-ja-scoring-spec.md` §6.3 が持つ。
比べた候補と根拠は [decisions/2026-11-20-onnx-runtime-for-reranker.md](decisions/2026-11-20-onnx-runtime-for-reranker.md) にある。

| 項目 | 決定 |
|---|---|
| ランタイム | Microsoft が配布する ONNX Runtime（ORT）を、C API で使う。同梱する ORT で使う実行 EP は CPU EP だけとする。GPU / NPU の EP は、R2 の再開後に Windows ML の ORT へ移してから使う（下の行、M57 は `docs/modernbert-ja-scoring-spec.md` §6.3）。DirectML EP は入れない（DirectML は Microsoft が新しい機能を足さない maintenance mode にある） |
| Zenzai の R2 との関係 | R2（`docs/copilot-pc-backend-spec.md` §4.1 の ONNX Runtime GenAI + Windows ML）が止まっている間は、ランタイムを共有しない。R2 を再開した後も、同梱する素の `onnxruntime.dll` は MSI に残す。Host は 1 つのプロセスで ORT を 1 つだけ読む。Windows ML の ORT を読める Host（OS build が 26100 以上で、Windows App SDK の bootstrap に成功した Host）は、M56 と M57 の CPU EP を含め、すべて Windows ML の ORT で動かす。読めない Host（26100 未満、bootstrap の失敗）は、素の ORT を読み、CPU EP だけを使う。どちらを読むかは Host が最初に ORT を使うときに決め、そのプロセスの間は変えない。Windows ML の ORT は ORT と同じ API を持つので、どちらでも推論の呼び出しは変えない |
| 依存の取り込み | ORT の公式の prebuilt を、URL と SHA256 で固定して取得する（`docs/build-ci-spec.md` §3.1 の 3）。取得は opt-in の `AZOOKEY_FETCH_ONNXRUNTIME`（既定 OFF）で行う。取得しない build は、reranker を mock の runtime で組む（llama.cpp を取り込まない build と同じ扱い）。既定の dev の configure は OFF のままとし、ネットワークに触れない。release 系の preset（`windows-release` など）は ON にし、release 系の preset で OFF のときは configure を失敗させる（Vulkan の preset が llama.cpp を要求するのと同じ扱い）。preset の変更は取り込む PR で行う。Linux の build は mock だけとする。NuGet restore を CMake に足すこと、ORT をソースから build すること、vcpkg は採らない |
| x64 / ARM64 | x64 と ARM64 の両方の prebuilt を固定する。取得元は ORT の GitHub release の zip とし、その arch の zip が無ければ NuGet の `Microsoft.ML.OnnxRuntime` の nupkg とする（どちらも公式の配布物）。ARM64 の配布物への同梱は M27 の出荷に合わせる |
| 配布 | `onnxruntime.dll` を base MSI の `azookey_inference_host.exe` の隣に置く。増えるサイズは取り込む PR で実測して記録する。M56 のモデルの配置は §5.3 に従う。M57 のモデルは同梱せずダウンロードする（`docs/modernbert-ja-scoring-spec.md` §6）。取り込む PR で、base MSI の内容（`docs/sideload-packaging-spec.md` §4.1・§4.4）と配布チャネルごとの同梱物の表（`docs/msix-packaging-spec.md` §1.6、`docs/copilot-pc-backend-spec.md` §8.3）に `onnxruntime.dll` を足す。`onnxruntime.dll` が MSVC runtime の DLL を import するなら、同じ PR で `scripts/check-app-local-runtime.ps1` の検査対象に加える |
| 読み込み | `onnxruntime.dll` を import library でリンクせず、実行時に `LoadLibraryExW` で読む。Host の隣の絶対パスと `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS` を渡し、ORT の入口 `OrtGetApiBase` を `GetProcAddress` で得る（ORT の C API はこの関数が返す表だけを入口にする）。import しないので delay-load の仕組みは要らない。絶対パスで読むのは、Windows の `System32` に別の版の `onnxruntime.dll` があり、名前だけで読むと取り違えるためである。`LOAD_LIBRARY_SEARCH_APPLICATION_DIR` だけを渡すと、依存 DLL の探索から `System32` が外れるため、`DEFAULT_DIRS` を合わせて渡す（Microsoft Learn の `LoadLibraryExW`）。DLL が無い、要求する API の版を返さない、session を作れないときは Host を落とさず、reranker を無効にする |
| fallback | 上の読み込みの失敗は、§7.1 と §7.2 の `reason=load_failed` に含める（M57 は `docs/modernbert-ja-scoring-spec.md` §4.0 順 4 の `not_loaded`）。原因の内訳（DLL が無い、API の版が合わない、など）は構造化ログにだけ出し、`reason` の値の集合と IPC・設定は変えない |
| BackendSelector | M24 の `BackendSelector`（Zenzai の backend の判定）には加えない。reranker は自分のロードの成否で使えるかを決める |
| ライセンス | ORT は MIT License（© Microsoft）。prebuilt に同梱される第三者表記（ThirdPartyNotices）は、配布物の第三者表記へ全文を引き継ぐ。取り込む PR で `THIRD_PARTY_LICENSES` に同梱の節を足し、リリース SBOM の生成（`scripts/complete-release-sbom.py` の source の種別）を URL と hash の pin に対応させる。表記の範囲は Human Gate の DEV-2174 で確定し、取り込む PR は DEV-2174 の確定の後に出す |
| opset | ONNX export の opset は、固定した版の ORT が読める範囲で、M56 は学習パイプラインが、M57 は変換スクリプト（`docs/modernbert-ja-scoring-spec.md` §6.4）が 1 つの値に固定する。ORT の版を上げるときは、固定した opset を読めることと latency を確かめる（`docs/build-ci-spec.md` §3.4 の比較項目） |
| 量子化 | M56 のモデルは fp32 とする。M57 の量子化は `docs/modernbert-ja-scoring-spec.md` §6.3（ロードする量子化）と §6.4（作り方）に従う |
| スレッド数 | ORT の intra-op / inter-op のスレッド数は明示して設定し、ORT の既定（全コア）は使わない。llama.cpp の推論スレッド数（`docs/copilot-pc-backend-spec.md` §6.2）と合わせて論理コア数を超えない値にする。M56 の値は取り込む PR の計測で、M57 の CPU EP の値は `docs/modernbert-ja-scoring-spec.md` §6.3 の計測ゲートで決める |

## 8. 設定スキーマ

`settings/mvp-settings.schema.json` の `reranker` ブロックの既定値:

```json
{
  "reranker": {
    "tinyNeuralEnabled": false,
    "tinyNeuralModelPath": "",
    "tinyNeuralTimeoutMs": 15,
    "tinyNeuralFailureThreshold": 3
  }
}
```

- `tinyNeuralEnabled`: 既定は **OFF**。Track B の `nllRerankEnabled`（`docs/neural-reranker-nll-spec.md` §B9）と同じく、
  リランク層は roadmap M56 の受け入れ条件（M52 ベンチの top1 と p95 の閾値）を
  満たしたことを確認したうえで、既定 ON への切替を別途判断する。**既定 OFF のとき
  ONNX セッションを生成せず**、候補列・`final_score`・`debug_info`・レイテンシは Track A
  導入前と等価であること（§11 の回帰対象）。ユーザー設定 OFF による非適用は
  `reason=disabled` で記録する（§7.2）。
- `tinyNeuralTimeoutMs`: 範囲 10〜20（§7.2）。範囲外は clamp。
- `tinyNeuralModelPath`: 明示パス、ユーザー領域、同梱モデルの順に解決する（§5.3）。
  明示パスは絶対パスに限る。無効なら `load_failed` とし、ほかの候補は探索しない。
- `tinyNeuralFailureThreshold`: §7.2 の circuit breaker 閾値。既定 3、範囲 1〜10。
  範囲外は clamp。
- `reranker.modernbert*`（`modernbertEnabled` / `modernbertTimeoutMs` 等）は M57 が使う。
  キー・既定値・範囲の正典は `docs/modernbert-ja-scoring-spec.md` §7。
- §6.2 の学習パイプラインパラメータ（`trainNegativesPerSample` /
  `correctionWindowMs` / `minDwellMs` 等）はオフライン用であり、本 runtime schema
  には登録しない。

## 9. スコア統合

M56 は適用点の候補スコアに上限付きの項を加える。`score_before_tiny` は、候補生成・統合と、
条件を満たす経路の NLL 補正を反映したスコアとする。

```
score_after_tiny = score_before_tiny + 0.15 * clamp(rerank_score, 0, 1)
```

係数 0.15 は tiny 項の初期重みを引き継ぎ、v1 の sigmoid 出力による加算を最大 0.15 に収める。
非適用時の加算は 0 とする。加算後に既存の `ApplyRerankerOrRaw` とタグ加点を行う。
M57 を適用する場合は `0.05 * normalize(modernbert_score)` を加える。適用条件と fallback は
`docs/modernbert-ja-scoring-spec.md` §4 と §8 に従う。重みの校正には M52 ベンチを使う。

本節は `final_score` 式の正典であり、`docs/modernbert-ja-scoring-spec.md` §8、
`docs/zenzai-inference-spec.md` §7.5、`docs/app-profile-spec.md` §7、`docs/neural-reranker-nll-spec.md` §B6.3 から参照される。

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

- unit: §4 の特徴名と並び、共有 golden JSON の 12 要素、重複候補の生信号の保持、
  NaN / Inf と欠損値の扱い
- unit: 既定 OFF（`tinyNeuralEnabled=false`）で ONNX セッションを生成せず、候補列・
  `final_score` が Track A 導入前と等価（§8）
- unit: ランタイムなし、ロード失敗、secure 要求での fallback
- unit: 最大 30 件の選定・同点順、加算の上限、出力の件数不一致や非有限値での全件 fallback
- unit: timeout 時の fallback
- unit: 連続失敗で circuit breaker が開き、再ロードで閉じる（§7.2）
- integration: M52 ベンチで top1 +3% / p95 latency +10ms 以内
- unit: M55 から値が渡らない場合は `typo_confidence`=0.0、
  `is_typo_corrected`=false
- snapshot: ONNX schema 固定（input / output 名）

## 12. M56 受け入れ条件

受け入れ条件の定義の正典は [`plans/windows-port-roadmap.md`](../plans/windows-port-roadmap.md)
の M56 節とする。本書は特徴量・モデル構造・学習データ・timeout と fallback を定義し、
受け入れ条件を複製しない。

## 13. プライバシー

- M46 secure 中は reranker を無効化（学習データへの影響を避ける）。fallback は
  `reason=secure_mode` で記録し（§7.2）、ユーザー設定 off の `disabled` と区別する
- 学習データは個人ローカルでのみ収集（クラウド送信なし）
- bundled モデルは公開コーパス由来のみ

## 14. 将来拡張（M56 範囲外）

- v2 以降の embedding 強化
- Mini Transformer 化
- reranker の個人 fine-tune（プライバシーを保ったまま。Zenzai 本体の個人 fine-tune は
  `docs/zenzai-inference-spec.md` §9.4 で採らないとした）
- M57 ModernBERT との 2 段 rerank（M57 が ambiguous 候補だけ精査）
