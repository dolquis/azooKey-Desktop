"""Fixed Mozc/CLDR source excerpts: BSD-3-Clause and Unicode-3.0.

Copyright 2010-2018 Google Inc.; Copyright 2004-2024 Unicode, Inc.
The japanese-usage-dictionary excerpt (USAGE): BSD-2-Clause,
Copyright (c) 2011, Hiroyuki Komatsu, Shoji Ogura, Tsuyoshi Horo.
See ../../THIRD_PARTY_LICENSES. The malformed cases are authored fixtures.
"""

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import emoji_porter
import homophone_notes_porter
import symbol_porter
from rewriter_data import normalize_reading, normalize_trigger, verify

SYMBOL = """POS\tCHAR\tReading (space separated)\tdescription
記号\t「」\t() 「」 かっこ かぎかっこ\tかぎ括弧
記号\t『』\t() 「」 かっこ かぎかっこ\t二重かぎ括弧
記号\tX\tASCII\t除外
記号\tY\tかな
"""
EMOJI = """# Mozc emoji excerpt
1F604\t😄\t えがお かお すまいる はっぴー わーい\t\t顔\tわーい スマイル ハッピー 笑顔 表情（嬉しい） 顔\tE0.6
"""
JA = """<ldml><annotations>
<annotation cp="😄">スマイル | ハッピー | わーい | 笑顔 | 顔</annotation>
<annotation cp="😄" type="tts">笑顔</annotation>
</annotations></ldml>"""
EN = """<ldml><annotations>
<annotation cp="😄">eye | eyes | face | grin | grinning | happy | laugh | lol | mouth | open | smile | smiling</annotation>
<annotation cp="😄" type="tts">grinning face with smiling eyes</annotation>
</annotations></ldml>"""


class RewriterPorterTests(unittest.TestCase):
    def test_trigger_only_lowercases_ascii(self):
        self.assertEqual(normalize_trigger("SMILE K+_-"), "smile+_-")

    def test_symbol_determinism_filtering_and_ranking(self):
        result = symbol_porter.convert(SYMBOL)
        self.assertEqual(result, symbol_porter.convert(SYMBOL))
        self.assertEqual(verify(result, False), 2)
        self.assertIn("「」\tかぎかっこ|かっこ\tかぎ括弧\t2\n", result)
        self.assertIn("『』\tかぎかっこ|かっこ\t二重かぎ括弧\t1\n", result)
        with self.assertRaises(ValueError):
            symbol_porter.convert(SYMBOL + SYMBOL)

    def test_emoji_join_names_and_aliases(self):
        result = emoji_porter.convert(EMOJI, JA, EN)
        self.assertEqual(result, emoji_porter.convert(EMOJI, JA, EN))
        self.assertEqual(verify(result, True), 1)
        self.assertIn("わらい", result)
        self.assertIn("grinning_face_with_smiling_eyes", result)
        self.assertIn("\t笑顔\t", result)
        self.assertIn("Emoji version ceiling: 15.0", result)
        self.assertEqual(verify(emoji_porter.convert(EMOJI.replace("E0.6", "E16.0"), JA, EN), True), 0)

    def test_shared_grammar_rejects_corruption(self):
        for row in ["x\ta||b\tname\t1\n", "x\ta|a\tname\t1\n",
                    "x\ta\tbad|name\t1\n", "x\ta\tname\t-1\n",
                    "x\ta\tname\t4294967296\n", "x\ta\tname\t1\r\n"]:
            with self.subTest(row=row), self.assertRaises(ValueError):
                verify(row, False)

    def test_bundled_data_and_normalization_idempotence(self):
        root = Path(__file__).resolve().parents[2]
        for emoji, filename in [(False, "symbol.tsv"), (True, "emoji.tsv")]:
            text = (root / "data" / filename).read_bytes().decode("utf-8")
            self.assertGreater(verify(text, emoji), 1000)
            for line in text.splitlines():
                if not line or line.startswith("#"):
                    continue
                for reading in line.split("\t")[1].split("|"):
                    self.assertEqual(normalize_reading(reading), reading)


# Rows of usage_dict.txt (tag 2025-01-25), out of order and with a repeated pair.
USAGE = (
    "すむ\t澄む\t五段・マ行\t(液体が)すきとおる。「この湧き水はとても澄んでいる」\n"
    "すむ\t住む\t五段・マ行\tある場所で生活する。「彼は東京に住んでいる」「森に住むキツネ」\n"
    "すむ\t棲む\t五段・マ行\t「住む」に同じ。特に、動物などが巣で生活する。「森に棲むキツネ」\n"
    "あおい\t青い\t形容詞・アウオ段\t1. 晴れているときの空や海の色。「空が青い」\\n"
    "2. 〈「蒼い」とも〉血の気がない。「青い顔をしている」\n"
    "うんこう\t運行\t*\tきまった路線をすすむこと。「バスの運行」\n"
    "うんこう\t運航\t*\t(船、航空機が)きまった航路をすすむこと。「飛行機は正常に運航している」\n"
    "かす\t科す\t*\t刑罰を負わせる。「刑罰を科す」\n"
    "かす\t科す\t*\t(刑罰を)負わせる。「刑罰を科す」\n"
    "あきる\t飽きる\t一段\t〈「厭きる」とも〉同じようなことの繰り返しにうんざりする。\n"
    "あずかる\t与る\t五段・ラ行\t〈かな書きも〉\\n1. かかわる。「計画に与る」\\n"
    "2. (恩恵などを)受ける。もらう。「恩恵に与る」「栄誉に与る」\n"
    "うれい\t憂い\t*\t〈「愁い」とも〉\\n1. 心配すること。\\n2. 悲しく思うこと。\n"
    "おく\t措く\t五段・カ行イ音便\t〈文語体〉〈「置く」とも〉とりあえず考えないことにする。"
    "棚上げする。「この問題はひとまず措くとする」\n"
    "ととのえる\t調える\t一段\t 〈「整える」とも〉(味などを)加減してちょうどいい状態にする。"
    "「塩コショウで味を調える」\n"
)


class HomophoneNotesPorterTests(unittest.TestCase):
    def test_rules_order_duplicates_and_empty_notes(self):
        result = homophone_notes_porter.convert(USAGE)
        self.assertEqual(result, homophone_notes_porter.convert(USAGE))
        rows = [line for line in result.splitlines() if not line.startswith("#")]
        self.assertEqual(rows, [
            "あおい\t青い\t晴れているときの空や海の色",
            # A 「 inside 〈〉 does not start the example.
            "あきる\t飽きる\t〈「厭きる」とも〉同じようなことの繰り返しにうんざりする",
            # A label-only line joins the next sense, without its "1. ".
            "あずかる\t与る\t〈かな書きも〉かかわる",
            "うれい\t憂い\t〈「愁い」とも〉心配すること",
            "うんこう\t運航\t(船、航空機が)きまった航路をすすむこと",
            "うんこう\t運行\tきまった路線をすすむこと",
            "おく\t措く\t〈文語体〉〈「置く」とも〉とりあえず考えないことにする。棚上げする",
            "かす\t科す\t刑罰を負わせる",
            "すむ\t住む\tある場所で生活する",
            "すむ\t澄む\t(液体が)すきとおる",
            "ととのえる\t調える\t〈「整える」とも〉(味などを)加減してちょうどいい状態にする",
        ])  # 棲む's note is empty and dropped; the first 科す row wins.
        self.assertEqual(homophone_notes_porter.verify(result), 11)

    def test_header_carries_the_attribution(self):
        header = [line for line in homophone_notes_porter.convert(USAGE).splitlines()
                  if line.startswith("#")]
        text = "\n".join(header)
        for needed in ["japanese-usage-dictionary 2025-01-25",
                       "https://github.com/hiroyuki-komatsu/japanese-usage-dictionary",
                       "BSD-2-Clause", "Hiroyuki Komatsu, Shoji Ogura, Tsuyoshi Horo",
                       "THIRD_PARTY_LICENSES", "porter.py v1", "section 3.3"]:
            self.assertIn(needed, text)

    def test_source_and_archive_are_checked(self):
        with self.assertRaises(ValueError):
            homophone_notes_porter.convert("すむ\t住む\tある場所で生活する\n")  # three columns
        with self.assertRaises(ValueError):
            homophone_notes_porter.source_from_archive(b"not the pinned archive")

    def test_grammar_rejects_corruption(self):
        good = homophone_notes_porter.convert(USAGE)
        body = "あ\t亜\t注記\nい\t井\t注記\n"
        for text in ["\ufeff" + good, good.replace("\n", "\r\n"), good.rstrip("\n"),
                     "あ\t亜\n", "あ\t\t注記\n", "あ\t亜\t注\x01記\n", "あ\t亜\t注記\t余り\n",
                     body + "あ\t亜\t重複\n", "い\t井\t注記\nあ\t亜\t注記\n",
                     body + "# late comment\n"]:
            with self.subTest(text=text[:20]), self.assertRaises(ValueError):
                homophone_notes_porter.verify(text)

    def test_failure_leaves_the_previous_output(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "homophone-notes.tsv"
            output.write_bytes(b"previous")
            source = Path(directory) / "usage_dict.txt"
            source.write_bytes("すむ\t住む\n".encode("utf-8"))
            with self.assertRaises(ValueError):
                homophone_notes_porter.main(["--source", str(source), "--output", str(output)])
            self.assertEqual(output.read_bytes(), b"previous")
            source.write_bytes(USAGE.encode("utf-8"))
            homophone_notes_porter.main(["--source", str(source), "--output", str(output)])
            self.assertEqual(output.read_bytes().decode("utf-8"),
                             homophone_notes_porter.convert(USAGE))
            self.assertEqual(sorted(path.name for path in Path(directory).iterdir()),
                             ["homophone-notes.tsv", "usage_dict.txt"])

    def test_bundled_data(self):
        root = Path(__file__).resolve().parents[2]
        text = (root / "data" / "homophone-notes.tsv").read_bytes().decode("utf-8")
        self.assertGreater(homophone_notes_porter.verify(text), 1000)
        self.assertIn("# SPDX-License-Identifier: BSD-2-Clause\n", text)


if __name__ == "__main__":
    unittest.main()
