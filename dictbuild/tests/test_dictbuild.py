import csv
from contextlib import redirect_stderr
import hashlib
import io
import json
from pathlib import Path
import shutil
import struct
import sys
import tempfile
import unittest
from unittest import mock
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import check_bundle
import build_bundled
import dictbuild
import extract_sudachi
from make_fixtures import generate


class BuilderTests(unittest.TestCase):
    def test_round_trip_reproducibility_and_corruption(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary)
            generate(path)
            dictbuild.verify((path / "valid.azdic").read_bytes())
            compatible = {"valid.azdic", "no_surface_index.azdic", "unknown_section.azdic"}
            for broken in path.glob("*.azdic"):
                if broken.name in compatible:
                    continue
                with self.subTest(name=broken.name), self.assertRaises(ValueError):
                    dictbuild.verify(broken.read_bytes())
            for broken in path.glob("key_order*.azdic"):
                with self.subTest(name=broken.name), self.assertRaisesRegex(ValueError, "invalid key order"):
                    dictbuild.verify(broken.read_bytes())
            for name in ("surface_index_order", "surface_index_duplicate", "surface_index_mismatch"):
                with self.subTest(name=name), self.assertRaisesRegex(ValueError, "invalid surface index order"):
                    dictbuild.verify((path / f"{name}.azdic").read_bytes())

    def test_surface_index_is_optional_and_stays_format_version_one(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary)
            generate(path)
            image = (path / "valid.azdic").read_bytes()
            self.assertEqual(struct.unpack_from("<H", image, 8)[0], 1)
            names = {image[64 + i * 24:68 + i * 24] for i in range(struct.unpack_from("<I", image, 28)[0])}
            self.assertIn(b"SIDX", names)
            dictbuild.verify((path / "no_surface_index.azdic").read_bytes())
            dictbuild.verify((path / "unknown_section.azdic").read_bytes())

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


class BundledBuildTests(unittest.TestCase):
    def archives(self) -> dict[str, bytes]:
        result = {}
        for name, rows in (
                ("small_lex.zip", [sudachi_row("東京", "4789", "3000", "東京",
                                             "名詞,固有名詞,地名,一般", "トウキョウ")]),
                ("core_lex.zip", [sudachi_row("東京", "4789", "2000", "東京",
                                            "名詞,固有名詞,地名,一般", "トウキョウ"),
                                  sudachi_row("佐藤", "4790", "4000", "佐藤",
                                             "名詞,固有名詞,人名,姓", "サトウ")])):
            csv_text = io.StringIO(newline="")
            csv.writer(csv_text).writerows(rows)
            zipped = io.BytesIO()
            with zipfile.ZipFile(zipped, "w") as archive:
                archive.writestr("lexicon.csv", csv_text.getvalue())
            result[name] = zipped.getvalue()
        return result

    def test_builds_all_layers_twice_with_pinned_cache_and_identical_notices(self):
        archives = self.archives()
        pins = tuple((name, hashlib.sha256(data).hexdigest()) for name, data in archives.items())
        downloads = []

        def download(url: str, timeout: int) -> io.BytesIO:
            downloads.append(url)
            self.assertEqual(timeout, 600)
            return io.BytesIO(archives[url.rsplit("/", 1)[-1]])

        with tempfile.TemporaryDirectory() as temporary, mock.patch.object(build_bundled, "SUDACHI_ARCHIVES", pins):
            path = Path(temporary)
            first, second, cache = path / "first", path / "different-path" / "second", path / "source-cache"
            with mock.patch.object(build_bundled.urllib.request, "urlopen", side_effect=download):
                build_bundled.build_bundle(first, cache, False)
            with mock.patch.object(build_bundled.urllib.request, "urlopen", side_effect=AssertionError("offline network")):
                build_bundled.build_bundle(second, cache, True)
            self.assertEqual(len(downloads), 2)
            self.assertEqual({p.name for p in (first / "dict").iterdir()},
                             {"sudachi_lexicon.azdic", "technical_terms_lexicon.azdic"})
            self.assertEqual(check_bundle.check(first, None), [])
            self.assertEqual(check_bundle.compare(first, second), [])
            notices = (first / "ThirdPartyNotices.txt").read_text(encoding="utf-8")
            for source in ("sudachidict-core", "azookey-technical-terms"):
                self.assertEqual(notices.count(source), 1)
            self.assertIn("Copyright (c) 2017-2023 Works Applications Co., Ltd.", notices)
            self.assertIn("Copyright (c) 2026 azooKey-Desktop contributors", notices)
            self.assertIn((build_bundled.HERE / "notices" / "sudachidict-legal.txt").read_text(encoding="utf-8"), notices)
            for artifact in (first / "dict").iterdir():
                image = artifact.read_bytes()
                dictbuild.verify(image)
                layer, _ = check_bundle.read_header(artifact)
                self.assertEqual(layer, dictbuild.LAYERS.index(artifact.stem))
                self.assertNotIn(str(path).encode("utf-8"), image)
            sudachi = (first / "dict" / "sudachi_lexicon.azdic").read_bytes()
            self.assertEqual(struct.unpack_from("<I", sudachi, 20)[0], 2)  # Duplicate Tokyo merged.
            # Metadata carries input names and hashes, never output/work/cache absolute paths.
            section_count = struct.unpack_from("<I", sudachi, 28)[0]
            sections = [struct.unpack_from("<4sIQQ", sudachi, 64 + 24 * i) for i in range(section_count)]
            _, _, offset, length = next(section for section in sections if section[0] == b"META")
            inputs = json.loads(sudachi[offset:offset + length])["inputs"]
            self.assertEqual([item["name"] for item in inputs], ["sudachi_lexicon.lex.tsv"])
            self.assertEqual(len(inputs[0]["sha256"]), 64)
            # A cached archive is still SHA256 checked in offline mode.
            (cache / build_bundled.SUDACHI_REVISION / "small_lex.zip").write_bytes(b"tampered")
            with self.assertRaisesRegex(ValueError, "pinned SHA256"):
                build_bundled.build_bundle(path / "tampered", cache, True)

    def test_authored_seed_keeps_default_cost_frequency_and_curated_categories(self):
        layer = build_bundled.TECHNICAL_LAYER
        source = build_bundled.HERE / "sources"
        metadata = json.loads((source / f"{layer}.metadata.json").read_text(encoding="utf-8"))
        entries = dictbuild.load_entries([source / f"{layer}.lex.tsv"], metadata)
        self.assertTrue(entries)
        self.assertTrue(all(e["cost"] == 5000 and "technical" in e["category"] for e in entries))
        tensor = next(e for e in entries if e["surface"] == "TensorRT")
        self.assertEqual(tensor["key"], "てんそるあーるてぃー")
        self.assertEqual(tensor["frequency"], .72)
        tsf = next(e for e in entries if e["surface"] == "TSF")
        self.assertEqual(tsf["frequency"], .3)
        image, notices = build_bundled.build_technical_terms()
        dictbuild.verify(image)
        self.assertIn("azookey-technical-terms", notices)
        self.assertIn("Apache License", notices)

    def test_rejects_an_online_download_with_the_wrong_pinned_hash(self):
        with tempfile.TemporaryDirectory() as temporary, mock.patch.object(
                build_bundled.urllib.request, "urlopen", return_value=io.BytesIO(b"wrong archive")):
            path = Path(temporary)
            with self.assertRaisesRegex(ValueError, "does not match the pinned"):
                build_bundled.fetch(path, "small_lex.zip", "0" * 64, False)
            self.assertFalse((path / build_bundled.SUDACHI_REVISION / "small_lex.part").exists())


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

    def test_reproducibility_checks_content_hash_all_bytes_notices_and_layer_set(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary)
            first = self.make_bundle(path)
            second = path / "second"
            shutil.copytree(first, second)
            self.assertEqual(check_bundle.compare(first, second), [])
            artifact = second / "dict" / "technical_terms_lexicon.azdic"
            image = artifact.read_bytes()
            changed = bytearray(image)
            changed[32] ^= 1
            artifact.write_bytes(changed)
            errors = "\n".join(check_bundle.compare(first, second))
            self.assertIn("content_hash differs", errors)
            self.assertIn("bytes differ", errors)
            changed = bytearray(image)
            changed[-1] ^= 1
            artifact.write_bytes(changed)
            errors = "\n".join(check_bundle.compare(first, second))
            self.assertNotIn("content_hash differs", errors)
            self.assertIn("bytes differ", errors)
            artifact.write_bytes(image)
            (second / "ThirdPartyNotices.txt").write_text("fixture\nMIT\nchanged\n", encoding="utf-8")
            self.assertEqual(check_bundle.compare(first, second),
                             ["ThirdPartyNotices.txt: bytes differ between builds"])
            artifact.unlink()
            self.assertTrue(any("dictionary names differ" in e for e in check_bundle.compare(first, second)))

    def test_cli_checks_the_second_build_for_forbidden_packs(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary)
            first = self.make_bundle(path)
            second = path / "second"
            shutil.copytree(first, second)
            image = (second / "dict" / "technical_terms_lexicon.azdic").read_bytes()
            (second / "dict" / "neologd_lexicon.azdic").write_bytes(image)
            stderr = io.StringIO()
            with mock.patch.object(sys, "argv", ["check_bundle.py", str(first), "--compare", str(second)]), \
                    redirect_stderr(stderr):
                self.assertEqual(check_bundle.main(), 1)
            self.assertIn("neologd_lexicon.azdic: not a bundled static layer", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
