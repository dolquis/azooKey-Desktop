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

- ✅ `Handshake(version, capabilities)` / `HandshakeResponse(accepted, model_loaded)`
- ✅ `Ping` / `Health`
- ✅ `LoadModel(path, backend, n_gpu_layers)` — 現状 Host は OK を返すスタブ。M8 で本実装。
- ✅ `QueryCandidates(request_id, kana, context)`
- ✅ `Cancel(target_request_id)`
- ✅ `CommitObservation(reading, chosen, shown, timestamp_ms)`
- ✅ `AddUserWord` / `RemoveUserWord`
- ⚠️ enum のみ定義済み、Payload/Dispatcher 未実装:
  - `QueryPredictions` `QueryCorrections` `CommitCorrection` `UpdateUserWord`
  - `InferenceEngine` 側には既に `QueryPredictions/QueryCorrections/CommitCorrection`
    関数があるため、Payload と Dispatcher ハンドラを追加すれば配線可能。

## 学習

- モデル重みは更新しない（安全性優先）。
- `learning.db` 相当の永続層へ観測を保存し、再ランキングで反映。
- 永続フォーマットは現状 TSV。未指定時は
  `%LOCALAPPDATA%\azooKey\data\learning.tsv` に保存する。
- ユーザー辞書は未指定時 `%LOCALAPPDATA%\azooKey\data\user_dict.json` に保存する。
  `--learning` / `--user-dict` 指定時は明示パスを優先する。
- 保存時は一時ファイルへ書き込んでから replace し、書き込み中クラッシュによる
  既存ファイル破損を避ける。
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

既存 9 種（Handshake / Ping / Health / LoadModel / QueryCandidates / Cancel /
CommitObservation / AddUserWord / RemoveUserWord）に加え、以下を Phase 5〜6 で
順次追加する。

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
