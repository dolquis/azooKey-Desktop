# CUDA backend 実リンクの中止と M8 の GPU/CPU 切替条件

- 種別: 決定記録
- 日付: 2026-08-04（DEV-223 の中止日）。v1.0 設定 UI のデバイス選択の再確定は DEV-854（2026-09-02 完了）
- 関連 Issue: DEV-223、DEV-854
- 結論の正典: [`zenzai-inference-spec.md`](../zenzai-inference-spec.md) §7.4（CUDA 要求の降格）、[`copilot-pc-backend-spec.md`](../copilot-pc-backend-spec.md) §4.3（CUDA の採否）、[`sideload-packaging-spec.md`](../sideload-packaging-spec.md) §3.7（v1.0 設定 UI）、[`plans/windows-port-roadmap.md`](../../plans/windows-port-roadmap.md) M8 の受け入れ条件
- 移設元: `plans/windows-port-roadmap.md` M8「受け入れ条件の前提に関する注記」（commit `32cf626` 時点）。原文をそのまま移設した

原記録は日付を持たないため、注記が語る決定のうち最も早い DEV-223 の中止日をファイル名に使う。

## 移設時点の M8 受け入れ条件

- Zenzai GGUF（上流 `Miwa-Keita/zenz-v3.2-small-gguf`）配置時に `LoadModel` 成功
- 未配置時も Host が落ちず、固定テーブル候補が動く
- GPU/CPU 切替が設定で効く

## 原文

**受け入れ条件の前提に関する注記**: 「GPU/CPU 切替が設定で効く」は CUDA backend の実リンクを
前提としていたが、その実リンク（DEV-223）は中止され、`cuda` 要求は
`docs/copilot-pc-backend-spec.md` §4.4 のとおり成功 LoadModel のまま CPU へ降格する。
実リンク済みビルドを配布するまで、この条件を満たす GPU 経路は存在しない。v1.0 設定 UI は
この状況に合わせてデバイス選択を `auto` / `cpu` の 2 値へ再確定した
（`docs/sideload-packaging-spec.md` §3.7、DEV-854）。
