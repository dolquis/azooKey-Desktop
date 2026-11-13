# Skill manifest

`.claude/skills/` と `.agents/skills/` に置く Skill の区分、origin、取り込み時点の記録。
1 ファイルだけを持ち、`.agents/skills/` には複製しない（複製は無効化されないキャッシュになる）。
`scripts/check_skill_references.py` は本表の区分 `repo` の Skill だけを検査対象にする。

区分の意味:

- `shared` — `dolquis/agent-ops` を origin とする共有 Skill。本文と `references/` はこの repo で編集しない。
- `repo` — この repo が所有する azooKey 固有 Skill。本文と `references/` の更新責任は実装 PR にある。
- `third-party` — 外部 upstream からの再配布。同梱の `NOTICE.md` に従う。

「取り込み revision」は origin 側の commit を指す。origin 側の commit が記録に残っていない場合は
`未記録` とし、取り込んだこの repo 側の commit だけを添える。推測で hash を書かない。

| Skill | 区分 | origin | 取り込み revision | Codex 側 |
|---|---|---|---|---|
| `argument-gap-edit` | shared | `dolquis/agent-ops` | `0cbbe66`（repo 側 `6c286e9`） | あり |
| `doc-governance` | shared | `dolquis/agent-ops` | 未記録（repo 側 `d5e19e7`） | あり |
| `japanese-doc-workflow` | shared | `dolquis/agent-ops` | `0cbbe66`（repo 側 `6c286e9`） | あり |
| `japanese-tech-writing` | shared | `dolquis/agent-ops` | `0cbbe66`（repo 側 `6c286e9`） | あり |
| `create-draft-pr` | shared | `dolquis/agent-ops` | 未記録（repo 側 `aa8378a`、`f8e5731` で同期） | あり |
| `pre-pr-self-review` | shared | `dolquis/agent-ops` | 未記録（repo 側 `aa8378a`、`f8e5731` で同期） | あり |
| `doc-coauthoring` | third-party | `anthropics/skills` | 未記録（repo 側 `aa8378a`。`NOTICE.md` 参照） | なし（`.docs-lint.toml` の `mirror.claude_only`） |
| `azookey-doc-governance` | repo | この repo | `6c286e9` で agent-ops から初回配布 | あり（`agents/openai.yaml`） |
| `azookey-inference-model-workflow` | repo | この repo | `6c286e9` で agent-ops から初回配布 | あり（`agents/openai.yaml`） |
| `azookey-learning-data-safety` | repo | この repo | `6c286e9` で agent-ops から初回配布 | あり（`agents/openai.yaml`） |
| `azookey-packaging-release-workflow` | repo | この repo | `6c286e9` で agent-ops から初回配布 | あり（`agents/openai.yaml`） |
| `azookey-settings-schema-evolution` | repo | この repo | `6c286e9` で agent-ops から初回配布 | あり（`agents/openai.yaml`） |
| `tsf-ipc-protocol` | repo | この repo | `6c286e9` で agent-ops から初回配布 | あり（`agents/openai.yaml`） |
| `tsf-tip-development` | repo | この repo | `6c286e9` で agent-ops から初回配布 | あり（`agents/openai.yaml`） |
| `azookey-core-conversion` | repo | この repo | 本 repo で作成 | あり（`agents/openai.yaml`） |
| `azookey-observability-diagnostics` | repo | この repo | 本 repo で作成 | あり（`agents/openai.yaml`） |
| `azookey-settings-app-ui` | repo | この repo | 本 repo で作成 | あり（`agents/openai.yaml`） |
| `azookey-ci-workflows` | repo | この repo | 本 repo で作成 | あり（`agents/openai.yaml`） |
| `azookey-linear-issue-ops` | repo | この repo | 本 repo で作成 | あり（`agents/openai.yaml`） |
| `azookey-quality-harness` | repo | この repo | 本 repo で作成 | あり（`agents/openai.yaml`） |

## 担当パス

区分 `repo` の Skill が担当するソースのパス。`scripts/check_skill_impact.py` が PR の変更ファイルと照合し、
担当パスを変えて Skill を変えない PR に、本文の「Skill impact」節の記入を求める。同じ表から、Skill の最終更新以降に
担当パスへ入った commit 数（`--freshness`）も出す。

- 担当パスは back-quote で囲んだ glob を「、」で区切って書く。`*` は `/` を越えず、`**` は末尾の 1 segment だけに使え、
  残りの全階層に一致する。`—` は担当パスを持たない（docs や運用の Skill で、ほぼ全 PR が触れるため）。
- テスト（`tests/`）と、モジュールの `CMakeLists.txt`（例 `tsf-tip/CMakeLists.txt`）は含めない。テストや target 登録の追加は
  Skill の入口表を変えることが少なく、含めると記入が形骸化する。
- 区分 `repo` の Skill は全員が 1 行を持つ。各 glob は 1 件以上のパスに一致する。`scripts/check_skill_references.py` が検査する。
- 担当パスを足す・直す PR は、同じ PR で本表を更新する。

| Skill | 担当パス |
|---|---|
| `azookey-doc-governance` | — |
| `azookey-inference-model-workflow` | `inference-host/src/**`、`inference-host/include/**` |
| `azookey-learning-data-safety` | `learning/src/**`、`learning/include/**` |
| `azookey-packaging-release-workflow` | `pkg/**`、`.github/workflows/release.yml` |
| `azookey-settings-schema-evolution` | `settings/*` |
| `tsf-ipc-protocol` | `ipc/src/**`、`ipc/include/**` |
| `tsf-tip-development` | `tsf-tip/src/**`、`tsf-tip/include/**` |
| `azookey-core-conversion` | `core/src/**`、`core/include/**`、`dictbuild/*` |
| `azookey-observability-diagnostics` | `diagnostics/*`、`diagnostics/etw/**` |
| `azookey-settings-app-ui` | `settings-app/*`、`settings-app/Strings/**` |
| `azookey-ci-workflows` | `.github/workflows/**`、`CMakePresets.json`、`.pre-commit-config.yaml`、`justfile` |
| `azookey-linear-issue-ops` | — |
| `azookey-quality-harness` | `bench/*`、`compat-test/runner/**`、`compat-test/cases/**`、`compat-test/targets/**` |

## 区分の根拠と未確定事項

`azookey-*` と `tsf-*` のうち `6c286e9` で取り込まれたものは、commit message 上は agent-ops からの
配布だが、本文は azooKey 固有で `agents/openai.yaml` を持つ。本 manifest はこれらを `repo` として
扱い、この repo で更新する。agent-ops 側に同名の複製が残っているかは、この repo からは確認できない。
agent-ops 側に複製があるなら、origin で削除するか、本表の区分を `shared` へ改める。

## 更新の規則

- Skill を追加・削除・rename したら本表を同じ変更で更新する。`scripts/check_skill_references.py` は
  本表に無い `.claude/skills/` の Skill ディレクトリと、本表にあるが実在しない Skill を検出する。
- `shared` を再配布したら「取り込み revision」に origin の commit を書く。
- `repo` の `references/` にある routing 表（ファイル名、symbol、テスト target）は、対象を
  追加・rename・削除する実装 PR が同じ PR で更新する。CTest target 名の正典は
  `docs/test-inventory.md` で、routing 表は入口であって網羅ではない。
