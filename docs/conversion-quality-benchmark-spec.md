# 変換品質評価ベンチ 仕様

対象リポジトリ: dolquis/azooKey-Desktop
対応マイルストーン: M52（変換品質トラック）
関連: `plans/windows-port-roadmap.md` M7 / M9 / M53 / M54 / M55 / M56 / M57、
      `bench/azookey_bench`、
      `docs/dev-infrastructure-spec.md` §7.7（M51 trace）、
      `docs/user-learning-enhancement-spec.md`（M54）、
      `docs/typo-correction-learning-spec.md`（M55）
作成日: 2026-05-27
位置づけ: 変換品質トラックの最初に実装（M53〜M57 の前提）

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

既存 `bench/azookey_bench.cpp` は latency smoke（p50/p95 < 50ms）を測定
する CTest として動作する（M38）。本 M52 はそれを発展させ、以下を追加
する:

- jsonl 評価データの読み込み
- top-k accuracy / MRR / カテゴリ別精度の計算
- typo 補正の評価（false positive / overcorrection）
- baseline JSON との diff 比較

既存 latency smoke 機能は維持する（後方互換）。

## 4. 評価データ

### 4.1 ファイル構成

```
bench/data/
├── kana_kanji_eval.jsonl       # 通常変換
├── homophone_eval.jsonl        # 同音異義語（kana_kanji の subset としても可）
├── named_entity_eval.jsonl     # 固有名詞
├── neologism_eval.jsonl        # 新語
├── user_learning_eval.jsonl    # 学習前後比較
└── typo_eval.jsonl             # 打ち間違え補正
```

`*.jsonl` は 1 行 1 ケース。データ作成は手動キュレーション + 既存
オープンコーパス（青空文庫など）から派生させる。§11 のライセンス方針
（CC0 / MIT 互換のみ）に従い、CC BY-SA / GFDL 系（Wikipedia 等）は
派生元として使用しない。

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
  "difficulty": 3
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
| `category` | カテゴリ tag の配列 |
| `difficulty` | 1（易）〜 5（難） |

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
  "difficulty": 2
}
```

`typo_type` は M55 で定義する 11 種:

`adjacent_key` / `missing_char` / `extra_char` / `transposition` /
`double_key` / `romaji_variant` / `n_handling` / `small_tsu` /
`long_vowel` / `dakuten_confusion` / `kana_shape_confusion`

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

## 6. 指標

### 6.1 通常変換指標

| 指標 | 定義 |
|---|---|
| `top1_accuracy` | 第一候補が `expected` or `acceptable[]` のいずれかと一致した率 |
| `top3_accuracy` | 上位 3 候補のいずれかが一致した率 |
| `top5_accuracy` | 上位 5 候補のいずれかが一致した率 |
| `MRR` | Mean Reciprocal Rank（正解の順位の逆数の平均） |
| `exact_match_rate` | 第一候補が `expected` と完全一致した率 |
| `acceptable_match_rate` | 第一候補が `acceptable[]` のいずれかと一致した率 |
| `reading_fidelity_rate` | 候補の読みが入力読みと一致する率（M55 補正候補除く） |
| `named_entity_recall_at_5` | category=named_entity の top5 率 |
| `neologism_recall_at_5` | category=neologism の top5 率 |

### 6.2 打ち間違え補正指標

| 指標 | 定義 |
|---|---|
| `typo_correction_top1_accuracy` | typo 入力に対し第一候補が正解になる率 |
| `typo_correction_top5_accuracy` | 正解が上位 5 候補に入る率 |
| `typo_false_positive_rate` | 正常入力を誤って typo 扱いした率 |
| `typo_overcorrection_rate` | 意図した入力を別候補へ過補正した率 |
| `typo_suggestion_accept_rate` | 補正候補が提示され採用された率 |
| `typo_suggestion_reject_rate` | 補正候補が提示され明示拒否された率 |

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

# 打ち間違え評価
azookey_bench.exe --eval bench/data/typo_eval.jsonl \
                  --output typo_result.json \
                  --baseline typo_baseline.json

# trace 連携（M51）
azookey_bench.exe --eval bench/data/kana_kanji_eval.jsonl \
                  --trace \
                  --output result.json
```

オプション:

| オプション | 意味 |
|---|---|
| `--eval <jsonl>` | 評価ケースファイル |
| `--output <json>` | 結果出力 |
| `--baseline <json>` | 比較 baseline |
| `--backend <name>` | backend 強制 |
| `--model <path>` | モデル強制 |
| `--trace` | M51 trace ログを出す |
| `--category <name>` | カテゴリ絞込 |
| `--iterations <N>` | 各ケースの繰返し回数（latency 用） |

## 8. 出力 JSON 形式

```json
{
  "version": 1,
  "timestamp": "2026-05-27T10:00:00+09:00",
  "config": {
    "backend": "cpu",
    "model": "zenzai-small.gguf",
    "host_version": "0.1.0",
    "build_id": "abc123"
  },
  "summary": {
    "top1_accuracy": 0.85,
    "top5_accuracy": 0.96,
    "MRR": 0.91,
    "latency_p95_ms": 45.2,
    "memory_peak_mb": 1820
  },
  "by_category": {
    "homophone": {
      "top1_accuracy": 0.78,
      "top5_accuracy": 0.94,
      "n": 250
    },
    "named_entity": { ... }
  },
  "diff_vs_baseline": {
    "top1_accuracy": +0.03,
    "homophone.top1_accuracy": +0.05
  }
}
```

## 9. 合格基準 v1

M53〜M57 完了時点で達成する目標:

| 指標 | 目標 |
|---|---:|
| `top1_accuracy` | baseline 比 +3% 以上 |
| `top5_accuracy` | 95% 以上 |
| `homophone.top1_accuracy` | baseline 比 +5% 以上 |
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

既存 GitHub Actions に optional な `quality-bench` ジョブを追加（M52
完了時）:

```yaml
- name: Run conversion quality bench
  if: contains(github.event.pull_request.labels.*.name, 'quality-bench')
  run: |
    .\build\windows-release\bench\azookey_bench.exe \
      --eval bench\data\kana_kanji_eval.jsonl \
      --output quality-result.json \
      --baseline bench\baselines\main.json
- name: Upload quality report
  uses: actions/upload-artifact@v4
  with:
    name: quality-bench
    path: quality-result.json
```

PR コメントに diff_vs_baseline サマリを投稿（PR レビューアが品質改善を
数値で確認できるように）。

## 11. 評価データの整備方針

- 初期データは 1 カテゴリあたり最低 100 ケース、合計 1000 ケース以上
- 手動キュレーション + 既存コーパス由来の派生で構築
- ライセンス: CC0 / MIT 互換のみ
- ユーザー入力ログからの自動生成は **行わない**（プライバシー優先）
- データ追加・修正は PR レビュー必須

## 12. M52 受け入れ条件

- `azookey_bench --eval bench/data/kana_kanji_eval.jsonl --output
  result.json` で全指標を計算できる
- `azookey_bench --eval bench/data/typo_eval.jsonl --output
  typo_result.json` で typo 指標が出る
- 出力 JSON が §8 の stable schema に従う
- baseline 比較レポート（diff_vs_baseline）が生成される
- `--trace` フラグは M51 完了後の任意統合チェックとして扱う。M51
  未完了時は本フラグの存在を確認するのみで、出力 schema 検証は M51
  完了後の follow-up とする
- 1 カテゴリ以上の評価データが `bench/data/` に存在する（初期版は
  general / homophone / typo の 3 カテゴリで十分）
