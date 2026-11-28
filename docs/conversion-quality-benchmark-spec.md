---
kind: spec
milestones: [M52, M74]
canonical_for: ["評価ベンチ"]
---

# 変換品質評価ベンチ 仕様

対象リポジトリ: dolquis/azooKey-Desktop
対応マイルストーン: M52（変換品質トラック）
関連: `plans/windows-port-roadmap.md` M7 / M9 / M53 / M54 / M55 / M56 / M57、
      `bench/` の target `azookey_bench`、
      `docs/observability-spec.md` §7.7（M51 trace）、
      `docs/user-learning-enhancement-spec.md`（M54）、
      `docs/typo-correction-learning-spec.md`（M55）

## 0. 節とマイルストーンの対応

本書の節と、その範囲を定める `plans/windows-port-roadmap.md` のマイルストーンの対応を次に示す。

| 節 | マイルストーン |
|---|---|
| §1〜§3 | M52 |
| §4（§4.3〜§4.3.1・§4.4 を除く） | M52 |
| §4.3〜§4.3.1 | M52（schema）。打ち間違え補正の評価は M55 |
| §4.4 | M63 |
| §5〜§8 | M52 |
| §9 | M52（目標の達成は M53〜M57） |
| §10 | M52 |
| §11〜§12 | M52 |
| §13〜§14 | M52 |
| §7 の `--dict-dir`・`--lattice`・`--lattice-with-model`・`--lattice-band`・`--nll-breaker`・`--first-clause-probe`、§10 のモデル無効構成、§14.1 の `dictionary_bundle_sha256`・`lattice`・`lattice_with_model`・`nll_breaker`・`first_clause_probe` | M65（`docs/lattice-conversion-spec.md` §10） |
| §7 の `--personal-eval`・`--profile`・`--profile-file`、§8 と §14.1 の `eval_source`・`profile_sha256`、§15 | M74 |

## 1. 目的

変換精度・同音異義語選択・固有名詞 recall・打ち間違え補正・latency・
メモリを **数値で比較** できるベンチを整備し、以降の M53（辞書強化） /
M54（学習強化） / M55（打ち間違え統合） / M56（Tiny Reranker） / M57
（ModernBERT スコアリング）の効果を baseline 比で測定可能にする。

「何を入れても効果測定不能では改善が回らない」のため、変換品質トラックは
本 M52 から着手する。

## 2. 非目標

- 学習・推論アルゴリズム自体の改善（M54 / M56 / M57 の責務）
- レイテンシ最適化（M51 trace + 各 M で対応）
- A/B テスト基盤（本 M では offline ベンチのみ）

## 3. 既存 `bench/` との関係

既存 `bench/live_bench.cpp`（`bench/CMakeLists.txt` で `azookey_bench`
ターゲットとしてビルド）は latency smoke（p50/p95 < 50ms）を測定する
CTest として動作する（M38）。本 M52 は **同一ターゲット `azookey_bench`
の `live_bench.cpp` を拡張する** か、評価ロジックを別 TU として
`live_bench.cpp` から呼び出すことで以下を追加する（新規 `azookey_bench.cpp`
は作らず、既存 CTest との後方互換を保つ）:

- jsonl 評価データの読み込み
- top-k accuracy / MRR / カテゴリ別精度の計算
- typo 補正の評価（false positive / overcorrection）
- baseline JSON との diff 比較

既存 latency smoke 機能は維持する（後方互換）。

### 3.1 `azookey_rich_features_bench`

`bench/rich_features_bench.cpp`（ターゲット `azookey_rich_features_bench`、CTest `azookey_rich_features_bench_smoke`）は、
M14 の軽量ライブ変換について、Host 推論（`InferenceEngine`。モデルを載せない `SimpleConverter` の経路）の latency を測る。合否の指標は
`live_conversion` の p95 で、既定の上限は 30ms（`--max-p95-ms` で変える）。p50・p99・最大値は同じ出力に載せるが、
合否には使わない。`--json` の出力は `BenchmarkResult` の schema（§8）に従う。`--baseline` を渡すと p95 / p99 の変化を warning として出すが、
合否には使わず、§10 の定期採取にも含まれない。

## 4. 評価データ

### 4.1 ファイル構成

```
bench/data/
├── kana_kanji_eval.jsonl                  # 通常変換（general / homophone）
├── typo_eval.jsonl                        # 打ち間違え補正（typo / typo_clean）
├── conversion_quality_smoke.jsonl         # CTest 専用の小さな入力（§11.2 の件数に含めない）
├── model_comparison_supplement_eval.jsonl # モデル比較で kana_kanji_eval に足す入力（品質ゲート・baseline 外）
├── mixed_english_eval.jsonl               # M63 の日英混在の分割器（§4.4）
└── mixed_english_holdout.jsonl            # M63 の保留セット（ゲート外）
```

各ファイルの使い分けは [bench/data/README.md](../bench/data/README.md) を参照する。
§11.2 の v1 完全版で加わるカテゴリ（`named_entity` / `neologism` / `user_adapt` など）の
入力ファイルは本節の一覧にない。ケースを足す PR で本節にファイルを加える。

`*.jsonl` は 1 行 1 ケース。**第一の作成ルートは自前で書き起こす
オリジナルケース**（読み → 表層 → 文脈を本リポジトリ著者が新規作成）で、
公開コーパスは限定的な seed・文脈素材としてのみ二次利用する。利用可能な
出典・ライセンスはカテゴリ別に §13 で確定する。§13.1 のライセンス方針
（**データへ複製・派生する出典は CC0 / パブリックドメイン / `authored`
のみ**。notice 付き許諾物 MIT/Apache/BSD は参照のみで複製不可。コピー
レフトは不可）に従い、CC BY-SA / GFDL 系（Wikipedia 本文・JMdict/EDICT 等）
と再配布不可・来歴不透明なデータ（BCCWJ・京大コーパス・NEologd 等）は
**派生元として使用しない**（§13 の除外表を正典とする）。

### 4.2 通常変換ケース形式

`kana_kanji_eval.jsonl`:

```json
{
  "id": "homophone_000001",
  "domain": "business",
  "app_profile": "word_processor",
  "left_context": "明日の会議では価格について",
  "right_context": "",
  "input": "こうしょうする",
  "expected": "交渉する",
  "acceptable": ["交渉する", "交渉します"],
  "category": ["homophone", "contextual"],
  "difficulty": 3,
  "provenance": "authored"
}
```

| フィールド | 内容 |
|---|---|
| `id` | 一意 ID（category と連番） |
| `domain` | `general` / `business` / `coding` / `casual` / `creative` |
| `app_profile` | プロファイル名（M48 と対応） |
| `left_context` | 左文脈（最大 200 文字） |
| `right_context` | 右文脈（最大 50 文字、optional） |
| `input` | 読み（ひらがな） |
| `expected` | 期待第一候補 |
| `acceptable` | 許容候補（top-1 == acceptable で正解扱い） |
| `category` | カテゴリ tag の配列。**先頭要素を主カテゴリ**とし §11.2 の件数カウントに用いる（順序が意味を持つ） |
| `difficulty` | 1（易）〜 5（難） |
| `provenance` | 出典種別（§13）。`authored`（自前作成・既定） / `wikidata`（CC0 seed） / `aozora`（青空文庫・著作権切れ）。値はデータセット manifest（§13.3）の `id` と対応し、ライセンス追跡に用いる |

`provenance` は必須。クリーン/typo ケース（§4.3）にも同じ規約で付与する。
`authored` 以外を付けたケースは §13.3 の manifest に対応エントリを持つこと。

### 4.3 打ち間違え評価ケース形式

`typo_eval.jsonl`:

```json
{
  "id": "typo_adjacent_001",
  "domain": "business",
  "raw_keys": "koujsyou",
  "observed_reading": "こうじしょう",
  "intended_reading": "こうしょう",
  "left_context": "価格について",
  "expected_surface": "交渉",
  "typo_type": "adjacent_key",
  "category": ["typo", "homophone"],
  "difficulty": 2,
  "provenance": "authored"
}
```

`typo_type` は M55 で定義する 11 種:

`adjacent_key` / `missing_char` / `extra_char` / `transposition` /
`double_key` / `romaji_variant` / `n_handling` / `small_tsu` /
`long_vowel` / `dakuten_confusion` / `kana_shape_confusion`

### 4.3.1 クリーン（非タイポ）ケース

`typo_false_positive_rate` および `typo_overcorrection_rate` は分母に
**typo 補正を発動すべきでない正常入力**を必要とする。`typo_eval.jsonl`
には typo ケースと同数程度のクリーンケースを含めること（`typo_type`
は不使用、`observed_reading == intended_reading`、`expected_surface` は
正常変換結果）:

```json
{
  "id": "typo_clean_001",
  "domain": "business",
  "raw_keys": "koushou",
  "observed_reading": "こうしょう",
  "intended_reading": "こうしょう",
  "left_context": "価格について",
  "expected_surface": "交渉",
  "typo_type": null,
  "category": ["typo_clean"],
  "difficulty": 1,
  "provenance": "authored"
}
```

**指標の分母**:
- `typo_false_positive_rate` = クリーンケース中、補正候補が top1 に
  上がってしまった件数 / クリーンケース総数
- `typo_overcorrection_rate` = クリーンケース中、`aggressive` モードでも
  元の `expected_surface` が top5 から外れた件数 / クリーンケース総数

クリーン:typo 比率の既定は 1:1。`category = ["typo_clean"]` で
typo メトリクスの分母にのみ採用し、その他のカテゴリ集計には含めない。

### 4.4 日英混在の分割器ケース形式（M63）

`mixed_english_eval.jsonl` は `docs/mixed-english-auto-conversion-spec.md` §15.2 の品質ゲートの入力で、
`azookey_bench` ではなく `azookey_mixed_english_eval` が読む。分割器（§6）だけを評価し、モデルは
使わない。英単語辞書は同梱の `data/english-words.tsv` をパスで参照する（`bench/data/` に写さない）。

```jsonc
{
  "id": "mixed_0001",
  "category": "mixed_english",          // mixed_english / japanese_only / romaji_like_english
  "raw_romaji": "kyouhagithubwotukatta", // 打鍵どおりの生ローマ字（Shift の大文字を含む）
  "reading": "きょうは…",                // 任意。mixed_english 以外では必須
  "expected_english": [                 // 英字にすべき区間。生ローマ字のバイト範囲と表層
    {"begin": 6, "end": 12, "surface": "GitHub"}
  ],
  "provenance": "authored"
}
```

- `reading` は TIP が送る読み（内蔵のローマ字表で `raw_romaji` をかなにしたもの）で、評価器が一致を
  確かめる。日本語の文（`japanese_only`、`romaji_like_english`）では必須で、書き起こしの誤りを検出する。
- `expected_english` の表層は、Shift で打った区間は打鍵どおり、それ以外は英単語辞書の最頻の表層
  （spec §6.4）とする。`japanese_only` では空でなければならない。
- 指標: 分割器の先頭の経路の高確度の E 区間のうち `expected_english` と一致した割合（適合率）、
  `japanese_only` で高確度の E 区間を持つ文の数、`mixed_english` の期待区間に対する再現率（参考値）。
  合格は、不正なケースが 0、適合率が 95% 以上、`japanese_only` の高確度の E が 0。
- 同じ形式の `mixed_english_holdout.jsonl` は重みの調整に使わない保留セットで、ゲートには入れない。
  両方とも `scripts/generate_mixed_english_eval.py` が生成する。

## 5. カテゴリ

| category | 内容 | 例 |
|---|---|---|
| `general` | 一般変換 | きょうはいいてんき |
| `homophone` | 同音異義語 | こうしょう、たいしょう |
| `named_entity` | 固有名詞 | 豊田市、三河八橋、TensorRT |
| `neologism` | 新語 | 生成AI、推し活、ブルスカ |
| `mixed_script` | 英数字混在 | RTX4070、iPhone16 Pro |
| `business` | ビジネス文 | ご確認お願いいたします |
| `coding` | 技術・コード | bundle id、App Group |
| `casual` | 口語・SNS | それな、やばい、尊い |
| `creative` | 創作文体 | 星明かり、記憶、名前 |
| `user_adapt` | ユーザー学習評価 | 学習前後比較 |
| `typo` | 打ち間違え補正 | koujsyou → こうしょう |
| `typo_clean` | typo 補正発動すべきでない正常入力（false-positive 分母） | koushou → こうしょう |

## 6. 指標

### 6.1 通常変換指標

| 指標 | 定義 |
|---|---|
| `top1_accuracy` | 第一候補が `expected` or `acceptable[]` のいずれかと一致した率 |
| `top3_accuracy` | 上位 3 候補のいずれかが一致した率 |
| `top5_accuracy` | 上位 5 候補のいずれかが一致した率 |
| `MRR` | Mean Reciprocal Rank（正解の順位の逆数の平均） |
| `exact_match_rate` | 第一候補が `expected` と**素（raw）完全一致**した率（§6.1.1）。合格基準・回帰ゲートの正典 |
| `nfkc_exact_match_rate` | 第一候補が `expected` と **NFKC 正規化後に一致**した率（§6.1.1）。診断用（gate 非対象） |
| `acceptable_match_rate` | 第一候補が**正準形 `expected` 以外の許容変種**（正解集合 `{expected} ∪ acceptable[]` から `expected` を除いた集合）に素一致した率（§6.1.1）。`top1_accuracy − exact_match_rate` に等しく、`top1_accuracy` の重複ではない診断用 |
| `cer` | 第一候補 vs `expected` の **CER（Wagner-Fischer 編集距離／参照長、micro 平均）**（§6.1.2）。素評価 |
| `nfkc_cer` | 第一候補 vs `expected` を NFKC 正規化後に測った CER（micro 平均）（§6.1.2）。診断用 |
| `reading_fidelity_rate` | 候補の読みが入力読みと一致する率（M55 補正候補除く） |
| `named_entity_recall_at_5` | category=named_entity の top5 率 |
| `neologism_recall_at_5` | category=neologism の top5 率 |

`expected` がカタカナ（U+30A1〜U+30FA の字）を 1 字以上含むケースだけについて、同じ通常変換指標と件数 `n` を
`by_subset.katakana` に出す（§8）。主カテゴリによらずに数え、`typo_clean` は含めない。カタカナ表記の罰金が
外来語を壊していないかを見る部分集合である（`docs/lattice-conversion-spec.md` §10.2）。

> **top5 系指標の前提**: `top5_accuracy` / `named_entity_recall_at_5` /
> `neologism_recall_at_5` / `typo_correction_top5_accuracy`（§6.2）は候補が
> 5 件以上あって初めて top5 として成立する。Zenzai の N_zenzai 既定は 4
> （`docs/zenzai-inference-spec.md` §6.1）だが、**bench 採取時は `beam_width` /
> `n_best` を 5 以上に上書き**して採取する（§8 config 例は 5）。4 のまま採ると
> top5 が実質 top4 になり §9 合格基準と不整合になる。

### 6.1.1 Exact Match と NFKC 二重評価

**文字列比較の単位**: すべての一致判定・編集距離は **Unicode スカラー値
（コードポイント）** を 1 文字として行う（karukan 参考実装 `ajimee_bench.rs`
の Rust `char` イテレーションと一致）。UTF-8 バイト単位・書記素クラスタ単位では
数えない。合成済み／結合文字列（濁点の合成・分解など）の差は §6.1.1 の NFKC 評価で
吸収する（後述）。

**素評価（raw）**: 正規化を一切かけず、第一候補 `top1` と `expected` を
コードポイント列として同一かで判定する。これが `exact_match_rate`。

**正解集合（accepted set）と `acceptable_match_rate` の区別**: 正解集合を
`{expected} ∪ acceptable[]` と定める（§4.2。`acceptable[]` は慣例上 `expected`
自身を含めて書いてよく、和集合で扱うため冪等）。`top1_accuracy` は
`top1 ∈ 正解集合`（素）で判定する。これに対し `acceptable_match_rate` は
`top1` が**正準形 `expected` 以外の許容変種**に一致した率、すなわち
`top1 ∈ (正解集合 \ {expected})` とする。定義上
`acceptable_match_rate = top1_accuracy − exact_match_rate`（3 者は
`top1_accuracy ≥ exact_match_rate`、`acceptable_match_rate ≥ 0` を常に満たす）。
これにより `acceptable_match_rate` は `top1_accuracy` の重複ではなく、
「正準形の代わりに許容変種が第一候補になった割合」を測る診断値になる。

**NFKC 評価**: `top1` と `expected` に Unicode 正規化形 **NFKC** を適用してから
一致判定する。これが `nfkc_exact_match_rate`。**参照は `expected` のみ**とし、
素の `exact_match_rate` と同じ対象に対して測る（`acceptable[]` は含めない）。
これにより差分 `nfkc_exact_match_rate − exact_match_rate` が**純粋に正規化・
全半角差だけ**を表し、許容表記ゆれ（`acceptable[]` で救うべきもの）が混ざらない。
NFKC 版の acceptable 一致は v1 では定義しない（必要になれば別指標として追加）。
NFKC は次を畳み込む:

- 全角英数字 → 半角（`Ａ`→`A`、`１`→`1`）
- 半角カタカナ → 全角カタカナ（`ｱ`→`ア`、半角濁点の合成）
- 互換文字の正準化（`①`→`1`、`㍿`→`株式会社`、丸数字・組文字など）

**二重評価の意味**: 両方を必ず出力する。差分
`nfkc_exact_match_rate − exact_match_rate ≥ 0` は「変換自体は正しいが
全半角・互換差だけで素一致から外れたケース」の割合を表す診断値であり、
**変換品質の悪化と表記ゆれを切り分ける**ために用いる。

**境界の扱い（全半角差・正規化前後）**:

- **正典は素評価**。`exact_match_rate` / `top1_accuracy` / `cer`（素）を §9
  合格基準・§14 回帰判定の正典系（primary）とする（ハード fail 条件は
  `top1_accuracy`、`exact_match_rate` / `cer` は回帰監視系。§14.3）。全半角・
  互換差はユーザーに見える実挙動差であり、gate で緩めない。
- **NFKC 系（`nfkc_exact_match_rate` / `nfkc_cer`）は診断専用**（report-only）で
  gate 対象にしない。§8 `summary` に併記する。
- 全半角のどちらを正解とするかが**設計上妥当な揺れ**であるケースは、
  `expected` を IME の正準出力形に固定したうえで、許容する表記を
  `acceptable[]` に列挙して素評価で救う（NFKC で暗黙に緩めない）。これにより
  「何が正解か」がデータ側で明示され、機械判定が恣意にならない。
- NFKC 実装は固定する。正規化テーブル（Unicode データ版）を変えると
  `nfkc_*` の値が動きうるため、正規化器を差し替えた場合は §8 出力 `version`
  を +1 する（§14.1）。

### 6.1.2 CER（Wagner-Fischer）

**定義**: 第一候補 `top1` を仮説、`expected` を参照とし、両者の
**Levenshtein 編集距離**（挿入・削除・置換のコスト各 1、一致 0、
Wagner-Fischer の動的計画法で算出。転置は数えない＝Damerau ではない）を
参照長で割る。文字単位はコードポイント（§6.1.1）。

- **per-case CER** = `edit_distance(top1, expected) / len(expected)`。
  `len` はコードポイント数。挿入過多で 1.0 を超えうる（クランプしない）。
- **参照は `expected` のみ**。CER は単一参照への距離を測る指標のため
  `acceptable[]` は使わない（複数参照では距離が定義できない）。
- **空参照の扱い**: `len(expected) == 0` のとき、`top1` も空なら per-case CER = 0、
  そうでなければ 1.0 とする（0 除算を避けるための規約）。評価データ側は
  `expected` 非空を必須とし、空参照は原則発生させない。

**集計（micro 平均を正典）**:

- `cer`（summary）= `Σ edit_distance / Σ len(expected)`（全ケースの編集距離合計を
  参照長合計で割る、micro 平均）。参照長で重み付けされ、コーパス CER の標準定義。
- per-case CER の単純平均（macro 平均）は短いケースに過大な重みを与えるため
  **正典にしない**。必要なら診断として per-case 内訳（§8.1）から算出できる。
- **NFKC CER**（`nfkc_cer`）は `top1` と `expected` を NFKC 正規化してから
  同じ micro 平均で算出する。素 CER 以下になるのが通常で、差は表記ゆれ由来の
  文字誤りの割合を示す診断値。

**決定性**: CER・NFKC はいずれも決定的な文字列演算であり浮動小数非決定性を
持たない（§14.2 の精度系「再実行差 ≤ 0.2pp」は候補集合が同一なら CER も
bit 一致する）。

### 6.2 打ち間違え補正指標

| 指標 | 定義 |
|---|---|
| `typo_correction_top1_accuracy` | typo 入力に対し第一候補が正解になる率 |
| `typo_correction_top5_accuracy` | 正解が上位 5 候補に入る率 |
| `typo_false_positive_rate` | 正常入力を誤って typo 扱いした率（§4.3.1 クリーンケース分母） |
| `typo_overcorrection_rate` | 意図した入力を別候補へ過補正した率（§4.3.1 クリーンケース分母） |

`typo_suggestion_accept_rate` / `typo_suggestion_reject_rate` はユーザーの
実インタラクション（提示→採用/拒否）を必要とするため **オフラインベンチの対象外**。
M54 / M55 の訂正イベントテレメトリで収集・計測する。

### 6.3 性能指標

| 指標 | 定義 |
|---|---|
| `latency_p50_ms` | 中央値 |
| `latency_p95_ms` | 95 パーセンタイル |
| `latency_p99_ms` | 99 パーセンタイル |
| `timeout_rate` | timeout 発生率 |
| `fallback_rate` | SimpleConverter / no-reranker fallback の率 |
| `memory_peak_mb` | プロセス RSS のピーク |

## 7. CLI

```powershell
# 通常変換評価
azookey_bench.exe --eval bench/data/kana_kanji_eval.jsonl \
                  --output result.json \
                  --baseline baseline.json \
                  --backend cpu \
                  --model %LOCALAPPDATA%\azooKey\models\zenzai-small.gguf

# 打ち間違え評価（M55 で補正有効モードを指定。M52 では既定 off で schema 確認）
azookey_bench.exe --eval bench/data/typo_eval.jsonl \
                  --typo-mode rank \
                  --output typo_result.json \
                  --baseline typo_baseline.json   # rank/aggressive は M55 機能

# trace 連携（M51）
azookey_bench.exe --eval bench/data/kana_kanji_eval.jsonl \
                  --trace \
                  --output result.json
```

オプション:

| オプション | 意味 |
|---|---|
| `--eval <jsonl>` | 評価ケースファイル |
| `--output <json>` | 集計結果出力（§8） |
| `--per-case <jsonl>` | per-case 内訳を JSONL で出力（§8.1、省略時は非出力）。回帰の原因ケース特定・baseline 差分の内訳確認に使う |
| `--baseline <json>` | 比較 baseline |
| `--backend <name>` | backend 強制 |
| `--model <path>` | モデル強制 |
| `--dict-dir <dir>` | 同梱辞書（`dictbuild/build_bundled.py` が出力する `dict` ディレクトリ）を静的層として読む。同じディレクトリの `lattice_core.azdic` は層に登録せず、ラティス変換だけが読む（`docs/lattice-conversion-spec.md` §8.2）。省略時は静的層を読まない。`config.dictionary_bundle_sha256` に記録（§14.1 互換キー） |
| `--lattice on\|off` | ラティス変換の段階 1（`docs/lattice-conversion-spec.md` §3.2 の内部設定）を設定する。`--dict-dir` があるときの既定は製品と同じ `on`、無いときは `off`。`--dict-dir` 無しの `on` と、`on` で lattice_core を読めないときはエラーで止める。`config.lattice` に記録（§14.1 互換キー） |
| `--nll on\|off` | NLL rerank を設定する（既定は製品と同じ off）。`--model` が要る。`config.nll` に記録（§14.1 互換キー） |
| `--lattice-with-model <K>` | 計測用に、ラティス変換の段階 2（`docs/lattice-conversion-spec.md` §3.3）を有効にする。ラティスの上位 K 経路（1〜8）を NLL rerank の候補に加える。`--eval`、`--model`、`--nll on`、`--lattice on` が要る。`config.lattice_with_model` に記録（§14.1 互換キー） |
| `--lattice-band best\|lowest` | 段階 2 の経路を置く score 帯。`best`（既定）はモデルの最上位の候補の直下、`lowest` はモデルの最下位の候補の直下に置く。`--lattice-with-model` が要る |
| `--nll-breaker product\|per-request` | NLL の breaker（`docs/neural-reranker-nll-spec.md` §B8）の扱い。`product`（既定）は製品と同じ。`per-request` は、設定の変更と同じく変換ごとに breaker を閉じ、予算を超えた変換が後の変換の NLL を止めないようにする。`--eval` と `--nll on` が要る。`config.nll_breaker` に記録（§14.1 互換キー） |
| `--first-clause-probe` | 各ケースの変換の後に、ラティスの 1 位経路が 2 文節以上なら、その先頭文節を、先頭文節の要求（`docs/segment-editing-spec.md` §5）として続けて照会する。終了時に、読み全体・先頭文節・両者の和の所要時間と、先頭文節の要求の deadline 打ち切りの件数を標準出力に出す。ケースの所要時間は両方の要求を含み、先頭文節の要求の NLL rerank も breaker に数える。`--eval`、`--model`、`--dict-dir` が要る。`config.first_clause_probe` に記録（§14.1 互換キー） |
| `--decode-stats <jsonl>` | 1 回の変換ごとに、所要時間、`docs/zenzai-inference-spec.md` §8.2 の deadline による打ち切りの有無、候補の source と未完了の印を 1 行ずつ出す（同 §2.4.3 の評価に使う）。候補ごとに、段階 2 が足した経路か（`lattice_model`）と NLL rerank が採点したか（`nll_scored`）も出す。`--first-clause-probe` があれば、先頭文節の要求の境目・所要時間・deadline 打ち切り・1 位を `first_clause` に出す |
| `--trace` | M51 trace ログを出す |
| `--trace-output <jsonl>` | `--trace` の出力先（既定 `trace.jsonl`、`docs/observability-spec.md` §7.7.4） |
| `--category <name>` | カテゴリ絞込 |
| `--iterations <N>` | 各ケースの繰返し回数（latency 用） |
| `--typo-mode <mode>` | typo 補正モード（`off` / `suggest` / `rank` / `aggressive`、既定 `off`）。`config.typo_correction_mode` に記録（§14.1 互換キー） |
| `--personal-eval` | 利用者の学習ストアで評価する個人回帰ベンチ（M74）。使えるオプションと出力の制約は §15.5・§15.7 |
| `--profile <text>` / `--profile-file <path>` | bench 専用の Zenzai のプロファイル（M74）。§4 の評価データでも使える。詳細は §15.5。`config.profile_sha256` に記録（§14.1 互換キー） |

**typo 評価のモード指定（マイルストーン別運用）**: §6.2 の typo 補正指標は
補正が有効なモードでのみ意味を持つ。ただし**補正有効モード `suggest` /
`rank` / `aggressive` は M55（打ち間違え統合）が提供する**。
そのため:

- **M52 受け入れ**: typo 補正は M55 が提供するため `--typo-mode off`（既定。M55 の
  補正が無い構成で使える唯一のモード）で走らせ、typo 指標の**算出・schema 出力**を確認する（実測値
  の良否は問わない。off では `typo_correction_top*` は自明な値になる）。
- **M55 受け入れ**: 補正有効モードでの**実測値**を検証する。
  `typo_correction_top1/5_accuracy` は評価対象モード（M55 既定の `rank` 等）で、
  `typo_overcorrection_rate` は §4.3.1 のとおり `aggressive` モードで測る。
  採取の構成は次のとおり。
  - モデル: Zenzai の実モデル（`ggml-model-Q5_K_M.gguf`、`--model`）
  - 辞書: 同梱辞書（`--dict-dir`）
  - ラティス: `--lattice off`
  - 学習ストア: 空（`learning_state = empty`）
  - モデルなしの構成は受け入れに用いない。補正が無くても clean の正解が top5 を外れる行があり、その行が
    `typo_overcorrection_rate` の床になるため。
- 通常変換（`kana_kanji_eval`）は常に `off`（補正非対象）。baseline は
  モードごとに別管理（互換キーに `typo_correction_mode` を含む、§14）。
- `off` 以外のモードでは、空の打ち間違え学習ストア（`learning_state = empty`）を Host の
  エンジンに渡して変換する。ケースに `raw_keys`（§4.3）があれば、Host の候補照会と同じく
  補正仮説の入力として渡す。v1 の `auto_replace` は bench のモードとして受け付けない
  （`aggressive` を指定する）。

## 8. 出力 JSON 形式

```json
{
  "version": 1,
  "timestamp": "2026-05-27T10:00:00+09:00",
  "config": {
    "backend": "cpu",
    "model": "zenzai-small.gguf",
    "model_sha256": "e3b0c44298fc1c149afbf4c8996fb924...",
    "host_version": "0.1.0",
    "build_id": "abc123",
    "decode": "beam",
    "beam_width": 5,
    "n_best": 5,
    "max_new_tokens": 64,
    "prompt_template_version": 1,
    "thread_count": 1,
    "batch_size": 1,
    "typo_correction_mode": "off",
    "learning_state": "empty",
    "dictionary_bundle_sha256": "none",
    "lattice": "off",
    "nll": "off",
    "lattice_with_model": "off",
    "nll_breaker": "product",
    "first_clause_probe": "off",
    "eval_source": "bundled",
    "profile_sha256": "none",
    "category_filter": "all",
    "eval_dataset_sha256": "9f86d081884c7d659a2feaa0c55ad015..."
  },
  "summary": {
    "top1_accuracy": 0.85,
    "top5_accuracy": 0.96,
    "MRR": 0.91,
    "exact_match_rate": 0.82,
    "nfkc_exact_match_rate": 0.85,
    "acceptable_match_rate": 0.03,
    "cer": 0.041,
    "nfkc_cer": 0.032,
    "reading_fidelity_rate": 0.99,
    "latency_p95_ms": 45.2,
    "latency_p99_ms": 61.8,
    "memory_peak_mb": 1820
  },
  "by_subset": {
    "katakana": { "top1_accuracy": 0.71, "cer": 0.12, "...": "...", "n": 38 }
  },
  "by_category": {
    "homophone": {
      "top1_accuracy": 0.78,
      "top5_accuracy": 0.94,
      "exact_match_rate": 0.75,
      "nfkc_exact_match_rate": 0.77,
      "cer": 0.058,
      "nfkc_cer": 0.049,
      "n": 250
    },
    "named_entity": { "...": "..." }
  },
  "diff_vs_baseline": {
    "top1_accuracy": 0.03,
    "cer": -0.006,
    "homophone.top1_accuracy": 0.05
  }
}
```

- `summary.cer` / `summary.nfkc_cer` は §6.1.2 の **micro 平均**。`by_category.<cat>.cer`
  も当該カテゴリ内の micro 平均（`Σ edit_distance / Σ len(expected)`、主カテゴリ§11.2 で集計）。
- `latency_p95_ms` と `latency_p99_ms` は、ケースごとの latency（§8.1）を昇順に並べた n 件の
  `floor(q × (n − 1))` 番目（0 始まり）の値である。
- `by_subset.katakana`（§6.1）は `by_category` と同じ指標と件数 `n` を持つ。`diff_vs_baseline` の対象にしない。
- `diff_vs_baseline` の符号は「値の増分」。精度系（`top1_accuracy` /
  `exact_match_rate` など）は **正が改善**、誤り系（`cer` / `nfkc_cer` /
  `typo_false_positive_rate` など）は**負が改善**。回帰ゲート（§14.3）はこの向きを
  踏まえて判定する。`acceptable_match_rate`（正準形以外の許容変種率、§6.1.1）は
  正負どちらが良いとは一概に言えない診断値のため gate 対象にしない。
- 恒等式（§6.1.1）: `top1_accuracy = exact_match_rate + acceptable_match_rate`。
  上例では 0.85 = 0.82 + 0.03。出力はこの整合を常に満たす。

### 8.1 per-case 内訳 schema（`--per-case <jsonl>`）

集計値（§8）だけでは回帰の**原因ケース**を追えないため、`--per-case` 指定時に
1 行 1 ケースの JSONL を出力する。baseline の per-case を保存しておけば、
`diff_vs_baseline` で悪化した指標の内訳（どのケースが素→NFKC で救われたか、
どのケースで CER が増えたか）を機械的に突き合わせられる。

**通常変換ケース（§4.2 由来）の 1 行**:

```json
{
  "id": "homophone_000001",
  "category": "homophone",
  "input": "こうしょうする",
  "expected": "交渉する",
  "top1": "交渉する",
  "candidates": ["交渉する", "交渉します", "交章する", "高尚する", "公証する"],
  "rank": 1,
  "exact_match": true,
  "nfkc_exact_match": true,
  "acceptable_match": false,
  "cer": 0.0,
  "nfkc_cer": 0.0,
  "reading_fidelity": true,
  "latency_ms": 12.3
}
```

> 上例は `top1` が正準形 `expected` と一致した素 exact なので `exact_match: true`
> かつ `acceptable_match: false`（許容変種一致ではない）。`top1` が `expected`
> 以外の許容変種（例 `"交渉します"`）だったケースでは `exact_match: false` /
> `acceptable_match: true` / `rank: 1` になる（§6.1.1 の恒等式に対応）。

**typo / typo_clean ケース（§4.3 由来）の 1 行**（`input` を持たず観測読みを持つ）:

```json
{
  "id": "typo_adjacent_001",
  "category": "typo",
  "observed_reading": "こうじしょう",
  "intended_reading": "こうしょう",
  "expected": "交渉",
  "top1": "交渉",
  "candidates": ["交渉", "交渉する", "公証"],
  "rank": 1,
  "typo_type": "adjacent_key",
  "exact_match": true,
  "nfkc_exact_match": true,
  "cer": 0.0,
  "nfkc_cer": 0.0,
  "typo_corrected": true,
  "latency_ms": 15.1
}
```

| フィールド | 内容 |
|---|---|
| `id` / `category` | ケース ID と主カテゴリ（`category[0]`、§11.2） |
| `input` | 入力読み（通常変換ケースのみ） |
| `observed_reading` / `intended_reading` / `typo_type` | typo 系ケースのみ（§4.3） |
| `expected` | 期待第一候補（CER の参照、§6.1.2） |
| `top1` | 実際の第一候補 |
| `candidates` | 採取した top-k 候補（k = `config.n_best`）。表層のみ |
| `rank` | 正解（`expected` or `acceptable[]`）の順位（1 始まり）。**圏外は 0** |
| `exact_match` / `nfkc_exact_match` | §6.1.1 の per-case 真偽（`top1` vs `expected` の素 / NFKC 一致） |
| `acceptable_match` | `top1` が正準形 `expected` 以外の許容変種に一致（`top1 ∈ 正解集合 \ {expected}`、§6.1.1）。`exact_match` と排他で、両者が同時に true にならない |
| `cer` / `nfkc_cer` | §6.1.2 の per-case CER（1.0 超あり得る、クランプなし） |
| `reading_fidelity` | 第一候補の読みが入力読みと一致（通常変換ケース） |
| `typo_corrected` | typo 系で第一候補が補正の候補か（補正が置いた候補か、補正後の読みで変換した結果。§4.3.1 の `typo_false_positive_rate` の分子。`--typo-mode off` では常に false） |
| `latency_ms` | 当該ケースの計測 latency（`--iterations` 指定時は中央値） |

- per-case JSONL は集計を持たない（集計は §8 の `--output` 側の正典）。
  両者は同一 run で整合する（`summary.cer` は per-case `cer` の micro 平均に一致）。
- per-case ファイルは大きくなる（v1 完全版で ≥1350 行）ため既定では出力しない。
  baseline と内訳を比べる用途では、baseline と同じ固定 config（§14.1）で採取する。
  行順は入力 `*.jsonl` の出現順を保つ（diff 安定のため）。

## 9. 合格基準 v1

M53〜M57 完了時点で達成する目標:

| 指標 | 目標 |
|---|---:|
| `top1_accuracy` | baseline 比 +3% 以上 |
| `top5_accuracy` | 95% 以上 |
| `homophone.top1_accuracy` | baseline 比 +5% 以上 |
| `user_adapt.top1_accuracy` | 学習前比 +3% 以上（M54） |
| `named_entity.recall_at_5` | 90% 以上 |
| `typo_correction_top5_accuracy` | 85% 以上 |
| `typo_false_positive_rate` | 1% 未満 |
| `typo_overcorrection_rate` | 0.5% 未満 |
| `latency_p95_ms` | 50ms 以下 |
| `latency_p99_ms` | 100ms 以下 |
| `timeout_rate` | 0.1% 未満 |
| inference-host crash | 0 件 |

各 M の受け入れ条件は本表を参照する。

## 10. CI 連携

main push / 日次 schedule / 手動実行の品質採取と advisory な baseline 比較は
独立した `quality-benchmarks.yml` の `quality-benchmark` ジョブで行う。実行条件、artifact、比較スキップ、
warning と終了コードの契約は `build-ci-spec.md` §4.5 を参照する。
辞書なしの定期採取はモデルなし CPU 経路の schema / artifact 接続 smoke とする。
`--dict-dir` を付けないので、当該データセットでは top1 / top5 / exact が 0 となり、精度低下の検知を実証できない。
実品質の回帰検証には実辞書または固定モデルが必要であり、PR 向け回帰ゲートを別途構成する。
実モデルで検証する場合は `--model` でモデルを指定し、
§14.1 の config と互換な baseline を用意する必要がある。
実辞書で検証する場合は `--dict-dir` で同梱辞書を指定する。モデルを指定せず `--lattice off` と `--lattice on` を
比べる構成は M65 の測定に使う（`docs/lattice-conversion-spec.md` §10）。品質 workflow は release と同じ
`dictbuild/build_bundled.py --cache` で、SHA256 で固定した raw lexicon と `matrix.def.zip` から同梱辞書を生成する。
`scripts/run-quality-bench.py --dict-dir <同梱辞書>` は辞書なし採取に加え、`kana_kanji_eval` を
`--dict-dir` 付きのラティス off / on で採取する。結果名と `comparison.json` のキーはそれぞれ
`kana_kanji_eval_lattice_off` / `kana_kanji_eval_lattice_on` とし、結果ファイルの拡張子は `.json` とする。
同名の baseline と §14.1 の互換キーを照合し、構成間の結果を baseline として混ぜない。
同梱辞書の生成に失敗した場合は `--dict-dir` を渡さず辞書なし 2 構成を採取し、artifact 保存後に job を失敗させる。

定期採取の CLI 配線は `scripts/run-quality-bench.py` が持つ。`kana_kanji_eval` と `typo_eval` の
辞書なし構成について次の形で CLI を呼ぶ。追加構成では出力名を分け、`--dict-dir` と `--lattice` を加える。
baseline は品質 workflow の直前の `main` 成功 run の
`conversion-quality-results` artifact にある同名の結果 JSON で、§14.1 の互換キーが一致するときだけ
`--baseline` を付けて評価し直す。

```bash
azookey_bench.exe --eval bench/data/kana_kanji_eval.jsonl \
  --output quality-results/kana_kanji_eval.json \
  --backend cpu --iterations 30 --typo-mode off \
  --baseline quality-baseline/kana_kanji_eval.json   # 互換な baseline があるときだけ
```

PR 向け回帰ゲートは、PR コメントに diff_vs_baseline サマリを投稿し、PR レビューアが品質改善を
数値で確認できるようにする。判定は §14.3 に従う。

## 11. 評価データの整備方針（件数・代表性）

### 11.1 構築原則

- **第一ルートは自前作成**（`provenance: authored`）。読み → 表層 → 文脈を
  本リポジトリ著者が新規に書き起こす。これにより配布物のライセンスを
  本リポジトリ（MIT）+ データ部 CC0 専用宣言（§13.4）に一本化し、
  上流ライセンスの巻き込みを避ける。
- 公開コーパスは §13 で USABLE と確定した CC0 / パブリックドメイン由来
  （Wikidata ラベル・青空文庫の著作権切れ作品）に限り、seed・文脈素材
  としてのみ二次利用する。コピーレフト（CC BY-SA / GFDL / GPL）と
  再配布不可・来歴不透明なデータは派生元に使わない（§13.2）。
- ユーザー入力ログからの自動生成は **行わない**（プライバシー優先）。
  利用者自身の確定履歴を端末内で評価に使う §15 の個人回帰ベンチ（M74）は、評価ケースをメモリ上に
  作るだけで、`bench/data/` のデータも、そのほかの評価データのファイルも作らない。
- データ追加・修正は PR レビュー必須。`authored` 以外を追加する PR は
  §13.3 manifest の更新を必須とする。

### 11.2 カテゴリ別件数（代表性）

段階導入する。**M52 初期版**は受け入れ条件（roadmap M52）に必要な 4 カテゴリ
（general / homophone / typo / typo_clean）の最小セットで、**M52 の gating
受け入れ条件**（roadmap M52）はこの初期版のみ。**v1 完全版**列（合格基準 §9 を測る
規模）は **gating ではなく目標規模**で、変換品質トラック（M53〜M57）の効果
測定までに整える**推奨スケジュール**である（roadmap に hard 依存として
追加しない。M53 着手は roadmap の前提に従う）。

| category | M52 初期 | v1 完全版 | 代表性の要点 |
|---|---:|---:|---|
| `general` | 100 | 200 | domain 5 種をほぼ均等。difficulty 1–5 を 2:3:3:1:1 目安 |
| `homophone` | 100 | 200 | 高頻度同音異義（こうしょう/たいしょう 等）を文脈で弁別。`left_context` 必須 |
| `typo` | 100 | 150 | M52 初期は §4.3 の typo_type 11 種を各最低 1 件で網羅（計 100）。v1 完全版で各最低 10 件（計 ≥110、150 枠内）に深掘り |
| `typo_clean` | 100 | 150 | typo と 1:1（§4.3.1）。false-positive 分母 |
| `named_entity` | — | 150 | person/place/station/product/software/company を各最低 20。seed は Wikidata(CC0) |
| `neologism` | — | 100 | 直近 2–3 年の新語。`authored`（語自体は事実、来歴非依存） |
| `mixed_script` | — | 80 | 英数字混在（RTX4070 等） |
| `business` | — | 80 | 敬語・定型文 |
| `coding` | — | 60 | 技術語・識別子読み |
| `casual` | — | 60 | 口語・SNS |
| `creative` | — | 60 | 創作文体。文脈素材に青空文庫(著作権切れ)を許容 |
| `user_adapt` | — | 60 | 学習前後比較（M54）。学習注入スクリプトとペアで管理 |
| **合計** | **≥400** | **≥1350** | カテゴリ最小 60、計 1000 超を満たす |

- 1 ケースは複数 `category` tag を持ってよい（例 `["typo","homophone"]`）が、
  **`category` 配列の先頭要素を主カテゴリ**と定め、件数は主カテゴリでのみ
  数える（重複カウントしない）。主カテゴリは §5 の 12 種のいずれかであること。
  この規則により「各 100」ゲートは機械的に検証可能（重複・恣意的選択が起きない）。
- `difficulty` と `domain` の分布は各カテゴリ内で偏らせない。極端な難問
  のみ／特定ドメインのみのカテゴリは代表性不足として PR レビューで差し戻す。
- 重複ケースは禁止。`id` は全ファイルで一意。重複キーはスキーマ別に定める:
  通常変換ケース（§4.2）は (`input`, `left_context`) の完全重複不可。
  typo / typo_clean ケース（§4.3、`input` を持たない）は
  (`observed_reading`, `intended_reading`, `expected_surface`, `left_context`)
  の完全重複不可。

初期セットの `general` / `homophone` は `bench/data/kana_kanji_eval.jsonl`、
`typo` / `typo_clean` は `bench/data/typo_eval.jsonl` に収める。
入力ファイルの使い分けと再実行の手順は [bench/data/README.md](../bench/data/README.md)
を参照する。`conversion_quality_smoke.jsonl` は CTest 専用入力とし、初期セットの
件数には含めない。

## 12. M52 受け入れ条件

受け入れ条件の定義の正典は [`plans/windows-port-roadmap.md`](../plans/windows-port-roadmap.md)
の M52 節とする。本書は CLI・指標・出力 schema・評価データの件数と出典・baseline 安定性を
定義し、受け入れ条件を複製しない。

## 13. 評価データの出典・ライセンス

### 13.1 方針

配布物（公開 GitHub リポジトリ）に同梱・再配布するため、**評価データへ
複製・派生する出典は CC0 / パブリックドメイン / `authored`（自前作成）の
3 種のみ**に限る。これらは notice / attribution / NOTICE 同梱義務を持たず、
データ部の一律 CC0 宣言（§13.4）と矛盾しない。MIT / Apache-2.0 / BSD など
**notice 付き許諾物（mozc・SudachiDict 等）はアイデア参照のみ**とし、
エントリ・文字列をデータへ複製・派生しない（複製すると上流の著作権表示・
NOTICE 義務が一律 CC0 と衝突するため）。コピーレフト（CC BY-SA / GFDL /
GPL）と再配布不可・来歴不透明なデータは参照・派生いずれも使わない。
本リポジトリ本体は MIT（`LICENSE`）。`authored` ケースは著者の新規創作物
であり、データ部は CC0 専用宣言（§13.4）で配布する。

調査根拠（2026-06、DEV-114）。各出典の一次情報 URL は §13.5。

### 13.2 出典判定表

| 出典 | ライセンス | 派生再配布 | 本ベンチでの扱い |
|---|---|---|---|
| 自前作成（authored） | 著者帰属 → CC0 専用宣言 | 可 | **PRIMARY**。全カテゴリの基幹 |
| Wikidata ラベル | CC0 1.0 | 可（義務なし） | **USABLE**。named_entity の seed |
| 青空文庫（著作権切れ作品） | パブリックドメイン | 可（商用可・改変可、クレジットは希望） | **USABLE（限定）**。creative/general の文脈素材のみ。著作権存続作品は不可 |
| mozc 辞書データ | BSD-3 + 第三者混在（IPAdic/ICOT 等） | 帰属付きで可・但し混在 | **参照のみ**（語の着想）。エントリ複製はしない |
| SudachiDict | Apache-2.0（UniDic + NEologd 一部内包） | 帰属付きで可・但し由来混在 | **参照のみ**。エントリ複製はしない |
| mecab-ipadic-NEologd | Apache-2.0 表示／語は Web・はてな・ニュース由来 | 来歴不透明 | **EXCLUDE**（派生元不可） |
| UniDic | GPL/LGPL/BSD 三重（BCCWJ 由来） | BCCWJ 由来で来歴制約 | **EXCLUDE** |
| JMdict / EDICT / KANJIDIC | CC BY-SA 4.0 | share-alike 伝播 | **EXCLUDE**（コピーレフト） |
| Wikipedia（日本語）本文・タイトル | CC BY-SA 4.0 + GFDL | share-alike 伝播 | **EXCLUDE** |
| Anthy 辞書/テスト | GPL | コピーレフト | **EXCLUDE** |
| BCCWJ / CSJ（NINJAL） | 有償・申請制 | 再配布不可 | **EXCLUDE** |
| 京大テキストコーパス / KWDLC | 注釈のみ（毎日新聞 CD-ROM 別途）/ 非許諾 | 本文再配布不可 | **EXCLUDE** |

> 注意（誤解しやすい点）: JMdict/EDICT/KANJIDIC は「自由辞書」と誤認され
> やすいが EDRDG の CC BY-SA 4.0（コピーレフト）であり、これを参照して
> 読み→表層ペアを書き起こす行為も派生物の share-alike を誘発し得るため
> 派生元に使わない。NEologd は Apache 表示だが語が Web/はてな由来で上流
> 著作権の来歴が追えないため EXCLUDE。

### 13.3 出典 manifest（`bench/data/PROVENANCE.json`）

`authored` 以外の `provenance` を 1 件でも含む場合、`bench/data/PROVENANCE.json`
に出典エントリを置く。ケースの `provenance` 値は manifest の `id` と対応する。

```json
{
  "version": 1,
  "sources": [
    {
      "id": "wikidata",
      "name": "Wikidata structured data (labels)",
      "license": "CC0-1.0",
      "url": "https://www.wikidata.org/wiki/Wikidata:Licensing",
      "used_for": ["named_entity seed surface/reading"],
      "attribution_required": false
    },
    {
      "id": "aozora",
      "name": "Aozora Bunko (copyright-expired works only)",
      "license": "Public-Domain",
      "url": "https://www.aozora.gr.jp/guide/kijyunn.html",
      "used_for": ["creative/general left_context snippets"],
      "attribution_required": false,
      "note": "作品名・著者名・入力者名の表示は青空文庫の希望事項。著作権存続作品は使用不可"
    }
  ]
}
```

### 13.4 データ部ライセンス宣言（`bench/data/DATA-LICENSE.md`）

`bench/data/` 配下の評価データは、リポジトリ本体（MIT）とは別に
**CC0 1.0（パブリックドメイン専用宣言）**として配布する旨を
`bench/data/DATA-LICENSE.md` に明記する（M52 実装時に追加）。これにより
評価データを他プロジェクトが再利用しても share-alike が伝播しない。
一律 CC0 が成立するのは §13.1 のとおり**複製・派生する出典を CC0 / PD /
`authored` のみに限定**し、notice 付き許諾物（MIT/Apache/BSD）由来データを
含めないためである。青空文庫由来の文脈素材を含む場合は §13.3 manifest と
DATA-LICENSE.md にパブリックドメイン根拠（底本著作権満了）と希望クレジット
を併記する（PD は法的義務ではないが出所明示として残す）。

### 13.5 一次情報 URL

- Wikidata Licensing（構造化データは CC0）: https://www.wikidata.org/wiki/Wikidata:Licensing
- 青空文庫 収録ファイルの取り扱い基準: https://www.aozora.gr.jp/guide/kijyunn.html
- EDRDG（JMdict/EDICT）Licence＝CC BY-SA 4.0: https://www.edrdg.org/edrdg/licence.html
- SudachiDict（Apache-2.0、UniDic+NEologd 内包）: https://github.com/WorksApplications/SudachiDict
- mecab-ipadic-NEologd: https://github.com/neologd/mecab-ipadic-neologd
- mozc LICENSE（BSD-3 + 第三者データ）: https://github.com/google/mozc/blob/master/LICENSE
- 京大テキストコーパス（本文非同梱）: https://github.com/ku-nlp/KyotoCorpus
- KWDLC（ライセンス未付与）: https://github.com/ku-nlp/KWDLC
- CSJ（有償・申請制）: https://clrd.ninjal.ac.jp/csj/en/subscription.html

## 14. baseline 安定性基準（再現性）

合格基準（§9）と diff_vs_baseline は、**精度系指標が同一条件で再実行しても
ブレない**ことを前提とする。Zenzai のデコードはビームサーチ・サンプリング
非使用（`docs/zenzai-inference-spec.md` §6.2、temperature 0）で原理上決定的だが、
llama.cpp は backend / スレッド数 / batch の違いで浮動小数演算順序が変わり、
僅差ビームの順位が反転し得る。よって baseline は固定条件に対して採取する。

### 14.1 決定的採取の固定条件

baseline 採取・比較時に以下の**決定的入力（互換キー）**を固定し、§8 `config`
に記録する。これらが一致する限り、commit をまたいでも `diff_vs_baseline` は
有効（§14.2）:

- モデルファイル（`model_sha256` で固定）/ `backend`
- `decode = "beam"`、`beam_width`（=`B`）、`n_best`（=`N_zenzai`）、`max_new_tokens`
- `prompt_template_version`（プロンプト改訂で baseline 無効化）
- `thread_count`（既定 1。FP 非決定性を避けるため baseline は単一スレッド固定）
- `batch_size`（llama.cpp の `n_batch`。バッチも FP 演算順序を変え僅差ビームの
  順位を反転し得るため baseline は固定（既定 1）。bench / inference-host 側で
  バッチを調整した場合は互換キー差として baseline 再採取が必要）
- `typo_correction_mode`（`off` / `suggest` / `rank` / `aggressive`。M55 補正
  モードは候補集合を変える（§4.3.1 は `aggressive` で overcorrection を定義）。
  bench は既定 `off`。モードを変えた採取は互換キー差として baseline 再採取が必要）
- `learning_state`（`empty` または固定スナップショットの SHA。学習状態が
  混ざると user_adapt 以外の再現性が壊れる）
- `dictionary_bundle_sha256`（`--dict-dir` で読んだ同梱辞書の内容ハッシュ。読まないときは `none`。
  `.azdic` のファイル名をバイト昇順に並べ、`ファイル名(UTF-8) + 0x00 + ファイル生バイト列` を連結した全体の
  SHA-256 とする。lattice_core の補正項（`docs/lattice-conversion-spec.md` §5.2）も含むので、校正し直すと
  baseline 再採取が必要）
- `lattice`（`on` / `off`。採取でラティス変換が有効だったか。`--dict-dir` が無いときは `off`。
  `docs/lattice-conversion-spec.md` §10.1 の A / B を区別する）
- `nll`（`on` / `off`。採取で NLL 再ランク（§7 の `--nll`）が有効だったか。NLL は候補の並びを変えるので互換キーに入れる。
  値は engine の設定値で、各変換で実際に再ランクが走ったか（モデル未ロードや deadline で飛ばした変換を含む）ではない。
  このキーを持たない結果は、製品の既定と同じ `off` として扱う）
- `lattice_with_model`（`off`、または `k=<K>;band=<best|lowest>`。§7 の `--lattice-with-model` と `--lattice-band` の値。
  段階 2 は候補集合を変えるので互換キーに入れる。このキーを持たない結果は `off` として扱う）
- `nll_breaker`（`product` / `per_request`。§7 の `--nll-breaker` の値。breaker の扱いは NLL が走る変換を変えるので
  互換キーに入れる。このキーを持たない結果は `product` として扱う）
- `first_clause_probe`（`on` / `off`。§7 の `--first-clause-probe` の有無。ケースの所要時間と breaker の状態を変えるので
  互換キーに入れる。このキーを持たない結果は `off` として扱う）
- `eval_source`（`bundled` / `personal`。§4 の同梱データを評価した結果は `bundled`、§15 の個人回帰ベンチの
  結果は `personal`。このキーを持たない結果は `bundled` として扱う。同梱データと利用者の確定履歴の結果を
  比べない）
- `profile_sha256`（§15.5 の bench 専用の `--profile` で渡した Zenzai のプロファイル（`docs/zenzai-inference-spec.md`
  §9.4.3 の測定に使う）の UTF-8 バイト列の SHA-256。渡さないときは `none`。このキーを持たない結果は `none` として
  扱う。プロファイルは Zenzai の候補を変えるので互換キーに入れる。プロファイルの文字列そのものは出力しない）
- `category_filter`（`--category` の値。既定 `all`。フィルタすると評価対象
  ケース集合が変わるが `eval_dataset_sha256` はファイル全体ハッシュで不変なため、
  本キーで区別する。フィルタ実行を全体 baseline と誤って比較しない。`eval_source = personal` では常に `all`。§15.5）
- `eval_dataset_sha256`（評価対象 `bench/data/*.jsonl` の内容ハッシュ。
  ケース集合が変わると指標差は converter 挙動ではなくデータ差に由来するため、
  **eval ファイル変更時は baseline 再採取が必須**）。**正準算法**: 評価対象の
  `*.jsonl` を `bench/data/` 起点の相対パスでバイト昇順ソートし、各ファイルに
  つき `相対パス(UTF-8) + 0x00 + ファイル生バイト列` を境界デリミタ無しで連結
  した全体を SHA-256。**改行はハッシュ前に LF へ正規化**（CRLF→LF）し、
  併せて `.gitattributes` の `bench/data/**.jsonl text eol=lf` で working-tree も
  LF に固定する（Windows の `core.autocrlf` で CRLF 化しても Win/Linux/CI で
  同一ハッシュになるように）。これにより glob 順・プラットフォーム差で同一
  データが別ハッシュになることを防ぐ。`eval_source = personal` では、ファイルではなく §15.7 の算法で求める
- `version`（§8 出力 JSON の**指標スキーマ版**。指標計算ロジックや JSON 意味論
  を変えたら +1 する。同一の model/dataset/decode でも指標定義が変わると旧
  baseline との比較は無意味になるため、**`version` 変更時は baseline 再採取が
  必須**）。CER の集計法（micro/macro）・NFKC 正規化器（正規化テーブルの
  Unicode データ版）を変えると `cer` / `nfkc_*` の意味が変わるため、これらの
  変更も `version` +1 の対象とする（§6.1.1 / §6.1.2）

`build_id` / `host_version` は**追跡用に記録するのみで互換キーに含めない**
（commit ごとに変わるため。これらをキーにすると毎 PR で baseline が無効化され
回帰ゲートが回らない）。

### 14.2 安定性の合否

- **精度系指標は決定的**であること: 同一マシン・同一 `config` で連続 2 回の
  フル実行が、全ケースの top-k 順位を bit 一致で再現し、§6.1/§6.2 の各指標の
  差が **≤ 0.2pp**。超過時は非決定要因（スレッド数・batch・未固定 seed）を
  調査してから baseline を確定する。
- **latency 系（§6.3）は本質的に可変**のため安定性合否の対象外。`--iterations N`
  （既定 N ≥ 30）で採取し p50/p95/p99 は複数実行の中央値で報告する。
- baseline は §14.1 の**互換キー**（`model_sha256` / `backend` / `decode` /
  `beam_width` / `n_best` / `max_new_tokens` / `prompt_template_version` /
  `thread_count` / `batch_size` / `typo_correction_mode` / `learning_state` /
  `dictionary_bundle_sha256` / `lattice` / `lattice_with_model` / `nll` / `nll_breaker` / `first_clause_probe` / `eval_source` / `profile_sha256` /
  `category_filter` / `eval_dataset_sha256` / `version`）の組ごとに固定する。
  **diff_vs_baseline は互換キーが一致するときのみ有効**で、`build_id` 差
  （=通常の commit 差）では無効化しない。よって `main` の直前の成功 run の結果（§10）に
  対する commit 間の回帰判定（roadmap M52 の「前 commit との diff」）が成立する。
  互換キーが変わる比較（モデル・デコード設定・プロンプト改訂、**および評価
  データセット自体の変更**）のみ「baseline 再採取が必要」と明示し、データ差を
  converter 回帰と誤認しない。

### 14.3 回帰ゲート（CI §10）

- PR 向け回帰ゲート（§10）の回帰判定は精度系のみを fail 条件にする
  （latency は警告レポートに留める）。
- 既定の fail 閾値: `top1_accuracy` が baseline 比 **−0.5pp 超**の悪化。
  flaky 誤検出を避けるため、悪化検出時は同一 config で 1 回再実行して確認した
  うえで fail を確定する（§14.2 が満たされていれば 2 回目は一致するはず）。
- `exact_match_rate` / `cer`（いずれも素、§6.1.1）は**回帰監視系**として
  `diff_vs_baseline` に必ず出し PR コメントに載せるが、v1 のハード fail 条件は
  `top1_accuracy` のみとする（`top1_accuracy` が `expected`/`acceptable[]` を
  救うため最も安定した gate になる）。`cer` の顕著な悪化（例 baseline 比 +0.5pp 超）は
  レビュー注意喚起に留め、閾値による自動 fail 化は M53 以降でデータ規模が
  揃ってから検討する。NFKC 系（`nfkc_*`）は診断専用で fail・監視いずれの
  対象にもしない。

## 15. 個人回帰ベンチ（M74）

利用者の端末の中だけで、利用者自身の確定履歴を評価ケースにして、変換器を差し替えた前後の当たり方を比べる。
§4 の評価データは合成で、利用者ごとの語彙への当たり方を測れない。本節はそれを補う。利用者が CLI を
実行したときだけ動き（opt-in）、結果をどこへも送らない。

### 15.1 範囲

- **測るもの**: 学習を空にした変換器が、左文脈なしで、利用者が確定した表層をどの順位に出すか。
  モデル（`--model`）、同梱辞書（`--dict-dir`）、ラティス変換（`--lattice`）、NLL 再ランク（`--nll`）、
  Zenzai のプロファイル（§15.5 の `--profile`）、ビルドを差し替えた前後を比べる。
- **v1 の対象外**:
  - 左文脈による同音異義語の選び分け。学習ストアは左文脈を `context_hash` でしか持たない
    （`docs/user-learning-enhancement-spec.md` §8.1）。評価のために左文脈を保存することは、左文脈の別用途の再利用を
    禁じる `docs/privacy-and-secure-input-spec.md` §5.3 と、文脈を hash で持つ `docs/user-learning-enhancement-spec.md`
    §2 に反するので行わない。
  - 学習係数（`docs/user-learning-enhancement-spec.md` §13）の校正。学習ストアを変換器に載せると、評価ケースを作った
    学習データ自身で候補を並べることになり、学習の効き目を過大に示す。学習ストアの行は集約で（同 §3.2）、時刻で
    学習用と評価用に分けることもできない。係数の校正は M52 の `user_adapt`（§11.2）で行う。
  - 同梱の評価データ（§4）の作成。§11.1 のとおり、利用者の入力からリポジトリのデータを作らない。
- **実行の入口**: `azookey_bench --personal-eval`。`azookey_bench` は配布物（`pkg/msi/`）に含まれないので、
  ソースからビルドした人だけが実行できる。設定アプリと配布される実行ファイル（`azookey_inference_host.exe` の
  サブコマンドなど）からの実行は v1 では持たない。

### 15.2 評価に使うデータ

- 追加の保存はしない。読むのはかなの学習ストア（`docs/user-learning-enhancement-spec.md` §3.1 の v2 ファイル。
  v2 ファイルが無ければ M7 ファイル）だけで、英語の学習チャネル、タイプミス補正、新語候補、ユーザー辞書は読まない。
- 移行をしない読み込み（`LearningStore::LoadReadOnly`）で読み、保存しない。平文の移行、`.corrupt-*` の複写、
  M7 ファイルの削除は起こさない。読み込みに失敗したら、部分結果を出さずに入力エラーとして終える。学習ストアの
  ファイルが無いときも読み込みの失敗として扱う。解析できない行は Host と同じく読み飛ばし
  （`docs/user-learning-enhancement-spec.md` §3.1）、その件数を除外の理由 `malformed_row`（§15.6）に数える。読みか表層が
  空の行と UTF-8 として不正な行も、評価できないので同じ理由に数える。
- 学習ストアの位置は Host と同じ既定の位置とし、`--learning <path>` で変えられる。privacy の判定（§15.4）に
  使う設定ファイルも Host と同じ既定の位置とし、`--settings <path>` で変えられる。
- 設定ファイルは、隔離（`.invalid*` への rename）をしない読み取り専用の parse で読む。Host の `SettingsStore::Load` の
  ように parse の失敗でファイルを移さない。parse の失敗は設定の不正として扱う（§15.4）。
- `--settings` で明示したパスにファイルが無いときは、評価せずに入力エラーとして終える。設定ファイルが無いことを
  normal とみなす `docs/privacy-and-secure-input-spec.md` §5.1 の規則は、既定の位置にファイルが無いとき（設定を
  一度も保存していないとき）だけに適用する。
- 行の単位は確定の文節である。文節ごとの確定観測（`CommitSegmentsObservation`）は文節ごとに 1 組を記録する。

### 15.3 評価ケースの作り方

1. §15.4 で除く行を除いた残りを、読みごとにまとめる。同じ読みで同じ表層の行（`app_name` だけが違う行）は、
   `commit_count` と `reject_count` を足し合わせる。和の差 `commit_count − reject_count` を、その表層の純確定数とする。
2. 読みごとに、表層を次の 3 つに分ける。
   - 純確定の表層: 純確定数が正
   - 相殺の表層: 純確定数が 0。確定の直後に即 Backspace で取り消した表層は、確定と拒否が 1 ずつ増えるので
     これに当たる（`docs/user-learning-enhancement-spec.md` §4）
   - 純拒否の表層: 純確定数が負
3. 純確定の表層を持つ読みごとに、ケースを 1 件作る。相殺の表層は、正解にも拒否にも数えない。

   | フィールド（§4.2） | 値 |
   |---|---|
   | `input` | 読み |
   | `expected` | 純確定数が最大の表層。同じなら `commit_count` の和が大きい方、それも同じなら UTF-8 バイト列の昇順で先の表層 |
   | `acceptable` | `expected` 以外の純確定の表層 |
   | `left_context` / `right_context` | 空 |
   | `app_profile` | 既定（アプリ別の設定を当てない） |

4. 純確定の表層を持たない読みはケースにせず、除外の件数（理由 `no_net_commit`）に数える。
5. 件数の上限は `--max-cases <N>`（既定 1000、0 は上限なし）とする。読みごとの純確定の表層の純確定数の和の降順、
   同じなら読みの UTF-8 バイト列の昇順に並べて先頭から N 件を取る。上限で落とした読みは除外の件数
   （理由 `over_max_cases`）に数える。ケースの順はこの並びとする。

### 15.4 含めないデータ

学習ストアの行は、確定観測と訂正を TIP と Host が学習を許したときに記録する（`docs/privacy-and-secure-input-spec.md`
§5.1.1）。secure の入力は記録しないので、評価ケースにも入らない。確定ごとの判定を通らない行は、M7 ファイルからの
移行（`docs/user-learning-enhancement-spec.md` §3.1）と、学習データのインポート（`docs/learning-data-management-spec.md`
§4.4。secure と学習不許可の間は止まる。同 §8）で入った行である。そのうえで次を適用する。

- **実行を止める設定**: 設定を解決した学習許可が偽のときは、評価をせずに入力エラーとして終える
  （`docs/privacy-and-secure-input-spec.md` §5.1 の解決に従う）。`privacy.mode` が `secure` のとき、`private` で学習が
  止まるとき、`custom` の `custom.learning` が `false` のとき、設定の読取失敗・JSON 不正・未知のモード、`--settings` で
  明示したファイルが無いとき（§15.2）を含む。学習データの書き出しを secure と学習不許可の間に止める
  `docs/learning-data-management-spec.md` §8 と同じ規則である。
- **除く行**: 実行時の設定で、行の `app_name` について次のどれかに当たる行を除く。理由ごとに件数を数える。
  複数に当たる行は、次の並びで最初に当たった理由で 1 回だけ数える。
  - 実効の `secureApps`（`docs/privacy-and-secure-input-spec.md` §4.1 の同梱リストと `privacy.secureApps` の和。
    大文字小文字を無視して比べる）に含まれる（理由 `secure_app`）。
  - アプリ別プロファイル（`docs/app-profile-spec.md` §5）を Host・TIP と同じ解決（大域の値に `profilesByApp["default"]` と
    `profilesByApp[app_name]` を重ねる）で求めた `privacyMode` が `private` か `secure` である（理由 `app_privacy_mode`）、
    または `learningEnabled` が `false` である（理由 `app_learning_disabled`）。行はウィンドウクラスを持たないので、
    `window_class` の層は使わない。
- **含める行**: `app_name` が空の行は含め、記録した時点の判定で学習が許された行として扱う。確定観測と訂正が `app` を
  運ばないとき（`docs/user-learning-enhancement-spec.md` §4.1）に Host が記録する大域行と、M7 ファイルから移行した行が
  これに当たる。
- 上の「除く行」は `app_name` を持つ行だけに効く。後から `secureApps` に加えたアプリや学習を止めたアプリの過去の
  確定でも、大域行に入っていれば除かれない。

### 15.5 変換の条件と CLI

engine は空の一時学習ストアで作る（§3 の `azookey_bench` と同じ。`config.learning_state = "empty"`）。
typo 補正は `off`、カテゴリの絞り込みは `all` とする。top5 系の指標のため、`n_best` は §6.1 の注記のとおり
5 以上で採取する。

| オプション | 意味 |
|---|---|
| `--personal-eval` | 個人回帰ベンチとして動かす。`--eval` と併用しない |
| `--learning <path>` | 学習ストアの位置（§15.2） |
| `--settings <path>` | privacy の判定に使う設定ファイルの位置（§15.2） |
| `--max-cases <N>` | ケースの上限（§15.3） |
| `--include-personal-text` | `--per-case` に読みと表層を書くことを許す（§15.7） |
| `--profile <text>` | bench 専用。engine がモデル変換に渡す文脈と NLL 再ランクに渡す文脈の両方の `instruction_profile` に文字列を入れ、Zenzai の prompt の `[PROF]` に置いて変換する。`docs/zenzai-inference-spec.md` §9.4.3 の効果の測定に使う。製品の設定（`model.profile`）は読まず、Host の挙動も変えない。§4 の評価データでも使える。空文字列、U+EE00〜U+EEFF を含む文字列、30 コードポイントを超える文字列、`[PROF]` を使わない形式のモデル（同 §2.4.2）、`--model` の無い構成では入力エラーとして終える。`config.profile_sha256` に記録する（§14.1） |
| `--profile-file <path>` | `--profile` と同じ。文字列を UTF-8 のファイルから読み、末尾の改行 1 つ（LF か CRLF）を除く。先頭の BOM と不正な UTF-8 は入力エラーとする（黙って除くと prompt と `profile_sha256` がずれる）。シェルの履歴に文字列を残さないために使う。`--profile` と併用しない |

個人回帰ベンチでは、上の表のオプションと、§7 と同じ意味で使う `--output`、`--baseline`、`--per-case`、`--backend`、
`--model`、`--dict-dir`、`--lattice`、`--nll`、`--iterations` だけを受け付ける。`--dict-dir` と `--lattice` は M65 が
`azookey_bench` に足すオプションで（§0）、それを持たないビルドでは受け付けない。それ以外（`--eval`、`--trace`、
`--trace-output`、`--decode-stats`、`--category`、`--typo-mode`、`--json`、`--ipc`、`--max-p95-ms` など）は入力エラーと
して終える。変換ごとの記録をファイルへ書く経路を、§15.7 の明示した per-case 以外に作らない。

### 15.6 指標

§6.1 の指標（`named_entity_recall_at_5` と `neologism_recall_at_5` を除く）を、§15.3 の `expected` と `acceptable` で
求める。加えて次を出す。純確定数は §15.3 の定義による。

| 指標 | 定義 |
|---|---|
| `commit_coverage_top1` | 全ケースの純確定の表層の純確定数の和に対する、第 1 候補が純確定の表層だったときのその純確定数の和の割合。学習が無いときに、過去の確定のうち第 1 候補で済んだ割合を表す |
| `commit_coverage_top5` | 同じ分母に対する、上位 5 候補に出た純確定の表層の純確定数の和の割合 |
| `rejected_top1_rate` | 純拒否の表層（§15.3）を持つケースのうち、第 1 候補が純拒否の表層だったケースの割合 |

ケースが 0 件のとき（学習データの全削除の後など）は、比率の指標をすべて 0 とし、`n_cases` を 0 として正常に終える。
`rejected_top1_rate` は、純拒否の表層を持つケースが 0 件なら 0 とする。

`by_category` は `expected` の字種で分ける。次の順に最初に当てはまるものとする。

| カテゴリ | 条件 |
|---|---|
| `personal_identity` | 表層が読みと同じ |
| `personal_kanji` | 漢字を含む |
| `personal_katakana` | カタカナと長音だけから成る |
| `personal_ascii` | ASCII だけから成る |
| `personal_other` | 上のどれにも当たらない |

`summary` には件数として、ケース数 `n_cases`、純拒否の表層を持つケース数 `n_rejected_cases`、読んだ行数
`n_rows_read`（評価に使える形で読めた行。`malformed_row` に数えた行は含まない）、理由ごとの除外の件数 `skipped`
（`malformed_row`、`no_net_commit`、`secure_app`、`app_privacy_mode`、
`app_learning_disabled`、`over_max_cases`）を載せる。latency は §6.3 と同じに求める。

### 15.7 出力と baseline

- `--output` は §8 の形式で、集計だけを書く。読み、表層、プロファイルの文字列は `--output` にも stderr にも出さない。
- `config.eval_source` は `personal` とする（§14.1）。`config.eval_dataset_sha256` は次の列の SHA-256 とする。
  §15.3 でケースにした読みについて、§15.4 の除外の後に残る全表層（純確定・相殺・純拒否）を、読み、表層の UTF-8
  バイト列の昇順に並べ、組ごとに `読み + 0x00 + 表層 + 0x00 + commit_count の和（10 進） + 0x00 + reject_count の和
  （10 進） + 0x00` とした列を連結する。ケースにならなかった読みと、上限で落とした読みは含めない。`expected` と
  `acceptable` はこの値だけから決まる。
- `--baseline` は、§14.1 の互換キーがすべて一致するときだけ `diff_vs_baseline` を出す。一致しないときは出さず、
  違うキーの名前だけを stderr に出す。A / B の比較は、学習ストアが変わらないうちに 2 つの構成を続けて実行する。
  間に確定があると `eval_dataset_sha256` が変わるので、実行し直す。学習ストアの写しは保存しない。プロファイルの
  有無のように互換キーが違う 2 回の結果は、`diff_vs_baseline` を使わず、2 つの `--output` の `summary` を比べる。
- `eval_source = personal` の結果ファイルは共有しない。`config.eval_dataset_sha256` と `config.profile_sha256` は
  塩を加えない SHA-256 なので、ケースが少ないときやプロファイルが短いときは、候補の文字列を総当たりして学習データや
  プロファイルを推測されうる（`docs/learning-data-management-spec.md` §5.1 が平文のハッシュを書かないのと同じ理由）。
  個人回帰ベンチは、実行のたびにそのことを stderr に出す。§4 の評価データで `--profile` か `--profile-file` を使った
  結果も、`profile_sha256` が `none` でなければ共有せず、そのときも同じことを stderr に出す。新しい秘密値（塩や鍵）は
  保存しない。
- `--per-case <jsonl>` は `--include-personal-text` を併せたときだけ書く。無ければ入力エラーとして終える。形式は
  §8.1 の通常変換ケースで、`id` は `personal_` に §15.3 のケースの順の 1 始まりの 6 桁の連番を付けたもの、行の順も
  ケースの順とし、`category` は §15.6 のカテゴリとする。書くときは、ファイルが利用者の読みと表層を平文で含むこと、
  暗号化しないこと、学習データの全削除（`docs/learning-data-management-spec.md` §4.6）では消えないことを stderr に
  出す。このファイルの削除は利用者が行う。
- ベンチはネットワークに接続しない。結果を送らず、テレメトリも持たない。CI の品質採取（§10、
  `scripts/run-quality-bench.py`）は個人回帰ベンチを呼ばない。

### 15.8 プライバシー spec との整合

本節は `docs/privacy-and-secure-input-spec.md` と次のとおり矛盾しない。

| privacy spec の規則 | 本節の対応 |
|---|---|
| §2 ローカル完結 | 端末の外へ何も送らない（§15.7） |
| §4.1 `secureApps` | `app_name` を持つ行のうち、実効の `secureApps` に入るアプリの行を除く。大域行に入った過去の確定は除けない（§15.4） |
| §5 secure 中は学習観測を送らず `Observe` を呼ばない | secure の入力は学習ストアに入らず、評価ケースにならない（§15.4） |
| §5.1 設定の読取失敗・不正は secure 相当 | 評価をせずに終える。`--settings` で明示したファイルが無いときも同じ（§15.2・§15.4） |
| §5.3 左文脈を別用途に再利用しない | 左文脈を保存も利用もしない（§15.1） |
| §8 ログに `reading` / `surface` を含めない | 出力と stderr に読みと表層を出さず、trace を作らない（§15.5・§15.7）。例外は利用者が明示した per-case ファイルだけ |

学習データの全削除の後は学習ストアが空になり、評価ケースは 0 件になる。ベンチは学習ストアの写しを持たないので、
全削除の後に評価用のデータは残らない。残りうるのは、利用者が `--include-personal-text` で明示して書いた per-case
ファイルと、利用者が保存した結果ファイルだけである。

学習ストアの読み手は、Host と `lookup` CLI に `azookey_bench --personal-eval` が加わる。
`docs/learning-data-management-spec.md` §14 の表もこの読み手を載せる。

### 15.9 テスト

- unit: ケースの作り方（§15.3）。`app_name` をまたぐ和、純確定・相殺・純拒否の分け方（確定の直後に即 Backspace で
  取り消した表層が正解にならないこと）、`expected` の同点の規則、`no_net_commit`、`--max-cases` の決定的な選び方
- unit: 除外（§15.4）。実効の `secureApps`、`privacyMode`、`learningEnabled` の各理由と、`app_name` が空の行を含めること
- unit: 実行を止める設定（§15.4）。secure、`private`、`custom.learning = false`、設定の読取失敗と JSON 不正、`--settings` で
  明示したファイルが無いときに評価しないこと。JSON 不正の設定ファイルを移さないこと（§15.2）
- unit: 解析できない行を読み飛ばし、`malformed_row` に数えること（§15.2）
- unit: 個人回帰ベンチの実行のたびに、結果ファイルを共有しない旨を stderr に出すこと（§15.7）
- unit: 学習ストアのファイルが無いときは入力エラー、空の学習ストアでは `n_cases = 0` で正常に終えること（§15.2・§15.6）
- unit: 出力に読み・表層・プロファイルの文字列が出ないこと（固定の学習ストアから作った `--output` と stderr を走査する）
- unit: `eval_dataset_sha256` が同じ学習ストアで一致し、行の順序に依存しないこと
- unit: `--per-case` が `--include-personal-text` 無しで入力エラーになり、有りでは警告を出し、`id` と行の順が §15.7 のとおりであること
- unit: 個人回帰ベンチで受け付けないオプション（§15.5）が入力エラーになること
- unit: 学習ストアを書き換えないこと（実行の前後でファイルのバイト列と更新時刻が同じ）
- unit: `--profile` と `--profile-file` が、モデル変換と NLL 再ランクの両方の prompt の `[PROF]` に文字列を置き、
  `config.profile_sha256` を記録し、渡さないときは `none` になること。§15.5 の入力エラーの条件と、文字列が出力に出ないこと
