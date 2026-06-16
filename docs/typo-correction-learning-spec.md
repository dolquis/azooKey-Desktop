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

打ち間違え補正は常時強制しない:

```
if original_candidates are weak
or top1/top2 score gap is small
or original_reading has no good dictionary hit
or observed pattern matches high-confidence personal typo pattern
then generate typo-corrected reading hypotheses
```

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

```
confidence = sigmoid(
  base
  + accept_count * accept_weight
  - reject_count * reject_weight
  + recency_bonus
  + app_specific_bonus
)
```

初期パラメータ:

| パラメータ | 値 |
|---|---:|
| `accept_weight` | 0.25 |
| `reject_weight` | 0.45 |
| `recency_half_life` | 60 日 |
| `max_confidence` | 0.95 |
| `min_confidence` | 0.05 |

**拒否の重みを採用より強くする**。誤補正は IME 体験を大きく壊すため、
学習は保守的に行う。

### 12.10 スコアリング

```
typo_score =
  typo_confidence
  × dictionary_hit_score
  × context_score
  × app_profile_weight
  × reading_similarity_score
  - overcorrection_penalty
```

### 12.11 誤補正防止条件

以下の場合は補正候補を第一候補にしない:

| 条件 | 処理 |
|---|---|
| 通常候補の top1 が十分強い | 補正候補は下位または非表示 |
| 補正 confidence が低い | 「もしかして」枠のみ |
| ユーザーが過去に拒否した | 強く減点 |
| 入力が短すぎる（2 文字以下） | 補正しない |
| パスワード欄・秘匿アプリ（M46） | 補正・学習ともに無効 |
| コード入力中（M48 profile） | 英字補正は控えめ |

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
| secret apps | 補正・学習ともに無効（M46） |

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
    "typo_correction_mode": "rank"
  }
}
```

`raw_keys` / `typo_correction_mode` は optional（TIP が取得可能・送信意図が
あるときのみ送る）。`MessageType` は既存 `QueryCandidates` のまま再利用し、
新規 enum 値は追加しない。M40 互換性ルールに従い、optional フィールドが
未指定のときは v1 動作に fallback する。

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

- M52 ベンチで `typo_correction_top5_accuracy` が 85% 以上
- M52 ベンチで `typo_false_positive_rate` が 1% 未満
- M52 ベンチで `typo_overcorrection_rate` が 0.5% 未満
- M35 の既存 `typo_corrections.tsv` から自動マイグレートできる
- M46 secure 中は補正・学習が一切発生しない
- v1 互換: `mode = off / suggest` の挙動が v1 と等価
- v1 の `mode = auto_replace` が v2 の `aggressive` として読み替え
  られる
