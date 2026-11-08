# モデルロード境界とユーザー辞書 CLI の先行

- 種別: 決定記録
- 日付: 2026-05-20
- 関連 Issue: なし
- 結論の正典: [`plans/windows-port-roadmap.md`](../../plans/windows-port-roadmap.md) M8「`LoadModel` 境界」と M9「実装範囲」。llama.cpp の採否は [`copilot-pc-backend-spec.md`](../copilot-pc-backend-spec.md) §4.3
- 移設元: `plans/windows-port-roadmap.md` M8「決定記録（2026-05-20）」と M9「決定記録（2026-05-20）」（commit `32cf626` 時点）。原文をそのまま移設した

llama.cpp バインディング選定の判断理由は [`2026-05-20-zenzai-engine-route.md`](2026-05-20-zenzai-engine-route.md) にある。
本ファイルは roadmap 側に書かれていた決定の原文を残す。

## M8: 決定記録（2026-05-20）

- llama.cpp バインディング選定: 初期実装は llama.cpp C API + CPU backend から
  開始し、CUDA は optional backend として追加する。DirectML / NPU は M24 まで
  予約値扱い。判断理由は `docs/decisions/2026-05-20-zenzai-engine-route.md`、計測ゲートは `docs/zenzai-gpu-route.md`。
- `LoadModel` 境界: `LoadModelRequest(path, backend, n_gpu_layers)` を
  `InferenceEngine::LoadModel` に渡し、`model_loaded` / `last_error` を
  `Handshake` / `Health` で観測できる状態とする。

## M9: 決定記録（2026-05-20）

本格設定 UI を待たず、`inference-host` の IPC 経由で
`AddUserWord` / `RemoveUserWord` を呼ぶ小 CLI または debug probe を先に用意する。
設定アプリ統合は M11 へ送る。

移設時点の M9「実装範囲」には、この決定に対応する次の 1 行があった。

- 設定 UI（M11）または暫定 CLI/デバッグ UI から呼べる経路
