# Copilot+ PC / NPU バックエンド 仕様（Phase 6-B）

本書は `docs/zenzai-gpu-route.md` の発展形として、DirectML / NPU / ARM64
バックエンドの選定と実装方針を定める。`plans/windows-port-roadmap.md` の
Phase 6 の M24〜M27 が本書を参照する。

## 1. BackendKind enum 拡張

`inference-host/include/azookey/host/InferenceEngine.h`：

```cpp
enum class BackendKind : uint8_t {
    CPU      = 0,   // 既定。llama.cpp 純 CPU
    CUDA     = 1,   // NVIDIA GPU (ggml-cuda)
    DirectML = 2,   // Windows DirectML (Intel/AMD/NVIDIA 横断)
    NPU      = 3,   // NPU 経由 (Qualcomm QNN / Intel OpenVINO / AMD VitisAI)
};

struct BackendInfo {
    BackendKind kind;
    std::string device_name;      // "NVIDIA GeForce RTX 4090", "Snapdragon X NPU" 等
    uint64_t    available_memory_bytes;
    bool        is_battery_efficient;  // NPU=true、CUDA=false
};
```

> **注（M24 決定で更新、§4.4 が正典）**: 上の 4 値 enum（CPU/CUDA/DirectML/NPU）は
> 初期スケッチであり、最終的な enum 拡張は §4.4 のポリシーに従う。DirectML が
> sustained engineering となったため、NPU / DirectML 系アクセラレーションは個別
> enum 値ではなく **`WinML` エンジン値（具体 EP は Windows ML が自動選択）**で表現し、
> `DirectML` / `NPU` を独立値として実装しない。R1 の enum は `{ Cpu, Cuda, Vulkan }`
> （llama.cpp）であり、拡張は後方互換の追記のみで行う。

## 2. 自動選択優先度

> **注（M24 決定）**: 下記の優先順位は「アクセラレータ種別」の概念的順位である。
> 実装では R2（`WinML` エンジン）内の NPU→GPU→CPU 自動選択は **Windows ML の EP
> 自動選択に委ね**、`BackendSelector` は「R2(auto) を要求するか / R1 のどの
> アクセラレータを要求するか」と電源状態の判断のみを担う。詳細な降格段位は §4.5。

### 2.1 既定優先順位（概念）

正典の降格段位は §4.5。概念的な優先順は次のとおり（`DirectML` は legacy のため
**R2=Windows ML（EP 自動選択）** に置換し、ベンダ横断 GPU は R1=`Vulkan`）:

```
R2(NPU) > R2/R1(GPU) > R1(CPU)
```

理由：
- NPU は省電力で常時稼働に最適（R2 / Windows ML）
- GPU は高スループット（R2 GPU EP / R1 CUDA(NVIDIA) / R1 Vulkan(ベンダ横断)）
- CPU は最終フォールバック（R1）

### 2.2 バッテリ駆動時の逆転

`SystemPowerStatus.ACLineStatus == 0`（バッテリ駆動）のとき、discrete GPU を避ける:

```
R2(NPU) > R1(CPU)   （GPU = R2 GPU device / R1 CUDA / R1 Vulkan は回避）
```

NPU は省電力なので最優先のまま。GPU はバッテリを激しく消費するため使わない。
バッテリ時の R2 は **NPU device のみ**に絞る（§4.6 のデバイスレベル選択）。詳細は §4.5。

### 2.3 BackendSelector

> **注**: 下のスケッチは初期案である。R1 の `auto` 解決の実装と interface は §4.4
> 「`auto` の解決（BackendSelector）」が正典。

`inference-host/src/BackendSelector.h` / `.cpp`（新規）：

```cpp
class BackendSelector {
public:
    // 利用可能なバックエンドを優先順で列挙
    std::vector<BackendInfo> Enumerate();

    // 設定 `backendPreference` + バッテリ状態から最適バックエンドを選択
    BackendInfo Select(std::string_view user_preference);

    // SystemPowerStatus 変化を監視
    void OnPowerStatusChange(BOOL ac_online);

private:
    std::vector<BackendInfo> cached_;
    BOOL                     ac_online_ = TRUE;
};
```

## 3. DXCore 列挙アルゴリズム

DirectML / NPU の対応 GPU/NPU を列挙する。

### 3.1 DXCore で列挙

```cpp
#include <dxcore.h>

Microsoft::WRL::ComPtr<IDXCoreAdapterFactory> factory;
DXCoreCreateAdapterFactory(IID_PPV_ARGS(&factory));

Microsoft::WRL::ComPtr<IDXCoreAdapterList> adapter_list;

// D3D12_GENERIC_ML 属性を持つアダプタ（NPU を含む）
const GUID attrs[] = { DXCORE_ADAPTER_ATTRIBUTE_D3D12_GENERIC_ML };
factory->CreateAdapterList(_countof(attrs), attrs, IID_PPV_ARGS(&adapter_list));

for (uint32_t i = 0; i < adapter_list->GetAdapterCount(); ++i) {
    Microsoft::WRL::ComPtr<IDXCoreAdapter> adapter;
    adapter_list->GetAdapter(i, IID_PPV_ARGS(&adapter));

    char desc[256];
    adapter->GetProperty(DXCoreAdapterProperty::DriverDescription, sizeof(desc), desc);

    bool is_npu = adapter->IsAttributeSupported(DXCORE_ADAPTER_ATTRIBUTE_D3D12_GENERIC_ML);
    bool is_gpu = adapter->IsAttributeSupported(DXCORE_ADAPTER_ATTRIBUTE_D3D12_GRAPHICS);

    // NPU 専用: !is_gpu && is_npu
    // 統合 GPU: is_gpu
}
```

### 3.2 NPU 判定の追加検査

- Snapdragon X Elite: `DriverDescription` に `"Qualcomm"` を含む + GPU 属性なし
- Intel Meteor Lake+ NPU: `"Intel(R) AI Boost"`
- AMD Ryzen AI: `"AMD XDNA"`

これらは `BackendSelector::IsNpuAdapter(IDXCoreAdapter*)` 内で判定。

> **注**: 本節と §3.1 は R2 再開時の NPU 判定の初期案である。R1 の `auto` 解決が使う列挙は
> §4.4「`auto` の解決（BackendSelector）」が正典で、NPU は `D3D12_GENERIC_ML` のみを持つ
> ハードウェアアダプタとして数えるだけに留める。

## 4. 推論エンジン × アクセラレータの選定（M8 / M24）

> **重要（2026 時点の Windows AI 指針）**: Microsoft は **DirectML を sustained
> engineering（保守のみ・新規機能開発停止）** とし、NPU / GPU アクセラレーションの
> 推奨経路を **Windows ML**（ONNX Runtime ベース、Execution Provider を Windows
> Update 経由で自動配信・自動選択）へ移行した。よって本書の旧 4 候補表（DirectML
> EP を前提）は前提が変わった。以下では「エンジン軸」と「アクセラレータ軸」を分離して
> 再定義する。
> 出典: [Get Started with DirectML（sustained engineering 注記）](https://learn.microsoft.com/windows/ai/directml/dml-get-started)、
> [What is Windows ML](https://learn.microsoft.com/windows/ai/new-windows-ml/overview)、
> [Accelerate AI models — Silicon-to-EP mapping](https://learn.microsoft.com/windows/ai/new-windows-ml/accelerate-ai-models#silicon-to-ep-mapping)、
> [Develop AI applications for Copilot+ PCs](https://learn.microsoft.com/windows/ai/npu-devices/)。

### 4.1 2 つのエンジン経路

| 軸 | エンジン | モデル形式 | アクセラレータ | NPU | EP 配布 |
|---|---|---|---|---|---|
| **R1** | llama.cpp C-API | GGUF（既存 zenz-v3 資産） | CPU（既定）/ CUDA（ggml-cuda, NVIDIA）/ Vulkan（ggml-vulkan, ベンダ横断 GPU） | ✕（実用外） | 自前バンドル |
| **R2** | ONNX Runtime GenAI + Windows ML | ONNX Runtime GenAI 形式（**要変換**。R1 と同じモデルの変換物、§4.3） | Windows ML が自動選択（NPU: QNN/OpenVINO/VitisAI、GPU: NvTensorRtRtx/OpenVINO、CPU: ORT） | ◎（Copilot+ PC） | Windows Update 配信（**非バンドル**） |

> **注（GenAI 対応 EP）**: R2 は GenAI（LLM）経路のため、**MIGraphX(AMD GPU) は除外**する。
> Windows ML の EP 仕様で `MIGraphXExecutionProvider` は現状 *GenAI シナリオ未対応*と
> 明記されている（[Windows ML execution providers](https://learn.microsoft.com/windows/ai/new-windows-ml/supported-execution-providers)）。
> AMD は NPU=VitisAI（Ryzen AI）で扱い、GPU GenAI は Microsoft が MIGraphX の GenAI 対応を
> 有効化した時点で再評価する。それまで AMD GPU 環境は R1（ggml-cuda は NVIDIA 専用のため
> 実質 ggml-vulkan）または R1 CPU にフォールバックする。

- 旧候補 **A（llama.cpp + DirectML backend）は不採用**。ggml の DirectML backend は
  保守経路でなく、ベンダ横断 GPU は R1 では **ggml-vulkan** に集約する。
- 旧候補 **B/C/D（ORT+DirectML EP / QNN SDK / OpenVINO Runtime の個別バンドル）は
  Windows ML（R2）の自動 EP 配信へ統合**し、ベンダ別 SDK は同梱しない。これにより
  MSIX サイズ肥大（旧 C/D の懸念）と EP 保守コストを回避する。

### 4.2 計測スパイク（bench）

R1 llama.cpp / CPU の実測は `bench/zenzai_bench.cpp`
（生成物 `azookey_zenzai_bench`）で行う。R2（Windows ML）比較スパイクでは、
この CLI と同じ入力セット・出力メトリクスに揃えて横断計測する。

- 同一プロンプト「こんにちは」「にほんご」「きょうはいいてんきです」等 20 件を各経路で実行。
- R1 メトリクス: 初回 `LoadModel` 時間 / P50・P95・P99 推論レイテンシ /
  `zenzai_candidates` / 先頭候補 `debug_info` / `requested_backend` /
  `effective_backend` / `load_warning`。実行は
  `azookey_zenzai_bench --model <gguf>` または `AZOOKEY_ZENZAI_MODEL=<gguf>`。
- R2 比較メトリクス: 初回 LoadModel 時間 / P50 推論レイテンシ / RSS / 実効配布サイズ。
- 判定ゲート（重み順）:
  1. P50 推論レイテンシ < 30ms
  2. 実効配布サイズ < 50MB（EP 非バンドル前提で R2 が有利）
  3. 初回 LoadModel 時間 < 3 秒
  4. ARM64 / NPU 可用性
- **上記 4 ゲートは R1 / R2 を横断比較するための相対ゲートであり、R1 単独の
  出荷可否は判定しない。** 両経路を同一条件で計測して優劣を決めるための基準であって、
  片方だけを測って合否を出す性格の数値ではない。**R2 artifact が存在しない間
  （§4.2.2）は、本ゲートに対する R1 CPU の成否を出荷判断に用いない。** R1 CPU が
  出荷可能かどうかの正典は `zenzai-inference-spec` §8 の性能予算であり、本ゲートとは
  別系統の数値である（§8 は絶対予算、本ゲートは相対比較）。
- **比較の対象**: 相対ゲートは、同じモデルの R1 artifact（GGUF）と R2 artifact
  （ONNX Runtime GenAI 形式）を比べる。R2 のモデルは §4.3 項 2 のとおり R1 と同じモデル
  であり、対象は `kanakanji-edge-33m`（§4.2.3）とする。zenz-v3 の R1 baseline（§4.2.1）は
  R1 CPU の推移を見る記録であり、R2 との比較には使わない。
- **R2 変換可否の判定**: R2 は対象モデルを **ONNX Runtime GenAI 形式へ
  変換できること**が前提。Foundry Toolkit の変換は Preview かつ対応モデルが限定列挙
  （Phi / Qwen / Llama-3.2-1B / DeepSeek-distill）で、**zenz-v3（独自小型 JP モデル）は
  turn-key 変換対象外**。ORT GenAI model builder による手動変換は、zenz-v3（GPT-2）では
  不可（§4.2.2）、Llama 構造の `kanakanji-edge-33m` では可（§4.2.3）と判定した。出典:
  [Run LLMs and other generative models（ONNX Runtime GenAI / Windows ML）](https://learn.microsoft.com/windows/ai/new-windows-ml/run-genai-onnx-models)、
  [Use any ONNX LLM in the AI Dev Gallery（変換対応モデル列挙）](https://learn.microsoft.com/windows/ai/ai-dev-gallery/tutorial-onnx)。
- `gate:human-required` の DEV-194 の検証範囲のうち、R1 CPU の実機計測は完了した。
  Snapdragon X Elite / Intel Core Ultra の NPU / GPU EP の計測は、§4.3 項 2 の再開条件を
  満たした R2 artifact で、同じ入力セットを使って Human Gate（DEV-1720）が行う。

#### 4.2.1 R1 CPU 実モデル baseline

DEV-194 では、次の固定条件で R1 CPU を計測した。

- リポジトリ: commit `6abce8d2595a05c4358dacf459b691b3cc946d04`
- OS / CPU: Windows build 26200.9168 / Intel Core i7-12700KF / x64 / NPU なし
- build: `windows-release`、`llama_cpp=1`、`requested_backend=cpu`、
  `effective_backend=cpu`
- model: `Miwa-Keita/zenz-v3.2-small-gguf` の `ggml-model-Q5_K_M.gguf`、
  73,871,936 bytes（70.45MiB）、SHA-256
  `29c223d4c23327b80fd13ebb5ab2555057a46317997d5da391584ffbef0db673`
- 各入力を別プロセスで実行し、warm-up 3 回、計測 20 回とした。

入力セットは次の 20 件である。

```text
こんにちは / にほんご / きょうはいいてんきです / わたしはがくせいです
よろしくおねがいします / ありがとうございます / きょうのかいぎ / めーるをかく
あしたのよてい / とうきょうえき / おーぷんえーあい / ぷろぐらみんぐ
へんかんこうほ / にほんごにゅうりょく / このぶんしょうをへんかん
しゅうまつのてんき / でんしゃにのる / しごとをはじめる
せっていをひらく / ぱそこんをさいきどうする
```

| 指標 | 結果 |
|---|---|
| 初回 `LoadModel` | 中央値 22.80ms |
| 入力別 p50 | 中央値 501.38ms、範囲 196.02–810.19ms |
| p50 < 30ms | 0 / 20 入力 |
| `にほんご` 50 回 | p50 197.03ms / p95 210.73ms / p99 215.37ms |
| peak RSS | 122.52MiB（`にほんご`、warm-up 3 回 + 計測 50 回の代表値） |
| 実推論 | 20 / 20 入力で exit 0、`zenzai_candidates=4` |

この計測時点では、ロード時間は 3 秒ゲートを満たし、推論 p50 は 20 入力すべてで
30ms を上回った。また、モデル単体が 50MB を超えるため、R1 の配布サイズは 50MB
ゲートを満たさない。いずれも §4.2 の通り相対ゲートに対する値であり、R1 CPU の
出荷可否を示すものではない。この結果は R1 CPU baseline であり、R2 の性能を
推定する根拠にはしない。R1 CPU の絶対予算に対する評価は
`zenzai-inference-spec` §8 による。

なお本節の値は計測時点（commit `6abce8d2`）の記録であり、その後の最適化を
反映しない。以降の実測は §4.2.1.1 に追記する。

##### 4.2.1.1 prompt KV 再利用後の再計測

DEV-853 の prompt KV cache 再利用（PR #295）後に、§4.2.1 と同じ入力セット・
同じ計測プロトコル（別プロセス実行、warm-up 3 回）で再計測した。build 構成と
model は §4.2.1 と同一である。計測環境（OS build / CPU）は DEV-853 の報告に
従い、本節では §4.2.1 との同一性を主張しない。

| 計測 | commit | 条件 | 結果 |
|---|---|---|---|
| `にほんご` 単体 | `36740c54` | 計測 50 回 | p50 79.11ms / p95 92.68ms / p99 106.87ms |
| decode phase 内訳 | `36740c54` | 同上 | prompt p50 10.22ms（samples 50）/ beam p50 66.84ms（samples 50、evaluations 850） |
| 20 入力 | `e594a420` | 各 計測 20 回 | 入力別 p50 中央値 209.24ms、範囲 97.92–436.01ms |

- `36740c54` は PR #295 のマージ対象 head（merge commit `9198f427`）である。
- 20 入力の系列は `e594a420`（PR head 初版）でのみ取得しており、レビュー反映後の
  `36740c54` では再取得していない。両者の差分はレビュー指摘対応であり、
  同一 commit 同士の比較ではない点に注意する。
- 20 入力セットは読み ≤ 8 文字と 8 文字超が混在する。混在集合の中央値は
  `zenzai-inference-spec` §8 の読み長別予算に直接対応しない（§8 参照）。

#### 4.2.2 zenz-v3 の R2 変換可否

2026-08-23 時点の ONNX Runtime GenAI model builder を、配布対象の
`Miwa-Keita/zenz-v3.2-small-gguf` に適用する経路はない。

1. 配布モデルは GPT-2 architecture の Q5_K_M GGUF であり、公開リポジトリにあるモデル
   本体は 73.9MB の当該ファイルだけである。
2. model builder の対応 architecture 一覧に GPT-2 はなく、`builder.py` に
   `GPT2LMHeadModel` の分岐もない。
   通常の変換要求は未対応モデルとして `NotImplementedError` になる。
3. model builder が入力として受け付ける GGUF は float16 / float32 に限られるため、
   Q5_K_M を変換元にはできない。
4. ONNX Runtime GenAI runtime 自体は `gpt2` model type を認識する。
   これは適合する ONNX graph と `genai_config.json` を実行できるという意味であり、
   model builder が zenz-v3 を出力できることは意味しない。

したがって、現行 model builder をそのまま使う手動変換は不可と判定する。
GPT-2 builder の新規実装、または別 exporter で作った ONNX graph と手書き
`genai_config.json` を検証する作業は、新しい推論 backend の実装に当たるため本スパイクの
範囲外とする。
zenz-v3 の変換済み artifact がないため、zenz-v3 での R2 の精度劣化、RSS、実効配布サイズ、
QNN / OpenVINO NPU レイテンシは計測できない。zenz-v3.1-small / xsmall には safetensors
（fp32）があるが、GPT-2 構造なので同じ制約を受ける。v3.2 は GGUF だけが公開されている。

根拠:

- [ONNX Runtime GenAI Model Builder: Current Support / GGUF Model](https://github.com/microsoft/onnxruntime-genai/blob/a0c04eb1313b886c963e415bbd974748cb89a4eb/src/python/py/models/README.md)
- [model builder の architecture 分岐](https://github.com/microsoft/onnxruntime-genai/blob/a0c04eb1313b886c963e415bbd974748cb89a4eb/src/python/py/models/builder.py)
- [runtime の model type 判定](https://github.com/microsoft/onnxruntime-genai/blob/a0c04eb1313b886c963e415bbd974748cb89a4eb/src/models/model_type.h)
- [Miwa-Keita/zenz-v3.2-small-gguf](https://huggingface.co/Miwa-Keita/zenz-v3.2-small-gguf/tree/c67e03e07d215c869f591b274c1631170d3e11fe)

#### 4.2.3 Llama 系互換モデルの R2 変換（DEV-1718）

model builder の対応 architecture には Llama と LFM2 MoE が含まれ、GPT-2 と Qwen3-Next は
含まれない（onnxruntime-genai `6b7ff3ca` の builder）。zenz-v3 のプロンプト形式で学習された
Llama 構造のかな漢字変換モデル `yuuki14202028/kanakanji-edge-33m`（33.5M パラメータ、
safetensors fp32）は、この builder で ONNX Runtime GenAI 形式へ変換できる。モデルそのものの
評価と、zenz-v3 とのプロンプト差の扱いは `docs/zenzai-inference-spec.md` §2.4 が正典である。

2026-10-25 の sandbox 調査（Linux、CPU 4 コア）で、`onnxruntime-genai` 0.18.0 の builder を
使い、CPU EP で AJIMEE-Bench 200 件を貪欲デコードした結果は次のとおりである。製品の
beam / n-best と読み制約は使っていない。

| 形式 | サイズ | Acc@1 | 備考 |
|---|---|---|---|
| fp32 | 129MB | 0.805 | llama.cpp f16 と 200 / 200 件で同じ出力 |
| int8 | 49MB | 0.805 | fp32 から劣化なし |
| int4（builder 既定） | — | 0.235 | 崩れる |
| int4（LM head 除外、shared embeddings 無効、block 32） | — | 0.755 | 原因は切り分けていない |
| fp16 | — | — | CPU EP では実行時エラー（GPU EP 向けの形式） |

この結果から、R2 で使う形式は次のとおりとする。

- CPU EP と GPU EP の第一候補は int8 とする。int4 は崩れの原因を切り分けるまで使わない。
  fp16 は GPU EP の候補として Human Gate の計測項目に残す。
- NPU EP（QNN / OpenVINO / VitisAI）は、静的 shape や QDQ 量子化など EP ごとの builder 設定を
  要する可能性がある。NPU で動く形式は Human Gate（DEV-1720）の計測で決める。
- `genai_config.json` と `tokenizer.json` は builder の出力を使う。ORT GenAI のトークナイザが
  プロンプトの制御文字（U+EE00〜U+EE02）を 1 トークンずつに解決すること（llama.cpp の
  `parse_special=true` と同じ結果になること）を、R2 の実装で確かめる。

変換の再現手順は次の形を取る。revision、builder のバージョン、評価スクリプトの固定は
R2 spike の Issue（follow-up）で repo に置く。

```text
pip install onnxruntime-genai==0.18.0
python -m onnxruntime_genai.models.builder \
  -m yuuki14202028/kanakanji-edge-33m -o <出力ディレクトリ> -p int8 -e cpu
```

### 4.3 結論（M24 実証結果の反映）

1. **v1.0 ベースライン = R1（llama.cpp / CPU）**とする。
   配布中の GGUF を無変換で使え、実モデルでロードと候補生成を確認できた。
2. **R2（Windows ML）の前提は「model builder が対応する architecture の互換モデル」とし、
   R1 と同じモデルを R2 でも使えることを再開の条件とする。**
   zenz-v3 は GPT-2 構造で builder の対象外（§4.2.2）なので、zenz-v3 を既定とする間は R2 を
   有効にしない。`kanakanji-edge-33m` は R2 artifact を作れる（§4.2.3）。このモデルを
   `docs/zenzai-inference-spec.md` §2.4 の判断基準で既定モデルに採ったときに、R2 を再開する。
   R1 と R2 で別のモデルを使うと、同じ入力でも機器によって候補の質が変わり、学習と
   NLL rerank の閾値もモデルごとに二重になるため、モデルが異なる構成は採らない。
3. 再開条件を満たす前は Snapdragon X Elite / Intel Core Ultra の NPU bench を実行しない。
   これは Windows ML や各 NPU の非対応を示す結果ではなく、R1 と同じモデルの R2 artifact が
   既定になっていないためである。
4. Copilot+ PC でも R1 CPU を既定とし、**NPU を必須化しない**。
   R2 を再開した後も、R2 の各 EP が §4.5 の降格順で使えないときは R1 CPU で動く。
5. 単体 DirectML backend（旧 A）は採らない。
   ベンダ横断 GPU を追加する場合は R1=ggml-vulkan、DirectML 由来の実行は
   R2=Windows ML に集約する。
6. Win11 24H2 (build 26100) 未満では、R2 を有効化した後も OS 判定で R1 CPU に
   フォールバックする。
7. **R1 CUDA（ggml-cuda の実リンク）は起票し直さない。**
   NVIDIA GPU は R1 Vulkan で使え、R2 の再開後は Windows ML の NvTensorRtRtx EP も候補になる。
   NVIDIA GPU での R1 Vulkan の実測が `zenzai-inference-spec` §8 の性能予算を満たさないと
   分かった場合に限り、CUDA を再検討する。

§4.4〜§4.6 は、R2 再開時に適用する設計とする。
再開条件を満たすまでは R1 の経路（CPU と Vulkan）だけを有効にする。

v1.0 設定 UI のデバイス選択は、この結論に合わせて `auto` / `cpu` の 2 値へ縮小する
（`docs/sideload-packaging-spec.md` §3.7 が正典）。`cuda` は §4.4 の降格が続くあいだ UI に出さず、
`vulkan` の露出条件は M24 ではなく、実行時選択経路の配線（DEV-944）と
ggml-vulkan ビルドの配布・起動保証（DEV-1001）とする。
`vulkan` は R1 に属するため、再開条件（§4.3 項 2）を満たす前の R2 のマイルストーンに従属させない。

> **参考（fkunn1326/azooKey-Windows, MIT）**: 先行 Windows 実装が R1（llama.cpp）を
> **CPU / CUDA / Vulkan の 3 プリビルド**で実働実証済み（Vulkan(ggml-vulkan) を含む）。
> R1 を v1.0 ベースラインとする本方針の外部裏付け。バックエンド可否の **一次フィルタ**
> として、当該バックエンドのランタイム DLL の存在判定（`cudart64_12.dll`+`cublas64_12.dll`
> = CUDA / `vulkan-1.dll` = Vulkan）を設定 UI の可否提示に流用できる（ただし DLL 存在 ≠
> 動作保証。版不一致・破損は検出不可のため、実選択は §3 DXCore 列挙 + `BackendSelector`
> に委ねる）。詳細は DEV-98 / DEV-120 のコメント参照。

### 4.4 BackendKind / LoadModel 境界の拡張ポリシー

R1 の enum は `enum class BackendKind { Cpu, Cuda, Vulkan };`
（`inference-host/include/azookey/host/InferenceEngine.h`、いずれも llama.cpp エンジン）。
拡張は**後方互換（既存シリアライズ値を不変・追記のみ）**で行う。

- R1 アクセラレータ: `Vulkan` は ggml-vulkan のデバイスへ解決する。
  `AZOOKEY_BACKEND=vulkan`（`windows-vulkan-release` preset）は ggml-vulkan と CPU を
  リンクし、Vulkan を `auto` の候補にする。`auto` の解決は下の「`auto` の解決
  （BackendSelector）」に従い、明示 `cpu` はデバイスリストと演算 offload を無効にして
  CPU へ固定する。
  `nGpuLayers` 省略時は Vulkan へ全層 offload を要求する。ビルドには Vulkan SDK が必要で、
  Vulkan 版の実行環境には Vulkan loader と対応ドライバーを要する。
- R2 エンジン追加: `WinML`（ONNX Runtime GenAI + Windows ML）を追加。**具体 EP
  （QNN / OpenVINO / VitisAI / …）は enum 値化せず**、Windows ML の自動選択に委ねる。
- `LoadModelRequest`（`ipc/include/azookey/ipc/Payloads.h`）に optional 予約 fields を追加:
  - `engine`: `"llama_cpp" | "winml"`（既定 `llama_cpp`、後方互換）
  - `ep_preference`: `"auto" | "npu" | "gpu" | "cpu"`（R2 のみ、既定 `auto`）
  - 既存 `n_gpu_layers` は R1（llama.cpp）専用として維持。
- 非対応の engine / EP 組合せは **fail-closed で R1 CPU に降格**する。Health 反映は
  **ロード成否と error 有無で決める**（`docs/zenzai-inference-spec.md` §9.2.1 と一致）:
  - **R1 CUDA 要求 → R1 CPU 降格**（CUDA 未配線の "for now"、§4.5）は**成功 LoadModel**。降格は
    `ModelLoadResult.error`（警告）で返すのみで `engine->last_error()` は空＝**`Health=ok`** を維持
    する（engine テスト `LoadModelCudaFallsBackToCpuForNow` / `LoadModelCudaFallbackKeepsHealthOk`
    が回帰防止に assert）。
  - 要求 EP が**実エラーで失敗**して降格した場合（例 §4.6 step5 の WinML `Failure`）は
    `last_error` に理由を設定。CPU でロードできれば `Health=degraded`、ロード不可なら `error`。
  - Vulkan デバイス不在、モデルまたは context 初期化失敗時は CPU で一度再試行し、成功時も
    降格理由を `last_error` に保持する。後続の成功 LoadModel またはモデル無効化で解除する。
    `auto` から選んだ Vulkan が失敗した場合も同じ扱いとする。

#### `WinMlBackend` の責務境界

- `WinMlBackend` は `InferenceEngine::LoadModel` の内側に閉じ、`core::IConverter` の別実装
  （ONNX Runtime GenAI 版の converter）として `ZenzaiModelConverter` と並べる。TIP と IPC の
  契約は変えない。
- プロンプトの構築（`docs/zenzai-inference-spec.md` §3 と §2.4.2 の `promptFormat`）、
  候補の健全性チェック、logprob の正規化、順位付け、重複除去は decode に依存しない処理として
  `ZenzaiModelConverter` から切り出し、R1 と R2 で共有する。R1 と R2 で同じモデルを使う
  （§4.3 項 2）ので、候補の扱いをエンジンごとに分けない。
- beam / n-best（`docs/zenzai-inference-spec.md` §9.2.3）の再現が論点である。ORT GenAI の
  Generator は `GetLogits`（最後のトークンの logits）、`SetLogits`、`AppendTokens`、
  `RewindTo` を持つが、sequence ごとに KV cache を分岐・共有する仕組みは持たない。次の順に
  R2 spike で確かめて方式を決める。
  1. 対象 EP（QNN / OpenVINO / VitisAI / NvTensorRtRtx）で ORT GenAI の組み込み beam search
     （`num_beams` > 1）が動くか。CPU EP に限られる場合、NPU / GPU EP では貪欲デコードしか
     取れないので、n-best を R1 に残すか、下の (c) へ進む。
  2. (a) 組み込み beam search と `num_return_sequences` に、`SetLogits` で制御トークンを
     除く処理を組み合わせ、R1 の beam と候補がどれだけ一致するか。
  3. (a) が成り立たなければ (c) Windows ML の ONNX Runtime session を直接使い、
     `past_key_values` を自前で管理する。beam ごとに `RewindTo` と `AppendTokens` で再計算する
     (b) は、再計算の量が読み長の 2 乗で増えるため採らない。
- NLL rerank（`docs/neural-reranker-spec.md` §B2）は候補表層の全位置の logits を要する。
  `GetLogits` は最後のトークンだけを返すので、(a) では R2 で NLL rerank を使えない。(a) を採ると
  R1 と R2 で候補の順位が変わり、上の「候補の扱いをエンジンごとに分けない」と §4.3 項 2 の
  理由（閾値の二重化を避ける）に反する。(a) を採って NLL rerank の無効化と順位の差を許すか、
  (c) を必須にするかは、R2 spike の Issue で順位の差を測ってから決める。

#### R1 / R2 の artifact と model catalog

- 同じモデルの R1 artifact（`.gguf`）と R2 artifact（ONNX のディレクトリ）は、model catalog の
  1 entry にエンジン別の artifact として並べ、ファイルごとに SHA-256 を持たせる。プロンプト形式
  （`promptFormat`）はモデル単位で R1 と R2 に共通なので entry に置く。R1 artifact は必須とする
  （§4.5 の終端の R1 CPU を常に持たせるため）。catalog の形は `docs/model-management-spec.md`
  §3.1.1「R2 再開時の catalog」が持つ。
- 設定の `model.selectedPath` は R1 artifact（GGUF）の絶対パスのまま使い、settings の schema は
  変えない。Host は `selectedPath` の GGUF を catalog の entry に照合し（`zenzai\` 配下にあり、
  ファイル名と SHA-256 が R1 artifact と一致する）、一致した entry の R2 artifact を解決する。
  照合できない GGUF（利用者が置いた GGUF）は R1 だけで使う。R2 だけの構成は持たない（§4.5 で
  R1 CPU が終端のため）。設定の移行、設定アプリのモデル選択、D-007 / D-008 は変わらない。
- 診断 D-007 / D-008（`docs/dev-infrastructure-spec.md` §12.2）の判定表は変えない。
  `selectedPath` は GGUF を指すので R1 の判定がそのまま通る。R2 artifact の欠落や SHA-256 の
  不一致は、R1 で動くので D-007 / D-008 の error にせず、§4.5 の降格理由（`last_error` と
  構造化ログ）で示す。表の R2 の判定（`selectedPath` が ONNX ディレクトリを指す場合）は、手で
  編集した設定の判定として残す。
- `kanakanji-edge-33m` で両方を取得すると、GGUF Q8_0 36MB と ONNX int8 49MB の計 85MB になる。
  R1 artifact は常に先に取得する。R2 artifact（ONNX）は次をすべて満たすときだけ取得し、
  上から順に、ネットワーク I/O の前に判定する。
  1. R2 の再開条件（§4.3 項 2）を満たした build と設定である。R2 を有効にするかの判定は 1 か所に
     置く。
  2. SafeMode でない。
  3. `privacy.mode` が `offline` でない。
  4. OS build が 26100 以上である（BackendSelector が記録する `winml_os_eligible`）。
  5. 実効の `backendPreference`（`docs/model-management-spec.md` §5.2）が `auto` か `winml` である。
  6. §4.6 の EP の取得と登録が成功し、使える EP device がある。ONNX の取得は EP の
     `EnsureReadyAsync()` の成功後に行い、EP を使えない機器では ONNX を取得しない。バッテリ
     駆動時は §4.6 のとおり NPU EP だけを試し、NPU device が無ければ取得しない。
- 未決（DEV-1767 / DEV-1768 の結果で決める）:
  - DEV-1767: `kanakanji-edge-33m` を既定モデルに採るか。採らなければ R2 の再開条件（§4.3 項 2）を
    満たさず、R2 artifact を持つ entry は置かない。
  - DEV-1768: R2 artifact の中身（量子化、`genai_config.json` の構成、ファイルの一覧）と、上の
    `WinMlBackend` の方式（(a) ORT GenAI の組み込み beam か、(c) Windows ML の ONNX Runtime
    session を直接使うか）。(c) では artifact が ORT GenAI のディレクトリでなく ONNX と
    `past_key_values` の構成になるため、catalog の R2 artifact のキー名と検証規則
    （`docs/model-management-spec.md` §3.3 の R2 の検証）もこの結果で決める。

#### `auto` の解決（BackendSelector）

`inference-host/include/azookey/host/BackendSelector.h` が、設定 `backendPreference` の
`auto` を R1 のどの backend に解決するかを決める。決めた backend は一次フィルタを通った
候補にすぎず、`InferenceEngine::LoadModelWithResult` で実際にロードを試み、失敗したら上記の
とおり R1 CPU へ降格する（DLL の存在やアダプタの列挙は動作を保証しない）。

- 優先順位: Host の `--backend` / `--cpu` / `--cuda` 明示 > 設定の `model.backendPreference`
  （`auto` 以外）> 設定の `backendPreference`（`auto` 以外）> `auto`。`auto` は Host 起動時に
  解決し、電源の状態が変わったときに解決し直す（下記）。`UpdateConfig` は、その時点で解決済みの
  値を使う。
- 入力はモック可能な 3 つの probe に分ける。
  - アダプタ列挙: `dxcore.dll` を System32 から実行時に読み込み、§3 の DXCore で
    `D3D12_GRAPHICS` と `D3D12_GENERIC_ML` のアダプタを列挙する。`IsHardware` が偽の
    アダプタ（WARP、Microsoft Basic Render Driver）は GPU に数えない。DXCore が無い、
    または列挙に失敗した場合は判定不能とする。
  - ランタイム DLL: プロセスに読み込み済みか、System32 と実行ファイルのディレクトリに
    ファイルがあるかだけを見る。新たにロードはしない。`vulkan-1.dll` = Vulkan loader、
    `cudart64_12.dll` + `cublas64_12.dll` = CUDA runtime。
  - OS build: `RtlGetVersion` で取得する。
- 判定（上から順に、最初に当たったもので CPU に決める）:
  0. SafeMode（`docs/dev-infrastructure-spec.md` §8.5.3。モデルを動かさないため probe を
     実行せず、`reason=safe_mode` とする。SafeMode を `UpdateConfig` で解除しても、`auto` は
     Host を再起動するまで CPU のまま）
  1. ビルドに ggml-vulkan が無い（`AZOOKEY_BACKEND=vulkan` 以外）
  2. バッテリ駆動（§6.3 の `powerProfile` が `performance` なら AC、`battery_saver` なら
     バッテリとみなす）
  3. アダプタが判定不能
  4. ハードウェア GPU が無い
  5. Vulkan loader が無い

  ggml-vulkan のビルドは `vulkan-1.dll` をインポートするため、loader が無い環境ではこの判定に
  至る前にプロセスが起動しない。この場合の CPU への切替えは `docs/sideload-packaging-spec.md`
  §1.6.3 の 2 実行体構成が担い、判定 5 は loader が読み込めない構成への防御として残す。
  `BackendSelectionPolicy::require_winml_os_build` を真にすると、判定 1 の次に「OS build が
  26100 未満」を CPU の条件に加える（既定は偽）。

  どれにも当たらなければ Vulkan を選ぶ。Windows 以外の OS では probe が判定不能を返すため、
  `auto` は常に CPU になる。CUDA は ggml-cuda をリンクするビルドが無いため、
  CUDA runtime があっても選ばない（明示 `cuda` は上記の CPU 降格）。
- OS build が §4.3 項 6 の 26100 以上かどうかは `winml_os_eligible` として記録する。R2 の再開前は
  R1 の選択に使わない。
- 電源の状態が変わったら（§6.1）、変化が 5 秒続いた後に `auto` を解決し直す
  （`PlanAutoBackendForPower`）。アダプタ・ランタイム DLL・OS build は起動時に集めた値を使い、
  電源の状態だけを入れ替える。`auto` の値は、要求が `auto` かどうか、SafeMode かどうかによらず
  電源に追従させる。後から要求を `auto` に戻したときも、その時点の電源に合った値を `UpdateConfig` が
  使うためである。値が変わったら `backend_selected` を `source=power_change` で出す。
  モデルの再ロードは、値が変わり、要求が `auto`（`source=auto`）で、SafeMode ではなく、モデルが
  ロード済みかロード中のときに限る（結果は `power_backend_reload`）。モデルをロードしていなければ
  値の記録だけにとどめ、ロードは利用者の操作か `UpdateConfig` を待つ。SafeMode で起動した Host は
  起動時の probe を実行していないので、`auto` は再起動まで CPU のままになる。ggml-vulkan を含まない
  ビルドでは `auto` は常に CPU なので、再ロードは起きない。
- 選択結果は Host の構造化ログ `backend_selected` に 1 件出す。フィールドは `source`
  （`cli` / `settings` / `auto` / `power_change`）、`requested`、`auto_backend`、`reason`、`adapters_known`、
  `hw_gpu_count`、`hw_ml_only_count`、`vulkan_loader`、`cuda_runtime`、`os_build`、
  `winml_os_eligible`、`on_battery`。アダプタ名は出さない。実際にロードした backend は
  Health と QueryDiagnostics の `backend` が示す。
  QueryDiagnostics の任意の `backend_selection` は起動時の `source`、`auto_backend`、
  `reason` と上記の事実を保持する。電源変化や設定再読込後も起動時の値として扱い、
  実ロード状態と区別する。wire 定義と診断 CLI 出力は
  `docs/dev-infrastructure-spec.md` §12.6 に従う。

### 4.5 フォールバック段位

| 電源 | 要求 | 降格順 |
|---|---|---|
| AC | R2(auto) | NPU EP → GPU EP → CPU EP →（engine 不可なら）R1 CPU |
| AC | R1 CUDA | CUDA → R1 CPU |
| AC | R1 Vulkan（非 NVIDIA / R2 不可の GPU） | Vulkan(ggml-vulkan) → R1 CPU |
| バッテリ | auto | **NPU EP のみ取得・選択**（§4.6 でバッテリ時は GPU EP を登録しない）→ NPU EP ready なら R2(NPU) / 不可なら R1 CPU。**GPU EP / CUDA / Vulkan を回避** |
| 任意 | 終端 | 常に **R1 CPU(GGUF)**。入力をブロックしない |

> バッテリ時に R2(auto) を Windows ML の全 EP 自動選択（NPU→GPU→CPU）に委ねると、NPU
> 非 ready の GPU ラップトップで GPU が選ばれ得る。これを防ぐため、バッテリ時は §4.6 の
> とおり **(a) NPU 系 EP のみ取得・登録**し、かつ **(b) EP は silicon と 1:1 でない**
> （OpenVINO/QNN は 1 EP で NPU/GPU/CPU を露出）ため、**セッションでデバイスレベルに NPU
> へ絞る**（`SetEpSelectionPolicy(MAX_EFFICIENCY)` か `GetEpDevices()` の
> `HardwareDevice.Type == NPU` フィルタ）。NPU device が無ければ R2 を bypass して R1 CPU
> とする（AC 時のみ GPU を許可）。

配布形態の決定は `docs/sideload-packaging-spec.md` §1.6 に反映する（R2 の EP は Windows
Update 配信で非バンドル、CUDA は optional add-on、base MSIX は llama.cpp CPU ランタイム
+ Windows ML bootstrap。**モデル本体（GGUF / ONNX）は MSIX 非同梱で初回起動時 DL**＝
同 §1.2 と一貫）。

### 4.6 R2 の EP 取得・登録（first-run フロー）

> **重要**: R2 の EP（QNN / OpenVINO / VitisAI / NvTensorRtRtx）が非バンドル＝Windows
> Update 配信であることは、**初回起動時に自動で使える意味ではない**。EP が
> `NotPresent` の機器では、アプリが `ExecutionProviderCatalog` API を呼んで
> **取得（download/install）+ 登録（register）するまで ONNX Runtime はその EP を
> ロードできない**。この明示ステップを省くと、対応 NPU/GPU を積んだ Copilot+ PC でも
> R1 CPU フォールバックに張り付く。出典:
> [Install Windows ML execution providers](https://learn.microsoft.com/windows/ai/new-windows-ml/initialize-execution-providers)、
> [Register Windows ML execution providers](https://learn.microsoft.com/windows/ai/new-windows-ml/register-execution-providers)。

R2 エンジン初期化時（`WinMlBackend` 起動 / 初回モデルロード前）に以下を行う:

1. `ExecutionProviderCatalog.GetDefault()` を取得。
2. 取得方針（**電源状態でスコープを絞る**、§4.5 と一致）:
   - **AC 接続時**: 簡易に `EnsureAndRegisterCertifiedAsync()`（対応 EP を全 DL + 一括
     登録。初回はネットワーク速度次第で数秒〜数分。**進捗 UX 必須**）か、`ep_preference`
     指定時は個別取得。
   - **バッテリ駆動時**: `EnsureAndRegisterCertifiedAsync()`（GPU EP も登録される）は
     **使わず**、`FindAllProviders()` で NPU 系 EP（QNN/OpenVINO/VitisAI）のみを
     `EnsureReadyAsync()` → `TryRegister()` する。NPU EP が取得不能なら R2 を bypass し
     R1 CPU。
   - 個別取得は `FindAllProviders()` で `ReadyState` を確認し、目的 EP に
     `EnsureReadyAsync()` → 成功時 `TryRegister()`。`ep_preference`（§4.4）で対象 EP を絞る。
3. **デバイスレベルの選択（重要）**: EP は silicon と 1:1 ではない（OpenVINO / QNN は
   1 EP で **NPU / GPU / CPU 複数デバイス**を露出する）。EP の登録を絞るだけでは GPU 選択
   を防げないため、セッション側でデバイスを絞る:
   - **バッテリ時**: `SessionOptions.SetEpSelectionPolicy(MAX_EFFICIENCY)`（NPU 優先 +
     CPU fallback、discrete GPU を避ける）を用いるか、明示選択で `GetEpDevices()` を
     `HardwareDevice.Type == NPU`（+ CPU fallback）でフィルタし、`AppendExecutionProvider_V2`
     で **NPU device のみ append**（GPU device は append しない）。NPU device が無ければ
     R2 を bypass し R1 CPU。
   - **AC 時**: `MAX_PERFORMANCE` / `PREFER_NPU` など、もしくは明示選択で GPU device も許可。
   - EP device 一覧は EP / ドライバ更新で**動的に変わる**ため、選択ロジックは再列挙に
     耐えるよう実装する。出典:
     [Select execution providers（Device Policies / GetEpDevices フィルタ）](https://learn.microsoft.com/windows/ai/new-windows-ml/select-execution-providers)。
4. `ReadyState` 遷移を扱う: `NotPresent`（未 DL）/ `NotReady`（DL 済・未登録）はいずれも
   `EnsureReadyAsync()`、`Ready` は `TryRegister()`。
5. **エラー / 進行中処理**: `EnsureReadyAsync()` の結果 `Status` を確認し、
   - `Failure` → `ExtendedError`(HRESULT) / `DiagnosticText` をログし、当該 EP を諦めて
     **R1 CPU にフォールバック**（§4.5、`Health=degraded` + `last_error`）。
   - `InProgress` → 完了を待ってからセッション生成。
6. **first-run UX**: 初回 DL は時間がかかるため、進捗インジケータ（download progress
   callback）を出す。オフライン / 制限ネットワーク環境は EP DL 不可のため R1 CPU 継続
   （bring-your-own EP は将来検討）。
7. 登録結果は ONNX Runtime の `GetEpDevices()` で検証可能（例: 登録後に
   `QNNExecutionProvider (DeviceType: NPU)` が現れる）。

この EP 取得・登録ステップの実装は M24（`WinMlBackend`）の必須要件とし、
EP の `ep` / `ep_state` / `ep_last_error` を診断に含める（`docs/dev-infrastructure-spec.md`
§7.7.2 トレースログ・§12.6 `QueryDiagnostics` に反映済み）。

## 5. mmap モデルロード

GGUF は llama.cpp のマップでロードする。マップしたページはファイルを裏付けにするため、
複数 Host プロセスが同じモデルを使うと物理ページを共有でき、メモリ圧迫時はカーネルが破棄して
ファイルから読み直せる。

### 5.1 ロード手順

`LoadZenzaiGgufModel`（`inference-host/src/ZenzaiModelConverter.cpp`）が次の順でロードする。
`ZenzaiRuntimeOptions::use_mmap`（既定は真）が偽なら、手順 1 と 2 を省いて読み込みでロードする。

1. preflight: `MmapModelLoader`（`inference-host/include/azookey/host/MmapModelLoader.h`）が
   ファイル全体を読み取り専用でマップし、すぐに解放する。Windows は `CreateFileW`、
   `CreateFileMappingW`、`MapViewOfFile`、それ以外の OS は `mmap` を使う。空のファイルは
   マップできないので失敗とする。preflight が失敗したら、手順 2 を省いて手順 3 へ進む。
2. `llama_model_load_from_file` を `load_mode=LLAMA_LOAD_MODE_AUTO` で呼ぶ。llama.cpp は、マップした
   バッファを使えないデバイスがあればマップせずに読み込む。この試行では `use_extra_bufts=false` とし、
   CPU の重み repack を使わない。repack は重みを並べ替えた複製を私有メモリに作るので、マップした
   重みが §5.2 の共有ページにならず、private bytes と commit charge がモデルのサイズに近い量だけ増える。
   repack の対象は CPU と量子化形式で決まり、x64 では Q4_0 / Q4_K / IQ4_NL など、ARM64 ではこれに
   Q5_K / Q6_K / Q8_0 が加わる。対象の形式では CPU 推論が repack ありより遅くなりうる。
3. 手順 2 が失敗したら、`load_mode=LLAMA_LOAD_MODE_NONE` と `lazy_mode=LLAMA_LAZY_MODE_OFF`
   （lazy 読み込みもマップを使うため）の読み込みで 1 回だけ再試行する。この試行では repack を
   使う（`use_extra_bufts=true`）。読み込みでは重みが元から私有メモリに載るので、repack しても増えない。
   llama.cpp はマップの失敗を他のロード失敗と区別して返さないため、失敗の理由は問わない。GGUF の破損や
   Vulkan の初期化失敗でも再試行するので、`model_mmap_fallback` の `reason=load` はマップが原因で
   あることを意味しない。
   再試行も失敗したら、通常のロード失敗として扱う（§4.4）。

試行の順序は `LoadWithMmapFallback` が、試行ごとの `load_mode` / `lazy_mode` / `use_extra_bufts` は
`ModelLoadSettingsFor` が持ち、どちらも llama.cpp を含まないビルドでもテストできる。

手順 1 か手順 3 を通って読み込みでロードできた場合も、ロードは成功として扱い、Health は `ok` の
まま `last_error` も設定しない。Host の構造化ログに `model_mmap_fallback`（Warn）を 1 件出す。
フィールドは `reason`（`preflight` / `load`）と `result`（`ok` / `error`）。

### 5.2 利点

- 複数 Host インスタンス間で物理ページ共有
- カーネルのページキャッシュに乗せて 2 回目以降の起動を高速化
- マップしたページはファイルを裏付けにする共有ページで、private bytes と commit charge に
  計上されず、メモリプレッシャ時にカーネルが破棄できる。触れたページは WorkingSet に共有分として
  計上される

これらの利点は、CPU backend がマップした重みをそのまま使う場合に成り立つ。Vulkan など重みを GPU へ
載せる backend では、GPU 側の確保（WDDM）が Host の commit charge と private bytes に計上されるため、
マップでロードしても private bytes がモデルのサイズを超えうる。M25 のメモリの判定（private bytes と
WorkingSet の内訳で、マップした重みが私有メモリに複製されていないことを確かめる）は CPU backend の
ロードだけを対象とし、GPU backend のロードは private bytes では判定しない。

## 6. 省電力モード連動

### 6.1 電源状態の検出

電源の状態は `GetSystemPowerStatus` の `ACLineStatus` で読む（1 は AC、0 はバッテリ、それ以外の
値と API の失敗は不明）。不明は AC として扱う。

`PowerStatusWatcher`（`inference-host/include/azookey/host/PowerStatusWatcher.h`）は、専用スレッドで
表示しない top-level window を持ち、`WM_POWERBROADCAST` を受ける。message-only window
（`HWND_MESSAGE`）は broadcast を受けないため使わない。`PBT_APMPOWERSTATUSCHANGE` と
`PBT_APMRESUMEAUTOMATIC` で電源の状態を読み直し、`PowerStateTracker` が前回と違うときだけ変化を
通知する。バッテリ残量だけの変化は通知しない。window を作れない環境と Windows 以外の OS では
watcher を起動せず、Host 起動時に読んだ電源の状態のまま動く。起動できたらログ
`power_watch_started`（`power_source`）、できなければ `power_watch_unavailable`（Warn）を出す。

変化を受けた Host は、`UpdateConfig` と同じロックを取り、読み込み済みの設定から
`ApplyRuntimeSettingsToEngineConfig` で engine の設定を作り直して適用する。この適用は window の
スレッドとは別のスレッドで行い、モデルのロード中も window のメッセージを止めない。適用のたびに
ログ `power_source_changed` を出す。フィールドは `from`、`to`（`ac` / `battery` / `unknown`）、
`on_battery`、`power_profile`、`lane`（`heavy` / `fast`）。backend の解決し直しと、それに伴う
モデルの再ロードは §4.4 に従う。

Host 起動時の設定の適用から watcher の起動までの間に電源の状態が変わった場合も、watcher の起動後に
読み直して同じ適用を行う。電源の変化と同時に進んでいたモデルロードは、完了時にロード開始時の
推論スレッド数を設定へ書き戻す。この場合のスレッド数は、次の電源の変化か `UpdateConfig` で
設定し直される。

Host の終了時は、window を閉じてスレッドを join してから engine を破棄する。

### 6.2 バッテリ時の挙動

`ResolveInferenceLane`（`inference-host/include/azookey/host/PowerLanePolicy.h`）が、電源の状態と
§6.3 の `powerProfile` から、重い推論を走らせるレーンを決める。バッテリ（`battery_saver` を含む）なら
Fast、AC（`performance` と、電源の状態が不明な場合を含む）なら Heavy とする。Fast は beam 1 本の
Zenzai の 1 pass、Heavy は既定の beam 幅の Zenzai と、その上に重ねる追加の pass（NLL rerank、
`docs/neural-reranker-spec.md`）を指す。解決したレーンは `EngineConfig::inference_lane` が持ち、
`ApplyRuntimeSettingsToEngineConfig` が設定する。

| 項目 | AC 接続時 | バッテリ駆動時 | Host での扱い |
|---|---|---|---|
| バックエンド選択 | §4.5 のフォールバック段位に従う（R2 NPU/GPU → R1 CUDA/Vulkan/CPU） | §4.5 に従う（**R2 は NPU device のみ**、§4.6 のデバイスレベル絞り込み。discrete GPU / CUDA / Vulkan を回避し、NPU 不可なら R1 CPU） | `auto` は Host 起動時の電源の状態で解決し、電源の変化が 5 秒続いたら解決し直す。値が変わり、モデルを使っていれば再ロードする（§4.4） |
| 推論スレッド数（`inferenceThreads` が 0 のとき） | 8 | 2 | 電源の状態が不明なら 4。いずれも論理コア数を上限とする。電源の変化で設定し直し、ロード済みのモデルにも次の変換から効く（`llama_set_n_threads`。走っている変換の終わりを待って切り替える） |
| Zenzai の beam 幅（候補ウィンドウ、`live = false`） | 既定の beam 幅（`docs/zenzai-inference-spec.md` §6.2） | 1（Fast レーン） | 電源の変化で設定し直し、次の変換から効く。モデルの候補は 1 件になり、辞書・user_dict の候補と、モデルが無いときの fallback converter の候補上限は変えない（モデルの劣化で Zenzai が内部から呼ぶ fallback には、上限 1 の文脈がそのまま渡る。`docs/zenzai-inference-spec.md` §7.1）。変換のハード予算（`docs/zenzai-inference-spec.md` §8.2）は変えない |
| NLL rerank（Heavy レーン） | 設定 `nll.enabled` に従う | 無効（Fast レーンのみ） | 電源の変化で設定し直し、次の変換から効く |
| ライブ変換重い推論 | 無効（Fast レーンのみ） | 無効（Fast レーンのみ） | ライブ変換は電源によらず `live = true` の Zenzai 1-pass で行い、NLL rerank などの Heavy の pass は走らせない。モデルが無いときは fallback converter（`docs/zenzai-inference-spec.md` §7.1） |
| 予測頻度 | 入力ごと | 200ms デバウンス | TIP 側の挙動で、Host は扱わない |
| Post-Commit Lint | 有効 | 無効 | Host は扱わない |
| Persona 再計算間隔 | 24h | 7 日 | Host は扱わない |

### 6.3 設定での上書き

`settings.powerProfile`:

```json
{
  "powerProfile": "auto"   // "auto" | "performance" | "battery_saver"
}
```

`"auto"` が既定。`"performance"` で AC モードを強制、`"battery_saver"` でバッテリ
モードを強制。

## 7. PerMonitorV2 DPI 対応

### 7.1 適用範囲

- `CandidateWindow.cpp`
- `PredictionWindow.cpp`
- デバッグウィンドウ（M18-3）
- Magic Conversion プロンプト（M16）
- 設定アプリ（M30、別プロセス）

### 7.2 マニフェスト

TIP DLL は in-proc COM なのでアプリ側の DPI 認識を継承する。
自前 HWND を作るときに `SetThreadDpiAwarenessContext` で per-monitor v2 を強制：

```cpp
HWND CandidateWindow::CreateWindow() {
    DPI_AWARENESS_CONTEXT prev = SetThreadDpiAwarenessContext(
        DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HWND hwnd = CreateWindowExW(...);
    SetThreadDpiAwarenessContext(prev);
    return hwnd;
}
```

### 7.3 WM_DPICHANGED ハンドリング

```cpp
case WM_DPICHANGED: {
    UINT dpi = LOWORD(wParam);
    RECT* suggested = reinterpret_cast<RECT*>(lParam);
    SetWindowPos(hwnd, nullptr,
                 suggested->left, suggested->top,
                 suggested->right - suggested->left,
                 suggested->bottom - suggested->top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    OnDpiChanged(dpi);
    break;
}
```

`OnDpiChanged(UINT dpi)`：

- フォントサイズを `MulDiv(base_size_pt, dpi, 96)` で再計算
- アイコン・余白を `MulDiv(base_px, dpi, 96)` で再計算
- レイアウト再構築

### 7.4 AdjustWindowRectExForDpi

枠なし `WS_POPUP` は不要だが、設定アプリでは `AdjustWindowRectExForDpi` を使う：

```cpp
RECT rc = { 0, 0, MulDiv(640, dpi, 96), MulDiv(480, dpi, 96) };
AdjustWindowRectExForDpi(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
```

## 8. ARM64 ビルド（M27）

> **本節の確定方針（M27 調査・DEV-111）**: ARM64 対応は「**ビルド（クロスコンパイル）**」と
> 「**テスト実行（ネイティブ ARM64 が必要）**」を分離して扱う。CI の緑ゲートは既存 x64
> ランナー上のクロスビルドで満たし、ARM64 バイナリの**実行テスト**は別経路（§8.4）に置く。
> NPU 経路は §4.3（R2=Windows ML）が正典であり、本節はビルド系・配布系の確定に限る。

### 8.1 ビルド方式（クロスコンパイル前提）

現行 CI（`.github/workflows/windows.yml`）は **Ninja + CMakePresets**（`windows-release`
等）で構成され、VS generator は使わない。よって ARM64 ターゲットは **VS generator の
`-A ARM64` ではなく、MSVC ARM64 環境（`vcvarsall x64_arm64`）+ Ninja + 明示クロス
toolchain** で指定する。`ilammy/msvc-dev-cmd@v1` の `arch` パラメータが SDK ヘッダ /
import lib / リンカの ARM64 環境を構成する。

> **重要（コンパイラは clang-cl、`cl.exe` ではない）**: llama.cpp / ggml は **ARM ターゲットで
> MSVC `cl.exe` を明示的に拒否**する。`ggml/src/ggml-cpu/CMakeLists.txt` に
> `if (MSVC AND NOT CMAKE_C_COMPILER_ID STREQUAL "Clang") → message(FATAL_ERROR "MSVC is
> not supported for ARM, use clang")` がある。よって ARM64 ビルドの**コンパイラは
> `clang-cl`**（`vcvarsall x64_arm64` の SDK/リンカ環境を流用しつつ `--target=arm64-pc-windows-msvc`）
> とする。`cl.exe` のままだと llama.cpp 依存を有効化した瞬間にクロスビルドゲートが
> FATAL_ERROR で落ちる。clang-cl は VS の「C++ Clang tools for Windows」コンポーネント、
> または LLVM 公式インストーラで入る。出典:
> [ggml-cpu CMakeLists.txt（MSVC ARM 非対応の FATAL_ERROR）](https://github.com/ggml-org/llama.cpp/blob/master/ggml/src/ggml-cpu/CMakeLists.txt)。

ARM64 用に**明示クロス toolchain（または同等の preset 変数）**を用意する。`CMAKE_SYSTEM_NAME`
を設定するとクロスコンパイルモードに入り、`CMAKE_SYSTEM_PROCESSOR` が確実に `ARM64` に
なる（§8.2 のフラグ分岐の前提。設定しないとホスト値 `AMD64` のまま＝§8.2 注を参照）:

```cmake
# cmake/toolchains/win-arm64-clang.cmake（新規）
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR ARM64)          # ← クロスモードを有効化し、分岐を確定させる
set(CMAKE_C_COMPILER   clang-cl)
set(CMAKE_CXX_COMPILER clang-cl)
set(CMAKE_C_COMPILER_TARGET   arm64-pc-windows-msvc)
set(CMAKE_CXX_COMPILER_TARGET arm64-pc-windows-msvc)
```

**ARM64 クロスビルドは既存 `windows-build` matrix に行追加せず、専用ジョブに分離する**。
理由は既存 `windows.yml` の後段ステップが x64 native（build + ctest）前提で書かれており、
ARM64 行を相乗りさせると次の 2 点で**ビルド成功後にジョブが失敗**するため（いずれも
Codex review 指摘）:

- **artifact 名の衝突**: 既存の upload ステップは `windows-${{ matrix.config }}-logs` /
  `windows-release-pdb` と **config のみ**で命名する。x64 Release と ARM64 Release は
  どちらも `config == Release` のため、`actions/upload-artifact@v4` の「matrix 内で
  artifact 名は一意」制約に違反しアップロードが失敗する。
- **最終 fail ゲートの誤判定**: 既存の最終ステップは `steps.run_tests.outputs.tests_ec`
  が `'0'` でなければ `exit 1` する。ARM64 行で ctest を**スキップ**すると `tests_ec` が
  未設定（空文字）になり、`'' -ne '0'` が真となってビルド成功でも落ちる。

専用ジョブの構成例（**ctest なし・arch を含む一意な artifact 名**）:
以下は環境構築とビルドの抜粋であり、後述の成果物検査とそのログ保存も必須とする。

```yaml
jobs:
  windows-arm64-crossbuild:        # 既存 windows-build とは別ジョブ
    name: Windows ARM64 cross-build
    runs-on: windows-2022          # x64 ホストでクロスビルド
    steps:
      - uses: actions/checkout@v4
        with: { submodules: false }
      - uses: ilammy/msvc-dev-cmd@v1
        with:
          arch: amd64_arm64        # ARM64 の SDK ヘッダ / import lib / リンカ環境
      - run: cmake --preset windows-release-arm64 -DAZOOKEY_FETCH_GOOGLETEST=ON
      - run: cmake --build --preset windows-release-arm64
      - run: cmake --build --preset windows-release-arm64 --target azookey_settings
      # ctest は実行しない（x64 ホストで ARM64 バイナリは走らない）。実行は §8.4。
      - uses: actions/upload-artifact@v4
        if: always()
        with:
          name: windows-arm64-crossbuild-logs   # ← arch を含め一意化
          path: |
            configure-arm64.log
            build-arm64.log
          if-no-files-found: ignore
      # fail ゲートは configure / build / 成果物検査を見る（tests_ec は参照しない）。
```

既存 `windows-build`（x64）matrix に ARM64 を**足さない**ため、x64 側の artifact 名・
fail ゲートは無改変で済む。`windows-release-arm64` preset は `windows-release` を継承し、
上記 toolchain file を `CMAKE_TOOLCHAIN_FILE` で指す（または同じ変数群を
`cacheVariables` で直接指定する）。なお §8.4 の `windows-11-arm` ネイティブテストジョブも
別ジョブとして独立させ、そちらは build + ctest + 自前の fail ゲートを持つ。

ARM64 クロスビルドは `scripts/check-arm64-hardening.ps1` で TIP DLL と Host の
`dumpbin /headers /loadconfig` を検査する。ARM64 machine、DLL characteristics の
CFG / DYNAMIC_BASE / NX_COMPAT / HIGH_ENTROPY_VA（必須ビット `0x4160`）、
`CF instrumented` / `FID table present`、非ゼロの関数テーブルと件数、
`Dependent Load Flag 0B00` を必須とし、出力をログ artifact に保存する。
`dumpbin` は MSVC cross 環境の `VCToolsInstallDir/bin/Hostx64/arm64/dumpbin.exe` を
明示して呼ぶ。clang-cl の未知引数・未使用引数は toolchain でエラーにする。
フラグ適用の正典は `dev-infrastructure-spec.md` §2.2、検査対象は同 §4.3 に従う。
設定アプリも `azookey_settings` ターゲットで ARM64 構成を生成し、PE Machine が
`0xAA64` であることを検査して `windows-arm64-crossbuild-binaries` に含める。
設定アプリの構成と NuGet lock は §8.3 に従い、TIP / Host の緩和策検査とは分ける。

### 8.2 ARM 最適化フラグ（cross-compile セーフ）

> **重要（クロスコンパイルの落とし穴）**: llama.cpp / ggml の `GGML_NATIVE`（旧
> `LLAMA_NATIVE`）`=ON` は **ビルドホストの CPU を検出**して `-march=native` 相当を当てる。
> x64 ホストから ARM64 をクロスビルドする本構成では、`GGML_NATIVE=ON` は **誤って x64 の
> 機能を検出**するか、ARM 向けに無意味な検出を行う。よって **クロスビルドでは
> `GGML_NATIVE=OFF` を強制**し、ARM の機能は明示フラグで指定する。

> **重要（ARM64 検出を `CMAKE_SYSTEM_PROCESSOR` だけに頼らない）**: toolchain file を
> 使わない Ninja + `vcvarsall` 構成では、CMake は `CMAKE_SYSTEM_PROCESSOR` を**ホスト値
> （Windows では `AMD64`）に追従**させ、コンパイラが ARM64 を狙っていても `ARM64` に
> ならない。この値だけで下記分岐を組むと、ARM64 クロスジョブで分岐が**無音スキップ**され
> host-native/x64 検出のまま通ってしまう。§8.1 のとおり **toolchain file で
> `CMAKE_SYSTEM_NAME` + `CMAKE_SYSTEM_PROCESSOR ARM64` を設定**すればクロスモードに入り
> 値が確実に `ARM64` になる。toolchain を使わない場合は `CMAKE_CXX_COMPILER_ARCHITECTURE_ID`
> （MSVC/clang-cl が `ARM64` を返す）や環境変数 `VSCMD_ARG_TGT_ARCH == arm64`、または
> 明示 preset 変数で判定する。出典:
> [CMAKE_SYSTEM_PROCESSOR（host 追従の注記）](https://cmake.org/cmake/help/latest/variable/CMAKE_SYSTEM_PROCESSOR.html)。

> **重要（i8mm を配布バイナリに焼き込まない）**: `GGML_CPU_ARM_ARCH` の指定機能は
> ggml CPU バイナリ全体の**必須命令**になる。**i8mm（FEAT_I8MM）は ARMv8.2〜v8.5 で任意**
> （必須化は v8.6 以降）であり、§8.4 のテスト経路に挙げた **Ampere Altra / Neoverse N1 は
> ARMv8.2 で i8mm 非対応**。よって `+i8mm` をベースラインに焼き込むと、その同じ Ampere
> Altra（および i8mm 非搭載の他 ARM64 Windows）で ARM64 ctest / 実行が **illegal
> instruction で落ちる**。配布ベースラインは **stated target が保証する機能のみ**に
> 留める。**dotprod（FEAT_DotProd）は ARMv8.2-A で Snapdragon X / Neoverse N1(Ampere
> Altra) / Cobalt(N2, CI ランナー) いずれも搭載**のため許可してよいが、**i8mm は外す**。

```cmake
# §8.1 の toolchain で CMAKE_SYSTEM_PROCESSOR=ARM64 を設定している前提（クロスモード）。
# toolchain を使わない場合は下記 MATCHES の代わりに
# CMAKE_CXX_COMPILER_ARCHITECTURE_ID / $ENV{VSCMD_ARG_TGT_ARCH} で判定する。
if (CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
    set(GGML_NATIVE OFF CACHE BOOL "" FORCE)   # クロス時のホスト誤検出を防ぐ
    # 配布ベースラインは全 stated ARM64 Windows ターゲットで安全な機能のみ。
    # dotprod は ARMv8.2 の Ampere Altra(N1) でも搭載。i8mm は v8.2〜v8.5 で任意
    # （Altra/N1 非搭載）のため焼き込まない（illegal instruction 回避）。
    set(GGML_CPU_ARM_ARCH "armv8.2-a+dotprod" CACHE STRING "")
endif()
```

- ベースライン ISA は **ARMv8.2-A + dotprod**（Snapdragon X / Ampere Altra(N1) /
  Cobalt(N2) いずれも満たす最小公倍数）。これ未満の古い ARM64 Windows を切る判断は M27 の
  前提（Copilot+ PC ターゲット）と整合。
- **i8mm を使いたい場合**（Copilot+ Snapdragon X の行列演算高速化）は、ベースラインに
  焼き込まず **i8mm 専用 variant を別ビルドし実行時ディスパッチ**で選ぶ（非搭載機では
  ベースライン variant を使う）。v1.0 では複雑性回避のためベースライン単一とし、i8mm
  variant は後続評価とする。
- ネイティブ ARM64 ビルド（§8.4）でも上記明示フラグを使い、クロス/ネイティブで同一
  バイナリ特性に揃える（再現性のため `GGML_NATIVE` は両構成で OFF）。
- コンパイラは §8.1 のとおり **clang-cl**（`cl.exe` は ggml が ARM で拒否）。

### 8.3 アーキ別 backend payload 同梱表

配布物の「同梱 / 非バンドル / DL」の正典は `docs/sideload-packaging-spec.md` §1.6、
フォールバック段位の正典は §4.5。本表は **アーキ軸**でどの payload が x64 / ARM64 に
存在するかを固定する（同梱可否は §1.6 を参照、本表で重複定義しない）。

| payload / エンジン | x64 | ARM64 | 備考 |
|---|---|---|---|
| llama.cpp（R1）CPU ランタイム | ◎ AVX2 等 | ◎ NEON（§8.2） | base MSIX 同梱（§1.6）。両アーキの **v1.0 既定** |
| ggml-cuda（R1 CUDA） | ○ optional add-on | **✕ N/A** | Windows on ARM に NVIDIA CUDA は無い。ARM64 では add-on を**作らない** |
| ggml-vulkan（R1 ベンダ横断 GPU） | ○ | △ 後続（Adreno）優先度低 | x64 を先行。ARM64 GPU 経路は v1.0 後に評価 |
| Windows ML bootstrap（R2） | ◎ | ◎ | base MSIX 同梱（薄い）。EP 本体は含めない（§1.6） |
| R2 EP（Windows Update 配信・非バンドル） | OpenVINO(Intel) / VitisAI(AMD) / NvTensorRtRtx(NVIDIA) / QNN(該当時) | **QNN（Snapdragon NPU）**中心 | EP は silicon 別に Windows ML が自動取得・選択（§4.6）。アーキで同梱物は変わらない（どちらも非バンドル） |
| Zenzai GGUF / ONNX モデル本体 | 非同梱・初回 DL | 非同梱・初回 DL | §1.2 / §1.6 と一貫。アーキ非依存（モデルは共通） |

要点:

- **ARM64 の add-on は作らない**（CUDA add-on は x64 限定）。ARM64 のアクセラレーションは
  R2（Windows ML / QNN EP、非バンドル）に一本化し、MSIX を肥大させない。
- これにより配布パッケージ構成は **base MSIX（x64 / ARM64 の 2 アーキ）+ x64 専用の
  optional add-on（CUDA / Vulkan）** に収束する。各 add-on の構成は
  `docs/sideload-packaging-spec.md` §1.6 と §1.6.3 が正典。アーキ別に別 payload を
  後決めする必要はない（後戻り回避）。
- リリース CI（`.github/workflows/release.yml`）は **x64 専用**で、`cmake --preset
  windows-release`（x64）を configure/build し、`pkg/msi/azooKey.wixproj` の
  `InstallerPlatform=x64` で未署名 MSI を生成し、未署名であることを検査して upload する。
  winget 用 MSI の定義は `docs/sideload-packaging-spec.md` に従う。本節の M28/M29 が扱う
  ARM64 MSIX は **wapproj の `Platform=ARM64` だけでは不十分**で、次を一式アーキ別に
  parameterize する必要がある（さもないと x64 バイナリを包むか、`arm64.msix` が署名・
  公開されない）:
  1. **ARM64 CMake ビルド出力**: `windows-release-arm64` preset（clang-cl・§8.1）で
     ARM64 バイナリ（TIP DLL / host / settings EXE）を生成。
     設定アプリの MSBuild `Platform` は `CMAKE_CXX_COMPILER_ARCHITECTURE_ID` を優先し、
     値がない場合は `CMAKE_SYSTEM_PROCESSOR` から x64 / ARM64 を解決する。
     未対応アーキでは設定アプリのターゲットを省略する。
     vcxproj は両アーキの Debug / Release 構成を持つ。`RuntimeIdentifiers` は
     `win10-x64;win10-arm64` とし、共通の `packages.lock.json` は Windows 上の NuGet
     restore で生成する。出力先は各 preset の `settings-app/<Debug|Release>/`（構成名）とする。
  2. **wapproj `Platform=ARM64`** でその ARM64 出力を取り込み MSIX 化。
  3. **MSIX の署名 / verify / upload をアーキ別パスで定義**（`Package_1.0.0_${arch}.msix` 等）。
  WAP 自体は x64 ホストから ARM64 をクロスパッケージ可能だが、上記 1〜3 が揃って初めて
  `arm64.msix` が成立する。詳細レシピは M28/M29（`docs/sideload-packaging-spec.md` §1/§2）で
  確定する。

### 8.4 ARM64 テスト実行と受け入れ条件

ARM64 バイナリは x64 ホストで実行できないため、`ctest` は ARM64 ネイティブ環境が要る。

| 経路 | 用途 | 制約 |
|---|---|---|
| **`windows-11-arm` GitHub-hosted ランナー** | ARM64 ネイティブ build + ctest | **public / private 両方で利用可**（標準ランナー）。public は無料、private は spec 4 vCPU/16GB ではなく **2 vCPU/8GB**で **GitHub の無料枠を消費し超過分は従量課金**。label 失敗ではない |
| **self-hosted ARM64 / Azure ARM64 VM**（Ampere Altra） | 課金回避 / 反復テスト最適化 | インフラ用意・運用コストが要る。ARM64 VM は x64 ハードでは作成不可（クラウドホスト必須） |
| **Snapdragon X Elite 実機** | NPU 含む統合検証 | `gate:human-required`（実装フェーズで付与）。CI 化しない |

**M27 受け入れ条件の確定（roadmap M27 を補足）**:

1. **CI 緑ゲート（必須・自動）** = §8.1 のクロスビルド（clang-cl / ARM64 toolchain）が
   既存 x64 ランナーで成功すること。ARM64 バイナリの生成と §8.1 の成果物検査を CI の最小ゲートとする
   （新インフラ不要）。
2. **ARM64 単体テスト実行** = `windows-11-arm` ランナーで `ctest` を緑にする。**public /
   private いずれでも label は有効**（private は従量課金）なので、リポジトリ可視性に
   依らず CI 化できる。課金やキュー待ちを避けたい場合のみ self-hosted ARM64 を選ぶ。
3. **実機（NPU 含む）動作確認** = Snapdragon X 実機で人手検証（`gate:human-required`）。
   §4.3（NPU 必須化はしない）と整合し、CI のブロッカーにはしない。

> 以前の「private では label 失敗」という制約は撤回する。現行の GitHub hosted-runner
> リファレンスは `windows-11-arm` を public / private 双方の標準ランナーとして掲載して
> おり、private は従量課金で実行される（label 解決失敗ではない）。出典:
> [GitHub-hosted runners reference](https://docs.github.com/en/actions/reference/runners/github-hosted-runners)。

### 8.5 Snapdragon X Elite NPU（QNN SDK 直叩きは非既定 fallback）

> **更新（M24 決定、§4.3 が正典）**: 既定方針は **Windows ML（R2）経由で QNN EP を
> 自動利用**することであり、**QNN SDK を直接同梱しない**（EP は Windows Update 配信）。
> 以下の QNN SDK 直叩き構成は、Windows ML / QNN EP が要件を満たさないと判明した場合
> にのみ検討する fallback（非既定）として残す。`inference-host/src/QnnBackend.cpp` も
> その場合に限る新規ファイルであり、既定実装は `WinMlBackend.cpp`（§4.4）とする。

QNN SDK を直接使う場合（非既定・fallback）：

- `qnn_sdk/include/QNN/` をインクルードパスに追加
- `qnn_htp/lib/aarch64-windows-msvc/` をリンク
- バイナリ配布時は QNN ランタイム DLL を MSIX に同梱（§8.3 の「非バンドル」方針の例外と
  なるため、この fallback 採用時は §1.6 の配布表も更新する）

`inference-host/src/QnnBackend.cpp`（非既定・fallback 時のみ新規）：

```cpp
class QnnBackend : public IBackend {
public:
    bool Initialize(const std::wstring& model_path) override;
    Result Infer(const std::string& prompt) override;
};
```

### 8.6 出典（M27 調査の一次情報）

- [GitHub-hosted runners reference（`windows-11-arm` は public / private 両対応の標準ランナー・spec / 課金）](https://docs.github.com/en/actions/reference/runners/github-hosted-runners)
- [GitHub Actions: Windows on Arm runners for all public repos（2025-04 の public 無料化アナウンス。private 対応はその後 reference に掲載）](https://blogs.windows.com/windowsdeveloper/2025/04/14/github-actions-now-supports-windows-on-arm-runners-for-all-public-repos/)
- [ggml-cpu CMakeLists.txt（MSVC は ARM 非対応・clang 必須の FATAL_ERROR）](https://github.com/ggml-org/llama.cpp/blob/master/ggml/src/ggml-cpu/CMakeLists.txt)
- [CMAKE_SYSTEM_PROCESSOR（toolchain なしでは host 値に追従）](https://cmake.org/cmake/help/latest/variable/CMAKE_SYSTEM_PROCESSOR.html)
- [Add Arm support to your Windows app（クロスコンパイル可・テストは ARM64 実機/VM 必須）](https://learn.microsoft.com/windows/arm/add-arm-support)
- [Windows on Arm overview（ARM64 VM はクラウドホスト必須・開発ツール）](https://learn.microsoft.com/windows/arm/overview)

## 9. テスト

| テスト | 場所 | 内容 |
|---|---|---|
| BackendSelector | `inference-host/tests/backend_selector_test.cpp` | probe のモックによる §4.4 の判定と判定順、起動時の電源状態と `powerProfile` による上書き、probe 失敗時の CPU、電源の変化での解決し直し（`auto` の値は常に追従し、再ロードは値が変わり、要求が `auto` で、SafeMode ではなく、モデルを使っているときだけ） |
| MmapModelLoader | `inference-host/tests/mmap_loader_test.cpp` | マップの成功、ファイル不在と空ファイルのエラー、サイズ取得、解放後のファイル削除、`LoadWithMmapFallback` の試行順（preflight の失敗、マップ失敗後の 1 回の再試行、両方の失敗） |
| Power Status | `inference-host/tests/power_status_test.cpp` | 電源状態の変化の検出と残量だけの変化の無視、不明を AC として扱うこと、`powerProfile` の上書き、変化によるレーンとスレッド数の切替え、Windows では watcher の window への `WM_POWERBROADCAST` が tracker に届くこと、変化が続いた後にだけ解決し直すためのデバウンス |
| DPI changed | `tsf-tip/tests/candidate_window_dpi_test.cpp` | Windows 限定。フォント再計算が呼ばれるか |
| ARM64 build | CI matrix | ビルド成功 + テスト pass |

## 10. 参照

- DXCore SDK: <https://learn.microsoft.com/windows/win32/dxcore/>
- DirectML: <https://learn.microsoft.com/windows/ai/directml/>
- QNN SDK: <https://www.qualcomm.com/developer/software/qualcomm-ai-engine-direct-sdk>
- OpenVINO: <https://docs.openvino.ai/>
- ベース：`docs/zenzai-gpu-route.md`
- ライブ変換のレーン分け：`docs/rich-features-spec.md` X-4-1
- マルチディスプレイ：`docs/legacy-parity-spec.md` 9 章
