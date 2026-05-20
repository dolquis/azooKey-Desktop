# azooKey-Desktop Windows 版 v1.0 開発プラン

## Context

azooKey-Desktop の Windows 版は `plans/windows-port-roadmap.md` で M0〜M12 が
定義されている。2026-05 時点の実装実態を再点検したところ、**Phase A / Phase B
が完了** し、`tsf-tip/` 側の COM 登録・Composition・候補 UI・確定・観測送信・
Cancel・staleness ガードまで動作する MVP に到達した。残るクリティカルパスは
**M8 (Zenzai 実機統合) と M11 (設定 UI + MSIX パッケージング) と
M12 (CI 強化 + 署名配布)** の 3 つに集中している。

本プランは Windows 版 v1.0（MSIX 配布可能な最小 IME）リリースまでの実行順を
再構築する。macOS 版（Issue #181）は本プランの対象外。

詳細なマイルストーン定義と実装現状は `plans/windows-port-roadmap.md` を参照。
**本ファイルはフェーズ実行計画のみを管理する。各マイルストーンのステータスは `plans/windows-port-roadmap.md` を正典とする。**

## 実装実態の再評価サマリ（2026-05 時点のスナップショット）

> 最新ステータスは `plans/windows-port-roadmap.md` を参照。以下は本プラン策定時点の状況。

| MS | 表記 | 実態 | 残作業 |
|---|---|---|---|
| M0 廃止資産削除 | 完了 | ✅ 完了 | なし |
| M1 IPC 疎通 | 完了 | ✅ 完了 | なし（`ipc/tests/tip_client_ipc_test.cpp` で回帰保護） |
| M2 TIP 登録 | 完了 | ✅ 完了 | なし（`DllRegisterServer/DllUnregisterServer` 実装済み） |
| M3 Composition 表示 | 完了 | ✅ 完了 | なし（`DoEditSession` 本実装＋display attribute） |
| M4 モック候補生成 | 完了 | ✅ 完了 | なし（IPC worker thread で配線） |
| M5 候補 UI | 完了 | ✅ 完了 | なし（`CandidateWindow` 実装済み） |
| M6 Commit/Observation | 完了 | ✅ 完了 | なし（`PostCommitObservation` 配線） |
| M7 再ランキング学習 | 完了 | ✅ ほぼ完成 | 実機長期運用での挙動確認 |
| M8 Zenzai ロード | 未着手 | ⚠️ スケルトン | llama.cpp バインディング選定 + 本実装 |
| M9 ユーザー辞書 | バックエンド完了 | ⚠️ UI 未接続 | M11 で設定 UI から接続 |
| M10 Cancel | 完了 | ✅ 完了 | なし（in-flight cancel + staleness check） |
| M11 設定 UI/パッケージング | 未着手 | ❌ 未着手 | フレームワーク選定 + MSIX |
| M12 CI と署名配布 | 部分着手 | ⚠️ build/test のみ | 署名・MSIX アーティファクト・タグ release 自動化 |

## フェーズ計画

### Phase A: TIP 基盤完成 ✅ 完了（mainマージ済み: `603cd1d`）

実機 IME としてローマ字を打鍵し、Host から候補を取得して候補ウィンドウに表示するまで動作。

### Phase B: 候補選択と確定動線 ✅ 完了（mainマージ済み: `603cd1d`）

候補選択・確定・観測送信・早打ち耐性（in-flight cancel + staleness）まで動作。

### Phase C: 実 Zenzai と辞書 UI のつなぎ込み (3〜5 週) 🚧 着手対象

1. **M8 Zenzai 統合** — `inference-host/src/InferenceEngine.cpp::LoadModel`
   の本実装。llama.cpp の C-API バインディングを採用、CMake オプションで
   `AZOOKEY_BACKEND=cpu|cuda` を切替。配布サイズと初回起動時間を `bench/` で
   計測。モデル未配置時は `SimpleConverter` フォールバックを維持。
   `docs/zenzai-gpu-route.md` を実装と整合させる。
2. **M9 ユーザー辞書ランタイム反映** — `AddUserWord`/`RemoveUserWord`
   （Host 側完成済み）を TIP もしくは設定 UI から呼べる経路を作る。
   今フェーズではコマンドラインまたはデバッグ UI で十分。

**Phase C 着手前タスク**:
- ✅ llama.cpp バインディング選定スパイク（2026-05-20）:
  M8 の初期実装は llama.cpp C API + CPU backend から開始し、CUDA は optional
  backend として追加する。DirectML / NPU は Phase 2-B M24 まで予約値扱い。
  判断理由と計測ゲートは `docs/zenzai-gpu-route.md` を参照。
- ✅ `LoadModel` 境界固定（2026-05-20）:
  `LoadModelRequest(path, backend, n_gpu_layers)` を
  `InferenceEngine::LoadModel` に渡し、`model_loaded` / `last_error` を
  `Handshake` / `Health` で観測できる状態にする。
- ✅ M9 最小操作面の決定（2026-05-20）:
  Phase C では本格設定 UI を待たず、`inference-host` の IPC 経由で
  `AddUserWord` / `RemoveUserWord` を呼ぶ小 CLI または debug probe を先に作る。
  設定アプリ統合は M11 に送る。
- `core/IConverter` 抽象は既に存在 — Zenzai converter は `IConverter` 実装として差し替え

検証: gguf 配置で `LoadModel` 成功、未配置で起動継続（`SimpleConverter` フォールバック）、CPU/GPU 切替が `--backend` で効く、ユーザー辞書追加が次の `QueryCandidates` で即反映。

### Phase D: 配布可能化 — v1.0 リリースゲート (4〜6 週)

3. **M11 設定 UI とインストーラ** — フレームワークは WinUI 3 を第一候補とし、
   Phase C 中に 1〜2 日のスパイクで WPF/Tauri と比較してから着手。設定アプリ
   は TIP/Host と別プロセス、IPC 経由で Host 設定（Zenzai ON/OFF、
   ユーザー辞書）を変更。配布は MSIX（ユーザースコープ自動登録、
   アンインストールでの登録解除）。
4. **M12 CI 完成と署名配布** — 現状 `.github/workflows/windows.yml` で Windows
   ランナーで build/test まで実施中。残: コード署名ステップ、タグ push 時の
   MSIX 自動 Release 公開、submodule 配信ポリシー確定。

検証: クリーン Win11 VM での MSIX インストール → IME 選択 → 入力 → 確定 →
アンインストールでクリーン状態に戻る。CI 緑、タグ push 時に署名済み MSIX が
自動公開。

## 直近 (Phase C) で触るファイル

- `inference-host/src/InferenceEngine.cpp` — `LoadModel` の本実装、Zenzai converter 配線
- `inference-host/include/azookey/host/InferenceEngine.h` — モデル状態の保持・解放API
- `core/include/azookey/core/IConverter.h` — Zenzai 実装が嵌まることを確認（変更不要が望ましい）
- `bench/` — Zenzai ロード時間・推論レイテンシを計測
- `CMakeLists.txt` — `AZOOKEY_BACKEND=cpu|cuda` オプション
- `docs/zenzai-gpu-route.md` — 実装結果と整合
- `inference-host/tests/` — Zenzai converter のモック実装でテスト追加

## 再利用すべき既存実装

- `core/include/azookey/core/IConverter.h` — Zenzai は `IConverter` 実装として
  `SimpleConverter` と差し替え可能
- `inference-host/src/InferenceEngine.cpp` の reranker・user_dict 経由パイプライン —
  Zenzai 出力にもそのまま適用可能
- `ipc/src/Payloads.cpp` の `LoadModelRequest/Response` — 既に CMake オプション
  `--backend cuda|cpu` をリクエストで指定する設計

## 検証手順（Phase C 完了時点で実施）

1. **ビルド**: `cmake -S . -B build -DAZOOKEY_BUILD_TESTS=ON -DAZOOKEY_BACKEND=cpu && cmake --build build`
2. **ユニットテスト**: `ctest --test-dir build --output-on-failure` で
   IPC/Core/Learning/Host/TSF-TIP の全テストが緑であること
3. **Windows 実機テスト**（Win11 VM 推奨）:
   - `scripts/register.ps1` で TIP DLL を登録（`DllRegisterServer` 経由）
   - `azookey_inference_host.exe --pipe --backend cpu` で起動
   - gguf を `%LOCALAPPDATA%\azooKey\models\` に配置し、`LoadModel` が成功すること
   - gguf を削除し起動した場合に `SimpleConverter` にフォールバックすること
   - メモ帳で `nihongo` 等を入力し、Zenzai 候補が出ること
4. **GPU 経路**: `--backend cuda` で起動し、CUDA 初期化失敗時は CPU にフォールバック
5. **ベンチ**: `./build/bench/azookey_bench.exe` の p50/p95 が CPU/GPU で許容内か
6. **`unregister.ps1`**: クリーン解除確認

## リスクと対応

| リスク | 影響 | 対応 |
|---|---|---|
| llama.cpp バインディング選定 (M8) | 配布サイズ・初回起動時間に直結 | Phase C 着手スパイクで確定 (`docs/zenzai-gpu-route.md` 更新)、`bench/` で計測 |
| CUDA SDK の配布制約 | MSIX のサイズ膨張・GPU なし PC でのフォールバック品質 | バックエンドは optional payload、CPU を default に、ggml-cuda は別 MSIX オプションパッケージで検討 |
| MSIX 配布 (M11) のユーザースコープ登録 | アンインストール後にレジストリが残る | 既に `DllRegisterServer` で HKCU 登録に統一済み。MSIX manifest で `comServer` を宣言し、アンインストール時に確実に消えることを VM テストで確認 |
| 設定 UI フレームワーク選定 (M11) | 配布サイズ・依存ランタイム | Phase C 中に WinUI 3 / WPF / Tauri を 1〜2 日比較スパイク |
| 署名証明書の調達 (M12) | リリース日に直結 | Phase D 着手前に EV/OV 証明書の手当てを並行 |

## このプランの範囲外

- macOS 版 v1.0（Issue #181 で別管理、`legacy/` 配下に保全）
- `plans/segment_edit.md` の文節エディット機能（現状 macOS 向けの上流計画、Windows MVP 後）
- `legacy/Core/Sources/Core/InputUtils/InputState.swift:271,336` の FIXME（macOS 側）
- Linux 版（コミュニティフォーク `fcitx5-hazkey` で対応）

## v1.0 以降の中長期フェーズ

v1.0 リリース後の中長期計画。各 Phase の詳細マイルストーン定義は
`plans/windows-port-roadmap.md` の Phase 1〜3 を、機能仕様は
`docs/legacy-parity-spec.md` / `docs/rich-features-spec.md` /
`docs/tsf-deep-integration-spec.md` / `docs/copilot-pc-backend-spec.md` /
`docs/native-ui-spec.md` / `docs/sideload-packaging-spec.md` を参照。

### Phase E: レガシー parity 復元（≒ Phase 1、8〜12 週）

- **目的**: 旧 macOS 版（`legacy/`）で実装されていたが Windows MVP では未移植の
  機能を復元し、azooKey として期待される入力体験の最低ラインに到達する。
- **着手前タスク**:
  - `legacy/Core/Sources/Core/InputUtils/InputState.swift` を読み直し、
    `docs/legacy-parity-spec.md` §1 の遷移表と差分がないかレビュー
  - OpenAI 互換エンドポイント（`gpt-4o-mini`）の API キー手当て
  - Magic Conversion / Replace Suggestion 用の Phase 1 簡易ダイアログ UI
    モック確認
- **主要マイルストーン**: M13（入力パイプライン）/ M14（ライブ変換）/
  M15（予測候補）/ M16（Magic Conv & Replace Suggestion）/ M17（カスタム
  ローマ字）/ M18（Unicode 入力 / 学習忘却 / デバッグウィンドウ）/
  M19（マルチディスプレイ）
- **検証**:
  - メモ帳で `liveConversion=true` 設定 → 入力中に preedit が更新される
  - 英数キーダブルタップ → プロンプトダイアログ → OpenAI 経由で結果反映
  - `%LOCALAPPDATA%\azooKey\custom-romaji.tsv` を更新するとホットリロード
  - Ctrl+Shift+U → `30A1` Enter で「ァ」が入力
  - F10 でデバッグウィンドウが開き IPC ログが見える
  - 2 枚モニタ環境でキャレット追従
- **直近で触るファイル**:
  - 新規: `core/include/azookey/core/UserAction.h`, `InputState.h`、
    `core/src/InputState.cpp`, `CustomRomajiLoader.cpp`,
    `UnicodeInputBuffer.cpp`
  - 新規: `tsf-tip/src/PredictionWindow.cpp`, `PromptDialog.cpp`,
    `DebugWindow.cpp`, `CaretRectResolver.cpp`
  - 新規: `inference-host/src/AiBackend.cpp`
  - 更新: `tsf-tip/src/TextService.cpp`, `core/src/RomajiKanaConverter.cpp`,
    `learning/src/LearningStore.cpp`, `ipc/src/Payloads.cpp`
- **再利用すべき既存実装**:
  - `core/src/RomajiKanaConverter.cpp`（既存）にカスタムテーブルとパスを乗せる
  - `learning/src/LearningStore.cpp` の `Observe` 実装は維持し、Forget API を追加
  - `tsf-tip/src/CandidateWindow.cpp` のレイアウトロジックを `PredictionWindow`
    の配置計算で参考にする
  - `RequestScheduler` の staleness check / Cancel をライブ変換でも流用
- **リスク**:
  - OpenAI API キー漏えい → 設定アプリ（Phase G）まで DPAPI 暗号化が無い
    ため、Phase E 期間中の API キーは平文保存（README で注意喚起）
  - 旧 macOS 仕様との差異が発覚した場合は `docs/legacy-parity-spec.md` を
    先に更新してから実装

### Phase F: Windows ネイティブ深耕（≒ Phase 2、12〜20 週、M8 と合流）

- **目的**: MS-IME 互換の TSF 統合、Copilot+ PC / NPU 最適化、UI のモダン化を
  並行で進め、「Windows で最も洗練された IME 体験」を目指す。
- **着手前タスク**:
  - M8 Zenzai 統合と並行のため、`docs/zenzai-gpu-route.md` に DirectML / NPU
    ルートのスパイク結果を追記
  - DXCore SDK ヘッダ手当て（Windows 11 SDK 22621 以降）
  - DirectComposition / DirectWrite サンプルコードの動作確認
  - 評価環境: Snapdragon X 実機 1 台 / Intel Core Ultra 1 台を Phase F 着手時に
    手配
- **主要マイルストーン**: M20（Reconversion）/ M21（UI-less Mode）/
  M22（半角全角等）/ M23（複合 DisplayAttribute）/ M24（DirectML / NPU）/
  M25（mmap / 省電力）/ M26（DPI / Dark / Mica）/ M27（ARM64）
- **検証**:
  - Office 2021 / Edge / Notion 等で UI-less Mode が動作
  - メモ帳「明日」選択 → 変換キーで「あした」候補
  - Snapdragon X 実機で NPU バックエンドが選ばれる
  - 96/144/192 DPI で正しくスケール、Dark/Light が追従、Mica 背景
  - ARM64 CI ビルドが緑
- **直近で触るファイル**:
  - 新規: `tsf-tip/src/ReconversionFunction.cpp`,
    `CandidateListUIElement.cpp`, `ConfigureFunction.cpp`,
    `ThemeColors.h`, `RenderingEngine.cpp`
  - 新規: `inference-host/src/BackendSelector.cpp`, `MmapModelLoader.cpp`,
    `PowerStatusWatcher.cpp`, `DirectMlBackend.cpp`,
    `QnnBackend.cpp`（NPU、スパイク結果次第）
  - 更新: `tsf-tip/src/CandidateWindow.cpp`, `PredictionWindow.cpp`,
    `DllMain.cpp`（Category 登録、新規 GUID）
  - 更新: `.github/workflows/windows.yml`（matrix x64/arm64）
- **再利用すべき既存実装**:
  - M8 で導入される llama.cpp バインディングを DirectML / NPU 経路の
    フォールバックとして残す
  - `tsf-tip/src/CandidateWindow.cpp` の HWND ライフサイクル管理は
    DComp + D2D ベースに置換しても流用
- **リスク**:
  - DXCore / NPU SDK のバージョン互換が頻繁に変わる → SDK は CMake で
    特定バージョンに pin
  - DirectML EP のレイテンシが要件未達なら llama.cpp 純 CPU + ARM NEON で
    Snapdragon 環境を救済（M27 連携）
  - Mica が古い OS で利用不可 → アクリル / 単色フォールバックを必ず実装

### Phase G: サイドロード配信（≒ Phase 3、8〜14 週）

- **目的**: Microsoft Store 配信に頼らず、署名済み MSIX / WiX を
  GitHub Releases + WinGet で配布し、自動更新と観測性を確立する。
- **着手前タスク**:
  - EV/OV コード署名証明書の調達（リリース日に直結するため Phase F 中盤から
    並行手当て）
  - GitHub Secrets に PFX を base64 で格納（`WINDOWS_PFX_BASE64`,
    `WINDOWS_PFX_PASSWORD`, `WINDOWS_CERT_THUMBPRINT`）
  - winget-pkgs リポジトリへの contribution ガイド確認
  - WinUI 3 / C++/WinRT の評価スパイク（既存 settings-app/ がない前提で
    プロジェクトテンプレートから着手）
- **主要マイルストーン**: M28（MSIX）/ M29（コード署名 CI）/ M30（設定アプリ）/
  M31（WiX / Inno）/ M32（WinGet + 自動更新）/ M33（ETW / WER）/
  M34（DPAPI 暗号化）
- **検証**:
  - クリーン Win10 22H2 / Win11 23H2 VM で `Add-AppxPackage` → 動作 →
    `Remove-AppxPackage` でクリーン
  - `signtool /verify` で署名チェック pass
  - `winget install dolquis.azooKey` で導入可能
  - 起動時 + 24h 周期で更新通知が出る
  - `wpr -start` でトレース取得 → `wpa` で Event ID 表示
  - `learning.tsv.enc` を他ユーザ環境で開けないことを確認
- **直近で触るファイル**:
  - 新規: `pkg/msix/AppxManifest.xml`, `Package.wapproj`, `Assets/`
  - 新規: `pkg/wix/Product.wxs`, `pkg/inno/setup.iss`
  - 新規: `settings-app/`（C++/WinRT WinUI 3 プロジェクト）
  - 新規: `core/src/EtwLogger.cpp`, `learning/src/DpapiCrypto.cpp`,
    `inference-host/src/UpdateChecker.cpp`, `CrashHandler.cpp`
  - 新規: `.github/workflows/release.yml`
  - 新規: `manifests/d/dolquis/azooKey/<ver>/*.yaml`（winget-pkgs への PR）
- **再利用すべき既存実装**:
  - `scripts/register.ps1` / `unregister.ps1` のロジックを WiX カスタム
    アクションへ落とし込む
  - `tsf-tip/src/DllMain.cpp::DllRegisterServer/DllUnregisterServer`（既存）を
    MSIX manifest の `comServer` 宣言と整合させる
  - 既存 `LearningStore::Save/Load` のシリアライズフォーマットを保ったまま
    DPAPI 暗号化を内側でラップ
- **リスク**:
  - 証明書調達遅延 → リリース日に直結（Phase F 中盤から並行）
  - winget-pkgs マージレビューに時間がかかる → リリースから 1〜2 週は MSIX
    直接 DL の運用を許容
  - DPAPI 暗号化への移行で旧形式 TSV を読み損ねるとデータ消失 → 移行 1 回限り
    で `learning.tsv.bak` を必ず残す
