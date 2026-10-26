#!/usr/bin/env python3
"""Generates bench/data/mixed_english_eval.jsonl, the M63-B splitter quality gate input.

docs/mixed-english-auto-conversion-spec.md section 15.2 and
docs/conversion-quality-benchmark-spec.md section 4.4 define the set. Every sentence is
authored here: the Japanese frames, the English word list and the romaji-like sentences
are written for this set, and the Japanese-only sentences are the authored CC0 inputs of
bench/data/kana_kanji_eval.jsonl spelled in romaji. The bundled English dictionary is read
only to pick each word's expected surface (spec section 6.4); nothing of it is copied.

Run it again after data/english-words.tsv changes, since the expected surfaces follow the
dictionary's most frequent surface:

    python3 scripts/generate_mixed_english_eval.py
    python3 scripts/generate_mixed_english_eval.py --check   # exit 1 when out of date

The romaji are written with a copy of the built-in romaji table read from
core/src/RomajiKanaConverter.cpp. The C++ evaluator (azookey_mixed_english_eval) checks
every Japanese reading against the real table, so a difference shows up there.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CONVERTER = ROOT / "core" / "src" / "RomajiKanaConverter.cpp"
DICTIONARY = ROOT / "data" / "english-words.tsv"
JAPANESE_SOURCE = ROOT / "bench" / "data" / "kana_kanji_eval.jsonl"
OUTPUT = ROOT / "bench" / "data" / "mixed_english_eval.jsonl"

VOWELS = "aeiou"


class RomajiTable:
    """The built-in table, for kana -> romaji (authoring) and romaji -> kana (checks)."""

    PREFERRED = {
        "し": "si", "ち": "ti", "つ": "tu", "ふ": "hu", "じ": "zi", "を": "wo", "ん": "nn",
        "しゃ": "sya", "しゅ": "syu", "しょ": "syo", "ちゃ": "tya", "ちゅ": "tyu",
        "ちょ": "tyo", "じゃ": "zya", "じゅ": "zyu", "じょ": "zyo", "ー": "-", "ぁ": "xa",
        "ぃ": "xi", "ぅ": "xu", "ぇ": "xe", "ぉ": "xo", "ゃ": "xya", "ゅ": "xyu",
        "ょ": "xyo", "っ": "xtu", "、": ",", "。": ".",
    }

    def __init__(self, converter_source: str) -> None:
        body = converter_source[converter_source.index("kRomajiMap"):
                                converter_source.index("kMaxRomajiKeyLength")]
        self.table = dict(re.findall(r'\{"([^"]+)",\s*"([^"]*)"\}', body))
        reverse: dict[str, str] = {}

        def rank(romaji: str) -> tuple[int, int, str]:
            return (sum(ch in "cqlx" for ch in romaji), len(romaji), romaji)

        for romaji, kana in self.table.items():
            if not kana or not romaji.isascii():
                continue
            if kana not in reverse or rank(romaji) < rank(reverse[kana]):
                reverse[kana] = romaji
        reverse.update(self.PREFERRED)
        self.reverse = reverse

    def _is_prefix(self, pending: str) -> bool:
        return any(len(k) > len(pending) and k.startswith(pending) for k in self.table)

    def _flush(self, pending: str, force: bool) -> tuple[str, str]:
        out = ""
        while pending:
            for length in range(min(4, len(pending)), 0, -1):
                chunk = pending[:length]
                if chunk in self.table:
                    out += self.table[chunk]
                    pending = pending[length:]
                    break
            else:
                if force and pending in ("nn", "n"):
                    out += "ん"
                    pending = ""
                elif force or (len(pending) >= 3 and not self._is_prefix(pending)):
                    out += pending[0]
                    pending = pending[1:]
                else:
                    break
        return out, pending

    def to_kana(self, romaji: str) -> str:
        pending = ""
        out = ""
        for ch in romaji:
            if pending == "nn":
                pending = "n" if ch in VOWELS + "y" else ""
                pending += ch
                converted, pending = self._flush(pending, False)
                out += "ん" + converted
                continue
            if pending == "n" and ch not in VOWELS + "y" and ch != "n":
                converted, pending = self._flush(ch, False)
                out += "ん" + converted
                continue
            if len(pending) == 1 and pending == ch and ch.isalpha() and ch not in VOWELS + "n":
                out += "っ"
                continue
            pending += ch
            converted, pending = self._flush(pending, False)
            out += converted
        converted, _ = self._flush(pending, True)
        return out + converted

    def to_romaji(self, kana: str) -> str:
        out = []
        i = 0
        while i < len(kana):
            if kana[i] == "っ" and i + 1 < len(kana):
                pair = kana[i + 1:i + 3]
                following = self.reverse.get(pair if pair in self.reverse else kana[i + 1], "")
                if following and following[0].isalpha() and following[0] not in VOWELS + "n":
                    out.append(following[0])
                else:
                    out.append("xtu")
                i += 1
                continue
            if kana[i] == "ん":
                following = self.reverse.get(kana[i + 1], "") if i + 1 < len(kana) else ""
                if following.startswith("y"):
                    out.append("xn")
                elif not following or following[0] in VOWELS + "n":
                    out.append("nn")
                else:
                    out.append("n")
                i += 1
                continue
            pair = kana[i:i + 2]
            if len(pair) == 2 and pair in self.reverse:
                out.append(self.reverse[pair])
                i += 2
                continue
            if kana[i] not in self.reverse:
                raise ValueError(f"no romaji for {kana[i]!r} in {kana!r}")
            out.append(self.reverse[kana[i]])
            i += 1
        return "".join(out)


# English words a Japanese sentence names in Latin letters. Only words that are in the
# dictionary and do not read as kana (section 8.1 (ii)) are expected as sure E segments.
WORDS = [
    "github", "slack", "zoom", "excel", "python", "chrome", "email", "server", "update",
    "deploy", "commit", "merge", "branch", "review", "script", "docker", "linux", "windows",
    "google", "twitter", "iphone", "android", "laptop", "mouse", "keyboard", "desktop",
    "browser", "folder", "backup", "login", "password", "link", "click", "check", "test",
    "debug", "build", "release", "report", "schedule", "meeting", "project", "design",
    "layout", "font", "print", "scan", "wifi", "bluetooth", "cloud", "storage", "video",
    "music", "photo", "push", "pull", "sql", "json", "chat", "file", "error", "issue",
]

# Frames: hiragana around one or two slots. The kana before a slot ends in a vowel and the
# kana after it starts with a consonant, so the romaji of a frame never runs into a word.
ONE_SLOT = [
    "きょうは{}をつかった", "あしたまでに{}をなおす", "{}のせっていをかえた",
    "さっき{}でれんらくした", "{}がうごかない", "かいぎのまえに{}をみておいて",
    "このあとで{}をひらく", "{}にあたらしいふぁいるをおいた", "せんしゅう{}をいれなおした",
    "{}のこうしんがおわった", "かれは{}がとくいだ", "まいにち{}をつかっている",
    "{}についてしらべた", "ぶちょうに{}をおくった", "{}のじゅんびをする",
    "よるに{}をとめた", "{}からしらせがきた", "いまから{}をはじめる",
    "ともだちと{}のはなしをした", "{}はべんりだ",
]
TWO_SLOTS = ["{}と{}をくらべた", "{}で{}をかいた", "きのうは{}に{}した", "{}の{}をさがす"]

# Japanese sentences whose romaji holds an English word that reads as kana (kite, same,
# made, take ...). No sure E segment is expected.
ROMAJI_LIKE = [
    "はやくきてください", "さめがおよいでいる", "えきまであるいた", "たけのこをほった",
    "なまえをかいた", "さけをのみすぎた", "ためになるはなしだ", "ものがおおいへやだ",
    "はてしないみちがつづく", "うみまでいこう", "かめがあるいている", "あめがふってきた",
    "まめをたべる", "こめをとぐ", "かさをもっていく", "かみをきった", "かねがない",
    "すしをたべにいく", "ねこがねている", "いえにかえった", "まどをあけた",
    "そとはさむい", "しごとがおわった", "たまごをわった", "にもつをおくった",
    "あさごはんをたべた", "みみがいたい", "ゆめをみた", "かぜをひいた", "はなをかざる",
    "いまきたところだ", "まえをむいてあるく", "おかねをはらう", "さいごまでみた",
    "なにもない", "ここにおいて", "これをみて", "あそこまではしる", "とてもたのしい",
    "まだまだだ",
]
# The same kind of word typed with a Shift capital: the English is meant (section 8.1 (i)).
SHIFT_TYPED = [
    ("{}をみて", "site"), ("{}をあげる", "kite"), ("{}をかえる", "name"), ("{}をひく", "line"),
    ("{}をつくる", "game"), ("{}をきく", "rate"), ("{}がすき", "same"), ("{}をよむ", "note"),
    ("{}をきめる", "rule"), ("{}をしらべる", "data"),
]
JAPANESE_ONLY_COUNT = 150
MIXED_COUNT = 100

# The held-out set (bench/data/mixed_english_holdout.jsonl): sentence shapes the gate set
# does not use, written after the weights were chosen and never used to tune them.
# [word] is typed in lower case, [^word] with a Shift capital first.
HOLDOUT_MIXED = [
    "かいしゃの[server]がまたおちた", "[notion]にめもをのこしておく", "あたらしい[figma]のふぁいるをきょうゆうする",
    "[outlook]のよていひょうがずれている", "[safari]だとがめんがくずれる", "[firefox]ではもんだいない",
    "けっきょく[rust]でかきなおした", "[kotlin]のほうがよみやすい", "[slack]ってべんりだね",
    "まだ[review]がおわっていない", "[merge]するまえにかくにんして", "そのまま[commit]してしまった",
    "[docker]のいめーじがおおきすぎる", "かれの[laptop]はふるい", "[backup]はまいばんとっている",
    "[password]をわすれてしまった", "[error]がでたらおしえて", "[print]するまえにみなおす",
    "[video]かいぎはごごからです", "[photo]をなんまいかおくります", "やっと[build]がとおった",
    "[test]がひとつだけおちる", "[debug]にじかんがかかった", "ぶかの[report]をよむ",
    "あしたの[meeting]はちゅうしです", "[project]のしんちょくをきいた", "[design]をすこしかえたい",
    "[layout]がくずれている", "おおきめの[font]にした", "[cloud]においておいた",
    "[storage]がいっぱいになった", "ひさしぶりに[music]をきいた", "[chat]でへんじがきた",
    "[issue]をいくつかたてた", "[python]と[excel]でしゅうけいした", "[github]から[pull]してきた",
    "[zoom]か[teams]でつなぎます", "[linux]で[script]をうごかす", "[android]と[iphone]のりょうほうでためす",
    "[wifi]と[bluetooth]をきった", "[^Notion]のぺーじをつくった", "[^Figma]でがめんをつくる",
    "[^Slack]にとうこうした", "[^Python]をべんきょうちゅう", "[^Excel]のしーとをわけた",
    "[^GitHub]のりぽじとりをみた", "[^Zoom]のりんくをおくる", "[^Docker]をいれなおした",
    "こんかいは[^Rust]でかく", "[^Kotlin]とじゃゔぁをくらべる",
]
HOLDOUT_JAPANESE = [
    "わたしはすしがすきです", "ここでまっていてください", "かれはとてもかしこい", "おちゃをいっぱいのみたい",
    "あさからあめがふっている", "でんしゃがおくれている", "らいしゅうのよていをきめよう", "しゅくだいがおわらない",
    "ともだちとこうえんであそんだ", "ことしのなつはあつい", "このほんはおもしろかった", "えきのまえでまちあわせる",
    "あたらしいくつをかった", "ねるまえにはをみがく", "やさいをたくさんたべる", "かいぎしつをよやくした",
    "しんかんせんでおおさかにいく", "てがみをかいておくった", "きょうはやすみをとった", "まいあさじょぎんぐをしている",
    "ゆうがたにかいものにいく", "そうじをてつだってほしい", "かぜがつよくてさむい", "たんじょうびおめでとう",
    "しゃしんをとってもらった",
]
HOLDOUT_OUTPUT = ROOT / "bench" / "data" / "mixed_english_holdout.jsonl"


def load_dictionary(path: Path) -> dict[str, list[tuple[str, int]]]:
    entries: dict[str, list[tuple[str, int]]] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("#") or not line.strip():
            continue
        cols = line.split("\t")
        entries.setdefault(cols[0].lower(), []).append((cols[0], int(cols[1])))
    for key in entries:
        entries[key].sort(key=lambda entry: -entry[1])
    return entries


def generate(converter_source: str, dictionary_text_path: Path, japanese_source: Path) -> str:
    table = RomajiTable(converter_source)
    dictionary = load_dictionary(dictionary_text_path)

    def readable(word: str) -> bool:
        return not any("a" <= c <= "z" for c in table.to_kana(word.lower()))

    def build(frame: str, words: list[str], shift_index: int | None = None):
        parts = frame.split("{}")
        raw = ""
        expected = []
        for i, part in enumerate(parts):
            raw += table.to_romaji(part) if part else ""
            if i >= len(words):
                continue
            word = words[i]
            typed = word.capitalize() if shift_index == i else word
            begin = len(raw)
            raw += typed
            if shift_index == i and len(word) >= 2:
                expected.append({"begin": begin, "end": len(raw), "surface": typed})
            elif len(word) >= 3 and not readable(word):
                expected.append({"begin": begin, "end": len(raw),
                                 "surface": dictionary[word][0][0]})
        return raw, expected

    words = [word for word in WORDS if word in dictionary]
    cases = []
    for i in range(MIXED_COUNT):
        if i % 5 == 4:
            frame = TWO_SLOTS[(i // 5) % len(TWO_SLOTS)]
            chosen = [words[(i * 7) % len(words)], words[(i * 7 + 3) % len(words)]]
        else:
            frame = ONE_SLOT[i % len(ONE_SLOT)]
            chosen = [words[(i * 3) % len(words)]]
        raw, expected = build(frame, chosen, 0 if i % 10 == 3 else None)
        cases.append({"id": f"mixed_{i + 1:04d}", "category": "mixed_english", "raw_romaji": raw,
                      "expected_english": expected, "provenance": "authored"})

    count = 0
    for line in japanese_source.read_text(encoding="utf-8").splitlines():
        reading = json.loads(line)["input"]
        try:
            raw = table.to_romaji(reading)
        except ValueError:
            continue
        if table.to_kana(raw) != reading:
            continue
        count += 1
        cases.append({"id": f"japanese_{count:04d}", "category": "japanese_only",
                      "raw_romaji": raw, "reading": reading, "expected_english": [],
                      "provenance": "authored"})
        if count == JAPANESE_ONLY_COUNT:
            break

    for i, sentence in enumerate(ROMAJI_LIKE):
        cases.append({"id": f"romaji_like_{i + 1:04d}", "category": "romaji_like_english",
                      "raw_romaji": table.to_romaji(sentence), "reading": sentence,
                      "expected_english": [], "provenance": "authored"})
    for i, (frame, word) in enumerate(SHIFT_TYPED):
        raw, expected = build(frame, [word], 0)
        cases.append({"id": f"mixed_shift_{len(ROMAJI_LIKE) + i + 1:04d}",
                      "category": "mixed_english", "raw_romaji": raw,
                      "expected_english": expected, "provenance": "authored"})

    return _dump(cases)


def _dump(cases: list[dict]) -> str:
    return "".join(json.dumps(case, ensure_ascii=False, separators=(",", ":")) + "\n"
                   for case in cases)


def generate_holdout(converter_source: str, dictionary_text_path: Path) -> str:
    table = RomajiTable(converter_source)
    dictionary = load_dictionary(dictionary_text_path)
    cases = []
    for i, sentence in enumerate(HOLDOUT_MIXED):
        raw = ""
        expected = []
        for piece in re.split(r"(\[\^?[A-Za-z]+\])", sentence):
            if not piece:
                continue
            if not piece.startswith("["):
                raw += table.to_romaji(piece)
                continue
            shift = piece.startswith("[^")
            typed = piece.strip("[]^")  # As typed: [^GitHub] keeps its capitals.
            word = typed.lower()
            begin = len(raw)
            raw += typed
            readable = not any("a" <= c <= "z" for c in table.to_kana(word))
            if shift:
                expected.append({"begin": begin, "end": len(raw), "surface": typed})
            elif len(word) >= 3 and not readable and word in dictionary:
                expected.append({"begin": begin, "end": len(raw),
                                 "surface": dictionary[word][0][0]})
        cases.append({"id": f"holdout_mixed_{i + 1:04d}", "category": "mixed_english",
                      "raw_romaji": raw, "expected_english": expected,
                      "provenance": "authored"})
    for i, sentence in enumerate(HOLDOUT_JAPANESE):
        cases.append({"id": f"holdout_japanese_{i + 1:04d}", "category": "japanese_only",
                      "raw_romaji": table.to_romaji(sentence), "reading": sentence,
                      "expected_english": [], "provenance": "authored"})
    return _dump(cases)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Generates bench/data/mixed_english_eval.jsonl (M63-B quality gate input).")
    parser.add_argument("--output", type=Path, default=OUTPUT)
    parser.add_argument("--holdout-output", type=Path, default=HOLDOUT_OUTPUT)
    parser.add_argument("--dictionary", type=Path, default=DICTIONARY)
    parser.add_argument("--check", action="store_true",
                        help="exit 1 when an output differs from what would be generated")
    args = parser.parse_args(argv)
    source = CONVERTER.read_text(encoding="utf-8")
    outputs = [(args.output, generate(source, args.dictionary, JAPANESE_SOURCE)),
               (args.holdout_output, generate_holdout(source, args.dictionary))]
    status = 0
    for path, text in outputs:
        if args.check:
            current = path.read_text(encoding="utf-8") if path.exists() else ""
            if current != text:
                print(f"{path} is out of date; run {Path(__file__).name}", file=sys.stderr)
                status = 1
        else:
            path.write_bytes(text.encode("utf-8"))
    return status


if __name__ == "__main__":
    sys.exit(main())
