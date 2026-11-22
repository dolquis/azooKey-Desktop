---
kind: spec
milestones: [M16, M58-C, X-3-3]
canonical_for: ["AiBackend 契約"]
---

# AiBackend（AI 変換バックエンド）契約 仕様

対象リポジトリ: dolquis/azooKey-Desktop
関連:
- `docs/legacy-parity-spec.md` §4（Magic Conversion の TIP 側の挙動。同 §4.4・§4.5 は本書へのリンク）
- `docs/romaji-batch-conversion-spec.md` §5 / §6(M58-C `ai-cleanup`、`raw_romaji`)
- `docs/proofread-view-spec.md`（`DetectAnomalies` を使う校正ビュー）
- `docs/user-learning-enhancement-spec.md` §4.2（訂正の学習の呼び出し元）
- `docs/privacy-and-secure-input-spec.md` §5（secure 中の挙動契約、プライバシー判定）
- `docs/app-profile-spec.md` §4.2（M48 アプリ別 backend 解決）
- `docs/sideload-packaging-spec.md` §9（M34 DPAPI）
- `plans/windows-port-roadmap.md` M16 / M32 / M34 / M46 / M47 / M48 / M58-C
- `legacy/Core/Sources/Core/MagicConversion/{AIBackend,OpenAIClient}.swift`（参照のみ）

> 本書は `inference-host/include/azookey/host/AiBackend.h` と
> `inference-host/src/AiBackend.cpp` の契約を定める正典仕様である。実装と差異が出た場合は
> 本書を先に更新する。
> 進捗・状態の正典は Linear であり、本書は状態を持たない。

---

## 0. 節とマイルストーンの対応

本書の節と、その範囲を定める `plans/windows-port-roadmap.md` のマイルストーンの対応を次に示す。

| 節 | マイルストーン |
|---|---|
| §1〜§3、§5〜§14 | M16（M58-C と X-3-3 も同じ契約を使う） |
| §4.1 | M16 |
| §4.2 | M58-C |
| §4.3 | X-3-3（roadmap M16 の横断項目） |

## 1. 目的とスコープ

### 1.1 目的

`AiBackend` は、選択テキストや確定テキスト・全文ローマ字を AI（OpenAI 互換 API
またはローカル Zenzai）に委譲して変換・整文・校正するための Host 側共有
コンポーネントである。本書は次を契約として確定する。

1. 4 消費者（M16 / M58-C / X-3-3 / X-3-6）が再利用できる **IPC payload と Host 側
   インターフェイスの抽象度**（§3, §4）。
2. **OpenAI 互換 API 呼び出し契約**（エンドポイント・認証・モデル・`mode`→
   system-prompt マッピング・`response_format`）（§5）。
3. **HTTP 実装経路**（共通 WinHTTP 基盤の契約。M32 `HttpDownloader` との分担）（§6）。
4. **エラー / リトライ / レート制限 / タイムアウト**ポリシー（M47 と整合）（§7）。
5. **secure ゲート連携**（M46 プライバシー判定。外部 AI へ渡さない保証）（§8）。
6. **API キーの at-rest 保存**（平文と `dpapi:` prefix 規約）（§9）。
7. **ストリーミング境界**（MVP `stream=false` と Phase 6 `stream=true`）（§10）。

### 1.2 スコープ外

- Foundation Models（Apple）は macOS 専用のため Windows 版では実装しない
  （`legacy/.../AIBackend.swift` の `foundationModels` ケースは移植しない）。
- ローカル LLM 推論（`aiBackend=local-zenzai`）の内部実装は M24（Zenzai 流用）の
  範囲。本書は `AiBackend` から見た **backend 抽象**と委譲契約のみを定める。
- プロンプト UI（簡易 Win32 ダイアログ）の実装詳細は `docs/legacy-parity-spec.md`
  §4.3 が正典。本書は UI から `AiBackend` へ渡る payload 契約のみを扱う。

---

## 2. 設計原則

- **契約先行**: 各消費者が個別実装で分岐しないよう、IPC・backend 抽象・fallback・
  secure ガード・タイムアウトを本書で先に固定する。
- **消費者は別 IPC メッセージ、Host は単一 backend 抽象**: トリガと payload 形が
  異なる各消費者はそれぞれ専用 IPC メッセージを持つが、Host 側は共有の
  `AiBackend` インターフェイス（単一 `Transform` コア）へ集約する（§3）。
- **入口で secure ガード**: 外部 AI へ渡らない保証は `AiBackend` の入口（Host 側）で
  プライバシー判定を強制チェックして担保する。TIP 側の抑止（§8）は多層防御の一層で
  あり、Host 側ガードを省略しない（§8）。
- **backend 中立の fallback**: `openai` 失敗・`none`・secure いずれでも、各消費者は
  定義された fallback 連鎖（§7.4）へ落ち、Host を落とさない（§7.5、M47 と整合）。
- **HTTP 基盤を共有契約化**: M16 の chat/completions POST と M32 のモデル DL
  （GET + SHA256）は用途が異なるが、同一の WinHTTP 基盤（セッション・プロキシ・
  TLS・タイムアウト設定）の上に実装する（§6）。M32 を Phase 5 へ前倒ししない。
- **後方互換と既定 OFF**: `aiBackend` 既定は `none`。設定 OFF では一切の外部送信・
  AI 呼び出しを行わない。

---

## 3. 消費者と共有インターフェイス（多消費者契約）

### 3.1 消費者の特性

| 消費者 | トリガ | 送信単位 | 既定 backend 方針 | 同期/非同期 |
|---|---|---|---|---|
| **M16** Magic Conversion / Replace Suggestion | 英数/かなキーのダブルタップ（ユーザー明示） | 選択テキスト | `aiBackend` 設定値（`openai` 可） | 窓で応答を待ち、[適用] で反映（モードレスの窓） |
| **M58-C** AI 整文 `ai-cleanup` | 一括変換確定（ユーザー明示） | 蓄積全文かな + 生ローマ字 | `aiBackend` 設定値（`openai` / `local-zenzai`） | 同期（変換確定経路） |
| **X-3-3** Post-Commit Lint | commit 後 800ms デバウンス（**自動**） | 直近 32 文字 | **`local-zenzai` を優先**（§3.3） | 非同期（背景、入力を阻害しない） |

**設計含意**: M16 / M58-C はユーザーの明示操作で起動するため外部 AI（`openai`）への
送信が妥当。一方 X-3-3 は**毎 commit 自動発火**するため、確定テキストを毎回外部 AI に
送るのはプライバシー・コスト両面で不適切である。X-3-3 は backend 選択を `local-zenzai`
へ寄せる既定とする（§3.3）。X-3-3 が Zenzai に渡すこと（§4.3）と、
本書の「同じ `AiBackend` 経路を流用」（roadmap M16 横断）は、この既定で整合する。

このほか X-3-6 バッチ訂正ビューの `DetectAnomalies`（設定アプリ → Host）も、Host が
`AiTransformRequest`（`task=Anomalies`）へ正規化して同じ `AiBackend` を呼ぶ。payload と
送信条件の正典は `docs/proofread-view-spec.md` §2 である。

### 3.2 Host 側 `AiBackend` の概念モデル

各消費者の IPC ハンドラ（`Dispatcher`）は payload を `AiTransformRequest` に
正規化して呼ぶ。以下は拡張対象も含む概念モデルであり、実際の C++ 宣言と許可・
キャンセル引数の正典は `inference-host/include/azookey/host/AiBackend.h` とする。

```cpp
namespace azookey::host {

enum class AiTask {
  Transform,    // M16: 選択テキストの言い換え/翻訳/要約/自由変換
  Cleanup,      // M58-C: 全文整文（誤字補正・句読点・整文）
  Lint,         // X-3-3: 確定テキストの誤変換検出（複数 finding を返す）
  Anomalies,    // X-3-6: 貼り付けた文章の誤りの疑いの検出（§3.1 末尾）
};

enum class AiBackendKind { None, OpenAi, LocalZenzai };

enum class AiErrorClass {
  None, Auth, RateLimit, ServerError, Network, Timeout, Parse, BlockedBySecure, Disabled,
  Canceled, KeyReentry
};

struct AiTransformRequest {
  AiTask task{AiTask::Transform};
  AiBackendKind backend{AiBackendKind::None};
  std::string mode;            // M16: "rewrite"|"translate"|"summarize"|"free"
                               // M58-C/X-3-3: task 固有（§5.2）
  std::string prompt;          // M16: ユーザー指示。他 task では空
  std::string text;            // 変換対象（選択/全文かな/確定 32 文字）
  std::string raw_romaji;      // M58-C のみ必須（生ローマ字）。他は空
  std::string left_context;    // 前後文脈（includeContextInAITransform 連動）
  bool auto_punctuation{false};// M58-C のみ意味を持つ
  uint32_t max_results{1};     // Lint: finding 上限。Transform/Cleanup: 1
};

struct AiFinding {            // Lint 用（§4.3）
  uint32_t start_char{};
  uint32_t end_char{};
  std::string current;
  std::string suggestion;
  std::string reason;
  float confidence{};
};

struct AiTransformResult {
  bool ok{false};
  std::string result;              // Transform/Cleanup の整文後テキスト
  std::vector<AiFinding> findings; // Lint の検出結果
  std::optional<std::string> error;// 失敗時の分類済みメッセージ（§7.2）
  AiErrorClass error_class{AiErrorClass::None}; // 呼び出し側の fallback 判断用
};

class AiBackend {
public:
  // 入口で プライバシー判定を強制チェックし（§8）、backend ごとの実装へ委譲する。
  // 同期 API。非同期化（X-3-3 / streaming）は既存 RequestScheduler
  // (inference-host/src/RequestScheduler.cpp) 側で行う。
  AiTransformResult Transform(const AiTransformRequest& req);
};

}  // namespace azookey::host
```

### 3.3 backend 選択と消費者別既定

- 実効 backend は次の順で解決する（既存契約を流用）。
  1. **secure ガード**（M46）: `外部 AI 許可==false` なら外部を
     禁止、`AI 候補生成許可==false` なら AI 自体を禁止 → 実効 backend を
     `None` に強制（§8）。`privacy-and-secure-input-spec.md` §5 / §5.2 が正典。
  2. **アプリ別プロファイル**（M48）: `app-profile-spec.md` §4.2 の backend 解決。
     **M48 のプロファイル解決が無い構成ではこの段を飛ばし、3 のグローバル設定へフォールバックする**
     （M16 は M48 に依存しない）。
  3. **グローバル設定** `aiBackend`（`settings/mvp-settings.schema.json`）。
- **X-3-3 の既定寄せ**: X-3-3（自動・毎 commit）は、`aiBackend=openai` でも
  Post-Commit Lint の送信先を `local-zenzai` へ寄せる。`local-zenzai` 不在時は
  Lint を**実行しない**（外部送信へフォールバックしない）。これにより確定テキストの
  無断外部送信を防ぐ。設定で明示的に X-3-3 の外部送信を許可する余地は将来拡張
  （§14）とし、MVP では設けない。

---

## 4. IPC payload 契約

IPC エンベロープ（`version` / `request_id` / `trace_id` / `type` / `payload`）と
length-prefixed フレーム（4 byte LE 長 + JSON、`kMaxFrameSize = 1 MiB`）は
`ipc/src/Messages.cpp` の既存規約に従う。payload フィールドは **snake_case**、
ビルダ/パーサは `Build*Request/Response` / `Parse*Request/Response` 命名で
`ipc/src/Payloads.cpp` に追加する。各メッセージ名を `MessageType` enum と
`TypeToString` / `TypeFromString` に追加する。

`ipc` の `MessageType` に定義があり Host で `AiBackend` を呼ぶのは、`TransformSelectedText`
（§4.1、`task=Transform`）、`QueryBatchConversion`（§4.2、`task=Cleanup`）と
`DetectAnomalies`（§3.1 末尾、`task=Anomalies`）である。§4.3 の `RequestPostCommitLint` は、
X-3-3 で `MessageType` に追加するメッセージの定義であり、`task=Lint` の IPC 上の入口を担う。

### 4.1 M16: `TransformSelectedText`（`docs/legacy-parity-spec.md` §4.4 を拡張）

> 本節が当該 payload の正典であり、`legacy-parity-spec.md` §4.4 は本節へのリンクだけを持つ
> （同 §4 冒頭の委譲注記）。

TIP は、Handshake 応答の capability に `transform_selected_text` がある Host にだけ送る。
この capability を持たない Host は未知の型を `Unknown` として扱い、要求と違う型で応答するので、
TIP はその応答を要求に対応づけられない。

```
TransformSelectedTextRequest:
  mode: enum { rewrite, translate, summarize, free }  // 必須。未知・欠落は不正な要求
  prompt: string            // ユーザー指示（例 "日本語に翻訳"）。free の指示（§5.2）。欠落は空。
                            // TIP はアプリ別の promptPrefix を先頭に付ける（下記）
  selection: string         // 変換対象。必須・非空
  context: string           // 前後文脈。欠落は空
  max_results: uint32       // 既定 1。M16 の結果は単一（§14）なので 1 に丸める
  secure: bool              // 入力先の secure 判定。欠落は true。wire には false だけを載せる
  ai_allowed: bool          // TIP が解いた AI 候補生成許可。欠落は false
  external_ai_allowed: bool // TIP が解いた外部 AI 許可。欠落は false。ai_allowed が false なら false
  ai_backend: string        // TIP がアプリ別プロファイルで解いた backend。空なら Host の aiBackend。
                            // none / openai / local-zenzai 以外は不正な要求
TransformSelectedTextResponse:
  result: string            // 変換後テキスト。成功のときだけ非空
  results: optional<string[]>// 複数案（予約。M16 の結果は単一なので wire に載せない。§14.1）
  error_class: optional<string> // 失敗の AiErrorClass 名（§7.2、呼出側 fallback 用）
  error: optional<string>   // 拒否（invalid_request / not_authenticated / safe_mode）
```

応答は、空でない `result`・`error_class`・`error` の少なくとも 1 つを持つ。どれも持たない応答と、
型が違う値を持つ応答は、不正な応答として扱う。予約の `results` を載せる将来の応答も `result` を併せて持つ。

要求の id は Envelope の `request_id` を使い、payload には載せない。`selection`・`prompt`・
`context`・`result` のバイト数の上限は `ipc/include/azookey/ipc/Limits.h` が正典である。
TIP は、入力先のアプリの `promptPrefix`（`docs/app-profile-spec.md` §4.1）が空でなければ、利用者の指示の前に
改行で区切って付けて `prompt` に載せる。`prompt` の上限は前置きを含めた長さに掛かる。
privacy のフィールドが欠けた要求と型が違う要求は、secure で AI 不可の要求として扱う。ほかのフィールドが
型の違う値で届いた要求は、欠落とみなさず不正な要求とする。

Host の `Dispatcher` は `MessageType::TransformSelectedText` を次の順で処理する。

1. Host が SafeMode（`docs/host-availability-spec.md` §8.5.3）なら、`AiBackend` を呼ばずに
   `error=safe_mode` を返す。SafeMode は §8.5.3 の設定の強制と engine の状態の両方で止め、判定には
   engine の状態を使う（起動時の突入で、保存の競合が設定から flag を落とすと、強制の当たらない設定の
   まま engine が SafeMode になりうる）。要求の `ai_backend` はこの停止を解かない。
2. 要求の `secure` が true か、Host の privacy 設定が secure（§8）なら、`AiBackend` を呼ばずに
   `error_class=BlockedBySecure` を返す。SafeMode の学習停止はこの判定に含めない（1 で止める）。
3. payload を `AiTransformRequest`（`task=Transform`）へ正規化する。AI 候補生成許可と外部 AI 許可は、
   Host の設定と要求の値の AND とする。設定を持たない Host はどちらも不許可とする。
4. `AiBackend::Transform` を呼ぶ。`task=Transform` はリモート専用で、外部 AI が許可されないか
   実効 backend が `local-zenzai` なら `Disabled` を返す（ローカルの変換器は選択テキストを変換しない）。
5. `AiBackend` を呼んだ後に、TIP が `Cancel` で取り消したと分かった要求には応答しない。

`AiBackend` を呼ぶ前に拒否した要求（不正な要求、Handshake 前、SafeMode、secure、保留の上限）には、
取り消し済みでも拒否の応答を返す。TIP は、待っていない `request_id` の応答を捨てる。
保留中の要求が接続ごとの上限に達していて受け付けられないときは、`error_class=ServerError` を返す。
Host は接続ごとに要求を順に処理するので、この要求は応答まで（外部 AI では最大で `openAiTimeoutMs`）
同じ接続の後続の要求を待たせる。`QueryBatchConversion` の `ai-cleanup` と同じ制約であり、取り消しの
`Cancel` は control 接続で届く。
Handshake 前の要求には、`AiBackend` を呼ばずに `error=not_authenticated` を返す。Host は選択テキスト・
指示・結果を保持せず、学習にも使わない。

IPC の ETW（`docs/observability-spec.md`）は、全型に共通して要求の型と payload の長さを記録する。
`TransformSelectedText` の長さは選択テキストと指示の長さの目安になるが、本文は持たない。secure の入力先から
TIP はこの型を送らないので、secure の入力の長さは記録されない。これを例外として認め、型ごとに長さを外さない。

失敗のログは `transform_selected_text_failed` の 1 行で、選択テキスト・指示・文脈・結果を持たない。
`result` は次の固定値をとる（`docs/observability-spec.md` §7.2 の、イベントを定義する spec が値を
列挙する場合に当たる）。

| `result` | 場面 | ほかのフィールド |
|---|---|---|
| `safe_mode` | SafeMode の拒否（手順 1） | `error_code=business` |
| `blocked` | secure の拒否（手順 2 と、`AiBackend` の入口の `BlockedBySecure`） | `error_code=business`、`error_class` |
| `error` | そのほかの `AiBackend` の失敗と保留の上限 | `error_code=business`、`error_class` |

### 4.2 M58-C: `QueryBatchConversion`（`romaji-batch-conversion-spec.md` §6 が正典）

M58-C は本書では**新規定義しない**。`ai-cleanup` 経路は既存の `QueryBatchConversion`
（`reading` / `raw_romaji` / `mode="ai-cleanup"` / `auto_punctuation`）を使い、Host の
`Dispatcher` が `mode=="ai-cleanup"` のとき payload を `AiTransformRequest`（`task=Cleanup`、
`text=reading`、`raw_romaji`、`auto_punctuation`）へ正規化して `AiBackend::Transform` を
呼ぶ（`QueryBatchConversionRequest` は `left_context` を持たない。M58-C で直近確定文が
必要になった場合は両 spec で同時に追加する）。`romaji-batch-conversion-spec.md`
§6.1 の必須フィールド（`mode=ai-cleanup` のとき `raw_romaji` 必須）と本書 §3.2 の
`AiTransformRequest` を相互に一致させる。`QueryBatchConversion` の `max_candidates`
（文節あたり候補数）は Cleanup task では使わず、Host 正規化時に `max_results=1` 固定とする
（文節候補数は M58-C 側の変換経路で扱う）。

`Dispatcher` は正規化の前に次の 3 つを判定し、どれかに当たれば `AiBackend` を呼ばずに `neural` へ落とす。
判定の意味は §4.1 の手順 1・2 と同じで、SafeMode は engine の状態で判定する。

- Host が SafeMode（`docs/host-availability-spec.md` §8.5.3）である。
- 要求の `secure` が true である。
- Host の privacy 設定が secure（§8）である。

要求が名指しする backend（`openai`、`local-zenzai`）はこの停止を解かない。M58-C は SafeMode でも
`error=safe_mode` を返さず、secure と同じ応答にする。応答の値は `romaji-batch-conversion-spec.md` §5・§6.2 が定める。

> 共有点: M58-C と M16 は**別 IPC メッセージ**だが、Host 側で同じ
> `AiBackend::Transform` に集約される。これが「消費者の共有」の実体である。

### 4.3 X-3-3: `RequestPostCommitLint`

確定済みテキストに対して、軽量 LLM で「より自然な解釈」を検出して波線表示する（roadmap M16 の横断項目）。
本節は予定の契約で、設定 `postCommitLint` は読み込み済みだが、`RequestPostCommitLint` は `MessageType` にも `Payloads.h` にも無い。
Host は payload を `AiTransformRequest`（`task=Lint`、`text`、`max_results`=
finding 上限）へ正規化して `AiBackend::Transform` を呼び、`AiFinding[]` を
`RequestPostCommitLintResponse.findings` にマップする。非同期（背景）の応答で、入力を阻害しない。

#### 4.3.1 タイミング

- commit 後 **800ms デバウンス**
- 直近 32 文字（or 段落末まで）を Zenzai に渡す

#### 4.3.2 IPC

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

#### 4.3.3 表示

- `ITfReadOnlyProperty`（既存 `GUID_PROP_ATTRIBUTE`）で **波線下線** 属性
  - 新規 GUID `kLintAttrGuid`（赤い波線、`TF_LS_SQUIGGLE`）
- 右クリック（ITfMouseSink、Phase 6-A）で候補一覧をポップアップ
- 採用したら訂正の学習を記録する（`docs/user-learning-enhancement-spec.md` §4.2）

---

## 5. OpenAI 互換 API 呼び出し契約

### 5.1 エンドポイント・認証・モデル

- エンドポイント: `{openAiApiEndpoint}/chat/completions`
  （`openAiApiEndpoint` 既定 `https://api.openai.com/v1`）。
- 認証: `Authorization: Bearer <openAiApiKey>`（at-rest 規約は §9）。
- `Content-Type: application/json`。
- モデル: `openAiModel`（既定 `gpt-4o-mini`）。
- MVP: `stream=false`（§10）。

### 5.2 `mode` → system-prompt マッピング

M16 の `mode` は legacy macOS の選択テキストのプロンプト窓（`OpenAIClient.swift` の
`sendTextTransformRequest`。単一の `result` を返す）に当たる。legacy の target-marker 辞書
（同ファイルの `Prompt.dictionary`。文章補完・絵文字・顔文字・記号・類義語・対義語・TeX・説明・つづきなど）は、未確定文字列を
`<target>` で挟んで 3〜5 件の候補を返す Replace Suggestion（legacy の Ctrl+S）のものであり、`mode` の系譜では
ない（`docs/legacy-parity-spec.md` §4.1）。`mode` は次の表の 4 値とし、target-marker 辞書に当たる拡張 mode は
置かない（§14）。`AiBackend` は `mode` から system プロンプトを選ぶ。`free` では `prompt`（ユーザー指示）を指示に加え、
ほかの `mode` は `prompt` を使わない。`text`（選択）と `context` は user メッセージに組む。

M16 のプロンプト窓は、英数・かなのどちらのダブルタップでも `free` を送り、利用者が窓で確かめた
指示を `prompt` に載せる（初期プロンプトだけが違う。`docs/legacy-parity-spec.md` §4.1）。legacy の
プロンプト窓も、初期プロンプトを含めて自由な指示として送る。`rewrite` / `translate` / `summarize` は
wire の値として残す。

| `mode` | 意図 | system プロンプト（要旨） |
|---|---|---|
| `rewrite` | 言い換え | 「入力テキストの意味を保ったまま、自然な日本語に書き換える。結果のみを返す」 |
| `translate` | 英訳 | 「入力テキストを英語へ翻訳する。結果のみを返す」 |
| `summarize` | 要約 | 「入力テキストを簡潔に要約する。結果のみを返す」 |
| `free` | 自由変換（英数・かなのダブルタップのプロンプト窓） | 「ユーザー指示（prompt）に従い入力テキストを変換する。結果のみを返す」 |

- system 文言の最終確定値は実装 PR（M16）でレビューする。本書は**マッピングの
  存在と意図**を契約として固定する（4 値の追加/削除は本書改訂を要する）。
- `context`（前後文脈）は、要求が持ち、Host の設定 `includeContextInAITransform`（§11）が true のときだけ
  user メッセージに「文脈」として付す。false のとき Host は要求の文脈を捨てる。M16 のプロンプト窓の TIP は
  `context` を常に空で送る（左文脈を外部 AI へ送らない。`docs/privacy-and-secure-input-spec.md` §5.3）。
- M58-C（`task=Cleanup`）の system プロンプトは「誤字補正・句読点挿入・整文。
  `raw_romaji`（生ローマ字）の打鍵誤りパターンを補正に使う。`auto_punctuation` が
  true のとき句読点を挿入する」を要旨とする。詳細は `romaji-batch-conversion-spec.md`
  §5。X-3-3（`task=Lint`）は「確定テキストの不自然箇所（同音異義語・敬語不一致等）を
  検出し、位置・現状・提案・理由・確信度の配列で返す」を要旨とする。<!-- lint:allow status -->

### 5.3 リクエストボディ（chat/completions）

- `messages`: `[{role:"system", content:<§5.2>}, {role:"user", content:<text+raw_romaji+context の JSON>}]`。
  system には §5.2 の指示を置き、`free` ではユーザー指示（`prompt`）もここに含める。user の JSON の値は
  指示として扱わないよう system で明示する。
- `model`: `openAiModel`。
- `stream`: §10 の境界に従う（MVP `false`）。
- **構造化出力**: legacy と同様に `response_format` で JSON 構造を固定する。
  - `Transform` / `Cleanup`: `json_schema` で `{ "result": string }`（`strict:true`、
    `additionalProperties:false`）。
  - `Lint`: 対象外。常にローカル経路で処理し、chat/completions へ送らない（戻り値の
    `{ "findings": [{start_char,end_char,current,suggestion,reason,confidence}] }` はローカル経路の形）。
    リクエストの `response_format` に載るのは `{ "result": string }` だけである（`Anomalies` は `result` の
    文字列に JSON 配列を載せる。§5.2）。
- OpenAI 互換だが `response_format` 非対応のエンドポイントを許容するため、応答
  パースは「`choices[0].message.content` を JSON として解釈し、失敗時はプレーン文字列を
  `result` とみなす」緩いパースを許す。`{` / `[` で始まらない本文だけをプレーン文字列として受理し、
  それ以外の失敗は下の表のとおり `Parse` とする。
- 400 のとき、`response_format` を外して再送しない（§14.5）。

**互換ケースの表**（モック HTTP の試験 `inference-host/tests/ai_backend_test.cpp` で守る。実 API は呼ばない。§14.3）。
`result` が空、または 64 KiB を超える応答は、受理する行でも `Parse` とする。

| 応答の形 | 例 | 分類 | 備考 |
|---|---|---|---|
| 構造化出力に従う | `content` が `{"result":"…"}` | 受理 | `result` の前後空白をトリムする |
| `response_format` を無視したプレーン文 | `content` が `今日はいい天気` | 受理 | 文全体を `result` とみなす |
| `{` / `[` で始まるが JSON として不正 | 途中で切れた `{"result":"今日は` | `Parse` | プレーン文として採らない |
| JSON として正しいが `result` が文字列でない | `["a"]`、`{"result":1}`、`{"result":null}`、`{"results":"x"}` | `Parse` | |
| `content` が文字列でない、または欠落 | 欠落、`null`（`refusal` のみ）、パーツの配列、オブジェクト | `Parse` | |
| `choices` が空・欠落 | `{"choices":[]}`、`{"error":{…}}` | `Parse` | |
| `finish_reason` が `stop` 以外 | `length`、`content_filter`、`tool_calls`、`function_call`、空文字 | `Parse` | `stop`、`null`、欠落は受理する |
| 400・404・405・413・415・422 | `response_format` を拒否する 400 など | `Parse` | §7.2 の表に無い非 200 の status。1 回だけ送る。再送しない。本文は読まない（§7.2） |
| コードフェンス付きの JSON | `content` が `` ```json … ``` `` | 試験では固定しない | 全文が `result` として受理される。DEV-2163 で扱う |
| 本文が JSON のスカラー | `content` が `42`、`true` | 試験では固定しない | 構造化出力と解釈され `Parse` になる。DEV-2163 で扱う |

### 5.4 応答パース

- 成功: HTTP 200 かつ `choices[0].message.content` を §5.3 のスキーマで解釈。
  `Transform`/`Cleanup` は `result` を取り出す。`Lint` はローカル経路だけで処理し、本節の対象ではない（§5.3）。
- `result` は前後空白をトリムする（legacy 準拠）。
- 非 200 / 構造不正は §7.2 のエラー分類へ。

---

## 6. HTTP 実装経路（共通 WinHTTP 基盤の契約）

> 判断（DEV-346）: **共通基盤を契約化**する。M16 は独自スタックを抱えず、また
> M32 を Phase 5 へ前倒しもしない。両者は同じ WinHTTP 基盤の上に、用途別の
> 経路を実装する。

### 6.1 共通 WinHTTP 基盤（`inference-host` 内）

次の関心を共通ヘルパとして切り出し、M16（API 呼び出し）と M32（モデル DL）が
共有する。
共通部は `inference-host/include/azookey/host/HttpSession.h`（`OpenHttpSession`）であり、
モデル DL の GET 経路（`inference-host/src/HttpDownloader.cpp`）と AI の POST 経路
（`inference-host/src/AiHttp.cpp`）が同じセッション初期化とプロキシ・TLS の方針を使う。
用途ごとに別の WinHTTP スタックを作らない。

- WinHTTP セッション初期化（`WinHttpOpen`、User-Agent、HTTP/2 可否）。
- **プロキシ解決**（システムプロキシ / WPAD / 明示設定の尊重）。
- **TLS**（既定の証明書検証を無効化しない）。
- **タイムアウト設定**（resolve / connect / send / receive。§7.1）。
- キャンセル（`trace_id` / `request_id` 連動。`romaji-batch-conversion-spec.md` §6.3 の
  キャンセル契約と整合）。

### 6.2 用途別経路の分担

| 経路 | 担当 M | メソッド | 追加要件 |
|---|---|---|---|
| chat/completions 呼び出し | **M16**（本書） | POST + JSON | Bearer 認証、`response_format`、（Phase 6）SSE ストリーミング |
| モデル DL（GGUF 等） | **M32** | GET | 取得手順（`.part`、Range、SHA256、原子置換）は `docs/model-management-spec.md` §3.1.2、照合する entry は同 §3.1.1 |

- AI の POST 経路と M32 の GET + SHA256 経路は、いずれも §6.1 の共通基盤の上に載せる
  （重複スタックを作らない）。
- M32 を Phase 5 へ前倒ししない（Phase 5 を自己完結に保つ）。roadmap の M16 / M32 /
  M34 の依存関係は roadmap が持つ（§13）。

---

## 7. エラー / リトライ / レート制限 / タイムアウト

### 7.1 タイムアウト階層

外部 API（OpenAI）のタイムアウトと、Host/IPC のタイムアウト（M47）を**区別**する。
以下の具体値（30 s / 10 s、§7.3 の再試行回数）は目安であり、実装 PR で `openAiTimeoutMs`
（§11）と連動して調整可とする。

| 層 | 対象 | 値（既定） | 備考 |
|---|---|---|---|
| 外部 API（本書） | OpenAI HTTP receive | **30 s**（接続/送信は各 10 s） | `chat/completions`。設定 `openAiTimeoutMs`（§11、既定 30000）で調整可 |
| **外部 AI 経路の IPC deadline** | TIP↔Host（M16 / M58-C の openai backend） | **`openAiTimeoutMs` + 余裕（既定 約 35 s）** | **M47 の 150 ms などの短い deadline や、ローカル Zenzai の設計上限 800 ms を流用しない**。外部 AI 応答は通常 800 ms を超え、流用すると正常応答が stale 化して M16 が壊れて見える。`host-availability-spec.md` §8.5.2 の timeout 表「外部 AI 呼び出し」行は本値に従う |
| ローカル経路の IPC（M47） | Zenzai 変換等（外部 AI でない重処理） | M47 の要求種別ごとの deadline（`host-availability-spec.md` §8.5.2。ライブ変換 150 ms など。一括変換は 30 s） | connected-but-silent を timeout 視（M47） |

- **外部 AI 経路（openai backend）は M47 の短い deadline ではなく、上表の「外部 AI 経路の
  IPC deadline」（`openAiTimeoutMs` + 余裕）で TIP が監視する**。Host は外部 API 待ちの間に
  TIP からの `Cancel`（§6.1）を受理し得る。タイムアウトした要求は破棄し、古い結果が
  後着しても捨てる（staleness check。M47 準拠）。
- 代替として M16 ダイアログ経路を**非同期化**してよい（変換中はスピナー表示で同期 deadline で
  殺さず、応答到着またはユーザーの明示キャンセルで確定）。長い API レイテンシでも M16 が
  壊れないことを保証する。同期 deadline か非同期かは実装 PR で選択する。
- `local-zenzai` backend の通常のAI経路はM47の要求種別ごとの deadline（§8.5.2）に従う。
  Hostの`AiBackend`は、ローカル経路の`Lint`に800 msの deadlineを課し、超過は`Timeout`とする。
  一方で変換要求（`QueryCandidates`、一括変換）には、
  800 msは1変換のハード予算の上限を決める設計値であり、request deadlineではない。
  M58-Cの一括整文（`Cleanup`）は実モデルで800 msを超えるため、独立した30秒の
  全体期限を使う。`openAiTimeoutMs`では変更しない。TIPは5秒の余裕を加えて待機する。
  Magic Conversion（`Transform`）と`Anomalies`はローカル経路を持たず、`Disabled`を返す（§4.1）。
- X-3-3 は非同期の背景要求であり、TIP の同期応答タイムアウトには載らない。

### 7.2 HTTP ステータス → `AiErrorClass` マッピング（legacy 準拠）

| HTTP | `AiErrorClass` | ユーザー向け要旨（legacy `OpenAIError` 準拠） |
|---|---|---|
| 200 | `None` | 成功 |
| 401 | `Auth` | API キーが無効。設定を確認 |
| 403 | `Auth` | アクセス拒否。キーの権限を確認 |
| 429 | `RateLimit` | レート制限。少し待って再試行 |
| 500–599 | `ServerError` | 一時的に利用不可。後で再試行 |
| 接続不可 / DNS | `Network` | 接続失敗。ネットワークを確認 |
| receive timeout | `Timeout` | 応答なし |
| 構造不正 | `Parse` | 応答を解釈できない |

ログには `reading` / `surface` / 入力 / 鍵 / プロバイダ応答本文を出さない
（secure 中は §8、通常時も `observability-spec.md` のログ規約に従う）。
エラー本文（API のエラー JSON）は任意の入力や鍵を含み得るため参照・記録せず、
HTTP status と内部 `AiErrorClass` だけで分類する。

`dpapi:` キーを復号できない場合は `KeyReentry` とし、外部送信せず設定アプリでの
キー再入力を促す。`QueryBatchConversionResponse.error_class` は失敗分類名を任意で
返し、旧 Host の省略応答と未知値を TIP が受け入れる。TIP の `ai-cleanup` 劣化案内は
`Auth` が「API キーを確認してください」、`RateLimit` が「少し待って再試行してください」、
`Timeout` が「応答がありません。再試行してください」、`KeyReentry` が
「API キーを再入力してください」を要旨とする。表示文言は固定値とし、候補の
表層形と確定文字列には含めない。

### 7.3 リトライ方針

- 再試行対象は `RateLimit`（429）と `ServerError`（5xx）と一時的 `Network` のみ。
  `Auth`（401/403）・`Parse` は**再試行しない**（無駄打ち防止）。
- **指数バックオフ + ジッタ**、最大 **2 回**再試行（合計 3 回試行）。`Retry-After`
  ヘッダがあれば優先。総待ち時間が §7.1 のタイムアウトを超えない範囲で打ち切る。
- Host の RequestScheduler が管理するキャンセル通知は、retry 待機を即時解除する。
  待機解除後は再送せず `Canceled` で終了する。
- M16（同期・ユーザー待ち）は体感を優先し、再試行を最大 1 回（合計 2 回試行）に抑える。
  X-3-3（非同期・背景）は静かに諦めてよい（finding 0 件として扱う）。

### 7.4 fallback 連鎖（消費者別）

| 消費者 | 一次 | 失敗/`none`/secure 時 | さらに失敗時 |
|---|---|---|---|
| M16 | `aiBackend` 変換 | エラーをユーザーに提示し**選択を変更しない**（原文維持） | — |
| M58-C | `ai-cleanup` | `neural`（zenz かな漢字変換）。Host の SafeMode でも同じ（§4.2） | かな確定 |
| X-3-3 | `local-zenzai` Lint | Lint 不実行（finding 0 件） | — |

- M58-C の連鎖は `romaji-batch-conversion-spec.md` §5 / M58-C 受け入れ条件が正典。
  `aiBackend=none` のときは `neural` へ自動 fallback。
- いずれの fallback でも Host を落とさない（M47 / M8 と整合）。

### 7.5 劣化 UX（M47 連携）

- M16 は失敗時、原文を維持し、プロンプト窓の状態行に分類ごとの固定の案内（§7.2）を出す
  （`docs/legacy-parity-spec.md` §4.3。文言は `tsf-tip/src/PromptDialogModel.cpp`）。secure 由来の抑止は
  「セーフ入力のため AI変換を使用しません」を明示する（混乱回避）。Host の SafeMode（`error=safe_mode`）は
  secure と別の案内「安全モードのため AI変換を使用しません」にする。外部 AI への明示同意（§14.2）だけが欠けているとき
  は、TIP が選択を読まずに案内だけの窓を出し、「OpenAI への送信がオフのため AI変換を使用しません。設定の「AI サービス」で
  オンにすると使えます。」を出す。出す条件と、案内を出さない他の不許可は `docs/legacy-parity-spec.md` §4.1 が定める。
- M58-C は neural fallback の候補表示中、候補の下に分類別の固定案内を表示する。
  app 描画の UI-less 経路では候補 UIElement の説明として同じ案内を公開する。
  ホストアプリが説明を実際に描くかはアプリの実機確認対象とする。候補 UI を閉じるか
  新しい要求へ切り替わると案内を消す。X-3-3 の劣化表示は M47 の規約に従う。

---

## 8. secure ゲート連携（M46 プライバシー判定）

`privacy-and-secure-input-spec.md` §5 が正典。本書は `AiBackend` 入口での
**強制ガード**を契約として固定する。

`AiBackend` の消費側は、設定と当該要求から解決した許可条件で
処理を制限する。入力先由来の secure 判定は TIP が担い、Host は前面アプリを
検出しない。`TransformSelectedText` の secure 中の送信抑止は TIP が担い、Host も要求の
`secure` と Host の privacy 設定で拒否する（§4.1）。`QueryBatchConversion` の `ai-cleanup`（§4.2）と
`DetectAnomalies`（`docs/proofread-view-spec.md` §2.9）も、Host の `Dispatcher` の入口で要求の値と Host の
privacy 設定の両方を検査し、SafeMode を加えた判定で `AiBackend` を呼ばずに止める。判定に使う項目は各節が定める
（`DetectAnomalies` は学習可否も見る）。
本節の AI backend ガードは消費側の設計契約であり、共有ゲートインスタンスや
M48 のモード通知 API を前提としない。

- **入口ガード（必須）**: `AiBackend::Transform` の入口で要求ごとの許可条件を評価し、
  - `AI 候補生成許可==false` → 実効 backend を `None`、`error_class=BlockedBySecure`
    で早期 return（ローカル zenzai も動かさない）。
  - `外部 AI 許可==false` かつ実効 backend が `OpenAi` → 外部送信を行わず、
    消費者の fallback（§7.4）へ。`local-zenzai` が許可されていればそちらへ寄せる。
    ローカル経路を持たない `Transform`（M16）と `Anomalies` は寄せずに `Disabled` を返す（§7.1）。
  - `privacy-and-secure-input-spec.md` §5 の抑止表は `aiBackend=none` 強制の実装
    ポイントを `inference-host/src/AiBackend.cpp` と明記しており、本契約はそれを満たす。
- **多層防御**: TIP 側でも secure 中は Magic Conversion を無効化する（同 §5 表。ダブルタップを検出する
  `StartAlnumDouble` / `StartKanaDouble` は `docs/legacy-parity-spec.md` §4.1 が定める）。Host 側入口ガードは TIP 側抑止に依存せず**独立に**
  保証する（呼び出し経路の取りこぼし・将来の新消費者に対する安全側）。この二重化は
  `privacy-and-secure-input-spec.md` §5.1.1 の二段ゲートの一部であり、TIP 側抑止だけでは本契約を満たさない。
- **不変条件**: `外部 AI 許可 ⇒ AI 候補生成許可`（同 §5.1）。`secure` では
  両方 false。`private` / `offline` は AI 候補可・外部不可（ローカルのみ）。
- **ログ**: secure 中は `reading` / `surface` / 本文を redact（同 §5、
  `observability-spec.md` §7.6 優先順位 1）。

---

## 9. API キーの at-rest 保存

> `dpapi:` prefix 規約は本書が定める。M16 は M34（DPAPI 暗号化）を hard prerequisite に
> しない（M16 が M34 にブロックされない）。Host は平文と `dpapi:` 付きの値の両方を受け付ける。

- 設定キー `openAiApiKey`（`settings/mvp-settings.schema.json`）に保存する。
- **`dpapi:` prefix 規約**: 値が `dpapi:` で始まる場合、続く Base64 を DPAPI
  （`CryptUnprotectData`、ユーザースコープ）で復号して実キーとする。prefix が無ければ
  **平文**とみなす。設定アプリはキー入力時に DPAPI で暗号化し `dpapi:` 付きで
  保存する（`roadmap` M34 / `sideload-packaging-spec.md` §9）。
- **平文値の受理**: Host は `inference-host/src/AiHttp.cpp` で
  `learning/include/azookey/learning/DpapiCrypto.h` の `UnprotectSecret` を通し、`dpapi:` 付きの
  値と平文の値の両方を受け付ける。README は平文も受け付ける旨を注意喚起する。`dpapi:` 付きの
  値を復号できないときは `KeyReentry` に分類する（§7.2）。
- キーをログ・エラーメッセージ・クラッシュダンプに出さない
  （redaction ポリシーの正典は `docs/observability-spec.md` §7.6）。
- 設定アプリでは鍵を伏字で読み込み、変更時はユーザースコープ DPAPI で保護してから
  `dpapi:` 形式で保存する。変更しない既存値は保持し、旧形式の平文値は次の保存で
  保護値に移行する。復号できない既存値を黙って消去せず、再入力または明示削除を促す。

---

## 10. ストリーミング境界

- **MVP（Phase 5）**: `stream=false`。`chat/completions` の一括応答を待って `result` を
  返す。M16 ダイアログは完了時に一度だけ結果反映（`legacy-parity-spec.md` §4.6）。
- **Phase 6 拡張**: `stream=true`（SSE）。Host が部分トークンを受信し、**push IPC**で
  TIP へ逐次反映する。片方向の push 経路を要し、その契約はこの拡張で本書に定める。
- **予約フィールド**: 将来の streaming / 複数案のため、本書の payload に予約を置く。
  - `TransformSelectedTextResponse.results`（複数案。M16 の結果は単一とし、使わない。§14）。
  - streaming 中間応答は `partial:true` のチャンク + 最終 `partial:false`
    （`romaji-batch-conversion-spec.md` §6.3.5 の partial 規約と同形。MVP 既定は単発）。
  - `AiTransformRequest` に将来 `stream` フラグを足す余地を残す（MVP は実装しない）。
- ストリーミング採用時も §8 の secure ガードは**送信前**に評価する（部分送信を含め
  外部に出さない）。

---

## 11. 設定キー（`settings/mvp-settings.schema.json`）

既存キー（変更なし、本書で意味を確定）:

| キー | 既定 | 意味 |
|---|---|---|
| `aiBackend` | `none` | `none`/`openai`/`local-zenzai`。AI 変換 backend |
| `openAiApiKey` | `""` | API キー。`dpapi:` prefix で暗号化（§9） |
| `openAiApiEndpoint` | `https://api.openai.com/v1` | OpenAI 互換エンドポイント |
| `openAiModel` | `gpt-4o-mini` | model 名 |
| `includeContextInAITransform` | `true` | 前後文脈（直近 paragraph）を含める。Host が要求の文脈を使うかを決め、M16 の TIP は文脈を送らない（§5.2） |
| `batchConversionMode` | `neural` | `neural`/`ai-cleanup`（M58-C） |
| `batchAutoPunctuation` | `false` | `ai-cleanup` 時の句読点自動挿入（M58-C） |
| `postCommitLint` | `false` | X-3-3 の有効化 |

共有AI基盤の期限設定:

| キー | 既定 | 意味 |
|---|---|---|
| `openAiTimeoutMs` | `30000` | 外部 API receive と再試行を含む全体期限。1000〜120000 ms（§7.1） |

外部送信の同意（§14.2）:

| キー | 既定 | 意味 |
|---|---|---|
| `privacy.externalAiConsent` | `false` | 外部 AI へ本文を送ることへの明示同意。どのモードでも外部 AI 許可（§8）の要件になる。キーが無ければ同意なしとし、真偽値でなければ AI 候補生成も不許可にする（`privacy-and-secure-input-spec.md` §5.2） |

`AiBackend::Transform`はM58-Cから共通利用する。WinHTTPはダウンロードと
proxy・TLS検証の方針を共有し、AIのPOSTは非同期ハンドルの終了通知まで待って
キャンセルする。HTTPS以外は数値loopbackのみ許可し、redirectは追従しない。
応答本文は256 KiB、整文結果は64 KiBまでとする。

ローカル整文はZenzaiのprofile付き変換を使用し、生ローマ字と句読点方針を
profileに渡す。モデルの指示追従と誤字補正の品質は実モデルで確認する。
自動Lintの呼び出し契約はローカル限定だが、finding生成とM16のUIは別範囲とする。

---

## 12. 受け入れ条件

受け入れ条件の定義の正典は [`plans/windows-port-roadmap.md`](../plans/windows-port-roadmap.md)
の M16 節とする。本書は `AiBackend` の IPC・Host 側抽象・エラー分類・secure ガード・
キー保存を定義し、受け入れ条件を複製しない。

> 既知のテストギャップ・実機検証（OpenAI 実 API での手動確認（§14.3）、secure 実機確認）は
> Linear に課題として起票し追跡する（roadmap には状態を書かない）。

---

## 13. 依存関係（roadmap への反映）

M16 と M32 / M34 / M46 との依存関係は `plans/windows-port-roadmap.md` の M16・M32・M34 が
正典である。本書が持つ契約側は、backend 解決順（§3.3。M16 は M48 に依存しない）、共通 WinHTTP 基盤の共有（§6）、
secure ガード（§8）、API キーの `dpapi:` prefix 規約（§9）である。

---

## 14. 未確定事項 / 将来拡張

### 14.1 決めた事項（DEV-1964）: M16 の結果は単一、`mode` は 4 値

M16 のプロンプト窓は複数案を出さず、単一の `result` を返す。`max_results` は 1 に丸め、
`TransformSelectedTextResponse.results` は wire に載せない予約として残す（§4.1・§10）。`mode` は §5.2 の
4 値とし、増やしも減らしもしない。TIP が送るのは `free` だけだが、`rewrite`・`translate`・`summarize` は
wire の値として残す。利用者から見える挙動と既定値、設定キー、§8 の入口ガード、AI の結果を学習しない
契約（§4.1）は、この決定で変わらない。

根拠は次のとおり。

- legacy の選択テキストのプロンプト窓も単一の結果を返す。複数案（3〜5 件の `predictions`）と target-marker
  辞書を使うのは Replace Suggestion だけである（§5.2）。
- 窓は結果を出した後も指示欄を編集でき、指示を直して [変換] で送り直せる（`docs/legacy-parity-spec.md` §4.3）。
  別の案は送り直しで得られる。
- 拡張 mode を足しても、窓は常に `free` を送り、mode を選ぶ操作を持たないので使われない。類義語・対義語・
  TeX のような変換は、`free` の指示で頼める。
- target-marker 辞書の一部はローカルの候補と重なる。絵文字は M62-D、記号は M62-C
  （`docs/candidate-rewriter-spec.md` §6・§19）がローカルで決定的に候補を出し、外部へ送らない。顔文字を
  生成するローカルの候補は無く、候補のラベル `CandidateTag::Kaomoji`（`docs/legacy-parity-spec.md` §3.9）と、
  ユーザー辞書の `symbol` 品詞（`docs/user-dictionary-pos-spec.md`）による登録だけがある。
- 複数案を出すと、`kMaxTransformResults`、応答の型と parser、§3.2 の `AiTransformResult`、§5.3・§5.4 の
  `response_format`、窓の UI に変更が及び、外部 API の出力トークンも増える。

次のときに再検討する。

- 利用者から複数案か拡張 mode の要望が出たとき。
- Replace Suggestion（未確定文字列の AI 候補。legacy の Ctrl+S）を Windows 版へ移植するマイルストーンを
  置くとき。この移植は、新しい IPC メッセージ、キーの割り当て（`docs/legacy-parity-spec.md` §1.4）、候補窓への
  表示、未確定文字列と左文脈の外部送信（M16 のプロンプト窓の文脈の扱いは §5.2 と
  `docs/privacy-and-secure-input-spec.md` §5.3 が定める）を要する。target-marker 辞書と複数案は、その移植で扱う。

### 14.2 決めた事項（DEV-1965）: 外部 AI 送信は normal モードでも明示同意を要する

`privacy.mode=normal` でも、外部 AI 許可（§8）に利用者の明示同意を要件として加える。同意は設定キー
1 つで表し、既定は false とする。キーを持たない旧版の settings.json も false として解決する
（fail closed。`docs/privacy-and-secure-input-spec.md` §2）。`aiBackend=openai` の選択と API キーの入力は、
同意とみなさない。

同意の無い要求は §8 の `外部 AI 許可==false` と同じに扱う。実効 backend が `OpenAi` なら外部へ送らず、
消費者の fallback（§7.4）へ進む。同意は設定アプリの OpenAI 節で取り、初回送信前の確認ダイアログは
設けない。不変条件（`外部 AI 許可 ⇒ AI 候補生成許可`、secure が最優先）はこの決定で変わらない。
キーは `privacy.externalAiConsent`（§11）であり、`docs/privacy-and-secure-input-spec.md` §5.2 の表にも載せる。

根拠は次のとおり。

- 未確定文字列や選択範囲を外部へ送る経路（M16・M58-C）は、利用者が送信を意識しないまま動きうる。
  backend の選択と鍵の入力は、送信への同意としては弱い。
- 同意の無い構成を安全側（ローカルのみ）へ倒すのは、privacy の既定 OFF の原則（§2）と同じである。

### 14.3 決めた事項（DEV-1968）: OpenAI の実 API を呼ぶ結合テストは CI に置かない

PR・定期実行・`workflow_dispatch` のいずれにも、OpenAI の実 API を呼ぶジョブを置かない。GitHub の
secrets に OpenAI の鍵を置かない。応答形式（§5.3 の互換ケースの表と §5.4）と HTTP ステータスの分類（§7.2）は、実 API を
呼ばないテストの互換ケースで守る。実 API での確認は、承認された OpenAI 互換 API を使う手動の実機確認で
行い、Linear の課題で追跡する。

根拠は次のとおり。

- fork の PR に鍵が渡る経路を作らない。
- 利用料金が CI の実行回数に比例して発生しない。

実機確認で見つからない応答形式の回帰が起きたときに再検討する。

### 14.4 未確定事項

- X-3-3 の**外部送信オプトイン**（§3.3。既定は `local-zenzai` 寄せで、MVP では外部送信を許可しない）。
- Phase 6 **streaming**（SSE → push IPC）の詳細プロトコル（中間 `partial` の単位）。

これらの追跡先は Linear の M16 tracking 課題（DEV-938）である。着手時に確定する
論点として同課題が保持する（本書は状態を持たない）。

### 14.5 決めた事項（DEV-1967）: 400 のときに `response_format` なしで再送しない

`response_format`（`json_schema`、`strict:true`）を拒否するエンドポイントが 400 を返したとき、`response_format`
を外して再送しない。400 は、§7.2 の表に無い非 200 の status と同じ `Parse`（「構造不正」の行と同じ分類）とし、1 回だけ送る。404・405・413・415・422 も同じである。
応答のパースが許すプレーン文（§5.3 の互換ケースの表）は、`response_format` を無視して 200 を返す
サーバーのための許容であり、400 を返すサーバーは対象にしない。

根拠は次のとおり。

- エラー本文は入力や鍵を含み得るので、参照しない（§7.2）。本文を読まなければ、400 が `response_format` の拒否か、
  ほかの原因（コンテキスト長の超過、モデル名の誤り、競合するパラメータ）かを見分けられない。再送するなら全部の
  400 が対象になり、別の原因の 400 でも選択テキストと文脈を含む同じ本文をもう 1 回送る。
- 再送で救えるのは、`json_schema` に対応しない旧モデルや旧 API バージョンの 400 に限られ、既定の
  `openAiModel` は対象ではない。
- 再送を入れると、M16 の再試行（最大 1 回。§7.3）に再送を数えるかどうかを決める必要が生じ、最悪で 1 要求に 3 回
  送る。期限は `openAiTimeoutMs` の 1 本（§7.1）なので、再送は残り時間に収まる範囲に限られる。
- 実 API の結合テストを CI に置かない（§14.3）ので、再送経路の実サーバーでの挙動を自動では確かめられない。

利用者から見える挙動は変わらない。送信先、本文、同意の範囲（§14.2）、再試行の回数も変わらない。

互換サーバーへの対応は、要望が出たときに、設定で `response_format` を送らない選択を足す案を Linear の別課題で
検討する。再送のように自動では切り替えない。コードフェンス付きの JSON と、JSON のスカラーだけの本文の扱い
（§5.3 の表の最後の 2 行）は DEV-2163 で決める。

次のときに再検討する。

- `response_format` を拒否する互換サーバーの利用の要望が出たとき。
- 実機確認（DEV-1046）で、再送すれば救えた 400 が見つかったとき。
