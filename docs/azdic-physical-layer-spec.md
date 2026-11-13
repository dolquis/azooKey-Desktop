# system 辞書の物理層 仕様（`.azdic`）

対象リポジトリ: dolquis/azooKey-Desktop
対応マイルストーン: M53（辞書層の物理層。DEV-412）
関連: `docs/auto-word-registration-spec.md`（§14 辞書層の論理設計）、
      `docs/sideload-packaging-spec.md`（MSI の辞書生成）、
      `plans/windows-port-roadmap.md` M53

本書は `docs/auto-word-registration-spec.md` から §15（system 辞書の物理層）を切り出した文書である。
節番号 §15.x は旧 spec のまま保ち、旧 spec の転送表が `§15` を本書へ解決する。
本書内の `§14.x` は `docs/auto-word-registration-spec.md` の節を指す。

## 正典範囲

読みの索引構造（double-array trie）、直列化形式 `.azdic` v1、マルチソースのビルドパイプライン、
`sudachi_lexicon` / `neologd_lexicon` の取得と形式、ビルダと Host の接続契約、物理層の受け入れ条件は、
本書が正典である。層構成・エントリの意味・スコア係数・ライセンス判定・設定スキーマ・source tagging は
`docs/auto-word-registration-spec.md` §14 が正典である。

## 節とマイルストーンの対応

| 節 | マイルストーン |
|---|---|
| §15 | M53（system 辞書の物理層） |

## 15. M53 追補: system 辞書の物理層

§14 は辞書層の**論理設計**（層構成・エントリ意味・スコア・ライセンス判定）を定める。
本章はその下にある**物理層**、すなわち読みの索引構造・ディスク上の直列化形式・
複数上流からのビルド手順を定める。DEV-412（system 辞書 double-array trie 化 +
マルチソースビルド）の実装はこの章を正典とする。

### 15.1 スコープと責務境界

| 対象 | 物理表現 | 本章の扱い |
|---|---|---|
| `base_lexicon` / `sudachi_lexicon` / `named_entity_lexicon` / `technical_terms_lexicon` | ビルド済み **`.azdic` アーティファクト**（double-array trie + エントリプール） | §15.2〜§15.6 |
| `neologd_lexicon`（別 pack DL） | 同じ `.azdic` 形式 | §15.5（§14.10 の「コンパイル済み層アーティファクト」は本形式を指す） |
| `user_dictionary` / `auto_words` / `app_specific_dictionary` | 既存の `std::map` + JSON / TSV を**維持** | §15.7 |

責務の置き場所は次のとおり。

- **`core/`**（`azookey_core`）: trie 構築・検索と `.azdic` の読み取り。辞書の意味論を
  持たない汎用データ構造として置く。`learning` は `core` に依存するため（`learning/CMakeLists.txt`）、
  この配置なら層の向きを崩さずに `DictionaryStore` から使える。
- **`learning/`**: `DictionaryStore`（§14.7）と各 `IDictionaryLayer` 実装。層の有効化・
  dedup・スコアは §14 の責務。
- **`dictbuild/`**（ホストツール）: 上流データから `.azdic` を生成するオフライン
  ビルダ。IME 実行時には存在しない。

ランタイムは `.azdic` を**読むだけ**であり、書かない。書き込みが要る層は §15.7 の
mutable 層に限る。

### 15.2 trie 実装の選定

**決定: `.azdic` 内の double-array trie は自前実装**（`core/src/DoubleArrayTrie.cpp`、
構築側は `dictbuild` と共有）とし、第三者 trie ライブラリはベンダリングしない。

| 候補 | 評価 |
|---|---|
| **自前 double-array**（採用） | 実行時は base / check 配列の走査のみで、追加の再配布義務が発生しない。構築側もキー整列済み入力に対する素直な配置で足りる |
| darts-clone | MeCab 系で実績のある単一ヘッダ実装。ただし採用しても §15.5 のコンテナ（セクション構成・整合性検証・mmap 境界）は自作になり、削減できるのは trie 本体に限られる |
| marisa-trie | LOUDS 系でサイズは有利だが、デュアルライセンス構成のため取り込み時にライセンス選択の判断が要る。サイズ予算（§15.9）が実測で超過した場合の再検討候補として記録する |
| yada（karukan が使用） | Rust 実装であり移植不可。設計思想の参照にとどめる |

外部 trie ライブラリを後から採用する場合は、コードもデータと同じく
`docs/licensing-policy.md` の採用ワークフロー（ライセンス互換性の判定 → 三層 attribution →
`THIRD_PARTY_LICENSES` 追記 → 配布ガード）に載せる。上流ライセンスは版によって
変わり得るため、判定時に一次情報（各上流リポジトリの `COPYING` / `LICENSE`）を参照して
確定させる。本節は候補の記録であり、ライセンス判定を先取りしない。

### 15.3 キー空間と読み正規化の適用点

trie のキーは §14.3 で正規化した `normalized_reading` の UTF-8 バイト列とする。
§14.3 の各規則を、ビルド時とクエリ時のどちらで効かせるかを次のとおり確定する。

| §14.3 の規則 | 適用点 | 理由 |
|---|---|---|
| カタカナ → ひらがな | **ビルド時**（キーを正規化） | 上流ごとの表記差をアーティファクト境界で吸収する |
| 全角英数 → 半角英数 | **ビルド時** | 同上 |
| ヴァ / バ、づ / ず、ぢ / じ の alias | **ビルド時にキーを展開** | クエリ時展開は入力長に対して指数的に候補キーが増え、順序の決定性も落ちる |
| 長音「ー」の緩い一致 | **クエリ時**（「ー」を除去した二次検索） | 長音の位置は任意個所に現れ、ビルド時展開ではキー数が発散する |

alias 展開の上限は 1 エントリあたり **8 キー**とする。超過するエントリは完全一致キーのみを
登録し、ビルドログに警告を出す（無音で語彙を落とさない）。展開されたキーには
`MatchKind::Alias` を、元のキー経由の参照には `MatchKind::Exact` を持たせ、§14.11 の
`exact_reading_bonus`（完全一致 +0.10 / alias 一致 +0.05 / 緩い長音一致 +0.02）を
検索結果から一意に決められるようにする。alias 展開された参照は元のキーと同じ終端に
合流しうるため、`MatchKind` はキー単位ではなく**キーからエントリへの参照**が持つ
（§15.5 の `EIDX`）。

クエリ側の評価順は §14.3 のとおり完全一致 → alias 一致 → 緩い長音一致とする。
alias キーは同じ trie に載るため、正規化済みキーでの 1 回の検索が `MatchKind::Exact` と
`MatchKind::Alias` の両方を返す。この検索が空を返したときに限り、長音を除去したキーで
再検索し、その結果に `MatchKind::LongVowelRelaxed` を付ける。評価順は結果の並べ替えで
表現し、上位の一致があっても下位の一致を捨てない（`exact_reading_bonus` の差として
スコアに現れる）。

エントリが保持する `reading`（§14.2）は正規化前の原表記であり、正規化は
`normalized_reading` 側にだけ効く。表示・エクスポートは `reading` を使う。

### 15.4 検索 API の契約

**探索の向きが逆な 2 つの検索を別 API に分ける。** 混同すると、同じモード名で静的層と
mutable 層が別の結果を返すことになる。

- **common-prefix 検索**: 登録キーが**入力の接頭辞**であるものを返す。入力
  「とうきょうと」に対し「とう」「とうきょう」が返る。変換対象の読みを辞書語で切り出す
  向きであり、完全一致（`ExactMatch`）はこの特殊形。
- **predictive 検索**: 登録キーが**入力で始まる**ものを返す。入力「とう」に対し
  「とうきょう」「とうきょうと」が返る。入力途中の読みを補完する向きであり、M15 予測候補
  ウィンドウが要求するのはこちら。

型の正典は `core/include/azookey/core/DoubleArrayTrie.h` の `PrefixMatch` と `MatchKind` である。

| 型 | 要素 | 意味 |
|---|---|---|
| `PrefixMatch` | `key_length` | マッチしたキーのバイト長（UTF-8 の文字境界で終わる） |
| 同上 | `key_id` | KEYS セクションの index（§15.5） |
| 同上 | `entry_ref_off` | EIDX セクション内のオフセット（§15.5） |
| 同上 | `entry_count` | このキーに紐づくエントリ数 |
| `MatchKind`（`uint8_t`） | `Exact` = 0 / `Alias` = 1 / `LongVowelRelaxed` = 2 | エントリへの参照の一致種別（§15.3）。数値は §15.5 の `EIDX` に書く値 |

```cpp
namespace azookey::core {

class DoubleArrayTrie {
 public:
  // 登録キーのうち key の接頭辞であるものを返す。max_results == 0 は無制限。
  void CommonPrefixSearch(std::string_view key, size_t max_results,
                          std::vector<PrefixMatch>& out) const;

  // 登録キーのうち key で始まるものを返す（key 自身を含む）。
  void PredictiveSearch(std::string_view key, size_t max_results,
                        std::vector<PrefixMatch>& out) const;

  // key 全体に一致する登録キーがあれば true。CommonPrefixSearch の
  // key_length == key.size() の項と同義。
  bool ExactMatch(std::string_view key, PrefixMatch& out) const;
};

}  // namespace azookey::core
```

共通の契約:

- **入力**は §15.3 で正規化済みの UTF-8 バイト列とする。正規化は呼び出し側の責務であり、
  `core` が提供する `NormalizeReading()` を通すこと。未正規化の入力を渡した場合、
  結果が空になることはあっても未定義動作にはならない。
- **出力順序**は `key_length` の昇順、同一長では `key_id` の昇順とする。同じ
  アーティファクトと同じ入力に対して常に同じ列を返す（決定的）。`PrefixMatch` は
  一致種別を持たない。同じキーに `Exact` と `Alias` のエントリが混在しうるため、
  `MatchKind` はエントリ単位で `EIDX` が持つ（§15.5）。
- **エントリの取り出し**は `EIDX[entry_ref_off .. entry_ref_off + entry_count)` が指す
  `ENTS` レコードを読む（§15.5）。`PrefixMatch` 自体はエントリの中身を持たない。
- **UTF-8 境界**: マッチは必ず文字境界で終わる。ビルド時に登録するキーが文字境界で
  終わるため、バイト単位走査でもこの性質が保たれる。不正な UTF-8 を含む入力は、
  一致しなかったものとして扱い、例外を投げない。
- **空入力**・欠落アーティファクトに対しては空の結果を返す。例外・アサートで落とさない。
- **`max_results` に達した場合**は `key_length` の短いものから順に埋めた時点で打ち切る。
  戻り値からは打ち切りの有無を区別できないため、上限は用途ごとに十分な値を呼び出し側が
  与える（§15.8）。

検索ごとに異なる点:

| | `CommonPrefixSearch` | `PredictiveSearch` |
|---|---|---|
| 返すキー | 入力の接頭辞である登録キー | 入力で始まる登録キー |
| 計算量 | 入力長 `n`、返却件数 `m` に対して O(n + m) | 入力長 `n`、返却件数 `m`、走査ノード数 `k` に対して O(n + k)。`k` は `max_results` で抑える |
| 打ち切り | 実用上ほぼ起きない（`m ≤ n`） | 起きうる。M15 は候補窓の表示件数に見合う上限を与える |

`PredictiveSearch` は入力に対応するノードまで遷移したあと、そのノードの部分木を
たどって終端を集める。double-array では、ノード `s` の子は `check[base[s] + c] == s`
を満たすバイト `c` として得られる。走査は幅優先とし、同じ深さでは親の辞書順と
`c` の昇順を保つ。これにより、上記の短いキー優先という契約を満たす。
`key_id` はキーの UTF-8 バイト辞書順に割り当て、同じ深さの終端もその順に返す。
`max_results` に達した時点で走査を打ち切る。文字境界は走査中にも検証する。

呼び出し側は次の 3 つを想定する。

1. `DictionaryStore::Lookup`（§14.7）の完全一致経路。`ExactMatch` を使う。
2. M15 予測候補ウィンドウへの補完供給。`PredictiveSearch` を使う。
3. 将来のラティス分割（複合語の切り出し）。`CommonPrefixSearch` を使う。M53 の範囲外
   であり、契約だけを満たしておく。

### 15.5 直列化フォーマット `.azdic` v1

単一ファイル、リトルエンディアン固定、全セクション 8 バイト境界、メモリマップ可能とする。
Windows は `CreateFileMappingW` / `MapViewOfFile`、テスト用の POSIX 経路は `mmap` を使い、
マップに失敗した場合は全読みにフォールバックする。

ヘッダ（64 バイト固定）:

| offset | size | field | 内容 |
|---|---|---|---|
| 0 | 8 | `magic` | `"AZDIC1\0\0"` |
| 8 | 2 | `format_version` | u16。v1 は `1` |
| 10 | 2 | `header_size` | u16 = 64 |
| 12 | 4 | `flags` | u32。bit0 = alias キー展開済み（§15.3）。未定義ビットは 0 |
| 16 | 4 | `layer_id` | u32。§14.1 の層識別子 |
| 20 | 4 | `entry_count` | u32 |
| 24 | 4 | `key_count` | u32 |
| 28 | 4 | `section_count` | u32 |
| 32 | 8 | `content_hash` | u64。先頭 `header_size` バイトを除く全バイト（セクションテーブルを含む）の FNV-1a 64 |
| 40 | 24 | `reserved` | 0 埋め |

セクションテーブルは offset 64 から `section_count` 個、1 エントリ 24 バイト
（`u32 type` / `u32 reserved` / `u64 offset` / `u64 size`）。

| type | 内容 |
|---|---|
| `TRIE` | double-array の base / check 配列。受理されたキーの値は `key_id`（u32） |
| `KEYS` | キーレコード列（`key_count` 個、8 バイト固定）。下記 |
| `EIDX` | エントリ参照レコード列（8 バイト固定）。キーからエントリへの間接参照 |
| `ENTS` | 固定長エントリレコード列（下表） |
| `STRS` | 文字列プール（UTF-8 の連結。終端子なし） |
| `META` | UTF-8 JSON。由来・ライセンス・ビルドレシピ（§15.6） |
| `SIDX` | 任意。表層形索引（`ENTS` の index 列、下記） |

`TRIE` の 1 ノードは 16 バイト固定の `u32 base` / `u32 check` /
`u32 key_id` / `u32 depth` とする。root は index 0、`check = 0`、
`depth = 0`。未使用ノードの `check` と非終端の `key_id` は `0xffffffff` とする。
`depth` は root からのバイト数であり、遷移先では親の値より必ず 1 大きい。
これを参照時に検証し、破損した循環参照による走査の継続を防ぐ。
`LayerId` は §14.1 の列挙順に 0〜7 を割り当てる。`.azdic` の対象は静的層の 0〜4 とする。
`category_mask` の bit 0〜9 は §14.4 の表の順に対応する。

ロード時の処理量を制限するため、セクション数は最大 64、`META` は最大 1 MiB、
`pos_table` は非空で最大 65536 要素とする。これらの上限を超える入力は拒否する。

**キーからエントリへの対応**。trie は受理したキーに対して `key_id` だけを返す。
`KEYS[key_id]` は 8 バイト固定で `u32 entry_ref_off` / `u32 entry_count` を持ち、実エントリは
`EIDX[entry_ref_off .. entry_ref_off + entry_count)` が指す `ENTS` のレコードである。
`EIDX` の 1 要素も 8 バイト固定で、`u32 entry_index`（`ENTS` の index）/ `u8 kind`
（`MatchKind`。ビルド時に確定するのは `Exact` か `Alias`）/ `u8 reserved[3]` を持つ。

`EIDX` を挟むのは、alias キー（§15.3）がエントリの複製なしに同じ `ENTS` レコードを
指せるようにするためである。`ENTS` は `(normalized_key, surface, source)` の昇順に
並ぶため、1 つの alias キーに集まるエントリは連続しない場合がある。`EIDX` があれば
任意の集合を 1 つの範囲として表現できる。ヘッダの `entry_count` は `ENTS` の
レコード数（論理エントリ数）であり、キー展開による重複を含まない。`EIDX` 内の並びは
`entry_index` の昇順とする（決定的）。

**`kind` をキー単位ではなくエントリ単位で持つ**理由は、同じ正規化キーに一致種別の異なる
エントリが集まるためである。「ば」を元キーとする語と、「ヴァ」から alias 展開されて「ば」に
なった語は同じ終端（同じ `key_id`）に集まるが、前者は完全一致 +0.10、後者は alias 一致
+0.05 で採点する（§14.11）。キー単位の `kind` ではこの区別ができない。

長音緩和の二次検索（§15.3）が返す一致は、`EIDX` の `kind` が何であっても
`MatchKind::LongVowelRelaxed` として報告する。二次検索は緩い一致であり、
`exact_reading_bonus`（§14.11）は最も弱い値を採るのが正しいためである。

エントリレコード（32 バイト固定）:

| offset | size | field |
|---|---|---|
| 0 | 4 | `surface_off`（`STRS` 内オフセット） |
| 4 | 4 | `surface_len` |
| 8 | 4 | `reading_off`（正規化前の原表記） |
| 12 | 4 | `reading_len` |
| 16 | 2 | `pos_id`（`META.pos_table` の index） |
| 18 | 2 | `category_mask`（§14.4 のカテゴリのビットマスク） |
| 20 | 2 | `cost`（i16、mecab 互換） |
| 22 | 2 | `frequency_q`（u16。`frequency × 65535` の量子化） |
| 24 | 1 | `source`（層識別子） |
| 25 | 1 | `priority_q`（u8。`priority × 255`） |
| 26 | 6 | `reserved` |

§14.2 の `created_at` / `updated_at` は `.azdic` に持たない。静的層は
`obsolete_penalty` が常に 0（§14.11）で日付を必要とせず、日付を埋めるとビルドの
再現性（§15.6）を壊すためである。アーティファクトの世代は日付ではなく、`META` が持つ
上流リビジョンとビルダ版で識別する。

`priority`（§14.2）は §14.11 のスコア式に現れない。v1 のビルダは層固定値
（当該層の `source_priority`）を `priority_q` に書き込むが、**スコア計算には使わない**。
`source_priority` を層から引くのと二重に加算すると §14.11 の確定係数が崩れるためで、
`priority_q` は debug probe 用の informational フィールドとして扱う。

**表層形索引（`SIDX`）**。再変換の逆引き（表層形の完全一致）に使う任意セクションで、
`entry_count` 個の `u32 entry_index` を `(surface の UTF-8 バイト列, entry_index)` の昇順に並べる。
リーダーは二分探索で一致範囲を求め、範囲内のエントリを `entry_index` の昇順、すなわち
`ENTS` の順に返す。`ENTS` は読みの順に並ぶため、索引が無ければ逆引きは `ENTS` の全件走査になり、
同梱の `sudachi_lexicon` では 1 回ごとに `ENTS` 全体のページインを伴う。

`SIDX` は `format_version` を上げずに足せる。v1 のリーダーは未知の `type` を無視するため、
`SIDX` を持つアーティファクトも読める。`SIDX` を持たないアーティファクトは、
`ENTS` を全件走査して同じ結果を返す。`dictbuild` は常に `SIDX` を書き出す。

検証は 3 段に分ける。**どの段でも、違反を検出したらクランプや切り詰めで続行せず拒否する。**
範囲外のオフセットを有効範囲へ丸めると、別のエントリや別の文字列を正しい値として読んで
しまい、破損を検出せずに誤った候補を出すことになる。

**(1) 構造検証（ロード時に必ず実行、O(1)）**。以下をすべて満たさなければ拒否する。
算術はすべて 64 bit で行い、加算前に残余を比較して overflow を避ける
（`off > file_size || size > file_size - off` の形で判定し、`off + size` を作らない）。

- `magic` 一致、`format_version` が既知、`header_size == 64`、`flags` に未知ビットが無い。
- セクションテーブル全体（`64 + section_count × 24` バイト）がファイル長に収まる。
- 必須セクション（`TRIE` / `KEYS` / `EIDX` / `ENTS` / `STRS` / `META`）がそれぞれ
  **ちょうど 1 個**存在する。重複した `type` は拒否する。未知の `type` は無視してよい
  （前方互換）が、重複判定の対象からは外さない。
- 各セクションの `offset` が 8 の倍数で、`offset` と `size` がファイル長に収まり、
  他のセクションおよびヘッダとセクションテーブルの領域と**重ならない**。
- 固定長レコードのセクションは `size` がレコード長の倍数であり、件数がヘッダと一致する:
  `KEYS.size == key_count × 8`、`EIDX.size % 8 == 0`、`ENTS.size == entry_count × 32`。
  `SIDX` は存在する場合だけ `SIDX.size == entry_count × 4` を課す。
- `TRIE` の `size` が double-array の要素長の倍数である。

**(2) 参照検証（逆参照の時点、O(1)）**。内部参照は全件走査せず、実際に読む瞬間に検査する。
違反を 1 件でも検出した層は、その場で無効化して以後は空を返す。検査項目は次のとおり。

- trie が返す `key_id < key_count`。
- `KEYS[key_id]` について `entry_ref_off ≤ EIDX.count` かつ
  `entry_count ≤ EIDX.count − entry_ref_off`。
- `EIDX[i].entry_index < entry_count`、`EIDX[i].kind` が既知の `MatchKind`。
- `ENTS[j]` について `surface_off ≤ STRS.size` かつ `surface_len ≤ STRS.size − surface_off`。
  `reading_off` / `reading_len` も同様。`pos_id < META.pos_table` の要素数、
  `source` が既知の層識別子。
- 二分探索で読む `SIDX[i]` が `entry_count` 未満。求めた一致範囲の中に、表層形が
  クエリと異なる要素や、`entry_index` が昇順でない要素がある場合は、検索の不一致ではなく
  順序の破損として扱う。

全件を先に走査しないのは、`ENTS` が数十万件規模になり、ロード予算（§15.9 の 20 ms）を
超えるためである。逆参照時の検査は分岐 2 つで済み、検索のレイテンシ予算には影響しない。

**(3) 全体検証（明示的に要求されたときのみ、O(N)）**。`content_hash` の照合と、(2) の
参照検証の全件走査、復元キーの `key_id` 順が UTF-8 バイト辞書順であることの検査を行う。
`SIDX` があれば、`(surface, entry_index)` が狭義の昇順であること（`ENTS` の置換であること）も検査する。
C++ と `dictbuild --verify` の両方で検査し、検索時の順序検査は防御的措置として残す。
実行するのは pack の DL 直後、`dictbuild --verify`、および設定
`dictionary.verifyOnLoad` が真のときとする。

いずれの段でも、検出した層**のみを無効化**し、他層はそのまま動作を続ける。無効化は
§14.8 の missing-pack と同じ扱い（layer が空として振る舞う）とし、検出した検証条件を
ログに残す。

`.azdic` は §14.10 の pack 配布経路でもそのまま使う。pack 全体の SHA256 検証は
ダウンローダ側（M36-B / M32 の `HttpDownloader` 基盤）の責務であり、`content_hash` は
アーティファクト内部の自己整合性チェックとして独立に持つ。

### 15.6 マルチソースビルドパイプライン

`dictbuild` は次の段を順に通す。CMake からは既定 OFF のオプション
（`AZOOKEY_BUILD_DICT_TOOLS`）で構築し、IME の配布物には含めない。

1. **extract**: 上流データを中間 TSV（`*.lex.tsv`、UTF-8 / LF）へ変換する。上流ごとに
   スクリプトが分かれる。中間 TSV の先頭には `docs/licensing-policy.md` の
   ファイルヘッダ層として、由来元・ライセンス・`THIRD_PARTY_LICENSES` への参照を
   コメント行で置く。
2. **normalize**: §15.3 の規則で `normalized_reading` を作り、alias キーを展開する。
3. **score**: `cost` と `frequency` を確定する（下記）。
4. **merge**: 層内で重複を解決する（下記）。dedup は確定済みの `frequency` を見るため、
   score より後に置く。
5. **build**: `ENTS` を `(normalized_key, surface, source)` の昇順に確定させたうえで、
   キーを整列して double-array を構築し、`KEYS` / `EIDX` を作って `.azdic` を書き出す。
6. **verify**: 書き出したアーティファクトを読み直し、全キーの `ExactMatch` が入力と
   一致すること、`content_hash` が一致することを確認する。

**層内 dedup**（ビルド時）は、§14.12 の層をまたぐ実行時 dedup とは別の規則を使う。
キーは `(normalized_reading, surface)` とし、衝突したときは `frequency` が最大の
エントリを残す。`category` は union、`cost` は最小値（強いほう）を採る。`frequency` が
同点のときは入力ファイル名の昇順、次いで行番号の昇順で勝者を決める（決定的）。

**`cost` と `frequency` の写像**。上流が mecab 系コストを持つ層（SudachiDict 等）は、
上流 `cost` を `[-32768, 32767]` にクランプしてそのまま格納する。コストを持たない
curated 層（`named_entity` / `technical_terms`）の既定は `5000` とする。
`frequency` は層内統計に依存しない固定スケールで導く。

```
frequency = clamp((8000 - cost) / 10000.0, 0.0, 1.0)
```

`cost >= 8000` は 0.0、`cost <= -2000` は 1.0 に飽和する。層内の分布で正規化しないのは、
上流の更新でスケールが動くとアーティファクトの再現性と §14.11 の worked example が
崩れるためである。中間 TSV が `frequency` を明示している場合はその値を優先し、上式は
適用しない（curated 層で人手の重み付けを効かせるため）。

**再現性**。同じ入力と同じビルダ版からはバイト単位で同一の `.azdic` が出る。キーの整列は
`(normalized_key, surface, source)` のバイト昇順、タイムスタンプ・絶対パス・並列実行順序は
アーティファクトに入れない。CI は同一入力で 2 回ビルドし、`content_hash` の一致を確認する。

**上流データの取得**は SHA256 でピンしたダウンロードを使う（`sudachi_lexicon` の経路は
§15.12）。生の上流データは配布物に含めず、同梱するのは変換後の `.azdic` だけである
（§14.10 の同梱判定はアーティファクトに対して適用する）。

**帰属**は `META` セクションに持たせる。各アーティファクトは寄与した上流ごとに次を保持し、
`ThirdPartyNotices.txt` はこの `META` から生成する。

| フィールド | 内容 |
|---|---|
| `source_id` | 上流の識別子（`sudachidict-core` など） |
| `spdx` | SPDX 識別子。共通ライセンス本文の引き当てに使う |
| `upstream_url` | 一次情報の URL |
| `upstream_revision` | 取り込んだ版（タグ・コミット・リリース名） |
| `transform` | 変換スクリプトの識別子（再生成注記） |
| `copyright` | 上流固有の著作権表示（逐語） |
| `notice_ids` | notice catalog のエントリ ID 配列。空配列は「付随 notice 無し」を意味する |

SPDX 識別子だけでは `ThirdPartyNotices.txt` を作れない。共通ライセンス本文は引けても、
上流固有の著作権表示、Apache-2.0 の `NOTICE`、SudachiDict LEGAL が列挙する内包データの
帰属（§14.10）、GeoNames の CC-BY-4.0 クレジット文は復元できないためである。これらは
リポジトリ内の **notice catalog**（`dictbuild/notices/<notice_id>.txt`、逐語のテキスト）に
置き、`META` は `notice_ids` でそれを参照する。カタログ本文はリポジトリで版管理され、
アーティファクトのビルドとは独立に差分がレビューできる。

生成の失敗条件を次のとおり定める。`ThirdPartyNotices.txt` の生成は、`META` に
`source_id` / `spdx` / `upstream_url` / `upstream_revision` / `copyright` のいずれかを欠く
上流がある場合、`notice_ids` が参照する ID が catalog に存在しない場合、いずれもエラーで
停止する（欠落を空欄として出力しない）。生成が失敗した状態のアーティファクトは配布物に
入れられない（§14.10 の配布ガードで検出する）。

`docs/licensing-policy.md` の三層 attribution のうち、テキストファイルではファイルヘッダが
担う層を、バイナリでは `META` が担う。集約ファイル層は生成された `ThirdPartyNotices.txt` と
ルート `THIRD_PARTY_LICENSES`、再生成注記層は `transform` が満たす。

### 15.7 mutable 層との併存

`user_dictionary`（M9）/ `auto_words`（M36-A）/ `app_specific_dictionary`（M48）は
`.azdic` へ移さない。既存の `user_dict.json`（`UserDictionary` の version 1 スキーマ）と
`auto_words.tsv` をそのまま使い、§14.13 の後方互換条件を維持する。

double-array は構築が全件走査であり、1 語の追加ごとに再構築するには向かない。これらの層は
即時反映・部分更新・quarantine つき atomic write という要件を持ち、`std::map` を基盤とする
現行実装のほうが要件に合う。

2 方向の検索（§15.4）はデータ構造を変えずに供給する。`std::map` はキー昇順であるため、
**predictive 検索**は `lower_bound(input)` から走査し、キーが `input` で始まらなくなった
時点で止めれば O(log N + 一致数) で得られる。**common-prefix 検索**は `input` の各接頭辞を
短いものから順に `find` すれば、読みの文字数 `n` に対して O(n log N) で得られる。どちらも
静的層と同じ意味論を返す（§15.8 の `LookupMode` は層の物理表現によらず同じ結果集合を指す）。

`PrefixMatch`（§15.4）はアーティファクトのエントリプールを指す索引であり mutable 層には
存在しないため、層の境界で交換するのは `DictionaryEntry` である（§15.8）。結果の並びは
静的層と同じく読みの短いものから先に並べる。

読みの正規化（§14.3）は mutable 層にも効かせる。各層はロードと挿入の時点で
`normalized_reading` を算出してそれをキーにし、ユーザーが入力した原表記（`UserWord::ruby`）は
レコード側に保持する。alias はメモリ内の索引にだけ §15.3 と同じ上限で展開し、
長音緩和はクエリ時に行う。alias 索引を永続化しないため、既存の
`user_dict.json` スキーマを維持できる。静的層と同じ展開規則を使い、特に
predictive 検索で、まだ入力していない部分の alias によって結果集合が変わることを防ぐ。

想定規模はいずれの層も数千から数万件である。この範囲では走査の実測差が候補提示の
レイテンシ予算（§15.9）に現れない。エントリ数が 10 万件を超えた層には警告ログを出し、
物理表現の見直しを別課題として扱う。

### 15.8 層インターフェースとロード失敗時の挙動

§14.7 の `IDictionaryLayer` に、物理表現を隠す検索インターフェースを置く。

```cpp
enum class LookupMode {
  Exact,             // キーが入力と完全一致
  CommonPrefix,      // キーが入力の接頭辞（変換対象の切り出し）
  PredictivePrefix,  // キーが入力で始まる（入力途中の補完）
};

class IDictionaryLayer {
 public:
  virtual LayerId Id() const = 0;
  // アーティファクト欠落・検証失敗・pack 未取得のとき false。
  virtual bool IsAvailable() const = 0;
  virtual void Lookup(std::string_view normalized_reading, LookupMode mode,
                      size_t max_results,
                      std::vector<DictionaryEntry>& out) const = 0;
};
```

- `.azdic` を持つ静的層は §15.4 の trie を、mutable 層は §15.7 の `std::map` 走査を
  使う。`DictionaryStore` からは区別しない。同じ `LookupMode` に対し、どの層も同じ
  意味論の結果集合を返す。
- `DictionaryStore::Lookup`（§14.7）は `LookupMode::Exact`、M15 の予測候補ウィンドウは
  `LookupMode::PredictivePrefix` を使う。`LookupMode::CommonPrefix` は将来のラティス
  分割向けであり、M53 では呼び出し側を持たない。
- `max_results` は層ごとに上限を掛ける。全層の結果は §14.12 の dedup を通したあと、
  §14.11 の `dictionary_score` の降順に並べる。
- `IsAvailable()` が false の層は、`EnableLayer` で有効にされていても結果に寄与しない。
  設定上は有効だがアーティファクトが無い状態（未 DL の pack、破損したアーティファクト）は、
  §14.8 の missing-pack と同じユーザー可視挙動になる。

### 15.9 サイズとレイテンシの予算

| 項目 | 予算 |
|---|---|
| 同梱 `.azdic` 合計（インストーラ圧縮前） | 120 MB 以下 |
| 常駐 RSS 増分（mmap 前提、trie イメージのページインのみ） | 40 MB 以下 |
| 層 1 つのロード（mmap + ヘッダ検証） | 20 ms 以下 |
| `CommonPrefixSearch` の p95（読み 16 文字以内、warm） | 0.2 ms 以下 |
| `PredictiveSearch` の p95（読み 4 文字、`max_results` = 32、warm） | 0.5 ms 以下 |

これらは設計上の予算であり、上流の実語彙数によって達成可能性が変わる。SudachiDict
（core 版）は全語彙を入れると `sudachi_lexicon` 単体で予算を超えるため、§15.12 の
足切りで絞り込む。§15.2 の trie 実装は切り替えない。上流の版を更新するときは
アーティファクトサイズを測り直し、同梱合計が予算内に収まることを確認する。

trie 単体の検索レイテンシとロード時間は `bench/` 配下のマイクロベンチで測る。
`docs/conversion-quality-benchmark-spec.md` §6.3 の `latency_*` / `memory_peak_mb` は
変換全体の指標であり、辞書層の寄与はその差分として観測する。M52 ベンチの指標定義に
辞書層専用の項目は足さない。

### 15.10 テスト計画

| 種別 | 内容 |
|---|---|
| ゴールデン round-trip | 固定の小さな入力セットから `.azdic` を生成し、既知の `content_hash` と一致することを確認する。ビルドの再現性（§15.6）を回帰から守る |
| 参照実装との一致 | ランダム生成した読みの集合について、`CommonPrefixSearch` と `PredictiveSearch` の結果が `std::map` ベースの素朴な実装と順序込みで完全一致することを確認する。両者が探索の向きを取り違えていないこと（入力「とう」に対し predictive は「とうきょう」を返し、common-prefix は返さない）を個別のケースで確認する |
| 層をまたぐ意味論の一致 | 同じ語彙を静的層と mutable 層の双方に入れ、`LookupMode` ごとに両者が同じ結果集合を返すことを確認する（§15.8） |
| 正規化と alias | §15.3 の各規則について、ビルド時展開されたキーとクエリ時の長音緩和が、期待する `MatchKind` を返すことを確認する。alias キーが `EIDX` 経由で元エントリと同じ `ENTS` レコードを指し、`entry_count`（ヘッダ）が重複を含まないことも確認する |
| 破損耐性（構造） | `magic` 破壊 / `format_version` 不一致 / 未知 `flags` ビット / 必須セクションの欠落と重複 / 8 バイト境界違反 / セクションの重なり / 固定長セクションの `size` がレコード長の倍数でない / ヘッダの件数と `size` の不一致 / `offset` と `size` の範囲外（overflow を誘う値を含む）/ ファイル末尾の切り詰め / `content_hash` 不一致 |
| 破損耐性（内部参照） | `key_id ≥ key_count` / `entry_ref_off + entry_count > EIDX.count` / `EIDX[i].entry_index ≥ entry_count` / `EIDX[i].kind` が未知 / `surface_off + surface_len > STRS.size` / `reading_off + reading_len > STRS.size` / `pos_id` が `META.pos_table` の範囲外 / `source` が未知の層識別子 / `SIDX[i] ≥ entry_count` / `SIDX` の一致範囲に表層形の異なる要素や重複がある / `SIDX` の順序違反 の各ケース |
| 表層形索引の互換 | `SIDX` による逆引きが `ENTS` の全件走査と同じ結果を同じ順で返すこと、`SIDX` を持たないアーティファクトと未知のセクションを持つアーティファクトが読み込まれて逆引きできること、`SIDX.size` の不一致を構造検証で拒否すること |
| 破損時の縮退 | 上記いずれのケースでも、当該層のみが無効化されて空を返し、値がクランプされて別のエントリや文字列として読まれないこと、プロセスが落ちず他層が機能することを確認する |
| 後方互換 | 既存 `user_dict.json` / `auto_words.tsv` が層として読み込まれる（§14.13 の再掲） |
| 帰属生成 | `META` から生成した `ThirdPartyNotices.txt` に、寄与した全上流の SPDX と帰属が含まれることを確認する |

### 15.11 物理層の受け入れ条件

物理層（`.azdic`）の受け入れ条件の定義も、論理層と同じく
[`plans/windows-port-roadmap.md`](../plans/windows-port-roadmap.md) の M53 節を正典とする
（§14.13 と同じ扱い）。本章 §15.2〜§15.10 は判定の根拠となる形式・契約・予算を定義し、
受け入れ条件を複製しない。roadmap M53 の物理層項目は、往復とビルド再現性は §15.2〜§15.3、
検索 API の契約は §15.4、層単位の無効化と既存永続化形式の維持は §15.5〜§15.7、
`META` 由来の帰属は §14.10、サイズとレイテンシ予算は §15.9 を参照する。

### 15.12 `sudachi_lexicon` の取得・抽出・足切り

本節は MSI の辞書生成・再現性検査（`sideload-packaging-spec.md` §4.1）から参照される。

**取得経路**。release workflow が `dictbuild/build_bundled.py` で、SudachiDict の raw lexicon
（core 版 = `small_lex.zip` + `core_lex.zip`）を毎回ダウンロードして生成する。版、URL、
SHA256 は同スクリプトに固定し、ハッシュが一致しない取得はビルドエラーとする。`--cache` は
取得済みアーカイブの置き場で、`--offline` はキャッシュだけを使う。生成した `.azdic` も
上流アーカイブもリポジトリには置かない。上流の版を上げるときは版と SHA256 を同時に更新し、
`dictbuild/sources/sudachi_lexicon.metadata.json` の `upstream_revision`、`THIRD_PARTY_LICENSES` の
SudachiDict 節、`dictbuild/notices/sudachidict-legal.txt`（上流 `LEGAL` の逐語）を合わせる。

**抽出規則**（`dictbuild/extract_sudachi.py`）。raw lexicon の 1 行を次のとおり中間 TSV へ写す。

| 中間 TSV | 写像 |
|---|---|
| `surface` | 表層形の列（`\uXXXX` エスケープを戻す） |
| `reading` | 読みの列をカタカナからひらがなへ変換したもの |
| `pos` | 品詞 1〜4 の `*` 以外を `-` で連結したもの（例: `名詞-固有名詞-人名-姓`）。活用型・活用形は持たない |
| `cost` | 上流 `cost` をそのまま（`frequency` は空欄とし、§15.6 の写像に従う） |
| `category` | 品詞が `固有名詞-人名` なら `person_name`、`固有名詞-地名` なら `place_name`、それ以外は `general` |

次の行は取り込まない。連接 ID が `-1` の分割専用エントリ、品詞 1 が `補助記号` / `空白` の
エントリ、読みがひらがなと長音符以外を含むエントリ（記号の読み「キゴウ」、英字や数字を含む
読み）、表層形が読みと同じひらがなのエントリ、表層形が `#` で始まるか TAB・改行・NUL・
二重引用符を含むエントリ（中間 TSV のコメント行と引用符の扱いと衝突するため）、固有名詞以外の
エントリのうち表層形が異体字を含むもの。

**異体字の除外**（DEV-1651）。異体字とは、CP932 で表せない漢字のうち、上流の正規化形（列 13）が
同じ位置で CP932 で表せる別の漢字に置き換えるものである（`硏究` → `研究` から `硏`、
`仁俠` → `任侠` から `俠`）。対応表は SudachiDict の全 lexicon から実行時に求め、手で持たない。
除外はエントリ自身の正規化形に依らず、その字を含む固有名詞以外の全エントリに掛かる
（`俠` 単独のように自身の正規化形では直らない語も落ちる）。固有名詞（`髙島屋` など）は、
異体字が正しい綴りでありうるため残す。常用漢字表（2010）が通用字体とする `剝`・`塡`・`頰`・
`𠮟` は、上流が CP932 の字へ正規化しても異体字の集合から外し、除かない
（`STANDARD_FORM_EXCEPTIONS`。`剝がす`、`充塡` などが残る）。20260723 版ではこの規則で
1,015 エントリを除く（候補に `硏究`、`俠` が出ていた。例外で残る語は 198 エントリ）。除かれる語のうち同じ読みの項目が他に無いものが 16 件あり、
`喰ひ繫ぎ`・`濁り醬` など古い表記や希少な語である。順位を下げる案は取らない（辞書形式と core の
変更が要り、効果が弱いため）。

**足切り**。上流 `cost` が 10000 を超えるエントリを捨てる（`--max-cost` の既定値）。
上流の core 版を全件入れると `sudachi_lexicon` 単体で §15.9 の同梱合計を超え、
`cost` 10000 超は希少な活用形と長い固有名詞が大半を占めるためである。品詞による足切りは
しない。20260723 版ではこの規則で 623,065 エントリ、388,011 キー、約 70 MB の
アーティファクトになる。

**同梱ビルドと再現性**。`build_bundled.py` は SudachiDict に続けて、プロジェクト自作の
`dictbuild/sources/technical_terms_lexicon.lex.tsv` を同じビルダで生成する。技術用語seedの
revision は `authored-v1`、ライセンスは Apache-2.0、category は `technical`（必要に応じ
`software` も付与）とし、由来は隣接する metadata JSON に記録する。外部辞書は転記しない。
帰属は層の固定順で `ThirdPartyNotices.txt` へ集約する。
生成完了時に `bundle-provenance.json` を書き、生成器とその依存コード、両層のmetadata、
技術用語seed、参照するnoticeのリポジトリ相対パスとSHA-256を固定順で集約する。
記録は入力全体のfingerprint、Sudachi revisionとアーカイブのSHA-256 pin、辞書2層と
帰属表示のSHA-256を結ぶ。生成の前後で入力が異なる場合は成功の記録を残さない。
絶対パス、生成時刻、commitはこの記録に含めず、同一入力での決定性を保つ。
実機検証zipの生成では `check_bundle.py --vm-package-json` が形式・出所・入力と出力の一致を検査する
（`dev-infrastructure-spec.md` §2.6）。この記録はローカル成果物の取り違え・古い入力の検出に使い、
署名済み配布物の真正性を保証するものではない。
release workflow は同じキャッシュを使って別出力先へ2回生成し、2回目を `--offline` とする。
`check_bundle.py --compare <2回目の出力先>` は両方の配布ガードに加え、層集合、ヘッダの
`content_hash`、ファイルサイズ、全バイトのSHA256と帰属文書の一致を検査する。
独立した2回の生成が一致しなければMSIを作成しない。

### 15.13 ビルダと Host の接続契約

`dictbuild/dictbuild.py` はローカルの中間 TSV を複数受け取り、層ごとに `.azdic` と
帰属テキストを出力する。Python 3.10 以降を使う。CMake の `AZOOKEY_BUILD_DICT_TOOLS`
はビルダの案内 target を有効にする。ビルダとテスト用入力を IME に同梱しない。

中間 TSV のヘッダ列は `surface` / `reading` / `pos` / `cost` / `frequency` /
`category` / `source_id` とする。`category` は §14.4 の識別子のカンマ区切りである。
`cost` と `frequency` の空欄は §15.6 の既定規則に従う。先頭コメントには
`SPDX-License-Identifier:` と `THIRD_PARTY_LICENSES` への参照を含める。
入力ファイルの basename は互いに異なるものとし、重複解決順に絶対パスを使わない。

`--metadata` の JSON は §15.6 の上流情報を `sources` 配列で指定する。
各 TSV の `source_id` と `sources` は寄与元の集合として一致させる。
`pos_table` は入力の `pos` 文字列を辞書順に並べて生成し、元の品詞名を保存する。
`--catalog` は共通ライセンス本文 `<spdx>.txt` と付随 notice のディレクトリを指定する。
帰属欠落、本文欠落、不正な頻度、未知のカテゴリはビルドエラーとする。

```powershell
python dictbuild/dictbuild.py source-a.lex.tsv source-b.lex.tsv `
  --metadata sources.json --layer technical_terms_lexicon --catalog notices `
  --output technical_terms_lexicon.azdic --notices ThirdPartyNotices.txt
python dictbuild/dictbuild.py --verify technical_terms_lexicon.azdic
```

Windows Host は実行ファイルの隣の `dict` ディレクトリから、静的層名をファイル名に
した `.azdic` を探索する。`neologd_lexicon` は §14.10 の別 pack 経路に限定し、
この自動探索に含めない。欠落・破損した層は空として扱う。
ユーザー辞書の JSON 読み書きは `UserDictionary` が所有し、予測用の索引は
メモリ内 revision の変更時に更新する。M9 の明示的なユーザー辞書スコアは維持する。

Host の辞書予測は入力と完全一致する検索キーを除外する。プロバイダの `limit` は
スコア降順の返却件数上限で、0 は空を返す。内部の `max_results` は層ごとの検索キー数
上限であり、同じキーの全レコードを比較するコストは残る。返却上限により後段の
reranker へ渡す辞書候補は既定で32件以下となる。

予測欄は最大5件とし、学習予測を最大3件、変換器の文脈つき予測を最大2件、
辞書予測を最大2件の順に空き枠へ採用する。残る空き枠には変換器の残りを入れる。
表層形は重複排除し、変換器に異なる候補が2件あれば辞書スコアにかかわらず2件を確保する。
変換器と辞書はそれぞれ reranker を適用し、層間スコアの大小で予約枠を変更しない。

`UserWord::value` は有限の実数をそのまま最終辞書スコアに使う。未指定時は現在の
`user_word_default_score` を使い、`frequency` の `[0,1]` へ圧縮しない。
索引内の頻度は層の重複統合用に保持し、明示スコアへの層加点や alias 加点は行わない。
`Candidate::reading` は完全一致検索では入力の読み（長音緩和前）、予測・共通接頭辞検索では
レコードの原表記とする。正規化形は検索と重複統合のために使う。

`IsAvailable` は有効設定や件数によらず、静的層のロード成功または mutable 層の初期化済み
状態を表す。空の mutable 層も初期化後は利用可能とする。層 ID 不一致は
`layer id mismatch` として診断を保持し、自動探索ログには `error` を含める。
探索対象の列挙とロードは共通コードで検証し、実行ファイルの位置解決は Windows で検証する。

META の JSON 読み取りは独立した `azookey_json` ターゲットを core と ipc から利用する。
既存の include パスと名前空間は維持するが、core は Named Pipe / Envelope 実装へ依存しない。

### 15.14 `neologd_lexicon` pack の形式と生成

§14.10 の別 pack は、`neologd_lexicon` 層（layer id 2）の `.azdic` v1 と、
それを記述するマニフェスト `neologd_lexicon.manifest.json` の 2 ファイルで構成する。
`.azdic` は同梱層と同じビルダとリーダを使い、SIDX を含む §15.5 の規則に従う。

**ファイル名**。pack は `neologd_lexicon-<revision>.azdic` とする。`<revision>` は上流
seed のリリース名で、英数字で始まり英数字・`.`・`_`・`-` だけからなる 64 文字以下の
文字列である。版をファイル名に含めるのは、Host がメモリマップ中の旧 pack を残したまま
新しい pack を取得し、検証後に層を差し替えられるようにするためである。

**抽出規則**（`dictbuild/extract_neologd.py`）。mecab-ipadic 形式の seed CSV
（13 列。`.csv` または公開形式の `.csv.xz`）の 1 行を次のとおり中間 TSV へ写す。

| 中間 TSV | 写像 |
|---|---|
| `surface` | 表層形の列 |
| `reading` | 読みの列をカタカナからひらがなへ変換したもの |
| `pos` | 品詞 1〜4 の `*` 以外を `-` で連結したもの |
| `cost` | 上流 `cost` をそのまま（`frequency` は空欄） |
| `category` | `名詞-固有名詞-人名` は `person_name`、`名詞-固有名詞-地域` は `place_name`、`名詞-固有名詞-組織` は `company_org`、それ以外は `neologism` |

取り込まない行は、§15.12 の除外条件のうち次のものとする。読みがひらがなと長音符以外を
含むエントリ、表層形が読みと同じエントリ、表層形が `#` で始まるか TAB・改行・NUL・
二重引用符を含むエントリ。seed には分割専用エントリと記号の品詞が無いため、それらの条件は
持たない。`cost` による足切りもしない。

中間 TSV の先頭コメントは §15.13 の規則に従う。ただし `THIRD_PARTY_LICENSES` への参照は、
この pack が `THIRD_PARTY_LICENSES` の対象外であり、帰属はマニフェストが持つことを示す文として書く。

**帰属の正典**は `dictbuild/sources/neologd_lexicon.metadata.json` とする。
`notice_ids` は上流 `COPYING` の逐語（`dictbuild/notices/mecab-ipadic-neologd-copying.txt`）を
参照する。`upstream_revision` は生成時に `--revision` の値で置き換える。pack の `META` の
`sources` と、マニフェストの `attribution` はいずれもこのファイルから生成し、別々に編集しない。

**マニフェスト**（`manifest_version` 1）は次のフィールドだけを持つ。

| フィールド | 内容 |
|---|---|
| `manifest_version` | `1` |
| `pack_id` | `neologd_lexicon` |
| `layer_id` | `2` |
| `format` / `format_version` | `azdic` / `1` |
| `builder_version` | `.azdic` の `META.builder_version` と同じ値（`azdic-1`） |
| `upstream_revision` | 取り込んだ seed のリリース名 |
| `file_name` | `neologd_lexicon-<upstream_revision>.azdic` |
| `size` | pack のバイト数（JSON の整数表記。`1.0` のような小数表記は不正） |
| `sha256` | pack 全体の SHA256（小文字 16 進 64 桁） |
| `url` | pack の取得先（`https://` で始まる、空白を含まない印字可能 ASCII） |
| `attribution` | `sources`（pack の `META.sources` と同値）と `notices`（DL 前に提示する帰属テキスト。§15.6 の ThirdPartyNotices と同じ生成規則） |

`url` と `sha256` がともに空のマニフェストは「公開された pack が無い」ことを表し、
このとき `upstream_revision` と `file_name` は空、`size` は 0 とする。Host はこの状態を
missing-pack（§14.8）として扱い、`neologdEnabled` が真でも層を有効にしない。

**pin したマニフェスト**。アプリが信頼するマニフェストは `dictbuild/packs/neologd_lexicon.manifest.json`
である。期待 SHA256 の信頼の起点はこの pin であり、配布元から取得したマニフェストや
sidecar のハッシュは使わない。pack を公開するときは、上流の個別条件（§14.9）を審査した
うえでこのファイルを公開物の値に更新する。`attribution` が metadata と notice catalog から
生成した値と一致しないマニフェストは不正とする（`neologd_pack.py check-pinned`）。

**生成と検証**（`dictbuild/neologd_pack.py`）。

```powershell
python dictbuild/extract_neologd.py mecab-user-dict-seed.<revision>.csv.xz `
  --revision <revision> --output neologd.lex.tsv
python dictbuild/neologd_pack.py build neologd.lex.tsv --revision <revision> `
  --url https://<host>/neologd_lexicon-<revision>.azdic --output out
python dictbuild/neologd_pack.py verify out/neologd_lexicon.manifest.json `
  out/neologd_lexicon-<revision>.azdic
python dictbuild/neologd_pack.py pin --from out/neologd_lexicon.manifest.json
```

取得した pack は、層としてロードする前に次の順で検証し、いずれかに失敗した pack は
使わない。(1) マニフェストが公開済みの pack を指す。(2) バイト数が `size` と一致する。
(3) SHA256 が `sha256` と一致する。(4) §15.5 の全体検証に通る。(5) ヘッダの layer id が 2 である。
(6) `META.sources` が `attribution.sources` と、`META.builder_version` が `builder_version` と一致する。(3) は `HttpDownloader`（§5-4）が
取得時にも行い、そのとき `size` を取得の上限バイト数として渡す。

**Host での取得とロード**（`inference-host/src/NeologdPack.cpp`）。

- **起動時だけ**: Host は起動時に一度だけ pack を扱う。`dictionary.neologdEnabled` が真で、
  SafeMode でないときに限る。設定の再読込（§14.8）は層の有効フラグだけを更新し、pack の
  取得やロードはしない。
- **offline**: `privacy.mode=offline` の間は通信しない。宛先に SHA256 の一致するファイルが
  あればそれを検証してロードし、無ければ取得せずに終える。設定の再読込で offline へ
  切り替えると、取得中の通信をキャンセルする。キャンセルは WinHTTP の接続・応答・本文の
  待ちの間にも確認するので、その timeout（接続・送信 15 秒、受信 30 秒）を待たずに止まる。
  `Dispatcher::HandleUpdateConfig` は有効な再読込のたびに、Fetcher の設定 callback の直後に
  通信の可否を通知する（`DispatcherConfig::on_network_policy_applied`）。不正な再読込では
  通知せず、直前の可否を保つ。通常モードへ戻しても再取得はせず、
  次の起動で `neologdEnabled` に従う。保存された `neologdEnabled` は変更しない。
  設定アプリの「再起動後に取得します」の表示は、offline の間は取得しないことを併記する。
- **pin の読み方**: pin したマニフェストは、ビルド時に `azookey_host` へバイト列として
  埋め込む。配布物に `neologd_lexicon.*` のファイルは置かないので、§14.10 の配布ガードは
  そのまま成り立つ。
- **公開されていないとき**: マニフェストが未公開なら、ネットワークにもファイルにも
  触れずに missing-pack とする。
- **取得**: 公開済みなら `%LOCALAPPDATA%\azooKey\packs\<file_name>` を宛先に
  `HttpDownloader` を呼ぶ。宛先に SHA256 の一致するファイルがすでにあれば、通信せずに
  それを使う。取得は別スレッドで行い、Host の起動と終了を待たせない。
- **検証してからロード**: 取得したファイルは、層とは別の `DoubleArrayTrie` で (2) と
  (4)〜(6) を検証する。すべて通ったものだけを `neologd_lexicon` 層へロードする。
  検証に失敗した pack は層に入らず、層は missing-pack のまま残る（§15.8）。
  検証を通った後のロードが失敗した場合は、`DictionaryStore::LoadStatic` の規則どおり層は
  利用不可となり、いずれの場合も候補に寄与しない。
- **旧版の削除**: ロードできる pack がそろったら、`packs` 内の別の版の
  `neologd_lexicon-*.azdic` と、その `.part` を削除する。
- **ログ**: 結果は `neologd_pack_load` イベントに記録する。`result` は `ok` /
  `missing_pack` / `offline` / `error` のいずれかで、失敗時は固定の分類名を `reason` に入れる。
  `offline` は offline のために取得しなかったことを表す。
  パスとサーバの応答はログに出さない。
- **状態の照会**: 同じ結果を `QueryDiagnostics` 応答の任意フィールド `neologd_layer`
  （`docs/dev-infrastructure-spec.md` §12.6）で返す。`state` は次のいずれかで、`reason` は
  `error` のときだけログと同じ分類名を入れる。値は Host プロセスの寿命の間だけ保持し、
  永続化しない。

  | `state` | 意味 |
  |---|---|
  | `not_requested` | この起動では pack を扱っていない（`neologdEnabled` が偽、SafeMode、`packs` が無い、offline で検証済みのコピーが無い） |
  | `loading` | 取得・検証・ロードの途中 |
  | `ready` | 層へロードした |
  | `missing_pack` | マニフェストが未公開 |
  | `error` | 取得・検証・ロードのいずれかで失敗した。層は missing-pack のまま |

  Host が pack を扱うのは起動時だけなので、起動後に `neologdEnabled` を真にしても、
  Host が再起動するまで `not_requested` のままである。

  設定アプリは「辞書」ペインの切替の下に、この状態を `QueryDiagnostics` で読んで表示する。
  切替が真のときの `not_requested` は失敗ではなく、「Host の再起動後に取得する」（切替が未保存なら
  「保存すると、Host の再起動後に取得する」）と示す。`error` のときは `reason` を添える。
  Host が `neologd_layer` を返さない（旧版の）ときは、状態を返さない旨を示す。

**設定アプリでの提示と同意**（`settings-app/NeologdAttribution.*`、`settings-app/MainWindow.xaml.cpp`）。
§14.9 / §14.10 の「DL 画面」は、設定アプリの「辞書」ペインにある `neologdEnabled` の切替である
（`docs/settings-app-spec.md` §3.2）。

- **文面の出所**: 設定アプリも pin したマニフェストをビルド時にバイト列として埋め込み
  （`settings-app/EmbedNeologdPin.cmake`）、`attribution.notices` を逐語で表示する。UI 側に
  帰属の文面を持たない。埋め込んだバイト列がマニフェストと一致することは
  `azookey_settings_persistence_tests` が確かめる。
- **同意**: 切替を偽から真へ操作したときに `attribution.notices` を表示するダイアログを出し、
  「同意して有効化」を選んだときだけ真にする。取り消すと偽へ戻す。保存済みの値が真のときや、
  保存の操作ではダイアログを出さない。帰属情報を読めないときや、ダイアログを表示できないときは真にせず、理由を表示する。
  帰属は切替の下のボタンからいつでも表示できる。
- **未公開のとき**: マニフェストの `url` が空なら、切替は操作できるまま、公開された pack が
  無いため取得しない旨を表示する。Host はこの状態を missing-pack として扱う。
- **反映の時期**: 真にして保存したときは、Host の再起動後（サインアウトして再度サインイン）に
  取得する旨を表示する（`docs/settings-app-spec.md` §3.6「反映方法の確定」）。未公開の
  ときは再起動しても取得しないので、取得しない旨だけを表示する。偽に戻して保存したら、この表示を閉じる。
- **既知の制限**: 同意を記録する設定キーは持たず、opt-in は `neologdEnabled` だけである。
  `settings.json` を直接編集して真にすると、帰属の提示を経ずに Host が取得する。
  pin したマニフェストの `attribution` が更新されても、すでに真のユーザーへ再同意は求めない。
