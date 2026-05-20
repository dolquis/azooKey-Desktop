# Rich Features 仕様（横断テーマ X-1〜X-4）

本書は、ライブ変換 / AI 予測 / 誤変換訂正の 3 機能を「単なる移植」ではなく
azooKey-Desktop の **差別化軸** に仕立てるためのリッチ化仕様を定める。

- Phase 5 末尾 = 短期実装
- Phase 6 統合 = 中期実装
- Phase 7 完走後 = 長期実装

各テーマは `plans/windows-port-roadmap.md` の対応するマイルストーンから参照する。

## X-1. ライブ変換のリッチ化

### X-1-1. 信頼度階層 4 段階 DisplayAttribute

#### GUID 設計

`tsf-tip/src/DllMain.cpp` に 4 新規 GUID を追加：

```cpp
// {AAB0...}: 高信頼ライブ変換結果 (score >= 0.9) — 黒・実線下線
constexpr GUID kLiveAttrConfidentGuid       = { 0xAAB0..., ... };
// {AAB1...}: 中信頼 (0.7..0.9) — 濃グレー・実線下線
constexpr GUID kLiveAttrLikelyGuid          = { 0xAAB1..., ... };
// {AAB2...}: 低信頼 (0.5..0.7) — 薄グレー・実線下線
constexpr GUID kLiveAttrUncertainGuid       = { 0xAAB2..., ... };
// {AAB3...}: 仮確定なし (< 0.5) — 既定下線（kInputAttributeGuid と同等）
constexpr GUID kLiveAttrTentativeGuid       = { 0xAAB3..., ... };
```

GUID 実値は M14 着手時に `uuidgen` で確定し、本書に追記する。

#### EnumDisplayAttributeInfo 拡張

`tsf-tip/src/DisplayAttributeProvider.cpp`（M3 で実装済）の enumerator に
4 新規エントリを追加。`ITfDisplayAttributeInfo::GetAttributeInfo` の返却内容：

| GUID | TF_DA_COLOR_TYPE | rgb | TF_DA_LINESTYLE | bAttr |
|---|---|---|---|---|
| Confident | RGB | 0x000000 | TF_LS_SOLID | TF_ATTR_INPUT |
| Likely | RGB | 0x404040 | TF_LS_SOLID | TF_ATTR_INPUT |
| Uncertain | RGB | 0x808080 | TF_LS_SOLID | TF_ATTR_INPUT |
| Tentative | RGB | 0xA0A0A0 | TF_LS_DOT | TF_ATTR_INPUT |

#### 段階分け閾値

| score | 段階 |
|---|---|
| ≥ 0.9 | Confident |
| 0.7 ≤ s < 0.9 | Likely |
| 0.5 ≤ s < 0.7 | Uncertain |
| < 0.5 | Tentative |

スコアは `QueryLiveConversionResponse.confidence`（0.0..1.0）で受け取る。
Zenzai 多 pass 推論時は最良候補の対数尤度を 0..1 に正規化（softmax 温度 1.0、
top-2 候補との差を基準）。

#### 文節単位の混在

文節（segment）ごとに異なる属性を割り当てる。`ITfRange` を文節境界で分割し、
`ITfProperty::SetValue` を文節ごとに呼ぶ。

文節境界は `QueryLiveConversionResponse.segments[]` で返す：

```
QueryLiveConversionResponse:
  request_id
  surface           // 文字列全体
  confidence        // 全体スコア
  segments[]: { start_char, end_char, score }
```

### X-1-2. TypingTempoTracker

`tsf-tip/src/TypingTempoTracker.h`（新規）：

```cpp
class TypingTempoTracker {
public:
    void RecordKeyDown(uint64_t now_ms);
    // 直近 N=8 キーストロークのインターバル平均（ms）
    double AverageIntervalMs() const;
    // true: タイピング中 (平均 < threshold)
    bool   IsTyping(double threshold_ms = 150.0) const;
private:
    std::array<uint64_t, 8> recent_ms_{};
    size_t                  count_  = 0;
    size_t                  head_   = 0;
};
```

- 平均 < 150ms = 「タイピング中」→ 軽量推論（SimpleConverter or Zenzai 1-pass）
- 平均 ≥ 150ms = 「思考中」→ 重い推論（Zenzai 多 passes）

`TextService::OnKeyDown` で `RecordKeyDown(GetTickCount64())` を呼ぶ。
`PostQueryLiveConversion` 時に `mode = tempo.IsTyping() ? Fast : Heavy` を
リクエストに含める。

新規 IPC フィールド：

```
QueryLiveConversionRequest:
  request_id, kana, context,
  mode: enum { Fast, Heavy }
```

Host 側スケジューラ：

- `Fast` レーン: SimpleConverter or Zenzai n_predict=8
- `Heavy` レーン: Zenzai n_predict=32 + 多 pass beam search

両レーンは X-4 「多段推論キュー」で管理。

### X-1-3. 後戻り再評価のトリガ条件

ユーザーが入力を止めた瞬間に、直近 commit を含めた全体を再評価して
より良い候補があれば差し替える。

#### トリガ条件

すべて満たすこと：
- 直近 commit から **5 文字以上経過**
- 直近 **5 キー未編集（無入力）が 800ms 以上継続**
- `settings.retroactiveRecompute == true`（既定 false、実験機能）

#### 動作

1. `TypingTempoTracker::IdleMs() >= 800` を検知
2. 直近 commit を含む文（句点 `。` or 改行までの範囲）を抽出
3. `PostQueryFullRecompute(request_id, paragraph)` を送信
4. Host が再評価した結果と現状の差分があれば、
   - 既存 commit 範囲は **読み取り専用波線**（X-3 Post-Commit Lint と統合）
   - composition 範囲は EditSession で差し替え

#### IPC

`QueryFullRecomputeRequest` / `Response` を新規追加（X-3 Post-Commit Lint と
内部実装を共有してよい）。

## X-2. AI 予測のリッチ化

### X-2-1. QueryPredictions.mode 拡張

既存 `QueryPredictionsRequest` の `mode` フィールドを下記 enum に拡張：

```cpp
enum class PredictionMode : uint8_t {
    Word     = 0,   // 単語予測（既定）
    Phrase   = 1,   // フレーズ予測（〜10 文字）
    Sentence = 2,   // 文単位予測（〜50 文字、LLM 必須）
};
```

| mode | 候補長 | バックエンド | 想定用途 |
|---|---|---|---|
| Word | 1〜6 文字 | SimpleConverter 辞書 | 速いタイピング補助 |
| Phrase | 〜10 文字 | Zenzai 軽量 (1-pass) | 慣用句・連語 |
| Sentence | 〜50 文字 | Zenzai 多 pass / LLM | メール定型・コード |

トリガ：

- Word: 全 composition 中（常時）
- Phrase: composition の末尾が「。」「、」「\n」直後 or 段落頭
- Sentence: 確定直後 + `settings.sentenceCompletion == true`

### X-2-2. paragraph_context 拡張

既存 `context` フィールド（左 1 文字）を **直近 N=512 字** に拡張。

取得方法：

1. `ITfContext::GetEnd` でカーソル位置を取得
2. `ITfRange::ShiftStart(read_cookie, -512, &shifted, nullptr)` で 512 字遡る
3. 段落境界（`\n` `。` `。`）に当たったらそこで打ち切り
4. `GetText` で UTF-16 を取得し、UTF-8 に変換して Payload に含める

新規 IPC フィールド：

```
QueryPredictionsRequest:
  request_id, kana, paragraph_context, mode, app_id
```

`app_id` は ForegroundAppDetector が返す `process_name`（X-4-2）。

### X-2-3. ラベル付き候補

`Candidate` 構造体に `tag` を追加：

```cpp
enum class CandidateTag : uint8_t {
    None        = 0,
    Polite      = 1,   // 敬語
    Casual      = 2,   // 砕け
    Technical   = 3,   // 専門用語/識別子
    English     = 4,
    Kaomoji     = 5,   // 顔文字
    Idiom       = 6,   // 慣用句
};

struct Candidate {
    std::string surface;
    std::string reading;
    double      score;
    CandidateTag tag;
};
```

候補 UI のアノテーション列に `[敬]` `[砕]` `[技]` 等の 2 文字バッジを表示
（`PredictionWindow.cpp` / `CandidateWindow.cpp` 共通）。

IPC：`PredictionItem` / `CandidateItem` Payload に `tag: uint8` フィールド追加。

### X-2-4. PredictWithLLM（Phase 6）

`InferenceEngine::PredictWithLLM(paragraph, mode, app_id, persona)` を追加。

- バックエンド: Zenzai（M24 で DirectML / NPU 経由も）
- パラメータ:
  - `max_tokens = 64`（Sentence モード時）/ `32`（Phrase）/ `16`（Word）
  - `temperature = 0.3`
  - `top_p = 0.9`
  - `stop = ["\n\n", "。", "？", "！"]`
- ストリーミング: `stream=true`、tokens を `PredictStreamChunk` で push

### X-2-5. PredictStreamChunk push IPC

新規 push メッセージ（X-4-1 と共有チャネル）：

```
PredictStreamChunk:
  request_id: uint64
  seq: uint32          // 0 から連番
  text_delta: string   // 追加トークンの差分
  done: bool           // 最終チャンク
```

TIP 側は受信ごとに predict ウィンドウの候補テキストを末尾追記する。
途中で別のキー入力があれば `Cancel(request_id)` を送って打ち切り。

### X-2-6. アプリ別 prompt prefix

設定ファイルでアプリ実行ファイル名 → prompt prefix のマップを定義：

```json
{
  "promptPrefixByApp": {
    "Code.exe":       "コードエディタ。識別子（CamelCase/snake_case）と日本語コメントを優先。",
    "OUTLOOK.EXE":    "ビジネスメール。敬語・挨拶・締めの定型句を優先。",
    "Teams.exe":      "チャット。短いカジュアル文と絵文字を優先。",
    "WINWORD.EXE":    "文書作成。丁寧体・接続詞を多めに。",
    "powershell.exe": "シェル。コマンドと識別子を優先、日本語は最小限。"
  }
}
```

`ForegroundAppDetector` が返す `process_name` で lookup。
マッチしない場合は prefix なし（generic prompt）。

### X-2-7. ペルソナ自動推定

`LearningStore` 全件から以下の比率を算出：

- 敬語率: 「ます/です/いただく」を含む確定の割合
- カジュアル率: 「だよ/だね/じゃん」を含む確定の割合
- 技術率: 英数字 + アンダースコア識別子の割合
- 顔文字率: 顔文字（記号 3〜10 連）の割合

これを `Persona` 構造体で表現し、`PredictWithLLM` のシステムプロンプトに混ぜる：

```cpp
struct Persona {
    double polite_ratio;
    double casual_ratio;
    double technical_ratio;
    double kaomoji_ratio;
};
```

毎日 1 回（起動時 + 24h）バックグラウンドで再計算してキャッシュ。

## X-3. 誤変換訂正のリッチ化

### X-3-1. RomajiKanaConverter::FuzzyMatch

入力ローマ字 `input` に対して、上位 `K=8` 通りのかな解釈を返す API。

```cpp
struct FuzzyKanaCandidate {
    std::string kana;
    double      cost;   // 編集距離 + ペナルティ
};

std::vector<FuzzyKanaCandidate>
RomajiKanaConverter::FuzzyMatch(std::string_view input, size_t k) const;
```

#### コスト表（Damerau-Levenshtein 拡張）

| 操作 | コスト |
|---|---|
| substitution | 1.0 |
| insertion | 1.0 |
| deletion | 1.0 |
| transposition (隣接 2 文字入れ替え) | 0.5 |
| QWERTY 隣接キー substitution | 0.7（−0.3 ボーナス） |
| 同一指 substitution | 0.6（−0.4 ボーナス） |

QWERTY 隣接：`q-w`, `w-e`, ..., `a-s`, `s-d`, ... 全 ASCII 小文字ペアを
`core/src/QwertyAdjacency.cpp` に静的テーブルとして持つ。

#### 適用順序

1. Standard parse（既存）→ 1 候補
2. FuzzyMatch (K=8) → 8 候補
3. それぞれを `SimpleConverter::Query` or `Zenzai::Query` に通す
4. 全候補を `score - alpha * cost` でソート（alpha=0.5、設定で変更可）

### X-3-2. 同音異義語の文脈再選択

commit 後に短い助詞（「を / に / が / は / で / と / も」）が続いたら、
直前 commit を再判定する。

#### トリガ

- commit から **3 文字以内** に助詞が続いて再 commit
- 設定 `contextReselection = true`

#### 動作

1. 直前 commit の `reading` を保持
2. 2 回目 commit 後、`InferenceEngine::ReevaluateInContext(reading, prev_surface, full_context)` を呼ぶ
3. 差分があれば、直前 commit 範囲を `ITfRange::SetText` で差し替え
4. 同時に `LearningStore::ObserveCorrection(reading, prev_surface, new_surface)` を記録

#### 安全策

- 差し替え後 5 秒間はユーザーの Backspace で即座に元に戻る undo を維持
  （TIP プロセス内の `pending_reselection_` キュー）

### X-3-3. Post-Commit Lint

確定済みテキストに対して、軽量 LLM で「より自然な解釈」を検出して波線表示する。

#### タイミング

- commit 後 **800ms デバウンス**
- 直近 32 文字（or 段落末まで）を Zenzai に渡す

#### IPC

```
RequestPostCommitLintRequest:
  request_id: uint64
  text: string        // 直近 32 文字
  app_id: string

RequestPostCommitLintResponse:
  request_id
  findings[]: LintFinding

LintFinding:
  start_char: uint32  // text 内の開始位置
  end_char: uint32
  current: string     // 現在のテキスト
  suggestion: string  // 提案テキスト
  reason: string      // 「同音異義語」「敬語不一致」等
  confidence: float
```

#### 表示

- `ITfReadOnlyProperty`（既存 `GUID_PROP_ATTRIBUTE`）で **波線下線** 属性
  - 新規 GUID `kLintAttrGuid`（赤い波線、`TF_LS_SQUIGGLE`）
- 右クリック（ITfMouseSink、Phase 6-A）で候補一覧をポップアップ
- 採用したら `LearningStore::ObserveCorrection` を記録

### X-3-4. 意味的訂正（Phase 6）

LLM で「不自然な確定箇所」を検出する API。Phase 5 の Post-Commit Lint と
似ているが、より長文（〜段落単位）を対象に、文法 / 敬語不一致 / 主述不一致
までを検出する。

```cpp
struct AnomalyFinding {
    Range            range;
    std::string      reason;
    std::vector<std::string> suggestions;
    float            confidence;
};

std::vector<AnomalyFinding>
InferenceEngine::DetectAnomalies(std::string_view paragraph, const Persona& p);
```

### X-3-5. 訂正の学習

`LearningStore::ObserveCorrection`（既存）を以下のフローで呼ぶ：

| トリガ | reading | prev_surface | new_surface |
|---|---|---|---|
| 候補ウィンドウで再選択 | 元 reading | 旧確定 | 新確定 |
| 文脈再選択 (X-3-2) | 元 reading | 旧確定 | 新確定 |
| Post-Commit Lint 採用 | 推測 reading | finding.current | finding.suggestion |
| 意味的訂正採用 | 推測 reading | 旧 surface | 新 surface |

次回同じ `(reading, context)` で `prev_surface` より `new_surface` が優先される
ように、`LearningStore` に「correction weight」フィールド（既存）を増分。

### X-3-6. バッチ訂正ビュー

Ctrl+Shift+Space で「文書内誤変換候補一覧」を別ウィンドウに表示。

- 起動: グローバルホットキー（TIP プロセスから `RegisterHotKey`）
- 表示: 設定アプリ内の「校正」タブ（Phase 7-M30）
- 動作:
  - フォアグラウンドアプリから可能な限り文書全体を取得（`ITfContext` 全 range）
  - Host に `DetectAnomalies` を投げる
  - 一覧表示、項目クリックでアプリ側のキャレットを該当箇所に移動 + 候補提示

Phase 7 まで実装しない（Phase 5 ではホットキー登録のみ）。

### X-3-7. 個人タイプミス学習（関連機能・別仕様）

X-3 が対象とするのは「変換結果（surface）の誤り」だが、これと関連して
**ユーザー個人のかな読みレベルの打鍵ミス**を学習・補正する機能を別マイルストーン
（M35）として定義する。汎用 LM 補正（`DebugTypoCorrection`）とも独立。

詳細は `docs/typo-correction-learning-spec.md` を正典とする。本書では扱わない。

## X-4. 横断基盤

### X-4-1. 多段推論キュー

`inference-host/src/RequestScheduler.cpp` を拡張し、3 レーンの優先キューを実装。

```cpp
enum class Lane : uint8_t {
    Fast      = 0,   // SimpleConverter / Zenzai 1-pass、目標 <30ms
    Heavy     = 1,   // Zenzai 多 pass、目標 <500ms
    Streaming = 2,   // LLM 予測、ストリーミング応答
};

class RequestScheduler {
public:
    uint64_t NextRequestId();
    void     Submit(uint64_t id, Lane lane, std::function<void()> work);
    void     Cancel(uint64_t id);
    bool     IsCanceled(uint64_t id) const;
    void     MarkLatest(uint64_t id, Lane lane);
    bool     IsLatest(uint64_t id, Lane lane) const;
};
```

- 各レーンは独立スレッドプール（Fast=2, Heavy=1, Streaming=2）
- Cancel 優先: 新しい同レーン Submit が来たら、古いものは即時 Cancel
- レーン間は独立（Fast の Cancel が Heavy を停めない）

### X-4-2. 片方向 push IPC

既存 `ipc/src/NamedPipeTransport.cpp` の Envelope に `push: bool` フラグを追加。

```
Envelope:
  message_type: uint16
  request_id: uint64
  push: bool         // 新規。true なら request_id は元リクエストの ID
  payload: bytes
```

- `push=false` は既存の request/response 動作
- `push=true` は server→client の片方向通知
- `push` メッセージの種類:
  - `PredictStreamChunk`（X-2-5）
  - `LintNotification`（X-3-3 の自動 push）
  - `BackgroundLearningUpdate`（将来）

TIP 側は受信スレッドで `push=true` を検出したら `request_id` から元リクエストを
lookup し、該当ハンドラを呼ぶ。

### X-4-3. ContextTracker

TIP プロセス内で段落 / 直前文 / アプリ別履歴を保持する。

新規: `tsf-tip/src/ContextTracker.h` / `.cpp`

```cpp
class ContextTracker {
public:
    // commit 時に履歴へ追加
    void RecordCommit(std::string_view reading,
                      std::string_view surface,
                      uint64_t timestamp_ms,
                      std::string_view app_id);

    // 直近の段落（句点まで）
    std::string CurrentParagraph(ITfContext* ctx) const;

    // アプリ別の直近 commit N=20 件
    std::vector<CommitRecord> RecentCommitsForApp(std::string_view app_id,
                                                  size_t n) const;
};
```

QueryCandidates / QueryPredictions / QueryLiveConversion で `context` フィールドに
`CurrentParagraph` の結果を含める。`paragraph_context` 取得の実体はここ。

### X-4-4. ForegroundAppDetector

```cpp
class ForegroundAppDetector {
public:
    // GetForegroundWindow + GetWindowThreadProcessId + GetModuleFileNameEx
    // 500ms TTL キャッシュ
    std::string CurrentAppId();
private:
    std::string cached_;
    uint64_t    cached_ms_ = 0;
};
```

- `OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION)` で対象プロセスハンドルを取得
- `K32GetModuleFileNameExW(hProc, nullptr, ...)` で実行ファイルパス
- パスから basename を取り出し、lowercase 化（`"Code.exe"` → `"code.exe"`）

### X-4-5. bench 拡張

`bench/rich_features_bench.cpp`（新規）に以下のメトリクスを追加：

| メトリクス | 単位 | 対象 |
|---|---|---|
| live_conversion_p50_ms | ms | QueryLiveConversion (Fast) |
| live_conversion_p95_ms | ms | 同上 |
| prediction_p50_ms | ms | QueryPredictions (Word) |
| prediction_streaming_first_chunk_ms | ms | QueryPredictions (Sentence) |
| post_commit_lint_p95_ms | ms | RequestPostCommitLint |
| memory_rss_after_zenzai_load_mb | MB | Host プロセス RSS |
| battery_delta_per_hour_pct | %/h | 30 分計測ベース |

`ETW`（Phase 7-M33）と連携して、本番環境でも同じメトリクスを継続観測。

## マイルストーン対応表

| テーマ | 短期 | 中期 | 長期 |
|---|---|---|---|
| X-1 ライブ変換 | M14 + 信頼度 4 段階 (M14-末) | M24 (Zenzai 重い推論) | — |
| X-2 AI 予測 | M15 + paragraph_context + Word/Phrase | M24 + Sentence + Streaming | M30 設定アプリで prompt prefix UI |
| X-3 誤変換訂正 | M17 末 FuzzyMatch + M16 末 Post-Commit Lint | M24 後 DetectAnomalies | M30 バッチ訂正ビュー |
| X-4 基盤 | M14〜M19 で各実装 | M24 後にスケジューラ統合 | — |

## 参照

- ライブ変換 旧実装：`legacy/Core/Sources/Core/InputUtils/SegmentsManager.swift`
- 予測候補 旧実装：`legacy/Core/Sources/Core/InputUtils/PredictionEngine.swift`
- 誤変換訂正 旧実装：`legacy/Core/Sources/Core/InputUtils/CorrectionEngine.swift`
- Phase 5 仕様：`docs/legacy-parity-spec.md`
- Phase 6-A 仕様：`docs/tsf-deep-integration-spec.md`
- Phase 6-B 仕様：`docs/copilot-pc-backend-spec.md`
