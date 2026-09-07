"""Fixed Mozc/CLDR source excerpts: BSD-3-Clause and Unicode-3.0.

Copyright 2010-2018 Google Inc.; Copyright 2004-2024 Unicode, Inc.
See ../../THIRD_PARTY_LICENSES. The malformed cases are authored fixtures.
"""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import emoji_porter
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


if __name__ == "__main__":
    unittest.main()
