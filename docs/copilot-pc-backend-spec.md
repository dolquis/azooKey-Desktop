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
    NPU      = 3,   // NPU 経由 (Qualcomm QNN / Intel OpenVINO / AMD MIGraphX)
};

struct BackendInfo {
    BackendKind kind;
    std::string device_name;      // "NVIDIA GeForce RTX 4090", "Snapdragon X NPU" 等
    uint64_t    available_memory_bytes;
    bool        is_battery_efficient;  // NPU=true、CUDA=false
};
```

## 2. 自動選択優先度

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

## 4. llama.cpp 経路 vs ONNX Runtime + DirectML EP

### 4.1 候補

| 経路 | バインディング | 配布サイズ | 量子化 | NPU 対応 |
|---|---|---|---|---|
| A. llama.cpp + DirectML backend | llama.cpp DML | 小 (〜20MB) | gguf | △ (実験) |
| B. ORT + DirectML EP | ONNX Runtime | 中 (〜50MB) | int4/int8 ONNX | ◯ |
| C. QNN SDK (Qualcomm) | QNN HTP | 小 | int4/int8 | ◎ (Snapdragon X) |
| D. OpenVINO | OpenVINO Runtime | 大 (〜100MB) | int4/int8 | ◎ (Intel) |

### 4.2 選定スパイク（M8 で実施）

`bench/zenzai_backend_bench.cpp`（新規）：

- 同一プロンプト「こんにちは」「日本語」等 20 件を各経路で実行
- メトリクス: 初回 LoadModel 時間 / P50 推論レイテンシ / RSS / 配布サイズ
- 判定基準（重み順）:
  1. P50 推論レイテンシ < 30ms
  2. 配布サイズ < 50MB
  3. 初回 LoadModel 時間 < 3 秒
  4. ARM64 サポート

### 4.3 結論プレースホルダ

M8 のスパイク結果でルートを 1〜2 本に絞る。本書ではここを **「TBD（M8 完了時に
本書を更新）」** とする。Phase 6 着手時点では「DirectML EP + ggml-cuda CUDA」を
仮の構成として実装スケジュールに組む。

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

QNN SDK を使う場合：

- `qnn_sdk/include/QNN/` をインクルードパスに追加
- `qnn_htp/lib/aarch64-windows-msvc/` をリンク
- バイナリ配布時は QNN ランタイム DLL を MSIX に同梱

`inference-host/src/QnnBackend.cpp`（新規、Phase 6-B 中盤）：

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
