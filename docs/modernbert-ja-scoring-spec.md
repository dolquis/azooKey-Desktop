# ModernBERT-Ja 候補スコアリング 仕様

対象リポジトリ: dolquis/azooKey-Desktop
対応マイルストーン: M57（変換品質トラック）
関連: `plans/windows-port-roadmap.md` M24 / M52 / M56、
      `docs/conversion-quality-benchmark-spec.md`（M52）、
      `docs/neural-reranker-spec.md`（M56）、
      `docs/copilot-pc-backend-spec.md`（M24 backend）
作成日: 2026-05-27
更新日: 2026-06-18（DEV-113: RSS 上限ロードゲート・ambiguity 重み正規化・
        context window・cache 整合・timeout 予算を確定）
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
- **アクセラレータ優先**: M24 の R2（Windows ML EP: NPU/GPU）または
  R1 GPU（CUDA / ggml-vulkan）で実行（CPU は実用上時間に合わない）。
  旧 `directml` / `npu` 値は deprecated で `winml`（EP 自動選択）に集約される

## 4. 起動条件

```
if candidate_count >= 2
and ambiguity_score >= threshold
and timeout_budget_remaining >= 30ms
and backend in {winml, cuda, vulkan}    # R2(WinML EP: NPU/GPU) / R1 GPU。CPU は除外
                                        # 旧 directml/npu は deprecated → winml に集約
then ModernBERTScorer ON
else skip
```

`threshold` 初期値: 0.5（M52 ベンチで校正）。

### 4.0 起動ゲート（順序付き）

各リクエストで以下を**この順**に評価し、いずれか 1 つでも不成立なら
ModernBERT を**呼ばず**に M56 の `final_score`（`modernbert_score` 抜き）で
確定する。スコアなし続行は失敗ではなく既定パスである（§5.4）。

| 順 | ゲート | 不成立時の `reason` | 補足 |
|---:|---|---|---|
| 1 | `modernbertEnabled != off` | `disabled` | `off` は明示無効。`auto` / `always` は通過 |
| 2 | secure モードでない（M46） | `secure_mode` | §9。最優先で OFF（順序上は 1 の直後） |
| 3 | model がロード済み（§6.1 のロードゲート通過） | `not_loaded` | RSS 上限超過時はそもそもロードしない |
| 4 | `backend in {winml, cuda, vulkan}` | `cpu_backend` | CPU は実用不可。R2 EP / R1 GPU のみ |
| 5 | `candidate_count >= 2` | `single_candidate` | 候補 1 件は曖昧性なし |
| 6 | `ambiguity_score >= threshold`（`always` 時は無条件通過） | `low_ambiguity` | §4.1。`always` は 6 をスキップ |
| 7 | `timeout_budget_remaining >= 30ms` | `no_time_budget` | §5.4 の予算管理 |

`reason` は構造化ログ（`modernbert_used=false, reason=<上記>`）と M52 ベンチ
集計に用いる。`always` は曖昧性ゲート（順 6）のみスキップし、その他の
安全ゲート（secure / RSS / backend / timeout）は必ず適用する。

### 4.1 ambiguity_score

候補がどれだけ「決め手に欠ける」かを 0.0〜1.0 で出す。各因子は
**0.0〜1.0 に正規化**してから重み付き加算する（重みの和 = 1.0）:

```
ambiguity_score =
  0.30 * small_gap_between_top1_and_top2     # gap が小さいほど 1 に近づく
+ 0.20 * homophone_candidate_count_norm      # 同音異義候補が多いほど大
+ 0.20 * context_dependency_score             # 文脈依存度
+ 0.15 * named_entity_mix_score               # NE 混在時
+ 0.15 * typo_correction_uncertainty          # M55 confidence 低い時
```

各因子の算出と正規化:

| 因子 | 重み | 定義 / 正規化 | 入力源 |
|---|---:|---|---|
| `small_gap` | 0.30 | `1 - sigmoid(α·(s1 - s2))`。`s1,s2` は top1/top2 の `final_score`（`modernbert_score` 抜き）。`α` は M52 で校正（初期 8.0、score を 0〜1 正規化済み前提） | M56 rerank 出力 |
| `homophone_candidate_count_norm` | 0.20 | `min(homophone_count, C_max) / C_max`。`homophone_count` = 同一 reading を共有する上位候補数、`C_max=5`（初期） | converter-core 候補列 |
| `context_dependency_score` | 0.20 | `left_context` トークン数に対する飽和関数 `min(left_tokens, L_sat)/L_sat`（`L_sat=16`）。文脈が無い変換（先頭・記号直後）では 0 に近づき起動を抑制 | §5.0 context |
| `named_entity_mix_score` | 0.15 | 上位候補に固有名詞タグ（`CandidateTag::NamedEntity` 等）と一般語が**混在**する場合 1.0、単一種なら 0.0。タグ未整備環境では 0.0（安全側） | 候補タグ |
| `typo_correction_uncertainty` | 0.15 | `1 - typo_confidence`（M55 が補正を適用した候補のみ。非補正候補は 0.0） | M55 confidence |

重み・`α` / `C_max` / `L_sat` の初期値は本 spec の既定であり、**M52 ベンチで
校正**する（`homophone.top1_accuracy` +5% と p95 +20ms 以内を同時に満たす点へ）。
タグや M55 confidence が未提供の環境では当該因子を 0.0 とし、欠損で
`ambiguity_score` が過大評価されない（=不要起動しない）安全側に倒す。

## 5. スコアリング方式

### 5.0 context window 定義

scoring_text に使う文脈は以下で固定する（PLL の forward 量と精度の両立）:

| 項目 | 値 | 根拠 |
|---|---|---|
| `left_context` | 確定済みテキスト末尾から最大 **64 token**（不足時はある分だけ） | 同音異義の弁別に十分かつ forward 量を抑制 |
| `right_context` | 原則 0 token（IME 入力中は未確定の右文脈が存在しないため） | 逐次入力では右が無い |
| 文頭処理 | `left_tokens == 0`（先頭・改行直後）では `context_dependency_score=0` で起動抑制（§4.1）。それでも `always` 時は候補単独を scoring_text とする | 文脈ゼロでの誤起動防止 |
| トークナイザ | ModernBERT-Ja 同梱の WordPiece/BPE。token 数は同トークナイザ基準 | モデルと一致 |

`left_context` は**確定（commit 済み）テキスト**のみを使い、未確定の編集中
composition は含めない。これにより打鍵ごとに context が揺れず、cache（§5.2）が
安定してヒットする。64 token を超える左文脈は**末尾優先**で切り詰める。

### 5.1 PLL 近似

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

PLL は **token 単位で別々の masked 文を作って forward する必要がある**
ため、1 候補が N トークンなら N 回の forward を要する。K 候補・各 N
トークンの場合、総 forward 回数は Σ N_k となる。

### 5.2 軽量化

| 軽量化 | 内容 |
|---|---|
| top K のみ評価 | 上位 5 候補のみ（K=5、選択基準は §5.3） |
| 候補部分のみ mask | 文全体ではなく候補トークンのみ |
| キャッシュ | 同一 `(left_context, candidate)` ペアの結果を 5 分保持（§5.2.1） |
| timeout | 30〜50ms で打ち切り（予算管理は §5.4） |
| backend アクセラレータ | M24 の R2（Windows ML EP: NPU/GPU）/ R1（CUDA・Vulkan）で実行 |
| masked 文を batch | 各候補の各トークン位置で生成した masked 文を 1 つの batch（最大 K×max_token_len）にまとめて 1 回の forward で評価する。「1 forward で K 候補」ではなく「**1 forward で K×N 個の masked 文**」 |
| FP16 | ONNX Runtime FP16 推論 |
| 量子化 | INT8 量子化を検討（精度劣化を M52 で確認）。低メモリ時の降格段でも使用（§6.1） |

#### 5.2.1 キャッシュ整合

逐次入力中も安定してヒットさせるためキャッシュ仕様を以下に固定する:

| 項目 | 値 |
|---|---|
| キー | `(model_id, quantization, normalize(left_context), candidate_surface)` |
| `normalize(left_context)` | §5.0 の確定テキスト末尾 64 token を tokenizer 正規化した列のハッシュ。**未確定 composition は含めない** |
| 値 | `modernbert_score`（正規化前の生 PLL 近似値） |
| TTL | 300 秒（`modernbertCacheTtlSec`） |
| 最大エントリ数 | 2,048（LRU evict）。超過時は最古アクセスから破棄 |
| 無効化 | TTL 失効 / LRU 押し出し / `model_id`・`quantization` 変更 / モデル再ロード時に全 flush |
| メトリクス | `cache_hit` / `cache_miss` を構造化ログ・M52 ベンチに記録 |

キーが確定テキストのみに依存するため、同一文を再変換しても打鍵途中の
未確定文字列ではキーが変動せず、再変換・候補再表示で再利用できる。
secure モード中（§9）は cache に**書き込まない / 読まない**（buffer 残存禁止）。

### 5.3 top-K 候補の選択基準

評価対象 K=5 は、**M56 の `final_score`（`modernbert_score` 抜き）降順の上位 5
候補**を取る。M56 rerank を通っていない経路（reranker 無効時）では
converter-core の確定前スコア降順を用いる。同点は reading 昇順 →
surface 昇順で決定的に tie-break し、baseline 再現性（benchmark §14）を保つ。
K は `modernbertTopK` で可変だが、増やすと forward 量（Σ N_k）が線形に増え
timeout に当たりやすくなるため既定 5 を上限の目安とする。

### 5.4 timeout 予算と部分結果

- 1 リクエストの ModernBERT 予算は `modernbertTimeoutMs`（既定 40ms、範囲
  30〜50ms）。起動ゲート（§4.0 順 7）で残予算 `>= 30ms` を要求する。
- 予算は **top1 候補から順に**消費し、各候補の全 mask forward 完了ごとに
  残時間を確認する。予算切れ時点で**評価済み候補のみ** `modernbert_score` を
  適用し、未評価候補は `modernbert_score` 抜きの `final_score` のまま順位付けする
  （部分適用は許容、IME を止めない）。
- forward 自体が失敗（EP 例外・モデル異常）した場合は当該リクエストを
  **スコアなしで続行**し、`modernbert_used=false, reason=infer_error` を記録する。
- 連続失敗が閾値（既定 3 回）に達したら当該セッションで一時的に
  `disabled`（circuit breaker）にし、次回モデル再ロードまで起動しない。

## 6. モデル

| 項目 | 値 |
|---|---|
| モデル | `modernbert-ja-70m`（70M params） |
| 形式 | ONNX FP16 / INT8 量子化 |
| サイズ | 140MB（FP16） / 80MB（INT8） |
| 配置 | `%LOCALAPPDATA%\azooKey\models\modernbert-ja-70m.onnx` |
| backend | R2 `winml`（Windows ML EP: NPU/GPU）/ R1 `cuda` / `vulkan`（CPU は実用不可。旧 `directml` / `npu` は deprecated → `winml`） |

bundle するかユーザーダウンロードかは Phase 7 で決定する。M57 v1 では
ロードマップ前提（M56 / M24）のみで成立させる必要があるため、初期は
**bundled（MSIX 同梱 or 既定パスへの後段配置）+ ベンチ用 manual download
スクリプト** で配布する。M45（モデル管理 UI）は前提ではなく、完了後の
follow-up として GUI 経由のインストール / 切替を統合する位置づけ。

### 6.1 RSS 許容上限（ロードゲート）

ModernBERT-Ja 70M をロードした場合の Host プロセス RSS 増分（**推定値**。
確定実測は §6.2 の検証で固定する）:

| backend / 量子化 | model RSS | activations | RSS 増分 | VRAM 増分 | 本 scorer での扱い |
|---|---:|---:|---:|---:|---|
| CPU FP32 | 280 MB | 50 MB | 330 MB | — | 参照のみ（§4.0 順 4 で CPU 除外） |
| CPU FP16 | 140 MB | 30 MB | 170 MB | — | 参照のみ（CPU 除外） |
| winml/cuda FP16 | 50 MB (RSS) | host 30 MB | **80 MB** | 240 MB | 既定。`modernbertQuantization=fp16` |
| winml/cuda INT8 | 30 MB (RSS) | host 20 MB | **50 MB** | 130 MB | 低メモリ降格段 / 明示選択 |

> CPU 行は参照情報。§4.0 順 4 で backend は `{winml, cuda, vulkan}` に限定され、
> CPU FP32/FP16 はこの scorer ではロード対象にならない。GPU/NPU EP では
> モデル重みは VRAM 側に載るため、プロセス RSS 増分は host 側 activation +
> ランタイム分が支配的になる。

**ハード上限（enforce する）**

| 指標 | 上限 |
|---|---|
| ModernBERT による Host **RSS 増分** | **+200 MB** |
| Host プロセス合計 RSS（SimpleConverter + Zenzai + ModernBERT 込み） | **2 GB 以下** |

上限は「既定で無効」という弱い扱いではなく、**ロードゲートで強制**する。
`modernbertEnabled` が `auto` / `always` であっても、下記ロードゲートを
満たさない限りロードしない（`always` でも上限を超えてロードはしない）。

**ロードゲート（モデルロード時に 1 回評価。降格ラダー付き）**

```
load_target = modernbertQuantization        # 既定 fp16
loop:
  est_rss   = RSS_INCREMENT[backend][load_target]      # 上表
  proj_total = current_host_rss + est_rss
  if est_rss <= 200MB and proj_total <= 2GB:
      load(load_target); break               # ロード成功
  elif load_target == fp16:
      load_target = int8                      # 降格して再評価（fp16→int8）
      continue
  else:
      do_not_load(reason = rss_cap)           # int8 でも不可 → ロードせず fallback
      break
```

- 降格ラダーは **fp16 → int8 → ロードせず**。INT8 でも上限を超える低メモリ
  環境ではロードせず、全リクエストを `modernbert_used=false, reason=rss_cap`
  で M56 `final_score`（ModernBERT 抜き）に fallback する（IME は通常動作）。
- ユーザーが `always` を選んでも上限は不変（上限超過時はロードしない）。
  これは「IME ごと不安定化させない」ための安全契約であり、設定で緩められない。
- ロード後に Host 合計 RSS が 2 GB を継続的に超過（既定: 5 秒移動平均で超過）
  した場合は ModernBERT を**アンロード**し、`reason=rss_runtime` で以降 fallback。
- 採用した `load_target`（fp16/int8）と推定/実測 RSS は load イベントログと
  M52 ベンチ（`rss_mb` / `vram_mb` / `load_ms`、benchmark spec §8）に記録する。

### 6.2 RSS 実測による上限の確定（検証ゲート）

§6.1 の増分値は設計上の**推定**であり、配布判断前に量子化別 RSS を**実測**して
固定する。実測は実機（Windows / 対象 EP）とモデル配置が必要なため、本 spec の
設計確定とは別タスク（実機検証）で行う。

実測手順（M52 ベンチ基盤を流用）:

1. backend = `winml`（R2 EP）/ `cuda`（R1）それぞれで、量子化 = `fp16` / `int8`
   をロードし、idle RSS と homophone ベンチ実行中ピーク RSS / VRAM を採取する。
2. ベースライン（ModernBERT 未ロード）との差分を「RSS 増分」「VRAM 増分」とする。
3. §6.1 の表と乖離がある場合は表値を実測へ更新し、ハード上限（+200 MB /
   合計 2 GB）に対する降格ラダーの分岐点が妥当か確認する。
4. 低メモリ機（合計 RSS が 2 GB に近い構成）で `reason=rss_cap` fallback が
   実際に発火し IME が継続動作することを確認する。

実測タスクは Linear に `gate:human-required`（実機検証）として別 issue 化し、
本設計 issue（DEV-113）からリンクする。本 spec の数値は実測確定までは推定として扱う。

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
    "modernbertCacheMaxEntries": 2048,
    "modernbertAmbiguityThreshold": 0.5,
    "modernbertTopK": 5,
    "modernbertQuantization": "fp16",
    "modernbertRssCapMb": 200
  }
}
```

> 本ブロックは M57 spec 上の予約定義であり、v1.0 の
> `settings/mvp-settings.schema.json` には未登録（M56/M57 は v1.0 後トラック）。
> schema への正式登録は M56/M57 実装着手時に行う。

- `modernbertEnabled`:
  - `auto`（既定）: ambiguity_score とリソース可用性に応じて自動 ON/OFF
  - `always`: 曖昧性ゲート（§4.0 順 6）のみスキップ。secure / RSS / backend /
    timeout の安全ゲートは適用され、上限超過時はロードしない
  - `off`: 無効
- `modernbertQuantization`: **初期ロード対象**（`fp16` 既定）。RSS 上限超過時は
  §6.1 の降格ラダー（fp16→int8→ロードせず）で実効値が変わりうる。
- `modernbertRssCapMb`: §6.1 のハード上限（既定 200）。設計上は `always` でも
  この上限を緩めない。`0` 指定は無効化ではなく**上限緩和禁止**のため最小値で
  clamp する（下限は実機検証で決定。暫定 100）。
- `modernbertTimeoutMs`: 範囲 30〜50（§5.4）。範囲外は clamp。

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
- M46 secure 中は完全 OFF（cache 読み書きも行わない）
- ambiguity_score < threshold のリクエストでは ModernBERT が呼ばれない
  （`reason=low_ambiguity` がログに残る）
- RSS 増分が §6.1 の許容上限内（200 MB 以下）。上限超過環境では
  `reason=rss_cap` で**ロードせず** fallback し、IME が継続動作する
- `modernbertEnabled=always` でも RSS / backend / secure の安全ゲートを
  逸脱しない（上限超過時はロードしない）
- timeout 予算切れ時は評価済み候補のみ適用し、未評価候補は
  ModernBERT 抜きの順位で確定する（§5.4）
- 同一確定文脈の再変換で cache がヒットする（§5.2.1）

## 12. 将来拡張（M57 範囲外）

- 個人 fine-tune（プライバシー保持下）
- ModernBERT-Ja の自前蒸留版（10M 程度の small モデル）
- Mini Transformer + ModernBERT の 2 段 rerank（M56 で実験）
