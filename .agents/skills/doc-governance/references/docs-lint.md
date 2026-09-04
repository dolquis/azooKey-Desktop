# docs-lint の運用

`scripts/docs-lint.py` は読み取り専用の静的検査で、`dolquis/agent-ops` の同名ファイルを各 repo へベンダリングしたコピーである。**repo 側で本体を編集しない。** 変更は origin で行い、配布し直す。乖離は `vendor-docs-governance.sh --check <repo>` が検出する。

## 実行

```bash
python3 scripts/docs-lint.py                      # 全カテゴリ
python3 scripts/docs-lint.py --category link --category section
python3 scripts/docs-lint.py --verbose            # heuristic も行単位で出す
python3 scripts/docs-lint.py --print-words        # 状態語の語彙（語彙の正典はスクリプト）
python3 scripts/docs-lint.py --json               # 機械可読
```

origin から他 repo を検査することもできる。

```bash
python3 /path/to/agent-ops/scripts/docs-lint.py --root /path/to/repo
```

## 2 つのティア

**DECISIVE** は実在しない参照と、状態を主張する見出しである。ヒットは本物なので処置する。

| カテゴリ | 検出するもの |
|---|---|
| `link` | 相対リンク・画像の参照先が存在しない |
| `section` | `<file>.md §N.M` が存在しない節・ファイルを指す |
| `index` | 索引に載っていない文書がある |
| `mirror` | `.claude/skills/` と `.agents/skills/` の本文が食い違う |
| `heading-state` | 見出しが進捗・完了・日付つきスナップショットを主張する |
| `line-ref` | 行番号付きコード参照、実測件数の記述 |
| `config` | 設定ファイルが存在しないパス・スキルを指す |
| `dead-allow` | 何も抑止していない免除コメントが残っている |

**HEURISTIC** は `status`（文中の状態語）だけである。既定ではファイルごとの件数しか出さない。件数をゼロにするのは目的ではなく、件数の多いファイルを見つけるための指標として使う。多いファイルは、たいてい生きた定義ではなく凍結すべき記録である。

## 免除

```md
<!-- lint:allow status -->              その行（または次の行）のその検査だけ免除
<!-- lint:allow-file heading-state,status -->   文書全体を免除（先頭 20 行以内に置く）
```

`lint:allow-file` はスナップショット文書に適している。免除であると同時に「この文書は特定時点の記録である」という読み手への宣言になるためである。

決定ログ・アーカイブ・`linear-conventions.md` は既定で prose 検査の対象外なので、免除を書く必要はない。

## ベースライン（増加だけを止める）

既存 repo には最初から相当量の findings がある。ゼロを要求すると検査ごと切られるので、今日の件数を凍結して増加だけを止める。

```bash
python3 scripts/docs-lint.py --write-baseline .docs-lint-baseline.json
python3 scripts/docs-lint.py --baseline .docs-lint-baseline.json   # 増えたら exit 1
```

ベースラインはカテゴリ × ファイルの件数を持つ。機械が書き出すので、手書きキャッシュにはならない。掃除が進んだら書き直す。

## CI への載せ方

既存の docs 系ジョブ（textlint など）にステップを足す。単独の workflow を新設すると、private repo ではランナー起動ごとに分単位で課金される。

導入直後は warning-only にする。

```yaml
      - name: docs-lint
        run: python3 scripts/docs-lint.py
```

掃除が済んだら段階的にゲート化する。`--strict` は DECISIVE だけで exit 1 する。`--strict all` は HEURISTIC も含めるが、過検出前提のティアをゲートにすると検査が読まれなくなるので推奨しない。

```yaml
      - name: docs-lint
        run: python3 scripts/docs-lint.py --baseline .docs-lint-baseline.json
```

## 設定ファイル

repo 固有の差分だけを `.docs-lint.toml`（Python 3.11 未満なら `.docs-lint.json`）に置く。設定なしでも既定値で動く。

```toml
[scan]
# prose 検査（status / heading-state / line-ref）の対象。
# 既定: README.md, README.ja.md, AGENTS.md, CLAUDE.md, ROADMAP.md, docs, plans
prose_roots = ["README.md", "AGENTS.md", "docs", "plans"]
# .md 以外で統治対象に含めるファイル
extra_text_files = ["THIRD_PARTY_LICENSES"]

[status]
# 既定の除外（archive / decisions / linear-conventions / CHANGELOG）に追加する glob
exempt = ["docs/PRIVACY_*.md"]
# 語彙は狭められるが増やせない。追加は origin 側で全 repo 分まとめて行う
remove_words = []

[line_ref]
extensions = ["swift", "ts", "tsx"]

[index]
file = "docs/README.md"
covers = ["docs", "docs/handoff"]

[mirror]
# .agents/ 側に置かない Claude 専用スキル
claude_only = ["doc-coauthoring"]
# 片方のツリーだけで運用する repo は true にする（既定 false）
# allow_single_tree = false

[heading_state]
# 日付つきスナップショット見出しを何日まで見逃すか（既定 0 = 常に報告）
grace_days = 0
```

**語彙は repo 側で増やせない。** インシデントごとに各 repo が語を足す入口を作ると、9 つの方言ができて、規約自身が無効化機構のない手書きキャッシュになる。追加は origin で一度だけ行う。

## 新規 repo への導入手順

1. `vendor-docs-governance.sh <repo>` で検査器（`scripts/docs-lint.py`）、AGENTS.md 予算検査器（`scripts/check_agent_instruction_size.py` とテスト）、設定ファイルの雛形を配る。
2. `vendor-shared-skills.sh --skill doc-governance <repo>` でスキル本体を配る。検査器とスキルは別のスクリプトが運ぶ。
3. `python3 scripts/docs-lint.py` を実行し、ベースラインを見る。
4. `config` と `mirror` のヒットを解消する（`claude_only` の登録など、設定で片づくもの）。
5. 残る DECISIVE を直す。判断を要するもの（見出しの書き換え、archive 移動、索引新設）は別 PR に分ける。
6. `--write-baseline` で残りを凍結し、CI に warning-only で載せる。
7. AGENTS.md に規則段落を足す。既存の記述が逆を指示している場合（「roadmap のステータスを更新せよ」など）は同じ PR で直す。規則文と検査が食い違うと、検査のほうが無視される。
