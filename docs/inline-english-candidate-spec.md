# ローマ字入力中インライン英単語候補 仕様（追加機能 / M60）

本書は azooKey-Desktop Windows 版の「ローマ字入力中インライン英単語候補（Inline
English Candidate in Romaji Input）」機能を定める。`plans/windows-port-roadmap.md` の
M60 が本書を参照する。本書は機能仕様（IPC payload・設定項目・ユーザー可視挙動）の
正典であり、進捗・状態は持たない（状態の正典は Linear。
運用規約は `docs/linear-conventions.md` を参照）。

関連 spec:

- `docs/legacy-parity-spec.md` … 候補ウィンドウ（§1.3）、ライブ変換（§2）、
  カスタムローマ字テーブル（§5）。本機能は候補生成・確定動線を土台にする。
- `docs/rich-features-spec.md` … X-2-3（ラベル付き候補・`CandidateTag::English`）。
  英単語候補の識別・バッジ表示に再利用する。
- `docs/romaji-batch-conversion-spec.md` … §4.1（生ローマ字バッファの常時保持）。
  本機能の英単語候補は同じ生ローマ字バッファを基にする。

## 1. 目的（と背景）

日本語ローマ字入力中に、**英数モードへ切り替えることなく**英単語を入力できるように
する。ユーザーがローマ字で `apple` と打つと、かな漢字候補（あっぷる / アップル …）に
加えて英単語候補（`apple` / `Apple` …）を候補列へ注入し、ユーザーが選べば英単語が
そのまま確定する。

これは azooKey 本家 `ConvertRequestOptions.englishCandidateInRoman2KanaInput`
（`legacy/Core/Sources/Core/InputUtils/SegmentsManager.swift`、macOS 版では
`false` で無効）に相当する機能の Windows 版実装である。Windows 版は Swift の
`KanaKanjiConverterModule` を使わず独自 C++ core + Zenzai 構成のため、同等機能を
再実装する。

## 1.1 スコープと非目標

- **スコープ: 候補注入のみ（1 語単位）。** 生ローマ字バッファ 1 つ（語の区切りまで）に
  対して英単語候補を生成し、候補列へ注入する範囲を本書の対象とする。
- **非目標（将来課題）: 連続英文タイプ。** スペースを含む複数語の英文を Japanese モードの
  まま連続入力する体験（語間スペース処理・文単位英語予測）は本書の対象外。将来、
  `requireEnglishPrediction`（legacy）相当の英語予測や M58 一括変換との統合として
  別マイルストーンで検討する（§9）。
- **非目標: 自動モード切替・自動言語判定。** 英単語を検出して英数 IME へ自動で切り替える
  ことはしない。あくまで「追加候補」をユーザーが明示的に選ぶ方式（azooKey と同じ）。
- **非目標: 英語スペル訂正・補完。** 辞書ゲーティング（§4.2）を超えるスペル補正や
  単語補完は対象外（将来課題）。
- **既定 OFF。** 既定 OFF で、有効化時のみ候補を注入する。OFF のとき候補生成・確定の
  挙動は一切変えない。

## 2. 設計原則

- **生ローマ字バッファを基にする。** TIP はかなバッファとは別に**生ローマ字（半角英字）
  バッファを常時保持**する（`docs/romaji-batch-conversion-spec.md` §4.1 が確立した
  方針を共有・再利用）。英単語候補はこの生ローマ字を素材に生成する。
- **決定的ベースライン + 辞書品質向上の二層。** まず生ローマ字そのもの + 大文字化
  バリアント（決定的・辞書不要）を必ず提示できるベースラインとし、英単語辞書による
  ランキング・ゲーティングを上位品質レイヤとする（SimpleConverter → Zenzai と同じ
  段階導入方針）。辞書が無くても最低限の英単語入力が成立する。
- **日本語候補を妨げない。** 英単語候補は**追加候補**であり、明確な英語意図シグナルが
  ない限り日本語候補より上位に出さない。誤って英単語が第一候補を奪わない（§4.3）。
- **既定 OFF・後方互換。** OFF のとき候補生成・rerank・確定を一切変えない。

## 3. 候補生成の素材

大小と幅のバリアントは、生ローマ字 `r` そのものではなく `lower(r)` を基準に生成する。

`r` は Shift 併用打鍵を保持する（§4.2 の `s_case` がこれをシグナルに使う）。`r` を基準にすると
`Apple` と打ったときに小文字形が 1 つも出ず、下表の 4 形（半角小文字、半角全大文字、全角小文字、
全角全大文字）が成立しない。`lower(r)` を基準にすれば、打鍵の大小によらず同じ 6 形が出る。

`apple`、`Apple`、`APPLE` のいずれを打鍵した場合も、生成しうる英単語候補は次のとおりである。

| 種別 | 例 | 生成条件 |
|---|---|---|
| 半角小文字 | `apple` | 常時（決定的ベースライン。`lower(r)`） |
| 半角先頭大文字 | `Apple` | `inlineEnglishCaseVariants` ON |
| 半角全大文字 | `APPLE` | `inlineEnglishCaseVariants` ON |
| 全角小文字 | `ａｐｐｌｅ` | `fullWidthEnglishCandidate` ON（legacy `fullWidthRomanCandidate` 相当） |
| 全角先頭大文字 | `Ａｐｐｌｅ` | `fullWidthEnglishCandidate` ON かつ `inlineEnglishCaseVariants` ON |
| 全角全大文字 | `ＡＰＰＬＥ` | `fullWidthEnglishCandidate` ON かつ `inlineEnglishCaseVariants` ON |
| 生ローマ字そのもの | `aPPle` | `r` が上の 6 形のいずれとも表層形が一致しないとき |
| 辞書一致語 | `iPhone` | `inlineEnglishDictionary` ON（品質レイヤ） |

`aPPle` のような打鍵は上の 6 形のどれとも一致しないため、打った文字列そのものを選べるように
`r` を候補へ加える。`apple` や `APPLE` のように 6 形のいずれかと一致する打鍵では、重複除去
（§4.3）によってこの行は現れない。

全候補に `CandidateTag::English`（X-2-3）を付与し、候補 UI で `[英]` バッジを出せる
ようにする。

M62-B（`docs/candidate-rewriter-spec.md` §18）が挙げる英字の 4 バリアント（半角の小文字、
半角の大文字、全角の小文字、全角の大文字）は、上表の 半角小文字、半角全大文字、全角小文字、
全角全大文字 の 4 行に対応する。先頭大文字の 2 行は、それに加えて本書が持つ形である。

M62-B の英字分はここへ統合し、TIP ローカルに別経路の英字候補生成を置かない。同じ打鍵に対して
2 つの経路が候補を注入すると、順序と重複除去をどちらが決めるかが定まらないためである。M62-B が
受け持つのはカタカナ分だけである。

## 4. 注入・ランキング・ゲーティング

### 4.1 注入対象の経路

- **候補ウィンドウ（M5）**: `StartConversion`（Space）で候補列に英単語候補を注入する
  （主経路）。
- **ライブ変換（M14）/ 予測（M15）（任意）**: 実装時に判断。M60 コアは候補ウィンドウ
  注入を必須とし、ライブ/予測への注入は任意拡張とする。

### 4.2 ゲーティング（いつ出すか）

あらゆるローマ字に英単語候補を出すと雑音になる（例: `ko` で英語を出さない）。host 側で
生ローマ字 `r` から**英語意図スコア** `english_intent ∈ [0,1]` を算出し、注入可否と順位を
決める。

シグナル（各 `[0,1]`）:

| シグナル | 定義 |
|---|---|
| `s_dict` | 英単語辞書に `lower(r)` がヒット = 1、else 0（`inlineEnglishDictionary` OFF 時は常に 0） |
| `s_nonkana` | `r` を `RomajiKanaConverter` で完全にかな分解できず ASCII 残余が出る = 1、else 0 |
| `s_cluster` | 英語特有の子音連続/2-gram（`th` `ck` `wr` `ght` 等）の出現割合 |
| `s_len` | `clamp((len(r) - inlineEnglishMinLength) / 4, 0, 1)`（長いほど英語らしい） |
| `s_case` | `r` が大文字を含む（Shift 併用打鍵）= 1、else 0 |

合成（重みは既定。bench M52 / 実機で調整。本式が既定の正典）:

```text
english_intent = 0.40*s_dict + 0.25*s_nonkana + 0.15*s_cluster
               + 0.10*s_len  + 0.10*s_case
```

注入可否（gating）:

```text
if !inlineEnglishCandidates:           英単語候補なし
if len(r) < inlineEnglishMinLength:     英単語候補なし（既定 2）
else:
    半角小文字 lower(r) 候補は常に生成可（順位は §4.3）
    半角の先頭大文字 / 全大文字は inlineEnglishCaseVariants ON 時
    全角小文字は fullWidthEnglishCandidate ON 時
    全角の先頭大文字 / 全大文字は fullWidthEnglishCandidate と inlineEnglishCaseVariants が共に ON 時
    生ローマ字 r そのものは、上のどの形とも表層形が一致しないとき（例: aPPle）
    辞書一致語（追加候補・score 上乗せ）は inlineEnglishDictionary ON 時
```

辞書非搭載（`inlineEnglishDictionary=false`）でも `s_dict=0` のまま他シグナルで
ベースライン動作する（生ローマ字 + 大文字化は出せる）。

### 4.3 順位（日本語を奪わない）

```text
threshold_promote = inlineEnglishPromoteThreshold（既定 0.6）
if english_intent >= threshold_promote:
    英単語候補を日本語第 1 候補の直後（index 1 付近）へ昇格
else:
    英単語候補を日本語候補群の後ろ（固定下位。例: 上位 5 件の後）へ
自動選択はしない（第 1 候補を英単語へ置換しない。ユーザー明示選択時のみ確定）
順位安定化: 同一 r では順位を固定（マッスルメモリ）。english_intent はしきい値を
           跨いだときのみ段階的に順位が変わる（連続値での微振動で順位を動かさない）
```

候補内の英語形の並び（固定順）:

```text
（辞書一致語があればその surface を先頭に置く）
（生ローマ字 r が下の 6 形のいずれとも一致しなければ、次に r 自体を置く）
半角小文字 → 半角先頭大文字 → 半角全大文字 → 全角小文字 → 全角先頭大文字 → 全角全大文字
（全角の 3 件は fullWidthEnglishCandidate ON 時。うち大小 2 件は inlineEnglishCaseVariants も
 ON のときだけ生成する。半角側をすべて出してから全角側を出す）
（重複除去は表層形で行い、この固定順で先に現れたものを残す）
```

辞書一致語は `lower(r)` 由来の 6 形を差し替えず、別の候補として先頭へ置く。辞書 surface が
`iPhone` のような独自の大小を持つとき、差し替えにすると小文字形が候補から消え、§3 の 4 形の
契約が崩れるためである。辞書 surface が 6 形のいずれかと一致する場合は、重複除去でこの行が
消え、順位の上乗せ（`s_dict`）だけが残る。

エッジ:

- ローマ字未確定（`RomajiKanaConverter` に pending がある。例: `n` 終端）でも、`raw_romaji`
  にその ASCII を含めて英単語素材にする。
- 記号/数字混在は `s_nonkana` に寄与しうるが、英字を 1 文字も含まない純記号/純数字列は
  `inlineEnglishMinLength` と「英字含有」チェックで除外する。

### 4.4 英単語辞書フォーマット（`inlineEnglishDictionary`）

辞書は品質レイヤ（§4.2 の `s_dict` と順位上乗せ）に使う。非搭載でもベースライン
（生ローマ字 + 大文字化）は動作する。

**ファイル形式**: TSV、UTF-8（BOM 許容）。M17 カスタムローマ字テーブルと同流儀
（`docs/legacy-parity-spec.md` §5.1）。1 行 1 エントリ:

```text
# english-words.tsv (UTF-8, BOM 許容)
# 行頭 # はコメント、空行は無視
# <surface>\t<frequency>\t[flags（カンマ区切り・任意）]
the	22038615
apple	542316
Apple	118242	proper
GitHub	30551	proper,tech
NASA	8123	acronym
```

| 列 | 必須 | 内容 |
|---|---|---|
| `surface` | ○ | 表示する英語表層（大小文字を保持。例 `Apple` / `GitHub` / `NASA`） |
| `frequency` | ○ | 出現頻度（正整数）。`s_dict` 判定と `score` 上乗せ・候補順に使う |
| `flags` | 任意 | カンマ区切り。`proper`（固有名詞・先頭大文字 surface を優先）/ `acronym`（全大文字）/ `tech`（技術語・M48 で boost 連携） |

**ルックアップ**: 入力 `raw_romaji` を lowercase したものをキーに引く（`apple` →
`apple`）。同一 lower キーに複数エントリを許す（`apple` 一般 / `Apple` 固有）。ヒット時は
**頻度降順**で複数 surface を返し、§4.3 の候補並び・順位に反映する。`flags` は大文字化
バリアントの既定優先度に影響する（`proper` は先頭大文字、`acronym` は全大文字を上位に）。

**配置と設定**:

- 既定パス: `%LOCALAPPDATA%\azooKey\dict\english-words.tsv`。
- 設定 `inlineEnglishDictionaryPath` で上書き可。
- `inlineEnglishDictionary == true` のときのみロード。

**ロードと性能**:

- 起動時にメモリへロード（`std::unordered_map<lower_surface, std::vector<Entry>>`）。
- 想定規模は数万〜十数万語。10 万語 × 平均 16 B 表層 + 頻度で数 MB 程度を目安とし、
  超える場合はコンパイル済みバイナリ形式（将来・M53 辞書層と共有）を検討する。
- パース失敗行は warning ログでスキップ（M17 と同流儀）。同一 `surface` 完全一致の重複は
  **ファイル末尾の定義を優先**（M17 と揃える）。
- ホットリロード（任意）: M17 の `ReadDirectoryChangesW` 監視基盤を再利用し、変更検出で
  差し替える（進行中の preedit は触らない）。

**配布元・ライセンス**: バンドルする初期辞書は公開された英単語頻度リスト由来とし、
ライセンスを明記する。上流のデータ生成パイプラインはクライアント実装の範囲外
（M36-B と同方針）。

**M53 辞書層との関係**: M60 は独立した軽量英単語辞書として始め、将来 M53
（`docs/auto-word-registration-spec.md` の DictionaryStore 再設計）に吸収・統合してよい。
本書は「ヒット有無と頻度で gating / 順位に寄与する」契約のみを正典とする。

### 4.5 辞書バイナリ形式（コンパイル済みキャッシュ）

§4.4 の TSV は**編集可能なソース**、本節の `.bin` は**起動高速化・低メモリのための
コンパイル済みキャッシュ**。起動時の TSV パースを避け、mmap で必要ページのみ読み込む
（M25 mmap ロードと整合）。

**ファイル構成**（全オフセットはファイル先頭からの絶対値。**リトルエンディアン固定**。
Windows x64 / ARM64 とも LE）:

```text
ヘッダ（32 B）
  off  size  field
  0    4     magic     = 'A','Z','E','D'（azooKey English Dict）
  4    4     version   (uint32) = 1
  8    4     entry_count (uint32)
  12   4     flags     (uint32; bit0 = records がキー昇順ソート済み)
  16   4     records_offset (uint32)
  20   4     strings_offset (uint32)
  24   4     generation (uint32; コンパクションごとに +1。reader の base 追従に使う。§4.7)
  28   4     content_hash (uint32; この base の全内容ハッシュ。**base 生成者**〔build / 再コンパイル /
              コンパクション〕が書き込む。定義は §4.6「base_fingerprint / content_hash の定義」)

レコード配列（entry_count × 20 B、キー（lower）昇順 → 同キー内 frequency 降順）
  off  size  field
  +0   4     key_offset     (string pool 内・lowercase キー先頭)
  +4   2     key_len        (バイト長)
  +6   2     surface_len    (バイト長)
  +8   4     surface_offset (string pool 内・表示表層)
  +12  4     frequency      (uint32)
  +16  1     flags          (bit0 proper, bit1 acronym, bit2 tech)
  +17  3     pad            (0)

string pool（strings_offset 以降）
  全キー・全表層の UTF-8 バイト列（連結。NUL 終端なし、長さは len フィールド）
```

**ルックアップ**: 入力 `raw_romaji` を lowercase 化し、レコード配列を**キーで二分探索**。
比較は string pool のバイト列で行う。同一キーのレコードは連続し frequency 降順なので、
ヒット位置から同キーの run を走査して複数 surface を頻度順に得る（§4.3 の並びへ反映）。

**ビルド / キャッシュ運用**:

- host は TSV と同ディレクトリの同名 `.bin`（拡張子のみ差替え。例
  `english-words.tsv` → `english-words.bin`）を参照する。
- **優先順**:
  1. `.bin` が存在し妥当（`magic`/`version` OK）で、かつ **TSV が無い、または `.bin` mtime ≥ TSV
     mtime** → `.bin` を mmap（**TSV 不在は `.bin` 単体ロードの正規ケース**）。
  2. そうでなく **TSV が存在** → TSV をパースし、書込可能なら `.bin` を再生成してキャッシュ。
  3. **どちらも無い** → 英単語辞書は無効（辞書なし。生ローマ字 + 大文字化のベースラインは動作）。
- **バンドル配布時はビルド済み `.bin` のみを同梱してよい（TSV 同梱は不要）**。TSV 無し +
  妥当な `.bin` は上記 1 でロードされる。
- オフラインのコンパイルツール（`tools/` 等、実装時に配置）でも TSV→`.bin` を生成可能とする。

**バージョニング・堅牢化**:

- `magic` 不一致 / `version` 不一致 / サイズ不整合の `.bin` は破棄し、**TSV があれば**再生成、
  **TSV が無ければ辞書無効**（ベースライン動作）。
- 破損 `.bin` で起動を妨げない（TSV があれば フォールバック、無ければ辞書無効）。`.bin` 生成
  不可（書込権限なし等）でも TSV パースで動作する（キャッシュは最適化であり必須ではない）。

**設定**: 新規キーは増やさない。`.bin` パスは `inlineEnglishDictionaryPath`（§7）から
拡張子差替えで導出する。コンパイルキャッシュの無効化が要る場合の内部フラグは実装時に定める。

**M53 辞書層との関係**: 大規模化時は LOUDS trie 等のより高密度な索引（azooKey 本家辞書系
で実績）への移行余地がある。M53 DictionaryStore が同形式 or 上位形式を採用する場合は
本節フォーマットを統合・置換してよい。

### 4.6 辞書の差分更新（増分コンパイル）

§4.5 の base `.bin` は不変・ソート済み・mmap。実行中の追加（M36 新語自動取得の英単語、
ユーザー追加）や TSV の小変更で**全再コンパイルを避ける**ため、オーバーレイ方式の差分更新を
定義する（append-only + 周期コンパクション。学習ストアの追記方式と同系の LSM 的構成）。

**構成**:

- **base**: `english-words.bin`（§4.5）。読み取り専用 mmap。
- **overlay（差分）**: サイドカー `english-words.delta.bin`。base への追加 / 更新 / 削除の op 列。
  小さく保ち、起動時に**メモリ内ソート索引**へ読む（後述。overlay ファイル自体は二分探索しない）。

**overlay フォーマット**（LE 固定。**末尾追記可能な自己完結フレーム列**。base の §4.5 形式とは
別物で、別個の string pool は持たない）:

```text
ヘッダ（16 B 固定）
  0  4  magic = 'A','Z','E','O'（azooKey English Overlay）
  4  4  version (uint32) = 1
  8  4  base_fingerprint (uint32; 適用先 base ヘッダの content_hash をコピーした値。§4.6「定義」)
  12 4  op_count (uint32)

op フレーム列（ヘッダ直後 = オフセット 16 から、op_count 個が連続。到着順 append-only）
  各フレームは可変長・自己完結（文字列はフレーム内にインライン。別 string pool を参照しない）:
  +0  1   op            (0 = upsert, 1 = delete[tombstone])
  +1  1   flags         (bit0 proper, bit1 acronym, bit2 tech)
  +2  2   key_len       (uint16, バイト長)
  +4  2   surface_len   (uint16, バイト長; delete でも 0 以上可)
  +6  4   frequency     (uint32)
  +10 …   key bytes     (key_len、UTF-8 lowercase キー)
  …       surface bytes (surface_len、UTF-8 表層)
  → 次フレームは直後（フレーム長 = 10 + key_len + surface_len）に続く
```

> overlay は**到着順の自己完結フレーム列**で、フレーム内に key/surface をインラインに持つ
> （別 string pool を持たない）。これにより**末尾に 1 フレーム追記するだけ**でよく、固定長
> レコード配列 + 後置 string pool（追記すると文字列領域が次レコード位置を侵食する）という
> base 形式の流用バグを避ける。reader は**オフセット 16 から op_count 個のフレームを長さ
> 計算で順に辿る**（`10 + key_len + surface_len`）。キー順にはソートしない（同一
> `(lower_key, surface)` に複数フレームが現れうる＝後勝ち）。**ファイル上の二分探索はしない**
> （下記のメモリ内索引で引く）。

**`base_fingerprint` / `content_hash` の定義**:

`content_hash` は base の全内容に対する **32-bit ハッシュ**（FNV-1a 等）。対象は base の
`version` + `entry_count` + `generation` + **全レコード配列 + 全 string pool**（構造フィールド
〔magic/flags/offsets/`content_hash` 自身/予約〕を除く全実体）。**base 生成者**（build ツール /
TSV 再コンパイル / コンパクション）が算出して **base ヘッダ offset 28（§4.5）に書き込む**。
overlay ヘッダの `base_fingerprint` は、その overlay が対象とする **base の `content_hash` を
コピーした値**（同一なら一致）。

- **基本方針**: base 自身が `content_hash` をヘッダに持つので、**reader / writer は base を
  全走査せずヘッダの 4 byte を読むだけ**で base の同一性を判定できる（フルハッシュ算出は base
  生成時の 1 回のみ）。overlay の妥当性は `overlay.base_fingerprint == 現 base header.content_hash`
  で判定する。
- **「先頭レコードのみ」や「count だけ」では不十分**: 同一 `version`/`entry_count`/`generation`
  で先頭が不変でも後方エントリだけ差し替えた base 再生成（**コンパクション外での再バンドル /
  TSV 再コンパイル**）も、全内容ハッシュなら `content_hash` が変わって検出できる（古い overlay の
  tombstone/upsert が新 base へ誤適用されるのを防ぐ）。
- 32-bit は事故的不一致検出が目的（敵対的衝突は非対象）。衝突を嫌う場合は `generation` 併用で
  更に弁別できる。
- `content_hash` を欠く（旧式・手製・破損）base はフィールドが 0 / 不整合になり、現 overlay と
  不一致扱い → overlay は破棄され base のみで動作（安全側）。

**メモリ内索引（overlay の読み取り構造）**:

- host は overlay の op を**到着順に再生**して **`(lower_key, surface)` → 最新 op**（upsert か
  tombstone）の**ソート済みインメモリ索引**を構築する（後の op が前を上書き＝後勝ち）。
- 索引はキーで二分探索 / ハッシュ引きできる（lookup が速い）。overlay 追記時は当該
  `(lower_key, surface)` の索引エントリを差分更新する（全再生は不要）。
- overlay はコンパクション（§4.6 末・§4.7）で上限を超えないため索引は小さく保たれる。

**ルックアップ統合**:

- 入力キー（lowercase）で **base はファイル上で二分探索、overlay はメモリ内索引で引く**
  （overlay ファイルは二分探索しない）。
- overlay の **`upsert` は base を上書き**（同一 `(lower_key, surface)` を同一視）、**`delete` は
  tombstone** として base のヒットを隠す。
- 同キー複数 surface は base + overlay をマージし、tombstone を除外して frequency 降順で返す。

**書き込み（増分）**:

- **実行時追加（M36 英単語 / ユーザー追加・削除）**: overlay に op を**追記するのみ**。base は
  触らない（全再コンパイル不要）。追記は単一ライタ（host）で行う。
- **TSV 小変更**: 既定は base 再コンパイル（§4.5）。**増分モード（任意）**では、保存済み TSV
  スナップショットとの行差分を取り、変更分のみ overlay へ反映して base 再コンパイルを
  コンパクションまで遅延する。

**コンパクション（base への取り込み）**:

- トリガ: overlay の op 数が base の一定割合（例 10%）超 or `op_count` > 閾値、または明示要求。
- 動作: base + overlay をマージして**新 base を一時ファイルへ書き、rename で原子置換**、
  overlay を**新 base の `base_fingerprint` + `op_count=0` で初期化**。読み取りは mmap ポインタ
  swap で無停止。**正確な順序（generation を rename 前の新 base に先行書込し、overlay 初期化
  〔fingerprint 更新含む〕は新 base が durable になった後）は §4.7**。
- 失敗時は旧 base + overlay を維持（部分書き込みを採用しない）。

**整合・堅牢化**:

- overlay の `base_fingerprint` が現 base と不一致（base が TSV から再生成された等）のときの扱い:
  **reader は当該 overlay を読み捨てる**（base のみで動作）、**writer は最初の append 前に overlay
  ヘッダを再初期化**してから追記する（§4.7「attach 前の fingerprint 検査」）。これにより
  stale な差分が現 base へ誤適用されず、かつ不一致 overlay へ追記して後で失う事故も防ぐ。
- overlay 破損 → 破棄して base のみで動作（差分は最適化であり必須でない）。
- 書込権限が無い環境では overlay を作らず**メモリ内差分のみ**（再起動で消える）。
- reader は base を read-only mmap、overlay 反映はメモリ上で COW ポインタ swap。

**連携**:

- **M36-A/B**: 自動取得した英単語（confirmed）を overlay の `upsert` として注入する自然な経路。
- **M53 辞書層**: 同じ base + overlay + コンパクション方式を共有・統合してよい。

### 4.7 overlay の同時実行・ロック

通常は 1 ユーザーセッションに inference-host が 1 つだが、再起動の重なり・複数 TIP
クライアント・将来のアプリ別 host 等で**複数プロセスが同じ辞書ファイルに触れうる**。
2 層のロックで整合を保つ。

**(A) プロセス内（host 内のワーカスレッド間）**:

- base ポインタ + overlay 構造を `std::shared_mutex` で保護。lookup は **shared（読み）**、
  append / コンパクションは **exclusive（書き）**。
- base 差し替え（コンパクション）は新 mmap を作ってから**ポインタを atomic swap**し、
  旧 mmap は参照が抜けてから unmap（読み取りは無停止）。

**(B) プロセス間（複数 host）**:

- **ライタロック**: `english-words.lock` を `LockFileEx`（排他）で取得してから overlay の
  append / コンパクションを行う。reader はロック不要（下記コミット規約で安全に読む）。
- **attach 前の fingerprint 検査（必須前提・overlay を書く全操作に適用）**: ライタは overlay を
  書く操作（**append / コンパクション**）に入る前に、現 live base ヘッダの `content_hash`（§4.5
  offset 28 を read。フル走査不要）を、overlay ヘッダの `base_fingerprint` と照合する。**不一致なら
  操作前に overlay ヘッダを再初期化**する（コンパクション §4.7 step 4 と同じ順序: **先に `op_count=0` を flush** →
  `SetEndOfFile` で stale フレーム truncate → **その後 `base_fingerprint` を現 base 値に更新**。
  fingerprint を先に書くと旧フレームが新 base に valid 化される窓ができるため）。これは (a) コンパクションのクラッシュ分岐で
  overlay が旧 fingerprint のまま残った場合、(b) コンパクション外の base 再生成（再バンドル / TSV
  再コンパイル）後に古い overlay が残った場合に、**stale な差分が現 base へ誤適用される / 追記語が
  後の reload で不一致破棄されて失われる**事故を防ぐ。とくに**コンパクションは stale overlay を
  マージしてはならない**（再初期化後は実質 base のみから新 base を作る）。stale フレームを捨てて
  よい理由: それらは別 base 向けの差分であり（クラッシュ分岐では既に新 base へマージ済みのため
  冪等に失える）、現 base へ適用すると誤上書き/誤隠蔽になるため破棄が正しい（reader 側も §4.6 で
  不一致 overlay を読み捨てる）。
- **append コミット規約（クラッシュ安全）**: 排他ロック下で（上記 fingerprint 検査・必要なら再初期化の後）
  1. op フレーム（ヘッダ + key/surface インライン。§4.6）を末尾へ 1 つ書く、2. `FlushFileBuffers`、
  3. **最後に**ヘッダ `op_count` を +1（4 byte の整列書き込み＝原子的）、4. flush、5. ロック解放。
  reader は**先に `op_count` を読み、その件数ぶんのフレームだけをオフセット 16 から長さ計算で
  順に辿ってメモリ内索引へ再生する**（§4.6。後勝ち）。ライタが途中でクラッシュしても
  `op_count` 超の半端フレームは辿られず無視される（次のライタは `op_count` 個のフレームを
  歩いて末尾オフセットを求め、そこへ `SetEndOfFile` で truncate して回収する。フレームは
  可変長のため固定 `recsize` 計算ではなく**フレーム走査で末尾を決める**）。
- **コンパクション規約（原子置換・generation 先行永続化）**: 排他ロック下で
  1. base + overlay をマージした新 base を一時ファイルへ書く。**このときヘッダ `generation` に
     「旧 base の `generation` + 1」を、`content_hash` に新 base の全内容ハッシュ（§4.6）を
     書き込んでおく**（merged データと新 generation/content_hash を一体で持たせ、後追いの別書き込みに
     しない）。
  2. `FlushFileBuffers`（一時 base を durable 化）。
  3. `MoveFileEx(REPLACE_EXISTING | WRITE_THROUGH)` で原子 rename（新 generation 入りの新 base が
     live になる）。
  4. **新 base が durable になった後で初めて** overlay ヘッダを**初期化し直す**。**書き込み順が重要**:
     (a) **先に `op_count=0` を書いて flush**（＝空状態を publish）、(b) ヘッダ末尾へ `SetEndOfFile`
     で truncate（stale フレーム破棄）、(c) **その後で `base_fingerprint` を新 base ヘッダの
     `content_hash`（手順 1 で書込済）に更新**して flush。
     - **順序の理由**: `base_fingerprint` を先に新値へ書くと、`op_count` がまだ旧値（非0・旧 base
       フレームを指す）の窓で overlay が「新 base に valid」に見え、lock-free reader が旧 op を新 base へ
       適用して**誤候補を返す**。`op_count=0` を先に publish すれば、その窓では fingerprint が新旧
       どちらでも replay 0 件＝空 overlay となり安全（旧 fingerprint 中は reader が不一致破棄、
       いずれにせよ空）。
     - 逆に「`base_fingerprint` を旧値のまま `op_count` だけ 0」も不可（次の append が reload 後に
       不一致破棄され追記語を失う）。だから両方更新しつつ**空状態を先**に出す。
  5. ロック解放。
  - クラッシュ地点別の安全性:
    - rename 前（手順 1–2 中）にクラッシュ → 旧 base（旧 generation）+ overlay 健全。読みは
      旧 base + overlay で正しい。一時ファイルは破棄。
    - rename 後・overlay 初期化前/途中（手順 3–4 間）にクラッシュ → 新 base（新 generation・
      merged 済み）が live。reader は現 base ヘッダ再読込で `generation` 変化を検出し再 mmap、overlay は
      **fingerprint 不一致（旧 base 値のまま）か `op_count=0`（4a 完了後）**のどちらでも空/破棄として
      安全に読まれる（その op は既に新 base へ取り込み済みなので欠落なし）。次のライタは
      **attach 前の fingerprint 検査**（上記）で overlay を再初期化してから追記するため、
      残存 overlay のまま追記して後で失う事故は起きない。
  - 失敗時は一時ファイルを捨て旧 base + overlay を維持（部分置換しない）。**overlay を先にクリア
    してから新 base / generation を永続化する順序は採らない**（クリア後・新 base 永続化前の
    クラッシュで新規学習語が失われ、generation 不変のため lock-free reader が旧 base のまま
    取りこぼすため。本順序の核心）。
- **reader の base 追従（置換検出）**: `MoveFileEx` で base が置換されても、reader が保持中の
  mmap は**旧 file object のビューのまま**で `generation` も変わらない（置換は別ファイル実体に
  なるため、保持マッピングを見ても気づけない）。よって reader は**保持 mmap のヘッダではなく、
  現在の base ファイルをパスから読み直して**追従する:
  - lookup バッチの境で、現 base ファイルを**開き直して 32 B ヘッダだけ読む**（`generation` と
    **`content_hash`**〔§4.5 offset 28〕）。保持中マッピングの値と異なれば、**現ファイルを新規に mmap
    して swap**し
    overlay を読み直す（`base_fingerprint` 不一致の overlay は §4.6 のとおり破棄）。ヘッダ 32 B
    read は安価。
  - もしくは `FindFirstChangeNotification` / `ReadDirectoryChangesW` で当該ディレクトリの変更通知を
    受け、通知時のみ上記の再オープン・再 mmap を行う（ポーリング回避）。
  - **共有モード**: 全プロセスは base / overlay を **`FILE_SHARE_READ | FILE_SHARE_WRITE |
    FILE_SHARE_DELETE`** で開く。`FILE_SHARE_DELETE` が無いと writer の `MoveFileEx`
    （REPLACE_EXISTING）が共有違反で失敗する。
- **読み取りの一貫性（seqlock 的リトライ）**: lock-free reader は base 置換 / overlay 再初期化と
  競合しうる。例: reader が `MoveFileEx` 直前に base ヘッダを sample し旧 mmap を保持したまま、
  compaction step 4a で `op_count=0`（`base_fingerprint` はまだ旧値の窓）を読むと、「旧 base に対する
  空 overlay」を受理し、旧 overlay にしか無かった更新（新 base へマージ済みだが reader 未 remap）を
  取りこぼす。これを**読み取り後の再チェック・リトライ**で防ぐ:
  1. 現 base ヘッダ（`generation`/`content_hash`、パスから read）を snapshot `S1` とし、保持 mmap が
     `S1` と一致するよう必要なら remap する。overlay ヘッダ（`base_fingerprint`/`op_count`）も読む。
  2. base + overlay から結果を計算する。
  3. **計算後に base ヘッダを再 read（`S2`）し、`S2 != S1`（base が置換された）または overlay の
     `base_fingerprint`/`op_count` が変化していれば、読み取りが compaction / 再初期化境界を跨いだと
     判断して手順 1 からリトライ**する（compaction は稀なので有界リトライで収束）。
  - これにより reader は compaction 境界を跨いだ不整合な結果（クリア済み overlay × 旧 base）を返さない。
    通常 append（`op_count` 増加）でも同様にリトライしうるが、追記はユーザー打鍵レートで稀なため
    実害は無い。snapshot は 32 B ヘッダ + overlay ヘッダの read のみで安価。
- **権限なし / ロック不可**: overlay ファイルを開けない・ロックできない環境では、当該
  host は**メモリ内差分のみ**で動作し永続化しない（再起動で消える）。base は read-only で
  常に読める。

**まとめ**: 「単一ライタを file lock で保証」「append は frame→`op_count` の順（op_count 最後）／
reinit・clear は `op_count=0`→fingerprint の順（空状態を先に publish）」「rename による原子置換 +
generation 先行永続化」「writer は書込前に fingerprint 不一致 overlay を再初期化」「reader は
lock-free + 現 base ヘッダ（パス再読込）で置換を検出し再 mmap + **読み取り後再チェックの seqlock
リトライ**で compaction 境界跨ぎを排除」により、複数 host でも破損・取りこぼし・部分読み・
stale 適用を起こさない。

## 5. 確定と学習

- 英単語候補を確定したとき、surface = 選んだ英語形（`apple` / `Apple` …）。
- 学習（`CommitObservation`）の reading は**生ローマ字（半角英字、例 `apple`）**とする。
  再度同じローマ字を打ったときに学習で英単語候補が再提示されるようにするため。かな
  （`あっぷる`）を reading にすると、かな漢字学習と混線するため避ける。
- 学習ストアには `CandidateSource`（既存 `Candidate.source`）/ English タグで識別して
  記録し、かな漢字学習と区別できるようにする（具体的な格納先は実装時に決定。既存
  `LearningStore` に English チャネルを設けるか、source タグで区別する）。

## 6. IPC プロトコル

新 `MessageType` は追加しない。既存 `QueryCandidates`
（`ipc/include/azookey/ipc/Messages.h` / `Payloads.h`）をオプションフィールドで拡張する。
すべての追加フィールドは**任意・後方互換**とし、既存 `Payloads.cpp` の流儀
（Build = `o.emplace(...)`、Parse = `GetString/GetBool/GetUInt().value_or(既定)`）に従う。

### 6.1 `CandidateField` に `tag` を追加

現状 `CandidateField` は `surface` / `reading` / `score` / `source` のみで候補タグを
持たない。M60 は X-2-3 と統一して `tag`（`uint8`）を**新規追加**する。

| field | 型 | 既定 | 説明 |
|---|---|---|---|
| `tag` | uint8 | `0`（None） | `docs/rich-features-spec.md` X-2-3 の `CandidateTag`。英単語候補は `English = 4` |

`CandidateToJson` / `CandidateFromJson` 追記（規約例）:

```cpp
// CandidateToJson
o.emplace("tag", j::Value(static_cast<double>(c.tag)));
// CandidateFromJson（後方互換: 省略時 0=None）
c.tag = static_cast<uint8_t>(v.GetUInt("tag").value_or(0));
```

`tag` の追加は `QueryCandidatesResponse` / `CommitObservationRequest`（`chosen` / `shown[]`）
など `CandidateField` を含む全 payload に波及するが、既定 0 で後方互換。

### 6.2 Request（`QueryCandidatesRequest` 拡張）

既存フィールド: `reading` / `left_context` / `max_candidates` / `live`。追加:

| field | 型 | 既定 | 説明 |
|---|---|---|---|
| `raw_romaji` | string | `""` | 生ローマ字（英単語候補の素材）。`reading` とは別フィールド |
| `english_candidates` | bool | `false` | `inlineEnglishCandidates` を伝搬 |

```jsonc
{
  "reading": "あっぷる",
  "left_context": "",
  "max_candidates": 10,
  "live": false,
  "raw_romaji": "apple",          // 追加
  "english_candidates": true       // 追加
}
```

`raw_romaji` は本機能の核。`reading` には生ローマ字を入れず別フィールドに分離する
（かな漢字変換の入力を汚染しないため。M58 §4.2 と同原則）。Build/Parse 規約:

```cpp
// Build
o.emplace("raw_romaji", j::Value(p.raw_romaji));
o.emplace("english_candidates", j::Value(p.english_candidates));
// Parse（後方互換）
p.raw_romaji         = v->GetString("raw_romaji").value_or(std::string());
p.english_candidates = v->GetBool("english_candidates").value_or(false);
```

### 6.3 Response

`QueryCandidatesResponse` は構造不変（`candidates[]` + `partial`）。英単語候補は
`candidates[]` に `tag = English(4)` を付けて混在させる（§6.1）。

```jsonc
{
  "candidates": [
    { "surface": "アップル", "reading": "あっぷる", "score": 0.80, "source": "model",     "tag": 0 },
    { "surface": "apple",   "reading": "apple",   "score": 0.55, "source": "heuristic", "tag": 4 },
    { "surface": "Apple",   "reading": "apple",   "score": 0.50, "source": "heuristic", "tag": 4 }
  ],
  "partial": false
}
```

英単語候補の `reading` には**生ローマ字**を入れる（§5 の学習方針と一致。確定時に
`reading=apple` で学習し、再入力で再提示するため）。

### 6.4 確定時学習（`CommitObservationRequest`）

payload は §6.1 の `tag` 追加以外**変更しない**。英単語候補を確定したとき:

```text
CommitObservationRequest{
  reading = raw_romaji,          // 例 "apple"（かな "あっぷる" ではない）
  chosen  = { surface:"Apple", reading:"apple", source:"...", tag:4 },
  ...
}
```

`reading` を生ローマ字にすることで、かな漢字学習（`reading="あっぷる"`）と学習空間を
分離する（混線させない。§5）。host 側は `chosen.tag == English` を見て English チャネル
（or source タグ）へ振り分ける。

英単語確定が**文の一部（multi-segment）**の場合は、当該文節を `ObservedSegment`
（`reading=生ローマ字`、`chosen.tag=English`）として
**`CommitSegmentsObservation`**（`docs/romaji-batch-conversion-spec.md` §6.4）に含めて送る。
単発の英単語確定は上記の単発 `CommitObservation` を使う。

### 6.5 staleness・Cancel

候補生成経路のため、既存 M10 の staleness / Cancel（`CancelPayload.target_request_id`）を
そのまま使う。追加経路は無い。

### 6.6 payloads_test 期待値（`ipc/tests/payloads_test.cpp`）

- `CandidateField` round-trip で `tag` が保存される。`tag` を欠く JSON のパースで `0`（None）。
- `QueryCandidatesRequest` round-trip で `raw_romaji` / `english_candidates` が保存される。
  欠落 JSON で `""` / `false` の既定（後方互換）。
- 英単語候補（`tag=4`、`reading=生ローマ字`）を含む `QueryCandidatesResponse` の往復。

## 7. 設定スキーマ

実装時に `settings/mvp-settings.schema.json` へ以下を追加する
（`additionalProperties:false` を維持。`description` に対応 M を記載する既存流儀に
合わせる）。本書（spec）が JSON schema の正典であり、実ファイルへの追加は M60 実装時に
行う（本セッションは設計確定のみでスキーマファイルは変更しない）。

| キー | 型 | 既定 | 説明 |
|---|---|---|---|
| `inlineEnglishCandidates` | boolean | `false` | M60: 日本語ローマ字入力中に英単語候補を候補列へ注入する |
| `inlineEnglishCaseVariants` | boolean | `true` | M60: 先頭大文字 / 全大文字バリアントも候補に出す |
| `fullWidthEnglishCandidate` | boolean | `false` | M60: 全角ローマ字候補も出す（legacy `fullWidthRomanCandidate` 相当） |
| `inlineEnglishMinLength` | integer (≥1) | `2` | M60: 英単語候補を出す生ローマ字の最小長 |
| `inlineEnglishPromoteThreshold` | number (0.0–1.0) | `0.6` | M60: `english_intent` がこの値以上で英単語候補を上位化（§4.3） |
| `inlineEnglishDictionary` | boolean | `false` | M60: 英単語辞書によるランキング・ゲーティングを有効化（品質レイヤ。OFF でも生ローマ字 + 大文字化は出せる） |
| `inlineEnglishDictionaryPath` | string | `%LOCALAPPDATA%\azooKey\dict\english-words.tsv` | M60: 英単語辞書 TSV のパス（§4.4）。ホットリロード対応 |

## 8. テスト計画

- **候補生成** (`core/tests` or host テスト): 大文字化バリアント生成（`apple` →
  `Apple` / `APPLE`）、全角バリアント生成（`ａｐｐｌｅ` と、大小バリアント併用時の
  `Ａｐｐｌｅ` / `ＡＰＰＬＥ`）、最小長ゲーティング、英語意図ヒューリスティック
  （`xyz` 等の非日本語ローマ字判定）。
- **大文字打鍵での 6 形の成立** (同上): `Apple` / `APPLE` / `aPPle` の打鍵でも
  `lower(r)` 基準で §3 の 6 形が出ること（`apple` が必ず含まれること）。`aPPle` では
  生ローマ字そのものの行が加わり、`apple` / `APPLE` では重複除去で消えること。
  §4.3 の固定順で先に現れた表層形が残ること。
- **辞書一致語との共存** (同上): 辞書 surface（`iPhone` 等）が先頭に付き、`lower(r)` 由来の
  6 形が消えないこと。辞書 surface が 6 形のいずれかと一致する場合は重複せず順位上乗せだけが
  効くこと。
- **順位** (host テスト): 弱シグナル時に英単語候補が日本語上位候補より下に来ること、
  自動選択されないこと、強シグナル時のみ上位化されること。
- **辞書 TSV** (`core/tests` or host テスト): TSV パース（surface/frequency/flags）、
  lower キー・頻度降順ルックアップ、同一キー複数 surface、不正行スキップ、末尾優先の重複。
- **辞書バイナリ** (同上): TSV→`.bin` 生成と round-trip（ルックアップ結果が TSV と一致）、
  `magic`/`version` 不一致・破損 `.bin` で TSV フォールバック、`.bin` が TSV より新しい
  ときに `.bin` 経路、古いときに再生成。
- **差分更新** (同上): overlay の `upsert` が base を上書き、`delete` tombstone が base ヒットを
  隠す、**到着順（非ソート）の op をメモリ内索引へ再生して後勝ち解決**（同一
  `(lower_key,surface)` の複数 op）、base+overlay マージの frequency 降順、コンパクション後の
  新 base がマージ結果と一致、`base_fingerprint` 不一致で overlay 破棄、overlay 破損時に
  base のみで動作。
- **同時実行** (host テスト): `op_count` を最後に更新する append でクラッシュ時に半端レコードが
  無視される（`op_count` 超を信用しない）、rename によるコンパクション原子置換、ロック不可時に
  メモリ内差分のみで動作（§4.7）。
- **reinit の書込順** (host テスト): overlay 再初期化が **`op_count=0` を先に publish→ truncate→
  `base_fingerprint` 更新**の順であること。`base_fingerprint` を先に書く実装だと「新 fingerprint +
  旧 `op_count`>0 + 旧フレーム」の窓で reader が stale op を新 base へ適用してしまう回帰を、
  その窓を再現して検出する（§4.7）。
- **base 置換の検出** (host テスト): 別ライタが `MoveFileEx` で base を置換した後、reader が
  **保持 mmap のヘッダではなく現 base ファイルを開き直して** `generation` / **`content_hash`** 変化を
  検出し再 mmap すること（保持 mmap だけ見る実装は置換に気づかず旧 base を出し続ける回帰を防ぐ）。
  **同一 `generation` でも `content_hash` 差で置換を検出**できること。base/overlay を
  `FILE_SHARE_DELETE` 付きで開くこと（無いと `MoveFileEx` が共有違反で失敗）。
- **読み取り一貫性（seqlock リトライ）** (host テスト): reader が base ヘッダ sample 後・結果計算後の
  再チェックで、compaction による base 置換 / overlay 再初期化（`op_count=0`）を跨いだ読み取りを
  検出してリトライし、「クリア済み overlay × 旧 base」で**更新を取りこぼした stale 結果を返さない**
  こと（再チェックなし実装の回帰を防ぐ。§4.7）。
- **コンパクションのクラッシュ安全** (host テスト): 新 base に generation を**先行書込**してから
  rename し、**overlay 初期化は新 base の durable 後**に行う順序で、rename 後・overlay 初期化前の
  クラッシュでも新 base（新 generation）から学習語が失われないこと。overlay 未クリアの op 再適用が
  冪等であること（§4.7）。
- **append 前 fingerprint 検査** (host テスト): overlay が**現 base と不一致な `base_fingerprint`**
  （コンパクションのクラッシュ残存 / base 再生成後）の状態で append すると、ライタが追記前に
  overlay を再初期化（新 fingerprint + `op_count=0` + truncate）してから追記し、その追記語が
  reload 後も残ること（再初期化せず追記すると後で不一致破棄される回帰を防ぐ。§4.7）。
- **コンパクション後の overlay fingerprint** (host テスト): コンパクションが overlay の
  `base_fingerprint` を**新 base の値に更新**すること、更新後に append → reload しても fingerprint
  一致で overlay が破棄されず追記語が残ること（旧 fingerprint のまま `op_count=0` だけにすると
  reload で破棄される回帰を防ぐ。§4.7）。
- **base 再生成の検出（全内容 content_hash）** (host テスト): base を**同一 `version`/`entry_count`/
  `generation`・先頭レコード不変で後方エントリだけ差し替え**て再生成すると base ヘッダの
  `content_hash` が変わり、`overlay.base_fingerprint` と不一致で overlay が破棄されること
  （先頭/件数のみのハッシュなら見逃す回帰を防ぐ。§4.6）。`content_hash` が base ヘッダから読めること。
- **IPC** (`ipc/tests/payloads_test.cpp`): `QueryCandidates` の `raw_romaji` /
  `english_candidates` フィールド、候補 `tag` の build/parse 往復。
- **学習** (`learning/tests`): 英単語確定で reading=生ローマ字として記録され、かな漢字
  学習と混線しないこと。再度同じローマ字で英単語候補が再提示されること。
- **手動 / 実機（Win11、`gate:human-required`）**: Japanese モードのまま `apple` を打つと
  候補列に `apple` / `Apple` が現れ、選択すると英数モード切替なしで英単語が確定する。
  `ko` 等では英単語が第一候補を奪わない。OFF で英単語候補が一切出ない。

## 9. 将来課題（本書の対象外）

- **連続英文タイプ**: スペースを含む複数語の英文を Japanese モードのまま連続入力する
  体験。語間スペース処理・文単位英語予測（`requireEnglishPrediction` 相当）・M58
  一括変換との統合が必要。
- **英語スペル補正・補完**: 辞書ゲーティングを超える補正・補完。
- **アプリ別の英語優先度**: M48（アプリ別入力プロファイル）で、コードエディタ等では
  英単語タグを boost する（`docs/app-profile-spec.md` の候補タグ重みと接続）。

## 10. 受け入れ条件（M60）

M60 の受け入れ条件は `plans/windows-port-roadmap.md` の M60 に定義する（本書は仕様、
roadmap は受け入れ条件の「定義」、達成状態は Linear）。

## 11. 参照

- 英語候補オプション 旧実装: `legacy/Core/Sources/Core/InputUtils/SegmentsManager.swift`
  （`englishCandidateInRoman2KanaInput` / `fullWidthRomanCandidate` / `requireEnglishPrediction`）
- 候補生成・確定動線: `docs/legacy-parity-spec.md` §1.2・§1.3
- ラベル付き候補（English タグ）: `docs/rich-features-spec.md` X-2-3
- 生ローマ字バッファ保持: `docs/romaji-batch-conversion-spec.md` §4.1
