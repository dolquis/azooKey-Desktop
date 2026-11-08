# 設定アプリの UI フレームワーク選定

- 種別: 決定記録
- 日付: 2026-06（Microsoft Learn の事実確認の時点）
- 関連 Issue: DEV-99（roadmap のリスク D-03）
- 結論の正典: [`sideload-packaging-spec.md`](../sideload-packaging-spec.md) §3.0
- 移設元: `docs/sideload-packaging-spec.md` §3.0（commit `c8d9a5b` 時点）。原文をそのまま移設した

本文中の `§` は移設元の `docs/sideload-packaging-spec.md` の節を指す。

### 3.0 UI フレームワーク選定（採用: WinUI 3 / C++/WinRT）

設定アプリの UI フレームワークは、**WinUI 3（Windows App SDK, C++/WinRT）を第一候補と
して確定する**。対象は **v1.0 スコープの M11（最小設定アプリ。Phase 4 リリースゲート）** と、
それを **post-v1.0 で本格化する M30（フル設定 UI）**、および同一 UI スタックに乗る後続機能
（モデル管理 UI = M45、学習データ可視化 UI = M49）。同一フレームワークに揃えて後続の
作り直しを避ける（v1.0 では M11 の最小機能セットのみを出荷し、M30 のフル UI / 横断機能は
v1.0 に引き込まない）。根拠は次の 3 点:

1. **C++/WinRT 親和性** — TIP（`tsf-tip/`）・Inference Host（`inference-host/`）が
   すでに C++ / C++/WinRT スタックであり、IPC クライアント（Named Pipe + 4-byte
   length-prefix + JSON、§3.3）の型・実装・ビルド系を設定アプリと共有できる。別言語
   ランタイム（.NET / Rust）を追加で持ち込まずに済む。
2. **Fluent Design 標準対応** — Mica / Acrylic backdrop（`Window.SystemBackdrop` に
   `MicaBackdrop` / `DesktopAcrylicBackdrop`）・Light/Dark・PerMonitorV2 DPI・
   アクセントカラーが追加実装なしで得られ、M26（Win11 ネイティブ体験）と整合する。
3. **配布形態との整合** — MVP の配布形態（§0 / §4 の未署名 MSI）に、設定アプリ
   `azookey_settings.exe` を **self-contained 配置（§1.3・§4 の同梱必須注記）**でペイロード同梱できる。
   MS Store 用 MSIX（§1）では packaged EXE として同梱する。いずれも WinUI 3 の unpackaged /
   packaged 両対応（下記事実更新）で成立し、単一 UI スタックのまま両チャネルに載る。

> **事実更新（Microsoft Learn, 2026-06 時点。旧記述の訂正）**
> - WinUI 3 がサポートする言語は **C# と C++/WinRT のみ**（C++/CX は非推奨）。
> - WinUI 3 / Windows App SDK の対応 OS は **Windows 10 バージョン 1809（build 17763）以降**。
>   ただしこれは**設定アプリ（WinUI 3）単体の下限**であり、配布パッケージ全体の最小 OS では
>   ない（下記）。
> - **unpackaged（MSIX なし）配布もサポートされる**（Windows App SDK 1.0 以降）。旧記述
>   「WinUI 3 デスクトップは MSIX 必須・unpackaged 不可」は**誤り**。本プロジェクトは §0 の
>   とおり **MVP を未署名 MSI（設定アプリは unpackaged / self-contained 同梱）**で配布し、
>   **MS Store 向けには packaged（MSIX）**を用いる。WinUI 3 は両形態に対応するため単一 UI スタックで両立する。
> - **配布パッケージ全体の最小 OS は TIP の配布経路で決まり、設定アプリの 1809 より高い**:
>   §1 の経路 A（external-location packaging）は **Win10 2004 / build 19041 以上**、経路 B
>   （通常 MSIX + `com4:InProcessServer`）は **build 20348 以上**（`MinVersion="10.0.20348.0"`、
>   §1.1）。設定アプリの WinUI 3（1809+）はこのパッケージ下限に内包されるため、MSIX の
>   `TargetDeviceFamily` には 1809 ではなく §1 の経路別下限を設定する。

#### フレームワーク比較

| 軸 | **WinUI 3 (C++/WinRT) ★採用** | WPF (.NET 9+) | Tauri (Rust + WebView2) |
|---|---|---|---|
| 言語 | C++/WinRT（既存スタックと同一） | C#（.NET ランタイム追加） | Rust + JS/TS |
| Fluent / Mica | 標準（`SystemBackdrop`） | .NET 9 で Fluent テーマ標準化（Mica は DWM API 併用） | WebView2 任せ（CSS 自前実装） |
| DPI / Dark | 標準（PerMonitorV2） | 対応（Fluent テーマ・`ThemeMode`） | WebView2 / 自前 |
| 配布 | packaged / unpackaged 両対応 | unpackaged 可・MSIX 可 | 最小バイナリ・WebView2 ランタイム前提 |
| C++ IPC 連携 | 同一言語で interop 不要 | C#↔C++ interop 層が必要 | Rust↔C++ FFI（または既存 Named Pipe を再利用） |
| MS サポート | first-party | first-party | 非サポート（OSS） |
| MSIX 整合 | ◎ | ○ | △（WebView2 依存） |

**代替案メモ**:
- **WPF (.NET 9+)** — .NET 9 で Fluent テーマ（Light/Dark/アクセント、`ThemeMode`、
  `PresentationFramework.Fluent`）が標準化され UI 体験差は縮小した（旧記述「Fluent /
  Mica は非標準」は .NET 9 以降では不正確）。ただし設定アプリに .NET ランタイム依存が
  増え、C++ IPC クライアントとの interop 層が必要になる。**v1.0 では非採用**。
- **Tauri** — 配布サイズ最小だが Microsoft 非サポートで、C++/WinRT との境界が増える
  （既存 Named Pipe IPC を再利用すれば緩和可能）。**v1.x の差別化リッチ UI（M30 拡張）で
  WebView ベースへ切り替える余地を残す**が、v1.0 では非採用。

**仮決定の確証（v1.0 着手前、`gate:human-required`）**: Windows 実機で WinUI 3 / WPF の
最小サンプル（チェックボックス + テキスト入力 + リストボックス + IPC 往復 1 経路）を作り、
(a) 同梱配布サイズ増分、(b) 初回起動時間、(c) C++/WinRT IPC 連携コード行数 の 3 指標を
計測してこの選定を確証する（実測値は Linear 課題に記録し、本 spec には決定と設計のみ残す）。
