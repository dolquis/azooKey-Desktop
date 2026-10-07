import copy
import hashlib
import json
import lzma
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import check_bundle
import dictbuild
import extract_neologd
import extract_sudachi
from make_fixtures import generate
import neologd_pack


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


def neologd_row(surface: str, cost: str, pos: str, reading: str) -> list[str]:
    fields = pos.split(",") + ["*"] * (4 - len(pos.split(",")))
    return [surface, "1288", "1288", cost, *fields, "*", "*", surface, reading, reading]


# Synthetic rows in the seed layout; never upstream data.
NEOLOGD_ROWS = [
    neologd_row("架空新語", "3000", "名詞,固有名詞,一般", "カクウシンゴ"),
    neologd_row("試験花子", "4000", "名詞,固有名詞,人名,一般", "シケンハナコ"),
    neologd_row("架空市", "4500", "名詞,固有名詞,地域,一般", "カクウシ"),
    neologd_row("架空工業", "5000", "名詞,固有名詞,組織", "カクウコウギョウ"),
    neologd_row("ためし", "5000", "名詞,一般", "タメシ"),  # surface equals reading
    neologd_row("ＡＢＣ", "5000", "名詞,固有名詞,一般", "ＡＢＣ"),  # not a kana reading
    neologd_row("#架空", "5000", "名詞,固有名詞,一般", "シャープカクウ"),
]


class NeologdExtractTests(unittest.TestCase):
    def test_keeps_reading_lookups_and_maps_proper_noun_categories(self):
        self.assertEqual(list(extract_neologd.convert(NEOLOGD_ROWS)), [
            ("架空新語", "かくうしんご", "名詞-固有名詞-一般", 3000, "neologism"),
            ("試験花子", "しけんはなこ", "名詞-固有名詞-人名-一般", 4000, "person_name"),
            ("架空市", "かくうし", "名詞-固有名詞-地域-一般", 4500, "place_name"),
            ("架空工業", "かくうこうぎょう", "名詞-固有名詞-組織", 5000, "company_org"),
        ])

    def test_rejects_unexpected_columns(self):
        with self.assertRaises(ValueError):
            list(extract_neologd.convert([["架空", "1"]]))

    def test_reads_xz_seed_into_an_attributed_tsv(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary)
            seed = path / "seed.csv.xz"
            seed.write_bytes(lzma.compress("\n".join(",".join(r) for r in NEOLOGD_ROWS).encode("utf-8")))
            output = path / "neologd.lex.tsv"
            self.assertEqual(extract_neologd.write_tsv([seed], "20991231", output), 4)
            metadata = neologd_pack.with_revision(
                json.loads(neologd_pack.METADATA.read_text(encoding="utf-8")), "20991231")
            self.assertEqual(len(dictbuild.load_entries([output], metadata)), 4)


class NeologdPackTests(unittest.TestCase):
    URL = "https://example.invalid/neologd_lexicon-20991231.azdic"

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.path = Path(self.temporary.name)
        self.metadata = json.loads(neologd_pack.METADATA.read_text(encoding="utf-8"))
        self.tsv = self.path / "neologd.lex.tsv"
        seed = self.path / "seed.csv"
        seed.write_text("\n".join(",".join(r) for r in NEOLOGD_ROWS) + "\n", encoding="utf-8")
        extract_neologd.write_tsv([seed], "20991231", self.tsv)

    def tearDown(self):
        self.temporary.cleanup()

    def build(self) -> tuple[dict, Path]:
        output = self.path / "out"
        manifest = neologd_pack.build_pack([self.tsv], self.metadata, "20991231",
                                           neologd_pack.CATALOG, output, self.URL)
        return manifest, output / manifest["file_name"]

    def test_round_trip_names_layer_two_and_carries_attribution(self):
        manifest, pack = self.build()
        self.assertEqual(pack.name, "neologd_lexicon-20991231.azdic")
        image = pack.read_bytes()
        dictbuild.verify(image)
        self.assertEqual(struct.unpack_from("<I", image, 16)[0], 2)
        self.assertEqual(manifest["size"], len(image))
        self.assertEqual(manifest["sha256"], hashlib.sha256(image).hexdigest())
        written = json.loads((pack.parent / "neologd_lexicon.manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(written, manifest)
        sources = neologd_pack.read_meta(image)["sources"]
        self.assertEqual(sources, manifest["attribution"]["sources"])
        self.assertEqual(sources[0]["upstream_revision"], "20991231")
        self.assertIn("はてなキーワード", manifest["attribution"]["notices"])
        self.assertIn("Apache License", manifest["attribution"]["notices"])
        again, _ = self.build()
        self.assertEqual(again, manifest)
        self.assertTrue(neologd_pack.validate_manifest(manifest))

    def test_verify_accepts_the_pack_and_rejects_each_mismatch(self):
        manifest, pack = self.build()
        neologd_pack.verify_pack(manifest, pack)
        image = pack.read_bytes()
        cases = {}
        cases["pack size mismatch"] = (manifest, image + b"\0")
        flipped = bytearray(image)
        flipped[-1] ^= 1
        cases["pack sha256 mismatch"] = (manifest, bytes(flipped))
        corrupt = dict(manifest, sha256=hashlib.sha256(bytes(flipped)).hexdigest())
        cases["invalid table or hash"] = (corrupt, bytes(flipped))
        generate(self.path / "fixture")
        other = (self.path / "fixture" / "valid.azdic").read_bytes()
        cases["pack layer id mismatch"] = (
            dict(manifest, size=len(other), sha256=hashlib.sha256(other).hexdigest()), other)
        foreign = copy.deepcopy(manifest)
        foreign["attribution"]["sources"][0]["upstream_revision"] = "20000101"
        cases["pack attribution mismatch"] = (foreign, image)
        sections = []
        for i in range(struct.unpack_from("<I", image, 28)[0]):
            name, _, offset, length = struct.unpack_from("<4sIQQ", image, 64 + i * 24)
            sections.append((name, image[offset:offset + length]))
        meta = dict(neologd_pack.read_meta(image), builder_version="azdic-2")
        sections = [(n, json.dumps(meta).encode("utf-8") if n == b"META" else d) for n, d in sections]
        header = struct.unpack_from("<8sHHIIIIIQ", image)
        rebuilt = dictbuild.pack(sections, 2, header[5], header[6])
        cases["pack builder version mismatch"] = (
            dict(manifest, size=len(rebuilt), sha256=hashlib.sha256(rebuilt).hexdigest()), rebuilt)
        for message, (case, data) in cases.items():
            with self.subTest(message):
                target = self.path / "candidate.azdic"
                target.write_bytes(data)
                with self.assertRaisesRegex(ValueError, message):
                    neologd_pack.verify_pack(case, target)

    def test_manifest_validation(self):
        manifest, _ = self.build()
        unpublished = neologd_pack.make_manifest(self.metadata, neologd_pack.CATALOG)
        self.assertFalse(neologd_pack.validate_manifest(unpublished))
        with self.assertRaisesRegex(ValueError, "no pack is published"):
            neologd_pack.verify_pack(unpublished, self.path / "missing.azdic")
        bad = [
            ("fields do not match", {k: v for k, v in manifest.items() if k != "url"}),
            ("unsupported manifest layer_id", dict(manifest, layer_id=4)),
            ("unsupported manifest format_version", dict(manifest, format_version=2)),
            ("url must be https", dict(manifest, url="http://example.invalid/pack.azdic")),
            # Only one of url and sha256 set is a published manifest with a missing value.
            ("url must be https", dict(manifest, url="")),
            ("sha256 must be 64", dict(manifest, sha256="")),
            ("sha256 must be 64", dict(manifest, sha256=manifest["sha256"].upper())),
            ("versioned pack", dict(manifest, file_name="neologd_lexicon.azdic")),
            ("must not name a pack", dict(unpublished, size=10)),
            ("size must be a non-negative", dict(manifest, size=True)),
            ("attribution is missing", dict(manifest, attribution={"sources": []})),
        ]
        for message, case in bad:
            with self.subTest(message), self.assertRaisesRegex(ValueError, message):
                neologd_pack.validate_manifest(case)
        for revision in ("", "../x", "a/b", "x" * 65):
            with self.subTest(revision=revision), self.assertRaises(ValueError):
                neologd_pack.pack_file_name(revision)

    def test_pinned_manifest_is_unpublished_and_matches_the_metadata(self):
        pinned = json.loads(neologd_pack.PINNED.read_text(encoding="utf-8"))
        self.assertFalse(neologd_pack.validate_manifest(pinned))
        neologd_pack.check_pinned(pinned, self.metadata, neologd_pack.CATALOG)
        drifted = copy.deepcopy(pinned)
        drifted["attribution"]["notices"] += "edited\n"
        with self.assertRaisesRegex(ValueError, "attribution drifted"):
            neologd_pack.check_pinned(drifted, self.metadata, neologd_pack.CATALOG)
        manifest, _ = self.build()
        neologd_pack.check_pinned(manifest, self.metadata, neologd_pack.CATALOG)
        stale = copy.deepcopy(manifest)
        stale["attribution"]["sources"][0]["copyright"] = "edited"
        with self.assertRaisesRegex(ValueError, "attribution drifted"):
            neologd_pack.check_pinned(stale, self.metadata, neologd_pack.CATALOG)

    def test_cli_exit_codes(self):
        script = str(Path(neologd_pack.__file__))
        run = lambda *args: subprocess.run([sys.executable, script, *map(str, args)],
                                           capture_output=True, text=True, encoding="utf-8")
        self.assertEqual(run("check-pinned").returncode, 0)
        output = self.path / "cli"
        built = run("build", self.tsv, "--revision", "20991231", "--output", output, "--url", self.URL)
        self.assertEqual(built.returncode, 0, built.stderr)
        manifest = output / "neologd_lexicon.manifest.json"
        self.assertEqual(run("verify", manifest, output / "neologd_lexicon-20991231.azdic").returncode, 0)
        failed = run("verify", manifest, self.tsv)
        self.assertEqual(failed.returncode, 1)
        self.assertIn("pack size mismatch", failed.stderr)


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

    def test_rejects_versioned_neologd_pack_and_manifest(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary)
            bundle = self.make_bundle(path)
            (bundle / "dict" / "neologd_lexicon-20991231.azdic").write_bytes(
                (bundle / "dict" / "technical_terms_lexicon.azdic").read_bytes())
            (bundle / "dict" / "neologd_lexicon.manifest.json").write_text("{}", encoding="utf-8")
            errors = "\n".join(check_bundle.check(bundle, None))
            self.assertIn("neologd_lexicon-20991231.azdic: not a bundled static layer", errors)
            self.assertIn("neologd_lexicon.manifest.json: not a bundled static layer", errors)
            package = path / "Package.wxs"
            package.write_text('<Wix xmlns="http://wixtoolset.org/schemas/v4/wxs"><Package>'
                               '<File Source="$(BundledDictionaryDir)\\dict\\technical_terms_lexicon.azdic" />'
                               '<File Source="$(PackDir)\\neologd_lexicon.manifest.json" /></Package></Wix>',
                               encoding="utf-8")
            clean = self.make_bundle(path / "clean")
            self.assertTrue(any("Package.wxs ships" in e for e in check_bundle.check(clean, package)))

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
