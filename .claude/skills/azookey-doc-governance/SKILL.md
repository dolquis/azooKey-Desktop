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

## 検証

```
python3 scripts/docs-lint.py --baseline .docs-lint-baseline.json
```

DECISIVE は全ファイル 0 件で凍結してあるので、1 件でも出れば増加として落ちる。HEURISTIC は既存の件数を凍結してあり、止めるのは増加だけである。警告を消すためにベースラインを書き直さない。

## 参考資料

- `references/canonical-matrix.md` — azooKey の正典マトリクス。
- `references/doc-lifecycle.md` — 一回限り文書・恒常 runbook・spec・roadmap・参考資料の置き場所。
- `references/forbidden-words.md` — azooKey での書き換え例と免除の判断。語彙そのものは `--print-words` で得る。
