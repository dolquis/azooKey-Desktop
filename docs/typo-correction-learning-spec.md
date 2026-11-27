---
kind: spec
milestones: [M35, M55]
canonical_for: ["v1 基本タイプミス学習", "v2 統合補正エンジン"]
---

# 個人タイプミス学習・自動修正 仕様（Typo Correction Learning）

本書は「ユーザー自身がよくする打ち間違い（タイプミス）を学習し、同じタイプミスを
したときに正しい入力へ変換・提示する」機能の仕様を定める。対象は **Windows 版
（C++ 移植: `core/` `learning/` `ipc/` `inference-host/` `tsf-tip/`）** のみ。
macOS 版 `legacy/` は対象外。

関連: `docs/ai-backend-spec.md` §4.3（Post-Commit Lint）、
      `docs/user-learning-enhancement-spec.md` §4.2（訂正の学習）、
      `docs/conversion-quality-benchmark-spec.md`（M52）、
      `docs/user-learning-enhancement-spec.md`（M54）、
      `docs/privacy-and-secure-input-spec.md`（M46）。

## 0. 節とマイルストーンの対応

本書の節と、その範囲を定める `plans/windows-port-roadmap.md` のマイルストーンの対応を次に示す。

| 節 | マイルストーン |
|---|---|
| §1〜§11 | M35（v1: 基本タイプミス学習） |
| §12 | M55（v2: 統合補正エンジン） |
| §12.4.6 | M17（`FuzzyMatch`。M17 の横断項目） |

§12 は M55 の追補章で、v1 の機能を発展させ、ReadingHypothesis 経由で CandidateGenerator 前段に
補正を組み込む統合エンジンに昇格する。v1 の `wrong_reading → correct_reading`
ペア学習は v2 でも下位互換として維持する。

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

設定 `typoCorrectionMode`（3 値）で切り替える。M55 の `typoCorrection.mode`（§12.14）があればそちらを使い、
v2 の 4 モードと本表の 3 値の対応は §12.14 の「engine への反映」が定める。

| 値 | 学習 | 適用 |
|---|---|---|
| `off` | しない | しない |
| `suggest`（既定） | する | 補正後読みの変換結果を `typo-correction` マーク付きで候補リスト先頭付近に注入。元の読みの候補も残す。 |
| `auto_replace` | する | 補正後読みのみで変換し、preedit のかなも補正後へ置換。 |

- 実効値の経路は `settings.json` の `typoCorrection.mode` と `typoCorrectionMode` とする（§8・§12.14）。host の
  `SettingsStore` が読み、`ApplyRuntimeSettingsToEngineConfig` が
  `EngineConfig::typo_correction_mode` へ反映する。TIP は設定を参照しない。
- `off` の最終ゲートは host 側。TIP は検出した観測を緩く送り、蓄積・適用の
  可否は host が判定する（TIP に設定プラミングを持ち込まない）。

## 4. 検出（TIP: `tsf-tip/`）

タイプミスの「検出」は TIP 側で行い、観測を `ObserveTypo`（fire-and-forget）で
host に送る。検出トリガは 2 種。

### 4-1. 未確定中の backspace 訂正

確定前に backspace でかな読みを消して打ち直したケース。

- `TextService` がかなを削除するキー処理に成功したとき、削除前の全かな読みを
  `TypoCorrectionTracker::BeforeBackspace` へ渡す。保留ローマ字だけを削除した場合は
  このトリガを発火させない。訂正中でなければ削除前の読みをスナップショットする。
- 連続 backspace ではスナップショットを更新しない（burst の最初の状態を保持）。
- core の確定アクション、`CommitSelected`、`CommitPreeditAsIs` は最終かな読みと
  tracker を保留する。`EditSession::DoEditSession` で本文の確定に成功した後、
  `TypoCorrectionTracker::Commit` が §6 のフィルタを満たすペアを返せば
  `ObserveTypo` を送る。要求の受理だけ、書込み失敗、アプリによる本文の置換では送らない。
  確定 callback は既存の edit cookie で入力先の privacy を再評価し、再試行や
  遅延実行の間に学習不可へ変わった観測を失効させる。
- 部分確定（M66、`docs/segment-editing-spec.md` §8）では tracker と保留中の確定を捨て、`ObserveTypo` を送らない。その後に残りの読みを確定したときも、部分確定の前の訂正の組は送らない。

### 4-2. 確定直後の打ち直し

確定したテキストを直後に backspace で消し、別入力をやり直したケース。

- 確定成功時に tracker が最終かな読みを保存し、最初の 1 キーの判定窓を開く。
- `TypoCorrectionTracker::BeginKey` は確定直後の **最初のキーが Ctrl / Alt / Win を伴わない VK_BACK** かつ
  preedit が空（= 確定済みテキストを消す方向）のとき、保存した読みを訂正前の読みとする。
  以降の打ち直し→確定で §4-1 と同じ比較ロジックが発火する。
- アプリへ渡すキーは `OnKeyDown` に届かないため、`OnTestKeyDown` でも判定する。
  同じキーの `OnKeyDown` で二重に数えず、確定の再試行に使われたキーは窓を消費しない。
- 訂正前の読みが未保存で判定窓が開いている、空 preedit の Backspace に限り
  privacy を評価する。判定窓の外でアプリへ渡す Backspace では scope probe を行わない。
- 通常入力が続いた場合は短いウィンドウ（最初の 1 キーのみ）でリセット。

### 4-3. 状態リセット

`Deactivate` / 外部からの `OnCompositionTerminated` / `OnSetFocus(FALSE)` と
入力先の変更で tracker、保留した確定観測、送信待ちの `ObserveTypo` をリセットする。
TIP 自身の確定に伴う composition 終了は、成功後の観測を消さない。
学習不可への遷移でも同じリセットを行い、IPC worker が取り出した観測は世代の照合で捨てる。
フォーカス移動・言語切替・学習不可の区間を跨いだ誤学習を防ぐ。

## 5. 蓄積と適用（host: `learning/` + `inference-host/`）

### 5-1. `TypoCorrectionStore`（`learning/`）

- 永続化: TSV `wrong_reading\tcorrect_reading\tcount last_updated_epoch`
  （`LearningStore` の `weight epoch` 区切り規約に合わせる）。
- 既定の保存先: `%LOCALAPPDATA%\azooKey\data\typo_corrections.tsv.enc`
  （`learning.tsv.enc` / `user_dict.json.enc` と同じ `data\` 配下。
  旧平文 `typo_corrections.tsv` は移行元。host CLI `--typo-store` で論理パスを上書き可）。
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
- typo のストアを非所有で保持（`SetTypoStore`）。M55 からは v2 の `TypoLearningStore`（§12.8）を持つ。
- `ObserveTypo(wrong, correct, now)`: 読みの組を観測し、件数と時間でまとめて保存する（§12.8）。
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
`preedit_kana_` を補正後読みへ置換する。応答は候補と同じ freshness 判定を通して
元読みと request ID を保留し、反映時にも入力先、元読み、request ID、secure を照合する。
TSF のスレッド制約のため非同期 preedit
更新は行わず、**次のキー入力時 / VK_SPACE 候補表示時に反映** する
（候補ウィンドウには既に補正後候補が出ているため体験上の影響は小さい）。
要求で送った `alt_readings` に含まれる `corrected_reading` は、`nn` + 母音の読みの選択であり、
補正ではない（`docs/legacy-parity-spec.md` §5.4）。TIP はこれで preedit を置換しない。

通常入力は `InputState::WithCorrectedReading` で読みを置換して保留ローマ字を消す。
Composing / Previewing / Selecting の状態、候補のスナップショット、選択位置、
候補待ち、live conversion 設定、カスタムローマ字表を保持する。
ライブ変換の表層を表示中は表層を保持し、対応する読みだけ補正する。
Enter では補正後のかな読みではなく、表示していたライブ変換の表層を確定する。
空読みとそれ以外の状態では置換しない。`suggest` / `off` の応答は補正読みが空なので、
TIP の読みを変更しない。モードの判定は §3 のとおり Host が担う。

## 6. 検出ロジックのエッジケース方針

- **編集距離しきい値**: 長い側の読みのコードポイント数を `len` とし、
  `min(3, max(1, ceil(len*0.34)))` を上限にする。TIP と Host は同じ条件を使い、
  どちらかの読みが 64 コードポイントを超えるペアを除外する。
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

payload の正典は `ipc/include/azookey/ipc/Payloads.h` の `ObserveTypoRequest` /
`ObserveTypoResponse` と `QueryCandidatesResponse` である（wire key は C++ のメンバ名と同じ）。
本節は `docs/windows-tsf-host-architecture.md`「IPC メッセージ」の索引から参照され、
M35 が使うフィールドの意味を定める。

- `ObserveTypo` は fire-and-forget であり、Host は応答を返さない。`ObserveTypoResponse` は
  対称性のためだけに定義する。
- TIP は Handshake の `capabilities` に `observe_typo` を載せる。

| payload | フィールド | 意味 | 欠落時 |
|---|---|---|---|
| `ObserveTypoRequest` | `wrong_reading` | 打ち間違えた読み | — |
| 同上 | `correct_reading` | 打ち直した正しい読み | — |
| 同上 | `timestamp_ms` | 観測時刻（epoch ミリ秒） | — |
| 同上 | `secure` / `learning_allowed` | そのイベントの privacy（§12.13） | `true` / `false`（fail-closed） |
| `QueryCandidatesResponse` | `corrected_reading` | Host が要求の読みの代わりに使った補正後の読み（§5-3）。空は補正なし | 空（補正なし。古い Host との後方互換） |

## 8. 設定（`settings/`）

型・既定値・範囲の正典は `settings/mvp-settings.schema.json` の root キー `typoCorrectionMode` /
`typoMinCount` である（`typoCorrectionMode` の schema の `description` が本節を指す）。
`SettingsStore` が読み、本節は各キーの意味と反映先を定める。M55 の `typoCorrection.*` は §12.14 が定め、
`typoCorrection.mode` があれば `typoCorrectionMode` より優先する。

| キー | 意味 | `EngineConfig` の反映先 |
|---|---|---|
| `typoCorrectionMode` | 動作モード（`off` / `suggest` / `auto_replace`。値の意味は §3。§12.14 の migration で v2 の mode として読む） | `typo_correction_mode` |
| `typoMinCount` | 同じ打ち間違えペアをこの回数観測してから補正候補に使う | `typo_min_count` |

`settings.json` に無いキーは schema の既定を使う。`typoMinCount` の範囲外の値は既定へ戻す。

## 9. テスト計画

- `tsf-tip/tests/typo_correction_tracker_test.cpp`: 2 トリガ、連続 backspace、
  最初の 1 キー以外での誤検出防止、リセット、UTF-8 長と編集距離のフィルタ。
- `tsf-tip/tests/onkeydown_preedit_test.cpp`: 成功後の観測、確定失敗と再試行、
  lifecycle と privacy のリセット、handshake、補正読みの保留・次キー反映・stale 拒否。
- `core/tests/input_state_test.cpp`: 補正読みの置換で状態、固定候補、選択位置、
  候補待ち、カスタムローマ字表を保持し、空読みと非入力状態では変更しない。

- `learning/tests/typo_correction_store_test.cpp`:
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

本節は削除した（変更対象は roadmap の M 節、テストは `docs/test-inventory.md` が持つ）。

## 11. 検証手順（実装後）

本節は削除した（ビルドとテストの手順は `README.md`、テスト一覧は `docs/test-inventory.md` が持つ）。

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
として残す。マイグレート時は v1 の組を §12.8 の `typo_reading_pairs` へ移し、
`typo_patterns` と `typo_events` には入れない（手順は §12.8「v1 からの移行」）。
v1 の組に由来する候補を示す `pattern_id` の文字列値（例: `"v1_legacy"`）は
JSON 候補メタデータ（§12.2 の `pattern_id` フィールド、TEXT）でのみ使い、
SQL の INTEGER 列（`typo_events.pattern_id`）とは混在させない。

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

C++ の型は `correction/include/azookey/correction/ReadingHypothesis.h` の `ReadingHypothesis` で、上の表に加えて次を持つ。

| フィールド | 内容 |
|---|---|
| `cost` | §12.4.1 の重み付き編集コスト（個人化の減額の後）。元の読みは 0 |
| `edits` | observed の向きの編集の列（§12.12.1 の align の操作）。元の読みは空 |
| `edit_space` | 編集を取った列。打鍵（`raw_keys`）か読み |

- `confidence` は、個人パターンが当たった仮説ではそのパターンの §12.9 の値、当たらない仮説では 0.5
  （§12.9 の `base`。実績の無いパターンと同じ）とする。0.5 は `minConfidenceForRanking` より低いので、
  汎用の規則だけの仮説は §12.11 により「もしかして」枠に留まる。元の読みは 1.0。
- `edit_distance` は、observed の読みと補正後の読みのコードポイント単位の編集距離（重みなし。§6 と同じ単位）。
- `pattern_id` は、個人パターンが当たった仮説だけが持つ。

### 12.3 対象 typo_type（11 種）

| `typo_type` | 内容 | 例 |
|---|---|---|
| `adjacent_key` | 隣のキーを押した | `koujsyou` → `kousyou` |
| `missing_char` | 文字抜け | `hasuru` → `haisuru` |
| `extra_char` | 余分な文字 | `kouhsyou` → `kousyou` |
| `transposition` | 文字順入れ替わり | `ahsita` → `ashita` |
| `double_key` | 同じキーを重複 | `koussyo` → `kousyo` |
| `romaji_variant` | ローマ字表記揺れ | `syou` / `shou` / `syo` |
| `n_handling` | んの入力ミス | `kani` / `kanni` / `kan'i` |
| `small_tsu` | 促音の入力ミス | `kita` / `kitta` |
| `long_vowel` | 長音・母音揺れ | `ou` / `oo` / `o` |
| `dakuten_confusion` | 濁点・半濁点のミス | `かいしや` / `がいしゃ` |
| `kana_shape_confusion` | かな形状・入力癖 | `しや` → `しゃ` |

補正エンジン（`correction/src/TypoCorrectionEngine.cpp`）は、仮説ごとに 1 つの型を次の規則で決める。

1. 補正後の読みが observed の読みと同じ長さで、違う文字がすべて下の表の組の置き換えなら、
   `dakuten_confusion`（清音・濁音・半濁音の組）か `kana_shape_confusion`（大小・形の組）とする。
   両方を含むときは `dakuten_confusion` とする。
2. そうでなければ、打鍵の編集ごとに、次の順で最初に当たる型を取る。
   - `n_handling`: `n` か `'` の余分・抜け
   - `small_tsu`: 直前の打鍵と同じ子音（`n` と母音を除く）の抜け
   - `double_key`: 直前の打鍵と同じキーの余分
   - `romaji_variant`: 編集の前後で §12.4.3 の正規化が同じになるもの（`shyou` と `shou` など）と、
     正規化すると `sho`・`shu`・`cho`・`chu`・`jo`・`ju` で終わる打鍵の後の `u` の余分・抜け（`syo` と `syou`）
   - `long_vowel`: `o` か `u` の後の `o`・`u` の余分・抜けと、`o` の後の `o` と `u` の置き換え
   - `transposition`: 隣り合う 2 キーの入れ替え
   - `adjacent_key`: 隣接か同一指（§12.4.6）のキーの置き換えと、直前の打鍵に隣接するキーの余分
   - `extra_char`: そのほかの余分
   - `missing_char`: そのほかの抜け

   どれにも当たらない置き換え（隣接でも同一指でもない）を含む仮説は作らない。編集が 2 つの仮説は、
   この節の冒頭の `typo_type` の表の順で先に来る型を取る（上の判定の順ではない）。

| 組の種類 | 組 |
|---|---|
| 清音・濁音・半濁音 | か行・さ行・た行の清音と濁音、は行の清音・濁音・半濁音、う と ゔ |
| 大小・形 | あ・い・う・え・お・つ・や・ゆ・よ・わ・か・け とその小書き、ぬ と め、わ と ね と れ、る と ろ、は と ほ、さ と ち |

`nn` + 母音を「ん」+ あ行・や行で読む読み（`RomajiKanaConverter::AmbiguousReadings`）は読みの選択であり
（`docs/legacy-parity-spec.md` §5.4）、補正の仮説にしない。

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

本節と §12.4.2 は重み付き編集距離のコストモデルの正典であり、§12.4.6 のコスト表を初期値とする。

例:

```
ユーザーがよく j を余分に打つ
  koujsyou → kousyou
  personalized_pattern_cost: low
```

補正エンジンは、observed の読みと `raw_keys`（§12.13）から次の 2 つの経路で仮説を作り、同じ読みは
最も安いものを 1 つ残す。

- 打鍵: `raw_keys` が英字の打鍵で、組み込みのローマ字表で observed の読みに変換されるときだけ、
  §12.4.6 の `FuzzyMatch` が 2 編集までの読みを挙げる。置き換えは隣接か同一指のキーに限る。それ以外の
  置き換えは §12.3 のどの型にも当たらず、清濁・大小の差になるものは読みの経路が同じコストで出すためである。
- 読み: observed の読みがひらがなと長音符だけのとき、その 1 文字または 2 文字を、§12.3 の表の組で
  置き換える。置き換え 1 つのコストは substitution と同じ 1.0 とする。

`personalized_pattern_cost` は、学習ストアのパターンを引く注入点が、仮説の型と編集の列に対して返す
減額とする。減額の後のコストは 0 を下回らない。減額は `FuzzyMatch` が読みごとに 1 つ選んだ打鍵の編集の列と、
その 256 件に残った読みにだけ当たる。同じ読みに届く別の打鍵の編集の列や、256 件の外に落ちた読みには当たらない。
仮説は安い順（同じコストは読みの順）に最大 256 件を返す。
`typoCorrection.maxReadingHypotheses`（§12.14）で切るのは、§12.4.4 の辞書の制約の後である。辞書を使わずに
コストだけで並べると、辞書に無い読みが意図した読みより上に多く並ぶためである。入力が長いと 1 編集の読みだけで
256 件を超え、同じコストの読みはかなの順で切られる。さらに長い入力（64 キー近く）では、`FuzzyMatch` の探索が
256 件より先に上限（§12.4.6 の 20,000 状態）に達し、そこまでに見つかった読みだけを返す。このときは同じコストの
読みが揃わず、かなの順で切ったものにならない。

Host の注入点は、§12.8 の `typo_patterns` を `typo_type` と 2 つのパターン（§12.12.1）で引く。`keyboard_layout` だけが
違う行は、件数を足し合わせて 1 つとして数える。warm-up（§12.11）を満たし confidence が `minConfidenceForRanking` 以上の
パターンは、`personal_cost_reduction`（§12.16）× confidence を減額として返す。ほかの当たったパターンは減額 0 で返し、
その confidence と件数は §12.10・§12.11 に渡る。引く表はストアの内容が変わるたびに作り直す。

observed の読みが `LEN_MIN`（§12.16）以下のとき、読みか `raw_keys` が 64 コードポイントを超えるとき（§6）、
どちらかが §12.12.1 の文字集合の外の文字を含むときは、仮説を作らない。

#### 12.4.2 Keyboard Adjacency Model

JIS / US 配列を考慮して隣接キー誤打鍵を低コストにする:

```
u の隣に i / y / j がある
s の隣に a / d / w / x がある
```

`settings.typoCorrection.keyboardLayout`（§12.14）で切替える。schema が受ける値は `auto`（OS の配列に
従う）だけとし、配列を固定する `jis` / `us` は配列ごとの隣接表と同時に足す。

隣接は、同じ行の左右のキーと、タッチタイピングの同じ列（q-a-z、w-s-x、e-d-c、r-f-v、t-g-b、y-h-n、
u-j-m、i-k、o-l、p）の上下のキーとする。英字の並びは JIS と US で同じなので、`auto` はこの 1 枚の表を使う。
`-` と `'` は配列によって位置が違うので、隣接を持たない。表は `core/src/QwertyAdjacency.cpp` に置き
（§12.4.6 の `FuzzyMatch` が core から使う）、`correction/` の `KeyboardAdjacencyModel` は、配列の選択と、
分類・コストの問い合わせをこの表に委ねる。

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

`correction/` の `RomajiVariantNormalizer` は、表の行を同じ行の a・u・e・o に広げ（`sya` → `sha`、
`zyu` → `ju` など）、表に無い書き換え（`ty` + a・u・e・o → `ch`、混在形の `shy` → `sh` と `chy` → `ch`）も行う。組み込みのローマ字表は、
`nn` と混在形を除き、表の左右を同じかなに変換する。そのため正規化は綴りを比べるためのもので、変換には
使わない。§12.3 の `romaji_variant` の判定に使う。

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

#### 12.4.6 `RomajiKanaConverter::FuzzyMatch`（M17 の横断項目）

入力ローマ字 `input` に対して、上位 `k` 通り（下の適用順序では `K=8`）のかな解釈を返す API。

```cpp
struct FuzzyKanaCandidate {
    std::string kana;
    double      cost;    // 編集距離 + ペナルティ
    std::string romaji;  // kana に変換される編集後のローマ字
};

enum class FuzzySubstitutions {
    All,           // すべての英字の置き換え
    NearKeysOnly,  // 隣接か同一指のキーの置き換えだけ
};

std::vector<FuzzyKanaCandidate>
RomajiKanaConverter::FuzzyMatch(std::string_view input, size_t k,
                                FuzzySubstitutions substitutions = FuzzySubstitutions::All) const;
```

コストは Damerau-Levenshtein を拡張した次の初期値で与える。個人化の項（§12.4.1）と
JIS / US 配列の隣接（§12.4.2）は、この初期値の上に載る。

| 操作 | コスト |
|---|---|
| substitution | 1.0 |
| insertion | 1.0 |
| deletion | 1.0 |
| transposition (隣接 2 文字入れ替え) | 0.5 |
| QWERTY 隣接キー substitution | 0.7（−0.3 ボーナス） |
| 同一指 substitution | 0.6（−0.4 ボーナス） |

QWERTY 隣接：`q-w`, `w-e`, ..., `a-s`, `s-d`, ... 全 ASCII 小文字ペアを
`core/src/QwertyAdjacency.cpp`（M17 の横断項目）に静的テーブルとして持つ。隣接は §12.4.2 の定義による。
同一指は標準のタッチタイピングの割り当て（左小指 q a z、左薬指 w s x、左中指 e d c、
左人差し指 r f v t g b、右人差し指 y h n u j m、右中指 i k、右薬指 o l、右小指 p）とする。
隣接かつ同一指の組（u と j など）は小さい方の 0.6 を取り、2 つのボーナスは足さない。

`FuzzyMatch` は組み込みのローマ字表だけを使い、次のとおりに読みを挙げる。

- 編集は、英字の置き換え、英字か `'` の挿入、キーの削除、隣り合う 2 キーの入れ替えで、2 つまでとする。
  置き換えを隣接か同一指のキーに限る選択（`FuzzySubstitutions::NearKeysOnly`）を持ち、既定はすべての英字の
  置き換えとする。
- 各読みは最も安いコストで 1 回だけ挙げ、コストの昇順、同じコストはかなの順に並べる。
- `ConvertForCommit` が `input` に与える読みと、英字・記号（`zj` の「↓」など）が残る読みは挙げない。
- カスタム表のとき、`input` が空か 64 文字を超えるとき、`a`〜`z`・`-`・`'` の外の文字を含むときは空を返す。
- 探索は 20,000 状態で打ち切る。

適用順序:

1. Standard parse（既存）→ 1 候補
2. FuzzyMatch (K=8) → 8 候補
3. それぞれを `SimpleConverter::Query` or `Zenzai::Query` に通す
4. 全候補を `score - alpha * cost` でソート（alpha=0.5、設定で変更可）

§12.4.1 の補正エンジンは、仮説を作るために `k` を 256 として呼ぶ。

### 12.5 発動条件

打ち間違え補正は常時強制しない。発動判定は **正規化済みスコア**で行う。
元読みの変換候補の生スコアを降順に `r1 = score_top1_original`、
`r2 = score_top2_original` とする。**生スコアは正とは限らない**点に注意する
（例: `SimpleConverter` は棄却 surface から 1.0 を引くため、retry/訂正文脈で
残候補が全て棄却され `r1 <= 0` になりうる）。また **top2 が存在するとは限らない**
（候補が 1 件、または `QueryCandidatesRequest.max_candidates = 1` で 1 件に制限）。
top2 が無いときは `r2 = 0` を既定とする（拮抗相手がいない = gap は最大）。そこで
top1 で割る正規化は `r1 > 0` のときだけ行い、`s2 = clamp(r2 / r1, 0, 1)` とする。
`dict_best` は元読みの最良辞書 / Zenzai ヒットの正規化スコア（ヒットなしは 0）。

```
activate_typo_correction(reading, observed_pattern):
  if Utf8CharLength(reading) <= LEN_MIN        → false   // §12.11。既定 2 = 2 文字以下は補正しない
  // 非正規化フォールバック: 元候補なし or r1 <= 0 は「元候補が無効に弱い」
  // = まさに補正したい状況。weak を立て、s2 正規化はスキップ。
  if (no original candidates) or (r1 <= 0):
    weak = true; small_gap = false
  else:
    r2        = (top2 が無ければ 0)             // 候補 1 件 / max_candidates=1 のガード
    s2        = clamp(r2 / r1, 0, 1)
    weak      = (dict_best < S_WEAK)           // 元候補が弱い。既定 0.40
    small_gap = ((1.0 - s2) < G_GAP)           // top1/top2 が拮抗。既定 0.15（top2 無し ⇒ s2=0 ⇒ false）
  no_dict    = (dict_best == 0)                // 辞書ヒットなし
  strong_pat = (personal_pattern_confidence(observed_pattern)
                 >= minConfidenceForRanking)   // 既定 0.70（§12.14）
  return weak or small_gap or no_dict or strong_pat
```

- **非正の top1 / 候補なしのガード**: `r1 <= 0` や候補ゼロのときに `r2 / r1` を
  計算しない（ゼロ除算・符号反転で `s2` が `[0,1]` を外れ `small_gap` が誤作動
  するのを防ぐ）。この状況は元候補が事実上無効なので `weak = true` として補正を
  発動させる（訂正・retry 文脈で補正を出したい意図と一致）。
- `S_WEAK` / `G_GAP` は初期値であり M52（`typo_false_positive_rate`）で校正
  する（§12.16）。`strong_pat` のしきい値は設定 `minConfidenceForRanking`
  （§12.14）と同一値を使い、二重定義しない。
- 発動しても補正候補が第一候補になるとは限らない。順位は §12.10 のスコアと
  §12.11 の誤補正防止条件で決まる。発動は「補正 hypothesis を **生成する**
  か」だけを決める前段ゲートである。
- Host は `dict_best` を 2 値で近似し、M52 の校正（§12.16、DEV-2150）で見直す。元の読みに辞書（システム辞書・
  ユーザー辞書・新語）の語があれば 1、無ければ 0 とする。このため `weak` は `no_dict` と同じになる。`observed_pattern` は仮説ごとに決まるので、
  `strong_pat` は仮説ごとに評価する。`weak`・`small_gap`・`no_dict` のどれも立たない読みでは、warm-up（§12.11）を
  満たし confidence が `minConfidenceForRanking` 以上の個人パターン（§12.8）に当たる仮説だけを作る。
  どの個人パターンも warm-up を満たさなければ、仮説を作らない。

### 12.6 補正モード（v2）

v1 の 3 モード（off / suggest / auto_replace）を **4 モード** に拡張:

| mode | 動作 |
|---|---|
| `off` | 打ち間違え補正を無効（v1 と互換） |
| `suggest` | 「もしかして」候補として提示（v1 互換、既定） |
| `rank` | 通常候補に混ぜるが控えめに加点（v2 で追加） |
| `aggressive` | 高信頼時のみ第一候補まで上げる（v2 で追加） |

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

`rank` と `aggressive` は、仮説の語（§12.4.4）を、§12.10 の `typo_score` の順に第一候補の直後へ最大
`maxReadingHypotheses` 件置く。`aggressive` で §12.11 の全ガードを満たす語は、その前の第一候補に置く。
このときも `corrected_reading`（§5-3）は返さず、候補の reading を仮説の読みとする。TIP の preedit は打った読みのまま残る。
warm-up を満たし confidence が `minConfidenceForRanking` 以上の個人パターンの語は通常候補として示し、それ以外の語には
「もしかして: <読み>」を付ける。v1 の読みの組の候補（§5-2）は、`suggest` と `rank` でこれらより前の先頭に置く。
`rank` と `aggressive` では、secure でない `QueryBatchConversion` の各 chunk にも同じ規則で仮説の語を置く。
一括変換の要求は `raw_keys` を持たないので、仮説は読みの経路（§12.4.1）だけから作る。

### 12.8 TypoLearningStore（v2）

M55 の v2 ストアは SQLite で持ち、本節のスキーマを基にする（DEV-1955）。v1 のストアは
`typo_corrections.tsv`（`# azookey-typo-correction-tsv escaped=1`）である。

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
CREATE UNIQUE INDEX typo_patterns_key ON typo_patterns
  (observed_pattern, intended_pattern, typo_type, IFNULL(keyboard_layout, ''));
```

パターンは 4 列（`observed_pattern`、`intended_pattern`、`typo_type`、`keyboard_layout`）で 1 つに決まり、
`keyboard_layout` の NULL は空と同じに扱う。この索引が無い DB は、下記 5 の破損として扱う。

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

#### typo_reading_pairs

M35 の読みの組（§5-1・§6 の `Observe` / `Lookup` が扱う誤った読みと正しい読みの組）を v2 で持つ表。
v1 の行は読みの組だけを持ち、§12.12.1 の抽象化（ローマ字の打鍵列の上で定義する）に写せないため、
`typo_patterns` に入れずにこの表へ移す。v1 から移した組と、v2 で観測した組を同じ表に持つ。

```sql
CREATE TABLE typo_reading_pairs (
  wrong_reading TEXT NOT NULL,
  correct_reading TEXT NOT NULL,
  count INTEGER NOT NULL,
  last_updated_at INTEGER NOT NULL,
  PRIMARY KEY (wrong_reading, correct_reading)
);
```

v2 は TSV の拡張を経ずに SQLite で始める（DEV-1955）。

#### パス

v2 のファイルと移行した v1 は、v1 の論理パス（既定は `%LOCALAPPDATA%\azooKey\data\typo_corrections.tsv`。
host CLI の `--typo-store` で上書きできる。§5-1）と同じディレクトリに置き、名前は下表と次の規則で決める。
どれも既存の `ReadProtectedText` / `WriteProtectedText`（`learning/include/azookey/learning/DpapiCrypto.h`）に
論理パスを渡して読み書きし、ディスク上は `<論理パス>.enc` になる。

| ファイル | 論理パス | ディスク上 |
|---|---|---|
| v1 のストア | `typo_corrections.tsv` | `typo_corrections.tsv.enc` |
| v2 のストア | `typo_learning.v2.db` | `typo_learning.v2.db.enc` |
| 移行した v1（以下「v1 の退避」） | `typo_corrections.v1.tsv.bak` | `typo_corrections.v1.tsv.bak.enc` |

- v2 の名前は、v1 の名前によらず `typo_learning.v2.db` とする。
- v1 の退避の名前は、v1 のファイル名の拡張子の前に `.v1` を入れ、末尾に `.bak` を付ける。`--typo-store` が
  `mine.tsv` なら `mine.v1.tsv.bak`、拡張子の無い `typos` なら `typos.v1.bak` である。
- v1 の退避の論理パスに `typo_corrections.tsv.bak` は使わない。M34 の平文移行が残す平文の退避と同名で、
  `WriteProtectedText` がこれを残った退避（orphan backup）とみなして保存を止めるためである。
- M34 の平文移行が残す `typo_corrections.tsv.bak`（平文）は M34 の規則に従い、本節の移行と削除の対象に
  入れない（全削除で消すかの全ストア共通の見直しは DEV-2200 で行う）。
- 各ファイルの状態は、論理パスを `ReadProtectedText` に渡した結果で決める。`Encrypted` と `Plaintext` は
  「ある」、`Missing` は「無い」、`Error` は「読めない」とする。ただし v1 のストアについて、
  `typo_corrections.tsv.enc` も平文の `typo_corrections.tsv` も無く、M34 の平文の退避 `typo_corrections.tsv.bak`
  だけがあるとき（`ReadProtectedText` は `Error` を返す）は、本節では「無い」と扱う。v1 を退避へ移した後や
  全削除の後に、M34 の平文の退避だけが残るためである。
- 「読めない」ファイルは、動かさず、上書きせず、消さない（全削除で消すときを除く）。

#### 保存の形

v2 のストアはメモリ上の SQLite DB として開き、ディスクへは DB 全体のバイト列として保存する。

- 保存: `sqlite3_serialize` で得たバイト列を `WriteProtectedText`（DPAPI 暗号化、file lock、atomic replace）で書く。
- 読み込み: 復号したバイト列を `sqlite3_malloc64` で確保した領域へ写し、`sqlite3_deserialize` に
  `SQLITE_DESERIALIZE_FREEONCLOSE | SQLITE_DESERIALIZE_RESIZEABLE` を付けて渡す。
- ディスク上に平文の DB を置かない。ジャーナル（`-journal`、`-wal`、`-shm`）も、`VACUUM` や並べ替えが使う
  一時ファイルもディスクに作らない（下記の `journal_mode` と `SQLITE_TEMP_STORE`）。`typo_events` と
  `typo_reading_pairs` は読みと表記を持つため、平文で置くと M34 の暗号化の契約を破る。
- メモリ上の平文の消去: 復号したバイト列と `sqlite3_serialize` のバッファは、使い終えたら `SecureErase` と同じく
  0 で上書きしてから解放する。SQLite が内部で確保する領域は、解放時に 0 で上書きする割り当て関数を
  `sqlite3_config(SQLITE_CONFIG_MALLOC)` で与えて消す。
- スキーマの版は `PRAGMA user_version` に持ち、本節のスキーマを 1 とする。
- 保存のたびに DB 全体を書き直す。行数が増える `typo_events` と `typo_reading_pairs` には保持上限（下記）を課す。
  確定ごとに同期保存せず、`docs/user-learning-enhancement-spec.md` §3.1.1 と同じく件数と時間で
  まとめて保存する。

#### SQLite の取り込み

- sqlite.org が配布する amalgamation（`sqlite3.c` と `sqlite3.h`）を repo の third_party/sqlite/ ディレクトリに、
  上流のバイト列のまま commit する（`.gitattributes` で改行を変換しない）。版、取得元の zip の URL と SHA256
  （sqlite.org が公表する SHA3-256 も）、各ファイルの SHA256 を、同じディレクトリの `PROVENANCE.json` に記録する。
- 既定の configure は、ネットワークに触れずに成功する（`docs/dev-infrastructure-spec.md` §1 のオフラインビルド原則）。
  そのため `docs/build-ci-spec.md` §3.1 の 2（ビルドが要る依存を opt-in の `FetchContent` で取る）ではなく、
  §3.1 の 4 によりソースを repo に置く。取得のための CMake option は設けない。
- `azookey_learning` に静的リンクし、配布物に DLL を足さない。x64・ARM64・Linux で同じソースを使う。
- 使わない機能を外して組み込む。少なくとも拡張の読み込みを外し（`SQLITE_OMIT_LOAD_EXTENSION`）、
  二重引用符の文字列リテラルを受けず（`SQLITE_DQS=0`）、一時ファイルを常にメモリに置く
  （`SQLITE_TEMP_STORE=3`）。削除した値と解放した領域を残さないため、`SQLITE_SECURE_DELETE` と
  `SQLITE_DEFAULT_PCACHE_INITSZ=0` も定義する。`sqlite3_serialize` / `sqlite3_deserialize` を含む版を使う
  （3.36 以降は既定で含む）。
- OS の `winsqlite3.dll` は使わない。版と compile option を固定できず、OS の更新に左右されるためである。
  vcpkg も使わない（`docs/build-ci-spec.md` §3.1 は vcpkg を必須にしない）。
- ライセンスは public domain（SPDX `blessing`）で、帰属表示の義務は無い。それでも配布物に含むので、
  `THIRD_PARTY_LICENSES` に同梱の節を置く。リリース SBOM の生成（`scripts/complete-release-sbom.py`）は、
  その節の `vendored:` の選択子から `PROVENANCE.json` を読んで版と zip の SHA256 を載せ、記録した SHA256 と
  ファイルが一致しないとき、記録に無いファイルがあるときに失敗する。

#### 由来の印

v2 のストアは、`typo_settings` のキー `store_origin` に作られた経緯を持つ。値は次のどれかで、v2 を初めて
保存するときに必ず書く。ただし v1 からの移行では、読み戻しの検証が済んでから書く。印の無い v2 は、
v1 からの移行の検証が済んでいないものだけである。

| 値 | 意味 |
|---|---|
| `v1_migrated` | v1（または v1 の退避）から移し、読み戻しの検証まで済んだ |
| `empty` | 移す v1 が無く、空で始めた |
| `rebuilt` | 破損から作り直した（移し直したときは `v1_migrated`） |

#### 起動時の読み込みと破損時の復旧

Host は起動時にストアを読み、次の順に判定する。状態（ある・無い・読めない）は「パス」の定義に従う。

1. v2 が無い:
   - v1 のストアがある: v1 からの移行（下記）を行う。平文の `typo_corrections.tsv` しか無いときは、
     v1 の `Load` が M34 の暗号化移行を済ませてから移す。
   - v1 のストアが読めない: v2 を作らず、v1 の既存の規則（空のストアで動き、保存を止める）に従う。
     v1 の退避があっても移さない。v2 を作ると、後で読めるようになる v1 を隠すためである。
   - v1 のストアが無く、v1 の退避がある: 退避から移す（退避は動かさない）。退避が読めないとき、または
     移すのに失敗したときは、v2 を作らず、空のストアで動いて保存を止める。この経路では「v1 のまま読む」に
     入らない（新しい v1 のストアを作ると、次の起動で退避と取り違えるため）。
   - v1 のストアも v1 の退避も無い: 空のストアで始め、印を `empty` にする。
2. v2 が読めない（復号の失敗）: 空のストアで動き、保存を止める。v2 にも v1 にも書かない（v1 と
   `docs/user-learning-enhancement-spec.md` §3.1 の v2 と同じ扱い）。
3. `sqlite3_deserialize` が失敗した、ファイルの長さが 0、または `user_version` が 0（本節の表が無い）:
   下記 5 の破損として扱う。Host は版 1 以上の DB しか書かないので、これらは破損である。
4. `user_version` が既知の版より新しい: 新しい版の Host が書いたファイルとみなし、保存を止めて上書きしない。
   既知の版より古い版（1 以上）からの移し替えは、スキーマを変えるときに本節へ足す。
5. `PRAGMA integrity_check` の結果が `ok` でない（または 3）: 暗号化したままのファイルを
   `<ファイル名>.corrupt-<epoch 秒>` へ一度だけ複写する。複写に失敗したら保存を止める。同じ名前の複写が
   既にあるときは、中身が同じなら複写済みとし、違えば複写に失敗したものとして扱う。複写の後は、
   v1 のストア、v1 の退避の順に、「ある」方から移し直す（印は `v1_migrated`。v1 のストアから移したときは
   移行の手順 5 で退避へ移す）。どちらも無いか、移し直しに失敗したときは空のストアを作り直す（印は `rebuilt`）。
   どちらかが読めないときも、読めないファイルは動かさずに次へ進む。どの場合も学習を続ける。
   破損からの作り直しは、その起動の間、診断 D-010 の warning に出す（`docs/diagnostics-compat-spec.md` §12.2）。
   ログには件数と理由だけを出し、行の内容は出さない。

以下の 6・7 は、v2 を 2〜5 で読めた後に限る。

6. 印が無い（移行の検証が済んでいない v2）:
   - v1 のストアがある: v2 を消し、v1 からの移行をやり直す。
   - v1 のストアが無い: v1 の退避から移し直す。退避が無ければ空のストアを作り直す（印は `rebuilt`）。
   - v1 のストアか、（v1 のストアが無いときの）v1 の退避が読めない: v2 も v1 の側も動かさず、空のストアで
     動いて保存を止める。
7. 印があり、v1 のストアがある（移行の手順 5 の前に止まった）: v1 の組を v2 の `typo_reading_pairs` に取り込む。
   同じ組は、`count` と `last_updated_at` をそれぞれ大きい方にする（列ごとの最大）。v2 の組は v1 から移した
   値を含むので、加算すると同じ履歴を二重に数えるためである（`docs/learning-data-management-spec.md` §4.4 の
   import は別の写しを合わせるので回数を足す）。v2 を保存してから、移行の手順 5 で v1 のストアを退避へ移す。
   - 印があり、v1 のストアが読めない: v2 を使い、v1 のストアを動かさない。その間、読みの組の忘却は
     v1 のストアへ及ぼせないので失敗として返す（全削除は読まずに消せるので行う）。
8. それ以外は v2 を使う。

`integrity_check` は起動時に毎回、DB 全体に対して行う。`.corrupt-*` の複写は全削除で消し、忘却は複写に及ばない
（`docs/user-learning-enhancement-spec.md` §3.1 の学習ストアと同じ扱い。手順は下記の「学習の削除」）。

**既知の制約（旧版の Host への戻し）**: v2 を読めない旧版の Host に戻すと、旧版は v1 のストアだけを使う
（移行で退避へ移した後なら空から始める）。旧版で行った観測は判定 7 で v2 に取り込むが、旧版で行った
忘却と全削除は v2 に届かず、新しい Host に戻すと消した組が提示されうる。旧版の Host への戻しは本節の
対象外とし、この経路の整合は保証しない。

#### 同時書き込み

- `journal_mode` は `MEMORY` とする。ジャーナルをディスクに作らない。WAL は使わない。
- 書き込むプロセスは Host だけとする。設定アプリと `azookey_diag` は v2 のファイルを直接開かず、
  学習データの操作は Host への IPC（`docs/learning-data-management-spec.md` §4）を通す。
- 排他は `WriteProtectedText` が取る file lock（`AcquireExclusiveFileLockForPath`）と atomic replace に従う。
  writer の一覧は `docs/learning-data-management-spec.md` §14 が持つ。
- ストアは自分のロックを持たず、読み書きの直列化は呼び出し側が行う（v1 と同じ）。

#### v1 からの移行

1. v1 を v1 の `Load` で読み、各組を `typo_reading_pairs` の行にする（`count` と `last_updated_at` は
   v1 の値をそのまま使う）。保持上限はこの段階では当てない。
2. v2 を印なしで保存する。
3. v2 を読み戻し、復号、`integrity_check`、行数の一致を確かめる。
4. 確かめられたら、印を `v1_migrated` にして v2 を保存し直す。
5. v1 のストアから移したときは、v1 のストアを v1 の退避へ移す（暗号化したまま。平文に戻さない）。
   - 退避が無い: v1 のストアを退避へ rename する。
   - 退避がある: 退避を置き換えない。退避の内容の後ろに v1 のストアの内容をそのまま足したものを退避として書き
     （DPAPI 暗号化と atomic replace）、書けてから v1 のストアを消す。退避の内容が改行で終わらないときは、
     最後の行と v1 のストアの 1 行目がつながらないよう、間に改行を 1 つ補う。v1 の parser は同じ組の後の行で前の行を
     上書きし、2 行目以降の `#` の行を読み飛ばすので、合わせた退避を読むと v1 のストアの値が勝つ。退避と
     v1 のストアの解析できない行も、バイト列のまま残る。
   - 退避が読めない: 足さず、v1 のストアも退避も動かさない（判定 7 の「v1 のストアが読めない」と同じく、
     読みの組の忘却は失敗として返す）。
   - v1 の退避から移したときは何も動かさない。

- v1 のストアから移す 1〜4 のどこかで失敗したら、v2 を書けていれば消し、その起動は下記の「v1 のまま読む」で動く。
  v2 を消せなくても、印の無い v2 は次の起動の判定 6 で消してやり直す。v1 の退避から移す途中で失敗したときは、
  判定 1 のとおり空のストアで動いて保存を止める。
- 5 だけが失敗したときは v2 を使い、次の起動の判定 7 で取り込みと 5 をやり直す。
- v1 の `Load` が解析できない行を報告したときも、読めた行で移す。v1 のストアは v1 の退避として残るので、
  解析できなかった行は退避に残る。
- 設定のモード（`typoCorrectionMode` の `auto_replace` など）の読み替えはストアの移行と別で、§12.14 が持つ。

**v1 のまま読む**: v1 のストアからの移行に失敗した起動では、Host は v1 のストアを M35 の規則（§5-1・§6）のまま
使い、v2 を持たない。

- 観測は v1 のストアに保存する。パターンと `typo_events` は保存しない。
- 忘却と全削除は v1 のストアに及ぼし（`docs/learning-data-management-spec.md` §4.2・§4.6）、v1 の退避が
  あれば下記の「学習の削除」と同じく退避にも及ぼす。
- 保持上限は当てない（v1 の `Lookup` の期限が効く）。
- 次の起動で移行をやり直す。

#### 学習の削除

一覧と忘却の id（`docs/learning-data-management-spec.md` §4.1）は次のとおり導く。

- 読みの組: チャネルを空、読みを誤った読み、表記を正しい読みとする（v1 と同じ）。移行の前後で id は変わらない。
- パターン: チャネルを `pattern`、読みを `observed_pattern`、表記を `intended_pattern` とする。
  一覧の entry の `channel` に `pattern` を出して、読みの組と見分ける。
  `typo_type` と `keyboard_layout` だけが違う行は 1 entry に畳み、`weight` を採用の回数の合計、metadata の
  `accept_count` / `reject_count` をそれぞれの合計、最終更新を `updated_at` の最大とする。

忘却（`ForgetLearningEntry`、`store` は `typo`）:

- パターンなら、該当する `typo_patterns` の行と、その `pattern_id` を持つ `typo_events` の行を消す。
- 読みの組なら、`typo_reading_pairs` の行と、`observed_reading` と `corrected_reading` が同じ組の
  `typo_events` の行を消し、v1 の退避（と、残っていれば v1 のストア）からも同じ組を除く。
- 書く順: v1 の退避と v1 のストアを先に書き戻し、成功してから v2 を保存する。書き戻しに失敗したとき、
  または v1 の退避か v1 のストアが読めないときは、v2 を変えずに忘却を失敗として返す。v2 の保存に失敗したら `save_failed` を返し、公開済みの内容を保つ
  （v1 の側からは組が除かれたままになるが、消した学習が戻る向きの不整合は生じない）。
- v1 の側の書き戻しでは、除く組の行だけを除き、解析できない行を含むほかのバイト列は保つ
  （`LearningStore` の M7 ファイルへの書き戻しと同じ。`docs/learning-data-management-spec.md` §4.2）。

全削除（`ResetLearningStore`、`store` は `typo`）: `typo_patterns`、`typo_events`、`typo_reading_pairs` の
全行を消し、v1 の退避（と、残っていれば v1 のストア）と v2 の破損時の複写（`.corrupt-*`）を消す。
`typo_settings` は学習ではないので残す。次の順に処理する。

1. 読み込みに失敗して保存を止めているときは、何も変えずに `save_failed` を返す。
2. v1 の退避、v1 のストア、v2 の破損時の複写を消す。複写は、v2 の DB の名前に `.corrupt-` を付けたものと、
   暗号化したファイル（`.enc`）の名前に付けたものの両方を消す（判定 5 は、その時点で格納されていた方の名前で
   複写する）。失敗したら `save_failed` を返す。このとき一部だけ消えていることが
   あるが、Host が提示する内容（v2）は変わらない。
3. v2 のメモリ上の写しを空にして保存する。失敗したら、公開済みの内容を保って `save_failed` を返す。
   v1 の側は既に無いので、消した学習が破損時の移し直しで戻ることは無い。

「v1 のまま読む」起動では、v1 の退避と v2 の破損時の複写を先に消し、その後で v1 のストアを空にして保存する。
どちらで失敗しても `save_failed` を返し、Host が提示する内容（v1 のストア）は失敗の前と変わらない。
いずれの経路でも、`save_failed` の後に Host が提示する学習の内容が変わらないという
`docs/learning-data-management-spec.md` §4.6 の契約を満たす。変わりうるのは v1 の側の写しと v2 の破損時の複写だけである。

- 消した値がページの残りとして保存されないよう、`PRAGMA secure_delete` を常に `ON` にする。
  忘却と全削除の後は `VACUUM` してから保存する。
- v1 の退避を削除の範囲に入れるのは、残すと破損時の移し直しで消した学習が戻るためである。
- v2 の破損時の複写を全削除の範囲に入れるのは、複写が `typo_events` を含む DB 全体を持ち、残すと全削除の後も
  学習が残るためである。忘却は複写に及ばない。暗号化した複写から 1 組だけを除くことはしない。

#### バックアップと export

backup の entry は `docs/learning-data-management-spec.md` §5・§5.1 が持つ。v2 の読みの組は item
`typo_corrections`（v1 の TSV）に、パターンは item `typo_patterns` に書き、`typo_events` は入れない。
どちらの item もテキストとして解析し、archive のバイト列を SQLite として開かない。

「v1 のまま読む」起動では item `typo_corrections` だけを書き、import の item `typo_patterns` は取り込まずに
skipped に数える。

#### 保持上限

`typo_events` と `typo_reading_pairs` は、それぞれ次の早く当たる方で古い行から消す。保存の直前に当てる。
印の無い v2（移行の検証の前）には当てない。

- 期間: 180 日（`created_at` / `last_updated_at` から。v1 の `Lookup` の期限と同じ）
- 件数: 10000 行（`docs/user-learning-enhancement-spec.md` §3.1.1 の `learning_max_records` と同じ）

`typo_patterns` は件数だけを上限（10000 行）とし、`updated_at` の古い行から消す。消すパターンの
`pattern_id` を持つ `typo_events` の行も消す。backup の import の merge も、表が上限に達した後の新しいキーは
取り込まずに skipped に数える（既にあるキーとの merge は続ける）。

どれも初期値であり、M52 で校正する。

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
| `accept_weight` | 0.25 | logit 増分。bonus 無しでは採用約 4 回で `minConfidenceForRanking`（0.70 ≈ logit 0.847）相当。ただし warm-up は confidence ではなく件数ゲート（§12.11、`accept_count >= typo_min_count`）で担保する。 |
| `reject_weight` | 0.45 | 拒否の logit 減分。採用の約 1.8 倍。**拒否を採用より強く**し、1 回の誤補正拒否を 2 回弱の採用で相殺する。 |
| `recency_half_life` | 60 日 | `recency_bonus` の減衰時定数。`docs/user-learning-enhancement-spec.md` §5（打ち間違えパターン 60 日）と一致させる。 |
| `B_rec` | 0.30 | `recency_bonus` の振幅（logit）。放置パターンを最大 0.30 減点。 |
| `B_app` | 0.20 | app 一致時の小さな加点（logit）。app をまたいだ過剰一般化を防ぐ弱さ。 |
| `max_confidence` | 0.95 | 上限。常に「もしかして」で覆せる余地を残す。 |
| `min_confidence` | 0.05 | 下限。完全な 0 にせず再学習の余地を残す。 |

- **拒否の重みを採用より強くする**。誤補正は IME 体験を大きく壊すため、学習は
  保守的に行う。
- **warm-up は confidence ではなく独立した件数ゲートで担保する**。`recency_bonus`
  （直近 +0.30）と `app_specific_bonus`（同 app +0.20）が加わると、採用 2 回でも
  logit = `2×0.25 + 0.30 + 0.20 = 1.0` → `sigmoid ≈ 0.73` となり、bonus だけで
  `minConfidenceForRanking`（0.70）を越えてしまう。confidence の値に warm-up を
  依存させない。代わりに §12.11 の **件数ゲート `accept_count >= typo_min_count`
  （既定 3）を confidence と独立した必須条件**として課し、bonus の有無に関わらず
  3 回の採用実績を経るまで `rank` / `aggressive` に昇格させない。confidence は
  順位付けの強さを表し、件数ゲートは昇格可否を表す（役割を分離する）。
- `dominant_app(pattern)` は当該パターンの `typo_events`（§12.8）で
  `event_type='typo_accept'` が最多の `app_name`。同数・該当なしは bonus 0。
- `last_used_at` は採用（`typo_accept`）で更新し、拒否（`typo_reject`）では更新しない。拒否で
  `recency_bonus` が上がらないようにするためである。採用の無いパターン（`last_used_at` が NULL）は
  `recency_bonus` を 0 とする。
- `typo_patterns.confidence` 列には `app_specific_bonus` を含めない値を、イベントと import のたびに求め直して
  持つ。app の加点は、問い合わせの時点の app で加える。
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
  `dictionary_hit_score × context_score` で定義する。§12.5 の生スコア `r1`
  （converter の生の点数で、符号も値域も保証されない）とは別物で、こちらは
  「元 top1 がどれだけ強いか」を `[0,1]` で測る。元候補なし / `r1 <= 0` のときは
  `s1_strength = 0`（元 top1 は強くない）とする。M57 文脈スコア未使用時は
  `context_score = 1.0` フォールバック。
- 係数 `P_*` / `S_STRONG` は初期値であり M52（`typo_overcorrection_rate`）で
  校正する（§12.16）。
- Host は `dictionary_hit_score` を 1（§12.4.4 で語のある仮説だけが残る）、`s1_strength` を元の top1 が辞書の語なら 1、
  そうでなければ 0 で近似し、M52 の校正（§12.16、DEV-2150）で見直す。`context_score` と `app_factor_typo` は 1.0 とする。
  個人パターンの confidence（§12.2）は、照会の時点の §12.9 の値で、app bonus を含めない。

### 12.11 誤補正防止条件

§12.10 の `typo_score` に加え、**順位の上限（rank cap）** を次の閾値で課す。
補正候補を第一候補に昇格できるのは `aggressive` モードかつ全ガードを満たす
ときのみとする。

| 条件（しきい値） | 処理 |
|---|---|
| warm-up 未達（`accept_count < typo_min_count`、既定 3） | 個人パターンを `rank` / `aggressive` に昇格させない（confidence・bonus とは独立の必須ゲート） |
| 元 top1 が十分強い（`s1_strength >= S_STRONG`、既定 0.85） | 補正候補を元 top1 より上にしない（rank cap = 2 位以下） |
| 補正 confidence が低い（`typo_confidence < minConfidenceForRanking`、0.70） | 「もしかして」枠のみ（rank 非混在） |
| top 候補化の信頼不足（`typo_confidence < minConfidenceForTopCandidate`、0.90） | 第一候補にしない（**`typo_score` ではなく `typo_confidence` で判定**） |
| ユーザーが過去に拒否（`net_reject >= 1`） | `overcorrection_penalty` で強く減点（§12.10）し top 化不可 |
| 入力が短すぎる（`Utf8CharLength(reading) <= LEN_MIN`、既定 2 = 2 文字以下） | 補正しない（§12.5 で生成前に遮断） |
| M46 で secure と判定された入力 | 補正・学習ともに無効（§12.12.1 のゲートで遮断） |
| コード入力中（M48 profile = code） | 英字・ローマ字補正を控えめにする（発動ゲートに `code` 抑制を加味） |

- **`typo_score` と昇格ゲートの役割分離**: `typo_score`（§12.10）は候補の
  **順位付け**にのみ使い、昇格可否のしきい値判定には使わない。`typo_score` は
  capped confidence（`max_confidence = 0.95`）に `reading_similarity_score`
  （= `1 - 1/編集距離長`、実 typo では常に < 1）を掛けるため構造的に上限が低く、
  例えば 1 編集・長さ n の補正は他因子が完璧でも `0.95 × (1 - 1/n)` までしか
  伸びず、`n < 19` だと 0.90 に決して届かない。これを top 化ゲートに使うと
  通常長（3〜8 文字）の typo が `aggressive` でも昇格不能になる。したがって
  ランキング閾値（0.70）/ top 化閾値（0.90）は **`typo_confidence`**（学習で
  0.95 まで到達しうる、パターンの信頼度）に対して評価する。`reading_similarity`
  は順位付け（`typo_score`）にのみ効かせ、昇格可否は塞がない。
- `minConfidenceForRanking`（0.70）/ `minConfidenceForTopCandidate`（0.90）は
  設定スキーマ §12.14 の同名フィールドと一致させ、ここで再定義しない。両者は
  `typo_confidence` に対するしきい値である。
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
各操作を次のトークンに写す（prev は直前の文字、なければ '^'）:
  insertion(c, prev)      → "<c>_insert_after_<prev>"   例 "j_insert_after_u"
  deletion(c, prev)       → "<c>_delete_after_<prev>"
  substitution(a → b)     → "<a>_sub_<b>"               例 "z_sub_x"
  transposition(a, b)     → "<a><b>_swap"               例 "ts_swap"
observed_pattern  = 上記トークンを連結（複数編集時は '+' 区切り）
intended_pattern  = observed_pattern の各編集を補正の向きに反転したトークン列
```

- prev は、編集の直前に打ったキー（読みの側の仮説では直前の文字）とする。同じキーが続く中の余分と抜けは、
  連なりの最後に置く。`kiggou`（意図は `kigou`）は `g_insert_after_g`、`ketei`（意図は `kettei`）は
  `t_delete_after_t` になる。
- 反転は insertion と deletion を入れ替え、`<a>_sub_<b>` を `<b>_sub_<a>` に、`<a><b>_swap` を
  `<b><a>_swap` にする。`j_insert_after_u` の intended_pattern は `j_delete_after_u` である。
  observed_pattern が決まれば intended_pattern も決まる。
- 読みの側の仮説（§12.3 の表の組の置き換え）は、かなのトークン（`か_sub_が`）で表す。

プライバシー上の不変条件:

- トークンは **1 文字 + 直前 1 文字の局所コンテキスト**のみを含み、単語全体や
  原文の読みを復元できない。
- 抽象化アルファベットは `[a-z]` 打鍵・かな・ローマ字正規化記号に限定する。
  この集合外の文字（数字・記号・貼り付け由来の任意文字列）を含む `raw_keys`
  は **抽象化せず破棄**し、パターン化しない。これによりパスワードや貼り付け
  文字列を誤って取り込まない。
- `left_context` は §8 と同じく SHA-256 上位 4 bytes の hash で保存する
  （`docs/user-learning-enhancement-spec.md` §8.1 と同一算出式）。
- ストア（`learning/` の `TypoLearningStore`）は、`observed_pattern` と `intended_pattern` の両方について、
  上のトークンの文法、§12.13 の `raw_keys` の文字集合、編集 3 つまでという長さを検査し、外れた値を保存しない。
  補正エンジン（§12.4）は、1 つの仮説の編集を 2 つまでとし、`intended_pattern` を上の反転で作る。

#### 12.12.2 M46 secure 抑止との連携タイミング

M46 の実効 secure 判定に基づく抑止は **検出時点で評価する fail-closed**
とし、TIP（検出）と host（蓄積・適用）の二段で遮断する。M46 のプライバシー判定の
判定主体と二段ゲートの分界は `docs/privacy-and-secure-input-spec.md` §5.1.1 を
正典とし（前面アプリ由来の判定は TIP 側、host は fail-closed の二次ゲート）、
本節はそれを M55 の経路へ適用する。

**protocol v1 の学習イベント契約**: TIP は secure 判定をイベント時に評価し、
`learning_allowed` を `!secure`、グローバル privacy の学習許可、app profile の
private 禁止、学習経路固有の条件から決める。batch では学習対象でない確定を除外する。

Host は `ObserveTypo` / `CommitObservation` / `CommitSegmentsObservation` ごとに、
受理済み接続の `secure_flag`、非 secure かつ学習許可の Host 設定、当該イベントの
`secure == false` と `learning_allowed == true` をすべて要求する。
欠落・型不正は安全側の既定値で学習を拒否する。直近の `QueryCandidates` の
フラグを観測イベントへ流用しない。TIP は secure 遷移で保留観測をクリアする。

**M55 の補正適用に関する設計契約**: 補正候補の `Lookup` は学習と別軸とし、
private の学習停止を理由に予測を停止しない。secure 中は検出スナップショット、
`raw_keys` 送信、補正適用を停止する。M55 の適用ゲートは当該要求の secure 判定を
消費する経路として定義する。Host の候補要求処理は、`QueryCandidates.secure` が true の要求（欠落・型不正を
含む。§12.13）を、v1 の読みの組も含めて補正せず、その `raw_keys` も使わない。Host の `privacy_mode_secure` が
立っている間は、どの要求も補正しない。`QueryLiveConversion` と `QueryBatchConversion` も、それぞれの要求の `secure` で
同じに扱う。`learning_allowed` は補正適用の判定に用いない。要求の secure で補正を止める
この規則は、Host が要求の secure を補正の判定に用いず TIP が補正読みを捨てるとした DEV-1956 の決定を、
DEV-2149 で改めたものである。

privacy 対応 TIP は handshake に `secure_flag` を広告し、毎 `QueryCandidates` /
`ObserveTypo` / `CommitObservation` / `CommitSegmentsObservation` に
`secure` と `learning_allowed` を載せる。学習ゲートは広告済みでも欠落・型不正を拒否する。

v2 のストアは、呼び出し側が secure の間に書き込みを止める入口を持つ。止めている間は、読みの組の観測と
パターンのイベントを拒み、保存はファイルに触れずに失敗する。忘却と全削除は入力を記録しないので止めず、
通常どおり成功・失敗を返す。このとき書くのはメモリ上の DB の写し全体なので、止める前に観測して未保存のままの
内容と、止めている間に import した内容も保存される。止めている間に拒んだ観測はメモリに入らないので含まない。
Host の学習ゲートに重ねる最後の段で、ゲートの代わりにはしない。起動時の読み込み（§12.8 の移行を含む）と
import の merge は学習ではないので止めない。import の内容は、止めている間は保存されない（応答は `io`）。
Host はこの入口を、公開した `privacy_mode_secure`（解析できない `settings.json` で fail-closed に立つものを含む）に
連動させ、起動時と `UpdateConfig` のたびに当てる。この書込みの停止は要求ごとの `secure` には連動させない。
要求ごとの `secure` は補正の適用を止める判定であり（上の「M55 の補正適用に関する設計契約」）、書込みの停止とは別である。
止める前に観測して未保存のままの内容と、止めている間に import した内容は、解除の後に保存する。
`learning_flush_interval_sec` が 0 なら解除の時点で保存し、それ以外は flush worker がその間隔の後に保存する。
止めたまま Host が終了したときは保存せず、その内容は失われる（`docs/learning-data-management-spec.md` §11）。

### 12.13 IPC（protocol v1 の加算的拡張）

v1 の `ObserveTypo` IPC に加え、既存 `QueryCandidates` の payload に
optional フィールドを追加する（エンベロープ schema 自体は変更しない）。payload の正典は
`ipc/include/azookey/ipc/Payloads.h` の `QueryCandidatesRequest` である（wire key は C++ のメンバ名と同じ）。
次の例は optional フィールドをすべて並べたもので、TIP の要求は `typo_correction_mode` を持たない（下記）:

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
    "secure": false,
    "learning_allowed": true
  }
}
```

| フィールド | 型 | 意味 | 欠落時・不正時 |
|---|---|---|---|
| `raw_keys` | string | 打鍵したキーの列（ローマ字入力は小文字の英字）。TIP が送るのはローマ字入力の打鍵だけ。ひらがなは Host が受け取れる値で、かな入力の打鍵は送らない | 空。補正の手がかりを持たない（v1 動作） |
| `typo_correction_mode` | string（`off` / `suggest` / `rank` / `aggressive`） | 要求が求める補正モード（§12.6） | 空。Host 設定の実効値（§12.14）に従う（v1 動作） |

両フィールドは optional で、空なら wire に出さない。`MessageType` は既存 `QueryCandidates` のまま
再利用し、新規 enum 値は追加しない。M40 互換性ルールに従い、**`raw_keys` /
`typo_correction_mode` が**未指定のときは v1 動作に fallback する。

- **`raw_keys` の境界検証**: Host の parse（`ParseQueryCandidatesRequest`）は、`"secure": false` を
  明示した要求の `raw_keys` だけを残し、`secure` の欠落・型不正・`true` の要求では捨てる。
  `kMaxRawKeysBytes`（256 バイト）を超えるものと、§12.12.1 の文字集合（`a`〜`z`、`-`、`'`、
  ひらがな U+3041〜U+3096、長音符 U+30FC）の外の文字を 1 つでも含むものは、切り詰めずに全体を捨てる。
  送り手の codec（`BuildQueryCandidatesRequest`）も、secure の要求には `raw_keys` を載せない。
- **capability**: Host は Handshake 応答の capabilities に `raw_keys` を入れる。TIP は `raw_keys` を
  広告した Host にだけ `raw_keys` を送り、secure の入力では送らない（§12.12.2）。capability は要求を
  組み立てる時点の接続の Handshake で判定する。
- **TIP が送る値**: 打鍵ごとの `QueryCandidates` と変換窓の `QueryCandidates` に、打鍵したキーを小文字に
  して載せる（`QueryLiveConversion`・`QueryBatchConversion`・絵文字 trigger の要求には載せない）。キーが
  読みを綴る間だけで、カスタムローマ字表、文字集合の外（句読点は打った `,` などを保つので外れる）、上限を
  超えるときは送らない。TIP は `raw_keys` をログに出さない。
- **`typo_correction_mode` の値**: 4 値以外（v1 の `auto_replace` を含む）と文字列以外は未指定として parse する。
  TIP は設定を参照しない（§3）ので、TIP の要求はこのフィールドを持たない。
- **要求値は Host の実効値を上回らない**: Host は `typo_correction_mode` を補正の判定に使わず、Host 設定から
  決まる実効値（§12.14）で変換する。`raw_keys` は、実効値が `rank` か `aggressive` の要求で §12.4.1 の打鍵の
  経路にだけ使い、補正のモードを変えない。要求値を使う経路を足すときも、要求値で補正を強めない。Host の
  実効値が `off`（設定または SafeMode）のときと secure の要求のときは、要求値は Host の実効値を変えない。
- **先頭文節の要求**: 残りの読みを持つ先頭文節の要求は、Host の実効値によらず補正しない（`docs/segment-editing-spec.md` §5.3）。
- **互換**: protocol version は 1 を維持する。

| 組み合わせ | 挙動 |
|---|---|
| 新しい TIP と古い Host | 古い Host は capability `raw_keys` を広告しないので、TIP は `raw_keys` を送らない。古い Host は両フィールドを未知のキーとして読み飛ばす |
| 古い TIP と新しい Host | 古い TIP は両フィールドを送らない。Host は空として parse し、打鍵の経路の仮説を作らずに Host の実効値（§12.14）で変換する。`secure` を載せない要求は secure として parse するので、補正しない（§12.12.2）。`secure` を載せない `QueryLiveConversion` / `QueryBatchConversion` も同じ |
| 新しい TIP と新しい Host | TIP は secure でない要求に `raw_keys` を載せる。Host は境界検証を通ったものだけを保持し、実効値が `rank` か `aggressive` のとき打鍵の経路の仮説に使う |

> privacy フラグの欠落・型不正は `secure = true` /
> `learning_allowed = false` として parse する。これは parse エラーではない。
> 学習イベントの Host ゲートはこの値で拒否する。候補要求のフラグ伝達と
> M55 補正適用の責務は §12.12.2 の区別に従う。

- 両フラグは protocol v1 の加算的フィールドである。Host の候補要求処理では
  `secure` だけを補正適用の判定に使う（§12.12.2）。学習イベントでは当該イベントの値を検査する。
- **フィールド存在規約**: `secure_flag` を広告する TIP は毎 `QueryCandidates` /
  `ObserveTypo` / `CommitObservation` / `CommitSegmentsObservation` に
  `secure` と `learning_allowed` を載せる。
  フィールド欠落は capability 広告の有無だけで推定せず、当該要求ごとに検査する。
  欠落・型不正時の既定は `secure = true` / `learning_allowed = false` とする。
  Host の学習許可は受理済み接続の `secure_flag` と両フラグの許可を必要とする。

M55 の `raw_keys` 抑止・補正適用停止は §12.12.2 の設計契約に従う。

訂正候補だけを問い合わせる `QueryCorrections`（`MessageType` の値だけがあり、Dispatcher のハンドラを
持たない。`docs/ipc-persistence-hardening-spec.md` §6.3）は M55 が所有する（DEV-1959）。上の
`QueryCandidates` の加算的フィールドは補正の手がかりを運ぶだけで、`QueryCorrections` の payload と
ハンドラは M55 の補正エンジンの実装で定める。

#### `ObserveTypoRequest` への per-event `secure` / `learning_allowed`

`ObserveTypo` は fire-and-forget で対応する `QueryCandidates` を持たないため、
§7 の v1 `ObserveTypoRequest` が持つ `secure = true` と
`learning_allowed = false` のイベント単位の契約を v2 でも維持する。
同じイベント単位の契約を `CommitObservationRequest` と
`CommitSegmentsObservationRequest` にも適用する。protocol version は 1 を維持する。
古い TIP と新しい Host では学習を拒否し、新しい TIP と古い Host では TIP の抑止を維持する。
`ObserveTypoRequest` のフィールドと欠落時の既定は §7 の表のとおりである。

- `secure` / `learning_allowed` は当該イベントで判定した値であり、
  TIP は `!secure`、private / custom の学習許可、学習経路の条件を使う。
- Host は受理済み接続の `secure_flag`、非 secure かつ学習許可の Host 設定、
  `secure == false` かつ `learning_allowed == true` で蓄積可否を判定する。
  直近の候補要求から推定しない。
- 欠落・型不正は parse 成功と安全側の既定値に解決し、学習ゲートで拒否する。

### 12.14 設定スキーマ拡張

v2 は nested `typoCorrection.*` に集約する。型・既定・範囲の正典は `settings/mvp-settings.schema.json` の
`typoCorrection` であり、v1 の root `typoCorrectionMode` / `typoMinCount`（§8）も残す:

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

| キー | 既定 | 意味 | 不正な値のとき |
|---|---|---|---|
| `typoCorrection.enabled` | `true` | false は `mode` によらず補正も学習もしない（実効値 `off`） | 既定 |
| `typoCorrection.mode` | `suggest` | §12.6 の 4 モード。あれば root の `typoCorrectionMode` より優先する | 未設定と同じ（下の migration） |
| `typoCorrection.learnPersonalPatterns` | `true` | false は `ObserveTypo` の蓄積だけを止め、学習済みの補正は使い続ける | 既定 |
| `typoCorrection.keyboardLayout` | `auto` | §12.4.2 の配列。値は `auto` だけ | 既定 |
| `typoCorrection.maxReadingHypotheses` | `3` | 1 つの読みから作る §12.2 の補正 hypothesis の上限（1〜8）。§12.4.4 の辞書の制約の後に数える | 既定 |
| `typoCorrection.minConfidenceForRanking` | `0.70` | §12.5・§12.11 のしきい値（0〜1） | 既定。2 つの大小が逆（等しいを含む）なら両方を既定 |
| `typoCorrection.minConfidenceForTopCandidate` | `0.90` | §12.11 のしきい値（0〜1） | 同上 |
| `typoCorrection.disableInSecretApps` | `true` | M55 の適用ゲートが secure の入力で補正を止める（§12.11・§12.12.2）。schema は `false` を拒否する。Host の候補要求処理はこのキーを読まず、secure の要求と `privacy_mode_secure` の間は常に補正を止める（§12.12.2） | Host は `false` を読んでも `true` として扱う |

信頼度の 2 キーを除き、各キーは単独で既定へ戻り、不正な値が他のキーを巻き込まない。`typoCorrection` が
オブジェクトでなければ全体を無いものとして扱う。

**v1 互換性**: v1 設定 `typoCorrectionMode`（root レベル、3 値 enum）を持つ
設定ファイルを読む。`SettingsStore` は読み込みのたびに次の順で `typoCorrection.mode` を決める:

1. v2 の `typoCorrection.mode` が 4 値のどれかならそれを採用
2. v2 が未設定・空・不正で、v1 root `typoCorrectionMode` が 3 値のどれかなら値を読み、
   `auto_replace` を `aggressive` に読み替えて採用（§12.6 と整合）
3. 両方未設定なら既定値 `suggest`

`typoCorrection.enabled` が false なら、決まった mode によらず実効値は `off` とする。SafeMode
（`docs/host-availability-spec.md` §8.5.3）は migration の後で `enabled` / `mode` / `learnPersonalPatterns` を
止める側へ上書きするので、どちらのキーからも補正は戻らない。

**engine への反映**: Host の `ApplyRuntimeSettingsToEngineConfig` は、実効値の 4 値をそのまま
`EngineConfig::typo_correction_mode` に渡し、`maxReadingHypotheses` と信頼度の 2 キーも渡す。engine は v1 の
`auto_replace` を `aggressive` と読む。v1 の読みの組を、`rank` は `suggest` と、`aggressive` は `auto_replace` と
同じに当てる。`rank` と `aggressive` は、v1 の読みの組が読みを置き換えないとき、§12.4 の仮説も作る（§12.7）。

**書き出し**: migration は読み取りだけで、Host は settings.json を書き戻さない。v1 key を残したまま
（破壊的削除はしない）両方を書くのは設定アプリである。「辞書」ペインはファイルが解決する実効値を
v1 の 3 モードで示し（`rank` は `suggest`、`aggressive` は `auto_replace`、`enabled` が false なら `off`）、
ペインでモードを変えた保存では root の `typoCorrectionMode` と `typoCorrection.mode`（`auto_replace` は
`aggressive`）の両方を書く。root はキーの書き込み規則（`docs/settings-app-spec.md` §3.7）に従い、
ファイルに無く既定値（`suggest`）なら書かない。古い Host もそれを `suggest` と読む。選んだモードが
`typoCorrection.mode` の v2 の値と同じ v1 の値（`rank` に対する `suggest` など）なら、v2 の値を残す。
`off` 以外を選んだときは、`typoCorrection.enabled` の false も true に戻す。
モードを変えない保存は `typoCorrection` をそのまま残すので、ペインの外で設定した `rank` は消えない。
「辞書」ペインは、モードのほかに `typoCorrection.learnPersonalPatterns`（スイッチ）を編集する。これは
`typoCorrection` の中に書き、既にファイルにあるか既定と違うときだけ書き、ほかの下位キーはそのまま残す。
`learnPersonalPatterns` は Host の `ObserveTypo` の蓄積を今すぐ変える（`SettingsStore` が `typo_learning` を決め、
`HandleObserveTypo` が止める）。
`typoCorrection.enabled` はペインに専用のスイッチを持たず、false は `off` として示す。`rank` / `aggressive`、
`maxReadingHypotheses`、`minConfidenceForRanking` / `minConfidenceForTopCandidate` は、ペインに出さず、保存で残す。
v1 root key は M55 を含むリリースから 3 マイナーバージョンの間は読み込み、
その後のリリースで削除する（deprecation warning を CHANGELOG に記載）。
M48 §6 の `promptPrefixByApp` と同じ段階的廃止ポリシー。

### 12.15 M55 受け入れ条件

受け入れ条件の定義の正典は [`plans/windows-port-roadmap.md`](../plans/windows-port-roadmap.md)
の M55 節とする。本書は補正の判定式・定数・スキーマ・v1 からの読み替え規則を定義し、
受け入れ条件を複製しない。

**実装上の留意点**: typo 補正指標は補正有効モードで採取する
（`conversion-quality-benchmark-spec.md` §7・§14）。`--typo-mode off` での値は受け入れにも
§12.16 の校正にも用いない。baseline はモード別に管理する。

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
| `personal_cost_reduction`（§12.4.1） | 0.5 | `0 <= personal_cost_reduction <= 1` | top5 accuracy（rank） |
| `HYP_MAX_READING` / `HYP_MAX_KEYS`（§12.4） | 10 / 20 | `HYP_MAX_READING > LEN_MIN`。読みがこのコードポイント数を、または `raw_keys` がこのキー数を超えると、`rank` / `aggressive` でも仮説を作らず v1 の読みの組だけを引く。設定キーではない | 候補表示の p95 と top5 accuracy（rank） |

- **受け入れ判定に用いる指標**: `typo_correction_top5_accuracy`（rank）/
  `typo_false_positive_rate`（rank）/ `typo_overcorrection_rate`（aggressive）。
  閾値は roadmap M55 の受け入れ条件を正典とする（§12.15）。
- 校正は係数の数値のみを動かし、本表の不変条件・式の形・モード別の rank cap
  （§12.11）は固定する。SQLite/TSV スキーマ（§12.8）を変える結果が出た場合は
  M55 範囲外とし別 M で扱う。
