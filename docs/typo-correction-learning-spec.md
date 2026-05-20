# 個人タイプミス学習・自動修正 仕様（Typo Correction Learning）

本書は「ユーザー自身がよくする打ち間違い（タイプミス）を学習し、同じタイプミスを
したときに正しい入力へ変換・提示する」機能の仕様を定める。対象は **Windows 版
（C++ 移植: `core/` `learning/` `ipc/` `inference-host/` `tsf-tip/`）** のみ。
macOS 版 `legacy/` は対象外。

対応マイルストーン: `plans/windows-port-roadmap.md` M35。
関連: `docs/rich-features-spec.md` X-3（誤変換訂正）。

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

- 設定ローダーは未実装（`settings/mvp-settings.schema.json` はスキーマ文書のみ）。
  当面の実効値は次の経路で受ける:
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

1. ビルド: `cmake -S . -B build -DAZOOKEY_BUILD_TESTS=ON && cmake --build build`
2. テスト: `ctest --test-dir build --output-on-failure`
   （`typo_correction_store_tests` / `payloads_test` / `engine_test` /
   `dispatcher_test` が green であること）
3. host を `--typo-mode suggest` / `auto_replace` で stdio 起動し、同一
   `wrong_reading` の `ObserveTypo` を 3 回送ってから `QueryCandidates` を投げ、
   suggest で `typo-correction` 候補が注入され、auto_replace で
   `corrected_reading` が返ることを確認。
4. Windows 実機があれば TIP を導入し `AZOOKEY_TYPO_MODE` を設定して、未確定中
   backspace 訂正・確定直後打ち直しを実操作で確認。実機が無い場合はその旨を
   明示し、自動テストとプロトコルレベル確認で代替する。
