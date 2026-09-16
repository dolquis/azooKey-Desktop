---
name: azookey-core-conversion
description: azooKey Desktop の core/ 配下の変換パイプライン、ローマ字かな変換、SimpleConverter と IConverter、DoubleArrayTrie と辞書レイヤ、live / batch の chunker、数字・半角カタカナ・記号・絵文字のリライター、括弧ペアリング、AppProfileResolver、UTF-8 境界処理、dictbuild の辞書ビルドを変更またはレビューするときに使う。「候補が出ない」「変換がおかしい」「リライターを足す」「辞書を更新する」のような依頼でも、実体が core/ や dictbuild/ の変更なら必ずこのスキルを使う。
---

# azooKey コア変換パイプライン

`core/` は TSF にも IPC にも依存しない純粋な C++ ライブラリで、Linux でもビルドとテストができる。
TIP と Host はここを呼ぶだけで、変換の判断を自前で持たない。この境界を守ると、変換の
バグは `core_tests` だけで再現でき、Windows 実機を待たずに直せる。

## 手順

1. `references/spec-routing.md` を読み、変更対象に対応する仕様、実装、テストを特定する。
   InputState（入力中の状態遷移）に触れる変更は、対応する `docs/*-spec.md` の
   「InputState への統合」節が先に確定していることを確認する。未確定なら実装より先に spec を直す。
2. ツール選択・初期化は AGENTS.md「調査と実装」に従い、`IConverter` の呼び出し元
   （`inference-host/src/InferenceEngine.cpp`、`DictionaryCandidateProvider.cpp`）と
   TIP 側の利用箇所まで参照元を確認する。
3. 読み・表記・スコアの契約、UTF-8 の符号位置境界、空入力・不正入力の挙動を先に列挙する。
4. 最小差分で実装し、対象の test ファイルへケースを足す。core は決定的なので、
   入力と期待出力を表で書ける変更は必ずテストにする。
5. `core_tests` を実行し、辞書レイヤに触れたら `dictionary_tests`、Host の候補生成に触れたら
   `host_engine_tests` へ広げる。最終確認は `azookey_check`。
6. リライターのデータ、辞書、ユーザー可視挙動が変わる場合は対応する spec を同じ変更で更新する。

## 必須ガードレール

- `core/` に `windows.h`、TSF、IPC、ファイル監視、ネットワークを持ち込まない。
  Windows 依存が要るなら Host か TIP に置き、core には値型の入出力だけを残す。
- UTF-8 は `azookey/core/Utf8.h` の境界関数で扱う。バイト単位の切断で surrogate や
  多バイト文字を割らない。不正シーケンスは例外ではなく明示的な失敗として返す。
- リライターと絵文字・記号データの出典とライセンスは `docs/candidate-rewriter-spec.md`
  の「ライセンス境界」「ライセンスと帰属」に従う。データを再ポートするときは
  `scripts/emoji_porter.py`、`scripts/symbol_porter.py`、`scripts/rewriter_data.py` を使い、
  手で編集した生成物を commit しない。
- 既定 OFF の機能（リライター、括弧ペアリング等）を既定 ON に変えない。
  フォールバックの不変条件は各 spec が定義する。
- `legacy/` の Swift 実装は参考資料に留め、Windows 版の変換仕様の正典にしない。
- 学習・永続化は `learning/` の責務で、core から書き込みを行わない。
  学習データに触れる場合は `azookey-learning-data-safety` を併用する。

## 完了条件

- 変更した挙動に対応する `core_tests` のケースが追加または更新されている。
- 実行したテストと、Windows でしか実行できなかったテストを区別して報告する。
- リライターやテーブルの変更では、データの出典・ライセンス・再ポート手順の整合を報告する。
