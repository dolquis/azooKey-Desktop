# ITf*** インターフェース実装一覧 (tsf-tip/)

このリポジトリの TSF TIP が実装している主要な COM インターフェースの一覧。
追加・削除した際は本ファイルを更新すること。

## 目次

- [正典の確認順](#正典の確認順)
- [TextService](#textservice-tsf-tipincludeazookeytsftextserviceh)
- [EditSession](#editsession-tsf-tipincludeazookeytsftextserviceh)
- [TextServiceFactory](#textservicefactory-tsf-tipincludeazookeytsftextservicefactoryh)
- [DisplayAttribute](#displayattribute-tsf-tipincludeazookeytsfdisplayattributeh)
- [Candidate UI](#candidatelistuielement-tsf-tipincludeazookeytsfcandidatelistuielementh)
- [DllMain](#dllmain-tsf-tipsrcdllmaincpp)
- [対応テスト](#対応テスト)

## 正典の確認順

1. `tsf-tip/include/azookey/tsf/*.h`の継承宣言を確認する。
2. 各classの`QueryInterface`が返すIIDと`AddRef`を確認する。
3. `tsf-tip/src/DllMain.cpp`のprofile/category登録とrollbackを確認する。
4. `tsf-tip/tests/query_interface_contract_test.cpp`などの契約テストを確認する。

`rg -n "public ITf|QueryInterface|RegisterCategory|RegisterProfile" tsf-tip/include tsf-tip/src`
を起点にし、宣言だけでなく実際の公開・登録経路まで追う。

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

`TextService::QueryInterface`は`IID_IUnknown`、`IID_ITfTextInputProcessor`、
`IID_ITfTextInputProcessorEx`、上記sink/providerのIIDを公開する。多重継承を追加しただけでは
COMから取得できないため、QueryInterfaceと契約テストを同じ変更で更新する。

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
  宣言されており、**変更禁止** (`scripts/register-dev.ps1` および
  `DllRegisterServer` 側と一致する必要がある)。

## DisplayAttribute (`tsf-tip/include/azookey/tsf/DisplayAttribute.h`)

- `ITfDisplayAttributeInfo` — 個別の下線属性情報を返す軽量実装。
- 列挙子 (`IEnumTfDisplayAttributeInfo` 相当) は `DisplayAttribute.cpp` 側で
  実装される。

## CandidateListUIElement (`tsf-tip/include/azookey/tsf/CandidateListUIElement.h`)

- `ITfCandidateListUIElement`を実装し、`QueryInterface`から`IUnknown`、`ITfUIElement`、
  `ITfCandidateListUIElement`として取得できる。
- `ITfUIElement`契約 — UI-less / app-rendered候補UIの説明、GUID、表示状態を公開する
  (`GetDescription`, `GetGUID`, `Show`, `IsShown`)。
- `ITfCandidateListUIElement`契約 — 候補リストを公開する
  (`GetCount`, `GetString`, `GetSelection`, `GetUpdatedFlags` ほか)。
- 候補なしでは`GetSelection`が`S_FALSE`を返す。out parameterの値を成功時と同様に扱わない。

## CandidateUiCoordinator (`tsf-tip/include/azookey/tsf/CandidateUiCoordinator.h`)

COM interfaceそのものではないが、自前`CandidateWindow`と`ITfUIElementMgr`公開を一元管理する。

- `BeginUIElement`の`pbShow=TRUE`ではTIPの自前HWNDを描画する。
- `pbShow=FALSE`では自前HWNDを表示せず、直後から`UpdateUIElement`でapp側描画へ通知する。
- どちらの経路でもlifecycle終了時に`EndUIElement`とpointer解放を行う。
- `TF_TMF_UIELEMENTENABLEDONLY`は`ITfThreadMgrEx::GetActiveFlags`から判定する。

## DllMain (`tsf-tip/src/DllMain.cpp`)

- エクスポート: `DllMain`, `DllGetClassObject`, `DllCanUnloadNow`,
  `DllRegisterServer`, `DllUnregisterServer` (`tsf-tip/src/exports.def`)。
- `DllRegisterServer` は machine-wide (HKLM) に COM クラスを登録し、
  `ITfInputProcessorProfileMgr::RegisterProfile` で TSF プロファイルを登録、
  `ITfCategoryMgr::RegisterCategory` で `GUID_TFCAT_TIP_KEYBOARD`、
  `GUID_TFCAT_DISPLAYATTRIBUTEPROVIDER`、`GUID_TFCAT_TIPCAP_UIELEMENTENABLED`
  を追加する（HKLM / CTF\TIP 書き込みのため管理者権限が必要）。
- profile/category登録の途中失敗では`RegistrationRollback`がbest-effort cleanupを行い、
  部分登録を残さない。category追加時はrollback側の解除対象も同期する。
- `DllUnregisterServer` は `UnregisterProfile` / `Unregister` でプロファイルと
  カテゴリを解除し、`SHDeleteKeyW` で HKLM の CLSID サブツリーを削除する。

## 対応テスト

| 変更領域 | 主なtarget |
|---|---|
| `QueryInterface` / IUnknown | `tsf_tip_query_interface_contract_tests` |
| DisplayAttribute | `tsf_tip_display_attribute_tests` |
| `ActivateEx` / UI-less | `tsf_tip_activate_uiless_tests` |
| Candidate UI negotiation | `tsf_tip_candidate_ui_coordinator_tests` |
| key / composition / preedit | `tsf_tip_onkeydown_preedit_tests` |
| COM/profile/category登録 | `tsf_tip_com_smoke_tests` (`tsf-com` label) |

## メンテナンス手順

1. `rg -n "public ITf|QueryInterface" tsf-tip/include tsf-tip/src`で実装を再確認する。
2. 新しい`ITf***`を追加したら、継承宣言、全`QueryInterface`経路、factory、categoryを同期する。
3. 取得・保管・解放のownershipをテストし、本ファイルへ責務を追記する。
4. UI能力を追加した場合は`Begin/Update/EndUIElement`とcategory登録を同時に検証する。
