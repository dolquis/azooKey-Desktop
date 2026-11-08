# zenz-v3 を ONNX Runtime GenAI 形式へ変換できるかの判定

- 種別: 決定記録
- 日付: 2026-08-23
- 関連 Issue: DEV-194
- 結論の正典: [`copilot-pc-backend-spec.md`](../copilot-pc-backend-spec.md) §4.2.2・§4.3 項 2
- 移設元: `docs/copilot-pc-backend-spec.md` §4.2.2（commit `c8d9a5b` 時点）。原文をそのまま移設した

本文中の `§` は移設元の `docs/copilot-pc-backend-spec.md` の節を指す。

#### 4.2.2 zenz-v3 の R2 変換可否

2026-08-23 時点の ONNX Runtime GenAI model builder を、配布対象の
`Miwa-Keita/zenz-v3.2-small-gguf` に適用する経路はない。

1. 配布モデルは GPT-2 architecture の Q5_K_M GGUF であり、公開リポジトリにあるモデル
   本体は 73.9MB の当該ファイルだけである。
2. model builder の対応 architecture 一覧に GPT-2 はなく、`builder.py` に
   `GPT2LMHeadModel` の分岐もない。
   通常の変換要求は未対応モデルとして `NotImplementedError` になる。
3. model builder が入力として受け付ける GGUF は float16 / float32 に限られるため、
   Q5_K_M を変換元にはできない。
4. ONNX Runtime GenAI runtime 自体は `gpt2` model type を認識する。
   これは適合する ONNX graph と `genai_config.json` を実行できるという意味であり、
   model builder が zenz-v3 を出力できることは意味しない。

したがって、現行 model builder をそのまま使う手動変換は不可と判定する。
GPT-2 builder の新規実装、または別 exporter で作った ONNX graph と手書き
`genai_config.json` を検証する作業は、新しい推論 backend の実装に当たるため本スパイクの
範囲外とする。
zenz-v3 の変換済み artifact がないため、zenz-v3 での R2 の精度劣化、RSS、実効配布サイズ、
QNN / OpenVINO NPU レイテンシは計測できない。zenz-v3.1-small / xsmall には safetensors
（fp32）があるが、GPT-2 構造なので同じ制約を受ける。v3.2 は GGUF だけが公開されている。

根拠:

- [ONNX Runtime GenAI Model Builder: Current Support / GGUF Model](https://github.com/microsoft/onnxruntime-genai/blob/a0c04eb1313b886c963e415bbd974748cb89a4eb/src/python/py/models/README.md)
- [model builder の architecture 分岐](https://github.com/microsoft/onnxruntime-genai/blob/a0c04eb1313b886c963e415bbd974748cb89a4eb/src/python/py/models/builder.py)
- [runtime の model type 判定](https://github.com/microsoft/onnxruntime-genai/blob/a0c04eb1313b886c963e415bbd974748cb89a4eb/src/models/model_type.h)
- [Miwa-Keita/zenz-v3.2-small-gguf](https://huggingface.co/Miwa-Keita/zenz-v3.2-small-gguf/tree/c67e03e07d215c869f591b274c1631170d3e11fe)
