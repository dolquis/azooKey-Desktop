---
kind: spec
milestones: [M41, M51]
canonical_for: ["構造化ログと可観測性"]
---

# 構造化ログと可観測性 仕様（M41・M51）

対象リポジトリ: dolquis/azooKey-Desktop
対応マイルストーン: M41（§7）、M51（§7.7）
関連: `docs/dev-infrastructure-spec.md`（開発基盤の入口と転送表）、
      `docs/ipc-persistence-hardening-spec.md`（IPC の堅牢化）、
      `docs/host-availability-spec.md`（Host の再接続と劣化モード）、
      `docs/privacy-and-secure-input-spec.md`（§7 が参照するプライバシー判定）、
      `plans/windows-port-roadmap.md` M41 / M51

本書は `docs/dev-infrastructure-spec.md` から §7を切り出した文書である。節番号 §7.x は旧 spec のまま保ち、旧 spec の転送表が§7を本書へ解決する。本書内で、本書にない節（旧 spec の他の節）を指すときは、文書名を付けて書く（例: `build-ci-spec.md` §4.3）。

## 正典範囲

構造化ログ（JSON Lines）、相関 ID とフェーズ、エラーコード体系、タイムアウト規約、redaction ポリシー（§7.1〜§7.6）と、
M51 の `trace_id`・計測フェーズ・`azookey_trace_viewer`・オーバーヘッド制御（§7.7）は本書が正典である。

## 節とマイルストーンの対応

| 節 | マイルストーン |
|---|---|
| §7（§7.7 を除く） | M41 |
| §7.7 | M51 |

## 7. 構造化ログと可観測性（M41）

### 7.1 解決する問題

TIP は `OutputDebugStringA`（DebugView）、Host は stderr 中心。遅延要因
（TIP 側 / Pipe 側 / Host 側）の切り分けと、エラー分類の体系がない。

#### 7.1.1 M41 v1 スコープ

M41 v1 は、Release ビルドで障害の一次情報を採取するためのファイルロガーを
TIP と Host に導入する。§7.2 以降が定める最終形のうち、相関 ID と phase
計測は M51 へ残し、次の契約を実装する。

- `AZOOKEY_LOG=1` のときだけ有効化する。未設定または `1` 以外では、
  Debug / Release のどちらもファイルを作成しない。
- `AZOOKEY_LOG_LEVEL` は `info`、`warn`、`error` のいずれかを受け付ける。
  未設定または不正値では `info` を使う。
- TIP は `AZOOKEY_LOG_KEYS` が厳密に `1` のときだけ、キー単位の経路（`key_path` 系の
  info レコード）を加える。キー・読み・表層は含めず、IME が切れている間、
  キーボード無効の context、secure 中の打鍵は、フォーカスした文書と理由の組ごとに
  長さを含まない 1 件にまとめる。
  項目は `docs/debugging.md` の「キー単位の経路を収集する」が持つ。
- 出力先とファイル名は `ipc-persistence-hardening-spec.md` §5.2 の
  `%LOCALAPPDATA%\azooKey\logs\host-YYYYMMDD.jsonl` と
  `tip-YYYYMMDD.jsonl` とする。
- 各行の必須フィールドは `ts`、`component`、`level`、`event` とする。
  `request_id`、`result`、`error_code` など、既存の呼び出し位置で得られる
  メタ情報は構造化フィールドとして追加してよい。
- 現行ファイルが 5 MiB を超える前にローテーションし、同じ日付の
  `.1` から `.3` まで 3 世代を保持する。`.1` が直前の世代である。
- UTC 日付単位で当日を含む直近 7 日分を保持し、それより古い TIP / Host の
  日次ファイルとローテーション世代は、各プロセスの初回書き込み時に削除する。
- 複数プロセスから同じ component のファイルへ書く場合は名前付き mutex で直列化する。
  待機は最大 200 ms とし、取得できなければ入力処理を妨げず、その行を破棄する。
- ディレクトリ作成、ローテーション、書き込みの失敗はロガー内で処理する。
  ログ失敗を TIP / Host の終了、例外、入力経路の失敗へ波及させない。
- Release ビルドの構造化フィールドはメタ情報に限定し、入力本文、候補本文、
  prompt、`raw_keys`、window title の生値を API へ渡さない。ロガーも §7.6 の
  本文系フィールド名を検出した場合は値を `***redacted***` に置換し、
  `window_title` フィールドは出力しない。
- 文字列フィールドは `RuntimeLogSafeText` で明示的に opt-in した値だけを受け付ける。
  本文を示しうるキーを部分一致で検出した場合は redact する。

M41 v1 では `trace_id` の生成と伝播、phase 別計測、ETW provider、WER / local
dump を実装しない。

### 7.2 構造化ログ（JSON Lines）

TIP / Host とも JSON Lines 形式のログを出力する。出力先は `ipc-persistence-hardening-spec.md` §5.2 の
`%LOCALAPPDATA%\azooKey\logs\host-YYYYMMDD.jsonl` /
`tip-YYYYMMDD.jsonl`。
Windows の `%LOCALAPPDATA%` 解決は TIP / Host とも
`SHGetKnownFolderPath(FOLDERID_LocalAppData)` を共通利用する。

各行に最低限含める必須フィールド:

| フィールド | 内容 |
|---|---|
| `ts` | ISO 8601 タイムスタンプ |
| `level` | `info` / `warn` / `error` |
| `component` | 行を書いたプロセス。`tip` / `host` |
| `event` | 機械可読なイベント名 |
| `request_id` | IPC リクエスト相関 ID（該当時。§7.3） |
| `trace_id` | ユーザー 1 アクション相関 ID（該当時。§7.3 / §7.7.1） |
| `phase` | 処理フェーズ。正典 enum は `core/include/azookey/logging/Phase.h`（§7.3 / §7.7.2） |
| `latency_ms` | 当該フェーズの所要時間 ms（計測時）。本フィールド名を全コンポーネント共通の正典とする |
| `result` | 共通の値は `ok` / `error` / `cancelled` / `blocked`（secure 抑止時。`docs/privacy-and-secure-input-spec.md` §5.1）。結果の種類が多いイベントは、そのイベントを定義する spec が固定の値を列挙したうえで、同じフィールド名に載せてよい（例: `langbar_menu_setting_write` の `written` / `lock_unavailable` / `read_failed` / `invalid_file` / `write_failed`。`docs/tsf-deep-integration-spec.md` §4.5）。値は固定の識別子に限り、入力本文や自由文を載せない |
| `error_code` | §7.4 のエラーコード（error 時） |

`component` はログファイル（`tip-*.jsonl` / `host-*.jsonl`）と 1:1 に対応する。
`core/` と `ipc/` はライブラリとして TIP / Host に組み込まれ、自分のファイルを持たないため、
その処理の行は呼び出したプロセスの `component` で書く。どのコードが計測したかは
`event` と `phase` で表し、`component` を増やして表さない。

候補窓の描画失敗は `component=tip`、`level=warn`、
`event=candidate_window_render_failed` として記録する。
フィールドは `stage`（描画処理が定義する固定の失敗段階）と `hr`（符号付き HRESULT）に限定し、
入力本文、候補本文、注釈、案内文は渡さない。記録は `AZOOKEY_LOG` の opt-in に従う。
同じ stage は `CandidateUiCoordinator::Create` から次の `Create` まで一度だけ記録し、
その最初の HRESULT を保持する。表示の終了や描画の成功では重複抑止を解除しない。
候補窓の再試行と復旧通知は `docs/native-ui-spec.md` §4.1 に従う。

`phase` の取り得る値は §7.7.2 の 11 値に固定し、`core/include/azookey/logging/Phase.h`
の `azookey::logging::Phase` を全コンポーネント（tip / host）の正典とする。
所要時間フィールドは `latency_ms` を正典名とし、`duration_ms` 等の別名は使わない。
phase 別の絶対オフセット（key_down=0 起点）が必要な場合のみ任意で `t_ms` を併記する。

記録対象イベント: Host 起動・終了、pipe listen 開始、model load 成否、
backend 選択、query latency、error、exception summary、learning/user-dict
の load/save 結果。
トレンド語取得の `trending_word_fetch` と固定 `reason` は
`auto-word-registration-spec.md` §5-1 で定義する。
更新の確認の `update_check` と固定 `reason` は `sideload-packaging-spec.md` §6.1 で定義する。

実装は自前の軽量 JSON Lines ロガーを第一候補とする（依存最小主義）。
`spdlog` 採用はビルド依存の増加に見合うかを評価したうえで判断し、本仕様の
必須範囲には含めない（`build-ci-spec.md` §11）。

### 7.3 相関 ID とフェーズ

相関 ID は **2 軸のみ** とし、3 つ目の概念（`correlation_id` 等）は導入しない。

| ID | 型 | 粒度 | 発番元 |
|---|---|---|---|
| `request_id` | uint64（≥1, allocator ごとに単調増加） | IPC リクエスト 1 往復 | **TIP（client）採番**。`QueryCandidates`/staleness は `ipc_pending_id_`、送信キュー（`CommitObservation` / `Cancel`）は接続ローカル連番（§7.3 注記）。Host は `req.request_id` を echo（`inference-host/src/Dispatcher.cpp` の `MakeResponse`） |
| `trace_id` | string（UUIDv7。**全 envelope 必須**） | 1 論理操作（キー押下 → UI 更新、または 1 lifecycle / settings IPC） | 操作の発行側が採番（キー駆動は TIP の `OnKeyDown`、§7.7.1） |

- 横断追跡のキーは **`(trace_id, request_id)` の組** とする。`trace_id` が
  1 アクション内の複数 IPC を束ね、`request_id` がその中の個々の往復を識別する。
- 両 ID は既存 IPC エンベロープ `{version, request_id, type, trace_id, payload}`
  にそのまま乗る（`diagnostics-compat-spec.md` §12.6 / `docs/privacy-and-secure-input-spec.md` §9）。新フィールドは追加しない。
- `trace_id` は **全 IPC envelope に必須**（`ipc::Deserialize` は `trace_id` 欠落を
  reject、`ipc/src/Messages.cpp`）。発番単位は「1 論理操作」:
  - キー駆動操作 — TIP が `OnKeyDown` で 1 `trace_id` を発番し、その操作の複数 IPC
    （`QueryCandidates` 等）を束ねる。
  - 非キー（lifecycle / settings）IPC — `Handshake` / `Ping` / `LoadModel` /
    `QueryDiagnostics`（`diagnostics-compat-spec.md` §12.6）等は、その発行側
    （TIP / 設定アプリ）が操作開始時に UUIDv7 を採番する。単発操作は 1 envelope = 1 `trace_id`。
- 受信側は既存 client との互換性のため、文字列だが UUIDv7 形式ではない `trace_id` も
  envelope として受け付け、応答に echo する。ただしその値を phase ログに記録せず、
  当該要求の phase 計測を行わない。発行側の UUIDv7 必須条件は緩めない。
- `request_id` は **すべて TIP（client）側で採番**し、Host は応答で echo するのみ。
  TIP には 2 系統の allocator があり、実装はこの分担を維持する（新たに統合しない）:
  - `ipc_pending_id_`（TIP メンバ）— `QueryCandidates` の応答相関と staleness 判定
    （M10、`req_id == ipc_pending_id_`）・`Cancel.target_request_id` に用いる「最新リクエスト」id。
  - 接続ローカル連番（`next_id`、Handshake=1 の後 2 から開始）— 送信キュー
    （`ipc_send_queue_`）に積む `CommitObservation` / `Cancel`、idle 時の `Health`（`host-availability-spec.md` §8.3）、
    設定変更時に確立済み接続で再実行する Handshake（`host-availability-spec.md` §8.2）の wire id。送信キューは
    応答有無が混在する: **`CommitObservation` は応答あり**（TIP が `expects_response=true`
    で送り、`CommitObservationResponse` を待つ。`tsf-tip/src/TextService.cpp` /
    `inference-host/src/Dispatcher.cpp`）、**`Cancel` のみ応答なし**。
  どちらも `tsf-tip/src/TextService.cpp`。2 系統は独立に採番するため、同じ接続上で
  同じ数値の `request_id` が現れうる。ログ相関では `(trace_id, request_id)` に加え
  MessageType で系統を区別する。`CommitObservation` の応答は**必ず受信・相関する**
  （読み飛ばすと次の受信が stale 応答を拾い pipe ストリームが desync するため、
  「応答なし」扱いにできるのは `Cancel` のみ）。
- `Cancel` は primary 接続とは別の短命な control 接続から送ってよい。
  この場合も control 接続は Handshake を完了してから `Cancel` を送る。
  `Cancel` envelope の `request_id` は control 接続ローカルの wire id であり、キャンセル
  対象は payload の `target_request_id` で指定する。
  Host 側では全接続の `Dispatcher` が共有する `RequestScheduler` に
  `target_request_id` が登録されるため、primary 接続で in-flight の `QueryCandidates`
  にもキャンセルが届く。
- Host の `RequestScheduler`（`inference-host/src/RequestScheduler.cpp`）は wire
  `request_id` を **発番しない**。TIP 由来の id をキーに cancellation / latest 追跡を
  行う側であり、`NextRequestId()` は wire ID の採番元ではない。

フェーズは §7.7.2 の **11 値に固定** し、正典 enum は
`core/include/azookey/logging/Phase.h`（`azookey::logging::Phase`）とする。
phase の記録は M51 で行う（§7.1.1）。phase 体系は 1 つだけで、M41 用と M51 用の別系統は持たない。
各フェーズの所要時間は §7.2 の `latency_ms` フィールドに記録して遅延要因を切り分ける。

ETW（`docs/sideload-packaging-spec.md` §7）は固定長の型付きイベントであり、
`IpcPhase`（3003）/ `InferencePhase`（4002）の `phase` に 4 値の `EtwPhase` を使う。
これは上記 11 値の別体系ではなく、ETW の固定レイアウトに載せるための粗い射影とする。
対応は次のとおりで、JSON Lines 側の集計はこの表で ETW と突き合わせる。

| `EtwPhase` | `Phase`（§7.7.2） |
|---|---|
| `Converter` | `model_inference` |
| `FrameWrite` | `pipe_send`（client 側の要求送信） |
| `FrameRead` | `pipe_recv`（client 側の応答受信） |
| `AiTransform` | 対応なし（外部 / ローカル AI backend の変換。11 値の外） |

`AiTransform` に対応する phase が必要になった場合は、§7.7.2 の規則に従い `Phase` へ
patch として追加し、本表を更新する。`EtwPhase` 側に JSON Lines に無い値を足すときも
本表へ行を加える。

ETW は文字列を持たない（`docs/sideload-packaging-spec.md` §7.2.1）ため `trace_id` を載せず、
相関キーを `(client_guid, request_id)` とする。`client_guid` は TIP インスタンスの ID で、
primary 接続と control 接続で共有する。TIP インスタンスごとに独立して採番される `request_id` の
TIP 間の衝突を分ける。1 つの TIP の中では 2 系統の allocator と control 接続の wire id が
同じ数値を取りうる。`IpcRequest`（3000）は `message_type` を持つが、`IpcResponse` / `IpcPhase` /
`InferenceStart` / `InferenceEnd` は MessageType も接続の識別子も持たないため、
`(client_guid, request_id)` は ETW の中で要求を一意に決めるキーではない。ETW は区間の所要時間と
OS イベント（CSwitch / DiskIO）との重ね合わせに使い、要求単位の厳密な結合は JSON Lines の
`(trace_id, request_id)` と MessageType で行う。ETW の行を要求へ結び付ける必要がある調査では、
JSON Lines の同じ `request_id` の行と時刻で照合し、一意に決まらない組は結合しない。
JSON Lines と ETW を突き合わせるときも、TIP 間の衝突は JSON Lines では `trace_id`、ETW では
`client_guid` で分ける。

### 7.4 エラーコード体系

エラーを 3 カテゴリの enum で固定する:

- `transport` — pipe 切断、接続失敗、フレーミング異常
- `protocol` — JSON パース失敗、未知 MessageType、payload 不整合
- `business` — モデル未ロード、変換失敗、辞書 I/O 失敗

JSON Lines の `error_code` と ETW `9000 Error` の `error_code`（`EtwErrorCode`）は同じ 3 値を使う。
JSON Lines ではカテゴリ名の文字列（`transport` / `protocol` / `business`）を値とする。
IPC 応答 payload の `error` 文字列（`unsupported_message_type`、`safe_mode` など）は
カテゴリより細かい理由であり、`error_code` の値に使わない。ログに理由を残す場合は
`error_code` にカテゴリを、理由は別フィールドに置く。

### 7.5 タイムアウト規約

変換候補問い合わせを含む request レイヤの timeout は、処理種別ごとに 1 つの deadline とする。
値は `host-availability-spec.md` §8.5.2 の timeout 表が正典である。deadline を超えた要求は打ち切り、遅れて届いた応答は
staleness check で破棄し、超過をログに記録する。変換要求（`QueryCandidates` など）は Host に
Cancel を送り、ローカル候補（`host-availability-spec.md` §8.3）で入力を続ける。`Health` のように Host 側で重い処理を
伴わない要求は Cancel を送らない。ソフト / ハードの 2 段は持たず、打ち切り前の警告段階は置かない。
打ち切りが連続した場合の劣化モードへの移行は `host-availability-spec.md` §8.3 の劣化判定に従う。

本節が定めるのは request レイヤ（要求送信から応答受信まで。推論時間を含む）の
規約であり、transport レイヤのフレームデッドライン（`ipc-persistence-hardening-spec` §6.4.7）とは別物である。
両者は独立に働き、値を一致させない。フレームデッドラインはソフト / ハードの 2 段を持ち、
ソフト超過はログ記録に留める（`ipc-persistence-hardening-spec` §6.4.7。ログの配線は M41）。

### 7.6 プライバシー配慮（redaction ポリシー正典）

IME である以上、入力本文・候補語をそのままログに出すとプライバシー
リスクがある。本文系フィールド（`reading` / `surface` / `candidate.text` /
確定文字列 / Magic Conversion prompt / typo の `raw_keys` 等）の出力可否は、
以下の **優先順位** で 1 つに定める。上位が下位を常に上書きする。

| 優先 | 条件 | 本文系フィールドの扱い |
|---|---|---|
| 1（最優先） | secure 中（`policy.secure == true`） | **常に redact**。Debug でも `AZOOKEY_LOG_BODY=1` でも出力しない |
| 2 | プライバシー設定が詳細ログ不許可（`policy.detailed_logging_allowed == false`。`privacy.redactLogs=true`〔既定〕、または mode が `private`／`secure`） | **常に redact**。build / env を無視 |
| 3 | Release ビルド（既定） | **常に redact**。`request_id` / `trace_id` / 長さ / `result` / `latency_ms` 等のメタ情報のみ |
| 4 | Debug ビルド かつ `AZOOKEY_LOG_BODY=1` | 本文を出力（opt-in。開発時のみ） |
| 5 | Debug ビルド（既定、env 未設定） | redact（メタ情報のみ） |

等価な単一条件として、本文出力は
**`Debug ∧ AZOOKEY_LOG_BODY=1 ∧ ¬policy.secure ∧ policy.detailed_logging_allowed`** が成り立つ
ときのみ。いずれか 1 つでも偽なら redact する。

- `core::ParsePrivacyPolicy(settings)` が `privacy.mode` と `privacy.redactLogs` を
  解決する。ログへ渡す TIP の設定は `TipAiSettings.privacy_policy` に保持する。
  Host の `RuntimeSettings.privacy_policy` は学習ゲートの secure 判定に用いる。
  詳細ログを許可するのは
  `redactLogs = false` を満たし、mode が `normal` / `offline`、または
  `custom` かつ `custom.detailedLogging = true` の場合に限る。
  `private` / `secure`、設定欠落・型不正・未知 mode は詳細ログ不許可とする。
- `RuntimeLogger::Log` / `FormatRecord` は任意のイベント単位 policy を受け取り、
  省略時は `PrivacyPolicy{secure = true, detailed_logging_allowed = false}` 相当で
  本文を拒否する。`AZOOKEY_LOG_BODY` は文字列が厳密に `1` の場合のみ有効。
- TIP の同期 `conversion_selected` イベントは、`CommitSelected` 内で
  `RequestCommitEditSession` より前に解決した policy と `reading` / `surface` を渡す。
  非 secure、入力先アプリの解決成功、入力 scope の検査成功、グローバルの詳細ログ許可をすべて満たし、
  プロファイルが `private` でない場合だけ本文を許可する。アプリ未解決または scope 不明なら、
  `privacy.autoSecureInput` の値によらず詳細ログを拒否する。
  非同期の確定処理へ許可を持ち越さない。他のメタデータの出力条件は変えない。
- redact 時は値を `***redacted***` に置換する。`window_title`、資格情報、
  パスは本文許可時も redact する。診断 ZIP は詳細ログを収集時に再 redact する。
- 本ポリシーの実装は M44 診断 ZIP（`diagnostics-compat-spec.md` §12.5）と secure redaction（`docs/privacy-and-secure-input-spec.md` §5 / §8）で
  **共通の redaction 関数** を用い、二重定義・不整合を作らない。共通関数は、フィールド名から
  本文系・機密を判定する `logging::IsSensitiveRuntimeLogField`
  （`core/include/azookey/logging/RuntimeLogger.h`）と、自由文字列のユーザーパスを環境変数表記へ
  正規化し資格情報の形をした部分を伏せる `core::RedactFreeText`
  （`core/include/azookey/core/Redaction.h`）とする。診断 ZIP の settings は、本文系 field の判定に
  `logging::IsSensitiveRuntimeLogField` を使い、それに加えて `diagnostics-compat-spec.md` §12.5 の `settings.redacted.json` 行が
  定める settings 固有の field（API key、prompt、path）を処理する。本文系の判定を別に書き起こさない。
- レイテンシ trace（§7.7）は本文を含まないメタ情報であり、本ポリシーの
  redact 対象外（phase 別 `latency_ms` は Release でも記録してよい）。
- 互換性テスト（`diagnostics-compat-spec.md` §13）でも、Release 既定で本文がログ・成果物に残らないことを
  確認対象に含める。

### M41 受け入れ条件

受け入れ条件の定義の正典は [`plans/windows-port-roadmap.md`](../plans/windows-port-roadmap.md)
の M41 節とする。本書は §7.1〜§7.6 でログ形式・エラーコード体系・タイムアウト規約・redaction を定義し、
受け入れ条件を複製しない。

### 7.7 M51 拡張: レイテンシ内訳トレーサ

M41 の構造化ログ基盤を発展させ、キー押下から候補表示までの全 phase を
1 リクエスト単位で追跡可能にする。Zenzai 最適化（M24 / M25 / M57）・
Tiny Reranker（M56）・ModernBERT スコアリング（M57）の効果測定の前提と
なる。

#### 7.7.1 trace_id

`trace_id` は §7.3 の wire format `{version, request_id, type, trace_id, payload}` に
**既に存在するフィールド**であり（`ipc::Deserialize` が欠落を reject）、M51 は wire format を
変更せず、この既存フィールドへ **UUIDv7 値を生成・伝播**するのみ。キー駆動操作では TIP が
`OnKeyDown` で生成し、対応する全 IPC 往復に付与する。非キー（lifecycle / settings）
IPC は §7.3 のとおり発行側が操作単位で採番する（全 envelope 必須は §7.3 参照）。
既存 `request_id` は IPC リクエスト単位のままとし、`trace_id` は
「ユーザーの 1 アクション（キー押下 → UI 更新）」単位で複数 IPC を
束ねる。

```json
{
  "type": "QueryCandidates",
  "request_id": 123,
  "trace_id": "018fd2c2-2a3e-7c9a-b8e1-7f3a92d4c5e2"
}
```

#### 7.7.2 計測フェーズ

| フェーズ | 説明 | 計測するコード（行の `component` は §7.2 のとおりプロセス名） |
|---|---|---|
| `key_down` | TIP がキーを受け取った時刻 | tsf-tip |
| `romaji_convert` | ローマ字かな変換 | core / tsf-tip |
| `ipc_serialize` | payload 生成（JSON 化） | ipc / tsf-tip |
| `pipe_send` | Named Pipe 送信 | ipc |
| `host_queue_wait` | Host scheduler の要求登録・排他待ち | inference-host |
| `model_inference` | SimpleConverter / Zenzai 推論 | inference-host |
| `rerank` | 学習 / user dict / tag boost / Tiny / BERT | inference-host |
| `pipe_recv` | 応答受信 | ipc / tsf-tip |
| `staleness_check` | request_id 確認 | tsf-tip |
| `ui_apply` | CandidateWindow / Preedit 更新 | tsf-tip |
| `total` | key_down → ui_apply の通算 | tsf-tip |

この 11 値が phase の正典であり、`core/include/azookey/logging/Phase.h` の
`azookey::logging::Phase` と 1:1 で対応する（wire 名は `PhaseName()`）。
値の **追加は後方互換（patch）**、**削除 / 改名は破壊的変更（major）** とする
（読み手は未知 phase を無視。集計ツールは wire 名に依存するため改名不可）。

各 phase は §7.2 の構造化ログ行として記録する（既存 schema 互換）。`key_down` は
`t_ms:0` の起点行とし、所要時間サンプルに含めない。ほかの phase の所要時間は
`latency_ms`（正典名）に入れ、絶対オフセットが必要なときのみ任意で `t_ms` を併記する。
`model_inference` 行には `backend` を付与し、ニューラルモデル経路では
`engine` も付与する。SimpleConverter fallback は `backend="cpu"` とし、
ニューラルエンジンを実行していないため `engine` を省く
（M24 `docs/copilot-pc-backend-spec.md` §4 整合）:

- `engine`: `"llama_cpp" | "winml"`
- `backend`: R1 アクセラレータ `cpu` / `cuda` / `vulkan`（旧 `directml` / `npu` は
  非推奨。R2 では `winml`）
- R2(`winml`) 時のみ `ep`（選択 EP 名。例 `QNNExecutionProvider`）と `ep_state`
  （`NotPresent` / `NotReady` / `Ready` / `Registered` / `Failed`、`docs/copilot-pc-backend-spec.md` §4.6）。EP 取得・登録
  失敗をトレースで切り分け可能にする。

モデル推論が例外で失敗して SimpleConverter に fallback した場合、JSONL の
`model_inference` は fallback の所要時間を 1 件だけ記録する。失敗したモデル試行は
同じ論理操作の集計サンプルに重ねず、ETW の失敗結果で診断する。

```json
{"ts":"2026-05-27T10:00:00.000Z","trace_id":"018fd2c2-2a3e-7c9a-b8e1-7f3a92d4c5e2","component":"tip","phase":"key_down","t_ms":0.0,"level":"info","result":"ok"}
{"ts":"2026-05-27T10:00:00.001Z","trace_id":"018fd2c2-2a3e-7c9a-b8e1-7f3a92d4c5e2","component":"tip","phase":"romaji_convert","latency_ms":0.04,"level":"info","result":"ok"}
{"ts":"2026-05-27T10:00:00.015Z","trace_id":"018fd2c2-2a3e-7c9a-b8e1-7f3a92d4c5e2","component":"host","phase":"model_inference","latency_ms":14.5,"backend":"cuda","level":"info","result":"ok"}
{"ts":"2026-05-27T10:00:00.018Z","trace_id":"018fd2c2-2a3e-7c9a-b8e1-7f3a92d4c5e2","component":"tip","phase":"total","latency_ms":18.3,"level":"info","result":"ok"}
```

#### 7.7.3 trace_viewer CLI

`bench/azookey_trace_viewer.cpp` を新設し、JSONL ログを集計する:

```powershell
azookey_bench.exe --trace --backend cuda --model zenzai-small.gguf
azookey_trace_viewer.exe trace.jsonl --summary
```

summary 出力例:

```
QueryCandidates latency summary (N=10000, skipped=0)
  model_inference (N=10000): p50=11.2 ms p95=30.4 ms p99=55.0 ms
  pipe_send (N=10000): p50=0.1 ms p95=0.5 ms p99=1.0 ms
  total (N=10000): p50=18.3 ms p95=42.1 ms p99=78.4 ms
```

`--json` は人間向け表示とは別に `schema_version: 1`、phase ごとの
`samples` / `p50_ms` / `p95_ms` / `p99_ms`、`skipped_lines` を出力する。
不正 JSON、未知・欠損 phase、無効な `latency_ms` は警告して集計から除く。

#### 7.7.4 オーバーヘッド制御

通常利用時の phase 計測・出力は無効とする。TIP / Host の phase 行は既存の
構造化ログを `AZOOKEY_LOG=1` で明示有効化したときだけ記録し、既存の
5 MiB・3 世代・7 日の保持規則を適用する。ログには `trace_id`、`phase`、
`latency_ms` 等のメタ情報だけを含め、入力本文は含めない。

ベンチの詳細 JSONL は `azookey_bench --trace` のときだけ生成する。
`--trace-output PATH` の既定値は `trace.jsonl` で、実行ごとに上書きし、
結果 JSON や評価入力とは別のパスとする。ベンチ出力の保持は呼出側が管理する。
サンプリングや設定 GUI からの有効化はこの契約に含めない。

### M51 受け入れ条件

受け入れ条件の定義の正典は [`plans/windows-port-roadmap.md`](../plans/windows-port-roadmap.md)
の M51 節とする。本書は §7.7 で `trace_id`・計測フェーズ・`azookey_trace_viewer`・オーバーヘッド制御を定義し、
受け入れ条件を複製しない。
