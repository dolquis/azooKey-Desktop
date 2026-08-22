# 参考になる TSF / IME OSS 実装

新しい `ITf***` インターフェースの呼び出し方や TIP の構造を確認したいときに
参照する OSS / 公式リソース。**実装の正解は `docs/*-spec.md` 側**であり、
これらの実装はあくまで実例として参考にする。

## OSS 実装

- <https://github.com/chewing/windows-chewing-tsf>
  繁体字向け Chewing IME。**現行の既定ブランチ `main` は Rust 実装**
  （2026-08-14 時点の GitHub languages 内訳は Rust / C / RTF / Batchfile で C++ は 0 バイト）。
  TIP 本体は `tip/src/text_service/` 配下（`key_event.rs`、`edit_session.rs`、
  `ui_elements/candidate_list.rs`）、候補 UI は別プロセス host
  （`crates/chewing_tip_host/src/ipc.rs`、`src/ui/window.rs`）に分離されている。
  Rust から COM 境界をどう張るか、`ITfUIElement` と外部 host プロセスをどう分けるか、
  Per-Monitor DPI 変換をどう扱うかの参考に向く。`key_event.rs` には WPF アプリが
  scan code 0 を送る場合の補正（`MapVirtualKeyW(vk, MAPVK_VK_TO_VSC)`）があり、
  実アプリ互換の実例として読める。
  ライセンスは GPL-3.0-or-later のため、**コードを引き写さず設計の比較に留める**。
- <https://github.com/fkunn1326/azooKey-Windows>
  @fkunn1326 さんによる azooKey の先行 Windows 実装 (Rust)。
  同じ azooKey ファミリで挙動の参考になる。
- <https://github.com/7ka-Hiira/hazkey>
  @7ka-Hiira さんによる Linux 向け実装 (fcitx5)。TSF ではないが
  azooKey コア API の利用パターンが参考になる。

## Microsoft 公式

- <https://github.com/microsoft/Windows-classic-samples/tree/main/Samples/Win7Samples/winui/input/tsf>
  MicrosoftのTSF sample群。text serviceとTSF clientの基本構造を確認する一次資料。
- <https://github.com/MicrosoftDocs/win32/tree/docs/desktop-src/TSF>
  Microsoft 公式 Win32 ドキュメントの TSF 章ソース (Markdown)。
- <https://learn.microsoft.com/en-us/windows/win32/tsf/text-services-framework>
  TSF 公式仕様 (Microsoft Learn)。Context7ではMicrosoft/TSF資料として検索し、
  必要な個別APIページを公式URLで確認する。
- <https://learn.microsoft.com/en-us/windows/win32/api/msctf/>
  `msctf.h` の API リファレンス。`ITf***` インターフェース定義の正典。

## 参照時の注意

- sampleのGUID、登録方式、threading、ownershipをそのままコピーしない。
- API signatureとHRESULTは使用中Windows SDKの`msctf.h`とMicrosoft Learnで再確認する。
- OSSの挙動とrepoのspecが違う場合はrepoのspecを優先し、差異を明示する。
