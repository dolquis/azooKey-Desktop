# 正典マトリクス

同じ情報を 2 箇所に書かない。ここに書いた場所が正典で、他の場所はリンクだけにする。

| 情報種別 | 正典 | 他の場所での扱い |
|---|---|---|
| 状態・進捗・優先度・担当・サイクル | Linear（team `Dev`） | repo docs には一切書かない |
| 課題トラッキング（バグ・タスクの起票/状態/優先度） | Linear | GitHub Issues は mirror |
| 機能仕様（IPC payload・JSON schema・永続化形式・責務境界・fallback・設定項目・ユーザー可視挙動） | 対応する `docs/*-spec.md` | 他の spec や roadmap から引用するときは「正典は X」の 1 行 + リンク |
| マイルストーン定義・依存関係・受け入れ条件の「定義」・スコープ・リスク | `plans/windows-port-roadmap.md`（1 本に一本化） | 受け入れ条件を spec 側に書かない。roadmap は達成状態を持たない |
| ビルド・テスト手順 | `README.md` | `docs/debugging.md` 等は差分（Linux 限定の挙動、CI 固有の引数など）だけを書く |
| Linear 運用のルール本文 | `docs/linear-conventions.md`（§1〜§12 は `dolquis/agent-ops` origin。本 repo では直接編集しない） | `AGENTS.md`「Linear とレビュー指摘」は要約。詳細は追わずリンクする |
| Linear 運用の repo 固有差分（ラベル・状態マップ等） | `docs/linear-conventions.md` §13 Project Delta | 本 repo で編集してよい唯一の Linear 運用節 |
| ドキュメント一覧・索引 | `docs/README.md` | 新規 `docs/*.md` を追加したら必ずここに載せる |
| 第三者資産の attribution | ルート `THIRD_PARTY_LICENSES` | 運用規約は `docs/licensing-policy.md`。配布経路の spec 側からはリンクのみで、attribution 一覧を重複させない |

## 正典宣言は双方向にする

A が「正典は B」と書いたら、B 側にも「本節は A から参照される」の 1 行を置く。片方向の宣言は、B の内容が変わったときに A が追随しない事故を生む。

## 1 ファイルに複数マイルストーンが同居する場合

`typo-correction-learning-spec.md`（M35 + M55）のように 1 spec が複数 M を扱うのは、機能的に地続きなら問題ない。ただし冒頭に「節 → M 対応表」を置き、どの節がどの M の範囲かを読み手が迷わないようにする。§ 番号を外部（roadmap・他 spec・README 索引）が参照している場合、番号を維持したまま拡張する（分割・改番は外部参照をすべて洗い出してから検討する）。

## README を簡潔に保つ

README はフォーク元に近い簡潔な紹介文書。詳細手順が伸びてきたら、コマンド例だけ残して詳細は `docs/debugging.md` や該当 spec にリンクする。「## 状態」「## 進捗」「## TODO」のような時系列で陳腐化する見出しは追加しない。
