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

### 2.1 既定優先順位

```
NPU > DirectML > CUDA > CPU
```

理由：
- NPU は省電力で常時稼働に最適
- DirectML はベンダー横断、Windows 標準
- CUDA は速度最強だが NVIDIA 専用
- CPU は最終フォールバック

### 2.2 バッテリ駆動時の逆転

`SystemPowerStatus.ACLineStatus == 0`（バッテリ駆動）のとき：

```
NPU > CPU > DirectML > CUDA
```

CUDA / DirectML（GPU） はバッテリを激しく消費するため後回し。
NPU は省電力なので最優先のまま。

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
- 非対応の engine / EP 組合せは **fail-closed で R1 CPU に降格**し、`Health=degraded`
  + `last_error` で理由を返す（M8 の既存劣化モード契約と一致）。

### 4.5 フォールバック段位

| 電源 | 要求 | 降格順 |
|---|---|---|
| AC | R2(auto) | NPU EP → GPU EP → CPU EP →（engine 不可なら）R1 CPU |
| AC | R1 CUDA | CUDA → R1 CPU |
| AC | R1 Vulkan（非 NVIDIA / R2 不可の GPU） | Vulkan(ggml-vulkan) → R1 CPU |
| バッテリ | auto | NPU EP(R2) → R1 CPU（CUDA / Vulkan 等 discrete GPU を回避） |
| 任意 | 終端 | 常に **R1 CPU(GGUF)**。入力をブロックしない |

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
2. 取得方針:
   - 簡易: `EnsureAndRegisterCertifiedAsync()`（対応 EP を全 DL + 一括登録。初回は
     ネットワーク速度次第で数秒〜数分。**進捗 UX 必須**）。
   - 個別: `FindAllProviders()` で `ReadyState` を確認し、目的 EP に
     `EnsureReadyAsync()` → 成功時 `TryRegister()`。`ep_preference`（§4.4）で対象 EP を絞る。
3. `ReadyState` 遷移を扱う: `NotPresent`（未 DL）/ `NotReady`（DL 済・未登録）はいずれも
   `EnsureReadyAsync()`、`Ready` は `TryRegister()`。
4. **エラー / 進行中処理**: `EnsureReadyAsync()` の結果 `Status` を確認し、
   - `Failure` → `ExtendedError`(HRESULT) / `DiagnosticText` をログし、当該 EP を諦めて
     **R1 CPU にフォールバック**（§4.5、`Health=degraded` + `last_error`）。
   - `InProgress` → 完了を待ってからセッション生成。
5. **first-run UX**: 初回 DL は時間がかかるため、進捗インジケータ（download progress
   callback）を出す。オフライン / 制限ネットワーク環境は EP DL 不可のため R1 CPU 継続
   （bring-your-own EP は将来検討）。
6. 登録結果は ONNX Runtime の `GetEpDevices()` で検証可能（例: 登録後に
   `QNNExecutionProvider (DeviceType: NPU)` が現れる）。

この EP 取得・登録ステップの実装は M24（`WinMlBackend`）の必須要件とし、
`docs/dev-infrastructure-spec.md` の起動シーケンス / 診断にも EP ReadyState を含める。

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
| バックエンド優先順 | NPU > DirectML > CUDA > CPU | NPU > CPU > DirectML > CUDA |
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

## 8. ARM64 ビルド

### 8.1 CMake / CI

`.github/workflows/windows.yml` に matrix を追加：

```yaml
strategy:
  matrix:
    arch: [x64, arm64]
    include:
      - arch: x64
        cmake_arch: x64
      - arch: arm64
        cmake_arch: ARM64

steps:
  - name: configure
    run: cmake -S . -B build -G "Visual Studio 17 2022" -A ${{ matrix.cmake_arch }}
```

### 8.2 ARM NEON 最適化

llama.cpp は `LLAMA_NATIVE=ON` で NEON を自動有効化。CMake オプション：

```cmake
if (CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64")
    set(GGML_ARM_NEON ON)
endif()
```

### 8.3 Snapdragon X Elite NPU

> **更新（M24 決定、§4.3 が正典）**: 既定方針は **Windows ML（R2）経由で QNN EP を
> 自動利用**することであり、**QNN SDK を直接同梱しない**（EP は Windows Update 配信）。
> 以下の QNN SDK 直叩き構成は、Windows ML / QNN EP が要件を満たさないと判明した場合
> にのみ検討する fallback（非既定）として残す。`inference-host/src/QnnBackend.cpp` も
> その場合に限る新規ファイルであり、既定実装は `WinMlBackend.cpp`（§4.4）とする。

QNN SDK を直接使う場合（非既定・fallback）：

- `qnn_sdk/include/QNN/` をインクルードパスに追加
- `qnn_htp/lib/aarch64-windows-msvc/` をリンク
- バイナリ配布時は QNN ランタイム DLL を MSIX に同梱

`inference-host/src/QnnBackend.cpp`（非既定・fallback 時のみ新規）：

```cpp
class QnnBackend : public IBackend {
public:
    bool Initialize(const std::wstring& model_path) override;
    Result Infer(const std::string& prompt) override;
};
```

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
