# R1（llama.cpp / CPU）実モデル baseline と prompt KV 再利用後の再計測

- 種別: 測定記録
- 日付: 2026-08-23（DEV-194 と DEV-853 の完了日）
- 関連 Issue: DEV-194、DEV-853（PR #295）、DEV-1720
- 結論の正典: [`copilot-pc-backend-spec.md`](../copilot-pc-backend-spec.md) §4.2・§4.2.1・§4.2.1.1
- 移設元: `docs/copilot-pc-backend-spec.md` §4.2 末尾の箇条、§4.2.1、§4.2.1.1（commit `c8d9a5b` 時点）。原文をそのまま移設した

本文中の `§` は移設元の `docs/copilot-pc-backend-spec.md` の節を指す。

## §4.2 末尾の検証範囲の記録

- `gate:human-required` の DEV-194 の検証範囲のうち、R1 CPU の実機計測は完了した。
  Snapdragon X Elite / Intel Core Ultra の NPU / GPU EP の計測は、§4.3 項 2 の再開条件を
  満たした R2 artifact で、同じ入力セットを使って Human Gate（DEV-1720）が行う。

#### 4.2.1 R1 CPU 実モデル baseline

DEV-194 では、次の固定条件で R1 CPU を計測した。

- リポジトリ: commit `6abce8d2595a05c4358dacf459b691b3cc946d04`
- OS / CPU: Windows build 26200.9168 / Intel Core i7-12700KF / x64 / NPU なし
- build: `windows-release`、`llama_cpp=1`、`requested_backend=cpu`、
  `effective_backend=cpu`
- model: `Miwa-Keita/zenz-v3.2-small-gguf` の `ggml-model-Q5_K_M.gguf`、
  73,871,936 bytes（70.45MiB）、SHA-256
  `29c223d4c23327b80fd13ebb5ab2555057a46317997d5da391584ffbef0db673`
- 各入力を別プロセスで実行し、warm-up 3 回、計測 20 回とした。

入力セットは次の 20 件である。

```text
こんにちは / にほんご / きょうはいいてんきです / わたしはがくせいです
よろしくおねがいします / ありがとうございます / きょうのかいぎ / めーるをかく
あしたのよてい / とうきょうえき / おーぷんえーあい / ぷろぐらみんぐ
へんかんこうほ / にほんごにゅうりょく / このぶんしょうをへんかん
しゅうまつのてんき / でんしゃにのる / しごとをはじめる
せっていをひらく / ぱそこんをさいきどうする
```

| 指標 | 結果 |
|---|---|
| 初回 `LoadModel` | 中央値 22.80ms |
| 入力別 p50 | 中央値 501.38ms、範囲 196.02–810.19ms |
| p50 < 30ms | 0 / 20 入力 |
| `にほんご` 50 回 | p50 197.03ms / p95 210.73ms / p99 215.37ms |
| peak RSS | 122.52MiB（`にほんご`、warm-up 3 回 + 計測 50 回の代表値） |
| 実推論 | 20 / 20 入力で exit 0、`zenzai_candidates=4` |

この計測時点では、ロード時間は 3 秒ゲートを満たし、推論 p50 は 20 入力すべてで
30ms を上回った。また、モデル単体が 50MB を超えるため、R1 の配布サイズは 50MB
ゲートを満たさない。いずれも §4.2 の通り相対ゲートに対する値であり、R1 CPU の
出荷可否を示すものではない。この結果は R1 CPU baseline であり、R2 の性能を
推定する根拠にはしない。R1 CPU の絶対予算に対する評価は
`zenzai-inference-spec` §8 による。

なお本節の値は計測時点（commit `6abce8d2`）の記録であり、その後の最適化を
反映しない。以降の実測は §4.2.1.1 に追記する。

##### 4.2.1.1 prompt KV 再利用後の再計測

DEV-853 の prompt KV cache 再利用（PR #295）後に、§4.2.1 と同じ入力セット・
同じ計測プロトコル（別プロセス実行、warm-up 3 回）で再計測した。build 構成と
model は §4.2.1 と同一である。計測環境（OS build / CPU）は DEV-853 の報告に
従い、本節では §4.2.1 との同一性を主張しない。

| 計測 | commit | 条件 | 結果 |
|---|---|---|---|
| `にほんご` 単体 | `36740c54` | 計測 50 回 | p50 79.11ms / p95 92.68ms / p99 106.87ms |
| decode phase 内訳 | `36740c54` | 同上 | prompt p50 10.22ms（samples 50）/ beam p50 66.84ms（samples 50、evaluations 850） |
| 20 入力 | `e594a420` | 各 計測 20 回 | 入力別 p50 中央値 209.24ms、範囲 97.92–436.01ms |

- `36740c54` は PR #295 のマージ対象 head（merge commit `9198f427`）である。
- 20 入力の系列は `e594a420`（PR head 初版）でのみ取得しており、レビュー反映後の
  `36740c54` では再取得していない。両者の差分はレビュー指摘対応であり、
  同一 commit 同士の比較ではない点に注意する。
- 20 入力セットは読み ≤ 8 文字と 8 文字超が混在する。混在集合の中央値は
  `zenzai-inference-spec` §8 の読み長別予算に直接対応しない（§8 参照）。
