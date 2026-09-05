# Zenzai 推論形式の特定とGPU化ルート決定

本書は Zenzai の重み形式と、それを実行するエンジン経路（**どの実行系か**）の正典である。
どう推論して候補を作るかは `docs/zenzai-inference-spec.md`、アクセラレータ選定と
フォールバック段位の詳細は `docs/copilot-pc-backend-spec.md` が正典であり、本書は
決定の要旨と根拠だけを持つ。

## 決定（ADR 要旨）

1. **重み形式は GGUF / ggml 系**とする。
2. **推論エンジンは R1（llama.cpp C-API 直結）**を採る。GGUF を無変換でロードでき、
   `core::IConverter` 差し替えで `SimpleConverter` フォールバックを維持できる。
3. **既定アクセラレータは CPU** とする。`AZOOKEY_BACKEND` は `cpu`（既定）と `cuda` を
   取り、CUDA は CMake オプションによる optional 経路である。
4. **DirectML 単体 backend は採らない**。ベンダ横断 GPU は R1 = ggml-vulkan、DirectML
   由来の実行は R2 = Windows ML に集約する。

決定 2〜4 の実証結果と、R2（Windows ML）を保留とする理由は
`docs/copilot-pc-backend-spec.md` §4.3 が正典。アクセラレータ要求が降格したときの
Health の扱いは同 §4.4 を正典とする。

## 判断根拠

- `legacy/Core/Sources/Core/InputUtils/SegmentsManager.swift` が
  `ConvertRequestOptions.ZenzaiMode.on` に `weight: ...gguf` を渡す。
- 重みは submodule `zenz-v3.2-small-gguf`（`.gitmodules`、
  `https://huggingface.co/Miwa-Keita/zenz-v3.2-small-gguf`）を
  `legacy/azooKeyMac/Resources/gguf/` にマウントして供給する。
- 依存は `AzooKeyKanaKanjiConverter` だが、呼び出し側の重み形式は GGUF である。
- CUDA / Windows ML / NPU の選定は
  `InferenceEngine::LoadModel(path, backend, n_gpu_layers)` の境界より内側に閉じるため、
  エンジンを差し替えても TIP 側の契約は変わらない。

## フォールバック

- CUDA 初期化失敗・GPU なしの場合は同一 Host API で CPU 実行する。
- TIP はバックエンド差を意識せず、IPC レスポンスのみで処理する。
- 終端は常に R1 CPU（GGUF）であり、入力をブロックしない。段位表は
  `docs/copilot-pc-backend-spec.md` §4.5。

## 実装構成

- `AZOOKEY_BACKEND=cpu|cuda` を CMake cache option として持つ。`BackendKind` は
  `{ Cpu, Cuda }` であり、`--backend directml` は unsupported として拒否する。
  `directml` は IPC payload と設定スキーマの予約文字列として残すが、enum 値には
  しない（enum 拡張ポリシーの正典は `docs/copilot-pc-backend-spec.md` §4.4）。
- `AZOOKEY_LLAMA_CPP_SOURCE_DIR` または `AZOOKEY_FETCH_LLAMA_CPP=ON` で llama.cpp
  `llama` target を接続し、`azookey_host` に `AZOOKEY_WITH_LLAMA_CPP` を伝播する。
  未接続時は no-egress の mock runtime でビルド・テストを継続する。
- `ZenzaiModelConverter` は GGUF magic / version を検証して
  `InferenceEngine::model_loaded()` / `Handshake` / `Health` に反映する。
- llama.cpp 接続時は `llama_model_load_from_file` / `llama_init_from_model` で GGUF を
  ロードし、`Convert` のモデル生成候補を `CandidateSource::Model` と
  `debug_info=zenzai;lp=...;avg=...` に写像する。
- モデル未配置・破損・推論例外・空生成時は既存 converter に劣化し、候補に
  `zenzai-degraded:<reason>` を付与する。
- CUDA 指定時は valid GGUF を CPU fallback としてロードし、降格理由は
  `ModelLoadResult.error` の警告として返す。CPU ロード自体が成功した場合、`Health` は
  ok を保つ。

実モデルを使う開発登録の手順（`windows-llama-debug` preset、`register-dev.ps1` の
`-ModelPath`、GGUF の配置先）は `README.md` を正典とする。GGUF を配置しただけでは
モデルを自動選択せず、`model.selectedPath` は確定しない。`-AllowMockHost` は
fallback-only の TIP テスト専用で llama.cpp preflight を迂回する経路であり、
`-ModelPath` とは併用できない（`scripts/register-dev.ps1` が併用を拒否する）。

## 計測ゲート

| 経路 | 実行 | ゲート |
|---|---|---|
| CPU fallback | `bench/azookey_bench.exe` | exit=0、p95 < 50ms |
| Zenzai CPU | `bench/azookey_zenzai_bench.exe --model <gguf> --require-model --require-zenzai`（または `AZOOKEY_ZENZAI_MODEL=<gguf>`） | `LoadModel` 成功 |
| Zenzai CUDA | 同上（`--backend cuda`） | 初期化失敗時に CPU または `SimpleConverter` へ降格 |

記録するメトリクスの一覧は `docs/copilot-pc-backend-spec.md` §4.2 を正典とする。
モデル未指定時は `status=skipped` で成功終了し、CTest では `--mock-zenzai` で
no-egress smoke を行う。分位点の目標値は `docs/zenzai-inference-spec.md` §8 が正典。

実測 baseline は本書に書かない。bench の実行結果は Linear の該当課題へ記録する。

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

- **前提**: zenz-v3 を ONNX Runtime GenAI 形式へ変換できること。現行 model builder に
  GPT-2 変換経路が無いため R2 artifact を作れず、R2 は保留である（`docs/copilot-pc-backend-spec.md` §4.2.2 / §4.3）。
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

**R1 CUDA（GGUF/ggml + ggml-cuda）は NVIDIA GPU 環境専用**である。AC 接続デスクトップ
で最速だが、ノート PC のバッテリ消費が激しい。`BackendSelector` の優先順位下位
（CPU の手前）にフォールバックとして置く。

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

## 配布ライセンス

配布形態とライセンス結論の正典は `docs/sideload-packaging-spec.md` §1.6 / §1.6.2。
本節は GPU ルート視点の要約を置く（**法的助言ではなく一次情報の整理**）。

- **モデル（zenz GGUF）**: ピンは `Miwa-Keita/zenz-v3.2-small-gguf`。HF タグは
  `apache-2.0` だが、zenz 一族（v1〜v3.1、safetensors 版含む）とベース
  `ku-nlp/gpt2-small-japanese-char` はすべて `cc-by-sa-4.0` で、CC-BY-SA-4.0 の
  ShareAlike と Apache-2.0 の非互換により apache タグは誤りの可能性がある。**保守側に
  倒し CC-BY-SA-4.0 として扱う**（帰属＋量子化改変の明示。再ホストせず上流 HF から取得）。
- **CUDA（R1 CUDA）**: `cudart64_*` / `cublas64_*` は CUDA Toolkit EULA
  Attachment A で再配布可。**optional add-on として同梱・再配布する**（NVIDIA 著作権
  表示の保持＋条項 pass-down を `ThirdPartyNotices.txt` に記載）。再配布 DLL は
  **アプリ専用ディレクトリに配置**し、共有 `PATH` / システムディレクトリへ入れない
  （EULA のアプリ限定アクセス条件）。
- **Vulkan（R1 Vulkan）**: `ggml-vulkan` は llama.cpp（MIT）成果物。ローダ / ドライバは
  GPU ベンダ提供で同梱不要のため、固有の再配布義務は無い。

## 参照

- 推論コントラクト（プロンプト整形・制約デコード・n-best・候補統合・性能予算）：
  `docs/zenzai-inference-spec.md`（本書は「どの実行系か」、同書は「どう推論するか」の正典）
- バックエンド選定・結論・フォールバック段位：`docs/copilot-pc-backend-spec.md` §3〜§4
- mmap モデルロード：`docs/copilot-pc-backend-spec.md` §5
- 省電力モード連動：`docs/copilot-pc-backend-spec.md` §6
- ARM64 ビルド：`docs/copilot-pc-backend-spec.md` §8
- 開発登録と実モデル検証の手順：`README.md`
