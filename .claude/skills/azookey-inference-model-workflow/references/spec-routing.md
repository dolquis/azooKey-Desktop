# 推論モデル変更のルーティング

## 仕様、実装、テスト

| 変更領域 | 最初に読む仕様 | 主な実装 | 優先テスト |
|---|---|---|---|
| Zenzai の変換、ロード、推論 | `docs/zenzai-inference-spec.md`, `docs/zenzai-gpu-route.md` | `inference-host/src/ZenzaiModelConverter.cpp`, `inference-host/src/InferenceEngine.cpp` | `host_engine_tests` |
| request dispatch と degraded mode | `docs/windows-tsf-host-architecture.md`, `tsf-ipc-protocol` skill | `inference-host/src/Dispatcher.cpp` | `host_dispatcher_tests` |
| scheduler、cancel、timeout | `docs/dev-infrastructure-spec.md`, `docs/rich-features-spec.md` | `inference-host/src/RequestScheduler.cpp` | `host_scheduler_tests` |
| ModelCatalog とモデル選択 | `docs/model-management-spec.md`, `settings/model-catalog.schema.json` | `inference-host/src/ModelCatalog.cpp` | `host_model_catalog_tests` |
| ユーザーデータパス | `docs/learning-data-management-spec.md` | `inference-host/src/UserDataPaths.cpp` | `host_user_data_paths_tests` |
| runtime 設定 | `docs/zenzai-gpu-route.md`, `docs/ai-backend-spec.md` | `inference-host/src/SettingsStore.cpp` | `host_settings_store_tests` |
| privacy と入力ログ | `docs/privacy-and-secure-input-spec.md` | engine / dispatcher のログ境界 | `host_engine_tests`, `host_dispatcher_tests` |
| 品質と性能の比較 | `docs/conversion-quality-benchmark-spec.md` | `bench/` | `azookey_zenzai_bench` と関連 bench |

## Runtime tier

| tier | 証明できること | 証明できないこと |
|---|---|---|
| llama.cpp 無効 / mock | Host の制御経路、fallback、エラー処理 | 実GGUFのロードと推論 |
| 最小GGUF probe | loader の入口、形式エラー、最低限の初期化 | 実用モデル品質、完全なtoken生成 |
| 完全GGUF / real model | 実ロード、実推論、Zenzai 経路、性能 | 別モデルや別GPUでの同等性 |

実モデル検証では `windows-llama-debug`、`llama_cpp=1`、`--require-model`、必要なら `--require-zenzai` を確認する。CTest では `zenzai-real-model` ラベルを候補にし、モデルがないための skip を合格として報告しない。

## 検証対象

- Host: `host_engine_tests`, `host_dispatcher_tests`, `host_scheduler_tests`
- パス、catalog、設定: `host_user_data_paths_tests`, `host_model_catalog_tests`, `host_settings_store_tests`
- CLI: `host_userdict_cli_tests`
- bench: `azookey_zenzai_bench`
- 横断確認: `cmake --build --preset windows-debug --target azookey_check`

Windows の実ビルドと CTest は Windows Headless CMake Build 手順に従う。ログにはモデルのローカル絶対パスやユーザー入力を必要以上に残さない。

## 実装後に失効する spec 記述の削除

Zenzai ロード・推論の実装が進むと、`docs/zenzai-inference-spec.md` や
`docs/model-management-spec.md` の「現状 X は未実装」「Y は fallback のみ」
「Z への委譲のみで実推論を行わない」といった記述が実体と食い違う。実装 PR では
対応する spec を grep し、実装した挙動と矛盾する現況記述を削除するか、
「M-N が定める」という定義文に書き換えてから Documentation impact を記載する
（詳細は `azookey-doc-governance` スキル）。
