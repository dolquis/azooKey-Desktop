import copy
import csv
from contextlib import redirect_stderr
import hashlib
import io
import json
import lzma
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import check_bundle
import build_bundled
import bundle_provenance
import dictbuild
import extract_neologd
import extract_sudachi
from make_fixtures import generate
from make_vm_bundle_fixture import generate as generate_vm_bundle
import neologd_pack


class BuilderTests(unittest.TestCase):
    def test_category_ids_match_cpp_dictionary_contract(self):
        header = (Path(__file__).resolve().parents[2] / "learning" / "include" /
                  "azookey" / "learning" / "DictionaryCategory.h").read_text(encoding="utf-8")
        cpp_ids = {
            re.sub(r"(?<!^)(?=[A-Z])", "_", name).lower(): int(category_id)
            for name, category_id in re.findall(r"^\s+(\w+) = (\d+),?$", header, re.MULTILINE)
        }
        self.assertEqual(cpp_ids, {name: index for index, name in enumerate(dictbuild.CATEGORIES)})

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
            ("url must be https", dict(manifest, url="https://example.invalid/パック.azdic")),
            ("url must be https", dict(manifest, url="https://example.invalid/a b.azdic")),
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


class VmBundleProvenanceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.repository = self.root / "repository"
        self.code = self.repository / "dictbuild"
        self.code.mkdir(parents=True)
        for name in (*bundle_provenance.CODE_INPUTS, "check_bundle.py"):
            shutil.copyfile(build_bundled.HERE / name, self.code / name)
        for directory in ("sources", "notices"):
            shutil.copytree(build_bundled.HERE / directory, self.code / directory)
        self.bundle = self.root / "bundle"
        self.build_here = mock.patch.object(build_bundled, "HERE", self.code)
        self.check_here = mock.patch.object(check_bundle, "HERE", self.code)
        self.build_here.start()
        self.check_here.start()
        self.addCleanup(self.build_here.stop)
        self.addCleanup(self.check_here.stop)
        with redirect_stderr(io.StringIO()):
            generate_vm_bundle(self.repository, self.bundle)

    def receipt(self):
        return self.bundle / bundle_provenance.RECEIPT

    def refresh_receipt(self):
        bundle_provenance.write_receipt(self.bundle, bundle_provenance.generation_inputs(self.repository),
                                        build_bundled.SUDACHI_REVISION, build_bundled.SUDACHI_ARCHIVES)

    def rewrite_meta(self, change):
        artifact = self.bundle / "dict" / "sudachi_lexicon.azdic"
        image = artifact.read_bytes()
        sections = []
        for i in range(struct.unpack_from("<I", image, 28)[0]):
            name, _, offset, length = struct.unpack_from("<4sIQQ", image, 64 + 24 * i)
            body = image[offset:offset + length]
            if name == b"META":
                meta = json.loads(body)
                change(meta)
                body = json.dumps(meta).encode("utf-8")
            sections.append((name, body))
        artifact.write_bytes(dictbuild.pack(sections, 1, struct.unpack_from("<I", image, 20)[0],
                                           struct.unpack_from("<I", image, 24)[0]))
        self.refresh_receipt()

    def test_accepts_valid_bundle_with_relative_deterministic_provenance(self):
        result = check_bundle.check_vm_package(self.bundle)
        self.assertEqual(set(result), {"schemaVersion", "sudachiRevision", "inputFingerprint", "provenanceSha256"})
        self.assertEqual(result["sudachiRevision"], "20260723")
        self.assertRegex(result["inputFingerprint"], r"^[a-f0-9]{64}$")
        self.assertRegex(result["provenanceSha256"], r"^[a-f0-9]{64}$")
        original = self.receipt().read_bytes()
        self.assertNotIn(str(self.root).encode(), original)
        with redirect_stderr(io.StringIO()):
            generate_vm_bundle(self.repository, self.root / "second")
        self.assertEqual(original, (self.root / "second" / bundle_provenance.RECEIPT).read_bytes())

    def test_vm_allowlist_ignores_extra_files_but_default_guard_rejects_them(self):
        (self.bundle / "dict" / "neologd_lexicon.azdic").write_bytes(b"not selected")
        (self.bundle / "unrelated.txt").write_text("ignored")
        check_bundle.check_vm_package(self.bundle)
        self.assertTrue(any("not a bundled static layer" in e for e in check_bundle.check(self.bundle, None)))

    def test_unrelated_repository_changes_do_not_invalidate_generation(self):
        (self.repository / "unrelated.txt").write_text("new commit")
        check_bundle.check_vm_package(self.bundle)

    def test_missing_receipt_fails(self):
        self.receipt().unlink()
        with self.assertRaisesRegex(ValueError, "missing generation receipt"):
            check_bundle.check_vm_package(self.bundle)

    def test_every_generation_input_is_bound(self):
        for relative in bundle_provenance.generation_inputs(self.repository):
            with self.subTest(input=relative):
                path = self.repository / relative
                original = path.read_bytes()
                path.write_bytes(original + b"\n")
                try:
                    with self.assertRaisesRegex(ValueError, "inputs does not match"):
                        check_bundle.check_vm_package(self.bundle)
                finally:
                    path.write_bytes(original)

    def test_receipt_fixed_fields_sets_pins_and_hashes_are_checked(self):
        original = json.loads(self.receipt().read_text(encoding="utf-8"))
        mutations = (
            lambda r: r.update(schemaVersion=True),
            lambda r: r.update(extra="unexpected"),
            lambda r: r["sudachi"].update(revision="old"),
            lambda r: r["sudachi"]["archives"].update({"small_lex.zip": "0" * 64}),
            lambda r: r["inputs"].pop("dictbuild/build_bundled.py"),
            lambda r: r["inputs"].update({str(self.root / "outside"): "0" * 64}),
            lambda r: r["artifacts"].update({"../../outside": "0" * 64}),
            lambda r: r["artifacts"].pop("ThirdPartyNotices.txt"),
            lambda r: r.update(inputFingerprint="0" * 64),
        )
        for mutate in mutations:
            with self.subTest(mutation=mutate):
                receipt = copy.deepcopy(original)
                mutate(receipt)
                self.receipt().write_text(json.dumps(receipt), encoding="utf-8")
                with self.assertRaises(ValueError):
                    check_bundle.check_vm_package(self.bundle)

    def test_duplicate_receipt_keys_fail(self):
        self.receipt().write_text('{"schemaVersion":1,"schemaVersion":1}', encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "duplicate provenance field"):
            check_bundle.check_vm_package(self.bundle)

    def test_all_selected_artifact_hashes_are_bound(self):
        for relative in bundle_provenance.ARTIFACTS:
            with self.subTest(artifact=relative):
                path = self.bundle / relative
                original = path.read_bytes()
                path.write_bytes(original + b"changed")
                try:
                    with self.assertRaisesRegex(ValueError, "artifacts does not match"):
                        check_bundle.check_vm_package(self.bundle)
                finally:
                    path.write_bytes(original)

    def test_full_binary_validation_even_if_receipt_hash_is_updated(self):
        artifact = self.bundle / "dict" / "sudachi_lexicon.azdic"
        original = artifact.read_bytes()
        mutations = {
            "magic": lambda image: image.__setitem__(0, 0),
            "version": lambda image: struct.pack_into("<H", image, 8, 2),
            "layer": lambda image: struct.pack_into("<I", image, 16, 4),
            "table": lambda image: struct.pack_into("<I", image, 28, 64),
            "content hash": lambda image: image.__setitem__(32, image[32] ^ 1),
            "truncation": lambda image: image.__delitem__(slice(-10, None)),
        }
        for name, mutate in mutations.items():
            with self.subTest(corruption=name):
                image = bytearray(original)
                mutate(image)
                artifact.write_bytes(image)
                self.refresh_receipt()
                with self.assertRaises(ValueError):
                    check_bundle.check_vm_package(self.bundle)

    def test_meta_sources_must_match_current_metadata(self):
        self.rewrite_meta(lambda meta: meta.update(sources=[]))
        with self.assertRaisesRegex(ValueError, "META sources"):
            check_bundle.check_vm_package(self.bundle)

    def test_meta_missing_sources_is_rejected(self):
        self.rewrite_meta(lambda meta: meta.pop("sources"))
        with self.assertRaises((ValueError, KeyError)):
            check_bundle.check_vm_package(self.bundle)

    def test_notices_must_include_full_source_attribution(self):
        (self.bundle / "ThirdPartyNotices.txt").write_text("sudachidict-core\nazookey-technical-terms\n", encoding="utf-8")
        self.refresh_receipt()
        with self.assertRaisesRegex(ValueError, "source attribution"):
            check_bundle.check_vm_package(self.bundle)

    def test_failed_rebuild_invalidates_previous_receipt(self):
        with mock.patch.object(build_bundled, "build_sudachi", side_effect=ValueError("failed build")):
            with self.assertRaisesRegex(ValueError, "failed build"):
                build_bundled.build_bundle(self.bundle, self.root / "cache", True)
        self.assertFalse(self.receipt().exists())

    def test_input_changes_during_build_prevent_receipt_publication(self):
        original = bundle_provenance.generation_inputs(self.repository)
        changed = dict(original, changed="0" * 64)
        with mock.patch.object(bundle_provenance, "generation_inputs", side_effect=[original, changed]), \
                redirect_stderr(io.StringIO()):
            with self.assertRaisesRegex(ValueError, "inputs changed during generation"):
                generate_vm_bundle(self.repository, self.bundle)
        self.assertFalse(self.receipt().exists())

    def test_receipt_publish_failure_cannot_leave_an_old_success(self):
        with mock.patch.object(bundle_provenance, "write_receipt", side_effect=OSError("publish failed")), \
                redirect_stderr(io.StringIO()):
            with self.assertRaisesRegex(OSError, "publish failed"):
                generate_vm_bundle(self.repository, self.bundle)
        self.assertFalse(self.receipt().exists())

    def test_validation_uses_one_snapshot_for_receipt_and_payload_hashes(self):
        expected = check_bundle.check_vm_package(self.bundle)
        original_read = Path.read_bytes
        reads = {}

        def read_once(path):
            relative = path.relative_to(self.bundle).as_posix() if path.is_relative_to(self.bundle) else None
            if relative in (*bundle_provenance.ARTIFACTS, bundle_provenance.RECEIPT):
                reads[relative] = reads.get(relative, 0) + 1
                self.assertEqual(reads[relative], 1, f"reread a changing file: {relative}")
            return original_read(path)

        with mock.patch.object(Path, "read_bytes", read_once):
            self.assertEqual(check_bundle.check_vm_package(self.bundle), expected)
        self.assertEqual(set(reads), {*bundle_provenance.ARTIFACTS, bundle_provenance.RECEIPT})

    def test_fixture_cli_imports_and_fingerprints_the_requested_checkout(self):
        helper = Path(__file__).resolve().parent / "make_vm_bundle_fixture.py"
        output = self.root / "from-cli"
        completed = subprocess.run([sys.executable, str(helper), "--repository", str(self.repository),
                                    "--output", str(output)], capture_output=True, text=True)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(json.loads((output / bundle_provenance.RECEIPT).read_text(encoding="utf-8"))["inputs"],
                         bundle_provenance.generation_inputs(self.repository))
        check_bundle.check_vm_package(output)

    def test_cli_outputs_only_json_and_errors_have_no_stdout(self):
        command = [sys.executable, str(self.code / "check_bundle.py"), str(self.bundle), "--vm-package-json"]
        accepted = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(accepted.returncode, 0, accepted.stderr)
        self.assertEqual(json.loads(accepted.stdout), check_bundle.check_vm_package(self.bundle))
        self.receipt().unlink()
        rejected = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(rejected.returncode, 1)
        self.assertEqual(rejected.stdout, "")
        self.assertIn("missing generation receipt", rejected.stderr)


if __name__ == "__main__":
    unittest.main()
