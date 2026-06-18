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
> `DirectML` / `NPU` を独立値として実装しない。現行コードの enum は `{ Cpu, Cuda }`
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
| **R2** | ONNX Runtime GenAI + Windows ML | ONNX Runtime GenAI 形式（**要変換**） | Windows ML が自動選択（NPU: QNN/OpenVINO/VitisAI、GPU: NvTensorRtRtx/OpenVINO、CPU: ORT） | ◎（Copilot+ PC） | Windows Update 配信（**非バンドル**） |

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

`bench/zenzai_backend_bench.cpp`（新規）で R1 / R2 を横断計測する。

- 同一プロンプト「こんにちは」「日本語」「今日は良い天気です」等 20 件を各経路で実行。
- メトリクス: 初回 LoadModel 時間 / P50 推論レイテンシ / RSS / 実効配布サイズ。
- 判定ゲート（重み順）:
  1. P50 推論レイテンシ < 30ms
  2. 実効配布サイズ < 50MB（EP 非バンドル前提で R2 が有利）
  3. 初回 LoadModel 時間 < 3 秒
  4. ARM64 / NPU 可用性
- **前提スパイク（R2 のブロッカー）**: R2 は zenz-v3 を **ONNX Runtime GenAI 形式へ
  変換できること**が前提。Foundry Toolkit の変換は Preview かつ対応モデルが限定列挙
  （Phi / Qwen / Llama-3.2-1B / DeepSeek-distill）で、**zenz-v3（独自小型 JP モデル）は
  turn-key 変換対象外**。ORT GenAI model builder による手動変換の可否を別スパイクで
  先に判定する（§4.3 の前提課題）。出典:
  [Run LLMs and other generative models（ONNX Runtime GenAI / Windows ML）](https://learn.microsoft.com/windows/ai/new-windows-ml/run-genai-onnx-models)、
  [Use any ONNX LLM in the AI Dev Gallery（変換対応モデル列挙）](https://learn.microsoft.com/windows/ai/ai-dev-gallery/tutorial-onnx)。
- 実機計測（NVIDIA GPU / Snapdragon X Elite / Intel Core Ultra）は人手が必要なため
  `gate:human-required` の子課題に分離する。

### 4.3 結論（M24 暫定方針）

M8 bench と zenz-v3 ONNX 変換可否スパイクの結果で最終確定する前提で、現時点の設計
決定を以下に固定する。

1. **v1.0 ベースライン = R1（llama.cpp, CPU 既定 + CUDA optional）**。GGUF 資産を無変換で
   使え、M8 で実装済み。Copilot+ PC でも当面は zenz-v3 を R1 CPU で動かす（小型モデル
   のため CPU でも省電力許容）。
2. **M24 の Copilot+ NPU 経路 = R2（Windows ML）に振る**。ベンダ別 SDK を同梱せず、
   Windows ML の自動 EP 配信・自動選択・NPU→GPU→CPU フォールバックに委ねる（ただし
   first-run の EP 取得・登録は §4.6 のとおりアプリ側で明示実行が必要）。
3. **R2 は zenz-v3 → ONNX Runtime GenAI 変換の成否に依存する後続トラック（v1.0 後）**。
   変換可否スパイクを R2 着手の前提課題とし、不可なら R2 を保留して R1 CPU を Copilot+
   の既定に据える。**NPU 必須化はしない**（入力が止まらないことを最優先）。
4. **不採用 / 集約**: 単体 DirectML backend（旧 A）は不採用。ベンダ横断 GPU は
   R1=ggml-vulkan。DirectML 由来のアクセラレーションは R2（Windows ML）に一本化。
5. Win11 24H2 (build 26100) 未満では R2 の NPU / HW EP が使えないため、OS バージョン
   判定で R1 CPU にフォールバックする。

> **参考（fkunn1326/azooKey-Windows, MIT）**: 先行 Windows 実装が R1（llama.cpp）を
> **CPU / CUDA / Vulkan の 3 プリビルド**で実働実証済み（Vulkan(ggml-vulkan) を含む）。
> R1 を v1.0 ベースラインとする本方針の外部裏付け。バックエンド可否の **一次フィルタ**
> として、当該バックエンドのランタイム DLL の存在判定（`cudart64_12.dll`+`cublas64_12.dll`
> = CUDA / `vulkan-1.dll` = Vulkan）を設定 UI の可否提示に流用できる（ただし DLL 存在 ≠
> 動作保証。版不一致・破損は検出不可のため、実選択は §3 DXCore 列挙 + `BackendSelector`
> に委ねる）。詳細は DEV-98 / DEV-120 のコメント参照。

### 4.4 BackendKind / LoadModel 境界の拡張ポリシー

現行コードの enum は `enum class BackendKind { Cpu, Cuda };`
（`inference-host/include/azookey/host/InferenceEngine.h`、いずれも llama.cpp エンジン）。
拡張は**後方互換（既存シリアライズ値を不変・追記のみ）**で行う。

- R1 アクセラレータ追加: `Vulkan`（ggml-vulkan, ベンダ横断 GPU）を予約追加。
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

複数 Host プロセスが同じモデルを使う場合（将来）に物理メモリを共有するため、
モデルロードは `CreateFileMapping` + `MapViewOfFile` で行う。

### 5.1 実装

`inference-host/src/MmapModelLoader.cpp`（新規）：

```cpp
class MmapModelLoader {
public:
    bool Load(const std::wstring& path);
    const std::byte* Data() const { return data_; }
    size_t Size() const { return size_; }
    ~MmapModelLoader() { Unload(); }

private:
    HANDLE       file_       = INVALID_HANDLE_VALUE;
    HANDLE       mapping_    = nullptr;
    std::byte*   data_       = nullptr;
    size_t       size_       = 0;
    void Unload();
};
```

実装：

```cpp
bool MmapModelLoader::Load(const std::wstring& path) {
    file_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                        nullptr, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER fs;
    GetFileSizeEx(file_, &fs);
    mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mapping_) { Unload(); return false; }
    data_ = static_cast<std::byte*>(MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0));
    if (!data_) { Unload(); return false; }
    size_ = static_cast<size_t>(fs.QuadPart);
    return true;
}
```

### 5.2 llama.cpp との接続

llama.cpp の `llama_model_load_from_buffer`（独自ヘッダで宣言）または、
GGUF を `LoadModelRequest.path` から mmap 経由で `llama_model_load_from_file`
（mmap=true）に渡す。

### 5.3 利点

- 複数 Host インスタンス間で物理ページ共有
- カーネルのページキャッシュに乗せて 2 回目以降の起動を高速化
- WorkingSet に計上されない（メモリプレッシャ時にカーネルが破棄可能）

## 6. 省電力モード連動

### 6.1 電源状態の検出

```cpp
SYSTEM_POWER_STATUS status;
GetSystemPowerStatus(&status);
bool on_battery = (status.ACLineStatus == 0);
```

`WM_POWERBROADCAST` メッセージで状態変化を購読：

```cpp
case WM_POWERBROADCAST:
    if (wParam == PBT_APMPOWERSTATUSCHANGE) {
        GetSystemPowerStatus(&status);
        backend_selector_->OnPowerStatusChange(status.ACLineStatus);
    }
    break;
```

### 6.2 バッテリ時の挙動

| 項目 | AC 接続時 | バッテリ駆動時 |
|---|---|---|
| バックエンド選択 | §4.5 のフォールバック段位に従う（R2 NPU/GPU → R1 CUDA/Vulkan/CPU） | §4.5 に従う（**R2 は NPU device のみ**、§4.6 のデバイスレベル絞り込み。discrete GPU / CUDA / Vulkan を回避し、NPU 不可なら R1 CPU） |
| 予測頻度 | 入力ごと | 200ms デバウンス |
| ライブ変換重い推論 | 有効 | 無効（Fast レーンのみ） |
| Post-Commit Lint | 有効 | 無効 |
| Persona 再計算間隔 | 24h | 7 日 |

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

専用ジョブ例（**ビルドゲートのみ・ctest なし・arch を含む一意な artifact 名**）:

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
      # ctest は実行しない（x64 ホストで ARM64 バイナリは走らない）。実行は §8.4。
      - uses: actions/upload-artifact@v4
        if: always()
        with:
          name: windows-arm64-crossbuild-logs   # ← arch を含め一意化
          path: |
            configure-arm64.log
            build-arm64.log
          if-no-files-found: ignore
      # このジョブの fail ゲートは configure + build のみを見る（tests_ec は参照しない）。
```

既存 `windows-build`（x64）matrix に ARM64 を**足さない**ため、x64 側の artifact 名・
fail ゲートは無改変で済む。`windows-release-arm64` preset は `windows-release` を継承し、
上記 toolchain file を `CMAKE_TOOLCHAIN_FILE` で指す（または同じ変数群を
`cacheVariables` で直接指定する）。なお §8.4 の `windows-11-arm` ネイティブテストジョブも
別ジョブとして独立させ、そちらは build + ctest + 自前の fail ゲートを持つ。

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
| zenz-v3 GGUF / ONNX モデル本体 | 非同梱・初回 DL | 非同梱・初回 DL | §1.2 / §1.6 と一貫。アーキ非依存（モデルは共通） |

要点:

- **ARM64 の add-on は作らない**（CUDA add-on は x64 限定）。ARM64 のアクセラレーションは
  R2（Windows ML / QNN EP、非バンドル）に一本化し、MSIX を肥大させない。
- これにより配布パッケージ構成は **base MSIX（x64 / ARM64 の 2 アーキ）+ x64 専用 CUDA
  optional add-on** に収束する。アーキ別に別 payload を後決めする必要はない（後戻り回避）。
- リリース CI（`.github/workflows/release.yml`）は現状 `Platform=x64` のみ MSIX をビルド
  する。winget マニフェスト（`docs/sideload-packaging-spec.md`）が `arm64.msix` を参照する
  ため、M28/M29 有効化時に **wapproj を `Platform=ARM64` でも build する matrix**へ拡張
  する（wapproj は x64 ホストから ARM64 MSIX をクロスパッケージ可能）。

### 8.4 ARM64 テスト実行と受け入れ条件

ARM64 バイナリは x64 ホストで実行できないため、`ctest` は ARM64 ネイティブ環境が要る。

| 経路 | 用途 | 制約 |
|---|---|---|
| **`windows-11-arm` GitHub-hosted ランナー** | ARM64 ネイティブ build + ctest | **public / private 両方で利用可**（標準ランナー）。public は無料、private は spec 4 vCPU/16GB ではなく **2 vCPU/8GB**で **GitHub の無料枠を消費し超過分は従量課金**。label 失敗ではない |
| **self-hosted ARM64 / Azure ARM64 VM**（Ampere Altra） | 課金回避 / 反復テスト最適化 | インフラ用意・運用コストが要る。ARM64 VM は x64 ハードでは作成不可（クラウドホスト必須） |
| **Snapdragon X Elite 実機** | NPU 含む統合検証 | `gate:human-required`（実装フェーズで付与）。CI 化しない |

**M27 受け入れ条件の確定（roadmap M27 を補足）**:

1. **CI 緑ゲート（必須・自動）** = §8.1 のクロスビルド（clang-cl / ARM64 toolchain）が
   既存 x64 ランナーで成功すること。ARM64 バイナリの生成までを CI の最小ゲートとする
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
| BackendSelector | `inference-host/tests/backend_selector_test.cpp` | DXCore 列挙のモック、優先順位、AC/バッテリ切替 |
| MmapModelLoader | `inference-host/tests/mmap_loader_test.cpp` | ロード成功、ファイル不在エラー、サイズ取得 |
| Power Status | `inference-host/tests/power_status_test.cpp` | `WM_POWERBROADCAST` で BackendSelector が呼ばれる |
| DPI changed | `tsf-tip/tests/dpi_test.cpp` | Windows 限定。フォント再計算が呼ばれるか |
| ARM64 build | CI matrix | ビルド成功 + テスト pass |

## 10. 参照

- DXCore SDK: <https://learn.microsoft.com/windows/win32/dxcore/>
- DirectML: <https://learn.microsoft.com/windows/ai/directml/>
- QNN SDK: <https://www.qualcomm.com/developer/software/qualcomm-ai-engine-direct-sdk>
- OpenVINO: <https://docs.openvino.ai/>
- ベース：`docs/zenzai-gpu-route.md`
- ライブ変換のレーン分け：`docs/rich-features-spec.md` X-4-1
- マルチディスプレイ：`docs/legacy-parity-spec.md` 9 章
