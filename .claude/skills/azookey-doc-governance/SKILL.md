---
name: azookey-doc-governance
description: azooKey Desktop の README.md、AGENTS.md、CLAUDE.md、docs/、plans/ を変更・レビューするとき、または新しい spec / runbook / roadmap 節を追加するときに使う。状態語（現状・実装済み・未実装・暫定・完了後など）の混入、コード実体との乖離（旧ファイル名・旧行番号・存在しない payload フィールド）、正典の重複記述、docs/README.md の索引漏れ、一回限り文書と恒常 runbook の混在を検出・防止する。「ドキュメントを直して」「spec を書いて」「roadmap に追記して」「READMEを更新して」のような依頼でも、実質的に docs/ や plans/ や README.md への変更を伴うなら必ずこのスキルを使う。
---

# azooKey ドキュメントガバナンス

一般規則は共有 Skill `doc-governance` が持つ。なぜ repo docs に状態を書かないか、見出しと本文で規則が違う理由、DECISIVE と HEURISTIC の読み分け、`.docs-lint.toml` とベースラインの運用、状態語から定義文への書き換えの型は、そちらの SKILL.md と `references/` にある。共有 Skill は `dolquis/agent-ops` が origin であり、この repo では編集しない。

本 Skill は azooKey Desktop 固有の判断だけを持つ。まず `doc-governance` を読み、その上で以下を適用する。

## 正典の判定

`references/canonical-matrix.md` に、README / AGENTS.md / roadmap / spec / Linear / `legacy/` の役割分担がある。正典でない場所に書くなら、リンクと 1〜2 行の案内に留める。

`legacy/` は macOS / Swift の参照資産である。Windows 版の仕様判断が `legacy/` の実装と食い違うときは `docs/*-spec.md` を優先する（`AGENTS.md`「対象と正典」）。

## 置き場所と寿命の判定

`references/doc-lifecycle.md` に分類基準がある。一回限りの引き継ぎ・セットアップ手順は DoD 達成後に `docs/archive/` へ移す。継続的に使う実機検証・診断手順は `docs/handoff/` に置き、対応する spec から正典として参照させ、`docs/README.md` の運用表に載せる。

`docs/*.md` を新規追加または rename したら `docs/README.md` の索引に必ず載せる。既存 spec に新しい M（例: M62-C）を足したら、索引の説明行も更新する。

## 書く前の裏取り（azooKey 固有）

- ファイル名・関数名・payload フィールド・設定キー・テスト名は `rg` で現物を確認してから書く。
- 行番号（`TextService.cpp:1001`）と CTest の実測件数は書かない。コードは変わるが、docs は変わったことに気づけない。
- IPC payload スキーマの正典は `ipc/include/azookey/ipc/Payloads.h` と `ipc/src/Payloads.cpp` である。spec は定義を持ち、どこまで配線したかという状態は持たない。
- azooKey での書き換え例と免除の判断は `references/forbidden-words.md` にある。

## 実装 PR を書くときの注意

機能を実装したら、対応する spec の「未実装」「暫定」「TODO」を消すところまでが実装 PR の一部である。spec が実装に追いつかないまま放置されるのは、たいてい「実装 PR は docs を更新しなくてもマージできる」からで、そこを止めるのがこの Skill の役目。`azookey-inference-model-workflow` 等の機能別 Skill を使うときも、対応する spec の失効チェックはこちらを併用する。

### 契約ファイルと docs impact ゲート

IPC の wire schema、設定 schema、CMake preset、workflow のように、変えると spec・roadmap・README の定義が変わりうるファイルを契約ファイルと呼ぶ。一覧の正典は `scripts/check_docs_impact.py` の定数 `CONTRACT_PATHS` であり、`python3 scripts/check_docs_impact.py --print-contract-paths` で得る。本 Skill や他の文書に一覧を写さない。

同スクリプトは、契約ファイルを変更して `docs/`・`plans/`・ルートの `README.md` のどれも変更しない PR に対し、PR 本文の「Documentation impact」節で「No documentation impact」へのチェックと、`Reason:` の後の空でない理由を求め、なければ `docs.yml` の `docs-impact` ジョブが失敗する（merge を止める required check かどうかは `docs/build-ci-spec.md` §4.9 の ruleset が決める）。コメントだけ、「なし」「N/A」だけの理由は空とみなす。照合する見出しとチェックボックスの文言は `.github/PULL_REQUEST_TEMPLATE.md` と揃えてあるので、片方だけを変えない。

契約ファイルに触れる実装 PR では、`spec-drift-checker` を必ず使い、どの spec・schema・テスト一覧が失効したかを列挙させてから、docs を更新するか影響なしの理由を書くかを決める。ゲートが求めるのは理由の記入だけで、理由の正しさは確かめないため、この確認を省かない。

## 検証

```
python3 scripts/docs-lint.py --baseline .docs-lint-baseline.json
```

DECISIVE は全ファイル 0 件で凍結してあるので、1 件でも出れば増加として落ちる。HEURISTIC は既存の件数を凍結してあり、止めるのは増加だけである。警告を消すためにベースラインを書き直さない。

docs-lint が見ない文書とコードの対応は、次の 5 本が CI（`docs-lint` ジョブ）で検査する。同ジョブは `check_agent_instruction_size.py`、`check_test_inventory.py`、`check_skill_references.py`、`check_agent_definitions.py` も実行する。

```
python3 scripts/check_doc_refs.py
python3 scripts/check_doc_contracts.py
python3 scripts/check_doc_format.py
python3 scripts/check_docs_budget.py
python3 scripts/check_doc_meta.py
```

`check_doc_refs.py` は、文書が名指しするパス・`file::symbol`・spec の § 参照（`app-profile §3.1` のような短縮名と、rich-features の `X-N-M` を含む）の実在を確かめる。spec を分割して節を別文書へ移すときは、旧 spec に `## 転送表` の見出しと `| 旧節 | 移動先 |` の表を残す。外部の § 参照は張り替えずに表で解決し、旧 spec と、番号を保つ行の移動先は一つの § 名前空間（転送グループ）になって、その中の裸の `§N` も検査される（書式と規則は `scripts/check_doc_refs.py` の冒頭の規則 6〜8）。予定ファイルなど、不在でも正しい参照は `scripts/doc-refs-allowlist.toml` に種別と理由（予定なら M 番号と正典 spec）を書いて登録する。登録した参照が実在したら、登録を消す。`check_doc_contracts.py` は、IPC の型の表、spec の設定キー表、文書中の preset 名を、`MessageType`・Dispatcher・schema・`CMakePresets.json` と突き合わせる。`check_doc_format.py` は、roadmap の全 M が依存図に現れること、spec の受け入れ条件の節に定義を複製しないこと、冒頭に作成日・更新日を置かないこと、M を 3 つ以上扱う spec が「節とマイルストーンの対応」を持つことを確かめる。`check_docs_budget.py` は、spec（1500 行）と節（400 行）の行数を警告のみで知らせ、`.docs-lint-baseline.json` を `scripts/docs-baseline-cap.json` の上限と一致させる（件数が減ったら上限も同じ PR で下げる）。`check_doc_meta.py` は、文書冒頭の front matter（`kind` ほか。書式は `references/doc-lifecycle.md`「冒頭のメタデータ」）を `docs/README.md` の表、決定記録の「結論の正典」と逆リンク、完了基準と突き合わせる。監査の手順は `docs/handoff/docs-audit-runbook.md`。

## 参考資料

- `references/canonical-matrix.md` — azooKey の正典マトリクス。
- `references/doc-lifecycle.md` — 一回限り文書・恒常 runbook・spec・roadmap・参考資料の置き場所。
- `references/forbidden-words.md` — azooKey での書き換え例と免除の判断。語彙そのものは `--print-words` で得る。
