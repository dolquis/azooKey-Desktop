# ローマ字一括変換 仕様（追加機能 / M58）

本書は azooKey-Desktop Windows 版の「ローマ字一括変換（Batch Romaji
Conversion）」機能を定める。`plans/windows-port-roadmap.md` の M58（M58-A /
M58-B / M58-C）が本書を参照する。本書は機能仕様（InputState・IPC payload・
設定項目・ユーザー可視挙動）の正典であり、進捗・状態は持たない（状態の正典は
Linear。`AGENTS.md`「Linear 運用（管制塔）」参照）。

関連 spec:

- `docs/legacy-parity-spec.md` … InputState 状態機械（§1.2）、キーバインド（§1.4）、
  ライブ変換（§2）。本機能はこれらを土台にする。
- `docs/rich-features-spec.md` … AI 変換・文単位予測。M58-C の AI 整文と接続。
- `docs/tsf-deep-integration-spec.md` … 再変換・multi-segment。M58-B の文節再変換と接続。

## 1. 目的（と背景）

「かな漢字変換のたびに思考と視線が中断される」問題を解消する。ユーザーは
ローマ字で文章を最後まで打ち切り、**最後に一度だけ一括変換**する。変換待ち・
候補選択・誤変換修正が入力の流れに割り込まないため、画面を注視せず（相手の目を
見たまま）連続入力できる。

参考: note 記事「ローマ字のまま入力して最後に AI 変換」
（https://note.com/fta7/n/nb8ccb733c425）、議論まとめ
（https://togetter.com/li/2702866）。これらが提起する入力スタイルを、決定的な
ニューラルかな漢字変換（既定）と、AI による誤字補正・整文（オプション）の
2 系統として IME 機能化する。

### 1.1 非目標

- 既定の逐次変換（liveConversion / 予測）の置き換えではない。`batchRomajiConversion`
  を有効化したときのみ動作する追加モードである（既定 OFF）。
- 上流のデータ生成・LLM ホスティングは範囲外。AI 整文は既存 `aiBackend`
  （local-zenzai / openai）に委譲する。
- セキュリティ非目標: `ai-cleanup`（M58-C）は secure 入力のセーフモード契約
  （M46 / `docs/privacy-and-secure-input-spec.md`）に従い、secure 指定時は外部 AI へ
  送らない。secure ゲートを迂回する独自の送信経路は設けない。

## 2. 設計原則

- **蓄積中は IPC を発火しない。** ローマ字→かな変換はローカル（`RomajiKanaConverter`、
  `core/src/RomajiKanaConverter.cpp`）で逐次行い、推論（候補・予測・ライブ変換）の
  クエリは一括変換トリガまで一切送らない。これが「中断ゼロ」の中核。
- **既定 OFF・後方互換。** OFF のときは従来の逐次変換・状態遷移を一切変えない。
- **段階導入。** M58-A（コア）→ M58-B（長文・文節再変換）→ M58-C（AI 整文）。

## 3. InputState への統合

`core/include/azookey/core/InputState.h`（`docs/legacy-parity-spec.md` §1.2 で
定義）の `InputStateKind` に状態を 1 つ追加する。

```cpp
enum class InputStateKind {
    Idle,
    Composing,
    Previewing,
    BatchAccumulating,   // 追加: ローマ字蓄積中（推論クエリを発火しない）
    BatchConverting,     // 追加: 一括変換リクエスト送信済み・応答待ち（候補未確定）
    Selecting,
    ReplaceSuggestion,
    UnicodeInput,
};
```

遷移表（`batchRomajiConversion == true` のとき。主要遷移のみ）:

| 現状 | 入力 | 次状態 | 副作用 |
|---|---|---|---|
| Idle | Input | BatchAccumulating | StartComposition / ローカルでかなバッファ更新 / Preedit 更新 |
| BatchAccumulating | Input | BatchAccumulating | かなバッファ更新 / Preedit 更新（**IPC 無し**） |
| BatchAccumulating | Backspace | BatchAccumulating / Idle | バッファ 1 単位削除（空になれば Idle） |
| BatchAccumulating | StartConversion (Space) | BatchConverting | `QueryBatchConversion` 送信（応答待ち。Preedit は蓄積内容のまま） |
| BatchConverting | 中間応答 (`partial:true`) | BatchConverting | `full_surface` を Preedit に漸進更新（**確定不可**。`segments` を蓄積） |
| BatchConverting | 最終応答 (`partial:false`) | Selecting | 完全な `full_surface` / `segments` を確定可能候補として表示 |
| BatchConverting | Commit (Enter) | BatchConverting | 無視（最終応答前は確定しない。部分結果を commit させない） |
| BatchConverting | Input | BatchAccumulating | in-flight を `Cancel` → 打鍵をバッファに追加 |
| BatchConverting | Cancel (Esc) | BatchAccumulating | in-flight を `Cancel` → 蓄積状態へ戻す |
| Selecting | NextCandidate / SelectByDigit | Selecting | 候補選択（M58-B では文節単位） |
| Selecting | Forward/Backward | Selecting | 文節カーソル移動（M58-B） |
| Selecting | Commit (Enter) | Idle | EndComposition + 全文確定（multi-segment は `CommitSegmentsObservation` §6.4、単一は `CommitObservation`） |
| Selecting | Cancel (Esc) | BatchAccumulating | 変換結果を破棄し蓄積状態へ戻す |
| BatchAccumulating | Cancel (Esc) | Idle | CancelComposition |

`batchRomajiConversion == false` のときは従来どおり Composing / Previewing に
遷移し、本状態は使われない。ライブ変換（M14）・予測（M15）は
BatchAccumulating 中は抑制する。

`QueryBatchConversion` は非同期のため、Space 直後は `Selecting` ではなく
`BatchConverting`（応答待ち）に入る。応答前の追加打鍵 / Esc は in-flight を `Cancel`
して `BatchAccumulating` に戻す（空の候補集合で `Selecting` に入らない）。

ストリーミング拡張（同一 `request_id` への複数応答）は **M58-B 既定では採用しない
（§6.3.5 で決定。将来の任意拡張）**。この拡張を採用した場合に限り `partial:true` の
中間応答が複数届きうる。その場合、中間応答は `BatchConverting` のまま `full_surface`
を Preedit に漸進更新するだけで**確定不可**とし、**最終応答 `partial:false` を受信して
から初めて `Selecting`（確定可能）へ遷移**する。これにより、最初のチャンクだけが確定
して残りの蓄積入力が欠落する事故を防ぐ（`BatchConverting` 中の Enter は無視する）。
M58-B 既定（ストリーミング非採用）では各（サブ）リクエストは `partial:false` の最終
応答を 1 つ返し、TIP は論理バッチ集約（§6.3.3）で全サブリクエスト完了後に `Selecting`
へ遷移する。M58-A は単一リクエスト・単一 `partial:false` 応答で即 `Selecting` へ遷移する。

## 4. 入力中の表示・トリガ・確定

### 4.1 蓄積中の Preedit 表示

設定 `batchRomajiPreviewStyle` で切替える（両対応）。

- `kana`（既定）: `RomajiKanaConverter` の出力かな（例: `nihongo` → `にほんご`）を
  アンダーライン付き Preedit に表示。漢字変換はしない。読みやすく既存 IME に近い。
- `romaji`: 入力された生ローマ字（半角英字、例 `nihongo`）をそのまま Preedit に
  表示。画面を見ない運用に最適。

いずれのモードでも内部ではかなバッファを保持し、一括変換時の読みとして使う。
**`TextService` は `batchRomajiPreviewStyle` に関わらず常に生ローマ字バッファも保持
する**（`kana` プレビューでも破棄しない）。`batchConversionMode=ai-cleanup` は
プレビュー表示と独立に `raw_romaji`（生ローマ字）を必須とする（§4.2・§6.1）ため、
`kana` プレビュー時に生ローマ字を捨てると AI 整文リクエストを組み立てられなくなる。
長音は既存 `RomajiKanaConverter` の仕様どおり `-`（ハイフン）入力で `ー` になる
（例: `ko-` → `こー`）。`ii` / `oo` 等は `いい` / `おお` になり `ー` には正規化
されないため、本書の例も実際の出力に合わせる（`kana` 読みがそのまま `reading` と
して zenz に渡るため、例と実装の不一致は変換品質を損なう）。

### 4.2 一括変換トリガ

**Space（`StartConversion`）をバッファ全体に適用**する。日本語入力中は空白を
打たないため、`docs/legacy-parity-spec.md` §1.4 の既存キーバインドと衝突しない。
`QueryBatchConversion` を送信する際は、`batchRomajiPreviewStyle` の表示設定や
`batchConversionMode` に関わらず、**常に蓄積した全文かなを `reading` に入れる**。
生ローマ字は `reading` に入れず、`raw_romaji` へ別途格納する。`raw_romaji` は
**`mode=ai-cleanup` では必須**、`mode=neural` では任意である（§5・§6.1）。`reading` に
生ローマ字を入れると `neural` 変換および `ai-cleanup`→`neural` fallback が zenz に
アルファベットを渡してしまい、変換が成立しなくなるため。

### 4.3 確定・キャンセル

- Enter（`Commit`）で全文を確定（`EndComposition` + 確定観測。multi-segment は
  `CommitSegmentsObservation`（§6.4）、単一文節は `CommitObservation`）。
- Esc（`Cancel`）は Selecting / BatchConverting からは蓄積状態へ戻し（後者は in-flight を
  `Cancel`）、BatchAccumulating からは破棄。
- M58-B では Selecting 中に ←/→ で文節カーソル移動、Space/数字キーで文節候補切替。

## 5. 一括変換モード（2 系統）

設定 `batchConversionMode` で選択する。

- `neural`（既定 / M58-A）: 蓄積かなを Zenzai/zenz でかな漢字変換する。誤字補正・
  句読点の自動挿入はしない素直な変換。
- `ai-cleanup`（M58-C）: `aiBackend`（local-zenzai / openai）に全文を渡し、
  誤字補正・句読点挿入・整文まで委譲する。非決定的でレイテンシが大きい。
  `includeContextInAITransform` の文脈付与方針と整合させる。`aiBackend == none`
  のときは `neural` に fallback する。**このモードでは `raw_romaji`（生ローマ字）が
  必須**（§6.1）。誤字補正は打鍵そのものの誤りパターンを参照するため。
  **secure 入力（M46 PrivacyGate）では本モードを強制無効化**し、全文を外部 AI へ
  送らない（`neural` / かな確定へ fallback）。`ai-cleanup` は全文を外部 AI
  （OpenAI 等）に渡しうるため、secure-app・パスワード欄の入力が漏れないよう M46 の
  セーフ入力契約（`docs/privacy-and-secure-input-spec.md`）に従う。

## 6. IPC プロトコル

新 `MessageType::QueryBatchConversion` を追加する（`ipc/include/azookey/ipc/Messages.h`
・`ipc/include/azookey/ipc/Payloads.h`）。

### 6.1 Request

```jsonc
{
  "reading": "全文かな",        // 必ず蓄積した全文かな（生ローマ字は入れない）
  "raw_romaji": "nihongo...",   // 生ローマ字。mode=ai-cleanup では必須 / neural では任意
  "left_context": "",            // 直近確定文（任意）
  "mode": "neural",             // "neural" | "ai-cleanup"
  "auto_punctuation": false,     // ai-cleanup 時の句読点自動挿入（batchAutoPunctuation を伝搬）
  "max_candidates": 5            // 文節あたり候補数
}
```

フィールド制約:

- `reading` は常に全文かな（必須）。`mode=neural` はこれを変換入力に使う。
- `raw_romaji` は **`mode=ai-cleanup` では必須**（任意ではない）。AI 整文は
  ユーザーが打った生ローマ字の誤字パターン（アルファベットの打ち間違い）を見て補正
  するため、かなの `reading` だけでは元の誤りを復元できず M58-C の補正挙動を満たせない。
  `mode=neural` では `raw_romaji` は任意。
- `auto_punctuation` は `batchAutoPunctuation` 設定をリクエストに**伝搬**する。句読点
  挿入は host 側で行われるため、設定値をペイロードに載せないと host が ON/OFF を
  判別できず、roadmap M58-C の「ON/OFF で句読点挿入が切り替わる」受け入れ条件を
  満たせない。`mode=neural` では無視される。

### 6.2 Response

```jsonc
{
  "segments": [                  // 文節構造（M58-B で複数、M58-A は単一でも可）
    {
      "surface": "日本語",
      "reading": "にほんご",
      "candidates": [ { "surface": "...", "reading": "...", "score": 0.0 } ]
    }
  ],
  "full_surface": "日本語を入力する", // 全文の最良連結（Preedit 即時表示用）
  "partial": false               // チャンク逐次変換途中は true
}
```

既存 `QueryCandidates`（単一読み・単一候補列）と分離する理由: 一括変換は
**複数文節（segments）構造**を返す必要があり、既存 `Candidate`
（`core/include/azookey/core/Candidate.h`、segments フィールド無し）では表現
できない。multi-segment は `docs/tsf-deep-integration-spec.md` で将来課題と
されていた領域であり、本機能で初めて必須化する。

### 6.3 キャンセル・長文・トランスポート契約

本節は M58-B のトランスポート設計を**確定**する。out-of-band Cancel 経路（§6.3.2）と
ストリーミング拡張の採否（§6.3.5）は本書で決定済みであり、実装はこの決定に従う。

#### 6.3.1 in-flight Cancel と状態

進行中の一括変換は既存 `CancelPayload`（`target_request_id`）でキャンセルする。
`BatchConverting`（応答待ち）中に追加打鍵 / Esc があれば in-flight リクエストを
`Cancel` して結果を破棄し、`BatchAccumulating` へ戻す（§3）。

#### 6.3.2 out-of-band Cancel 経路の確定（決定）

**決定: 案 (a)「Cancel 専用の制御接続（control connection）+ host 共有キャンセル
レジストリ + チャンク境界での協調キャンセル」を採用する。案 (b)（host 非同期
ディスパッチャ）は採用しない。**

決定の根拠（IPC 設計影響の比較）:

- 現行 `NamedPipeServer` は接続ごとに専用スレッドで `ClientLoop` を回す
  （`AcceptLoop` が接続ごとに `std::thread(ClientLoop).detach()` する。
  `ipc/src/NamedPipeTransport.cpp`）。`ClientLoop` はハンドラを同期実行し、応答を
  返すまで**同一接続**の次フレームを読まない。すなわちブロッキングは**接続単位**で
  あり、サーバは複数接続を**並行**処理できる（ヘッダ「One server can accept multiple
  clients」）。したがって `Cancel` を**別接続**（control connection）に載せれば、遅い
  `QueryBatchConversion`（長文 / `ai-cleanup`）を処理中の接続スレッドとは別スレッドで
  Cancel が即時に読まれる。必要なトランスポート変更は「TIP が 2 本目の接続を張る」
  ことだけで、現行 `NamedPipeTransport` の **1 リクエスト 1 応答（request/response）
  契約を温存**できる。
- 案 (b) は、`QueryBatchConversion` ハンドラを即時 `return std::nullopt` してワーカへ
  委譲しない限り同一接続で Cancel を読めない。だが `ClientLoop` は `nullopt` のとき
  応答を書かない（`ipc/src/NamedPipeTransport.cpp` の `if (response && ...)`）ため、
  変換結果を**後から**同じ接続へ届けるには server 起点の push（同一接続への複数応答）
  = multi-response/streaming 拡張が必須になる。これは「正しさは streaming を前提に
  しない」原則（§6.3.5）に反し、本来 optional の拡張へ必須 Cancel 経路を結合させ、
  IPC トランスポート層の作り直しを招く。よって却下する。

確定した機構:

- **host 側 共有キャンセルレジストリ = 既存 `RequestScheduler` の一般化**: 新しい並行
  構造は作らず、**既存 `RequestScheduler`（`inference-host/.../RequestScheduler.{h,cpp}`、
  全 `Dispatcher` が `scheduler_` で共有する process-wide オブジェクト）の cancel-state
  マップを `(trace_id, request_id)` キーへ一般化**して使う。既存の
  `TrackCancellation` / `IsCanceled` / `Cancel` / `CompleteRequest` /
  `PruneInactiveBefore` のライフサイクルをそのまま踏襲する（本書では便宜上
  「キャンセルレジストリ」と呼ぶが実体は `RequestScheduler`）。接続をまたいで共有
  される点は既存と同じ（接続ごとの分離状態とは別に共有）。
  - **レジストリのキーは `(trace_id, request_id)`（`target_request_id` 単独ではない）**:
    `request_id` は allocator ごとの単調増加で**グローバル一意ではない**（TIP の
    `ipc_pending_id_` はインスタンスごと。`docs/dev-infrastructure-spec.md` §相関 ID
    の表）。複数 TIP インスタンス / アプリプロセスが per-user パイプを共有すると
    `request_id` が衝突し、`target_request_id` 単独で引く process-wide レジストリでは
    あるクライアントの `Cancel` が**別クライアントの同 id バッチを誤って canceled に
    する**。そこでキーは、全 envelope 必須でグローバル一意な **`trace_id`（UUIDv7）**
    と `request_id` の組とする（横断追跡キー `(trace_id, request_id)` をそのまま流用。
    `docs/dev-infrastructure-spec.md` の「3 つ目の ID 概念を導入しない」方針に従い、
    新たな session/cancel トークンは足さない）。
  - **前提（クロスマイルストーン依存・M51）**: 上記キーがクロスクライアント分離として
    機能するには、`trace_id` が実際にグローバル一意（UUIDv7）でなければならない。現行
    TIP は envelope に**定数 `trace_id`**（`tsf-tip/src/TextService.cpp` の `"tip-key-query"`
    / `"tip-faf"` 等）を載せており、UUIDv7 採番は **M51**（`docs/dev-infrastructure-spec.md`
    §7.7.1 / `plans/windows-port-roadmap.md` M51）のスコープである。M51 の trace 生成が無い
    まま M58-B を実装すると、複数 TIP インスタンスが同一の定数 `trace_id` を共有し
    `request_id` も衝突しうるため、`(trace_id, request_id)` でも一意にならず本節が防ごう
    とするクロスクライアント誤キャンセルが再発する。したがって **M58-B は M51 の UUIDv7
    `trace_id` 生成・伝播を前提**とする（roadmap M58-B 前提に明記）。M58-B を M51 本体より
    先行させる場合は、out-of-band cancel 対象 envelope（batch query / cancel）に限り UUIDv7
    `trace_id` を採番する処理を M58-B スコープに含め、M51 の生成セマンティクス（1 論理操作
    = 1 `trace_id`、§7.7.1）と一致させること。いずれにせよ定数 `trace_id` の現状実装の
    ままでは本節のクロスクライアント分離は成立しない。
  - `MessageType::Cancel` を受けた接続は（どの接続でも）受信 envelope の `trace_id` と
    `target_request_id` で `scheduler_->Cancel(trace_id, target_request_id)` を呼ぶ。
  - `QueryBatchConversion` ハンドラは処理開始時に自身の `(trace_id, request_id)` を
    `TrackCancellation(trace_id, request_id)` で登録し、§7 の host 側チャンク分割の
    **各チャンク境界で `IsCanceled(trace_id, request_id)` を協調的にポーリング**する。
    canceled なら以降のチャンク変換を中止し、現接続へ「canceled」応答（部分結果なし・
    確定不可）を 1 つ返して終了する（1 リクエスト 1 応答契約を維持）。
  - パイプにバッファされ未読のサブリクエストは、`ClientLoop` が読み出した時点で
    ハンドラが**冒頭で `IsCanceled` を確認**し、変換せず canceled 応答で短絡する。
  - **終端パスでの cleanup（必須・リーク防止）**: ハンドラは**全ての終端パス**
    （最終 `partial:false` / canceled / エラー応答のいずれを返す場合も）で
    `CompleteRequest(trace_id, request_id)` を呼ぶ（既存 `Dispatcher` が
    `scheduler_->CompleteRequest(request_id_)` を呼ぶのと同じ。`active_count` を減算し
    0 で当該エントリを erase）。これにより長時間稼働 host でバッチごとのエントリが
    無限に残らない。
  - **送信済みサブリクエストの cancel マーカは読み出し / 終端まで保持（必須）**:
    論理バッチは複数サブリクエストを primary パイプにまとめて送るため、先頭の長い
    変換中に後続サブリクエストは**未読のままキュー**に残る。これらに対する `Cancel`
    （Esc / 追加打鍵）で生じるマーカは `active_count == 0`（まだ `TrackCancellation`
    されていない）だが、**`ClientLoop` がそのフレームを読むまでプルーンしてはならない**。
    プルーンすると、ハンドラ冒頭の `IsCanceled(trace_id, request_id)` がマーカを取り
    こぼし、キャンセル済みの queued サブリクエストが変換されてしまい out-of-band
    cancel 契約を破る。したがって、**TIP が実際に送った `(trace_id, request_id)` の
    マーカは、対応フレームが読まれて終端（`CompleteRequest`）に達するまで保持**する。
  - **孤児マーカのプルーン**: query が一度も来ない `(trace_id, request_id)` への
    `Cancel`（真の取りこぼし）のみを、`active_count == 0` の非アクティブマーカとして
    回収し cancel-state マップの非有界増加を防ぐ。`trace_id` キーはクライアントを
    またぐため単調 `request_id` 軸では掃けないので、プルーンの TTL は**queued サブ
    リクエストが未読で待ちうる最大時間（per-サブリクエスト / 論理バッチのタイム
    アウト予算）より長く**取り、上記「送信済みマーカ保持」を侵さない（具体値は実装で
    規定）。
- **TIP 側 制御接続**: TIP は同一パイプへ 2 本の `NamedPipeClient` 接続を張る。
  - primary: `QueryBatchConversion` / `CommitSegmentsObservation` 等の query と応答。
  - control: `Cancel`（fire-and-forget。ハンドラは `nullopt` 応答）専用。`BatchConverting`
    中の Esc / 追加打鍵で、in-flight な各サブリクエスト ID に対し control 接続から
    `Cancel` を送る（§6.3.3 論理バッチ）。**control 接続から送る `Cancel` envelope の
    `trace_id` は、対象バッチ（primary で送った `QueryBatchConversion`）の `trace_id` と
    同一にする**。host は `(trace_id, target_request_id)` でキャンセル対象を一意に
    特定するため、これにより別 TIP インスタンス / 別バッチの誤キャンセルを防ぐ
    （§6.3.2 レジストリ）。1 論理バッチの全サブリクエストは同一 `trace_id` を共有する
    （1 論理操作 = Space トリガの一括変換。`docs/dev-infrastructure-spec.md` の
    `trace_id` 粒度に整合）。
  - **control 接続は最初に自身の Handshake を完了してから Cancel を送る（必須）**:
    host の認証状態は**接続単位**で持たれる（`inference-host/src/main.cpp` は接続ごとに
    `Dispatcher` を生成し、`Dispatcher::Dispatch` は `AZOOKEY_IPC_HANDSHAKE_TOKEN` /
    `--handshake-token` 設定時、Handshake 以外を `authenticated_` 確立まで
    `HandleUnauthenticated` で拒否する。`Cancel` も認証ゲートの後）。したがって control
    接続が Handshake を踏まないと、token 保護構成では `Cancel` が未認証として捨てられ
    out-of-band cancel が**無言で効かなくなる**。control 接続は primary と同じ
    `handshake_token` で Handshake を済ませてから Cancel を送る。
  - 共有 `CancellationRegistry` / scheduler は process-wide（全接続の `Dispatcher` が同一
    インスタンスを参照。`inference-host/src/main.cpp`）なので、認証済み control 接続から
    の `Cancel` は primary 接続で in-flight な変換に到達する（§6.3.2 冒頭の機構）。
- **capability ネゴシエーション**: out-of-band cancel は host 側の共有レジストリ +
  協調キャンセル実装に依存する。§6.4.3 で追加する `HandshakeResponse.capabilities` に
  **`"oob_cancel"`** を載せて広告する。
  - TIP は応答 capabilities に `"oob_cancel"` が**含まれるときだけ** control 接続経由の
    out-of-band Cancel に依存する。
  - 含まれないとき（旧 host）は、TIP は primary 接続に best-effort で `Cancel` を送り
    （現行 query 完了後に処理される）、ローカル状態遷移で in-flight 結果を**到着時に
    破棄**する（M58-A 相当）。長文 / `ai-cleanup` の即時キャンセルは保証されないため、
    その構成では M58-B の長文経路は `oob_cancel` 対応 host を要求する。

#### 6.3.3 論理バッチ集約・タイムアウト・確定可否

- **論理バッチ（複数サブリクエストの集約）**: TIP がフレーム上限超の蓄積を複数の
  `QueryBatchConversion` へ事前分割した場合、それらを **1 つの論理バッチ**として扱う。
  TIP は全サブリクエストの `request_id` 集合と、各サブリクエストの最終応答
  （`partial:false`）受信状況を管理し、**全サブリクエストが最終応答を返してから初めて**
  `BatchConverting` → `Selecting` へ遷移する（最初のチャンクの最終応答で `Selecting`
  に入らない）。`full_surface` / `segments` は送信順どおりに連結する。`CancelPayload`
  は単数（`target_request_id`）のため、キャンセル時は **in-flight な各サブリクエスト
  ID に対して control 接続から `Cancel` を 1 件ずつ送る**（§6.3.2。取りこぼした
  サブリクエストを走らせ続けない）。M58-A（単一リクエスト）は論理バッチがサブ
  リクエスト 1 件の特殊ケース。
- **確定可否**: 論理バッチは全サブリクエストが `partial:false` を返すまで**確定不可**。
  途中の Enter は無視する（部分結果を commit させない。§3）。
- **タイムアウト戦略**: TIP は各 in-flight サブリクエストに受信タイムアウト `T_sub` を
  設ける（`NamedPipeClient::ReceiveWithTimeout` を利用）。`T_sub` 超過は当該サブ
  リクエストの**失敗**とみなす。論理バッチ全体にも上限予算を設けてよい（具体値は
  実装で規定。spec は戦略のみ固定）。
- **失敗時方針（部分確定しない）**: いずれかのサブリクエストがタイムアウト / エラーに
  なった論理バッチは**部分確定しない**。TIP は残る in-flight 全サブリクエスト ID へ
  control 接続から `Cancel` を送り、§7 の fallback 連鎖（`ai-cleanup` → `neural` →
  かな確定）で入力を失わずに確定可能にする。
- **stale 応答の drain（必須）**: `ReceiveWithTimeout` はフレーム到着待ちの
  タイムアウト時に応答を消費せず `nullopt` を返す（`ipc/src/NamedPipeTransport.cpp`。
  相の定義は `docs/dev-infrastructure-spec.md` §6.4.8。フレーム転送中に read ハード
  デッドラインを超えた場合のみ接続を切断するため、以下の drain は接続が維持されて
  いる場合の規約である）。host 側 `ClientLoop` は
  ハンドラ完了後に最終 / canceled 応答を **primary 接続に必ず書き込む**ため、タイム
  アウト / キャンセルした各サブリクエストの応答は遅れて primary パイプに到着する。
  したがって TIP は、**fallback / 新しいバッチを同じ primary 接続へ送る前に**、次の
  いずれかで stale 応答を排除しなければならない:
  - **(推奨) `(trace_id, request_id)` 相関**: TIP は全応答を envelope の
    `(trace_id, request_id)`（`ipc/include/azookey/ipc/Messages.h`・
    `docs/dev-infrastructure-spec.md` の横断追跡キー）で照合し、**現在 await 中の
    in-flight サブリクエスト集合に属さない応答は drain して破棄**する（タイムアウト /
    キャンセル済みの遅延応答を含む）。`request_id` 単独はインスタンスごと採番で
    衝突しうるため、グローバル一意な `trace_id` を組で使う（§6.3.2）。これにより
    古い `segments` が新しい入力に誤って結合される事故を防ぐ。
  - **(代替) primary 接続の再接続**: タイムアウト / キャンセル後に primary
    `NamedPipeClient` を `Disconnect` → 再 `Connect`（+ 再 Handshake）して未読の stale
    応答を捨てる。
  どちらの場合も、`(trace_id, request_id)` での応答相関は論理バッチの正しさの前提と
  する（送信順だけに依存して応答を結合しない）。

#### 6.3.4 フレーム上限と分割（シリアライズ後バイト基準）

- 1 リクエストは `ipc/include/azookey/ipc/Limits.h` の `kMaxJsonInputBytes` /
  `kMaxFrameSize`（ともに 1 MB）以内に収める必要がある。IPC フレーミング/パーサが
  この上限を超えるフレームを `inference-host` 到達前に拒否するため、フレーム上限を
  超えうる蓄積バッファは **TIP 側で文境界により複数リクエストへ事前分割**してから
  送る（M58-B）。host 側分割は到達済みリクエストにしか効かず、フレーム上限超は救済
  できない。
- **分割判定はシリアライズ後のフレームバイト数で行う**: IPC 上限は**エンコード後の
  フレーム**に適用される。`QueryBatchConversion` は `reading` に加え（`ai-cleanup` では）
  `raw_romaji` を必須で重複保持し、JSON/エンベロープのオーバーヘッドも乗るため、
  元のかなバッファ長が 1 MB 未満でもシリアライズ後に上限超となりうる。TIP は
  **エンコード後のリクエスト/エンベロープサイズ（重複フィールド込み）**を測って分割
  境界を決める（元バッファの文字数ではなく）。特に `ai-cleanup` は `reading` +
  `raw_romaji` で実効ペイロードがほぼ倍になる点に注意する。
- これとは別に、フレーム上限内のリクエストでも zenz のコンテキスト長に収めるため、
  `inference-host` 側でさらに文境界でチャンク分割して逐次変換する（§7）。

#### 6.3.5 トランスポート契約とストリーミング拡張の採否（決定）

- **トランスポート契約と `partial` の扱い**: 現行の Named Pipe は **1 リクエストにつき
  1 Envelope 応答**の request/response 契約である（`ipc/include/azookey/ipc/NamedPipeTransport.h`
  のハンドラ戻り値・`ipc/src/NamedPipeTransport.cpp` の `ClientLoop` は単一応答のみ書き出す）。
  したがって**正しさの担保には streaming を前提にしない**: 各 `QueryBatchConversion`
  （およびサブリクエスト）は host 側で内部チャンク分割・変換・連結を済ませ、
  **`partial:false` の最終応答を 1 つだけ返す**。長文の進捗フィードバックは、TIP 側
  論理バッチで**サブリクエストが 1 つ完了するたびに Preedit を更新**することで得る
  （サブリクエストはフレーム上限超のときのみ複数になる）。
- **ストリーミング拡張の採否（決定）**: M58-B の既定（正しさ）経路では request 内の
  `partial:true` ストリーミング（同一 `request_id` への複数応答）を**採用しない**。
  採用には案 (b) と同じ multi-response トランスポート改造（`NamedPipeTransport` の
  多重応答対応）が必要であり、§6.3.2 で温存した 1 リクエスト 1 応答契約を崩すため、
  **M58-B 必須スコープから除外**し、将来マイルストーンの任意拡張に回す。これにより
  M58-B が必須とするトランスポート変更は「control 接続 + 共有キャンセルレジストリ +
  協調キャンセル」（§6.3.2）に限定され、multi-response 改造を要しない。`partial:true`
  中間応答は送られず、`Response.partial` フィールド自体は将来拡張のために予約する
  （既定では常に `false`）。`partial:true` を扱う §3 遷移表の行は、この任意拡張を
  採用したときにのみ有効になる。

### 6.4 multi-segment commit observation（M58-B / M59 / M60 共有）

文節列を **1 メッセージで原子的に確定・学習**するため、新
`MessageType::CommitSegmentsObservation` を追加する（`ipc/include/azookey/ipc/Messages.h`・
`Payloads.h`）。M58-B（一括変換の文節列確定）・M59（句読点込み文の確定、自動句読点除外。
`docs/dynamic-punctuation-spec.md` §5.3）・M60（英単語確定 `reading=生ローマ字`。
`docs/inline-english-candidate-spec.md` §6.4）が共有する正典 payload。

#### 6.4.1 Payload

```cpp
struct ObservedSegment {
  std::string reading;                 // 文節読み（is_auto_punctuation=true は空）
  CandidateField chosen;               // 確定候補（surface はテキストに含む。tag/source 付き）
  std::vector<CandidateField> shown;   // その文節で提示した候補（任意・学習文脈）
  bool is_auto_punctuation{false};     // true: テキストには含むが Observe しない（M59）
};

struct CommitSegmentsObservationRequest {
  std::vector<ObservedSegment> segments;  // 送信順 = テキスト連結順
  std::string left_context;               // 文全体の直前確定文脈（文頭のみ）
  uint64_t timestamp_ms{};
};

struct CommitSegmentsObservationResponse {
  bool ok{false};
};
```

```jsonc
{
  "segments": [
    { "reading": "きょうは",       "chosen": { "surface": "今日は",   "reading": "きょうは",       "score": 0.95, "source": "model", "tag": 0 }, "shown": [], "is_auto_punctuation": false },
    { "reading": "いいてんきです", "chosen": { "surface": "いい天気です","reading": "いいてんきです","score": 0.90, "source": "model", "tag": 0 }, "shown": [], "is_auto_punctuation": false },
    { "reading": "",               "chosen": { "surface": "。",        "reading": "",               "score": 0.0,  "source": "punct", "tag": 0 }, "shown": [], "is_auto_punctuation": true }
  ],
  "left_context": "",
  "timestamp_ms": 0
}
```

#### 6.4.2 host セマンティクス

既存 `InferenceEngine::CommitObservation`（`store_->Observe` +
`active_converter_->Commit`）を文節列へ一般化する:

```text
ctx = request.left_context
for seg in request.segments:                       # 送信順
    if !seg.is_auto_punctuation:
        store_->Observe(seg.reading, seg.chosen.surface, alpha, ts)
        active_converter_->Commit(Candidate{seg.chosen.surface, seg.reading, ...}, ctx)
    ctx += seg.chosen.surface                       # 句読点含め文脈を伸ばす
return { ok: true }
```

- `is_auto_punctuation` セグメントは Observe / Commit しないが、`ctx` には連結する
  （後続文節の文脈に句読点を反映）。
- **原子性**: 1 メッセージで全文節を確定し、N 回の単発 `CommitObservation` 往復を置換。
- `left_context` は文頭の 1 つだけ送り、host が文節ごとに伸ばす（TIP は文節別 context を
  組まなくてよい）。

#### 6.4.3 後方互換・capability ネゴシエーション

- 単一文節確定（通常確定 / M58-A 単一）は既存 `CommitObservation`（単発）を継続使用。
  本 payload は廃止・置換しない。
- 旧 host は新 `MessageType` を解せない。**TIP は host が `CommitSegmentsObservation` を
  解せると確認できたときだけ送る**。確認は Handshake で行うが、**現行スキーマでは
  `capabilities` は `HandshakeRequest` 側にしか無く**（`ipc/include/azookey/ipc/Payloads.h`。
  これは TIP→host の広告）、host→TIP の広告フィールドが存在しない。そこで M58-B は
  **`HandshakeResponse` に host 側 `capabilities` を追加**する:

  ```cpp
  struct HandshakeResponse {
    std::string host_version;
    int protocol_version{1};
    bool accepted{false};
    bool model_loaded{false};
    std::vector<std::string> capabilities;   // 追加: host が解せる拡張機能の広告
  };
  ```

  このフィールドは複数の host 拡張機能を広告する共通枠であり、M58-B では少なくとも
  **`"commit_segments"`**（本節）と **`"oob_cancel"`**（§6.3.2 の out-of-band Cancel
  対応）を載せうる。TIP は各 capability 文字列の有無で当該拡張への依存可否を個別に
  判定する。

  - host は対応時に `capabilities` へ **`"commit_segments"`** を載せて応答する。
  - TIP は `HandshakeResponse.capabilities` に `"commit_segments"` が**含まれるときだけ**
    `CommitSegmentsObservation` を送る。
  - Build/Parse は既存流儀（`BuildHandshakeResponse` で `o.emplace("capabilities", array)`、
    `ParseHandshakeResponse` で配列が無ければ**空**）。**後方互換**: 旧 host は本フィールドを
    返さない → TIP からは空 capabilities = 未対応とみなす。
- **未対応 host では TIP がフォールバック**して、`!is_auto_punctuation` の各文節を既存
  `CommitObservation` で順次送る（自動句読点は学習に送らない、という不変条件は同じ）。

#### 6.4.4 M58-B 統合点

- M58-B の Selecting 確定（Enter）で、各 segment の**選択候補**を `ObservedSegment.chosen`
  に、残りを `shown` に詰めて送る（§6.2 の `segments[].candidates` から選ぶ）。
- §6.2 応答の `full_surface` は `ObservedSegment.chosen.surface` の連結と一致する
  （不一致はバグ）。これにより §3 遷移表「Selecting →（Enter）→ Idle: EndComposition +
  CommitObservation（全文確定）」は本 payload を使う（下記注記）。

#### 6.4.5 M59 / M60 統合点

- **M59**: 句読点込み文の確定で、`!auto_punctuation` 文節を `chosen` に、自動句読点を
  `is_auto_punctuation=true`（`reading=""`、`chosen.surface="、"`/`"。"`）として送る。
  host は前者のみ学習、後者は `ctx` 連結のみ。`docs/dynamic-punctuation-spec.md` §5.3 /
  §7.4 の「文節ごと複数回 CommitObservation」を本 payload に統合する。
- **M60**: 英単語確定が文の一部（multi-segment）の場合、その `ObservedSegment.reading=
  生ローマ字`、`chosen.tag=English`。単発確定なら既存 `CommitObservation`
  （`reading=生ローマ字`）を使う。

#### 6.4.6 テスト（`ipc/tests/payloads_test.cpp`）

- `CommitSegmentsObservationRequest` の round-trip（`segments[]` の `reading` /
  `chosen`(tag 含む) / `shown[]` / `is_auto_punctuation` / `left_context` / `timestamp_ms`）。
- `is_auto_punctuation=true` 要素が学習対象外であることを host テスト（`inference-host/tests`）で
  検証（Observe 呼び出し回数 = `!is_auto_punctuation` 文節数）。
- `HandshakeResponse.capabilities` の round-trip（`"commit_segments"` を含む応答・欠落応答の
  build/parse）。欠落時に空 capabilities へパースされ、TIP が単発 `CommitObservation`
  フォールバックを選ぶこと（§6.4.3）。

## 7. 長文・性能・失敗時（主に M58-B）

- **TIP 側事前分割（フレーム上限対策）**: シリアライズ後のフレームサイズが IPC フレーム
  上限（`kMaxFrameSize` = 1 MB）を超えうる場合、TIP は送信前に文境界で複数の
  `QueryBatchConversion` リクエスト（各々上限未満）へ分割して送る。**分割判定は
  エンコード後のフレーム/エンベロープバイト数**（`reading` +（ai-cleanup の）`raw_romaji`
  + オーバーヘッド込み）で行い、元バッファ長では判定しない（§6.3）。フレーム上限を
  超える単一リクエストは IPC 層で拒否されるため、host 側分割では救済できない。
  **文境界が無い場合のフォールバック分割**: 文境界（句点・改行・推定文節境界）で
  各サブリクエストを `kMaxFrameSize` 未満に収められない場合（例: 長い無区切りの
  ローマ字 / ASCII 連続列）、TIP は**バイト安全なハード分割**（UTF-8 境界を壊さない
  位置で、シリアライズ後サイズが上限の安全マージン以内になるオフセットで強制分割）に
  切り替える。これにより、上限超フレームの送信や変換不能を回避する。蓄積バッファ
  自体にも早期ハードキャップ（後述「蓄積長上限」）を設ける。
  分割した複数リクエストは 1 つの論理バッチとして集約し、全サブリクエストの最終応答
  受信で `Selecting` へ、Esc / 追加打鍵では全 in-flight サブリクエストへ `Cancel` を
  送る（§6.3）。
- **host 側チャンク分割（モデルコンテキスト対策）**: フレーム上限内の 1 リクエストでも、
  `inference-host` 側で文境界（句点・改行・推定文節境界）により分割し、zenz の
  コンテキスト長制限内で逐次変換 → 結合する。**分割・連結は host 内部で完結し、
  当該リクエストには `partial:false` の最終応答を 1 つだけ返す**（現行トランスポートの
  1 リクエスト 1 応答契約に従う。§6.3）。request 内の `partial:true` 逐次表示は
  multi-response トランスポート拡張を要するため **M58-B では非採用**（§6.3.5。将来の
  任意拡張）。
- **host 側のコンテキスト超フォールバック分割**: TIP のフレーム上限分割とは別問題として、
  フレーム上限内（< 1 MB）でも zenz のコンテキスト長を超え、かつ文境界（句点・改行・
  推定文節境界）が無いリクエスト（長い無区切り入力）があり得る。この場合 host チャンカは
  文境界が無くても**バイト/トークン安全なハード分割**（UTF-8 / トークン境界を壊さず、
  各チャンクが zenz コンテキスト長内に収まる位置で強制分割）に切り替える。これにより
  モデルコンテキスト溢れや変換不能を回避する。
- **蓄積長上限**: バッファ長の上限と超過時の挙動を実装で規定する。文境界が無い
  無区切り入力でも各サブリクエストが `kMaxFrameSize` 未満になるよう、バイト安全な
  ハード分割（上記）または早期ハードキャップ（警告ビープ + 以降の打鍵抑制 / 強制
  変換）で必ず救済し、上限超フレームを送らないことを保証する。
- **非同期・キャンセル可能**: 一括変換は UI スレッドをブロックしない。
- **fallback 連鎖**: `ai-cleanup` 失敗時は `neural` へ、`neural` 失敗時はかなのまま
  確定可能にする（入力を失わない）。

## 8. 設定スキーマ

`settings/mvp-settings.schema.json` に以下を追加する（`additionalProperties:false`
を維持。`description` に対応 M を記載する既存流儀に合わせる）。

| キー | 型 | 既定 | 説明 |
|---|---|---|---|
| `batchRomajiConversion` | boolean | `false` | M58-A: ローマ字を蓄積し最後に一括変換するモードを有効化 |
| `batchRomajiPreviewStyle` | enum `kana`/`romaji` | `kana` | M58-A: 蓄積中 Preedit の表示（かなプレビュー / 生ローマ字） |
| `batchConversionMode` | enum `neural`/`ai-cleanup` | `neural` | M58-A/M58-C: 一括変換の処理系（zenz かな漢字変換 / AI 整文） |
| `batchAutoPunctuation` | boolean | `false` | M58-C: `ai-cleanup` 時に句読点を自動挿入 |

## 9. テスト計画

- **状態機械** (`core/tests/input_state_test.cpp`): `batchRomajiConversion` ON で
  Idle→BatchAccumulating→（Space）→BatchConverting→（`partial:false`）→Selecting→
  （Enter）→Idle、`partial:true` 中間応答では BatchConverting に留まり Enter が
  確定しないこと、BatchConverting 中の追加打鍵 / Esc で in-flight Cancel →
  BatchAccumulating 復帰、Esc の戻り、OFF で従来遷移が不変であることを網羅。
- **IPC** (`ipc/tests/payloads_test.cpp`): `QueryBatchConversion` の build/parse、
  segments 構造の往復、`partial` フラグ、Cancel の ID 整合。`HandshakeResponse.
  capabilities` の round-trip（`"oob_cancel"` を含む応答 / 欠落応答）と、欠落時に TIP が
  out-of-band Cancel に依存せず best-effort fallback を選ぶこと（§6.3.2）。
- **out-of-band Cancel** (`inference-host/tests` + `ipc/tests`): 共有
  `CancellationRegistry` 経由で、別接続（control 接続相当）からの `Cancel` が in-flight な
  長い `QueryBatchConversion` をチャンク境界で中止させ、canceled 応答（部分結果なし・
  確定不可）を 1 つ返すこと。パイプにバッファされ未読のサブリクエストが冒頭の
  `IsCanceled` チェックで変換せず短絡すること（§6.3.2）。token 保護構成
  （`handshake_token` 設定）では、Handshake 前の control 接続からの `Cancel` が
  `HandleUnauthenticated` で捨てられ in-flight を止めないこと、Handshake 済み control
  接続からの `Cancel` が共有 registry/scheduler 経由で停止させることの双方を検証（§6.3.2）。
  **クロスクライアント分離**: 2 つのクライアント（別 `trace_id`）が同じ `request_id` を
  持つとき、片方の `(trace_id, request_id)` への `Cancel` が他方の in-flight バッチを
  キャンセルしないこと（レジストリが `(trace_id, request_id)` でキーされ、`request_id`
  単独では引かないこと。§6.3.2）。本テストは各クライアントが**一意な UUIDv7 `trace_id`**
  を持つこと（M51 の trace 生成前提。§6.3.2 前提）を前提とし、定数 `trace_id` のままでは
  分離が成立しないことを併せて確認する。**レジストリ cleanup** (`scheduler_test.cpp`):
  終端パス（最終 `partial:false` / canceled / エラー）で `CompleteRequest(trace_id,
  request_id)` によりエントリが erase されること、query が走らなかった孤児 cancel
  マーカがプルーンされ cancel-state マップが非有界増加しないこと（§6.3.2 cleanup）。
- **論理バッチ集約・タイムアウト** (`core/tests` + `inference-host/tests`): 複数サブ
  リクエストの全 `partial:false` 受信まで `Selecting` へ遷移しないこと、いずれかの
  サブリクエストが `T_sub` タイムアウト / エラーのとき部分確定せず全 in-flight を
  Cancel して fallback 連鎖（`ai-cleanup`→`neural`→かな確定）へ進むこと（§6.3.3）。
  タイムアウト後に遅れて届く stale 応答が `(trace_id, request_id)` 相関で drain・破棄され、
  fallback / 新バッチの応答に古い `segments` が結合されないこと（§6.3.3 stale 応答の drain）。
- **変換** (`inference-host/tests`): 固定かな全文 → 妥当な segments / full_surface。
  チャンク分割（句点を含む長文）・キャンセルの動作。
- **手動 / 実機（Win11、`gate:human-required`）**: `kyouhaiitenkidesu`（→ きょうは
  いいてんきです → 今日はいい天気です）等を蓄積 → Space で一括変換 → Enter 確定で
  妥当な日本語が入る。設定 OFF で従来逐次動作が不変。`batchRomajiPreviewStyle`
  切替が反映される。

## 10. 受け入れ条件（M58 全体）

M58-A / M58-B / M58-C ごとの受け入れ条件は `plans/windows-port-roadmap.md` の
各マイルストーンに定義する（本書は仕様、roadmap は受け入れ条件の「定義」、
達成状態は Linear）。
