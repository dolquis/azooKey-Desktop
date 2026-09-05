# AiBackend（AI 変換バックエンド）契約 仕様

対象リポジトリ: dolquis/azooKey-Desktop
対応マイルストーン: M16（Magic Conversion / Replace Suggestion。本書の主対象）。
  共有消費者として M58-C（AI 整文 `ai-cleanup`）/ X-3-3（Post-Commit Lint）。
関連:
- `docs/legacy-parity-spec.md` §4（Magic Conversion。本書が §4.4〜§4.6 を拡充する）
- `docs/romaji-batch-conversion-spec.md` §5 / §6(M58-C `ai-cleanup`、`raw_romaji`)
- `docs/rich-features-spec.md` X-3-3（Post-Commit Lint）/ X-3-5（訂正の学習）
- `docs/privacy-and-secure-input-spec.md` §5（secure 中の挙動契約、`PrivacyGate`）
- `docs/app-profile-spec.md` §4.2（M48 アプリ別 backend 解決）
- `docs/sideload-packaging-spec.md` §9（M34 DPAPI）
- `plans/windows-port-roadmap.md` M16 / M32 / M34 / M46 / M47 / M48 / M58-C
- `legacy/Core/Sources/Core/MagicConversion/{AIBackend,OpenAIClient}.swift`（参照のみ）
作成日: 2026-06-24
位置づけ: Phase 5（M16）正典仕様。`AiBackend` は M16 単独でなく M58-C / X-3-3 が
  同一経路を流用する**共有コンポーネント**であり、実装着手前に本書で契約を確定する。

> 本書は `inference-host/src/AiBackend.cpp`（新規・未実装）の契約を、実装着手前に
> 確定するための正典仕様である。実装と差異が出た場合は本書を先に更新する。
> 進捗・状態の正典は Linear（M16 tracking 課題 DEV-938。本書の契約確定は DEV-346）
> であり、本書は状態を持たない。

---

## 1. 目的とスコープ

### 1.1 目的

`AiBackend` は、選択テキストや確定テキスト・全文ローマ字を AI（OpenAI 互換 API
またはローカル Zenzai）に委譲して変換・整文・校正するための Host 側共有
コンポーネントである。本書は次を契約として確定する。

1. 3 消費者（M16 / M58-C / X-3-3）が再利用できる **IPC payload と Host 側
   インターフェイスの抽象度**（§3, §4）。
2. **OpenAI 互換 API 呼び出し契約**（エンドポイント・認証・モデル・`mode`→
   system-prompt マッピング・`response_format`）（§5）。
3. **HTTP 実装経路**（共通 WinHTTP 基盤の契約。M32 `HttpDownloader` との分担）（§6）。
4. **エラー / リトライ / レート制限 / タイムアウト**ポリシー（M47 と整合）（§7）。
5. **secure ゲート連携**（M46 `PrivacyGate`。外部 AI へ渡さない保証）（§8）。
6. **API キーの at-rest 保存**（M34 DPAPI 前の暫定平文と `dpapi:` prefix 規約）（§9）。
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

- **契約先行**: 3 消費者が個別実装で分岐しないよう、IPC・backend 抽象・fallback・
  secure ガード・タイムアウトを本書で先に固定する。
- **消費者は別 IPC メッセージ、Host は単一 backend 抽象**: トリガと payload 形が
  異なる 3 消費者はそれぞれ専用 IPC メッセージを持つが、Host 側は共有の
  `AiBackend` インターフェイス（単一 `Transform` コア）へ集約する（§3）。
- **入口で secure ガード**: 外部 AI へ渡らない保証は `AiBackend` の入口（Host 側）で
  `PrivacyGate` を強制チェックして担保する。TIP 側の抑止（§8）は多層防御の一層で
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

### 3.1 3 消費者の特性

| 消費者 | トリガ | 送信単位 | 既定 backend 方針 | 同期/非同期 |
|---|---|---|---|---|
| **M16** Magic Conversion / Replace Suggestion | 英数/かなキーのダブルタップ（ユーザー明示） | 選択テキスト（or キャレット段落） | `aiBackend` 設定値（`openai` 可） | 同期（ダイアログ完了で結果反映） |
| **M58-C** AI 整文 `ai-cleanup` | 一括変換確定（ユーザー明示） | 蓄積全文かな + 生ローマ字 | `aiBackend` 設定値（`openai` / `local-zenzai`） | 同期（変換確定経路） |
| **X-3-3** Post-Commit Lint | commit 後 800ms デバウンス（**自動**） | 直近 32 文字 | **`local-zenzai` を優先**（§3.3） | 非同期（背景 push、入力を阻害しない） |

**設計含意**: M16 / M58-C はユーザーの明示操作で起動するため外部 AI（`openai`）への
送信が妥当。一方 X-3-3 は**毎 commit 自動発火**するため、確定テキストを毎回外部 AI に
送るのはプライバシー・コスト両面で不適切である。X-3-3 は backend 選択を `local-zenzai`
へ寄せる既定とする（§3.3）。`rich-features-spec.md` §X-3-3 が「Zenzai に渡す」と記すのと
本書の「同じ `AiBackend` 経路を流用」（roadmap M16 横断）は、この既定で整合する。

### 3.2 Host 側 `AiBackend` インターフェイス

`inference-host/src/AiBackend.cpp`（新規）に次の抽象を実装する。3 消費者の IPC
ハンドラ（`Dispatcher`）は payload を `AiTransformRequest` に正規化して呼ぶ。

```cpp
namespace azookey::host {

enum class AiTask {
  Transform,    // M16: 選択テキストの言い換え/翻訳/要約/自由変換
  Cleanup,      // M58-C: 全文整文（誤字補正・句読点・整文）
  Lint,         // X-3-3: 確定テキストの誤変換検出（複数 finding を返す）
};

enum class AiBackendKind { None, OpenAi, LocalZenzai };

enum class AiErrorClass {
  None, Auth, RateLimit, ServerError, Network, Timeout, Parse, BlockedBySecure, Disabled
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
  // 入口で PrivacyGate を強制チェックし（§8）、backend ごとの実装へ委譲する。
  // 同期 API。非同期化（X-3-3 / streaming）は既存 RequestScheduler
  // (inference-host/src/RequestScheduler.cpp) 側で行う。
  AiTransformResult Transform(const AiTransformRequest& req);
};

}  // namespace azookey::host
```

### 3.3 backend 選択と消費者別既定

- 実効 backend は次の順で解決する（既存契約を流用）。
  1. **secure ガード**（M46）: `PrivacyGate::ExternalAiAllowed()==false` なら外部を
     禁止、`AiCandidateAllowed()==false` なら AI 自体を禁止 → 実効 backend を
     `None` に強制（§8）。`privacy-and-secure-input-spec.md` §5 / §5.2 が正典。
  2. **アプリ別プロファイル**（M48）: `app-profile-spec.md` §4.2 の backend 解決。
     **M48 未実装時はこの段を飛ばし、3 のグローバル設定へフォールバックする**
     （M16 は M48 に依存しない。§13）。
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

### 4.1 M16: `TransformSelectedText`（`docs/legacy-parity-spec.md` §4.4 を拡張・確定）

> `legacy-parity-spec.md` §4.4 の骨子に対し、本節は `max_results` / `results` /
> `error_class` を追加する。本節が当該 payload の正典であり、§4.4 の旧定義は骨子参照に
> 留める（`legacy-parity-spec.md` §4 冒頭の委譲注記参照）。

```
TransformSelectedTextRequest:
  request_id: uint64
  mode: enum { rewrite, translate, summarize, free }
  prompt: string            // ユーザー指示（例 "もっと丁寧に"）。free 以外でも併用可
  selection: string         // 変換対象（空なら呼出側がキャレット段落へ拡張済み）
  context: string           // 前後文脈。includeContextInAITransform=false なら空
  max_results: uint32       // 既定 1。複数案 UI は将来拡張（§14）
TransformSelectedTextResponse:
  request_id
  result: string            // 変換後テキスト（max_results=1 の最良案）
  results: optional<string[]>// 複数案（予約。MVP では未使用）
  error: optional<string>   // 失敗時の分類済みメッセージ（§7.2）
  error_class: optional<string> // AiErrorClass 名（呼出側 fallback 用）
```

`MessageType::TransformSelectedText` を追加。Host は payload を `AiTransformRequest`
（`task=Transform`）へ正規化して `AiBackend::Transform` を呼ぶ。

### 4.2 M58-C: `QueryBatchConversion`（`romaji-batch-conversion-spec.md` §6 が正典）

M58-C は本書では**新規定義しない**。`ai-cleanup` 経路は既存の `QueryBatchConversion`
（`reading` / `raw_romaji` / `mode="ai-cleanup"` / `auto_punctuation`）を使い、Host の
`Dispatcher` が `mode=="ai-cleanup"` のとき payload を `AiTransformRequest`（`task=Cleanup`、
`text=reading`、`raw_romaji`、`auto_punctuation`）へ正規化して `AiBackend::Transform` を
呼ぶ（`left_context` は `QueryBatchConversionRequest` に未実装。M58-C で直近確定文が
必要になった場合に両 spec で同時に追加する）。`romaji-batch-conversion-spec.md`
§6.1 の必須フィールド（`mode=ai-cleanup` のとき `raw_romaji` 必須）と本書 §3.2 の
`AiTransformRequest` を相互に一致させる。`QueryBatchConversion` の `max_candidates`
（文節あたり候補数）は Cleanup task では使わず、Host 正規化時に `max_results=1` 固定とする
（文節候補数は M58-C 側の変換経路で扱う）。

> 共有点: M58-C と M16 は**別 IPC メッセージ**だが、Host 側で同じ
> `AiBackend::Transform` に集約される。これが「3 消費者の共有」の実体である。

### 4.3 X-3-3: `RequestPostCommitLint`（`rich-features-spec.md` §X-3-3 が正典）

X-3-3 も本書では新規定義しない。既存定義（`text` 32 文字 / `app_id` / `findings[]`）を
使い、Host が payload を `AiTransformRequest`（`task=Lint`、`text`、`max_results`=
finding 上限）へ正規化して `AiBackend::Transform` を呼び、`AiFinding[]` を
`RequestPostCommitLintResponse.findings` にマップする。非同期（背景 push）である点は
`rich-features-spec.md`（`LintNotification`）に従う。

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

M16 の `mode` は legacy macOS の target-marker 辞書（`OpenAIClient.swift` の
`Prompt.dictionary`）を Windows 版で 4 値に簡約したものである。`AiBackend` は
`mode` から system プロンプトを選び、`prompt`（ユーザー指示）と `text`（選択）・
`context` を user メッセージに組む。

| `mode` | 意図 | system プロンプト（要旨） |
|---|---|---|
| `rewrite` | 言い換え（かなダブルタップ / Replace Suggestion） | 「入力テキストの意味を保ったまま、ユーザー指示に従って自然な日本語に書き換える。結果のみを返す」 |
| `translate` | 翻訳（指示言語へ） | 「ユーザー指示で指定された言語へ翻訳する。指定が無ければ英語。結果のみを返す」 |
| `summarize` | 要約 | 「入力テキストを簡潔に要約する。結果のみを返す」 |
| `free` | 自由変換（英数ダブルタップ / Magic Conversion） | 「ユーザー指示（prompt）に従い入力テキストを変換する。指示が空なら自然な続き/言い換えを返す。結果のみを返す」 |

- system 文言の最終確定値は実装 PR（M16）でレビューする。本書は**マッピングの
  存在と意図**を契約として固定する（4 値の追加/削除は本書改訂を要する）。
- `context`（前後文脈）は `includeContextInAITransform=true` のとき user メッセージに
  「文脈」として付す。`false` のとき空（§11）。
- M58-C（`task=Cleanup`）の system プロンプトは「誤字補正・句読点挿入・整文。
  `raw_romaji`（生ローマ字）の打鍵誤りパターンを補正に使う。`auto_punctuation` が
  true のとき句読点を挿入する」を要旨とする。詳細は `romaji-batch-conversion-spec.md`
  §5。X-3-3（`task=Lint`）は「確定テキストの不自然箇所（同音異義語・敬語不一致等）を
  検出し、位置・現状・提案・理由・確信度の配列で返す」を要旨とする。

### 5.3 リクエストボディ（chat/completions）

- `messages`: `[{role:"system", content:<§5.2>}, {role:"user", content:<text+prompt+context>}]`。
- `model`: `openAiModel`。
- `stream`: §10 の境界に従う（MVP `false`）。
- **構造化出力**: legacy と同様に `response_format` で JSON 構造を固定する。
  - `Transform` / `Cleanup`: `json_schema` で `{ "result": string }`（`strict:true`、
    `additionalProperties:false`）。
  - `Lint`: `json_schema` で `{ "findings": [{start_char,end_char,current,suggestion,
    reason,confidence}] }`。
- OpenAI 互換だが `response_format` 非対応のエンドポイントを将来許容するため、応答
  パースは「`choices[].message.content` を JSON として解釈し、失敗時はプレーン文字列を
  `result` とみなす」緩いパースを許す（§7.2 の `Parse` エラーは構造致命時のみ）。

### 5.4 応答パース

- 成功: HTTP 200 かつ `choices[0].message.content` を §5.3 のスキーマで解釈。
  `Transform`/`Cleanup` は `result` を、`Lint` は `findings` を取り出す。
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
M32 の GET 経路は `inference-host/src/HttpDownloader.cpp` に実装し、M16 の POST 経路を
追加するときは別の WinHTTP スタックを作らず、同じセッション初期化とポリシーを
共通部へ抽出する。

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
| モデル DL（GGUF 等） | **M32** | GET | `.part`→SHA256 検証→rename、レジューム、`expected.json` ピン照合 |

- モデル DL は既存の確定ファイルが期待 SHA256 と一致する場合、ネットワーク I/O を
  行わない。
  `.part` のレジュームでは `206` の `Content-Range` 始点を検証し、Range を無視した
  `200` は全量取得として `.part` を切り詰める。
  HTTP 失敗または SHA256 不一致では既存の確定ファイルを置換しない。
- M16 実装時に §6.1 の基盤を最小で導入し、M32 は同じ基盤上に GET + SHA256 経路を
  追加する（重複スタックを作らない）。
- M32 を Phase 5 へ前倒ししない（Phase 5 を自己完結に保つ）。roadmap の M16 / M32 /
  M34 依存関係に本契約を反映する（§13）。

---

## 7. エラー / リトライ / レート制限 / タイムアウト

### 7.1 タイムアウト階層

外部 API（OpenAI）のタイムアウトと、Host/IPC のタイムアウト（M47）を**区別**する。
以下の具体値（30 s / 10 s、§7.3 の再試行回数）は目安であり、実装 PR で `openAiTimeoutMs`
（§11）と連動して調整可とする。

| 層 | 対象 | 値（既定） | 備考 |
|---|---|---|---|
| 外部 API（本書） | OpenAI HTTP receive | **30 s**（接続/送信は各 10 s） | `chat/completions`。設定 `openAiTimeoutMs`（§11、既定 30000）で調整可 |
| **外部 AI 経路の IPC deadline** | TIP↔Host（M16 / M58-C の openai backend） | **`openAiTimeoutMs` + 余裕（既定 約 35 s）** | **M47 の Heavy 800 ms を流用しない**。外部 AI 応答は通常 800 ms を超え、Heavy を流用すると正常応答が stale 化して M16 が壊れて見える。`dev-infrastructure-spec.md` の timeout 表「Heavy inference（Magic Conversion 等）」行は本値に従う |
| ローカル経路の IPC（M47） | Zenzai 変換等（外部 AI でない重処理） | M47 規約（Heavy 800 ms 等） | connected-but-silent を timeout 視（M47） |

- **外部 AI 経路（openai backend）は M47 の Heavy 800 ms ではなく、上表の「外部 AI 経路の
  IPC deadline」（`openAiTimeoutMs` + 余裕）で TIP が監視する**。Host は外部 API 待ちの間に
  TIP からの `Cancel`（§6.1）を受理し得る。タイムアウトした要求は破棄し、古い結果が
  後着しても捨てる（staleness check。M47 準拠）。
- 代替として M16 ダイアログ経路を**非同期化**してよい（変換中はスピナー表示で同期 deadline で
  殺さず、応答到着またはユーザーの明示キャンセルで確定）。長い API レイテンシでも M16 が
  壊れないことを保証する。同期 deadline か非同期かは実装 PR で選択する。
- `local-zenzai` backend での AI 経路は外部 API ではないため、ローカル経路の M47 deadline に従う。
- X-3-3 は非同期 push であり、TIP の同期応答タイムアウトには載らない。

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

ログには `reading` / `surface` / 本文を出さない（secure 中は §8、通常時も本文ログは
`dev-infrastructure-spec.md` のログ規約に従う）。エラー本文（API のエラー JSON）は
要旨化してログし、生レスポンスをそのまま残さない。

### 7.3 リトライ方針

- 再試行対象は `RateLimit`（429）と `ServerError`（5xx）と一時的 `Network` のみ。
  `Auth`（401/403）・`Parse` は**再試行しない**（無駄打ち防止）。
- **指数バックオフ + ジッタ**、最大 **2 回**再試行（合計 3 回試行）。`Retry-After`
  ヘッダがあれば優先。総待ち時間が §7.1 のタイムアウトを超えない範囲で打ち切る。
- M16（同期・ユーザー待ち）は体感を優先し再試行を抑制（最大 1 回 or 即時 fallback）。
  X-3-3（非同期・背景）は静かに諦めてよい（finding 0 件として扱う）。

### 7.4 fallback 連鎖（消費者別）

| 消費者 | 一次 | 失敗/`none`/secure 時 | さらに失敗時 |
|---|---|---|---|
| M16 | `aiBackend` 変換 | エラーをユーザーに提示し**選択を変更しない**（原文維持） | — |
| M58-C | `ai-cleanup` | `neural`（zenz かな漢字変換） | かな確定 |
| X-3-3 | `local-zenzai` Lint | Lint 不実行（finding 0 件） | — |

- M58-C の連鎖は `romaji-batch-conversion-spec.md` §5 / M58-C 受け入れ条件が正典。
  `aiBackend=none` のときは `neural` へ自動 fallback。
- いずれの fallback でも Host を落とさない（M47 / M8 と整合）。

### 7.5 劣化 UX（M47 連携）

- M16 は失敗時、原文を維持し、ダイアログ/インジケータで分類済みメッセージ（§7.2）を
  控えめに提示する。secure 由来の抑止は「セーフ入力のため外部 AI を使用しません」を
  明示する（混乱回避）。
- M58-C / X-3-3 の劣化表示は各 spec（M47 の候補ウィンドウ下部インジケータ規約）に従う。

---

## 8. secure ゲート連携（M46 `PrivacyGate`）

`privacy-and-secure-input-spec.md` §5 が正典。本書は `AiBackend` 入口での
**強制ガード**を契約として固定する。

`AiBackend` は host 側にあるため、ここで参照する `PrivacyGate` は
`privacy-and-secure-input-spec.md` §5.1.1 の **host 側インスタンス（二次ゲート）**
である。前面アプリ由来の secure 判定は TIP 側が行い、host 側インスタンスは設定
`privacy.mode` と M48 プロファイル通知（および per-request フラグを持つ経路では
その値）から解決する。`TransformSelectedText` は privacy フラグを持たないため、
secure 中に TIP が Magic Conversion を発火させないことと、本入口ガードの
2 層で担保する（同 §5.1.1）。

- **入口ガード（必須）**: `AiBackend::Transform` の先頭で `PrivacyGate` を問い合わせ、
  - `AiCandidateAllowed()==false` → 実効 backend を `None`、`error_class=BlockedBySecure`
    で早期 return（ローカル zenzai も動かさない）。
  - `ExternalAiAllowed()==false` かつ実効 backend が `OpenAi` → 外部送信を行わず、
    消費者の fallback（§7.4）へ。`local-zenzai` が許可されていればそちらへ寄せる。
  - `privacy-and-secure-input-spec.md` §5 の抑止表は `aiBackend=none` 強制の実装
    ポイントを `inference-host/src/AiBackend.cpp` と明記しており、本契約はそれを満たす。
- **多層防御**: TIP 側でも secure 中は Magic Conversion を無効化する（同 §5 表、
  `TextService.cpp::OnDoubleTap`）。Host 側入口ガードは TIP 側抑止に依存せず**独立に**
  保証する（呼び出し経路の取りこぼし・将来の新消費者に対する安全側）。この二重化は
  同 §5.1.1 の二段ゲートの一部であり、TIP 側抑止だけでは本契約を満たさない。
- **不変条件**: `ExternalAiAllowed() ⇒ AiCandidateAllowed()`（同 §5.1）。`secure` では
  両方 false。`private` / `offline` は AI 候補可・外部不可（ローカルのみ）。
- **ログ**: secure 中は `reading` / `surface` / 本文を redact（同 §5、
  `dev-infrastructure-spec.md` §7.6 優先順位 1）。

---

## 9. API キーの at-rest 保存

> 判断（DEV-346）: **暫定平文 + M34 前倒し**。`dpapi:` prefix 規約を本書で確定し、
> M34（DPAPI 暗号化）を Phase 5 直後へ前倒しする。M16 は M34 を hard prerequisite に
> しない（M16 が M34 にブロックされない）。

- 設定キー `openAiApiKey`（`settings/mvp-settings.schema.json`）に保存する。
- **`dpapi:` prefix 規約**: 値が `dpapi:` で始まる場合、続く Base64 を DPAPI
  （`CryptUnprotectData`、ユーザースコープ）で復号して実キーとする。prefix が無ければ
  **平文**とみなす。M34 以降は設定アプリがキー入力時に DPAPI で暗号化し `dpapi:` 付きで
  保存する（`roadmap` M34 / `sideload-packaging-spec.md` §9）。
- **暫定（M34 前）**: M16 着手時点では平文保存を許容する。README で平文保存を注意喚起
  する（roadmap M34）。`AiBackend` は読み取り時に prefix を見て平文/復号を分岐するため、
  M34 投入後も設定値の移行のみで `AiBackend` の変更を要しない。
- キーをログ・エラーメッセージ・クラッシュダンプに出さない
  （redaction ポリシーの正典は `docs/dev-infrastructure-spec.md` §7.6）。

---

## 10. ストリーミング境界

- **MVP（Phase 5）**: `stream=false`。`chat/completions` の一括応答を待って `result` を
  返す。M16 ダイアログは完了時に一度だけ結果反映（`legacy-parity-spec.md` §4.6）。
- **Phase 6 拡張**: `stream=true`（SSE）。Host が部分トークンを受信し、**push IPC**で
  TIP へ逐次反映する。push 通知は既存の push 経路（`rich-features-spec.md` の
  notification 系）に合わせる。
- **予約フィールド**: 将来の streaming / 複数案のため、本書の payload に予約を置く。
  - `TransformSelectedTextResponse.results`（複数案。MVP 未使用）。
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
| `includeContextInAITransform` | `true` | 前後文脈（直近 paragraph）を含める（§5.2） |
| `batchConversionMode` | `neural` | `neural`/`ai-cleanup`（M58-C） |
| `batchAutoPunctuation` | `false` | `ai-cleanup` 時の句読点自動挿入（M58-C） |
| `postCommitLint` | `false` | X-3-3 の有効化 |

追加提案キー（実装 PR で確定。本書で予約）:

| キー | 既定 | 意味 |
|---|---|---|
| `openAiTimeoutMs` | `30000` | 外部 API receive タイムアウト（§7.1） |

> `mvp-settings.schema.json` への `openAiTimeoutMs` 追加は M16 実装 PR の範囲。本書は
> キーの存在と既定値を契約として予約する（schema 変更は実装 PR でレビュー）。

---

## 12. 受け入れ条件 / テスト観点

本書を満たす M16（および共有経路）の受け入れ条件:

- **M16**: 英数/かなダブルタップで `TransformSelectedText` が発火し、`gpt-4o-mini`
  応答（`mode` 別 system プロンプト）が選択へ置換される（`legacy-parity-spec.md` §4）。
- **共有抽象**: M16 / M58-C / X-3-3 が Host 側で同一 `AiBackend::Transform` を通る
  （別 backend 実装に分岐しない）。ユニットテストで 3 `AiTask` の正規化を検証する。
- **secure ガード**: `PrivacyGate` が `AiCandidateAllowed()==false`（secure）を返すとき、
  `AiBackend::Transform` が外部送信せず `BlockedBySecure` で早期 return する
  （Host 側ガード単体テスト。TIP 抑止に依存しない）。
- **エラー分類**: 401/403/429/5xx/timeout/parse が §7.2 の `AiErrorClass` に分類され、
  再試行対象（429/5xx/一時 Network）のみバックオフ再試行される（モック HTTP で検証）。
- **fallback**: `aiBackend=none` で M58-C が `neural` へ、X-3-3 が Lint 不実行へ落ちる。
- **キー保存**: `dpapi:` prefix の有無で復号/平文を分岐し、平文でも動作する（§9）。
- **HTTP 基盤共有**: M16 の POST 経路が §6.1 の WinHTTP 基盤を使う（M32 が後から同
  基盤に GET+SHA256 を載せられる構造であることをレビューで確認）。

> 既知のテストギャップ・実機検証（OpenAI 実 API への結合テスト、secure 実機確認）は
> Linear に課題として起票し追跡する（roadmap には状態を書かない）。

---

## 13. 依存関係（roadmap への反映）

本契約に伴う `plans/windows-port-roadmap.md` の依存関係明確化:

- **M16 ↔ M32**: M16 は §6.1 の共通 WinHTTP 基盤を最小導入し、M32 はその基盤上に
  GET + SHA256 ダウンロードを実装する。**M32 を Phase 5 へ前倒ししない**。M16 は
  M32 に依存しない（基盤の共有点のみ契約で固定）。
- **M16 ↔ M34**: API キーは §9 の `dpapi:` prefix 規約で保存し、M34（DPAPI）を
  Phase 5 直後へ前倒しする。M16 は M34 を hard prerequisite にしない（暫定平文 +
  README 注意喚起）。
- **M16 ↔ M46**: secure ガードは §8 で `AiBackend` 入口に強制。M46 同時期/先行が
  望ましい（roadmap M16 既述）。`PrivacyGate` 不在時は外部 AI を有効化しない安全側に
  倒す（実装上の暫定は M16 PR でレビュー）。
- **M16 ↔ M48**: backend 解決順（§3.3）の第 2 段がアプリ別プロファイル
  （`app-profile-spec.md` §4.2）。M16 は M48 に依存せず、M48 未実装時はこの段を
  飛ばしてグローバル `aiBackend` へフォールバックする。
- **M58-C / X-3-3**: 本書の `AiBackend` 抽象・fallback・secure 契約を前提に積む。

---

## 14. 未確定事項 / 将来拡張

- M16 の**複数案 UI**（`results[]`）と候補選択 UX（MVP は単一 `result`）。
- X-3-3 の**外部送信オプトイン**（既定は `local-zenzai` 寄せ。明示許可の設計）。
- Phase 6 **streaming**（SSE → push IPC）の詳細プロトコル（中間 `partial` の単位）。
- legacy の target-marker 辞書（絵文字/顔文字/類義語/TeX 等）相当の**拡張 mode**を
  Windows 版に再導入するか（MVP は 4 値）。
- `response_format` 非対応の OpenAI 互換実装に対する互換性マトリクス。
- 実 API 結合テストの CI 組み込み（鍵管理・コスト）。

これらの追跡先は Linear の M16 tracking 課題（DEV-938）である。着手時に確定する
論点として同課題が保持する（本書は状態を持たない）。
