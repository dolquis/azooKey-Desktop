---
kind: spec
milestones: [M65]
canonical_for: ["モデル非経由のラティス変換"]
defers_to: [docs/azdic-physical-layer-spec.md §15.5, docs/conversion-quality-benchmark-spec.md §14.1]
---

# モデル非経由のラティス変換 仕様

対象リポジトリ: dolquis/azooKey-Desktop
対応マイルストーン: M65（変換品質トラック）
関連: `docs/azdic-physical-layer-spec.md`（`.azdic` の形式と取り込み）、
      `docs/zenzai-inference-spec.md`（§4.2.2 辞書ラティス制約付きデコード、§7.4 劣化モード）、
      `docs/neural-reranker-nll-spec.md`（NLL 再ランクの候補集合）、
      `docs/conversion-quality-benchmark-spec.md`（測定構成）、`docs/licensing-policy.md`

ラティス変換が無いとき、Zenzai を通らない経路の候補源は、辞書層の読み全体の完全一致
（`docs/auto-word-registration-spec.md` §14）と `SimpleConverter` だけになる。どちらも読みを語に分割しないので、
助詞を含む文は変換されない。本書は、辞書層の語と
連接コストからラティスを作り、コストの小さい順に N 件の経路を出す変換を定める。経路は語の列と文節の区切りを
持ち、文節の伸縮と部分確定、ユーザー辞書の活用対応の土台になる。

## 節とマイルストーンの対応

| 節 | マイルストーン |
|---|---|
| §1〜§12 | M65 |

## 正典範囲

本書が正典であるもの: ラティスの構築、コストと補正項、N-best の探索、区切りの制約と文節のまとめ、
エンジン上の位置づけと score 帯、連接情報の入手元と ID 空間、性能とメモリの予算、モデル無効構成の測定方法。

本書が委ねるもの:

| 主題 | 正典 |
|---|---|
| `.azdic` の任意 section `CONN`・`CMAT` のバイト配置と検証 | `docs/azdic-physical-layer-spec.md` §15.5 |
| SudachiDict からの連接情報の取り込み規則 | `docs/azdic-physical-layer-spec.md` §15.12 |
| ベンチの CLI と baseline の互換キー | `docs/conversion-quality-benchmark-spec.md` §7・§14.1 |
| Zenzai の劣化時の規則 | `docs/zenzai-inference-spec.md` §7.4 |
| 受け入れ条件 | `plans/windows-port-roadmap.md` M65 |

## 1. 目的と範囲

目的は次の 3 つである。

- モデル未ロード、劣化、deadline で Zenzai の途中結果が無いときに、文を語に分割した候補を出す。
- Zenzai が動いているときに、NLL 再ランク（`docs/neural-reranker-nll-spec.md` §B5）へ渡す辞書由来の候補を、
  読み全体に一致する語以外にも広げる。
- 文節の区切りを持つ変換結果を、区切りを指定した再変換の供給元にする。

範囲に含めないもの:

- Zenzai の辞書ラティス制約付きデコード（`docs/zenzai-inference-spec.md` §4.2.2）。本書のラティスは構造を共有できる
  形にする（§4.5）が、デコードそのものは同 spec が定める。
- IPC の変更。文節の区切りを wire に載せるかは、文節の伸縮の spec が決める（§7.4）。
- ユーザー辞書の品詞と活用の展開。本書は ID の無い語の扱いだけを定める（§8.3）。
- 確定履歴による連接コストの学習。

## 2. 用語

| 用語 | 意味 |
|---|---|
| ノード | 読みの区間 `[begin, end)`（正規化済み読みの code point 位置）に一致した 1 語。表層、左 ID、右 ID、語コスト、品詞を持つ |
| 左 ID・右 ID | 連接 ID。ノードが前の語とつながるときは左 ID、次の語とつながるときは右 ID を使う |
| 連接コスト | 前のノードの右 ID と次のノードの左 ID の組に対するコスト。連接行列から引く |
| 経路 | 読みの先頭（BOS）から末尾（EOS）までノードをつないだ列 |
| 経路コスト | §5.1 の式で求めるコスト。小さいほど良い |
| 連接の変種 | 同じ（読み, 表層）が持つ、異なる（左 ID, 右 ID, 語コスト, 品詞）の組。`.azdic` の 1 エントリが `CONN` に 0 個以上を持つ。0 個のエントリは §8.3 の既定 ID で使う |
| ID 空間 | 連接 ID の番号体系。連接行列と、各層の `CONN` が同じ ID 空間に属するときだけ組み合わせられる |
| lattice_core | ラティスだけが読む辞書ファイル `lattice_core.azdic`。かなだけの語、連接行列、品詞の表を持つ（§8.2） |

## 3. エンジン上の位置づけ

### 3.1 置き場所

ラティスは `IConverter` の実装にしない。`InferenceEngine::QueryCandidates` が、辞書層の完全一致を引くのと同じ
`state_mutex_` の区間で `DictionaryStore` を前方一致（`LookupMode::CommonPrefix`）で引き、ラティスを作る。
`DoubleArrayTrie` は持ち主が問い合わせを直列化する契約で、外から注入した変換器からは辞書層が見えないためである。
`SimpleConverter`、`ZenzaiModelConverter`、`IConverter` は変えない。`SimpleConverter` は引き続き
identity・長音・鉤括弧の候補、`Learn` / `Forget` / `Contains`（M36-A の新語判定）、`--mock-dict` を受け持つ。

ラティスの構築と探索は core の部品とし、辞書の問い合わせは語彙のインターフェース越しに受ける。部品は
`learning` に依存しない。エンジンが `DictionaryStore` と lattice_core をそのインターフェースへつなぐ。

### 3.2 段階 1: モデルを通らない経路

次のどれかに当たる要求で、ラティスの N-best を候補に足す。

- active converter が `SimpleConverter` である（モデル未ロード、ロード失敗、SafeMode）。
- Zenzai が `fallback_->Convert` の結果を返した。変換結果の `debug_info` に `zenzai-degraded` または
  `zenzai-deadline-fallback` が付く場合である（`docs/zenzai-inference-spec.md` §7.4）。
- エンジンが変換中の例外を受けて `SimpleConverter` で変換し直した（同 §7.1）。

モデル未ロードの要求では、辞書の完全一致と同じロック区間でラティスを作る。モデル経路から落ちた要求では、
変換の後に `state_mutex_` を取り直して作る。どちらも、合成（`DedupMergedCandidates` 以降）の前に候補へ足す。

段階 1 はエンジンの内部設定で切り替えられ、既定で有効とする。利用者向けの設定キーは持たない。ベンチの
`--lattice off`（§10.1）はこの内部設定を使う。

### 3.3 段階 2: Zenzai が動いているとき

Zenzai が正常に候補を返した要求で、ラティスの上位 k 件を `SystemDictionary` の候補として足し、NLL 再ランクの
候補集合（`docs/neural-reranker-nll-spec.md` §B5）に入れる。段階 2 は既定で無効とし、エンジンの内部設定で切り替える。
利用者向けの設定キーにするか、k と score 帯をどう置くかは、M52 で遅延と top1 を測ってから決める。score 帯は
モデルの 1 位より下に置き、`docs/zenzai-inference-spec.md` §7.5 の score 統合と矛盾させない。M65 の受け入れ条件は
段階 2 を含まない。段階 2 の採否は、段階 1 とは別の作業として測ってから決める。

### 3.4 score 帯、source、debug_info

段階 1 で足す候補は次のとおりとする。

- `score`: i 位（1 始まり）の経路について `1.0 − min(0.9, (cost_i − cost_1) / C)`。値は `[0.1, 1.0]` に入る。
  C は経路コストの尺度で、lattice_core の `META` に置き、M52 で校正する（§10.3）。
- `source`: `CandidateSource::SystemDictionary`。
- `reading`: 要求の読み全体。
- `debug_info`: `lattice;rank=<i>`。
- `tag`: 付けない。`AssignHeuristicTags` と `ApplyTagBoosts` は他の候補と同じに掛かる。

この帯は、ユーザー辞書の既定（1.5）と学習で足した候補（1.2）より下にある。1 位は `SimpleConverter` の identity
（0.6）より上に来る。辞書の完全一致の候補（`docs/auto-word-registration-spec.md` §14.11 の `dictionary_score`）は、
帯を変えずにそのまま合成する。その score は頻度の高い語で 1.0 を超えるので、読み全体に一致する語はラティスの 1 位より
上に来うる。1.0 以下の語はラティスの候補の間に入る。同じ表層の候補は `DedupMergedCandidates` が 1 つに畳む。
`UserDictionary` の候補は score によらず残り、それ以外は score の高い方が残る。

### 3.5 live 変換と件数

live 変換（`ConversionContext::live`）では N = 1 とする。それ以外の N は既定 8 とし、要求の `max_candidates` が
小さければそちらに合わせる。8 は NLL 再ランクの `nllTopK` の既定と同じ値である。

### 3.6 縮退

| 状況 | 挙動 |
|---|---|
| lattice_core が無い、検証で拒否された | ラティスを作らない。候補は段階 1 の前と同じになる |
| ある層が `CONN` を持たない | その層の語は §8.3 の既定 ID で使う |
| ある層の `connection_space` が lattice_core と違う | その層の `CONN` を使わず、§8.3 の既定 ID で使う |
| ある層の `CONN` の ID が `CMAT` の添字表に無い | その層の `CONN` を使わず、§8.3 の既定 ID で使う。層の完全一致・前方一致・予測は続ける |
| 読みが 64 code point を超える | ラティスを作らない |
| 探索中に cancel または deadline に達した | §6.4 のとおり、1 位だけを足すか、ラティスの候補を足さない |

どの縮退も、理由をログに残し、Host を止めない。ログには読みと表層を入れない（`docs/observability-spec.md` §7.6）。

## 4. ラティスの構築

### 4.1 入力

入力は `NormalizeReading()` で正規化した読みである。位置は code point で数える。

### 4.2 ノード

各開始位置 i について、次の語彙を `CommonPrefixSearch` で引く。

- 静的層（`base_lexicon`、`sudachi_lexicon`、`neologd_lexicon`、`named_entity_lexicon`、`technical_terms_lexicon`
  のうち有効なもの）
- lattice_core
- 可変層（`user_dictionary`、`auto_words`、`app_specific_dictionary`）

`max_results` は 0（無制限）で呼ぶ。上限に達した `CommonPrefixSearch` は短いキーから埋めて打ち切り、打ち切りを
知らせない（`docs/azdic-physical-layer-spec.md` §15.4）。ラティスでは長い語ほど落ちるためである。

一致した 1 エントリは、連接の変種ごとに 1 ノードになる。ノードの品詞と表記の種別（§5.2）は、変種の `attr`
（`docs/azdic-physical-layer-spec.md` §15.5。品詞の添字と、表層がカタカナだけか・上流の正規化形と別表記かの印）から
取る。変種の無いエントリは、§8.3 の既定 ID、`ENTS` の `cost` と `pos_id` で 1 ノードになり、表記の種別は表層の
文字から決める。複数の層が同じ（読み, 表層, 左 ID, 右 ID, 品詞）を持つときは、語コストの小さい 1 ノードに畳む。

探索の間、ノードは（層, エントリの index, 変種の index）と区間、ID、コストだけを持つ。表層の文字列は、出力する
経路について最後に組み立てる。`ReadEntries` の文字列の複写と UTF-8 の検証が、構築の主なコストになるためである。

### 4.3 未知語ノード

各位置に、読みの 1 文字をそのまま表層とする未知語ノードを 1 つ置く。どの位置からも経路が必ずあるようにする
ためである。左 ID と右 ID は `名詞-普通名詞-一般` の代表 ID（§8.3）、語コストは 0 とし、コストは §5.2 の
補正項 P_unk と P_node だけが掛かる。

### 4.4 BOS と EOS

BOS の右 ID と EOS の左 ID は 0 とする。SudachiDict の連接行列は ID 0 を文頭・文末に使う。

### 4.5 構造の共有

ラティスは、ノードの列（区間、ID、語コスト、エントリの参照）と、位置ごとの開始・終了ノードの索引として持つ。
探索（§6）はこの構造を読むだけで書き換えない。`docs/zenzai-inference-spec.md` §4.2.2 の制約付きデコードは、
同じ構造を経路の候補として読める。

## 5. コスト

### 5.1 経路コスト

経路 `n_1 … n_k` のコストを次の式で求める。

```
cost = Σ_{t=1..k} ( word_cost(n_t) + penalty(n_t) )
     + connect(0, left(n_1))
     + Σ_{t=1..k-1} connect(right(n_t), left(n_{t+1}))
     + connect(right(n_k), 0)
```

`connect(r, l)` は連接行列の値で、前のノードの右 ID r と次のノードの左 ID l から引く。和は 32 bit 符号付き整数で
計算する。語コストと連接コストは 16 bit で、補正項（§5.2）は 1 ノードあたり 16 bit の範囲に収まる値とするので、
64 code point の読みで溢れない。

### 5.2 補正項

SudachiDict の語コストと連接コストは、形態素解析のために学習された値である。解析では表層が与えられるので、
同じ語の漢字表記とカタカナ表記は競合しない。変換では読みから表層を選ぶので、両者が同じ読みで競合する。補正なしの
試作では、カタカナ表記の異表記が経路の多くを占めた。そこで次の補正項を足す。

| 補正項 | 掛ける対象 | 初期値 |
|---|---|---|
| P_kata_variant | 表層がカタカナと長音符だけで、上流の正規化形が別の表記の語（漢字・かなで書く語のカタカナ表記） | 6000 |
| P_kata_own | 表層がカタカナと長音符だけで、上流の正規化形が表層と同じ語（外来語など、カタカナで書く語） | 0 |
| P_node | すべてのノード（1 語ごと） | 3000 |
| P_unk | 未知語ノード（1 文字ごと） | 20000 |

`CONN` を持たない語は上流の正規化形を持たないので、表層がカタカナだけなら P_kata_own を掛ける。

`penalty(n)` は、そのノードに当たる補正項の和である。値は lattice_core の `META` に置く。カタカナ表記の罰金を 2 つに
分けるのは、外来語を罰しないためである。試作ではカタカナだけの表層すべてに 6000 を掛けたが、評価データに外来語が
ほとんど無いので、その値は外来語への影響を測れていない。初期値は M52 で校正し直す（§10.3）。補正項を変えても
`.azdic` の形式は変わらない。

### 5.3 可変層の語コスト

可変層の語は語コストを持たない。lattice_core の `META` が持つ既定の語コスト（`名詞-普通名詞-一般` の語コストの
中央値を初期値とする）を使う。ユーザー辞書の `value` と学習の加点は、ラティスのコストに入れない。それらは合成の
段階で、別の候補の score として効く。

## 6. N-best の探索

### 6.1 前向き Viterbi

状態を（終了位置, 右 ID）とし、BOS から各状態への最小コストを求める。同じ状態に入る経路は、それより先の
コストが同じになるので、最小の 1 本だけを残せば 1 位の経路は正しい。区切りの制約（§7.3）があるときは、
状態に最後の語の種別（接頭辞・未知語・その他の 3 値）を足し、制約を遷移の可否として掛けたまま前向きの計算をする。

### 6.2 後ろ向き A*

EOS から BOS へ向けて経路を伸ばし、前向きの最小コストを残りの見積もりに使う。前向きの計算と後ろ向きの
展開が同じ状態と同じ遷移の可否を使うので、見積もりは制約の有無にかかわらず実際のコストと一致し、取り出す順が
経路コストの順になる。表層が同じ経路は畳み、表層の異なる経路が N 件そろうまで続ける。取り出しの回数は 64 × N を
上限とする。

### 6.3 同点の扱い

経路コストが同じ経路は、表層の UTF-8 バイト列、次に区切りの位置の列で順序を決める。M52 の baseline は
同じ入力で同じ順位を返すことを求める（`docs/conversion-quality-benchmark-spec.md` §14.2）。

### 6.4 打ち切り

`ConversionContext::cancel` が立つか `deadline` に達したら、前向き Viterbi が終わっていればその 1 位だけを返す。
終わっていなければ何も返さない。

## 7. 区切りの制約と文節

### 7.1 文節のまとめ

経路の語の列を、自立語にその後ろの付属語を付けたまとまりに分ける。

- 付属語: 品詞 1 が `助詞`・`助動詞`・`接尾辞` の語、または品詞 2 が `非自立可能` の語。
- 接頭辞（品詞 1 が `接頭辞`）は、後ろの自立語と同じ文節に入れる。
- 未知語ノードは自立語として扱う。続く未知語ノードは同じ文節にまとめる。

語の品詞はノードの品詞（§4.2）とする。`CONN` の変種は `attr` の品詞の添字で `META.conn_pos_table`（品詞 1〜4 と
活用型・活用形）を指し、変種の無い語は `ENTS.pos_id` で `META.pos_table` を指す。品詞を持たない可変層の語は
自立語とする。

### 7.2 結果型

ラティスの部品は、`Candidate` とは別の内部の結果型を返す。結果型は経路ごとに、語の列（読みの区間、表層、
左 ID、右 ID、品詞の index）と、文節の区間の列を持つ。エンジンは結果型から `Candidate` を作り、§3.4 の値を入れる。
`Candidate` に文節のフィールドは足さない。

### 7.3 区切りの制約

探索は制約を受け取れる。制約は、読みの code point 位置の集合 2 つからなる。

- 区切る位置: 文節の境界がここに必ず来る。
- 区切らない位置: 文節の境界がここに来てはならない。

位置 p が文節の境界になるのは、p が語の境目で、p から始まる語が自立語・接頭辞・未知語のどれかであり、
かつ p で終わる語が接頭辞でなく、両側がともに未知語でもないときである（§7.1）。この判定は、遷移の両端の語と
状態が持つ最後の語の種別（§6.1）だけで決まる。区切る位置をまたぐノードと、区切る位置で境界にならない遷移、
区切らない位置で境界になる遷移を探索から外す。

区切りを指定した再変換は、この制約付きの探索を供給元にする。どの操作でどの制約を作るかは、文節の伸縮の spec が
決める。

### 7.4 SegmentPos との対応

文節の末尾の語と先頭の自立語の品詞は、lattice_core の品詞名の表を通して `SegmentPos`
（`core/include/azookey/core/SegmentPos.h`）へ写せる。`docs/dynamic-punctuation-spec.md` §7.2.2 の対応表は
IPADIC 系の品詞名で書かれている。ラティスの品詞は UniDic 系の名前（`名詞-普通名詞-一般`、`動詞-一般` など）なので、
同表に UniDic 系の名前を足す改訂は同 spec が持つ。

## 8. 連接情報と ID 空間

### 8.1 入手元とライセンス

連接行列は SudachiDict の `matrix.def` を使う。語彙（`small_lex` と `core_lex`）と同じ ID 空間に属するためである。
SudachiDict の上流の `LEGAL` は、`matrix.def.zip` を UniDic の一部と明記している。帰属は、既存の
`dictbuild/notices/sudachidict-legal.txt`（UniDic の BSD-3-Clause 全文を含む）と Apache-2.0 の連鎖で足り、
`docs/licensing-policy.md` の同梱可能な範囲に入る。BSD-3-Clause の第 3 条（名称による推奨の禁止）を守る。

`matrix.def.zip` は SudachiDict の版ごとの置き場ではなく、raw 配布の直下に 1 本だけある。そのため版ではなく
SHA256 で固定する。取得経路、固定値、`THIRD_PARTY_LICENSES` と `META` への記載は
`docs/azdic-physical-layer-spec.md` §15.12 が定める。

### 8.2 lattice_core

lattice_core は `.azdic` v1 の器を使う辞書ファイル `lattice_core.azdic` で、同梱辞書の `dict` ディレクトリに
置く。`DictionaryStore` の層としては登録しない。ラティスだけが読むので、かなだけの語が完全一致・予測・`Contains` の
結果に入らない。持つものは次のとおりとする。

- かなだけの語（表層が読みと同じひらがなの語）の `ENTS` と `CONN`。`sudachi_lexicon` が取り込まない語である
  （`docs/azdic-physical-layer-spec.md` §15.12）。
- 連接行列の section `CMAT`。ID を詰めた密な行列と、元の ID から詰めた添字への表を持つ。
- `META` 直下の `connection_space`（§8.4）と `conn_pos_table`（`CONN` の `attr` の品詞の添字が指す品詞の文字列表）。
- `META` の `lattice` オブジェクト: 品詞の表（品詞 1〜4 と活用型・活用形の組から代表の左 ID・右 ID へ）、品詞名の表、
  補正項の値（P_kata_variant・P_kata_own・P_node・P_unk を別々に。§5.2）と C の値（§3.4）、可変層の既定の
  語コスト（§5.3）。

### 8.3 ID の無い語

連接 ID を持たない語には、品詞の文字列から代表の ID を与える。

| 語の出どころ | 品詞の文字列 | 与える ID |
|---|---|---|
| `CONN` を持たない静的層（`base_lexicon`、`technical_terms_lexicon` など） | `META.pos_table` の品詞（UniDic 系） | 品詞の表で一致する組の代表 ID。無ければ `名詞-普通名詞-一般` |
| `neologd_lexicon` | mecab-ipadic 系の品詞 | 固有名詞は人名・地名・組織名の区分を保って UniDic 系へ写す。それ以外は `名詞-普通名詞-一般` |
| 可変層 | 持たない | `名詞-普通名詞-一般` |

代表 ID は、SudachiDict の語彙でその品詞に最も多く現れる（左 ID, 右 ID）の組とする。ID は品詞と活用型・活用形の組より
細かく分かれるので、代表 ID は一意な対応ではない。ユーザー辞書の `cid` / `mid`（`learning/include/azookey/learning/UserDictionary.h`）
は別の ID 空間（Mozc 系）に属するので、ラティスの ID として読まない。ユーザー辞書の品詞から ID を引く規則は、
ユーザー辞書の品詞と活用を定める spec が、品詞の表を使って定める。

### 8.4 ID 空間の識別

lattice_core と各層の `META` は `connection_space` を持つ。値は `sudachidict-matrix:` に `matrix.def.zip` の SHA256 を
続けた文字列とする。ラティスは、lattice_core と値が一致する層の `CONN` だけを使う。連接行列の入手元を替えるとき
（§8.5）は、この値を変えて全層を作り直す。

### 8.5 前提が崩れたときの代案

M52 のモデル無効構成で、ラティスが受け入れ条件の top1 と CER に届かない場合は、次の順で検討する。

1. ラティスを候補の生成に限り、順位はモデルと NLL 再ランクに任せる（段階 2 を主にする）。モデルを通らない経路では、
   N-best を辞書の完全一致の候補より下の帯に置く。
2. 変換用の語コストを推定し直す。コーパスの来歴とライセンスを `docs/licensing-policy.md` の手順で審査する。
3. 連接情報の入手元を替える（Mozc 系の連接表、AzooKeyKanaKanjiConverter の辞書と連接データなど）。どちらも採否の審査が要る。
   形式は §8.4 の `connection_space` で入手元を区別するので、`CONN` と `CMAT` の配置は変えずに済む。

## 9. 性能とメモリの予算

| 項目 | 予算 |
|---|---|
| 構築と N-best（N ≤ 16、読み 16 code point、warm、Release、1 スレッド）の p95 | 5 ms 以下 |
| 同（読み 32 code point）の p95 | 15 ms 以下 |
| lattice_core のロード（mmap とヘッダの検証） | 20 ms 以下 |
| ラティスによる常駐 RSS の増分（連接行列、`CONN`、lattice_core のページイン） | 30 MB 以下 |

RSS の 30 MB は、`docs/azdic-physical-layer-spec.md` §15.9 の「常駐 RSS 増分 40 MB 以下」とは別の枠とする。連接行列は
ランダムに読むので、ほぼ全ページが常駐する前提で見積もる。同梱物のサイズは、lattice_core と `CONN` を含めて
§15.9 の「同梱 `.azdic` 合計 120 MB 以下」に収める。

予算を超えたら、次の順で縮める。

1. 連接行列で内容が同じ行と列を 1 つにまとめる（`CMAT` の添字表が同じ添字を指すだけで、配置は変わらない）。
2. 連接コストを 8 bit に量子化する。`CMAT` の配置が変わるので、`docs/azdic-physical-layer-spec.md` §15.5 を
   改訂してから行う。

構築と探索の時間は `bench/` のマイクロベンチで測る。変換全体の遅延は M52 の `latency_*` で見る
（`docs/conversion-quality-benchmark-spec.md` §6.3）。

## 10. モデル無効構成の測定

### 10.1 構成

M52 のベンチを、モデルを指定せず、同梱の辞書を読ませて回す。ラティスの有無だけを変えた 2 つの構成を比べる。

| 構成 | ラティス | 共通の指定 |
|---|---|---|
| A | 無効（`--lattice off`） | `--eval bench/data/kana_kanji_eval.jsonl --dict-dir <同梱辞書> --backend cpu --typo-mode off --iterations 30` |
| B | 有効（`--lattice on`） | 同上 |

モデルを指定しないので `--nll` は付けない（`--nll` には `--model` が要る）。B で lattice_core を読めないときは、
ベンチはエラーで止まる（A と同じ結果のまま測定が通らないようにする）。

モデルを通る経路への影響は、同じ構成に `--model` を足した 2 つ（`--lattice off` と `--lattice on`）で比べる。
段階 1 はモデルが正常に返した要求の候補を変えないので、差はモデルが落ちた要求だけから生じる。

CLI の正典は `docs/conversion-quality-benchmark-spec.md` §7、baseline の互換キーは同 §14.1 である。

### 10.2 指標

`top1_accuracy`、`exact_match_rate`、`cer`、`top5_accuracy`、`latency_p95_ms`、`latency_p99_ms` を A と B で並べる。
`top5_accuracy` は N ≥ 5 で採る。あわせて、`expected` がカタカナを含むケースだけの `top1_accuracy` を A と B で並べる。
カタカナ表記の罰金（§5.2）が外来語を壊していないかを、この部分集合で見る。受け入れの閾値は
`plans/windows-port-roadmap.md` M65 が定める。

`bench/data/kana_kanji_eval.jsonl` には、`expected` がカタカナを含むケースがごく少ない。そのため、構成 A の
baseline を採る前に、外来語を含む文のケースを 30 件以上足す。ケースは自作（`provenance` は `authored`）とし、
置き場所とカテゴリは `docs/conversion-quality-benchmark-spec.md` §4・§5・§11 に、出典とライセンスは同 §13 と
`bench/data/DATA-LICENSE.md` に従う。ケースを足すと `eval_dataset_sha256` が変わるので、A と B は足した後の
同じデータで採る。

### 10.3 補正項の校正

補正項（§5.2）と C（§3.4）は、評価ケースの半分で選ぶ。半分は、ケースの `id` を UTF-8 で SHA-256 した先頭バイトが
偶数のケースとする。受け入れの判定は全件で行い、校正に使った半分と残りの半分の値も添える。P_kata_variant と
P_kata_own は別々に校正する。校正の結果は
lattice_core の `META` に入るので、`dictionary_bundle_sha256` が変わり、baseline は採り直しになる
（`docs/conversion-quality-benchmark-spec.md` §14.1）。

### 10.4 定期採取

モデル無効構成を品質 workflow の定期採取で回すには、同梱辞書の生成を workflow に足す必要がある
（`docs/conversion-quality-benchmark-spec.md` §10）。定期採取に入っていない構成は、`dictbuild/build_bundled.py` の
`--cache` に置いた上流アーカイブから手元で辞書を作って測り、結果を PR に記録する。

## 11. テスト計画

| 対象 | 試験 |
|---|---|
| core のラティス部品 | 小さな語彙と行列の fixture で、1 位の経路が全列挙の最小と一致する。N-best がコスト順で、表層が重複しない。同点の順序が決まっている。区切りの制約に合わない経路が出ない。未知語ノードだけの読みでも経路が出る。cancel と deadline で §6.4 のとおり返る |
| `.azdic` の読み出し | `CONN` を持つファイルを、`CONN` を知らない reader が読める。`CONN` と `CMAT` の破損で、その層またはラティスだけが無効になる |
| dictbuild | 連接の変種が `CONN` に残り、同じ入力から 2 回作った lattice_core がバイト単位で一致する |
| エンジン | §3.2 の各条件で N-best が足され、それ以外では足されない。score、source、debug_info が §3.4 のとおり。§3.6 の各縮退で従来の候補が返る |
| ベンチ | §10.1 の A と B が同じ入力で同じ順位を返す（`docs/conversion-quality-benchmark-spec.md` §14.2） |

試験の一覧の正典は `docs/test-inventory.md` である。

## 12. 受け入れ条件

M65 の受け入れ条件は `plans/windows-port-roadmap.md` M65 が定める。測定の構成と校正の手順は §10 に従う。
