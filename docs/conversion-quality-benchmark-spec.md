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

`*.jsonl` は 1 行 1 ケース。**第一の作成ルートは自前で書き起こす
オリジナルケース**（読み → 表層 → 文脈を本リポジトリ著者が新規作成）で、
公開コーパスは限定的な seed・文脈素材としてのみ二次利用する。利用可能な
出典・ライセンスはカテゴリ別に §13 で確定する。§11 のライセンス方針
（CC0 / パブリックドメイン / MIT・Apache・BSD 互換のみ。コピーレフトは
不可）に従い、CC BY-SA / GFDL 系（Wikipedia 本文・JMdict/EDICT 等）と
再配布不可・来歴不透明なデータ（BCCWJ・京大コーパス・NEologd 等）は
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
| `category` | カテゴリ tag の配列 |
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
    "model_sha256": "e3b0c44298fc1c149afbf4c8996fb924...",
    "host_version": "0.1.0",
    "build_id": "abc123",
    "decode": "beam",
    "beam_width": 4,
    "n_best": 4,
    "max_new_tokens": 64,
    "prompt_template_version": 1,
    "thread_count": 1,
    "learning_state": "empty"
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
- データ追加・修正は PR レビュー必須。`authored` 以外を追加する PR は
  §13.3 manifest の更新を必須とする。

### 11.2 カテゴリ別件数（代表性）

段階導入する。**M52 初期版**は受け入れ条件（§12）に必要な 4 カテゴリ
（general / homophone / typo / typo_clean）の最小セットで緑化し、
**v1 完全版**（合格基準 §9 を測る規模）を M53 着手前までに満たす。

| category | M52 初期 | v1 完全版 | 代表性の要点 |
|---|---:|---:|---|
| `general` | 100 | 200 | domain 5 種をほぼ均等。difficulty 1–5 を 2:3:3:1:1 目安 |
| `homophone` | 100 | 200 | 高頻度同音異義（こうしょう/たいしょう 等）を文脈で弁別。`left_context` 必須 |
| `typo` | 100 | 150 | §4.3 の typo_type 11 種を各最低 10 件で網羅 |
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

- 1 ケースは複数 `category` tag を持ってよい（例 typo ∧ homophone）が、
  件数カウントは主カテゴリ 1 つで数える（重複カウントしない）。
- `difficulty` と `domain` の分布は各カテゴリ内で偏らせない。極端な難問
  のみ／特定ドメインのみのカテゴリは代表性不足として PR レビューで差し戻す。
- 1 表層・1 読みの重複ケースは禁止（`id` 単位で一意、(`input`,`left_context`)
  の完全重複も不可）。

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
- 初期版の評価データが §11.2「M52 初期」列を満たす
  （general / homophone / typo / **typo_clean** の 4 カテゴリ各 100、計 ≥400）。
  `typo_clean` は §4.3.1 のとおり `typo_false_positive_rate` /
  `typo_overcorrection_rate` の分母であり、欠くと typo 指標を正しく計算
  できないため初期版から必須とする
- すべての評価ケースが §13 の USABLE 出典・ライセンス方針に適合し、
  `provenance` を持つ。`authored` 以外を含む場合は §13.3 manifest が存在する
- baseline が §14 の安定性基準（決定的設定で再実行差 ≤ 0.2pp）を満たす

## 13. 評価データの出典・ライセンス（確定）

### 13.1 方針

配布物（公開 GitHub リポジトリ）に同梱・再配布するため、評価データの
派生元は **CC0 / パブリックドメイン / MIT・Apache・BSD 互換**に限る。
コピーレフト（CC BY-SA / GFDL / GPL）と再配布不可・来歴不透明なデータは
派生元に使わない。本リポジトリ本体は MIT（`LICENSE`）。`authored` ケースは
著者の新規創作物であり、データ部は CC0 専用宣言（§13.4）で配布する。

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
青空文庫由来の文脈素材を含む場合は §13.3 manifest と DATA-LICENSE.md に
パブリックドメイン根拠（底本著作権満了）と希望クレジットを併記する。

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

baseline 採取・比較時に以下を固定し、§8 `config` に記録する:

- モデルファイル（`model_sha256` で固定）/ `backend` / `build_id`
- `decode = "beam"`、`beam_width`（=`B`）、`n_best`（=`N_zenzai`）、`max_new_tokens`
- `prompt_template_version`（プロンプト改訂で baseline 無効化）
- `thread_count`（既定 1。FP 非決定性を避けるため baseline は単一スレッド固定）
- `learning_state`（`empty` または固定スナップショットの SHA。学習状態が
  混ざると user_adapt 以外の再現性が壊れる）

### 14.2 安定性の合否

- **精度系指標は決定的**であること: 同一マシン・同一 `config` で連続 2 回の
  フル実行が、全ケースの top-k 順位を bit 一致で再現し、§6.1/§6.2 の各指標の
  差が **≤ 0.2pp**。超過時は非決定要因（スレッド数・batch・未固定 seed）を
  調査してから baseline を確定する。
- **latency 系（§6.3）は本質的に可変**のため安定性合否の対象外。`--iterations N`
  （既定 N ≥ 30）で採取し p50/p95/p99 は複数実行の中央値で報告する。
- baseline は (`build_id`, `backend`, `model_sha256`, `prompt_template_version`)
  の組ごとに固定する。**diff_vs_baseline は同一の組に対してのみ有効**。
  組が変わる比較は「baseline 再採取が必要」と明示し、誤った回帰判定をしない。

### 14.3 回帰ゲート（CI §10）

- `quality-bench` ジョブの回帰判定は精度系のみを fail 条件にする
  （latency は警告レポートに留める）。
- 既定の fail 閾値: `top1_accuracy` が baseline 比 **−0.5pp 超**の悪化。
  flaky 誤検出を避けるため、悪化検出時は同一 config で 1 回再実行して確認した
  うえで fail を確定する（§14.2 が満たされていれば 2 回目は一致するはず）。
