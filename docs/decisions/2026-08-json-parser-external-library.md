# JSON パーサを外部ライブラリへ移行しない決定

- 種別: 決定記録
- 日付: 2026-08
- 関連 Issue: DEV-390、DEV-163、DEV-188、DEV-211
- 結論の正典: [`dev-infrastructure-spec.md`](../dev-infrastructure-spec.md) §11.2.1
- 移設元: `docs/dev-infrastructure-spec.md` §11.2.1（commit `c8d9a5b` 時点）。原文をそのまま移設した

本文中の `§` は移設元の `docs/dev-infrastructure-spec.md` の節を指す。

#### 11.2.1 外部ライブラリ移行の方針（DEV-390 で確定 / 2026-08）

「将来判断」として残していた RapidJSON / nlohmann-json への移行方針を、次のとおり確定
する。**v1.0 の範囲では自前パーサを維持し、外部ライブラリへは移行しない。**

移行を推す論拠は、自前実装に起因するバグクラスを構造的に解消できることにあった。その
バグクラスは §6.2 のパーサ強化で既に解消済みである。数値 codec の locale 依存
（DEV-163）は `std::from_chars` / `std::to_chars` への置換で閉じ、int64/uint64 精度・
深度上限・unicode escape・round-trip の直接単体テスト（DEV-188）は `json_test.cpp` に
入った。残る DEV-211 は IPC メッセージ毎の冗長な JSON 往復を減らす性能課題であり、
パーサ実装の出自とは独立している。したがって移行の主たる動機は既に失われている。

対して移行のコストは残ったままである。自前パーサは §6.2 の上限値と wire contract を
spec と `static_assert` で結び付けており、置換すればこの対応関係を作り直すことになる。
TIP は任意アプリへ in-proc ロードされる DLL であり、依存追加はコンパイル時間と
バイナリサイズに直接効く。no-egress 方針のもとでは FetchContent のピン留めと
`AZOOKEY_FETCH_*=OFF` 既定も前提として維持しなければならない。

再評価は次のいずれかが起きたときに行う。

* §6.2 の範囲を超える JSON 要件が生じる（streaming parse、JSON Pointer、
  schema validation など、自前実装への追加が大きくなる機能）
* IPC schema の大規模な作り直しが決まり、payload codec を書き直す機会が生じる
  （このとき RapidJSON を新規ペイロードだけに部分導入し、性能・コンパイル時間・
  バイナリサイズを実測する）
* DEV-211 の bench で JSON parse / stringify 自体がレイテンシのボトルネックだと
  示される（往復回数の削減で足りるなら移行の根拠にはならない）

いずれも v1.0 のリリースゲートではない。設定ファイル側（`SettingsStore` /
`UserDictionary`）はホットパスでないため、部分導入を試すなら先行させる候補になる。
