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
- [内部 EditSession](#内部で-itfeditsession-を実装する補助)
- [COM ではない補助クラス](#com-ではない補助クラス)
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
- `ITfFnConfigure` — 言語バー / 設定メニューからの設定 UI 起動 (`Show`)。
  `Show` は UI thread で `SettingsLauncher` の `LaunchSettingsApplication` へ委譲し、
  `azookey_settings.exe` を `ShellExecuteExW` で起動する。TIP 自身は設定 UI を描画しない。

`TextService::QueryInterface`は`IID_IUnknown`、`IID_ITfTextInputProcessor`、
`IID_ITfTextInputProcessorEx`、上記sink/providerのIID、`IID_ITfFunction`、
`IID_ITfFnConfigure`を公開する。多重継承を追加しただけでは
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

## 内部で `ITfEditSession` を実装する補助

`EditSession` のほかに、翻訳単位の短い同期 EditSession を内部 class として持つ補助がある。
どちらも public header には COM class を出さず、関数または static メソッドだけを公開する。

- `AiInputAllowed` (`tsf-tip/include/azookey/tsf/AiInputGuard.h`) — 内部の `ScopeSession` が
  `ITfEditSession` を実装し、`TF_ES_SYNC | TF_ES_READ` で `RequestEditSession` を発行して
  context の `InputScope` を読む。owner thread 専用で、InputScope が判定できない場合は
  fail closed（AI 入力を許可しない）。UIA や network を呼ばない。
- `BracketEditSession` (`tsf-tip/include/azookey/tsf/BracketEditSession.h`) — 内部の
  `SynchronousSession` が `ITfEditSession` を実装し、`ReadHint` / `Apply` / `Finish` の static
  メソッドが同期 EditSession を要求して括弧ペアリング（`core::BracketPairingAction`）を
  TSF 操作へ翻訳する。判定ロジックは `core/` 側にあり、ここは翻訳だけを持つ。

新しい内部 EditSession を足すときも、`QueryInterface` で `IID_ITfEditSession` を返し、
`DoEditSession` から C++ 例外を漏らさない点は `EditSession` と同じ契約に従う。

## COM ではない補助クラス

TSF/COM object ではないが、thread 境界と責務境界を持つため変更時に確認する。

- `CandidateWindow` (`CandidateWindow.h`) — 候補リストを描画する popup HWND。生成した thread
  でだけ使い、内部 lock を持たない。`CandidateUiCoordinator` が `pbShow` に応じて表示を決める。
- `TipLocalSettings` (`TipLocalSettings.h`) — Host に依存せず共有 `settings.json` を読み、
  directory watch worker で再読込する。worker は TSF/COM object に触らず、`Snapshot` /
  `RewriterSnapshot` の値型だけを UI thread へ渡す。
- `ForegroundAppDetector` (`ForegroundAppDetector.h`) — アプリ別プロファイル解決のための
  前面プロセス判定。owner thread 専用で、window title を読まず、identity を IPC へ送らない。
- `SettingsLauncher` (`SettingsLauncher.h`) — `ITfFnConfigure::Show` の実体。テスト用 hook は
  `AZOOKEY_TSF_TESTING` 時のみ公開する。

## DllMain (`tsf-tip/src/DllMain.cpp`)

- エクスポート: `DllMain`, `DllGetClassObject`, `DllCanUnloadNow`,
  `DllRegisterServer`, `DllUnregisterServer` (`tsf-tip/src/exports.def`)。
- `DllRegisterServer` は machine-wide (HKLM) に COM クラスを登録し、
  `ITfInputProcessorProfileMgr::RegisterProfile` で TSF プロファイルを登録、
  `ITfCategoryMgr::RegisterCategory` で `GUID_TFCAT_TIP_KEYBOARD`、
  `GUID_TFCAT_DISPLAYATTRIBUTEPROVIDER`、`GUID_TFCAT_TIPCAP_UIELEMENTENABLED`、
  `GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT`
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
| 応答の鮮度（stale response 破棄） | `tsf_tip_staleness_tests` |
| caret 位置と候補窓の座標 | `tsf_tip_caret_position_tests` |
| 候補窓の DPI スケール | `tsf_tip_candidate_window_dpi_tests` |
| TIP ローカル設定の読込と監視 | `tsf_tip_local_settings_tests` |
| COM/profile/category登録 | `tsf_tip_com_smoke_tests` (`tsf-com` label) |

上の target 名は入口であり網羅ではない。CTest 一覧の正典は `docs/test-inventory.md` で、
target の追加・削除はそちらと `scripts/check_test_inventory.py` が追う。

## メンテナンス手順

1. `rg -n "public ITf|QueryInterface" tsf-tip/include tsf-tip/src`で実装を再確認する。
2. 新しい`ITf***`を追加したら、継承宣言、全`QueryInterface`経路、factory、categoryを同期する。
3. 取得・保管・解放のownershipをテストし、本ファイルへ責務を追記する。
4. UI能力を追加した場合は`Begin/Update/EndUIElement`とcategory登録を同時に検証する。
