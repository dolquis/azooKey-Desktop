# Windows TSF TIP + Inference Host 分離設計

## 構成

- `tsf-tip/`: in-proc COM DLL。キー処理、Composition、候補UI制御、EditSession要求のみ担当。
- `inference-host/`: per-user常駐EXE。モデル常駐、候補生成、学習再ランキング、CPU/CUDA切替。
- `ipc/`: Named Pipe向けのバージョン付きメッセージ定義（現実装はJSON + length-prefix）。
- `learning/`: 頻度 + 時間減衰の再ランキング永続化。

## なぜ分離するか

- TIPはアプリ内で動くため、GPU初期化や巨大モデルロードを持つとアプリ巻き込みクラッシュの危険が高い。
- Host分離で、推論クラッシュはHost側に閉じ込め、TIPは再接続できる。

## TSF EditSessionルール

- テキスト更新は必ず `RequestEditSession` を経由。
- 非同期推論結果到着後、UIスレッドでEditSessionを再要求し、最新 `request_id` のみ反映。
- 古い `request_id` は破棄（ライブ変換での逆転防止）。
  - 実装: `TextService::IpcWorkerThread` で `ipc_pending_id_` と受信 `req_id` を
    比較する staleness check。
- 確定時は `shown_candidates_` をスナップショットし、in-flight `QueryCandidates`
  に `Cancel` を送ってから EditSession を要求する。
- EditSession が拒否されたとき (`hr_session != S_OK`) は preedit と
  `committing_` フラグをロールバックし、確定済み観測も送らない。

## IPCメッセージ（実装済み = ✅）

各メッセージの payload フィールドの正典スキーマは `ipc/include/azookey/ipc/Payloads.h`
（および `ipc/src/Payloads.cpp` の serialize/deserialize）を参照。以下は現状の配線済み
フィールドの要約。

- ✅ `Handshake` — 要求 `(tip_version, protocol_version, capabilities, handshake_token?)` /
  応答 `HandshakeResponse(host_version, protocol_version, accepted, model_loaded)`。
  `handshake_token` 経由の per-connection 認証ゲートを持つ（後述）。
- ✅ `Ping` / `Health`
- ✅ `LoadModel(path, backend, n_gpu_layers?)` / `LoadModelResponse(ok, error?)`。
  Host は `ProbeZenzaiGgufModel` で GGUF を実プローブし、成功時は `ZenzaiModelConverter`
  を構築する（`InferenceEngine::LoadModelWithResult`）。CUDA backend は未リンクのため
  CPU に fallback し、`error` にその旨を入れて `ok=true` を返す。`path` 空文字時は MVP
  fallback converter を active にする。probe 失敗時は、まだモデル未ロードの初回ロードなら
  MVP fallback converter へ切り替える一方、既にモデルロード済みの再ロードでは直前にロード
  済みのモデルを active のまま維持する（`LoadModelFailureKeepsPreviouslyLoadedModel`）。
- ✅ `QueryCandidates` — 要求 `(reading, left_context, max_candidates, live)` /
  応答 `(candidates[], partial)`。各 candidate は `(surface, reading, score, source)`。
  応答前に `max_candidates` で件数を切り詰める。
- ✅ `Cancel(target_request_id)`
- ✅ `CommitObservation(reading, chosen, shown, left_context, timestamp_ms)`
- ✅ `AddUserWord` / `RemoveUserWord`
- ⚠️ enum のみ定義済み、Payload/Dispatcher 未実装:
  - `QueryPredictions` `QueryCorrections` `CommitCorrection` `UpdateUserWord`
  - `InferenceEngine` 側には既に `QueryPredictions/QueryCorrections/CommitCorrection`
    関数があるため、Payload と Dispatcher ハンドラを追加すれば配線可能。

### Handshake 認証ゲート

- Host 設定 `handshake_token` が非空のとき、`Dispatcher::RequiresAuthenticatedSession`
  が有効化され、Handshake 成立（`protocol_version` 一致 + `handshake_token` 一致）まで
  後続メッセージを拒否する。token 未設定（空）の場合は認証不要で全メッセージを受け付ける。
- Handshake の成否は接続ごとの `authenticated_` 状態に反映され、未認証時は Health が
  `last_error="not authenticated"` を返す。

## 学習

- モデル重みは更新しない（安全性優先）。
- `learning.db` 相当の永続層へ観測を保存し、再ランキングで反映。
- 永続フォーマットは現状 TSV。未指定時は
  `%LOCALAPPDATA%\azooKey\data\learning.tsv` に保存する。
- TSV の `reading` / `surface` は保存時に
  `# azookey-learning-tsv escaped=1` ヘッダーを付け、`\` → `\\`、
  tab → `\t`、LF → `\n`、CR → `\r` としてエスケープする。ヘッダーのない
  旧ファイルは backslash を literal として扱い、既存データを壊さない。
- ユーザー辞書は未指定時 `%LOCALAPPDATA%\azooKey\data\user_dict.json` に保存する。
  `--learning` / `--user-dict` 指定時は明示パスを優先する。
- 保存時は一時ファイルへ書き込んでから replace し、書き込み中クラッシュによる
  既存ファイル破損を避ける（`learning/src/AtomicFile.h`）。一時ファイルは
  `FlushFileBuffers` / `fsync` で flush 後、`MoveFileExW(MOVEFILE_REPLACE_EXISTING |
  MOVEFILE_WRITE_THROUGH)` で原子的に置換し、置換途中のクラッシュでも既存ファイルを
  無傷に保つ。
- 確定・訂正ごとの観測はメモリ上で即時反映し、TSV への永続化は
  `learning_flush_every_n`（既定 8 件）または `learning_flush_interval_sec`
  （既定 5 秒）の background timer でデバウンスする。Host 破棄時と `LoadModel` 境界では
  `FlushLearningStore()` で明示 flush する。
- `Save()` 失敗時は Host stderr に error を出し、dirty 状態を維持して
  次回 observation または明示 flush で再試行する。
- `learning_max_records`（既定 10000）と `learning_min_weight`（既定 0.05）で
  低スコア・減衰済みレコードを GC し、TSV の無制限増大を防ぐ。
- 破損時はリセット可能（`LearningStore::Reset` or ファイル削除）。
- 時間減衰: `exp(-0.15 * days)` で `LearningStore::Score` 内で適用。

## 実装ルール

### スレッドモデル

- COM apartment: TSF text service DLL は `InProcServer32` でロードされるため、
  ホストアプリ文脈に従う。
- TSF インターフェースは UI スレッド境界で扱う。推論・重い変換をワーカースレッドへ
  逃がす際は、TSF オブジェクトへ直接触れず、結果はメッセージ／キューで UI スレッドへ
  戻して反映する（実装: `IpcWorkerThread`）。
- `AddRef`/`Release`/`QueryInterface` を厳格実装する。COM ポインタは
  `wil::com_ptr` または `Microsoft::WRL::ComPtr` を用いる。

### 例外・障害耐性

- COM 境界をまたぐ関数は**例外を外へ出さない**。失敗時は `HRESULT` で返却し、
  ログに詳細を書き込む。
- 最低ログ要件: 起動／終了、例外、キーイベント要約、変換失敗理由。
  出力先は `%LOCALAPPDATA%\azooKey\logs\`（現状は TIP=`OutputDebugStringA` /
  Host=stderr、Phase 4 で JSON Lines ファイルログへ移行予定）。

### 互換性優先の実装ルール

- Notepad / Chrome / VSCode / Office の挙動差を前提に、未処理キーは極力食わない。
- Ctrl / Alt / Win 併用のキーは、明示的に実装済みの TIP 機能でない限りアプリまたは
  OS ショートカットへ pass-through する。Shift 単独は通常のローマ字入力として扱う。
- composition 状態の不整合時は安全側（キャンセル）で復帰する。

## 新規モジュール（Phase 5/6 で追加予定）

下記モジュールは v1.0 以降の Phase で追加予定。正典仕様は各 spec を参照。

### Phase 5（レガシー parity、`docs/legacy-parity-spec.md`）

- `core/include/azookey/core/UserAction.h` — VK → 抽象アクション enum
- `core/include/azookey/core/InputState.h` — 入力状態機械
- `core/src/UserActionMap.cpp` — VK → UserAction 変換テーブル
- `core/src/CustomRomajiLoader.cpp` — TSV パース + ホットリロード
- `core/src/UnicodeInputBuffer.cpp` — Ctrl+Shift+U hex バッファ
- `tsf-tip/src/PredictionWindow.cpp` — 予測候補 HWND
- `tsf-tip/src/PromptDialog.cpp` — Magic Conversion プロンプト
- `tsf-tip/src/DebugWindow.cpp` — F10 デバッグウィンドウ
- `tsf-tip/src/CaretRectResolver.cpp` — `GetTextExt` / `GetGUIThreadInfo` 3 段
  フォールバック
- `inference-host/src/AiBackend.cpp` — OpenAI 互換 API クライアント

### Phase 5〜6 横断（`docs/rich-features-spec.md`）

- `tsf-tip/src/ContextTracker.cpp` — 段落 / 直前文 / アプリ別履歴
- `tsf-tip/src/ForegroundAppDetector.cpp` — 前面アプリ実行ファイル名取得
- `tsf-tip/src/TypingTempoTracker.h` — タイピング速度推定
- `core/src/QwertyAdjacency.cpp` — FuzzyMatch 用 QWERTY 隣接表
- `bench/rich_features_bench.cpp` — リッチ化機能のレイテンシ計測

### Phase 6-A（TSF 深部、`docs/tsf-deep-integration-spec.md`）

- `tsf-tip/src/ReconversionFunction.cpp` — `ITfFnReconversion`
- `tsf-tip/src/CandidateListUIElement.cpp` — UI-less Mode 用
- `tsf-tip/src/PredictionListUIElement.cpp` — Suggestion UI 用
- `tsf-tip/src/ConfigureFunction.cpp` — `ITfFnConfigure`
- `tsf-tip/src/InstalledPath.cpp` — 設定アプリ EXE 解決

### Phase 6-B（Copilot+ PC / NPU、`docs/copilot-pc-backend-spec.md`）

- `inference-host/src/BackendSelector.cpp` — DXCore 列挙 + 優先順
- `inference-host/src/MmapModelLoader.cpp` — `CreateFileMapping` + `MapViewOfFile`
- `inference-host/src/PowerStatusWatcher.cpp` — AC/バッテリ検知
- `inference-host/src/DirectMlBackend.cpp` — DirectML EP 経路
- `inference-host/src/QnnBackend.cpp` — Snapdragon X NPU 経路（M27 と合流）

### Phase 6-C（UI モダン化、`docs/native-ui-spec.md`）

- `tsf-tip/src/ThemeColors.h` — Light/Dark 色テーブル
- `tsf-tip/src/RenderingEngine.cpp` — DComp + D2D + DirectWrite 共通

### Phase 7（サイドロード配信、`docs/sideload-packaging-spec.md`）

- `settings-app/` — C++/WinRT WinUI 3 設定アプリ
- `core/src/EtwLogger.cpp` — ETW Provider ラッパ
- `inference-host/src/UpdateChecker.cpp` — GitHub Releases ベース更新確認
- `inference-host/src/CrashHandler.cpp` — `SetUnhandledExceptionFilter`
- `learning/src/DpapiCrypto.cpp` — DPAPI ラッパ
- `pkg/msix/AppxManifest.xml` — MSIX マニフェスト
- `pkg/wix/Product.wxs` — WiX インストーラ

### 長期

- `learning/EmbeddingIndex.h` — 文脈ベクトルベース再ランキング（仮）

## 新規 IPC メッセージ

既存の配線済み 9 種（Handshake / Ping / Health / LoadModel / QueryCandidates /
Cancel / CommitObservation / AddUserWord / RemoveUserWord）に加え、以下を
Phase 5〜6 で順次追加する。

> 注: `MessageType` enum は 13 の named 型 + `Unknown` sentinel = 14 entries。
> このうち Payload/Dispatcher まで配線済みは上記 9 種。新メッセージ型を enum に
> 追加する際は **`Unknown` sentinel の前に挿入**すること（末尾の `Unknown` の
> 後ろに追加しない）。

| メッセージ | 方向 | 導入 Phase | 参照 |
|---|---|---|---|
| `QueryLiveConversion` / `Response` | TIP → Host | Phase 5 (M14) | legacy-parity §2 |
| `QueryPredictions` / `Response` | TIP → Host | Phase 5 (M15) | legacy-parity §3 + rich X-2 |
| `TransformSelectedText` / `Response` | TIP → Host | Phase 5 (M16) | legacy-parity §4 |
| `RequestPostCommitLint` / `Response` | TIP → Host | Phase 5 末 (M16 拡張) | rich X-3-3 |
| `LintFinding` | データ型 | Phase 5 末 | rich X-3-3 |
| `PredictStreamChunk`（push） | Host → TIP | Phase 6 (M24) | rich X-2-5 |
| `ReverseConvert` / `Response` | TIP → Host | Phase 6-A (M20) | tsf-deep §1 |
| `UpdateSettings` / `Response` | Settings → Host | Phase 7 (M30) | sideload §3 |
| `QueryFullRecompute` / `Response` | TIP → Host | Phase 5 末 | rich X-1-3 |
| `UpdateUserWord` / `Response` | Settings → Host | Phase 7 (M30) | 既存 enum 配線 |
| `QueryCorrections` / `CommitCorrection` Payload | TIP → Host | Phase 5〜6 | 既存 enum 配線 |

Envelope に `push: bool` フラグを追加し、`server → client` 一方向通知に
対応する（`docs/rich-features-spec.md` X-4-2）。

## TIP プロセス HWND 構成

TIP DLL がアプリプロセスにロードされた状態で、最大以下の HWND を保持する：

| HWND | 用途 | 親 | 生存期間 |
|---|---|---|---|
| `CandidateWindow` | 変換候補（M5 で実装済） | デスクトップ | composition 中のみ表示 |
| `PredictionWindow` | 予測候補（M15 で追加） | デスクトップ | composition 中常時表示（設定 ON 時） |
| `PromptDialog` | Magic Conversion / Replace Suggestion（M16） | フォアグラウンド | モーダル表示中のみ |
| `DebugWindow` | F10 デバッグ（M18-3） | デスクトップ | トグル表示 |

設定アプリ（M30）は **別プロセス**（`azookey_settings.exe`）で、IPC 経由で
Host 設定を変更する。TIP プロセスからは `ITfFnConfigure::Show` で起動するのみ。
