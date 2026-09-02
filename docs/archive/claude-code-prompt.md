# Claude Code 起動プロンプト:`dolquis/azooKey-Desktop` セットアップ

> **アーカイブ済み**: 本書が起動した両 PR（`.claude/` / `.codex/` セットアップ）は
> マージ済みで、本書の役目は終えている。他リポジトリへ同種のセットアップを
> 横展開する際のテンプレートとして保全する（再利用時は本文中の `docs/handoff/`
> パスをそのまま新しいリポジトリの `docs/handoff/` に配置すればよい）。

> このファイルの中身を Claude Code にそのまま貼り付けて使ってください。
> 事前準備として、`docs/handoff/` 配下に 2 本の引き継ぎドキュメントを配置しておきます
> (「事前準備」セクション参照)。

---

## 事前準備(ユーザー側で実施)

Claude Code を起動する前に、リポジトリ `dolquis/azooKey-Desktop` のローカルクローンで
以下を実施してください:

1. `main` を最新化:`git switch main && git pull`
2. 引き継ぎドキュメントを `docs/handoff/` に配置:
   ```
   docs/handoff/claude-code-bootstrap.md   ← azookey-desktop-claude-skills-bootstrap.md をリネーム
   docs/handoff/codex-bootstrap.md         ← azookey-desktop-codex-bootstrap.md をリネーム
   ```
3. `docs/handoff/` がまだコミットされていない場合、作業前に
   `git add docs/handoff/ && git commit -m "docs: add handoff docs for agent bootstrap"` で
   先にコミット(あるいは作業ブランチで一緒にコミットしてもよい)。
4. Claude Code をリポジトリ root で起動し、下記プロンプトを貼り付け。

---

## 以下を Claude Code に貼り付け

```
あなたは dolquis/azooKey-Desktop (Windows 版 IME、C++/CMake/TSF) で作業する Claude Code です。

# ゴール

このリポジトリに、Claude Code 用と Codex CLI 用のエージェントセットアップを
**別ブランチ・別 Draft PR** として2本導入してください。

# 厳守事項

- PR は必ず dolquis/azooKey-Desktop 宛・Draft。フォーク元には絶対に出さない。
- main への直接 push 禁止。新規ブランチを切ること。
- gh pr create は --repo dolquis/azooKey-Desktop --base main --draft を必ず明示。
- legacy/ 配下(旧 macOS 実装)は絶対に触らない。参照のみ。
- 不明点や判断が必要な場面では、勝手に進めず必ず私に確認する。
- 2つの PR は完全に独立。フェーズ1完了→私の承認→フェーズ2着手の順で進める。

# 着手前の確認(ファーストレスポンス)

まず以下を確認して、結果を箇条書きで報告してから次に進んでください:

1. docs/handoff/claude-code-bootstrap.md と docs/handoff/codex-bootstrap.md が存在するか
2. 現在のブランチが main で、git status がクリーンか
3. 既に .claude/ .codex/ .agents/ .mcp.json のいずれかが存在しないか
4. 既存の CLAUDE.md と AGENTS.md の有無、ある場合はその冒頭 20 行
5. legacy/ 以外の主要ディレクトリ(tsf-tip/ inference-host/ core/ ipc/ scripts/)が
   READMEどおりに存在するか

確認結果を出したら、私の「進めて」を待ってください。

# フェーズ 1:Claude Code 用セットアップ

## 1-1. 引き継ぎドキュメントを通読

docs/handoff/claude-code-bootstrap.md を最初から最後まで読んで、要点を5行以内で
要約してください。読まずに作業を始めないこと。

## 1-2. ブランチを切る

```bash
git switch -c chore/claude-bootstrap
```

## 1-3. ファイル作成

docs/handoff/claude-code-bootstrap.md の §3〜§6 に従って以下を作成:

- `.mcp.json`(§3)
- `.claude/settings.json`(§4)
- `.claude/skills/tsf-tip-development/SKILL.md` + `references/`(§5.1)
- `.claude/skills/tsf-ipc-protocol/SKILL.md` + `references/`(§5.2)
- `CLAUDE.md` への追記(§6)— 既存があれば末尾に追加、無ければ新規作成

`references/itf-interfaces.md` は `grep -r "ITf" tsf-tip/` で実装中の
インターフェースを抽出して埋めること。`references/sample-projects.md` と
`references/frame-format.md` は §5/§6 のガイドに従って埋める。

**重要な検証ポイント**:
- .mcp.json の powershell / windows-mcp の起動コマンドは、
  各 MCP の最新ドキュメント(URL は §3 に記載)を WebFetch で確認して反映。
- .claude/settings.json の enabledPlugins スキーマは、
  https://code.claude.com/docs/en/plugin-marketplaces を WebFetch で確認。
- シークレットを直接書かない。必要なら ${ENV_VAR} 参照。

## 1-4. 動作確認

docs/handoff/claude-code-bootstrap.md §7 の項目をすべて実施し、結果を報告:
- `claude mcp list` で 3 MCP が見えるか
- `/plugin marketplace list` / `/plugin list` の出力
- CLAUDE.md の追記が既存内容と矛盾していないか目視確認

## 1-5. DoD チェック

§8 のチェックボックスを 1 つずつ確認し、すべて埋まったことを報告。

## 1-6. PR 作成

§7 のコマンドどおりに Draft PR を作成。PR 本文には以下を含めること:
- このセットアップでカバーされる範囲
- マーケットから入れた MCP / プラグインの一覧
- 自作スキル 2 個の概要
- legacy/ を触っていないことの明示

## 1-7. 報告

PR URL と、レビューしてほしい観点を3つ挙げて、私の確認を待つ。
私が「フェーズ2に進めて」と言うまでフェーズ2には着手しないこと。

---

# フェーズ 2:Codex CLI 用セットアップ

(私の承認後に着手)

## 2-1. main を最新化して新ブランチ

```bash
git switch main
git pull
git switch -c chore/codex-bootstrap
```

フェーズ1のブランチからは切らない。**main から切り直す**こと。

## 2-2. 引き継ぎドキュメントを通読

docs/handoff/codex-bootstrap.md を最初から最後まで読んで、要点を5行以内で要約。

## 2-3. ファイル作成

docs/handoff/codex-bootstrap.md の §4〜§6 に従って以下を作成:

- `.codex/config.toml`(§4)
- `AGENTS.md` の新規作成または既存への追記(§5)
- `.agents/skills/tsf-tip-development/SKILL.md` + `references/`(§6.1)
- `.agents/skills/tsf-ipc-protocol/SKILL.md` + `references/`(§6.2)

**注意点**:
- .codex/config.toml は TOML。JSON ではない。Codex の最新スキーマは
  https://developers.openai.com/codex/config-reference を WebFetch で確認。
- SKILL.md の本文は Claude Code 版と完全に同じでよい(クロス互換規格)。
- ただし配置先は .agents/skills/(.claude/skills/ ではない)。
- references/ の中身もフェーズ1で書いたものと同じ内容で構わない
  (コピーで可。symlink は今回使わない)。
- フェーズ1で作成した .claude/ .mcp.json CLAUDE.md には絶対に触らない。

## 2-4. 動作確認

docs/handoff/codex-bootstrap.md §7 の項目をすべて実施し、結果を報告:
- `codex mcp list` で 3 MCP が見えるか
  (powershell / windows-mcp は Windows 以外では起動失敗するが構成上は見える)
- Codex CLI 起動時に AGENTS.md が読み込まれるか
- /skills でスキル 2 個が見えるか

## 2-5. DoD チェック

§9 のチェックボックスを 1 つずつ確認し、すべて埋まったことを報告。
特に「Claude Code 用ファイル(.claude/ .mcp.json CLAUDE.md)が変更されていない」を
git diff で確認。

## 2-6. PR 作成

§7 のコマンドどおりに Draft PR を作成。PR 本文に以下を含める:
- このセットアップが Codex CLI 単独運用を対象としていること
- Claude Code 版 PR (#XXX) と独立した別系統であること
- 自作スキル 2 個は内容としては Claude Code 版と同等(将来 symlink 統合の余地あり)

## 2-7. 報告

PR URL と、フェーズ1 PR との関係(競合がないこと等)を報告。

---

# 一般ルール

- ツール呼び出しの前に意図を 1〜2 行で説明する。
- 大量のファイル作成や破壊的操作の前は必ず確認を取る。
- WebFetch で公式ドキュメントを参照したときは、参照先を報告に含める。
- エラーが起きたら自己判断で回避策を講じず、まず私に状況を共有する。
- 引き継ぎドキュメントと食い違う実装になりそうなときは、ドキュメント側を信頼する。
  どうしても食い違いが必要な場合は私に確認。

# 始めるとき

まず「着手前の確認」セクションの 5 項目を実施して結果を報告してください。
```

---

## ユーザーへの補足

### このプロンプトの設計意図

- **PR を 2 本に分ける**:Claude Code 用と Codex 用は独立して導入したいので、
  ブランチも PR も別。レビュー・rollback・将来の更新がしやすい。
- **フェーズ間に承認ゲート**:フェーズ 1 完了時にユーザーが PR を見て問題ないことを
  確認してから、フェーズ 2 を開始する。「全部一気にやってから持ってきて」では
  PR が肥大化・コンフリクトしやすい。
- **着手前の確認をファーストレスポンスに**:Claude Code が暴走しないように、
  最初の 1 ターンは現状確認に絞る。
- **引き継ぎドキュメントの存在を前提**:本プロンプトは短く保ち、詳細は
  docs/handoff/ 配下の 2 本に任せる。これで「プロンプトと引き継ぎドキュメントの
  食い違い」を起こしにくくする。

### 想定する所要時間

- 着手前の確認 → 数分
- フェーズ 1(Claude Code 用):30〜60 分(ドキュメント通読 + 5〜6 ファイル作成 + references 埋め)
- ユーザー確認 → 必要に応じて
- フェーズ 2(Codex 用):20〜40 分(基本はフェーズ 1 のコピー + TOML 化)

### PR 完了後の作業

両 PR が merge されたら、`docs/handoff/` 配下の 2 本は役目を終えます。
そのまま残しておいてもよいし、`docs/archive/` に移動するか、`git rm` するかは
お好みで。今後同じセットアップを他のリポジトリに横展開する可能性があるなら
残しておく価値があります。
