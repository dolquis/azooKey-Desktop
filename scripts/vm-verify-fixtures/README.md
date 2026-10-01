# vm-verify-fixtures — VM 検証用の合成 fixture と gate map

Hyper-V VM で人間ゲートをまとめて消化するときに、ゲストへ持ち込む入力データと補助スクリプトを置く。
検証の手順、レーンの順序、検証メモの様式は [`docs/handoff/human-gate-batch-runbook.md`](../../docs/handoff/human-gate-batch-runbook.md) が正典である。
本書は fixture の中身と使い方だけを持つ。

> 状態、進捗、各ゲートの合否は Linear（team `Dev`）が正典である。本書には書かない（`AGENTS.md`）。

どのデータも合成であり、実際の入力に由来する語を含まない。
検証メモへ fixture の本文を写す必要がある場合も、ここにある語だけを使う。

## 収録物

| パス | 使うゲート | 内容 |
|---|---|---|
| `data/custom-romaji/` | DEV-1360（M17） | カスタムローマ字 TSV。正常版、差し替え用、空、不正行のみ |
| `data/user-dict/emoji.tsv` | DEV-716（C-007） | 絵文字を表層に持つユーザー辞書の取り込み用 TSV |
| `data/plaintext-stores/` | DEV-1446（M34） | DPAPI 暗号化より前の形式で書いた平文ストア 4 種 |
| `data/stdio/` | DEV-759 | `--stdio` の Host へ貼り付ける Handshake と CommitObservation の行 |
| `new-non-ascii-path-fixture.ps1` | DEV-963 / DEV-1144 | 非 ASCII のディレクトリ名とファイル名の組をゲスト内で生成する |
| `ai-loopback-stub.ps1` | DEV-1411 | 401、429 + `Retry-After`、遅延応答を返すループバック stub |
| `gate-map.json` | 全体 | `scripts/vm-verify-linear-drafts.ps1 -GateMapPath` に渡す対応表 |

DEV-758 は専用のデータを持たない。
runbook「DEV-758 の 2 writer 実走」の合成 entry をそのまま使い、前後の状態は `learning-data-snapshot.ps1` で記録する。

GGUF モデルは収録しない。
`new-non-ascii-path-fixture.ps1 -ModelPath` が、検証 zip に入れたモデルをゲスト内で複製する。

## ゲストへの持ち込み

このディレクトリは検証 zip に含まれない。
検証 zip と同じ commit の checkout からディレクトリごとゲストへコピーする。
転送の前提と資格情報の扱いは [`docs/handoff/hyper-v-tip-verification.md`](../../docs/handoff/hyper-v-tip-verification.md) の手順 3 に従う。
以下の例は、ゲストの `C:\azookey-verify\fixtures\` へ置いたものとして書く。

コピーの後に、ホストとゲストで同じ一覧が出ることを確かめる。

```powershell
Get-ChildItem -LiteralPath . -Recurse -File | Sort-Object FullName |
  ForEach-Object { "{0}  {1}" -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash, $_.Name }
```

このディレクトリの全ファイルは改行を LF に固定してある（`.gitattributes`）。
ストアはヘッダー行をバイト単位で照合するため、CRLF や BOM が付くとレコードとして読まれなくなる。
エディタで開いて保存し直さない。

2 つのスクリプトは Windows PowerShell 5.1 で動く。
ゲストに PowerShell 7 を入れる必要はない。

## カスタムローマ字 TSV（DEV-1360）

書式と、内蔵表へ戻る条件は `docs/legacy-parity-spec.md` §5 が定める。

| ファイル | 期待する結果 |
|---|---|
| `valid.tsv` | `a` `i` `u` `ka` `ki` `ku` `n` `nn` がカタカナになる。`@@` は `@` になる |
| `valid-updated.tsv` | `valid.tsv` と同じ規則で、`ka` だけが半角カタカナ `ｶ` になる |
| `empty.tsv` | 0 バイト。有効な規則が無いので内蔵表が使われる |
| `invalid-only.tsv` | 全行が書式違反。有効な規則が無いので内蔵表が使われる |

`inputStyle=custom` は内蔵表を差し替える（§5.4）。
収録した規則はわざと少なくしてあり、出力をカタカナにしてあるので、内蔵表との違いが打鍵 1 回で分かる。

`customRomajiTablePath` には作業用のコピーを指定し、fixture そのものを書き換えない。

```powershell
$work = "C:\azookey-verify\work\custom-romaji.tsv"
New-Item -ItemType Directory -Force -Path (Split-Path $work) | Out-Null
Copy-Item C:\azookey-verify\fixtures\data\custom-romaji\valid.tsv $work
# 設定アプリで inputStyle=custom と customRomajiTablePath=$work を保存し、`ka` が「カ」になることを確かめる。
Copy-Item C:\azookey-verify\fixtures\data\custom-romaji\valid-updated.tsv $work -Force
# 進行中の preedit が変わらず、次の入力から `ka` が「ｶ」になることを確かめる。
Copy-Item C:\azookey-verify\fixtures\data\custom-romaji\empty.tsv $work -Force
# `ka` が内蔵表の「か」に戻ることを確かめる。invalid-only.tsv でも同じ確認をする。
```

`invalid-only.tsv` の各行は、タブなし、9 文字以上の入力、非 ASCII の入力、空の入力、範囲外や数値でない consume、4 列、UTF-16 で 9 単位以上の出力に当たる。

## 絵文字のユーザー辞書（DEV-716 の C-007）

`emoji.tsv` は `読み<TAB>表層` の 3 行で、`userdict import` が読む形式である。

| 読み | 表層 | UTF-16 での形 |
|---|---|---|
| `えもじ` | U+1F600 | サロゲートペア 1 組 |
| `えもじ` | U+1F389 | サロゲートペア 1 組 |
| `かぞくえもじ` | U+1F468 U+200D U+1F469 U+200D U+1F467 | サロゲートペア 3 組を ZWJ でつないだ列 |

```powershell
$exe = (Resolve-Path .\azookey_inference_host.exe).Path
& $exe userdict import C:\azookey-verify\fixtures\data\user-dict\emoji.tsv
& $exe userdict list --format json
```

`import` は稼働中の Host を経由せず、辞書ファイルを直接書く。
取り込みの後に Host を起動し直してから打鍵する。
`list` の表層は UTF-8 で出力されるので、コンソールで読むときは `[Console]::OutputEncoding` を UTF-8 にする。

`えもじ` を打鍵して候補から確定し、アプリ側の文字列でサロゲートペアが壊れないことを見る。
確認の後は `userdict remove` で 3 件を消すか、checkpoint へ戻す。

## 平文ストア（DEV-1446 の移行確認）

`data/plaintext-stores/` は、暗号化を持たない版が書いていた形式のファイル 4 つである。

| ファイル | レコード数 |
|---|---|
| `learning.tsv` | 3 |
| `typo_corrections.tsv` | 2 |
| `auto_words.tsv` | 2（`confirmed` と `rejected`） |
| `user_dict.json` | 1 |

Host は起動時にこの 4 つを読み、それぞれ `<名前>.enc` と `<名前>.bak` へ移して平文を消す（`docs/sideload-packaging-spec.md` §9.4）。
移行は Host の最初の起動でしか観測できない。
検証 zip を導入して Host を一度も起動していない状態で配置し、checkpoint を取ってから始める。

```powershell
$data = "$env:LOCALAPPDATA\azooKey\data"
New-Item -ItemType Directory -Force -Path $data | Out-Null
Copy-Item C:\azookey-verify\fixtures\data\plaintext-stores\* $data
$snapshot = "C:\azookey-verify\learning-snapshots.json"
powershell -ExecutionPolicy Bypass -File .\learning-data-snapshot.ps1 -Label before-migration -OutputPath $snapshot
# Host を起動する（register-dev.ps1 または verify-bootstrap.ps1）。
powershell -ExecutionPolicy Bypass -File .\learning-data-snapshot.ps1 -Label after-migration -OutputPath $snapshot
powershell -ExecutionPolicy Bypass -File .\learning-data-snapshot.ps1 -From before-migration -To after-migration -OutputPath $snapshot
```

配置先に `.enc` や `.bak` が既にあると移行は起きない。
データディレクトリが空であることを先に確かめる。

比較結果には、4 つのストアそれぞれについて平文の `removed` と、`.enc` と `.bak` の `added` が出る。
`.bak` の SHA-256 は fixture の SHA-256 と一致する。
件数は次で確かめる。

```powershell
& $exe lookup --mode exact --query ごうせいがくしゅう     # 2 件
& $exe lookup --mode exact --query あずーきーけんしょう   # 1 件
& $exe userdict list --format json                        # 1 件
```

レコードの時刻は 2026-10-01 00:00 UTC（`1790812800`）に固定してある。
学習の重みは時刻とともに減衰し、打ち間違え学習は古いレコードを捨てる。
移行そのものの確認（`.enc` と `.bak` の生成、`.bak` の一致）は時刻に依存しない。

0 バイトの平文ストアは移行の入力にしない（DEV-1460）。
収録した 4 つは、どれもレコードを持つ。

## stdio の行（DEV-759）

`data/stdio/` の行は、`--stdio` で起動した Host の標準入力へ 1 行ずつ渡す。
読みと表層は ASCII だけなので、コンソールのコードページの影響を受けない。

| ファイル | 内容 |
|---|---|
| `handshake.jsonl` | Handshake 1 行。最初に必ず送る |
| `commit-single.jsonl` | CommitObservation 1 行（読み `fixturesingle`） |
| `commit-burst.jsonl` | CommitObservation 20 行（読み `fixtureburst01`〜`fixtureburst20`） |

Handshake を送らないと、Host は CommitObservation に `"ok":false` を返し、学習しない。
学習を受け付けるのは、Handshake で `secure_flag` を申告した接続だけである。

終了経路ごとに新しいデータルートを使う。
レーン 2 の Host とデータを共有しない。

```powershell
$exe = (Resolve-Path .\azookey_inference_host.exe).Path
$root = "C:\azookey-verify\dev759\ctrl-c"      # 経路ごとに変える。絶対パスで指定する
# 検証するコンソール（classic conhost または Windows Terminal）で起動する。
& $exe --data-root $root --stdio
```

別の PowerShell から行をクリップボードへ入れ、Host のコンソールへ貼り付ける。

```powershell
$fx = "C:\azookey-verify\fixtures\data\stdio"
Get-Content "$fx\handshake.jsonl" | Set-Clipboard      # 貼り付けて "accepted":true を確かめる
Get-Content "$fx\commit-single.jsonl" | Set-Clipboard  # 貼り付けて "ok":true を確かめる
```

応答を確かめたら、検証する経路（Ctrl+C、または `×`）で終了させ、残ったレコードを数える。

```powershell
& $exe --data-root $root lookup --mode exact --query fixturesingle    # 保持なら 1 件、消失なら "count":0
```

連続 commit の確認では、Handshake の後に `commit-burst.jsonl` の 20 行をまとめて貼り付ける。
`"ok":true` が 20 行返ったことを確かめ、5 秒以内に `×` で閉じる。

```powershell
(& $exe --data-root $root lookup --mode prefix --query fixtureburst | Select-String FIXTURE_BURST).Count
```

消失件数は 20 から上の値を引いたものである。
判定の区分と上限は `docs/learning-data-management-spec.md` §11 に従う。

前後の状態を `learning-data-snapshot.ps1` で記録するときは `-DataDirectory "$root\data"` を付ける。

## 非 ASCII パス（DEV-963 / DEV-1144）

`new-non-ascii-path-fixture.ps1` は、`-Root` の下に 2 組のディレクトリを作る。
ファイル名はコードポイントから組み立てるので、リポジトリには非 ASCII のファイル名を置いていない。

| 組 | 文字 | CP932 で表せるか |
|---|---|---|
| `cp932` | カタカナと漢字 | 表せる |
| `unicode` | ハングル 1 文字（U+D55C）と CJK 拡張 B の 1 文字（U+20BB7、サロゲートペア） | 表せない |

```powershell
powershell -ExecutionPolicy Bypass -File C:\azookey-verify\fixtures\new-non-ascii-path-fixture.ps1 `
  -Root C:\azookey-verify\non-ascii -ModelPath <検証 zip のモデル>
$sets = (Get-Content -LiteralPath C:\azookey-verify\non-ascii\paths.json -Raw -Encoding UTF8 | ConvertFrom-Json).sets
```

各組には、合成レコード 1 件（読み `にほんご`、表層 `日本語`）を持つ平文の学習ファイルが入る。
`-ModelPath` を付けると、モデルを非 ASCII の名前で複製し、SHA-256 の一致を確かめる。
`paths.json` には、ゲストの active code page と各パスが入る。

DEV-963 の 3 経路は、組ごとに次で確かめる。

```powershell
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
foreach ($set in $sets) {
  & $exe --learning $set.learningPath --user-dict $set.userDictPath lookup --mode exact --query にほんご
  & $exe --learning $set.learningPath --user-dict $set.userDictPath userdict add --reading じしょ --surface 辞書 --offline
  & $exe --learning $set.learningPath --user-dict $set.userDictPath userdict list --format json
  & $exe --learning $set.learningPath --user-dict $set.userDictPath userdict remove --reading じしょ --surface 辞書 --offline
  Get-ChildItem -LiteralPath $set.directory | Select-Object Name, Length
}
```

日本語のリテラルを含むコマンドは、対話コンソールへ直接入力する。
Windows PowerShell 5.1 は BOM の無い `.ps1` を ANSI コードページで読むので、スクリプトファイルへ保存すると文字が変わる。

実行の後、各ディレクトリに残るのは学習ファイルの `.enc` と `.bak`、辞書の `.enc`、複製したモデルだけである。
それ以外の名前のファイルがあれば、パスの文字が変換されている。

DEV-1144 では、`$set.modelPath` を `--model` と `model.selectedPath` の両方へ渡す。
`unicode` の組は、active code page が CP932 の環境でも ANSI の API では開けないパスになる。

## AI 整文のループバック stub（DEV-1411）

`ai-loopback-stub.ps1` は `127.0.0.1` だけで待ち受ける。
`TcpListener` を使うので、管理者権限と URL ACL は要らない。

| `-Mode` | 応答 | 誘発する分類（`docs/ai-backend-spec.md` §7.2） |
|---|---|---|
| `auth` | 401 | `Auth` |
| `ratelimit` | 429 と `Retry-After: <秒>` | `RateLimit` と再試行待機 |
| `delay` | `-DelaySeconds` だけ待ってから 504 | `Timeout`（待ち時間を `openAiTimeoutMs` より長くする） |

```powershell
powershell -ExecutionPolicy Bypass -File C:\azookey-verify\fixtures\ai-loopback-stub.ps1 `
  -Mode ratelimit -RetryAfterSeconds 5 -LogPath C:\azookey-verify\ai-stub.log
```

設定は `aiBackend=openai`、`openAiApiEndpoint=http://127.0.0.1:18089/v1`、`openAiApiKey` に機密でない非空の文字列とする。
エンドポイントは `127.0.0.1` と直書きする。
`localhost` と書くと、接続の前に拒否される。

分類を切り替えるときは、Ctrl+C で止めて別の `-Mode` で起動し直す。
`Network` は stub を止めた状態で確かめる。

stub は要求の本文とヘッダーを読み捨てる。
コンソールと `-LogPath` に出るのは、UTC 時刻、メソッド、パス、本文の長さ、返した status だけである。
応答の本文は固定の文字列で、要求の内容を含まない。

## gate map

`gate-map.json` は、自動判定の ID とそれを転記する課題の対応表である。
書式と使い方は runbook の Part C にある。

```powershell
pwsh -File .\scripts\vm-verify-linear-drafts.ps1 `
  -SummaryPath .\build\vm-verify-summary\verification-summary.json `
  -GateMapPath .\scripts\vm-verify-fixtures\gate-map.json `
  -OutputDirectory .\build\vm-verify-drafts
```

対応を置くのは、課題本文か runbook がその観測対象を名指ししている場合に限る。
下書きの行は、サマリを作った時点の観測である。
ゲートの操作の後の状態を根拠にする場合は、操作の後に `verify-bootstrap.ps1 -Json` と `azookey_diag.exe --json` を採り直し、サマリを作り直す。
行があることは合否を意味しない。

### bootstrap（`verify-bootstrap.ps1 -Json`）

| ID | 課題 | 根拠 |
|---|---|---|
| `tipRegistration` | DEV-1266 | 課題の確認手順が TIP の登録を開始条件にしている |
| `tipRegistration` | DEV-1211 | 課題の手順 1 が `register-dev.ps1` による登録である |
| `inferenceHost` | DEV-676 | 項目 1 が supervisor、Host プロセス、pipe の存在を見る |
| `inferenceHost` | DEV-1263 | Host が Ready の状態から停止と復帰を見る |

### diag（`azookey_diag.exe --json`）

| ID | 課題 | 根拠 |
|---|---|---|
| `D-001` `D-002` `D-003` | DEV-1266 | TIP DLL、COM 登録、言語プロファイル。TIP の登録が開始条件である |
| `D-001` `D-002` `D-003` | DEV-1211 | 課題が HKLM CLSID と TSF profile の登録、失敗後の残骸、解除を確認対象にしている |
| `D-004` `D-005` `D-006` | DEV-676 | Host プロセス、Handshake、ping。項目 1 と項目 2 の観測対象である |
| `D-004` `D-005` `D-006` | DEV-1263 | 課題が Host の停止と、再起動の後の再接続を確認対象にしている |
| `D-007` `D-008` | DEV-1144 | 課題が `model.selectedPath` と、ロード済みモデルのパスの一致を見る |
| `D-008` `D-009` | DEV-1398 | runbook レーン 2 の手順 5 が `degraded_model`、`degraded_simple`、SafeMode を誘発する |
| `D-010` | DEV-1446 | 課題の項目 1 と項目 2 が、学習ストアの移行と復号の可否を見る |
| `D-014` | DEV-1446 | 課題の項目 4 が D-014 を名指ししている |
| `D-014` | DEV-1411 | runbook レーン 2 の手順 2 が、`dpapi:` キーを復号できない状態で `KeyReentry` を誘発する |
| `D-011` | DEV-758 | 同時更新の対象が `user_dict.json` であり、更新の後に辞書が壊れていないことの観測になる |
| `D-013` | DEV-1092 | 課題が診断の採取物をローカルへ保存できることを見る。ログのディレクトリへ書けることはその前提である |

### 対応を置かない ID

| ID | 理由 |
|---|---|
| `vcRuntime` | 対象の課題（MSI のレーン）は bootstrap を実行しない |
| `microsoftIme` `vmCheckpoint` `debugView` | セッションの安全装置であり、個別の課題の観測対象ではない |
| `D-009`（DEV-1263 へ） | 課題が見る `degraded` は TIP の接続状態であり、D-009 が報告する Host の fallback 状態とは別である |
| `D-012` | 設定ファイルの schema 適合を観測対象として名指しする課題が無い |
| `D-015` | 前面アプリの記録であり、常に warning か error になる |

compat の行は、`compat-test/README.md` のケースと、各課題が挙げる C-xxx に対応する。
C-005 は DEV-782 が扱うので載せない。

対象の課題を入れ替えたときは、この表と `gate-map.json` を同じ変更で直す。

## テスト

`scripts/tests/vm-verify-fixtures.Tests.ps1` が次を検査する。

- `gate-map.json` が `vm-verify-linear-drafts.ps1` の検証関数を通り、bootstrap と diag の ID が実装の出力する ID に含まれること
- データファイルが LF、BOM なしで、各ストアのヘッダーと列数に合うこと
- カスタムローマ字 TSV の正常版に不正行が無く、不正行のみの版に有効な行が無いこと
- stub が各モードの status と `Retry-After` を返し、要求の内容を応答とログに出さないこと
- 非 ASCII パスの生成結果と、2 つのスクリプトが ASCII だけで書かれていること

```powershell
pwsh -File .\scripts\test-powershell-quality.ps1
```
