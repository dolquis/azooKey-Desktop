# ITf*** インターフェース実装一覧 (tsf-tip/)

このリポジトリの TSF TIP が実装している主要な COM インターフェースの一覧。
追加・削除した際は本ファイルを更新すること。

## TextService (`tsf-tip/include/azookey/tsf/TextService.h`)

`TextService` クラスが以下を多重継承して TSF TIP の本体を構成する。

- `ITfTextInputProcessorEx` — TIP のライフサイクル
  (`Activate` / `Deactivate` / `ActivateEx`)。
- `ITfKeyEventSink` — キー入力フック
  (`OnTestKeyDown/Up`, `OnKeyDown/Up`, `OnPreservedKey`)。
- `ITfThreadMgrEventSink` — フォーカス / ドキュメント遷移
  (`OnInitDocumentMgr`, `OnUninitDocumentMgr`, `OnSetFocus`,
  `OnPushContext`, `OnPopContext`)。
- `ITfCompositionSink` — composition 終了通知
  (`OnCompositionTerminated`)。
- `ITfDisplayAttributeProvider` — 下線 / 色付けスタイル提供
  (`EnumDisplayAttributeInfo`, `GetDisplayAttributeInfo`)。

内部で参照する TSF 側インターフェース: `ITfThreadMgr`, `ITfContext`,
`ITfDocumentMgr`, `ITfComposition`, `ITfRange`, `ITfContextView`,
`ITfContextComposition`, `ITfProperty`, `ITfCategoryMgr`。

## EditSession (`tsf-tip/include/azookey/tsf/TextService.h`)

- `ITfEditSession` — TSF コンテキストへの編集要求を実行する短命オブジェクト
  (`DoEditSession`)。`TextService` から `RequestEditSession` 経由で投入される。

## TextServiceFactory (`tsf-tip/include/azookey/tsf/TextServiceFactory.h`)

- `IClassFactory` — COM クラスファクトリ。`DllGetClassObject` から
  `kTextServiceClsid` の問い合わせに応答して `TextService` を生成する。
- `kTextServiceClsid` / `kTextServiceProfileGuid` は本ヘッダで固定値として
  宣言されており、**変更禁止** (`scripts/register.ps1` および
  `DllRegisterServer` 側と一致する必要がある)。

## DisplayAttribute (`tsf-tip/include/azookey/tsf/DisplayAttribute.h`)

- `ITfDisplayAttributeInfo` — 個別の下線属性情報を返す軽量実装。
- 列挙子 (`IEnumTfDisplayAttributeInfo` 相当) は `DisplayAttribute.cpp` 側で
  実装される。

## DllMain (`tsf-tip/src/DllMain.cpp`)

- エクスポート: `DllMain`, `DllGetClassObject`, `DllCanUnloadNow`,
  `DllRegisterServer`, `DllUnregisterServer` (`tsf-tip/src/exports.def`)。
- `DllRegisterServer` は machine-wide (HKLM) に COM クラスを登録し、
  `ITfInputProcessorProfileMgr::RegisterProfile` で TSF プロファイルを登録、
  `ITfCategoryMgr::RegisterCategory` で `GUID_TFCAT_TIP_KEYBOARD` /
  `GUID_TFCAT_DISPLAYATTRIBUTEPROVIDER` / `GUID_TFCAT_TIPCAP_UIELEMENTENABLED` を
  追加する（HKLM / CTF\TIP 書き込みのため管理者権限が必要）。
- `DllUnregisterServer` は `UnregisterProfile` / `Unregister` でプロファイルと
  カテゴリを解除し、`SHDeleteKeyW` で HKLM の CLSID サブツリーを削除する。

## メンテナンス手順

1. `grep -rn "ITf[A-Z][A-Za-z]*" tsf-tip/include tsf-tip/src` で実装中の
   インターフェースを再確認する。
2. 新しい `ITf***` を継承クラスに追加したら、本ファイルにその責務を 1 行で追記。
3. `QueryInterface` の分岐に新インターフェースの IID を必ず追加する
   (`TextService::QueryInterface` を参照)。
