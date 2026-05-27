# Tiny Neural Reranker 仕様

対象リポジトリ: dolquis/azooKey-Desktop
対応マイルストーン: M56（変換品質トラック）
関連: `plans/windows-port-roadmap.md` M52 / M53 / M54 / M55 / M57、
      `docs/conversion-quality-benchmark-spec.md`（M52）、
      `docs/dev-infrastructure-spec.md` §7.7（M51 trace）、
      `docs/modernbert-ja-scoring-spec.md`（M57）
作成日: 2026-05-27
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
- **embedder 共有**: 文脈・読み・候補の embedding 供給元を spec で
  固定（§4.1）

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
| `app_profile_score` | float | 0.08 | M48 |
| `candidate_length` | int | 4 | length |
| `segment_count` | int | 2 | M55 segments |
| `is_named_entity` | bool | false | M53 category |
| `is_user_dict` | bool | false | M9 |
| `is_neologism` | bool | false | M53 category |
| `is_typo_corrected` | bool | true | M55 |

合計 vector 次元: 128 × 3 + scalar 11 = 395 次元 + flags 4。

### 4.1 Embedding 供給元

**重要な設計判断**: 3 種類の embedding をどう供給するかを **M56 着手前
に固定する** 必要がある。以下のいずれかを選ぶ:

**Option A**: 独立小型 encoder（推奨）
- 自前の char-level CNN または BERT-tiny（〜10MB ONNX）
- 学習データから fine-tune
- ロード時間短、独立に最適化可能

**Option B**: ModernBERT 共用（M57 と並存）
- M57 の ModernBERT-Ja を embedder として再利用
- メモリ効率良いが、M57 未配置時は reranker 動作不能
- IME の embedder としては BERT は過剰

**Option C**: 計算により回避
- candidate / reading の char n-gram + reading-candidate edit distance
  などの**手作り特徴量で代替**し、embedding を使わない
- ONNX 不要、推論軽量、表現力は劣る

**初期実装は Option C → Option A の段階導入を推奨**:
- M56 v1（Option C 相当）: 手作り特徴量のみで MLP/LightGBM。学習データ
  収集と pipeline を確立
- M56 v2（Option A）: 独立小型 encoder を導入、+1〜2% の精度向上を狙う

詳細は §10 のモデル変遷で扱う。

## 5. モデル

### 5.1 候補

| 案 | 内容 | 段階 |
|---|---|---|
| LightGBM | 特徴量ベースの GBDT、CI で再現性高い | v0（baseline 用） |
| MLP reranker | 軽量 NN、ONNX 化 | v1（採用） |
| Mini Transformer | 文脈・読み・候補を直接入力 | v2（将来） |
| ModernBERT fine-tuned reranker | 高品質だが重い | M57 と統合 |

### 5.2 v1 MLP 構造

- 入力: §4 の手作り特徴量（embedding なし、Option C）
- 隠れ層: 2 層、各 64 ユニット、GELU
- 出力: 1 unit（rerank_score）
- パラメータ数: 約 6 KB（量子化なし）

### 5.3 ONNX export

PyTorch / scikit-learn で学習 → ONNX export。配置先:

```
%LOCALAPPDATA%\azooKey\models\tiny_reranker.onnx
```

または bundled として `models/` 配下に同梱（M28 MSIX 同梱可）。
モデルパスは `settings.reranker.tinyNeuralModelPath` で上書き可能。

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
- bundled モデルは公開コーパスから生成（青空文庫 / Wikipedia / 既存
  IME 公開データセット）

## 7. 推論仕様

| 項目 | 仕様 |
|---|---|
| 入力候補数 | 最大 30 |
| 出力 | rerank_score per candidate |
| timeout | 10〜20ms |
| fallback | 失敗時は reranker なしで返す（candidate 順序維持） |
| 実行頻度 | 通常変換ごと |
| backend | ONNX Runtime CPU 優先、GPU optional |
| バッチ | 全候補を 1 forward で処理 |

### 7.1 失敗 fallback

- ONNX load 失敗 → reranker 無効化、log 記録、続行
- 推論 timeout → 当該リクエストの rerank skip、log 記録、続行
- 入力 NaN/Inf 検出 → 当該候補だけ skip、他は継続

M47 `Recovering` / `DegradedModel` 状態と整合（Host は落ちない）。

## 8. 設定スキーマ

`settings/mvp-settings.schema.json` に追加:

```json
{
  "reranker": {
    "tinyNeuralEnabled": true,
    "tinyNeuralModelPath": "",
    "tinyNeuralTimeoutMs": 20,
    "modernbertEnabled": "auto",
    "modernbertTimeoutMs": 30
  }
}
```

`modernbertEnabled` は M57 が使う（本 spec では枠のみ予約）。

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
| v3 | Mini Transformer | Option A or B | baseline 比 +5% 以上 |

本 M56 は **v0 → v1** までを範囲とし、v2 以降は将来 M に分離する。

## 11. テスト

- unit: 特徴量抽出（NaN / 欠損値）
- unit: ONNX load 失敗時の fallback
- unit: timeout 時の fallback
- integration: M52 ベンチで top1 +3% / p95 latency +10ms 以内
- integration: M55 typo 候補に対する正しい boost / penalty
- snapshot: ONNX schema 固定（input / output 名）

## 12. M56 受け入れ条件

- M52 ベンチで `top1_accuracy` が baseline 比 +3% 以上
- M52 ベンチで `latency_p95_ms` 悪化が +10ms 以内
- timeout / load 失敗時に reranker なしで候補が返る（IME が止まらない）
- v0（LightGBM baseline）と v1（MLP）の比較レポートが残る
- embedding 供給方針（§4.1）が spec に明記されている

## 13. プライバシー

- M46 secure 中は reranker を無効化（学習データへの影響を避ける）
- 学習データは個人ローカルでのみ収集（クラウド送信なし）
- bundled モデルは公開コーパス由来のみ

## 14. 将来拡張（M56 範囲外）

- v2 以降の embedding 強化
- Mini Transformer 化
- 個人 fine-tune（プライバシーを保ったまま）
- M57 ModernBERT との 2 段 rerank（M57 が ambiguous 候補だけ精査）
