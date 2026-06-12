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
- backend の自動推奨と手動切替（CPU / CUDA / WinML〔EP 自動選択〕。旧 `directml` /
  `npu` は `winml` に統合・非推奨。`docs/copilot-pc-backend-spec.md` §4.4 参照）
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
| `recommended_backend` | `cpu` / `cuda` / `winml` | §5 ロジック（旧 `directml` / `npu` は `winml` に統合・非推奨） |
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

既存 IPC エンベロープ `{version, request_id, type, trace_id, payload}`
（`ipc/src/Messages.cpp`）に従い、`type` は `MessageType` enum 値文字列を
使う（`ListModelsRequest` のような派生名ではなく `ListModels`）。
request と response は同一 `type` を共有し、payload schema で区別する
（他の `QueryCandidates` 等と同じ慣習）。

Request（client → host）:

```json
{
  "version": 1,
  "request_id": 42,
  "type": "ListModels",
  "trace_id": "018fd2c2-2a3e-7c9a-b8e1-7f3a92d4c5e2",
  "payload": {
    "directory": "%LOCALAPPDATA%\\azooKey\\models",
    "compute_sha256": false
  }
}
```

Response（host → client、同一 `request_id` / `trace_id` を返す）:

```json
{
  "version": 1,
  "request_id": 42,
  "type": "ListModels",
  "trace_id": "018fd2c2-2a3e-7c9a-b8e1-7f3a92d4c5e2",
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

Request（client → host）:

```json
{
  "version": 1,
  "request_id": 43,
  "type": "BenchmarkModel",
  "trace_id": "018fd2c2-...",
  "payload": {
    "path": "...",
    "backend": "cuda",
    "cases": ["nihongo", "watashi", "konnichiwa", "kyouhaiitenki"],
    "iterations": 50,
    "warmup": 5
  }
}
```

Response（host → client）:

```json
{
  "version": 1,
  "request_id": 43,
  "type": "BenchmarkModel",
  "trace_id": "018fd2c2-...",
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

ベンチは **既存稼働中の backend と並行する独立 runtime インスタンス**
（別 InferenceEngine + 別 model handle）で実行し、ライブの
`QueryCandidates` は本番 backend に routing し続ける。M24 完了済み環境
では Heavy レーンスケジューラ上に独立 worker として配置し、M24 未完了
環境では M45 専用 worker thread で逐次処理する（どちらの場合も
ライブクエリ用 worker とは別 thread / 別 backend ハンドルを保つ）。
対象 backend のロード / iteration 実行 / unload が完了するまでベンチ用
runtime のみを更新し、既存 backend の差し替えは行わない。これにより
load/unload とライブクエリの race を構造的に排除する。

### 4.3 IPC 追加方針

`MessageType` enum の `Unknown` sentinel の**前**に `ListModels` / `BenchmarkModel`
を追加する（enum は 13 named 型 + `Unknown` = 14 entries。末尾の `Unknown` の後ろに
追加しない）。これにより M40 の互換性ルールを満たす。
`Handshake` 時の capabilities で client が対応版本を判別する。

## 5. Backend 自動選択

### 5.1 推奨ロジック

`backendPreference = auto` のとき、`docs/copilot-pc-backend-spec.md` §4.3 / §4.5 の
M24 決定（R1=llama.cpp / R2=Windows ML）に従って選ぶ:

1. **engine の選択**: ONNX 変換モデルかつ Win11 24H2+ かつ対象 EP 取得・登録済み
   （§4.6）なら **R2（`winml`、EP 自動選択 NPU→GPU→CPU）**。それ以外（GGUF モデル /
   非対応 OS / EP 未取得）は **R1**: NVIDIA かつ CUDA 可なら `cuda`、不可なら `cpu`。
   バッテリ駆動時は §4.5 に従い discrete GPU(CUDA) を後回しにする。
2. **同一順位内のタイブレーカーとしてのみ**ベンチマーク履歴を参照する
   （直近 7 日以内、同一モデル、`status = success` のもの。同 rank 内に
   複数 backend がある場合に p95 最良を採用）。順位を跨いだ並べ替えは
   行わない

M45 は backend 順位を独自に上書きせず、`copilot-pc-backend-spec.md` §4 で定義した
順位をそのまま利用する（既存 root `backendPreference` の `auto` 挙動を変えないため）。
順位を変更したい場合は同 spec を更新するか、ユーザーが明示的に
`model.backendPreference` を非 `auto` 値に設定する。

R2 の可用性検出（NPU/GPU EP）は Windows ML の `ExecutionProviderCatalog`
（`FindAllProviders()` の `ReadyState`、§4.6）で行う。`IDXCoreAdapter` 列挙は
device 名表示などの補助に留める。

> **`directml` / `npu` 値の非推奨（M24 決定で更新）**: 旧 `backendPreference` の
> `directml` / `npu` は、具体 EP を enum 値化しない方針（§4.4）に伴い **`winml` に
> 統合・非推奨**とする。後方互換のため受理はするが、内部的に `winml`（EP 自動選択）へ
> マップする（不能なら §5.3 で R1 CPU フォールバック）。新規設定は `winml` を使う。

### 5.2 後方互換

既存の root レベル `backendPreference` キー（M11 / M24 で先行導入）
との衝突を避けるため、解決順位を以下とする:

1. `model.backendPreference`（M45 で追加）
2. root `backendPreference`（既存）
3. デフォルト `auto`

両方が存在する場合は警告ログを出し、`model.backendPreference` を採用
する。

`epPreference` も同様に二層（`model.epPreference` → root `epPreference` → 既定 `auto`）で
解決する。schema 正典は `settings/mvp-settings.schema.json`（root tier の
`backendPreference` / `epPreference` と、`model` オブジェクト内の同名キーの双方を定義）。

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
    "epPreference": "auto",
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
| `backendPreference` | enum | "auto" | `auto` / `cpu` / `cuda` / `winml`。旧 `directml` / `npu` は受理するが `winml` に統合・**非推奨**（§5.1） |
| `epPreference` | enum | "auto" | R2(`winml`) 時の EP 希望: `auto` / `npu` / `gpu` / `cpu`（`copilot-pc-backend-spec.md` §4.4） |
| `nGpuLayers` | int | -1 | R1(llama.cpp) 専用。-1 = 全レイヤ GPU、0 = CPU only、正値 = 部分オフロード |
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
