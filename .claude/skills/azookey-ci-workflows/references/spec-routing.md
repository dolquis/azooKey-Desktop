# CI とワークフロー変更のルーティング

## workflow ごとの正典と検証

| workflow / 設定 | 役割 | 正典 | ローカルで回せる検査 |
|---|---|---|---|
| `.github/workflows/windows.yml` | 変更分類、Windows Debug / Release build と CTest、settings schema、PowerShell 品質、pre-commit、dependency review、llama.cpp 実モデル smoke、Vulkan compile | `docs/dev-infrastructure-spec.md` §4.1〜§4.4、§4.7、§4.8 | `pre-commit run --all-files`、`check-jsonschema`（`azookey-settings-schema-evolution` 参照）、`scripts/test-powershell-quality.ps1`（Windows） |
| `.github/workflows/docs.yml` | docs-lint、AGENTS.md 予算、テスト一覧、Skill 参照の突合 | `docs/dev-infrastructure-spec.md` §4.3、`docs/test-inventory.md` | `python3 scripts/docs-lint.py --baseline .docs-lint-baseline.json`、`python3 scripts/check_agent_instruction_size.py`、`python3 scripts/check_test_inventory.py`、`python3 scripts/check_skill_references.py`、`scripts/tests/` の各 `test_*.py` |
| `.github/workflows/sanitizers.yml` | 週次の ASan / UBSan（Linux）と MSVC ASan | `docs/dev-infrastructure-spec.md` §4.6 | `cmake --preset linux-asan-ubsan` で Linux 側を再現 |
| `.github/workflows/benchmarks.yml` | Release benchmark の履歴と回帰警告 | `docs/dev-infrastructure-spec.md` §4.5、`azookey-quality-harness` skill | bench の単体テスト（`benchmark_result_tests`） |
| `.github/workflows/compat.yml` | `compat_test` のビルド | `docs/dev-infrastructure-spec.md` §13、`azookey-quality-harness` skill | `compat_test_unit_tests`（Windows） |
| `.github/workflows/release.yml` | MSI、settings app、SBOM、attestation | `docs/sideload-packaging-spec.md`、`azookey-packaging-release-workflow` skill | `python scripts/tests/test_release_sbom.py` |
| `.github/workflows/sbom.yml` | SBOM 補完スクリプトのテスト | `scripts/complete-release-sbom.py` の docstring | `python scripts/tests/test_release_sbom.py` |
| `.github/workflows/secret-scan.yml` | gitleaks による PR range / 作業ツリーの走査 | `docs/dev-infrastructure-spec.md` §4.3 | pre-commit の `gitleaks` hook |
| `CMakePresets.json` | preset の定義 | `docs/dev-infrastructure-spec.md` §4.2、`README.md` | `cmake --list-presets` |
| `.pre-commit-config.yaml` | clang-format、gitleaks、actionlint、PowerShell 品質、settings schema、yamlfmt、taplo | `docs/dev-infrastructure-spec.md` §4.3 | `pre-commit run --all-files` |
| `justfile` | preset を束ねる開発者向けレシピ（`ci`、`doctor`、`register` など） | `README.md`、`docs/handoff/agent-tooling-setup.md` | `just --list` |
| `.github/dependabot.yml`、`.github/CODEOWNERS` | Actions の週次更新、レビュー担当 | `docs/dev-infrastructure-spec.md` §3 | なし |
| `scripts/cloud-setup.sh`、`.claude/settings.json` の `SessionStart` hook | Claude Code on the web の初期化 | `docs/handoff/claude-code-web-setup.md` | `bash scripts/cloud-setup.sh` |

## 変更時に同期するもの

- path filter: `windows.yml` の `changes` ジョブと、各 workflow の `on.*.paths`。
- required check: `docs/dev-infrastructure-spec.md` §4.9 の表と GitHub 側の ruleset（人間が変更）。
- 検査一覧: `docs/test-inventory.md`「CTest 以外の自動検査」。
- 手順: `README.md` と `docs/debugging.md`「CI」。

数値（実行時間、件数、キャッシュヒット率）は spec や README に書かない。
必要なら Linear の検証メモに残す（詳細は `azookey-doc-governance` スキル）。
