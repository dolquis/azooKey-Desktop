# AIエージェント向け規約

@AGENTS.md

このファイルは Claude Code 固有事項だけを持つ。`AGENTS.md` の再読と要約の再掲をしない。

## ターンの終わり方

ターンを終える前に最後の段落を読み返す。計画、質問、次の手順、「次に〜する」で終わっているなら、その作業を今ツールで実行する。可逆で依頼から導かれる操作は許可を求めず進め、破壊的な操作、依頼範囲の変更、`AGENTS.md` が確認を求める操作では止まる。一部が塞がったら残りを仕上げ、外したものと理由を書く。ユーザーが問題を述べ質問している段階では、成果物は評価であって修正ではない。人間 lead の判断を求める質問・選択肢・提案は、`AskUserQuestion` が使える環境ではそれで出し、本文だけで尋ねない。

長いツール連鎖では着手時に 1 行、作業中に短い進捗、最後に単独で読める要約を置く。小さな変更はファイル全体の書き換えではなく該当箇所の編集にする。テストは依頼が求めるか repo が同種の変更で持つ場合にだけ commit し、隣接テストと同規模に収める。

## アドバイザー（Fable）への相談

本節は Claude Code 専用（Advisor は Claude Code 固有機能のため）。判断を誤るとコストの大きい局面では、Advisor 機能でアドバイザー（Fable）に相談してから進める。助言は批判的に検討し、最終判断は自分で行う。メインモデルが Fable のときは、上位への相談ではなく同格モデルの独立チェックとして扱う。アドバイザーは `.claude/settings.json` の `advisorModel: "fable"` でリポジトリ設定済み。この設定が無い環境（フォーク先等）では本節は無視してよい。

相談する場面は、実装方針を確定する前の計画レビュー、TSF / COM 境界や独自 IPC プロトコルに踏み込む変更の着手前、`legacy/` と Windows 仕様の食い違いの判断、同じエラーや CTest 失敗が繰り返すとき、重要な変更を完了扱いにする前の独立チェックである。

typo・コメントのみ・軽微で可逆な変更など計画の余地が小さい作業では使わない（トークンを消費し利用枠にも計上されるため）。`gate:human-required`（実機検証・TIP 登録・署名値設定など）や Linear 管制塔運用の代替にはしない。

## サブエージェントの使い分け

本節は Claude Code 専用（Agent tool、Explore、Plan、カスタム agent、プラグイン agent は Claude Code 固有機能のため）。アドバイザーが判断の独立チェックであるのに対し、サブエージェントは作業の分担である。分担の規約は `AGENTS.md`「ローカルサブエージェント」から辿る。本節は、どの場面でどの agent を使うかだけを持つ。

| 場面 | 使うもの |
|---|---|
| 複数ファイルや命名規約をまたぐ調査で、結論だけが要る | Explore agent。結果は実ファイルで確認する |
| `AGENTS.md`「調査と実装」が求める、非自明な変更の編集前のスコープ整理 | Plan mode、または Plan agent |
| 高リスク差分で、spec・安全規則・依頼範囲との整合を設計意図から切り離して読む | `diff-auditor`（`.claude/agents/`、read-only）。適用基準は `docs/handoff/agent-orchestration.md` に従い、`pre-pr-self-review` の差分レビューと併用する |
| コード品質の観点別レビュー（規約準拠、silent failure、テスト網羅、型設計、コメント、簡素化） | `pr-review-toolkit` の各 agent。まとめて掛けるときは `/pr-review-toolkit:review-pr` |
| 互いに独立した調査・実装を同時に進める | Agent tool を同一メッセージで複数起動する |
| Windows の configure / build / CTest / bench を回し、失敗した target・CTest 名・warning・ログ位置だけが要る | `windows-build-runner`（`.claude/agents/`）。ソース編集と git 操作はしない |
| 実装 PR で spec・schema・テスト一覧のどこが失効したかを網羅列挙する | `spec-drift-checker`（`.claude/agents/`、read-only）。`azookey-doc-governance` の失効チェックと併用する |
| docs-lint、`scripts/` の check 系、clang-format 18 の diff など build を伴わないゲートを複数回し、合否と失敗箇所だけが要る | `quality-gate-runner`（`.claude/agents/`、read-only）。formatter による書き換えはしない |
| 分割レベル L2・L3 に当たる作業や、複数の保守領域をまたぐ調査を一括で進める | Workflow ツール。明示の依頼なしに起動せず、目的と agent 数の見込みを添えて提案し、承認後に起動する。PR 前の L2 レビューとゲートは、同じ手順で保存済みの `pre-pr-gate`（`.claude/workflows/`）を提案できる |

agent の不変条件（親による push と PR の直列化、read-only の担保、spawn の禁止、結論を入力として扱うこと、Human Gate と Codex Cloud 起動を代替しないこと）は `docs/handoff/agent-orchestration.md` が持つ。

### Agent Teams

Agent Teams は `.claude/settings.json` の `env` で有効にしてある（experimental。対話セッション専用）。使う場面は、互いに独立した実装を複数ファイルへ同時に進めるときに限る。teammate に適用される定義、read-only reviewer を teammate にしない理由、書込み境界は `docs/handoff/agent-orchestration.md` に従う。表示モード（`teammateMode`）は個人設定で選び、repo 設定には置かない。

## push・PR・マージ

作業 branch を `dolquis/azooKey-Desktop` へ push し、`main` 向けの Draft PR を作るのは、依頼された作業の一部として承認なしで行う通常の手順である。push は `cd`、パイプ、commit との連結を付けず、`git push -u origin <branch>` を単独のコマンドで実行し、`.claude/hooks/git-push-guard.py` と allow ルールに判定させる。`main` への push、force push、upstream（`azooKey/`）への push はしない。マージは、人間 lead が PR を名指しで許可したときに限り、CI green を確かめてノーマルマージで行う。
