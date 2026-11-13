# 設定アプリ 仕様（settings-app）

対象リポジトリ: dolquis/azooKey-Desktop
対応マイルストーン: M11（最小設定アプリ）、M30（フル設定 UI）
関連: `docs/sideload-packaging-spec.md`（配布方針、§4 WiX / MSI）、
      `settings/mvp-settings.schema.json`（設定キーの正典スキーマ）、
      `docs/windows-tsf-host-architecture.md`（設定の読み込みと `UpdateConfig`）

本書は `docs/sideload-packaging-spec.md` から §3（設定アプリ）を切り出した文書である。
節番号 §3.x は旧 spec のまま保ち、旧 spec の転送表が `§3` を本書へ解決する。

## 正典範囲

設定アプリ（`settings-app/`、`azookey_settings.exe`）の UI フレームワーク選定、構成、ナビゲーション、
Host との IPC、設定ファイルのパス、`ITfFnConfigure` 連携、設定キーの一覧と永続化と拡張方針、
v1.0 の最小機能セットは本書が正典である。配布チャネル、MSI / MSIX、署名、更新、観測は
`docs/sideload-packaging-spec.md` と各節の正典が定める。

## 節とマイルストーンの対応

| 節 | マイルストーン |
|---|---|
| §3.0〜§3.5 | M11（最小）・M30（フル） |
| §3.6 | M11・M30（設定キーの確定） |
| §3.7 | M11（v1.0 最小機能セット） |

## 3. 設定アプリ（M11 最小 / M30 フル）

### 3.0 UI フレームワーク選定（採用: WinUI 3 / C++/WinRT）

設定アプリの UI フレームワークは **WinUI 3（Windows App SDK, C++/WinRT）** とする。対象は
**v1.0 スコープの M11（最小設定アプリ。Phase 4 リリースゲート）** と、それを
**post-v1.0 で本格化する M30（フル設定 UI）**、および同一 UI スタックに乗る後続機能
（モデル管理 UI = M45、学習データ可視化 UI = M49）である。同一フレームワークに揃えて後続の
作り直しを避ける（v1.0 では M11 の最小機能セットのみを出荷し、M30 のフル UI / 横断機能は
v1.0 に引き込まない）。

TIP と Inference Host と同じ C++/WinRT スタックで IPC クライアント（§3.3）を共有でき、
Fluent Design（Mica / Acrylic、Light/Dark、PerMonitorV2 DPI）を追加実装なしで得られ、
unpackaged / packaged の両配布に載る。WPF (.NET 9+) と Tauri との比較、および選定時に確かめた
Microsoft Learn の事実は
[`decisions/2026-06-settings-app-ui-framework.md`](decisions/2026-06-settings-app-ui-framework.md) にある。

配布形態と OS 下限は次のとおりとする。

- MVP の未署名 MSI（§0 / §4）では、`azookey_settings.exe` を unpackaged の
  **self-contained 配置（§1.3・§4 の同梱必須注記）**でペイロードに同梱する。
  MS Store 用 MSIX（§1）では packaged EXE として同梱する。
- WinUI 3 がサポートする言語は C# と C++/WinRT である（C++/CX は使わない）。
- WinUI 3 / Windows App SDK の対応 OS は **Windows 10 バージョン 1809（build 17763）以降**である。
  これは設定アプリ単体の下限であり、配布パッケージ全体の最小 OS ではない。
- **配布パッケージ全体の最小 OS は TIP の配布経路で決まり、設定アプリの 1809 より高い**:
  §1 の経路 A（external-location packaging）は **Win10 2004 / build 19041 以上**、経路 B
  （通常 MSIX + `com4:InProcessServer`）は **build 20348 以上**（`MinVersion="10.0.20348.0"`、
  §1.1）。MSIX の `TargetDeviceFamily` には 1809 ではなく §1 の経路別下限を設定する。

再評価の条件は次のとおりとする。

- **確証（`gate:human-required`）**: Windows 実機で WinUI 3 / WPF の最小サンプル
  （チェックボックス + テキスト入力 + リストボックス + IPC 往復 1 経路）を作り、
  (a) 同梱配布サイズ増分、(b) 初回起動時間、(c) C++/WinRT IPC 連携コード行数 の 3 指標で
  この選定を確かめる。実測値は Linear 課題に記録する。
- v1.x の差別化リッチ UI（M30 拡張）では、WebView ベース（Tauri）へ切り替える余地を残す。

### 3.1 構成

- 言語: C++/WinRT（TIP / Host と統一）
- フレームワーク: WinUI 3（採用 component packages と版は
  `settings-app/azookey_settings.vcxproj` / `THIRD_PARTY_LICENSES` を正典とする。更新時は
  self-contained 出力と MSI の回収結果を再検証する）
- 配布: MVP は MSI にペイロード同梱（別 EXE `azookey_settings.exe`。self-contained 配置、§1.3 / §4）。
  MS Store 用 MSIX（§1）では packaged EXE として同梱（配布方針は §0）

### 3.2 ナビゲーション

設定アプリは左側の `NavigationView` でペインを切り替える。ペインは 2 つの群（`NavigationViewItemHeader`
で見出しを付ける）と、フッターの 1 つに分ける。

- 「設定」群: `settings.json` のキーを編集するペイン。変えた値は保存バーで保存するまで書かない。
- 「データとツール」群: Host への IPC で操作し、その場で反映するペイン。保存の対象を持たず、保存バーを出さない。
- フッター（`NavigationView.FooterMenuItems`）: 「バージョン」。`autoUpdate` の 3 キーを持ち、保存バーを出す。

起動時は「入力」を表示する。`--proofread-session` で起動されたときは「校正」を表示し（`rich-features-spec.md`
X-3-6）、`--pane <名前>` を受けたときはそのペインを表示する（名前は下表のペインの英名 `input` / `candidates` /
`dictionary` / `ai-conversion` / `ai-service` / `privacy` / `profiles` / `advanced` / `learning` / `proofread` /
`version`。知らない名前は無視して「入力」を表示する）。既に開いている窓へ転送されたときは、`--pane` があればそのペインへ移り、無ければ今のペインを保つ（§3.5）。`--pane` は入力インジケーターの「設定を開く」が渡す
（`docs/tsf-deep-integration-spec.md` §4.5）。`ITfFnConfigure::Show`（§3.5）は渡さない。
`liveConversion` / `predictionEnabled` だけは、入力インジケーターの右クリックのメニューからも切り替えられる（TIP が対象の 1 キーを書く。`docs/tsf-deep-integration-spec.md` §4.5）。

| 群 | ペイン | 内容 |
|---|---|---|
| 設定 | 入力 | 入力方式（`convertKeysAction` / `fullWidthSpace` / `inputStyle` / `customRomajiTablePath` / `liveConversion` / `predictionEnabled`）/ 句読点（`punctuationConversion` と動的句読点）/ ローマ字の一括変換 |
| 設定 | 候補 | インライン英単語候補（`mixedEnglishAutoConversion` を含む）/ 候補リライター（数字・カタカナ・記号・絵文字）/ カッコ補完（カッコの字形を決める `inputMode` を含む）/ `maxCandidates` |
| 設定 | 辞書 | `dictionary` の層切替（`neologd_lexicon` pack の帰属提示と同意、Host が返す層の状態〔`QueryDiagnostics` の `neologd_layer`〕の表示を含む）/ `autoWordRegistration` / タイプミス学習 / 「学習」ペインのユーザー辞書タブを開くボタン |
| 設定 | AI 変換 | `model.enabled` / `models` フォルダのモデル一覧と検証結果（`ListModels`）/ ベンチマーク（`BenchmarkModel`）/ `model.selectedPath` の選択（「選択を外す」で空の未選択に戻す。§3.7）/ `model.benchmarkHistory` の表示 / 既定の catalog の entry の一覧と取得ボタン（`FetchCatalogModel` と進捗の表示。§1.6.1「利用者の操作による取得」）と取得中の中止（`CancelCatalogFetch`。`model-management-spec.md` §6.1）（`model-management-spec.md` §4 / §6）/ `model.backendPreference` / `powerProfile` / `inferenceThreads` / `maxContextLength` |
| 設定 | AI サービス | `aiBackend` / `openAiApiKey` / `openAiApiEndpoint` / `openAiModel` / `openAiTimeoutMs` / `llmMagicConversion` / `includeContextInAITransform` |
| 設定 | プライバシー | `privacy`（`crashReportConsent` を含む） |
| 設定 | アプリ別 | `profilesByApp` の追加・編集・削除（`candidateTagBoosts` の 1.0〜3.0 を含む）と、`promptPrefixByApp` の読み取り専用の一覧と、行ごとの「プロファイルへ移す」操作（`app-profile-spec.md` §4 / §6 / §8） |
| 設定 | 詳細 | `logLevel` / 実験フラグ / SafeMode の解除 |
| データとツール | 学習 | ストア別（学習候補 / ユーザー辞書 / タイプミス補正 / 新語候補）の LearningStore 表示と検索 / 個別忘却 / 全削除 / エクスポート / インポート（学習データ管理 IPC。`learning-data-management-spec.md` §3 / §4）/ ユーザー辞書への語の追加（`AddUserWord`。§3.7）/ タブ「入力の傾向」: Persona 4 比率と算出の基になった確定の回数・算出時刻の表示（読み取り専用。`QueryPersona`。`rich-features-spec.md` X-2-7） |
| データとツール | 校正 | 貼り付けた文章、または TIP が Ctrl+Shift+Space で入力中のアプリから取った文章の誤りの疑いの一覧（`DetectAnomalies`。外部の AI へ本文を送る旨を常に画面に示す。`rich-features-spec.md` X-3-6） |
| フッター | バージョン | 製品のバージョン / 更新の確認（`autoUpdate` の 3 キーと、その下に結果。下記）/ ライセンス / 起動コンテキスト（言語 ID とプロファイル。起動引数を受けたときだけ表示し、受けなかった値は「未指定」と示す） |

`autoUpdate` の 3 キーは「バージョン」の「更新の確認」の節で編集できる（`enabled` をオンにしたときだけ `channel` と
`checkIntervalHours` を操作できる）。同じ節の下に、Host の最後の更新確認の結果（§6.1 の
`update-status.json`）を表示し、新しいバージョンがあればリリースページを開くリンクを置く。

「プライバシー」の障害診断の節には `privacy.crashReportConsent` のほか、「診断ファイルの保存先を開く」と
「ログ出力先を開く」（§3.7）のボタンを置く（`docs/dev-infrastructure-spec.md` §8.5.4・§12.1.1 が設定アプリの診断として指す先は、この節である）。

「AI 変換」は「設定」群に置くが、モデル一覧の更新・ベンチマーク・取得・中止は Host への IPC でその場で行い、
保存バーを経ない。保存バーが書くのは、このペインの設定キー（`model.selectedPath` を含む）だけである。

保存バー:

- 「設定」群のペインと「バージョン」で、スクロール領域の外のウィンドウ下端に固定して表示する。保存結果の InfoBar も保存バーに出す。
- 保存は「設定」群の全ペインと「バージョン」の値をまとめて書く（モデルの取得などの IPC 操作は含まない。書くキーの規則は §3.7「UI に露出したキーの書き込み」）。
- 読み込んだ値から変えたキーの数を示し、変えたキーが無い間は保存ボタンを無効にする。
  例外として、読み込んだファイルの形式が不正か無効な項目を含むときは、保存で退避や除去をして直すため、変えたキーが無くても保存ボタンを有効にし、件数の代わりに「保存すると設定ファイルを修復します」と示す。
- 変えたキーが残ったままウィンドウを閉じようとしたときは、保存する・破棄する・閉じるのをやめる、を選ぶ確認を出す。

各ペインの見出しはペイン名とし、その下にペインごとの 1 行の説明を置く。アプリ名の見出しと、全ペインに共通の説明文は置かない。

「データとツール」群の各ペインと、「AI 変換」のモデル一覧・ベンチマーク・取得は、Host の IPC を呼ぶ設定アプリ側の
client（`SettingsIpcClient`）と同じ変更でナビゲーションに置く。「AI 変換」でモデルを選ぶと、同じペインの
「使うモデル」の表示が変わり、保存バーが `model.selectedPath` を書く。一覧から選べるのは検証に通った GGUF だけである。
一覧に無いローカルの GGUF は、同じペインの「参照…」ボタン（ファイルピッカー）で選ぶ（§3.7「モデルパスの
probe-then-commit」の明示選択）。モデルのパスを手で入力する欄は置かない。
Host が capability（`learning_reset` / `persona` など）を告知しないときは、その操作を送らず、
ペインに「この変換エンジンは未対応です」と示す。
`ListModels` が返す `onnx_genai` は一覧に出すが選択と測定の対象にしない。学習データは
設定アプリが直接開かず、一覧・忘却・エクスポート・インポートをすべて Host への IPC で行う。
忘却・全削除・インポートは確認ダイアログを経る。暗号化しないエクスポートは警告を出して確認する。

Host の IPC を読むペインは、その IPC を呼ぶ client と同じ変更でナビゲーションに加え、
中身の無いペインは置かない。

上の表と §3.6「UI ペイン」列に対する実装の追跡先は Linear とする。「校正」が読む文章をフォアグラウンドの
アプリから取る経路（TIP の preserved key、選択位置の周りの文書の取得、TIP 所有の session、アプリ側の範囲の
選択）は `rich-features-spec.md` X-3-6 が正典である。

> 本表はペイン割り当ての概観である。設定キーの正典一覧（全 top-level キー・型・既定・永続化・
> 反映方法・拡張方針）は §3.6 を参照する。

専用コントロールを持たないキーの UI は、`settings-app/SettingsFields.cpp` の記述子表から生成する。
記述子は各キーの型・既定・範囲・選択肢・ペインと、機能別 spec が「効果を持たない」と定める条件
（例: `privacy.custom.*` は `privacy.mode` が `custom` のときだけ、カッコ補完の各キーは `bracketPairing` が
真のときだけ）を持ち、条件を満たさない間はコントロールを無効にする。`dynamicPunctuation` /
`batchRomajiConversion` / `inlineEnglishCandidates` / `bracketPairing` の 4 つのスイッチは、同じ節で後に並ぶ
項目を、スイッチの下の閉じた「詳細設定」に畳む。畳むのは表示だけで、有効・無効は上の条件で決める。
保存できない値が畳んだ項目にあるときは、「詳細設定」を開いてその項目へフォーカスする。
OpenAI API キーの欄は「OpenAI」の節の末尾に置き、同じ節の条件（`aiBackend` が `openai`）を満たさない間は
入力欄だけを無効にする。保存済みのキーを消せるように、「削除」は条件によらず押せる。記述子と schema の型・既定・範囲・
選択肢の一致、schema の全キーが「生成する / 専用コントロール / 編集しない（理由付き）」のいずれかに
属すること、表示文字列の有無は `azookey_settings_persistence_tests` が検査する。

#### 表記規則

設定アプリの表示文字列（`settings-app/Strings/ja-JP/Resources.resw`）と、入力インジケーターのメニュー
（`docs/tsf-deep-integration-spec.md` §4.5）は次の規則に従う。保存する値（schema の enum 値）は変えない。
規則が拘束するのは表示文字列とメニューであり、spec の地の文は、画面の語を指す箇所（ペイン名・項目名・機能名）を
これに揃える。

- 利用者向けの文に内部の語を出さない。置き換えは次の表に従う。

  | 内部の語 | 表示 |
  |---|---|
  | Host | 変換エンジン（例「変換エンジンが起動していません」「この変換エンジンは未対応です」） |
  | backend（推論） | 推論デバイス |
  | backend（AI サービス） | AI サービス |
  | catalog | 取得できるモデル |
  | pack | 追加辞書 |
  | prompt prefix | AI への指示文 |
  | Persona | 入力の傾向 |
  | Settings アプリ | 設定アプリ |
  | merge / overwrite / keep_both | 統合する / 上書きする / 両方残す（英語の併記をしない） |
  | bytes | KiB（校正の文字数の上限は「0 / 60 KiB」の形） |

- 1 つの概念に 1 つの語を使う。「カッコ」（機能名「カッコ補完」と `inputMode` の表示名「カッコ補完の字形」は
  `bracket-pairing-spec.md` に合わせる。「括弧」と書かない）、「予測変換」（「予測」だけにしない）。
  「セーフモード」は Host が繰り返し異常終了したときの安全な構成だけを指し、パスワード欄などの保護は
  「セーフ入力」と書く。`privacy.mode` の `secure` は「常にセーフ入力」と表示する。
- 列挙値は日本語で表示する。`logLevel` は `error` / `warn` / `info` / `debug` を「エラー」/「警告」/「情報」/「デバッグ」と表示する。
- 状態は絵文字で示さず、文と InfoBar・InfoBadge で示す。
- 注記は 1 項目につき 1 文を目安にし、同じ注記を複数の項目に繰り返さない。版の番号（「v1.0 の」など）を注記に書かない。
- 画面に無い語で操作の手順を書かない。手順は、ペイン名と項目の表示名をかぎ括弧で示す（例「『プライバシー』の『外部 AI を使う』」）。

> **参考（fkunn1326/azooKey-Windows, MIT）**: 「AI 変換」ペインの `backendPreference` 選択は、
> 各バックエンドのランタイム DLL 存在判定（`cudart64_12.dll`+`cublas64_12.dll` / `vulkan-1.dll`）
> で**未対応バックエンドを disable + 理由 Tooltip 表示**する能力検出 UX を採れる（先行実装の
> `check_capability` 相当）。一次フィルタ扱いとし、実 EP 選択は `docs/copilot-pc-backend-spec.md`
> §3-§4 に委ねる。`backendPreference` は自分側 schema が `cpu/cuda/vulkan/winml/directml/npu`
> を持ち、参考側（cpu/cuda/vulkan）より広い。詳細は DEV-120 のコメント参照。**この能力検出は、当該
> バックエンドをリンクしたビルドを配布してから導入する**（DEV-854 再確定）。DLL 存在判定が答えるのは
> 機器側の可否であって、ビルドが当該バックエンドを含むかではないため、未リンクのバックエンドを
> DLL 判定で出し分けると実効のない選択肢を有効化してしまう。v1.0 UI の縮小と露出条件は §3.7 を正典とする。

### 3.3 Host との IPC

設定アプリは `%LOCALAPPDATA%\azooKey\config\settings.json` を更新した後、Host に
payload 空の `UpdateConfig` メッセージを送信して再読込を促す（入力インジケーターのメニューでライブ変換を切り替えたときは TIP も送る）。設定オブジェクトは
IPC schema に二重定義しない。設定アプリと Host の保存・quarantine は同一の named mutex 下で
read-modify-write または read から rename までを直列化する（writer 責務とこの排他の
正典は `docs/learning-data-management-spec.md` §14）。

```
UpdateConfigRequest:
  request_id

UpdateConfigResponse:
  request_id
  ok: bool
  error: optional<string>
```

Host は `settings.json` を再読込し、即時反映可能なものを適用する。破損などで
再読込結果が invalid の場合、`ok=false` と `error` を返し、現在の runtime 設定は
維持する。応答は再起動の要否を持たない。Host の再起動後にだけ反映される変更は、設定アプリが
保存の成功後に再起動指示を表示する（対象は §3.6「反映方法の確定」）。

### 3.4 設定ファイルパス

| 用途 | パス |
|---|---|
| 設定 | `%LOCALAPPDATA%\azooKey\config\settings.json` |
| カスタムローマ字 | `%LOCALAPPDATA%\azooKey\custom-romaji.tsv` |
| 学習データ | `%LOCALAPPDATA%\azooKey\data\learning.v2.tsv.enc`（M54 の v2 形式。DPAPI 暗号化、M34。M7 形式の `learning.tsv.enc` は移行元） |
| ユーザー辞書 | `%LOCALAPPDATA%\azooKey\data\user_dict.json.enc`（DPAPI 暗号化、M34） |
| モデル | `%LOCALAPPDATA%\azooKey\models\zenzai\<file>.gguf`（`<file>` は §1.6.1 のピン（既定の catalog）が定める実ファイル名。出所は上流 `Miwa-Keita/zenz-v3.2-small-gguf`） |
| ログ | `%LOCALAPPDATA%\azooKey\logs\*.jsonl` |

拡張子 `.enc` のない `learning.tsv` と `user_dict.json` は旧版からの移行元であり、移行後の保存先ではない。

### 3.5 `ITfFnConfigure` 連携（Windows 設定「詳細設定」からの起動）

Windows の「設定 > 時刻と言語 > 言語と地域 > 日本語 > キーボード」や従来の Text Services
コントロールパネルから本 IME の「プロパティ / オプション」を開くと、TSF マネージャは TIP の
CLSID を `CoCreateInstance` し `IID_ITfFnConfigure` を要求して
`ITfFnConfigure::Show(hwndParent, langid, rguidProfile)` を呼ぶ（M30 受け入れ条件
「Windows 設定からの『詳細設定』起動で開く」に対応）。

- **実装場所**: `ITfFnConfigure` は **TIP DLL（`tsf-tip/`）側**が実装する（設定アプリ EXE
  ではない）。TSF が `DllRegisterServer`（§1）で登録した CLSID に対して直接
  `CoCreateInstance` するため。語句登録の `ITfFnConfigureRegisterWord` が
  `ITfFunctionProvider::GetFunction` 経由で取得されるのと異なり、`ITfFnConfigure` は
  **CLSID 直 `CoCreateInstance`** で取得される点に注意 — すなわち
  `CoCreateInstance(kTextServiceClsid, IID_ITfFnConfigure)` であり、**TextService オブジェクト
  自身が `QueryInterface(IID_ITfFnConfigure)` で応答する**必要がある（別クラスで実装する場合も
  TextService の QI 経由で到達可能にする。さもないと `E_NOINTERFACE` で「詳細設定」が開かない。
  `docs/tsf-deep-integration-spec.md` §6.2）。
- **呼び出し元**: `Show` は **Windows の言語/IME 設定（Text Services コントロールパネル
  相当）が TIP の CLSID を `CoCreateInstance` したプロセス上**で同期的に呼ばれ、
  `hwndParent` はその設定 UI のウィンドウである（入力先のメモ帳・ブラウザ等ではない）。
- **方式（非同期起動）**: `Show` は設定アプリを起動して `S_OK` を即時返す。理由と
  single-instance の扱いは `docs/tsf-deep-integration-spec.md` §6.2 が正典。設定値の反映は
  §3.3 の `UpdateConfig` IPC で行う。
- **引数受け渡し**: `langid` / `rguidProfile` は起動コマンドライン引数として
  `azookey_settings.exe` に渡し、「バージョン」の起動コンテキストに表示する（§3.2）。設定アプリは
  `AppInstance::FindOrRegisterForKey` で単一インスタンスを登録し、後続起動は
  `RedirectActivationToAsync` で新しい引数を既存インスタンスへ転送する。既存ウィンドウは転送後に
  前面化し、ウィンドウタイトルによるプロセス間検索は行わない。受信側は LANGID を16進値、
  profile を GUID として検証し、未指定と不正値を区別する。起動コンテキストの表示は、転送された起動を含め、
  最後に受けた起動の引数で決め直す（`langid` も profile も無い起動では隠す）。
- **校正ビューの引数**: TIP の校正ビュー（`rich-features-spec.md` X-3-6）は
  `--proofread-session <token>` だけを渡す。token は小文字 16 進 32 桁で、重複・値なし・それ以外の値は
  parse error とする。後続起動の転送でも同じく既存インスタンスへ渡り、「校正」ペインがその session を開く。
- **開くペインの引数**: `--pane <名前>`（§3.2）は開くペインだけを指定する。`--proofread-session` と同時に
  受けたときは `--proofread-session` を優先して「校正」を開く。`langid` / `rguidProfile` は開くペインを
  変えず、どちらも受けたときも `--pane` が無ければ「入力」を開く（言語プロファイルごとの設定ページは持たない）。
  既に開いている窓へ転送されたときは、`--pane` があればそのペインへ移り、無ければ今のペインを保つ。
- **正典実装**: 具体コード（`ITfFnConfigure`・`ShellExecuteExW` による起動・
  インストールパス解決）は
  `docs/tsf-deep-integration-spec.md` §6 を正典とする。本節は配布・プロセス境界の観点を補う。

### 3.6 設定スキーマの確定（キー一覧・永続化・拡張方針）

設定キーが各機能 spec に分散したまま実装に入ると、設定アプリと Host の整合が崩れる。
本節は schema の最終形（全 top-level キー）と永続化・拡張方針を一箇所に確定する索引で
あり、M30 設定アプリ・後続 UI（M45 モデル管理 / M49 学習データ可視化）の共通基盤とする。

**正典アーティファクトと責務境界**:

- **スキーマ正典**: `settings/mvp-settings.schema.json`（JSON Schema draft 2020-12）。
  **固定形オブジェクト**（root、および `model` / `autoUpdate` のように既知フィールド集合を持つもの）は
  `additionalProperties: false` を維持し、未知キーを拒否する。一方、**マップ形オブジェクト**は値の型を
  制約した `additionalProperties`（`false` ではない）で任意キーを許容する設計であり、本規則の対象外とする
  （例: `promptPrefixByApp` = `additionalProperties: {type: string}`、`profilesByApp` および
  プロファイル内 `candidateTagBoosts` も同様のマップ）。
- **永続インスタンス**: `%LOCALAPPDATA%\azooKey\config\settings.json`（§3.4）。設定アプリが
  書き込み（`liveConversion` / `predictionEnabled` の 1 キーずつは入力インジケーターのメニューから TIP も書く）、Host が読み込んで適用する（往復は §3.3 の `UpdateConfig`）。
- **意味論の正典は各機能 spec**: 各キーの挙動・enum 値の意味・閾値は下表「正典」列の spec が
  定義する。本節はそれらを **再定義せず**、設定アプリ統合の観点（UI ペイン / 導入マイルストーン /
  反映方法）を一覧化する。v1.0 出荷時に同梱する**最小サブセット**の選定は DEV-107（M11 schema
  サブセット）が扱い、本節は schema 最終形と永続化・拡張方針を確定する。

#### キー一覧（top-level）

| キー | 型 / enum | 既定 | UI ペイン | 導入 | 反映 | 正典 |
|---|---|---|---|---|---|---|
| `inputMode` | enum `hiragana`/`alnum_half`/`alnum_full` | `hiragana` | 候補 | Phase 5（基本） | 即時 | `bracket-pairing-spec.md` §4.4（表示名「カッコ補完の字形」） |
| `convertKeysAction` | enum `default`/`imeOnOff` | `default` | 入力 | DEV-1736 | 即時 | `tsf-deep-integration-spec.md` §3.1.1 |
| `fullWidthSpace` | bool | `true` | 入力 | DEV-1814 | 即時 | `tsf-deep-integration-spec.md` §3.7 |
| `inputStyle` | enum `default`/`custom` | `default` | 入力 | M17 | 即時 | roadmap M17 |
| `customRomajiTablePath` | string（パス） | `…\custom-romaji.tsv` | 入力 | M17 | 即時（ホットリロード） | roadmap M17 |
| `liveConversion` | bool | `false` | 入力 | M14 | 即時 | roadmap M14 |
| `punctuationConversion` | enum `off`/`commit` | `off` | 入力 | DEV-1638 | 即時 | `tsf-deep-integration-spec.md` §3.5.1 |
| `dynamicPunctuation` | bool | `false` | 入力 | M59 | 即時 | `dynamic-punctuation-spec.md` |
| `dynamicPunctuationStyle` | enum `ja`/`fullwidth_latin` | `ja` | 入力 | M59 | 即時 | `dynamic-punctuation-spec.md` |
| `dynamicPunctuationStability` | enum `onPause`/`eager` | `onPause` | 入力 | M59 | 即時 | `dynamic-punctuation-spec.md` |
| `dynamicPunctuationIdleMs` | integer (ms) | `400` | 入力 | M59 | 即時 | `dynamic-punctuation-spec.md` |
| `segmentBoundaryConfidence` | number (0.0–1.0) | `0.5` | 入力 | M59 | 即時 | `dynamic-punctuation-spec.md` |
| `punctuationRulesPath` | string（パス） | `…\punctuation-rules.tsv` | 入力 | M59 | 即時（ホットリロード） | `dynamic-punctuation-spec.md` |
| `predictionEnabled` | bool | `true` | 入力 | M15 | 即時 | roadmap M15 |
| `batchRomajiConversion` | bool | `false` | 入力 | M58-A | 即時 | `romaji-batch-conversion-spec.md` |
| `batchRomajiPreviewStyle` | enum `kana`/`romaji` | `kana` | 入力 | M58-A | 即時 | `romaji-batch-conversion-spec.md` |
| `batchConversionMode` | enum `neural`/`ai-cleanup` | `neural` | 入力 | M58-A/C | 即時 | `romaji-batch-conversion-spec.md` |
| `batchAutoPunctuation` | bool | `false` | 入力 | M58-C | 即時 | `romaji-batch-conversion-spec.md` |
| `llmMagicConversion` | bool | `false` | AI サービス | M16 | 即時 | roadmap M16 |
| `aiBackend` | enum `none`/`openai`/`local-zenzai` | `none` | AI サービス | M16 | 即時 | roadmap M16 |
| `openAiApiKey` | string（機微・§9 DPAPI） | `""` | AI サービス | M16 / M34 | 即時 | roadmap M16 / §9 |
| `openAiApiEndpoint` | string（URL） | `https://api.openai.com/v1` | AI サービス | M16 | 即時 | roadmap M16 |
| `openAiModel` | string | `gpt-4o-mini` | AI サービス | M16 | 即時 | roadmap M16 |
| `includeContextInAITransform` | bool | `true` | AI サービス | M16 | 即時 | roadmap M16 |
| `promptPrefixByApp` | map<string,string> | `{}` | アプリ別（読み取り専用の一覧と移行操作。保存時保持。新規編集は `profilesByApp[].promptPrefix`） | rich X-2-6 | 即時 | `rich-features-spec.md` X-2-6 / `app-profile-spec.md` §6 |
| `contextReselection` | bool（実験） | `false` | 詳細 | rich X-3-2 | 即時 | `rich-features-spec.md` X-3-2 |
| `postCommitLint` | bool（実験） | `false` | 詳細 | rich X-3-3 | 即時 | `rich-features-spec.md` X-3-3 |
| `retroactiveRecompute` | bool（実験） | `false` | 詳細 | rich X-1-3 | 即時 | `rich-features-spec.md` X-1-3 |
| `sentenceCompletion` | bool（実験） | `false` | 詳細 | rich X-2-1 | 即時 | `rich-features-spec.md` X-2-1 |
| `backendPreference` | enum `auto`/`cpu`/`cuda`/`vulkan`/`winml`/`directml`/`npu` | `auto` | AI 変換※ | M24（device 選択 UI は `model.backendPreference` にバインド〔下記※〕。v1.0 UI は `auto`/`cpu` に縮小、§3.7） | モデル再ロード | `copilot-pc-backend-spec.md` §4 |
| `epPreference` | enum `auto`/`npu`/`gpu`/`cpu` | `auto` | AI 変換※ | M24（EP 選択 UI は `model.epPreference` にバインド〔下記※〕。root は back-compat のみ・UI 非バインド） | モデル再ロード | `copilot-pc-backend-spec.md` §4.4 |
| `powerProfile` | enum `auto`/`performance`/`battery_saver` | `auto` | AI 変換 | M25 | 即時 | `copilot-pc-backend-spec.md` §5–§6 |
| `logLevel` | enum `error`/`warn`/`info`/`debug` | `info` | 詳細 | Phase 5（基本） | 即時 | schema / §7 |
| `model` | object（`model-management-spec.md` §7 が下位フィールドを定義） | — | AI 変換（`enabled` / `selectedPath` / `backendPreference` とモデル一覧〔§3.2〕。`epPreference` は §3.7 の解禁条件まで UI に出さない） | M45（`enabled`/`selectedPath`/`backendPreference` の 3 フィールドは v1.0=M11 で先行露出、§3.7） | モデル再ロード | `model-management-spec.md` §5/§7 |
| `autoUpdate` | object（`enabled`/`channel`/`checkIntervalHours`） | — | バージョン（結果も同じ節に表示） | M32 | 即時 | 本書 §6 |
| `dictionary` | object（7つの層切替） | 下位キーの既定に従う | 辞書 | M53 | 即時（設定再読込）。`neologdEnabled` を真にしたときの pack 取得だけは Host 起動時 | `auto-word-registration-spec.md` §14.8 / §15.14 |
| `openAiTimeoutMs` | integer（ms、1000–120000） | `30000` | AI サービス | M16 | 即時 | `ai-backend-spec.md` |
| `inferenceThreads` / `maxCandidates` / `maxContextLength` | integer（0–8 / 1–32 / 0–30） | `0` / `9` / `30` | AI 変換（`inferenceThreads` / `maxContextLength`）/ 候補（`maxCandidates`） | — | 即時 | `windows-tsf-host-architecture.md` |
| `bracketPairing` ほか `bracket*` の 10 キー | bool / enum / string[] / string（パス） | 各キーの schema 既定 | 候補 | M61 | 即時 | `bracket-pairing-spec.md` |
| `inlineEnglish*` の 6 キーと `fullWidthEnglishCandidate` | bool / integer / number / string（パス） | 各キーの schema 既定 | 候補 | M60 | 即時 | `inline-english-candidate-spec.md` |
| `mixedEnglishAutoConversion` | bool | `false` | 候補 | M63 | 即時 | `mixed-english-auto-conversion-spec.md` §14 |
| `numberRewriter` / `katakanaRewriter` / `symbolRewriter` / `symbolDataPath` と `emoji*` の 5 キー | bool / integer / string（パス） | 各キーの schema 既定 | 候補 | M62 | 即時 | `candidate-rewriter-spec.md` |
| `typoCorrectionMode` / `typoMinCount` | enum `off`/`suggest`/`auto_replace` / integer（1–100） | `suggest` / `3` | 辞書 | M35 | 即時 | `typo-correction-learning-spec.md` |
| `autoWordRegistration` | object | 下位キーの既定に従う | 辞書 | M36-A | 即時（設定再読込） | `auto-word-registration-spec.md` |
| `reranker` | object | 下位キーの既定に従う | —（保存時保持。専用 UI は持たない） | M56 | 即時 | `neural-reranker-nll-spec.md` §B9 |
| `privacy` | object | 下位キーの既定に従う | プライバシー | M46（`crashReportConsent` は M33） | 即時 | `privacy-and-secure-input-spec.md` §7 |
| `profilesByApp` | map<string,object> | `{}` | アプリ別（追加・編集・削除。保存は読み込み時と異なるときだけ、オブジェクト全体を書く） | M48 | 即時 | `app-profile-spec.md` §4 |
| `safeMode` | object（Host が書く） | 下位キーの既定に従う | 詳細（`enabled` の解除だけを操作し、`enteredAt` / `lastCrashCount` は表示専用。解除は保存まで取り消せ、保存時の `enteredAt` が読み込んだ値と異なる〔Host が再び SafeMode に入った〕ときは解除しない） | M47 | 即時（設定再読込） | `dev-infrastructure-spec.md` |

> オブジェクト型キー（`model` / `autoUpdate` / `dictionary` / `autoWordRegistration` / `reranker` /
> `privacy` / `safeMode`）とマップ型キーの下位フィールドは「正典」列の spec が確定形を持つ。本表で
> 再掲せず、ネスト構造の単一情報源を維持する。
>
> 「—（保存時保持）」のキーは設定アプリに編集 UI を持たないが、保存時に有効な値を書き戻す
> （§3.7 の write-back 規則）。Host が書く値（`safeMode.enteredAt` / `safeMode.lastCrashCount` /
> `model.benchmarkHistory`）は、設定アプリから編集しない。`model.benchmarkHistory` は「AI 変換」ペインが
> 表示だけし、保存は既存の値を書き戻す。

> **※ device 選択 UI のバインド先（§3.7）**: `backendPreference` / `epPreference` の **root tier は後方互換用の
> 下位レイヤ**であり（解決順 `model.*` ＞ root、`model-management-spec.md` §5.2）、設定アプリの device 選択 UI は
> **v1.0 / M24 とも `model.backendPreference` / `model.epPreference`（`model.*` tier）にバインド**する。**root tier は
> UI にバインドしない** — M24 で root を再露出すると、`model.*` を設定済みのユーザーでは編集しても実効値が変わらず
> （`model.*` ＞ root）「selector が壊れて見える」二重ソース問題に戻るため。root tier は `settings.json` 直書きの
> 後方互換としてのみ残し、§3.7 の保存規律（device 保存時に root `backendPreference` を削除して `model.*` を単一
> 実効ソースにする）と整合させる。

#### 永続化形式と適用順

- 単一の `settings.json`（UTF-8）に top-level キーを格納する。書き込みは破損耐性のため
  atomic write（一時ファイル → rename）で行う。
- **レイヤリング / 優先順位**: per-app `profilesByApp`（M48）＞ `model.*`（同名 root キーを上書き、
  `model-management-spec.md` §5.2）＞ root。実効値の解決順は `app-profile-spec.md` §5 を正典とする。
- **反映方法の確定（v1.0 キー）**: Host は `settings.json` を hot-reload し、**プロセス再起動を要する
  キーを持たない**。モデルロードに影響するキー（`backendPreference` / `epPreference` / `model.*`）は
  Host 内部の推論エンジン再ロードで反映する（`model-management-spec.md` §5.3、`model.autoLoadOnHostStart`）。
  それ以外は次回変換から即時反映する。例外は `dictionary.neologdEnabled` を偽から真にする変更で、
  Host は `neologd_lexicon` pack を起動時にだけ取得するため（`azdic-physical-layer-spec.md` §15.14）、
  反映は Host の再起動後になる。設定アプリはこの変更を保存したときに、サインアウトして再度サインイン
  すると反映される旨を表示する（Host はログオン時に起動する。§1.7）。再起動を要する変更の判定は
  設定アプリが持つ静的な分類で行い、`UpdateConfig` の応答には含めない。
- **機微値**: `openAiApiKey` は M34 で DPAPI 暗号化し `dpapi:` プレフィックス付きで保存する（§9）。
  平文保存は移行期のみ許容し、Host は両形式を受理する。

#### 拡張方針（`additionalProperties: false` 下でのキー追加）

- **追加は加算的**: 新規キーは既定値付きで追加し、旧 `settings.json` は欠落キーを schema 既定で
  補完して前方互換を保つ。リネーム・削除・enum 縮小は破壊的変更とし、Host の `SettingsStore` または
  設定アプリの `settings-app/SettingsDocument.*` に移行処理を実装し、`docs/dev-infrastructure-spec.md`
  §12.2.1 に旧形式を登録してから schema を変更する。
- **schema とコードの同時更新**: 新規 top-level キーの schema 追加と Host 側読み書き実装は同一 PR で
  行い、schema 不在のままキーを書き込む不整合を作らない（`privacy-and-secure-input-spec.md` §7 /
  `app-profile-spec.md` §4 と同方針）。
- **下位フィールドの正典**: `privacy`（M46。`crashReportConsent` だけ M33）は
  `docs/privacy-and-secure-input-spec.md` §7、`profilesByApp`（M48）は `docs/app-profile-spec.md` §4 の
  schema fragment を正典とする。既存 `promptPrefixByApp` との統合は `app-profile-spec.md` §6 に従う。
- **§3.2 ナビゲーションとの整合**: §3.2 はペイン割り当ての概観であり、キーの正典一覧は本節とする。
  §3.2 に挙げる「ETW プロバイダ」は固定 GUID（§7.1）であって設定キーではない（ログ詳細度は `logLevel`
  が制御する）。「学習」ペインのストア別の一覧と「入力の傾向」タブはキーではなく、Host の IPC を読むビューである。

### 3.7 v1.0 設定 UI の最小機能セット（M11）と v1.0 / v1.x 境界

§3.6 は schema の最終形（全 top-level キー）を確定する索引である。本節はそのうち
**v1.0（M11）出荷時に設定アプリへ UI として出す最小サブセット**を確定し、残りを v1.x（M30
フル UI）へ送る境界を引く。狙いは 2 つ: D-03（WinUI 3 UI フレームワークスパイク）の実装
ボリュームを確定させ、v1.0 を遅らせないこと。本節は新キーを追加しない —— v1.0 UI は
`settings/mvp-settings.schema.json`（§3.6 の正典 schema）の**部分集合**を描画するだけで、
schema 自体は superset のまま変えない。

> **設計判断（DEV-107）**: 起票時の推奨案は `zenzaiEnabled` / `modelPath` / `userDictionaryPath`
> という新キーを提案していたが、§3.6 で schema 正典が確定した後はこれらを新設しない。M11 の目的
> （`plans/windows-port-roadmap.md` M11「Zenzai ON/OFF・辞書管理・デバイス選択」）を**既存の正典
> キーへ写像**して実現する: Zenzai ON/OFF → `model.enabled`、デバイス選択 → `model.backendPreference`
> （v1.0 は enum を縮小）、モデル選択 → `model.selectedPath`。ユーザー辞書は設定 `settings.json` の
> キーではなく `%LOCALAPPDATA%\azooKey\data\user_dict.json.enc`（§3.4）の内容であり、v1.0 はキーではなく
> 「編集」アクション（下記ボタン）で扱う。

#### v1.0（M11）で UI 化するキー（最小サブセット）

| キー | v1.0 UI の型 / 値域 | UI ペイン | 裏づけ M | 備考 |
|---|---|---|---|---|
| `model.enabled` | bool（「Zenzai を使う」トグル） | AI 変換 | M8 | false で SimpleConverter 固定（`model-management-spec.md` §7） |
| `model.backendPreference` | enum **`auto` / `cpu` に縮小** | AI 変換 | M8 | `auto` は Host 起動時に BackendSelector が解決する backend（`copilot-pc-backend-spec.md` §4.4、`--backend` 明示時はその値）に従い、`cpu` は CPU に固定する。`cuda` を含む残り enum と `epPreference` の解禁条件は下記「v1.0 / v1.x 境界表」に置く（判断の根拠は「デバイス選択の enum 縮小（DEV-854 再確定）」） |
| `model.selectedPath` | string（モデルの絶対パス。**空＝モデル未選択**） | AI 変換 | M8 | 空は「ピン既定へ自動解決」ではない。Host は `selectedPath` をそのまま `autoLoadOnHostStart` でロードし、空なら何もロードせず SimpleConverter（M8 受け入れ「未配置時も落ちない」）。下記「probe-then-commit」を参照 |
| `logLevel` | enum `error`/`warn`/`info`/`debug` | 詳細 | M2〜 | 診断用。ログ詳細度のみ（ETW プロバイダ GUID は設定キーではない、§3.6） |

- **デバイス選択の enum 縮小（DEV-854 再確定）**: v1.0 UI は `model.backendPreference` を
  `auto` / `cpu` の 2 値だけ描画する。**`cuda` は v1.0 UI に出さない**。CUDA backend の実リンク
  （DEV-223）は中止され、`cuda` 要求は `docs/copilot-pc-backend-spec.md` §4.4 のとおり成功 LoadModel の
  まま CPU へ降格するため、選択肢として出すと実効のない選択になる。ランタイム DLL の存在判定による
  disable + Tooltip（§3.2 参考実装注）はここでは採らない。DLL 判定が答えるのは「この機器に CUDA
  ランタイムがあるか」であって、実際の制約は「配布ビルドが CUDA をリンクしていないか」だからである。
  CUDA ランタイムを備えた NVIDIA 機ではむしろ選択肢が有効化され、実効のない選択がそのまま残る。
  能力検出は CUDA リンク済みビルドを配布してから導入する。
- **v1.0 の 2 値が持つ意味**: `auto` は Host 起動時に `docs/copilot-pc-backend-spec.md` §4.4
  「`auto` の解決（BackendSelector）」が解決する backend に従い、`cpu` は CPU に固定する（解決順は `model-management-spec.md` §5.2、
  `auto` のときに root `backendPreference` へ委ねる挙動を含む）。CPU 推論のみを含む v1.0 配布ビルドでは
  どちらも R1 CPU に解決するが、保存される意図は異なる。`auto` はアクセラレータをリンクした
  ビルドを配布したときにそれへ追随し、`cpu` は追随せず CPU に留まる。`auto` が選ぶのは、ビルドに
  リンクされ、かつ同 §4.4 の一次フィルタ（ハードウェア GPU、loader、電源状態）を通ったアクセラレータに
  限られ、選んだ backend もロードに失敗すれば CPU へ降格する。M11 の目的「デバイス選択」はこの 2 値で満たし、
  UI にはアクセラレーションの有無を誤解させないため「この配布ビルドは CPU 推論のみを含む」という
  静的な補足文を添える。
- **`vulkan` の UI 露出条件を R1 側へ切り離す**: `vulkan`（R1 ggml-vulkan）の UI 露出は、これまで
  v1.x（M24）としていたが、M24 は R2 / Windows ML のマイルストーンであり `docs/copilot-pc-backend-spec.md`
  §4.3 のとおり R2 には再開条件がある。R1 に属する `vulkan` を R2 の再開に従属させないため、**露出条件を
  「ggml-vulkan ビルドを配布に含め、かつ実行時にそれを選ぶ経路が配線された時点」へ改める**（R1 側の
  条件であり、M24 の進捗に依存しない）。実行時の選択は `docs/copilot-pc-backend-spec.md` §4.4 の
  BackendSelector が担い、Vulkan ビルド（`windows-vulkan-release` preset）の `auto` は一次フィルタを
  通った機器で Vulkan を選ぶ。
  配布物への組み込みと §1.6 の配布条件は DEV-1001 で扱う。その条件を満たしてから別途 UI の
  選択肢を追加し、それまでは **v1.0 UI には出さない**。
- **降格の可視化**: Host 側の Health 判定は §4.4 / `docs/zenzai-inference-spec.md` §9.2.1 のまま変えない。
  `cuda` 降格はモデルをロードでき変換も動くため `Health=ok` であり、degraded は実エラーで降格した場合に
  限る。v1.0 UI から `cuda` を外すことで UI 由来の実効のない選択は消え、残るのは `settings.json` 直書きの
  `cuda` だけになる。この値は下記の「UI が描画しない schema 有効値」と同じ扱いとし、UI は選択なしの状態で
  現在値と「現在の配布ビルドでは CPU 実行へ降格する」旨を表示し、保存時にその値を温存する。
- **UI が描画しない schema 有効値の扱い**: `settings.json` に v1.0 UI が出さない schema 有効値
  （`cuda` / `vulkan` / `winml` / `directml` / `npu`）が直書きされていても、Host は schema 上受理し、
  UI は選択肢として描画しないだけで破棄しない。schema 正典（§3.6）は完全 enum を保持する
  （enum 縮小は §3.6 拡張方針で破壊的変更に当たるため、UI 露出だけを変え schema には手を入れない）。
- **`model.*` の UI 露出を M11 へ前倒し**: §3.6 の「導入」列は `model` を M45 と記すが、これは**フル
  モデル管理 UI**（`epPreference` / `nGpuLayers` / `benchmark*` / `autoLoadOnHostStart` / `fallbackToSimpleConverter`
  等の全フィールド）の導入時期である。v1.0 はそのうち `enabled` / `backendPreference`（縮小）/ `selectedPath`
  の 3 フィールドのみを露出する。キーは schema に既存のため schema 変更は不要で、変わるのは UI 露出の時期だけ。
  実効値の解決順（`model.*` ＞ root 同名キー、§3.6 永続化 / `model-management-spec.md` §5.2）は v1.0 でも同一。
- **モデルパスの probe-then-commit（既定 autoselect と明示選択を分ける）**: 空 `model.selectedPath` は
  「ピン既定モデルを使う」を意味しない（Host は空を未選択として扱い SimpleConverter へ落ちる、
  `model-management-spec.md` §7 / `autoLoadOnHostStart`）。よって v1.0 UI は**空を既定ファイルへ暗黙解決しない**。
  `selectedPath` 確定の規律は §1.6.1「配置パスとバージョニング」の取得経路定義に従い、**2 経路を混同しない**:
    - **既定モデルの autoselect（ピン依存、§1.6.1 (b) DL / (c1)）**: ピン定義（既定の catalog、§1.6.1）の
      モデルファイルが**実在し SHA256 がピン一致することを probe してから**、その絶対パスを `model.selectedPath`
      へ commit する（commit 後に「既定モデル使用中」と表示）。ピンと一致しなければ autoselect しない。
    - **ユーザー / 管理者の明示選択（ピン非依存、§1.6.1 (c2)）**: 設定 UI のモデル選択（ファイルピッカ）や
      `settings.json` 手編集で選ばれた**任意の実在 GGUF パス**を probe-load して `model.selectedPath` へ commit する。
      **ピン一致を要求しない**（§1.6.1 (c2)「ピン非依存の唯一の手動経路」を v1.0 UI が塞がない。
      非ピンのローカル GGUF を管理者が選ぶ M28/M11 経路を degraded に落とさない）。
  いずれの経路でも、パス未確定の間は `selectedPath` を空のまま残し、UI は「使うモデル」に「未選択（AI モデルを使わずに
  変換します）」と表示する（Host は SimpleConverter で動く）。これにより「既定モデルと表示しているのに Host にパスが渡らず fallback のまま」と「明示選択した非ピン
  モデルが commit されず degraded のまま」の両方の不整合を防ぐ。
- **root `backendPreference` を v1.0 UI に二重表示せず、保存時に UI 選択値で上書きする**: デバイス選択は
  `model.backendPreference` 一本に統一する（`model-management-spec.md` §5.2 の解決順は `model.backendPreference`
  → root `backendPreference` → 既定 `auto`）。root を非表示のまま write-back で温存すると、ユーザーが直書きした
  root `cuda` 等が UI の「auto」選択を上書きし続け、UI からバックエンドを auto へ戻せない。これを防ぐため、**v1.0 UI が
  デバイス選択を保存するとき、UI で選んだ値（`auto` を含む）を `model.backendPreference` に書き、root `backendPreference`
  を削除する**。**root の隠れ値を `model.*` へ「移行」してはならない** —— 移行すると root `cuda` が
  `model.backendPreference` に複製され、UI の `auto` 選択を上書きし続けてリセット不能になる（root 値を持ち越さず、
  常に現在の UI 選択で上書きする）。これにより `model.backendPreference` が単一の実効ソースになる。この root
  `backendPreference` 削除は後述 write-back 規則の例外（保持対象外）とする。`epPreference` は v1.0 UI が露出せず
  device 選択にも従属しないため、root / `model.epPreference` とも温存する（直書き intent を壊さない。v1.x で管理）。
  **`model.backendPreference` が v1.0 UI の描画しない schema 有効値（`cuda` 等）のときは上書き対象から外し、
  その値を温存する**（DEV-854 再確定）。このとき UI はデバイス選択を未選択の状態にし、現在値と降格の注記を
  表示する。上書きは UI が選択を持つときの規律であり、UI が選べない値をその 2 値へ丸めると直書き intent を
  壊すためである。root `backendPreference` の削除はこの場合も行い、実効ソースを `model.backendPreference` に
  一本化する規律は変えない。

#### v1.0（M11）の UI アクション（設定キーではないボタン）

- **ユーザー辞書の追加・削除** → 「学習」ペイン（§3.2）のユーザー辞書タブに置く。タブは一覧と検索、
  「読み」「語」の 2 欄と「追加」ボタン、行ごとの「忘却」を持つ。追加は `AddUserWord`（roadmap M9）、
  削除は `ForgetLearningEntry`（`learning-data-management-spec.md` §4.2）で Host へ送る
  （詳細は同 spec §3）。v1.0 は専用 GUI を作らず、この最小の導線だけを持つ。語の編集、辞書の単独の書き出し、
  既定で入る語は v1.x（M30）。設定アプリは v1.0 / v1.x とも `user_dict.json.enc` を直接開かず、Host への IPC を
  経由する（`docs/learning-data-management-spec.md` §14）。開発者向けの
  CLI（`AddUserWord` / `RemoveUserWord`）も同じ経路で残る。
- **「ログ出力先を開く」** → `%LOCALAPPDATA%\azooKey\logs\`（§3.4）を Explorer で開く。

#### v1.0 / v1.x 境界表

「v1.0 で UI 化」しないキーは、v1.0 では `settings.json` 直書き（＋ Host hot-reload、§3.3）か
debug probe で操作し、v1.x（M30 フル UI / 各機能の UI 化マイルストーン）で UI 露出する。下表は
§3.6 の全キーに対する v1.0 / v1.x の UI 露出区分である（意味論の正典は §3.6「正典」列に従う）。

| キー | v1.0 UI | v1.x で UI 化（v1.0 では schema 直書き / probe） |
|---|---|---|
| `model.enabled` | ◯（AI 変換） | — |
| `model.backendPreference` | ◯（AI 変換、`auto`/`cpu` のみ） | `vulkan` = ggml-vulkan ビルド配布・起動保証（DEV-1001）+ 実行時選択経路の配線（DEV-944） / `cuda` = CUDA リンク済みビルド配布 + 同配線 / `winml`・`directml`・`npu` = M24（`winml` 統合先。§5.1） |
| `model.selectedPath` | ◯（AI 変換） | — |
| `logLevel` | ◯（詳細） | — |
| `model.*` の残りフィールド（`nGpuLayers`/`benchmark*`/`autoLoadOnHostStart`/`fallbackToSimpleConverter` 等） | — | M45（「AI 変換」ペイン。§3.2） |
| `model.epPreference` / `epPreference`（root） | — | `model.epPreference` は当該バックエンドをリンクしたビルドの配布後（M24）。root は UI にバインドしない（§3.6 ※） |
| `powerProfile` / `inferenceThreads` / `maxCandidates` / `maxContextLength` / 実験フラグ | — | M30（`powerProfile` / `inferenceThreads` / `maxContextLength` は AI 変換ペイン、`maxCandidates` は候補ペイン、実験フラグは詳細ペイン） |
| `inputMode` / `convertKeysAction` / `fullWidthSpace` / `inputStyle` / `customRomajiTablePath` / `liveConversion` / `predictionEnabled` | — | M30（入力ペイン。`inputMode` は候補ペイン） |
| 句読点・バッチ変換・インライン英単語候補・候補リライター・カッコ補完の各キー | — | M30（句読点・バッチ変換は入力ペイン、インライン英単語候補・候補リライター・カッコ補完は候補ペイン） |
| `llmMagicConversion` / `aiBackend` / `openAiApiEndpoint` / `openAiModel` / `openAiTimeoutMs` / `includeContextInAITransform` | — | M30（AI サービスペイン） |
| `openAiApiKey` | — | M34（AI サービスペイン。鍵は §9 DPAPI） |
| `promptPrefixByApp` / `profilesByApp` | — | M48（アプリ別プロファイルの編集 UI。新規編集は `profilesByApp`） |
| `autoUpdate.*` | — | M32（更新の確認と「バージョン」の結果表示。§6） |
| `dictionary.*` / `autoWordRegistration.*` / `typoCorrectionMode` / `typoMinCount` | — | M30（辞書ペイン。`neologdEnabled` を真にするときは帰属の提示と同意を経る。`azdic-physical-layer-spec.md` §15.14） |
| `privacy.*` | — | M30（プライバシーペイン） |
| `safeMode.*` | — | M30（詳細ペインで解除だけを操作する） |
| `reranker.*` | — | 専用 UI を持たない（`neural-reranker-nll-spec.md` §B9） |
| ユーザー辞書の内容（設定キーではない。`user_dict.json.enc`） | ◯（学習ペインのユーザー辞書タブで一覧・追加・忘却） | M30（語の編集、単独の書き出し、既定で入る語） |

> v1.0 で UI 化しないキーも schema 正典（§3.6）には残り、`settings.json` 直書きと Host hot-reload で
> 機能自体は動く。v1.0 設定アプリは未露出キーを**消さない**（下記バリデーションの write-back 規則）。

#### 設定アプリ起動時の schema バリデーション

- 設定アプリは起動時に `settings.json` を `settings/mvp-settings.schema.json`（JSON Schema draft 2020-12）で
  検証する。**未知キー / 型不正 / enum 外の値は警告ログ（`logLevel`）に記録して skip し、当該キーは既定値
  （§3.6 拡張方針「欠落キーは schema 既定で補完」）で扱う。**破損キー 1 つで設定アプリやランタイムを
  停止させない（fail-safe）。
- **write-back 規則**: 設定アプリが `settings.json` を保存するとき、**schema 上有効で UI に露出していないキー**
  （v1.x キー・ユーザーが直書きした有効値）を**保持して書き戻す**。v1.0 UI が知らないだけの有効キーを silently に
  消去しない（直書きワークフローを壊さないため）。一方、**起動時バリデーションで弾いた未知キー / 型不正 / enum 外の
  エントリは write-back で温存しない**（quarantine して書き戻さない）。これらを温存すると、`additionalProperties: false`
  の固定オブジェクト（§3.6）に対して Host 再読込が `ok=false`（§3.3）になり、UI 側の有効な変更まで Host に拒否されるため。
  すなわち保持対象は「schema-known かつ UI-hidden」に限る。書き込みは破損耐性のため atomic write（一時ファイル → rename、§3.6 永続化）。
- **UI に露出したキーの書き込み**: 記述子表から生成した UI は、読み込んだ値（有効な値が無ければ schema
  既定）から変えたキーだけを書く。変えたキーも、`settings.json` にそのキーが無く値が schema 既定と等しいときは
  書かない。ユーザーが触れていないキーは、記述子の範囲外の値も含めてディスク上の値のまま残り、欠落キーは
  schema 既定を引き継ぎ続ける（§3.6 拡張方針「欠落キーは schema 既定で補完」）。有効なコントロールの値が
  型・範囲・選択肢に合わないときは何も書かずに保存を失敗させ、該当する設定名を表示し、そのペインを開いて
  コントロールにフォーカスする。数値の入力は範囲へ丸めず、表示のために値を丸めない。依存条件を満たさず
  無効になっているコントロールの不正な値は、保存の対象から外す。`dictionary` の 7 キーも、読み込んだ値から
  変えたスイッチだけを、ディスクにあるか schema 既定と異なるときに書く。
- Host 側の再読込時バリデーション（無効なら `UpdateConfigResponse.ok=false` + `error`、runtime 設定維持）は
  §3.3 を正典とする。本節は**設定アプリ側の起動時検証**を補い、二重定義しない。
