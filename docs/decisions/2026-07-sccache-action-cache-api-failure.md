# sccache-action の旧 Cache API による CI 全滅と pin の決定

- 種別: 決定記録
- 日付: 2026-07
- 関連 Issue: なし
- 結論の正典: [`dev-infrastructure-spec.md`](../dev-infrastructure-spec.md) §4.7
- 移設元: `docs/dev-infrastructure-spec.md` §4.7 末尾の段落（commit `c8d9a5b` 時点）。原文をそのまま移設した

**action のバージョンに注意（過去の失敗の教訓）。** 初回導入時に
`sccache-action@v0.0.6` を pin したところ、全ビルドジョブが失敗した（2026-07）。
このタグは旧 (v1) Actions Cache API 用の env（`ACTIONS_CACHE_URL`）しか渡さないが、
GitHub は v1 を **2025-04-15 に完全停止**しており、廃止済みエンドポイントが HTTP 400
（`Our services aren't available right now`）を返して sccache サーバ起動が失敗、
compiler launcher に設定された sccache が全コンパイルを道連れにした。現行 sccache は
v2 サービスの env（`ACTIONS_RESULTS_URL`）を要求するため、**action は v2 を繋ぐ
`v0.0.9` 以降を pin する**こと。あわせて
`SCCACHE_IGNORE_SERVER_IO_ERROR=1` を設定し、実行時にキャッシュ backend が失敗しても
ローカルコンパイルへ fallback してビルドを緑に保つ（キャッシュはビルドを壊しては
ならない）。
