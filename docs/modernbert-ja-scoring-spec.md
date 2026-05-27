# ModernBERT-Ja 候補スコアリング 仕様

対象リポジトリ: dolquis/azooKey-Desktop
対応マイルストーン: M57（変換品質トラック）
関連: `plans/windows-port-roadmap.md` M24 / M52 / M56、
      `docs/conversion-quality-benchmark-spec.md`（M52）、
      `docs/neural-reranker-spec.md`（M56）、
      `docs/copilot-pc-backend-spec.md`（M24 backend）
作成日: 2026-05-27
位置づけ: 変換品質トラックの最終段（M56 完了後、v1.0 後の品質向上 phase）

## 1. 目的

同音異義語など難しい候補のみ ModernBERT-Ja で**文脈自然度**を評価し、
品質を底上げする。**生成には使わない** — Pseudo Log Likelihood（PLL）
近似で候補文の自然さを scorer 的に計算する。

M52 ベンチで homophone top1 を baseline 比 +5% 以上 / 通常入力 p95 悪化
+20ms 以内を目標。

## 2. 非目標

- 文生成 / Magic Conversion（M16 / 外部 AI の責務）
- 一般的な変換候補のスコアリング（M56 Tiny Reranker の責務）
- リアルタイム逐次推論（IME の入力 latency に乗らない頻度）

## 3. 設計原則

- **常時 ON ではない**: `ambiguity_score >= threshold` のときだけ起動
- **top-K のみ評価**: 上位 5 候補に絞る
- **PLL 近似**: 候補位置のみ mask、文全体 mask しない
- **キャッシュ**: 同一 context を再利用
- **timeout 必須**: 30〜50ms で打ち切り、failure 時は scoreなしで続行
- **GPU/NPU 優先**: M24 の DirectML / NPU backend で実行（CPU は実用上
  時間に合わない）

## 4. 起動条件

```
if candidate_count >= 2
and ambiguity_score >= threshold
and timeout_budget_remaining >= 30ms
and backend in {cuda, directml, npu}    # CPU は除外
then ModernBERTScorer ON
else skip
```

`threshold` 初期値: 0.5（M52 ベンチで校正）。

### 4.1 ambiguity_score

候補がどれだけ「決め手に欠ける」かを 0.0〜1.0 で出す:

```
ambiguity_score =
  0.30 * small_gap_between_top1_and_top2     # gap が小さいほど 1 に近づく
+ 0.20 * homophone_candidate_count_norm      # 同音異義候補が多いほど大
+ 0.20 * context_dependency_score             # 文脈依存度
+ 0.15 * named_entity_mix_score               # NE 混在時
+ 0.15 * typo_correction_uncertainty          # M55 confidence 低い時
```

`small_gap` の例: `1 - sigmoid(top1_score - top2_score)`。

## 5. スコアリング方式

候補を文脈へ挿入し、候補部分の **Pseudo Log Likelihood（PLL）を近似計算**
する。

```
left_context: 明日の会議では価格について
candidate: 交渉する
scoring_text: 明日の会議では価格について交渉する
```

```
modernbert_score = (1/N) Σ log P(token_i | sentence with token_i masked)
                   for i in candidate_token_indices
```

候補部分のトークンだけ順次 mask して forward。文全体を mask する MLM の
完全 PLL は計算量が膨大なので回避する。

### 5.1 軽量化

| 軽量化 | 内容 |
|---|---|
| top K のみ評価 | 上位 5 候補のみ（K=5） |
| 候補部分のみ mask | 文全体ではなく候補トークンのみ |
| キャッシュ | 同一 `(left_context, candidate)` ペアの結果を 5 分保持 |
| timeout | 30〜50ms で打ち切り |
| backend GPU/NPU | M24 の DirectML / NPU で実行 |
| batch 5 候補一括 | 1 forward で 5 候補分の logits を取得 |
| FP16 | ONNX Runtime FP16 推論 |
| 量子化 | INT8 量子化を検討（精度劣化を M52 で確認） |

## 6. モデル

| 項目 | 値 |
|---|---|
| モデル | `modernbert-ja-70m`（70M params） |
| 形式 | ONNX FP16 / INT8 量子化 |
| サイズ | 140MB（FP16） / 80MB（INT8） |
| 配置 | `%LOCALAPPDATA%\azooKey\models\modernbert-ja-70m.onnx` |
| backend | DirectML / NPU / CUDA（CPU は実用不可） |

bundle するかユーザーダウンロードかは Phase 7 で決定する。M57 v1 では
ロードマップ前提（M56 / M24）のみで成立させる必要があるため、初期は
**bundled（MSIX 同梱 or 既定パスへの後段配置）+ ベンチ用 manual download
スクリプト** で配布する。M45（モデル管理 UI）は前提ではなく、完了後の
follow-up として GUI 経由のインストール / 切替を統合する位置づけ。

### 6.1 RSS 許容上限

ModernBERT-Ja 70M を常時ロードした場合の IME プロセス RSS への影響:

| backend | model RSS | activations | total 増分 |
|---|---:|---:|---:|
| CPU FP32 | 280 MB | 50 MB | 330 MB |
| CPU FP16 | 140 MB | 30 MB | 170 MB |
| CUDA FP16 | 140 MB (VRAM) + 50 MB (RSS) | 100 MB VRAM | 50 MB RSS + 240 MB VRAM |
| INT8 量子化 | 80 MB | 20 MB | 100 MB |

**IME プロセスの許容上限**: Host RSS +200 MB を上限とする（既存
SimpleConverter + Zenzai 込みで合計 2GB 以下を維持）。これを超える
場合は `enabled = false` 既定 + ユーザー明示で有効化のみ許可する。

## 7. 設定スキーマ

M56 で予約した `reranker.modernbert*` を本 M57 で使う:

```json
{
  "reranker": {
    "modernbertEnabled": "auto",
    "modernbertModelPath": "",
    "modernbertModel": "modernbert-ja-70m",
    "modernbertTimeoutMs": 40,
    "modernbertCacheTtlSec": 300,
    "modernbertAmbiguityThreshold": 0.5,
    "modernbertTopK": 5,
    "modernbertQuantization": "fp16"
  }
}
```

`modernbertEnabled`:
- `auto`（既定）: ambiguity_score とリソース可用性に応じて自動 ON/OFF
- `always`: 常時 ON
- `off`: 無効

## 8. スコア統合

M56 の `final_score` 式の `modernbert_score` weight = 0.05（初期）。
M52 ベンチで校正する。

```
final_score = ... + normalize(modernbert_score) * W_BERT + ...
```

`W_BERT` を高くすると homophone 精度が上がるが latency 悪化が大きい。
balanced mode（既定）では W_BERT = 0.05、quality mode では 0.10 など
で調整する（`settings.conversionQuality` キー）。

## 9. PrivacyGate 連携

M46 secure 中は ModernBERT を **完全 OFF** にする:
- ロードしない / 既ロード済みなら使用しない
- 候補文（reading + surface）を内部 buffer にも残さない
- ログに `modernbert_used = false, reason = secure_mode` を記録

## 10. テスト

- unit: PLL 近似の計算（候補トークンの logit から）
- unit: ambiguity_score 計算（5 因子）
- unit: cache hit / miss
- unit: timeout fallback
- integration: M52 ベンチで homophone top1 +5% 以上、p95 latency +20ms
  以内
- integration: M46 secure 中の完全 OFF
- snapshot: ONNX schema / FP16 / INT8 量子化版の結果同等性

## 11. M57 受け入れ条件

- M52 ベンチで `homophone.top1_accuracy` が baseline 比 +5% 以上
- 通常入力 p95 latency 悪化が +20ms 以内
- ModernBERT がロードできない環境（CPU only / モデル未配置）でも
  fallback で動作する（IME が止まらない）
- M46 secure 中は完全 OFF
- ambiguity_score < threshold のリクエストでは ModernBERT が呼ばれない
- RSS 増分が §6.1 の許容上限内（200 MB 以下）

## 12. 将来拡張（M57 範囲外）

- 個人 fine-tune（プライバシー保持下）
- ModernBERT-Ja の自前蒸留版（10M 程度の small モデル）
- Mini Transformer + ModernBERT の 2 段 rerank（M56 で実験）
