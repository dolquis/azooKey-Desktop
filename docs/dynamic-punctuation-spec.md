# 動的自動句読点 仕様（追加機能 / M59）

本書は azooKey-Desktop Windows 版の「動的自動句読点（Dynamic Auto
Punctuation）」機能を定める。`plans/windows-port-roadmap.md` の M59 が本書を
参照する。本書は機能仕様（InputState・IPC payload・設定項目・ユーザー可視挙動）の
正典であり、進捗・状態は持たない（状態の正典は Linear。`AGENTS.md`「Linear 運用
（管制塔）」参照）。

関連 spec:

- `docs/legacy-parity-spec.md` … ライブ変換（§2）。本機能はライブ変換経路を土台にする。
- `docs/rich-features-spec.md` … X-1（ライブ変換のリッチ化）・X-1-2（TypingTempoTracker）。
  挿入安定化に再利用する。X-3（誤変換訂正・Post-Commit Lint）とは目的が異なる（後述 §2）。
- `docs/romaji-batch-conversion-spec.md` … M58-C `batchAutoPunctuation`（一括変換 +
  ai-cleanup 限定の句読点挿入）。本機能（逐次ライブ変換中の動的挿入・削除）とは別経路
  （後述 §2）。
- `docs/tsf-deep-integration-spec.md` … 再変換（M20）・multi-segment。surface に
  読みを持たない句読点が混在するときの再変換境界に影響する（§5）。

## 1. 目的（と背景）

ローマ字でかな漢字変換しながら文章を書くとき、文の区切り（読点 `、`）や文末
（句点 `。`）をユーザーが明示的に打鍵しなくても、IME が**文章の適切な位置へ動的に
句読点を挿入し、文脈の変化に応じて削除・再配置する**。これにより「句読点キーを
意識して打つ」操作を減らし、入力の流れを保つ。

「動的」とは、ライブ変換が打鍵ごとに preedit 全体を再評価するのに合わせ、句読点も
**毎回再計算**することを指す。直前に挿入した読点が、続く入力で文節構造が変われば
自然に消える（= 削除）。

## 1.1 非目標

- **句読点キーの字種設定ではない。** legacy macOS の `Config.PunctuationStyle`
  （`legacy/Core/Sources/Core/Configs/PuntuationConfigItem.swift`）は「`,` / `.` キー
  押下時に `、。` と `，．` のどちらの字を出すか」を決めるキー入力マッピングであり、
  文中への自動挿入とは無関係。本機能はそれとは別物（字種は §6 で別途扱う）。
- **一括変換 + AI 整文（M58-C）ではない。** `batchAutoPunctuation`（M58-C）は
  `batchRomajiConversion` + `batchConversionMode=ai-cleanup` 時に host 側 AI が全文を
  整文する経路で、逐次変換中は動作しない。本機能は**通常のライブ変換中**に動作する
  別系統。両者は設定で独立に ON/OFF できる。
- **文法校正・敬語チェックではない。** 句読点の挿入・削除のみを扱う。より広い不自然さ
  検出は X-3（Post-Commit Lint / DetectAnomalies）の領域で、本書は扱わない。
- **既定 ON ではない。** 既定 OFF の実験機能であり、有効化時のみ動作する。OFF のとき
  ライブ変換の挙動は一切変えない。

## 2. 設計原則

- **ライブ変換の上に載せる。** 動的句読点は `liveConversion == true` を前提とする
  enhancement である。候補ウィンドウ方式（liveConversion OFF）では「流れる文の
  preedit」が存在しないため動作しない。`liveConversion == false` のときは本機能を
  無効化する（設定が ON でも適用しない）。
- **かなバッファが正典、句読点は派生物。** ユーザーが打鍵した内容（かな / 生ローマ字）
  だけが編集可能な正典バッファである。自動句読点は surface のレンダリング時に**毎回
  再計算される派生物**であり、ユーザーが直接編集・カーソル選択する対象にしない
  （§5）。これにより「削除」を特別なロジックなしに full-preedit 再計算で実現する。
- **決定的ベースライン + ニューラル品質向上の二層。** まず host 側の決定的な
  文節境界ヒューリスティック（`PunctuationInserter`）で挿入位置を決め、テスト可能な
  baseline とする。zenz が surface に句読点を直接出力できる構成では、それを上位品質
  レイヤとして使う（SimpleConverter → Zenzai と同じ段階導入方針）。
- **既定 OFF・後方互換。** OFF のとき従来のライブ変換・状態遷移・学習を一切変えない。

## 3. InputState への統合

新しい状態は追加しない。`docs/legacy-parity-spec.md` §1.2 の `Composing` /
`Previewing`（ライブ変換中）の中で動作する。`Previewing` の preedit 更新時に、
surface へ自動句読点を含めて表示する。

遷移への影響（`dynamicPunctuation == true` かつ `liveConversion == true` のとき）:

| 現状 | 入力 | 次状態 | 副作用 |
|---|---|---|---|
| Composing/Previewing | Input | Previewing | ライブ変換要求（現状 `QueryCandidates` の `live=true`。§7）を送信。`auto_punctuation` は安定化モードに従う（**`onPause` のタイピング中は `false`＝抑制**、`eager` は `true`。§7.1.1）。応答 surface で Preedit 全体を差し替え |
| Previewing | Backspace | Previewing/Composing | **かなバッファを 1 単位削除**（自動句読点は削除単位に数えない。§5）。再ライブ変換要求（Backspace も編集イベント。`auto_punctuation` は §7.1.1 の timing 規則に従い `onPause` 編集中は `false`、idle タイマー再アーム） |
| Previewing | IdleTimeout（`onPause`時） | Previewing | idle タイマー満了で `auto_punctuation=true` のライブ変換要求を post し、句読点込みで Preedit 更新（§4.3.1）。`onPause` で句読点を出す必須トリガ |
| Previewing | Commit (Enter) | Idle | 句読点を含む全文を確定。`CommitObservation` は自動句読点を分離して観測（§5・§7） |
| Previewing | Cancel (Esc) | Idle | CancelComposition |

`dynamicPunctuation == false` または `liveConversion == false` のときは、
ライブ変換要求に `auto_punctuation=false` を載せ、従来どおり句読点を挿入しない。

## 4. 挿入・削除の判定

### 4.1 挿入位置

host 側（`inference-host`）で判定する。**host はリクエストの `auto_punctuation=true`
（§7.1.1）のときだけ**句読点を挿入し、`false` のときは句読点なしの surface を返す
（タイピング中の抑制は TIP がこのフラグで制御する）。挿入位置の判定は二層構成:

- **決定的レイヤ（既定・M59 コア）**: `PunctuationInserter`（新規）が、変換後 surface と
  **文節境界（live 変換応答の `segments[]`、§7.2）**を入力に、文節境界での読点 `、` と
  文末の句点 `。` を挿入する。形態素解析器を持たないため、各文節末尾の**表層サフィックス
  一致**で節境界を判定する。規則表（既定。bench M52・実機で調整。本表が決定的レイヤの
  正典）:

  **読点 `、` 挿入（文節 i と i+1 の境界。直前文節 i の末尾表層で判定）**

  | 分類 | トリガ（直前文節末尾の表層パターン） | 例 | 動作 |
  |---|---|---|---|
  | 接続助詞(順接/理由) | 「ので」「から」「ため」 | 雨なので→ | 境界に読点 |
  | 接続助詞(逆接) | 「が」「けど」「けれど」「のに」「ても」「でも」 | 行ったが→ | 境界に読点 |
  | 連用中止/並列 | 「て」「で」(連用中止)、「し」、「たり」 | 食べて→ / 安いし→ | 境界に読点 |
  | 引用/条件 | 「と」(引用)、「ば」「なら」「たら」 | 〜すれば→ | 条件節境界に読点 |
  | 副詞節 | 「とき」「ところ」「場合」「うえで」 | 〜の場合→ | 境界に読点 |
  | 接続詞(文頭) | 文節頭が「しかし」「だから」「また」「そして」「ただし」「つまり」 | 〜。しかし→ | 接続詞**直後**に読点 |

  **句点 `。` 挿入**

  | 分類 | トリガ | 動作 |
  |---|---|---|
  | 終止 | 末尾文節が終止形/丁寧形（「です」「ます」「だ」「である」「した」「ない」等）で終わり、かつ確定直前 or `onPause` idle | 文末に句点 |
  | 疑問 | 末尾が疑問終助詞「か」 | 文末に句点（`？` 化は将来・設定） |

  **抑制（挿入しない）規則**

  | 条件 | 理由 |
  |---|---|
  | 直前/直後が既に句読点 | 連続句読点防止 |
  | 文頭（バッファ先頭） | 行頭読点防止 |
  | 直近 K=6 文字以内に既に読点がある | 読点過密防止 |
  | 全体が N=8 文字未満かつ単一文節 | 短文への過剰挿入防止 |
  | 英数字列・記号列・URL 様パターンの内部 | 誤挿入防止（M60 英単語候補・記号との干渉回避） |
  | 当該文節境界スコアが `segmentBoundaryConfidence`（既定 0.5）未満 | ライブ変換中の不安定境界への挿入を防ぐ |

  （`onPause` のタイピング中の抑制は host 側のこの表ではなく、**TIP がリクエストに
  `auto_punctuation=false` を載せる**ことで実現する。host は `auto_punctuation=true` の
  リクエストでのみ上表に従って挿入する。§7.1.1・§4.3）

- **ニューラルレイヤ（任意・品質向上）**: zenz が surface に句読点を直接出力できる構成
  では、その出力を採用する。zenz 出力と決定的レイヤの整合は実装時に定める（既定は
  決定的レイヤ、zenz 句読点が利用可能なら上書き）。

#### 4.1.1 境界評価モデル（スコアリング）

各文節境界 `b`（`seg_i` と `seg_{i+1}` の間）と最終境界（バッファ末尾）を評価する。

```text
for each boundary b between seg_i and seg_{i+1}:
    rule = longest_suffix_match(seg_i.surface, 読点ルール表)   # 最長サフィックス一致
    if rule is None: continue
    score  = rule.base_score
    score *= next_guard_factor(rule, seg_{i+1})    # 曖昧性ガード(§4.1.2) ∈ [0,1]
    score -= suppression_penalty(b)                # 抑制(§4.1) で減点
    if score >= kCommaThreshold and not hard_suppressed(b):
        insert 読点 at b
```

`kCommaThreshold` は `PunctuationInserter` の**内部定数**（既定 0.5、bench M52 で調整）。
ユーザー設定にはしない（base_score とセットで調整する実装パラメータのため）。複数ルールが
一致したら最長サフィックスを採り、同長なら base_score 最大を採る。

読点ルール表の `base_score`（§4.1 の分類に付与）:

| 分類 | base_score |
|---|---|
| 文頭接続詞（しかし/だから/また/そして/ただし/つまり） | 0.90 |
| 逆接接続助詞（が/けど/けれど/のに/ても/でも） | 0.85 |
| 順接・理由（ので/から/ため） | 0.80 |
| 引用・条件（と/ば/なら/たら） | 0.70 |
| 副詞節（とき/ところ/場合/うえで） | 0.65 |
| 連用中止・並列（て/で/し/たり） | 0.60 |

#### 4.1.2 曖昧性ガード（格助詞 vs 接続助詞 等）

表層サフィックスだけでは節境界か否かを誤判定する代表例を `next_guard_factor`
（0=挿入しない 〜 1=フル base_score）でガードする。**品詞情報（§7.2.1 の `pos` /
`head_pos`）が応答に付与されていれば、それを一次根拠**とし、無い（`Unknown`）構成では
表層ヒューリスティック（括弧内）にフォールバックする。

- **「が」**: 逆接接続助詞（読点要）と主格格助詞（読点不要）の判別。
  - `seg_i.pos == JoshiConj`（接続助詞、例「行った→が」）→ factor=1。
  - `seg_i.pos == JoshiCase`（格助詞、例「私→が」）→ factor=0。
  - フォールバック（pos 無し）: 「直前が動詞/形容詞の活用語尾（た/だ/る/い/う 段 等）」なら
    接続助詞とみなし factor=1、体言終止なら factor=0。
- **「て」「で」**: 連用中止（読点要）と補助用言への接続（読点不要、例「食べて→いる」）の判別。
  - `seg_{i+1}.head_pos == HojoYougen`（補助用言: いる/ある/おく/みる/しまう/くれる/もらう/
    いく/くる 等）→ factor=0。
  - それ以外 → factor=1。
  - フォールバック: `seg_{i+1}` 先頭表層が補助用言語彙集合に一致するか。
- **「と」**: 引用・条件（読点要）と並列格助詞（「A と B」読点不要）の判別。
  - `seg_{i+1}.head_pos == Taigen` かつ並列継続 → factor=0。
  - `seg_{i+1}.head_pos == Yougen`（思考/発話動詞等、引用）or 条件文脈 → factor=1。
  - フォールバック: `seg_{i+1}` 先頭が体言か用言かの表層推定。

品詞付与は任意の品質向上であり、M59 コアは pos 無しフォールバックでも動作する。pos が
あるほど誤挿入が減る（特に「が」の格/接続判別）。

#### 4.1.3 句点の精緻化（終止形 vs 連体形・数値）

- 句点はバッファ末尾境界でのみ評価する。
- **終止形 vs 連体形**: 「食べる」は終止（句点可）にも連体（後続体言を修飾、句点不可）にも
  なる。**続く入力が無く（確定 or `onPause` idle）かつ末尾が終止/丁寧**（です/ます/だ/
  である/した/ない/終止活用）のときのみ句点。連用・連体接続が続きうる間は句点を入れず、
  確定時の最終評価（§5.3）で補う。
- **数値・小数・略記**: 末尾が数字（「3.14」「No.5」）の場合はピリオドを句点化しない。
  英数字列・記号列内部は §4.1 抑制規則に従う（M60 英単語候補とも干渉させない）。
- **疑問**: 末尾が疑問終助詞「か」→ 句点。`？` 化は将来・設定。

#### 4.1.4 ルール定義の外部化（TSV）

§4.1〜§4.1.3 の規則は `PunctuationInserter` に**組み込みの既定ルール**として持つが、
再コンパイルせず調整・カスタムできるよう **TSV で上書き・追加**できる。M17 カスタム
ローマ字テーブル（`docs/legacy-parity-spec.md` §5）と同じファイル運用・ホットリロード基盤を
再利用する。

**ファイル形式**: TSV、UTF-8（BOM 許容）。1 行 1 ルール:

```text
# punctuation-rules.tsv (UTF-8)
# 行頭 # はコメント、空行は無視
# <kind>\t<match>\t<base_score>\t<guard>
#   kind      : comma | period
#   match     : comma=直前文節末尾の表層パターン / period=文末の終止形キーワード
#   base_score: 0.0–1.0（comma のスコア。0 で当該ルールを無効化＝既定の打消し）
#   guard     : 任意。';' 区切り AND。下記ミニ言語
comma	が	0.85	prev_pos=JoshiConj
comma	ので	0.80
comma	て	0.60	next_head_pos!=HojoYougen
comma	しかし	0.90	prev_pos=Setsuzoku
period	です	1.0
period	ます	1.0
period	か	1.0	sentence_final
```

**guard ミニ言語**（評価系は `PunctuationInserter` 内に固定。未知トークンは warning 行スキップ）:

| guard | 意味 |
|---|---|
| `prev_pos=<SegmentPos>` / `prev_pos!=<SegmentPos>` | 直前文節 `seg_i.pos`（§7.2.1） |
| `next_head_pos=<SegmentPos>` / `!=` | 次文節 `seg_{i+1}.head_pos` |
| `sentence_final` | バッファ末尾境界でのみ適用（period 既定） |

`<SegmentPos>` は §7.2.1 の列挙名（`Taigen` `Yougen` `JoshiCase` `JoshiConj`
`HojoYougen` `Setsuzoku` …）。guard が満たされないルールは `next_guard_factor=0`
（適用しない）。pos が `Unknown`（品詞付与なし）のときは `prev_pos` / `next_head_pos`
guard を**表層フォールバック**（§4.1.2）で評価する。

**字種**: TSV には字（`、` `。`）を書かない。`kind=comma`→読点、`kind=period`→句点として、
実際の字は `dynamicPunctuationStyle`（§6）で決まる（`ja`=`、。` / `fullwidth_latin`=`，．`）。
これにより字種設定と外部ルールが独立する。

**マージ規則（M17 の「置換」と異なる）**: 組み込み既定は常にロードし、TSV 行は
**`(kind, match)` キーで既定を上書き**し、新規キーは追加する（同一キーはファイル末尾が
勝つ）。`base_score=0` の comma 行は当該既定ルールの**無効化**として働く（打消しの
エスケープハッチ）。これにより「数ルールだけ微調整」が安全にできる。

**配置・ロード**:

- 既定パス: `%LOCALAPPDATA%\azooKey\punctuation-rules.tsv`。設定 `punctuationRulesPath` で上書き。
- ファイルが無ければ組み込み既定のみで動作（後方互換）。
- パース失敗行は warning ログでスキップ（M17 流儀）。
- ホットリロード: M17 の `ReadDirectoryChangesW` 監視基盤を再利用し、変更検出で新規入力から
  差し替える（進行中の preedit は触らない）。
- 既定ルールを TSV へ書き出して編集の土台にするダンプは Phase 7 設定アプリ（M30）で提供
  （M17 §5.4 と同方針）。
- 任意の先頭ディレクティブ `# version: <N>`（1 行目のコメント形式）で文法バージョンを
  宣言できる。未知バージョンは warning を出しつつ既知トークンのみ解釈する（前方互換）。

#### 4.1.5 guard ミニ言語の文法（EBNF）

§4.1.4 の `guard` 列の文法を EBNF（ISO/IEC 14977 準拠記法）で固定する。評価系は
`PunctuationRules`（`core`）に実装する。

```ebnf
guard        = [ ws ] , [ condition , { [ ws ] , ";" , [ ws ] , condition } ] , [ ws ] ;
condition    = pos-cond | sem-cond | flag-cond ;
pos-cond     = pos-field , [ ws ] , ( "=" | "!=" ) , [ ws ] , pos-value ;
sem-cond     = sem-field , [ ws ] , ( "=" | "!=" ) , [ ws ] , sem-value ;
pos-field    = "prev_pos" | "next_head_pos" ;
sem-field    = "prev_sem" | "next_head_sem" ;
pos-value    = "Unknown" | "Taigen" | "Yougen" | "JoshiCase" | "JoshiConj"
             | "JoshiOther" | "Setsuzoku" | "Jodoushi" | "HojoYougen"
             | "Rentai" | "Fukushi" | "Kigou" | "English" ;
sem-value    = "Unknown" | "Generic" | "PersonName" | "PlaceName"
             | "OrgName" | "ProductName" | "DateTime" | "Number" ;
flag-cond    = "sentence_final" ;
ws           = { " " | "\t" } ;
```

字句・評価規約:

- `pos-field` / `pos-value` / `sem-field` / `sem-value` / `flag-cond` は **厳密一致**
  （`SegmentPos` / `SegmentSemantic` 列挙名と同綴り。大文字小文字を区別）。
- **空 guard**（空文字列）は **常に真**（無条件適用）。
- 複数 `condition` は **`;` で AND**。OR は提供しない（OR が要る場合は同じ `(kind,match)` の
  別行で表現する）。
- `prev_pos` / `prev_sem` は `seg_i` の `pos` / `sem`（末尾形態素）、`next_head_pos` /
  `next_head_sem` は `seg_{i+1}` の `head_pos` / `head_sem`（先頭自立語）を参照する。
- **未知トークン**（未定義 field / value、余分なトークン、構文崩れ）を含む行は
  **warning ログを出して行ごとスキップ**（§4.1.4 のパース失敗扱い）。他行は読み続ける。

**Unknown pos / sem 時の評価**（不確実性を誤挿入回避側へ倒すバイアス）:

- 当該 field の `pos` / `sem` が `Unknown` のとき、host はまず表層ヒューリスティック（§4.1.2）で
  `SegmentPos` を推定し（sem には表層推定が無いので `Unknown` のまま）、その値で比較する。
- 値が `Unknown` のままのとき:
  - `=` 条件 → **偽**（一致を確認できない＝挿入根拠なし）。
  - `!=` 条件 → **真**（不一致を否定できない＝抑制根拠なし）。
  - 例: `next_head_pos!=HojoYougen` は次文節品詞が不明なら真（補助用言と断定できない →
    読点挿入を許す）。`prev_sem=PlaceName` は意味不明なら偽（連鎖抑制を発動しない）。

**将来拡張（予約。`# version` で管理）**:

- `prev_surface_endswith="…"` / `next_head_surface="…"` 等の表層条件。
- 追加時は本 EBNF を改訂し、TSV 先頭 `# version: <N>` で互換判定する（`prev_sem` /
  `next_head_sem` は本版で正式採用済み）。

### 4.2 削除・再配置

ライブ変換は打鍵ごとに preedit 全体を再評価し置き換える（M14）。したがって、ある
位置の読点は、続く入力で文節構造が変わって `PunctuationInserter` がそこを境界と
判定しなくなれば**自然に消える**。明示的な削除ロジックは持たず、full-preedit 再計算で
挿入・削除・再配置を一括して扱う。

### 4.3 安定化（ちらつき抑制）

打鍵ごとに句読点が出現・消滅すると視覚的に不安定になる。安定化規則:

安定化の制御は **TIP がリクエストの `auto_punctuation` で行う**（§7.1.1。host はモードを
知らず、`auto_punctuation=true` のときだけ挿入する）。

- 設定 `dynamicPunctuationStability`:
  - `onPause`（既定）: タイピング中（`TypingTempoTracker::IsTyping()` = X-1-2 を再利用、
    平均打鍵間隔 < 閾値）は TIP が打鍵リクエストに `auto_punctuation=false` を載せて句読点を
    **挿入させない**。入力が止まった（idle）瞬間に `IdleTimeout`（§4.3.1）で
    `auto_punctuation=true` のリクエストを送って挿入する。連続入力中のちらつきを防ぐ。
  - `eager`: 打鍵ごとに `auto_punctuation=true` を送り常に挿入する（反応は速いがちらつきうる）。
- どちらでも、確定（Enter）直前には必ず最終評価を行い、文末句点を補う。

#### 4.3.1 idle タイマー（`onPause` の挿入トリガ）

`onPause` では「タイピング中は挿入しない」ため、**最後の打鍵の後に句読点を出す追加トリガが
必要**である。打鍵イベントだけに依存すると、最後のキー以降は次の打鍵や Enter までイベントが
発生せず句読点が現れない。よって TIP は**idle タイマーで再評価を駆動する**ことを必須とする。

- TIP は `Previewing` 中、各**編集イベント（打鍵 Input / Backspace）**で **idle タイマーを
  `dynamicPunctuationIdleMs`（既定 400ms、目安は `TypingTempoTracker` の閾値以上）で
  リセット（再アーム）**する。
- タイマー満了（= idle 確定）で、TIP は **`auto_punctuation=true` のライブ変換要求を 1 回 post**
  し、応答で Preedit を句読点込みに更新する（§3 の `IdleTimeout` 遷移）。
- 次の打鍵が来たらタイマーをキャンセル/再アームし、`eager` でない限り満了まで挿入しない。
- 実装は TIP プロセス内のタイマー（`SetTimer` / スレッドタイマー等。実装時に選択）。`eager` では
  打鍵ごとに挿入するため idle タイマーは不要（無効化してよい）。
- このトリガは `dynamicPunctuation && liveConversion && onPause` のときのみ動作する。

## 5. 読み↔surface の非対称と編集

自動句読点は**ユーザーが打鍵していない**ため、対応する読みを持たない。これが
surface と読みの 1:1 対応を崩す。各サブシステムの扱いを以下に固定する。

- **Backspace の削除単位**: Backspace は**かな / 生ローマ字バッファの 1 単位**を削除し、
  自動句読点は削除単位に数えない。句読点は再計算で増減する派生物のため、ユーザーが
  Backspace で句読点だけを消そうとしても、消えるのは直近のかな単位であり、句読点の
  有無は再評価で決まる。
- **カーソル/キャレット**: M59 コアは**バッファ末尾編集**（末尾への追記・末尾からの削除）を
  対象とする。preedit 途中へのキャレット移動編集（文中挿入）は、読み↔surface の
  オフセット写像（自動句読点ぶんのズレ）を要するため M59 コアの対象外とし、M20
  （再変換 / multi-segment）統合時に扱う。
- **学習（CommitObservation）**: 学習ストアは `(reading, surface)` ペアを観測する。
  surface に読みを持たない句読点が混じると学習が汚染される。確定時は応答の `segments`
  情報を使って**自動句読点スパンを分離**し、各文節の `(reading, surface)` を句読点抜きで
  観測する（§7）。自動句読点そのものは学習対象にしない。
- **再変換（M20）**: 確定済みテキストに含まれる自動句読点は読みを持たないため、
  再変換時は**文節境界 / リテラル**として扱い、読みへ逆変換しない。詳細は M20 統合時に
  `docs/tsf-deep-integration-spec.md` 側で確定する。

### 5.1 データ構造

正典バッファは**読み（かな）+ 生ローマ字**であり、句読点込み surface は派生物。
TIP 側 composition 状態:

```cpp
// TIP（tsf-tip）side composition state（概念。既存 TextService に統合）
std::string kana_buffer_;        // 打鍵由来の読み。編集の正典
std::string raw_romaji_;         // 生ローマ字（M58/M60 と共有）
std::string rendered_surface_;   // 句読点込み表示文字列。毎回再計算で全置換
std::vector<LiveSegment> segments_;  // host 応答（§7.2）。auto_punctuation マーカ付き
```

`LiveSegment`（host 応答 = §7.2 の各要素）:

```cpp
struct LiveSegment {
  uint32_t start_char;       // rendered_surface_ 上の開始（UTF-16 code unit。ITfRange 操作用）
  uint32_t end_char;         // 同 終了（排他）
  double   score;            // 文節境界/変換信頼度
  bool     auto_punctuation; // true: 読みを持たない自動挿入句読点
  std::string surface;       // 文節の表層（UTF-8。学習・確定はこれを直接使う。§5.3）
  std::string reading;       // 文節読み（auto_punctuation=true は空）
};
```

不変条件: `auto_punctuation == true` の要素は `reading.empty()` かつ
`kana_buffer_` に対応文字を持たない（純粋な派生物）。各 `surface` の連結は
`rendered_surface_` と一致する。

> **`start_char`/`end_char`（UTF-16）と `surface`（UTF-8）の使い分け**: オフセットは TIP の
> `ITfRange::SetText` / DisplayAttribute など**範囲操作専用**。**学習スライスにオフセットを使って
> `rendered_surface_`（UTF-8 `std::string`）を `substr` してはならない**（UTF-16 単位を UTF-8 バイト
> 位置に誤用し、日本語でマルチバイト境界を割って壊れた surface を学習する）。学習・確定には
> 各 segment が自前で持つ `surface` 文字列を使う。

### 5.2 Backspace（削除単位）

自動句読点は `kana_buffer_` に存在しないため、削除は常に**読みバッファ**に対して行う。

```text
on Backspace in Previewing(dynamicPunctuation && liveConversion):
    if RomajiKanaConverter has pending:   # 例: "k" だけ打って未確定
        PopPending()                      # 生ローマ字を 1 つ戻す
    else:
        remove last 1 kana unit from kana_buffer_ (+ 対応 raw_romaji_ 末尾)
    # Backspace も編集（打鍵）イベント: auto_punctuation は timing 規則(§7.1.1)に従う
    #   onPause → false（抑制）, eager → true。idle タイマー(§4.3.1)を再アーム
    re-issue live conversion (auto_punctuation = timing_flag())   # onPause 編集中は false
    rearm idle timer (onPause)
    # rendered_surface_ と segments_ を新応答で全置換
```

Backspace は他の打鍵と同じく**編集イベント**なので、`auto_punctuation` を `true` 固定にせず
§7.1.1 の timing 規則で導出する（`onPause` の編集中は `false`＝抑制し、idle タイマー満了で
初めて挿入）。これにより「idle で句読点が出た後に Backspace すると即座に再挿入されて
ちらつく」事象を防ぐ。

ユーザーには「Backspace で句読点が消えた」ように見える場合があるが、実際は読みが
変わって再計算で句読点が落ちただけ。「読点だけ残してかなを消す」「かなだけ残して
読点を消す」という分離編集は提供しない（句読点は読みに従属する派生物のため）。

### 5.3 確定と学習分離（CommitObservation）

```text
on Commit(Enter):
    final = re-evaluate(kana_buffer_, auto_punctuation=true)  # 文末句点を補う最終評価
    EndComposition(final.rendered_surface)                    # 句読点込みでアプリへ確定
    # 学習は auto_punctuation セグメントを除外し、文節ごとに観測
    # 重要: chosen.surface は seg.surface（UTF-8 文字列）を直接使う。
    #       UTF-16 オフセット(start_char/end_char)で rendered_surface_ を substr しない(§5.1)
    for seg in final.segments where !seg.auto_punctuation:
        CommitObservation(reading = seg.reading,
                          chosen.surface = seg.surface)
    # auto_punctuation セグメントは学習に渡さない（reading 無し）
```

**学習不変条件**: 自動句読点の文字は確定観測の `reading` にも `chosen.surface` にも
学習入力として含めない（句読点はテキストには入るが Observe しない）。確定は
**`CommitSegmentsObservation`（`docs/romaji-batch-conversion-spec.md` §6.4、M58-B と共有）**で
行い、自動句読点文節を `is_auto_punctuation=true` として送る。host は当該文節を Observe
せず文脈連結のみ行う。host が `commit_segments` capability 非対応の場合は、TIP が
`!auto_punctuation` の各文節を既存 `CommitObservation` で順次送るフォールバックに切替える
（§6.4.3）。

### 5.4 カーソル/オフセット写像

M59 コアは**末尾編集（末尾 append / 末尾 delete）のみ**を対象とするため、
`reading 末尾 ↔ surface 末尾` の対応で足り、フルオフセット写像は不要。
文中キャレット編集（preedit 途中挿入）は、`segments_` の
`(start_char, end_char, reading)` から surface↔reading 逆写像を構築する必要があり、
M20（再変換 / multi-segment）統合時に対応する。`auto_punctuation` セグメントは
reading 長 0 として写像に組み込む。

## 6. 表示と字種

- **字種**: 設定 `dynamicPunctuationStyle` で挿入する句読点の字種を選ぶ。
  - `ja`（既定）: 読点 `、` / 句点 `。`
  - `fullwidth_latin`: 全角カンマ `，` / 全角ピリオド `．`
  （legacy `Config.PunctuationStyle` の 4 値のうち、自動挿入で意味を持つ「読点字 / 句点字」の
  組のみを 2 値で表現する。キー押下時の字種設定とは別物。）
- **DisplayAttribute（任意）**: 自動挿入した句読点を、ユーザー打鍵文字とは異なる
  控えめな属性（例: X-1-1 の `kLiveAttrTentativeGuid` 相当の薄い下線）で描画し、
  「IME が補った・以後自動調整される」ことを示してよい。既定は周囲と同一属性で
  目立たせない。実装時に決定する。

## 7. IPC プロトコル

新 `MessageType` は追加しない。**実装の現状整合**: ライブ変換は現状
`QueryCandidatesRequest.live = true`（`ipc/include/azookey/ipc/Payloads.h`）で運ばれており、
`docs/legacy-parity-spec.md` §2.2 が提案した独立 `QueryLiveConversion` payload は未実装。
よって M59 は**現行 `QueryCandidates` 経路にオプションフィールドを追加**する。M14 が独立
`QueryLiveConversion` payload に分離する場合は、本節のフィールドを**そのまま新 payload へ
移設**する（フィールド定義・既定値・後方互換規約は不変）。

すべての追加フィールドは**任意・後方互換**とし、既存 `Payloads.cpp` の流儀
（Build = `o.emplace(...)`、Parse = `GetBool/GetString/GetUInt/...().value_or(既定)`）に従う。
省略時は既定値となり、旧 TIP / 旧 host と相互運用できる。

### 7.1 Request（`QueryCandidatesRequest` 拡張）

既存フィールド: `reading` / `left_context` / `max_candidates` / `live`。追加:

| field | 型 | 既定 | 説明 |
|---|---|---|---|
| `auto_punctuation` | bool | `false` | **このリクエストの応答で句読点を挿入するか**（per-request）。host は `true` のときだけ挿入する。timing 判断（onPause のタイピング中は抑制 / idle・commit で挿入）は **TIP 側**が行い、本フラグに符号化する（§7.1.1） |
| `punctuation_style` | string | `"ja"` | `"ja"`（`、。`）/ `"fullwidth_latin"`（`，．`） |

#### 7.1.1 `auto_punctuation` の timing 符号化（host は typing/idle を知らない）

host は「タイピング中か idle か」を知らない（その情報はリクエストに無い）。よって
**挿入タイミングの判断は TIP が持ち、各リクエストの `auto_punctuation` に符号化する**。
host は受け取った `auto_punctuation` が `true` のときだけ句読点を挿入する（§4.1）。

| 状況（`dynamicPunctuation=true` かつ `liveConversion=true`） | TIP が送る `auto_punctuation` |
|---|---|
| `onPause`・編集イベント（打鍵 Input / **Backspace**、タイピング中） | `false`（抑制。句読点なしの surface を返させる。idle タイマー再アーム） |
| `onPause`・`IdleTimeout`（§4.3.1）/ Commit 前の最終評価 | `true`（挿入） |
| `eager`・打鍵による Input | `true`（毎回挿入） |
| `dynamicPunctuation=false` または `liveConversion=false` | `false`（常に） |

これにより、同じ kana バッファでも「打鍵時リクエスト＝抑制」「idle/commit リクエスト＝挿入」を
host 側のモード判定なしに区別できる（Codex 指摘の「host が打鍵と idle を区別できない」問題の解消）。

```jsonc
{
  "reading": "きょうはいいてんきです",
  "left_context": "",
  "max_candidates": 10,
  "live": true,                  // ライブ変換経路（既存）
  "auto_punctuation": true,       // 追加（例: idle/commit/eager の挿入リクエスト。onPause 打鍵中は false。§7.1.1）
  "punctuation_style": "ja"       // 追加
}
```

Build/Parse 追記（規約例）:

```cpp
// Build
o.emplace("auto_punctuation", j::Value(p.auto_punctuation));
o.emplace("punctuation_style", j::Value(p.punctuation_style));
// Parse（後方互換）
p.auto_punctuation  = v->GetBool("auto_punctuation").value_or(false);
p.punctuation_style = v->GetString("punctuation_style").value_or("ja");
```

`auto_punctuation` は `live == true` のときのみ意味を持つ（host は `live==false` では無視）。

### 7.2 Response（`QueryCandidatesResponse` に `segments[]` を追加）

現状 `QueryCandidatesResponse` は `candidates[]` + `partial` のみで文節構造を持たない。
M59 は**任意配列 `segments[]` を新規追加**する（X-1-1 の segments と整合）。`segments` を
省略した応答は従来どおり（句読点なし・文節情報なし）に解釈される。

各 segment フィールド:

| field | 型 | 必須 | 説明 |
|---|---|---|---|
| `start_char` | uint32 | ○ | 最良 surface 上の開始オフセット（**UTF-16 code unit**。TSF `ITfRange` と整合） |
| `end_char` | uint32 | ○ | 同 終了（排他） |
| `score` | number | ○ | 文節境界/変換信頼度（抑制判定 `segmentBoundaryConfidence` に使用） |
| `auto_punctuation` | bool | 既定 false | true = 読みを持たない自動挿入句読点スパン |
| `surface` | string | ○ | 文節の表層（UTF-8）。**学習・確定はこれを直接使う**（§5.3） |
| `reading` | string | 既定 "" | 文節読み（`auto_punctuation=true` は空） |

最良 surface 全体は `candidates[0].surface`（句読点込み）に等しく、各 `segments[].surface` の
連結と一致する。**per-segment `surface` を持たせる**のは、学習・確定で `start_char`/`end_char`
（UTF-16）を UTF-8 文字列のバイトオフセットに誤用して切り出す事故を避けるため（§5.1）。オフセットは
TIP の `ITfRange` 範囲操作専用、学習スライスは `surface` 文字列を使う。

```jsonc
{
  "candidates": [
    { "surface": "今日はいい天気です。", "reading": "きょうはいいてんきです", "score": 0.93, "source": "model" }
  ],
  "partial": false,
  "segments": [
    { "start_char": 0,  "end_char": 3,  "score": 0.95, "auto_punctuation": false, "surface": "今日は",     "reading": "きょうは" },
    { "start_char": 3,  "end_char": 9,  "score": 0.90, "auto_punctuation": false, "surface": "いい天気です", "reading": "いいてんきです" },
    { "start_char": 9,  "end_char": 10, "score": 0.0,  "auto_punctuation": true,  "surface": "。",          "reading": "" }
  ]
}
```

> `start_char`/`end_char` を UTF-16 code unit に固定する理由: TIP は `ITfRange::SetText` /
> Property を UTF-16 オフセットで操作するため、UTF-8 codepoint で返すと TIP 側で再計算が
> 必要になり境界ズレの温床になる。host は UTF-16 単位でオフセットを計算して返す。

#### 7.2.1 品詞フィールド（`pos` / `head_pos`）の正式化

§4.1.2 の曖昧性ガードを品詞で駆動するため、各 segment に**任意の品詞フィールド**を追加する。

```cpp
// core/include/azookey/core/SegmentPos.h（新規・core で converter と host が共有）
enum class SegmentPos : uint8_t {
  Unknown    = 0,   // 品詞不明（既定。フォールバックで表層ヒューリスティック）
  Taigen     = 1,   // 体言（名詞・代名詞・数詞）
  Yougen     = 2,   // 用言（動詞・形容詞・形容動詞の活用語）
  JoshiCase  = 3,   // 格助詞（が・を・に・へ・で・と・から・より）
  JoshiConj  = 4,   // 接続助詞（が・ので・から・て・し・のに・ても）
  JoshiOther = 5,   // 副助詞・係助詞・終助詞（は・も・か・ね・よ 等）
  Setsuzoku  = 6,   // 接続詞（しかし・だから・また・そして）
  Jodoushi   = 7,   // 助動詞（です・ます・た・ない・だ・である）
  HojoYougen = 8,   // 補助用言（いる・ある・おく・しまう・くれる・もらう・いく・くる）
  Rentai     = 9,   // 連体詞
  Fukushi    = 10,  // 副詞
  Kigou      = 11,  // 記号・句読点
  English    = 12,  // 英単語（M60）
};
```

各 segment への追加フィールド（すべて任意・後方互換、既定 `0=Unknown`）:

| field | 型 | 既定 | 説明 |
|---|---|---|---|
| `pos` | uint8 | `0` | 文節の**末尾形態素**の品詞（`SegmentPos`）。境界助詞の格/接続判別（「が」等）に使う |
| `head_pos` | uint8 | `0` | 文節の**先頭自立語**の品詞。次文節の補助用言/体言判別（「て・で」「と」）に使う |

- **末尾形態素の `pos`** を主に使う理由: 文節境界マーカは末尾の付属語（助詞）であり、
  「行った**が**」=`JoshiConj` / 「私**が**」=`JoshiCase` のように末尾品詞が格/接続を直接
  決めるため、表層一致より頑健。
- **host 側導出**: 変換器の品詞 ID（辞書の `cid`/`mid`。`AddUserWordRequest.cid/mid` と
  同系）から `SegmentPos` への**粗いマッピング表**を host が持ち、応答に詰める。細粒度
  `cid` の直接公開はしない（粗 enum に正規化。将来必要なら別フィールドで追加）。
- **後方互換**: `pos`/`head_pos` を欠く応答は `Unknown` となり、§4.1.2 は表層フォールバックで
  動作する。Build/Parse は `o.emplace("pos", j::Value((double)seg.pos))` /
  `seg.pos = (uint8_t)v.GetUInt("pos").value_or(0)`（既存流儀）。

```jsonc
// §7.2 の segment に pos/head_pos を加えた例
{ "start_char": 0, "end_char": 4, "score": 0.95, "auto_punctuation": false,
  "reading": "いったが", "pos": 4, "head_pos": 2 }   // 末尾=JoshiConj(が), 先頭=Yougen(行っ)
```

#### 7.2.2 辞書 cid/mid → SegmentPos マッピング

`pos` / `head_pos`（§7.2.1）は host が変換器辞書の品詞 ID から導出する。azooKey 系辞書は
各形態素に **`cid`**（接続 ID。mecab / Mozc 系の品詞細分類に対応。`AddUserWordRequest.cid`
と同体系）と `mid`（意味 ID）を持つ。`cid` の数値は辞書ビルドの `id.def`（cid→品詞名）で
決まるため、**正典マッピングは「品詞名 → SegmentPos」**とし、host は辞書同梱の cid→品詞名表を
介して適用する（cid 番号の振り直しに頑健）。

- **末尾 `pos`**: 文節末尾形態素の **rcid**（右文脈 ID）→ 品詞名 → SegmentPos。
- **`head_pos`**: 文節先頭自立語の **lcid**（左文脈 ID）→ 品詞名 → SegmentPos。
- `mid` は意味分類（人名 / 地名 / 組織等）で句読点判定には原則使わない。固有名詞細分類が
  必要なときの補助に留める。

品詞名 → SegmentPos（mecab / azooKey 細分類。本表が正典。`pos` 列は
`docs/auto-word-registration-spec.md` §14.2 の `pos:"名詞-固有名詞"` 等と同体系）:

| 品詞名（細分類） | 代表 cid 例 | SegmentPos |
|---|---|---|
| 名詞-一般 / 名詞-固有名詞-* / 代名詞 / 名詞-数 | 1285（固有名詞-一般。`learning/tests/user_dictionary_test.cpp` の例） | `Taigen` |
| 動詞-自立 / 形容詞-自立 / 名詞-形容動詞語幹 | | `Yougen` |
| 助詞-格助詞（が・を・に・へ・で・と・から・より） | | `JoshiCase` |
| 助詞-接続助詞（て・で・が・し・から・ので・のに・ても・けど） | | `JoshiConj` |
| 助詞-係助詞 / 副助詞 / 終助詞（は・も・こそ・さえ・か・ね・よ） | | `JoshiOther` |
| 接続詞（しかし・だから・また・そして・ただし） | | `Setsuzoku` |
| 助動詞（です・ます・た・ない・だ・である・れる・られる） | | `Jodoushi` |
| 動詞-非自立 / 補助（いる・ある・おく・しまう・くる・いく・くれる・もらう） | | `HojoYougen` |
| 連体詞（この・その・大きな） | | `Rentai` |
| 副詞 | | `Fukushi` |
| 記号-* / 補助記号 / 句点読点 | | `Kigou` |
| 辞書登録の英単語（M60） | | `English` |

実装メモ:

- **「が」格/接続の判別は cid が直接担う**: 格助詞「が」と接続助詞「が」は別 cid のため、
  表層 "が" だけでは区別できないが rcid → 品詞名で確定する。§4.1.2 の最重要ガードが頑健になる。
- **「て・で」**は連用接続後の**次文節 head（lcid）**が「動詞-非自立（補助用言）」か否かで判別。
- host はマッピング失敗（未知 cid / 辞書由来でない候補）時に `Unknown` を返し、ルールは
  表層フォールバック（§4.1.2）へ切替える。
- 代表 cid の数値は辞書ビルド依存のため**数値直書きせず** cid→品詞名表経由で解決する
  （`id.def` 差し替えに追従）。

#### 7.2.3 mid → SegmentSemantic（固有名詞の細分化・補助判定）

`pos` は構文カテゴリ（体言/用言…）を表すが、固有名詞の**意味細分類**（人名/地名/組織/製品/
日付/数）は句読点の一部判断に効く。これを `mid`（意味 ID）から導出する**任意の補助分類**
`SegmentSemantic` として持つ。`pos` を上書きはしない（補助シグナル）。

```cpp
// core/include/azookey/core/SegmentPos.h（SegmentPos と同居）
enum class SegmentSemantic : uint8_t {
  Unknown     = 0,
  Generic     = 1,   // 一般（固有性なし）
  PersonName  = 2,   // 人名
  PlaceName   = 3,   // 地名
  OrgName     = 4,   // 組織・団体名
  ProductName = 5,   // 製品・作品名
  DateTime    = 6,   // 日付・時刻
  Number      = 7,   // 数量
};
```

任意フィールド（既定 `0=Unknown`・後方互換）:

| field | 型 | 既定 | 説明 |
|---|---|---|---|
| `sem` | uint8 | `0` | 文節末尾自立語の `SegmentSemantic`（mid 由来） |
| `head_sem` | uint8 | `0` | 文節先頭自立語の `SegmentSemantic` |

句読点への補助適用（§4.1.4 の guard で参照。`prev_sem` / `next_head_sem`、§4.1.5）:

- **固有名詞連鎖の読点抑制**: `prev_sem` と `next_head_sem` がともに `PlaceName`（例「東京都→
  新宿区」）/ ともに固有名詞系 → 読点抑制（連鎖の途中に読点を入れない）。
- **日付・数の内部抑制**: `sem == DateTime` / `Number` の連なり（「2026年→6月→10日」）内部は
  読点を入れない（§4.1.3 の数値抑制を意味側からも補強）。
- **呼びかけ読点（任意・既定 OFF）**: `PersonName` + 敬称（さん/様）で文節が切れる呼びかけに
  読点（「田中さん、」）。誤爆しやすいため**既定ルールには入れず TSV で opt-in**。

host 側導出: `mid` → `SegmentSemantic` の対応表を mid 定義（§7.2.4）から作る。mid が
無い / 未知なら `Unknown`（補助判定は無効＝従来どおり pos のみで判断）。

#### 7.2.4 cid/mid 表のロード経路（`id.def`）

host は §7.2.2・§7.2.3 のマッピングを、辞書アセット同梱の **`id.def`（cid→品詞名）** と
**mid 定義表（mid→意味名）** から構築する。

- **配置**: 辞書 / モデルアセット群の一部として配布（zenz モデル・辞書と同じ場所）。アセット
  管理は M45（`docs/model-management-spec.md`）の対象。
- **形式（mecab / Mozc 系）**: `id.def` は 1 行 `<cid> <品詞-細分類,活用,…>`（例
  `1285 名詞,固有名詞,一般,*,*,*`）。mid 表は `<mid> <意味名>`（例 `501 人名`）。
- **ロード時機**: モデルロード（M8 `LoadModel`）時に host が読み、**cid→SegmentPos** と
  **mid→SegmentSemantic** の密配列（cid / mid を添字、値 1 byte）を構築する。lookup は O(1)。
- **名前→enum 変換**: §7.2.2 の「品詞名→SegmentPos」表は **品詞-細分類のプレフィックス一致**で
  適用（例 `名詞,固有名詞,*`→`Taigen`、`動詞,非自立,*`→`HojoYougen`、`助詞,接続助詞`→
  `JoshiConj`、`助詞,格助詞`→`JoshiCase`）。mid→`SegmentSemantic` も意味名一致で適用。
- **堅牢化**: `id.def` / mid 表が欠落・パース不能なら、全 cid / mid を `Unknown` とし、ルールは
  表層フォールバック（§4.1.2）で動作する（機能低下のみ・クラッシュしない）。
- **更新**: 辞書差し替え（M45）で `id.def` が変わったら配列を再構築する。cid / mid の数値は
  ビルド依存だが本経路（表経由）で吸収する。

### 7.3 キャンセル・staleness

ライブ変換経路のため、既存 M10 の `ipc_pending_id_` staleness check と `Cancel`
（`CancelPayload.target_request_id`）をそのまま使う（`docs/legacy-parity-spec.md` §2.5）。
古い応答は破棄。確定時は in-flight のライブ変換リクエストに `Cancel` を送ってから
`EndComposition`。

### 7.4 確定観測の整合（学習分離 / multi-segment commit）

句読点込み文の確定は、**M58-B と共有する `CommitSegmentsObservation`**
（`docs/romaji-batch-conversion-spec.md` §6.4）で行う。`!auto_punctuation` 文節を `chosen`、
自動句読点を `is_auto_punctuation=true`（`reading=""`、`chosen.surface="、"`/`"。"`）として
1 メッセージで原子的に送り、host は前者のみ Observe、後者は文脈連結のみ行う（§5.3）。
これにより §5.3 の学習不変条件を満たす。host が `commit_segments` capability 非対応のときは
TIP が `!auto_punctuation` 各文節を既存 `CommitObservation` で順次送る（§6.4.3 フォールバック）。
単一文節（句読点なし）確定は従来どおり `CommitObservation` を使う。

### 7.5 payloads_test 期待値（`ipc/tests/payloads_test.cpp`）

- `QueryCandidatesRequest` round-trip で `auto_punctuation` / `punctuation_style` が保存される。
- これらを欠く JSON のパースで `false` / `"ja"` の既定になる（後方互換）。
- `QueryCandidatesResponse` round-trip で `segments[]`（`auto_punctuation` マーカ・`surface`・
  `reading`・UTF-16 オフセット）が保存される。`segments` を欠く JSON は空 `segments` にパースされ、
  従来応答として解釈できる。各 `segments[].surface` の連結が `candidates[0].surface` と一致する。
- **学習スライスのバイト安全性** (host or 状態機械テスト): 日本語を含む確定で、各文節の学習
  surface が **`seg.surface` 文字列**から取られ、UTF-16 オフセットでの `substr` で壊れないこと
  （マルチバイト境界を割らない。§5.1・§5.3）。

## 8. 設定スキーマ

実装時に `settings/mvp-settings.schema.json` へ以下を追加する
（`additionalProperties:false` を維持。`description` に対応 M を記載する既存流儀に
合わせる）。本書（spec）が JSON schema の正典であり、実ファイルへの追加は M59 実装時に
行う（本セッションは設計確定のみでスキーマファイルは変更しない）。

| キー | 型 | 既定 | 説明 |
|---|---|---|---|
| `dynamicPunctuation` | boolean | `false` | M59: ライブ変換中に句読点を動的挿入・削除する（`liveConversion=true` のときのみ有効） |
| `dynamicPunctuationStyle` | enum `ja`/`fullwidth_latin` | `ja` | M59: 自動挿入する句読点の字種（`、。` / `，．`） |
| `dynamicPunctuationStability` | enum `onPause`/`eager` | `onPause` | M59: 挿入タイミング（入力停止時のみ / 打鍵ごと） |
| `dynamicPunctuationIdleMs` | integer (ms) | `400` | M59: `onPause` の idle タイマー閾値。最後の打鍵からこの時間で句読点を再評価・挿入（§4.3.1） |
| `segmentBoundaryConfidence` | number (0.0–1.0) | `0.5` | M59: この境界スコア未満の文節境界には読点を挿入しない（§4.1 抑制規則） |
| `punctuationRulesPath` | string | `%LOCALAPPDATA%\azooKey\punctuation-rules.tsv` | M59: 句読点ルール TSV のパス（§4.1.4）。無ければ組み込み既定のみ。ホットリロード対応 |

## 9. テスト計画

- **句読点挿入ロジック** (`core/tests/punctuation_inserter_test.cpp` 新規 or
  host テスト): 文節境界・接続表現での読点挿入、文末句点、連続読点の抑制、字種切替
  （`ja` / `fullwidth_latin`）。
- **品詞ガード** (同上): `pos`/`head_pos` 駆動で「が」の格助詞（読点なし）/接続助詞（読点）、
  「て・で」の補助用言接続（読点なし）/連用中止（読点）が分岐すること。`pos=Unknown` で
  表層フォールバックに切替わること。
- **cid → SegmentPos マッピング** (host テスト): 代表 cid（例 1285=固有名詞→`Taigen`）と
  格助詞/接続助詞「が」の別 cid が正しい `SegmentPos` に写ること、未知 cid が `Unknown` に
  なること（§7.2.2）。`id.def` 欠落・パース不能時に全 cid が `Unknown` へ縮退すること（§7.2.4）。
- **mid → SegmentSemantic 補助判定** (host テスト): 地名連鎖（`prev_sem=next_head_sem=PlaceName`）
  で読点抑制、日付連なりの内部抑制、mid 欠落時に `Unknown`（補助無効）になること（§7.2.3）。
- **TSV ルール外部化** (`core/tests/punctuation_rules_test.cpp` 新規): TSV のパース、`(kind,match)`
  上書き・新規追加・`base_score=0` 無効化、不正行スキップ、字を含めない（字種は
  `dynamicPunctuationStyle` 由来）こと。
- **guard EBNF 評価** (同上): `=`/`!=`/`;`(AND)・空 guard 常真・`sentence_final`・`prev_sem` /
  `next_head_sem`、Unknown pos/sem での評価バイアス（`=`→偽 / `!=`→真）、未知トークン行
  スキップ（§4.1.5）。
- **状態機械** (`core/tests/input_state_test.cpp`): `dynamicPunctuation` ON で
  Backspace がかな単位を削除し自動句読点を削除単位に数えないこと、入力変化で句読点が
  再配置・削除されること、`liveConversion=false` で本機能が無効化され従来遷移が不変で
  あること、OFF で従来遷移が不変であること。
- **IPC** (`ipc/tests/payloads_test.cpp`): ライブ変換要求（現状 `QueryCandidates`、§7）の
  `auto_punctuation` / `punctuation_style` フィールド、応答 `segments[]`（`auto_punctuation` /
  `surface` / `reading`）の build/parse 往復。`CommitSegmentsObservation` の往復（§7.4）。
- **学習分離** (`learning/tests`): 自動句読点を含む確定で、句読点が `(reading, surface)`
  観測に混入しないこと。
- **安定化** (host テスト or 状態機械): `onPause` でタイピング中は句読点が出ず、idle で
  挿入されること（`TypingTempoTracker` をモックした時間注入）。
- **idle タイマー / timing 符号化** (状態機械): `onPause` で**打鍵 Input リクエストは
  `auto_punctuation=false`**（抑制）、最後の打鍵後の **`IdleTimeout` は `auto_punctuation=true`**
  のライブ変換要求が 1 回 post され句読点が挿入されること、次の打鍵でタイマーが再アーム
  されること、`eager` では打鍵ごとに `auto_punctuation=true` で挿入されること、`commit` 前の
  最終評価が `auto_punctuation=true` であること（§4.3.1・§7.1.1）。
- **手動 / 実機（Win11、`gate:human-required`）**: ライブ変換 ON + `dynamicPunctuation`
  ON で文を打つと節境界・文末に句読点が現れ、続けて打つと再配置・削除され、Enter で
  妥当な句読点付き日本語が確定する。OFF で句読点が一切自動挿入されない。学習が汚染
  されない（直後に同じ読みを打って句読点なしの素直な候補が出る）。

## 10. 受け入れ条件（M59）

M59 の受け入れ条件は `plans/windows-port-roadmap.md` の M59 に定義する（本書は仕様、
roadmap は受け入れ条件の「定義」、達成状態は Linear）。

## 11. 参照

- ライブ変換 旧実装: `legacy/Core/Sources/Core/InputUtils/SegmentsManager.swift`
- 句読点字種設定（本機能とは別物）: `legacy/Core/Sources/Core/Configs/PuntuationConfigItem.swift`
- ライブ変換仕様: `docs/legacy-parity-spec.md` §2
- リッチ化（信頼度 segments・TypingTempoTracker）: `docs/rich-features-spec.md` X-1
- 一括 AI 整文の句読点（別経路）: `docs/romaji-batch-conversion-spec.md` §5・M58-C
