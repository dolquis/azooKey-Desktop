# Tiny Neural Reranker 仕様

対象リポジトリ: dolquis/azooKey-Desktop
対応マイルストーン: M56（変換品質トラック）
関連: `plans/windows-port-roadmap.md` M52 / M53 / M54 / M55 / M57、
      `docs/conversion-quality-benchmark-spec.md`（M52）、
      `docs/dev-infrastructure-spec.md` §7.7（M51 trace）、
      `docs/modernbert-ja-scoring-spec.md`（M57）
作成日: 2026-05-27
更新日: 2026-06-26（DEV-112: embedding 供給方針・特徴量の重み付け・学習データ
        ラベリングプロトコル・timeout / fallback 閾値を確定）
位置づけ: 変換品質トラック（M53 / M54 / M55 のすべてが完了し、M52 ベンチで
baseline 固定後）

## 1. 目的

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
- candidate / reading の char n-gram + reading-candidate edit distance
  などの手作り特徴量で代替し、embedding を使わない
- ONNX は最小 MLP のみ（embedder 不要）、推論軽量、CPU で完結
- → **v1 で採用**（§5.2 / §10）

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

- M54 の `correction_events` から自動生成（オフライン）
- 個人ユーザーの学習データを **送信せず**、ローカルで個人 fine-tune（v2 で
  検討）
- bundled モデルは公開コーパスから生成（青空文庫 / CC0・MIT 互換の
  IME 公開データセット）。M52 §11 と同じく CC BY-SA / GFDL 系
  （Wikipedia 等）は採用しない

### 6.2 ラベリングプロトコル（決定）

正例・負例・強負例の定義と採否条件を以下に固定する。曖昧な収集はノイズになり
M52 ベンチの精度評価を歪めるため、採否条件まで spec で固定する。

| ラベル | 定義 | 採否条件 |
|---|---|---|
| 正例（`label`=1） | 同一変換機会でユーザーが確定した候補 | 候補ウィンドウに**表示された**こと。確定が即時取り消し（直後の undo / 再変換）された場合は除外 |
| 負例（`label`=0） | 同一機会で表示されたが選ばれなかった候補 | **表示された候補のみ**を対象とする（表示されなかった候補は負例にしない＝露出バイアスを入れない）。1 機会あたり負例は上位 N（既定 `trainNegativesPerSample`=8）まで |
| 強負例（`label_strength`=`strong_negative`） | 訂正イベントで reject され別候補へ訂正された候補 | M54 `correction_events` 由来（§6.1）。`rejected_candidate` と `accepted_candidate` のペアで保持。確定後**一定時間内**（既定 `correctionWindowMs`=10000）の訂正のみ採用 |

追加の採否・整形ルール:

- **重複排除**: 同一 `(normalize(left_context), input, candidate)` のサンプルは
  集約し、label を多数決でまとめ、出現回数を frequency 系特徴の補強に使う。
- **滞留時間ゲート**: 候補ウィンドウ表示から確定までが極端に短い（誤確定の疑い、
  既定 `minDwellMs`=120 未満）サンプルは正例から除外する。
- **セッション境界**: `left_context` はその確定時点の**確定済みテキスト**を使い、
  後続編集の影響を混ぜない（§7 推論時の context 定義と一致させる）。
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
- **連続失敗の circuit breaker**: ONNX load 失敗・推論例外・timeout が**連続
  `tinyNeuralFailureThreshold`=3 回**に達したら、当該セッションで reranker を
  **無効化**し（runtime 状態）、以降は fallback（candidate 順序維持）で返す。
  次回モデル再ロード / セッション再初期化で再有効化する。これは M57
  （`docs/modernbert-ja-scoring-spec.md` §5.4）の circuit breaker と同じ runtime
  安全パターンであり、ユーザー設定 `tinyNeuralEnabled` とは独立した内部フラグである。
- **部分適用**: 入力 NaN / Inf を含む候補は当該候補のみ skip し、他候補には rerank を
  適用する（§7.1）。全候補が skip された場合は順序維持で返す。
- **fallback は失敗ではなく既定パス**: 上記いずれの fallback も
  `tiny_used=false, reason=<load_failed | timeout | circuit_open | disabled>` を
  構造化ログに記録し、M52 ベンチの `fallback_rate`
  （`docs/conversion-quality-benchmark-spec.md`）集計に使う。

## 8. 設定スキーマ

`settings/mvp-settings.schema.json` に追加:

```json
{
  "reranker": {
    "tinyNeuralEnabled": true,
    "tinyNeuralModelPath": "",
    "tinyNeuralTimeoutMs": 15,
    "tinyNeuralFailureThreshold": 3,
    "modernbertEnabled": "auto",
    "modernbertTimeoutMs": 30
  }
}
```

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

- M46 secure 中は reranker を無効化（学習データへの影響を避ける）
- 学習データは個人ローカルでのみ収集（クラウド送信なし）
- bundled モデルは公開コーパス由来のみ

## 14. 将来拡張（M56 範囲外）

- v2 以降の embedding 強化
- Mini Transformer 化
- 個人 fine-tune（プライバシーを保ったまま）
- M57 ModernBERT との 2 段 rerank（M57 が ambiguous 候補だけ精査）
