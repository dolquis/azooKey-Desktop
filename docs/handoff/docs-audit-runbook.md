---
kind: runbook
canonical_for: ["repo docs を read-only で読み直し、機械検査が拾えない陳腐化・矛盾・重複を 6 領域に分けて所見として起票する手順"]
---

# docs 監査 runbook

本書は、repo docs（`README.md`、`AGENTS.md`、`CLAUDE.md`、`docs/`、`plans/`）を read-only で読み直し、機械検査が拾えない陳腐化・矛盾・重複を所見として Linear に起票する手順である。

機械検査が拾うものは、検査に任せる。本書が扱うのは、検査の網をすり抜ける内容の誤りである。

## 1. 機械検査との分担

| 検査 | 拾うもの |
|---|---|
| `scripts/docs-lint.py`（origin 管理） | 状態語、日付、DEV 番号、正典の重複、ミラーのずれ |
| `scripts/check_doc_refs.py` | 文書が名指しするパス、`file::symbol`、spec の § 参照の実在 |
| `scripts/check_doc_contracts.py` | IPC の型の表、spec の設定キー表、preset 名と、コードの突合 |
| `scripts/check_doc_format.py` | roadmap の全 M が依存図に現れること、spec の受け入れ条件の複製、冒頭の日付、節と M の対応表 |
| `scripts/check_docs_budget.py` | spec と節の行数の警告、docs-lint ベースラインの上限 |
| `scripts/check_doc_meta.py` | 文書冒頭の front matter と `docs/README.md` の表の対応、決定記録の結論の正典と逆リンク、one-off の完了基準 |
| `scripts/check_test_inventory.py` | テスト一覧と CMake の登録 |
| `scripts/check_docs_impact.py` | 契約ファイルを変えて docs を変えない PR |

監査は、これらが通った状態から始める。通らない状態で始めると、所見が機械検査の指摘と重なる。

## 2. 実施の方針

- 監査は手動で起動する。定期実行にはしていない。
- 起動するのは、大きな機能の追加後、spec の分割や統合の後、リリースの前、機械検査の指摘が続けて増えたときである。
- 最初の 1〜2 回は人間 lead が結果を見る。所見の費用と有用性を測ったうえで、月次などの定期実行にするかを人間 lead が決める。決めるまでは、監査を自動で繰り返さない。
- 監査は read-only である。監査の担当は文書も Linear の状態も書き換えず、所見の起票だけを行う。直すのは別の作業とする。

## 3. 領域と観点

次の 6 領域を、領域ごとに独立した読み手（subagent など）へ割り当てる。複数の領域を 1 人に持たせない。前の領域の結論が次の領域の読みを偏らせるためである。

| 領域 | 読む範囲 | 見る点 |
|---|---|---|
| roadmap | `plans/windows-port-roadmap.md` | 各 M の前提・変更対象・参照仕様が、実在するファイル・spec の節と合うか。依存図と前提の記述が食い違っていないか。廃止した M や統合した M が、他の節から生きているように参照されていないか |
| 開発基盤 | `docs/dev-infrastructure-spec.md`、`docs/build-ci-spec.md`、`docs/vm-verification-package-spec.md`、`docs/diagnostics-compat-spec.md`、`docs/debugging.md`、`docs/test-inventory.md`、`README.md`、`docs/handoff/` の検証系 | ビルド手順、preset、CI ジョブ、診断の手順が実体と合うか。手順が参照するスクリプトの引数と出力が合うか |
| 配布・推論 | `docs/sideload-packaging-spec.md`、`docs/zenzai-inference-spec.md`、`docs/copilot-pc-backend-spec.md`、`docs/model-management-spec.md`、`docs/ai-backend-spec.md` | 配布チャネルと署名要否、モデルのロード・fallback、backend の選定が、コードと他の spec の記述と合うか |
| TSF・parity | `docs/tsf-deep-integration-spec.md`、`docs/legacy-parity-spec.md`、`docs/native-ui-spec.md`、`docs/windows-tsf-host-architecture.md` | TIP のクラス名・メソッド名・メッセージ名が実在するか。macOS 版の挙動の写しが `legacy/` と合うか。TIP と Host の責務境界が複数の文書で一致するか |
| 変換・学習 | 変換・辞書・学習・予測・リライターの各 spec（`docs/README.md` の機能仕様の表を参照） | 式・定数・スキーマ・設定キーの既定値が、コードと他の spec の写しと合うか。同じ式を複数の spec が詳述していないか |
| エージェント運用 | `AGENTS.md`、`CLAUDE.md`、各ディレクトリの `AGENTS.md`、`.claude/skills/`、`.agents/skills/`、`.claude/agents/`、`docs/linear-conventions.md`、`docs/handoff/` の運用系 | 規約の重複と矛盾。Skill の routing 表と実体の差。Claude 側と Codex 側の本文のずれ |

どの領域でも共通して見る点は次のとおりである。

- 文書の記述を実ファイルで照合する。記述どおりのシンボル・設定キー・IPC の型が実在し、意味が合うかを確かめる。存在の確認だけは機械検査が済ませているので、意味と数値の一致を見る。
- 同じ内容を複数の文書が詳述していないか。正典は `azookey-doc-governance` の `references/canonical-matrix.md` に従う。
- コードに実体があるものを「予定」と書いていないか。実体のないものを既存の契約のように書いていないか。
- 受け入れ条件が roadmap 以外に書かれていないか。

## 4. 返却形式

読み手は、所見を 1 件ずつ次の形で返す。所見の無い領域は「所見なし」と書く。

| 項目 | 内容 |
|---|---|
| 場所 | ファイルと節（行番号は書かない。見出しで特定する） |
| 現象 | 文書が何と言い、実体が何であるか。両方の根拠のファイルと節を示す |
| 影響 | その誤りを信じた読み手が何を間違えるか |
| 推奨修正 | どちらを正として何を直すか。正典が決まらない場合は、その旨と選択肢 |
| 区分 | 事実の誤り、正典の重複、文書間の矛盾、残骸、肥大のいずれか |
| 確度 | 実ファイルで両側を確認した、または片側だけ確認した |

推測だけの所見は返さない。片側しか確認できなかった場合は、その旨を確度に書く。

## 5. 所見の検証と起票

1. 親（監査を起動した人）が、所見を実ファイルで照合し直す。成り立たない所見は捨てる。
2. 機械検査で拾えるはずの所見は、起票せずに検査の改善課題として扱う。検査が拾えない理由を 1 行で添える。
3. 残った所見を、Linear の Project「azooKey Desktop / Windows IME MVP」に起票する。起票の前に重複を検索し、既存の課題があれば追記する。
4. 起票する課題には、`docs/linear-conventions.md` に従い、ラベル `area:docs`、`repo:azooKey-Desktop`、種別、担当を付ける。本文に場所・現象・影響・推奨修正を書く。同じ領域の小さな所見は 1 課題にまとめてよい。
5. 正典の決定や方針の選択を含む所見は、人間 lead の判断が要るので、`gate:human-required` を付けて選択肢と推奨を書く。
6. 全所見の件数と領域別の内訳を、監査の親課題（なければ tracking 課題）にコメントする。

## 6. 結果の読み方

- 所見が毎回、同じ型で出る場合は、機械検査へ昇格できる。昇格の候補は、新しい検査の課題として起票する。
- 所見が特定の文書に偏る場合は、その文書の分割や統合を人間 lead が検討する。`scripts/check_docs_budget.py` の警告が重なるなら、分割の根拠になる。
- 所見が少ない領域は、次回の対象を絞ってよい。絞った場合は、絞った理由を親課題に書く。
