"""The M63-B evaluation set generator is deterministic and the committed set is current.

The generator reads the built-in romaji table, the bundled English dictionary and the
authored kana_kanji_eval inputs from the repository; see
../generate_mixed_english_eval.py.
"""

import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import generate_mixed_english_eval as generator


class GenerateMixedEnglishEvalTest(unittest.TestCase):
    def generate(self) -> str:
        return generator.generate(generator.CONVERTER.read_text(encoding="utf-8"),
                                  generator.DICTIONARY, generator.JAPANESE_SOURCE)

    def test_same_inputs_give_the_same_set(self):
        first = self.generate()
        self.assertEqual(first, self.generate())
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "mixed_english_eval.jsonl"
            holdout = Path(directory) / "mixed_english_holdout.jsonl"
            paths = ["--output", str(output), "--holdout-output", str(holdout)]
            self.assertEqual(generator.main(paths), 0)
            self.assertEqual(output.read_text(encoding="utf-8"), first)
            self.assertEqual(generator.main(paths + ["--check"]), 0)
            output.write_text(first + "\n", encoding="utf-8")
            self.assertEqual(generator.main(paths + ["--check"]), 1)

    def test_the_committed_sets_are_current(self):
        self.assertEqual(generator.OUTPUT.read_text(encoding="utf-8"), self.generate())
        holdout = generator.generate_holdout(generator.CONVERTER.read_text(encoding="utf-8"),
                                             generator.DICTIONARY)
        self.assertEqual(generator.HOLDOUT_OUTPUT.read_text(encoding="utf-8"), holdout)

    def test_cases_follow_the_format(self):
        cases = [json.loads(line) for line in self.generate().splitlines()]
        self.assertEqual(len({case["id"] for case in cases}), len(cases))
        categories = {case["category"] for case in cases}
        self.assertEqual(categories, {"mixed_english", "japanese_only", "romaji_like_english"})
        table = generator.RomajiTable(generator.CONVERTER.read_text(encoding="utf-8"))
        for case in cases:
            self.assertEqual(case["provenance"], "authored")
            if case["category"] != "mixed_english":
                self.assertEqual(table.to_kana(case["raw_romaji"]), case["reading"], case["id"])
            if case["category"] == "japanese_only":
                self.assertEqual(case["expected_english"], [], case["id"])
            for english in case["expected_english"]:
                typed = case["raw_romaji"][english["begin"]:english["end"]]
                self.assertEqual(typed.lower(), english["surface"].lower(), case["id"])

    def test_kana_round_trip(self):
        table = generator.RomajiTable(generator.CONVERTER.read_text(encoding="utf-8"))
        for kana in ["きょうはいいてんきですね", "がっこうへいく", "きんようび", "ふぁいる"]:
            self.assertEqual(table.to_kana(table.to_romaji(kana)), kana)


if __name__ == "__main__":
    unittest.main()
