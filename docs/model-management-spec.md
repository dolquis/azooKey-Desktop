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

Zenzai モデル（R1=GGUF / R2=ONNX Runtime GenAI ディレクトリ。
`docs/copilot-pc-backend-spec.md` §4）の配置・検証・ロード・backend 選択・fallback
状態を、GUI で完結して操作可能にする。M8 で実装したモデルロード境界と、M30 の
WinUI 3 設定アプリを土台に、ユーザーが以下を行えるようにする:

- モデル一覧の閲覧と検証状態（valid / invalid / VRAM 不足の可能性。R1/R2 両形式）
- backend の自動推奨と手動切替（CPU / CUDA / WinML〔EP 自動選択〕。旧 `directml` /
  `npu` は `winml` に統合・非推奨。`docs/copilot-pc-backend-spec.md` §4.4 参照）
- ベンチマーク実行と p50/p95/load_ms/rss_mb/vram_mb の表示
- 選択モデルの永続化（Host 再起動後の自動ロード）

## 2. 非目標

- M45 の設定 UI からダウンロードを開始する機能。
  v1.0 の最小取得経路（初回起動時 DL + 手動配置フォールバック）は
  `docs/sideload-packaging-spec.md` §1.6.1 が正典であり、共通取得基盤は §3.1.2 に定める
- モデルの自動更新機能（同上）
- llama.cpp バインディング自体の改修（M8 / M24 の責務）
- 推論精度評価（M52 変換品質評価ベンチの責務）

## 3. モデル検出

### 3.1 既定ディレクトリ

```
%LOCALAPPDATA%\azooKey\models\
```

### 3.1.1 宣言的モデルカタログ（DEV-438）

DEV-438 では、ダウンロード・検証・プリフェッチを行わず、Zenzai GGUF の
宣言的カタログとローカルパス解決だけを実装する。カタログ v1 の schema 正典は
`settings/model-catalog.schema.json` とする。

カタログは、`%LOCALAPPDATA%\azooKey\models\zenzai\` 配下に置かれる GGUF ファイルを
次の形で宣言する:

```json
{
  "schemaVersion": 1,
  "defaultModelId": "zenzai-small-q4",
  "models": [
    {
      "id": "zenzai-small-q4",
      "displayName": "Zenzai small Q4",
      "fileName": "zenzai-small-q4.gguf",
      "sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
      "backendHint": "llama.cpp"
    }
  ]
}
```

| キー | 型 | 既定値 | 意味 |
|---|---|---|---|
| `schemaVersion` | const | 1 | catalog schema version |
| `defaultModelId` | string | 先頭 `models[].id` | 既定モデル。指定時は `models[].id` と一致必須 |
| `models[].id` | string | 必須 | モデル識別子。ASCII 英数字・`.`・`_`・`-` のみ |
| `models[].displayName` | string | `id` | UI / ログ表示名。指定する場合は非空 |
| `models[].fileName` | string | 必須 | `zenzai\` 配下の GGUF ファイル名。絶対パス、サブディレクトリ、control 文字、`..`、drive prefix は不可 |
| `models[].sha256` | string | 必須 | 期待 SHA-256（64 hex）。DEV-438 では保持のみで、検証は後続 B が行う |
| `models[].backendHint` | enum | `"llama.cpp"` | 現行 v1 は GGUF / llama.cpp のみ |

resolver は catalog を受け取り、`DefaultZenzaiModelDirectory(modelsDir)`
（`modelsDir\zenzai`）と `fileName` を結合してローカルパスを返す。存在確認は
`is_regular_file` で行い、未配置の場合も解決済みパスと `MissingLocalFile` を返す。
このため単体テストは実 GGUF を配置せず、一時ファイルの有無だけで完結できる。

### 3.1.2 GGUF 取得基盤（DEV-439）

DEV-439 の `HttpDownloader` は、呼び出し元が指定した URL から GGUF を取得し、
`%LOCALAPPDATA%\azooKey\models\zenzai\` 配下の確定パスへ配置する共通 GET 基盤である。
カタログから URL を選ぶ処理、Host 起動時のプリフェッチ、`selectedPath` の更新は
後続処理の責務とし、ダウンローダはネットワーク取得と整合検証だけを担当する。

取得手順は次のとおり。

1. 確定パスの既存ファイルが期待 SHA256 と一致する場合は、ネットワークへ接続せず
   `AlreadyValid` を返す。
2. `<file>.gguf.part` が存在する場合は、そのサイズを始点とする `Range: bytes=<size>-`
   を送る。
   `206 Partial Content` は `Content-Range` の始点が一致した場合だけ追記する。
   サーバーが Range を無視して `200 OK` を返した場合は `.part` を切り詰め、応答全体で
   再開する。
   `416 Range Not Satisfiable` では完成済み `.part` の SHA256 を先に確認し、不一致なら
   Range なしの GET を一度だけ再試行する。
3. 受信完了後に Windows CNG（BCrypt）で `.part` の SHA256 を計算する。
   期待値と一致した場合だけ、同じディレクトリ内で `MoveFileExW` の replace +
   write-through により確定パスへ昇格する。
4. HTTP 失敗、書き込み失敗、SHA256 不一致では確定パスを変更しない。
   `.part` は次回レジューム用に残すため、ネットワーク不通時も既存の確定ファイルを
   継続利用できる。

本番 URL は HTTPS を用い、WinHTTP 既定の証明書検証を無効化しない。
HTTP はループバックの自動テストに限って使用する。
プロキシは WinHTTP の automatic proxy を使用し、resolve / connect / send / receive の
タイムアウトを呼び出しごとに設定する。

サブディレクトリは再帰的にスキャンする（1 階層のみ）。検出対象は 2 種:

- **R1（llama.cpp）**: 拡張子 `.gguf` ファイル。
- **R2（Windows ML / ONNX Runtime GenAI）**: ORT GenAI モデル**ディレクトリ**
  （`genai_config.json` を含むフォルダを 1 エントリとして扱う）。zenz-v3 の ONNX 変換
  モデルが optional パッケージとして配置された場合に検出する
  （`docs/copilot-pc-backend-spec.md` §4.3、変換可否は同 §4.2 のスパイク依存）。

R2 モデルが存在し前提（Win11 24H2+ / EP 取得・登録可、§4.6）を満たす場合のみ `winml`
経路へ入れる（§5.1）。R2 モデルが無ければ R1（GGUF）のみを一覧する。非対応拡張子の
ファイルは無視する。

### 3.2 検出情報

各モデルについて以下のメタデータを `ModelCatalogEntry` として保持する:

| フィールド | 内容 | 取得元 |
|---|---|---|
| `path` | 絶対パス（R2 はモデルディレクトリのパス） | filesystem |
| `file_name` | ファイル名 / ディレクトリ名 | filesystem |
| `format` | `gguf`（R1）/ `onnx_genai`（R2 ORT GenAI ディレクトリ） | filesystem |
| `size_bytes` | ファイル / ディレクトリ合計サイズ | filesystem |
| `sha256` | SHA-256 ハッシュ（任意、初回または明示時のみ計算。R2 は config 参照 ONNX 等） | computed |
| `valid` | 形式別検証結果（R1=GGUF magic/version、R2=`genai_config.json` + その config が参照する ONNX の存在、§3.3）。旧 `gguf_valid` は R1 別名として後方互換維持 | parse |
| `model_family` | metadata から推定（`gpt2` / `llama` 等。R2 は `genai_config.json` から） | GGUF metadata / genai_config |
| `quantization` | `Q4_K_M` / `Q5_K_M` / `Q8_0` 等（R1）。R2 は int4/int8 等 | GGUF metadata / genai_config |
| `n_params` | 推定パラメータ数 | GGUF metadata / genai_config |
| `recommended_backend` | `cpu` / `cuda` / `vulkan` / `winml` | §5 ロジック（`vulkan`=R1 ベンダ横断 GPU。旧 `directml` / `npu` は `winml` に統合・非推奨） |
| `last_load_status` | `success` / `failed` / `not_loaded` | 過去ログ |
| `last_error` | 直近エラー文字列 | 過去ログ |
| `last_benchmark` | 直近ベンチマーク結果（§6） | 過去ログ |

### 3.3 形式別検証

**R1（GGUF）**: M8 で実装した GGUF magic / version 検証を再利用する。invalid GGUF は
`valid = false`（旧 `gguf_valid`）として一覧に残し、ロード対象から除外する。

不正検出パターン:
- magic mismatch（`GGUF` 以外）
- version 不一致（サポート範囲外）
- ファイル末端切詰め（claimed size > actual size）
- metadata 必須キー欠落

**R2（ONNX Runtime GenAI）**: ディレクトリに `genai_config.json` が存在すること、
かつ **その config がパースでき、参照する ONNX ファイルが実在すること**を検証する。
ONNX のファイル名は `model.onnx` 固定ではなく config が指すため、ファイル名を
ハードコードせず config から解決する: `model.decoder.filename`（および pipeline 構成
では各ステージの `filename`）が指す相対パスのファイル、および tokenizer 関連ファイル
（`tokenizer.json` / `tokenizer_config.json` 等、config / 規約で要求されるもの）が揃うかを
確認する（ORT GenAI C API はディレクトリに `genai_config.json` があることを要件とし、
ONNX 本体パスは config の `filename` 系フィールドで定義される。
cf. https://onnxruntime.ai/docs/genai/api/c.html ,
https://onnxruntime.ai/docs/genai/reference/config.html ）。
`genai_config.json` 欠落・JSON 不正・参照 ONNX 欠落のいずれも `valid = false` として
一覧に残し、ロード対象から除外する（R1 と同じ degraded 扱い）。深い ONNX グラフ検証は
行わず、ロード失敗時は §5.3 の fallback に委ねる。

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
        "format": "gguf",
        "size_bytes": 1234567890,
        "valid": true,
        "metadata": {
          "model_family": "llama",
          "quantization": "Q4_K_M",
          "n_params": 1300000000
        },
        "recommended_backend": "cuda",
        "last_load_status": "success"
      },
      {
        "path": "C:\\Users\\me\\AppData\\Local\\azooKey\\models\\zenz-v3-onnx",
        "file_name": "zenz-v3-onnx",
        "format": "onnx_genai",
        "size_bytes": 980000000,
        "valid": true,
        "metadata": {
          "model_family": "gpt2",
          "quantization": "int4",
          "n_params": 760000000
        },
        "recommended_backend": "winml",
        "last_load_status": "not_loaded"
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

1. **engine の選択**: ONNX 変換モデルがあり Win11 24H2+ かつ対応 HW なら、まず §4.6 の
   EP 取得・登録フローを試行する（EP が `NotPresent` でも `EnsureReadyAsync()` で取得を
   試みる。**「未取得」を理由に即 R1 へ落とさない**）。取得・登録に成功したら
   **R2（`winml`）**。取得失敗（`Failure`）/ ONNX モデル無し / 非対応 OS のときに **R1**:
   NVIDIA かつ CUDA 可なら `cuda`、ベンダ横断 GPU（非 NVIDIA / R2 不可）なら `vulkan`
   （ggml-vulkan ビルド時）、いずれも不可なら `cpu`。
   バッテリ駆動時は §4.5 / §4.6 に従い、NPU 系 EP のみ取得し、かつ **セッションで
   デバイスレベルに NPU へ絞る**（`SetEpSelectionPolicy(MAX_EFFICIENCY)` か `GetEpDevices()`
   の `HardwareDevice.Type == NPU` フィルタ。EP は silicon と 1:1 でないため登録の限定だけ
   では不十分）。discrete GPU(CUDA / Vulkan / GPU device) を回避し、NPU device が無ければ
   R1 CPU。
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
| ✅ | `valid = true` かつ ロード成功履歴あり（R1=GGUF 検証 OK / R2=`genai_config.json` + その config が参照する ONNX 存在、§3.3） |
| ⚠️ | `valid = true` だが VRAM 不足 / ロード失敗履歴あり |
| ❌ | `valid = false` または重大エラー |
| 🔄 | ロード中 / ベンチマーク中 |

ステータスは format-neutral な `valid`（§3.2）で駆動する。`gguf_valid` は
R1 後方互換の別名であり、R2（`onnx_genai`）エントリは `gguf_valid` を持たない
ため、これを参照すると valid な ONNX GenAI モデルが ❌ 扱いになり Model タブ
から除外される。形式固有の詳細（GGUF magic / `genai_config.json`）は補助情報
としてのみ用いる。

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
| `backendPreference` | enum | "auto" | `auto` / `cpu` / `cuda` / `vulkan` / `winml`。`vulkan`=R1 ggml-vulkan（ベンダ横断 GPU）。旧 `directml` / `npu` は受理するが `winml` に統合・**非推奨**（§5.1） |
| `epPreference` | enum | "auto" | R2(`winml`) 時の EP 希望: `auto` / `npu` / `gpu` / `cpu`（`copilot-pc-backend-spec.md` §4.4） |
| `nGpuLayers` | int | -1 | R1(llama.cpp) 専用。-1 = 全レイヤ GPU、0 = CPU only、正値 = 部分オフロード |
| `autoLoadOnHostStart` | bool | true | Host 起動時に `selectedPath` を background preload する。ロード完了までは SimpleConverter で継続 |
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

- unit: `ModelCatalog` の宣言的 catalog parse / default 補完 / 不正 entry reject /
  `zenzai\` パス resolver、および一覧検出 / 検証 / 履歴更新
- integration: ループバック HTTP モックで SHA256 不一致時に rename しないこと、
  Range レジューム、Range 無視時の全量再取得、ネットワーク不通時の既存ファイル保持を
  検証する
- integration: 不正 GGUF・サイズ不一致・metadata 欠落で fallback する
- snapshot: `ListModels` / `BenchmarkModel` の JSON schema 固定
- e2e（M50 と連携）: GUI 上でモデル切替 → 再起動 → 自動ロード

## 10. M45 受け入れ条件

- `%LOCALAPPDATA%\azooKey\models\` 内の GGUF が一覧表示される
- invalid GGUF はロード不可として表示される
- モデルロード失敗時も Host は落ちず、`SimpleConverter` fallback へ
  移行する
- ベンチマーク結果が GUI に表示される
- 選択モデルが Host 再起動後も background preload され、ロード中 / 失敗時も
  SimpleConverter fallback で入力を継続できる
- `model.backendPreference` が root `backendPreference` より優先される
- `--json` 出力（IPC）が stable schema として CI でテストされる
