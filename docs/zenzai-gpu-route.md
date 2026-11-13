---
kind: design
canonical_for: ["Zenzai の重み形式と推論エンジン経路の結論、実装構成（AZOOKEY_BACKEND、llama.cpp 接続）、計測ゲート"]
---

# Zenzai 推論形式の特定とGPU化ルート決定

本書は Zenzai の重み形式と、それを実行するエンジン経路（**どの実行系か**）の正典である。
どう推論して候補を作るかは `docs/zenzai-inference-spec.md`、アクセラレータ選定と
フォールバック段位の詳細は `docs/copilot-pc-backend-spec.md` が正典であり、本書は
決定の結論、実装構成、計測ゲートを持つ。

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

Vulkan / CUDA / Windows ML / NPU の選定は
`InferenceEngine::LoadModel(path, backend, n_gpu_layers)` の境界より内側に閉じる。
エンジンを差し替えても TIP 側の契約は変わらない。

判断根拠（`legacy/` の重みの渡し方と submodule）と、先行 Windows 実装
（fkunn1326/azooKey-Windows）による R1 の実証は
[`decisions/2026-05-20-zenzai-engine-route.md`](decisions/2026-05-20-zenzai-engine-route.md) にある。

## フォールバック

- 降格（CUDA 要求の CPU ロード、Vulkan 失敗時の CPU 再試行）、ロード失敗時の
  `SimpleConverter` への劣化、そのときの Health は `docs/zenzai-inference-spec.md` §7.4 が正典。
  段位表は `docs/copilot-pc-backend-spec.md` §4.5。
- TIP はバックエンド差を意識せず、IPC レスポンスのみで処理する。

## 実装構成

- `AZOOKEY_BACKEND=cpu|cuda|vulkan` を CMake cache option として持つ。`BackendKind` は
  `{ Cpu, Cuda, Vulkan }` であり、`--backend directml` は unsupported として拒否する。
  `directml` は IPC payload と設定スキーマの予約文字列として残すが、enum 値には
  しない（enum 拡張ポリシーの正典は `docs/copilot-pc-backend-spec.md` §4.4）。
- `AZOOKEY_LLAMA_CPP_SOURCE_DIR` または `AZOOKEY_FETCH_LLAMA_CPP=ON` で llama.cpp
  `llama` target を接続し、`azookey_host` に `AZOOKEY_WITH_LLAMA_CPP` を伝播する。
  CPU 構成の未接続時は no-egress の mock runtime でビルド・テストを継続する。
  Vulkan 構成は実 llama target を必須とし、`windows-vulkan-release` preset でビルドする。
- `ZenzaiModelConverter` は GGUF magic / version を検証して
  `InferenceEngine::model_loaded()` / `Handshake` / `Health` に反映する。
- llama.cpp 接続時は `llama_model_load_from_file` / `llama_init_from_model` で GGUF を
  ロードし、`Convert` のモデル生成候補を `CandidateSource::Model` と
  `debug_info=zenzai;lp=...;avg=...` に写像する。
- モデル未配置・破損・推論例外・空生成時の劣化と、CUDA / Vulkan 指定時の CPU への降格は
  「フォールバック」節のとおり `docs/zenzai-inference-spec.md` §7.4 に従う。Vulkan 版の
  配布への組み込みと UI 露出条件は DEV-1001 で扱う。

実モデルを使う開発登録の手順（`windows-llama-debug` preset、`register-dev.ps1` の
`-ModelPath`、GGUF の配置先）は `README.md` を正典とする。GGUF を配置しただけでは
モデルを自動選択せず、`model.selectedPath` は確定しない。`-AllowMockHost` は
fallback-only の TIP テスト専用で llama.cpp preflight を迂回する経路であり、
`-ModelPath` とは併用できない（`scripts/register-dev.ps1` が併用を拒否する）。

## 計測ゲート

| 経路 | 実行 | ゲート |
|---|---|---|
| CPU fallback | `build/<preset>/bench/azookey_bench.exe` | exit=0、p95 < 50ms |
| Zenzai CPU | `build/<preset>/bench/azookey_zenzai_bench.exe --model <gguf> --require-model --require-zenzai`（または `AZOOKEY_ZENZAI_MODEL=<gguf>`） | `LoadModel` 成功 |
| Zenzai CUDA | 同上（`--backend cuda`） | 初期化失敗時に CPU または `SimpleConverter` へ降格 |
| Zenzai Vulkan | 同上（`--backend vulkan`） | GPU offload、失敗時の CPU 再試行と Health degraded を確認 |

記録するメトリクスの一覧は `docs/copilot-pc-backend-spec.md` §4.2 を正典とする。
モデル未指定時は `status=skipped` で成功終了し、CTest では `--mock-zenzai` で
no-egress smoke を行う。分位点の目標値は `docs/zenzai-inference-spec.md` §8 が正典。

実測 baseline は本書に書かない。bench の実行結果は Linear の該当課題へ記録する。

## 将来拡張

NPU / DirectML 系のアクセラレーションは Windows ML（R2）へ一本化する。R2 の前提と再開条件、
自動 EP 選択、R1 CUDA の位置付けは `docs/copilot-pc-backend-spec.md` §4.3 が正典であり、
enum 拡張ポリシーとフォールバック段位は同 §4.4・§4.5、電源状態による優先順位は同 §2 が持つ。

Zenzai 本体が ONNX/TensorRT に寄る更新をした場合は、Backend 抽象を追加して
ORT CUDA EP / TensorRT EP に切り替え可能にすることを検討する。GGUF を重視する間は範囲外とする。

## 配布ライセンス

モデル（zenz GGUF、kanakanji-edge-33m）・CUDA ランタイム・Vulkan の配布形態とライセンスの結論は
`docs/msix-packaging-spec.md` §1.6 / §1.6.2 が正典。CUDA の採否は
`docs/copilot-pc-backend-spec.md` §4.3 項 7 に従う。第三者資産の attribution の集約はルートの
`THIRD_PARTY_LICENSES`、運用規約は `docs/licensing-policy.md` が持つ。

## 参照

- 推論コントラクト（プロンプト整形・制約デコード・n-best・候補統合・性能予算）：
  `docs/zenzai-inference-spec.md`（本書は「どの実行系か」、同書は「どう推論するか」の正典）
- バックエンド選定・結論・フォールバック段位：`docs/copilot-pc-backend-spec.md` §3〜§4
- mmap モデルロード：`docs/copilot-pc-backend-spec.md` §5
- 省電力モード連動：`docs/copilot-pc-backend-spec.md` §6
- ARM64 ビルド：`docs/copilot-pc-backend-spec.md` §8
- 開発登録と実モデル検証の手順：`README.md`
- 決定の根拠と経緯：[`decisions/2026-05-20-zenzai-engine-route.md`](decisions/2026-05-20-zenzai-engine-route.md)
