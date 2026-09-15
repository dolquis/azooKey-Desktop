# 品質ハーネス変更のルーティング

## 仕様、実装、テスト

| 変更領域 | 最初に読む仕様 | 主な実装 | 優先テスト |
|---|---|---|---|
| latency smoke と IPC ベンチ | `docs/dev-infrastructure-spec.md` §4.5、`docs/debugging.md`「Bench」 | `bench/live_bench.cpp`、`bench/IpcBenchmark.cpp`（target `azookey_bench`） | `azookey_bench_smoke`（CTest の `add_test`、p95 上限付き） |
| 変換品質評価（top-k、MRR、typo） | `docs/conversion-quality-benchmark-spec.md` §4〜§10、§14 | `bench/ConversionQuality.cpp`、`bench/data/conversion_quality_smoke.jsonl` | `conversion_quality_tests`、`azookey_conversion_quality_smoke` |
| 結果 JSON と baseline 比較 | `docs/conversion-quality-benchmark-spec.md` §8、§14、`docs/dev-infrastructure-spec.md` §4.5 | `bench/BenchmarkResult.cpp`、`bench/BenchmarkCommandLine.cpp` | `benchmark_result_tests` |
| bench 用一時学習ファイル | `docs/conversion-quality-benchmark-spec.md` §3 | `bench/TemporaryLearningFile.cpp` | `temporary_learning_file_tests` |
| Zenzai 推論ベンチ | `docs/zenzai-inference-spec.md`、`azookey-inference-model-workflow` skill | `bench/zenzai_bench.cpp`（target `azookey_zenzai_bench`） | 実モデル label 付き CTest（pin モデルが要る） |
| NLL 計測 | `docs/nll-release-measurement.md`（計測記録。正典ではない）、`docs/modernbert-ja-scoring-spec.md` | `bench/nll_bench.cpp`（target `azookey_nll_bench`）、`bench/data/nll_fixture.*` | 実モデル label 付き CTest |
| 評価データの出典 | `docs/conversion-quality-benchmark-spec.md` §11、§13 | `bench/data/DATA-LICENSE.md` | なし（レビューで確認） |
| アプリ互換性ハーネス | `docs/dev-infrastructure-spec.md` §13、`compat-test/README.md` | `compat-test/runner/`、`compat-test/cases/`、`compat-test/targets/`（target `compat_test`） | `compat_test_unit_tests`、対話セッションでの `compat_test.exe` 実行 |
| MSIX の install / uninstall シナリオ | `docs/sideload-packaging-spec.md`、`azookey-packaging-release-workflow` skill | `compat-test/msix_install_uninstall.ps1` | `scripts/tests/msix-lifecycle-scenarios.Tests.ps1` |
| 定期実行 workflow | `docs/dev-infrastructure-spec.md` §4.5、§13、`azookey-ci-workflows` skill | `.github/workflows/benchmarks.yml`、`.github/workflows/compat.yml` | workflow_dispatch での実行 |

## 検証コマンドの選び方

- bench の単体: `benchmark_result_tests`、`conversion_quality_tests`、`temporary_learning_file_tests`
- bench 本体: `cmake --build --preset windows-release --target azookey_bench` のあと `just bench`
- compat の単体: `compat_test_unit_tests`（Windows 構成のみ登録）
- compat 本体: `compat-test/README.md` の手順で対話セッションから実行する
- 横断確認: `cmake --build --preset windows-debug --target azookey_check`

上の target 名は入口であり網羅ではない。CTest 一覧の正典は `docs/test-inventory.md` で、
target の追加・削除はそちらと `scripts/check_test_inventory.py` が追う。

## 実装後に失効する spec 記述の削除

指標や case を追加したら、`docs/conversion-quality-benchmark-spec.md` と
`docs/dev-infrastructure-spec.md` §13.3 の一覧を同じ変更で更新する。
測定値そのものは spec に書かず、`docs/nll-release-measurement.md` のような日付つき記録か
Linear の検証メモに残す（詳細は `azookey-doc-governance` スキル）。
