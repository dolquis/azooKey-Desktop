# 開発基盤・品質強化 仕様（Development Infrastructure & Quality）

本書は azooKey-Desktop（Windows ポート）の**開発基盤・ビルド再現性・CI 品質
ゲート・IPC/JSON 堅牢性・観測性・Host 可用性**に関する仕様を定める。対象は
Windows 版（C++ 移植: `core/` `ipc/` `learning/` `inference-host/` `tsf-tip/`
`bench/`）のみ。macOS 版 `legacy/` は対象外。

対応マイルストーン: `plans/windows-port-roadmap.md` M37〜M43（開発基盤・品質
強化トラック） + M44/M47/M50/M51（同トラックの自然な延長）。

本書は 2 通の第三者レビュー（開発環境・ライブラリ改善レビュー / プロジェクト
レビュー 2026-05-22）の指摘を評価・取捨選択した結果を仕様化したものである。
レビュー指摘のうち既存マイルストーン（M8/M11/M12/M28〜M34）と重複するもの、
本プロジェクトの方針と整合しないものは「§11 不採用とした提案」に理由とともに
記録する。

## 1. 目的と非目標

### 目的

MVP（Phase 1〜2 完了）から「実運用に耐える IME」へ進むにあたり、Phase 3
（Zenzai 統合）/ Phase 4（配布）着手前に開発基盤・品質の負債を解消する。
具体的には次の 4 点をボトルネックとみなし、マイルストーン化する。

1. ビルド再現性（手元・CI・AI エージェントでビルド入口が統一されていない）
2. IPC/JSON の境界堅牢性（自前 JSON パーサと Named Pipe 入力の事故耐性）
3. 観測性（遅延要因の切り分け、構造化ログ、エラーコード体系の不在）
4. Host 可用性（プロセス停止時の TIP 側再接続戦略が限定的）

### 非目標（本トラックでやらないこと）

- Zenzai 本実装の完成（M8 の範囲）
- CUDA / DirectML / NPU の実装（M24 の範囲）
- WinUI 設定アプリの完成（M11/M30 の範囲）
- MSIX/MSI 配布の完成（M11/M12/M28〜M32 の範囲）
- 自前 JSON パーサの即時全置換（§6・§11 参照）

### 横断する設計原則

- **オフラインビルド原則を維持する。** `cmake -S . -B build` がネットワーク
  アクセスなしで成功すること。ネットワーク取得は明示オプトイン
  （`AZOOKEY_FETCH_GOOGLETEST=ON` 等）に限る。本トラックの依存追加は
  この原則を崩してはならない（§3）。
- **依存最小主義。** header-only ライブラリを優先し、ビルドが必要な依存は
  導入価値を個別に評価する。
- **段階導入。** 警告強化・新ライブラリ導入は一括ではなく、新規コードから
  段階的に適用し、既存コードの大規模差分を避ける。

## 2. ビルド再現性（M37）

### 2.1 CMakePresets.json

ルートに `CMakePresets.json`（schema version 6）を追加し、configure /
build / test の入口を統一する。最低限のプリセット:

| プリセット | generator | `CMAKE_BUILD_TYPE` | 用途 |
|---|---|---|---|
| `windows-debug` | Ninja | Debug | 手元・CI Debug |
| `windows-release` | Ninja | Release | 手元・CI Release |

各 configure preset の `cacheVariables` は `AZOOKEY_BUILD_TESTS=ON`,
`AZOOKEY_BUILD_BENCH=ON` を既定とする。`AZOOKEY_FETCH_GOOGLETEST` は
**プリセット既定で ON にしない**（オフラインビルド原則。CI ジョブ側で
明示的に上書きする）。`binaryDir` は `${sourceDir}/build/<preset>`。

`CMakeUserPresets.json` は各自のローカル設定用とし、`.gitignore` に追加する
（§2.4）。

受け入れ条件:

```
cmake --preset windows-debug && cmake --build --preset windows-debug && ctest --preset windows-debug
cmake --preset windows-release && cmake --build --preset windows-release && ctest --preset windows-release
```

### 2.2 共通オプション INTERFACE target

ルート `CMakeLists.txt` で 2 つの `INTERFACE` ライブラリを定義し、コンパイル
オプションを一元管理する。

- `azookey_project_options` — C++ 標準（`cxx_std_17`）と MSVC ハードニング
  系オプション（`/utf-8` `/permissive-` `/EHsc` `/Zc:__cplusplus` `/sdl`）。
  各実体 target が `PUBLIC` でリンク。
- `azookey_project_warnings` — 警告レベル（MSVC `/W4`、非 MSVC
  `-Wall -Wextra -Wpedantic`）。各実体 target が `PRIVATE` でリンク。

`/W4` は既存コードで警告が大量に出る場合、target 単位で段階導入する
（新規モジュールから適用し、既存は警告解消とセットで切替）。TSF DLL / Host
/ IPC の致命的警告（C4715 制御パス、C4477 書式不一致 等）は本マイルストーン
内で解消する。

### 2.3 .clang-format

ルートに `.clang-format` を追加する。初期値は `BasedOnStyle: Google`,
`Standard: c++17`, `ColumnLimit: 100`, `IndentWidth: 2`,
`PointerAlignment: Left`, `DerivePointerAlignment: false`,
`SortIncludes: true`。

既存コードに大規模差分が出る場合、まず全体整形を独立 PR として分離し、
機能変更 PR と混ぜない。CI の `clang-format --dry-run --Werror` は全体整形
PR がマージされた後に有効化する（§4.3）。

### 2.4 .gitignore 整理

`.gitignore` から macOS/Xcode 専用エントリ（`*.dmg` `*.pkg` `*.app`
`DerivedData/` `*.xcodeproj/*` 等）は `legacy/` 保全のため削除しないが、
Windows/CMake 向けエントリを追記する:

```
CMakeUserPresets.json
out/
install/
compile_commands.json
.cache/
.idea/
*.ilk
*.pdb
*.exp
*.obj
*.tlog
```

`.pdb` は Git 管理しないが CI artifact としては保存する（§4.4）。`.vscode/`
はチーム共有設定の余地を残し、一括 ignore はしない。

### M37 受け入れ条件

- `cmake --preset windows-debug` / `windows-release` の configure→build→test
  が成功する
- 既存全 target が `azookey_project_options` / `azookey_project_warnings`
  をリンクした状態でビルドできる
- `.clang-format` がルートに存在し、`clang-format --dry-run` が新規追加
  コードに対して差分ゼロ
- `.gitignore` に Windows/CMake エントリが追加され、ビルド生成物が
  `git status` に現れない

## 3. 依存管理方針（M37 / 横断）

### 3.1 基本方針

レビューは `vcpkg.json` の一括導入を提案するが、本プロジェクトは
**オフラインビルド原則**（§1）を持つため、vcpkg を必須化しない。依存ごとに
次の優先順で導入手段を選ぶ。

1. header-only ライブラリ → submodule または `FetchContent`（オプトイン）
2. ビルドが必要な依存 → 導入価値を個別評価し、必要なら `FetchContent` を
   明示オプトインで
3. 外部から取得する prebuilt バイナリ / モデル（llama.cpp ランタイム・GGUF 等）は
   **供給源を固定し SHA256 等でハッシュ pin** する。先行実装 fkunn1326/azooKey-Windows が
   CI で個人 gist / 個人 fork の成果物をチェックサム無しで取得しているのは**反面教師**とし、
   自分側は採らない（サプライチェーン耐性）。

### 3.2 依存ごとの導入手段

| 依存 | 種別 | 導入手段 | 状態 |
|---|---|---|---|
| GoogleTest | テスト | `find_package` →（オプトイン時）`FetchContent` | 導入済み（現行方式を維持） |
| llama.cpp | 推論 | `FetchContent` / submodule、`AZOOKEY_BACKEND` で制御 | M8 で導入（本トラック対象外） |
| WIL | header-only | submodule または `FetchContent`（オプトイン） | M43 で導入（§9） |
| nlohmann-json | header-only | 任意・保留 | §6・§11 参照（即時導入しない） |
| spdlog | 要ビルド | 任意・保留 | §7・§11 参照（自前ロガーを優先評価） |
| miniz | 要ビルド（C ソース 2 ファイル） | vendored（既定） →（オプトイン時）`FetchContent`（`-DAZOOKEY_FETCH_MINIZ=ON`） | M49 で導入予定（`docs/learning-data-management-spec.md` §7 の ZIP backup で使用） |

### 3.3 vcpkg.json の扱い

`vcpkg.json` は「vcpkg を使いたい開発者向けの利便ファイル」として将来追加
してよいが、**必須化しない**。追加する場合も CI の正系は preset ベースの
`FetchContent`/submodule 経路を維持し、README に「vcpkg は任意」と明記する。

## 4. CI 品質ゲート拡張（M38）

M38 着手前の `.github/workflows/windows.yml`（windows-latest + msvc-dev-cmd +
Ninja + Debug 単一構成、失敗時 PR コメント + test_report artifact）を基盤に、
以下を追加する。

### 4.1 Debug/Release マトリクス

```yaml
strategy:
  matrix:
    config: [Debug, Release]
```

runner は安定性のため `windows-2022` を明示する（`windows-latest` の暗黙
更新を避ける）。

### 4.2 preset 利用

CI の configure/build/test を §2.1 のプリセット経由に統一する。CI ジョブは
`AZOOKEY_FETCH_GOOGLETEST=ON` をジョブ側で明示指定する（プリセット既定は
OFF のまま）。

### 4.3 追加チェック

- `clang-format --dry-run --Werror`（§2.3 の全体整形 PR 後に有効化）
- Linux 補助ジョブ — `core` / `ipc` / `learning` / `inference-host` の
  非 Windows 依存部分のみをビルド・テストし、移植性回帰を早期検出する
- bench smoke — `azookey_bench` を CTest から exit=0 で実行（§4.5）
- `AZOOKEY_BUILD_TESTS=OFF` ビルドが壊れていないことの確認ジョブ

clang-tidy / CodeQL はオプション扱いとし、本マイルストーンの必須範囲には
含めない（導入コストが高く、段階導入とする）。

### 4.4 artifact 整理

configure / build / test の各ログと、Release ビルドの `.pdb` を artifact
として保存する。PR diagnostic コメントはマトリクスの config ごとの結果を
反映する。

### 4.5 bench smoke と回帰監視

`bench/azookey_bench` を CTest 登録し、CI で exit=0 と CPU `SimpleConverter`
経路の p95 < 50ms を確認する。p50/p95 レイテンシの推移監視（夜間ベンチ回帰）
は将来拡張とし、本マイルストーンでは smoke 実行までを範囲とする。

### M38 受け入れ条件

- Debug / Release 両構成が CI で緑
- CI が preset 経由で configure/build/test を実行する
- Linux 補助ジョブが非 Windows 部分のテストを実行する
- bench smoke が CTest 経由で実行され exit=0
- PR コメントが config ごとの結果に対応する

## 5. ユーザーデータ永続化の堅牢化（M39）

### 5.1 現状の問題

M39 着手前の `inference-host/src/main.cpp` は学習・辞書ファイルの既定パスを相対パス
（`azookey_learning.tsv` / `azookey_user_dict.json`）で持つため、Host の
起動元ディレクトリによって保存先が変わり、ユーザーデータが迷子になりやすい。

### 5.2 標準ディレクトリレイアウト

明示指定がない場合、`%LOCALAPPDATA%\azooKey\` 配下を既定とする:

```
%LOCALAPPDATA%\azooKey\
  config\   settings.json
  data\     learning.tsv / user_dict.json
  logs\     host-YYYYMMDD.jsonl / tip-YYYYMMDD.jsonl
  models\   zenzai\
  typo_corrections.tsv   ← M35（root 直下）
  auto_words.tsv         ← M36-A（root 直下）
```

注: `typo_corrections.tsv`（M35）と `auto_words.tsv`（M36-A）は歴史的経緯
により root 直下に配置されている。将来の独立 M で `data\` 配下へ統合する
follow-up を予定（既存ユーザーデータ migration を伴うため、本 spec の M39
範囲では維持する）。M49 backup（`docs/learning-data-management-spec.md` §2）
の対象範囲は本レイアウトを正典とし、root 直下の TSV も含む。

`%LOCALAPPDATA%` は `SHGetKnownFolderPath(FOLDERID_LocalAppData, ...)` で
取得する（WIL 導入後は `wil::unique_cotaskmem_string` で受ける）。必要な
サブディレクトリは起動時に自動作成する。

### 5.3 パス解決規約

- `--learning` / `--user-dict` が指定された場合は、従来どおり**明示パスを
  優先**する（既存テスト・CI・開発フローを壊さない）。
- 指定がない場合のみ `%LOCALAPPDATA%\azooKey\data\...` を使う。
- 保存先決定ロジックは純粋関数として切り出し、unit test で
  「明示指定優先 / 既定パス生成 / 環境変数欠落時の挙動」を検証する。

### 5.4 原子的書き込み

学習・辞書ファイルの保存は「一時ファイルへ書き込み → flush →
`MoveFileEx`（`MOVEFILE_REPLACE_EXISTING` + `MOVEFILE_WRITE_THROUGH`）で
rename」で原子的に行い、書き込み中クラッシュによる破損を防ぐ。

### 5.5 本マイルストーンの範囲と将来課題

本マイルストーンの範囲は **保存先統一・ディレクトリ自動作成・原子的書き込み・
パス解決の unit test** まで。次の項目は将来課題として扱い、ここでは仕様化
しない（対応マイルストーンを明記）:

- ファイルロック（単一 writer 保証） — M39 で実装可だが、Host は単一
  プロセス前提のため多重起動検出として軽量に扱う
- 破損時のバックアップ世代・起動時検証 — 将来課題
- DPAPI 暗号化 — **M34** の範囲（重複実装しない）

### M39 受け入れ条件

- 明示指定なしで `%LOCALAPPDATA%\azooKey\data\learning.tsv` /
  `user_dict.json` が使われる
- `--learning` / `--user-dict` 指定時は指定パスが優先される
- 必要なディレクトリが自動作成される
- 保存が原子的（書き込み中クラッシュで既存ファイルが壊れない）
- 保存先決定ロジックの unit test が追加され緑
- 既存テストが回帰しない

## 6. IPC/JSON 堅牢化（M40）

### 6.1 方針

レビューは自前 JSON パーサ（`ipc/src/Json.cpp`）の `nlohmann-json` 置換
（提案 A）と、自前を残す場合の fuzz/malformed テスト追加（提案 B）を挙げる。
本仕様は**提案 B を主**とする。即時の全置換はしない（理由は §11）。自前
パーサを残したまま境界堅牢性を上げ、`nlohmann-json` 置換は将来判断とする。

### 6.2 JSON パーサ強化要件

`ipc/src/Json.cpp` は次の防御を備える。各上限値は**本節（spec）を正典**とする
（AGENTS.md 正典マトリクス: IPC payload / JSON schema / 挙動の決定は
`docs/*-spec.md`）。`ipc/include/azookey/ipc/Limits.h` の定数は本節の値に
一致させ、齟齬が出た場合は spec を正として Limits.h を修正する。コード側の
上限変更は wire contract と受け入れ条件に影響するため、spec 更新（レビュー）と
セットで行うこと。定数と spec 値の同期は `static_assert` / テストで担保する
（code-only な変更で wire contract が無断で変わる drift を防ぐ）。

- **ネスト深度上限 = 64**（`kMaxJsonNestDepth`） — 配列/オブジェクトのネストが
  64 を超えたらパース失敗を返す（再帰によるスタック枯渇防止）。`ParseValue` は
  depth を持ち回り、`ParseObject` / `ParseArray` が超過を検査する。
- **最大入力長 = 1 MiB**（`kMaxJsonInputBytes = 1024 * 1024`） — `ParseDocument`
  冒頭で入力バイト長を検査し、超過は即失敗。§6.4.3 の最大フレームサイズと同値で
  揃える（フレームを読み切れた時点で入力長上限も満たされる）。
- **サロゲートペア結合** — `\uXXXX` の上位（U+D800–U+DBFF）に続く下位
  （U+DC00–U+DFFF）を結合し、単独サロゲート（上位のみ / 下位のみ）は不正として
  拒否する。
- **不正 UTF-8 / 制御文字の拒否** — 文字列内の生の制御文字（U+0000–U+001F）を
  拒否し、生 UTF-8 バイト列は長さ・継続バイト・overlong（C0/C1、E0<A0、F0<90）・
  範囲外（F4>8F、> U+10FFFF）を検査して不正シーケンスを拒否する。
- **末尾ゴミの拒否** — 値の後ろに空白以外が残る入力を拒否する
  （`ParseDocument` が `pos_ == size` を要求）。
- **数値の安全な扱い** — `0` 始まりの多桁・小数点後桁なし・指数部桁なし等の
  不正形を拒否し、`1e9999` 等は `std::isfinite` で弾く。
- **uint64 精度（plain 整数 token のみ保持・非 plain 形と直列化は残課題）** —
  **plain 整数 token**（符号なし数字のみ）の抽出（`GetInt` / `GetUInt`）は
  元トークン文字列から `std::stoll` / `std::stoull` で復元するため double 経由の
  精度欠落がなく、uint64 全域 `18446744073709551615` まで正しく取り出せ、桁あふれは
  `nullopt`。ただし**小数・指数形**（例 `18446744073709551616.0`）は
  `IsPlainIntegerToken` が false となり double 経路へフォールバックし、丸めに加え
  範囲ガードが不十分（`d > static_cast<double>(UINT64_MAX)` は `(double)UINT64_MAX`
  が 2^64 に丸まるため 2^64 を弾けず、範囲外 double を `nullopt` にせずキャスト
  し得る）。非 plain 形の桁あふれ拒否は残課題として overflow テスト / 修正を要する
  （DEV-188 / DEV-163）。一方
  **直列化**側は現状 `ipc/src/Messages.cpp` / `ipc/src/Payloads.cpp` が uint64
  フィールド（`request_id` / `Ping.nonce` / `t_ms` / `Cancel.target_request_id`
  / `CommitObservation.timestamp_ms` 等）を `static_cast<double>` してから
  `Value` 化するため、token を持たない `Value(double)` 経路に落ち、2^53 超の値が
  wire 上で丸まる。token 保持版 `Value(uint64_t)`（`std::to_string` 済み）または
  `std::to_chars` 経由へ改め、**parse / serialize の双方向**で uint64 全域を
  保証すること（残課題）。
- **数値 codec の locale 非依存（残課題）** — 数値の parse / stringify は
  C ロケール固定で行い、ホストプロセス（TIP は任意アプリ内 in-proc、Host も
  CRT locale を変える可能性）が非 C ロケール（小数点が `,` 等）を設定しても
  wire 表現が壊れないこと。現状 `std::stod` / `std::ostringstream` は実行時
  locale 依存のため、`std::from_chars` / `std::to_chars`（または明示 `C`
  ロケール）への置換を要する。

  上記の残課題（直列化側 uint64 精度・非 plain 数値形の桁あふれ拒否・locale
  非依存）は numeric codec correctness として Linear DEV-163 で一括追跡し、
  該当 uint64 フィールドの round-trip テストと非 plain 形 overflow テストの追加
  までを完了条件とする（パーサ直接単体テストの拡充は DEV-188）。

### 6.3 追加テストと協定外メッセージの扱い

`ipc/tests/`（`messages_test.cpp` の `JsonTest` スイートほか）に境界・malformed
テストを置く。v1.0 の堅牢性バーは「**決定的な境界コーパス** + **有界な擬似乱数
スモーク**」で満たす。libFuzzer ベースの継続 fuzz ハーネスはオフラインビルド
原則（§1）・MSVC 主体の CI と相性が悪いため**任意の将来拡張**とし、必須には
しない。

- 深すぎるネスト・最大長超過を拒否する（`kMaxJsonNestDepth + 1` /
  `kMaxJsonInputBytes + 1`）
- 末尾ゴミ・`0` 始まり・指数部欠落を拒否する
- 巨大数（`1e9999`）・uint64 桁あふれ（`...616`）を安全に拒否する
- 不正 Unicode escape（単独サロゲート `\uD800` / `\uDC00`）を拒否する
- 文字列内の生制御文字（`\x01`）・overlong UTF-8（`C0 AF`、`E0 80 80`）を拒否する
- 擬似乱数バイト列を `Parse` してもクラッシュせず失敗を返す（有界回数スモーク）
- 最大フレーム超過の length-prefix を `Decode/EncodeLengthPrefixed` が拒否する
- `Payloads.cpp` 側で必須キー欠損・型不一致を安全に拒否する
- 自前 JSON パーサの直接単体テスト（int64/uint64 精度・深度・unicode escape・
  round-trip）を独立ファイルへ拡充する（残課題、Linear DEV-188）

**enum 予約のみで未配線の MessageType**（`QueryPredictions` / `QueryCorrections` /
`CommitCorrection` / `UpdateUserWord`。`docs/windows-tsf-host-architecture.md` の
⚠️ 項）を受信した際、Dispatcher は黙って無視せず**明示的なエラー応答**を返す。
応答待ちの blocking client をハングさせないため、既存ワイヤ形式の envelope に
「未対応 type」を表すエラー payload（例: `{"ok":false,"error":"unsupported_message_type"}`）
を載せて返す。`Cancel` など fire-and-forget の type のみ無応答を許す。現状の
Dispatcher は `default:` で `nullopt` を返し client をハングさせ得るため要修正
（残課題、Linear DEV-162）。併せて MessageType 列挙 ↔ `Payloads` codec の網羅
整合をテスト / CI で検査し、列挙追加時の codec 取りこぼしを防ぐ。

### 6.4 Named Pipe セキュリティ強化

現行 `ipc/src/NamedPipeTransport.cpp`（per-user pipe 名 + DACL +
length-prefix フレーミング + `kMaxFrameSize`）を基盤に強化する:

- **6.4.1 Release で fail-closed** — SID 取得失敗時、現在は fallback pipe
  名 / 既定セキュリティに落ちる。Debug/test ではこの fallback を許可し、
  Release/production では SID 取得失敗時に Host 起動を失敗させる
  （ビルド構成で分岐）。Debug/test の restricted-token 実行環境では current-user
  SID ACE だけでは同一プロセス client の write open が拒否されるため、Release 以外に限り
  restricted-token 互換 ACE を追加して transport tests を実行可能にする
- **6.4.1a remote client rejection** — Named Pipe はローカル IME ↔ Host 専用
  であり、server 作成時に `PIPE_REJECT_REMOTE_CLIENTS` を指定して remote client
  接続を OS レベルで拒否する。これは DACL / per-user pipe 名の補助防御であり、
  本項は remote client 拒否に限る。**同一ユーザー内の別プロセスを秘密だけで
  区別する認証は不可能**であり（§6.4.4）、Handshake トークンは同一ユーザー
  なりすまし対策の認証ではなく多層防御として §6.4.4 で扱う
- **6.4.2 接続インスタンス上限** — `PIPE_UNLIMITED_INSTANCES` を使わず、
  TIP と設定 UI などの同時接続を許容する有界な上限
  (`kMaxPipeInstances = 32`) を設ける
- **6.4.3 最大フレームサイズ = 1 MiB** — `kMaxFrameSize`（`= kMaxJsonInputBytes`、
  `Limits.h`）。4-byte little-endian length-prefix の値が 0 または本上限超過の
  フレームは `ReadEnvelope` / `DecodeLengthPrefixed` が拒否する。候補問い合わせ
  （数 KB）には十分で、長文一括変換（5,000 文字 ≒ 15 KB）も収まる。5 万文字級の
  超長文は分割前提（M58-B、`docs/romaji-batch-conversion-spec.md` と整合）。
  §6.2 の JSON 最大入力長と**同値に固定**し、片方だけ広げない
- **6.4.4 Handshake トークン** — Handshake で共有秘密トークンを検証する
  （`HandshakeRequest.handshake_token` ↔ Host の
  `DispatcherConfig.handshake_token`）。トークンが設定されている場合、Host は
  Handshake 成立まで他メッセージを未認証として type 別エラーで弾く
  （`Dispatcher::RequiresAuthenticatedSession`）。

  **脅威モデルと到達限界（重要）**: 本トークンは**多層防御（defense-in-depth）**で
  あり、**悪意ある同一ユーザープロセスからのなりすましを防ぐ認証ではない**。
  TIP は TSF DLL として任意のアプリプロセスに in-proc ロードされるため、Host と
  「正規の TIP プロセス」を結ぶ事前の信頼チャネルが存在しない。共有秘密を
  どのチャネルで配っても、同一ユーザー権限で動く悪意あるプロセスは正規 TIP と
  同じ経路でその秘密を入手でき（ファイルなら読み取り、env なら環境継承）、同一
  ユーザー内のプロセス同士を秘密だけで区別することは Windows のセキュリティ
  モデル上できない。**同一ユーザー境界の主防御は per-user pipe ACL + SID
  fail-closed（§6.4.1）+ remote 拒否（§6.4.1a）**であり、「どのプロセスが正規 TIP か」
  の信頼境界は TSF 自体が担う。トークンに認証を期待しない。

  **トークンが実際に与える価値**:
  - 非悪意の誤接続（バージョン違い / 別ビルドの Host・TIP / 残存プロセス）を弾く
  - トークン格納先（`%LOCALAPPDATA%`）を**読めない**同一ユーザープロセス、
    具体的には **capability 分離された AppContainer プロセス**（ユーザー通常 SID で
    `%LOCALAPPDATA%` を読めず、パッケージ SID / capability で隔離。DEV-204 関連）を
    結果的に排除する。**低 IL（low integrity）プロセスは排除できない** — Windows の
    整合性レベルは通常 write-up のみ制限し read-up は許すため、低 IL でも同一ユーザー
    DACL のトークンファイルを読めてしまう。この価値は AppContainer 等の capability
    隔離ケースに限る
  - 攻撃の手数を一段増やす speed bump

  **トークンの配布チャネル（v1.0 決定）**: env 事前共有（`AZOOKEY_IPC_HANDSHAKE_TOKEN`）
  は in-proc TIP が Host 起動時の環境を共有しないため production では成立しない。
  そこで Host が起動時に暗号論的乱数 16 byte（hex 32 文字）を生成し、現在ユーザー
  のみが読める `%LOCALAPPDATA%\azooKey\config\ipc-token`（NTFS ACL 継承で
  current-user RX）へ §5.4 の write-then-rename で原子的に書き出す。TIP は Handshake
  直前に同ファイルを読みトークンを得る。Host 再起動時はファイルを新トークンで
  上書きし、TIP は**再 Handshake のたびにファイルを読み直す**（M42 再接続時も同様）。
  環境変数 `AZOOKEY_IPC_HANDSHAKE_TOKEN` は開発・テスト用の上書き経路として残す。
  本チャネルは上記の defense-in-depth 価値に見合うものであり、認証の保証は与えない。

  **トークン未設定時の縮退**: ファイルも環境変数も無い場合は per-user pipe ACL
  のみに依拠し、Host は warn ログを出す（現行 `inference-host/src/main.cpp` の
  挙動）。Release では §6.4.1 の SID fail-closed により pipe 自体が current-user
  に限定されるため、トークン未設定でも remote / 別ユーザーは到達しない
- **6.4.5 client cleanup** — 切断済み client が Stop まで保持される現状を
  見直し、切断検出時に解放する（長時間稼働でのリーク様の蓄積を防ぐ）
- **6.4.6 length-prefix read/write hardening** — Named Pipe は
  `PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT` を使う。server は
  `FILE_FLAG_OVERLAPPED` 付きで instance を作成し、`ConnectNamedPipe` の
  pending accept を `OVERLAPPED` event で待つ。`Stop()` 時は accept / client I/O
  を cancel して、listen 中の thread が無期限に残らないようにする。4-byte
  little-endian length-prefix によってフレーム境界を復元し、`ERROR_MORE_DATA`
  を扱って指定長まで読み切る。フレーム途中の一時的な `ERROR_NO_DATA` は bounded
  retry に留め、フレーム開始前の no-data / zero-byte read は切断扱いにする。
  write 側も `ERROR_PIPE_BUSY` / zero-byte write を bounded retry に留め、
  読まない peer による client thread 滞留を防ぐため blocking flush に依存しない。
  64KiB を超えるフレームが pipe write 単位で分割されても往復できる。

### M40 受け入れ条件

- 既存 `ipc_payloads_tests` / `ipc_named_pipe_transport_tests` が緑
- malformed JSON・ランダムバイト列でクラッシュしない
- ネスト深度・最大長超過を拒否する
- サロゲートペアを正しく結合し、単独サロゲートを拒否する
- Release ビルドで SID 取得失敗時に Host 起動が失敗する
- 複数接続・切断テストが追加され緑
- 切断済み client が解放される
- 64KiB を超える length-prefix フレームが往復できる
- 未配線 MessageType に明示エラー応答が返り、blocking client がハングしない。
  MessageType 列挙 ↔ `Payloads` codec の網羅整合が検査される（DEV-162）
- 数値の parse / stringify が locale 非依存（C ロケール固定）で round-trip する（DEV-163）
- uint64 フィールド（`request_id` / `Ping` / `Cancel.target_request_id` /
  `CommitObservation.timestamp_ms`）が 2^53 超でも丸めず全域 round-trip する（DEV-163）
- 非 plain 数値形の桁あふれ（例 `18446744073709551616.0`）を `nullopt` で拒否する（DEV-163 / DEV-188）
- Handshake トークンが per-user ファイルチャネルで配布され、未設定時は
  ACL 縮退 + warn ログとなる（§6.4.4）

> 注: 本節は受け入れ条件の「定義」のみを持ち、各項目の達成状態は持たない
> （正典は Linear）。協定外メッセージ（DEV-162）・数値 codec の locale 非依存
> （DEV-163）・直接パーサ単体テスト拡充（DEV-188）・トークン配布チャネル
> （§6.4.4）の進捗は対応する Linear 課題で追跡する。

## 7. 構造化ログと可観測性（M41）

### 7.1 現状

TIP は `OutputDebugStringA`（DebugView）、Host は stderr 中心。遅延要因
（TIP 側 / Pipe 側 / Host 側）の切り分けと、エラー分類の体系がない。

### 7.2 構造化ログ（JSON Lines）

TIP / Host とも JSON Lines 形式のログを出力する。出力先は §5.2 の
`%LOCALAPPDATA%\azooKey\logs\host-YYYYMMDD.jsonl` /
`tip-YYYYMMDD.jsonl`。

各行に最低限含める必須フィールド:

| フィールド | 内容 |
|---|---|
| `ts` | ISO 8601 タイムスタンプ |
| `level` | `info` / `warn` / `error` |
| `component` | `tip` / `host` / `ipc` |
| `request_id` | IPC リクエスト相関 ID（該当時。§7.3） |
| `trace_id` | ユーザー 1 アクション相関 ID（該当時。§7.3 / §7.7.1） |
| `phase` | 処理フェーズ。正典 enum は `core/include/azookey/logging/Phase.h`（§7.3 / §7.7.2） |
| `latency_ms` | 当該フェーズの所要時間 ms（計測時）。本フィールド名を全コンポーネント共通の正典とする |
| `result` | `ok` / `error` / `cancelled` / `blocked`（secure 抑止時。`docs/privacy-and-secure-input-spec.md` §5.1） |
| `error_code` | §7.4 のエラーコード（error 時） |

`phase` の取り得る値は §7.7.2 の 11 値に固定し、`core/include/azookey/logging/Phase.h`
の `azookey::logging::Phase` を全コンポーネント（tip / ipc / host）の正典とする。
所要時間フィールドは `latency_ms` を正典名とし、`duration_ms` 等の別名は使わない。
phase 別の絶対オフセット（key_down=0 起点）が必要な場合のみ任意で `t_ms` を併記する。

記録対象イベント: Host 起動・終了、pipe listen 開始、model load 成否、
backend 選択、query latency、error、exception summary、learning/user-dict
の load/save 結果。

実装は自前の軽量 JSON Lines ロガーを第一候補とする（依存最小主義）。
`spdlog` 採用はビルド依存の増加に見合うかを評価したうえで判断し、本仕様の
必須範囲には含めない（§11）。

### 7.3 相関 ID とフェーズ

相関 ID は **2 軸のみ** とし、3 つ目の概念（`correlation_id` 等）は導入しない。

| ID | 型 | 粒度 | 発番元 |
|---|---|---|---|
| `request_id` | uint64（≥1, allocator ごとに単調増加） | IPC リクエスト 1 往復 | **TIP（client）採番**。`QueryCandidates`/staleness は `ipc_pending_id_`、fire-and-forget（commit / cancel 等）は接続ローカル連番（§7.3 注記）。Host は `req.request_id` を echo（`Dispatcher::MakeResponse`） |
| `trace_id` | string（UUIDv7 推奨） | ユーザー 1 アクション（キー押下 → UI 更新） | TIP が `OnKeyDown` で発番（§7.7.1） |

- 横断追跡のキーは **`(trace_id, request_id)` の組** とする。`trace_id` が
  1 アクション内の複数 IPC を束ね、`request_id` がその中の個々の往復を識別する。
- 両 ID は既存 IPC エンベロープ `{version, request_id, type, trace_id, payload}`
  にそのまま乗る（§12.6 / `docs/privacy-and-secure-input-spec.md` §9）。新フィールドは追加しない。
- `request_id` は **すべて TIP（client）側で採番**し、Host は応答で echo するのみ。
  TIP には 2 系統の allocator があり、実装はこの分担を維持する（新たに統合しない）:
  - `ipc_pending_id_`（TIP メンバ）— `QueryCandidates` の応答相関と staleness 判定
    （M10、`req_id == ipc_pending_id_`）・`Cancel.target_request_id` に用いる「最新リクエスト」id。
  - 接続ローカル連番（`next_id`、Handshake=1 の後 2 から開始）— 応答相関を要しない
    fire-and-forget envelope（`CommitObservation` 等）の wire id。
  どちらも `tsf-tip/src/TextService.cpp`。ログ相関では `(trace_id, request_id)` に加え
  MessageType で系統を区別する。
- Host の `RequestScheduler`（`inference-host/src/RequestScheduler.cpp`）は wire
  `request_id` を **発番しない**。TIP 由来の id をキーに cancellation / latest 追跡を
  行う側であり、`NextRequestId()` は wire ID の採番元ではない。

フェーズは §7.7.2 の **11 値に固定** し、正典 enum は
`core/include/azookey/logging/Phase.h`（`azookey::logging::Phase`）とする。
M41 では粗い部分集合（例: `romaji_convert` / `model_inference` / `total`）のみ記録し、
M51 で全 11 phase を記録する。phase 体系は 1 つだけで、M41 用と M51 用の別系統は持たない。
各フェーズの所要時間は §7.2 の `latency_ms` フィールドに記録して遅延要因を切り分ける。

### 7.4 エラーコード体系

エラーを 3 カテゴリの enum で固定する:

- `transport` — pipe 切断、接続失敗、フレーミング異常
- `protocol` — JSON パース失敗、未知 MessageType、payload 不整合
- `business` — モデル未ロード、変換失敗、辞書 I/O 失敗

### 7.5 タイムアウト規約

変換候補問い合わせのタイムアウトを定義する（例: ソフト 150ms / ハード
300ms）。ソフト超過はログに記録、ハード超過は当該リクエストを打ち切り
劣化モード（§8.3）へ移行する。

### 7.6 プライバシー配慮（redaction ポリシー正典）

IME である以上、入力本文・候補語をそのままログに出すとプライバシー
リスクがある。本文系フィールド（`reading` / `surface` / `candidate.text` /
確定文字列 / Magic Conversion prompt / typo の `raw_keys` 等）の出力可否は、
以下の **優先順位** で 1 つに定める。上位が下位を常に上書きする。

| 優先 | 条件 | 本文系フィールドの扱い |
|---|---|---|
| 1（最優先） | secure 中（`PrivacyGate::IsSecure()==true`） | **常に redact**。Debug でも `AZOOKEY_LOG_BODY=1` でも出力しない |
| 2 | Release ビルド（既定） | **常に redact**。`request_id` / `trace_id` / 長さ / `result` / `latency_ms` 等のメタ情報のみ |
| 3 | Debug ビルド かつ `AZOOKEY_LOG_BODY=1` | 本文を出力（opt-in。開発時のみ） |
| 4 | Debug ビルド（既定、env 未設定） | redact（メタ情報のみ） |

- redact 時は値を `***redacted***` に置換し、`window_title` は `window_title_hash`
  のみに置換する（`docs/privacy-and-secure-input-spec.md` §8 と同一規約）。
- 本ポリシーの実装は M44 診断 ZIP（§12.5）と secure redaction（同 §5 / §8）で
  **共通の redaction 関数** を用い、二重定義・不整合を作らない。
- レイテンシ trace（§7.7）は本文を含まないメタ情報であり、本ポリシーの
  redact 対象外（phase 別 `latency_ms` は Release でも記録してよい）。
- 互換性テスト（§13）でも、Release 既定で本文がログ・成果物に残らないことを
  確認対象に含める。

### M41 受け入れ条件

- TIP / Host が JSON Lines ログを `%LOCALAPPDATA%\azooKey\logs\` に出力
- 各行に `request_id` / `phase` / `latency_ms` / `result` が含まれる
- エラーコードが 3 カテゴリ enum で固定される
- タイムアウト規約がコードとログに反映される
- Release ビルドで入力本文・候補語がログに出力されない

### 7.7 M51 拡張: レイテンシ内訳トレーサ

M41 の構造化ログ基盤を発展させ、キー押下から候補表示までの全 phase を
1 リクエスト単位で追跡可能にする。Zenzai 最適化（M24 / M25 / M57）・
Tiny Reranker（M56）・ModernBERT スコアリング（M57）の効果測定の前提と
なる。

#### 7.7.1 trace_id

全 IPC envelope に `trace_id`（UUIDv7 推奨）を追加する。TIP 側で
`OnKeyDown` のタイミングで生成し、対応する全 IPC 往復に付与する。
既存 `request_id` は IPC リクエスト単位のままとし、`trace_id` は
「ユーザーの 1 アクション（キー押下 → UI 更新）」単位で複数 IPC を
束ねる。

```json
{
  "type": "QueryCandidates",
  "request_id": 123,
  "trace_id": "018fd2c2-2a3e-7c9a-b8e1-7f3a92d4c5e2"
}
```

#### 7.7.2 計測フェーズ

| フェーズ | 説明 | 計測コンポーネント |
|---|---|---|
| `key_down` | TIP がキーを受け取った時刻 | tsf-tip |
| `romaji_convert` | ローマ字かな変換 | core / tsf-tip |
| `ipc_serialize` | payload 生成（JSON 化） | ipc / tsf-tip |
| `pipe_send` | Named Pipe 送信 | ipc |
| `host_queue_wait` | Host scheduler 待ち | inference-host |
| `model_inference` | SimpleConverter / Zenzai 推論 | inference-host |
| `rerank` | 学習 / user dict / tag boost / Tiny / BERT | inference-host |
| `pipe_recv` | 応答受信 | ipc / tsf-tip |
| `staleness_check` | request_id 確認 | tsf-tip |
| `ui_apply` | CandidateWindow / Preedit 更新 | tsf-tip |
| `total` | key_down → ui_apply の通算 | tsf-tip |

この 11 値が phase の正典であり、`core/include/azookey/logging/Phase.h` の
`azookey::logging::Phase` と 1:1 で対応する（wire 名は `PhaseName()`）。
値の **追加は後方互換（patch）**、**削除 / 改名は破壊的変更（major）** とする
（読み手は未知 phase を無視。集計ツールは wire 名に依存するため改名不可）。

各 phase は §7.2 の構造化ログ行として記録する（既存 schema 互換）。所要時間は
`latency_ms`（正典名）に入れ、`key_down` 等の絶対オフセット（key_down=0 起点）が
必要なときのみ任意で `t_ms` を併記する。
`model_inference` 行に以下を付与する（M24 `docs/copilot-pc-backend-spec.md` §4 整合）:

- `engine`: `"llama_cpp" | "winml"`
- `backend`: R1 アクセラレータ `cpu` / `cuda` / `vulkan`（旧 `directml` / `npu` は
  非推奨。R2 では `winml`）
- R2(`winml`) 時のみ `ep`（選択 EP 名。例 `QNNExecutionProvider`）と `ep_state`
  （`NotPresent` / `NotReady` / `Ready` / `Registered` / `Failed`、§4.6）。EP 取得・登録
  失敗をトレースで切り分け可能にする。

```json
{"ts":"2026-05-27T10:00:00.000Z","trace_id":"abc","component":"tip","phase":"key_down","t_ms":0.0,"level":"info","result":"ok"}
{"ts":"2026-05-27T10:00:00.001Z","trace_id":"abc","component":"core","phase":"romaji_convert","latency_ms":0.04,"level":"info","result":"ok"}
{"ts":"2026-05-27T10:00:00.015Z","trace_id":"abc","component":"host","phase":"model_inference","latency_ms":14.5,"backend":"cuda","level":"info","result":"ok"}
{"ts":"2026-05-27T10:00:00.018Z","trace_id":"abc","component":"tip","phase":"total","latency_ms":18.3,"level":"info","result":"ok"}
```

#### 7.7.3 trace_viewer CLI

`bench/azookey_trace_viewer.cpp` を新設し、JSONL ログを集計する:

```powershell
azookey_bench.exe --trace --backend cuda --model zenzai-small.gguf
azookey_trace_viewer.exe trace.jsonl --summary
```

summary 出力例:

```
QueryCandidates latency summary (N=10000)
  total_p50: 18.3 ms
  total_p95: 42.1 ms
  total_p99: 78.4 ms
  model_inference_p95: 30.4 ms
  pipe_p95: 1.2 ms
  ui_apply_p95: 3.8 ms
```

#### 7.7.4 オーバーヘッド制御

通常利用時は §7.6 のプライバシー方針に従い、Release では phase 別
`latency_ms`（メタ情報のみ。本文は含まない）を記録する。詳細な per-key
trace は以下のいずれかで明示有効化する:

- `bench/azookey_bench --trace`（ベンチ実行時）
- `settings.latencyTracing.enabled = true`（設定 GUI から）
- 環境変数 `AZOOKEY_TRACE=1`（開発時）

サンプリングレート（`latencyTracing.sampleRate`、既定 0.01）で本番でも
低コストで取得可能にする。

### M51 受け入れ条件

- 全 IPC envelope に `trace_id` フィールドが追加され、JSON parse 後
  伝播する
- 1 リクエスト単位で TIP / IPC / Host / UI の各 phase 時間を JSONL に
  出力できる
- `azookey_trace_viewer --summary` で p50 / p95 / p99 を出力できる
- Zenzai backend 比較（R1: cpu / cuda / vulkan、R2: winml〔EP 自動選択〕）に使える
- 通常利用時（trace 無効）の追加 overhead が p95 で +1ms 未満

## 8. Host 可用性・再接続（M42）

### 8.1 現状の問題

Host が停止した際の TIP 側再接続戦略が限定的で、入力中に候補更新が停止する
恐れがある。常駐方法（Run キー）だけではアップデート時・クラッシュ時の復旧
が弱い。

### 8.2 接続状態機械

TIP 側 IPC ワーカー（`tsf-tip/src/TextService.cpp::IpcWorkerThread`）に
明示的な接続状態機械を導入する:

```
Disconnected ─→ Connecting ─→ Handshaking ─→ Ready
     ↑              │              │           │
     └──────────────┴──────────────┴───────────┘ (失敗・切断時)
Ready ─→ Degraded （ハードタイムアウト / 連続失敗時）
```

各状態遷移はログ（§7）に記録する。

### 8.3 再接続と劣化モード

- **Exponential backoff + jitter** で再接続を試行する（例:
  200ms→400ms→800ms→…→最大 5s、各値に jitter を加算）。
- Host から一定時間応答がない場合（§7.5 のハードタイムアウト、または
  ヘルス監視の無応答）、TIP は `Degraded` 状態へ移行し、`SimpleConverter`
  相当のローカルフォールバックで入力継続を保証する。Host 復帰後は
  `Ready` へ戻す。
- 「Host から一定時間応答がない場合」には、pipe 接続自体は維持されているが
  `QueryCandidates` / `Health` の有効応答が deadline 内に返らない
  connected-but-silent 状態を含める。TIP は pipe 切断を待たず、処理種別ごとの
  deadline で劣化判定する。
- ヘルス監視は既存 `Health` メッセージを流用し、定期的に往復確認する。

### 8.4 本マイルストーンの範囲

接続状態機械・backoff 再接続・ヘルス監視・劣化フォールバックまでを範囲と
する。Host の Windows サービス化や監視ランチャ導入は将来課題とし、本仕様
では扱わない。

### M42 受け入れ条件

- TIP 側 IPC ワーカーが接続状態機械に基づいて動作する
- Host 停止 → 再起動で TIP が exponential backoff + jitter により自動
  再接続する
- Host 無応答時に TIP が劣化モードへ移行し、入力が止まらない
- 状態遷移がログに記録される
- Host 復帰後に `Ready` へ復帰する

### 8.5 M47 拡張: ユーザー可視復旧 UX

M42 は IPC transport 層の状態機械と劣化フォールバックまでを範囲とした。
M47 はその上に乗る**ユーザー可視 UX レイヤ**を扱う。Zenzai モデル単位の
劣化（モデル未配置 / ロード失敗 / 推論 timeout）と、連続クラッシュ時の
SafeMode を導入する。

#### 8.5.1 拡張状態機械

M42 の `Ready` / `Degraded` を更に細分化する:

```
Healthy
  ↓ Host no response (M42 transport)
DegradedSimple   ← SimpleConverter で継続
  ↓ reconnect success
Recovering
  ↓ health ok
Healthy

Healthy
  ↓ model load failed / inference timeout
DegradedModel    ← Host は健康だが Zenzai 無効、辞書 + 学習 + Simple
  ↓ reload success
Healthy

Any
  ↓ repeated crash (Host process が N 回連続クラッシュ)
SafeMode         ← AI / 学習 / 外部 API を全停止、最小限の入力のみ
```

| 状態 | 説明 | ユーザー影響 |
|---|---|---|
| `Healthy` | Host + Zenzai 正常 | 通常 |
| `DegradedSimple` | Host またはモデル不調。SimpleConverter で継続 | 変換品質低下 |
| `DegradedModel` | Host は正常だが Zenzai 無効 | 変換品質低下 |
| `Recovering` | 再接続 / 再ロード中 | 一時的に候補更新遅延 |
| `SafeMode` | 連続クラッシュにより AI / 学習を停止 | 安定優先 |

遷移トリガと駆動 timeout（§8.5.2）を一覧化する。`request_id` / `trace_id`
（§7.3）で staleness を判定し、状態遷移は §7 のログに記録する。

| From | To | トリガ | 駆動 timeout / 閾値 |
|---|---|---|---|
| `Healthy` | `DegradedSimple` | Host 無応答（pipe 切断 or connected-but-silent） | Ping 500ms / QueryCandidates fast 150ms / Live 80ms / Heavy 800ms 超過 |
| `Healthy` | `DegradedModel` | Zenzai モデル load 失敗 or 推論 timeout | Model load 30s / 推論 deadline 超過 |
| `DegradedSimple` | `Recovering` | 再接続成功（pipe 再確立 + Handshake） | exponential backoff（§8.3） |
| `DegradedModel` | `Recovering` | `LoadModel` 再ロード受理 | Model load 30s |
| `Recovering` | `Healthy` | health OK（Ping 往復成功） | Ping 500ms 以内 |
| `Recovering` | `DegradedSimple` / `DegradedModel` | 再接続 / 再ロード失敗 | 同上 timeout 再超過 |
| `Any` | `SafeMode` | Host プロセスが 60s 以内に 3 回連続クラッシュ（§8.5.3） | crash カウンタ ≥3 / 60s |
| `SafeMode` | `Healthy` | ユーザーが手動解除（`settings.safeMode.enabled=false`） | 手動のみ（自動復帰しない） |

#### 8.5.2 timeout 表

M42 §7.5 のソフト/ハードを処理種別ごとに具体化する:

| 処理 | timeout |
|---|---:|
| IPC Ping | 500ms |
| QueryCandidates fast | 150ms |
| QueryLiveConversion | 80ms |
| Heavy inference（Magic Conversion 等） | 800ms |
| ModernBERT scoring（M57） | 30〜50ms |
| Model load | 30s |

timeout は request 送信または backend 処理開始からの wall-clock deadline として扱う。
connected-but-silent Host でも、pipe 切断や blocking read の解除を待たない。

timeout 時は Cancel を送信し、古い結果は staleness check（M10）で
破棄する。`request_id` と `trace_id`（M51）で staleness を判定する。
Cancel / deadline は Dispatcher の応答抑止だけでなく、converter / reranker /
backend 推論処理まで伝播させる。重い処理が deadline 後も継続し、IME 側の
fallback や次リクエスト処理を妨げる設計は不可とする。

#### 8.5.3 SafeMode 突入条件

直近 60 秒以内に Host プロセスが 3 回連続でクラッシュした場合、TIP は
`SafeMode` に入り、設定で AI / 学習を一時的に強制 OFF にする。次回起動
時にユーザー通知で復旧手順を案内する。M47 v1（M30 未完了時）は TIP の
候補ウィンドウ下部バナーで通知する。設定アプリの通知バナーへの統合は
M30 完了後の follow-up とし、M30 を M47 v1 の前提にはしない。
SafeMode は `settings.safeMode.enabled = true` フラグとして永続化し、
ユーザーが手動で解除するまで継続する。M47 実装時に
`settings/mvp-settings.schema.json`（既存スキーマは
`additionalProperties: false` のため、未定義キーは現状 reject される）に
以下を追加する:

```json
"safeMode": {
  "type": "object",
  "description": "M47: Host 連続クラッシュ時の安全モード状態",
  "properties": {
    "enabled": { "type": "boolean", "default": false },
    "entered_at": { "type": "string", "description": "RFC 3339 タイムスタンプ" },
    "last_crash_count": { "type": "integer", "default": 0 }
  },
  "additionalProperties": false
}
```

スキーマ追加と Host 側の読み書きパスを同じ PR / コミットで揃え、永続化
されない不整合状態を作らない。

#### 8.5.4 UI 通知

候補ウィンドウ下部の控えめインジケータで状態を表示する:

```
⚠️ Zenzai が応答しないため、簡易変換で継続しています [詳細] [再試行]
```

毎回ラベルを出すと邪魔になるため:

- 状態遷移直後のみ 1 回表示（5 秒で自動消滅）
- 詳細クリックの遷移先は M30 / M44 の進捗で分岐する。M47 v1（M30 未完了
  かつ M44 v1 完了時）は `azookey_diag.exe` の起動方法を案内する TIP 内
  ポップアップを表示する。M30 完了と M44 §12.7 の診断タブ統合（M44
  follow-up）の両方が揃った後は、設定アプリの診断タブを直接開く動作に
  切り替える
- 「再試行」クリックで `LoadModel` IPC を送る（現在選択中のモデル・backend で再ロード）

### M47 受け入れ条件

- Host を手動 kill しても入力中のアプリが固まらない（M42 と同じ）
- Host 再起動後に自動復帰する（M42 と同じ）
- Zenzai モデルロード失敗時に `DegradedModel` 状態が UI に明示される
- 連続クラッシュ時は `SafeMode` に入り、次回起動時に通知する
- 各処理の timeout が §8.5.2 の表通りに動作する

## 9. WIL 段階導入（M43）

### 9.1 方針

Windows C++ では `HANDLE` / COM ポインタ / レジストリハンドル / `HRESULT`
/ `CoTaskMemFree` / `LocalFree` の管理が事故りやすい。Windows Implementation
Library（WIL、header-only）を導入し、RAII で安全化する。

WIL は header-only のため、vcpkg ではなく submodule または `FetchContent`
（オプトイン）で取り込む（§3.2）。

### 9.2 段階導入

- **新規コードは最初から WIL を使う**（`wil::unique_handle` /
  `wil::com_ptr` / `wil::unique_hlocal` / `wil::unique_cotaskmem_string`
  / `wil::unique_hkey` 等）。
- 既存の `NamedPipeTransport.cpp`（`SecurityDescriptor` 周り）、
  `tsf-tip` の COM/HANDLE/レジストリ処理は、一気に置き換えず段階的に
  置換する。M40（Named Pipe 強化）・M39（永続化）で当該ファイルを触る
  際に併せて WIL 化するのが効率的。

### M43 受け入れ条件

- WIL が header-only 依存として取り込まれ、オフラインビルドを壊さない
- 新規コードが WIL の RAII 型を使用する
- `tsf-tip` の COM / HANDLE / レジストリ処理の RAII 化が進む
- 既存テストが回帰しない

## 10. 成果測定 KPI

レビュー（プロジェクトレビュー 2026-05-22）が提案する KPI を、本トラックの
効果測定指標として記録する。これらは目標値であり、達成可否のゲートは各
マイルストーンの受け入れ条件側で判定する。

| KPI | 目標値 | 関連 |
|---|---|---|
| 候補表示レイテンシ | p50 < 80ms / p95 < 180ms | §4.5, §7.3 |
| Host 異常停止時の復旧時間 | 中央値 3 秒以内 | §8.3 |
| CI 成功率 | main 連続 14 日で > 95% | §4 |
| 学習永続化の破損率 | 0%（クラッシュ注入テストで確認） | §5.4 |

## 11. 不採用とした提案

レビュー指摘のうち、本トラックで採用しない / 修正して採用するものを理由と
ともに記録する。

### 11.1 vcpkg.json の一括導入 — 修正採用

レビューは `vcpkg.json` でビルド依存基盤を作ることを提案する。本プロジェクト
は**オフラインビルド原則**（§1）を持ち、`AZOOKEY_FETCH_GOOGLETEST` が意図的に
オプトインである。vcpkg の必須化はこの原則と衝突するため、必須化はしない。
依存ごとに header-only 優先・`FetchContent`/submodule オプトインで導入する
（§3）。`vcpkg.json` は任意の利便ファイルとしてのみ許容する。

### 11.2 自前 JSON パーサの即時全置換 — 修正採用

レビュー提案 A（`nlohmann-json` への即時全置換）は採らない。自前パーサは
軽量・低依存という利点があり、IPC の JSON は大きくない。提案 B（堅牢化 +
fuzz/malformed テスト）を主とし（§6）、置換は将来判断とする。

### 11.3 Zenzai runtime 抽象 `IModelRuntime` の新設 — 不採用

レビューは `IModelRuntime` インターフェース（`SimpleRuntime` /
`LlamaCpuRuntime` / `LlamaCudaRuntime` …）の新設を提案する。しかし
`core/IConverter` が既に同じ役割（変換バックエンドの差し替え可能境界）を
担い、M8 も Zenzai を `IConverter` 実装として `SimpleConverter` と差し替え
る方針が確定している。`IModelRuntime` 新設は二重抽象になるため不採用。
バックエンド別実装の内部構造は M8 / M24 の範囲で `IConverter` 実装として
整理する。

### 11.4 packaging / signing 足場の新設 — 不採用（重複）

レビューは packaging / signing の足場作りを提案するが、MSIX・WiX・署名 CI・
DPAPI 暗号化は既存マイルストーン M11 / M12 / M28〜M34 でカバー済みである。
重複するため本トラックでは新設しない。`scripts/register.ps1` /
`unregister.ps1` を「開発用」と明確化する小改善（命名分離の検討）は M28 の
残作業に追記する。

### 11.5 clang-tidy / CodeQL の必須化 — 見送り

CI への clang-tidy / CodeQL 追加は導入・調整コストが高い。本トラックでは
`clang-format` チェックまでを必須とし、clang-tidy / CodeQL は将来の任意
拡張とする（§4.3）。

## 12. IME 診断・修復ウィザード（M44）

### 12.1 目的

azooKey が動作しない、候補が出ない、Zenzai が使われていない、学習が
反映されない、IME 登録が壊れているといった問題をユーザー自身または
開発者が短時間で切り分けられるようにする。IME は「入力できない」時点で
UX が即死しやすいため、本機能は配布前（Phase 4 ゲート）に優先実装する。

### 12.2 診断項目

| ID | 項目 | チェック内容 | 失敗時の推奨修復 |
|---|---|---|---|
| D-001 | TIP DLL 存在 | 登録済み DLL パスが存在するか | 再登録を促す |
| D-002 | COM 登録 | CLSID / InprocServer32 / Profile GUID が正しいか | `register-dev.ps1` または MSIX 修復 |
| D-003 | 言語プロファイル | 日本語 `0x0411` の Profile があるか | Profile 再登録 |
| D-004 | Host 起動 | Host プロセスが存在するか | Host 起動 |
| D-005 | IPC Handshake | Named Pipe 接続 + Handshake 成功 | Host 再起動 |
| D-006 | IPC Ping | Ping 往復 latency 測定 | pipe / firewall / Host 状態確認 |
| D-007 | モデルパス | 設定上のモデルファイルが存在するか | モデル選択 UI（M45）へ誘導 |
| D-008 | モデル検証 | GGUF magic / version / metadata 確認 | 破損モデル扱い |
| D-009 | fallback 状態 | Zenzai / SimpleConverter / degraded を表示 | モデルロード再試行 |
| D-010 | learning store | 読み込み可能か、破損していないか | バックアップ後に初期化 |
| D-011 | user dict | JSON 読み込み可能か | バックアップ後に修復 |
| D-012 | settings | schema validation 成功 | 不正値のリセット |
| D-013 | logs | `%LOCALAPPDATA%\azooKey\logs\` 書き込み可能 | ディレクトリ作成 |
| D-014 | DPAPI | 暗号化データを復号できるか | 再認証 / 再入力を促す |
| D-015 | app compatibility | 現在の前面アプリで TSF context が取得できるか | 互換性情報表示（M50 result） |

### 12.3 `azookey_diag.exe` CLI

3 サブコマンドを持つ:

```powershell
azookey_diag.exe --json                                  # 全項目を JSON で出力
azookey_diag.exe --repair                                # 自動修復可能なものを実行
azookey_diag.exe --collect --output azookey-diag.zip     # 診断 ZIP 生成
```

`--json` 出力例:

```json
{
  "status": "warning",
  "timestamp_ms": 1780000000000,
  "checks": [
    {
      "id": "D-008",
      "name": "model_validation",
      "status": "warning",
      "message": "Model is not loaded. SimpleConverter fallback is active.",
      "details": {
        "configured_path": "%LOCALAPPDATA%\\azooKey\\models\\zenzai.gguf",
        "exists": false,
        "backend": "cpu"
      }
    }
  ]
}
```

`status` は `ok` / `warning` / `error` の 3 値。`--json` はテスト可能な
stable schema として固定する。

### 12.4 診断 ZIP 構成

```
azookey-diagnostics-YYYYMMDD-HHMMSS.zip
├── diag.json
├── settings.redacted.json
├── host-health.json
├── ipc-ping.json
├── logs/
│   ├── host-YYYYMMDD.jsonl   (直近 7 日)
│   └── tip-YYYYMMDD.jsonl    (直近 7 日)
├── environment.txt           (OS バージョン / CPU / RAM / GPU)
└── crash-summary.txt         (WER ダンプの要約のみ、ダンプ本体は含めない)
```

`diag.json` は §12.3 `--json` 出力と同一の **stable schema** とし、以下を正典とする
（snapshot テストで固定。本文系フィールドは持たない）:

```json
{
  "status": "ok | warning | error",
  "timestamp_ms": 0,
  "checks": [
    {
      "id": "D-001 .. D-015",
      "name": "string (安定 ID。例 model_validation)",
      "status": "ok | warning | error",
      "message": "string (人間向け。本文・候補・prompt を含めない)",
      "details": { "...": "check 固有のメタ情報のみ（パス / bool / 件数 / hash / mtime）" }
    }
  ]
}
```

- `status` は `checks[].status` の最悪値（error > warning > ok）。
- 各 ZIP メンバの突き合わせは §7.3 の相関 ID では行わず、診断は単発スナップショット。
  `host-health.json` は §12.6 `QueryDiagnostics` の payload をそのまま保存する。
- 新しい check は `D-0NN` を末尾追加（後方互換）。既存 ID の `name` 改名は破壊的変更扱い。

### 12.5 機密情報の取り扱い

診断 ZIP には以下を**含めない**:

- OpenAI API key（DPAPI 暗号化済みでも除外）
- 入力本文・候補本文
- Magic Conversion の prompt
- 変換前後の全文
- ユーザー辞書の実データ
- 学習 TSV / typo_corrections.tsv / auto_words.tsv の本文

必要な場合でも、**件数・サイズ・hash・mtime のみ**を記録する。
`settings.redacted.json` は API key 等の機密 field を `***redacted***`
に置換した copy。

### 12.6 IPC: QueryDiagnostics

Host 側の状態を取得する新規 IPC。MessageType enum 末尾に追加する
（M40 互換性ルール）。

エンベロープは既存 wire format `{version, request_id, type, trace_id, payload}`
に従う。

```
Request:
  { "version": 1, "request_id": 1, "type": "QueryDiagnostics",
    "trace_id": "...", "payload": {} }

Response:
  { "version": 1, "request_id": 1, "type": "QueryDiagnostics",
    "trace_id": "...",
    "payload": {
      "model_loaded": bool, "engine": str, "backend": str, "rss_mb": int,
      "ep": str (optional, R2/winml 時の選択 EP 名),
      "ep_state": str (optional, "NotPresent"|"NotReady"|"Ready"|"Registered"|"Failed"),
      "ep_last_error": str (optional, EP 取得・登録失敗の HRESULT/診断文; §4.6),
      "learning_entries": int, "user_dict_entries": int,
      "fallback_state": "healthy" | "degraded_simple" |
                        "degraded_model" | "safe_mode",
      "last_error": str (optional)
    } }
```

`--collect` 時はこの IPC で取得した値を `host-health.json` に保存する。

### 12.7 UI

M44 v1（Phase 4 ゲート）は **CLI（`azookey_diag.exe`）+ 診断 ZIP** のみで
完結させる。設定アプリ `診断` タブは M30 完了後の follow-up として
v1.x で追加する位置づけ（M30 を M44 v1 の前提にはしない）。下記の GUI
要件は v1.x（M30 完了後）に適用する。Host 未起動でも GUI 単体で項目
D-001〜D-003 / D-007 / D-008 / D-011〜D-013 までは実行可能とする。

```
[azooKey 診断]

状態: 一部問題があります

[1] TIP 登録状態             ✅ 正常
[2] Host 起動状態            ✅ 起動中
[3] IPC 接続                 ✅ Ping 12ms
[4] Zenzai モデル            ⚠️ 未ロード。SimpleConverter fallback 中
[5] 学習データ               ✅ 正常
[6] ユーザー辞書             ✅ 正常
[7] 設定ファイル             ✅ 正常
[8] ログ出力                 ✅ 有効
[9] セキュリティ設定         ✅ API キーは DPAPI 暗号化済み

[再チェック] [自動修復] [診断 ZIP を作成] [ログフォルダを開く]
```

### M44 受け入れ条件

- クリーン環境で全項目チェックが実行できる
- Host 未起動でも診断アプリがクラッシュしない
- Zenzai モデル未配置時に `warning` として fallback 状態を表示する
- 診断 ZIP に秘密情報が含まれない（snapshot テストで保証）
- `--json` 出力が stable schema としてテストされる
- `--repair` で D-001 / D-002 / D-003 / D-013 の自動修復が動く

## 13. アプリ互換性テストハーネス（M50）

### 13.1 目的

主要アプリで TSF composition / 候補ウィンドウ位置 / 確定 / キャンセル /
Unicode / 絵文字 / Undo / Redo が壊れないことを半自動で検証する。
M28（MSIX サイドロード）着手前にアプリ互換性のベースラインを確保する。

### 13.2 対象アプリ

| 種別 | アプリ | 自動化レベル |
|---|---|---|
| 標準 | Notepad | full |
| 標準 | WordPad / Notepad 後継 | best-effort |
| ブラウザ | Edge | full |
| ブラウザ | Chrome | full |
| ブラウザ | Firefox | best-effort |
| Electron | VS Code | full |
| Electron | Discord | best-effort |
| Electron | Slack | best-effort |
| Office | Word | recorder |
| Office | Excel | recorder |
| Office | Outlook | recorder |
| Terminal | Windows Terminal | full |
| Terminal | PowerShell ISE | best-effort |
| UWP / WinUI | Windows Settings | full |
| UWP / WinUI | Store apps | best-effort |

`full` = UI Automation + SendInput + screenshot による完全自動。
`best-effort` = 自動化可能な範囲のみ、残りは checklist。
`recorder` = キーボード操作の記録・再生（Office は UI Automation の
信頼性が低い）。

### 13.3 テストケース

| ID | ケース | 期待動作 |
|---|---|---|
| C-001 | `nihongo` → Space → Enter で確定 | 「日本語」が確定される |
| C-002 | Backspace で preedit が戻る | 1 文字分戻る |
| C-003 | ESC で composition 破棄 | preedit が消える |
| C-004 | 候補ウィンドウがキャレット付近に出る | キャレット下に出る |
| C-005 | マルチディスプレイ端で候補が画面外に出ない | 画面内にクランプ |
| C-006 | DPI 150% で候補位置がずれない | 正しくスケール |
| C-007 | 絵文字 / サロゲートペア入力 | 正しく入力される |
| C-008 | Undo / Redo が破綻しない | 元に戻る |
| C-009 | フォーカス移動時に composition が安全に処理される | crash しない |
| C-010 | Host kill 中も入力が固まらない | DegradedSimple で継続 |
| C-011 | `Ctrl+A/C/V/L/S`、Alt メニュー、Win キー併用を押す | TIP が食わずアプリ / OS へ通る |
| C-012 | `ja` / `ju` / `jo` / `jya` / `jyu` / `jyo` を入力 | 「じゃ」「じゅ」「じょ」として preedit / commit できる |

### 13.4 実装

`compat-test/` ディレクトリを新設:

```
compat-test/
├── CMakeLists.txt
├── runner/
│   ├── CompatRunner.cpp        # UI Automation + SendInput
│   ├── ScreenshotCapture.cpp   # GDI 経由のスクリーンショット
│   └── ReportWriter.cpp
├── cases/
│   ├── C001_basic_input.cpp
│   ├── C002_backspace.cpp
│   ├── ...
│   └── C010_host_kill.cpp
└── targets/
    ├── notepad.json
    ├── edge.json
    ├── vscode.json
    └── ...
```

各 target JSON は AppId / window class / 自動化レベルを定義する。
本ディレクトリの雛形（README + `targets/notepad.json` サンプル）は
`compat-test/` に置く。雛形はまだ CMake のビルド対象に組み込まず（トップ
`CMakeLists.txt` は `add_subdirectory` を明示列挙する方式）、M50 実装時に
runner / cases を追加して配線する。

### 13.5 出力

```
compat-report-YYYYMMDD-HHMMSS/
├── report.md         # 人間向けサマリ
├── report.json       # CI artifact 用
├── screenshots/
│   ├── notepad_C001_pass.png
│   └── vscode_C004_fail.png
├── logs/
│   └── ...
└── failures/
    └── vscode_C004_fail/  # 失敗時の詳細スクショ + ログ
```

### 13.6 CI 連携

GitHub Actions の Windows ジョブに optional な `compat` ステージを
追加する（M50 完了時）:

```yaml
- name: Run compat tests (optional)
  if: github.event_name == 'pull_request' && contains(github.event.pull_request.labels.*.name, 'compat-test')
  run: |
    cmake --build --preset windows-release --target compat_test
    .\build\windows-release\compat-test\compat_test.exe --output compat-report
  continue-on-error: true
- name: Upload compat report
  uses: actions/upload-artifact@v4
  with:
    name: compat-report
    path: compat-report-*/
```

CI で常時実行するとコストが高いため、`compat-test` ラベル付き PR
または手動 dispatch でのみ実行する。

### M50 受け入れ条件

- Notepad / VS Code / Edge で C-001〜C-010 の自動テストが通る
- 失敗時にスクリーンショットとログが `failures/` に保存される
- `report.json` が CI artifact としてアップロードできる
- `report.md` が PR コメント用に整形されている
