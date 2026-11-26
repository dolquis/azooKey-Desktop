---
kind: spec
milestones: [M57]
canonical_for: ["ModernBERT-Ja 候補スコアリング"]
---

# ModernBERT-Ja 候補スコアリング 仕様

対象リポジトリ: dolquis/azooKey-Desktop
対応マイルストーン: M57（変換品質トラック）
関連: `plans/windows-port-roadmap.md` M24 / M52 / M56、
      `docs/conversion-quality-benchmark-spec.md`（M52）、
      `docs/neural-reranker-spec.md`（M56）、
      `docs/copilot-pc-backend-spec.md`（M24 backend）

## 0. 節とマイルストーンの対応

本書の節と、その範囲を定める `plans/windows-port-roadmap.md` のマイルストーンの対応を次に示す。

| 節 | マイルストーン |
|---|---|
| §1〜§3 | M57 |
| §4 | M57（起動条件。M46・M55・M56 に依存する） |
| §5 | M57（§6.3 の実行 EP 上で実行する） |
| §6 | M57（ロードゲートと RSS 上限、推論ランタイムと実行 EP、モデルの取得と変換。R2 の EP は M24 が前提） |
| §7 | M57（M56 で予約した `reranker.modernbert*` キーの正典） |
| §8 | M57（M56 の `final_score` との統合） |
| §9 | M57（M46 secure 中の完全 OFF） |
| §10〜§11 | M57 |
| §12 | M57 範囲外（将来拡張） |

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
- **許可した実行 EP だけで動かす**: ONNX Runtime（`docs/neural-reranker-spec.md` §7.3）の EP のうち、
  §6.3 で許可したものだけでロードする。CPU EP は §6.3 の計測ゲートに合格するまで許さず、
  M24 の R2（Windows ML EP: NPU/GPU）は R2 の再開後に許す

## 4. 起動条件

```
if candidate_count >= 2
and ambiguity_score >= threshold
and timeout_budget_remaining >= 30ms
and execution_ep in allowed_eps         # §6.3。CPU EP は計測ゲートの合格後だけ入る
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
| 3 | §6.3 の許可集合が空でなく、かつモデルのロード時の §6.3 の選択で実行 EP を選べた | `cpu_backend` | 許可集合とロード時の選択は、リクエストより前に決まる静的な条件である。どの EP も選べないときはモデルをロードしない（§6.1）ため、loaded ゲート（順 4）より**前**に判定して `cpu_backend` を握り潰さない |
| 4 | model がロード済み（§6.1 のロードゲート通過） | `rss_cap`（§6.1 ロード時に上限超過で抑止）/ `rss_runtime`（§6.1 runtime ガードでアンロード済み）/ `not_loaded`（モデル未配置・ロード失敗） | §6.1 のロード抑止 / runtime アンロード理由を保持して引き継ぐ。ロード時上限超過は `rss_cap`、ロード後 runtime 超過は `rss_runtime` を立て、roadmap M57 の RSS fallback 検証に使う（`not_loaded` に潰さない） |
| 5 | circuit breaker が開いていない | `circuit_open` | §5.4 の連続失敗で開く **runtime 状態**（ユーザー設定 `modernbertEnabled` とは独立）。次回モデル再ロードまで閉じない。`off` のようなユーザー設定値ではない |
| 6 | `candidate_count >= 2` | `single_candidate` | 候補 1 件は曖昧性なし |
| 7 | `ambiguity_score >= threshold`（`always` 時は無条件通過） | `low_ambiguity` | §4.1。`always` は 7 をスキップ |
| 8 | `timeout_budget_remaining >= 30ms` | `no_time_budget` | §5.4 の予算管理 |

`reason` は構造化ログ（`modernbert_used=false, reason=<上記>`）と M52 ベンチ
集計に用いる。`always` は曖昧性ゲート（順 7）のみスキップし、その他の
安全ゲート（secure / circuit breaker / RSS / 実行 EP / timeout）は必ず適用する。
circuit breaker（順 5）はユーザー設定ではなく runtime 状態であり、`always`
でもスキップしない。

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
| `small_gap` | 0.30 | `2·(1 - sigmoid(α·(s1 - s2)))` を `[0,1]` にクランプ。§5.3 で候補は `final_score` 降順に並ぶため `s1 >= s2`（gap >= 0）であり、tie（gap=0）で 1.0・gap 大で 0 に漸近する。`s1,s2` は top1/top2 の `final_score`（`modernbert_score` 抜き）。`α` は M52 で校正（初期 8.0、score を 0〜1 正規化済み前提） | M56 rerank 出力 |
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
| トークナイザ | モデルと同じ revision の SentencePiece（`tokenizer.model`、§6.4）。token 数は同トークナイザ基準 | モデルと一致 |

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

PLL は **token 単位で別々の masked 文（サンプル）を作る必要がある**ため、
1 候補が N トークンなら **N 個の masked サンプル**を生む。K 候補・各 N
トークンの場合、masked サンプル総数は **Σ N_k**。

ここで「masked サンプル数（Σ N_k）」と「backend forward **呼び出し**回数」は
別単位である。§5.2 の候補単位 batch により、1 候補の N_k サンプルを 1 回の
batched forward にまとめるため、**実 forward 呼び出し回数は K 回**（候補数）に
なる。§5.4 の timeout 予算と部分適用は「1 候補 = 1 forward 呼び出し」を単位と
し、latency 見積り・キャンセルもこの forward 呼び出し単位で行う。

### 5.2 軽量化

| 軽量化 | 内容 |
|---|---|
| top K のみ評価 | 上位 5 候補のみ（K=5、選択基準は §5.3） |
| 候補部分のみ mask | 文全体ではなく候補トークンのみ |
| キャッシュ | 同一 `(left_context, candidate)` ペアの結果を 5 分保持（§5.2.1） |
| timeout | 30〜50ms で打ち切り（予算管理は §5.4） |
| 実行 EP | §6.3 で許可した ONNX Runtime の EP で実行 |
| masked 文を batch（**候補単位**） | **1 候補分**の N 個の masked 文を 1 batch（最大 `N_k`、上限 `max_token_len`）にまとめ、その候補を 1 forward で評価する。K 候補なら **K 回**の forward（naive な Σ N_k 回ではなく候補数 K 回）。候補をまたいで 1 batch にはせず、候補単位で区切ることで §5.4 の候補単位 timeout チェック・部分適用を可能にする |
| FP16 | GPU / NPU の EP（R2）でだけ使う（§6.3） |
| 量子化 | INT8。CPU EP では INT8 だけをロードする（§6.3）。精度劣化を M52 で確認する。GPU / NPU の EP では低メモリ時の降格段で使う（§6.1） |

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
| メトリクス | `cache_hit` / `cache_miss` を**構造化ログ**に記録し、§10 の integration テストで cache hit を検証する。M52 bench スキーマ（`docs/conversion-quality-benchmark-spec.md` §6.3/§8）には cache カウンタが無いため bench 出力には含めない。bench で可視化する場合は §6.1 の `load_ms`/`vram_mb` と同じく bench スキーマ拡張として実装時に同期する |

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
  30〜50ms）。起動ゲート（§4.0 順 8）で残予算 `>= 30ms` を要求する。
- 予算は **top1 候補から順に**消費する。§5.2 の batch は**候補単位**（1 候補 =
  1 forward）なので、各候補の forward 完了ごとに残時間を確認できる。予算切れ
  時点で**評価済み候補のみ** `modernbert_score` を適用し、未評価候補は
  `modernbert_score` 抜きの `final_score` のまま順位付けする（部分適用は許容、
  IME を止めない）。候補をまたぐ単一 batch にはしない（全候補一括だと候補単位の
  部分結果が取れず予算超過・全キャンセルに陥るため）。
- forward 自体が失敗（EP 例外・モデル異常）した場合は当該リクエストを
  **スコアなしで続行**し、`modernbert_used=false, reason=infer_error` を記録する。
- 連続失敗が閾値（既定 3 回）に達したら **circuit breaker を開く**。これは
  §4.0 順 5 で判定する **runtime 状態**であり、ユーザー設定
  `modernbertEnabled`（`auto`/`always`/`off`）とは独立した内部フラグで、設定値を
  書き換えない。開いている間は順 5 が `reason=circuit_open` で fallback し、
  次回モデル再ロード（またはセッション再初期化）で閉じる。

## 6. モデル

| 項目 | 値 |
|---|---|
| モデル | `modernbert-ja-70m`（70M params） |
| 形式 | ONNX FP16 / INT8 量子化（§6.4） |
| サイズ | 約 141 MB（FP16） / 約 134 MB（INT8） |
| 配置 | ディレクトリ `%LOCALAPPDATA%\azooKey\models\modernbert-ja-70m\` に `model-fp16.onnx`、`model-int8.onnx`、`tokenizer.model` を置く |
| 実行 EP | §6.3 で許可した ONNX Runtime の EP |

モデルは MSI に同梱せず、既定のディレクトリへダウンロードして配置する（`docs/neural-reranker-spec.md` §7.3）。
量子化（FP16 / INT8）ごとに別の artifact とする。CPU EP で INT8 の artifact が無ければロードせず、`not_loaded` で fallback する（§4.0 順 4）。
製品が変換済みのモデルを取得する経路（配布元の URL、通信先）と catalog の artifact の定義は、DEV-2208 で決める。
上流の重みを取得して手元で変換する手順は §6.4 が持ち、ベンチと実機の計測はその出力を使う。M45（モデル管理 UI）は前提ではなく、
完了後の follow-up として GUI 経由のインストール / 切替を統合する位置づけ。

### 6.1 RSS 許容上限（ロードゲート）

ModernBERT-Ja 70M をロードした場合の Host プロセス RSS 増分（**推定値**。
確定実測は §6.2 の検証で固定する）:

| 実行 EP / 量子化 | model RSS | activations | RSS 増分 | VRAM 増分 | 本 scorer での扱い |
|---|---:|---:|---:|---:|---|
| CPU FP32 | 280 MB | 50 MB | 330 MB | — | 参照のみ（ロードしない） |
| CPU FP16 | 140 MB | 30 MB | 170 MB | — | 参照のみ（CPU EP では FP16 をロードしない。§6.3） |
| CPU INT8 | — | — | §6.3 の計測で決める | — | CPU EP が計測ゲートに合格した後のロード対象 |
| GPU / NPU の EP（R2）FP16 | 50 MB (RSS) | host 30 MB | **80 MB** | 240 MB | R2 の再開後の既定。`modernbertQuantization=fp16` |
| GPU / NPU の EP（R2）INT8 | 30 MB (RSS) | host 20 MB | **50 MB** | 130 MB | 低メモリ降格段 / 明示選択 |

> CPU FP32 / FP16 の行は参照情報で、この scorer ではロード対象にならない。CPU INT8 の行は、§6.3 の
> 計測ゲートで実測した値を入れるまで定義せず、その間 CPU EP は許可集合に入らない（§4.0 順 3）。
> GPU/NPU EP ではモデル重みは VRAM 側に載るため、プロセス RSS 増分は host 側 activation +
> ランタイム分が支配的になる。

**ハード上限（enforce する）**

| 指標 | 上限 |
|---|---|
| ModernBERT による Host **RSS 増分** | **`modernbertRssCapMb`（既定 200 MB、§7）** |
| Host プロセス合計 RSS（SimpleConverter + Zenzai + ModernBERT 込み） | **2 GB 以下**（固定の絶対上限。設定では緩められない） |

RSS 増分上限は `modernbertRssCapMb` を参照する（リテラル固定ではない）。設定で
既定 200 MB より小さい値（例 100 MB）にした場合は、その値が§6.1 のロードゲート・
runtime ガード双方の判定境界になる。合計 2 GB は固定の絶対上限で、増分上限とは
独立に常に適用する。`always` でもこれらの上限は緩めない。

上限は「既定で無効」という弱い扱いではなく、**ロードゲートで強制**する。
`modernbertEnabled` が `auto` / `always` であっても、下記ロードゲートを
満たさない限りロードしない（`always` でも上限を超えてロードはしない）。

**ロードゲート（モデルロード時に 1 回評価。降格ラダー付き）**

```
ep          = execution_ep                   # §6.3 の選択順で決めた実行 EP
load_target = (ep == CPU) ? int8 : modernbertQuantization   # CPU EP は INT8 から（§6.3）
loop:
  est_rss   = RSS_INCREMENT[ep][load_target]           # 上表の行（CPU INT8 / GPU・NPU の EP）
  proj_total = current_host_rss + est_rss
  if est_rss <= modernbertRssCapMb and proj_total <= 2GB:
      load(load_target); break               # ロード成功
  elif load_target == fp16:
      load_target = int8                      # 降格して再評価（fp16→int8）
      continue
  else:
      do_not_load(reason = rss_cap)           # int8 でも不可 → ロードせず fallback
      break
```

- 降格ラダーは **fp16 → int8 → ロードせず**（CPU EP は §6.3 のとおり INT8 から始め、**int8 → ロードせず**）。INT8 でも上限を超える低メモリ
  環境ではロードせず、全リクエストを `modernbert_used=false, reason=rss_cap`
  で M56 `final_score`（ModernBERT 抜き）に fallback する（IME は通常動作）。
- ユーザーが `always` を選んでも上限は不変（上限超過時はロードしない）。
  これは「IME ごと不安定化させない」ための安全契約であり、設定で緩められない。
- ロード直前のベースライン RSS を保持し、ロード後も runtime ガードを継続する。
  以下のいずれかを継続的に超過（既定: 5 秒移動平均）した場合は ModernBERT を
  **アンロード**し、`reason=rss_runtime` で以降 fallback する:
  - (a) Host 合計 RSS が **2 GB** を超過
  - (b) **ModernBERT 増分**（現在 RSS − ロード前ベースライン RSS）が
    **`modernbertRssCapMb`（既定 200 MB）** を超過（合計が 2 GB 未満でも増分上限は
    強制。表値は実測まで推定であり、負荷時 activation 変動で増分が膨らみ得るため
    runtime でも増分を監視する）
- 採用した `load_target`（fp16/int8）と推定/実測 RSS は load イベントログと
  M52 ベンチに記録する。RSS ピークは既存フィールド `memory_peak_mb`
  （`docs/conversion-quality-benchmark-spec.md` §6.3/§8）に対応づける。
  GPU の `vram_mb` とモデルの `load_ms` は **M52 bench スキーマに未定義**であり、
  M57/M45（モデル管理 UI が同じ `load_ms` / `vram_mb` を表示、roadmap M45）実装時に
  bench 出力スキーマへ追加する拡張として扱う（本 PR では bench spec は変更せず、
  追加は当該実装 PR で benchmark spec と同期する）。

### 6.2 RSS 実測による上限の確定（検証ゲート）

§6.1 の増分値は設計上の**推定**であり、配布判断前に量子化別 RSS を**実測**して
固定する。実測は実機（Windows / 対象 EP）とモデル配置が必要なため、本 spec の
設計確定とは別タスク（実機検証）で行う。

実測手順（M52 ベンチ基盤を流用）:

1. §6.3 の許可集合に入る実行 EP それぞれで、その EP がロードする量子化
   （CPU EP は `int8`、GPU / NPU の EP は `fp16` / `int8`）をロードし、idle RSS と homophone ベンチ
   実行中ピーク RSS / VRAM を採取する。
2. ベースライン（ModernBERT 未ロード）との差分を「RSS 増分」「VRAM 増分」とする。
3. §6.1 の表と乖離がある場合は表値を実測へ更新し、ハード上限（+200 MB /
   合計 2 GB）に対する降格ラダーの分岐点が妥当か確認する。
4. 低メモリ機（合計 RSS が 2 GB に近い構成）で `reason=rss_cap` fallback が
   実際に発火し IME が継続動作することを確認する。

実測タスクは Linear に `gate:human-required`（実機検証）として別 issue 化し、
本設計 issue（DEV-113）からリンクする。本 spec の数値は実測確定までは推定として扱う。

### 6.3 推論ランタイムと実行 EP（決定）

ModernBERT は M56 と同じ ONNX Runtime で動かす（ランタイム、依存の取り込み、配布、読み込み、fallback、
ライセンス、opset の正典は `docs/neural-reranker-spec.md` §7.3）。比べた候補と根拠は
[decisions/2026-11-20-onnx-runtime-for-reranker.md](decisions/2026-11-20-onnx-runtime-for-reranker.md) にある。
本節は M57 に固有の実行 EP の条件を持つ。§4.0 順 3 の「許可集合」は次の EP からなる。

| 実行 EP | 許可集合に入る条件 | ロードする量子化 |
|---|---|---|
| CPU EP | 下記の計測ゲートに合格したとき | INT8 だけ。`modernbertQuantization` が `fp16` でも INT8 をロードし、§6.1 の降格ラダーは INT8 → ロードせず、になる |
| GPU / NPU の EP（Windows ML、R2） | M24 の R2 が再開し、その Host が Windows ML の ORT を読んでいるとき（`docs/copilot-pc-backend-spec.md` §4.3、`docs/neural-reranker-spec.md` §7.3） | `modernbertQuantization` に従う（FP16 → INT8 → ロードせず） |

- R1（llama.cpp）の CUDA / Vulkan は許可集合に入れない。ONNX Runtime に Vulkan の EP は無く、
  CUDA の実リンクは中止している（`docs/decisions/2026-08-04-cuda-real-link-cancellation.md`）。
- DirectML の EP は使わない（`docs/neural-reranker-spec.md` §7.3）。
- 許可集合に入る EP が無いあいだは、M57 はロードせず `reason=cpu_backend` で fallback する（§4.0 順 3）。
- 実行 EP の選択順: モデルのロード時に、許可集合の中から GPU / NPU の EP（R2）を先に選ぶ。その機器で GPU / NPU の
  EP を使えない（OS build が 26100 未満、Windows ML の EP の取得や登録に失敗した、など。
  `docs/copilot-pc-backend-spec.md` §4.3・§4.6）ときは、CPU EP が許可集合に入っていれば CPU EP を選ぶ。
  どちらも選べなければロードしない（`reason=cpu_backend`）。選んだ EP は load イベントログに記録する。
- GPU / NPU の EP で session を作れなかったとき: CPU EP が許可集合に入っていれば、CPU EP で 1 回だけ作り直す。
  作り直しの理由（GPU / NPU の EP での session の作成の失敗）は load イベントログに記録する。CPU EP でも作れない、
  または CPU EP が許可集合に入っていなければ、ロードせず `not_loaded` で fallback する（§4.0 順 4）。
- GPU / NPU の EP の登録: reranker は Windows ML の EP の catalog を自分で呼ばない。Zenzai の R2 が
  `docs/copilot-pc-backend-spec.md` §4.6 で取得・登録した EP device を使う。R2 がその起動で EP を登録していなければ、
  GPU / NPU の EP は選べない。バッテリ駆動時は、R2 と同じく NPU の EP だけを選ぶ（同 §4.5・§4.6）。
- CPU EP のスレッド数: ORT の intra-op / inter-op のスレッド数は明示して設定し、ORT の既定（全コア）は使わない。
  値は llama.cpp の推論スレッド数（`docs/copilot-pc-backend-spec.md` §6.2 の電源ごとの表）と合わせて論理コア数を
  超えない範囲で、下記の計測ゲートで決める。

**CPU EP の計測ゲート**

CPU EP を許すかは、推定ではなく実測で決める。本 spec は CPU の latency の推定値を持たない。

1. 計測の対象: ONNX Runtime の CPU EP、INT8 の `modernbert-ja-70m`、§5.2 の batch（K=5 の候補単位）、
   §5.0 の context window。量子化と ONNX への変換は §6.4 の手順を使う。
2. 計測の条件: R1 CPU の baseline と同じクラスの機材（`docs/decisions/2026-08-23-r1-cpu-baseline.md`）で、
   Zenzai（R1 CPU）が動いている状態で測る。ORT のスレッド数は上記の方針で決めた値にする。
3. 計測の項目: 候補単位の forward の latency（p50 / p95）、K=5 を評価し終えるまでの時間、§6.1 の RSS 増分、
   M52 の homophone top1 と通常入力の p95 の悪化。
4. 合格の条件: 次をすべて満たす。
   - K=5 を評価し終えるまでの p95 が `modernbertTimeoutMs` の既定（40ms、§5.4）に収まる。
   - 通常入力の p95 の悪化が +20ms 以内（§1、roadmap M57）。
   - homophone top1 が baseline 比 +5% 以上（§1、roadmap M57）。CPU EP の INT8 は M57 を届ける経路なので、
     M57 の受け入れ条件をその経路でも満たすことを合格に含める。
   - RSS 増分が `modernbertRssCapMb` の既定（200 MB、§6.1）以下。
5. 合格したら、CPU EP を許可集合に入れ、§6.1 の CPU INT8 の行に実測値を入れる。
6. 不合格なら、CPU EP は許可集合に入れず、M57 は R2 の再開を待つ。
7. 計測の機材・OS build・CPU・スレッド数と結果は、`docs/decisions/` に測定記録として残す。

合格の効く範囲: 合格は計測した機材のクラスでの結果であり、より遅い機材での速さは保証しない。そのため CPU EP では、
実行時の降格として、1 リクエストで候補を 1 つも評価できずに予算（§5.4）が切れたことを、§5.4 の circuit breaker の
連続失敗に数える。連続して閾値（既定 3 回）に達すると breaker が開き、以降は `circuit_open` で fallback する。

### 6.4 モデルの取得と変換（決定）

上流の重みから ONNX のモデルを作る手順は `scripts/modernbert_model.py` が持つ。固定する値（上流の repository と
revision、ファイルのサイズと SHA256、量子化の条件、toolchain、出力のサイズと SHA256）の正典は
`scripts/modernbert-ja-70m.manifest.json` で、本書は revision と SHA256 を写さない。依存の版は同スクリプトと
`scripts/modernbert_model.py.lock` が固定する。重みと変換したモデルは repo にも MSI にも置かない。
帰属は `THIRD_PARTY_LICENSES` が持つ。

**手順**（Windows x64、uv。リポジトリのルートで実行する）

```
uv run --no-config --locked --script scripts/modernbert_model.py fetch --output-dir <空のディレクトリ>
uv run --no-config --locked --script scripts/modernbert_model.py convert --input-dir <fetch の出力> --output-dir <空のディレクトリ>
uv run --no-config --locked --script scripts/modernbert_model.py verify --input-dir <fetch の出力> --model-dir <convert の出力>
```

| サブコマンド | 内容 | 終了コード |
|---|---|---|
| `fetch` | 固定した revision の上流ファイルを `huggingface.co` とその転送先から HTTPS で取得し、サイズと SHA256 が一致するファイルだけを残す | 0 は成功、1 は失敗 |
| `convert` | 入力を照合し、`model-fp32.onnx`、`model-fp16.onnx`、`model-int8.onnx`、`tokenizer.model` を書いて manifest の記録と比べる。`--record` は比べずに記録を書き換える | 0 は一致または記録、1 は不一致か失敗、2 は manifest に記録が無い |
| `verify` | 上流の PyTorch モデルと fp32 の ONNX の一致を判定し、fp16 と int8 の CPU EP での実行の可否と fp32 との差を JSON で出す | 0 は合格、1 は不合格（manifest との不一致、fp32 の不一致、int8 の実行の失敗）か、固定値と違う入力やファイルの欠落などの失敗 |

引数の誤りは、どのサブコマンドも 2 で終わる。manifest に出力の記録が無いとき、`verify` は記録との照合を
省いて残りの項目で判定する。

`fetch` のほかに通信するのは uv で、依存を `pypi.org` と `files.pythonhosted.org` から取得する。固定した版の
Python が無ければ、uv は Python も取得する（既定の取得元は `releases.astral.sh` で、失敗すると `github.com`。
uv の版と設定で変わる）。`convert` と `verify` のスクリプトは通信しない。
`verify` が確かめるのは変換の正しさで、変換品質の合否は §6.3 の計測ゲートと M52 のベンチで判定する。

**モデルの入出力**（fp32、fp16、int8 で同じ）

| 名前 | 型と形 | 内容 |
|---|---|---|
| `input_ids`（入力） | int64 `[N, L]` | 1 行が 1 つの masked 文（§5.1）。短い行は `<pad>` で埋める |
| `attention_mask`（入力） | int64 `[N, L]` | token の位置が 1、`<pad>` の位置が 0 |
| `mask_positions`（入力） | int64 `[N]` | 各行で logit を取る位置。呼び出し側は `[0, L)` の値を渡す |
| `logits`（出力） | float32 `[N, 102400]` | 各行の `mask_positions` の位置の語彙全体の logit。log softmax は Host が計算する |

opset は 18 とする。tokenizer は SentencePiece の unigram（語彙に無い文字は byte fallback）で、呼び出し側が
文頭に `<s>`（ID 1）、文末に `</s>`（ID 2）を付ける。`<pad>` は ID 3、mask に使う `<mask>` は ID 5 である。

**量子化**

- FP16: ONNX Runtime の float16 変換を使い、入出力の型は変えない。
- INT8: ONNX Runtime の dynamic quantization で MatMul と Gather の重みを 8 bit の整数にする。MatMul の
  重みは per channel の int8、Gather（embedding）の重みはテンソル全体で 1 つの scale を持つ uint8 になる。
  FFN の出力側の MatMul（重みの形が 1536×384、層ごとに 1 つ）は fp32 に残す。この層を量子化すると
  fp32 との差が大きくなるためである。ONNX Runtime 1.32.0 の CPU EP と短文 12 文（スクリプトの
  `SENTENCE_PAIRS`）の比較では、mask 位置の log 確率の平均絶対差は、すべて量子化すると 1.07、この層を
  残すと 0.17、ファイルの大きさは約 111 MB と約 134 MB だった。層は重みの形で選び、個数が manifest の値と
  違えば変換を失敗させる。
- INT8 では、embedding と decoder が共有する重みが、uint8 の重みと転置した int8 の重みの 2 本になる。
- INT8 の score は batch の組み方で変わる（dynamic quantization は activation の値の範囲を batch 全体から決める）。
  §5.2 の候補単位の batch では、同じ入力は同じ batch になる。INT8 の score を比べる計測は batch の組み方をそろえる。

**再現性**

- 同じ SHA256 を得る条件は、manifest に記録した toolchain（platform、Python、package の版、lock の SHA256）と
  同じであることとする。CPU の機種など、記録していない条件が違う機材での一致は保証しない。
- 記録する package の版は直接の依存だけである。推移的な依存の版は実行環境から照合しないので、上の手順の
  `uv run --locked` で実行する。
- `convert` が不一致で終了したときは、その出力を使わず、表示される記録と実行環境の toolchain を比べる。
- 版や量子化の条件を変えるときは、lock を更新して `--record` で記録し直し、`verify` に合格することを確かめて、
  スクリプト、lock、manifest を同じ PR で変える。上流の revision や本節と §6 に書いた値が変わるときは、
  `THIRD_PARTY_LICENSES` と本書も同じ PR で合わせる。

## 7. 設定スキーマ

M56 で予約した `reranker.modernbert*` を本 M57 で使う。本節は `reranker.modernbert*` キーの正典であり、
`docs/neural-reranker-spec.md` §8 から参照される:

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

> 本ブロックは M57 spec 上の予約定義であり、M56/M57 が
> `settings/mvp-settings.schema.json` へ追加する予定のキーである。v1.0 の schema には無い
> （M56/M57 は v1.0 後トラック）。

- `modernbertEnabled`:
  - `auto`（既定）: ambiguity_score とリソース可用性に応じて自動 ON/OFF
  - `always`: 曖昧性ゲート（§4.0 順 7）のみスキップ。secure / circuit breaker /
    RSS / 実行 EP / timeout の安全ゲートは適用され、上限超過時はロードしない
  - `off`: 無効
- `modernbertModelPath`: モデルのファイルではなく、§6 の配置と同じファイルを持つディレクトリを指す。
  読むファイルは、ロードする量子化（§6.1）の `.onnx` と `tokenizer.model` である。
- `modernbertQuantization`: **初期ロード対象**（`fp16` 既定）。RSS 上限超過時は
  §6.1 の降格ラダー（fp16→int8→ロードせず）で実効値が変わりうる。CPU EP では値によらず
  INT8 をロードする（§6.3）。
- `modernbertRssCapMb`: §6.1 のハード上限（既定 200）。設計上は `always` でも
  この上限を緩めない。`0` 指定は無効化ではなく**上限緩和禁止**のため最小値で
  clamp する（下限の既定は 100。実機検証で校正する）。
- `modernbertTimeoutMs`: 範囲 30〜50（§5.4）。範囲外は clamp。

## 8. スコア統合

`modernbert_score` は `final_score` の 1 項である。式と初期重み（`W_BERT`）は
`docs/neural-reranker-spec.md` §9 が正典。

`W_BERT` を高くすると homophone 精度が上がるが latency 悪化が大きい。
balanced mode（既定）では `neural-reranker-spec` §9 の初期値、quality mode では 0.10 など
で調整する（`settings.conversionQuality` キー。M57 が schema へ追加する予定）。

## 9. secure 判定との連携

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
- snapshot: ONNX schema / FP16 / INT8 量子化版の結果同等性（FP16 は GPU / NPU の EP がある環境だけで比べる。§6.3）

## 11. M57 受け入れ条件

受け入れ条件の定義の正典は [`plans/windows-port-roadmap.md`](../plans/windows-port-roadmap.md)
の M57 節とする。本書は起動条件・timeout 予算・cache・RSS 上限・fallback を定義し、
受け入れ条件を複製しない。

## 12. 将来拡張（M57 範囲外）

- 個人 fine-tune（プライバシー保持下）
- ModernBERT-Ja の自前蒸留版（10M 程度の small モデル）
- Mini Transformer + ModernBERT の 2 段 rerank（M56 で実験）
