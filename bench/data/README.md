# 変換品質評価データ

`kana_kanji_eval.jsonl` は通常変換と文脈付き同音異義語、`typo_eval.jsonl` は
打ち間違えと、それに対応する正常入力の評価に使います。
各ケースの `provenance` は `authored` です。コーパスやユーザー入力ログからの
転用はなく、データのライセンスは [DATA-LICENSE.md](DATA-LICENSE.md) の CC0 です。
`PROVENANCE.json` が必要になる条件は
[評価仕様 §13.3](../../docs/conversion-quality-benchmark-spec.md#133-出典-manifestbenchdataprovenancejson)
を参照してください。

ケースの形式、件数、代表性、重複判定は
[評価仕様 §4・§11](../../docs/conversion-quality-benchmark-spec.md) に従います。
`conversion_quality_smoke.jsonl` は CTest 用の小さな入力で、フル評価の入力とは別です。

## 評価の実行

リポジトリのルートから、ビルド済みの `azookey_bench` を使います。
以下は PowerShell で CPU fallback の再現性を測る例です。
モデルを指定しないため、実モデルの品質評価には使いません。
出力先の `build/quality-eval/` は先に作成してください。

```powershell
$bench = './build/windows-release/bench/azookey_bench.exe'
New-Item -ItemType Directory -Force build/quality-eval | Out-Null
foreach ($dataset in 'kana_kanji_eval', 'typo_eval') {
    & $bench --eval "bench/data/$dataset.jsonl" --backend cpu --typo-mode off --iterations 30 --output "build/quality-eval/$dataset.first.json" --per-case "build/quality-eval/$dataset.first.jsonl"
    if ($LASTEXITCODE -ne 0) { throw "First evaluation failed: $dataset" }
    & $bench --eval "bench/data/$dataset.jsonl" --backend cpu --typo-mode off --iterations 30 --output "build/quality-eval/$dataset.second.json" --per-case "build/quality-eval/$dataset.second.jsonl" --baseline "build/quality-eval/$dataset.first.json"
    if ($LASTEXITCODE -ne 0) { throw "Second evaluation failed: $dataset" }
}
```

再現性の判定では、集計 JSON の互換キー、精度系指標の差と、per-case JSONL の
`candidates` の順序を比較します。latency とメモリ使用量は一致判定から除外します。
判定の正典は [評価仕様 §14](../../docs/conversion-quality-benchmark-spec.md#14-baseline-安定性基準再現性) です。
データを変更したら baseline を採取し直してください。

個別のカテゴリには `--category general` / `homophone` / `typo` / `typo_clean` を指定します。
通常変換の2カテゴリには `kana_kanji_eval.jsonl`、打ち間違えの2カテゴリには
`typo_eval.jsonl` を使います。フィルタごとに baseline を分けてください。
`typo_clean` は正常入力を補正してしまう割合の分母にのみ使うため、
集計 JSON の `by_category` には出ません。対象ケースは per-case JSONL で確認します。
M52 の `off` で得た typo 指標は、補正有効モードの精度を表しません（評価仕様 §7）。
