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

2026-05-23 時点の初期実装:

- `AZOOKEY_BACKEND=cpu|cuda` を CMake cache option として追加。
- `ZenzaiModelConverter` を追加し、GGUF magic/version を検証して
  `InferenceEngine::model_loaded()` / `Handshake` / `Health` に反映する。
- llama.cpp 未接続の間は既存 converter に委譲し、候補には
  `zenzai-gguf-loaded;fallback-converter` を付与する。
- CUDA 指定時は valid GGUF を CPU fallback としてロードし、`Health` を
  `degraded` にして `last_error` で理由を返す。

計測ゲート:

- CPU fallback: `bench/azookey_bench.exe` が exit=0、p95 < 50ms。
  2026-05-20 Debug build baseline: p50=0.0179ms, p95=0.0249ms, p99=0.052ms。
- Zenzai CPU: GGUF 配置時の `LoadModel` 成功、初回ロード時間、p50/p95。
- Zenzai CUDA: CUDA 初期化失敗時に CPU または `SimpleConverter` へフォールバック。

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

## 参照

- バックエンド選定の詳細：`docs/copilot-pc-backend-spec.md`
- mmap モデルロード：`docs/copilot-pc-backend-spec.md` §5
- 省電力モード連動：`docs/copilot-pc-backend-spec.md` §6
- ARM64 ビルド：`docs/copilot-pc-backend-spec.md` §8
