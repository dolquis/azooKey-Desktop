# azooKey-Desktop Windows ポート: 開発計画・マイルストーンロードマップ

本書は Windows 版 azooKey-Desktop の**唯一の開発計画ドキュメント**。
v1.0 までの実行計画（Phase 1〜4）と、v1.0 以降のマイルストーン定義
（Phase 5〜7・追加機能）・受け入れ条件・依存関係・テスト体系を一本化して
管理する。前提となる方針・構成は以下を参照:

- `docs/windows-port-asset-audit.md`: 既存 macOS 資産の流用可否棚卸し
- `docs/windows-tsf-host-architecture.md`: TSF TIP + Inference Host 分離設計

本書はそれらを前提に、「いつ何をどの順で」「何をもって完了とするか」を定める。

> **状態・進捗の正典は Linear。** 各マイルストーンの進捗・状態・優先度・担当は Linear
> （team `Dev` / project「azooKey Desktop / Windows IME MVP」）を正典とする。本書は構造・
> マイルストーン定義・依存関係・受け入れ条件の「定義」・テスト体系・リスクの正典であり、
> 完了状態（✅/🚧/⚠️）や「現状」「残作業」は持たない。運用は `AGENTS.md`「Linear 運用（管制塔）」を参照。

## 全体目標

- **MVP**: Windows 10/11 上で TSF 経由のローマ字入力 → かな漢字変換 → 確定までの
  最小フローが動作する IME。
- **配布形態**: ユーザーごとインストールの MSIX または MSI。
- **コア方針**: TIP (in-proc COM DLL) はキー処理と UI のみ担当し、推論・学習は
  Named Pipe 経由で `inference-host` (per-user 常駐 EXE) に委譲する。

## 現在のソース構成（2026-05 時点）

| ディレクトリ        | 役割                                                     | 概要（構成・主な実装単位）                   |
|---------------------|----------------------------------------------------------|---------------------------------------------|
| `core/`             | OS 非依存の変換コア（C++）                               | `RomajiKanaConverter` / `SimpleConverter` / `IConverter`（tests あり） |
| `ipc/`              | Named Pipe 上の JSON + length-prefix プロトコル          | 全 15 `MessageType` 定義・主要 Payload・Named Pipe（tests あり） |
| `learning/`         | 頻度 + 時間減衰の再ランキング永続化                      | `LearningStore` / `Reranker` / `UserDictionary`（tests あり） |
| `inference-host/`   | 常駐 EXE。モデル推論・候補生成・学習集約                 | `InferenceEngine` / `Dispatcher` / `RequestScheduler` / `main.cpp`。モデルロード境界は M8 で扱う |
| `tsf-tip/`          | TIP 本体 (COM DLL)                                       | COM 登録・Composition・候補 UI・確定・Cancel（M1〜M10 の範囲） |
| `bench/`            | パフォーマンス計測                                       | `azookey_bench`                             |
| `legacy/Core/` (Swift) | macOS 版の仕様参照源                                  | 移植対象ではなく仕様参照のみ（`legacy/` に保全・未保守） |

## マイルストーン

依存関係: 矢印は「→ が前提」。並行可能なものは並列に記載。

```
M0 ─→ M1 ─→ M2 ─→ M3 ─→ M4 ─→ M5 ─→ M6 ─→ M11 ─→ M12
              └→ M7 (並行)
              └→ M8 (M4 完了後に並行)
              └→ M9 (M6 完了後に並行)
              └→ M10 (M5 完了後に並行)
```

### M0: 廃止資産の削除

- **目的**: 旧 `ime-tsf/` ディレクトリを削除し、現行の `tsf-tip/` のみが
  ビルド対象になっている状態を明示化。
- **変更**: `ime-tsf/` 8 ファイルを削除（コミット `3938f49`）。
- **受け入れ条件**:
  - `ime-tsf` への参照がリポジトリ全体に残らない
  - ルート `CMakeLists.txt` のサブディレクトリが現行構成と一致

### M1: IPC ハンドシェイク疎通

- **目的**: TIP と Host 間で Named Pipe を確立し `Handshake` + `Ping` が
  往復するところまで到達。
- **変更対象**: `ipc/`, `inference-host/main.cpp`, `tsf-tip/src/TextService.cpp`
- **実装範囲**:
  - Named Pipe サーバ (Host) / クライアント (TIP) 実装（DACL・長さプリフィックスフレーミング）
  - `Handshake(version, capabilities)` と `Ping`/`Health` のメッセージ実装
  - バージョン不一致時の切断ポリシー
- **受け入れ条件**:
  - `ipc/tests` に Handshake/Ping のラウンドトリップ単体テストが通る
  - TIP 側 NamedPipeClient（TIP-client 経路）のテストが CTest に統合される

### M2: TIP 登録と最小キーボード活性化

- **目的**: Windows 側に TIP として登録され、IME バーから選択でき、
  キーイベントが TIP に届く。
- **変更対象**: `tsf-tip/src/DllMain.cpp`、`scripts/register-dev.ps1`、`scripts/unregister-dev.ps1`
- **実装範囲**:
  - `regsvr32` / インストーラ向けの自己登録ロジック（machine-wide HKLM CLSID + `RegisterProfile` による TSF プロファイル + キーボード / UIElement カテゴリ + Lang `0x0411`、管理者権限が必要）
  - 言語バー有効化
  - `ITfKeyEventSink` 接続（`ActivateEx` で `ITfKeystrokeMgr::AdviseKeyEventSink` /
    `ITfSource::AdviseSink`、`Deactivate` で対応する Unadvise）
- **受け入れ条件**:
  - 開発機にビルド成果物をインストールして言語切替で azooKey が選べる
  - キー押下が `ITfKeyEventSink::OnKeyDown` まで到達することをログで確認できる
  - `regsvr32` 単体（管理者）で CLSID + TSF プロファイル + キーボードカテゴリが登録されること

### M3: Composition / Preedit 表示

- **目的**: ローマ字キー入力が preedit としてアプリ側 (例: メモ帳) に
  表示され、Backspace で削除でき、ESC で破棄できる。
- **変更対象**: `tsf-tip/src/TextService.cpp` 内の EditSession 周り。
- **実装範囲**:
  - `ITfCompositionSink` / `ITfComposition` の保持
  - `RequestEditSession` 経由でのテキスト挿入・置換（`GUID_PROP_ATTRIBUTE` でアンダーライン、
    caret 位置を `ITfContextView::GetTextExt` でキャッシュ）
  - ローマ字 → かな変換テーブル（小書きっ・ん・長音対応）
  - `EnumDisplayAttributeInfo` / `GetDisplayAttributeInfo`（`GUID_TFCAT_DISPLAYATTRIBUTEPROVIDER` 登録）
- **受け入れ条件**:
  - 「ka」「ki」等の入力で「か」「き」がアンダーライン付きで表示される
  - ESC で composition がクリアされる
  - Backspace で 1 文字戻る

### M4: モック候補生成（Host 経由）

- **目的**: Host 側に固定テーブルベースの簡易変換を実装し、
  `QueryCandidates` の往復が成立する。
- **変更対象**: `inference-host/src/InferenceEngine.cpp`,
  `inference-host/src/RequestScheduler.cpp`, `tsf-tip/src/TextService.cpp`, `ipc/`
- **実装範囲**:
  - `QueryCandidates(request_id, kana, context)` の Host 実装（`atomic<bool>* cancel` 対応）
  - 固定テーブル or 簡易 N-best（`core/src/SimpleConverter.cpp`: 固定辞書 + TSV ロード + prefix fallback + `Learn()`）
  - `request_id` 追跡と古い ID の破棄（`RequestScheduler`）
- **設計メモ**: 「bigram context bonus」は `SimpleConverter.cpp` のデータ駆動の静的
  bigram ボーナス表（組み込みシード + TSV ロード）で扱う。これは CPU fallback の
  de-stub であり、本格的な統計的/辞書ベース bigram スコアリングは M52/M53 で対応する。
- **受け入れ条件**:
  - `inference-host/tests` で固定 kana → 期待候補リストが返る
  - TIP 側で `OnKeyDown` から `QueryCandidatesRequest` を `NamedPipeClient` 経由で送信し、候補リストが Host から受信される

### M5: 候補 UI 表示

- **目的**: Space キーで候補ウィンドウが表示され、↑/↓ で選択、Enter で確定。
- **変更対象**: `tsf-tip/` 内の Candidate UI (新規)
- **実装範囲**:
  - `CandidateWindow` クラス（自前 WS_POPUP HWND）を新規追加
  - キャレット位置追従（`ITfContextView::GetTextExt` でキャッシュ）
  - Space で候補表示/次候補へ巡回、↑/↓ で上下移動、1〜9 で直接選択
  - マウス左クリックで即時確定
  - Host 候補リストをリアルタイムで差し替え
  - UI-less mode 最小契約（`CandidateUiCoordinator`・UI-less フラグ検出
    （`ITfThreadMgrEx::GetActiveFlags`）・`GUID_TFCAT_TIPCAP_UIELEMENTENABLED`
    登録・`BeginUIElement` 動線）。詳細は
    `docs/tsf-deep-integration-spec.md` §2.0・§2.8〜§2.11
- **受け入れ条件**:
  - 「nihongo」入力 → Space で「日本語」等の候補が出る
  - 矢印キーで選択移動、Enter で確定、ESC でキャンセル
  - UI-less / アプリ描画ホスト（Office 等、`pbShow==FALSE` を返すアプリ）で TIP が
    activate され、自前 HWND が出ず OS/アプリ UI に候補が乗る（実機確認は
    `docs/legacy-parity-spec.md` §12 の `gate:human-required` チェックリスト）
  - Win11 スタート検索では TIP が activate され入力できること（統合インライン検索の
    **統合表示は M21 スコープ**。検索統合 API `ITfIntegratableCandidateListUIElement`
    + `ITfFnSearchCandidateProvider` を要するため M5 は統合表示なしの劣化モードまで。
    `docs/tsf-deep-integration-spec.md` §2.7）

### M6: Commit と Observation

- **目的**: 確定動作で composition が commit され、学習用 observation が
  Host に通知される。
- **変更対象**: `tsf-tip/`, `inference-host/`, `learning/`
- **実装範囲**:
  - `CommitObservation(reading, chosen, shown, timestamp_ms)` 送信（候補確定時: Enter/数字/クリック）
  - Host 側で `learning/LearningStore` に書き込み（重み累積 + 時間減衰 + TSV 永続化）
- **受け入れ条件**:
  - 確定時にアプリへ最終テキストが入る
  - `learning.tsv` に observation 行が増える

### M7: 学習による再ランキング

- **目的**: M6 で記録した observation を `Reranker` が読み、次回以降の
  候補順位に反映する。
- **前提**: M4
- **変更対象**: `learning/src/Reranker.cpp`, `inference-host/`
- **実装範囲**:
  - `Reranker::Apply(reading, candidates, now_epoch_sec)`（`LearningStore::Score` の
    時間減衰 `exp(-0.15 * days)` を加算して `stable_sort`）
  - `InferenceEngine::QueryCandidates` のパイプラインへ組み込み
- **受け入れ条件**:
  - 単体テスト: 同一 context で複数回確定した候補が上位に来る
  - 手動: 同じ語を 3 回確定後、4 回目に第一候補で出る

### M8: Zenzai モデルのロード

- **目的**: `inference-host` が gguf モデルを optional にロードでき、
  CPU/CUDA 切替が configure 可能。
- **前提**: M4
- **変更対象**: `inference-host/`
- **実装範囲**:
  - `LoadModel(path, options)` の実装（空 path は `SimpleConverter` fallback、
    GGUF path は `ZenzaiModelConverter` で magic/version 検証 → `model_loaded`）
  - llama.cpp C-API を `ZenzaiModelConverter` に接続し GGUF から実候補を生成
  - `AZOOKEY_BACKEND=cpu|cuda` CMake オプション（M37 の `CMakePresets.json` の
    `cacheVariables` 化を含む）。CUDA 初期化失敗時は CPU fallback、`Health(status=degraded)` で観測
  - モデル未配置時はモックにフォールバック
  - Zenzai ロード時間・推論 p50/p95 を `bench/` で計測
- **設計メモ**: バックエンド別実装（CPU/CUDA 等）は `core/IConverter` 実装
  として `SimpleConverter` と差し替える方式で吸収する。第三者レビューが
  提案した別系統の `IModelRuntime` 抽象は `IConverter` と二重抽象になるため
  新設しない（`docs/dev-infrastructure-spec.md` §11.3）。
- **受け入れ条件**:
  - Zenzai GGUF（上流 `Miwa-Keita/zenz-v3.2-small-gguf`）配置時に `LoadModel` 成功
  - 未配置時も Host が落ちず、固定テーブル候補が動く
  - GPU/CPU 切替が設定で効く

### M9: ユーザー辞書

- **目的**: ユーザー登録語の追加・削除がランタイムで反映される。
- **前提**: M6
- **変更対象**: `learning/`, `inference-host/`
- **実装範囲**:
  - `AddUserWord` / `RemoveUserWord` メッセージ実装（`Dispatcher` から
    `InferenceEngine` 経由で同期・永続化まで）
  - 永続化フォーマット定義（`UserDictionary`: JSON `{version, entries: [{word, ruby, cid, mid, value}]}`）
  - `InferenceEngine::QueryCandidates` が `user_dict_->Lookup` を最優先で返す統合
  - 設定 UI（M11）または暫定 CLI/デバッグ UI から呼べる経路
- **受け入れ条件**:
  - 設定 UI（M11 で繋ぐ）から語を追加し、即座に候補に出る

### M10: Cancel とライブ変換同期

- **目的**: 入力中の高速タイピングで、古い推論結果が UI に上書きしない。
- **前提**: M5
- **変更対象**: `tsf-tip/`, `inference-host/`
- **実装範囲**:
  - `Cancel(request_id)` 送信と Host 側の早期中断（`InferenceEngine` が `atomic<bool>* cancel` をポーリング）
  - TIP 側で最新 `request_id` のみ EditSession を要求するガード（staleness check で候補逆転を防止）
  - 確定時（CommitSelected / CommitPreeditAsIs）に `Cancel` を送り、未処理 QueryCandidates を Host に通知
- **受け入れ条件**:
  - 単体テスト: 連続 5 リクエストのうち最新のみが UI 反映される（staleness check）
  - 手動: 早打ちしても候補が逆転しない

### M11: 設定 UI とパッケージング

- **目的**: ユーザーが Zenzai ON/OFF、辞書管理、デバイス選択を行える
  最小設定アプリと、配布可能なインストーラ。
- **変更対象**: 新規 `settings-app/`（WinUI 3 / C++/WinRT に確定、§3.0）、`pkg/` 配下 (新規)
- **実装範囲**:
  - 設定アプリ (TIP/Host とは別プロセス)。v1.0 設定 UI の最小機能セット（露出キー
    `model.enabled`/`model.backendPreference`〔`auto`/`cpu`/`cuda` 縮小〕/`model.selectedPath`/`logLevel`
    と「ユーザー辞書を編集」「ログ出力先を開く」ボタン）および v1.0 / v1.x 境界は
    `docs/sideload-packaging-spec.md` §3.7 を正典とする
  - 設定の永続化・反映に必要な**最小 IPC / 設定ストアを本マイルストーンで導入**:
    `ipc/src/Payloads.cpp` の `UpdateConfig`（`docs/sideload-packaging-spec.md` §3.3）と
    `inference-host` 側 SettingsStore 最小実装（settings.json 読込 / default 補完 / 反映、
    DEV-203）。Zenzai ON/OFF・ユーザー辞書の変更が Host に反映されるところまでを M11 の
    スコープとする（M30 はこの最小層の上に UI を本格化する）。
  - MSIX または WiX ベースの MSI
  - ユーザースコープ自動登録、アンインストール時の自動解除
- **受け入れ条件**:
  - クリーンな Win11 VM でインストール → IME 選択 → 入力 → 確定が動く
  - アンインストールでレジストリ・ファイルが残らない
- **設計メモ**: 設定 UI が読み書きするユーザーデータの保存先は M39 で確定
  する `%LOCALAPPDATA%\azooKey\` レイアウトに合わせる。先行して M37〜M39
  を完了しておくとパッケージング側の手戻りが減る。

### M12: 配布リリースと CI

- **目的**: GitHub Release から MVP リリースを提供。MVP は **未署名 MSI**（配布方針は spec §0 / DEV-415）。
- **変更対象**: `.github/workflows/`, `pkg/`
- **実装範囲**:
  - Windows ランナーでのビルド + 単体テスト（windows-2022 + msvc-dev-cmd + Ninja、
    Debug / Release matrix を preset 経由で configure → build → CTest）
  - MSI 生成（M11 で `pkg/` 構成決定後）。コード署名は MVP では行わない（M29 で延期、DEV-255）。
  - リリースタグ push → アーティファクト自動公開
- **設計メモ**: M38（CI 品質ゲート拡張）で整備済みの Debug/Release matrix・
  preset 利用・Linux 補助ジョブ・bench smoke を前提に、M12 では MSI 生成・Release
  公開ステップを足す（署名は MVP 対象外、spec §0）。
- **受け入れ条件**:
  - main への merge で CI が緑（テストが個別 exe 実行で全件 pass）
  - タグ push で未署名 MSI が Draft Release に上がる

## 横断的な作業

- **テスト**: 各マイルストーンで `*/tests/` 配下に最低 1 件の単体テストを
  追加する。Windows 依存のないものは Linux/macOS CI でも回す。
  詳細なテストカバレッジとギャップは `## テスト体系` 章を参照。
- **ログ**: TIP/Host とも構造化ログ（JSON Lines）を `%LOCALAPPDATA%\azooKey\logs\`
  に出す。当面は TIP=`OutputDebugStringA`（DebugView）/ Host=stderr。
  JSON Lines ファイルログへの切替は M41（構造化ログと可観測性）で行う。
  ログスキーマ・相関 ID・エラーコード体系は `docs/dev-infrastructure-spec.md`
  §7 を正典とする。
- **ドキュメント**: 各マイルストーン完了時に `docs/windows-tsf-host-architecture.md`
  を実装に合わせて更新する。

## テスト体系（2026-05 現在）

### テストフレームワーク

テストフレームワークは **GoogleTest**、実行ランナーは **CTest** を併用する。
GoogleTest はまず `find_package` でシステムインストール版を探し、見つからず
かつ `-DAZOOKEY_FETCH_GOOGLETEST=ON` が指定されたときのみ `FetchContent` で
ダウンロードする（ネットワーク取得は明示オプトイン）。いずれでも入手できない
場合は警告を出してテストのみスキップし、ビルド自体は継続する（オフライン環境で
`cmake -S . -B build` が失敗しないようにするため）。
各テストは共通ヘルパ `azookey_discover_tests`（内部で `gtest_discover_tests` を呼び出し、
`DISCOVERY_TIMEOUT` は既定 60 秒）により **ケース単位**（`SuiteName.TestName`）で CTest に
登録されるため、下表の各実行ファイルは内部の `TEST()`/`TEST_F()` ごとに個別の CTest
エントリへ展開される。現行 preset では `ctest --preset windows-debug` または
`ctest --preset windows-release` で一括実行する。

### 現存テスト一覧

| ターゲット | テスト | 主要シナリオ |
|---|---|---|
| `core_tests`（`core/tests/`） | `romaji_kana_converter_test.cpp` | `Feed`/`Flush`/`Preview`/`ConvertForCommit`（小書きっ・ん・長音） |
| `core_tests` | `simple_converter_test.cpp` | 固定辞書、TSV ロード、prefix fallback、静的 bigram コンテキスト表（suffix/最長一致）、`Correct`、`Learn` |
| `ipc_tests` | `messages_test.cpp` | Envelope シリアライズ、length-prefix フレーミング、`MessageType` mapping |
| `ipc_json_tests` | `json_test.cpp` | JSON パーサの int64/uint64 精度、深度・入力長上限、Unicode escape、不正入力、round-trip |
| `ipc_payloads_tests` | `payloads_test.cpp` | Handshake/Ping/Health/LoadModel/QueryCandidates/QueryBatchConversion/Cancel/Commit/UserWord の build/parse + malformed reject |
| `ipc_named_pipe_transport_tests` | `named_pipe_transport_test.cpp` | サーバ起動 → クライアント接続 → Handshake/Ping ラウンドトリップ、overlapped 即時完了エラー保持 |
| `ipc_tip_client_tests` | `tip_client_ipc_test.cpp` | TIP-client 経路（StartDebugIpcProbe 相当）の Handshake → Ping → QueryCandidates |
| `learning_tests` | `learning_test.cpp` | `LearningStore::Observe/ObserveCorrection/Score`、`Reranker::Apply` 間接テスト |
| `reranker_tests` | `reranker_test.cpp` | null-store、空 candidates、stable sort、時間減衰、学習ブースト、correction downweight |
| `user_dictionary_tests` | `user_dictionary_test.cpp` | Add/Lookup/Remove、Save/Load round trip、missing file、malformed JSON |
| `host_engine_tests` | `engine_test.cpp` | 学習ブースト、user-dict 注入、cancel 早期 return、legacy overload |
| `host_dispatcher_tests` | `dispatcher_test.cpp` | Handshake/Ping/QueryCandidates/QueryBatchConversion/Cancel/Commit/AddUserWord/RemoveUserWord/Health の主要ハンドラ |
| `host_scheduler_tests` | `scheduler_test.cpp` | `NextRequestId` 連番、`Cancel`/`IsCanceled`、`MarkLatest`/`IsLatest`、thread-safety smoke |
| `host_user_data_paths_tests` | `user_data_paths_test.cpp` | `UserDataPaths` のパス解決（root/config/data/logs/models、`learning.tsv`/`user_dict.json`） |
| `tsf_tip_com_smoke_tests` | `com_smoke_test.cpp` | DLL `DllGetClassObject` → `IClassFactory::CreateInstance(IID_IUnknown)` |
| `tsf_tip_onkeydown_preedit_tests` | `onkeydown_preedit_test.cpp` | `OnKeyDown`/`OnTestKeyDown` で romaji→kana preedit 蓄積、Backspace（pending romaji / UTF-8 単位 kana 削除）、Escape クリア、Space で pending flush、preedit 無し時の制御キー非消費 |
| `tsf_tip_display_attribute_tests` | `display_attribute_test.cpp` | `ITfDisplayAttributeProvider`（`GetDisplayAttributeInfo`/`EnumDisplayAttributeInfo`）と `InputDisplayAttributeInfo`（GUID/説明/下線属性、`Next`/`Reset`/`Skip`/`Clone`、null 引数 reject） |
| `tsf_tip_activate_uiless_tests` | `activate_uiless_test.cpp` | `ActivateEx` が `ITfThreadMgrEx::GetActiveFlags`（`dwFlags` ではなく）から UI-less 状態を導出する（spec §2.10） |
| `azookey_bench_smoke` | `azookey_bench` | CPU `SimpleConverter` 経路の p50/p95/p99 出力、p95 < 50ms |

### 既知のテストギャップ（Phase 3/4 着手前に解消したい）

> 解消済みのギャップは本リストに残さず `現存テスト一覧` 表に反映する（達成状態の正典は Linear）。
> 以下は未解消の目標カバレッジの定義。

中期（Phase 3 / Zenzai 統合と並行）:
1. **`InferenceEngine` バックエンドフォールバック** — `--backend cuda` 指定だが CUDA 初期化失敗時に `cpu` にフォールバックすることをテスト。
2. **`InferenceEngine::LoadModel` モック** — gguf 未配置時に false を返し、配置時に true を返すモックバックエンド。
3. **`NamedPipeServer` 同時接続耐性** — 単一クライアント前提だが、Host を別 process で起動 → クライアント再接続シナリオ（TIP再ロード時の挙動）。
4. **`tsf-tip` レジストリ smoke** — `DllRegisterServer` 後に HKLM の COM in-proc 登録と TSF プロファイル（`ITfInputProcessorProfileMgr::GetProfile` が `GUID_TFCAT_TIP_KEYBOARD` を返す）が存在し、`DllUnregisterServer` 後に消えることを検証する round-trip テスト。`com_smoke_test.cpp` に実装済み。対話的 TSF セッションを要するため opt-in 環境変数 `AZOOKEY_RUN_REGISTRATION_SMOKE` + 昇格時のみ実行で、**CI では走らない**（headless ランナーは TSF セッションが無く `GetProfile` が登録直後のプロファイルを観測できない）。

長期（Phase 4 / 配布前に必須）:
5. **MSIX manifest と `DllRegisterServer` の整合** — MSIX `comServer` 宣言が `kTextServiceClsid` と一致し、アンインストール時に CLSID / TSF プロファイルキーが残らない smoke（`Add-AppxPackage` → 登録確認 → `Remove-AppxPackage` → 残骸 0）。配布経路（spec §1.0 Option A/B/C）により登録先が変わるため、経路別の合否定義は経路確定を前提とする。
6. **`UpdateUserWord` payload** — enum のみで Payload 未実装。設定 UI で必要になった時点で `BuildUpdateUserWordRequest`/`Parse...` を実装し、`payloads_test.cpp` と `dispatcher_test.cpp` に追加。
7. **`QueryPredictions`/`QueryCorrections`/`CommitCorrection` payload** — `InferenceEngine` には既に対応関数があるので、IPC 経由で叩けるよう Payload と Dispatcher ハンドラを追加。

開発基盤・品質強化トラック（M37〜M43 と並行、`docs/dev-infrastructure-spec.md` 参照）:
8. **`NamedPipeServer` 再接続耐性（劣化モード復帰）** — Host を別 process で停止 → 再起動し、TIP-client が exponential backoff で再接続して劣化モードから復帰するシナリオを M42 の状態機械テストで扱う（複数接続・切断時の client cleanup 単体テストは M40 で対応）。
9. **アプリ互換マトリクス試験** — Notepad / Office / ブラウザ / VS Code / ターミナルで composition・確定・フォーカス遷移・サロゲートペア・絵文字・結合文字・Undo/Redo の端ケースを確認（手動チェックリスト主体、Phase 6 の M20〜M23 と関連）。
10. **bench IPC 内訳メトリクス** — `bench/` に serialize / send / host_compute / recv / apply_ui のフェーズ別レイテンシ計測を追加し、遅延要因の切り分けを可能にする（M41 の相関 ID・フェーズ設計と整合）。

## リスクと不確実性

未決の設計判断:

- llama.cpp バインディング選択（M8）はビルド時間と配布サイズに影響大。
  M4 → M8 の間で技術調査が必要。
- ~~候補 UI（M5）を `ITfCandidateListUIElement` で実装するか自前 HWND にするか~~
  → **確定（DEV-97 / D-01）**: 二者択一ではなく「両立 (coexistence)」。
  `ITfCandidateListUIElement`（3-interface 契約）と自前 `WS_POPUP` HWND を両方
  実装し、`ITfUIElementMgr::BeginUIElement` の `pbShow` で per-call に切り替える。
  v1.0（M5）で最小契約（UI-less フラグ検出（`GetActiveFlags`）・最小 UIElement 候補公開と
  それに伴う `GUID_TFCAT_TIPCAP_UIELEMENTENABLED` カテゴリ登録・`CandidateUiCoordinator`）
  まで実装し、M21 で full 実装。カテゴリ登録は候補公開実装と必ず一体で行う（公開が
  無いまま登録すると UI-less-only ホストで候補が消える）。詳細は
  `docs/tsf-deep-integration-spec.md` §2.8〜§2.11。
- ~~設定アプリ（M11）の UI フレームワーク（WinUI 3 / WPF / Tauri）は別途検討。~~
  → **確定（DEV-99 / D-03）**: **WinUI 3（C++/WinRT）**。根拠は既存 C++/WinRT スタック
  との親和性・Fluent/Mica 標準対応・MSIX 整合の 3 点。WPF（.NET 9+）/ Tauri は代替案へ
  縮退。実機での配布サイズ・初回起動・IPC 連携行数の確証スパイクは `gate:human-required`
  で残す。詳細は `docs/sideload-packaging-spec.md` §3.0。

v1.0 リリースに向けたリスクと対応:

| リスク | 影響 | 対応 |
|---|---|---|
| llama.cpp バインディング選定 (M8) | 配布サイズ・初回起動時間に直結 | Phase 3 着手スパイクで確定 (`docs/zenzai-gpu-route.md` 更新)、`bench/` で計測 |
| CUDA SDK の配布制約 | MSIX のサイズ膨張・GPU なし PC でのフォールバック品質 | バックエンドは optional payload、CPU を default に、ggml-cuda は別 MSIX オプションパッケージで検討 |
| MSIX 配布 (M11) の machine-wide 登録 | アンインストール後にレジストリが残る | `DllRegisterServer` は machine-wide (HKLM) 登録に統一済み。MSIX manifest で `comServer` を宣言し、アンインストール時に確実に消えることを VM テストで確認 |
| 設定 UI フレームワーク選定 (M11) | 配布サイズ・依存ランタイム | **WinUI 3（C++/WinRT）に確定（DEV-99 / `docs/sideload-packaging-spec.md` §3.0）**。残る確証は実機での配布サイズ・初回起動・IPC 行数の 1〜2 日スパイク（`gate:human-required`） |
| 署名証明書の調達 (M29) | MVP リリースには影響しない（配布方針転換、spec §0） | MVP は未署名 MSI（DEV-415）、MS Store は MS 再署名（DEV-416）で有料証明書不要。証明書はスタンドアロン MSIX サイドロード着手時のみ（経路 B 確定済み・延期、DEV-255） |
| Host 停止・無応答時の入力停止 (M42) | 入力中に候補更新が止まり UX が劣化 | 接続状態機械 + exponential backoff 再接続、無応答時は `SimpleConverter` 劣化モードへ（`docs/dev-infrastructure-spec.md` §8） |
| IPC 観測性不足による遅延切り分け困難 (M41) | TIP/Pipe/Host のどこが遅いか特定できず最適化が滞る | 構造化ログ（相関 ID・フェーズ別 `latency_ms`）とエラーコード体系を導入（同 §7） |
| 自前 JSON パーサの IPC 境界堅牢性 (M40) | malformed 入力でのクラッシュ・未定義動作 | ネスト深度/最大長制限・fuzz テスト・Named Pipe 強化。即時の `nlohmann-json` 全置換はせず段階対応（同 §6, §11.2） |
| 学習永続化の書き込み増幅・無制限増大 | 確定時レイテンシ増加、SSD 書き込み増、学習 TSV 肥大化 | `LearningStore` は N 件/T 秒デバウンス、明示 flush、保存失敗ログ、上限件数 + weight 閾値 GC で抑制 |
| 品質 KPI 未設定 | 改善効果を定量評価できない | レイテンシ・復旧時間・CI 成功率・永続化破損率の目標値を設定（同 §10） |

## スコープ外

本ロードマップ（Windows 版）の対象外:

- macOS 版 v1.0（Issue #181 で別管理、`legacy/` 配下に保全）
- `legacy/segment-edit-upstream.md` の文節エディット機能（macOS 向けの上流
  計画、Windows MVP 後）
- `legacy/Core/Sources/Core/InputUtils/InputState.swift` の FIXME（macOS 側）
- Linux 版（コミュニティフォーク `fcitx5-hazkey` で対応）

## v1.0 までの実行計画（Phase 1〜4）

ロードマップの依存図とは別に、v1.0（MSIX 配布可能な最小 IME）リリースまでの
実行順を 4 フェーズで管理する。各マイルストーンの進捗・状態の正典は **Linear**
（project「azooKey Desktop / Windows IME MVP」）であり、本章は定義・受け入れ条件・依存のみを持つ。
macOS 版（Issue #181）は本計画の対象外（「スコープ外」参照）。

### Phase 1: TIP 基盤完成（M1〜M4）

実機 IME としてローマ字を打鍵し、Host から候補を取得して候補ウィンドウに
表示するまでの動線。**実機 IME 動作は M2 のキーイベント sink 配線（Issue #33）に依存する**。

### Phase 2: 候補選択と確定動線（M5/M6/M10）

候補選択・確定・観測送信・早打ち耐性（in-flight cancel + staleness）。実機での
動作確認は M2（Issue #33）に依存する。

### Phase 3: 実 Zenzai と辞書 UI のつなぎ込み（M8/M9、3〜5 週）

1. **M8 Zenzai 統合** — `inference-host/src/InferenceEngine.cpp::LoadModel`
   の本実装。初期境界として GGUF magic/version 検証と `ZenzaiModelConverter`
   差し替えを行う。続いて llama.cpp の C-API バインディングを接続し、CMake オプションで
   `AZOOKEY_BACKEND=cpu|cuda` を切替。配布サイズと初回起動時間を `bench/` で
   計測。モデル未配置時は `SimpleConverter` フォールバックを維持。
   `docs/zenzai-gpu-route.md` を実装と整合させる。
2. **M9 ユーザー辞書ランタイム反映** — `AddUserWord`/`RemoveUserWord`
   （Host 側完成済み）を TIP もしくは設定 UI から呼べる経路を作る。
   本フェーズではコマンドラインまたはデバッグ UI で十分。

**Phase 3 着手前の確定事項（2026-05-20 決定の記録）**:
- llama.cpp バインディング選定（2026-05-20 決定）: M8 の初期実装は
  llama.cpp C API + CPU backend から開始し、CUDA は optional backend として
  追加する方針。DirectML / NPU は M24 まで予約値扱い。判断理由と計測ゲートは
  `docs/zenzai-gpu-route.md` を参照。
- `LoadModel` 境界（2026-05-20 決定）: `LoadModelRequest(path, backend,
  n_gpu_layers)` を `InferenceEngine::LoadModel` に渡し、`model_loaded` /
  `last_error` を `Handshake` / `Health` で観測できる状態とする設計。
- M9 最小操作面（2026-05-20 決定）: 本格設定 UI を待たず、`inference-host`
  の IPC 経由で `AddUserWord` / `RemoveUserWord` を呼ぶ小 CLI または debug
  probe を先に用意する方針。設定アプリ統合は M11 に送る。

**Phase 3 で触るファイル**:
- `inference-host/src/InferenceEngine.cpp` — `LoadModel` の本実装、Zenzai converter 配線
- `inference-host/src/ZenzaiModelConverter.cpp` — GGUF ロード境界、llama.cpp 接続点
- `inference-host/include/azookey/host/InferenceEngine.h` — モデル状態の保持・解放 API
- `core/include/azookey/core/IConverter.h` — Zenzai 実装が嵌まることを確認
- `bench/` — Zenzai ロード時間・推論レイテンシを計測
- `CMakeLists.txt` — `AZOOKEY_BACKEND=cpu|cuda` オプション
- `docs/zenzai-gpu-route.md` — 実装結果と整合
- `inference-host/tests/` — Zenzai converter のモック実装でテスト追加

**Phase 3 で再利用すべき既存実装**:
- `core/include/azookey/core/IConverter.h` — Zenzai は `IConverter` 実装として
  `SimpleConverter` と差し替え可能
- `inference-host/src/InferenceEngine.cpp` の reranker・user_dict 経由
  パイプライン — Zenzai 出力にもそのまま適用可能
- `ipc/src/Payloads.cpp` の `LoadModelRequest/Response` — 既に `--backend
  cuda|cpu` をリクエストで指定する設計

**Phase 3 検証**:
1. ビルド: `cmake --preset windows-debug -DAZOOKEY_FETCH_GOOGLETEST=ON && cmake --build --preset windows-debug`
2. ユニットテスト: `ctest --preset windows-debug --output-on-failure` で全テスト緑
3. ユーザー辞書 round-trip:
   1. `azookey_inference_host.exe --user-dict <temp>\user_dict.json userdict add --reading azookey --surface azooKey --offline`
   2. `azookey_inference_host.exe --user-dict <temp>\user_dict.json userdict list --format tsv`
      で追加語を確認
   3. 同じ `--user-dict` で Host/TIP を起動し、`azookey` の `QueryCandidates` で
      ユーザー辞書候補が最優先に出ることを確認
   4. `azookey_inference_host.exe --user-dict <temp>\user_dict.json userdict remove --reading azookey --surface azooKey --offline`
   5. `list --format tsv` で削除済みを確認
4. Windows 実機（Win11 VM 推奨）: `scripts/register-dev.ps1` で TIP DLL 登録 →
   `azookey_inference_host.exe --pipe --backend cpu` 起動 → gguf を
   `%LOCALAPPDATA%\azooKey\models\` に配置し `LoadModel` 成功 → gguf 削除時は
   `SimpleConverter` フォールバック → メモ帳で `nihongo` 入力で Zenzai 候補
5. GPU 経路: `--backend cuda` 起動で失敗時は CPU フォールバック
6. ベンチ: `./build/windows-release/bench/azookey_bench.exe` の p50/p95 が許容内
7. `unregister-dev.ps1` でクリーン解除確認

### Phase 4: 配布可能化 — v1.0 リリースゲート（M11/M12、4〜6 週）

3. **M11 設定 UI とインストーラ** — フレームワークは **WinUI 3（C++/WinRT）に確定**
   （DEV-99 / D-03、`docs/sideload-packaging-spec.md` §3.0）。残る確証は実機での
   配布サイズ・初回起動・IPC 連携行数の 1〜2 日スパイク（`gate:human-required`）。設定アプリは
   TIP/Host と別プロセス、IPC 経由で Host 設定（Zenzai ON/OFF、ユーザー辞書）を
   変更。配布は未署名 MSI（MVP 既定。TIP 登録はインストーラのカスタムアクション。
   spec §0 / DEV-415。MSIX は MS Store 経由 = DEV-416）。
   **M30（WinUI 3 設定アプリ）と UI フレームワークを揃え、後続の作り直しを
   避ける**（M30 は M11 の設定 UI を WinUI 3 で本格化する位置づけ）。`ITfFnConfigure`
   からの起動は別プロセス EXE を非同期起動する方式（§3.5、正典は
   `docs/tsf-deep-integration-spec.md` §6）。
4. **M12 CI 完成と配布** — `.github/workflows/windows.yml` の build/test に
   加え、タグ push 時の未署名 MSI 自動 Release 公開（署名は MVP 対象外、spec §0 / DEV-415）、
   submodule 配信ポリシー確定を行う。

**Phase 4 検証**: クリーン Win11 VM での MSI インストール → IME 選択 → 入力
→ 確定 → アンインストールでクリーン状態に戻る。CI 緑、タグ push 時に未署名
MSI が自動公開。

## Phase 5〜7 の依存関係と実行順

Phase 5/6/7 は主題別のグルーピングであり、M 番号順がそのまま実行順を
意味するわけではない。各マイルストーンの「前提」欄に基づくと、以下の
独立トラックを並行で進められる。

```
【TIP parity トラック】
Phase 4 完了 ─→ M13 ─┬─→ M14 ─┐
                     ├─→ M15 ─┴─→ M19 ─→ M26
                     ├─→ M16
                     ├─→ M17
                     └─→ M18
Phase 5 完了 ─→ M20 ─→ M23
Phase 5 完了 ─→ M21 ─→ M22

【推論バックエンド トラック】（M8 のみに依存。Phase 5 と並行可）
M8 完了 ─→ M24 ─┬─→ M25
                └─→ M27

【設定・配信トラック】
Phase 5 完了 ─→ M30（Phase 6 と並行可。M36-A の承認 UI 前提）
Phase 5 完了 ─→ M34（Phase 5 直後へ前倒し推奨）
Phase 6 完了 ─→ M31（MVP MSI 直接配布）─→ M32
Phase 6 完了 ─→ M28（Store MSIX。並行準備。MVP 直接配布の前提ではない）
Phase 6 完了 ─→ M33
（M29 = スタンドアロン MSIX 署名は当面延期。M28 の後続だが M32 等の能動チェーンの前提からは外す。spec §0 / DEV-255）
M6 完了 ─→ M35（Phase 4 後に並行可能な独立トラック）
M6 完了 ─→ M36-A ─→ M36-B（M32 完了も前提）

【開発基盤・品質強化トラック】（Phase 5〜7 に依存しない独立トラック）
M37 ─┬─→ M38
     └─→ M43
M39 ─→ M41 ─→ M42
M40（独立）
M41 ─┬─→ M44             （観測性 → 診断ウィザード）
     └─→ M51             （観測性 → trace 拡張。M44 とは独立）
M42 ─→ M47               （Host 可用性 → ユーザー可視復旧 UX）
M38 ─→ M50               （CI → アプリ互換性テスト）

【プライバシー / モデル管理 / 学習データ UI トラック】
（Phase 5/6/7 の既存 M に依存する付加機能。M48 は追加機能トラック側）
M7 ─→ M46                （学習 → セーフ入力モード）
M8  / M30 ─→ M45         （Zenzai ロード境界 + 設定アプリ → モデル管理 UI）
M30 / M34 ─→ M49         （設定アプリ + DPAPI → 学習データ可視化）

【追加機能トラック（続き）】
M46 / promptPrefixByApp ─→ M48  （セーフ入力 + 既存 promptPrefix → アプリ別プロファイル）
M13 ─→ M61-A ─→ M61-B            （自動カッコペアリング。M61-B は M48 にも依存。無 IPC・Host 非依存の独立トラック）

【変換品質トラック】（Phase 5〜7 と独立。bench / 学習 / 辞書を発展）
bench / M7 / M9 ─→ M52
                  ├─→ M53     （辞書・固有名詞・新語強化。M36-A 統合が必須、neologd optional pack は別 follow-up）
                  ├─→ M54     （ユーザー学習強化、M7 発展）
                  └─→ M55     （打ち間違え学習統合）
M36-A ─→ M53                  （AutoWordStore の移行元として必須）
M36-B / M32（DL 基盤）⇢ M53    （neologd_lexicon optional pack の取り込みは別 follow-up が M36-B/M32 の HttpDownloader+SHA256 を再利用。M36-B 単体は trending→AutoWordStore で別経路。spec §14.10。未完了時は当該 pack 無効）
M35 ─→ M55                    （TypoLearningStore v1 を v2 統合エンジンへ昇格）
M46 ─→ M55                    （secure 中の補正・学習抑止契約）
M53 ─→ M55                    （Dictionary-Constrained Correction が辞書層を要する）
M52 + M53 + M54 + M55 ─→ M56  （Tiny Neural Reranker。4 つ全てを前提）
M56 + M24 ─→ M57              （ModernBERT-Ja 候補スコアリング）
```

### 推奨実行順（依存関係に基づく最適化）

M 番号は通し連番だが、依存上は以下の前倒し・並行化が可能。spec から参照される
ため M 番号・Phase グルーピングは変更しない。

- **推論バックエンド トラック（M24 / M25 / M27）を Phase 5 と並行** —
  M24 の前提は「M8 完了」のみで、Phase 5（レガシー parity）に依存しない。
  TIP 側 parity 作業と独立した「推論バックエンド トラック」として、M8 完了後
  すぐ Phase 5 と並行で着手できる。Phase 6 への配置は主題分類であり、Phase 5
  完了を待つ必要はない。
- **M30（WinUI 3 設定アプリ）を Phase 6 と並行** — M30 の前提は「Phase 5 完了
  （設定キー確定）」のみで、M28/M29 に依存しない。Phase 7 ではなく Phase 6 と
  並行で着手でき、M36-A の承認 UI 前倒しにも効く。
- **M34（DPAPI 暗号化）を Phase 5 直後へ前倒し（セキュリティ優先）** —
  M34 の前提は「Phase 5 完了」のみ。Phase 5 の M16（Magic Conversion）が
  OpenAI API キーの平文保存を持ち込むため、M34 を Phase 7 末尾に置くと平文
  保存期間が長期化する。Phase 5 直後に前倒しして暗号化ギャップを早期に塞ぐ。
- **M35 / M36-A は Phase 4 完了後に並行可能な独立トラック** — 詳細は
  「追加機能マイルストーン」章。
- **開発基盤・品質強化トラック（M37〜M43）は Phase 5〜7 に依存しない独立
  トラック** — 第三者レビューの改善指摘をマイルストーン化したもの。M37
  （ビルド再現性）・M38（CI 拡張）・M39（永続化）は Phase 3 着手前の実施を
  推奨し、M40（IPC/JSON 堅牢化）・M41（ログ）・M42（Host 再接続）・M43
  （WIL）は Phase 3/4 と並行可能。**M44（診断）・M47（復旧 UX）・M50（互換性
  テスト）・M51（trace）は本トラックの自然な延長**として M41/M42/M38 完了後に
  順次着手する。詳細は「開発基盤・品質強化トラック」章と
  `docs/dev-infrastructure-spec.md`。
- **プライバシー / モデル管理 / 学習データ UI トラック（M45/M46/M49）** —
  Phase 5/6/7 の既存 M に依存する付加機能。M46（セーフ入力）は M7 完了後に
  Phase 5 直後の前倒し対象（M16 Magic Conversion を擁護する契約なので
  M16 着手と同時期または前を推奨）。M45（モデル管理 UI）は M8/M30 完了後
  の Phase 6-C 併合トラック。M49（学習データ可視化）は M30/M34 完了後、
  Phase 7 末尾。
- **追加機能トラック（M35/M36/M48）** — M48（アプリ別プロファイル）は
  既存 `promptPrefixByApp` の発展として M46 完了後に着手し、本トラック
  に属する。詳細は「追加機能マイルストーン」章および
  `docs/app-profile-spec.md`。
- **自動カッコペアリング（M61）は M13 のみに依存する無 IPC・Host 非依存の独立
  トラック** — M61-A（コア）は M13（InputState）完了後、Phase 5 と並行で前倒し
  可能。M61-B（TSV 外部化・per-app 互換）は M61-A + M48 を前提とする。推論
  バックエンド・TSF 深耕・パッケージングに依存しない小規模機能。詳細は
  `docs/bracket-pairing-spec.md`。
- **変換品質トラック（M52〜M57）は主に Phase 5〜7 と独立した新トラック** —
  M7（学習）・M9（ユーザー辞書）・既存 `bench/` を前提に、M52（評価ベンチ）
  → M53（辞書）/ M54（学習強化）/ M55（打ち間違え統合）並行 → M56（Tiny
  Reranker） → M57（ModernBERT スコアリング）の順で進める。M55 は M35 を、
  M53 は **M36-A** を必須前提として `AutoWordStore` を多層
  DictionaryStore に統合する。**`neologd_lexicon` 任意 pack の取り込みは
  M53 v1 の必須前提ではなく別 follow-up**（M36-B/M32 の `HttpDownloader`+SHA256
  基盤を再利用するが、専用 pack 形式 + DictionaryStore ローダを要する別作業。
  M36-B 単体の trending→AutoWordStore とは別経路。spec §14.10。未完了時は当該
  pack を無効として M53 v1 を受け入れる）。
  詳細は「変換品質トラック（M52〜M57）」章と各 `docs/*-spec.md`。

## Phase 5: レガシー parity 復元（M13〜M19）

> Phase 4 完了後に着手。**正典仕様**は `docs/legacy-parity-spec.md`。
> リッチ化の横断テーマは `docs/rich-features-spec.md`。

### M13: 入力パイプライン（UserAction / InputState / ClientAction）

- **目的**: 旧 macOS 版の入力状態機械を C++ に移植し、TIP の `OnKeyDown` から
  「UserAction → InputState 遷移 → ClientAction → TSF 操作」の一貫した
  パイプラインを構築する。
- **前提**: M0〜M12（Phase 1〜4）完了。
- **変更対象**: `core/include/azookey/core/UserAction.h`（新規）、
  `core/include/azookey/core/InputState.h`（新規）、
  `core/src/InputState.cpp`（新規）、`tsf-tip/src/TextService.cpp`、
  `core/src/UserActionMap.cpp`（新規）。
- **実装範囲**: 仕様書 §1（UserAction enum、InputState 遷移表、ClientAction→TSF
  対応表、VK → UserAction マッピング）の全項目。
- **受け入れ条件**:
  - `core/tests/input_state_test.cpp` が全状態 × 全 UserAction の遷移を網羅
  - `tsf-tip/tests/keymap_test.cpp` で VK 全エントリのマッピングを検証
  - 既存の M3〜M10 挙動が回帰しない（候補往復・確定・staleness）
- **参照仕様**: `docs/legacy-parity-spec.md` §1

### M14: ライブ変換

- **目的**: `settings.liveConversion=true` のとき、Composing 中は候補ウィンドウ
  ではなく Preedit に最良候補を常時表示する。
- **前提**: M13 完了。
- **変更対象**: `ipc/src/Payloads.cpp`（`QueryLiveConversion` 追加）、
  `inference-host/src/InferenceEngine.cpp`、`tsf-tip/src/TextService.cpp`。
- **実装範囲**: 仕様書 §2（IPC、シーケンス、キャンセル経路、staleness）。
- **横断**: 仕様完了後に X-1-1（信頼度 4 段階 DisplayAttribute）を追加実装する。
- **受け入れ条件**:
  - 軽量推論で 30ms 以下の応答（`bench/rich_features_bench.cpp`）
  - Esc で composition がクリア、Backspace で 1 文字戻る
  - 高速タイピングで preedit が逆転しない
- **参照仕様**: `docs/legacy-parity-spec.md` §2、`docs/rich-features-spec.md` X-1

### M15: 予測候補ウィンドウ

- **目的**: 入力中常時、キャレット右側に予測候補（Word モード）を別 HWND で表示。
- **前提**: M13 完了。
- **変更対象**: `tsf-tip/src/PredictionWindow.cpp`（新規）、`ipc/src/Payloads.cpp`
  （`QueryPredictions` Payload 本実装）、`inference-host/src/Dispatcher.cpp`。
- **実装範囲**: 仕様書 §3（配置アルゴリズム、キャッシュ、操作、設定）。
- **横断**: 仕様完了後に X-2-2/X-2-3（paragraph_context、ラベル付き候補）。
- **受け入れ条件**:
  - キャレット右側 / 画面外なら左反転、上下反転で配置
  - Tab で第一候補受理、Esc で閉鎖
  - 1 秒キャッシュで連続呼び出し抑制
- **参照仕様**: `docs/legacy-parity-spec.md` §3、`docs/rich-features-spec.md` X-2

### M16: Magic Conversion / Replace Suggestion

- **目的**: 英数キーダブルタップで AI 自由テキスト変換、かなキーダブルタップで
  AI 言い換え。
- **前提**: M13 完了。
- **推奨**: M46（プライバシーゲート / セーフ入力モード）が同時期 or 先行で
  実装されていることが望ましい（hard prerequisite ではない）。M16 が単独
  で先行すると、M46 の secure 抑止契約が無いまま OpenAI 呼び出しが走り、
  secure アプリ向けの初期プライバシーギャップが発生する。詳細は
  `docs/privacy-and-secure-input-spec.md` §1 を参照。
- **変更対象**: `tsf-tip/src/TextService.cpp`（ダブルタップ検出）、
  `tsf-tip/src/PromptDialog.cpp`（新規）、`ipc/src/Payloads.cpp`
  （`TransformSelectedText` 追加）、`inference-host/src/AiBackend.cpp`（新規）。
- **実装範囲**: 仕様書 §4（トリガ、選択取得、プロンプト UI、IPC、OpenAI 呼び出し、
  結果反映）。
- **横断**: 仕様完了後に X-3-3（Post-Commit Lint）を本マイルストーン末尾に
  追加実装する（同じ AiBackend 経路を流用）。
- **AiBackend 契約（DEV-346 で確定。`docs/ai-backend-spec.md` 正典）**:
  - M16 / M58-C / X-3-3 は別 IPC メッセージだが Host 側 `AiBackend::Transform` に集約。
  - **HTTP 経路**: M16 は共通 WinHTTP 基盤を最小導入し、M32 はその基盤上に
    GET+SHA256 ダウンロードを後から実装（**M32 を Phase 5 へ前倒ししない**。§6 / §13）。
  - **API キー**: `dpapi:` prefix 規約で保存（§9）。M34 を Phase 5 直後へ前倒し、ただし
    M16 は M34 を hard prerequisite にしない（暫定平文 + README 注意喚起）。
  - **secure ゲート**: `AiBackend` 入口で `PrivacyGate` を強制チェック（§8、M46 連携）。
- **受け入れ条件**:
  - 英数 / かな双方のダブルタップで `TransformSelectedText` が呼ばれる
  - OpenAI 互換エンドポイントで `gpt-4o-mini` 応答が表示される
  - 結果が selection range に置換される
- **参照仕様**: `docs/ai-backend-spec.md`（AiBackend 契約・正典）、
  `docs/legacy-parity-spec.md` §4、`docs/rich-features-spec.md` X-3-3

### M17: カスタムローマ字テーブル

- **目的**: TSV のカスタムテーブルでローマ字→かな変換を差し替え可能にする。
- **前提**: M13 完了。
- **変更対象**: `core/src/RomajiKanaConverter.cpp`、
  `core/src/CustomRomajiLoader.cpp`（新規）、`tsf-tip/src/TextService.cpp`
  （`ReadDirectoryChangesW` 監視）。
- **実装範囲**: 仕様書 §5（TSV フォーマット、配置、ホットリロード、内蔵
  テーブルとの関係）。
- **横断**: 仕様完了後に X-3-1（FuzzyMatch）を `RomajiKanaConverter` に追加。
- **受け入れ条件**:
  - `%LOCALAPPDATA%\azooKey\custom-romaji.tsv` から読み込んで変換に反映
  - TSV を保存すると次の入力からテーブルが入れ替わる
  - 不正行は warning ログでスキップ
- **参照仕様**: `docs/legacy-parity-spec.md` §5、`docs/rich-features-spec.md` X-3-1

### M18: Unicode 入力 / 学習忘却 / デバッグウィンドウ

- **目的**: 3 つの周辺機能をまとめて実装。
- **前提**: M13 完了。
- **変更対象**: `core/src/UnicodeInputBuffer.cpp`（新規）、`learning/src/LearningStore.cpp`
  （Forget API 追加）、`tsf-tip/src/DebugWindow.cpp`（新規）。
- **実装範囲**:
  - M18-1: 仕様書 §6（Ctrl+Shift+U、hex バッファ、サロゲートペア生成）
  - M18-2: 仕様書 §7（Forget、ForgetMostRecent、TSV 永続化）
  - M18-3: 仕様書 §8（F10 起動、IPC ログ・状態遷移ログ、セキュリティ配慮）
- **受け入れ条件**:
  - Ctrl+Shift+U → `30A1` Enter で「ァ」が入る、範囲外は beep
  - Ctrl+Shift+Backspace で直近 commit の weight が 0 化
  - F10 でデバッグウィンドウが開き、直近 50 件の IPC ログが見える
- **参照仕様**: `docs/legacy-parity-spec.md` §6〜§8

### M19: マルチディスプレイ / カーソル追従

- **目的**: 候補・予測ウィンドウが現在のキャレット位置のモニタ作業領域内に
  正しく配置される。
- **前提**: M14, M15 完了。
- **変更対象**: `tsf-tip/src/CandidateWindow.cpp`、`tsf-tip/src/PredictionWindow.cpp`、
  `tsf-tip/src/CaretRectResolver.cpp`（新規）。
- **実装範囲**: 仕様書 §9（キャレット矩形取得の 3 段フォールバック、
  モニタ判定、Phase 5 範囲の DPI 対応）。
- **受け入れ条件**:
  - 2 枚モニタ環境で、ウィンドウ移動後の入力でも対象モニタの作業領域内に
    候補が出る
  - Chromium / Electron アプリで `GetGUIThreadInfo` フォールバックが動く
- **参照仕様**: `docs/legacy-parity-spec.md` §9

## Phase 6: Windows 特化（M20〜M27）

> Phase 5 完了後に着手。Phase 6-A（TSF 深耕）、6-B（Copilot+ PC / NPU）、
> 6-C（ネイティブ UI）の 3 サブフェーズを並列に進められる。
> なお推論バックエンド トラック（M24/M25/M27）は M8 のみに依存するため、
> Phase 5 と並行で先行着手できる（「Phase 5〜7 の依存関係と実行順」参照）。

### M20: ITfReconversion / ITfFnReconversion

- **目的**: 確定済みテキストの再変換に対応（MS-IME 互換）。
- **前提**: Phase 5 完了。
- **変更対象**: `tsf-tip/src/ReconversionFunction.cpp`（新規）、
  `tsf-tip/src/DllMain.cpp`（Category 登録）、`ipc/src/Payloads.cpp`
  （`ReverseConvert` 追加）。
- **実装範囲**: `docs/tsf-deep-integration-spec.md` §1。
- **受け入れ条件**:
  - メモ帳で「明日」選択 → 変換キーで「あした」候補が出る
  - 候補選択で範囲が置換される
- **参照仕様**: `docs/tsf-deep-integration-spec.md` §1

### M21: UI-less Mode / IME On-Off 状態管理

- **目的**: Windows 11 標準 Suggestion UI に乗る + IME On/Off の MS-IME 互換挙動。
- **前提**: Phase 5 完了。
- **変更対象**: `tsf-tip/src/CandidateListUIElement.cpp`（新規）、
  `tsf-tip/src/TextService.cpp`（`ActivateEx` でフラグ判定、
  `ITfKeyboardOpenCloseCompartment` 購読）。
- **実装範囲**: `docs/tsf-deep-integration-spec.md` §2、§4。
- **受け入れ条件**:
  - Office 2021/365 で自前候補ウィンドウが出ず OS 標準 UI に乗る
  - Win+Space / アプリ切替時に composition が確定される
- **参照仕様**: `docs/tsf-deep-integration-spec.md` §2, §4

### M22: 半角全角 / 無変換 / 変換 / Caps

- **目的**: MS-IME 互換のキー挙動。
- **前提**: M21 完了。
- **変更対象**: `tsf-tip/src/TextService.cpp::OnKeyDown` のテーブル拡張、
  `core/src/CharacterFormCycle.cpp`（新規、無変換キーの巡回ロジック）。
- **実装範囲**: `docs/tsf-deep-integration-spec.md` §3。
- **受け入れ条件**:
  - 「あした」選択 → 無変換キーで巡回（カナ → 半角カナ → 全角英数 → ASCII →
    元）
  - 変換キーで composition 中は候補表示、なしなら再変換
- **参照仕様**: `docs/tsf-deep-integration-spec.md` §3

### M23: 複合 DisplayAttribute / ITfMouseSink

- **目的**: 文節ごとに「注目」「変換済み」「未変換」の色分け + マウスクリックで
  注目文節を移動。
- **前提**: M20 完了。
- **変更対象**: `tsf-tip/src/DllMain.cpp`（3 新規 GUID）、
  `tsf-tip/src/DisplayAttributeProvider.cpp`、`tsf-tip/src/TextService.cpp`
  （文節ごとに `SetValue`、`ITfMouseSink` 接続）。
- **実装範囲**: `docs/tsf-deep-integration-spec.md` §5、§7。
- **受け入れ条件**:
  - 「nihongo」入力 → Space で候補表示 → カーソル位置の文節が青背景
  - preedit クリックで該当文節が注目に切替
- **参照仕様**: `docs/tsf-deep-integration-spec.md` §5, §7

### M24: DirectML / NPU バックエンド

- **目的**: Copilot+ PC（NPU）/ DirectML / CUDA / CPU の自動選択と切替。
- **前提**: M8 完了（llama.cpp バインディング選定）。
- **推奨実装時期**: M8 完了後、Phase 5 と並行する「推論バックエンド トラック」
  として着手可能（M25/M27 がこの先に連なる）。Phase 6 への配置は主題分類で
  あり、Phase 5 完了を待つ必要はない。
- **変更対象**: `inference-host/src/BackendSelector.cpp`（新規）、
  `inference-host/src/InferenceEngine.cpp`（エンジン分岐）、
  `inference-host/src/WinMlBackend.cpp`（新規、ONNX Runtime GenAI + Windows ML。
  DirectML が sustained engineering 化したため旧 `DirectMlBackend.cpp` /
  `QnnBackend.cpp` の個別 SDK 実装は採らず、EP は Windows ML の自動配信に委ねる）。
- **前提（追加）**: zenz-v3 → ONNX Runtime GenAI 形式の変換可否スパイク
  （Windows ML 経路 R2 のブロッカー。不可なら Copilot+ も R1 CPU を既定とする）。
- **実装範囲**: `docs/copilot-pc-backend-spec.md` §1〜§4（決定は §4.3、enum / 境界
  ポリシーは §4.4、フォールバック段位は §4.5）。
- **横断**: X-1-2 (TypingTempoTracker)・X-2-4 (PredictWithLLM)・
  X-3-4 (DetectAnomalies) はこの段階で Heavy レーンに乗せる。
- **受け入れ条件**:
  - Snapdragon X / Intel Core Ultra / NVIDIA GPU の各環境で適切な
    BackendKind が選ばれる
  - `--backend cuda` 指定で失敗時 CPU に自動フォールバック
- **参照仕様**: `docs/copilot-pc-backend-spec.md` §1〜§4、
  `docs/rich-features-spec.md` X-1-2, X-2-4, X-3-4

### M25: mmap ロード / 省電力モード

- **目的**: モデルを mmap でロードしてメモリ圧迫を抑制、バッテリ駆動時に挙動を
  抑制。
- **前提**: M24 完了。
- **変更対象**: `inference-host/src/MmapModelLoader.cpp`（新規）、
  `inference-host/src/PowerStatusWatcher.cpp`（新規）、
  `inference-host/src/main.cpp`（`WM_POWERBROADCAST` ハンドラ）。
- **実装範囲**: `docs/copilot-pc-backend-spec.md` §5、§6。
- **受け入れ条件**:
  - 7B gguf をロードしても WorkingSet が肥大化しない
  - バッテリ駆動時に Heavy レーンが Fast にデグレ
- **参照仕様**: `docs/copilot-pc-backend-spec.md` §5, §6

### M26: PerMonitorV2 DPI / Dark / Mica / DirectWrite

- **目的**: 候補・予測・デバッグウィンドウを Windows 11 標準ルックに統一。
- **前提**: M19 完了。
- **変更対象**: `tsf-tip/src/CandidateWindow.cpp`、`tsf-tip/src/PredictionWindow.cpp`、
  `tsf-tip/src/ThemeColors.h`（新規）、`tsf-tip/src/RenderingEngine.cpp`（新規、
  DComp + D2D + DirectWrite ラッパ）。
- **実装範囲**: `docs/copilot-pc-backend-spec.md` §7、`docs/native-ui-spec.md`
  §1〜§4。
- **受け入れ条件**:
  - 96/144/192 DPI で正しくスケール
  - Dark/Light テーマがシステムに追従
  - Windows 11 22H2 以降で Mica 背景
- **参照仕様**: `docs/copilot-pc-backend-spec.md` §7、`docs/native-ui-spec.md`

### M27: ARM64 ビルド

- **目的**: Snapdragon X Elite / 他 ARM64 Windows をネイティブサポート。
- **前提**: M24 完了。
- **変更対象**: `.github/workflows/windows.yml`（ARM64 クロスビルドジョブ追加。
  `amd64_arm64` SDK/リンカ環境 + **clang-cl** + Ninja + ARM64 toolchain。§8.1）、
  `cmake/toolchains/win-arm64-clang.cmake`（新規）、各 `CMakeLists.txt`（ARM 最適化
  フラグ。クロス時 `GGML_NATIVE=OFF` 強制。§8.2）。
- **実装範囲**: `docs/copilot-pc-backend-spec.md` §8。
- **受け入れ条件**（§8.4 で詳細確定）:
  - **CI 緑ゲート（必須・自動）**: 既存 x64 ランナー上の ARM64 **クロスビルド**（clang-cl
    必須＝ggml が MSVC ARM を拒否）が成功（ARM64 バイナリ生成まで。新インフラ不要）。
  - **ARM64 単体テスト実行**: `windows-11-arm` ランナーで ctest 緑。**public / private
    いずれでも label は有効**（private は従量課金）なので可視性に依らず CI 化できる。
  - **実機検証**: Snapdragon X 実機での動作確認（`gate:human-required`。CI ブロッカーに
    しない。§4.3 の NPU 非必須方針と整合）。
- **参照仕様**: `docs/copilot-pc-backend-spec.md` §8

## Phase 7: サイドロード配信（M28〜M34）

> Phase 6 完了後に着手。**配布方針（2026-06 確定）**: MVP は未署名 MSI 直接配布（DEV-415）、
> MSIX は Microsoft Store 経由で並行準備（MS 再署名のため自前署名不要、DEV-416）、有料署名を要する
> スタンドアロン MSIX サイドロードは当面延期（DEV-255）。**正典仕様**は `docs/sideload-packaging-spec.md` §0。
> なお M30（設定アプリ）・M34（DPAPI 暗号化）は Phase 5 完了のみが前提で、
> Phase 6 と並行・Phase 5 直後への前倒しが可能（依存関係の節を参照）。

### M28: MSIX パッケージング（MS Store 向け）

- **目的**: MSIX パッケージを構成し、MS Store 配布（DEV-416）の基盤とする。Store は Microsoft が
  再署名するため自前署名不要。MVP の直接配布は未署名 MSI（DEV-415、spec §4）であり、本マイルストーンは
  Store チャネル準備として位置づける（配布方針は spec §0）。
- **前提**: Phase 6 完了。
- **変更対象**: `pkg/msix/AppxManifest.xml`（新規）、`pkg/msix/Package.wapproj`
  （新規）、`pkg/msix/Assets/*.png`（新規）。
- **実装範囲**: `docs/sideload-packaging-spec.md` §1（ただし §1.6.1 (b) 初回起動時
  DL は共有 `HttpDownloader` に依存するため M32 へ送り、M28 は (c) 手動配置を
  operative default として出荷する。§1.6.1「マイルストーン順序の制約」）。
- **受け入れ条件**（Store 提出パッケージは Partner Center 提出後に Microsoft が署名するため、
  ローカル検証は**開発用自己署名テスト証明書**で行う。配布方針は spec §0）:
  - ローカル検証: 自己署名テスト証明書で署名した MSIX を、証明書を信頼させたクリーン VM で
    `Add-AppxPackage` 成功（`compat-test/msix_install_uninstall.ps1`、残骸 0 smoke）。**対象 OS は
    §1.0 で確定する TIP 配布経路に従う**（Option A = external-location は Win10 2004/build 19041+ / Win11、
    Option B/C = `com4` により build 20348+ = Win11 21H2+。Win10 22H2 client(19045) は B/C では対象外。spec §1.1）
  - 言語バーから azooKey が選べ、アンインストールで CLSID が消える
  - Store 提出・審査・Store 署名後のインストール確認は DEV-416（Partner Center）で扱う
- **設計メモ**: 開発用 `regsvr32` スクリプトは MSIX 登録方式と混同されないよう
  `-dev` 接尾辞（`scripts/register-dev.ps1` / `unregister-dev.ps1`）で経路を分離する。
  MSIX 登録経路と開発用 `regsvr32` 経路を取り違えると登録・解除事故につながる
  ため、`compat-test/msix_install_uninstall.ps1`（残骸 0 smoke）で検証する。
  詳細は `docs/sideload-packaging-spec.md` §1.1.1。
- **参照仕様**: `docs/sideload-packaging-spec.md` §1

### M29: EV/OV コード署名 CI（当面延期）

- **位置づけ**: 自前コード署名はスタンドアロン MSIX サイドロード専用であり、配布方針転換
  （spec §0）により **当面延期**（DEV-255）。MVP の未署名 MSI（DEV-415）・MS Store の MSIX
  （MS 再署名、DEV-416）はいずれも本マイルストーンを必要としない。
- **目的**: GitHub Actions で署名済み MSIX を自動生成（延期チャネル着手時）。
- **前提**: M28 完了 + EV/OV 証明書手当て済み。
- **変更対象**: `.github/workflows/release.yml`（新規）、GitHub Secrets 設定。
- **実装範囲**: `docs/sideload-packaging-spec.md` §2。
- **受け入れ条件**:
  - タグ push で署名済み MSIX が Draft Release に添付される
  - `signtool /verify` が成功
- **参照仕様**: `docs/sideload-packaging-spec.md` §2

### M30: WinUI 3 設定アプリ

- **目的**: TIP / Host とは別プロセスの GUI 設定アプリ。
- **前提**: Phase 5 完了（設定キーが確定）。
- **推奨実装時期**: Phase 5 完了後、Phase 6 と並行で着手可能（M28/M29 に
  依存しない）。M36-A の承認 UI（`confirm` モード）が M30 に依存するため、
  早期着手が望ましい。M11 の最小設定 UI と UI フレームワーク（WinUI 3）を
  揃えること。
- **変更対象**: `settings-app/`（M11 で導入した設定アプリを本格化）、
  `ipc/src/Payloads.cpp`（M11 の最小 `UpdateConfig` を拡張）、
  `inference-host/src/SettingsManager.cpp`（M11 の SettingsStore 最小実装を拡張）。
  ※ 最小 IPC / 設定ストア（`UpdateConfig` / settings.json 読込・反映）は M11 で導入済み。
  M30 はその上に UI とフル機能（横断項目・バッチ訂正等）を載せる。
- **実装範囲**: `docs/sideload-packaging-spec.md` §3。
- **横断**: X-3-6（バッチ訂正ビュー）、X-2-6（promptPrefixByApp UI）、
  X-2-7（Persona 表示）を本マイルストーンで設定アプリ内に集約。
- **受け入れ条件**:
  - 設定アプリで値を変更 → Host が即時反映 or 再起動指示
  - Windows 設定からの「詳細設定」起動で開く（`ITfFnConfigure`）
- **参照仕様**: `docs/sideload-packaging-spec.md` §3、`docs/rich-features-spec.md`
  X-2-6, X-2-7, X-3-6

### M31: WiX MSI インストーラ（MVP 既定配布）

- **目的**: v1.0 MVP の既定配布形態となる未署名 WiX MSI（DEV-415）。MSIX 不可環境（LTSC 等）も
  本経路で対応する。配布方針転換（spec §0）により代替から MVP 主経路へ格上げ。
- **前提**: Phase 6 完了（MVP 直接配布。Store MSIX の M28 には依存しない。spec §0 / DEV-415）。
- **変更対象**: `pkg/wix/Product.wxs`（新規）、`pkg/inno/setup.iss`（オプション）。
- **実装範囲**: `docs/sideload-packaging-spec.md` §4。
- **受け入れ条件**:
  - Win10 LTSC でインストール → IME 選択 → 入力 → 確定 → アンインストール
- **参照仕様**: `docs/sideload-packaging-spec.md` §4

### M32: WinGet マニフェスト + 自動更新 + 初回モデル取得（§1.6.1 (b)）

- **目的**: `winget install dolquis.azooKey` で導入できる + アプリ内自動更新 +
  v1.0 既定のモデル初回取得（§1.6.1 (b) 初回起動時オンデマンド DL）。
- **前提**: M31（MVP MSI インストーラ）完了。WinGet は出荷アーティファクト（未署名 MSI）を
  ラップし、自動更新・モデル DL は署名に依存しないため、延期した M29（スタンドアロン MSIX 署名）は
  前提としない（spec §0 / DEV-255）。**(b) 初回モデル DL の有効化に限り**、DEV-202（zenz GGUF
  再配布可否。`gate:human-required`）の確定を追加前提とする — 配信元が
  GitHub 再ホスト / 上流 HF / 保留 のいずれになるかを律するため（§1.6.1
  ライセンス分岐）。WinGet（§5）/ 自動更新（§6）は DEV-202 非依存で先行可。
- **変更対象**: `manifests/d/dolquis/azooKey/<ver>/*.yaml`（winget-pkgs への
  外部 PR）、`inference-host/src/UpdateChecker.cpp`（新規）、
  `inference-host/src/HttpDownloader.cpp`（新規。WinHTTP + SHA256 共通ヘルパの
  切り出し。M36-B が再利用）。**共通 WinHTTP 基盤（セッション/プロキシ/TLS/タイムアウト）は
  M16 が先に最小導入し（`docs/ai-backend-spec.md` §6 で契約化）、M32 はその基盤上に
  GET+SHA256 ダウンロード経路を追加する（Phase 5 への前倒しは不要）**、初回モデル取得
  ロジック（`expected.json` ピン照合 →
  `.part`→検証→rename → probe-load → `selectedPath` コミット）、設定アプリ UI。
- **実装範囲**: `docs/sideload-packaging-spec.md` §5、§6、および §1.6.1 (b)
  （初回起動時オンデマンド DL。同 §1.6.1 の取得方式・ピン・autoselect 規律に従う）。
- **受け入れ条件**:
  - winget-pkgs に PR が merge され `winget install` で導入可能
  - 起動時 + 24h 周期で新版検出、ユーザー承認で MSIX 適用
  - **DEV-202 確定（配信元決定）かつ `expected.json` ピン投入済みのとき**: 既定
    設定下（§1.6.1 (b) の前提ガード成立 — `autoLoadOnHostStart=true` かつ
    非 `offline`）で初回起動時に ピンと一致する GGUF を DL → SHA256 検証 →
    probe-load 成功 → `selectedPath` コミット。`offline` / `autoLoadOnHostStart=false`
    時は DL せず (c2) 明示選択に委ねる。
  - **DEV-202 未確定（配信元 保留）または ピン未投入のとき**: (b) DL を有効化せず、
    **(c2) ユーザー明示選択（`model.selectedPath` を `settings.json` / 設定 UI で
    設定）で Zenzai がロードできること**を受け入れ条件とする。ピン未投入では
    (c1) autoselect が無効なため、ファイルを置くだけ（空 `selectedPath`）では
    不合格（§1.6.1 (c2)）。DL 受け入れ項目は DEV-202 確定 + ピン投入後に適用。
    いずれの分岐でも DL 失敗 / 不一致時は手動配置へフォールバックし Host は
    落ちない（§1.6.1 / M8 / M47 と整合）
- **参照仕様**: `docs/sideload-packaging-spec.md` §5, §6, §1.6.1

### M33: ETW / WER

- **目的**: 本番環境の観測性とクラッシュ収集。
- **前提**: M28 完了。
- **変更対象**: `core/src/EtwLogger.cpp`（新規）、`inference-host/src/CrashHandler.cpp`
  （新規、`SetUnhandledExceptionFilter`）、`settings/mvp-settings.schema.json`
  （`privacy.crashReportConsent` subfield を先行導入。§3.6 / `privacy-and-secure-input-spec.md` §7）。
- **実装範囲**: `docs/sideload-packaging-spec.md` §7、§8。
- **受け入れ条件**:
  - `wpr -start` でトレース取得、`wpa` で Event ID が見える
  - ETW トレース（`.etl`）に入力本文（`reading` / `surface` 等）が含まれない
    （build / env / mode によらず。spec §7.2.1）
  - Host クラッシュで `%LOCALAPPDATA%\azooKey\crashes\*.dmp` が残る
  - `privacy.crashReportConsent = off` で azooKey 管理のダンプ
    （`%LOCALAPPDATA%\azooKey\crashes\`）が書かれない（OS の WER / LocalDumps はマシン
    ポリシーで azooKey 同意スコープ外。azooKey は自身向け LocalDumps 登録をしない。spec §8.3）
  - ダンプ保持上限（既定 最新 5 / 50 MB / 30 日）超過で古いダンプが削除される（spec §8.2）
- **参照仕様**: `docs/sideload-packaging-spec.md` §7, §8 / `docs/privacy-and-secure-input-spec.md` §7（`crashReportConsent` schema）

### M34: DPAPI 学習データ暗号化

- **目的**: `learning.tsv` / `user_dict.json` / OpenAI API キーをユーザースコープ
  で暗号化。M35 / M36 で追加される学習データ（`typo_corrections.tsv` /
  `auto_words.tsv`）も同等の機微情報として対象に含める。
- **前提**: Phase 5 完了。
- **推奨実装時期**: Phase 5 直後へ前倒し推奨（セキュリティ優先）。Phase 5 の
  M16（Magic Conversion）が OpenAI API キーの平文保存を持ち込むため、M34 を
  Phase 7 末尾に置くと平文保存期間が長期化する。Phase 5 直後に前倒しして
  暗号化ギャップを早期に塞ぐ。M16 着手時点では README で平文保存を注意喚起する。
  API キーの at-rest 保存は `dpapi:` prefix 規約（`docs/ai-backend-spec.md` §9）で行い、
  prefix の有無で復号/平文を分岐する。M34 投入後は設定値の移行のみで AiBackend は不変。
- **変更対象**: `learning/src/DpapiCrypto.cpp`（新規）、`learning/src/LearningStore.cpp`
  （Load/Save をラップ）、`settings-app/`（API キー入力時に暗号化）。M35 / M36 着手
  済みの場合は `TypoCorrectionStore` / `AutoWordStore` の Load/Save も同様にラップ。
- **実装範囲**: `docs/sideload-packaging-spec.md` §9。
- **受け入れ条件**:
  - `learning.tsv.enc` が暗号化済みで、他ユーザでは復号失敗
  - 初回起動時に既存平文 TSV から暗号化形式へ移行
- **参照仕様**: `docs/sideload-packaging-spec.md` §9

## 追加機能マイルストーン

> Phase 5〜7 の番号体系とは独立に、差別化機能として追加するもの。

### M35: 個人タイプミス学習・自動修正

- **目的**: ユーザー個人が繰り返す打ち間違い（タイプミス）を学習し、同じ
  タイプミスをしたときに正しい入力へ補正・提示する。汎用 LM 補正
  （`DebugTypoCorrection`）とは独立した、個人の打鍵の癖を学習する機能。
- **前提**: M6（Commit / Observation）完了。M7（学習）と独立に着手可能。
- **推奨実装時期**: v1.0（Phase 4）完了直後、Phase 5 と並行する独立トラックと
  して前倒し可能。Zenzai・TSF 深耕・パッケージングのいずれにも依存しない
  小規模機能（3〜4 週）で、差別化価値を早期に提供できる。設定 UI（M30）完成
  までは host CLI / 環境変数で実効値を受ける。
- **変更対象**: `learning/src/TypoCorrectionStore.cpp`（新規）、`ipc/src/Payloads.cpp`
  ・`ipc/src/Messages.cpp`（`ObserveTypo` 追加）、`inference-host/src/InferenceEngine.cpp`
  ・`Dispatcher.cpp`・`main.cpp`、`tsf-tip/src/TextService.cpp`、
  `settings/mvp-settings.schema.json`。
- **実装範囲**: `docs/typo-correction-learning-spec.md` §1〜§8。
  - 検出 2 トリガ（未確定中の backspace 訂正 / 確定直後の打ち直し）
  - `wrong_reading → correct_reading` のかな読みペアを頻度カウントで永続化
  - 動作モード 3 値（`off` / `suggest` / `auto_replace`）
- **受け入れ条件**:
  - 同一タイプミスを `typo_min_count`（既定 3）回観測すると、以降の変換で
    `suggest` は `typo-correction` マーク付き候補を注入、`auto_replace` は
    補正後読みで変換する
  - しきい値未満では蓄積のみで適用されない
  - `off` で学習・適用が一切行われない
  - `typo_corrections.tsv` の Save→Load で学習内容が保持される
- **参照仕様**: `docs/typo-correction-learning-spec.md`

### M36-A: 新語自動取得（ユーザー入力マイニング）

- **目的**: ユーザー自身の確定履歴から辞書に無い未知語（OOV）を検出し、新語専用
  ストアに蓄積。承認済み（confirmed）の語を変換候補へ注入する。手動登録の
  `UserDictionary` とは独立した自動取得機能。
- **前提**: M6（Commit / Observation）完了のみ。M7・M32 と独立に着手可能。
  HTTP に依存しない（オフライン完結）。
- **推奨実装時期**: 既定の `confirm` モードの承認フローが M30（WinUI 3 設定
  アプリ）に依存するため、Phase 7 内で M30 の直後に配置する。M30 より前に
  着手する場合は `auto` モード / デバッグ CLI 限定運用になる。
- **変更対象**: `learning/src/AutoWordStore.cpp`（新規）、`ipc/src/Messages.cpp`
  ・`ipc/src/Payloads.cpp`（`ListNewWordCandidates` / `ResolveNewWord` 追加）、
  `inference-host/src/InferenceEngine.cpp`・`Dispatcher.cpp`・`main.cpp`、
  `settings/mvp-settings.schema.json`。
- **実装範囲**: `docs/auto-word-registration-spec.md` §3〜§4・§6〜§9。
  - `CommitObservation` を hook した OOV 検出（新 IPC 不要）
  - 新ストア `AutoWordStore`（pending / confirmed / rejected の状態管理）
  - 承認フロー IPC（`ListNewWordCandidates` / `ResolveNewWord`）
  - 登録モード 2 値（`confirm` / `auto`）
- **受け入れ条件**:
  - `auto` モード: 辞書に無い surface を `miningMinCount`（既定 3）回確定すると
    confirmed に自動昇格し、以降の変換で `auto-word` マーク付き候補が注入される
  - `confirm` モード（既定）: 検出語は count を加算しつつ pending のまま保持され、
    `ResolveNewWord` での承認後にはじめて `auto-word` 候補として注入される。
    承認前（pending）は変換に使われない
  - 記号のみ・英数のみ・1 文字は新語として記録されない
  - reject した語は再観測しても再提示されない
  - `auto_words.tsv` の Save→Load で内容が保持される
- **参照仕様**: `docs/auto-word-registration-spec.md`

### M36-B: 新語自動取得（リモートトレンド語）

- **目的**: プロジェクトがホストする整形済みトレンド語リスト（静的アセット）を
  定期ダウンロード・検証し、新語ストアにマージする。
- **前提**: M36-A 完了 + M32 の WinHTTP 基盤（共通ヘルパ `HttpDownloader` の
  切り出し）。
- **推奨実装時期**: M32 の WinHTTP 基盤にハード依存するため、M32 以降に配置する。
- **変更対象**: `inference-host/src/TrendingWordFetcher.cpp`・
  `inference-host/src/HttpDownloader.cpp`（M32 で新規作成・共有する WinHTTP +
  SHA256 ヘルパを再利用）、`inference-host/src/main.cpp`、
  `inference-host/CMakeLists.txt`（`winhttp.lib` リンク）。
- **実装範囲**: `docs/auto-word-registration-spec.md` §5。
  - WinHTTP による定期 DL → SHA256 検証 → `AutoWordStore::IngestTrending`
  - 取得元はプロジェクトホストの静的アセット（GitHub Releases 等）。
    Google Trends の直接スクレイピングはしない。上流のデータ生成パイプラインは
    クライアント実装の範囲外
- **受け入れ条件**:
  - 起動時 + 周期で正規アセットを取得し、SHA256 検証通過時のみ取り込む
  - ハッシュ不一致のアセットは破棄しストアを変更しない
  - `trendingEnabled=false` で一切ネットワークアクセスしない
- **参照仕様**: `docs/auto-word-registration-spec.md`

### M48: アプリ別入力プロファイル

- **目的**: 前面アプリに応じて予測・学習・文体・AI backend・候補タグ重みを
  切り替える。既存 `promptPrefixByApp`（`settings/mvp-settings.schema.json`）の
  発展統合として位置づける。
- **前提**: M46 完了（セーフ入力モード）。`ForegroundAppDetector` を M46 と
  共用する。
- **推奨実装時期**: M46 完了直後、Phase 6 と並行する独立トラック。M30（設定
  アプリ）完成後に UI が揃う。
- **変更対象**: `settings/mvp-settings.schema.json`（`profilesByApp` ブロック
  追加、`promptPrefixByApp` を後方互換で読み続ける）、
  `inference-host/src/AppProfileResolver.cpp`（新規）、
  `inference-host/src/Dispatcher.cpp`（候補生成・rerank へ `app_id` を伝播）、
  `settings-app/`（アプリ別設定タブ）。
- **実装範囲**: `docs/app-profile-spec.md`。
  - `ForegroundAppDetector` + 500ms TTL キャッシュ
  - 解決順: `profilesByApp[process_name]` →
    `profilesByApp[window_class]` → `profilesByApp[default]` → グローバル
  - 候補タグ重み（Technical / English / Polite など）の boost
  - secure / private / normal の指定（M46 と接続）
  - 既存 `promptPrefixByApp` 値は `profilesByApp[].promptPrefix` として読む
- **受け入れ条件**:
  - VS Code（`code.exe`）で技術語タグの候補順位が上がる
  - Outlook（`outlook.exe`）で polite タグの候補順位が上がる
  - secure 指定アプリで学習・外部 AI が停止する（M46 と整合）
  - アプリ切替後 1 秒以内にプロファイルが反映される
- **参照仕様**: `docs/app-profile-spec.md`

### M58: ローマ字一括変換（Batch Romaji Conversion）

> ローマ字で文章を最後まで打ち切り、最後に一度だけ一括変換する追加入力モード。
> 「変換のたびに思考と視線が中断される」問題の解消を狙う差別化機能。既定 OFF・
> 後方互換で、有効化時のみ動作する。M58-A（コア）→ M58-B（長文・文節再変換）→
> M58-C（AI 整文）の段階構成。正典仕様は `docs/romaji-batch-conversion-spec.md`。

#### M58-A: 一括変換コア

- **目的**: ローマ字蓄積中は推論クエリを発火せず、トリガ（Space）で全文かなを
  Zenzai でかな漢字変換し、全体確定する最小フローを実現する。
- **前提**: M6（Commit / Observation）、M13（InputState 状態機械）、M14（ライブ変換の
  状態基盤）完了。**推奨**: M8/M9（実 Zenzai）完了後に品質が揃う。
- **変更対象**: `core/include/azookey/core/InputState.h`（`BatchAccumulating` /
  `BatchConverting` 追加）、
  `tsf-tip/src/TextService.cpp`（`OnKeyDown` の蓄積分岐・Preedit 表示切替）、
  `core/src/RomajiKanaConverter.cpp`（蓄積運用）、`ipc/src/Messages.cpp`・
  `ipc/src/Payloads.cpp`（`QueryBatchConversion` 追加）、
  `inference-host/src/Dispatcher.cpp`・`InferenceEngine.cpp`、
  `settings/mvp-settings.schema.json`。
- **実装範囲**: `docs/romaji-batch-conversion-spec.md` §3〜§6・§8。
  - `BatchAccumulating` / `BatchConverting`（応答待ち）状態と遷移（Space=一括変換要求、
    応答受信=Selecting、応答前の追加打鍵/Esc=in-flight Cancel→蓄積復帰、Enter=全体確定）
  - 蓄積中は IPC 非発火（ローカルでかなバッファのみ更新）
  - `batchRomajiPreviewStyle`（`kana` / `romaji`）の Preedit 表示切替
  - `QueryBatchConversion`（`mode="neural"`）IPC の往復、設定キー 3 種
- **受け入れ条件**:
  - `batchRomajiConversion=true` で、ローマ字全文を蓄積中に候補/予測/ライブ変換の
    IPC が一切送られない（中断ゼロ）
  - Space で全文が一括変換され、Enter で妥当な日本語が確定する
    （例: `kyouhaiitenkidesu` → きょうはいいてんきです → 今日はいい天気です。
    既存 `RomajiKanaConverter` が実際に出力するかなで例を取り、`ii`/`oo` を `ー` に
    正規化しない前提）
  - `batchRomajiPreviewStyle` 切替で Preedit がかな / 生ローマ字に切り替わる
  - `batchRomajiConversion=false` で従来の逐次変換・状態遷移が一切変わらない
  - 実機 Win11 での end-to-end 確認（`gate:human-required`）
- **参照仕様**: `docs/romaji-batch-conversion-spec.md`

#### M58-B: 長文・文節再変換

- **目的**: 長文の一括変換を成立させ、変換結果を文節単位で再選択できるようにする。
- **前提**: M58-A 完了、M20（再変換）、**M51 の UUIDv7 `trace_id` 生成・伝播**。
  out-of-band Cancel のクロスクライアント分離（spec §6.3.2）は host キャンセルレジストリ /
  TIP 応答相関を `(trace_id, request_id)` でキーするが、これは `trace_id` が実際に
  グローバル一意（UUIDv7）であることを前提とする。現行 TIP は envelope に**定数
  `trace_id`**（`tsf-tip/src/TextService.cpp` の `"tip-key-query"` / `"tip-faf"` 等）を
  載せており、UUIDv7 採番は M51 のスコープ（§7.7.1 / 本書 M51）であるため、これが無いと
  複数 TIP インスタンスが同一の定数 `trace_id` を共有し `request_id` も衝突しうるため、
  `(trace_id, request_id)` でも一意にならずクロスクライアント誤キャンセルが再発する。
  M51 本体のうち本 M58-B が依存するのは **`trace_id` の UUIDv7 生成・伝播の部分のみ**で
  あり、レイテンシトレーサ / viewer CLI には依存しない。**M58-B を M51 本体より先行
  させる場合**は、out-of-band cancel 対象 envelope（batch query / cancel）に限り UUIDv7
  `trace_id` を採番する処理を M58-B スコープに含め、M51 の生成セマンティクス（1 論理操作
  = 1 `trace_id`、§7.7.1）と一致させること（後の M51 全体実装と矛盾しない）。spec §6.3.2。
- **変更対象**: `inference-host/src/Dispatcher.cpp`（文境界チャンク分割・結合・
  共有キャンセルレジストリへの協調キャンセルチェック）、`ipc/`（segments 構造・
  `HandshakeResponse.capabilities`・共有 `CancellationRegistry`）、
  `tsf-tip/src/TextService.cpp`（文節カーソル移動・候補切替 UI・Cancel 専用 control 接続）。
  out-of-band Cancel は **control 接続 + 共有キャンセルレジストリ + チャンク境界での
  協調キャンセル**で実現し（spec §6.3.2 で確定）、`NamedPipeTransport` の多重応答改造は
  不要（spec §6.3.5）。
- **実装範囲**: `docs/romaji-batch-conversion-spec.md` §6.2・§6.3・§7。
  - アウトオブバンドな Cancel 経路（**確定: Cancel 専用 control 接続 + 既存
    `RequestScheduler` を `(trace_id, request_id)` キーへ一般化した共有レジストリ +
    チャンク境界協調キャンセル**。新規並行構造は作らず `TrackCancellation` /
    `IsCanceled` / `CompleteRequest` / prune を踏襲し、全終端パスで `CompleteRequest`
    してエントリをリークさせない。spec §6.3.2）。同期 ClientLoop +
    単一接続では遅い変換中に Cancel が処理されないため必須。host は `HandshakeResponse.
    capabilities` に `"oob_cancel"` を広告し、TIP は対応 host にのみ依存（非対応 host は
    best-effort Cancel + 結果破棄に fallback）。host 認証は接続単位のため、control 接続は
    primary と同じ `handshake_token` で Handshake 後に Cancel を送る（token 保護構成で
    Cancel が無言で捨てられるのを防ぐ。spec §6.3.2）
  - TIP 側事前分割（フレーム上限 `kMaxFrameSize` = 1 MB 超の蓄積を文境界で複数リクエストへ。
    文境界が無い場合はバイト安全ハード分割でフォールバック）
  - host 側チャンク分割（フレーム上限内リクエストを zenz コンテキスト長で文境界分割）→
    逐次変換 → 結合。文境界が無くコンテキスト超の場合はバイト/トークン安全ハード分割で
    フォールバック
  - `segments[]` 返却と Selecting 中の ←/→ 文節移動・Space/数字での候補切替
  - multi-segment commit payload `CommitSegmentsObservation`（新 `MessageType`。文節列を
    1 メッセージで原子的に確定・学習。`HandshakeResponse` に host 側 `capabilities` を追加して
    `commit_segments` を広告、TIP は応答に含まれるときだけ送り未対応 host へは単発
    `CommitObservation` フォールバック）。M59 / M60 と共有（spec §6.4）
  - 進捗フィードバックは**サブリクエスト粒度**（各サブリクエスト完了ごとに Preedit を
    漸進更新し**確定不可**、全サブリクエストの最終応答 `partial:false` が揃って初めて
    `Selecting`（確定可能）へ遷移）。request 内 `partial:true` ストリーミングは下記の
    将来拡張のみで使い、M58-B 既定経路では使わない（spec §6.3.5）
  - 複数サブリクエストを 1 論理バッチとして集約（全サブリクエストの最終応答受信で
    Selecting、`full_surface`/`segments` は送信順に連結）、`Cancel` は control 接続から
    各 in-flight サブリクエスト ID へ個別送信。タイムアウト / エラー時は部分確定せず
    全 in-flight を Cancel して fallback 連鎖へ。host キャンセルレジストリと TIP 応答相関は
    `(trace_id, request_id)` でキーする（`request_id` 単独はインスタンスごと採番で衝突し、
    別クライアントを誤キャンセルするため。`trace_id` は全 envelope 必須の UUIDv7 で、その
    生成は M51 前提。上記 M58-B 前提節参照）。
    タイムアウト / キャンセル済みの stale 応答は fallback / 新バッチ送信前に drain・破棄
    （または primary 再接続）して古い segments の誤結合を防ぐ（spec §6.3.2・§6.3.3）
  - 既定の正しさ経路は現行トランスポートの 1 リクエスト 1 応答契約に従い、host 内部
    チャンク分割で `partial:false` を 1 つ返す（進捗はサブリクエスト粒度）
  - request 内 `partial:true` 逐次表示（同一 `request_id` への複数応答ストリーミング、
    `NamedPipeTransport` の多重応答対応）は **M58-B では採用しない（spec §6.3.5 で確定。
    将来の任意拡張）**。`Response.partial` は予約フィールドとして既定 `false`
- **受け入れ条件**:
  - フレーム上限内の長文が host 側チャンク分割で変換される
  - 既定経路（単一 `partial:false`／論理バッチ）: 論理バッチの全（サブ）リクエストの
    最終応答が揃うまでは確定不可で、途中の Enter は部分結果を確定しない
  - ストリーミング拡張を採用する場合のみ: `partial:true` の途中で Enter を押しても
    部分結果が確定されず、最終応答まで確定不可
  - フレーム上限（`kMaxFrameSize` = 1 MB）を超える蓄積は TIP 側で複数リクエストへ
    事前分割され、各リクエストが上限未満で送信・変換される（フレーム上限超の単一
    リクエストは IPC 層で拒否されるため送らない）。**分割判定は元バッファ長ではなく
    シリアライズ後のフレーム/エンベロープバイト数で行う**（`reading` +（ai-cleanup の）
    `raw_romaji` + JSON/envelope オーバーヘッド込み）
  - 事前分割した複数サブリクエストは 1 論理バッチとして集約され、全サブリクエストの
    最終応答が揃ってから確定可能になる（最初のチャンクだけで Selecting に入らない）
  - 変換中の Esc / 追加打鍵で **全 in-flight サブリクエスト**がキャンセルされ、
    取り残されたサブリクエストが走り続けない
  - 変換結果の特定文節だけ候補を選び直して確定できる
- **参照仕様**: `docs/romaji-batch-conversion-spec.md`

#### M58-C: AI 整文モード

- **目的**: `ai-cleanup` モードで誤字補正・句読点挿入・整文まで AI に委譲する。
- **前提**: M58-A 完了、M16（aiBackend）、M24（local-zenzai）、**M46（セーフ入力モード /
  PrivacyGate）**。`ai-cleanup` は全文を外部 AI（OpenAI 等）に送りうるため、M46 の
  secure ゲートを前提とする（`docs/privacy-and-secure-input-spec.md`）。
- **変更対象**: `inference-host/src/Dispatcher.cpp`・`InferenceEngine.cpp`
  （`mode="ai-cleanup"` 経路）、`settings/mvp-settings.schema.json`
  （`batchAutoPunctuation`）、M46 の `PrivacyGate` 連携。
- **実装範囲**: `docs/romaji-batch-conversion-spec.md` §5・§6.1・§7。
  - `aiBackend`（local-zenzai / openai）へ全文委譲、`includeContextInAITransform` 整合
  - `mode=ai-cleanup` のリクエストでは `raw_romaji`（生ローマ字）を必須で送る
  - **secure 入力（M46 PrivacyGate）では `ai-cleanup` を強制無効化**し外部 AI へ送らない
    （`neural` / かな確定へ fallback）。secure-app・パスワード欄の全文が外部 AI に
    渡らないことを保証する
  - `batchAutoPunctuation` を `QueryBatchConversion` の `auto_punctuation` として host へ
    伝搬し、句読点自動挿入を行う（host が ON/OFF を判別できるようペイロードに載せる）
  - `ai-cleanup` 失敗時 `neural` fallback、`neural` 失敗時かな確定の連鎖
- **受け入れ条件**:
  - `batchConversionMode=ai-cleanup` で誤字を含むローマ字全文が補正・整文される
    （`raw_romaji` を必須送信し、生ローマ字の誤字パターンを補正に使う）
  - secure 指定アプリ / パスワード欄では `ai-cleanup` が外部 AI に送信せず `neural`
    / かな確定に fallback する（M46 PrivacyGate と整合）
  - `aiBackend=none` のとき `neural` に fallback して動作する
  - `batchAutoPunctuation` ON/OFF で句読点挿入が切り替わる
- **参照仕様**: `docs/romaji-batch-conversion-spec.md`

### M59: 動的自動句読点（ライブ変換）

> ライブ変換中に、文章の適切な位置へ句読点（読点 `、` / 句点 `。`）を動的に挿入し、
> 文脈の変化に応じて削除・再配置する追加機能。既定 OFF・後方互換で、有効化時のみ
> 動作する。M58-C（一括変換 + AI 整文の句読点）とは別経路で、**通常の逐次ライブ変換中**に
> 動作する。正典仕様は `docs/dynamic-punctuation-spec.md`。

- **目的**: ローマ字でかな漢字変換しながら書くとき、句読点キーを明示的に打鍵しなくても
  IME が節境界・文末へ句読点を補い、入力変化に応じて再配置・削除する。
- **前提**: M13（InputState 状態機械）、**M14（ライブ変換）完了**。決定的ベースライン
  （`PunctuationInserter`）は M14 後に着手可能。ニューラル句読点（zenz 出力）品質レイヤは
  M8（Zenzai）に依存。X-1-2（`TypingTempoTracker`）を安定化に再利用するため M14 末の
  リッチ化と前後して実装すると効率がよい。
- **推奨実装時期**: M14（ライブ変換）完了直後、X-1 リッチ化と並行する独立トラック。
  設定 UI（M30）完成までは host CLI / 環境変数で実効値を受ける。
- **変更対象**: `inference-host/src/PunctuationInserter.cpp`（新規・決定的挿入レイヤ）、
  `inference-host/src/Dispatcher.cpp`・`InferenceEngine.cpp`（ライブ変換要求
  〔現状 `QueryCandidatesRequest.live`〕の `auto_punctuation` / `punctuation_style` 処理、
  `segments[].auto_punctuation` 返却、`CommitSegmentsObservation` ハンドラ）、
  `ipc/src/Messages.cpp`・`ipc/src/Payloads.cpp`（`QueryCandidates` 拡張・応答 segments の
  自動句読点マーカ・`CommitSegmentsObservation` 追加。spec §6.4／M58-B と共有）、
  `core/include/azookey/core/InputState.h` / 状態機械（Backspace 削除単位から自動句読点を
  除外）、`core/include/azookey/core/SegmentPos.h`（新規・`pos` 列挙）、
  `core/src/PunctuationRules.cpp`（新規・TSV ルールパーサ／マージ／ホットリロード）、
  `tsf-tip/src/TextService.cpp`（Preedit 描画・Backspace 単位・確定時の学習分離）、
  `settings/mvp-settings.schema.json`。
- **実装範囲**: `docs/dynamic-punctuation-spec.md` §3〜§9。
  - host 側 `PunctuationInserter`（決定的な節境界・文末ヒューリスティック挿入）
  - ライブ変換経路（M14）への統合。`liveConversion=true` のときのみ動作
  - full-preedit 再計算による挿入・削除・再配置（明示的削除ロジックを持たない）
  - 安定化（`dynamicPunctuationStability` = `onPause` でタイピング中は挿入せず idle で挿入。
    `TypingTempoTracker` を再利用）
  - 読み↔surface 非対称の扱い: Backspace はかな単位を削除し自動句読点を数えない／
    確定時に自動句読点スパンを分離して学習を汚染しない／文中キャレット編集は M20 統合へ送る
  - 確定は M58-B と共有の `CommitSegmentsObservation`（spec §6.4）で行い、自動句読点文節を
    `is_auto_punctuation=true` として送って学習対象外にする（capability 非対応 host は単発
    `CommitObservation` フォールバック）。M58-B 未着手時は単発フォールバック経路で先行可能
  - 字種切替（`dynamicPunctuationStyle` = `ja` / `fullwidth_latin`）
  - 品詞フィールド `segments[].pos` / `head_pos`（`core` の `SegmentPos` 列挙。任意・後方互換。
    曖昧性ガード〔「が」格/接続、「て・で」補助用言〕を品詞駆動化。pos 無しは表層フォールバック。spec §7.2.1）。
    host が辞書 cid/mid（rcid/lcid → 品詞名 → `SegmentPos`）から導出（数値直書きせず cid→品詞名表経由。spec §7.2.2）。
    mid → `SegmentSemantic`（人名/地名/組織/日付…）を補助判定に（固有名詞連鎖・日付の読点抑制。spec §7.2.3）。
    cid/mid 表は辞書アセット同梱 `id.def` / mid 定義から M8 ロード時に密配列化、欠落時は全 `Unknown` へ縮退（spec §7.2.4）
  - 句読点ルールの TSV 外部化（`punctuation-rules.tsv`: kind/match/base_score/guard。組み込み既定を
    `(kind,match)` で上書き・追加、`base_score=0` で無効化。字種は TSV に書かず `dynamicPunctuationStyle`
    由来。M17 ホットリロード基盤再利用。spec §4.1.4）。guard はミニ言語（EBNF・`;` AND・`=`/`!=`・
    Unknown 評価バイアス・未知トークン行スキップ。spec §4.1.5）
  - 安定化（`onPause` は idle タイマー `dynamicPunctuationIdleMs` で `IdleTimeout` 駆動の
    再評価が必須。最後の打鍵後にライブ変換要求を post し句読点を挿入。spec §4.3.1）。timing は
    TIP がリクエストの `auto_punctuation` に符号化（打鍵中=false 抑制／idle・commit=true 挿入。
    host は typing/idle を知らず `true` のときだけ挿入。spec §7.1.1）
  - 設定キー 6 種（`dynamicPunctuation` / `dynamicPunctuationStyle` /
    `dynamicPunctuationStability` / `dynamicPunctuationIdleMs` / `segmentBoundaryConfidence` /
    `punctuationRulesPath`）
- **受け入れ条件**:
  - `liveConversion=true` + `dynamicPunctuation=true` で、文を打つと節境界・文末に
    句読点が現れ、続けて打つと文節構造の変化に応じて句読点が再配置・削除される
  - Enter で句読点を含む妥当な日本語が確定する
  - Backspace がかな 1 単位を削除し、自動句読点を削除単位に数えない
  - 自動句読点を含む確定で学習が汚染されない（直後に同じ読みを打って句読点なしの
    素直な候補が出る）
  - `dynamicPunctuationStyle` 切替で挿入字種が `、。` / `，．` に切り替わる
  - `dynamicPunctuationStability=onPause` でタイピング中は句読点が出ず、入力停止時に挿入される
  - `liveConversion=false` または `dynamicPunctuation=false` で句読点が一切自動挿入されず、
    従来のライブ変換・候補ウィンドウ・状態遷移が一切変わらない
  - 実機 Win11 での end-to-end 確認（`gate:human-required`）
- **参照仕様**: `docs/dynamic-punctuation-spec.md`

### M60: ローマ字入力中インライン英単語候補

> 日本語ローマ字入力中に、英数モードへ切り替えることなく英単語を候補列へ注入し、
> 選択で英単語を確定できる追加機能。azooKey 本家 `englishCandidateInRoman2KanaInput`
> 相当。既定 OFF・後方互換。**スコープは候補注入のみ（1 語単位）**。連続英文タイプは
> 将来課題。正典仕様は `docs/inline-english-candidate-spec.md`。

- **目的**: ローマ字で `apple` と打つと、かな漢字候補に加え英単語候補（`apple` /
  `Apple` …）を注入し、英数モード切替なしで英単語を入力できるようにする。
- **前提**: M5（候補 UI）、M6（Commit / Observation）完了。生ローマ字バッファ保持
  （`docs/romaji-batch-conversion-spec.md` §4.1）を M58 と共有・再利用する。English タグ
  描画は X-2-3（`CandidateTag`）を再利用。辞書ゲーティング（品質レイヤ）は任意で、
  ベースライン（生ローマ字 + 大文字化）は辞書なしで動作する。
- **推奨実装時期**: v1.0（Phase 4）完了直後、Phase 5 と並行する独立トラックとして
  前倒し可能。Zenzai・TSF 深耕・パッケージングに依存しない小規模機能。設定 UI（M30）
  完成までは host CLI / 環境変数で実効値を受ける。
- **変更対象**: `tsf-tip/src/TextService.cpp`（生ローマ字バッファ保持の共有・候補注入経路・
  英単語確定時の reading=生ローマ字での Observe）、`ipc/src/Payloads.cpp`
  （`QueryCandidates` に `raw_romaji` / `english_candidates`、候補 `tag` 付与）、
  `inference-host/src/EnglishCandidateProvider.cpp`（新規・生成/ゲーティング/順位）、
  `inference-host/src/EnglishDictionary.cpp`（新規・TSV パース / `.bin` コンパイル・mmap・
  ルックアップ。spec §4.4・§4.5）、
  `inference-host/src/Dispatcher.cpp`・`InferenceEngine.cpp`、
  `learning/`（English チャネル or source タグでの区別）、
  `settings/mvp-settings.schema.json`。
- **実装範囲**: `docs/inline-english-candidate-spec.md` §3〜§8。
  - 候補生成（生ローマ字そのもの + 大文字化バリアント + 任意で全角ローマ字・辞書一致語）
  - ゲーティング（最小長・英語意図ヒューリスティック・辞書ヒット）と順位（日本語上位候補を
    奪わない／自動選択しない）
  - `QueryCandidates` 拡張（`raw_romaji` / `english_candidates` / 候補 `tag=English`）
  - 確定時 reading=生ローマ字での学習（かな漢字学習と混線させない）
  - 英単語辞書フォーマット（TSV: `surface`/`frequency`/`flags`。`spec` §4.4）と
    ルックアップ（lower キー・頻度降順・`flags` で大文字化優先）。ベースラインは辞書なしで動作
  - 辞書バイナリ形式（コンパイル済み `.bin`: ヘッダ + ソート済みレコード配列 + string pool。
    LE 固定・二分探索・mmap。TSV をソース、`.bin` をキャッシュ。**TSV 不在のバンドル `.bin` 単体
    ロードも正規ケース**、破損時は TSV があればフォールバック・無ければ辞書無効。spec §4.5）
  - 辞書の差分更新（overlay `english-words.delta.bin`: **末尾追記可能な自己完結フレーム列**
    〔文字列インライン・別 string pool 無し〕、upsert/delete tombstone を到着順 append-only、
    ルックアップはメモリ内ソート索引〔後勝ち〕、base+overlay マージ参照、周期コンパクションで
    原子置換。M36 自動取得語の注入経路。spec §4.6）
  - overlay の同時実行（プロセス内 `shared_mutex` + プロセス間 `LockFileEx`、`op_count` 最後更新の
    クラッシュ安全 append、rename 原子置換、reader の `generation` 追従・lock-free 読み。spec §4.7）
  - 設定キー 7 種（`inlineEnglishCandidates` / `inlineEnglishCaseVariants` /
    `fullWidthEnglishCandidate` / `inlineEnglishMinLength` / `inlineEnglishDictionary` /
    `inlineEnglishPromoteThreshold` / `inlineEnglishDictionaryPath`）
- **受け入れ条件**:
  - `inlineEnglishCandidates=true` で、Japanese モードのまま `apple` を打つと候補列に
    `apple` / `Apple` が現れ、選択すると英数モード切替なしで英単語が確定する
  - `inlineEnglishCaseVariants` / `fullWidthEnglishCandidate` の ON/OFF で対応する
    バリアント候補が増減する
  - `inlineEnglishMinLength` 未満の生ローマ字では英単語候補を出さない
  - 弱シグナル（例: `ko`）では英単語候補が第一候補を奪わない／自動選択されない
  - 英単語確定で reading=生ローマ字として学習され、再度同じローマ字で英単語候補が
    再提示される（かな漢字学習と混線しない）
  - `inlineEnglishCandidates=false` で英単語候補が一切出ず、従来の候補生成・rerank・確定が
    一切変わらない
  - 実機 Win11 での end-to-end 確認（`gate:human-required`）
- **参照仕様**: `docs/inline-english-candidate-spec.md`

### M61: 自動カッコペアリング（Bracket Auto-Pairing）

> 開きカッコ（`（` `「` `[` `{` `"` など）を打鍵したとき、iPhone の日本語入力
> キーボードのように対応する閉じカッコを自動補完し、**カーソルをカッコの内側に置く**
> 追加機能。閉じカッコの飛び越え（スキップ）・空ペアの一括削除を含む。既定 OFF・後方
> 互換で、有効化時のみ動作する。**TIP ローカルかつ決定的で、IPC・推論ホストに依存しない**
> （M58/M59/M60 と異なる固有性質）。M61-A（コア）→ M61-B（外部化・アプリ互換・拡張）の
> 段階構成。正典仕様は `docs/bracket-pairing-spec.md`。

#### M61-A: ペアリングコア

- **目的**: 開きカッコ打鍵で対を自動挿入してカーソルを内側へ置き、閉じカッコの飛び越えと
  空ペアの Backspace 一括削除を、英数モードを含めて実現する最小フローを作る。
- **前提**: M13（InputState 状態機械）完了。確定 + カーソル配置は既存 M5/M6 の commit
  経路（`SetText` → `Collapse` → `SetSelection`）を再利用するため追加の前提なし。
- **推奨実装時期**: M13 完了直後、Phase 5 と並行可能な独立トラック。Zenzai・TSF 深耕・
  パッケージングに依存しない小規模・無 IPC 機能。設定 UI（M30）完成までは
  `%LOCALAPPDATA%\azooKey\config\settings.json` を TIP がローカル読み（手編集 / 環境変数で補う。
  host CLI 経由にしない。§6.1）。
- **変更対象**: `core/include/azookey/core/InputState.h` / 状態機械（開き・閉じカッコ
  codepoint の分類と新 ClientAction `insertBracketPair` / `skipOverClosing` /
  `deleteBracketPair`、純粋 core 用 `EditContextHint`〔隣接文字を TIP から渡す。§3.1.1〕。
  新 `UserAction` enum 値は追加しない）、
  `core/src/UserActionMap.cpp` / `tsf-tip/src/TextService.cpp::OnKeyDown` 内テーブル
  （ブラケット/記号を生む VK〔`VK_OEM_4`/`VK_OEM_6` 等・JIS の `「」` キー〕を
  `Input`/`InputAlnum` へ写し、入力モード/`ToUnicode` から codepoint を解決して載せる
  VK→UserAction 表の拡張。新 enum 値なし・純粋追加。§3.1）、
  `core/src/BracketTable.cpp`（新規・組み込み対応表）、
  `tsf-tip/src/TextService.cpp`（`OnTestKeyDown` / `OnKeyDown` のカッコ・Backspace 分岐、
  隣接文字の同期読取→`EditContextHint` 構築→純粋 core 呼び出し、`ApplyClientAction` の
  カーソル内側配置・空ペア削除〔空ペア確認時のみ Backspace を eaten、それ以外はアプリへ
  パススルー。§5.3〕、**TIP ローカル設定読み取り**〔`config\settings.json` を in-proc で読む +
  `ReadDirectoryChangesW` ホットリロード。Host 非依存。§6.1〕）、
  `settings/mvp-settings.schema.json`（設定キー 5 種）。
- **実装範囲**: `docs/bracket-pairing-spec.md` §3〜§6・§8。
  - VK→UserAction 表の拡張: ブラケット/記号 VK を `Input`/`InputAlnum` へ写し codepoint を
    入力モード/`ToUnicode` から解決（M13 §1.4 は A〜Z のみのため OEM キー追加が必須。§3.1）
  - 純粋 core の維持: 隣接文字は TIP が §5.3 で読んで `EditContextHint` に詰め、
    `HandleEvent(event, hint)` へ渡す。core は文書を直接見ず hint から分岐（テスト可能。§3.1.1）
  - TIP ローカル設定読み取り: `bracketPairing` ほか M61 設定を TIP が正典
    `%LOCALAPPDATA%\azooKey\config\settings.json` から in-proc で読み、Host 非依存で `OnKeyDown`
    判定に使う（設定 UI / host が書くのと同一ファイル。host 側 SettingsStore 経由にしない。§6.1）
  - Backspace は**空ペア確認時のみ** TIP が eaten して `deleteBracketPair`、それ以外はアプリへ
    パススルーし通常削除の挙動を変えない（§5.3・§4.3）
  - 組み込みカッコ対応表（全角 + 半角の非対称ペア。対称デリミタ・`<>` は既定除外）
  - immediate トリガ（既定）の対挿入とカーソル内側配置（§5.2）。`composition` トリガは
    設定で選択可能にする（§4.0.1）。**範囲選択中の開きカッコは（M61-A は wrap OFF 固定のため）
    開きカッコ 1 文字で選択を置換するリテラル挿入とし、ペア化・カーソル内側化しない**（§3.3・§4.8）
  - 閉じカッコのスキップ（カーソル右 1 文字読取で飛び越え判定。§4.2）
  - 空ペアの Backspace 一括削除（カーソル左右読取で判定。§4.3）
  - 英数モード（`alnum_half` / `alnum_full`）でのペアリング（§4.4）
  - 隣接文字の同期読取 EditSession と、拒否時のリテラル挿入 / 通常 Backspace
    フォールバック（§4.8・§5.3）。`OnTestKeyDown` の eaten 宣言（アプリ素通し防止）
  - 設定キー 5 種（`bracketPairing` / `bracketPairingTrigger` / `bracketSkipOverClosing` /
    `bracketBackspaceDeletesPair` / `bracketPairingInAlnumMode`）
- **受け入れ条件**:
  - `bracketPairing=true` で、`「` を打つと `「」` が入りカーソルが内側に来る。続けて
    入力するとカッコ内に入る（immediate、Enter 不要）
  - `「あ」` の `」` 直前で `」` を打つと二重化せずカーソルが `」` の外へ進む（スキップ）
  - 空ペア `「|」` の内側で Backspace を押すと開き・閉じが両方消える
  - `hiragana` と `alnum_half` の双方でペアリングが働く（`bracketPairingInAlnumMode=false`
    で英数モードのみリテラル挿入になる）
  - `bracketPairingTrigger=composition` で `「` 打鍵時に preedit に `「」` が出て、確定操作で
    カーソル内側に確定する。Esc で破棄される
  - 読取 EditSession 拒否・選択取得失敗時にリテラル挿入 / 通常 Backspace へフォールバックし、
    入力を失わない
  - `bracketPairing=false` でカッコ・Backspace・確定・候補・学習の挙動が一切変わらない
  - IPC メッセージを一切追加せず、Host 未接続でも本機能が動作する（TIP が正典
    `config\settings.json` をローカル読みして `bracketPairing` を判定。host 側 SettingsStore 非依存。§6.1）
  - `bracketPairing=true` でも空ペアでない Backspace はアプリへパススルーされ、通常削除・
    read-only 欄・Undo の挙動が変わらない（§5.3）
  - core `HandleEvent` が純粋関数のまま `EditContextHint` 注入で全分岐をテストできる（§3.1.1）
  - 実機 Win11 での end-to-end 確認（`gate:human-required`）
- **参照仕様**: `docs/bracket-pairing-spec.md`

#### M61-B: 外部化・アプリ互換・拡張

- **目的**: カッコ対応表の TSV 外部化（カッコ対専用）、自動ペアするエディタでの二重化回避
  （per-app 制御。アプリリストは `bracketPairingApps` / M48 プロファイル）、対称デリミタ・
  選択囲みなどの拡張挙動を追加する。
- **前提**: M61-A 完了。per-app 制御は M48（アプリ別入力プロファイル）に統合する
  （`docs/app-profile-spec.md`）。M48 未完了時は本機能専用の最小リスト設定で先行可能。
- **変更対象**: `core/src/BracketTable.cpp`（TSV パース / マージ / ホットリロード。M17
  基盤再利用）、`tsf-tip/src/TextService.cpp`（前面アプリ判定 = `promptPrefixByApp` 基盤
  再利用・選択囲み・対称デリミタの語境界判定）、M48 プロファイル連携 +
  `docs/app-profile-spec.md` 更新（`profilesByApp` プロファイル〔`additionalProperties:false`〕へ
  `bracketPairing`〔`auto`/`on`/`off`、既定 `auto`〕フィールドを追加。spec §4.5.0）、
  `settings/mvp-settings.schema.json`（設定キー 4 種）。
- **実装範囲**: `docs/bracket-pairing-spec.md` §4.1.1・§4.5・§4.5.0・§4.5.1・§4.9。
  - カッコ対応表（**カッコ対専用**）の TSV 外部化（`bracket-pairs.tsv`: `open`/`close`/`flags`。
    組み込み既定を `open` キーで上書き・追加、`off` で無効化。M17 ホットリロード基盤再利用。
    **アプリ名〔プロセス名〕はこの TSV に書かない**。spec §4.5.1）
  - per-app 有効範囲（`bracketPairingAppPolicy` = denylist（既定）/ allowlist。アプリリストは
    プロセス名配列 `bracketPairingApps`（**カッコ対 TSV とは別スキーマ**）+ 組み込み既定 denylist
    シード〔VS Code / Visual Studio / JetBrains 系等〕。M48 プロファイルがあれば優先。spec §4.5.0）
  - M48 プロファイルスキーマ拡張（`profilesByApp` 各プロファイルへ `bracketPairing` enum
    `auto`/`on`/`off` を追加。`additionalProperties:false` のため明示追加が必須。`docs/app-profile-spec.md`
    §4.1 を更新。spec §4.5.0）。**マスタートグル `bracketPairing` が最優先**で、false なら
    プロファイル `on` でも有効化しない（評価順: マスター → per-app。§4.5.0）
  - 対称デリミタ（`"` `'` `` ` ``）の語境界判定付きペアリング（`bracketSymmetricQuotePairing`、既定 OFF）
  - 範囲選択中の開きカッコで選択を囲む（`bracketWrapSelection`、既定 OFF）
  - 設定キー 5 種（`bracketSymmetricQuotePairing` / `bracketWrapSelection` /
    `bracketPairingAppPolicy` / `bracketPairingApps` / `bracketPairsPath`）
- **受け入れ条件**:
  - `bracket-pairs.tsv` でカッコ対の追加・上書き・`off` 無効化ができ、保存で次の入力から
    反映される（ホットリロード）。不正行は warning でスキップ
  - denylist 掲載アプリ（VS Code 等）でペアリングが抑制され二重化しない。allowlist ポリシーで
    掲載アプリのみ有効になる
  - `bracketSymmetricQuotePairing=true` で語境界の `"` がペアになり、語の途中では単一挿入になる
  - `bracketWrapSelection=true` で範囲選択中の開きカッコが選択を囲む
  - 実機 Win11 での end-to-end 確認（`gate:human-required`）
- **参照仕様**: `docs/bracket-pairing-spec.md`

### M62: 候補リライター層（数字・記号・半角カタカナ・英字・絵文字の異表記候補生成）

> 確定済み候補（または読み）から、数字の各種表記（漢数字・大字・ローマ数字・丸数字・
> 16/8/2 進数）、記号の関連候補、半角/全角カタカナ、英字の大小・全半角、絵文字を
> **自動派生して候補列へ注入する**追加機能。現状 `core/` `tsf-tip/` `inference-host/` に
> 該当機能は存在しない（真のギャップ。`docs/rich-features-spec.md` は richness X-1〜X-3 を
> 定めるが、数字/記号/絵文字の異表記展開リライターは未定義）。参考実装は karukan
> （`karukan-engine/src/rewriter/`、MIT OR Apache-2.0）。**ロジック（アルゴリズム）と
> データ（Mozc 由来）を分離**して扱い、karukan のコードは逐語コピーせず設計移植する。
> M62-A（数字）→ M62-B（半角カタカナ/英字）→ M62-C（記号）→ M62-D（絵文字）の段階構成。
> 既定 OFF・後方互換。正典仕様は新設予定の `docs/candidate-rewriter-spec.md`。
>
> ライセンス方針: M62-A/B はアルゴリズムのみで Mozc 由来データに依存しない（最も安全）。
> M62-C/D が Mozc 由来データ（`symbol.tsv` / `emoji_data.tsv`、BSD 3-Clause © Google）+
> Unicode CLDR を使う場合、各データの著作権表記保持と `THIRD_PARTY_LICENSES` 集約ファイルの
> 新設が前提（横断依存。azooKey には現状この集約ファイルが不在）。

#### M62-A: 数字リライター（コア・最小）

- **目的**: 読みが純十進数字のとき、漢数字・大字（壱弐参）・ローマ数字（ⅻ/Ⅻ）・丸数字（⑫）・
  16/8/2 進数（`0x7b` / `0173` / `0b1111011`）の異表記候補を生成し、各候補に和文注釈
  （「漢数字」「大字」「16進数」等）を付けて候補列へ注入する。
- **前提**: M5/M6（候補 UI・確定）と M13（InputState / ClientAction）完了。辞書・推論・IPC に
  依存しない決定的処理で、M61 と同じ **TIP ローカル・無 IPC・Host 非依存**モデルに乗る。
- **変更対象**: `core/include/azookey/core/NumberRewriter.h`・`core/src/NumberRewriter.cpp`（新規。
  漢数字/大字/ローマ/丸数字テーブルを自前定義。Mozc 由来データ非依存）、
  `core/include/azookey/core/Candidate.h`（候補注釈 description フィールド追加）、
  `tsf-tip/src/TextService.cpp`（候補列への注入・dedup、および**注釈表示のための候補ウィンドウ
  view-model 拡張**。現状 `TextService.cpp:1654-1655` は `candidate.surface` のみで候補ウィンドウを
  構築するため、TIP ローカルの注釈付き候補型を導入し surface とは別に description を保持・表示する。
  確定文字列には注釈を畳み込まない。詳細は下記「M62 横断依存」の候補注釈伝送を参照）、
  `settings/mvp-settings.schema.json`（`numberRewriter`、既定 OFF）、
  `core/tests/number_rewriter_test.cpp`（新規）。
- **受け入れ条件**:
  - `numberRewriter=true` で `123` の変換候補に `百二十三`/`壱百弐拾参`/`0x7b`/`0b1111011`
    等が注釈付きで出る（丸数字・ローマ数字は範囲内の入力のみ。例: `12`→`⑫`/`ⅻ`）
  - 範囲外（丸数字 51 超等）の表記はスキップし、巨大数で 16 進等を破綻させない
  - `20世紀` のような数字混在は素通し（リライトしない）
  - `numberRewriter=false` で候補・確定・学習の挙動が一切変わらない
  - `core` の純粋関数として全分岐を単体テストできる（TSF/IPC/モデル非起動）
- **参照仕様**: `docs/candidate-rewriter-spec.md`（新設）

#### M62-B: 半角カタカナ・英字リライター

- **目的**: 純かな候補から全角/半角カタカナ、ASCII/全角英字から半角小・半角大・全角小・全角大の
  各バリアントを生成する。英字部分は M60（インライン英単語候補）と機能重複するため **M60 設計へ
  統合**し、独立実装にしない。
- **前提**: M62-A 完了。英字部分は M60（`docs/inline-english-candidate-spec.md`）に統合。
  いずれも決定的・Mozc データ非依存で TIP ローカル可。
- **変更対象**: `core/`（HalfKatakana / Alphabet リライター）、M60 設計への統合、
  `settings/mvp-settings.schema.json`、`core/tests/`。
- **受け入れ条件**:
  - 純かな候補に全/半角カタカナ候補が注釈付きで出る。漢字混在（`愛してる`）はリライトしない
  - 英字の幅・大小バリアントが M60 の候補注入経路に統合され二重実装しない
  - 既定 OFF で既存挙動不変
- **参照仕様**: `docs/candidate-rewriter-spec.md` / `docs/inline-english-candidate-spec.md`

#### M62-C: 記号リライター（Mozc 由来データ）

- **目的**: 記号の関連候補チェーン（`「`→`『【〔（`）と、かな読み→記号（`かぎかっこ`→`「」`）を
  生成する。データは Mozc `symbol.tsv` 由来。
- **前提**: M62-A 完了 + **`THIRD_PARTY_LICENSES` 集約ファイル新設**（Mozc BSD-3 表記保持）。
  データ駆動かつ大きくなりうるため、かな読み引きは **inference-host 側**（別プロセス）で
  `QueryCandidates` 応答に混ぜ、in-proc TIP に大データを抱えない。variant chain（小データ）は
  TIP ローカル可。
- **変更対象**: `core/`（SymbolRewriter ロジック）、記号データの再ポート（Mozc 原典準拠で
  azooKey 形式へ。逐語コピーせず）、`inference-host/`（読み引き経路）、`THIRD_PARTY_LICENSES`、
  `docs/candidate-rewriter-spec.md`。
- **受け入れ条件**:
  - `「`→`『【〔（` の関連候補、`かぎかっこ`→`「」` の読み引きが出る
  - データ欠損時にフォールバックして既存候補が壊れない
  - Mozc 由来データの BSD-3 表記が `THIRD_PARTY_LICENSES` とデータヘッダに保持される
- **参照仕様**: `docs/candidate-rewriter-spec.md`

#### M62-D: 絵文字リライター（Mozc + CLDR 由来データ）

- **目的**: かな読み→絵文字（`わらい`→`😄`）と Slack 風 `:trigger` 曖昧検索（`:smile`→`😄`）を
  生成する。データは Mozc `emoji_data.tsv` + Unicode CLDR 由来。
- **前提**: M62-C 完了 + `THIRD_PARTY_LICENSES` に Mozc BSD-3 + CLDR 表記。大規模データ
  （数万行）のため Host 側配信が前提。候補窓 UX（注釈・確定動線）は M5/M14/M15 に合わせ再設計。
- **変更対象**: `core/`（EmojiRewriter ロジック・`:trigger` 曖昧検索スコアリング）、絵文字データ
  再ポート、`inference-host/`、`THIRD_PARTY_LICENSES`、`docs/candidate-rewriter-spec.md`。
- **受け入れ条件**:
  - かな読み引きと `:trigger` 曖昧検索が機能し、ランキングが妥当
  - 既定 OFF で既存挙動不変。Mozc / CLDR 表記保持
- **参照仕様**: `docs/candidate-rewriter-spec.md`

#### M62 横断依存・既知のテストギャップ

- **横断依存**: `THIRD_PARTY_LICENSES` 集約ファイル新設（M62-C/D と M53 辞書強化の共通前提）。
  `docs/candidate-rewriter-spec.md`（IPC payload・データ形式・ライセンス・TIP/Host 責務境界・
  確定時の学習 reading 扱いの正典）新設。
- **候補注釈の伝送（必須）**: リライタ候補は注釈（description）付きで表示するため、候補表示経路の
  拡張が前提。現状 `ipc::CandidateField`（`ipc/include/azookey/ipc/Payloads.h:52-57`）は
  `surface`/`reading`/`score`/`source` のみ、候補ウィンドウは `candidate.surface` のみで構築
  （`tsf-tip/src/TextService.cpp:1654-1655`）。よって (a) TIP ローカルリライタ（M62-A/B）は
  **TIP 内の注釈付き候補型 + 候補ウィンドウ view-model** で description を保持・表示し、
  (b) Host 側データ駆動リライタ（M62-C/D）は **`ipc::CandidateField` に description フィールドを
  追加**して伝送する。いずれも確定文字列には注釈を畳み込まない。正典は `docs/candidate-rewriter-spec.md`。
- **既知のテストギャップ**: `core/tests/number_rewriter_test.cpp` 等の純粋関数テスト未作成
  （karukan の `rewriter/number.rs` 等のテストを**期待値表として**移植する。逐語コピーしない）。
  記号/絵文字のデータ駆動リライトは再ポート出力に対する round-trip テストが必要。
- **リスク**: Mozc 由来データ（M62-C/D）の取り込みは BSD-3 / CLDR 表記義務を新規に背負う。
  数字（M62-A）はデータ非依存で最もリスクが低く、ここから着手する。

## 開発基盤・品質強化トラック（M37〜M43 + M44/M47/M50/M51）

> Phase 5〜7 の番号体系とは独立した、開発基盤・品質の負債解消トラック。
> **正典仕様**は `docs/dev-infrastructure-spec.md`。
> 第三者レビュー 2 通の指摘を評価・取捨選択した結果をマイルストーン化した
> もので、Phase 3（Zenzai 統合）/ Phase 4（配布）着手前の基盤固めにあたる。
> M 番号は通し連番（既存最終 M36-B の続き）。spec から参照されるため
> M 番号・Phase グルーピングは変更しない。
>
> M44（診断ウィザード）・M47（復旧 UX）・M50（互換性テスト）・M51（trace
> 内訳）は本トラックの自然な延長として、M41（構造化ログ）・M42（Host 再接続）
> ・M38（CI）の完了後に追加で取り組む。M44/M50 は配布前（Phase 4 ゲート）に
> 投入することでサポートコストを大きく下げられる。

### M37: ビルド再現性

- **目的**: 手元・CI・AI エージェントでビルド入口を統一し、コンパイル
  オプションを一元管理する。
- **前提**: なし（独立トラック。Phase 3 着手前の実施を推奨）。
- **変更対象**: `CMakePresets.json`（新規）、ルート `CMakeLists.txt`、
  各 `*/CMakeLists.txt`、`.clang-format`（新規）、`.gitignore`。
- **実装範囲**: `docs/dev-infrastructure-spec.md` §2。
  - `CMakePresets.json`（`windows-debug` / `windows-release`）
  - `azookey_project_options` / `azookey_project_warnings` の INTERFACE
    target 化と各 target への適用（`/W4` は段階導入）
  - `.clang-format`（Google ベース）追加。全体整形は独立 PR に分離
  - `.gitignore` に Windows/CMake エントリ追加
- **受け入れ条件**:
  - `cmake --preset windows-debug` / `windows-release` の
    configure→build→test が成功する
  - 既存全 target が共通オプション/警告 target をリンクしてビルドできる
  - `.clang-format` がルートに存在し新規コードに差分が出ない
  - ビルド生成物が `git status` に現れない
- **参照仕様**: `docs/dev-infrastructure-spec.md` §2, §3

### M38: CI 品質ゲート拡張

- **目的**: Debug/Release 両構成・preset 利用・移植性チェックを CI に加え、
  品質ゲートを強化する。
- **前提**: M37 完了（preset / clang-format）。
- **変更対象**: `.github/workflows/windows.yml`、Linux 補助ワークフロー。
- **実装範囲**: `docs/dev-infrastructure-spec.md` §4。
  - Debug/Release マトリクス、runner を `windows-2022` に明示
  - CI の configure/build/test を preset 経由に統一
  - `clang-format --dry-run --Werror`（全体整形 PR 後に有効化）
  - Linux 補助ジョブ（非 Windows 依存部分のみビルド・テスト）
  - bench smoke の CTest 実行、artifact 整理
- **受け入れ条件**:
  - Debug / Release 両構成が CI で緑
  - Linux 補助ジョブが非 Windows 部分のテストを実行する
  - bench smoke が CTest 経由で exit=0
  - PR コメントが config ごとの結果に対応する
- **参照仕様**: `docs/dev-infrastructure-spec.md` §4

### M39: ユーザーデータ永続化の堅牢化

- **目的**: 学習・辞書ファイルの保存先を実行ディレクトリ依存から脱却させ、
  原子的書き込みで破損を防ぐ。
- **前提**: なし（独立トラック。Phase 3 着手前の実施を推奨）。
- **変更対象**: `inference-host/src/main.cpp`、
  `inference-host/src/`（パス解決ロジック新規）、
  `learning/src/LearningStore.cpp`・`UserDictionary.cpp`（原子的書き込み）。
- **実装範囲**: `docs/dev-infrastructure-spec.md` §5。
  - 既定保存先を `%LOCALAPPDATA%\azooKey\{config,data,logs,models}\` に
  - `SHGetKnownFolderPath` でのパス取得、サブディレクトリ自動作成
  - `--learning` / `--user-dict` 明示指定を優先するパス解決規約
  - 一時ファイル → rename による原子的書き込み
  - 保存先決定ロジックの unit test
- **受け入れ条件**:
  - 明示指定なしで `%LOCALAPPDATA%\azooKey\data\` 配下が使われる
  - `--learning` / `--user-dict` 指定時は指定パスが優先される
  - 必要なディレクトリが自動作成される
  - 書き込み中クラッシュで既存ファイルが壊れない
  - 保存先決定ロジックの unit test が緑、既存テストが回帰しない
- **参照仕様**: `docs/dev-infrastructure-spec.md` §5

### M40: IPC/JSON 堅牢化

- **目的**: 自前 JSON パーサと Named Pipe 入力の境界堅牢性を上げ、IPC
  境界での事故を減らす。
- **前提**: なし（独立トラック。Phase 3/4 と並行可能）。
- **変更対象**: `ipc/src/Json.cpp`、`ipc/src/Payloads.cpp`、
  `ipc/src/NamedPipeTransport.cpp`、`ipc/tests/`。
- **実装範囲**: `docs/dev-infrastructure-spec.md` §6。
  - JSON: ネスト深度上限（64）・最大入力長（1 MiB）・サロゲートペア結合・
    不正 UTF-8/制御文字拒否・末尾ゴミ拒否・巨大数の安全な拒否。数値 codec の
    correctness（locale 非依存 / uint64 全域の双方向 round-trip /
    非 plain 数値形の桁あふれ拒否）
  - malformed/fuzz テスト追加（決定的境界コーパス + 有界擬似乱数スモーク。
    libFuzzer は任意拡張）
  - 未配線 MessageType への明示エラー応答 + 列挙↔codec 整合検査
    （client ハング防止）
  - Named Pipe: Release で SID 取得失敗時 fail-closed、接続インスタンス
    上限（32）、最大フレームサイズ 1 MiB 固定、Handshake トークン
    （per-user ファイル `%LOCALAPPDATA%\azooKey\config\ipc-token` 配布 +
    env 上書き）、client cleanup、overlapped accept と `Stop()` 時の
    pending accept cancel
- **受け入れ条件**:
  - 既存 `ipc_payloads_tests` / `ipc_named_pipe_transport_tests` が緑
  - malformed JSON・ランダムバイト列でクラッシュしない
  - ネスト深度・最大長超過を拒否する
  - サロゲートペアを正しく結合し、単独サロゲートを拒否する
  - Release ビルドで SID 取得失敗時に Host 起動が失敗する
  - 複数接続・切断テストが追加され緑
  - pending accept 中の `Stop()` が無期限に待たない
  - 切断済み client が解放される
  - 未配線 MessageType に明示エラー応答が返り blocking client がハングしない
  - 数値 codec が locale 非依存で round-trip する
  - uint64 フィールド（`request_id` / `Ping` / `Cancel.target_request_id` /
    `CommitObservation.timestamp_ms`）が 2^53 超でも丸めず全域 round-trip する
  - 非 plain 数値形の桁あふれ（例 `18446744073709551616.0`）を `nullopt` で拒否する
  - Handshake トークンが per-user ファイルチャネルで配布される
- **参照仕様**: `docs/dev-infrastructure-spec.md` §6

### M41: 構造化ログと可観測性

- **目的**: 遅延要因の切り分けとエラー分類を可能にする構造化ログ基盤を
  整える。
- **前提**: M39 完了（ログ出力先 `%LOCALAPPDATA%\azooKey\logs\`）。
- **変更対象**: `core/` または各モジュールの軽量 JSON Lines ロガー
  （新規）、`tsf-tip/src/TextService.cpp`、`inference-host/src/`、
  `ipc/src/`。
- **実装範囲**: `docs/dev-infrastructure-spec.md` §7。
  - JSON Lines ログ（`ts`/`level`/`component`/`request_id`/`phase`/
    `latency_ms`/`result`/`error_code`）
  - 相関 ID（`request_id` は TIP 採番。`QueryCandidates`/staleness は `ipc_pending_id_`、
    送信キュー〔`CommitObservation`〔応答あり〕/ `Cancel`〔応答なし〕〕は接続ローカル連番。
    `trace_id` と組で相関。詳細は `docs/dev-infrastructure-spec.md` §7.3）とフェーズ別レイテンシ
  - エラーコード体系 enum（transport / protocol / business）
  - タイムアウト規約（ソフト/ハード）
  - 入力本文・候補語のログ redaction は §7.6 の優先順位に従う。本文出力は
    `Debug ∧ AZOOKEY_LOG_BODY=1 ∧ ¬secure ∧ DetailedLoggingAllowed()` のときのみ
    （`privacy.redactLogs` 既定 `true`。単に Debug というだけでは出さない）
- **受け入れ条件**:
  - TIP / Host が JSON Lines ログを所定ディレクトリに出力する
  - 各行に `request_id` / `phase` / `latency_ms` / `result` が含まれる
  - エラーコードが 3 カテゴリ enum で固定される
  - Release ビルドで入力本文・候補語がログに出力されない
- **参照仕様**: `docs/dev-infrastructure-spec.md` §7

### M42: Host 可用性・再接続

- **目的**: Host 停止・無応答時も入力が止まらないよう、TIP 側の再接続と
  劣化モードを実装する。
- **前提**: M41 完了（状態遷移ログ）。
- **変更対象**: `tsf-tip/src/TextService.cpp`（`IpcWorkerThread`）、
  `ipc/`（`Health` 利用）。
- **実装範囲**: `docs/dev-infrastructure-spec.md` §8。
  - 接続状態機械（Disconnected/Connecting/Handshaking/Ready/Degraded）
  - exponential backoff + jitter による再接続
  - ヘルス監視（`Health` メッセージ流用）
  - 無応答時の劣化モード（`SimpleConverter` 相当のローカルフォールバック）
- **受け入れ条件**:
  - Host 停止 → 再起動で TIP が自動再接続する
  - Host 無応答時に TIP が劣化モードへ移行し入力が止まらない
  - 状態遷移がログに記録され、Host 復帰後に `Ready` へ復帰する
- **参照仕様**: `docs/dev-infrastructure-spec.md` §8

### M43: WIL 段階導入

- **目的**: HANDLE / COM / レジストリ / `HRESULT` の管理を WIL の RAII で
  安全化する。
- **前提**: M37 完了（依存導入手段）。M40 / M39 と並行で対象ファイルを
  WIL 化すると効率的。
- **変更対象**: 依存取り込み（submodule または `FetchContent`）、
  `tsf-tip/src/`、`ipc/src/NamedPipeTransport.cpp`。
- **実装範囲**: `docs/dev-infrastructure-spec.md` §9。
  - WIL を header-only 依存として取り込み（vcpkg は使わない）
  - 新規コードは WIL の RAII 型を使用
  - `tsf-tip` の COM / HANDLE / レジストリ処理を段階的に RAII 化
- **受け入れ条件**:
  - WIL の取り込みがオフラインビルドを壊さない
  - 新規コードが WIL の RAII 型を使用する
  - 既存テストが回帰しない
- **参照仕様**: `docs/dev-infrastructure-spec.md` §9

### M44: IME 診断・修復ウィザード

- **目的**: TIP / Host / IPC / モデル / 学習 / 設定の状態をユーザー自身が
  切り分けられるようにし、配布前のサポートコストを下げる。GitHub Issue /
  Discord 報告用の診断 ZIP 生成も含む。
- **前提**: M40（IPC/JSON 堅牢化）完了、M41（構造化ログ）完了。
- **推奨実装時期**: Phase 4（v1.0 配布）ゲート前に投入。M11/M12 と並行可能。
  クラッシュ・登録不整合・モデル未配置などの「即詰み」シナリオを早期に
  ユーザーが自己解決できるようにする。
- **変更対象**: `diagnostics/`（新規ディレクトリ、`azookey_diag.cpp` CLI）、
  `ipc/src/Payloads.cpp`（`QueryDiagnostics` 追加）、
  `inference-host/src/Health.cpp`（状態詳細化）。Phase 4 ゲートでは
  CLI + 診断 ZIP までを範囲とし、`settings-app/` 診断タブは M30 完了後の
  follow-up タスクとして切り出す（M30 を M44 v1 の前提にはしない）。
- **実装範囲**: `docs/dev-infrastructure-spec.md` §12（本マイルストーンで
  追加）。
  - 診断項目 D-001〜D-015（TIP DLL 存在 / COM 登録 / 言語プロファイル /
    Host 起動 / IPC Handshake / IPC Ping / モデルパス / モデル検証 /
    fallback 状態 / learning store / user dict / settings / logs / DPAPI /
    app compatibility）
  - `azookey_diag.exe --json` / `--repair` / `--collect` の 3 サブコマンド
  - 診断 ZIP（`diag.json` + `settings.redacted.json` + `host-health.json` +
    `ipc-ping.json` + `logs/*.jsonl` + `environment.txt` +
    `crash-summary.txt`）
  - 機密除去: OpenAI API key、入力本文、Magic prompt、学習 TSV 中身を
    含めない（件数 / サイズ / hash / mtime のみ記録）
- **受け入れ条件**:
  - クリーン環境で全項目チェックが実行できる
  - Host 未起動でも診断アプリがクラッシュしない
  - Zenzai 有効でモデル未選択時に `warning` として fallback 状態を表示する
    （設定済みパスが不在の場合は `error`。spec §12.2.1）
  - 各診断項目が判定基準（spec §12.2.1）どおりに ok / warning / error を返す
    （空ストア・未設定 optional 機密は誤検知させない）
  - 診断 ZIP から秘密情報が除去され、各メンバが redaction ルール
    （spec §12.5）どおり処理される
  - `--json` 出力が stable schema としてテストされる
  - `--repair` で D-001 / D-002 / D-003 / D-013 の自動修復が動く（spec §12.2.1）
- **参照仕様**: `docs/dev-infrastructure-spec.md` §12

### M47: Host / Zenzai 障害時の自動復旧 UX

- **目的**: Host 停止・IPC 切断・Zenzai ロード失敗・推論 timeout が
  発生しても最低限の入力を継続できるよう、ユーザー可視な状態機械と
  UI 通知を実装する。**M42（transport 層の再接続）の上に乗るユーザー
  体験レイヤ**として位置づける。
- **前提**: **M42 完了**（再接続・劣化モードの基盤）。M41 の構造化ログを
  状態遷移のトレースに使う。
- **推奨実装時期**: M42 完了直後。Phase 5/6/7 と並行可能。
- **変更対象**: `inference-host/src/HealthStateMachine.cpp`（新規）、
  `tsf-tip/src/TextService.cpp`（候補ウィンドウ下部の控えめ UI 通知、
  および SafeMode 入り時の TIP 内通知バナー）。`settings-app/` への
  SafeMode 通知タイル統合は M30 完了後の follow-up とし、M47 v1 は
  TIP 単体で完結させる（M30 を M47 の前提にはしない）。
- **実装範囲**: `docs/dev-infrastructure-spec.md` §8 拡張。
  - 状態機械 5 種（`Healthy` / `DegradedSimple` / `DegradedModel` /
    `Recovering` / `SafeMode`）
  - 各処理の timeout: Ping 500ms / QueryCandidates fast 150ms /
    QueryLiveConversion 80ms / Heavy 800ms / Model load 30s
  - Host process / pipe 接続が生きていても有効応答が返らない
    connected-but-silent 状態を timeout として扱い、pipe 切断を待たず
    `DegradedSimple` へ遷移する
  - timeout 時の Cancel + staleness check による古い結果破棄
  - Cancel / deadline を Host Dispatcher から converter / reranker /
    backend 推論処理まで伝播し、応答抑止だけに依存しない
  - 連続クラッシュ N 回で `SafeMode` 突入、次回起動時にユーザー通知
  - UI: `⚠️ Zenzai が応答しないため、簡易変換で継続しています [詳細]
    [再試行]` を候補ウィンドウ下部の控えめインジケータで表示
- **受け入れ条件**:
  - Host を手動 kill しても入力中のアプリが固まらない
  - Host が接続済みのまま `QueryCandidates` に応答しない場合でも、
    `QueryCandidates fast` timeout 後に簡易変換へ劣化し、次の入力を処理できる
  - Host 再起動後に自動復帰する
  - Zenzai ロード失敗時に fallback 状態が UI に明示される
  - 連続クラッシュ時は SafeMode に入り、次回起動時に通知する
- **参照仕様**: `docs/dev-infrastructure-spec.md` §8

### M50: アプリ互換性テストハーネス

- **目的**: 主要アプリで TSF composition / 候補ウィンドウ位置 / 確定 /
  キャンセル / Unicode / 絵文字 / Undo / Redo が壊れないことを半自動で
  検証する。
- **前提**: M38（CI 品質ゲート拡張）完了。
- **推奨実装時期**: Phase 4 ゲート前または直後。M28（MSIX サイドロード）
  着手前に最低限のアプリ互換性ベースラインを確保する。
- **変更対象**: `compat-test/`（新規ディレクトリ）、`.github/workflows/`
  （compat ジョブ追加、optional）。
- **実装範囲**: `docs/dev-infrastructure-spec.md` §13（本マイルストーンで
  追加）。
  - 対象アプリ: Notepad / WordPad / Edge / Chrome / Firefox / VS Code /
    Discord / Slack / Word / Excel / Outlook / Windows Terminal /
    Windows Settings
  - テストケース C-001〜C-012（`nihongo` Space-Enter / Backspace / ESC /
    候補位置 / マルチディスプレイ端 / DPI 150% / 絵文字 / Undo Redo /
    フォーカス移動 / Host kill 中 / ショートカット・修飾キー透過 /
    ローマ字 ja-ju-jo 系）
  - 実装方針: UI Automation + SendInput + screenshot による半自動テスト。
    完全自動化が難しい Office はチェックリスト + recorder で代替
    （詳細は spec §13.3.1）
  - 出力: `compat-report-YYYYMMDD/{report.md, report.json, screenshots/,
    logs/, failures/}`
- **受け入れ条件**:
  - Notepad / VS Code / Edge で C-001〜C-012 の自動テストが通る
  - Office（Word / Excel / Outlook）が手動チェックリスト（spec §13.3.1）で
    検証され、結果が `report.md` に記録される
  - 失敗時にスクリーンショットとログが保存される
  - report.json が CI artifact としてアップロードできる
- **参照仕様**: `docs/dev-infrastructure-spec.md` §13

### M51: レイテンシ内訳トレーサ

- **目的**: キー押下から候補表示までのどこに遅延があるかを 1 リクエスト
  単位で可視化し、Zenzai 最適化 / 回帰検出の基盤にする。
- **前提**: M41（構造化ログ）完了。
- **推奨実装時期**: M41 完了後、Zenzai 最適化（M24 / M25 / M57）着手前。
  M56（Tiny Reranker） / M57（ModernBERT）の効果測定の前提でもある。
- **変更対象**: `ipc/src/Messages.cpp`（envelope の既存 `trace_id` フィールドへ
  UUIDv7 を生成・伝播。wire format 変更ではない）、
  `tsf-tip/src/TextService.cpp` / `inference-host/src/Dispatcher.cpp` /
  `inference-host/src/InferenceEngine.cpp`（各フェーズで `latency_ms` 記録、
  絶対オフセットが要る場合のみ任意 `t_ms`）、
  `bench/live_bench.cpp`（既存 `azookey_bench` ターゲットに `--trace`
  フラグ追加）、`bench/azookey_trace_viewer.cpp`（新規 CLI）。
- **実装範囲**: `docs/dev-infrastructure-spec.md` §7 拡張。
  - phase 一覧: `key_down` / `romaji_convert` / `ipc_serialize` /
    `pipe_send` / `host_queue_wait` / `model_inference` / `rerank` /
    `pipe_recv` / `staleness_check` / `ui_apply` / `total`
  - JSONL 出力（既存 M41 ログに合流。`trace_id` で相関）
  - `azookey_trace_viewer trace.jsonl --summary` で p50/p95/p99 を出力
  - 通常利用時の overhead を抑えるため、詳細出力は `--trace` 明示有効化
    時のみ
- **受け入れ条件**:
  - 1 リクエスト単位で TIP / IPC / Host / UI の時間を追跡できる
  - p50 / p95 / p99 を出力できる
  - Zenzai backend 比較（cpu / cuda / directml / npu）に使える
  - 通常利用時の overhead が小さい
- **参照仕様**: `docs/dev-infrastructure-spec.md` §7

## プライバシー / モデル管理 / 学習データ UI トラック（M45/M46/M49）

> Phase 5/6/7 の既存 M に依存する付加機能トラック。Phase 連番に対し
> M45/M46/M49 は依存先の都合で番号が前後する（M46 は Phase 5 直後、M45 は
> M30 完了後、M49 は M34 完了後）。**正典仕様**は本トラック専用の
> `docs/model-management-spec.md` / `docs/privacy-and-secure-input-spec.md` /
> `docs/learning-data-management-spec.md` に分割する。
>
> M48（アプリ別入力プロファイル）は本トラックと密接だが、依存上は M46
> 完了後の追加機能トラック扱いとし、上記「追加機能マイルストーン」章に
> 配置する。

### M45: Zenzai モデル管理 UI

- **目的**: Zenzai モデル（R1=GGUF / R2=ONNX Runtime GenAI 変換モデル）の
  配置・検証・ロード・backend 選択・fallback 状態確認を GUI で行えるように
  する。M8（モデルロード境界）と M30（WinUI 3 設定アプリ）の上に乗る
  Phase 6-C 拡張。
- **前提**: M8 完了（`LoadModel` 境界）、M30 完了（設定アプリ）。
- **推奨実装時期**: M30 完了直後。M24（推論バックエンド選定 = R1 llama.cpp /
  R2 Windows ML）と並行着手すると backend 推奨ロジックの実装が捗る。
- **変更対象**: `settings-app/`（Model タブ追加）、`ipc/src/Payloads.cpp`
  （`ListModels` / `BenchmarkModel` 追加）、
  `inference-host/src/ModelCatalog.cpp`（新規）、
  `settings/mvp-settings.schema.json`（`model.*` ブロック追加）。
- **実装範囲**: `docs/model-management-spec.md`。
  - `%LOCALAPPDATA%\azooKey\models\` のスキャン。R1=`.gguf` ファイル
    （GGUF magic / version / metadata 検証、quantization 推定）と
    R2=ORT GenAI モデルディレクトリ（`genai_config.json` をパースし、その
    config が参照する ONNX の presence 検証。ファイル名はハードコードしない、
    §3.3）の両方式を検出する（§3.1）。zenz-v3 変換 ONNX の
    optional パッケージはここで discovery される
  - `ListModels`（`format` = `gguf` / `onnx_genai` を含む）/ `BenchmarkModel` IPC
  - backend 自動選択は M24 決定（`docs/copilot-pc-backend-spec.md` §4.3 / §4.5、
    R1=llama.cpp / R2=Windows ML）に委譲する。旧 `NPU > DirectML > CUDA > CPU`
    順は使わない。ONNX モデルがあり対応 HW なら §4.6 の EP 取得・登録を試みて
    R2（`winml`）、不可なら R1（`cuda` / `vulkan` / `cpu`）。バッテリ時は
    §4.5 / §4.6 に従い NPU device のみ（device-level フィルタ）。ベンチ履歴は
    同順位内のタイブレーカーとしてのみ p95 最良を採用
  - 既存 `backendPreference` との後方互換（`model.backendPreference` >
    root `backendPreference` > `auto`）。`directml` / `npu` は deprecated で
    内部的に `winml`（EP 自動選択）へ集約、ベンダ横断 GPU は `vulkan`
- **受け入れ条件**:
  - モデル一覧が GUI に表示される
  - invalid GGUF は「ロード不可」として明示される
  - ロード失敗時も Host が落ちず `SimpleConverter` fallback へ移行する
  - ベンチマーク結果（p50 / p95 / load_ms / rss_mb / vram_mb）が表示される
  - 選択モデルが Host 再起動後も自動ロードされる
- **参照仕様**: `docs/model-management-spec.md`

### M46: プライバシー / セーフ入力モード

- **目的**: 学習・予測・外部 AI・ログをユーザーが制御し、パスワード欄や
  機密入力時に自動で安全側に倒す。M16（Magic Conversion / OpenAI API）と
  M34（DPAPI）の前提として「ユーザーが AI/学習を停止できる」契約を確立する。
- **前提**: M7（学習）完了。M34 と並行・前倒し可能。M46 は M48 の前提でもある。
- **推奨実装時期**: Phase 5 内で **M16 着手前または同時期** に投入する
  （M16 Magic Conversion / OpenAI API は M46 の secure 抑止契約に依存する
  ため、M16 が先行すると secure アプリ向けの初期プライバシーギャップが
  発生する）。M34（DPAPI）とは並行で進められる。
- **変更対象**: `settings/mvp-settings.schema.json`（`privacy.*` ブロック
  追加）、`inference-host/src/PrivacyGate.cpp`（新規）、
  `inference-host/src/Dispatcher.cpp`（CommitObservation /
  QueryPredictions / Magic Conversion の抑止）、
  `tsf-tip/src/ForegroundAppDetector.cpp`（新規、M48 と共用）。
- **実装範囲**: `docs/privacy-and-secure-input-spec.md`。
  - モード 5 種（`normal` / `private` / `secure` / `offline` / `custom`）
  - `secureApps` 自動判定（KeePass.exe / 1Password.exe / Bitwarden.exe など）
  - secure 中の IPC 抑止契約（`CommitObservation` / `QueryPredictions` /
    Magic Conversion を送らない、`aiBackend=none` 強制）
  - ログ redaction（reading / surface を Release ログから除外）
  - 候補ウィンドウ下部の控えめインジケータ
- **受け入れ条件**:
  - `secureApps` 指定アプリで `LearningStore::Observe` が呼ばれない
  - secure 中は OpenAI API 呼び出しが発生しない
  - 構造化ログに入力本文が残らない（Debug ビルドでもデフォルト無効）
  - secure アプリから通常アプリへ復帰すると元のモードに戻る
- **参照仕様**: `docs/privacy-and-secure-input-spec.md`

### M49: 学習データ可視化・バックアップ

- **目的**: ユーザーが学習内容を確認・削除・バックアップ・復元できるように
  し、透明性を担保する。M34（DPAPI）と M30（設定アプリ）の自然な拡張。
- **前提**: M30 完了（設定アプリ）、M34 完了（DPAPI 暗号化）。
- **推奨実装時期**: Phase 7 末尾、M34 完了直後。M35 / M36-A / M55 の学習
  データも対象に含めるため、これらが実装済みの場合は範囲を拡張する。
- **変更対象**: `settings-app/`（学習データタブ）、`ipc/src/Payloads.cpp`
  （`ListLearningEntries` / `ForgetLearningEntry` / `ExportLearningData` /
  `ImportLearningData` 追加）、`learning/src/LearningStore.cpp`（列挙 API 追加）、
  `inference-host/src/BackupArchive.cpp`（新規）。
- **実装範囲**: `docs/learning-data-management-spec.md`。
  - 学習候補 / ユーザー辞書 / typo 補正 / 新語候補の 4 タブ UI
  - 個別忘却（候補単位で weight を 0 化）
  - DPAPI 暗号化済み ZIP バックアップ（`manifest.json` + 各 `*.tsv.enc` /
    `*.json.enc`）
  - インポート時の衝突解決（weight 加算 / 上書き / 両方保持を設定）
- **受け入れ条件**:
  - 学習データを UI から検索できる
  - 個別忘却が次回候補順位に反映される
  - 同一 Windows ユーザー / 同一マシン上で export → import の round-trip
    が件数一致で復元できる（DPAPI ユーザースコープのため他マシン / 他
    ユーザーへの移行は本受け入れ範囲外。クロス環境復元は §5.2 の
    明示的平文エクスポートを使う）
  - 暗号化済みデータは他ユーザーで復号できない
- **参照仕様**: `docs/learning-data-management-spec.md`

## 変換品質トラック（M52〜M57）

> Phase 5〜7 と独立した新トラック。改善提案
> `azookey_windows_ime_improvement_spec.md` の §5〜§10 を取り込み、変換品質
> （top-k 精度 / 同音異義語選択 / 固有名詞 recall / 打ち間違え補正 /
> latency）を数値で計測しながら段階改善する。
>
> **正典仕様**は本トラック専用の以下:
> - `docs/conversion-quality-benchmark-spec.md`（M52）
> - `docs/auto-word-registration-spec.md`（M36-A/B 改訂 + M53 追補）
> - `docs/user-learning-enhancement-spec.md`（M54）
> - `docs/typo-correction-learning-spec.md`（M35 改訂 + M55 追補）
> - `docs/neural-reranker-spec.md`（M56）
> - `docs/modernbert-ja-scoring-spec.md`（M57）

### M52: 変換品質評価ベンチ

- **目的**: 変換精度・同音異義語選択・固有名詞 recall・打ち間違え補正・
  latency・メモリを数値で比較できるベンチを整備し、以降の M53〜M57 の
  効果を baseline 比で測定可能にする。
- **前提**: M7（学習）完了、M9（ユーザー辞書）完了、既存 `bench/`。
- **推奨実装時期**: Phase 4 完了直後。M53〜M57 のいずれよりも先に着手する。
- **変更対象**: 既存 `bench/live_bench.cpp`（`azookey_bench` ターゲットの
  ソース。spec §3 の方針に従い拡張する。新規 `azookey_bench.cpp` は
  作らない）、`bench/data/*.jsonl`（新規評価データ）、
  `bench/CMakeLists.txt`、`.github/workflows/`（評価ジョブ追加、optional）。
- **実装範囲**: `docs/conversion-quality-benchmark-spec.md`。
  - 評価データ形式: `kana_kanji_eval.jsonl` / `typo_eval.jsonl`
  - カテゴリ: general / homophone / named_entity / neologism /
    mixed_script / business / coding / casual / creative / user_adapt /
    typo
  - 指標: top1/3/5 acc / MRR / reading_fidelity_rate /
    named_entity_recall_at_5 / typo_correction_top5_accuracy /
    typo_false_positive_rate / typo_overcorrection_rate /
    latency_p50/95/99 / timeout_rate / memory_peak_mb / fallback_rate
  - baseline 比較レポート（前 commit との diff）
- **受け入れ条件**:
  - `azookey_bench --eval bench/data/kana_kanji_eval.jsonl --output
    result.json` で全指標を計算できる
  - `azookey_bench --eval bench/data/typo_eval.jsonl --output
    typo_result.json` で typo 指標が算出・schema 出力される（M52 時点では
    typo 補正は未実装＝M55 のため既定 `off`。補正有効モード `rank`/`aggressive`
    での実測値検証は M55 受け入れで行う。spec §7 参照）
  - baseline 比較レポートが CI artifact 化される
  - 初期評価データが spec §11.2「M52 初期」を満たす（general / homophone /
    typo / typo_clean 各 100・計 ≥400。typo_clean は false-positive /
    overcorrection の分母のため必須）
  - 全評価ケースが `provenance` を持ち spec §13.1 のライセンス方針に適合する
    （データへ複製・派生する出典は CC0 / PD / authored のみ。notice 付き許諾物
    MIT/Apache/BSD は参照のみで複製不可、コピーレフト・来歴不透明は使用不可）
  - `bench/data/DATA-LICENSE.md`（spec §13.4 の CC0 専用宣言。`bench/data/` を
    CC0 として再配布する根拠）が存在する。`authored` 以外の `provenance` を含む
    場合は `bench/data/PROVENANCE.json`（spec §13.3）も存在し各出典に対応する
  - baseline が spec §14 の安定性基準（決定的採取・精度系の再実行差 ≤ 0.2pp）
    を満たす
- **合格基準 v1**（M53〜M57 完了時点で達成）:
  - top1_accuracy: baseline 比 +3% 以上
  - top5_accuracy: 95% 以上
  - homophone top1: baseline 比 +5% 以上
  - named_entity top5: 90% 以上
  - typo_correction_top5_accuracy: 85% 以上
  - typo_false_positive_rate: 1% 未満
  - typo_overcorrection_rate: 0.5% 未満
  - p95 latency: 50ms 以下
  - p99 latency: 100ms 以下
  - timeout_rate: 0.1% 未満
  - inference-host crash: 0 件
- **参照仕様**: `docs/conversion-quality-benchmark-spec.md`

### M53: 辞書・固有名詞・新語強化

- **目的**: Zenzai が苦手な固有名詞・新語・技術語・地名・人名・製品名を
  辞書層で補強する。M36-A（AutoWordStore）の上に DictionaryStore 階層を
  載せて全体を再設計する（`neologd_lexicon` optional pack の取り込みは別
  follow-up）。
- **前提**: M9（ユーザー辞書）、**M36-A**（AutoWordStore の移行元として
  必須）、M52（ベンチ）。`neologd_lexicon` optional pack は M53 v1 必須では
  なく別 follow-up 扱い（M36-B / M32 の `HttpDownloader` + SHA256 基盤を
  再利用するが、専用 pack 形式 + DictionaryStore ローダを要する別作業。
  M36-B の trending→AutoWordStore とは別経路。`docs/auto-word-registration-spec.md`
  §14.10）。未完了時は当該 layer を無効化して受け入れる。
- **推奨実装時期**: M52 完了後。M54 / M55 と並行可能。
- **変更対象**: `learning/src/DictionaryStore.cpp`（新規）、
  `learning/src/DictionaryImporter.cpp`（新規）、
  `inference-host/src/DictionaryCandidateProvider.cpp`（新規）、
  `learning/src/AutoWordStore.cpp`（M36-A から移行）、
  `docs/auto-word-registration-spec.md`（改訂）。
- **実装範囲**: `docs/auto-word-registration-spec.md` の M53 追補章。
  - 辞書階層: base / sudachi / neologd / named_entity / technical_terms /
    user / app_specific
  - 辞書エントリ形式（surface / reading / normalized_reading / pos /
    category / cost / frequency / source / priority / created_at /
    updated_at）
  - 読み正規化（カタカナ↔ひらがな / 長音緩和 / ヴァ↔バ alias /
    づ↔ず alias）
  - 固有名詞カテゴリ（person_name / place_name / station_name /
    product_name / software / anime_game / company_org）
  - `dictionary_score` = base_frequency + source_priority +
    exact_reading_bonus + category_bonus - obsolete_penalty
    （M48 候補タグ boost は含めない。app-profile-spec §7 が `final_score`
    に 1 回適用。spec §14.5/§14.11）
  - 辞書更新パイプライン（bundled / neologism pack / technical pack /
    user / app-specific）
- **受け入れ条件**:
  - M52 ベンチで named_entity_recall_at_5 が 90% 以上
  - M52 ベンチで neologism カテゴリの top5 が baseline 比で改善する
    （M53 v1 では**同梱の `sudachi_lexicon`(core, NEologd 由来データを
    Apache-2.0 で内包) + `base_lexicon`** の範囲で評価。NEologd 本体は
    同梱しない（`docs/auto-word-registration-spec.md` §14.9）。optional
    `neologd_lexicon` pack（別 DL）有効時の追加改善は neologd pack
    follow-up（spec §14.10。M36-B とは別作業）で当該 pack 有効構成にて確認）
  - 同梱辞書のライセンス遵守（`docs/auto-word-registration-spec.md` §14.9
    の配布判定に従う。同梱は Apache-2.0 / BSD-3-Clause（SudachiDict 内包
    UniDic）/ CC0 / CC-BY-4.0 / 権利主張なし（日本郵便 郵便番号データ等の
    パブリックドメイン相当）のみ、NEologd は同梱せず別 pack DL）。
    **standalone NEologd 単体パック**の混入なしを
    MSIX 構築の配布ガードで CI チェック（SudachiDict 内包の NEologd 由来
    データは対象外。§14.10）
- **参照仕様**: `docs/auto-word-registration-spec.md` M53 追補（§14。
  ライセンス & 配布判定 §14.9 / パッケージング §14.10 / スコア係数 §14.11 /
  source tagging §14.12 / 受け入れ条件 §14.13）

### M54: ユーザー学習強化

- **目的**: 確定履歴・訂正履歴・アプリ別傾向・打ち間違え採否を細粒度に
  学習し、個人適応を強化する。M7（既存 LearningStore）の発展。
- **前提**: M7、M34（DPAPI）、M52（ベンチ）。
- **推奨実装時期**: M52 完了後。M53 / M55 と並行可能。
- **変更対象**: `learning/src/LearningStore.cpp`（既存 TSV を後方互換で
  拡張、SQLite 化は M54-B 以降の別 M に分離）、
  `inference-host/src/UserLearningScorer.cpp`（新規）、
  `docs/user-learning-enhancement-spec.md`（新規）。
- **実装範囲**: `docs/user-learning-enhancement-spec.md`。M54 v1 は TSV
  拡張で完結させ、SQLite 分割テーブル（committed_candidates /
  correction_events / app_profiles）は spec §3.3 に将来案として残すのみ。
  - TSV スキーマ拡張: `reading\tsurface\tweight\tlast_updated_epoch_sec\tcommit_count\tapp_name\tevent_type\tcontext_hash`（`last_updated_epoch_sec` は epoch 秒、ミリ秒ではない。`LearningStore.cpp` の単位と一致）
  - 学習イベント 7 種: 候補確定 / 即 Backspace / 再変換 / ユーザー辞書登録 /
    アプリ別確定 / typo 採用 / typo 拒否（typo 系は M55 完了後）
  - 時間減衰: half_life = 一般 30 日 / 固有名詞 90 日 / 技術語 120 日 /
    一時話題 14 日 / typo 60 日
  - `user_score` = log(1 + commit_count) × recency_score ×
    app_profile_weight × correction_penalty
  - 既存 TSV（M7 形式）からの自動マイグレーション戦略
- **受け入れ条件**:
  - 同じ入力を複数回確定すると、次回以降候補順位が上がる
  - M52 ベンチで user_adapt カテゴリが学習前後で改善する
  - 既存 `learning.tsv` から自動マイグレートできる
- **参照仕様**: `docs/user-learning-enhancement-spec.md`

### M55: 打ち間違え学習統合（M35 発展）

- **目的**: M35（個人タイプミス学習）を発展させ、ReadingHypothesis 経由で
  CandidateGenerator 前段に補正を組み込む統合エンジンに昇格する。
- **前提**: **M35 を改訂統合**、M46（secure mode で補正・学習無効化）、
  M52（ベンチ）、M53（辞書）。
- **推奨実装時期**: M52 / M53 完了後。M35 を v1（基本タイプミス学習）として
  残し、M55 を v2（統合補正エンジン）として spec 内に段差をつける。
- **変更対象**: `correction/`（新規ディレクトリ）、
  `correction/TypoCorrectionEngine.cpp`、
  `correction/KeyboardAdjacencyModel.cpp`、
  `correction/RomajiVariantNormalizer.cpp`、
  `correction/ReadingHypothesis.h`、
  `learning/src/TypoLearningStore.cpp`（M35 の TypoCorrectionStore を改名・
  拡張）、`ipc/src/Payloads.cpp`（`QueryCandidates` に `raw_keys`
  optional フィールド追加）、`docs/typo-correction-learning-spec.md`
  （改訂）。
- **実装範囲**: `docs/typo-correction-learning-spec.md` の M55 追補章。
  - TypoCorrectionEngine の位置: `InputNormalizer` →
    `TypoCorrectionEngine` → `ReadingHypotheses` → `CandidateGenerator`
  - Weighted Edit Graph（insertion / deletion / substitution /
    transposition / personalized_pattern_cost）
  - Keyboard Adjacency Model（JIS / US 配列差を考慮）
  - Romaji Variant Normalizer（si↔shi / ti↔chi / tu↔tsu / syo↔sho /
    nn↔n など）
  - Dictionary-Constrained Correction（補正後の読みが辞書ヒットしない場合
    は棄却）
  - Context-Constrained Correction（補正候補の surface が文脈に合わない
    場合は減点）
  - 補正モード 4 種（`off` / `suggest` / `rank` / `aggressive`）
  - TypoLearningStore テーブル: typo_patterns / typo_events / typo_settings
  - 信頼度更新: accept_weight=0.25 / reject_weight=0.45（拒否を強く）
  - 発動条件（top1/top2 gap 小 / dictionary hit なし / 高信頼 personal
    pattern）
  - 誤補正防止条件（top1 強 / confidence 低 / 過去拒否 / 入力短 /
    パスワード欄 / コード入力）
  - プライバシー: raw_keys は抽象化パターンのみ保存
- **受け入れ条件**（typo 補正指標は補正有効モードで採取する。
  `conversion-quality-benchmark-spec.md` §7・§14。`--typo-mode off` での値は
  受け入れに用いない）:
  - M52 ベンチ（`--typo-mode rank`）で typo_correction_top5_accuracy が 85% 以上
  - M52 ベンチ（`--typo-mode rank`）で typo_false_positive_rate が 1% 未満
  - M52 ベンチ（`--typo-mode aggressive`）で typo_overcorrection_rate が
    0.5% 未満
  - M35 の既存 `typo_corrections.tsv` から自動マイグレートできる
- **参照仕様**: `docs/typo-correction-learning-spec.md` M55 追補

### M56: Tiny Neural Reranker

- **目的**: Zenzai / 辞書 / ユーザー辞書 / 補正候補を、文脈・特徴量で軽量に
  並べ替える。生成は行わず、候補選択に専念する。
- **前提**: M52（ベンチ）、M53（辞書）、M54（学習強化）、M55（打ち間違え統合）
  の **全 4 つが完了**。reranker の入力特徴量は `dictionary_score`（M53）/
  `user_frequency`（M54）/ `typo_confidence`（M55）を含むため、いずれかが
  欠けると acceptance を満たせない。
- **推奨実装時期**: M53 / M54 / M55 がすべて完了し、M52 ベンチで baseline を
  固定した時点。学習データ収集（§6）は M54 / M55 の出力に依存するため、
  早期着手しても本実装フェーズは前提完了後に行う。
- **変更対象**: `reranker/`（新規ディレクトリ）、
  `reranker/TinyReranker.h`、`reranker/TinyRerankerOnnx.cpp`、
  `models/tiny_reranker.onnx`（新規アセット）、
  `inference-host/src/Dispatcher.cpp`（rerank フェーズに統合）、
  `docs/neural-reranker-spec.md`（新規）。
- **実装範囲**: `docs/neural-reranker-spec.md`。
  - 特徴量（v1 = embedding を除く 12 種）: zenzai_score / dictionary_score /
    user_frequency / recency_score / typo_confidence / app_profile_score /
    candidate_length / segment_count / is_named_entity / is_user_dict /
    is_neologism / is_typo_corrected。
    left_context_embedding / reading_embedding / candidate_embedding は
    **v2 以降**（embedding 導入時）の追加特徴で v1 には含めない
    （spec §4 / §4.1 / §5.2 の `features_v1`=12 次元固定と整合）
  - モデル: v1 = MLP reranker、v2 候補 = Mini Transformer
  - 学習データ: 正例 = 確定候補、負例 = 表示されたが選ばれなかった候補、
    強い負例 = 訂正イベント
  - ONNX Runtime CPU 優先、timeout 10〜20ms、failure 時は reranker なしで
    返す fallback
  - **embedding 供給方針は `docs/neural-reranker-spec.md` §4.1 で決定済み**
    （v1 = 手作り特徴量のみ / Option C、v2 以降 = 独立小型 encoder / Option A の
    段階導入。M57 ModernBERT 共用 / Option B は不採用）。同 §4.1 を正典とする
- **受け入れ条件**:
  - M52 ベンチで top1 が baseline 比 +3% 以上
  - p95 latency 悪化が +10ms 以内
  - timeout 時に reranker なしで候補が返る（fallback 動作）
- **参照仕様**: `docs/neural-reranker-spec.md`

### M57: ModernBERT-Ja 候補スコアリング

- **目的**: 同音異義語など難しい候補のみ ModernBERT-Ja で文脈自然度を
  評価し、品質を底上げする。**生成には使わない**。
- **前提**: M56（Tiny Reranker）、M24（DirectML / NPU バックエンド）。
- **推奨実装時期**: M56 完了後。v1.0 後の品質向上フェーズに位置づける。
- **変更対象**: `reranker/ModernBertScorer.h`、
  `reranker/ModernBertScorerOnnx.cpp`、
  `models/modernbert-ja-70m.onnx`（新規アセット）、
  `inference-host/src/Dispatcher.cpp`（ambiguity 判定で起動制御）、
  `docs/modernbert-ja-scoring-spec.md`（新規）。
- **実装範囲**: `docs/modernbert-ja-scoring-spec.md`。
  - 起動条件: `candidate_count >= 2` かつ
    `ambiguity_score >= threshold` かつ
    `timeout_budget_remaining >= 30ms`
  - `ambiguity_score` = top1/top2 gap + homophone_candidate_count +
    context_dependency_score + named_entity_mix_score +
    typo_correction_uncertainty
  - PLL 近似（候補位置のみ mask、top-K のみ評価）
  - cache（同一 context 再利用）
  - timeout 30〜50ms、failure 時はスコアなしで続行
  - **RSS 許容上限を spec で明示**（IME プロセスとしての許容値）
- **受け入れ条件**:
  - M52 ベンチで homophone top1 が baseline 比 +5% 以上
  - 通常入力の p95 latency 悪化が +20ms 以内
  - ModernBERT がロードできない環境でも fallback で動作する
- **参照仕様**: `docs/modernbert-ja-scoring-spec.md`

## 横断テーマと Phase の対応

各リッチ化テーマ（`docs/rich-features-spec.md`）の実装タイミングは
以下のマイルストーンに紐付ける：

| テーマ | 短期（Phase 5 末） | 中期（Phase 6 統合） | 長期（Phase 7 後） |
|---|---|---|---|
| X-1 ライブ変換 | M14 末（信頼度 4 段階） | M24（重い推論） | — |
| X-2 AI 予測 | M15 末（paragraph_context） | M24（PredictWithLLM + Stream） | M30（promptPrefix UI） |
| X-3 誤変換訂正 | M16 末（Post-Commit Lint）/ M17 末（FuzzyMatch） | M24 後（DetectAnomalies） | M30（バッチ訂正ビュー） |
| X-4 基盤 | M13〜M19 で個別 | M24 後にスケジューラ統合 | — |

個人タイプミス学習（M35）は X-3「誤変換訂正」と関連するが、**かな読みレベルの
打鍵ミス**を対象とする独立機能であり、`docs/typo-correction-learning-spec.md` を
正典とする。M55 は M35 を発展させた統合補正エンジンであり、同じ正典 spec
（M55 追補章）で扱う。

新語自動取得（M36-A / M36-B）は手動登録の `UserDictionary` とは独立した辞書自動
拡充機能であり、`docs/auto-word-registration-spec.md` を正典とする。M53 は
M36-A/B を内包する辞書層全体の再設計であり、同じ正典 spec（M53 追補章）で扱う。

動的自動句読点（M59）は X-1「ライブ変換」経路の上に載る enhancement であり、
`docs/dynamic-punctuation-spec.md` を正典とする。M58-C の `batchAutoPunctuation`
（一括変換 + ai-cleanup 限定の句読点挿入）とは別経路で、逐次ライブ変換中に動作する。
X-1-2（`TypingTempoTracker`）を挿入安定化に再利用する。

インライン英単語候補（M60）は X-2-3（`CandidateTag::English`）と生ローマ字バッファ保持
（M58 §4.1）を再利用する候補注入機能であり、`docs/inline-english-candidate-spec.md` を
正典とする。スコープは 1 語単位の候補注入のみで、連続英文タイプは将来課題。

変換品質トラック（M52〜M57）の各 spec は本ロードマップで定めた M 番号と
1:1 対応する。M52 ベンチで baseline を固定し、M53〜M55 並行 → M56 → M57 の
順で効果を測定しながら進める。

karukan（`togatoga/karukan`、MIT OR Apache-2.0、Linux/macOS 向け日本語 IME）は、azooKey が
計画済みのいくつかのマイルストーンに対する**設計参照実装**として扱う。コードは逐語移植せず
設計思想のみ参照する。主な対応: M13（TSF 非依存 IM 状態機械 = `karukan-im` の InputState /
EngineAction / 純粋関数。`docs/legacy-parity-spec.md` §1.1 が既に引用済み）、M14（ライブ変換の
増分再チャンク・非日本語チャンク verbatim 通過）、M32 / M45（モデル DL・宣言的レジストリ・
プリフェッチ・pre-tokenizer 上書き。ブロッカー: zenz GGUF 配布ライセンス DEV-202）、M52（Exact
Match / CER 評価 CLI。評価データは品質ベンチ spec §13 準拠で自前作成）、M15 / M54（学習の
reading-keyed 二層 + 前方一致予測）、M62（候補リライター層、上記）。詳細な比較・優先度・最小
導入案・テスト方針・ライセンス注記は調査レポート `plans/karukan-comparison-report.md`
（特定時点のスナップショット）を参照する。
