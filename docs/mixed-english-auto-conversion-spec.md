# 日英混在ローマ字入力の自動変換 仕様（追加機能 / M63）

本書は azooKey-Desktop Windows 版の「日英混在ローマ字入力の自動変換（Mixed Japanese-English
Auto Conversion）」を定める。`plans/windows-port-roadmap.md` の M63 が本書を参照する。本書は
機能仕様（分割の規則・IPC payload・設定項目・ユーザー可視挙動）の正典であり、マイルストーンの
定義・受け入れ条件・リスクは roadmap の M63、進捗と状態は Linear が持つ。

関連 spec:

- `docs/inline-english-candidate-spec.md` … M60。生ローマ字バッファ（§4.1）、英語意図シグナル
  （§4.2）、英単語辞書の形式（§4.4〜§4.7）、English 学習チャネル（§5・§6.4）。本書はこれらを
  再利用し、M60 の設定キーの意味を変えない。
- `docs/zenzai-inference-spec.md` … Zenzai のプロンプト（§3）と性能予算（§8）。
- `docs/romaji-batch-conversion-spec.md` … `CommitSegmentsObservation`（§6.4）。
- `docs/dev-infrastructure-spec.md` … ライブ変換の timeout（§8.5.2、150ms）。
- `docs/typo-correction-learning-spec.md` … M35 / M55 の打ち間違い補正。
- `docs/conversion-quality-benchmark-spec.md` … M52 の評価データと品質指標。
- `docs/mixed-english-input-feasibility-report.md` … 実現可能性と先行事例の調査記録（参考。正典ではない）。

## 1. 目的

日本語入力モードのまま、英単語を含むローマ字の文を打つと、日本語部分はかな漢字に、英単語部分は
英字に打ち分けて、ライブ変換と第 1 候補に出す。

```text
kyouhagithubnipushsita → 今日はGitHubにpushした
```

M60 は英単語を「追加候補」として注入するだけで、第 1 候補を英単語へ置き換えず、ライブ変換へも
注入しない（M60 §4.1・§4.3）。M63 は、M63 の設定が ON のときに限ってこの 2 点を変える。

## 2. スコープと非目標

- **段階**:
  - **M63-A（1 語）**: composition 全体が 1 語のとき、英語の確度が高ければライブ変換と第 1 候補を
    英字にする。低ければ日本語を第 1 候補、英字を第 2 候補にする。
  - **M63-B（文中混在）**: 生ローマ字を日本語区間と英語区間に分割し、区間ごとに打ち分ける。
- **非目標**:
  - 空白を含む複数語の英文（M60 §9 の「連続英文タイプ」）。
  - 一括変換（M58、`QueryBatchConversion`）での分割。一括変換は M60 §4.1.1 の 1 文節の扱いのまま
    とする（§13）。
  - カスタムローマ字表（`inputStyle=custom`）での動作（§5）。
  - 英語区間の前後への空白の自動挿入（§8.4）。
  - 英語のスペル補正・補完。
- **既定 OFF。** 設定が OFF のとき、候補生成・ライブ変換・rerank・確定・学習を一切変えない。

## 3. 設計原則

1. **判別と分割は Zenzai の外で決める。** 分割器（§6）が、M60 の英語意図シグナル（H1）と、
   生ローマ字の上の混合ラティス（H2）で、英語区間を決める。Zenzai は言語判別に使わない。
2. **英語区間は、辞書の大小の英字として Zenzai の入力に入れる。** Zenzai には日本語区間だけを
   変換させ、英語区間はプロンプトの `[IN]` に英字のまま置く（§7）。Zenzai は `[IN]` の英字を
   大小も含めて写し、大小を補わない。表層の大小は辞書と打鍵が決める。
3. **NllScorer は言語判別に使わない。** NllScorer の採点は、文脈より `[IN]` に書いた形に強く従う。
   日英の表層の比較でも、ローマ字として読める英単語（`kite` / 来て）の判別でも、`[IN]` に
   入れた側が勝つため、判別の根拠にならない。`docs/neural-reranker-spec.md` Track B（§B1・§B5）の
   適用範囲は変えない。
4. **打鍵ごとの推論は 1 回。** 分割の候補ごとに Zenzai を回さない（§10）。
5. **日本語を奪わない。** ローマ字として読める英単語は既定で日本語として扱い、英字は第 2 候補に置く。
   自動で英字にするのは、確度の高い区間（§8.1）に限る。

規則の根拠は、実 pin モデル（zenz-v3.2-small Q5_K_M）による測定である。測定値と条件は Linear の
DEV-1592 に記録してある。

## 4. 用語

| 用語 | 定義 |
|---|---|
| 生ローマ字 `r` | TIP が保持する打鍵どおりの印字可能 ASCII（Shift の大文字を保つ）。M60 §4.1 のバッファと同じもの |
| 区間 | `r` を連続する部分に分けたもの。日本語区間（J）と英語区間（E）がある |
| J 区間のかな | J 区間の生ローマ字を、内蔵のローマ字表（`RomajiKanaConverter::ConvertForCommit`）でかなにしたもの |
| E 区間の表層 | E 区間の英字の表層。決め方は §6.4 |
| 高確度の E 区間 | ライブ変換と第 1 候補で英字にする区間（§8.1） |
| 低確度の E 区間 | 第 1 候補では日本語として扱い、英字を後ろの候補（M63-A では第 2 候補、M63-B では §8.3 の 3）に置く区間（§8.1） |

## 5. 有効化の条件

M63 は、次のすべてを満たす composition でだけ働く。どれかを欠くときは、その composition は
M63 の設定が OFF のときと同じ挙動になる。

- 設定 `mixedEnglishAutoConversion`（§14）が true で、かつ M60 の `inlineEnglishCandidates` が
  true である。M63 は M60 の生ローマ字バッファを使うため、M60 の設定が OFF のときは働かない。
  TIP の生ローマ字の保持条件（M60 §4.1）は変えない。
- Handshake 応答の capabilities に `mixed_english` と、M60 の `english_candidates` の両方がある
  （§9.3）。生ローマ字は M60 の条件（M60 §4.1）で送るため、`english_candidates` が無い Host には
  生ローマ字を送らない。
- カスタムローマ字表が有効でない。Host はカスタム表を持たず、J 区間のかなを作れないため
  （M60 §4.1.1 と同じ制約）。
- 入力欄が secure でない。secure の入力では生ローマ字を送らない。
- 生ローマ字を内蔵のローマ字表でかなにした結果が、送る読み（`reading` / `kana`）と一致する。
  一致しないとき（Backspace で消したかなの打鍵、記号、Unicode 入力、読みの補正など）は、M60 §4.1
  と同じく生ローマ字を送らない。
- 生ローマ字の長さが 64 文字以下である（§10）。

## 6. 分割器

分割器は `core/` に置き、Host が呼ぶ。TIP は分割しない。辞書、English 学習チャネル、Zenzai が
Host にあり、分割と変換を 1 か所で行えるためである。

### 6.1 入力と出力

- 入力: 生ローマ字 `r`、英単語辞書の参照（M60 §4.4 の照合: `lower` キーから、頻度降順の表層と
  flags を得る）、ローマ字かな変換（内蔵の表）。
- 出力: 区間の列。各区間は、種類（J / E）、`r` の中の範囲、J 区間ならかな、E 区間なら表層と
  確度（高 / 低）を持つ。分割の候補は上位 8 件まで返し、先頭が採用する分割である。

英単語辞書が無いとき（`inlineEnglishDictionary` が false、ファイルが無い・読めない）は、E 区間を
張れないので文中の分割を行わない。この場合は composition 全体を 1 区間とみなす M63-A の判定（§8.2）
だけを行う。composition の先頭の文字を Shift で大文字にして打ったときは、composition 全体を 1 つの
E 区間（表層は打鍵どおり。§6.4）とし、それ以外は全体を 1 つの J 区間とする。この E 区間も、
M60 §4.2 のゲーティング（`inlineEnglishMinLength` など）を満たさないときは英字にせず、J 区間として
扱う（§8.2）。

### 6.2 混合ラティス

`lower(r)` の各位置の間に、次の 2 種類の辺を張る。

- **J の辺**: 任意の部分列。辺のかなは、その部分列だけを内蔵のローマ字表でかなにしたものとする。
  J の辺は連続させない（隣り合う J の部分列は 1 つの J の辺として扱う）。ローマ字かな変換は
  前後の文字に依存するため、同じ範囲を分けたかなと、まとめたかなは一致しないことがある。
- **E の辺**: 3 文字以上（先頭の文字を Shift で大文字にして打った部分列は 2 文字以上）で、英単語辞書に
  `lower` キーが一致する部分列。長さ 2 の辞書語（`ky`、`as`、`sh` など）がローマ字の途中で E の辺に
  なるのを避けるため、文中では 3 文字からとする。composition 全体が 1 語のときの判定（§8.2）の
  最小長は M60 の `inlineEnglishMinLength` のままとする。

経路のコストは辺のコストの和とし、コストの小さい順に上位 8 経路を得る。

composition 全体が辞書語のときは、ラティスを使わずに §8.2 の判定を行う。ラティスの先頭の経路に
E 区間が 1 つも無く、composition の先頭の文字が Shift の大文字のときは、§6.1 のとおり全体を
1 つの E 区間とする。

### 6.3 辺のコスト

| 辺 | コストの項 |
|---|---|
| J | かなに残った ASCII 英字 1 文字ごとに大きな罰（例 10）。不自然なかな 1 文字ごとに罰（例 1.5）。Shift で打った大文字 1 文字ごとに罰（例 2.0） |
| E の直後の J | J の辺の項に加えて、部分列が母音の文字から始まるときに罰（例 1.0）、`ん` から始まるときに不自然なかなの罰 |
| E | 基本のコスト（例 1.0）。長い語ほど小さく（例 −0.05 × 長さ、下限あり）。部分列がかなとして読める（ASCII が残らない）ときに罰（例 2.0）。ただし先頭の文字を Shift で打った部分列には読める罰を掛けない |

- **E の直後の J**: E の辺が後ろのかなの子音を取り込むと（`pull` + `sita` が `pulls` + `ita`、
  `chrome` + `de` が `chromed` + `e`）、続く J の部分列は母音から始まる。`tosca` + `nnohanasi` のように
  `ん` から始まる J も、日本語の語の始まりとして不自然である。どちらも E の辺の切れ目が誤っている
  信号として罰を掛ける。
- **Shift の大文字**: 利用者が大文字で打った文字は英語の意図の信号なので、J の辺に含まれると罰を
  掛け、その文字から始まる E の辺には読める罰を掛けない（`Sitewomite` → `Site` + `をみて`）。

- **不自然なかな**: 前のかなと組み合わさらない小書きの母音（`ぁぃぅぇぉ`）と小書きの `ゃゅょ`、
  および `ゔ`。`love` → `ぉゔぇ`、`script` + `wo` → `…とぉ…` のように、英単語をローマ字かな変換
  したときに現れる。`l` / `q` / `v` / `x` は内蔵のローマ字表で正規の入力（`ltu` → `っ`、
  `vaiorin` → `ゔぁいおりん`）なので、文字の種類だけでは英語と判定しない。不自然なかなの項は、
  変換結果のかなの並びで判定する。
- 例に挙げた重みは初期値であり、正典の値は実装 PR が品質ゲート（§15.2）を通る値として
  `core/` の定数に定める。
- かなとして読める部分列の E の辺に罰を掛けるので、ローマ字として読める英単語（`kite`、`same`、
  `made`、`take`）は既定で J になる（ユーザー可視挙動の要求 3）。

### 6.4 E 区間の表層

- 区間の生ローマ字に大文字が含まれる（Shift で打った）ときは、打鍵どおりの形を表層にする。
- それ以外は、英単語辞書の表層を頻度降順で引き、先頭を表層にする（`github` → `GitHub`）。
  `proper` / `acronym` の flags の扱いは M60 §4.4 と同じ。
- 辞書に複数の表層があるとき、残りは候補一覧の別候補になる（§8.3）。

## 7. Zenzai への入力

- Host は採用した分割から読みを組み立てる。J 区間はかな、E 区間は表層（§6.4）を、`r` の順に
  連結する（例 `きょうはGitHubにpushした`）。この読みを Zenzai の変換に渡す。プロンプトの形
  （`docs/zenzai-inference-spec.md` §3.2）は変えない。
- 本節の読みの組み立てと次の `DropMixedKanaAsciiModelCandidates` の例外は、J 区間と E 区間が混在する
  M63-B から効く。M63-A（§8.2）の 1 語の composition では、英字の候補は表層そのもので、日本語の
  変換は M63 が OFF のときと同じかなの読みで行うため、Zenzai の読みに E 区間は入らない。
- `DropMixedKanaAsciiModelCandidates`（M60 §4.3.1）は、ASCII 英字の有無を判定するときに M63 が
  入れた E 区間を除く。M63 が入れた英字を含む読みでは、かなと英字の混在した候補を落とさない。
  読みに残った ASCII（J 区間のかなに残った英字）による落とす規則は変えない。
- `LiveConversionChunker` の Verbatim 分類は M63 では使わない。E 区間を Zenzai の外に出すと、
  日本語区間の変換から英語区間の文脈が消えるため、E 区間も `[IN]` に入れる。

## 8. 順位規則

### 8.1 確度

E 区間は、次のどちらかを満たすときに高確度とし、それ以外は低確度とする。

- (i) 区間の先頭の文字を Shift で大文字にして打った（M60 の `s_case`）。
- (ii) 区間の生ローマ字を内蔵のローマ字表でかなにすると ASCII 英字が残り（M60 の `s_nonkana`）、
  かつ英単語辞書に一致する。

ローマ字として読める英単語は (ii) を満たさないので、Shift で打たない限り低確度である。

学習による抑制: (ii) で高確度になる区間でも、English チャネルにその区間のキーの負の観測（§11）が
あるときは低確度として扱う。このとき、その区間は日本語のまま第 1 候補に入り、英字は §8.3 の 3
（その区間だけを英字にした候補）になる。抑制は M63-B で入れ、M63-A（§8.2）は学習で確度を変えない。
キーは M60 §5 と同じく、区間の生ローマ字を小文字に畳んだもの（`lower`）とする。抑制は、区間の
生ローマ字が大文字を 1 つも含まないときだけ掛ける。(i)（Shift 先頭大文字）を含め、大文字を含む
打鍵の区間は学習で変えない。

### 8.2 M63-A（composition 全体が 1 語）

- M63-A は、composition 全体の生ローマ字が英単語辞書に一致するか、先頭の文字を Shift で大文字にして
  打ったとき、全体を 1 つの E 区間とし、§8.1 で確度を決める。§6.3 の罰（かなとして読める部分列の
  E の辺）は掛けない。§6.3 は M63-B の混合ラティスの規則である。そのため `kite` も E 区間になり、
  低確度として次の規則で英字が第 2 候補に入る。
- その区間が高確度なら、ライブ変換と第 1 候補をその表層にする。日本語の変換を第 2 候補以降に置く。
  ライブ変換では Zenzai を呼ばず、表層をそのまま返す（`segments` は E 区間 1 つ。§9.2）。
  M59 の句読点の規則（`docs/dynamic-punctuation-spec.md`）は M63 が OFF のときと同じく掛ける。
- 低確度なら、日本語の変換を第 1 候補、英字を第 2 候補にする。
- 英単語辞書が無いときは (ii) を満たせないので、(i) だけを高確度とする。
- 英単語候補の生成条件（M60 §4.2 のゲーティング）を満たさない入力（例 `kyouha`）には英字の候補を
  足さない。
- §8.1 の学習による抑制と §11 の負の観測は M63-B で入れる。M63-A では、英字の確定を M60 §6.4 の
  English チャネルの学習に流すだけとし、英字の候補の確度を学習で変えない。

### 8.3 M63-B（文中混在）

- ライブ変換と第 1 候補は、高確度の E 区間だけを英字にし、低確度の E 区間は J として扱った読みの
  変換とする。
- 候補一覧（`QueryCandidates`、`live = false`）は次の順に並べる。
  1. 第 1 候補（上の規則）。
  2. すべての区間を J として扱った変換（第 1 候補が英字を含むとき）。
  3. 低確度の E 区間を 1 つずつ英字にした変換（最大 3 件）。
  4. 以降は M60 §4.3 の英単語候補と、通常の候補。
- 1〜3 の候補は、それぞれの区間の範囲と種類を候補ごとの `segments`（§9.2）に持つ。区間単位の
  選び直しは、1 区間だけが異なるこれらの候補で提供する。候補ウィンドウで区間に焦点を移して選び直す UI は本書の対象外とする。

### 8.4 空白

英語区間の前後に空白を入れない（`今日はGitHubにpushした`）。英単語の後に半角空白を入れる設定は
本書の対象外とする。

### 8.5 M60 §4.3 との関係

M60 §4.3 の「第 1 候補を英単語へ置き換えない」は、M63 の設定が OFF のときの規則である。M63 の設定が
ON の composition では、本章の規則が優先する。順位安定化（同じ `r` では順位を固定する）は M63 でも
同じとする。

## 9. IPC

payload の正典は `ipc/include/azookey/ipc/Payloads.h` と `ipc/src/Payloads.cpp` で、本章の
フィールドはそこに定義がある。どのフィールドも省略可能で、既定値（空・false・0。
`secure` は true）のときは wire に出さない。

### 9.1 Request

| メッセージ | 追加するフィールド | 内容 |
|---|---|---|
| `QueryLiveConversionRequest` | `raw_romaji`（string） | 生ローマ字。§5 の条件を満たすときだけ送る |
| 同上 | `mixed_english`（bool） | TIP の M63 の設定（§5 の条件を満たすとき true） |
| 同上 | `secure`（bool） | 入力欄が secure か（§9.4）。省略時と bool 以外の値は true（secure）。false のときだけ送る |
| `QueryCandidatesRequest` | `mixed_english`（bool） | 同上。生ローマ字は M60 の `raw_romaji` を使う |
| `QueryBatchConversionRequest` | `secure`（bool） | 入力欄が secure か（§9.4）。省略時と bool 以外の値は true（secure）。false のときだけ送る |

`QueryCandidatesRequest` は `secure` を既に持つ（省略時は true）。
`QueryBatchConversionRequest.secure` は M63 の処理ではなく、M60 の一括変換の英単語候補
（`docs/inline-english-candidate-spec.md` §4.1.1）を secure の入力から守るために足す。一括変換に
`mixed_english` は加えない（§13）。

`QueryCandidatesRequest` で `mixed_english` を true にするときは、M60 §4.1 の条件で `raw_romaji` と
`english_candidates` も送る。`live = true` の `QueryCandidates` には M60 と同じく載せない。

### 9.2 Response

- `LiveSegment` に `tag`（uint8、`CandidateTag` の値）を加える。E 区間は `English`（4）、J 区間は
  `None`（0）とする。`None` のときは、`CandidateField.tag` と同じく wire に出さない（M63 を知らない
  側との往復で形を変えないため）。E 区間の `reading` は区間の生ローマ字とする。
- `QueryLiveConversionResponse.segments` は、`mixed_english` が true で、結果に E 区間を 1 つ以上
  含むとき、全区間を載せる。M59 の句読点の `segments` と同じ配列に載せ、句読点の印
  （`auto_punctuation`）と区間の印（`tag`）は独立に持つ。
- `CandidateField` に、省略可能な `segments`（`LiveSegment` の配列。offset はその候補の `surface`
  の UTF-16 単位）を加える。Host は、§8.2 で E 区間を英字にした第 1 候補と、§8.3 の 1〜3 の候補に
  だけ載せ、それ以外の候補と、
  `mixed_english` が false の応答では出さない。`QueryCandidatesResponse.segments`（`candidates[0]`
  の句読点の区間）の意味は変えない。
- 応答の `surface` / 候補の表層は、区間の表層を連結したものである。

### 9.3 capability と互換

- Host は M63 に対応する版で、Handshake 応答の capabilities に `mixed_english` を入れる。
- TIP は capabilities に `mixed_english` が無い Host へは、§9.1 のフィールドを送らない。
  `mixed_english` があっても M60 の `english_candidates` が無い Host へは、`raw_romaji` を送らない
  （§5）。
- 新しい Host と旧 TIP: 旧 TIP はフィールドを送らないので、Host は M63 を行わず、M63 の設定が
  OFF のときと同じ挙動になる。
- 新しい TIP と旧 Host: capability が無いので TIP は送らない。送ってしまった場合も、旧 Host の
  パーサはキーで読んで未知のフィールドを無視するので、M63 を知らない版の挙動のままになる。
- `mixed_english` が true でも `raw_romaji` が空なら、Host は M63 を行わない。

### 9.4 プライバシー

生ローマ字はユーザー入力そのものである。secure の扱いの正典は
`docs/privacy-and-secure-input-spec.md` §5 である。

- TIP は secure の入力では、どの要求にも `raw_romaji` と `mixed_english` を載せない（本書 §5）。
  一括変換（`QueryBatchConversionRequest`）も含む。一括変換の TIP は生ローマ字で読みを分割するが、
  secure の入力では送る要求から `raw_romaji` を除き、かなの `reading` だけを送る。
- Host の防御:
  - `QueryCandidatesRequest.secure` と `QueryLiveConversionRequest.secure` が true（省略時を含む）の
    要求では、`mixed_english` を無視し、`raw_romaji` を M63 の処理に使わない。`QueryCandidates` では
    M60 の英単語候補も作らない（`docs/inline-english-candidate-spec.md` §4.1）。
  - `QueryBatchConversionRequest.secure` が true（省略時を含む）の要求では、M60 の英単語候補を
    作らない（同 §4.1.1・§6.7）。
  - `mixed_english` が false の要求では、`raw_romaji` を M63 の処理に使わない。M60 §4 の用途は
    変えない。
  - Host は、M63 の処理で受け取った生ローマ字を、その要求の処理のほかの経路（キャッシュの鍵、
    診断の出力、別の要求）に使わない。
- TIP は、secure でない入力の要求に `secure = false` を載せる。`secure` を送らない旧 TIP の要求は
  secure として扱われる。そのため、一括変換の `secure` を判定する Host と旧 TIP の組み合わせでは、
  一括変換に M60 の英単語候補が出ない。ライブ変換は、旧 TIP が `raw_romaji` を送らないので影響を受けない。
- 生ローマ字と区間の表層は、ログとダンプに平文で出さない。`RuntimeLogger` の redaction
  （`docs/dev-infrastructure-spec.md` §7.6。secure 中は `docs/privacy-and-secure-input-spec.md` §8）で
  本文系のフィールドとして扱い、出すのは件数と長さだけにする。

## 10. レイテンシと打ち切り

- 打鍵ごとのライブ変換では、分割器を 1 回、Zenzai の変換を 1 回だけ行う。分割の候補ごとに
  Zenzai を回す方式（H3）と、分割に NllScorer を使う方式は採らない。M63-A で composition 全体が
  高確度の E 区間のときは、Zenzai の変換を行わない（§8.2）。
- ライブ変換の応答は `docs/dev-infrastructure-spec.md` §8.5.2 の 150ms に収める。Zenzai の 1 回の
  予算は `docs/zenzai-inference-spec.md` §8 のとおりで、M63 は増やさない。
- 分割器の上限: 生ローマ字は 64 文字まで、上位の経路は 8 件まで。上限を超えた composition、
  分割器が失敗したとき、辞書の照合が失敗したときは、その要求を M63 の設定が OFF のときと同じ
  挙動で処理する。
- 候補一覧（§8.3）で追加の変換（すべて J、低確度の区間を英字にしたもの）を作るのは
  `live = false` の要求だけとし、打鍵ごとの経路には載せない。追加の変換は合計 4 回までとし、
  Zenzai の予算を超えたものは候補一覧から省く。
- 合計時間の打ち切り: 第 1 候補の変換と追加の変換は直列に行う。1 回ずつ Zenzai の予算（600ms）に
  収まっても、合計は候補窓用 `QueryCandidates` の timeout 750ms（`docs/dev-infrastructure-spec.md`
  §8.5.2）を超えうる。Host は要求の受信から 650ms（750ms から、辞書候補・rerank・応答の組み立てと
  IPC の余裕 100ms を引いた値）を、追加の変換の期限とする。
  - 追加の変換は、期限を過ぎたら始めない。
  - 実行中の変換の deadline は、Zenzai の予算と、この期限の早いほうにする。
  - 期限で打ち切った変換と、始めなかった変換は候補一覧から省き、第 1 候補と辞書の候補だけを返す。
- 上限の根拠（実 pin モデル zenz-v3.2-small Q5_K_M、CPU、threads 8、warm。測定値と条件は Linear の
  DEV-1592 のコメント）:
  - 打鍵ごとの変換（`live = true`）は p95 19〜37ms。
  - composition 全体の候補一覧用の変換 1 回は、中央値 50〜91ms、p95 約 95ms（最大 100ms）。
  - 第 1 候補の変換と追加の 4 回を合わせた 5 回は、中央値で約 460ms、p95 で約 480ms。候補窓用の
    `QueryCandidates` の timeout 750ms（`docs/dev-infrastructure-spec.md` §8.5.2）に収まり、1 回ずつは
    Zenzai 1 変換のハード予算 600ms（`docs/zenzai-inference-spec.md` §8.2）に収まる。

## 11. 学習

- 確定は `CommitSegmentsObservation`（`docs/romaji-batch-conversion-spec.md` §6.4）で区間ごとに送る。
  区間は、確定した候補の `segments`（ライブ変換の確定は `QueryLiveConversionResponse.segments`）
  から作る。composition が 1 区間のときは `CommitObservation` を使う。
- E 区間は `ObservedSegment` の `chosen.tag` を `English`、`reading` を区間の生ローマ字とし、Host は
  M60 §6.4 の条件で English チャネルへ振り分ける。J 区間はかな漢字学習へ記録する。
- 第 1 候補で英字だった区間（§8.1 の (ii) で高確度の区間）を、ユーザーが日本語へ選び直して
  確定したときは、English チャネルにその区間のキー（`lower(区間の生ローマ字)`。§8.1）の負の観測を
  記録する。次に同じキーの区間が (ii) を満たしても低確度として扱う（§8.1 の学習による抑制）。
  (i)（Shift 先頭大文字）で高確度だった区間は、選び直しても負の観測にしない。負の観測の記録と
  抑制は M63-B で入れる（§8.2）。
  - TIP は、確定した区間の `ObservedSegment.shown`（1 区間の composition では `CommitObservation`
    の `shown`）に、既存の中身に加えて、第 1 候補で英字だった区間の候補を入れる。この候補は
    `tag` が `English`、`reading` が区間の生ローマ字で、その区間を表す `segments`（E 区間 1 つ。
    §9.2）を持つ。payload に新しいフィールドは足さない。
  - `segments` を持つ候補は M63 の Host だけが作る（§9.2）。M60 の英単語候補は `segments` を
    持たないので、M63 が OFF の composition の確定（M60 の候補窓で英単語を見て日本語を選んだ確定を
    含む）は、負の観測にならない。
  - Host は、次のすべてを満たす区間だけを、そのキーの負の観測として English チャネルへ記録する。
    - `chosen.tag` が `English` でない。
    - `shown` に、`segments` を持ち、M60 §6.4 の生ローマ字の条件を満たす `English` の候補がある。
    - その生ローマ字が大文字を 1 つも含まず、(ii) を満たす。先頭に限らず大文字を含む区間
      （`GitHub`、`gItHub`）は、ユーザーが大小を指定したものとして、負の観測の記録にも §8.1 の
      抑制にも使わない。
  - 負の観測はキーごとに 1 件とし、同じキーの記録は上書きする（冪等）。抑制の後も
    `segments` を持つ候補が `shown` に入り、日本語の確定のたびに同じ判定が成り立つためである。
  - 抑制された区間の英字（§8.1 の抑制で下げた英字の候補）を、大文字を含まない同じキーの打鍵で
    ユーザーが確定したときは、そのキーの負の観測を消す。大文字を含む打鍵（(i) を含む）での英字の
    確定は、負の観測を消さない。
  - English チャネルに負の観測を持たせる形式は、M63 の実装 PR が M60 §5 の形式の上に定める。
- `secure` の入力と、`learning_allowed` が false の確定では、English チャネルの記録（負の観測を
  含む）もかな漢字学習の記録も行わない。判定の正典は `docs/privacy-and-secure-input-spec.md` §5 である。
- English チャネルの記録は、M60 §5 と同じく、かな漢字の候補・予測・逆変換・未知語マイニングの
  いずれにも入らない。

## 12. 打ち間違い補正（M35 / M55）との優先

かなに ASCII 英字が残ることは、英語の信号でも打ち間違いの信号でもある。M63 が働く composition では
分割を先に行い、E 区間は打ち間違い補正の対象にしない。採用した分割が E 区間を含むときは、その
composition の変換に打ち間違い補正を掛けない。補正は読み全体を置き換える（`corrected_reading`）ので、
J 区間のかなだけを補正すると、E 区間の範囲と補正後の読みの対応が崩れるためである。E 区間を含まない
（すべて J の）composition には、M63 が OFF のときと同じく補正を掛ける。

## 13. M58（一括変換）との関係

一括変換は M63 の対象外とする。`QueryBatchConversion` には `mixed_english` を加えず、英単語候補は
M60 §4.1.1 のとおり 1 文節の場合に限る。

## 14. 設定

`settings/mvp-settings.schema.json` は次のキーを持つ（`additionalProperties:false` を維持する）。
schema の正典は `settings/mvp-settings.schema.json` である。

| キー | 型 | 既定 | 説明 |
|---|---|---|---|
| `mixedEnglishAutoConversion` | boolean | `false` | M63: 日英混在のローマ字を区間ごとに打ち分け、確度の高い英単語をライブ変換と第 1 候補で英字にする。`inlineEnglishCandidates` が true のときだけ働く |

- 設定アプリでは `inlineEnglishCandidates` の下に置き、`inlineEnglishCandidates` が false のときは
  無効表示にする。
- TIP は設定ファイルから読み（設定の監視で再読込する。欠落・読めない値は false）、§5 の条件で
  要求に載せる。Host は要求の `mixed_english` で受け取り、Host 側の設定キーは持たない。
- 英単語辞書のキー（`inlineEnglishDictionary`、`inlineEnglishDictionaryPath`）は M60 のものを
  そのまま使う。M60 のキーの意味は変えない。
- 英単語辞書の出典は本書では定めない。本書が依存するのは M60 §4.4 の辞書形式（表層の大小を保ち、
  `lower` キーで引けること、頻度と flags）だけである。

## 15. テスト計画

### 15.1 CTest

| 層 | 内容 |
|---|---|
| core | 分割器: E の辺の張り方、J の辺の連続の禁止、コストの各項（ASCII 残余、不自然なかな、かなとして読める語の罰）、上位 8 件、64 文字の上限、辞書なしの扱い、表層の大小（§6.4）、確度（§8.1） |
| host | mock の Zenzai で、Zenzai に渡る読みが §7 の形になること。`DropMixedKanaAsciiModelCandidates` が M63 の E 区間を除いて判定すること。§8.2・§8.3 の順位。§10 の打ち切りで OFF と同じ挙動になること。§10 の合計時間の期限（650ms）で追加の変換を省くこと。`mixed_english` が false のときに出力が変わらないこと。§11 の負の観測が条件を満たす区間だけに記録され、`segments` を持たない M60 の英単語候補・Shift で打った区間・`secure` / `learning_allowed` が false の確定では記録されないこと。§8.1 の学習による抑制と、英字の確定による解除 |
| ipc | §9 のフィールドの往復。各フィールドを欠いた payload を、M63 を知らない版の payload と同じ形で読めること。`LiveSegment.tag` が `None` のとき、`CandidateField.segments` が空のとき wire に出ないこと |
| tip | §5 の送信条件（capability、secure、カスタム表、生ローマ字とかなの不一致、M60 の設定）。`segments` からの確定時の `CommitSegmentsObservation`（E 区間の `reading` が生ローマ字。日本語へ選び直した区間の `shown` に英字の候補が入ること） |

### 15.2 品質ゲート

- 評価セットは `bench/data/mixed_english_eval.jsonl` である。自作の文だけで作り、M52 の評価データ規約
  （`docs/conversion-quality-benchmark-spec.md` §4、`bench/data/DATA-LICENSE.md`）に従う。混在文、
  日本語だけの文、ローマ字として読める英単語を含む文の 3 種を含める。英単語辞書は同梱の
  `data/english-words.tsv` をパスで参照し、`bench/data/` に写さない。
- 合格の基準:
  - 自動で英字にした E 区間の適合率（英字にした区間のうち、正解でも英語区間である割合）が 95% 以上。
    分割器の先頭の経路の高確度の E 区間を、期待する範囲と表層の両方で照合する。
  - 日本語だけの文で、分割器の先頭の経路に高確度の E 区間が 1 つも無い。
  - 日本語だけの文の変換結果が、M63 の設定が OFF のときと比べて 1 件も劣化しない。
- 前の 2 つは Zenzai に依らない分割器の指標で、CTest `azookey_mixed_english_quality_gate`
  （`azookey_mixed_english_eval`）が CI で確かめる。最後の 1 つは実モデル（Zenzai の実 pin）で
  M63 の ON / OFF の第 1 候補を比べる計測で、CI には載せず、日付つきの計測記録として残す。
- 基準を満たさない分割器では、文中の区間（M63-B）の英字を §8.3 の 3 の候補だけで出し、
  ライブ変換と第 1 候補を英字にしない。このとき、本来は高確度になる区間も低確度として扱い、
  §8.3 の 3（1 区間だけ英字にした候補）に入れる。

## 16. 受け入れ条件

M63 の受け入れ条件は `plans/windows-port-roadmap.md` の M63 が定義する。文中の区間の選び直しは、
§8.3 の候補一覧で提供する。
