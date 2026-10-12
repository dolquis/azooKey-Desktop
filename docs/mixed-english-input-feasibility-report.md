<!-- lint:allow-file line-ref,status -->
<!-- 本ファイルは作成日時点の調査スナップショットであり、更新しない前提の記録である。 -->

# 日英混在ローマ字入力の自動変換 実現可能性調査

> **種別**: 実現可能性と先行事例の調査（特定時点のスナップショット）
> **作成日**: 2026-10-12
> **位置づけ**: 本ファイルは時点依存の調査記録であり、正典ではない。
> - マイルストーン定義・受け入れ条件・リスクの正典 → `plans/windows-port-roadmap.md` の M63
> - 機能仕様の正典 → M63-0 が新設する `docs/*-spec.md`（1 語の候補注入は `docs/inline-english-candidate-spec.md`）
> - 進捗・状態・優先度の正典 → Linear（team `Dev`）
>
> 調査方法: リポジトリの実ファイル（`core/`、`inference-host/`、`ipc/`、`tsf-tip/`、関連 spec）を
> 読み、`RomajiKanaConverter::ConvertForCommit` を単体でビルドして英単語の変換結果を確かめた。
> 先行事例は Web 上の公開情報から集めた。実 Zenzai モデルによる測定は行っていない
> （本調査の環境にモデルが無いため）。

## 1. 要求

ユーザーとの合意（2026-10-12）:

1. 英語の可能性が高い語は、ライブ変換と第 1 候補で英字になる。日本語の可能性が高い語は日本語が
   第 1 候補、英字が第 2 候補になる。
2. 1 文の中に日本語と英単語を混ぜて打てる（例: `kyouhagithubnipushsita` → 「今日はGitHubにpushした」）。
3. ローマ字としても読める英単語（`kite` / 来て など）は既定で日本語として扱う。先頭を Shift で
   大文字にして打った語は英語として扱う。
4. 対象アプリを限定しない。
5. 英単語辞書の出典に指定は無い（再配布可能なものを選ぶ）。

## 2. 結論

段階を分ければ実現できる。可否は語の種類で分かれる。

| 区分 | 例 | 判別の手がかり | 見通し |
|---|---|---|---|
| ローマ字として読めない英単語 | `github`、`apple`、`thread`、`push` | ローマ字かな変換で英字が残る、子音連続、辞書一致 | 高い。M60 の英語意図シグナルがそのまま使える |
| Shift 先頭大文字で打った語 | `Kite`、`Made` | 打鍵の大文字（M60 の `s_case`） | 高い。ユーザーの明示的な合図 |
| ローマ字として読める英単語 | `kite`、`same`、`made`、`take` | 文脈のみ | 既定は日本語（要求 3）なので、英字を第 2 候補に置ければ足りる。文脈で英語を第 1 候補へ上げるのは Zenzai の測定次第 |
| 文中の区間境界 | `githubni` の `github` / `ni` | 分割の候補を作って採点する | 中程度。新しい分割器と IPC 拡張が要る。最大の工数 |

最大の不確定要素は、Zenzai（zenz-v3 系）が英字を含む入力を英字のまま出力できるか、
または NllScorer による採点で日英の表層を判別できるかであり、実モデルで測るまで確定しない。
そのため M63-0 を測定と仕様化の段階として最初に置く。

先行事例として、macOS 標準の日本語入力（High Sierra 以降）がライブ変換の上で日英の連続入力を
製品化しており、研究では日本語ローマ字と英語の区間判別（池上・鶴田）、中国語ピンインと英語の判別
（Xia・Cheung）が高い精度を報告している（§5）。区間の判別は既知の手法で解ける見込みが高く、
難所は判別した区間を Zenzai の変換とライブ変換の UX にどう載せるかにある。

## 3. 既存実装の棚卸し（M63 が再利用するもの）

| 素材 | 所在 | M63 での使い道 |
|---|---|---|
| 英単語候補の生成と英語意図スコア | `inference-host/src/EnglishCandidates.cpp`（`ComputeEnglishIntent`、`BuildEnglishCandidates`） | 区間ごとの英語らしさの判定。シグナルは辞書一致、ローマ字の解析失敗、子音連続、長さ、大文字 |
| 英単語辞書（TSV・`.bin`・overlay・ロック） | `inference-host/src/EnglishDictionaryStore.cpp`、`EnglishDictionaryFiles.cpp` | 区間候補の辞書照合と表層（`GitHub` などの大小）の決定 |
| English 学習チャネル | `docs/inline-english-candidate-spec.md` §5 | 英語区間の確定の学習。かな漢字学習と混線しない |
| 生ローマ字の送信（`raw_romaji`） | `ipc/src/Payloads.cpp`（`QueryCandidates`、`QueryBatchConversion`） | 分割の素材。`live = true` の問い合わせには載らない |
| `CandidateTag::English` | `core/include/azookey/core/Candidate.h` | 英字候補の識別と `[英]` バッジ |
| ライブ変換のチャンク分割（Convert / Verbatim） | `core/src/LiveConversionChunker.cpp` | 英語区間を Verbatim チャンクとして素通しする候補。本調査の時点では `core/tests` からだけ呼ばれ、TIP / Host のライブ変換経路には配線されていない。配線は M63 の工数に入る |
| NllScorer（Zenzai による表層の NLL 採点） | `inference-host/src/NllScorer.cpp`、`docs/neural-reranker-spec.md` Track B | 分割候補や日英表層の文脈込みの比較。Track B はライブ変換（`live=true`）を対象外とし、採点対象を辞書由来の候補に限る（§B1・§B5）ため、ライブ変換で使うには適用範囲の拡張が要る |
| 複数区間の確定観測 | `docs/romaji-batch-conversion-spec.md` §6.4（`CommitSegmentsObservation`） | 区間ごとの学習 |
| 評価ベンチ | `bench/zenzai_bench.cpp`、`bench/nll_bench.cpp`、`bench/ConversionQuality.cpp` | 混在文の評価セットでの測定 |

## 4. 技術的な課題

### 4.1 英単語はかなの読みに変換された時点で崩れる

`RomajiKanaConverter::ConvertForCommit` に英単語を与えた結果（本調査で単体ビルドして確認）:

| 入力 | かな |
|---|---|
| `github` | `ぎてゅb` |
| `apple` | `あっpぇ` |
| `thread` | `thれあd` |
| `push` + `sita` | `ぷshした` |
| `repository` | `れぽしとry` |
| `kyouhagithubnipushsita` | `きょうはぎてゅbにぷshした` |
| `kite` / `same` / `made` / `take` | `きて` / `さめ` / `まで` / `たけ` |

- 英字が残る語は英語らしさの強い信号になるが、残る位置は語の一部（`github` では末尾の `b` だけ）で
  あり、残った英字の位置から区間の境界は決まらない。分割は生ローマ字の上で行う必要がある。
- Host へ届く `reading` と、ライブ変換の `kana` はこの崩れた読みであり、Zenzai も同じ読みを見る。
  Zenzai に英語区間を見せるには、分割後に英語区間を英字のまま入力へ入れる（§4.3）。
- `kite` などはかなに完全に変換できるため、ローマ字の解析だけでは英語と判別できない。
- `l` / `q` / `v` / `x` は内蔵のローマ字表で正規の入力である（`ltu` → `っ`、`qa` → `くぁ`、
  `vaiorin` → `ゔぁいおりん`）。文字の種類だけで英語と判定すると日本語入力を奪う。`love` → `ぉゔぇ` の
  ように、かなには変換できるが不自然な読みになる語もある。

### 4.2 生ローマ字はライブ変換に届かない

M60 は生ローマ字を `live = false` の `QueryCandidates` にだけ載せる
（`docs/inline-english-candidate-spec.md` §4.1）。ライブ変換（`QueryLiveConversion`）は `kana` だけを
送るため、要求 1（ライブ変換で英字）には IPC の拡張か、TIP 側での分割が要る。

### 4.3 Zenzai が英字を扱えるか

- zenz のプロンプトは `[CTX]<文脈>[IN]<カタカナの読み>[OUT]` であり（`docs/zenzai-inference-spec.md`
  §3.2）、入力の英数は前処理で保持される（同 §3.1）。英語区間を英字のまま `[IN]` に入れる
  （例: `キョウハgithubニpushシタ`）ことは形式上できる。
- その入力で英字がそのまま出力されるか、`GitHub` のような大小が再現されるかは、学習データに
  依存し、本調査では確かめていない（§5.3 参照）。
- 学習データの読みが MeCab + ipadic-NEologd で作られているなら（§5.3）、英単語はカタカナ読み
  （`ギットハブ`）として入力に現れ、英字表層（`GitHub`）として出力に現れていると推測される。
  その場合は、英語区間を辞書でカタカナ読みへ置き換えて `[IN]` に入れる方式も候補になる。
- 代替として、NllScorer で「日本語の表層」と「英字の表層」を同じ文脈の下で採点し、NLL の差で
  選ぶ方式がある。NllScorer は辞書候補のリランクに使う既存の仕組みで、prefix KV を共有するため
  候補数に対するコストが生成より小さい。

### 4.4 分割の担い手

| 案 | 利点 | 欠点 |
|---|---|---|
| TIP で分割し、区間ごとの読みと英字を Host へ送る | `inputStyle=custom` の表が TIP にある。生ローマ字とかなの対応を TIP が持つ | TIP が辞書を持たないため、英語らしさの判定が粗くなる。TIP の処理が増える |
| Host で分割し、TIP は生ローマ字を送る | 辞書・学習・Zenzai が Host にあり、分割と採点を一か所で行える | Host はカスタムローマ字表を持たない（M60 §4.1.1 と同じ制約）。表を Host へ渡す仕組みが要る |
| TIP は区間の境界候補だけを作り、Host が採点する | 両者の長所を取れる | IPC が最も複雑になる |

M63-0 の spec で、レイテンシとカスタム表の扱いを根拠に決める。

### 4.5 レイテンシ

ライブ変換は打鍵ごとに走る。分割候補ごとに Zenzai を回すと推論回数が増える。分割候補の上限、
確定済みの区間を再計算しない増分処理、予算超過時に M60 相当の候補注入へ落とす打ち切りを
spec で定める必要がある。ライブ変換の待ち時間は `docs/dev-infrastructure-spec.md` §8.5.2 の timeout 表
（150ms。client が打ち切る）が正典で、Zenzai 1 変換の予算（`docs/zenzai-inference-spec.md` §8）より短い。

### 4.6 辞書

英単語辞書は同梱されていない（`data/` には `emoji.tsv` と `symbol.tsv` だけがある）。
M63 は辞書なしでもローマ字として読めない語を判別できるが、区間の切り出しと表層の大小
（`GitHub`、`iPhone`）には辞書が要る。再配布可能な出典の候補は §5.4 に挙げる。

## 5. 先行事例

Web 上の公開情報から集めた。製品の現行版の挙動は実機で確かめていない。

### 5.1 日本語 IME

| 事例 | できること | 判別の仕方 | 出典 |
|---|---|---|---|
| macOS 標準の日本語入力（High Sierra 以降） | ひらがなモードのまま英単語を打つと英語として認識して変換する。ライブ変換が有効なら、変換・確定を挟まずに日本語と英語を続けて入力できる。英単語の後の空白は半角、日本語の後は全角と自動で判断する | 公開情報なし。よく使われる語が対象で、`macOS` のような大小の混在には弱いという利用者の指摘がある | [Mac Fan Portal（2017-10-19）](https://macfan.book.mynavi.jp/article/m79600) |
| Google 日本語入力（2013 年の開発版 1.12 以降） | ローマ字入力中に日本語ではないと思われる文字列を打つと、かなに変換する前の英字を「もしかして」候補に出す（`あdみにstらとr` → `administrator`）。自動で英字にはしない | 日本語ではないと思われる文字列の判定（詳細は非公開） | [窓の杜（2013-11-11）](https://forest.watch.impress.co.jp/docs/news/20131111_623062.html) |
| Google 日本語入力（2010 年のベータ版以降） | ひらがなで打ったカタカナ英語を英字の綴りへ変換する（`あどみにすとれーたー` → `Administrator`） | 辞書（読みから英字表層への対応） | [窓の杜（2010-01-29）](https://forest.watch.impress.co.jp/docs/news/20100129_345718.html) |
| ATOK | 英語入力支援（ATOK 4E）、F4 での英単語変換、日本語入力 OFF で打ったローマ字の後からの日本語化 | 打鍵中の自動判別は公開情報で確かめられなかった | [窓の杜（2014-02-10）](https://forest.watch.impress.co.jp/docs/news/20140210_634624.html) |
| azooKey 本家（`AzooKeyKanaKanjiConverter`） | `englishCandidateInRoman2KanaInput` による英単語候補（M60 の元）。macOS 版はこのオプションを `false` にしている（`legacy/Core/Sources/Core/InputUtils/SegmentsManager.swift`） | 候補の追加。自動の英字化はしない | `legacy/`、[AzooKeyKanaKanjiConverter](https://swiftpackageregistry.com/azooKey/AzooKeyKanaKanjiConverter) |

macOS の挙動はユーザーの要求 1・2 とほぼ一致しており、製品として成立している先行例である。
判別手法は公開されていない。

### 5.2 研究

| 研究 | 内容 | 結果 | 出典 |
|---|---|---|---|
| 池上・鶴田「Hybrid method for modeless Japanese input using N-gram based binary classification and dictionary」（Multimedia Tools and Applications） | ローマ字の各区間をかなへ変換するかどうかを、文字 n-gram（表層・字種・履歴）を特徴にした SVM の二値分類と、Web から作った非日本語語の辞書で判定する。`nekohacatdesu` → `ねこはcatです` | 非かな文字の予測の F 値が n-gram だけの方式より 7.7% 向上。利用者試験で、手動のモード切替より入力時間の評価が高い | [Springer](https://link.springer.com/article/10.1007/s11042-013-1805-1)、[発表資料](https://www.slideshare.net/slideshow/modeless-japanese-input-method/49419726) |
| 同著者「Modeless Japanese input method using multiple character sequence features」（IEEE SITS、2012） | 上記の前身。複数の文字列特徴による判定 | — | 上記論文の参考文献 |
| Xia・Cheung「Accurate Pinyin-English Codeswitched Language Identification」（CodeSwitch Workshop、2016） | 中国語ピンインと英語が混ざった入力の言語判別 | 語単位は SVM で F1 99.3%、文字単位は線形連鎖 CRF で F1 98.2%。言語判別は混在入力 IME のボトルネックではなく、ピンインから漢字への変換が難所だと結論している | [ACL Anthology W16-5809](https://aclanthology.org/W16-5809/) |

日本語ローマ字と英語の判別は、ピンインと英語の判別と構造が近い。研究は「区間の言語判別は
文字単位の系列モデルで高精度に解ける」ことを示しており、azooKey では判別した英語区間を
かな漢字変換から外し、日本語区間の変換を Zenzai に任せる構成が自然である。

### 5.3 zenz モデル

- Hugging Face のモデルカード（`Miwa-Keita/zenz-v3.1-small`、`zenz-v3.2-small-gguf` など）には
  ライセンス以外の記載がなく、学習データの構成は公開情報から確かめられなかった。
- `Miwa-Keita/zenz-v1` のカードによれば、基盤は `ku-nlp/gpt2-small-japanese-char` で、読みの生成に
  MeCab と ipadic-NEologd を使っている（[zenz-v1](https://huggingface.co/Miwa-Keita/zenz-v1)）。
  zenz-v3 も同じ方式なら、学習データの英単語は NEologd のカタカナ読み（例: `GitHub` → `ギットハブ`）を
  入力に持つと推測される。これが正しければ、英字のまま `[IN]` に入れるより、英語区間を辞書で
  カタカナ読みへ置き換えて入力し、出力の英字表層を受け取る方が学習分布に近い。推測であり、
  M63-0 の測定で確かめる（§4.3）。

### 5.4 英単語辞書の出典候補

DEV-1593 で一次情報を確認する前提の候補。

| 出典 | ライセンス（公開情報からの要約） | 備考 | 出典 |
|---|---|---|---|
| SCOWL | 著作権表示と許諾表示を残せば、無償で使用・複製・改変・配布・販売できる | 綴りの一覧で、頻度はサイズ区分のみ | [Debian の copyright](https://sources.debian.org/src/scowl/7.1-1/Copyright/) |
| wordfreq | コードとデータで条件が異なる。一部データは CC BY-SA 4.0 で、出典表示の条件がある | 頻度つき。ShareAlike の扱いを確認する必要がある | [NOTICE](https://cdn.jsdelivr.net/npm/nodewordfreq@0.2.1/NOTICE.md) |
| Google Books Ngram | 出典表示で自由に使えるとされる | 頻度の素材。語彙の抽出と正規化が要る | wordfreq の NOTICE の記載 |

## 6. 判別手法の選択肢

| 手法 | 内容 | 長所 | 短所 |
|---|---|---|---|
| H1 ヒューリスティック | ローマ字の解析失敗・子音連続・辞書一致・Shift 大文字（M60 の英語意図スコア） | 実装済みの素材で軽い。ローマ字として読めない語に強い | ローマ字として読める英単語を判別できない。区間境界を決められない |
| H2 混合ラティス + 辞書 | 生ローマ字の上に「ローマ字としてかなへ変換する辺」と「英単語辞書に一致する辺」を張り、最良経路を探す | 文中の区間境界を決められる。中国語ピンイン IME の中英混在と同じ構造 | 経路の採点にコストモデルが要る |
| H3 ラティス + LM 採点 | H2 の上位 N 経路を Zenzai（または NllScorer）で文脈込みに採点する | 曖昧語を文脈で判別できる可能性がある | 推論回数が増える。Zenzai の英字の扱いに依存する |
| H4 分類器 | 区間の英語らしさを文字単位の系列モデル（文字 n-gram の SVM、CRF 等）で判定する。§5.2 の池上・鶴田と Xia・Cheung の方式 | 速い。辞書に無い語も扱える。研究で高い精度が報告されている | 学習データと配布物が増える。学習データは日本語コーパスを MeCab で読みにし、ローマ字化して作れる（§5.2）。M56 の基盤と重なる |

## 7. 推奨

1. **M63-0**: 実 pin モデルで次を測る。(a) 英語区間を英字のまま `[IN]` に入れたときの出力、
   (b) 英語区間をカタカナ読みへ置き換えて入れたときの出力（§5.3 の仮説）、(c) NllScorer による
   日英表層の判別精度、(d) H2 の分割精度と、H4（文字単位の分類器、§5.2）の判別精度。評価セットは
   混在文・日本語だけの文・曖昧語を含む文で作り、M52 の評価形式に載せる。辞書の出典とライセンスを
   確定する。結果から分割の担い手と IPC を決め、spec を新設する。
2. **M63-A**: 1 語の自動英字化。H1 で高確度の語（ローマ字として読めない語、Shift 先頭大文字）を
   ライブ変換と第 1 候補にし、それ以外は英字を第 2 候補に置く。Zenzai の測定結果に依存しない
   範囲から始められる。
3. **M63-B**: 文中混在。H2（必要なら H4 を併用）で分割し、M63-0 の結果が良ければ H3 で曖昧語を
   文脈判別する。結果が悪ければ、曖昧語は要求 3 のとおり日本語を第 1 候補に固定し、英字を
   第 2 候補に置く。英語区間を Zenzai の外で確定し、日本語区間だけを Zenzai で変換する構成は、
   §5.2 の研究が示す「言語判別と変換の分離」と同じであり、Zenzai の測定結果が悪い場合の基本形になる。

macOS 標準の日本語入力（§5.1）は、要求 1・2 とほぼ同じ体験をライブ変換の上で実現しており、
UX の参照先になる（英語区間の前後の空白を半角・全角で自動判断する挙動を含む）。

## 8. M63-0 で決める設計上の問い

- 分割を TIP と Host のどちらで行うか（§4.4）。カスタムローマ字表の扱い。
- ライブ変換の IPC に生ローマ字または区間情報をどう載せるか。旧 Host / 旧 TIP との互換。
- 区間単位の候補ウィンドウ: 英語区間の第 2 候補（日本語）を文節単位で選べるようにするか。
- 英語区間の前後の空白（`今日は GitHub に` か `今日はGitHubに` か）。既定と設定。macOS 標準の
  日本語入力の挙動（英単語の後は半角、日本語の後は全角。§5.1）を参照する。
- Backspace や読みの補正で生ローマ字とかなの対応が崩れたときの戻し方（M60 §4.1 と揃える）。
- 学習: 区間ごとの確定の記録先、ユーザーが英字を日本語へ選び直したときの次回の順位。
- M58（一括変換）との統合: 一括変換でも同じ分割を使うか。
- 設定キーと既定値、M60 の設定との関係。M63 を `inlineEnglishCandidates` が ON のときだけ有効に
  するか、独立させるか（TIP は M60 の設定が ON のときだけ生ローマ字を保持する）。M60 の spec
  （§2・§4.1・§4.3）と受け入れ条件へ「M63 の設定が OFF のとき」の限定を加える範囲。
- かなには変換できるが不自然な読みになる語（`love` → `ぉゔぇ`）を英語らしさの信号にするか。
- 打ち間違い補正（M35 / M55）との優先。ローマ字かな変換の後に英字が残ることは両方の信号になる。
- レイテンシ予算と打ち切り、劣化時の fallback。
