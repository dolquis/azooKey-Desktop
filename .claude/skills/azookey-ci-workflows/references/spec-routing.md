# CI とワークフロー変更のルーティング

## workflow ごとの正典と検証

| workflow / 設定 | 役割 | 正典 | ローカルで回せる検査 |
|---|---|---|---|
| `.github/workflows/windows.yml` | build と CTest、検査、変更分類、advisory の集計。必須チェックは `ci-gate` が集約する。job の一覧は下の「windows.yml の job」 | `docs/dev-infrastructure-spec.md` §4.1〜§4.4、§4.7、§4.8 | `pre-commit run --all-files`、`check-jsonschema`（`azookey-settings-schema-evolution` 参照）、`scripts/test-powershell-quality.ps1`（Windows） |
| `.github/workflows/docs.yml`（ジョブは `docs-lint` と `docs-impact`） | docs-lint、AGENTS.md 予算、テスト一覧、Skill 参照、agent 定義、文書の参照の実在、文書と実体（IPC の型・設定キー・preset）の突合、docs impact ゲート | `docs/dev-infrastructure-spec.md` §4.3・§4.9、`docs/test-inventory.md` | `python3 scripts/docs-lint.py --baseline .docs-lint-baseline.json`、`python3 scripts/check_agent_instruction_size.py`、`python3 scripts/check_test_inventory.py`、`python3 scripts/check_skill_references.py`、`python3 scripts/check_agent_definitions.py`、`python3 scripts/check_doc_refs.py`、`python3 scripts/check_doc_contracts.py`、`python3 scripts/check_doc_format.py`、`python3 scripts/check_docs_budget.py`、`python3 scripts/check_doc_meta.py`、`python3 scripts/check_docs_impact.py --print-contract-paths`、`scripts/tests/` の各 `test_*.py` |
| `.github/workflows/sanitizers.yml` | 週次の ASan / UBSan（Linux）と MSVC ASan | `docs/dev-infrastructure-spec.md` §4.6 | `cmake --preset linux-asan-ubsan` で Linux 側を再現 |
| `.github/workflows/benchmarks.yml` | Release benchmark の履歴と回帰警告 | `docs/dev-infrastructure-spec.md` §4.5、`azookey-quality-harness` skill | bench の単体テスト（`benchmark_result_tests`） |
| `.github/workflows/compat.yml` | `compat_test` のビルド | `docs/dev-infrastructure-spec.md` §13、`azookey-quality-harness` skill | `compat_test_unit_tests`（Windows） |
| `.github/workflows/release.yml` | MSI、settings app、SBOM、attestation。任意の Vulkan add-on MSI（`vulkan-addon`）を base と独立に作り、base 成功後に `attach-vulkan-addon` で添付する。`RELEASE_ENABLED` repository variable が `true` のときだけ動く | `docs/sideload-packaging-spec.md`、`azookey-packaging-release-workflow` skill | `python scripts/tests/test_release_sbom.py` |
| `.github/workflows/sbom.yml` | SBOM 補完スクリプトのテスト | `scripts/complete-release-sbom.py` の docstring | `python scripts/tests/test_release_sbom.py` |
| `.github/workflows/secret-scan.yml` | gitleaks による PR range / 作業ツリーの走査 | `docs/dev-infrastructure-spec.md` §4.3 | pre-commit の `gitleaks` hook |
| `CMakePresets.json` | preset の定義 | `docs/dev-infrastructure-spec.md` §2.1・§4.2、`README.md` | `cmake --list-presets` |
| `.pre-commit-config.yaml` | clang-format、gitleaks、actionlint、PowerShell 品質、settings schema、yamlfmt、taplo | `docs/dev-infrastructure-spec.md` §4.3 | `pre-commit run --all-files` |
| `justfile` | preset を束ねる開発者向けレシピ（`ci`、`doctor`、`register` など） | `README.md`、`docs/handoff/agent-tooling-setup.md` | `just --list` |
| `.github/dependabot.yml`、`.github/CODEOWNERS` | Actions の週次更新（group せず個別 PR）、レビュー担当 | `docs/dev-infrastructure-spec.md` §3.4（pin 済み依存の更新手順）。CODEOWNERS の正典はファイル自身 | なし |
| `scripts/cloud-setup.sh`、`.claude/settings.json` の `SessionStart` hook | Claude Code on the web の初期化 | `docs/handoff/agent-tooling-setup.md`「Claude Code on the web」 | `bash scripts/cloud-setup.sh` |

## windows.yml の job

`ci-gate` 列の `必須` は `ci-gate` の `needs` にある job、`advisory` はそれ以外である。
`scripts/check_skill_tables.py` が、本表と `windows.yml` の job と `ci-gate` の `needs` の
食い違いを検出する。job を足す・消す・`needs` を変える PR は同じ PR で本表を更新する。
`ci-gate` 自身は集約 job なので表に載せない。

| job | ci-gate | 内容 |
|---|---|---|
| `changes` | 必須 | 変更ファイルから、後続 job を走らせるかを分類する |
| `settings-schema` | 必須 | settings の JSON Schema と sample の検査 |
| `powershell-quality` | 必須 | PowerShell の lint とテスト |
| `quality` | 必須 | pre-commit（clang-format、gitleaks、actionlint など） |
| `dependency-review` | 必須 | 依存の変更の検査 |
| `windows-build` | 必須 | Windows Debug / Release の build と CTest |
| `windows-llama-build` | 必須 | llama.cpp の実モデル smoke |
| `windows-vulkan-compile` | 必須 | Vulkan の compile と link だけ |
| `windows-arm64-crossbuild` | 必須 | ARM64 の cross-build |
| `linux-build` | 必須 | Linux Debug の build と CTest |
| `linux-coverage` | 必須 | Linux coverage（名前は informational だが `needs` にある） |
| `linux-no-tests` | 必須 | Linux のテスト無し build |
| `windows-no-tests` | 必須 | Windows のテスト無し build |
| `cpp-format` | 必須 | 変更行の clang-format 18 gate |
| `cpp-tidy` | advisory | clang-tidy |
| `windows-coverage-shard` | advisory | Windows coverage の shard 実行 |
| `windows-coverage` | advisory | Windows coverage の集計 |
| `advisory-summary` | advisory | advisory job の結果の要約 |

## 変更時に同期するもの

- path filter: `windows.yml` の `changes` ジョブと、各 workflow の `on.*.paths`。
- required check: `docs/dev-infrastructure-spec.md` §4.9 の表と GitHub 側の ruleset（人間が変更）。
- 検査一覧: `docs/test-inventory.md`「CTest 以外の自動検査」。
- 手順: `README.md` と `docs/debugging.md`「CI」。

数値（実行時間、件数、キャッシュヒット率）は spec や README に書かない。
必要なら Linear の検証メモに残す（詳細は `azookey-doc-governance` スキル）。
