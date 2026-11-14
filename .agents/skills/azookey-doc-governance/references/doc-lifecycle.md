# 文書の分類と置き場所

新しい文書を作る前に、種別を決めてから置き場所を選ぶ。種別を誤ると `docs/README.md` の索引が実態と合わなくなる。

| 種別 | 特徴 | 置き場所 | 索引 |
|---|---|---|---|
| 機能仕様 | IPC payload・schema・永続化・責務境界などユーザー可視挙動の定義。恒常的に更新される | `docs/*-spec.md` | `docs/README.md`「機能仕様」表に必須 |
| 開発計画 | マイルストーン定義・依存関係・受け入れ条件（定義） | `plans/windows-port-roadmap.md`（1 本に一本化。新規ファイルを作らない） | 不要（roadmap 自体が唯一の計画文書） |
| 恒常 runbook | 実機検証・診断・VM 構築など、繰り返し使う手順。対応する spec から正典として参照される | `docs/handoff/` | `docs/README.md`「運用」表に必須 |
| 一回限りの引き継ぎ | 特定のセットアップ・移行作業の一度きりの指示書。DoD（完了基準）を持つ | 作業中は `docs/handoff/`、DoD 達成後は `docs/archive/` へ移動 | archive 後は索引不要（テンプレートとしての位置づけのみ AGENTS.md 等から参照可） |
| 参考資料・調査記録 | 特定時点のスナップショット。更新しない前提で書かれる（例: 初回調査、他実装との比較） | `docs/`（`docs/README.md` に「参考資料。正典ではない」と明記） | 索引に載せるが「正典ではない」を明記 |
| 決定記録・測定記録 | 日付つきの決定の経緯・根拠・不採用理由と、固定条件での実測値。凍結し、追記のみで書き換えない | `docs/decisions/YYYY-MM-DD-<slug>.md`（命名と書式は `docs/decisions/README.md`） | `docs/README.md`「決定記録と測定記録」表に必須。状態語を含んでよい（docs-lint の prose 検査の対象外） |
| 運用規約 | Linear 運用・ラベル規約など、リポジトリ横断の作業ルール | `docs/linear-conventions.md`（共有コア部分は origin 側で編集）、`AGENTS.md`（要約） | `docs/README.md`「運用」表 |

## 冒頭のメタデータ

`docs/**/*.md` は、1 行目からの front matter に種別を宣言する。`scripts/check_doc_meta.py` が `docs/README.md` の表と突き合わせる（キーの定義と規則の正典は同スクリプトの docstring）。

```markdown
---
kind: runbook
canonical_for: ["索引の役割の列にある句"]
---
```

- `kind` は spec / design / runbook / one-off / reference / policy のどれかにする。decision と archive は置き場所で決まるので書かない。
- spec は `milestones`（`[M13, M14]` のように列挙する）、one-off は `dod`（完了基準の節。例 `"§6"`）を持つ。spec・design・policy・runbook は `canonical_for` を持ち、その句は索引の行にそのまま含める。
- 主題の一部の正典を別文書へ委ねるときは、任意の `defers_to` に委ね先（repo のパス。節を指すなら後ろに ` §N`）を並べる。委ね先は実在し、この文書のファイル名を本文で挙げて逆リンクする。
- 値は 1 行の文字列か `[a, b]` のリストに限る。状態語は書かない（docs-lint が front matter も検査する）。
- 付けないもの: `docs/decisions/`（固定ヘッダを読む）、`docs/archive/`、`docs/linear-conventions.md`（共有コアのミラー）、README・AGENTS・CLAUDE。全 spec が front matter を持つ（`kind: spec`、`milestones`、`canonical_for`）。どの M にも属さない spec は `milestones: []` と書き、索引の行にも M の ID を置かない。

## 一回限り文書を archive するタイミング

1. 文書自身が DoD（完了基準）チェックリストを持っている場合、そのチェックリストを実体（`.mcp.json` の存在、スキルファイルの実在など）と照合する。
2. 全項目が達成済みなら、`git mv docs/handoff/<file> docs/archive/<file>` で移動する。
3. 移動先のファイル冒頭に「アーカイブ済み」の一言と、再利用する場合の注意を追記する（内部の相対パス参照は、再利用時にそのまま使えるテンプレートとして機能する場合は書き換えない）。
4. その文書を指す索引・説明文（`docs/README.md`、`AGENTS.md` 等）を更新する。

## 恒常 runbook と一回限り文書が同じファイルに同居している場合

`human-gate-batch-runbook.md` のように、恒常的なレーン設計・チェックリストの型と、特定セッションの実走記録（日付・PR番号・DEV 状態）が同居してしまうことがある。実走記録は Linear のコメントへ移し、ファイル自体は手順と記録様式だけを残す。両者を無理に 1 パスで分離しきれない場合は Linear へ課題化し、分離作業として追跡する。
