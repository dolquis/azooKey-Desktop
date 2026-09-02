# 学習データ可視化・バックアップ 仕様

対象リポジトリ: dolquis/azooKey-Desktop
対応マイルストーン: M49（プライバシー / モデル管理 / 学習データ UI トラック）
関連: `plans/windows-port-roadmap.md` M7 / M9 / M30 / M34 / M35 / M36-A / M55、
      `docs/typo-correction-learning-spec.md`、
      `docs/auto-word-registration-spec.md`、
      `docs/privacy-and-secure-input-spec.md`
作成日: 2026-05-27
位置づけ: Phase 7 末尾（M30 / M34 完了後）

## 1. 目的

ユーザーが azooKey の学習内容を **確認・削除・バックアップ・復元** できる
ようにする。透明性をユーザー体験として提供し、学習機能への信頼を担保
する。

## 2. 対象データ

| データ | 既定パス | 由来 M | 暗号化 |
|---|---|---|---|
| `learning.tsv` | `%LOCALAPPDATA%\azooKey\data\learning.tsv` | M7 | M34 で `.enc` |
| `user_dict.json` | `%LOCALAPPDATA%\azooKey\data\user_dict.json` | M9 | M34 で `.enc` |
| `typo_corrections.tsv` | `%LOCALAPPDATA%\azooKey\data\typo_corrections.tsv` | M35 / M55 | M34 で `.enc` |
| `auto_words.tsv` | `%LOCALAPPDATA%\azooKey\data\auto_words.tsv` | M36-A | M34 で `.enc` |
| `user_learning.db` | `%LOCALAPPDATA%\azooKey\data\user_learning.db`（将来 SQLite 化） | M54 | M34 で wrapper |

プロセス終了時にこれらのデータが保存される範囲は §11 で定める。

## 3. UI（設定アプリ 学習データタブ）

```
[学習データ]

タブ:
  [学習候補] [ユーザー辞書] [タイプミス補正] [新語候補] [バックアップ]

検索: [__________________]

読み        候補       重み   最終使用       タグ     操作
にほんご    日本語     4.2    2026-05-27    [学]     [忘却]
ぜんざい    Zenzai     3.1    2026-05-26    [辞]     [忘却]
こうしょう  交渉       2.8    2026-05-25    [典]     [忘却]
```

操作:

- **検索**: reading / surface の部分一致
- **忘却**: 個別エントリを weight = 0 で削除（or remove）
- **エクスポート**: 暗号化 ZIP 出力（§5）
- **インポート**: 別環境からの復元（§6）
- **全削除**: 当該タブのデータを全消去（DPAPI 鍵は維持）

## 4. IPC

設定アプリは学習データファイルを直接開かず、一覧、忘却、エクスポート、インポートを
すべて Host への IPC として発行する（writer 責務の正典は
`docs/windows-tsf-host-architecture.md`「共有ユーザーデータの writer 責務」）。
`user_dict.json` を含む本節の対象ストアはすべてこの経路に従う。

`MessageType` enum 末尾に append（M40 互換性）。新規 type 名は
`ListLearningEntries` / `ForgetLearningEntry` / `ExportLearningData` /
`ImportLearningData`。エンベロープは `ipc/src/Messages.cpp` の既存
wire format `{version, request_id, type, trace_id, payload}` に従い、
request と response は同一 `type` を共有して payload schema で区別する。

### 4.1 ListLearningEntries

Request（settings-app → host）:

```json
{
  "version": 1,
  "request_id": 100,
  "type": "ListLearningEntries",
  "trace_id": "018fd2c2-...",
  "payload": {
    "store": "learning",
    "query": "にほん",
    "limit": 100,
    "offset": 0
  }
}
```

`store` の取り得る値: `learning` / `user_dict` / `typo` / `auto_word`。

Response（host → settings-app）:

```json
{
  "version": 1,
  "request_id": 100,
  "type": "ListLearningEntries",
  "trace_id": "018fd2c2-...",
  "payload": {
    "total": 1234,
    "entries": [
      {
        "id": "abc",
        "reading": "にほんご",
        "surface": "日本語",
        "weight": 4.2,
        "last_updated_epoch_sec": 1780000000,
        "tags": ["learned"],
        "metadata": {}
      }
    ]
  }
}
```

ページネーション（`limit` / `offset`）必須。エントリ数が多くなる学習
ストアでも UI 応答を保つ。

### 4.2 ForgetLearningEntry

Request:

```json
{
  "version": 1,
  "request_id": 101,
  "type": "ForgetLearningEntry",
  "trace_id": "018fd2c2-...",
  "payload": {
    "store": "learning",
    "id": "abc"
  }
}
```

Response:

```json
{
  "version": 1,
  "request_id": 101,
  "type": "ForgetLearningEntry",
  "trace_id": "018fd2c2-...",
  "payload": {
    "removed": true
  }
}
```

該当エントリの weight を 0 化 + soft-delete。再観測時は新規エントリと
して扱い、過去履歴を引き継がない。

### 4.3 ExportLearningData

Request:

```json
{
  "version": 1,
  "request_id": 102,
  "type": "ExportLearningData",
  "trace_id": "018fd2c2-...",
  "payload": {
    "stores": ["learning", "user_dict", "typo", "auto_word"],
    "destination_path": "C:\\Users\\me\\Desktop\\azookey-backup.zip",
    "encrypt": true,
    "include_settings": false
  }
}
```

Response:

```json
{
  "version": 1,
  "request_id": 102,
  "type": "ExportLearningData",
  "trace_id": "018fd2c2-...",
  "payload": {
    "status": "success",
    "file_size_bytes": 12345,
    "manifest": {}
  }
}
```

`encrypt = true` のとき各ファイルを DPAPI で暗号化する。`encrypt =
false` は明示的な選択時のみ許可し、UI で大きな警告を表示する。

### 4.4 ImportLearningData

Request:

```json
{
  "version": 1,
  "request_id": 103,
  "type": "ImportLearningData",
  "trace_id": "018fd2c2-...",
  "payload": {
    "source_path": "...",
    "conflict_resolution": "merge",
    "stores": ["learning", "user_dict"]
  }
}
```

`conflict_resolution` の取り得る値: `merge` / `overwrite` / `keep_both`。

Response:

```json
{
  "version": 1,
  "request_id": 103,
  "type": "ImportLearningData",
  "trace_id": "018fd2c2-...",
  "payload": {
    "status": "success",
    "imported_counts": { "learning": 1234, "user_dict": 42 },
    "skipped_counts": { "learning": 5 },
    "conflicts": []
  }
}
```

## 5. バックアップ形式

```
azookey-backup-YYYYMMDD-HHMMSS.zip
├── manifest.json
├── learning.tsv.enc       (DPAPI 暗号化)
├── user_dict.json.enc
├── typo_corrections.tsv.enc
├── auto_words.tsv.enc
└── settings.redacted.json  (include_settings=true 時のみ)
```

### 5.1 manifest.json

```json
{
  "version": 1,
  "created_at": "2026-05-27T00:00:00+09:00",
  "app_version": "0.1.0",
  "host_version": "0.1.0",
  "encrypted": true,
  "encryption_method": "dpapi-user-scope",
  "items": [
    { "name": "learning", "file": "learning.tsv.enc",
      "count": 1234, "sha256": "..." },
    { "name": "user_dictionary", "file": "user_dict.json.enc",
      "count": 42, "sha256": "..." },
    { "name": "typo_corrections", "file": "typo_corrections.tsv.enc",
      "count": 89, "sha256": "..." },
    { "name": "auto_words", "file": "auto_words.tsv.enc",
      "count": 17, "sha256": "..." }
  ]
}
```

各 entry に SHA-256 を含めて整合性を検証する。

### 5.2 暗号化

DPAPI ユーザースコープ（`CryptProtectData`）で各ファイルを暗号化する
（M34 と同じ）。**他ユーザー / 他マシンでは復号できない**ことを設計の
前提とする。

明示エクスポート（平文）時のみ、ユーザーが自己責任で平文 ZIP を出力
できる。この場合 manifest の `encrypted = false` とし、設定アプリで
ユーザーに「秘密情報が平文で保存されます」と警告する。

## 6. インポートの衝突解決

| 衝突パターン | `merge` | `overwrite` | `keep_both` |
|---|---|---|---|
| 同 reading/surface の既存 weight あり | 加算 or 大きい方 | 上書き | 別 id で両方保持 |
| user_dict で同 reading/word | 既存維持 | 上書き | 両方保持 |
| auto_word で同 surface | count 加算 | 上書き | 両方保持 |
| 形式バージョン違い | migration 実行 | migration 実行 | migration 実行 |
| 暗号化データで復号失敗 | error 返却 | error 返却 | error 返却 |

`merge` を既定とする。UI で選択可能にする。

## 7. BackupArchive 実装

`inference-host/src/BackupArchive.cpp`（新規）:

```cpp
class BackupArchive {
public:
  Result Export(const ExportOptions& opts);
  Result Import(const ImportOptions& opts);
private:
  std::vector<uint8_t> EncryptDpapi(std::span<const uint8_t> plain);
  std::vector<uint8_t> DecryptDpapi(std::span<const uint8_t> encrypted);
  bool VerifySha256(std::span<const uint8_t> data, std::string_view expected);
};
```

ZIP 操作は標準ライブラリだけでは困難なため、`miniz`（header-only 互換）
を依存に追加する。M37 §3.2 の **オフライン既定** ビルド方針に従い、
配布形態は以下とする:

1. **vendored（既定）**: `third_party/miniz/` にソース 1〜2 ファイル
   （`miniz.h` / `miniz.c`）を **submodule または直接コピー**で取り込み、
   オフライン CI / 開発機でもネットワーク無しで configure / build が成功する
2. **opt-in `FetchContent`**: `-DAZOOKEY_FETCH_MINIZ=ON` でのみ
   `FetchContent_Declare(miniz ...)` を有効化（vendored を選べない CI 等の
   逃げ道として残す）。既定 OFF
3. **system package**: distro パッケージ（`apt install libminizip-dev` 等）
   は API 差異があるため、本 M49 範囲では vendored を優先する

CMake の探索順は vendored → opt-in fetch の 2 段。M37 の
configure offline ガードに違反しないことを CI で確認する。

## 8. プライバシー

- M46 secure mode 中はエクスポート / インポート操作を blocking する
  （M46 `PrivacyGate::CurrentMode() == Mode::Secure` または
  `!PrivacyGate::LearningAllowed()` のとき。`docs/privacy-and-secure-input-spec.md`
  §6 の API を使用し、本 spec で新規 API を追加しない）
- バックアップ ZIP には API キー / OpenAI 関連設定を含めない（`settings`
  含める場合は M44 §12.5 と同じ redaction）

## 9. テスト

- unit: `BackupArchive` の暗号化 / 復号 round-trip
- unit: manifest SHA-256 検証
- unit: 衝突解決 3 種
- integration: export → 別環境（別 DPAPI 鍵）で import 失敗
- integration: 同一ユーザーでの export → import で件数一致
- snapshot: manifest.json schema 固定

## 10. M49 受け入れ条件

- 学習データを UI から検索できる
- 個別忘却が次回候補順位に反映される
- 同一 Windows ユーザー / 同一マシン上で export → import の round-trip
  が件数一致で復元できる（DPAPI ユーザースコープの制約により他マシン /
  他ユーザーへの移行は本受け入れ範囲外。クロス環境復元が必要な場合は
  §5.2 の明示的な平文エクスポートを使う）
- 暗号化済みデータは他ユーザーで復号できない（DPAPI ユーザースコープ）
- M55 typo / M36-A auto_words の学習データも対象に含む（実装済みなら）
- M46 secure 中は export / import が blocking される

## 11. プロセス終了時の flush 保証境界

学習データは確定のたびに同期保存せず、未保存の観測が一定件数に達するか一定時間が経過した時点でまとめて書き出す（`docs/user-learning-enhancement-spec.md` §3.1.1）。
そのため、プロセスが終了する瞬間には未保存の観測が残りうる。
本節は、どの終了経路まで保存を保証するか、保証しない経路で最悪どれだけ失われるかを定める。
本節の対象は Host が書く `learning.tsv` であり、`user_dict.json` のように操作のたびに保存するストアは含まない。

### 11.1 終了経路の区分

終了経路を次の 3 区分で扱う。

- **保証**：未保存の観測を書き出してから終了する。失われる観測は 0 件。
- **best-effort**：終了処理へ到達できれば書き出すが、OS が与える猶予に依存するため成立を約束しない。失われる量は §11.3 の上限に従う。
- **対象外**：終了処理が走らない。失われる量は §11.3 の上限に従う。

| 終了経路 | 区分 |
|---|---|
| `FlushLearningStore()` の明示呼び出し、モデルロード境界 | 保証 |
| `SIGINT` / `SIGTERM`、`CTRL_C_EVENT` / `CTRL_BREAK_EVENT` | 保証 |
| stdio モードで stdin が EOF になったときの正常終了 | 保証 |
| pipe モードで停止要求を検出したあとの正常終了 | 保証 |
| コンソールウィンドウの `×`（`CTRL_CLOSE_EVENT`） | best-effort |
| ログオフとシステムシャットダウン（`CTRL_LOGOFF_EVENT` / `CTRL_SHUTDOWN_EVENT`、およびセッション終了に伴うプロセス終了） | 対象外 |
| `TerminateProcess` 相当（タスクマネージャの強制終了、外部からの kill、クラッシュ） | 対象外 |

`CTRL_CLOSE_EVENT` を保証に含めないのは、ハンドラへ与えられる猶予が有限で、しかも環境に依存するためである。
ConPTY を使う Windows Terminal では classic `conhost.exe` より短くなりうる。
Windows 11 build 22621 の実測では、classic `conhost.exe` の `×` で最終 flush が成立し、Windows Terminal の `×` では成立しなかった（DEV-791）。
`SetConsoleCtrlHandler` によるハンドラ登録は DEV-178 で入っており、登録漏れではない。
保証をこの経路の成立に賭けない。

ログオフとシャットダウンを対象外に置くのは、現行の実装がこれらのイベントで停止処理を行わないためである。
`inference-host/src/main.cpp` のコンソール制御ハンドラは `CTRL_C_EVENT` と `CTRL_BREAK_EVENT` と `CTRL_CLOSE_EVENT` だけを処理し、残りを既定ハンドラへ渡す。

製品構成の終了も対象外に入る。
`scripts/host-supervisor.ps1` は Host をウィンドウ非表示で起動し、停止要求を受けても稼働中の Host を落とさずに監督だけをやめる。
Host はログオンセッションの終了とともに終了する。
したがって製品構成で学習データを守るのは、終了時の書き出しではなく §11.3 の損失上限である。

### 11.2 失う量を減らす手段

現行の debounce では、静止した状態から 1 件だけ確定して即座に終了すると、その 1 件が丸ごと失われる。
終了経路をどれだけ網羅しても対象外の経路は救えないため、保存の契機を前へ倒す。

**burst 先頭の同期 flush** を導入する。

- 契機：未保存の観測が 0 件の状態で新しい観測を受けたとき。
- 条件：直近の保存成功から `learning_flush_interval_sec` 秒以上が経過していること。
- 動作：その観測を含めて `Save()` を同期実行し、`CommitObservation` の応答を返す前に永続化を終える。
- レート制限：直近の保存から `learning_flush_interval_sec` 秒未満なら即時保存せず、既存の件数契機と時間契機に委ねる。

レート制限を置かないと、保存のたびに未保存件数が 0 へ戻るため次の 1 件が常に burst 先頭になり、確定ごとの同期保存に退化する。
これは DEV-11 が性能上の理由で退けた形である。
レート制限のもとでは追加の書き込みは 1 つの burst につき最大 1 回であり、時間契機が同じ burst に対して行う書き込みと同じ回数に収まる。

判定には既存の `learning_flush_interval_sec` を使い、新しい設定キーを追加しない。
経過時間の判定は直近の保存成功時刻を steady clock で保持して行う。
起動直後は保存の実績が無いため、最初の観測を即時保存の対象とする。
DEV-791 が再現させた「Host を起動し 1 件だけ確定して閉じる」操作は、この規定によって終了経路によらず保存される。

`Save()` が失敗したときの扱いは変えない。
dirty と未保存件数を保ったまま stderr へ error を出し、次の観測または明示 flush で再試行する（`docs/user-learning-enhancement-spec.md` §3.1.1）。
即時保存の失敗を理由に `CommitObservation` を失敗させない。

### 11.3 失われうる最大量

`EngineConfig` の既定値は `learning_flush_every_n = 8` と `learning_flush_interval_sec = 5` である。
どちらも `settings.json` には露出せず、ビルド時の既定値として持つ。

この既定値のもとで、best-effort の経路と対象外の経路で失われうる観測は次の範囲に収まる。

- 件数：直近の保存以降に受けた観測のうち最大 7 件（8 件目で件数契機の保存が走るため）。
- 時間：未保存の観測列の先頭から最大 5 秒ぶん。
- 静止状態から再開した最初の 1 件は、§11.2 の即時保存により失われない。

ユーザーへ見せる表現としては「保存を保証しない終了では、直前 5 秒以内に連続して行った確定のうち最大 7 件ぶんの学習が失われうる」と書ける。

### 11.4 DEV-791 が実装する範囲

- §11.2 の burst 先頭の同期 flush を `InferenceEngine::NoteLearningMutationLocked` に実装する。設定キーは追加しない。
- `CTRL_CLOSE_EVENT` の経路は現行のまま best-effort として維持する。ConPTY で最終 flush が成立しないことは、本節のもとでは欠陥ではない。原因切り分けのための診断ログ追加は任意とする。
- `host_engine_tests` に次の 2 件を追加する。静止後の 1 件が明示 flush なしで永続化されること。`learning_flush_interval_sec` 未満の間隔で続く観測が即時保存を繰り返さないこと。

### 11.5 実機検証との対応

DEV-759（人間ゲート）の検証項目は本節の区分に従う。
保証の経路は保持を必須とし、best-effort と対象外の経路は §11.3 の上限内であれば未達として記録しない。
チェックリストの本体は `docs/handoff/human-gate-batch-runbook.md` に置く。
