---
name: tsf-tip-development
description: tsf-tip/ 配下のC++、COM/TSFインターフェース、EditSession、候補UI、DllGetClassObject、登録・解除、Win32コールバックを変更・レビュー・デバッグするときに使用する。
---

# TSF TIP 開発ガイド

## 最初に確認する

1. `docs/windows-tsf-host-architecture.md`と変更対象の`docs/*-spec.md`を読む。
2. `.codegraph/`がある場合はCodeGraphで対象symbol、call path、関連テストを確認する。
3. `references/itf-interfaces.md`で現在のclass、IID、登録category、テスト対応を確認する。
4. API契約はMicrosoft Learnと使用中Windows SDKの`msctf.h`を突き合わせる。
5. 参考実装が必要な場合だけ`references/sample-projects.md`を読む。参考実装よりrepoのspecを優先する。

## 守るべき原則

1. **TIP は arbitrary process にロードされる in-proc DLL**。
   - 推論、モデルロード、永続化など重い処理を直接実行せず`inference-host`へ委譲する。
   - 不要なmutable process-global状態を増やさない。必要なmodule handleやtest hookは用途を限定し、
     共有状態には`std::atomic`またはlockを使う。
2. **TSF/COMオブジェクトをUI apartment外から操作しない**。
   - IPC workerとは値型、queue、window messageで連携し、結果反映はUI threadのEditSessionで行う。
3. **COM境界からC++例外を漏らさない**。失敗は契約に合う`HRESULT`へ変換する。
4. **`IUnknown`のidentityと参照カウントを守る**。
   - `QueryInterface`成功時は`AddRef`する。所有・borrow・transferを呼び出しごとに確認する。
   - out pointerは契約に従って検証・初期化し、既存実装とテストが使う`E_POINTER`または
     `E_INVALIDARG`を一律に置換しない。
5. **CLSID、Profile GUID、既存UIElement GUIDを変更しない**。categoryは実装済み能力と同期する。
6. **lifecycleを対称にする**。advise/unadvise、Begin/EndUIElement、composition、worker、
   COM pointerの取得/解放をactivate/deactivateと失敗経路の両方で閉じる。

## 参照リソース

- `references/itf-interfaces.md` — 本プロジェクトで実装している ITf*** インターフェースの一覧と責務
- `references/sample-projects.md` — 参考になる OSS TSF IME 実装の一覧

## ツールと実機境界

- 仕様確認ではContext7でMicrosoft/Win32 TSF資料を検索し、Microsoft Learnの個別APIページと
  使用中SDK headerでsignature、HRESULT、threading条件を確認する。Context7を任意URL fetchとして扱わない。
- clangd診断は補助として使い、MSVC buildとGoogleTestの代替にしない。
- Windows build/testはプロジェクト指定のWindows Headless CMake Build経路を使う。
- TIP登録/解除の検証：`DllRegisterServer` は COM クラスと TSF プロファイルを
  machine-wide (HKLM / CTF\TIP) に登録するため**管理者権限が必須**
  (`scripts/register-dev.ps1` は非管理者なら UAC 昇格して `regsvr32` を実行)。
  PowerShell.MCP(共有コンソール)で登録コマンドを提示し、**実行はユーザーが
  管理者 PowerShell で完了させる**。エージェント単独で登録を完了させない。
  なお HKCU を使うのは inference-host の自動起動(`Run` キー)のみで、TIP の
  COM / プロファイル登録は HKLM 側である。
- 実アプリでの入力検証：Windows-MCP の UI Automation

## 変更手順

1. 対象interfaceの公式signature、IID、継承関係、apartment条件を確認する。
2. headerの継承、`QueryInterface`、factory/registration、呼び出し元、解除経路を列挙する。
3. COM pointerごとに取得時のref、保管期間、解放箇所を確認する。
4. 最小差分で実装し、COM entry pointの例外境界とout parameterを検証する。
5. 変更対象に合わせて次を実行する。
   - IID/COM契約: `tsf_tip_query_interface_contract_tests`
   - display attribute: `tsf_tip_display_attribute_tests`
   - UI-less activation: `tsf_tip_activate_uiless_tests`
   - candidate UI: `tsf_tip_candidate_ui_coordinator_tests`
   - key/preedit: `tsf_tip_onkeydown_preedit_tests`
   - 登録round-trip: `tsf_tip_com_smoke_tests`（昇格可能時。未昇格ではskipを確認）
6. 最終確認では`cmake --build --preset windows-debug --target azookey_check`を優先する。
7. UI-less描画、TIP登録、実アプリ入力などunit testで代替できない検証は人間ゲートとして明記する。

## 禁止事項

- TIP DLLでモデルロード、永続化、長時間のfile I/Oを行わない。
- `MessageBox`などのモーダルUIをTIPから表示しない。
- IPC workerから`ITfContext`、`ITfRange`、`ITfComposition`、UIElementを直接操作しない。
- categoryだけを先に登録し、対応する`QueryInterface`やUIElement公開実装を後回しにしない。
- C++/WinRTを`tsf-tip/`へ持ち込まない。
- `legacy/`のSwift実装をWindows仕様の正解として扱わない。
