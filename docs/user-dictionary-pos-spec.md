---
kind: spec
milestones: [M71, M71-A, M71-B]
canonical_for: ["ユーザー辞書の品詞と活用", "テキスト形式の辞書の取り込み"]
defers_to: [docs/lattice-conversion-spec.md §8.3, docs/learning-data-management-spec.md §4.5, docs/learning-data-management-spec.md §14]
---

# ユーザー辞書の品詞・活用と他 IME の辞書の取り込み 仕様

対象リポジトリ: dolquis/azooKey-Desktop
対応マイルストーン: M71（変換品質トラック。M71-A 品詞の保存と取り込み、M71-B 活用の展開）
関連: `docs/lattice-conversion-spec.md`（ラティス変換）、`docs/learning-data-management-spec.md`（ユーザー辞書の
IPC・CLI・writer 責務）、`docs/auto-word-registration-spec.md` §14（辞書層）、`docs/azdic-physical-layer-spec.md` §15.7
（mutable 層）、`docs/settings-app-spec.md` §3.7、`docs/licensing-policy.md`

ユーザー辞書（`learning::UserDictionary`、`user_dict.json.enc`）の語は品詞を持たず、登録した形でしか候補に出ない。
「ググる」を登録しても「ググった」は出ない。他の IME の辞書を持ち込む手段も無い。本書は、ユーザー辞書の語に
品詞を持たせ、動詞と形容詞の活用形をラティス（`docs/lattice-conversion-spec.md`）のノードとして展開する規則と、
Microsoft IME と Google 日本語入力のテキスト形式の辞書を取り込む規則を定める。

## 節とマイルストーンの対応

| 節 | マイルストーン |
|---|---|
| §1〜§3、§6〜§8 | M71-A |
| §4・§5 | M71-B |
| §9 | M71-A・M71-B 共通 |

## 正典範囲

本書が正典であるもの: 品詞の語彙、`user_dict.json` の version 2 と旧版との互換、活用の展開規則と展開形の
見える範囲、活用語の経路（Zenzai が正常に返した要求でユーザー辞書の活用形を含む経路を足す規則）、テキスト形式の
辞書の取り込み（形式、文字コード、品詞の対応表、衝突、上限、手順）、そのための IPC と CLI の追加、設定アプリの導線。

本書が委ねるもの:

| 主題 | 正典 |
|---|---|
| ラティスの構築、コスト、探索、縮退 | `docs/lattice-conversion-spec.md` §3〜§7 |
| 品詞の組から代表 ID を引く規則 | `docs/lattice-conversion-spec.md` §8.3 |
| ユーザー辞書の編集のロックの順と待ちの予算 | `docs/learning-data-management-spec.md` §4.5・§13 |
| `user_dict.json.enc` の writer と atomic replace | `docs/learning-data-management-spec.md` §13・§14 |
| backup の archive の衝突解決 | `docs/learning-data-management-spec.md` §6 |
| secure mode 中の学習データ操作の拒否 | `docs/learning-data-management-spec.md` §8 |
| 受け入れ条件 | `plans/windows-port-roadmap.md` M71 |

## 1. 目的と範囲

目的は次の 2 つである。

- 動詞と形容詞を登録したとき、活用した形を変換の候補に出す。
- Microsoft IME と Google 日本語入力から乗り換える利用者が、書き出した辞書を取り込めるようにする。

範囲に含めないもの:

- ATOK、ことえり、Gboard、macOS のユーザ辞書の形式。
- 抑制単語（候補に出さない語の登録）とサジェスト専用の語。
- カ行変格・ラ行変格・文語の活用、助動詞と付属語の登録。
- 確定履歴からの品詞の推定。
- 他 IME の辞書のコメントの保存。
- Zenzai の辞書ラティス制約付きデコード（`docs/zenzai-inference-spec.md` §4.2.2）。

## 2. 品詞の語彙

### 2.1 品詞 ID の表

ユーザー辞書の語は、次の表の品詞 ID を 1 つだけ持てる。品詞 ID は ASCII の文字列で、`user_dict.json` の
`pos`（§3.1）に保存する。表示名は設定アプリの選択肢（§8）に使う。UniDic 系の品詞は、ラティスの品詞の表
（`docs/lattice-conversion-spec.md` §8.2 の `META.lattice`）を引く品詞 1〜4 と活用型である。品詞はラティスの
ID と文節のまとめにだけ使い、ユーザー辞書の候補の score とタグは変えない（候補の score は今までどおり
`value` か既定の 1.5 である）。

| 品詞 ID | 表示名 | 品詞 1〜4 | 活用型 |
|---|---|---|---|
| （持たない） | 名詞 | `名詞,普通名詞,一般,*` | — |
| `noun_sahen` | サ変名詞 | `名詞,普通名詞,サ変可能,*` | — |
| `noun_adjectival` | 形容動詞 | `形状詞,一般,*,*` | — |
| `proper_noun` | 固有名詞 | `名詞,固有名詞,一般,*` | — |
| `person_name` | 人名 | `名詞,固有名詞,人名,一般` | — |
| `surname` | 姓 | `名詞,固有名詞,人名,姓` | — |
| `given_name` | 名 | `名詞,固有名詞,人名,名` | — |
| `place_name` | 地名 | `名詞,固有名詞,地名,一般` | — |
| `organization` | 組織名 | `名詞,固有名詞,一般,*` | — |
| `adverb` | 副詞 | `副詞,*,*,*` | — |
| `adnominal` | 連体詞 | `連体詞,*,*,*` | — |
| `conjunction` | 接続詞 | `接続詞,*,*,*` | — |
| `interjection` | 感動詞 | `感動詞,一般,*,*` | — |
| `prefix` | 接頭辞 | `接頭辞,*,*,*` | — |
| `suffix` | 接尾辞 | `接尾辞,名詞的,一般,*` | — |
| `counter` | 助数詞 | `接尾辞,名詞的,助数詞,*` | — |
| `symbol` | 記号・顔文字 | `記号,一般,*,*` | — |
| `verb_godan_ka` | 動詞（カ行五段） | `動詞,一般,*,*` | `五段-カ行` |
| `verb_godan_ga` | 動詞（ガ行五段） | `動詞,一般,*,*` | `五段-ガ行` |
| `verb_godan_sa` | 動詞（サ行五段） | `動詞,一般,*,*` | `五段-サ行` |
| `verb_godan_ta` | 動詞（タ行五段） | `動詞,一般,*,*` | `五段-タ行` |
| `verb_godan_na` | 動詞（ナ行五段） | `動詞,一般,*,*` | `五段-ナ行` |
| `verb_godan_ba` | 動詞（バ行五段） | `動詞,一般,*,*` | `五段-バ行` |
| `verb_godan_ma` | 動詞（マ行五段） | `動詞,一般,*,*` | `五段-マ行` |
| `verb_godan_ra` | 動詞（ラ行五段） | `動詞,一般,*,*` | `五段-ラ行` |
| `verb_godan_wa` | 動詞（ワ行五段） | `動詞,一般,*,*` | `五段-ワア行` |
| `verb_ichidan` | 動詞（一段） | `動詞,一般,*,*` | `上一段-<行>` か `下一段-<行>`（§4.3） |
| `verb_sahen` | 動詞（サ変・〜する） | `動詞,一般,*,*` | `サ行変格` |
| `adjective` | 形容詞 | `形容詞,一般,*,*` | `形容詞` |

- 名詞は品詞 ID を持たない形で表す。品詞を持たない語（§2.2）と同じ扱いになるので、名詞だけの辞書は
  version 1 のまま書ける（§3.3）。
- サ変名詞と形容動詞は活用しない。「する」「だ」「な」は付属語としてラティスの側（lattice_core）がつなぐ。
- UniDic 系の品詞は組織名を分けないので、`organization` は固有名詞と同じ組で引く。
- 表に無い品詞 ID を足すときは、本表とラティスの品詞の表への写像と、§6.5 の対応表を同時に改める。

### 2.2 品詞を持たない語

品詞を持たない語と、`pos` が本表に無い値の語（§3.2）は、名詞として扱う。ラティスでは
`docs/lattice-conversion-spec.md` §8.3 の既定（`名詞-普通名詞-一般` の代表 ID）で入り、文節のまとめでは
自立語になる（同 §7.1）。

### 2.3 出典とライセンス

品詞 ID の表、§4.2 の語尾表、§6.5 の対応表は本プロジェクトで作ったもので、Apache-2.0 で配布する。品詞名と
活用の語尾は文法上の事実である。UniDic 系の品詞名は、同梱済みの SudachiDict（内包の UniDic は BSD-3-Clause）が
使う名前を参照するだけで、データを複写しない。帰属は既存の `dictbuild/notices/sudachidict-legal.txt` と
`docs/auto-word-registration-spec.md` §14.10 の ThirdPartyNotices で足りる。

他 IME の形式と品詞名は、Mozc（BSD-3-Clause）の user dictionary の importer と書き出しのソース
（google/mozc の `src/client/user_dictionary/` と `src/data/rules/`）を読んで確かめた。§6.5 の Microsoft IME の
品詞名は、Microsoft IME の書き出しに出る名前で、Mozc の `third_party_pos_map.def` の MS-IME 節と名前を照合した。
Google 日本語入力の品詞名は、同じく `user_pos.def` と照合した。Mozc の表とコードは複写していない。品詞名は書き出しに
出る事実の名前なので帰属を要しないと判断し、`THIRD_PARTY_LICENSES` と `docs/licensing-policy.md` は変えない。

## 3. `user_dict.json` の形式と互換

### 3.1 形式

version 2 は、version 1（`learning/include/azookey/learning/UserDictionary.h` の形）の entry に任意の `pos` を足した形である。

```json
{
  "version": 2,
  "entries": [
    { "word": "ググる", "ruby": "ぐぐる", "pos": "verb_godan_ra" },
    { "word": "azooKey", "ruby": "あずーきー", "cid": 1285, "mid": 501, "value": -5.0 }
  ]
}
```

- `pos` は §2.1 の品詞 ID の文字列である。名詞の語は `pos` を持たない。
- `cid`、`mid`、`value` の意味は変えない。`cid` と `mid` はラティスの ID として読まない
  （`docs/lattice-conversion-spec.md` §8.3）。

### 3.2 読み込み

- `version` が無い辞書と、`version` が 1 か 2 の辞書を読む。
- `version` が 3 以上の整数の辞書も読み、候補と一覧には使う。ただし、その辞書を書き換える操作と書き出す操作を
  すべて止め、ファイルを変えない。新しい版が書いた内容を、知らない形のまま書き戻したり、違う版の番号で書き出したり
  しないためである。ファイルの隔離（`.corrupt.<stamp>`）はしない。止める操作と返す値は次のとおりとする。
  - `AddUserWord`、`RemoveUserWord`、`ImportUserDictionaryText`（§7.2）: `error = dictionary_too_new`
  - `ForgetLearningEntry`・`ResetLearningStore`・`ExportLearningData`・`ImportLearningData` の `user_dict`:
    `error = store_unavailable`（`docs/learning-data-management-spec.md` §6 の「保存を止めているストア」と同じ扱い）。
    backup の archive の `user_dict` の item 自体が version 3 以上のときも、import 全体を `store_unavailable` で失敗させる
  - `userdict` CLI の writer 操作（`--offline` の add / remove、`import`）と `export`: 終了コード 1
- Host の上の操作は、ディスクから読み直した辞書が version 3 以上なら、ディスクの辞書を変えずに断り、公開中の辞書を
  読み直した辞書に置き換える。稼働中に別の版が書き換えた辞書を、候補・一覧・export が古いまま扱わないためである。
  公開した後に利用者が辞書のファイルを消しても、Host を再起動するまで上の操作は断られ続ける（既知の制約）。
- root が object で `version` が 3 以上の整数なら、`entries` が無いか配列でないなど、その先の構造を解釈できなくても、
  隔離（平文の移行元の `.corrupt.<stamp>` を含む）をせず、空の辞書として保存もせず、上と同じに保存を止める。新しい版が
  構造を変えた辞書を、壊れたものとして退避して空の辞書で置き換えないためである。このとき候補と一覧は空になる。
- version 3 以上の辞書が平文の移行元（`.enc` へ移す前の `user_dict.json`）のときも、`.enc` へ移行しない。移行は書き換えに当たるためである。
- `version` が整数でない辞書と、0 以下の整数の辞書は、version 1 と同じに読む。`3.0` のように整数に等しい数は、その整数として扱う。
- entry と root の、本書と version 1 が定めない名前のメンバー（未知のメンバー）は、値を保ったまま読み込み、
  保存のときに同じ位置（entry のメンバーはその entry、root のメンバーは root）へ書き戻す。メンバーの順は保たなくてよい。
- `pos` が文字列でない entry は、`pos` を持たない語として読み、元の値を未知のメンバーとして保つ。
- `pos` が §2.1 の表に無い文字列の entry は、名詞として扱い（§2.2）、値をそのまま保って書き戻す。
  新しい版が足した品詞 ID を、品詞に対応した版どうしで消さないためである。

### 3.3 書き出し

- 品詞 ID を持つ語（表に無い文字列を保っている語を含む）が 1 つでもあれば `version: 2` を書き、無ければ
  `version: 1` を書く。品詞を使わない利用者の辞書は、品詞に対応する前の版と同じ形のまま残る。
- 読み込んだ `version` が 3 以上なら、保存も書き出しもしない（§3.2）。
- `Save` の書き方（DPAPI と atomic replace）と `.enc` への移行、平文の `.bak` は変えない
  （`docs/learning-data-management-spec.md` §13）。

### 3.4 キーと編集

- 語のキーは今までどおり（読み, 語）で、`pos` はキーに入れない。同じ読みと語を品詞違いで 2 つ持たない。
- `Add` は同じキーの語を、`pos` と未知のメンバーを含めて置き換える。品詞欄を持たない設定アプリや CLI で同じ語を
  足し直すと、品詞は消える。
- 削除（`RemoveUserWord`、`ForgetLearningEntry`）と、一覧の `id` の作り方は変えない。
- backup の archive の import（`docs/learning-data-management-spec.md` §6）は、`pos` を語の一部として扱う。
  `merge` と `keep_both` は既存の語を `pos` ごと残し、`overwrite` は `pos` ごと置き換える。
- `userdict export` は `Save` と同じ形（version 1 か 2）で書く。

### 3.5 品詞に対応する前の版との互換（既知の制約）

品詞に対応する前の `UserDictionary` は、`version` を検査せず、未知のメンバーを捨てて、`version: 1` と既知の
5 つのメンバーだけを書く。したがって次のとおりになる。

- 品詞に対応する前の版は version 2 の辞書を読める。品詞を持つ語は名詞として候補に出る。
- 品詞に対応する前の版の Host（`AddUserWord`、`RemoveUserWord`、`ForgetLearningEntry`、学習データの import、全削除）と `userdict` CLI
  （`--offline` の add / remove、`import`）が辞書を保存すると、すべての語の `pos` と未知のメンバーが消え、
  `version: 1` に戻る。語そのものは消えない。
- 品詞に対応する前の版は直せないので、この消失は防げない。本書はこれを既知の制約とし、版を戻した利用者には
  品詞を付け直してもらう。品詞に対応した版は、この消失を検出しない（消えた後の辞書は正しい version 1 の辞書である）。

## 4. 活用の展開

### 4.1 対象の活用型

活用を展開するのは、品詞 ID が `verb_godan_*`、`verb_ichidan`、`verb_sahen`、`adjective` の語である。
展開は、登録した形（終止形）から語幹を取り、§4.2 の語尾を付けた形（展開形）を作る。展開形は保存しない（§4.6）。

### 4.2 語尾表

語尾は読みに付けるひらがなである。語幹は、登録した読みから終止形の語尾を除いたものとする。五段は行の 5 段の
かな（カ行なら「か・き・く・け・こ」、ワア行は「わ・い・う・え・お」）で書く。

| 活用形 | 五段 | 一段 | サ行変格（〜する） | 形容詞（〜い） |
|---|---|---|---|---|
| 終止形の語尾（語幹の取り方） | う段 | る | する | い |
| `未然形-一般` | あ段（ワア行は「わ」） | （語尾なし） | し | — |
| `未然形-セ` | — | — | せ | — |
| `連用形-一般` | い段 | （語尾なし） | し | く |
| 連用形の音便（下記） | 行による | — | — | `連用形-促音便`「かっ」 |
| `連体形-一般` | う段 | る | する | い |
| `仮定形-一般` | え段 | れ | すれ | けれ |
| `命令形` | え段 | ろ | しろ | — |
| `意志推量形` | お段＋「う」 | よう | しよう | かろう |
| `語幹-一般` | — | — | — | （語尾なし） |

五段の連用形の音便は次のとおりとする。

| 行 | 活用形 | 語尾 |
|---|---|---|
| カ行・ガ行 | `連用形-イ音便` | い |
| タ行・ラ行・ワア行 | `連用形-促音便` | っ |
| ナ行・バ行・マ行 | `連用形-撥音便` | ん |
| サ行 | 持たない | — |

カ行五段で、読みが「いく」か「ゆく」で終わる語（「行く」の型）は、`連用形-イ音便` の代わりに `連用形-促音便`「っ」を作る。

- 終止形は登録した形そのもので、展開形に含めない。`連体形-一般` は終止形と同じ読みだが、連接の ID が違うので
  展開形として作る。
- 表に無い活用形（`連用形-融合`、`仮定形-融合`、`未然形-撥音便`・`終止形-撥音便` などの話し言葉の形、形容詞の
  `連用形-ウ音便`、サ行変格の命令形「せよ」）は作らない。
- 本表は、SudachiDict 20260723 の語（走る、笑う、書く、泳ぐ、話す、待つ、死ぬ、遊ぶ、読む、見る、食べる、察する、
  高い、行く）の活用形と、上の各形が一致することを確かめて定めた。SudachiDict は `意志推量形` に「う」を含む形
  （「走ろう」）と含まない形を両方持つ。本表は「う」を含む形だけを作る。

### 4.3 一段の行

`verb_ichidan` の活用型は、読みの末尾の「る」の直前のかなで決める。

- い段のかななら `上一段`、え段のかななら `下一段` とする。
- 行は、そのかなの行とする。「い」「え」は `ア行`、濁音は `ガ行`・`ザ行`・`ダ行`・`バ行` とする。
- 例: 「たべる」（べ）は `下一段-バ行`、「みる」（み）は `上一段-マ行`、「かんがえる」（え）は `下一段-ア行`。
- 直前のかなが、カ・ガ・サ・ザ・タ・ダ・ナ・ハ・バ・マ・ラ・ア の各行のい段・え段でないとき（「ぺ」「ゃ」、う段の
  「ぐ」など）は、活用型が決まらないので `verb_ichidan` を付けられない（§4.4）。

### 4.4 登録できる語

活用する品詞 ID は、次をすべて満たす語にだけ付けられる。満たさない語は、`AddUserWord` が `invalid_pos` で拒否し、
取り込みが `pos_mismatch` で除く（§6.4）。

- 読みが終止形の語尾（§4.2 の表の 1 行目）で終わり、語幹が 1 文字以上ある。五段は、読みの末尾のかなが品詞 ID の
  行のう段である（`verb_godan_ra` なら「る」）。一段は、「る」の直前のかなで §4.3 の活用型が決まる。
- 語の末尾が、読みと同じ終止形の語尾（ひらがな）で終わる。「ググる／ぐぐる」は付けられ、「Google／ぐぐる」と
  「ググル／ぐぐる」は付けられない。

展開形の表層は、語から終止形の語尾を除いた部分に、§4.2 の語尾を付けたものとする（「ググる」→「ググっ」）。

### 4.5 ID と語コスト

- 各展開形の左 ID と右 ID は、（品詞 1〜4、活用型、活用形）の組を `META.lattice` の品詞の表で引いた代表 ID とする
  （`docs/lattice-conversion-spec.md` §8.3）。表に無い組の展開形は作らない。
- 登録した形の ID は、活用する品詞では `終止形-一般` の組、活用しない品詞では（品詞 1〜4、`*`、`*`）の組で引く。
  表に無ければ、同 §8.3 の規則（品詞 1〜4 だけで引き、無ければ `名詞-普通名詞-一般`）に従う。
- ノードの品詞は引いた組の品詞とし、文節のまとめ（同 §7.1）に使う。`prefix` は後ろの自立語と同じ文節に入り、
  `suffix` と `counter` は付属語になる。
- 語コストは、品詞によらず、可変層の既定の語コスト（同 §5.3）とする。展開形にも同じ値を使う。
- lattice_core が無いか無効なとき（同 §3.6）は、ラティスを作らないので、展開形も使わない。

### 4.6 展開形の索引と見える範囲

- Host は、公開したユーザー辞書（`dictionary.userDictionaryEnabled` が真のとき）と lattice_core から、展開形の
  索引をメモリに作る。索引のキーは展開形の読みを `NormalizeReading()` で正規化したもので、表層と品詞の表の行を持つ。
  ユーザー辞書を公開し直したとき（編集、取り込み、設定の再読込）と lattice_core を読み直したときに作り直す。
- 索引は永続化しない。`user_dict.json` にも `.azdic` にも入らない。
- 展開形は、ラティスがノードを引く語彙のインターフェース（`docs/lattice-conversion-spec.md` §3.1）にだけ渡す。
  次の経路には出さない: `DictionaryStore` の各層の `Lookup`（完全一致・前方一致・予測）、ユーザー辞書の独立した
  候補注入、新語判定（`ContainsKnownWord`）、`ListLearningEntries`、`userdict list` / `export`、`lookup` CLI。
  ユーザー辞書の層が返すのは登録した形だけなので、`docs/azdic-physical-layer-spec.md` §15.8 の「どの層も同じ
  意味論の結果集合を返す」は変わらない。
- 展開形の数は、活用する語 1 語あたり 7 以下である。索引が 100,000 件を超える分は作らず、警告ログを 1 回残す。
  作る順は、ユーザー辞書のキーの昇順とする。

## 5. 活用語の経路

### 5.1 段階 1 との関係

ラティスの段階 1（`docs/lattice-conversion-spec.md` §3.2）では、展開形は可変層のノードとして他の語と同じに入る。
本節の規則は足さない。

### 5.2 Zenzai が正常に返した要求

段階 1 の条件に当たらない要求（Zenzai が正常に候補を返した要求）では、次をすべて満たすときに限り、ラティスを作って
ユーザー辞書の展開形を含む経路を 1 件足す。この経路を活用語の経路と呼ぶ。

- `live` が偽の要求である（`ConversionContext::live`）。ライブ変換は 1 位しか示さず、活用語の経路は 1 位に来ないためである。
  再変換も `live = true` で送られ、Host はライブ変換と区別できない（`docs/host-availability-spec.md` §8.5.2）ので、
  再変換の候補にも本節の経路は足されない。ラティスが `live` の要求で 1 件だけを出す（`docs/lattice-conversion-spec.md` §3.5）のと同じ扱いである。
- `dictionary.userDictionaryEnabled` が真で、lattice_core が使える。
- 読みの長さが 64 code point 以下である。
- 正規化した読みのどこかの位置から、展開形の索引（§4.6）が前方一致で 1 件以上当たる。

ラティスは段階 1 と同じに作る（同 §4）。探索は、前向き Viterbi の状態（同 §6.1）に「経路が活用する品詞のユーザー辞書の
語のノード（登録した形か展開形）を含むか」の 1 bit を足し、EOS でその bit が立つ状態のうち最小コストの経路を 1 件取る。
同点は同 §6.3 で決める。

同じ前向きの計算で、bit によらない最小コスト（制約なしの最良の経路のコスト）も EOS で得られる。bit が立つ最良の経路の
コストが、制約なしの最良の経路のコストより T を超えて大きいときは、何も足さない。T はエンジンの定数で、初期値は 10000
（P_unk の初期値の半分）とし、実機確認（§9 の Human Gate）で見直す。語幹だけの展開形（一段の未然形・連用形、形容詞の
語幹）は索引のキーが短く、無関係な読みでも当たるので、その読みでモデルの 1 位の直下に不自然な候補を出さないためである。
たとえば「エモい」を登録して「えもじ」と打つと、「エモ」で索引が当たるが、「エモ＋じ」の経路が「絵文字」の経路より
T を超えて高ければ足さない。「ぐぐった」のように、ユーザー辞書の語を含む経路が最良そのものなら差は 0 で、足す。

作る時機と期限は、同 §3.2 のモデル経路から落ちた要求と同じとする。変換の後に `state_mutex_` を取り直して作り、
作り始めた時刻から最大 15 ms を deadline とし、要求の `cancel` と、要求自身の期限の残りを引き継ぐ。前向きの計算が
終わる前に cancel か deadline に達したら、何も足さない。Zenzai の予算 600 ms とこの 15 ms の和は、候補窓用の
`QueryCandidates` の timeout 750 ms に収まる。

ラティスの段階 2（同 §3.3）を有効にしたときも、本節の規則は別に掛かる。同じ表層は `DedupMergedCandidates` が畳む。

### 5.3 候補の値

| 項目 | 値 |
|---|---|
| `surface` | 経路の表層 |
| `reading` | 要求の読み全体 |
| `score` | `max(0.1, s₁ − 0.01)`。s₁ は変換器の結果のうち source が `Model` の候補の最大の score。`Model` の候補が無ければ足さない |
| `source` | `CandidateSource::SystemDictionary` |
| `debug_info` | `lattice-user` |
| `tag` | 付けない |

- score はモデルの 1 位の直下に置く（同 §3.3 の「score 帯はモデルの 1 位より下」に合わせる）。経路の残りの語は
  ラティスの精度なので、モデルの 1 位を押し下げない。
- source を `UserDictionary` にしない。同じ表層の Zenzai の候補があるとき、`DedupMergedCandidates` のソース優先
  （`docs/zenzai-inference-spec.md` §7.6）が低い score の側を残してしまうためである。
- 合成（`DedupMergedCandidates` 以降）の前に足す。NLL 再ランク、学習の再ランク、タグの補正は他の候補と同じに掛かる。

### 5.4 縮退

| 状況 | 挙動 |
|---|---|
| lattice_core が無いか無効（同 §3.6） | 活用語の経路を足さない |
| 展開形の索引が当たらない | ラティスを作らない。候補は本節が無いときと同じになる |
| 展開形を含む経路が無い（EOS で bit が立つ状態が無い） | 何も足さない |
| bit が立つ最良の経路のコストが、制約なしの最良から T を超えて大きい（§5.2） | 何も足さない |
| cancel か deadline | 何も足さない |

どの縮退も Host を止めない。理由をログに残すときは、読みと表層を入れない（`docs/observability-spec.md` §7.6）。
M52 のベンチはユーザー辞書を読まないので、本節は baseline を変えない。

## 6. テキスト形式の辞書の取り込み

### 6.1 形式

| 形式 ID | 出どころ | 構造 |
|---|---|---|
| `msime` | Microsoft IME の辞書ツールの一覧の出力 | 1 行目が `!Microsoft IME` で始まる（大文字と小文字を区別しない）。`!` で始まる行は注釈。データ行は TAB 区切りで、読み・語句・品詞・コメント（任意）の順 |
| `google` | Google 日本語入力の辞書ツールの書き出し | ヘッダを持たない。`#` で始まる行は注釈。データ行は TAB 区切りで、読み・単語・品詞・コメントの順。コメントが空でも 4 列目の区切りを持つ |
| `azookey_tsv` | `userdict import` の TSV（§7.5） | 読み・語・`cid`・`mid`・`weight`・品詞 ID の 2〜6 列。3 列目以降は空でよい |

形式の確かさは次のとおりである。

- `google` の構造は、Mozc の書き出し（`UserDictionaryStorage::ExportDictionary`）のソースで確かめた。書き出しは
  ヘッダと BOM を持たず、Mozc の内部の文字列（UTF-8）をそのまま書く。Google 日本語入力の製品が同じ形で書くことと、
  Windows 版の改行が CRLF か LF かは、ソースからは決まらない。実ファイルとの照合は §9 の Human Gate が担う。
- `msime` の構造は、Mozc の importer が Microsoft IME の書き出しとして受け付ける形で確かめた。Microsoft は書き出しの
  形式を公開していない（Microsoft Learn にあるのは `msime.h` の API だけである）。Microsoft IME が書く
  文字コード、ヘッダ行の並び、品詞名の一覧の実ファイルとの照合は、§9 の Human Gate が担う。Mozc の importer は、利用者が
  選べる品詞の名前のほかに `msime.h` の内部の品詞の名前（地名、社名、組織、数、記号、英字など）も受け付ける。それらが
  実ファイルに現れるかの照合も同じ Human Gate が担い、§6.5 の表に無いので `unsupported_pos` として除く。

### 6.2 文字コード

ファイルの先頭のバイト列で決め、決めた文字コードで全体を厳格に解く。

1. `EF BB BF` で始まれば UTF-8、`FF FE` なら UTF-16LE、`FE FF` なら UTF-16BE とする。BOM は内容に含めない。
2. BOM が無ければ、全体を UTF-8 として検査し、正しければ UTF-8 とする。
3. UTF-8 でなければ CP932 として解く。Windows では `MultiByteToWideChar(932, MB_ERR_INVALID_CHARS, ...)` を使う。
   CP932 の解読を持たないビルド（Linux）では `unsupported_encoding` で失敗する。
4. どれでも解けないか、解いた結果に U+0000 を含むときは `bad_encoding` で失敗する。BOM の無い UTF-16 は扱わない。

### 6.3 形式の判定

要求の `format` が `auto` のときは、解いた内容から次の順で決める。

1. 最初の空でない行が `!Microsoft IME` で始まれば（大文字と小文字を区別しない）`msime`。`# Gboard Dictionary version:`、
   `!!DICUT`、`!!ATOK_TANGO_TEXT_HEADER` で始まれば（大文字と小文字を区別しない）、`unknown_format` で失敗する。
2. `#` で始まらない最初の空でない行を TAB で分け、3 列以上あって 3 列目（§6.4 の品詞の正規化の後）が §6.5 の
   Google 日本語入力の品詞名にあれば `google`。判定には、対象外の行（抑制単語、サジェストのみなど）の名前も含める。
   先頭の行が対象外の品詞の Google 日本語入力の辞書を、`unknown_format` にしないためである。
3. 同じ行が `azookey_tsv` の 1 行として読めれば `azookey_tsv`。
4. どれでもなければ `unknown_format` で失敗する。

`format` を明示したときは判定しない。ただし `msime` を明示して最初の空でない行が `!Microsoft IME` で始まらなければ
`unknown_format` で失敗する。どの `format` でも、手順 1 の Gboard と ATOK のヘッダで始まるファイルは `unknown_format` で
失敗する。ことえり（引用符で囲む CSV）は TAB を含まないので、`auto` では手順 4 の `unknown_format` になる。

### 6.4 行の解釈と正規化

- 改行は CRLF、LF、CR のどれでもよい。空の行と、形式ごとの注釈の行は読み飛ばし、件数に数えない。
- `msime` と `google` は TAB で分け、3 列未満の行を `malformed` として除く。5 列目以降は無視する。
- `azookey_tsv` は `userdict import` の規則（`docs/learning-data-management-spec.md` §13）に 6 列目の品詞 ID を
  足して読み、読めない行を `malformed` として除く。6 列目が空なら名詞とする。6 列目が §2.1 の表に無い値なら
  `unsupported_pos` として除く（外部のファイルの未知の値は、§3.2 と違って保たない）。
- 読みと語は、前後の空白（U+3000 を含む）を除き、読みのカタカナをひらがなに直す。設定アプリの追加
  （`docs/learning-data-management-spec.md` §3）と同じ正規化である。
- 品詞名は、前後の空白を除き、全角英数を半角に、半角カタカナを全角に直してから §6.5 の表で引く。
- 次の行を除き、理由ごとに数える。
  - 読みか語が空、または制御文字（U+0000〜U+001F、U+007F）を含む: `malformed`
  - 読みが 64 code point を超えるか、語が 128 code point を超える: `too_long`
  - 品詞名が表に無いか、表で対象外とした品詞: `unsupported_pos`
  - 活用する品詞で §4.4 を満たさない: `pos_mismatch`
- コメントは保存しない。`msime` と `google` の行は `cid`、`mid`、`value` を持たない。

### 6.5 品詞の対応表

他 IME の品詞名から §2.1 の品詞 ID への対応を次に定める。品詞名は、Mozc の importer が各 IME の書き出しとして
受け付ける名前と照合した（Microsoft IME は `third_party_pos_map.def` の MS-IME 節のうち利用者が選べる品詞の名前、
Google 日本語入力は `user_pos.def` の利用者向けの品詞の名前。§2.3）。表に無い名前の行は `unsupported_pos` として除く。

| 品詞 ID | Microsoft IME の品詞名 | Google 日本語入力の品詞名 |
|---|---|---|
| （名詞） | 名詞、副詞的名詞、その他自立語、慣用句、独立語、短縮よみ | 品詞なし、名詞、短縮よみ、数、アルファベット、独立語 |
| `noun_sahen` | さ変名詞、ざ変名詞 | 名詞サ変 |
| `noun_adjectival` | 形容動詞、形動名詞、さ変形動名詞 | 名詞形動 |
| `proper_noun` | 固有名詞 | 固有名詞 |
| `person_name` | 人名 | 人名 |
| `surname` | 姓 | 姓 |
| `given_name` | 名 | 名 |
| `place_name` | 地名その他 | 地名 |
| `organization` | — | 組織 |
| `adverb` | 副詞 | 副詞 |
| `adnominal` | 連体詞 | 連体詞 |
| `conjunction` | 接続詞 | 接続詞 |
| `interjection` | 感動詞 | 感動詞 |
| `prefix` | 接頭語、姓名接頭語、地名接頭語 | 接頭語 |
| `suffix` | 接尾語、姓名接尾語、地名接尾語 | 接尾一般、接尾人名、接尾地名 |
| `counter` | 助数詞 | 助数詞 |
| `symbol` | 顔文字 | 記号、顔文字、句読点 |
| `verb_godan_ka` | か行五段 | 動詞カ行五段 |
| `verb_godan_ga` | が行五段 | 動詞ガ行五段 |
| `verb_godan_sa` | さ行五段 | 動詞サ行五段 |
| `verb_godan_ta` | た行五段 | 動詞タ行五段 |
| `verb_godan_na` | な行五段 | 動詞ナ行五段 |
| `verb_godan_ba` | ば行五段 | 動詞バ行五段 |
| `verb_godan_ma` | ま行五段 | 動詞マ行五段 |
| `verb_godan_ra` | ら行五段 | 動詞ラ行五段 |
| `verb_godan_wa` | あわ行五段 | 動詞ワ行五段 |
| `verb_ichidan` | 一段動詞 | 動詞一段 |
| `verb_sahen` | — | 動詞サ変 |
| `adjective` | 形容詞 | 形容詞 |
| 対象外（`unsupported_pos`） | 抑制単語 | サジェストのみ、抑制単語、終助詞、動詞ハ行四段、動詞カ変、動詞ザ変、動詞ラ変 |

- 短縮よみ・独立語・慣用句は、登録した語全体を 1 つの名詞として扱う。
- 対象外の品詞は、取り込むと意味が変わる（抑制単語が候補に出る）か、§4 で展開できない活用型である。

### 6.6 衝突と重複

- キーは正規化した後の（読み, 語）である（§3.4）。
- 同じファイルの中で同じキーが繰り返されたら、後の行を採り、`duplicates_in_file` に数える。既存の `userdict import` と同じ規則である。
- 既存の語との衝突は `ImportConflictPolicy`（`learning/include/azookey/learning/ImportConflictPolicy.h`）の 3 値で決める。
  品詞だけが違う同じキーも衝突とする。

| 方針 | 衝突した語 |
|---|---|
| `merge`（設定アプリの既定） | 既存の語を `pos`、`cid`、`mid`、`value` ごと残す |
| `overwrite` | 取り込んだ語で置き換える。`msime` と `google` の語は `cid`、`mid`、`value` を持たないので、それらは消える |
| `keep_both` | `merge` と同じ。キーが一意なので 2 つは持てない |

- 衝突しない語は、どの方針でも足す。
- 外部のファイルなので、§6.4 で除いた行があっても、残りを取り込んで成功とする。
  `docs/learning-data-management-spec.md` §6 の「解析できない行を含む item は import 全体を `bad_item` で失敗させる」は、
  Host が書いた backup の archive だけに掛かり、ここには当てはめない。

### 6.7 上限

| 項目 | 上限 | 超えたとき |
|---|---|---|
| ファイルの大きさ | 16 MiB | `too_large`。上限を 1 バイト超えるまでしか読まない |
| 行の数（空の行と注釈を含む） | 200,000 | `too_large` |
| 取り込んだ後の辞書の語数 | 100,000 | `too_many_words` |
| 取り込める行の数 | 1 以上 | 0 なら `no_entries` |

100,000 は、mutable 層に警告ログを出す閾値（`docs/azdic-physical-layer-spec.md` §15.7）と同じ値である。
上限を超えたときは、何も変えずに失敗する。

### 6.8 手順とデータ安全

取り込みは Host が行う。設定アプリは `user_dict.json.enc` を開かない（`docs/learning-data-management-spec.md` §14）。

1. `source_path` を検査する。絶対パスで `..` を含まないこと、ファイル名に `:` を含まないこと、UNC・デバイス名前空間・
   予約デバイス名でないこと、シンボリックリンクでない通常のファイルであることを求める（規則は
   `docs/learning-data-management-spec.md` §5 の `source_path` と同じ。拡張子は問わない）。違反は `invalid_path`、
   ファイルが無ければ `source_missing`（backup の import と同じ名前）とする。通常のファイルとして開けない・読めないときも `invalid_path` とする。
2. どのロックも取らずに、ファイルを読み、文字コードと形式を決め、全行を解析・正規化・検証する。ここで失敗したら、
   何も変えずに失敗を返す。元のファイルは変えず、隔離もしない。読み込んだ平文のバッファは、解析の後に
   `learning::SecureErase` で消す。
3. `dry_run` なら、状態のロックの中で、公開中の辞書のキーの集合と、その辞書が version 3 以上か・読み込みの失敗で保存を
   止めているかだけを複写する。version 3 以上なら `dictionary_too_new`、保存を止めていれば `store_unavailable` で失敗する
   （確定のときと同じ判定）。そうでなければロックの外で比べて件数を数え、取り込んだ後の語数が §6.7 の上限を超えるなら
   `too_many_words` で失敗し、超えなければ何も変えずに返す。変換の問い合わせを止める区間を
   短くするためである。下見の件数は確定のときの件数と違いうる（その間に他の編集が入りうる）。
4. `AddUserWord` と同じ順と同じ区間でロックを取る（`docs/learning-data-management-spec.md` §4.5）。ユーザー辞書の writer の
   mutex、ユーザー辞書のファイルロックの順に取り、どちらも手順 7 の公開まで持つ。状態のロックは、公開中の辞書を複写する
   短い区間と、手順 7 の公開の区間だけで取る。手順 5 と 6 の読み直し・merge・上限の検査・保存（DPAPI の暗号化を含む）は
   状態のロックの外で行い、その間も変換の問い合わせとライブ変換を待たせない。
   待ちの予算は `AddUserWord` と同じ 5 秒とし、手順 2 を終えた時刻から数える（要求の受信からではない。大きなファイルの
   解析で予算を使い切らないためである）。予算は、他の編集の終わりを待つ時間、ファイルロックを待つ時間、読み直しと merge
   の時間を合わせて数え、保存を始める前に過ぎていたら `busy` で失敗する。保存を始めた後は打ち切らない
   （`docs/learning-data-management-spec.md` §13 の `AddUserWord` と同じ）。
5. ディスクの辞書を読み直した複写（`ReloadIfChanged`）へ merge する。読み直しに失敗したら `load_failed`、読み込みの
   失敗で保存を止めている辞書なら `store_unavailable`、version 3 以上なら `dictionary_too_new`（§3.2）で、ディスクの辞書を変えずに失敗する。version 3 以上のときは、公開中の辞書が読み直した辞書に置き換わる（§3.2）。それ以外の失敗では公開中の辞書も変えない。
6. 語数の上限（§6.7）を検査してから、`Save`（DPAPI と atomic replace）で保存する。保存に失敗したら `save_failed` を返し、
   公開中の辞書を変えない。atomic replace なので、ディスクの辞書は変更前か変更後のどちらかである。
7. 保存に成功したときだけ、状態のロックを取って複写を公開し、展開形の索引（§4.6）を作り直す。

- secure mode の拒否（`docs/learning-data-management-spec.md` §8）は、Host 全体の状態で判定する。設定アプリの要求は
  入力欄の secure と学習可否を持たないためである。Host の privacy の方針（`SettingsStore::LockPrivacyPolicy`）が
  secure か学習を許さないとき、または SafeMode のとき、手順 1 の前に `error = blocked` で失敗する。`AddUserWord` は
  利用者が 1 語ずつ明示する編集なのでこの判定を掛けない（同 §3）が、外部のファイルからの一括の取り込みは、backup の
  import と同じく §8 の対象とする。
- Host は 1 回の取り込みを RuntimeLogger の `persistent_edit`（`docs/learning-data-management-spec.md` §13）に、
  `op` を `user_dict_import` として記録する。項目は同 §13 の項目と、件数（`added`、`replaced`、`kept`、
  `duplicates_in_file`、除いた行の合計）である。語、読み、パスは記録しない。

## 7. IPC と CLI

### 7.1 `AddUserWord`

- 要求に任意の `pos`（§2.1 の品詞 ID の文字列）を足す。無ければ名詞として足す。
- Host は `pos` が表に無い値か、§4.4 を満たさない語に付いているとき、`ok = false`、`error = invalid_pos` を返し、保存しない。
- 品詞に対応する前の Host は `pos` を黙って捨てる（`ipc/src/Payloads.cpp` の `ParseAddUserWordRequest` は
  知らないメンバーを無視する）。設定アプリは capability `user_dict_pos`（§7.4）を告知した Host にだけ `pos` を送る。
- ユーザー辞書の `error` に `invalid_pos` と `dictionary_too_new`（§3.2）を足す。backup の `BackupErrorCode` の
  `unsupported_version`（archive の manifest の版）と意味を分けるため、別の名前にする。`userdict add` の表示は、
  `invalid_pos` は品詞と語が合わないこと、`dictionary_too_new` は新しい版が書いた辞書なので変えないことを示す
  （表示の規則は `docs/learning-data-management-spec.md` §13）。

### 7.2 `ImportUserDictionaryText`

`MessageType` の末尾に足す（M40 の互換規則）。Host は Handshake の後でだけ受け付ける。

```json
{ "type": "ImportUserDictionaryText",
  "payload": { "source_path": "C:\\Users\\me\\Desktop\\msime-dict.txt",
               "format": "auto", "conflict_resolution": "merge", "dry_run": true } }
```

- `format`: `auto` / `msime` / `google` / `azookey_tsv`。省略は `auto`。
- `conflict_resolution`: `merge` / `overwrite` / `keep_both`。省略は `merge`。
- `dry_run`: 真なら §6.8 の手順 3 で返す。省略は偽。

応答:

```json
{ "ok": true, "format": "msime", "encoding": "utf-16le",
  "counts": { "added": 120, "replaced": 0, "kept": 3, "duplicates_in_file": 1,
              "skipped": { "malformed": 2, "too_long": 0, "unsupported_pos": 5, "pos_mismatch": 1 } } }
```

- `encoding`: `utf-8` / `utf-16le` / `utf-16be` / `cp932`。
- `kept` は衝突して既存を残した語、`replaced` は衝突して置き換えた語の数である。
- 失敗のときは `ok = false` と `error` を返す。`error` は次のどれかである: `invalid_request`、`not_authenticated`、
  `blocked`、`invalid_path`、`source_missing`、`too_large`、`unsupported_encoding`、`bad_encoding`、`unknown_format`、
  `no_entries`、`too_many_words`、`busy`、`lock_failed`、`load_failed`、`store_unavailable`、`dictionary_too_new`、
  `save_failed`、`unavailable`（Host がユーザー辞書を持たない）。失敗のときも、解析を終えていれば `format`、`encoding`、
  `counts` を返してよい。

### 7.3 一覧

`ListLearningEntries` の `user_dict` の項目の `metadata` に、語が品詞 ID を持つとき `pos` を足す。展開形は一覧に出さない（§4.6）。

### 7.4 capability

| capability | 意味 |
|---|---|
| `user_dict_pos` | version 2 の読み書き（§3）、`AddUserWord` の `pos`、一覧の `pos` |
| `user_dict_text_import` | `ImportUserDictionaryText` |

Host は両方を Handshake の capabilities に載せる。設定アプリは、告知の無い Host には品詞欄と取り込みの導線を出さない。

### 7.5 CLI

`userdict` CLI は直接の writer のままとする（`docs/learning-data-management-spec.md` §13・§14）。

- `userdict add` に `--pos <品詞 ID>` を足す。IPC 送信でも `--offline` でも、§7.1 と同じに検査する。IPC 送信では、
  Handshake の応答が capability `user_dict_pos` を告知しないとき、送らずに終了コード 1 で終わる（品詞に対応する前の
  Host は `pos` を黙って捨てて `ok` を返すためである）。
- `userdict import <path>` に `--input-format <auto|msime|google|azookey_tsv>` と `--conflict <overwrite|merge|keep_both>` を
  足す。既存の `--format <json|tsv>` は出力の形の引数なので、入力の形には別の名前を使う。
- 既定は `--input-format azookey_tsv` と `--conflict overwrite` で、5 列以下の行の扱いは品詞に対応する前と同じ
  （`docs/learning-data-management-spec.md` §13 の規則のまま。BOM の除去、読みの正規化、§6.7 の上限は掛けない。
  既存の語も後勝ちで置き換え、不正な行は skip として数える）。設定アプリと IPC の既定の `merge` と違うのは、既存の
  スクリプトの結果を変えないためである。
- 6 列目の品詞 ID は、どの経路でも §2.1 と §4.4 で検査する。表に無い値の行は `unsupported_pos`、§4.4 を満たさない行は
  `pos_mismatch` として skip に数える。品詞に対応する前の CLI は 6 列の行を skip していたので、変わるのは 6 列の行だけである。
- `--input-format` が `msime`・`google`・`auto` のときは、解析、文字コード、正規化、上限を §6.2〜§6.7 に従う。
  JSON の報告は既存の `imported` と `skipped` に、§7.2 の `counts` を足す。

同じ `azookey_tsv` でも、IPC と CLI では次のとおり扱いが違う。CLI は既存のスクリプトの結果を保つことを優先する。

| 項目 | IPC（`ImportUserDictionaryText` の `azookey_tsv`） | CLI（`userdict import --input-format azookey_tsv`、既定） |
|---|---|---|
| 文字コード | §6.2（BOM を除き、UTF-8 か CP932） | 既存のまま（UTF-8 として読み、BOM を除かない） |
| 読みと語の正規化 | §6.4（前後の空白を除き、カタカナをひらがなに直す） | しない |
| 制御文字・長さの検査（§6.4） | する | しない |
| 上限（§6.7） | 掛ける | 掛けない |
| 6 列目の品詞 ID | §2.1・§4.4 で検査し、外れた行を除く | 同じ |
| 衝突の既定 | `merge` | `overwrite` |
| 5 列以下の行の結果 | 上の規則による | 品詞に対応する前と同じ |
- `userdict list` の JSON 行と `lookup` の JSON 行（`user_dict` の一致）に `pos` を足す。`lookup` の TSV の列は変えない。
- `userdict export` は §3.3 の形で書く。

## 8. 設定アプリの導線

学習ペインのユーザー辞書タブ（`docs/learning-data-management-spec.md` §3、`docs/settings-app-spec.md` §3.7）に次を足す。
Host が §7.4 の capability を告知したときだけ出す。macOS 版の監査の P-23（`docs/macos-input-parity-audit-report.md`）の
ユーザー辞書の導線は、この節の分も含めてこのタブに置く。

- 「品詞」の選択欄を、「読み」「語」の欄の隣に置く。選択肢は §2.1 の表の順と表示名で、既定は「名詞」。
  名詞のときは `pos` を送らない。`invalid_pos` が返ったら、品詞と語の末尾が合わないことを示す。
- 一覧に「品詞」の列を足し、表示名を出す。名詞と、表に無い品詞 ID の語は「名詞」と出す。
- 「ほかの IME の辞書を取り込む…」ボタンを置く。流れは次のとおりとする。
  1. ファイルを選ぶ（拡張子 `.txt`、`.tsv`、すべてのファイル）。
  2. 形式を選ぶ（自動 / Microsoft IME / Google 日本語入力 / azooKey TSV。既定は自動）。
  3. `dry_run` で下見をし、判定した形式と文字コード、足す語・既存と重なる語・除く行の件数（理由ごと）を示す。
  4. 衝突の方針を選ぶ（既存の語を残す〔`merge`、既定〕/ 取り込んだ語で置き換える〔`overwrite`〕）。
  5. 確認ダイアログを経て取り込み、結果の件数を示し、一覧を読み直す。
- 取り込みの応答は 30 秒まで待つ。待ちきれなかったときは、取り込みが後から終わりうることと、一覧で確かめることを示す。
- 失敗は §7.2 の `error` ごとの文言で示す。`busy` は再試行を促し、`unsupported_encoding`・`bad_encoding`・
  `unknown_format` は対応する形式と文字コードを示し、`dictionary_too_new` は新しい版の azooKey が書いた辞書なので
  変えないことを示し、`blocked` はプライバシーの設定か SafeMode のために取り込めないことを示す。

## 9. テスト計画

| 対象 | 試験 |
|---|---|
| `UserDictionary` | 品詞を持たない辞書の読み書きで version 1 のまま変わらない。品詞を足すと version 2 で書き、読み戻せる。未知のメンバー（entry と root）が往復で残る。表に無い品詞 ID が残る。version 3 以上の辞書で、§3.2 の各操作（Host の編集・忘却・全削除・export・import、CLI の writer と export）が決めた値で失敗し、ファイルが変わらない |
| 展開 | §4.2 の各型の代表の語で展開形の読み・表層・活用形が表どおりになる。一段の行（§4.3）。§4.4 を満たさない語で展開しない。索引の上限（§4.6） |
| 見える範囲 | 展開形が `DictionaryStore` の完全一致・前方一致・予測、`ContainsKnownWord`、`ListLearningEntries` に出ない |
| ラティス | fixture の語彙と行列で、登録した動詞の展開形と助動詞がつながる経路が出る（「ぐぐった」→「ググった」）。§5.2 の 1 bit の状態で、ユーザー辞書の語を含む最小コストの経路と、制約なしの最小コストが、それぞれ全列挙の最小と一致する |
| エンジン | §5.2 の各条件で活用語の経路が足され、それ以外（live、索引が当たらない、lattice_core が無い、cancel、deadline、制約なしの最良から T を超えて高い）では足されない。T の境目（差が T ちょうどなら足し、T を超えたら足さない）。§5.3 の score・source・debug_info |
| 取り込み | 自作の fixture（`msime` を UTF-16LE の BOM 付き、`google` を BOM なし UTF-8、`azookey_tsv`、CP932 の `msime`）で、§6.5 の対応表どおりに入る。§6.4 の除外の理由ごとの件数。§6.7 の各上限。`merge` と `overwrite`。CP932 は Windows だけで試す |
| データ安全 | 保存の失敗、ロックの時間切れ、読み直しの失敗、version 3 以上のそれぞれで、ディスクの辞書が変わらない。公開中の辞書は、version 3 以上のときだけ読み直した辞書に置き換わり、ほかでは変わらない。`dry_run` が何も変えない。待ちの予算が解析の後から数えられる（予算の注入で確かめる）。privacy の方針が secure・学習不可、SafeMode のとき `blocked`。ログに語・読み・パスが出ない |
| IPC と CLI | `AddUserWord` の `pos` と `invalid_pos`。`ImportUserDictionaryText` の要求と応答の往復。`userdict import` の既定で 5 列以下の行の結果が品詞に対応する前と同じになり、6 列目が §2.1・§4.4 で検査される。`--pos` を capability `user_dict_pos` の無い Host へ送らない |
| dictbuild | §4.2 の語尾表の各（活用型、活用形）の組が `META.lattice` の品詞の表にあり、SudachiDict の代表の語の活用形と語尾表が一致する |
| Human Gate | Microsoft IME と Google 日本語入力の実際の書き出しファイルを取り込める（§6.1 が照合を Human Gate に委ねた形式、文字コード、改行、品詞名を実ファイルと照らし、`msime.h` の内部の品詞名が現れるかを記録する）。実機で、活用した登録語が候補に出る |

試験の一覧の正典は `docs/test-inventory.md` である。利用者の実際の辞書は fixture にしない。

## 10. 受け入れ条件

M71 の受け入れ条件は `plans/windows-port-roadmap.md` M71 が定める。
</content>
</invoke>
