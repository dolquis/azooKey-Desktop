# M27 の着手条件に ARM64 実機の入手を加える

- 種別: 決定記録
- 日付: 2026-09
- 関連 Issue: DEV-908、DEV-909
- 結論の正典: [`plans/windows-port-roadmap.md`](../../plans/windows-port-roadmap.md) M27「前提」、[`copilot-pc-backend-spec.md`](../copilot-pc-backend-spec.md) §8
- 移設元: `plans/windows-port-roadmap.md` M27 冒頭の「着手条件（2026-09 確定）」（commit `32cf626` 時点）。原文をそのまま移設した

## 原文

> **着手条件（2026-09 確定）**: ARM64 Windows 実機の入手を本マイルストーンの着手条件に加える。
> クロスビルドジョブ（DEV-908）だけを先行させても、生成した ARM64 バイナリを検証する実機ゲート
> （DEV-909）が着手できず、検証されない成果物が増えるだけになる。再開は DEV-908 → DEV-909 の順
> とする。`windows-11-arm` ランナーでの ctest 実行（下記受け入れ条件）は従量課金を伴うため、
> 再開時に別途判断する。
