---
name: azookey-quality-harness
description: azooKey Desktop の bench/（azookey_bench の latency smoke と変換品質評価、azookey_zenzai_bench、azookey_nll_bench、BenchmarkResult の baseline 比較、評価データと DATA-LICENSE）、compat-test/（M50 アプリ互換性ハーネスの runner、cases、targets、report）、Benchmark history と Compatibility tests の workflow を変更、レビュー、実行するときに使う。「ベンチを取って」「品質が落ちていないか見て」「Notepad や VS Code で壊れていないか確認して」のような依頼でも、bench/ や compat-test/ に触れるなら必ずこのスキルを使う。
---

# azooKey 品質ハーネス

bench と compat-test は機能テストの代替ではなく、機能テストが答えられない問い
（速いか、正確か、実アプリで壊れないか）に答える。条件を揃えない測定は比較にならないので、
このスキルは「何を測ったか」を結果と同じ重さで報告させる。

## 手順

1. `references/spec-routing.md` を読み、対象が latency、変換品質、NLL、実アプリ互換のどれかを決める。
2. ツール選択・初期化は AGENTS.md「調査と実装」に従い、対象 bench や case の入力データ、
   出力 schema、baseline 比較の閾値を確認する。
3. 測定条件（preset、ハードウェア、モデルとそのハッシュ、入力データ、反復回数、warm-up）を
   先に固定し、report に含める。
4. 最小差分で実装し、schema と閾値の単体テストを更新する。
5. 単体テストを実行し、Windows で bench 本体や `compat_test` を回せる場合は結果 JSON と
   report を保存する。回せない場合は未実施として報告する。
6. 指標、閾値、評価データ、対象アプリ、case が変わる場合は対応する spec を同じ変更で更新する。

## 必須ガードレール

- bench の成功を機能テストの成功に読み替えない。逆に、機能テストの成功で回帰なしと言わない。
- 評価データの追加は `bench/data/DATA-LICENSE.md` の出典・ライセンス条件を満たすものに限る。
  ライセンス不明のコーパス、ユーザーの実入力、モデルバイナリを commit しない。
- baseline 比較の閾値と絶対ノイズ床は `BenchmarkResult` が持つ。baseline 欠落や schema 非互換を
  「回帰なし」として扱う既存の判定を変えない。
- 実モデルの bench は pin したモデルとハッシュで行い、モデル不在の skip を合格として報告しない。
- compat runner は自分が起動したウィンドウだけを操作し、TIP の登録・選択は行わない。
  登録は Human Gate であり、対話セッション・対象アプリ・UI Automation が無い場合は
  silent skip ではなく `failing-skip` として報告する。
- compat の case を足すときは `compat-test/targets/*.json` の自動化契約と既知回避策を確認し、
  スクリーンショットに他アプリの内容を含めない。
- 週次の benchmark と compat の workflow を通常 PR の必須チェックに昇格しない。

## 完了条件

- 測定条件（preset、ハードウェア、モデル、入力、反復、warm-up）と結果の所在を報告する。
- baseline との比較結果と、閾値を超えた場合の原因の切り分けを報告する。
- 実機でしか回せない compat / bench の未実施分を明記する。
