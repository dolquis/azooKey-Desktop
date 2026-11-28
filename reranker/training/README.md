# M56 オフライン学習

[ニューラルリランカー仕様](../../docs/neural-reranker-spec.md) §4〜§6 の学習ツール。
作業はリポジトリのルートで実行し、出力には `build/tiny-reranker/` 以下の新しいディレクトリを指定する。

## 人工データで確かめる

```powershell
$trainer = 'reranker/training/tiny_reranker.py'
$trainingOutput = 'build/tiny-reranker'
uv run --no-config --locked --script $trainer fixture --output-dir "$trainingOutput/fixture"
uv run --no-config --locked --script $trainer synthesize --queries "$trainingOutput/fixture/queries.jsonl" --candidates "$trainingOutput/fixture/candidates.jsonl" --output-dir "$trainingOutput/dataset"
uv run --no-config --locked --script $trainer train --dataset-dir "$trainingOutput/dataset" --output-dir "$trainingOutput/models"
uv run --no-config --locked --script $trainer verify --model-dir "$trainingOutput/models"
uv run --no-config --locked --script $trainer check-reproducibility --dataset-dir "$trainingOutput/dataset" --output-dir "$trainingOutput/repeat"
```

`synthesize` は `bench/data/` の評価 JSONL を照合用の SHA256 に変換する。
読みと期待表層・許容表層、左文脈と読み、または混在入力の評価で明示された読みが重なる変換機会を除外する。
除外件数は `synthesis.json` の `counts.m52_overlaps` に記録し、最終データに重複が残れば作成を止める。
評価セットの内容は指紋と照合キーだけを保存する。
ラベルには表示順の先頭 9 件から正例 1 件、負例を最大 8 件使う。§6.2 のキーで多数決し、同数は除外する。
holdout は作品単位で固定する。両分割に現れる変換機会は両側から除外し、`counts.cross_split_opportunities` に件数を記録する。

`train` は LightGBM と GELU 付き 2 層 MLP を学習する。
標準化は学習分割の先頭 6 特徴から推定し、ONNX 内で実行する。
両モデルの入出力は fp32 の `features_v1` [batch, 12] と `rerank_score` [batch, 1]。
メタ情報には `model_version`、`feature_schema`、学習入力の `dataset_sha256` を持たせる。
比較用の v0 `tiny_reranker_v0.onnx` には、onnxmltools が実際に出力する標準 opset 9 と
`ai.onnx.ml` opset 1 を引き継ぐ。変換時に指定する標準 opset 15 は上限である。
配布候補の v1 `tiny_reranker.onnx` は標準 opset 20 を使う。
各モデルの SHA256 と domain ごとの opset は `receipt.json` に記録し、v0 のメタ情報にも
`opsets` を持たせる。`verify` は両モデルを ORT 1.32.0 の CPU EP で検査する。
`check-reproducibility` は異なる `PYTHONHASHSEED` を指定した二つのプロセスで学習し、モデルと receipt の SHA256 を照合する。
実コーパスで学習したモデルと receipt の採用には DEV-2228 の帰属確定を要する。

## 公開コーパスの入力

`sources.json` の `corpora` は作品別の許可リストである。1 作品ごとに
`id`、`url`、`revision`、`sha256`、`license`、`notice`、ZIP 内のテキストの `member` を固定する。
青空文庫は著作権切れが確認された作品を `PD` として記録し、`notice` に底本と帰属を記す。
`fetch` の既定動作はキャッシュ照合で、承認後の明示的な `fetch --download` が作品 ZIP を取得する。
SudachiDict core の ZIP は `reading_dictionary` に記録した SHA256 を確定し、キャッシュの
`asset-sudachidict-core-20260723.zip` に配置する。`fetch` の取得対象は作品 ZIP である。
`prepare` は検証済み ZIP の辞書を SHA256 付きの名前でキャッシュに展開し、再利用時も内容を照合する。

`prepare --output-dir <dir>` は承認済みのキャッシュから、青空文庫のルビを優先し、残りを Sudachi で
読んだ `queries.jsonl` を作る。各行は `id`、`work`、`left_context`、`input`、`chosen` を持つ。
`synthesize` の候補ファイルは、同じ `id` と表示前の候補順の `candidates` を持つ JSONL で、
各候補に `surface` と 12 要素の `features` が要る。配列順は [仕様](../../docs/neural-reranker-spec.md) §4 に従う。
本番特徴の採取には DEV-2154 の FeatureExtractor が必要で、人工データの入口は `fixture` である。

## 試験

```powershell
uv run --no-config --locked --script reranker/training/tiny_reranker.py test
uv run --no-config --locked --script reranker/training/tiny_reranker.py typecheck
python -B scripts/tests/test_tiny_reranker.py
```

標準ライブラリで実行した場合は、学習依存が欠けていればモデル検証を理由付きで skip する。
Sudachi のテストには人工の辞書と tokenizer の mock を使う。
