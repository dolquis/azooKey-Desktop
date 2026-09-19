# AIエージェント向け規約

@AGENTS.md

このファイルは Claude Code 固有事項だけを持つ。`AGENTS.md` の再読と要約の再掲をしない。

## ターンの終わり方

ターンを終える前に最後の段落を読み返す。計画、質問、次の手順、「次に〜する」で終わっているなら、その作業を今ツールで実行する。可逆で依頼から導かれる操作は許可を求めず進め、破壊的な操作、依頼範囲の変更、`AGENTS.md` が確認を求める操作では止まる。一部が塞がったら残りを仕上げ、外したものと理由を書く。ユーザーが問題を述べ質問している段階では、成果物は評価であって修正ではない。

長いツール連鎖では着手時に 1 行、作業中に短い進捗、最後に単独で読める要約を置く。小さな変更はファイル全体の書き換えではなく該当箇所の編集にする。テストは依頼が求めるか repo が同種の変更で持つ場合にだけ commit し、隣接テストと同規模に収める。

## アドバイザー（Fable）への相談

本節は Claude Code 専用（Advisor は Claude Code 固有機能のため）。判断を誤るとコストの大きい局面では、Advisor 機能でアドバイザー（Fable）に相談してから進める。助言は批判的に検討し、最終判断は自分で行う。メインモデルが Fable のときは、上位への相談ではなく同格モデルの独立チェックとして扱う。アドバイザーは `.claude/settings.json` の `advisorModel: "fable"` でリポジトリ設定済み。この設定が無い環境（フォーク先等）では本節は無視してよい。

相談する場面の例:

- 複数ステップの作業で、実装方針・設計を確定する前の計画レビュー。
- TSF / COM 境界や独自 IPC プロトコルなど、境界設計に踏み込む変更の着手前。
- `legacy/`（macOS 実装）と Windows 仕様が食い違い、どちらを正解とするか判断するとき。
- 同じエラー・テスト（CTest）失敗が繰り返し、原因の切り分けに行き詰まったとき。
- 重要な変更を完了扱いにする前の独立チェック。

typo・コメントのみ・軽微で可逆な変更など計画の余地が小さい作業では使わない（トークンを消費し利用枠にも計上されるため）。`gate:human-required`（実機検証・TIP 登録・署名値設定など）や Linear 管制塔運用の代替にはしない。

## サブエージェントの使い分け

本節は Claude Code 専用（Agent tool、Explore、Plan、カスタム agent、プラグイン agent は Claude Code 固有機能のため）。アドバイザーが判断の独立チェックであるのに対し、サブエージェントは作業の分担である。分担の共通規約は `docs/linear-conventions.md` §2.1、分割レベル（L0〜L4）、touched path からの route、background の可否、spawn 時に渡す snapshot 契約、統一返却形式、ロールバックは `docs/handoff/agent-orchestration.md` に従う。agent の所有（shared / repo）と権限は `.claude/agents/MANIFEST.md` が持ち、`scripts/check_agent_definitions.py` が Claude / Codex の対称性を検査する。同じ head への push と PR 作成は `create-draft-pr` のとおり親が直列に行い、返された結論は親が実ファイルと最新 diff で検証する。

| 場面 | 使うもの |
|---|---|
| 複数ファイルや命名規約をまたぐ調査で、結論だけが要る | Explore agent。結果は実ファイルで確認する |
| `AGENTS.md`「調査と実装」が求める、非自明な変更の編集前のスコープ整理 | Plan mode、または Plan agent |
| Draft PR 作成前・最終報告前に、spec・安全規則・依頼範囲との整合を設計意図から切り離して読む | `diff-auditor`（`.claude/agents/`、read-only）。`pre-pr-self-review` の差分レビューと併用する |
| コード品質の観点別レビュー（規約準拠、silent failure、テスト網羅、型設計、コメント、簡素化） | `pr-review-toolkit` の各 agent。まとめて掛けるときは `/pr-review-toolkit:review-pr` |
| 互いに独立した調査・実装を同時に進める | Agent tool を同一メッセージで複数起動する |
| Windows の configure / build / CTest / bench を回し、失敗した target・CTest 名・warning・ログ位置だけが要る | `windows-build-runner`（`.claude/agents/`）。ソース編集と git 操作はしない |
| 実装 PR で spec・schema・テスト一覧のどこが失効したかを網羅列挙する | `spec-drift-checker`（`.claude/agents/`、read-only）。`azookey-doc-governance` の失効チェックと併用する |

各 agent の役割分担、read-only の担保（`disallowedTools` と frontmatter `hooks.PreToolUse`）、reviewer は spawn しないという不変条件は `docs/handoff/agent-orchestration.md` が持つ。repo 固有の agent は `.claude/agents/` と `.codex/agents/` で本文を byte 一致させる。サブエージェントの結論は完了判定ではなく入力であり、Human Gate や Codex Cloud 起動の代替にもしない。

### Agent Teams

Agent Teams は `.claude/settings.json` の `env` で有効化してある（experimental。対話セッション専用で、`-p` 実行では通常のサブエージェントとして動く）。サブエージェントが親へ結果を返して終わるのに対し、teammate は自分のコンテキストを持ち、共有 task list と `SendMessage` で lead や他の teammate と協調する。使う場面は、互いに独立した実装を複数ファイルへ同時に進めるとき、または `diff-auditor` と `pr-review-toolkit` のレビューを実装と並行させるときに限る。調査だけなら Explore agent、build / test だけなら `windows-build-runner` で足りる。

- teammate の役割は `.claude/agents/` の定義を spawn 時に指定して使う（`tools` と `model` と本文が適用される。`skills` は settings から読み、frontmatter の `hooks` と `disallowedTools` は適用されない。read-only reviewer は名前を付けない通常 subagent で起動する）。
- 書込み境界は spawn prompt で明示する。同じファイルと共有 build directory を複数の teammate に割り当てない。push と PR 作成は lead が直列に行う。
- in-process の teammate は background subagent を起動できない。`/resume` で teammate は復元されない。nested team は作れない。
- 表示モード（`teammateMode`）は個人設定で選ぶ。repo 設定には置かない。
