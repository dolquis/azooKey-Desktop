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
読みと期待表層・許容表層、または左文脈と読みが重なれば、データセットの作成を止める。
混在入力の評価では明示された読みも除外する。出力には評価セットの指紋と照合キーだけを保存する。
ラベルには表示順の先頭 9 件から正例 1 件、負例を最大 8 件使う。§6.2 のキーで多数決し、同数は除外する。
holdout は作品単位で固定し、同じ変換機会が分割をまたぐと失敗する。

`train` は LightGBM と GELU 付き 2 層 MLP を学習する。
標準化は学習分割の先頭 6 特徴から推定し、ONNX 内で実行する。
両モデルの入出力は fp32 の `features_v1` [batch, 12] と `rerank_score` [batch, 1]。
メタ情報には `model_version`、`feature_schema`、学習入力の `dataset_sha256` を持たせる。
モデルファイル自身の SHA256 は `receipt.json` に記録する。
v0 の `tiny_reranker_v0.onnx` は比較用。配布候補は v1 の `tiny_reranker.onnx` であり、
実コーパスで学習したモデルと receipt の採用には DEV-2228 の帰属確定を要する。

## 公開コーパスの入力

`sources.json` の `corpora` は作品別の許可リストである。1 作品ごとに
`id`、`url`、`revision`、`sha256`、`license`、`notice`、ZIP 内のテキストの `member` を固定する。
青空文庫は著作権切れが確認された作品を `PD` として記録し、`notice` に底本と帰属を記す。
`fetch` の既定動作はキャッシュ照合で、承認後の明示的な `fetch --download` が作品 ZIP を取得する。
SudachiDict core の ZIP は `reading_dictionary` に記録した SHA256 を確定し、キャッシュの
`asset-sudachidict-core-20260723.zip` に配置する。`fetch` の取得対象は作品 ZIP である。

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
