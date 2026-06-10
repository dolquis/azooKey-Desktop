# ローマ字入力中インライン英単語候補 仕様（追加機能 / M60）

本書は azooKey-Desktop Windows 版の「ローマ字入力中インライン英単語候補（Inline
English Candidate in Romaji Input）」機能を定める。`plans/windows-port-roadmap.md` の
M60 が本書を参照する。本書は機能仕様（IPC payload・設定項目・ユーザー可視挙動）の
正典であり、進捗・状態は持たない（状態の正典は Linear。`AGENTS.md`「Linear 運用
（管制塔）」参照）。

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
（`legacy/Core/Sources/Core/InputUtils/SegmentsManager.swift:179`、macOS 版では
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

`apple` を例に、生成しうる英単語候補:

| 種別 | 例 | 生成条件 |
|---|---|---|
| 生ローマ字そのもの | `apple` | 常時（決定的ベースライン） |
| 先頭大文字 | `Apple` | `inlineEnglishCaseVariants` ON |
| 全大文字 | `APPLE` | `inlineEnglishCaseVariants` ON |
| 全角ローマ字 | `ａｐｐｌｅ` | `fullWidthEnglishCandidate` ON（legacy `fullWidthRomanCandidate` 相当） |
| 辞書一致語 | `apple`（辞書ヒットで上位化） | `inlineEnglishDictionary` ON（品質レイヤ） |

全候補に `CandidateTag::English`（X-2-3）を付与し、候補 UI で `[英]` バッジを出せる
ようにする。

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
    生ローマ字そのもの候補は常に生成可（順位は §4.3）
    先頭大文字 / 全大文字バリアントは inlineEnglishCaseVariants ON 時
    全角ローマ字は fullWidthEnglishCandidate ON 時
    辞書一致語（surface 差し替え・score 上乗せ）は inlineEnglishDictionary ON 時
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
生ローマ字そのもの → 先頭大文字 → 全大文字 → 全角ローマ字
（辞書一致語があれば「生ローマ字そのもの」位置の surface を辞書 surface に差し替え）
```

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
  28   4     reserved  (0)

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
- **優先順**: `.bin` が存在し、`version` 一致かつ TSV より新しい（mtime）→ `.bin` を mmap。
  そうでなければ TSV をパースし、**`.bin` を再生成してキャッシュ**してから使う。
- バンドル配布時はビルド済み `.bin` を同梱してよい（初回パースも不要にできる）。
- オフラインのコンパイルツール（`tools/` 等、実装時に配置）でも TSV→`.bin` を生成可能とする。

**バージョニング・堅牢化**:

- `magic` 不一致 / `version` 不一致 / サイズ不整合の `.bin` は破棄し、TSV から再生成。
- 破損 `.bin` で起動を妨げない（TSV へフォールバック）。`.bin` 生成不可（書込権限なし等）でも
  TSV パースで動作する（キャッシュは最適化であり必須ではない）。

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
  小さく保ち、起動時にメモリへ読む（小さければそのまま二分探索）。

**overlay フォーマット**（LE 固定。base と同系）:

```text
ヘッダ（32 B）
  0  4  magic = 'A','Z','E','O'（azooKey English Overlay）
  4  4  version (uint32) = 1
  8  4  base_fingerprint (uint32; 適用先 base の version^entry_count^records 先頭ハッシュ)
  12 4  op_count (uint32)
  16 4  records_offset
  20 4  strings_offset
  24 8  reserved (0)

op レコード（op_count × 24 B、**到着順（append-only）。キー順ソートはしない**）
  +0  1  op             (0 = upsert, 1 = delete[tombstone])
  +1  3  pad (0)
  +4  4  key_offset
  +8  2  key_len
  +10 2  surface_len
  +12 4  surface_offset
  +16 4  frequency
  +20 1  flags          (bit0 proper, bit1 acronym, bit2 tech)
  +21 3  pad (0)

string pool（strings_offset 以降。キー・表層の UTF-8 連結）
```

> overlay の on-disk レコードは**到着順**（末尾追記の append-only。§4.7 のクラッシュ安全 append と
> 整合）であり、**キー順にソートしない**。同一 `(lower_key, surface)` に複数 op が現れうる
> （後勝ち）。したがって**ファイル上の二分探索はしない**（下記のメモリ内索引で引く）。

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
  overlay をクリア（`op_count=0`）。読み取りは mmap ポインタ swap で無停止。
- 失敗時は旧 base + overlay を維持（部分書き込みを採用しない）。

**整合・堅牢化**:

- overlay の `base_fingerprint` が現 base と不一致（base が TSV から再生成された等）→ overlay を
  破棄 or 再構築する。
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
- **append コミット規約（クラッシュ安全）**: 排他ロック下で
  1. op レコードと string バイトを末尾へ書く、2. `FlushFileBuffers`、3. **最後に**ヘッダ
  `op_count` を +N（4 byte の整列書き込み＝原子的）、4. flush、5. ロック解放。
  reader は**先に `op_count` を読み、その件数ぶんのレコードだけを到着順にメモリ内索引へ
  再生する**（§4.6。後勝ち）。ライタが途中でクラッシュしても `op_count` 超の半端レコードは
  無視される（次のライタが `op_count × recsize` へ truncate して回収）。
- **コンパクション規約（原子置換）**: 排他ロック下で新 base を一時ファイルへ書き
  `FlushFileBuffers` → `MoveFileEx(REPLACE_EXISTING | WRITE_THROUGH)` で rename →
  overlay を `op_count=0` にリセット → base ヘッダの `generation`（§4.5 reserved を 1 つ
  使う）を +1 → ロック解放。失敗時は一時ファイルを捨て旧 base + overlay を維持
  （部分置換を採用しない）。
- **reader の base 追従**: reader は保持中 base の `generation` / `base_fingerprint` を、
  lookup バッチの境で軽くチェックし、変化していれば base を**再 mmap**して overlay を
  読み直す（`base_fingerprint` 不一致の overlay は §4.6 のとおり破棄）。
- **権限なし / ロック不可**: overlay ファイルを開けない・ロックできない環境では、当該
  host は**メモリ内差分のみ**で動作し永続化しない（再起動で消える）。base は read-only で
  常に読める。

**まとめ**: 「単一ライタを file lock で保証」「op_count を最後に更新する append」「rename に
よる原子置換」「reader は lock-free + 境界チェック + 世代追従」により、複数 host でも
破損・取りこぼし・部分読みを起こさない。

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
  `Apple` / `APPLE`）、全角ローマ字生成、最小長ゲーティング、英語意図ヒューリスティック
  （`xyz` 等の非日本語ローマ字判定）。
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
  無視される（`op_count` 超を信用しない）、rename によるコンパクション原子置換、reader が
  `generation` 変化で base を再 mmap、ロック不可時にメモリ内差分のみで動作（§4.7）。
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
