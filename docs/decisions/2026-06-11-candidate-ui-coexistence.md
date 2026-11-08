# 候補 UI の実装方式（UIElement と自前 HWND の両立）

- 種別: 決定記録
- 日付: 2026-06-11（DEV-97 の完了日）
- 関連 Issue: DEV-97（roadmap のリスク D-01）
- 結論の正典: [`tsf-deep-integration-spec.md`](../tsf-deep-integration-spec.md) §2.8〜§2.11 と [`plans/windows-port-roadmap.md`](../../plans/windows-port-roadmap.md) M5
- 移設元: `plans/windows-port-roadmap.md`「リスクと不確実性」の「未決の設計判断」（commit `32cf626` 時点）。取り消し線つきの 2 項目の原文をそのまま移設した

原記録は日付を持たないため、所有 Issue DEV-97 の完了日をファイル名に使う。
2 項目目（設定アプリの UI フレームワーク）の選定は
[`2026-06-settings-app-ui-framework.md`](2026-06-settings-app-ui-framework.md) が記録する。

## 原文

- ~~候補 UI（M5）を `ITfCandidateListUIElement` で実装するか自前 HWND にするか~~
  → **確定（DEV-97 / D-01）**: 二者択一ではなく「両立 (coexistence)」。
  `ITfCandidateListUIElement`（3-interface 契約）と自前 `WS_POPUP` HWND を両方
  実装し、`ITfUIElementMgr::BeginUIElement` の `pbShow` で per-call に切り替える。
  v1.0（M5）で最小契約（UI-less フラグ検出（`GetActiveFlags`）・最小 UIElement 候補公開と
  それに伴う `GUID_TFCAT_TIPCAP_UIELEMENTENABLED` カテゴリ登録・`CandidateUiCoordinator`）
  まで実装し、M21 で full 実装。カテゴリ登録は候補公開実装と必ず一体で行う（公開が
  無いまま登録すると UI-less-only ホストで候補が消える）。この制約は
  `GUID_TFCAT_TIPCAP_UIELEMENTENABLED` のみに適用し、Windows アプリ互換宣言の
  `GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT` には適用しない。詳細は
  `docs/tsf-deep-integration-spec.md` §2.8〜§2.11。
- ~~設定アプリ（M11）の UI フレームワーク（WinUI 3 / WPF / Tauri）は別途検討。~~
  → **確定（DEV-99 / D-03）**: WinUI 3（C++/WinRT）。根拠・代替案・残る
  `gate:human-required` スパイクは M11「UI フレームワーク決定」を正典とする。
