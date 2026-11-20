---
kind: spec
milestones: [M73]
canonical_for: ["同音異義語の注記"]
defers_to: [docs/candidate-rewriter-spec.md §10]
---

# 同音異義語の注記 仕様

対象リポジトリ: dolquis/azooKey-Desktop
対応マイルストーン: M73（追加機能）
関連: `docs/candidate-rewriter-spec.md`（§8.2 の `CandidateField.description`、§10.1 の注記付きの候補窓、§10.4 の
      UI-less での劣化、§14 の既定 OFF の不変条件、§15 の帰属の具体化）、`docs/native-ui-spec.md`（§4.1 の候補窓の幅）、
      `docs/licensing-policy.md`（三層 attribution とデータ採用の手順）

「制作」と「製作」のように読みが同じ候補は、字面だけでは選び分けにくい。本書は、同音異義語の候補に使い分けを示す
短い注記を付ける機能を定める。注記は候補の `description` 欄で運び、M62 の注記付きの候補窓に表層形と並べて出す。

## 節とマイルストーンの対応

| 節 | マイルストーン |
|---|---|
| §1〜§10 | M73 |

## 正典範囲

本書が正典であるもの: 注記データの出典と採否、データの形式と再ポートの規則、注記を付ける候補の決め方、Host での
合流、表示する経路と出さない経路、設定キー `homophoneNotes`、既定 OFF の不変条件と読み込みに失敗したときの扱い、
注記データの帰属。

本書が委ねるもの:

| 主題 | 正典 |
|---|---|
| `CandidateField.description` の wire 表現と互換性 | `ipc/include/azookey/ipc/Payloads.h`、`docs/candidate-rewriter-spec.md` §8.2 |
| 注記付きの候補窓の view-model と描画 | `docs/candidate-rewriter-spec.md` §10.1、`docs/native-ui-spec.md` §4.1 |
| UI-less と `ITfCandidateListUIElement` で注記を落とす規則 | `docs/candidate-rewriter-spec.md` §10.4 |
| 三層 attribution の運用規約と同梱できるライセンス | `docs/licensing-policy.md` |
| 受け入れ条件 | `plans/windows-port-roadmap.md` M73 |

## 1. 目的と範囲

同じ読みの候補が候補窓に並ぶとき、各候補に使い分けを示す短い注記を付ける。注記は表示だけに使い、候補の順序、
確定文字列、学習は変えない。

範囲に含めないもの:

- 活用した形への注記（「会った」「合った」など）。データの表層形と完全に一致する候補だけを対象にする（§4）。
- 注記の全文（用例を含む語釈）の表示。候補窓には §3.3 で短くした注記だけを出す。
- 文脈に応じて注記を選び分けること。注記は読みと表層形だけで決まる。

## 2. 出典とライセンス

注記データは japanese-usage-dictionary（<https://github.com/hiroyuki-komatsu/japanese-usage-dictionary>）の
`usage_dict.txt` から再ポートする。

| 項目 | 内容 |
|---|---|
| ライセンス | BSD-2-Clause |
| 著作権表記 | Copyright (c) 2011, Hiroyuki Komatsu, Shoji Ogura, Tsuyoshi Horo |
| 目的 | 同音異義語を区別するための項目。Mozc 向けに作られ、他の用途にも使えると README が述べる |
| 固定する版 | tag `2025-01-25`。Mozc の `src/MODULE.bazel` が同じ tag の archive を SHA256 で固定して取り込む |
| 形式 | TSV。列は読み、表層形、品詞、語釈（用例付き） |

BSD-2-Clause は `docs/licensing-policy.md` の同梱できるライセンスに含まれる。

語釈の元（既存の辞書から写したものか、著作権者が書き下ろしたものか）は、README と LICENSE に書かれていない。
本書はこれを未確認として扱う。出典の repo が来歴を示したときは、本節を見直す。

採らなかった出典と理由は次のとおりである。

| 出典 | 理由 |
|---|---|
| Mozc 本体の `src/data` | 用例の辞書を持たない。Mozc は上記の外部 repo を参照している |
| JMdict、Wiktionary | CC-BY-SA（コピーレフト）であり、`docs/licensing-policy.md` で同梱できない |
| 文化庁「「異字同訓」の漢字の使い分け例」 | 文化庁のサイトは、特別な記載がない限りコンテンツの著作権は文化庁にあるとする。この資料の利用条件は確かめられない。対象も異字同訓であり、同音異義語の熟語を含まない |
| 自作 | 同梱はできるが、1,700 語規模の注記を書き、正しさを人が確かめる手間が大きい。上記の出典を使えるので採らない |

## 3. データの形式と再ポート

### 3.1 配置と供給

注記データは `inference-host` が読む 1 本の TSV であり、絵文字データ（`docs/candidate-rewriter-spec.md` §7.1）と同じく、
Host の実行ファイルと同じ配布単位の `data` ディレクトリに読み取り専用の資産として置く。ファイル名は実装で決める。
配る経路も絵文字データと同じく、ファイルを個別に挙げる 3 か所に足す。ビルドで Host の出力先の `data` へ写す
`inference-host/CMakeLists.txt`、MSI の `pkg/msi/Package.wxs`（`RewriterDataDir`）、VM 検証パッケージの
`scripts/make-vm-verify-package.ps1`（同梱する `data` のファイルの一覧と、その一覧を照合する試験）である。
利用者が書き換える経路とホットリロードは持たない。TIP はこのファイルを読まない。

### 3.2 列定義

TSV は UTF-8（BOM なし）、改行は LF、区切りはタブ 1 文字とする。`#` で始まる行はコメントで、先頭のコメント行群が
§8 のヘッダを担う。

| 列 | 内容 |
|---|---|
| 1 `reading` | ひらがなの読み。原典の読みのまま |
| 2 `surface` | 表層形。原典の表層形のまま |
| 3 `note` | §3.3 で短くした注記 |

どの列にもタブと改行を含めない。`(reading, surface)` の組は重複させない。

### 3.3 再ポートの規則

`scripts/` に置く porter スクリプトが、固定した版の原典を取得し、本形式へ変換する（スクリプトの名前は実装で決める）。
原典のファイルをそのまま同梱しない。注記は語釈から次の順に作る。

1. 先頭の番号付きの語義（`1. `）があれば、番号を外して最初の語義だけを残す。
2. 原典の改行の印（`\n` の 2 文字）より前だけを残す。
3. 最初の用例の `「` より前だけを残す。
4. 末尾の `。` を外す。

- `〈〉` の中の `「` は、3 の用例の始まりとみなさない。
- 2 で、原典の `\n` の前がラベル（`〈…〉`）だけのときは、次の語義をつなげる。語義の先頭の `1. ` は外す。
- 前後の空白を外す。

例: 原典の「(特に美術品などを)作ること。「絵画を制作する」…」は、注記「(特に美術品などを)作ること」になる。

- 品詞の列は使わず、出力しない。
- 同じ `(reading, surface)` の行が原典に複数あるときは、原典で最初の行だけを使う。
- 注記が空になった行は落とす。
- 注記は字数で切り詰めない。長い注記は候補窓が省略する（§5）。

porter は決定的にし、同じ版の原典からは常に同じバイト列を出す。出力は `reading`、`surface` のコードポイント列の昇順に
並べる。出力の前に §3.2 の文法を全行で検査し、満たさなければ部分的な出力を残さずに失敗する。

## 4. 注記を付ける候補

Host は、1 つの候補列（`QueryCandidates` の応答の候補列と、`QueryBatchConversion` の各文節の候補列）ごとに、次の
条件をすべて満たす候補の `description` に注記を入れる。

1. 候補の `(reading, surface)` がデータにある。
2. 同じ候補列に、同じ `reading` でデータにある候補が、その候補を含めて 2 つ以上ある。
3. 候補の `description` が空である。rewriter（`docs/candidate-rewriter-spec.md`）が付けた注記は上書きしない。

2 の数は、Host が応答に入れる候補列で数える。TIP ローカルの候補（TIP が候補窓に足す rewriter の候補）は数えない。
2 の条件は、読みの同じ候補が 1 つしか並ばないときに注記を出さないためである。選び分ける相手の無い注記は、候補窓を
広げるだけになる。

突き合わせは `core/` の純粋関数とし、データの索引を引数に取る。ファイル I/O と IPC を持たない。Host は候補生成と
rewriter の合流を終えた後にこの関数を呼び、候補の順序と件数を変えない。

## 5. 表示

注記は M62 の注記付きの候補窓（`docs/candidate-rewriter-spec.md` §10.1）で、表層形の右に副次色で描く。

- 出すのは候補モード（`docs/native-ui-spec.md` §4.1）だけである。通常変換、先頭文節、一括変換の文節の候補窓を含む。
- 予測モード、ライブ変換の preedit、UI-less と `ITfCandidateListUIElement` の文字列には出さない（UI-less の規則は
  `docs/candidate-rewriter-spec.md` §10.4）。
- 確定文字列と学習観測に注記を入れない。
- 窓の幅は全ページの候補で測る（`docs/native-ui-spec.md` §4.1、M68）。ページを替えても窓の幅は変わらない。注記の欄は
  窓の最大幅の中に収め、超える注記は末尾を省略記号にする（同節）。

## 6. 設定キー

`settings/mvp-settings.schema.json` へ次を追加する（M73）。

| キー | 型 | 既定 | 説明 |
|---|---|---|---|
| `homophoneNotes` | boolean | `false` | 同音異義語の注記の有効化。`false` で §7 の不変条件が成り立つ |

データのパスは設定にしない。`UpdateConfig` で `false` から `true` に変わったときは、次の問い合わせでデータを読み込む。

## 7. 既定 OFF の不変条件と失敗したときの扱い

`homophoneNotes == false` のとき、次が成り立つ。

- Host は注記データを読まず、索引を作らない。
- 応答の候補の `description` が、本機能の導入前と一致する。
- 候補列、確定文字列、学習データが、本機能の導入前と一致する。

データの読み込みに失敗したとき（ファイルが無い、読めない、§3.2 の文法に合わない行がある）は、注記だけを止めて候補生成を
続ける。文法に合わない行は、その行だけを落として読み込みを続け、落とした行数を warning に含める。同じパスの成功と失敗は
キャッシュし、失敗の warning は初回だけ記録する。

## 8. ライセンスと帰属

`docs/licensing-policy.md` の三層 attribution モデルを、注記データに次のとおり当てはめる。

- **ファイルヘッダ**: TSV の先頭のコメント行に、出典（repo の URL と tag）、BSD-2-Clause と著作権表記、
  `THIRD_PARTY_LICENSES` への参照、porter のバージョン、§3.3 で短くした旨を書く。
- **集約ファイル**: ルート `THIRD_PARTY_LICENSES` に、SPDX `BSD-2-Clause`、著作権表記、ライセンスの全文、格納パス、
  派生である旨の節を足す。MSI は同ファイルを `THIRD_PARTY_LICENSES.txt` として同梱する（`pkg/msi/Package.wxs`）。
- **再生成注記**: §3.3 の porter スクリプトが変換手順を残す。

配布物の `ThirdPartyNotices.txt` は静的辞書層の帰属を集める生成物であり（`docs/sideload-packaging-spec.md` §4.1）、注記
データは辞書層ではないので、その生成経路に載せない。注記データの帰属は `THIRD_PARTY_LICENSES` が担う。

## 9. secure とログ

注記データは同梱の静的データであり、利用者の入力を含まない。secure の文脈（`docs/privacy-and-secure-input-spec.md` §5）
でも注記を付けてよい。注記を付けた候補の読み、表層形、注記はログに出さない。warning には件数とパスだけを出す。

## 10. テスト観点

- porter: §3.3 の各規則（番号付きの語義、改行の印、用例、末尾の句点、重複の扱い、空の注記）と、決定性、文法の検査。
- core: §4 の 3 条件（データに無い候補、読みの同じ候補が 1 つだけの候補列、rewriter の注記がある候補）と、候補の順序と
  件数が変わらないこと。
- Host: `homophoneNotes` の値による読み込みの有無と §7 の不変条件、読み込みに失敗したときに候補生成が続くこと、
  `QueryBatchConversion` の文節の候補列への合流。
