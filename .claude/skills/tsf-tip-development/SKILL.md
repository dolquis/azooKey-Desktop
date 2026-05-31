---
name: tsf-tip-development
description: tsf-tip/ 配下の C++ コード(ITfTextInputProcessor 実装、COM コンポーネント、TSF コールバック)を編集・追加・デバッグするときに使用する。COM / TSF / Win32 API の作業で自動起動する。
allowed-tools: Read, Edit, Grep, Glob, WebFetch
---

# TSF TIP 開発ガイド

## このスキルが扱う範囲

- `tsf-tip/` 配下のすべての C++ 実装
- COM クラス登録(CLSID)と TSF プロファイル登録(Profile GUID)
- `ITfTextInputProcessor`, `ITfThreadMgrEventSink`, `ITfCompositionSink`, `ITfKeyEventSink` 等の実装
- `DllRegisterServer` / `DllUnregisterServer` のエクスポート

## 守るべき原則

1. **TIP は arbitrary process にロードされる in-proc DLL**。
   - 重い処理を直接書かない。`ipc/` 経由で `inference-host` に投げる。
   - グローバル状態を持たない。スレッドセーフを徹底(`std::atomic`、`std::mutex`)。
   - C++ 例外を COM 境界の外に漏らさない(`HRESULT` で返す)。
2. **CLSID / Profile GUID は不変**。既存の GUID を変更しない。
3. **`IUnknown` の参照カウントは厳密に管理**。`Microsoft::WRL::ComPtr` を使う。
4. **すべての COM メソッドは `STDMETHODIMP` 戻り値、`E_POINTER` 等の防御チェックを入れる**。

## 参照リソース

- `references/itf-interfaces.md` — 本プロジェクトで実装している ITf*** インターフェースの一覧と責務
- `references/sample-projects.md` — 参考になる OSS TSF IME 実装の一覧

## 補助ツール(マーケット品を活用)

- 仕様確認：context7 MCP 経由で <https://learn.microsoft.com/en-us/windows/win32/tsf/> を fetch
- 補完・診断：clangd ベースの LSP（Claude Code は `clangd-lsp` プラグイン経由）
- TIP登録/解除の検証：PowerShell.MCP(共有コンソール)で対象ユーザーとして実行（HKCU user-scope、昇格不要）
- 実アプリでの入力検証：Windows-MCP の UI Automation

## やってはいけない

- TIP の DLL から直接ファイル I/O を行う(ホスト側に委譲)。
- `MessageBox` 等のモーダル UI を TIP から出す。
- C++/WinRT を `tsf-tip/` に持ち込む(古い COM ベースの TSF と相性が悪い)。
- `legacy/` の Swift 実装の挙動を「正解」として参照する(仕様は `docs/*-spec.md` に従う)。

## 新規 ITf*** インターフェース実装時の手順

1. `docs/` 配下に該当する仕様 md があるか確認。無ければ先に仕様を起こす。
2. インターフェース ID(IID)と必要なメソッドを最新仕様で確認(context7 経由)。
3. `tsf-tip/` にヘッダ＋実装を追加。`ComPtr` で受け取り、`HRESULT` で返す。
4. GoogleTest にユニットテスト(COM 境界を mock 化して呼び出し検証)。
5. 既存 TIP に `QueryInterface` 経路を追加。
