"""Fixed SCOWL/linguist-style excerpts (authored fixtures) and the committed english-words.tsv.

The data file derives from SCOWL, Google Books Ngram (CC-BY-3.0) and linguist (MIT);
see ../../THIRD_PARTY_LICENSES.
"""

import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

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

    # Host rule (EnglishCandidates.cpp): the entries of a lowercase key decide the half-width
    # order; any acronym entry puts the uppercase form first, otherwise any proper entry the
    # capitalized one. Dictionary surfaces come out by frequency, highest first.
    @staticmethod
    def host_order(rows, key):
        entries = [r for r in rows if r[0].lower() == key]
        entries.sort(key=lambda r: -int(r[1]))
        flags = {f for r in entries for f in (r[2].split(",") if len(r) > 2 else [])}
        if "acronym" in flags:
            forms = [key.upper(), key, key.capitalize()]
        elif "proper" in flags:
            forms = [key.capitalize(), key, key.upper()]
        else:
            forms = [key, key.capitalize(), key.upper()]
        return [r[0] for r in entries], forms

    def test_common_words_lead_with_lowercase(self):
        text = (ROOT / "data" / "english-words.tsv").read_text(encoding="utf-8")
        rows = [line.split("\t") for line in text.splitlines() if line and line[0] != "#"]
        for key in ("in it or on so who us am act care a he can will may no the is".split()):
            surfaces, forms = self.host_order(rows, key)
            self.assertEqual(surfaces[0], key, key)
            self.assertEqual(forms[0], key, key)
        _surfaces, forms = self.host_order(rows, "nasa")
        self.assertEqual(forms[0], "NASA")

    def test_case_flags_depend_on_the_lowercase_sibling(self):
        counts = {"in": 1000, "In": 50, "IN": 10, "bros": 10, "Bros": 500, "NASA": 7}
        rows = {r[0]: r for r in porter.build_rows(
            {"in", "In", "IN", "bros", "Bros", "NASA"}, set(), counts)}
        self.assertEqual(rows["In"][2] + rows["IN"][2], "")
        self.assertEqual(rows["Bros"][2], "proper")  # 50 times the lowercase count
        self.assertEqual(rows["NASA"][2], "acronym")  # no lowercase sibling

    def test_tech_drops_ordinary_words(self):
        scowl = {"be", "make", "Python", "python"}
        tech = porter.select_tech({"be", "Make", "cpp", "Python"}, scowl)
        self.assertNotIn("be", tech)
        self.assertNotIn("Make", tech)
        self.assertIn("cpp", tech)
        self.assertIn("GitHub", tech)  # hand-written terms are always kept

    def test_window_boundaries(self):
        self.assertEqual(porter.window_total(b"1989,5,1\t1990,7,1\t2019,11,1\t2020,13,1"), 18)
        self.assertEqual(porter.window_total(b"1989,5,1\t2020,13,1"), 0)

    def test_fetch_refuses_a_mismatching_download(self):
        class Response:
            def __enter__(self):
                return self

            def __exit__(self, *args):
                return False

            def read(self):
                return b"tampered"

        with tempfile.TemporaryDirectory() as directory:
            cache = Path(directory) / "x.zip"
            with mock.patch.object(porter.urllib.request, "urlopen", return_value=Response()):
                with self.assertRaises(ValueError):
                    porter.fetch("https://example.invalid/x", "0" * 64, cache)
            self.assertFalse(cache.exists())  # an unverified download is not cached
            cache.write_bytes(b"stale")
            with self.assertRaises(ValueError):
                porter.fetch("https://example.invalid/x", "0" * 64, cache)

    def test_entry_limit(self):
        def word(i):
            return "".join(chr(97 + i // 26 ** k % 26) for k in range(5))

        words = {word(i) for i in range(porter.MAX_ENTRIES + 1)}
        with self.assertRaises(ValueError):
            porter.build_rows(words, set(), {})
        rows = "".join(f"{word(i)}\t1\n" for i in range(porter.MAX_ENTRIES + 1))
        with self.assertRaises(ValueError):
            porter.verify(rows)

    def test_pins_are_complete(self):
        self.assertEqual(len(porter.NGRAM_SHARD_SHA256), porter.NGRAM_SHARDS)
        self.assertEqual(len(porter.NGRAM_SHARD_BYTES), porter.NGRAM_SHARDS)
        self.assertRegex(porter.COUNTS_SHA256, r"^[0-9a-f]{64}$")

    def test_license_texts_are_aggregated_verbatim(self):
        aggregate = (ROOT / "THIRD_PARTY_LICENSES").read_text(encoding="utf-8")
        for name in ("scowl-en_US-copyright.txt", "CC-BY-3.0.txt", "linguist-LICENSE.txt"):
            notice = (ROOT / "dictbuild" / "notices" / name).read_text(encoding="utf-8")
            self.assertIn(notice.replace("\r\n", "\n").rstrip("\n"),
                          aggregate.replace("\r\n", "\n"), name)


if __name__ == "__main__":
    unittest.main()
