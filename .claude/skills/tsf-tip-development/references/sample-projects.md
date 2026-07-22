# 参考になる TSF / IME OSS 実装

新しい `ITf***` インターフェースの呼び出し方や TIP の構造を確認したいときに
参照する OSS / 公式リソース。**実装の正解は `docs/*-spec.md` 側**であり、
これらの実装はあくまで実例として参考にする。

## OSS 実装

- <https://github.com/chewing/windows-chewing-tsf>
  繁体字向け Chewing IME。C++ で TSF TIP を実装している実用例。
  composition / candidate window 周りの参考に向く。
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
