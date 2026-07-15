# Zenzai推論形式の特定とGPU化ルート決定

## 調査結果（根拠）

- `legacy/Core/Sources/Core/InputUtils/SegmentsManager.swift` で `ConvertRequestOptions.ZenzaiMode.on` に `weight: ...ggml-model-Q5_K_M.gguf` を渡している。
- READMEでも `azooKeyMac/Resources/zenz-v3-small-gguf/ggml-model-Q5_K_M.gguf` (約70MB) が明記されていた（**調査当時の記述**）。現行リポジトリでは submodule `zenz-v3.2-small-gguf`（`.gitmodules`、`https://huggingface.co/Miwa-Keita/zenz-v3.2-small-gguf`）を `legacy/azooKeyMac/Resources/gguf/` にマウントする。
- 依存は `AzooKeyKanaKanjiConverter` だが、呼び出し側の重み形式は GGUF である。

→ 現状のZenzaiは **GGUF/ggml系** と判断する。

## GPU化ルート

### 採用: ルートA（GGUF/ggml系 → ggml-cuda）

理由:
1. 既存資産（GGUF重み）をそのまま活かせる。
2. ONNX変換を挟まずにHost側へ導入しやすい。
3. TIP in-proc制約に対して、Host分離 + ggml-cudaロードが最短。

## フォールバック

- CUDA初期化失敗・GPUなしの場合は同一Host APIでCPU実行。
- TIPはバックエンド差を意識せず、IPCレスポンスのみで処理。

## Phase 3 M8 着手前スパイク結果（2026-05-20）

Phase 3 の最初の実装単位は **llama.cpp C API + CPU backend** とし、CUDA は
CMake オプションで optional に追加する。理由は以下。

1. GGUF を直接ロードでき、既存 Zenzai 資産を変換せずに検証できる。
2. `core/IConverter` 差し替えで `SimpleConverter` フォールバックを維持しやすい。
3. CUDA / DirectML / NPU の選定を `InferenceEngine::LoadModel(path, backend,
   n_gpu_layers)` の境界より内側に閉じ込められる。

M8 では `BackendKind::Cpu` / `BackendKind::Cuda` だけを有効化する。
`directml` は IPC payload の予約値として残すが、Phase 3 では unsupported として
扱い、Phase 6-B M24 の DirectML / NPU スパイクで再度有効化する。

2026-06-21 時点の実装:

- `AZOOKEY_BACKEND=cpu|cuda` を CMake cache option として追加。
- `AZOOKEY_LLAMA_CPP_SOURCE_DIR` または `AZOOKEY_FETCH_LLAMA_CPP=ON` で
  llama.cpp `llama` target を接続し、`azookey_host` に `AZOOKEY_WITH_LLAMA_CPP`
  を伝播する。未接続時は no-egress の mock runtime でビルド・テストを継続する。
- 実機ゲート用の `windows-llama-debug` preset は pin 済みの llama.cpp を取得する。
  `register-dev.ps1` の既定パスもこの preset を参照し、登録前に
  `azookey_zenzai_bench.exe` の `llama_cpp=1` を確認する。
- 実モデルを使う開発登録では、GGUF を
  `%LOCALAPPDATA%\azooKey\models\zenzai\` に配置したうえで、
  `register-dev.ps1 -ModelPath <gguf>` を実行する。
  ピン定義が未投入の間は任意の GGUF を自動選択しないため、配置だけでは
  `model.selectedPath` は確定しない。
  スクリプトは明示パスを現在セッションの Host と HKCU Run 登録の両方へ
  `--model` として渡す。
- `ZenzaiModelConverter` は GGUF magic/version を検証して
  `InferenceEngine::model_loaded()` / `Handshake` / `Health` に反映する。
- llama.cpp 接続時は `llama_model_load_from_file` / `llama_init_from_model` で
  GGUF をロードし、`Convert` のモデル生成候補を `CandidateSource::Model` と
  `debug_info=zenzai;lp=...;avg=...` に写像する。
- モデル未配置・破損・推論例外・空生成時は既存 converter に劣化し、候補には
  `zenzai-degraded:<reason>` を付与する。
- CUDA 指定時は valid GGUF を CPU fallback としてロードし、降格理由は
  `ModelLoadResult.error` の警告として返す。CPU ロード自体が成功した場合、
  `Health` は ok を保つ。

計測ゲート:

- CPU fallback: `bench/azookey_bench.exe` が exit=0、p95 < 50ms。
  2026-05-20 Debug build baseline: p50=0.0179ms, p95=0.0249ms, p99=0.052ms。
- Zenzai CPU: `bench/azookey_zenzai_bench.exe --model <gguf> --require-model
  --require-zenzai`（または
  `AZOOKEY_ZENZAI_MODEL=<gguf>`）で `LoadModel` 成功、初回ロード時間、p50/p95/p99、
  `zenzai_candidates`、先頭候補の `debug_info`、`requested_backend`、
  `effective_backend`、`load_warning` を記録する。モデル未指定時は `status=skipped`
  で成功終了し、CTest では `--mock-zenzai` で no-egress smoke を行う。
- fallback-only の TIP テストで no-llama ホストを登録する場合に限り、
  `register-dev.ps1 -AllowMockHost` で preflight を明示的に迂回できる。
- Zenzai CUDA: CUDA 未配線・初期化失敗時に CPU または `SimpleConverter` へフォールバック。

## 将来拡張

> **M24 決定（2026 更新、正典は `docs/copilot-pc-backend-spec.md` §4.3）**:
> DirectML が Microsoft により sustained engineering（新規開発停止）化されたため、
> 下記「DirectML EP ルート」「NPU ルート」を **Windows ML（ORT ベース・EP 自動配信／
> 自動選択）への一本化**として再定義する。ベンダ別 SDK（QNN / OpenVINO / MIGraphX）
> の個別バンドルは行わない。NPU / DirectML 系は独立 `BackendKind` 値ではなく
> `WinML` エンジン値で表し、具体 EP は Windows ML に委ねる。

### Windows ML ルート（R2、Phase 6-B M24 採用予定）

ONNX Runtime GenAI + Windows ML により、ベンダ横断で NPU / GPU / CPU を自動選択する。
DirectML を直接叩く旧 2 候補（llama.cpp + DirectML backend / ORT + DirectML EP）は、
前者を不採用、後者を Windows ML へ統合した。

- **前提**: zenz-v3 を ONNX Runtime GenAI 形式へ変換できること（Foundry Toolkit の
  turn-key 変換は対応モデルが限定列挙で zenz-v3 は対象外。手動変換可否を先にスパイク）。
- **アクセラレータ選択**: Windows ML が NPU（QNN / OpenVINO / VitisAI）→ GPU
  （NvTensorRtRtx / OpenVINO）→ CPU の順で自動選択・自動フォールバック。GenAI 経路の
  ため MIGraphX(AMD GPU) は現状除外（GenAI 未対応。`docs/copilot-pc-backend-spec.md`
  §4.1 注記参照）。
- **配布**: EP は Windows Update 経由で配信され MSIX に同梱不要（配布サイズ最小化）。

詳細選定の判断基準・enum 拡張ポリシー・フォールバック段位・DXCore 列挙アルゴリズムは
`docs/copilot-pc-backend-spec.md` §3〜§4 を参照。

### NPU ルート（Copilot+ PC）= Windows ML に統合

Copilot+ PC（Snapdragon X Elite / Intel Core Ultra / AMD Ryzen AI）の NPU は、上記
Windows ML ルート（R2）の自動 EP 選択（Qualcomm=QNN / Intel=OpenVINO / AMD=VitisAI）
で扱う。個別 SDK をバンドルせず、`BackendSelector` は R2(auto) 要求と電源状態の判断のみを
担う（概念的な優先順位は AC 時 **NPU > GPU > CPU**、バッテリ時 **NPU > CPU > GPU**。
GGUF/llama.cpp（R1）の CUDA / discrete GPU はバッテリ時に後回し）。

### ONNX/TensorRT ルート（保留）

Zenzai 本体が ONNX/TensorRT に寄る更新をした場合、Backend 抽象を追加して
ORT CUDA EP / TensorRT EP に切り替え可能にする。
現状は GGUF 重視のため M24 着手時点では Out-of-Scope。

### 既存「ルート A」の位置付け

**ルート A（GGUF/ggml + ggml-cuda）は NVIDIA GPU 環境専用**。AC 接続デスクトップ
で最速だが、ノート PC のバッテリ消費が激しい。Phase 6-B M24 完了後は
`BackendSelector` の優先順位下位（CPU の手前）にフォールバックとして残す。

## 参考: 先行 Windows 実装（fkunn1326/azooKey-Windows, MIT）による R1 実証

先行 Windows 実装は、本書のルート A / R1（GGUF/ggml + llama.cpp）を実働で実証している。

- 変換エンジンは azooKey の Swift `AzooKeyKanaKanjiConverter` を Windows ビルドし、
  `zenz.gguf`（`Miwa-Keita/zenz-v3.x-small-gguf`）を `ConvertRequestOptions.zenzaiMode.on`
  に渡す。重み形式が GGUF である点は本書の判断と一致する。
- llama.cpp を **CPU / CUDA / Vulkan の 3 プリビルド**で同梱し、`launcher` が設定値に
  応じて該当フォルダを PATH 先頭へ注入して切り替える。**Vulkan(ggml-vulkan) も実働**
  しており、`docs/copilot-pc-backend-spec.md` §4.4-§4.5 の R1 Vulkan 経路の実在性を裏付ける。
- 自分側は同じ R1 を **C++ `inference-host` から llama.cpp C-API 直結**で実装する点が
  参考と異なる（参考は Swift FFI 経由）。FFI を採る場合は **C 文字列の所有権・解放規約を
  別途設計**する必要がある（参考実装は `strdup` 戻り値を解放しておらずリーク懸念＝反面教師）。
- 関連: DEV-98（R1/R2 選定スパイク・コメント）/ DEV-202（zenz GGUF 配布ライセンス確認）。

## 配布ライセンス（DEV-202）

配布形態の正典は `docs/sideload-packaging-spec.md` §1.6 / §1.6.2。本節は GPU ルート
視点の要約を置く（**法的助言ではなく一次情報の整理**。v1.0 は保守運用で確定＝DEV-202 は
Done。著者確認を要するのは再ホスト解禁の任意最適化のみで、その追跡は DEV-497）。

- **モデル（zenz GGUF）**: ピンは `Miwa-Keita/zenz-v3.2-small-gguf`。HF タグは
  `apache-2.0` だが、zenz 一族（v1〜v3.1、safetensors 版含む）とベース
  `ku-nlp/gpt2-small-japanese-char` はすべて `cc-by-sa-4.0` で、CC-BY-SA-4.0 の
  ShareAlike と Apache-2.0 の非互換により apache タグは誤りの可能性がある。**当面は
  保守側に倒し CC-BY-SA-4.0 として扱う**（帰属＋量子化改変の明示。再ホストせず上流 HF
  から取得）。詳細と残作業（著者確認＝DEV-497）は spec §1.6.2。
- **CUDA（ルート A / R1 CUDA）**: `cudart64_*` / `cublas64_*` は CUDA Toolkit EULA
  Attachment A で再配布可。**optional add-on として同梱・再配布する**（NVIDIA 著作権
  表示の保持＋条項 pass-down を `ThirdPartyNotices.txt` に記載）。再配布 DLL は
  **アプリ専用ディレクトリに配置**し、共有 `PATH` / システムディレクトリへ入れない
  （EULA のアプリ限定アクセス条件。詳細は spec §1.6.2）。
- **Vulkan（R1 Vulkan）**: `ggml-vulkan` は llama.cpp（MIT）成果物。ローダ / ドライバは
  GPU ベンダ提供で同梱不要 → 固有の再配布義務は無い（最小リスク）。

## 参照

- 推論コントラクト（プロンプト整形・制約デコード・n-best・候補統合・性能予算）：
  `docs/zenzai-inference-spec.md`（本書は「どの実行系か」、同書は「どう推論するか」の正典）
- バックエンド選定の詳細：`docs/copilot-pc-backend-spec.md`
- mmap モデルロード：`docs/copilot-pc-backend-spec.md` §5
- 省電力モード連動：`docs/copilot-pc-backend-spec.md` §6
- ARM64 ビルド：`docs/copilot-pc-backend-spec.md` §8
