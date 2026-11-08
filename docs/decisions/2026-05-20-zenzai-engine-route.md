# Zenzai の重み形式と推論エンジン経路

- 種別: 決定記録
- 日付: 2026-05-20（決定 1・2）。決定 3・4 は M24 の結論を反映した後の要旨で、原記録に日付が無い
- 関連 Issue: なし（roadmap M8 の決定記録）
- 結論の正典: [`zenzai-gpu-route.md`](../zenzai-gpu-route.md)「決定（ADR 要旨）」、[`copilot-pc-backend-spec.md`](../copilot-pc-backend-spec.md) §4.3
- 移設元: `docs/zenzai-gpu-route.md`「決定（ADR 要旨）」「判断根拠」「将来拡張」「参考: 先行 Windows 実装」（commit `c8d9a5b` 時点）。原文をそのまま移設した

ファイル名の日付は `plans/windows-port-roadmap.md` M8「決定記録（2026-05-20）」の llama.cpp バインディング選定による。本文中の「本書」は移設元の `docs/zenzai-gpu-route.md` を指す。

## 決定（ADR 要旨）

1. **重み形式は GGUF / ggml 系**とする。
2. **推論エンジンは R1（llama.cpp C-API 直結）**を採る。GGUF を無変換でロードでき、
   `core::IConverter` 差し替えで `SimpleConverter` フォールバックを維持できる。
3. **既定アクセラレータは CPU** とする。`AZOOKEY_BACKEND` は `cpu`（既定）、`cuda`、
   `vulkan` を取る。Vulkan 構成は ggml-vulkan をリンクし、Host の既定値も Vulkan とする。
   CUDA 指定は CPU fallback の警告を返す予約経路とする。
4. **DirectML 単体 backend は採らない**。ベンダ横断 GPU は R1 = ggml-vulkan、DirectML
   由来の実行は R2 = Windows ML に集約する。

決定 2〜4 の実証結果と、R2（Windows ML）の再開条件は
`docs/copilot-pc-backend-spec.md` §4.3 が正典。アクセラレータ要求が降格したときの
Health の扱いは `docs/zenzai-inference-spec.md` §7.4 を正典とする。

## 判断根拠

- `legacy/Core/Sources/Core/InputUtils/SegmentsManager.swift` が
  `ConvertRequestOptions.ZenzaiMode.on` に `weight: ...gguf` を渡す。
- 重みは submodule `zenz-v3.2-small-gguf`（`.gitmodules`、
  `https://huggingface.co/Miwa-Keita/zenz-v3.2-small-gguf`）を
  `legacy/azooKeyMac/Resources/gguf/` にマウントして供給する。
- 依存は `AzooKeyKanaKanjiConverter` だが、呼び出し側の重み形式は GGUF である。
- Vulkan / CUDA / Windows ML / NPU の選定は
  `InferenceEngine::LoadModel(path, backend, n_gpu_layers)` の境界より内側に閉じるため、
  エンジンを差し替えても TIP 側の契約は変わらない。

## 将来拡張

> **正典は `docs/copilot-pc-backend-spec.md` §4.3**: DirectML は Microsoft により
> sustained engineering（新規開発停止）となったため、NPU / DirectML 系
> アクセラレーションは **Windows ML（ORT ベース・EP 自動配信／自動選択）へ一本化**する。
> ベンダ別 SDK（QNN / OpenVINO / MIGraphX）の個別バンドルは行わない。NPU / DirectML 系は
> 独立 `BackendKind` 値ではなく `WinML` エンジン値で表し、具体 EP は Windows ML に委ねる。

### Windows ML ルート（R2）

ONNX Runtime GenAI + Windows ML により、ベンダ横断で NPU / GPU / CPU を自動選択する。
DirectML を直接叩く旧 2 候補（llama.cpp + DirectML backend / ORT + DirectML EP）は、
前者を不採用、後者を Windows ML へ統合した。

- **前提**: R1 と同じモデルを ONNX Runtime GenAI 形式へ変換できること。zenz-v3（GPT-2）は
  model builder の対象外で変換できない。Llama 構造の `kanakanji-edge-33m` は変換できるので、
  このモデルを既定に採ったときに R2 を再開する（`docs/copilot-pc-backend-spec.md` §4.2.2・
  §4.2.3・§4.3 項 2、`docs/zenzai-inference-spec.md` §2.4）。
- **アクセラレータ選択**: Windows ML が NPU（QNN / OpenVINO / VitisAI）→ GPU
  （NvTensorRtRtx / OpenVINO）→ CPU の順で自動選択・自動フォールバックする。GenAI 経路の
  ため MIGraphX（AMD GPU）は除外する（`docs/copilot-pc-backend-spec.md` §4.1 注記）。
- **配布**: EP は Windows Update 経由で配信され MSIX に同梱しない。

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
ORT CUDA EP / TensorRT EP に切り替え可能にする。現状は GGUF 重視のため Out-of-Scope。

### R1 CUDA の位置付け

**R1 CUDA（GGUF/ggml + ggml-cuda）は NVIDIA GPU 環境専用**である。実リンクは起票し直さず、
再検討の条件は `docs/copilot-pc-backend-spec.md` §4.3 項 7 に従う。AC 接続デスクトップ
で最速だが、ノート PC のバッテリ消費が激しい。`BackendSelector` の `auto` は `cuda` を選ばない
（`docs/copilot-pc-backend-spec.md` §4.4「`auto` の解決（BackendSelector）」）。再検討の結果
実リンクする場合は、優先順位下位（CPU の手前）にフォールバックとして置く。

## 参考: 先行 Windows 実装（fkunn1326/azooKey-Windows, MIT）による R1 実証

先行 Windows 実装は、本書の R1（GGUF/ggml + llama.cpp）を実働で実証している。

- 変換エンジンは azooKey の Swift `AzooKeyKanaKanjiConverter` を Windows ビルドし、
  `zenz.gguf`（`Miwa-Keita/zenz-v3.x-small-gguf`）を `ConvertRequestOptions.zenzaiMode.on`
  に渡す。重み形式が GGUF である点は本書の判断と一致する。
- llama.cpp を **CPU / CUDA / Vulkan の 3 プリビルド**で同梱し、`launcher` が設定値に
  応じて該当フォルダを PATH 先頭へ注入して切り替える。**Vulkan(ggml-vulkan) も実働**
  しており、`docs/copilot-pc-backend-spec.md` §4.4-§4.5 の R1 Vulkan 経路の実在性を裏付ける。
- 自分側は同じ R1 を **C++ `inference-host` から llama.cpp C-API 直結**で実装する点が
  参考と異なる（参考は Swift FFI 経由）。FFI を採る場合は **C 文字列の所有権・解放規約を
  別途設計**する必要がある（参考実装は `strdup` 戻り値を解放しておらずリーク懸念＝反面教師）。
