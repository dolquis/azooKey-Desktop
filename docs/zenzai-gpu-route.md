# Zenzai推論形式の特定とGPU化ルート決定

## 調査結果（根拠）

- `Core/Sources/Core/InputUtils/SegmentsManager.swift` で `ConvertRequestOptions.ZenzaiMode.on` に `weight: ...ggml-model-Q5_K_M.gguf` を渡している。
- READMEでも `azooKeyMac/Resources/zenz-v3-small-gguf/ggml-model-Q5_K_M.gguf` (約70MB) が明記される。
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

## Phase C M8 着手前スパイク結果（2026-05-20）

Phase C の最初の実装単位は **llama.cpp C API + CPU backend** とし、CUDA は
CMake オプションで optional に追加する。理由は以下。

1. GGUF を直接ロードでき、既存 Zenzai 資産を変換せずに検証できる。
2. `core/IConverter` 差し替えで `SimpleConverter` フォールバックを維持しやすい。
3. CUDA / DirectML / NPU の選定を `InferenceEngine::LoadModel(path, backend,
   n_gpu_layers)` の境界より内側に閉じ込められる。

M8 では `BackendKind::Cpu` / `BackendKind::Cuda` だけを有効化する。
`directml` は IPC payload の予約値として残すが、Phase C では unsupported として
扱い、Phase 2-B M24 の DirectML / NPU スパイクで再度有効化する。

計測ゲート:

- CPU fallback: `bench/azookey_bench.exe` が exit=0、p95 < 50ms。
  2026-05-20 Debug build baseline: p50=0.0179ms, p95=0.0249ms, p99=0.052ms。
- Zenzai CPU: GGUF 配置時の `LoadModel` 成功、初回ロード時間、p50/p95。
- Zenzai CUDA: CUDA 初期化失敗時に CPU または `SimpleConverter` へフォールバック。

## 将来拡張

### DirectML EP ルート（Phase 2-B M24 採用予定）

ONNX 化を経ない経路として、以下のいずれかを Phase 2-B M24 で採用する。
M8 スパイクで配布サイズ / 初回起動時間 / レイテンシ / 量子化対応を比較して決定する。

候補:

1. **llama.cpp + DirectML backend** — GGUF をそのまま流用。配布サイズ最小。
   ベンダー横断（Intel / AMD / NVIDIA / Qualcomm）。NPU 対応は実験的。
2. **ONNX Runtime + DirectML EP** — 中間 ONNX 化が必要だが、NPU 対応が成熟。
   量子化 int4/int8 のサポートが厚い。配布サイズ中。

詳細選定の判断基準と DXCore 列挙アルゴリズムは
`docs/copilot-pc-backend-spec.md` §3〜§4 を参照。

### NPU ルート（Copilot+ PC）

Copilot+ PC（Snapdragon X Elite / Intel Meteor Lake+ / AMD XDNA）の NPU を
活用する経路として以下を比較対象に追加：

- **Qualcomm QNN SDK** — Snapdragon X Elite の Hexagon NPU。ARM64 専用、
  省電力で常時稼働に最適。
- **Intel OpenVINO** — Intel Core Ultra の NPU + iGPU + CPU を統合制御。
  配布サイズが大きい（〜100MB）が int4/int8 量子化が成熟。
- **AMD MIGraphX** — Ryzen AI 300 系の XDNA NPU。

Phase 2-B M24 で `BackendKind::NPU` として統合し、`BackendSelector` の優先順位
は **NPU > DirectML > CUDA > CPU**（バッテリ駆動時は **NPU > CPU > DirectML
> CUDA**）とする。

### ONNX/TensorRT ルート（保留）

Zenzai 本体が ONNX/TensorRT に寄る更新をした場合、Backend 抽象を追加して
ORT CUDA EP / TensorRT EP に切り替え可能にする。
現状は GGUF 重視のため M24 着手時点では Out-of-Scope。

### 既存「ルート A」の位置付け

**ルート A（GGUF/ggml + ggml-cuda）は NVIDIA GPU 環境専用**。AC 接続デスクトップ
で最速だが、ノート PC のバッテリ消費が激しい。Phase 2-B M24 完了後は
`BackendSelector` の優先順位下位（CPU の手前）にフォールバックとして残す。

## 参照

- バックエンド選定の詳細：`docs/copilot-pc-backend-spec.md`
- mmap モデルロード：`docs/copilot-pc-backend-spec.md` §5
- 省電力モード連動：`docs/copilot-pc-backend-spec.md` §6
- ARM64 ビルド：`docs/copilot-pc-backend-spec.md` §8
