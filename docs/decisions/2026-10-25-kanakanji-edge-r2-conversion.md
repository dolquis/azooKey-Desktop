# kanakanji-edge-33m の ONNX Runtime GenAI 変換と精度

- 種別: 測定記録
- 日付: 2026-10-25
- 関連 Issue: DEV-1718、DEV-1720
- 結論の正典: [`copilot-pc-backend-spec.md`](../copilot-pc-backend-spec.md) §4.2.3
- 移設元: `docs/copilot-pc-backend-spec.md` §4.2.3（commit `c8d9a5b` 時点）。原文をそのまま移設した（形式の決定と再現手順は spec にも残した）

本文中の `§` は移設元の `docs/copilot-pc-backend-spec.md` の節を指す。

#### 4.2.3 Llama 系互換モデルの R2 変換（DEV-1718）

model builder の対応 architecture には Llama と LFM2 MoE が含まれ、GPT-2 と Qwen3-Next は
含まれない（onnxruntime-genai `6b7ff3ca` の builder）。zenz-v3 のプロンプト形式で学習された
Llama 構造のかな漢字変換モデル `yuuki14202028/kanakanji-edge-33m`（33.5M パラメータ、
safetensors fp32）は、この builder で ONNX Runtime GenAI 形式へ変換できる。モデルそのものの
評価と、zenz-v3 とのプロンプト差の扱いは `docs/zenzai-inference-spec.md` §2.4 が正典である。

2026-10-25 の sandbox 調査（Linux、CPU 4 コア）で、`onnxruntime-genai` 0.18.0 の builder を
使い、CPU EP で AJIMEE-Bench 200 件を貪欲デコードした結果は次のとおりである。製品の
beam / n-best と読み制約は使っていない。

| 形式 | サイズ | Acc@1 | 備考 |
|---|---|---|---|
| fp32 | 129MB | 0.805 | llama.cpp f16 と 200 / 200 件で同じ出力 |
| int8 | 49MB | 0.805 | fp32 から劣化なし |
| int4（builder 既定） | — | 0.235 | 崩れる |
| int4（LM head 除外、shared embeddings 無効、block 32） | — | 0.755 | 原因は切り分けていない |
| fp16 | — | — | CPU EP では実行時エラー（GPU EP 向けの形式） |

この結果から、R2 で使う形式は次のとおりとする。

- CPU EP と GPU EP の第一候補は int8 とする。int4 は崩れの原因を切り分けるまで使わない。
  fp16 は GPU EP の候補として Human Gate の計測項目に残す。
- NPU EP（QNN / OpenVINO / VitisAI）は、静的 shape や QDQ 量子化など EP ごとの builder 設定を
  要する可能性がある。NPU で動く形式は Human Gate（DEV-1720）の計測で決める。
- `genai_config.json` と `tokenizer.json` は builder の出力を使う。ORT GenAI のトークナイザが
  プロンプトの制御文字（U+EE00〜U+EE02）を 1 トークンずつに解決すること（llama.cpp の
  `parse_special=true` と同じ結果になること）を、R2 の実装で確かめる。

変換の再現手順は次の形を取る。revision、builder のバージョン、評価スクリプトの固定は
R2 spike の Issue（follow-up）で repo に置く。

```text
pip install onnxruntime-genai==0.18.0
python -m onnxruntime_genai.models.builder \
  -m yuuki14202028/kanakanji-edge-33m -o <出力ディレクトリ> -p int8 -e cpu
```
