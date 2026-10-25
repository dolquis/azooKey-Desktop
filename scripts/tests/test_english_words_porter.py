"""Fixed SCOWL/linguist-style excerpts (authored fixtures) and the committed english-words.tsv.

The data file derives from SCOWL, Google Books Ngram (CC-BY-3.0) and linguist (MIT);
see ../../THIRD_PARTY_LICENSES.
"""

import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import english_words_porter as porter

AFF = """SET UTF-8
ONLYINCOMPOUND c
NOSUGGEST !

PFX A Y 1
PFX A   0     re         .

SFX D Y 3
SFX D   0     d          e
SFX D   y     ied        [^aeiou]y
SFX D   0     ed         [^ey]

SFX G Y 2
SFX G   e     ing        e
SFX G   0     ing        [^e]
"""
DIC = """7
use/ADG
try/DG
walk/ADG
1th/c
damn/!
Apple
don't
"""
YML = """# comment
"C++":
  type: programming
  aliases:
  - cpp
  extensions:
  - ".cpp"
Python:
  type: programming
  aliases:
  - python3
Markdown:
  type: prose
  aliases:
  - md
Git Config:
  type: data
"""


class EnglishWordsPorterTests(unittest.TestCase):
    def test_hunspell_expansion(self):
        words = porter.expand_hunspell(DIC, AFF)
        for expected in ("use", "used", "using", "reuse", "reused", "reusing",
                         "tried", "trying", "walked", "walking", "rewalked", "Apple"):
            self.assertIn(expected, words)
        self.assertNotIn("1th", words)  # ONLYINCOMPOUND
        self.assertNotIn("damn", words)  # NOSUGGEST
        self.assertEqual(porter.letters_only(words) & {"don't"}, set())

    def test_linguist_terms(self):
        terms = porter.parse_linguist(YML)
        self.assertEqual(terms, {"cpp", "Python"})  # no "C++", "python3", prose, or two-word names
        self.assertNotIn("md", terms)

    def test_scale_is_monotonic_and_bounded(self):
        counts = [0, 1, 2, 10, 10**6, 10**9, 10**12, 10**15]
        scaled = [porter.scale(c) for c in counts]
        self.assertEqual(scaled[0], porter.FLOOR_FREQUENCY)
        self.assertEqual(scaled, sorted(scaled))
        self.assertLess(scaled[-1], 2**31)
        self.assertGreater(scaled[1], porter.FLOOR_FREQUENCY)

    def test_flags(self):
        self.assertEqual(porter.flags_for("apple", False), "")
        self.assertEqual(porter.flags_for("Apple", False), "proper")
        self.assertEqual(porter.flags_for("NASA", False), "acronym")
        self.assertEqual(porter.flags_for("GitHub", True), "proper,tech")

    def test_build_is_deterministic_and_verified(self):
        counts = {"apple": 100, "Apple": 90, "cpp": 5}
        first = porter.render(porter.build_rows({"apple", "Apple", "zzz"}, {"cpp"}, counts), "x")
        second = porter.render(porter.build_rows({"zzz", "Apple", "apple"}, {"cpp"}, counts), "x")
        self.assertEqual(first, second)
        rows = [line for line in first.splitlines() if not line.startswith("#")]
        self.assertEqual(rows[0].split("\t")[0], "apple")  # highest count first
        self.assertTrue(rows[-1].startswith("zzz\t1"))  # no Ngram match -> floor

    def test_verify_rejects_bad_rows(self):
        for bad in ("a\t0\n", "a\t1\ta\n", "a-b\t1\n", "a\t1\nA\t1\ttypo\n", "a\t1\na\t2\n",
                    "a\t4294967296\n", "a\t1"):
            with self.assertRaises(ValueError, msg=repr(bad)):
                porter.verify(bad)

    def test_committed_dictionary(self):
        path = ROOT / "data" / "english-words.tsv"
        text = path.read_text(encoding="utf-8")
        self.assertGreater(porter.verify(text), 50_000)
        self.assertIn(f"{porter.NGRAM_VERSION}", text.split("\n\n")[0])
        self.assertIn(f"counts-sha256: {porter.COUNTS_SHA256}", text)
        rows = [line.split("\t") for line in text.splitlines() if line and line[0] != "#"]
        frequencies = [int(row[1]) for row in rows]
        self.assertEqual(frequencies, sorted(frequencies, reverse=True))

    def test_license_texts_are_aggregated_verbatim(self):
        aggregate = (ROOT / "THIRD_PARTY_LICENSES").read_text(encoding="utf-8")
        for name in ("scowl-en_US-copyright.txt", "CC-BY-3.0.txt", "linguist-LICENSE.txt"):
            notice = (ROOT / "dictbuild" / "notices" / name).read_text(encoding="utf-8")
            self.assertIn(notice.replace("\r\n", "\n").rstrip("\n"),
                          aggregate.replace("\r\n", "\n"), name)


if __name__ == "__main__":
    unittest.main()
