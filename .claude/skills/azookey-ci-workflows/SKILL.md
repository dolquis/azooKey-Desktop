---
name: azookey-ci-workflows
description: azooKey Desktop の .github/workflows（Build、Docs lint、Sanitizers、Benchmark history、Compatibility tests、Release、SBOM、Secret scan）、CMakePresets.json、.pre-commit-config.yaml、dependabot、CODEOWNERS、justfile、scripts/ の Python チェッカーと cloud-setup.sh、main の branch ruleset を変更、レビュー、デバッグするときに使う。「CI が落ちた」「ジョブを足す」「preset を増やす」「チェックを必須にする」のような依頼でも、workflow や preset に触れるなら必ずこのスキルを使う。
---

# azooKey CI とワークフロー

CI は Human Gate の代替ではなく、Windows 実機でしか確かめられないことを CI 成功で
完了扱いにしない。一方で CI が持つ検査（secret scan、schema、docs-lint、テスト一覧の突合）は
PR の唯一の機械的ゲートなので、弱めるときは理由を PR 本文に残す。

## 手順

1. `references/spec-routing.md` を読み、対象 workflow の正典節（`docs/dev-infrastructure-spec.md` §4）と
   `docs/test-inventory.md`「CTest 以外の自動検査」の該当行を確認する。
2. `.github/workflows/windows.yml` の `changes` ジョブが持つ path 分類を読み、新しいディレクトリや
   ファイル種別が既存の filter に入るかを確認する。入らないなら filter を同じ変更で更新する。
3. 変更する job の名前が branch ruleset の required check に含まれるかを
   `docs/dev-infrastructure-spec.md` §4.9 で確認する。名前を変えると必須チェックが外れる。
4. ローカルで実行できる検査を先に回す。`pre-commit run --all-files`、`python3 scripts/tests/` の
   各テスト、`python3 scripts/docs-lint.py --baseline .docs-lint-baseline.json`、
   `python3 scripts/check_test_inventory.py`、`python3 scripts/check_skill_references.py`。
5. Windows ジョブはローカルで再現できないことが多い。差分を読んで preset、target、
   環境変数、artifact 名の整合を確認し、必要なら `workflow_dispatch` で実行して結果を待つ。
6. 頻度、対象、preset、必須チェックが変わる場合は §4 の該当節と test-inventory の表を同じ変更で更新する。

## 必須ガードレール

- GitHub Actions は commit SHA で pin し、末尾コメントにバージョンを書く。ツールは版を固定する
  （例 `check-jsonschema==0.37.3`、PSScriptAnalyzer `1.24.0`、gitleaks `v8.30.1`）。
  dependabot の更新以外で pin を緩めない。
- `secret-scan`、`dependency-review`、`docs-lint`、`settings-schema` の検査を path filter や
  `continue-on-error` で迂回しない。
- `main` の ruleset は「PR を経由すること」を担保する設定であり、required approvals は 0 である。
  設定変更は人間が GitHub 上で行う Human Gate として扱う。
- workflow のログ、artifact、PR コメントに secret、証明書、ユーザーパス、モデルの絶対パスを出さない。
- 実モデルを使うジョブ（`windows.yml` の `windows-llama-build`）は pin したモデルと SHA256 検証を前提にする。
  pin なしの実モデル取得を required check に入れない。`benchmarks.yml` の `benchmark` ジョブは Linux でモデル無しの bench を回すもので、実モデル検証ではない。
- `scripts/docs-lint.py`、`scripts/check_agent_instruction_size.py`、
  `.claude/hooks/post-edit-docs-lint.py` は `dolquis/agent-ops` のベンダリングコピーで、
  この repo では編集しない。呼び出し方だけを workflow 側で変える。
- Sanitizer や benchmark の定期実行を、通常 PR の必須チェックへ昇格しない。§4.6 / §4.5 の分離を保つ。
- CMake preset を増やすときは `CMakePresets.json`、`justfile`、README の手順、§4.2 を同期する。

## 完了条件

- ローカルで実行した検査と、CI でしか実行できない検査を分けて報告する。
- required check の名前、path filter、pin の変更有無を明記する。
- 弱めた検査があれば理由と代替を PR 本文に残す。
