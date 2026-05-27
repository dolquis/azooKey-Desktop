# Zenzai モデル管理 UI 仕様

対象リポジトリ: dolquis/azooKey-Desktop
対応マイルストーン: M45（プライバシー / モデル管理 / 学習データ UI トラック）
関連: `plans/windows-port-roadmap.md` M8 / M30 / M24、
      `docs/zenzai-gpu-route.md`、
      `docs/copilot-pc-backend-spec.md`、
      `docs/dev-infrastructure-spec.md` §12 (M44 診断)
作成日: 2026-05-27
位置づけ: Phase 6-C 拡張（M30 設定アプリのモデル管理タブ）

## 1. 目的

Zenzai GGUF モデルの配置・検証・ロード・backend 選択・fallback 状態を、
GUI で完結して操作可能にする。M8 で実装したモデルロード境界と、M30 の
WinUI 3 設定アプリを土台に、ユーザーが以下を行えるようにする:

- モデル一覧の閲覧と検証状態（valid / invalid / VRAM 不足の可能性）
- backend の自動推奨と手動切替（CPU / CUDA / DirectML / NPU）
- ベンチマーク実行と p50/p95/load_ms/rss_mb/vram_mb の表示
- 選択モデルの永続化（Host 再起動後の自動ロード）

## 2. 非目標

- モデルのダウンロード機能（M45 範囲外。将来 M に分離）
- モデルの自動更新機能（同上）
- llama.cpp バインディング自体の改修（M8 / M24 の責務）
- 推論精度評価（M52 変換品質評価ベンチの責務）

## 3. モデル検出

### 3.1 既定ディレクトリ

```
%LOCALAPPDATA%\azooKey\models\
```

サブディレクトリは再帰的にスキャンする（1 階層のみ）。
拡張子は `.gguf` のみ。

### 3.2 検出情報

各モデルについて以下のメタデータを `ModelCatalogEntry` として保持する:

| フィールド | 内容 | 取得元 |
|---|---|---|
| `path` | 絶対パス | filesystem |
| `file_name` | ファイル名 | filesystem |
| `size_bytes` | ファイルサイズ | filesystem |
| `sha256` | SHA-256 ハッシュ（任意、初回または明示時のみ計算） | computed |
| `gguf_valid` | magic / version 検証結果 | GGUF parse |
| `model_family` | metadata から推定（`gpt2` / `llama` / `mistral` 等） | GGUF metadata |
| `quantization` | `Q4_K_M` / `Q5_K_M` / `Q8_0` 等 | GGUF metadata |
| `n_params` | 推定パラメータ数 | GGUF metadata |
| `recommended_backend` | `cpu` / `cuda` / `directml` / `npu` | §5 ロジック |
| `last_load_status` | `success` / `failed` / `not_loaded` | 過去ログ |
| `last_error` | 直近エラー文字列 | 過去ログ |
| `last_benchmark` | 直近ベンチマーク結果（§6） | 過去ログ |

### 3.3 GGUF 検証

M8 で実装した GGUF magic / version 検証を再利用する。invalid GGUF は
`gguf_valid = false` として一覧に残し、ロード対象から除外する。

不正検出パターン:
- magic mismatch（`GGUF` 以外）
- version 不一致（サポート範囲外）
- ファイル末端切詰め（claimed size > actual size）
- metadata 必須キー欠落

## 4. IPC

### 4.1 ListModels

```json
{
  "message_type": "ListModelsRequest",
  "payload": {
    "directory": "%LOCALAPPDATA%\\azooKey\\models",
    "compute_sha256": false
  }
}
```

```json
{
  "message_type": "ListModelsResponse",
  "payload": {
    "models": [
      {
        "path": "C:\\Users\\me\\AppData\\Local\\azooKey\\models\\zenzai-small-q4.gguf",
        "file_name": "zenzai-small-q4.gguf",
        "size_bytes": 1234567890,
        "gguf_valid": true,
        "metadata": {
          "model_family": "llama",
          "quantization": "Q4_K_M",
          "n_params": 1300000000
        },
        "recommended_backend": "cuda",
        "last_load_status": "success"
      }
    ]
  }
}
```

### 4.2 BenchmarkModel

```json
{
  "message_type": "BenchmarkModelRequest",
  "payload": {
    "path": "...",
    "backend": "cuda",
    "cases": ["nihongo", "watashi", "konnichiwa", "kyouhaiitenki"],
    "iterations": 50,
    "warmup": 5
  }
}
```

```json
{
  "message_type": "BenchmarkModelResponse",
  "payload": {
    "backend": "cuda",
    "p50_ms": 18.2,
    "p95_ms": 41.7,
    "p99_ms": 78.4,
    "load_ms": 1300,
    "rss_mb": 1850,
    "vram_mb": 1400,
    "status": "success",
    "iterations_completed": 50,
    "error": null
  }
}
```

ベンチ中は Host を一時的に占有するため、対象 backend を起動 → 全
iteration 実行 → unload してから既存 backend に戻す。ベンチ実行中は
他の QueryCandidates をブロックせず、Heavy レーン扱いで非同期実行
する（M24 のスケジューラを利用）。

### 4.3 IPC 追加方針

`MessageType` enum 末尾に `ListModels` / `BenchmarkModel` を append。
既存 14 種の後ろに追加するため M40 の互換性ルールを満たす。
`Handshake` 時の capabilities で client が対応版本を判別する。

## 5. Backend 自動選択

### 5.1 推奨ロジック

`backendPreference = auto` のとき、以下の優先順位で backend を選ぶ:

1. **ベンチマーク履歴があれば p95 最良**（直近 7 日以内、同一モデルで
   `status = success` のもの）
2. NPU が利用可能 かつ モデル対応あり → `npu`
3. AC 電源 かつ CUDA 利用可能 → `cuda`
4. DirectML 利用可能（DirectX 12 GPU 検出） → `directml`
5. CPU fallback → `cpu`

NPU 検出は `IDXCoreAdapterList` 経由（M24 で実装）。AC 電源判定は
`GetSystemPowerStatus`。

### 5.2 後方互換

既存の root レベル `backendPreference` キー（M11 / M24 で先行導入）
との衝突を避けるため、解決順位を以下とする:

1. `model.backendPreference`（M45 で追加）
2. root `backendPreference`（既存）
3. デフォルト `auto`

両方が存在する場合は警告ログを出し、`model.backendPreference` を採用
する。

### 5.3 失敗時 fallback

選択した backend で `LoadModel` が失敗した場合:

1. ログに `error_code: business / backend_load_failed` を記録
2. `last_error` を `ModelCatalogEntry` に保存
3. CPU backend で再試行
4. CPU も失敗した場合は `SimpleConverter` fallback（M47
   `DegradedModel` 状態）

ここで Host が落ちないことが必須要件（M47 受け入れ条件と整合）。

## 6. UI（設定アプリ Model タブ）

```
[Zenzai モデル]

現在の状態: SimpleConverter fallback 中
Backend: auto → CPU

モデル一覧:
  ✅ zenzai-small-q4.gguf     1.2GB   valid   CPU/CUDA 対応
  ⚠️ zenzai-large.gguf        4.8GB   VRAM 不足の可能性
  ❌ broken.gguf              900MB   invalid magic

[モデルを追加] [フォルダを開く] [選択モデルをロード] [ベンチマーク]

推奨:
  この PC では zenzai-small-q4.gguf + CUDA が推奨です。
  p50: 18ms / p95: 42ms
```

### 6.1 操作

- **モデルを追加**: explorer ダイアログで `.gguf` を選択 → 既定ディレ
  クトリへコピー（権限不足時は警告）
- **フォルダを開く**: explorer で `%LOCALAPPDATA%\azooKey\models\` を
  開く
- **選択モデルをロード**: `LoadModel` IPC を送信、進捗バー表示
- **ベンチマーク**: `BenchmarkModel` IPC を送信、結果を表示・保存

### 6.2 状態表示

| 表示 | 意味 |
|---|---|
| ✅ | `gguf_valid = true` かつ ロード成功履歴あり |
| ⚠️ | `gguf_valid = true` だが VRAM 不足 / ロード失敗履歴あり |
| ❌ | `gguf_valid = false` または重大エラー |
| 🔄 | ロード中 / ベンチマーク中 |

## 7. 設定スキーマ

`settings/mvp-settings.schema.json` に以下を追加（既存キーとの衝突なし、
§3 で確認済み）:

```json
{
  "model": {
    "enabled": true,
    "selectedPath": "",
    "backendPreference": "auto",
    "nGpuLayers": -1,
    "autoLoadOnHostStart": true,
    "fallbackToSimpleConverter": true,
    "benchmarkOnModelChange": false,
    "benchmarkHistory": []
  }
}
```

| キー | 型 | 既定値 | 意味 |
|---|---|---|---|
| `enabled` | bool | true | Zenzai を使うか（false なら SimpleConverter 固定） |
| `selectedPath` | string | "" | 選択中モデルの絶対パス |
| `backendPreference` | enum | "auto" | `auto` / `cpu` / `cuda` / `directml` / `npu` |
| `nGpuLayers` | int | -1 | -1 = 全レイヤ GPU、0 = CPU only、正値 = 部分オフロード |
| `autoLoadOnHostStart` | bool | true | Host 起動時に `selectedPath` を自動ロード |
| `fallbackToSimpleConverter` | bool | true | ロード失敗時に Simple へ落ちる |
| `benchmarkOnModelChange` | bool | false | モデル変更時に自動ベンチ |
| `benchmarkHistory` | array | [] | 直近 7 件のベンチ結果（自動回収） |

## 8. 性能・コスト

- `ListModels` は ファイル列挙 + GGUF magic 読み（先頭 4 KB）のみで
  完結させる。完全 parse は明示的 `compute_sha256 = true` または ロード
  試行時のみ。
- `BenchmarkModel` の実行は最大 60 秒。timeout 時は `status = timeout` を
  返す。
- Host 占有中の表示は M47 `Recovering` 状態として候補ウィンドウに通知。

## 9. テスト

- unit: `ModelCatalog` の検出 / 検証 / 履歴更新
- integration: 不正 GGUF・サイズ不一致・metadata 欠落で fallback する
- snapshot: `ListModels` / `BenchmarkModel` の JSON schema 固定
- e2e（M50 と連携）: GUI 上でモデル切替 → 再起動 → 自動ロード

## 10. M45 受け入れ条件

- `%LOCALAPPDATA%\azooKey\models\` 内の GGUF が一覧表示される
- invalid GGUF はロード不可として表示される
- モデルロード失敗時も Host は落ちず、`SimpleConverter` fallback へ
  移行する
- ベンチマーク結果が GUI に表示される
- 選択モデルが Host 再起動後も自動ロードされる
- `model.backendPreference` が root `backendPreference` より優先される
- `--json` 出力（IPC）が stable schema として CI でテストされる
