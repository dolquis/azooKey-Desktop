from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import check_bundle
import dictbuild
import extract_sudachi
from make_fixtures import generate


class BuilderTests(unittest.TestCase):
    def test_round_trip_reproducibility_and_corruption(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary)
            generate(path)
            dictbuild.verify((path / "valid.azdic").read_bytes())
            for broken in path.glob("*.azdic"):
                if broken.name == "valid.azdic":
                    continue
                with self.subTest(name=broken.name), self.assertRaises(ValueError):
                    dictbuild.verify(broken.read_bytes())
            for broken in path.glob("key_order*.azdic"):
                with self.subTest(name=broken.name), self.assertRaisesRegex(ValueError, "invalid key order"):
                    dictbuild.verify(broken.read_bytes())

    def test_normalization_aliases_and_limit(self):
        self.assertEqual(dictbuild.normalize("カタカナＡ１"), "かたかなA1")
        self.assertEqual(dictbuild.aliases("ば"), ["ば", "ゔぁ"])
        self.assertEqual(dictbuild.aliases("づづづづ"), ["づづづづ"])

    def test_attribution_is_mandatory(self):
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaises(ValueError):
                dictbuild.validate_sources({"sources": [{}]}, Path(temporary))

    def test_multiple_inputs_deduplicate_and_union_categories(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary)
            generate(path)
            source = dict(source_id="fixture", spdx="MIT", upstream_url="https://example.invalid/fixture",
                          upstream_revision="1", transform="synthetic", copyright="Test fixture", notice_ids=[])
            other = path / "other.lex.tsv"
            other.write_text("# SPDX-License-Identifier: MIT; THIRD_PARTY_LICENSES\n"
                             "surface\treading\tpos\tcost\tfrequency\tcategory\tsource_id\n"
                             "東京\tとうきょう\t別品詞\t1000\t0.9\tstation_name\tfixture\n", encoding="utf-8")
            inputs = [path / "fixture.lex.tsv", other]
            image, _ = dictbuild.build(inputs, {"sources": [source]}, 4, path)
            reverse, _ = dictbuild.build(list(reversed(inputs)), {"sources": [source]}, 4, path)
            self.assertEqual(image, reverse)
            entries = dictbuild.load_entries(inputs, {"sources": [source]})
            tokyo = next(e for e in entries if e["surface"] == "東京")
            self.assertEqual(tokyo["frequency"], .9)
            self.assertEqual(tokyo["cost"], 1000)
            self.assertEqual(tokyo["category"], {"place_name", "station_name"})
            source["notice_ids"] = ["missing"]
            with self.assertRaises(OSError):
                dictbuild.build(inputs, {"sources": [source]}, 4, path)


def sudachi_row(headword: str, left: str, cost: str, surface: str, pos: str, reading: str) -> list[str]:
    fields = pos.split(",") + ["*"] * (6 - len(pos.split(",")))
    return [headword, left, left, cost, surface, *fields, reading, surface, "*", "A", "*", "*", "*", "*"]


class SudachiExtractTests(unittest.TestCase):
    def test_keeps_reading_lookups_and_drops_the_rest(self):
        rows = [
            sudachi_row("東京", "4789", "3000", "東京", "名詞,固有名詞,地名,一般", "トウキョウ"),
            sudachi_row("佐藤", "4790", "4000", "佐藤", "名詞,固有名詞,人名,姓", "サトウ"),
            sudachi_row("& co.\\u002cltd.", "5139", "5000", "& Co.\\u002CLtd.", "名詞,普通名詞,一般",
                        "アンドコーエルティーディー"),
            sudachi_row("ヴァイオリン", "5139", "5000", "ヴァイオリン", "名詞,普通名詞,一般", "ヴァイオリン"),
            sudachi_row("100", "-1", "0", "100", "名詞,数詞", "ヒャク"),  # split-only entry
            sudachi_row("、", "5980", "0", "、", "補助記号,読点", "、"),
            sudachi_row("$", "5969", "2784", "$", "名詞,普通名詞,一般", "キゴウ$"),
            sudachi_row("ひらがな", "5139", "5000", "ひらがな", "名詞,普通名詞,一般", "ヒラガナ"),
            sudachi_row("稀語", "5139", "10001", "稀語", "名詞,普通名詞,一般", "マレゴ"),
            sudachi_row("\"q\"", "5139", "5000", "\"q\"", "名詞,普通名詞,一般", "キュー"),
            sudachi_row("#九", "4785", "6000", "#九", "名詞,固有名詞,一般", "シャープキュウ"),
        ]
        self.assertEqual(list(extract_sudachi.convert(rows)), [
            ("東京", "とうきょう", "名詞-固有名詞-地名-一般", 3000, "place_name"),
            ("佐藤", "さとう", "名詞-固有名詞-人名-姓", 4000, "person_name"),
            ("& Co.,Ltd.", "あんどこーえるてぃーでぃー", "名詞-普通名詞-一般", 5000, "general"),
            ("ヴァイオリン", "ゔぁいおりん", "名詞-普通名詞-一般", 5000, "general"),
        ])

    def test_rejects_unexpected_columns(self):
        with self.assertRaises(ValueError):
            list(extract_sudachi.convert([["東京", "1"]]))


class BundleGuardTests(unittest.TestCase):
    def make_bundle(self, path: Path) -> Path:
        generate(path / "fixture")
        bundle = path / "bundle"
        (bundle / "dict").mkdir(parents=True)
        (bundle / "dict" / "technical_terms_lexicon.azdic").write_bytes(
            (path / "fixture" / "valid.azdic").read_bytes())
        (bundle / "ThirdPartyNotices.txt").write_text("fixture\nMIT\n", encoding="utf-8")
        return bundle

    def write_package(self, path: Path, *names: str) -> Path:
        package = path / "Package.wxs"
        files = "".join(f'<File Source="$(BundledDictionaryDir)\\dict\\{name}" />' for name in names)
        package.write_text('<Wix xmlns="http://wixtoolset.org/schemas/v4/wxs">'
                           f'<Package>{files}<File Source="$(HostExePath)" /></Package></Wix>',
                           encoding="utf-8")
        return package

    def test_accepts_a_consistent_bundle(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary)
            bundle = self.make_bundle(path)
            package = self.write_package(path, "technical_terms_lexicon.azdic")
            self.assertEqual(check_bundle.check(bundle, package), [])

    def test_rejects_neologd_pack_wrong_layer_missing_notice_and_package_drift(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary)
            bundle = self.make_bundle(path)
            image = (bundle / "dict" / "technical_terms_lexicon.azdic").read_bytes()
            (bundle / "dict" / "neologd_lexicon.azdic").write_bytes(image)
            (bundle / "dict" / "base_lexicon.azdic").write_bytes(image)
            (bundle / "ThirdPartyNotices.txt").write_text("unrelated\n", encoding="utf-8")
            package = self.write_package(path, "technical_terms_lexicon.azdic")
            errors = "\n".join(check_bundle.check(bundle, package))
            self.assertIn("neologd_lexicon.azdic: not a bundled static layer", errors)
            self.assertIn("base_lexicon.azdic: layer id 4 does not match", errors)
            self.assertIn("source fixture is not attributed", errors)
            self.assertIn("Package.wxs ships", errors)

    def test_compares_the_installed_name_not_the_source_name(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary)
            bundle = self.make_bundle(path)
            package = path / "Package.wxs"
            package.write_text('<Wix xmlns="http://wixtoolset.org/schemas/v4/wxs"><Package>'
                               '<File Source="$(BundledDictionaryDir)\\dict\\technical_terms_lexicon.azdic" '
                               'Name="base_lexicon.azdic" /></Package></Wix>', encoding="utf-8")
            self.assertTrue(any("Package.wxs ships" in e for e in check_bundle.check(bundle, package)))

    def test_rejects_an_empty_bundle(self):
        with tempfile.TemporaryDirectory() as temporary:
            errors = check_bundle.check(Path(temporary), None)
            self.assertTrue(any("no bundled dictionary" in e for e in errors))
            self.assertTrue(any("ThirdPartyNotices.txt" in e for e in errors))


if __name__ == "__main__":
    unittest.main()
