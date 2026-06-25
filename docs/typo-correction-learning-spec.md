# 個人タイプミス学習・自動修正 仕様（Typo Correction Learning）

本書は「ユーザー自身がよくする打ち間違い（タイプミス）を学習し、同じタイプミスを
したときに正しい入力へ変換・提示する」機能の仕様を定める。対象は **Windows 版
（C++ 移植: `core/` `learning/` `ipc/` `inference-host/` `tsf-tip/`）** のみ。
macOS 版 `legacy/` は対象外。

対応マイルストーン: `plans/windows-port-roadmap.md` M35（v1: 基本タイプ
ミス学習）+ M55（v2: 統合補正エンジン）。
関連: `docs/rich-features-spec.md` X-3（誤変換訂正）、
      `docs/conversion-quality-benchmark-spec.md`（M52）、
      `docs/user-learning-enhancement-spec.md`（M54）、
      `docs/privacy-and-secure-input-spec.md`（M46）。

> **本仕様の段差**: §1〜§11 は M35（v1: 基本タイプミス学習）の正典。
> §12 以降は M55（v2: 統合補正エンジン）の追補章で、v1 の機能を発展
> させ、ReadingHypothesis 経由で CandidateGenerator 前段に補正を組み
> 込む統合エンジンに昇格する。v1 の `wrong_reading → correct_reading`
> ペア学習は v2 でも下位互換として維持する。

## 1. 背景と目的

- 既存の `DebugTypoCorrection`（macOS 版）は HuggingFace 配布の汎用 n-gram 言語
  モデル（`input_n5_lm_v1`）による確率的なタイプミス補正であり、**ユーザー個人の
  打鍵の癖は学習しない**。
- 本機能は、ユーザー個人が繰り返す特定のタイプミス（例: 「こんにちは」を
  「こんちには」と打つ転置ミス）を観測・蓄積し、しきい値を超えた頻出パターンを
  変換時に補正する。
- 汎用 LM 補正と独立した機能であり、併用可能。

## 2. 用語と学習単位

- **学習単位はかな読み（reading）**。ローマ字入力のミスは結果的に誤ったかな読みに
  なるため、`wrong_reading(かな) → correct_reading(かな)` のペアで学習する。
  変換器（SimpleConverter / Zenzai）に非依存で、IPC が既に `reading` を扱うため
  実装が素直。
- **頻度カウント方式**。`LearningStore` の時間減衰つき重みとは別の永続化を持つ。

## 3. 動作モード

設定 `typoCorrectionMode`（3 値）で切り替える。

| 値 | 学習 | 適用 |
|---|---|---|
| `off` | しない | しない |
| `suggest`（既定） | する | 補正後読みの変換結果を `typo-correction` マーク付きで候補リスト先頭付近に注入。元の読みの候補も残す。 |
| `auto_replace` | する | 補正後読みのみで変換し、preedit のかなも補正後へ置換。 |

- host 側 `SettingsStore` は導入済みだが、`typoCorrectionMode` の schema 追加・runtime
  反映は本機能の実装範囲。未配線の間、当面の実効値は次の経路で受ける:
  - inference-host: CLI 引数 `--typo-mode off|suggest|auto_replace`
    （環境変数 `AZOOKEY_TYPO_MODE` フォールバック）
  - TIP: 環境変数 `AZOOKEY_TYPO_MODE`（`ActivateEx` で取得）
- `off` の最終ゲートは host 側。TIP は検出した観測を緩く送り、蓄積・適用の
  可否は host が判定する（TIP に設定プラミングを持ち込まない）。

## 4. 検出（TIP: `tsf-tip/`）

タイプミスの「検出」は TIP 側で行い、観測を `ObserveTypo`（fire-and-forget）で
host に送る。検出トリガは 2 種。

### 4-1. 未確定中の backspace 訂正

確定前に backspace でかな読みを消して打ち直したケース。

- VK_BACK ハンドラ（`tsf-tip/src/TextService.cpp` 182-203 行付近）で、かな 1 文字を
  削除する直前、`in_backspace_correction_ == false` のときに
  `pre_correction_reading_ = preedit_kana_`（削る前の全読み）をスナップショットし、
  `in_backspace_correction_ = true` にする。
- 連続 backspace ではスナップショットを更新しない（burst の最初の状態を保持）。
- 確定成功時（`CommitSelected` / `CommitPreeditAsIs`）に、
  `in_backspace_correction_` かつ `pre_correction_reading_` 非空なら
  `wrong = pre_correction_reading_`、`correct = 確定時の最終かな読み` とし、
  §6 のフィルタを満たせば `PostObserveTypo(wrong, correct)`。

### 4-2. 確定直後の打ち直し

確定したテキストを直後に backspace で消し、別入力をやり直したケース。

- 確定成功時に `last_committed_reading_` / `last_committed_surface_` を保存し、
  `keystrokes_since_commit_ = 0` にする。
- `OnKeyDown` 冒頭で、確定直後の **最初のキーが VK_BACK** かつ preedit が空
  （= 確定済みテキストを消す方向）なら、`pre_correction_reading_ =
  last_committed_reading_`、`in_backspace_correction_ = true` を設定。
  以降の打ち直し→確定で §4-1 と同じ比較ロジックが発火する。
- 通常入力が続いた場合は短いウィンドウ（最初の 1 キーのみ）でリセット。

### 4-3. 状態リセット

`Deactivate` / `OnCompositionTerminated` / `OnSetFocus(FALSE)` で
`in_backspace_correction_` / `pre_correction_reading_` /
`keystrokes_since_commit_` をリセットする。フォーカス移動・言語切替を跨いだ
誤学習を防ぐ。

## 5. 蓄積と適用（host: `learning/` + `inference-host/`）

### 5-1. `TypoCorrectionStore`（新規・`learning/`）

- 永続化: TSV `wrong_reading\tcorrect_reading\tcount last_updated_epoch`
  （`LearningStore` の `weight epoch` 区切り規約に合わせる）。
- 既定パス: `%LOCALAPPDATA%\azooKey\typo_corrections.tsv`
  （host CLI `--typo-store` で上書き可）。
- API:
  - `bool Observe(wrong, correct, now_epoch_sec)`
    — §6 のフィルタを満たすペアのみ count++。範囲外は記録せず false。
  - `std::optional<std::string> Lookup(wrong, uint32_t min_count = 3)`
    — count 最大かつ `min_count` 以上の correct を返す。`last_updated` が
    極端に古い（例: 180 日超）レコードは無視。
  - `bool Load(); bool Save() const; void Reset();`
  - `static size_t Utf8EditDistance(a, b); static size_t Utf8CharLength(s);`
    — かな 1 文字 = 3 バイトのため、編集距離はバイト単位ではなく
    **UTF-8 コードポイント単位**で計算する。

### 5-2. `InferenceEngine`（`inference-host/`）

- `EngineConfig` に `typo_correction_mode`、`typo_min_count`（既定 3）、
  編集距離しきい値を追加。
- `TypoCorrectionStore* typo_store_` を非所有で保持（`SetTypoStore`）。
- `ObserveTypo(wrong, correct, now)`: `typo_store_->Observe(...)` → `Save()`。
- 候補取得は新オーバーロード `QueryCandidatesEx` で補正後読みも返す:
  - `struct CandidatesResult { std::vector<core::Candidate> candidates;
    std::string corrected_reading; }`
  - 変換前に `typo_store_->Lookup(kana, typo_min_count)`:
    - **suggest**: 補正読みでも `converter_->Convert` し、候補を
      `debug_info="typo-correction"` でマークして先頭付近へ注入。
      `corrected_reading` は空。
    - **auto_replace**: 補正読みのみで変換し、reranker も補正読みで適用、
      `corrected_reading = 補正読み` を返す。
    - **off / 非ヒット**: 従来動作。
  - 既存 `QueryCandidates` は `QueryCandidatesEx(...).candidates` のラッパに。

### 5-3. preedit への反映（auto_replace）

`auto_replace` で `QueryCandidatesResponse.corrected_reading` が非空のとき、TIP は
`preedit_kana_` を補正後読みへ置換する。TSF のスレッド制約のため非同期 preedit
更新は行わず、MVP では **次のキー入力時 / VK_SPACE 候補表示時に反映** する
（候補ウィンドウには既に補正後候補が出ているため体験上の影響は小さい）。

## 6. 検出ロジックのエッジケース方針

- **編集距離しきい値**: 読み長に対し相対化。`max(1, ceil(len*0.34))` を上限とし、
  絶対上限を 3 程度にする。固定値だと長文ペアで誤学習が増える。
- **最小長**: 2 文字以上のペアのみ学習対象。1 文字読み（「い」→「え」等）は
  単なる候補選択ミスと区別不能。
- **完全一致除外**: `wrong == correct` は学習しない（同じものを打ち直しただけ）。
- **空読み除外**: 確定読みが空なら学習しない（入力放棄の可能性）。
- **頻度しきい値**: `typo_min_count`（既定 3）未満は蓄積のみで適用しない。
  誤学習が混入しても適用されない安全マージン。
- **確定直後打ち直しの誤検出回避**: 「確定直後の最初のキーが VK_BACK」に厳格
  限定し、判定ウィンドウを最初の 1 キーのみに絞る。
- **古いレコード**: `Lookup` 時に `last_updated` が極端に古いレコードは無視。

## 7. IPC プロトコル変更（`ipc/`）

- `MessageType` に `ObserveTypo` を追加（fire-and-forget）。
- 新ペイロード:
  - `ObserveTypoRequest { std::string wrong_reading, correct_reading;
    uint64_t timestamp_ms; }`
  - `ObserveTypoResponse { bool ok; }`（対称性のため定義。応答は送らない）
- `QueryCandidatesResponse` に `std::string corrected_reading` を追加
  （空 = 補正なし。フィールド欠如時は空文字で後方互換）。
- TIP の handshake `capabilities` に `"observe_typo"` を追加。

## 8. 設定（`settings/`）

`settings/mvp-settings.schema.json` に文書化のみ追加する（ローダー実装は将来課題）。

```json
"typoCorrectionMode": {
  "type": "string",
  "enum": ["off", "suggest", "auto_replace"],
  "default": "suggest",
  "description": "個人のタイプミス傾向を学習し修正する動作モード。実効値は当面 host の --typo-mode 引数 / 環境変数 AZOOKEY_TYPO_MODE。設定ローダー実装は将来課題。"
}
```

## 9. テスト計画

- 新規 `learning/tests/typo_correction_store_test.cpp`:
  Observe/Lookup、しきい値未満で `nullopt`、編集距離超過ペアの拒否、
  UTF-8 編集距離（かな）、Save→Load ラウンドトリップ、Reset。
  `learning/tests/CMakeLists.txt` に登録。
- `ipc/tests/payloads_test.cpp`: `ObserveTypoRequest` Build→Parse 往復、
  `QueryCandidatesResponse.corrected_reading` 往復、欠如時の後方互換。
- `ipc/tests/messages_test.cpp`: `ObserveTypo` の `TypeToString`/`TypeFromString`。
- `inference-host/tests/engine_test.cpp`: ObserveTypo 後の suggest 注入 /
  auto_replace の `corrected_reading` / off 無変化 / しきい値未満で非適用。
- `inference-host/tests/dispatcher_test.cpp`: `ObserveTypo` で応答なし
  （`std::nullopt`）かつストア更新、`QueryCandidates` 応答に `corrected_reading`。

## 10. 変更対象ファイル一覧

| 区分 | ファイル |
|---|---|
| 新規 | `learning/include/azookey/learning/TypoCorrectionStore.h` |
| 新規 | `learning/src/TypoCorrectionStore.cpp` |
| 新規 | `learning/tests/typo_correction_store_test.cpp` |
| 編集 | `learning/CMakeLists.txt`, `learning/tests/CMakeLists.txt` |
| 編集 | `ipc/include/azookey/ipc/Messages.h`, `ipc/src/Messages.cpp` |
| 編集 | `ipc/include/azookey/ipc/Payloads.h`, `ipc/src/Payloads.cpp` |
| 編集 | `inference-host/include/azookey/host/InferenceEngine.h`, `src/InferenceEngine.cpp` |
| 編集 | `inference-host/include/azookey/host/Dispatcher.h`, `src/Dispatcher.cpp` |
| 編集 | `inference-host/src/main.cpp` |
| 編集 | `tsf-tip/include/azookey/tsf/TextService.h`, `src/TextService.cpp` |
| 編集 | `settings/mvp-settings.schema.json` |

## 11. 検証手順（実装後）

1. ビルド: `cmake --preset windows-debug -DAZOOKEY_FETCH_GOOGLETEST=ON && cmake --build --preset windows-debug`
2. テスト: `ctest --preset windows-debug --output-on-failure`
   （`typo_correction_store_tests` / `payloads_test` / `engine_test` /
   `dispatcher_test` が green であること）
3. host を `--typo-mode suggest` / `auto_replace` で stdio 起動し、同一
   `wrong_reading` の `ObserveTypo` を 3 回送ってから `QueryCandidates` を投げ、
   suggest で `typo-correction` 候補が注入され、auto_replace で
   `corrected_reading` が返ることを確認。
4. Windows 実機があれば TIP を導入し `AZOOKEY_TYPO_MODE` を設定して、未確定中
   backspace 訂正・確定直後打ち直しを実操作で確認。実機が無い場合はその旨を
   明示し、自動テストとプロトコルレベル確認で代替する。

---

## 12. M55 追補: 統合補正エンジン（v2）

本章は M55 で導入する v2 仕様。M35（v1）が `wrong_reading →
correct_reading` の頻度ペア学習だったのに対し、v2 は以下を統合する:

- 入力 hypothesis 生成（Weighted Edit Graph + Keyboard Adjacency +
  Romaji Variant + Dictionary/Context Constraint）
- 4 モード（off / suggest / rank / aggressive）
- TypoLearningStore による pattern 学習（accept_weight=0.25 /
  reject_weight=0.45）
- 11 種の `typo_type` 対応
- raw_keys を IPC で受領（プライバシー処理）

v1 の `wrong_reading → correct_reading` ペア学習は v2 でも下位互換
として残す。マイグレート時は §12.8 の `typo_patterns` テーブル
（`id INTEGER PRIMARY KEY`）に **legacy 集約用の予約行 1 行**
（`typo_type = 'legacy_v1'`, `observed_pattern = ''`, `intended_pattern = ''`）
を INSERT し、得られた整数 `id` を全 v1 由来 `typo_events.pattern_id`
（INTEGER）に書き込む。`pattern_id` の文字列値（例: `"v1_legacy"`）は
JSON 候補メタデータ（§12.2 の `pattern_id` フィールド、TEXT）でのみ使い、
SQL の INTEGER 列とは混在させない。

### 12.1 アーキテクチャ

```
raw_keys / observed_reading
  ↓
InputNormalizer
  ↓
TypoCorrectionEngine        ← M55 で新設
  ↓
ReadingHypotheses
  ├─ original_reading
  ├─ corrected_reading_1
  ├─ corrected_reading_2
  └─ corrected_reading_3
  ↓
CandidateGenerator
  ├─ Zenzai
  ├─ Dictionary
  └─ UserDictionary
  ↓
CandidateMerger
  ↓
ScoringPipeline
```

`TypoCorrectionEngine` は CandidateGenerator の **前段** に置く。
複数の reading hypothesis を生成し、それぞれに対して候補を求めて
merge する。

### 12.2 ReadingHypothesis

```json
{
  "reading": "こうしょう",
  "source": "typo_correction",
  "confidence": 0.86,
  "typo_type": "adjacent_key",
  "pattern_id": "j_insert_after_u",
  "edit_distance": 1,
  "personalized": true
}
```

| フィールド | 内容 |
|---|---|
| `reading` | 補正後の読み |
| `source` | `original` / `typo_correction` |
| `confidence` | 0.0〜1.0 |
| `typo_type` | §12.3 の 11 種 |
| `pattern_id` | TypoLearningStore の pattern_id |
| `edit_distance` | observed との編集距離 |
| `personalized` | 個人パターンか汎用ルールか |

### 12.3 対象 typo_type（11 種）

| `typo_type` | 内容 | 例 |
|---|---|---|
| `adjacent_key` | 隣のキーを押した | `koujsyou` → `kousyou` |
| `missing_char` | 文字抜け | `kousho` → `koushou` |
| `extra_char` | 余分な文字 | `kouhsyou` → `kousyou` |
| `transposition` | 文字順入れ替わり | `ksouyou` → `kousyou` |
| `double_key` | 同じキーを重複 | `koussyo` → `kousyo` |
| `romaji_variant` | ローマ字表記揺れ | `syou` / `shou` / `syo` |
| `n_handling` | んの入力ミス | `kani` / `kanni` / `kan'i` |
| `small_tsu` | 促音の入力ミス | `kita` / `kitta` |
| `long_vowel` | 長音・母音揺れ | `ou` / `oo` / `o` |
| `dakuten_confusion` | 濁点・半濁点のミス | `かいしや` / `がいしゃ` |
| `kana_shape_confusion` | かな形状・入力癖 | `しや` → `しゃ` |

### 12.4 補正候補生成アルゴリズム

#### 12.4.1 Weighted Edit Graph

ローマ字列または読み列に対して重み付き編集距離を使う:

```
cost =
  insertion_cost
+ deletion_cost
+ substitution_cost
+ transposition_cost
+ personalized_pattern_cost
```

個人がよく行う誤りは `personalized_pattern_cost` を下げる。

例:

```
ユーザーがよく j を余分に打つ
  koujsyou → kousyou
  personalized_pattern_cost: low
```

#### 12.4.2 Keyboard Adjacency Model

JIS / US 配列を考慮して隣接キー誤打鍵を低コストにする:

```
u の隣に i / y / j がある
s の隣に a / d / w / x がある
```

`settings.typoCorrection.keyboardLayout`（`auto` / `jis` / `us`）で
切替。

#### 12.4.3 Romaji Variant Normalizer

| variant | canonical |
|---|---|
| `si` | `shi` |
| `ti` | `chi` |
| `tu` | `tsu` |
| `syo` | `sho` |
| `syou` | `shou` |
| `zya` | `ja` |
| `jya` | `ja` |
| `nn` | `n` |

ただし全部を一律補正すると誤爆が増えるため、個人設定・辞書ヒット・
文脈スコアと組み合わせて判定する。

#### 12.4.4 Dictionary-Constrained Correction

補正後の読みが辞書・Zenzai・ユーザー辞書いずれにも候補を持たない場合
は **棄却** する:

```
observed_reading
  ↓ typo correction
corrected_reading
  ↓ dictionary lookup（M53）
候補あり → 採用候補
候補なし → 棄却
```

#### 12.4.5 Context-Constrained Correction

補正候補の surface が文脈に合わない場合は減点する:

```
left_context: 明日の会議では価格について
observed: こうじゃう
corrected: こうしょう
candidate: 交渉
```

文脈上「交渉」が高スコアなら補正候補を上げる。M57 ModernBERT が有効
なら文脈自然度スコアを利用する。

### 12.5 発動条件

打ち間違え補正は常時強制しない。発動判定は **正規化済みスコア**で行う。
`s1 = score_top1_original`、`s2 = score_top2_original` を、元読みの変換候補
スコアを top1 で割って `[0, 1]` に正規化した値とする（`s1 = 1.0` 固定、
`s2 ∈ [0, 1]`）。`dict_best` は元読みの最良辞書 / Zenzai ヒットの正規化スコア
（ヒットなしは 0）。

```
activate_typo_correction(reading, observed_pattern):
  if Utf8CharLength(reading) <= LEN_MIN        → false   // §12.11。既定 2 = 2 文字以下は補正しない
  weak       = (dict_best < S_WEAK)            // 元候補が弱い。既定 0.40
  small_gap  = ((1.0 - s2) < G_GAP)            // top1/top2 が拮抗。既定 0.15
  no_dict    = (dict_best == 0)                // 辞書ヒットなし
  strong_pat = (personal_pattern_confidence(observed_pattern)
                 >= minConfidenceForRanking)   // 既定 0.70（§12.14）
  return weak or small_gap or no_dict or strong_pat
```

- `S_WEAK` / `G_GAP` は初期値であり M52（`typo_false_positive_rate`）で校正
  する（§12.16）。`strong_pat` のしきい値は設定 `minConfidenceForRanking`
  （§12.14）と同一値を使い、二重定義しない。
- 発動しても補正候補が第一候補になるとは限らない。順位は §12.10 のスコアと
  §12.11 の誤補正防止条件で決まる。発動は「補正 hypothesis を **生成する**
  か」だけを決める前段ゲートである。

### 12.6 補正モード（v2）

v1 の 3 モード（off / suggest / auto_replace）を **4 モード** に拡張:

| mode | 動作 |
|---|---|
| `off` | 打ち間違え補正を無効（v1 と互換） |
| `suggest` | 「もしかして」候補として提示（v1 互換、既定） |
| `rank` | 通常候補に混ぜるが控えめに加点（新規） |
| `aggressive` | 高信頼時のみ第一候補まで上げる（新規） |

初期設定は `suggest` または `rank` を推奨。`aggressive` は誤爆リスクが
あるため設定アプリで明示的に有効化する。

v1 の `auto_replace` モードは v2 の `aggressive` 相当として読み替える
（後方互換）。

### 12.7 表示仕様

| 条件 | 表示 |
|---|---|
| 初回補正 | ラベル表示「もしかして: ...」 |
| 同じ補正を複数回採用済み | ラベル省略可 |
| 高信頼補正 | 通常候補として自然に表示 |
| 低信頼補正 | 下位に「もしかして」として表示 |

```
1 交渉      もしかして: こうしょう
2 校章
3 高尚
```

### 12.8 TypoLearningStore（v2）

`last_used_at` / `created_at` / `updated_at` はいずれも epoch 秒
（INTEGER、ミリ秒ではない。`docs/user-learning-enhancement-spec.md` §3.1
の `LearningStore.cpp` 単位と整合）。

#### typo_patterns

```sql
CREATE TABLE typo_patterns (
  id INTEGER PRIMARY KEY,
  observed_pattern TEXT NOT NULL,
  intended_pattern TEXT NOT NULL,
  typo_type TEXT NOT NULL,
  keyboard_layout TEXT,
  confidence REAL DEFAULT 0.5,
  accept_count INTEGER DEFAULT 0,
  reject_count INTEGER DEFAULT 0,
  last_used_at INTEGER,
  created_at INTEGER NOT NULL,
  updated_at INTEGER NOT NULL
);
```

#### typo_events

```sql
CREATE TABLE typo_events (
  id INTEGER PRIMARY KEY,
  observed_reading TEXT NOT NULL,
  corrected_reading TEXT NOT NULL,
  selected_surface TEXT,
  typo_type TEXT,
  pattern_id INTEGER,
  app_name TEXT,
  left_context_hash TEXT,
  event_type TEXT NOT NULL,
  created_at INTEGER NOT NULL
);
```

#### typo_settings

```sql
CREATE TABLE typo_settings (
  key TEXT PRIMARY KEY,
  value TEXT NOT NULL,
  updated_at INTEGER NOT NULL
);
```

実装は §3.1（M54）と同様に TSV 拡張で先行し、SQLite 化は将来 M に分離
する。v1 の `typo_corrections.tsv` から自動マイグレートする。

### 12.9 信頼度更新

`confidence` は logit 空間で線形に積み上げ、sigmoid で `(0, 1)` に写してから
`[min_confidence, max_confidence]` にクランプする。各項は logit（対数オッズ）
単位で定義する。

```
days_idle    = max(0, (now_epoch_sec - last_used_at) / 86400)
recency_bonus = B_rec * (2 * exp(-ln(2) * days_idle / recency_half_life) - 1)
                // 値域 [-B_rec, +B_rec]。直近利用で +、放置で −。
                // recency_half_life は真の半減期: days_idle == recency_half_life
                // で exp 項が 0.5 になり recency_bonus = 0（中立）。ln(2) 係数を
                // 省くと半減期にならない（M54 §5 と同じ正規化）。
app_specific_bonus =
    B_app   if app_aware_learning_enabled
            and current_app == dominant_app(pattern)   // accept 多数派 app
    else 0

raw_conf   = sigmoid(
               base
             + accept_count * accept_weight
             - reject_count * reject_weight
             + recency_bonus
             + app_specific_bonus
             )
confidence = clamp(raw_conf, min_confidence, max_confidence)
```

初期パラメータ:

| パラメータ | 値 | 意味 / 根拠 |
|---|---:|---|
| `base` | 0.0 | logit(0.5)。実績ゼロの新規パターンは中立 0.5 から始める。 |
| `accept_weight` | 0.25 | logit 増分。採用約 4 回で発動しきい値 `minConfidenceForRanking`（0.70 ≈ logit 0.847）に到達。`typo_min_count`（§5、既定 3）と整合する慎重さ。 |
| `reject_weight` | 0.45 | 拒否の logit 減分。採用の約 1.8 倍。**拒否を採用より強く**し、1 回の誤補正拒否を 2 回弱の採用で相殺する。 |
| `recency_half_life` | 60 日 | `recency_bonus` の減衰時定数。`docs/user-learning-enhancement-spec.md` §5（打ち間違えパターン 60 日）と一致させる。 |
| `B_rec` | 0.30 | `recency_bonus` の振幅（logit）。放置パターンを最大 0.30 減点。 |
| `B_app` | 0.20 | app 一致時の小さな加点（logit）。app をまたいだ過剰一般化を防ぐ弱さ。 |
| `max_confidence` | 0.95 | 上限。常に「もしかして」で覆せる余地を残す。 |
| `min_confidence` | 0.05 | 下限。完全な 0 にせず再学習の余地を残す。 |

- **拒否の重みを採用より強くする**。誤補正は IME 体験を大きく壊すため、学習は
  保守的に行う。新規パターン（accept=reject=0）の `raw_conf` は recency/app 項
  のみで約 0.5 になるが、発動ゲート `minConfidenceForRanking`（0.70）に届かない
  ため、**約 3〜4 回の採用実績を経るまで rank に混ざらない**。これが
  `typo_min_count` と二重の安全マージンになる。
- `dominant_app(pattern)` は当該パターンの `typo_events`（§12.8）で
  `event_type='typo_accept'` が最多の `app_name`。同数・該当なしは bonus 0。
- `accept_weight` / `reject_weight` / `B_rec` / `B_app` は初期値であり M52 で
  校正する（§12.16）。`reject_weight > accept_weight` の不変条件は固定する。

### 12.10 スコアリング

補正候補のスコアは、`[0, 1]` 値域の因子の積から `overcorrection_penalty`
（加法・`[0, 1]`）を引いて求める。各因子は **1.0 を中立**とし、欠ける情報
（M48 app / M57 context など未完了）は 1.0 にフォールバックする。

```
app_factor_typo = min(1.0, app_profile_weight)   // M54 §6.1 を [0,1] に丸める（下記）

typo_score = clamp(
    typo_confidence            // §12.9。[min_confidence, max_confidence]
  × dictionary_hit_score       // 補正後読みの最良候補の正規化スコア。ヒットなしは §12.4.4 で棄却済
  × context_score              // M57 ModernBERT 文脈自然度 [0,1]。M57 未使用時は 1.0
  × app_factor_typo            // [0,1]。同 app/不明=1.0、別 app=W_diff(0.8)。M48 未完了時は 1.0
  × reading_similarity_score   // = 1 - normalized_edit_distance(observed, corrected)
  - overcorrection_penalty,    // 下記
    0.0, 1.0)

overcorrection_penalty =
    P_reject     if net_reject(pattern) >= 1                  // 既定 0.50。過去に拒否
  + P_strong_top1 * max(0, s1_strength - S_STRONG)           // 既定 P=0.40, S_STRONG=0.85
  + P_lowconf    * max(0, minConfidenceForRanking - typo_confidence)  // 既定 0.50
```

- `app_factor_typo` は M54 §6.1 の `app_profile_weight`（同 app 1.2 / 別 app
  0.8 / 不明 1.0）を `min(1.0, ·)` で `[0, 1]` に丸めた typo 専用因子。**補正
  スコアでは app 一致を加点しない**（同 app でも 1.0 = 中立にとどめ、別 app は
  0.8 に減点）。M54 の `user_score`（純粋な乗算合成で 1.2 倍まで許す）と異なり、
  ここで 1.2 を許すと §12.10 冒頭の「因子は `[0,1]`・1.0 中立」契約を破り、
  clamp 前に app 一致が補正候補を第一候補へ押し上げる隠れ boost になる。
  §12.11 の「app 一致だけで top 化させない」誤補正防止方針と整合させるため、
  上限を 1.0 に固定する。
- `net_reject(pattern) = max(0, reject_count - accept_count)`（M54 §6.2 と
  同じ純拒否の考え方）。
- `s1_strength` は **元読み top1 の絶対的な確からしさ** `[0, 1]` で、元 top1 の
  `dictionary_hit_score × context_score` で定義する。§12.5 の `s1`（top1 で
  正規化したため常に 1.0）とは別物で、こちらは「元 top1 がどれだけ強いか」を
  測る。M57 文脈スコア未使用時は `context_score = 1.0` フォールバック。
- 係数 `P_*` / `S_STRONG` は初期値であり M52（`typo_overcorrection_rate`）で
  校正する（§12.16）。

### 12.11 誤補正防止条件

§12.10 の `typo_score` に加え、**順位の上限（rank cap）** を次の閾値で課す。
補正候補を第一候補に昇格できるのは `aggressive` モードかつ全ガードを満たす
ときのみとする。

| 条件（しきい値） | 処理 |
|---|---|
| 元 top1 が十分強い（`s1_strength >= S_STRONG`、既定 0.85） | 補正候補を元 top1 より上にしない（rank cap = 2 位以下） |
| 補正 confidence が低い（`typo_confidence < minConfidenceForRanking`、0.70） | 「もしかして」枠のみ（rank 非混在） |
| top 候補化の信頼不足（`typo_score < minConfidenceForTopCandidate`、0.90） | 第一候補にしない |
| ユーザーが過去に拒否（`net_reject >= 1`） | `overcorrection_penalty` で強く減点（§12.10）し top 化不可 |
| 入力が短すぎる（`Utf8CharLength(reading) <= LEN_MIN`、既定 2 = 2 文字以下） | 補正しない（§12.5 で生成前に遮断） |
| パスワード欄・秘匿アプリ（M46） | 補正・学習ともに無効（§12.12.1 のゲートで遮断） |
| コード入力中（M48 profile = code） | 英字・ローマ字補正を控えめにする（発動ゲートに `code` 抑制を加味） |

- `minConfidenceForRanking`（0.70）/ `minConfidenceForTopCandidate`（0.90）は
  設定スキーマ §12.14 の同名フィールドと一致させ、ここで再定義しない。
- モード別の rank cap: `suggest` = 常に「もしかして」枠（元候補を一切上書き
  しない）、`rank` = `typo_confidence >= 0.70` で通常候補に混在（ただし元 top1
  が強ければ 2 位以下）、`aggressive` = 全ガード通過時のみ第一候補化。これは
  M52 で `--typo-mode rank`（FP 率）と `--typo-mode aggressive`（overcorrection
  率）を別管理する受け入れ基準（§12.15）と対応する。

### 12.12 プライバシー（v2）

| 項目 | 仕様 |
|---|---|
| `raw_keys` | 原則長期保存しない、抽象化パターンに変換して保存 |
| `typo pattern` | 抽象化して保存（`j_insert_after_u` 等） |
| `left_context` | hash 保存を標準（SHA-256 上位 4 bytes） |
| `app_name` | 保存 ON/OFF 可能 |
| 学習停止 | 必須（`enabled = false` でストア更新せず） |
| 全削除 | 必須（M49 と連携） |
| エクスポート | JSON / CSV（M49 と連携） |
| secret apps | 補正・学習ともに無効（M46。§12.12.2） |

#### 12.12.1 `raw_keys` のパターン抽象化スキーム

`raw_keys`（生の打鍵列）は **メモリ上で抽象化トークンへ変換した直後に破棄し、
永続化・ログ出力しない**。`typo_patterns.observed_pattern` /
`intended_pattern`（§12.8）に保存するのは原文ではなく抽象化トークンである。

抽象化は §12.4.1 Weighted Edit Graph が出力する編集操作列から構成する:

```
align(observed, intended) → 編集操作の列（各操作は 1 文字単位）
各操作を次のトークンに写す（prev は直前の確定文字、なければ '^'）:
  insertion(c, prev)      → "<c>_insert_after_<prev>"   例 "j_insert_after_u"
  deletion(c, prev)       → "<c>_delete_after_<prev>"
  substitution(a → b)     → "<a>_sub_<b>"               例 "z_sub_x"
  transposition(a, b)     → "<a><b>_swap"               例 "ts_swap"
observed_pattern  = 上記トークンを連結（複数編集時は '+' 区切り）
intended_pattern  = 対応する canonical 形（Romaji Variant 正規化後、§12.4.3）
```

プライバシー上の不変条件:

- トークンは **1 文字 + 直前 1 文字の局所コンテキスト**のみを含み、単語全体や
  原文の読みを復元できない。
- 抽象化アルファベットは `[a-z]` 打鍵・かな・ローマ字正規化記号に限定する。
  この集合外の文字（数字・記号・貼り付け由来の任意文字列）を含む `raw_keys`
  は **抽象化せず破棄**し、パターン化しない。これによりパスワードや貼り付け
  文字列を誤って取り込まない。
- `left_context` は §8 と同じく SHA-256 上位 4 bytes の hash で保存する
  （`docs/user-learning-enhancement-spec.md` §8.1 と同一算出式）。

#### 12.12.2 M46 secure 抑止との連携タイミング

secure（パスワード欄・秘匿アプリ）抑止は **検出時点で評価する fail-closed**
とし、TIP（検出）と host（蓄積・適用）の二段で遮断する。M46 `PrivacyGate` の
判定主体は前面フォーカスを見られる TIP 側である。

1. **TIP（検出・送信ゲート、一次）**: `ObserveTypo` の発火条件（§4-1 / §4-2）
   を満たしても、その時点の M46 `PrivacyGate` が secure を返すなら
   `pre_correction_reading_` のスナップショットを取らず、`ObserveTypo` も
   `raw_keys` 送信も行わない。判定は **イベント発生時（OnKeyDown / commit）**
   に行い、flush 時ではない。
2. **フォーカス遷移時のクリア**: secure コンテキストへ遷移した瞬間、TIP は
   §4-3 のリセット（`pre_correction_reading_` 等）を実行する。これにより
   non-secure 中に取ったスナップショットが secure 確定に巻き込まれて漏れる
   こと、およびその逆を防ぐ。
3. **`raw_keys` 同梱の抑止**: secure 中の `QueryCandidates` では TIP は
   `raw_keys` を省略し、`typo_correction_mode` を実効 `off` として送る
   （§12.13 の optional フィールド省略 = v1 fallback）。
4. **host（適用・蓄積ゲート、二次・fail-closed）**: host は窓を見られないため
   TIP を信頼するが、防御的二重化として、直近 `QueryCandidates` の `secure`
   フラグ（§12.13 で M55 が追加する bool。`PrivacyGate::IsSecure()` の IPC
   表出）が secure を示すセッションでは `ObserveTypo` を記録せず `Lookup`
   （適用）も行わない。`secure` が **未指定（未知）のとき**は、M46 の
   「解決不能な privacy 状態は secure 扱い」契約（`privacy-and-secure-input-spec.md`
   §4.3）に従い **fail-closed（deny: 補正・学習を抑止）を既定**とする。
   - これを正常入力のブロックなく成立させるため、privacy 対応 TIP は
     handshake `capabilities` に `"secure_flag"` を広告し（§7）、**毎回の
     `QueryCandidates` に `secure`（secure 時 true / 非 secure 時 false）を
     必ず載せる**（§12.13）。`secure_flag` を広告した TIP からのリクエストで
     `secure` が欠落することは無く、欠落＝privacy 非対応 TIP（古い TIP・未配線）
     と判定できるため、その場合に deny へ倒しても通常入力を巻き込まない。
   - 例外として、secure 検出が存在しない M46 未完了の暫定期間に限り、
     host は `--secure-unknown=allow` を明示指定して未知を allow に倒せる
     （`secure_flag` 未広告 TIP のみが対象）。M46 完了・`secure_flag` 配線後は
     既定の `deny` で運用し、§12.15「secure 中は補正・学習が一切発生しない」を
     検証する。`--secure-unknown` の既定は **`deny`** とする。

### 12.13 IPC（v2 拡張）

v1 の `ObserveTypo` IPC に加え、既存 `QueryCandidates` の payload に
optional フィールドを追加（エンベロープ schema 自体は変更しない）:

```json
{
  "version": 1,
  "request_id": 300,
  "type": "QueryCandidates",
  "trace_id": "018fd2c2-...",
  "payload": {
    "reading": "こうしょう",
    "raw_keys": "kousyou",
    "left_context": "...",
    "app": {},
    "typo_correction_mode": "rank",
    "secure": false
  }
}
```

`raw_keys` / `typo_correction_mode` は optional（TIP が取得可能・送信意図が
あるときのみ送る）。`MessageType` は既存 `QueryCandidates` のまま再利用し、
新規 enum 値は追加しない。M40 互換性ルールに従い、**`raw_keys` /
`typo_correction_mode` が**未指定のときは v1 動作に fallback する。

> **`secure` は v1 fallback の対象外**。上記の「未指定 ⇒ v1 動作」ルールは
> `secure` には適用しない。`secure` の欠落は v1 互換の allow ではなく、
> §12.12.2-4 の fail-closed 規約に従い **deny（補正・学習を抑止）** として
> 扱う（既定 `--secure-unknown=deny`）。privacy は後方互換より優先する。

- `secure`（bool）は TIP 側 M46 `PrivacyGate::IsSecure()`
  （`docs/privacy-and-secure-input-spec.md` §5）の IPC 表出であり、host 二次
  ゲート（§12.12.2-4）が参照する唯一の secure シグナルである。**現行
  `QueryCandidatesRequest` には secure 相当フィールドが無いため、本フィールドは
  M55 が追加する。** M46 が同等のリクエスト単位 privacy フィールド（例
  `privacy_mode`）を別途定義する場合は、二重定義せず M46 のフィールドへ寄せて
  本フィールドを廃止する（その場合 §12.12.2 の参照先も M46 フィールドへ更新）。
- **フィールド存在規約**: `secure` は wire 上は optional だが、handshake
  `capabilities` に `"secure_flag"` を広告する TIP（= M46 配線済み）は
  **毎リクエストで `secure` を必ず送る**（secure 時 true / 非 secure 時 false）。
  欠落は `secure_flag` 非広告 TIP（古い TIP・未配線）を意味し、host は
  §12.12.2-4 のとおり既定 `deny`（fail-closed）で扱う。`raw_keys` /
  `typo_correction_mode` の「送信意図があるときのみ」とは規約が異なる点に注意。
- secure 中は §12.12.2-3 のとおり TIP が `raw_keys` を省略し
  `typo_correction_mode` を実効 `off` で送るが、`secure: true` は明示的な
  fail-closed シグナルとして併送し、host が未配線時にフォールバック解釈で
  漏れることを防ぐ。

### 12.14 設定スキーマ拡張

v2 は nested `typoCorrection.*` に集約する:

```json
{
  "typoCorrection": {
    "enabled": true,
    "mode": "suggest",
    "learnPersonalPatterns": true,
    "keyboardLayout": "auto",
    "maxReadingHypotheses": 3,
    "minConfidenceForRanking": 0.70,
    "minConfidenceForTopCandidate": 0.90,
    "disableInSecretApps": true
  }
}
```

**v1 互換性**: M55 リリース時点で v1 設定 `typoCorrectionMode`（root レベル、
3 値 enum）を保持しているユーザーが存在する。SettingsManager は読み込み時に
以下の migration を実行する:

1. v2 の `typoCorrection.mode` が存在すればそれを採用
2. v2 が未設定 / 空で、v1 root `typoCorrectionMode` が存在すれば値を読み、
   `auto_replace` → `aggressive` に読み替え（§12.6 と整合）し、
   `typoCorrection.mode` へコピー
3. 両方未設定なら既定値 `suggest`

migration 後は v1 key を残したまま（破壊的削除はしない）両方を書き出し、
将来 3 マイナーバージョン後に v1 root key を削除予定（deprecation
warning を CHANGELOG に記載）。M48 §6 の `promptPrefixByApp` と同じ
段階的廃止ポリシー。

### 12.15 M55 受け入れ条件

typo 補正指標は補正有効モードで採取する（`conversion-quality-benchmark-spec.md`
§7・§14。`--typo-mode off` での値は受け入れに用いない。baseline はモード別管理）。

- M52 ベンチ（`--typo-mode rank`）で `typo_correction_top5_accuracy` が 85% 以上
- M52 ベンチ（`--typo-mode rank`）で `typo_false_positive_rate` が 1% 未満
- M52 ベンチ（`--typo-mode aggressive`）で `typo_overcorrection_rate` が 0.5% 未満
- M35 の既存 `typo_corrections.tsv` から自動マイグレートできる
- M46 secure 中は補正・学習が一切発生しない
- v1 互換: `mode = off / suggest` の挙動が v1 と等価
- v1 の `mode = auto_replace` が v2 の `aggressive` として読み替え
  られる

### 12.16 補正定数と M52 校正の対応

§12.5 / §12.9 / §12.10 / §12.11 の定数は **式の形を本書で確定し、係数は初期値
として与える**。係数は M52 ベンチ（`conversion-quality-benchmark-spec.md`）の
モード別指標で校正する。`--typo-mode off` の値は校正にも受け入れにも用いない
（§12.15）。

| 定数 | 既定 | 不変条件（校正で破ってはならない） | 校正に用いる指標（モード） |
|---|---:|---|---|
| `accept_weight`（§12.9） | 0.25 | `reject_weight > accept_weight` | `typo_correction_top5_accuracy`（rank） |
| `reject_weight`（§12.9） | 0.45 | 同上 | `typo_false_positive_rate`（rank） |
| `B_rec` / `recency_half_life`（§12.9） | 0.30 / 60 日 | `recency_half_life` は `user-learning-enhancement-spec.md` §5（typo 60 日）と一致 | top5 accuracy（rank） |
| `B_app`（§12.9） | 0.20 | `0 <= B_app < accept_weight×3` | app 別 FP 率（rank） |
| `S_WEAK`（§12.5） | 0.40 | `0 < S_WEAK < 1` | `typo_false_positive_rate`（rank） |
| `G_GAP`（§12.5） | 0.15 | `0 < G_GAP < 1` | 同上 |
| `LEN_MIN`（§12.5/§12.11） | 2 | `LEN_MIN >= 2`。`Utf8CharLength(reading) <= LEN_MIN` を遮断するため 2 文字以下は補正しない（文字数 = UTF-8 コードポイント、§5-1） | FP 率（rank） |
| `P_reject`（§12.10） | 0.50 | 拒否済みパターンを top 化させない強さ | `typo_overcorrection_rate`（aggressive） |
| `P_strong_top1` / `S_STRONG`（§12.10/§12.11） | 0.40 / 0.85 | `S_STRONG` は強い元 top1 の下限 | `typo_overcorrection_rate`（aggressive） |
| `P_lowconf`（§12.10） | 0.50 | — | overcorrection 率（aggressive） |
| `minConfidenceForRanking`（§12.14） | 0.70 | `< minConfidenceForTopCandidate` | FP 率（rank） |
| `minConfidenceForTopCandidate`（§12.14） | 0.90 | `> minConfidenceForRanking` | overcorrection 率（aggressive） |

- **受け入れ判定（§12.15）の指標と閾値**: `typo_correction_top5_accuracy`
  ≥ 85%（rank）/ `typo_false_positive_rate` < 1%（rank）/
  `typo_overcorrection_rate` < 0.5%（aggressive）。
- 校正は係数の数値のみを動かし、本表の不変条件・式の形・モード別の rank cap
  （§12.11）は固定する。SQLite/TSV スキーマ（§12.8）を変える結果が出た場合は
  M55 範囲外とし別 M で扱う。
