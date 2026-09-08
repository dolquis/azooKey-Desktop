# NLL リランクの Release 測定記録

2026-09-08 に [DEV-1012](https://linear.app/dolquis/issue/DEV-1012) で行った固定入力の測定記録である。
更新しない参考資料であり、仕様の正典は [neural-reranker-spec.md](neural-reranker-spec.md) Track B とする。

## 判断

CPU では NLL の追加評価が既定 20 ms に収まらなかった。
文脈ありとなしの両方で 30 標本すべてが超過し、NLL 後の次回変換には prefix 再計算の負担も残った。
既定 OFF、`nllWeight=0.15`、`nllTopK=8`、`nllBudgetMs=20`、all-or-nothing を維持する。
§B12 に従い、生成との prefix KV/logits 共有とコピー削減を [DEV-1055](https://linear.app/dolquis/issue/DEV-1055) に切り出す。

Vulkan でも機能検査は通ったが、専有環境の性能評価には使えない。
最終測定終了直後、ベンチ終了済みの状態で GPU 使用率 98%、P0、181.92 W、64 ℃を観測した。
測定中の利用率を連続記録しておらず、各標本の外部負荷量と遅延原因は特定していない。
GPU の予算適合と CPU/GPU の速度比較は保留し、DEV-1055 で負荷条件を管理して再測定する。

## 条件と集計方法

| 項目 | 値 |
|---|---|
| ソース基点 | `e37f8f2a3c736e2150c8b0516f908a7b65800607` と本変更の測定器 |
| CPU | Intel Core i7-12700KF、hardware concurrency 20、使用 threads 8 |
| GPU | NVIDIA GeForce RTX 4070、driver 616.64、Vulkan SDK 1.4.357.0 |
| ビルド | MSVC 14.51.36231、Release、`AZOOKEY_WITH_LLAMA_CPP=1` |
| presets | CPU: `windows-release`、GPU: `windows-vulkan-release`（出力 `build/vk`） |
| llama.cpp | `427291b5b34cd914a31b3fd3b61a68f6184f4b9f` の既存ローカルソース |
| モデル | 既存の `ggml-model-Q5_K_M.gguf`。ダウンロードと置換なし |
| GGUF SHA256 | `29c223d4c23327b80fd13ebb5ab2555057a46317997d5da391584ffbef0db673` |
| GPU 配置 | ロードログで RTX 4070 と 13/13 layers offload を確認 |
| 入力 | `こうせい`、固定候補8件。文脈ありは `原稿の誤字を` |
| 反復 | 各プロセス warm-up 2 回、測定10回。各条件を3セット |
| 実行順 | 各セットで CPU 文脈あり、CPU なし、Vulkan あり、Vulkan なし。直列実行 |
| 時間予算 | 診断評価は10秒、bounded 評価と QueryCandidates の NLL は既定20 ms |

各セットの p95 は nearest-rank（`ceil(0.95*N)-1`）で、N=10 では最大値に等しい。
合算 p95 は3セットの生標本30件から計算し、セット p95 の平均ではない。
超過率は全評価時間が20 msを超えた標本の割合である。
専用 CPU/GPU ホスト、固定クロック、負荷隔離を用いた測定ではなく、小標本の観測値として読む。

## NLL 追加評価

時間の単位は ms。
「prefix 平均 / p95」は NLL 内部の prefix 評価、「全評価」は呼び出し全体を含む。

| 経路と文脈 | 各セットの全評価 p95 | 合算 p95 | prefix 平均 / p95 | 20 ms 超過 |
|---|---|---:|---:|---:|
| CPU あり | 33.3192 / 32.9109 / 30.5965 | 32.9109 | 13.5035 / 14.5032 | 30/30（100%） |
| CPU なし | 25.3111 / 25.4581 / 24.3979 | 25.4249 | 7.5655 / 8.0883 | 30/30（100%） |
| Vulkan あり（負荷未隔離） | 415.151 / 303.911 / 297.262 | 303.911 | 34.4645 / 65.2054 | 30/30（100%） |
| Vulkan なし（負荷未隔離） | 396.804 / 305.867 / 231.577 | 305.867 | 28.7943 / 50.1267 | 30/30（100%） |

## 次回変換と連続要求

`Convert` は、同一入力をキャッシュ付きで反復した場合と、NLL 評価直後の場合を対にして測る。
`QueryCandidates` は別のエンジンで OFF と ON を測り、それぞれ2回の OFF warm-up 後に10要求を送る。
ON の circuit を途中でリセットしない。
合成 TechnicalTerms 辞書を使い、辞書候補を保持するため返却上限を32に設定した。
製品既定の返却上限9を用いた入力品質ベンチではない。

| 経路と文脈 | Convert キャッシュ付き p95 | Convert NLL直後 p95 | Query OFF p95 | Query ON p95 |
|---|---:|---:|---:|---:|
| CPU あり | 16.3984 | 29.0098 | 16.2611 | 46.8003 |
| CPU なし | 20.2079 | 25.0300 | 19.2852 | 44.9525 |
| Vulkan あり（負荷未隔離） | 174.606 | 194.140 | 121.744 | 247.328 |
| Vulkan なし（負荷未隔離） | 234.126 | 200.149 | 149.581 | 182.953 |

CPU の ON 要求では、最初の3回が予算超過で全件破棄された。
次の要求も再利用トークン数0で prefix を作り直し、その次から再利用が戻った。
ON 集計には circuit が開いた後の評価省略も含まれるため、NLL 単体時間との加算や差引きを性能保証に使わない。
Vulkan のキャッシュ有無の大小関係は外部負荷を統制しておらず、最適化効果を表すものではない。

## 機能検証と限界

- CPU/Vulkan、文脈あり/なし、各3セットの全プロセスが終了コード0で完走した。
- 最上位は文脈ありで `校正`、なしで `構成` に完全一致し、相対ペナルティ合成後も一致した。
- 逆順評価の NLL は有限値で、対応候補との差が `1e-5` 以下だった。
- NLL 前後とキャッシュ付き生成の候補文字列、件数、順序、source が一致した。
  生成スコア最大差は CPU 0、Vulkan は文脈あり0.00407814、なし0.000647987だった。
  GPU の生成スコア完全一致は主張しない。
- 各条件3セット×3回の bounded 評価はすべて `budget_exceeded`、適用件数0だった。
  `RerankNll` の同一入力に対する前後で、全候補の score と debug_info の完全不変を検証した。
  Query の比較には候補統合による生成スコアや debug_info の差が入るため、全件破棄の根拠を混同しない。
- Release の `Nll*` 制御テストで prefix snapshot、off-by-one、cancel、全件破棄、circuit を確認した。
  実機 TIP 操作、広い入力集合の品質校正、GPU 専有条件の性能保証は対象外である。

## 再現手順

README の標準ビルド環境を使い、`$llamaSource` は上記 SHA の既存ソース、`$gguf` は上記 SHA256 の既存モデルへ設定する。
CPU は `AZOOKEY_LLAMA_CPP_SOURCE_DIR` を指定した `windows-release`、GPU は既存 Vulkan SDK を認識する `windows-vulkan-release` を構成し、`azookey_nll_bench` をビルドする。
CPU/GPU の両ビルドを終えてから測定する。

```powershell
cmake --preset windows-release "-DAZOOKEY_LLAMA_CPP_SOURCE_DIR=$llamaSource"
cmake --build --preset windows-release --target azookey_nll_bench
cmake --preset windows-vulkan-release "-DAZOOKEY_LLAMA_CPP_SOURCE_DIR=$llamaSource"
cmake --build --preset windows-vulkan-release --target azookey_nll_bench

New-Item -ItemType Directory -Force build/nll-fixture
Copy-Item LICENSE build/nll-fixture/MIT.txt
python dictbuild/dictbuild.py bench/data/nll_fixture.lex.tsv `
  --metadata bench/data/nll_fixture.metadata.json --layer technical_terms_lexicon `
  --catalog build/nll-fixture --output build/nll-fixture/fixture.azdic `
  --notices build/nll-fixture/notices.txt

$exe = './build/windows-release/bench/azookey_nll_bench.exe'
$backend = 'cpu'
& $exe --model $gguf --backend $backend --threads 8 --iterations 10 `
  --context '原稿の誤字を' --expect-top '校正' `
  --query-dictionary build/nll-fixture/fixture.azdic
& $exe --model $gguf --backend $backend --threads 8 --iterations 10 `
  --expect-top '構成' --query-dictionary build/nll-fixture/fixture.azdic
```

GPU は `$exe='./build/vk/bench/azookey_nll_bench.exe'`、`$backend='vulkan'` に替える。
上記の順序を3セット繰り返し、stdout/stderr と終了コードを各回保存する。
`nll_sample` の各行から全評価と prefix を集計する。
`status=ok` の行だけで成功判定せず、後続の bounded 検査と Query 検査を含む終了コードを確認する。
計測ログ、生成辞書、モデルはコミット対象に含めない。
