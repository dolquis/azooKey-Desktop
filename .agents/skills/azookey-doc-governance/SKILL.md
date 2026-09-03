---
name: azookey-doc-governance
description: azooKey Desktop の README.md、AGENTS.md、CLAUDE.md、docs/、plans/ を変更・レビューするとき、または新しい spec / runbook / roadmap 節を追加するときに使う。状態語（現状・実装済み・未実装・暫定・完了後など）の混入、コード実体との乖離（旧ファイル名・旧行番号・存在しない payload フィールド）、正典の重複記述、docs/README.md の索引漏れ、一回限り文書と恒常 runbook の混在を検出・防止する。「ドキュメントを直して」「spec を書いて」「roadmap に追記して」「READMEを更新して」のような依頼でも、実質的に docs/ や plans/ や README.md への変更を伴うなら必ずこのスキルを使う。
---

# azooKey ドキュメントガバナンス

このリポジトリの docs は「今どうなっているか」の実況ではなく「何が正しいか」の定義を保つ契約書として扱う。実況を書くと、実装が追いつくたびに嘘になる。`references/canonical-matrix.md` の正典マトリクスと本 Skill の状態禁止規約は、この劣化を防ぐために存在する。

## なぜ状態語を書いてはいけないか

「現状 X は未実装」は書いた瞬間は正しくても、X が実装された日に無言で嘘になる。読み手はその日付を知らないので、嘘だと気づけない。実際に本リポジトリでは「Zenzai 推論は未実装」「TIP 登録は PS1 が暫定」といった記述が、対応する機能の実装後も何週間も残り、runbook の判断を誤らせた。対策は簡単で、状態語を消して定義文に変えるだけでよい。

- 「現状 X は未実装」→ 削除するか「X は M-N の範囲」に変える。
- 「Y は暫定的に Z」→ Z を仕様として確定するか、暫定を Linear へ。
- 「A が B を完了後」→ 「A の完了は B の前提」（依存関係の定義。時系列の実況ではない）。
- DEV-xxx への参照は「追跡先」としてはよいが、「DEV-xxx は Done/Canceled/延期」のような状態記述にはしない。

状態そのもの（進捗・優先度・完了/未完了）は Linear が正典。repo docs にその状態を書いた瞬間、二重管理が始まり、どちらかが必ず腐る。

## 作業手順

1. **正典を確定する**: この情報の正典は roadmap か、spec か、README か、Linear か。`references/canonical-matrix.md` で判定する。正典でない場所に書くなら、リンクだけにする。
2. **状態語を検査する**: `references/forbidden-words.md` の語彙が混入していないか確認する。書く必要があるなら定義文に言い換える。
3. **コード実体を確認する**: ファイル名・関数名・payload フィールド・設定キー・テスト名・CTest 件数を grep で裏取りしてから書く。行番号と実測件数（「157件中156 passed」等）は書かない — コードは変わるが、docs は変わったことに気づけない。
4. **重複を避ける**: 同じ内容を 2 箇所目に書きたくなったら、1 箇所目へのリンクにする。正典側には「〜は X が正典」と書き、参照される側にも「本節は X から参照される」と書く（片方向の宣言は腐りやすい）。
5. **索引を更新する**: `docs/*.md` を新規追加したら `docs/README.md` に必ず載せる。既存 spec に新しい M（例: M62-C）を追加したら、索引の説明行も更新する。
6. **文書の寿命を判定する**: 一回限りの引き継ぎ・セットアップ手順は、DoD 達成後に `docs/archive/` へ移す。継続的に使う実機検証・診断手順は `docs/handoff/` に置き、対応する spec から正典として参照させ、`docs/README.md` の運用表に載せる。
7. **検証する**: `python3 scripts/docs-lint.py` を実行し、新しく増えた warning が正当か確認する（誤検知は `<!-- lint:allow <category> -->` で個別に免除できる）。

## 実装 PR を書くときの注意（コード側からの視点）

機能を実装したら、対応する spec の「現状は未実装」「暫定」「TODO」を消すところまでが実装 PR の一部である。spec が実装に追いつかないまま放置されるのは、たいてい「実装 PR は docs を更新しなくてもマージできる」からで、そこを止めるのがこのスキルの役目。`azookey-inference-model-workflow` 等の機能別スキルを使う場合も、対応する spec の失効チェックはこちらを併用する。

## 参考資料

- `references/canonical-matrix.md` — 正典マトリクス（何がどこの正典か）と、README / AGENTS.md / roadmap / spec / Linear の役割分担。
- `references/forbidden-words.md` — 状態語の語彙リストと、定義文への書き換え例。`scripts/docs-lint.py` の禁止語検査と同じ語彙。
- `references/doc-lifecycle.md` — 一回限り文書・恒常 runbook・spec・roadmap・参考資料の分類基準と置き場所。
