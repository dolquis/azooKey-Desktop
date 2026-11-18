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
            compatible = {"valid.azdic", "no_surface_index.azdic", "unknown_section.azdic",
                          "connections.azdic", "lattice_core.azdic"}
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

    def test_skips_common_entries_spelled_with_a_variant_glyph(self):
        def row(surface, normalized, pos, reading):
            built = sudachi_row(surface, "5139", "5000", surface, pos, reading)
            built[12] = normalized
            return built

        rows = [
            row("硏究", "研究", "名詞,普通名詞,サ変可能", "ケンキュウ"),
            row("研究", "研究", "名詞,普通名詞,サ変可能", "ケンキュウ"),
            row("仁俠", "任侠", "名詞,普通名詞,一般", "ニンキョウ"),
            row("俠", "俠", "名詞,普通名詞,一般", "キョウ"),
            row("俠田", "侠田", "名詞,固有名詞,人名,姓", "キョウダ"),  # proper noun
            row("塼", "塼", "名詞,普通名詞,一般", "セン"),  # no normalization evidence
            row("剝がす", "剥がす", "動詞,一般", "ハガス"),  # Joyo standard form: kept
            row("頰", "頬", "名詞,普通名詞,一般", "ホオ"),
            row("𠮟る", "叱る", "動詞,一般", "シカル"),
            row("嚙む", "噛む", "動詞,一般", "カム"),  # in the set: skipped
            row("長い", "長い", "形容詞,一般", "ナガイ"),
        ]
        variants = extract_sudachi.variant_characters(rows)
        self.assertEqual(variants, frozenset("硏俠嚙"))
        self.assertFalse(variants & extract_sudachi.STANDARD_FORM_EXCEPTIONS)
        self.assertEqual([entry[0] for entry in extract_sudachi.convert(rows, variants=variants)],
                         ["研究", "俠田", "塼", "剝がす", "頰", "𠮟る", "長い"])
        self.assertEqual(len(list(extract_sudachi.convert(rows))), len(rows))

    def test_variant_characters_need_equal_length_and_a_cp932_standard_form(self):
        rows = [sudachi_row("硏究", "5139", "5000", "硏究", "名詞,普通名詞,一般", "ケンキュウ")]
        rows[0][12] = "研究所"  # different length: the positions do not line up
        self.assertEqual(extract_sudachi.variant_characters(rows), frozenset())
        rows[0][12] = "硏究"
        self.assertEqual(extract_sudachi.variant_characters(rows), frozenset())

    def test_rejects_unexpected_columns(self):
        with self.assertRaises(ValueError):
            list(extract_sudachi.convert([["東京", "1"]]))

    def connection_rows(self) -> list[list[str]]:
        rows = [
            sudachi_row("東京", "4789", "3000", "東京", "名詞,固有名詞,地名,一般", "トウキョウ"),
            sudachi_row("ひらがな", "5139", "5000", "ひらがな", "名詞,普通名詞,一般", "ヒラガナ"),
            sudachi_row("書く", "700", "4000", "書く", "動詞,一般,*,*,五段-カ行,終止形-一般", "カク"),
            sudachi_row("コーヒー", "5142", "3000", "コーヒー", "名詞,普通名詞,一般", "コーヒー"),
            sudachi_row("稀語", "5139", "10001", "稀語", "名詞,普通名詞,一般", "マレゴ"),  # above max cost
            sudachi_row("、", "5980", "0", "、", "補助記号,読点", "、"),
        ]
        rows[2][2] = "701"  # A right id that differs from the left id.
        rows[3][12] = "\\u73c8琲"  # The upstream normalized form, escaped like the other columns.
        return rows

    def test_connection_rows_keep_kana_words_and_all_six_part_of_speech_fields(self):
        rows = self.connection_rows()
        self.assertEqual([entry[0] for entry in extract_sudachi.convert(rows)], ["東京", "書く", "コーヒー"])
        self.assertEqual(list(extract_sudachi.convert(rows, kana_only=True)),
                         [("ひらがな", "ひらがな", "名詞-普通名詞-一般", 5000, "general")])
        self.assertEqual(list(extract_sudachi.convert_connections(rows)), [
            ("とうきょう", "東京", "4789", "4789", "3000", "名詞-固有名詞-地名-一般", "", "東京",
             "名詞,固有名詞,地名,一般,*,*"),
            ("ひらがな", "ひらがな", "5139", "5139", "5000", "名詞-普通名詞-一般", "", "ひらがな",
             "名詞,普通名詞,一般,*,*,*"),
            ("かく", "書く", "700", "701", "4000", "動詞-一般", "五段-カ行-終止形-一般", "書く",
             "動詞,一般,*,*,五段-カ行,終止形-一般"),
            ("こーひー", "コーヒー", "5142", "5142", "3000", "名詞-普通名詞-一般", "", "珈琲",
             "名詞,普通名詞,一般,*,*,*"),
        ])
        broken = self.connection_rows()
        broken[0][12] = "東\t京"
        with self.assertRaisesRegex(ValueError, "normalized form"):
            list(extract_sudachi.convert_connections(broken))

    def test_writes_the_lexicon_kana_and_connection_tsvs_in_one_pass(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary)
            source = path / "small_lex.csv"
            with source.open("w", encoding="utf-8", newline="") as stream:
                csv.writer(stream).writerows(self.connection_rows())
            lexicon, kana, connections = path / "a.lex.tsv", path / "kana.lex.tsv", path / "a.conn.tsv"
            self.assertEqual(extract_sudachi.write_tsv([source], "20991231", lexicon,
                                                       connections=connections, kana=kana), 3)
            metadata = json.loads((build_bundled.HERE / "sources" / "sudachi_lexicon.metadata.json")
                                  .read_text(encoding="utf-8"))
            self.assertEqual([e["surface"] for e in dictbuild.load_entries([lexicon], metadata)],
                             ["書く", "コーヒー", "東京"])
            self.assertEqual([e["surface"] for e in dictbuild.load_entries([kana], metadata)], ["ひらがな"])
            rows = dictbuild.read_connections(connections)
            self.assertEqual([(r.key, r.surface, r.left_id, r.right_id, r.cost, r.kana_only) for r in rows], [
                ("とうきょう", "東京", 4789, 4789, 3000, False), ("ひらがな", "ひらがな", 5139, 5139, 5000, True),
                ("かく", "書く", 700, 701, 4000, False), ("こーひー", "コーヒー", 5142, 5142, 3000, False)])
            text = connections.read_text(encoding="utf-8")
            self.assertIn("SPDX-License-Identifier: Apache-2.0", text)
            self.assertIn("\n" + "\t".join(dictbuild.CONNECTION_COLUMNS) + "\n", text)


SYNTHETIC_HEADER = "# SPDX-License-Identifier: MIT; THIRD_PARTY_LICENSES; synthetic fixture\n"


class ConnectionTests(unittest.TestCase):
    """CONN, CMAT and lattice_core (azdic spec sections 15.5 and 15.12) on synthetic data."""

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.path = Path(self.temporary.name)
        (self.path / "MIT.txt").write_text("Synthetic test license text.\n", encoding="utf-8")
        self.metadata = {"sources": [dict(source_id="fixture", spdx="MIT",
                                          upstream_url="https://example.invalid/fixture",
                                          upstream_revision="1", transform="synthetic",
                                          copyright="Test fixture", notice_ids=[])]}

    def lexicon(self, name: str, rows: list[tuple]) -> Path:
        path = self.path / name
        path.write_text(SYNTHETIC_HEADER + "surface\treading\tpos\tcost\tfrequency\tcategory\tsource_id\n" +
                        "".join(f"{s}\t{r}\t{p}\t{c}\t\tgeneral\tfixture\n" for s, r, p, c in rows),
                        encoding="utf-8", newline="\n")
        return path

    def connections(self, rows: list[tuple]) -> Path:
        """rows: (reading, surface, left, right, cost, six POS fields, normalized form)."""
        path = self.path / "words.conn.tsv"
        lines = []
        for reading, surface, left, right, cost, pos, normalized in rows:
            fields = pos.split(",")
            lines.append("\t".join(map(str, (
                reading, surface, left, right, cost, "-".join(f for f in fields[:4] if f != "*"),
                "-".join(f for f in fields[4:] if f != "*"), normalized, pos))) + "\n")
        path.write_text(SYNTHETIC_HEADER + "\t".join(dictbuild.CONNECTION_COLUMNS) + "\n" + "".join(lines),
                        encoding="utf-8", newline="\n")
        return path

    def matrix(self, header: str, cells: dict[tuple[int, int], int]) -> Path:
        path = self.path / "matrix.def"
        path.write_text(matrix_text(header, cells), encoding="utf-8", newline="\n")
        return path

    NOUN = "名詞,普通名詞,一般,*,*,*"
    WORDS = [("東京", "とうきょう", "名詞-固有名詞-地名-一般", 3000), ("コーヒー", "こーひー", "名詞-普通名詞-一般", 3000),
             ("珈琲", "こーひー", "名詞-普通名詞-一般", 4000), ("ヴァイオリン", "ゔぁいおりん", "名詞-普通名詞-一般", 5000)]
    LINKS = [
        ("とうきょう", "東京", 12, 13, 3000, "名詞,固有名詞,地名,一般,*,*", "東京"),
        ("とうきょう", "東京", 11, 11, 3000, "名詞,固有名詞,地名,一般,*,*", "東京"),
        ("とうきょう", "東京", 11, 11, 3000, "名詞,固有名詞,地名,一般,*,*", "東京"),  # Identical: kept once.
        ("こーひー", "コーヒー", 20, 20, 3000, NOUN, "珈琲"),  # Katakana variant of 珈琲.
        ("こーひー", "珈琲", 20, 20, 4000, NOUN, "珈琲"),
        ("ゔぁいおりん", "ヴァイオリン", 20, 20, -5000, NOUN, "ヴァイオリン"),  # Katakana word itself.
        ("は", "は", 40, 40, 3846, "助詞,係助詞,*,*,*,*", "は"),  # Kana word: lattice_core only.
    ]

    def build_words(self) -> tuple[bytes, bytes]:
        words = self.lexicon("words.lex.tsv", self.WORDS)
        plain, _ = dictbuild.build([words], self.metadata, 1, self.path)
        linked, _ = dictbuild.build([words], self.metadata, 1, self.path, self.connections(self.LINKS),
                                    "sudachidict-matrix:" + "0" * 64)
        return plain, linked

    def variants(self, image: bytes) -> list[list[tuple[int, int, int, int]]]:
        count = struct.unpack_from("<I", image, 20)[0]
        data = sections_of(image)[b"CONN"]
        begin = struct.unpack_from(f"<{count + 1}I", data)
        offset = 4 * (count + 1) + (-4 * (count + 1) % 8)
        records = list(struct.iter_unpack("<HHhH", data[offset:]))
        return [records[begin[i]:begin[i + 1]] for i in range(count)]

    def test_conn_keeps_each_variant_and_leaves_the_lookup_sections_unchanged(self):
        plain, linked = self.build_words()
        dictbuild.verify(linked)
        before, after = sections_of(plain), sections_of(linked)
        for name in (b"TRIE", b"KEYS", b"EIDX", b"ENTS", b"STRS", b"SIDX"):
            self.assertEqual(before[name], after[name], name)
        self.assertNotIn(b"CONN", before)
        self.assertNotIn(b"CMAT", after)
        meta = json.loads(after[b"META"])
        self.assertEqual(meta["pos_table"], json.loads(before[b"META"])["pos_table"])
        # Code point order: 固 U+56FA before 普 U+666E.
        self.assertEqual(meta["conn_pos_table"], ["名詞,固有名詞,地名,一般,*,*", self.NOUN])
        self.assertEqual(meta["connection_space"], "sudachidict-matrix:" + "0" * 64)
        self.assertEqual([item["name"] for item in meta["inputs"]], ["words.conn.tsv", "words.lex.tsv"])
        # ENTS order is (key, surface): こーひー/コーヒー, こーひー/珈琲, とうきょう/東京, ゔぁいおりん/ヴァイオリン.
        katakana, other = dictbuild.KATAKANA_ONLY, dictbuild.NOT_NORMALIZED
        self.assertEqual(self.variants(linked), [
            [(20, 20, 3000, 1 | katakana | other)],
            [(20, 20, 4000, 1)],
            [(11, 11, 3000, 0), (12, 13, 3000, 0)],  # Same cost: ordered by left id.
            [(20, 20, -5000, 1 | katakana)],
        ])

    def test_conn_rejects_rows_without_an_entry_and_a_bad_space(self):
        words = self.lexicon("words.lex.tsv", self.WORDS[:1])
        with self.assertRaisesRegex(ValueError, "connection row without a lexicon entry"):
            dictbuild.build([words], self.metadata, 1, self.path, self.connections(self.LINKS),
                            "sudachidict-matrix:" + "0" * 64)
        with self.assertRaisesRegex(ValueError, "invalid connection space"):
            dictbuild.build([words], self.metadata, 1, self.path, self.connections(self.LINKS[:2]), "other")

    def rebuild(self, image: bytes, change) -> bytes:
        sections = sections_of(image)
        change(sections)
        return dictbuild.pack(list(sections.items()), struct.unpack_from("<I", image, 16)[0],
                              *struct.unpack_from("<II", image, 20))

    def test_verify_rejects_variant_order_attr_and_table_corruption(self):
        _, linked = self.build_words()
        count = struct.unpack_from("<I", linked, 20)[0]
        offset = 4 * (count + 1) + (-4 * (count + 1) % 8)

        def records(change):
            def apply(sections):
                data = bytearray(sections[b"CONN"])
                change(data)
                sections[b"CONN"] = bytes(data)
            return apply

        def meta(change):
            def apply(sections):
                value = json.loads(sections[b"META"])
                change(value)
                sections[b"META"] = json.dumps(value).encode("utf-8")
            return apply

        def swap_tokyo(data):  # Entry 2 holds the two 東京 variants, records 2 and 3.
            first, second = offset + 16, offset + 24
            data[first:first + 8], data[second:second + 8] = data[second:second + 8], data[first:first + 8]

        cases = {
            "invalid connection variant order": records(swap_tokyo),
            "invalid connection variant attr": records(lambda d: d.__setitem__(offset + 7, d[offset + 7] ^ 0x40)),
            "invalid connection variant attr ": records(lambda d: struct.pack_into("<H", d, offset + 14, 2)),
            "invalid connection table": records(lambda d: struct.pack_into("<I", d, 4, 5)),
            "invalid connection table ": records(lambda d: d.extend(b"\0" * 8)),
            "invalid conn_pos_table": meta(lambda m: m.pop("conn_pos_table")),
            "invalid connection_space": meta(lambda m: m.update(connection_space="")),
        }
        for message, change in cases.items():
            with self.subTest(message), self.assertRaisesRegex(ValueError, f"^{re.escape(message.strip())}$"):
                dictbuild.verify(self.rebuild(linked, change))

    def test_matrix_keeps_previous_right_by_next_left_cells_of_the_used_ids(self):
        # Header and rows put the previous word's right id first: 4 right ids, 3 left ids.
        matrix = self.matrix("4 3", {(r, l): 10 * r + l for r in range(4) for l in range(3)})
        data = dictbuild.matrix_section(dictbuild.read_matrix(matrix), {2}, {3})
        raw_left, raw_right, left_count, right_count, *reserved = struct.unpack_from("<6I", data)
        self.assertEqual((raw_left, raw_right, left_count, right_count, reserved), (3, 4, 2, 2, [0, 0]))
        self.assertEqual(struct.unpack_from("<3H", data, 24), (0, 0xFFFF, 1))
        self.assertEqual(struct.unpack_from("<4H", data, 30), (0, 0xFFFF, 0xFFFF, 1))
        # cost[r x left_count + l]: (r, l) = (0, 0), (0, 2), (3, 0), (3, 2).
        self.assertEqual(struct.unpack_from("<4h", data, 40), (0, 2, 30, 32))
        self.assertEqual(len(data), 48)
        left, right = dictbuild.matrix_indices(data)
        self.assertEqual((list(left), list(right)), ([0, 0xFFFF, 1], [0, 0xFFFF, 0xFFFF, 1]))

    def test_matrix_rejects_missing_duplicate_and_out_of_range_cells(self):
        cells = {(r, l): 1 for r in range(3) for l in range(3)}
        cases = {
            "missing cells": matrix_text("3 3", {k: v for k, v in cells.items() if k != (2, 2)}),
            "duplicate cell": matrix_text("3 3", cells) + "2 2 5\n",
            "outside the header sizes": matrix_text("3 3", cells) + "3 0 1\n",
            "cost range": matrix_text("3 3", {**cells, (1, 1): 40000}),
            "invalid header": "3\n0 0 0\n",
            "invalid row": "3 3\n0 0\n",
        }
        for message, text in cases.items():
            with self.subTest(message), self.assertRaisesRegex(ValueError, message):
                dictbuild.matrix_section(iter(text.splitlines(keepends=True)), {2}, {2})
        with self.assertRaisesRegex(ValueError, "connection id outside matrix.def"):
            dictbuild.matrix_section(iter(matrix_text("3 3", cells).splitlines(keepends=True)), {3}, {1})

    def lattice_rows(self) -> list[tuple]:
        verb = "動詞,一般,*,*,五段-カ行,"
        return [
            ("は", "は", 40, 40, 3846, "助詞,係助詞,*,*,*,*", "は"),
            ("かく", "かく", 700, 700, 9000, verb + "終止形-一般", "書く"),
            ("かく", "かく", 710, 710, 9000, verb + "終止形-一般", "書く"),
            ("かく", "書く", 710, 710, 4000, verb + "終止形-一般", "書く"),
            ("かく", "書く", 700, 700, 4100, verb + "終止形-一般", "書く"),
            # More rows in another form must not move the conjugation-free representative.
            *[(f"か{i}", f"書{i}", 720, 721, 4000, verb + "連用形-一般", "書") for i in range(5)],
            ("ひと", "人", 5142, 5142, 3000, self.NOUN, "人"),
            ("ひと", "ひと", 5142, 5142, 5000, self.NOUN, "人"),
            ("もの", "物", 5143, 5143, 7000, self.NOUN, "物"),
            ("こと", "事", 5142, 5142, 9000, self.NOUN, "事"),
            ("さ", "さ", 900, 901, 6000, "接尾辞,名詞的,一般,*,形容詞,連用形-一般", "さ"),
        ]

    def build_lattice(self, rows=None) -> bytes:
        rows = rows or self.lattice_rows()
        kana = self.lexicon("lattice_core.lex.tsv", sorted({(r[1], r[0], "x", r[4]) for r in rows if r[0] == r[1]}))
        ids = sorted({0, *(r[2] for r in rows), *(r[3] for r in rows)})
        matrix = self.matrix("6000 6000", {(r, l): (r * 7 + l * 3) % 200 - 100 for r in ids for l in ids})
        image, _ = dictbuild.build_lattice_core([kana], self.connections(rows), matrix, self.metadata, 1, self.path)
        return image

    def test_lattice_core_holds_kana_words_cmat_and_the_part_of_speech_tables(self):
        image = self.build_lattice()
        self.assertEqual(image, self.build_lattice())  # Same input, same bytes.
        dictbuild.verify(image)
        self.assertEqual(struct.unpack_from("<II", image, 16), (1, 4))  # layer sudachi_lexicon; かく は ひと さ
        sections = sections_of(image)
        meta = json.loads(sections[b"META"])
        self.assertEqual(meta["connection_space"], "sudachidict-matrix:" + dictbuild.file_sha256(self.path / "matrix.def"))
        lattice = meta["lattice"]
        # Code point order: 助 U+52A9, 動 U+52D5, 名 U+540D, 接 U+63A5.
        self.assertEqual(lattice["pos_names"], ["助詞-係助詞", "動詞-一般", "名詞-普通名詞-一般", "接尾辞-名詞的-一般"])
        self.assertEqual(lattice["pos_ids"], [
            [0, "*", "*", 40, 40],
            [1, "五段-カ行", "終止形-一般", 700, 700],  # (700, 700) and (710, 710) tie at two rows each.
            [1, "五段-カ行", "連用形-一般", 720, 721],
            [2, "*", "*", 5142, 5142],
            [3, "形容詞", "連用形-一般", 900, 901],
        ])
        # Conjugation unknown: the 終止形-一般 rows, or the "*" rows; 接尾辞 has neither.
        self.assertEqual(lattice["pos_name_ids"], [[40, 40], [700, 700], [5142, 5142], None])
        self.assertEqual(lattice["penalties"], {"P_kata_variant": 6000, "P_kata_own": 0, "P_node": 3000, "P_unk": 20000})
        self.assertEqual(lattice["C"], dictbuild.LATTICE_C)
        self.assertEqual(lattice["default_word_cost"], 5000)  # median_low of 3000, 5000, 7000, 9000
        # CMAT holds id 0 and every id of every row, whichever layer the word goes to.
        raw_left, raw_right, left_count, right_count = struct.unpack_from("<4I", sections[b"CMAT"])
        self.assertEqual((raw_left, raw_right, left_count, right_count), (6000, 6000, 8, 8))
        # Only the kana words: かく (two variants), さ, は, ひと.
        self.assertEqual([len(v) for v in self.variants(image)], [2, 1, 1, 1])

    def test_lattice_core_requires_connection_rows_for_every_kana_word(self):
        rows = self.lattice_rows()
        words = sorted({(r[1], r[0], "x", r[4]) for r in rows if r[0] == r[1]}) + [("ほか", "ほか", "x", 1)]
        kana = self.lexicon("lattice_core.lex.tsv", words)
        ids = sorted({0, *(r[2] for r in rows), *(r[3] for r in rows)})
        matrix = self.matrix("6000 6000", {(r, l): 0 for r in ids for l in ids})
        with self.assertRaisesRegex(ValueError, "^lexicon entry without connection rows$"):
            dictbuild.build_lattice_core([kana], self.connections(rows), matrix, self.metadata, 1, self.path)

    def test_verify_rejects_cmat_corruption_and_ids_outside_it(self):
        image = self.build_lattice()

        def cmat(change):
            def apply(sections):
                data = bytearray(sections[b"CMAT"])
                change(data)
                sections[b"CMAT"] = bytes(data)
            return apply

        cases = {
            "invalid connection matrix": cmat(lambda d: struct.pack_into("<I", d, 16, 1)),
            "invalid connection matrix size": cmat(lambda d: d.extend(b"\0" * 8)),
            "invalid connection matrix index": cmat(lambda d: struct.pack_into("<H", d, 24 + 2 * 40, 9)),
            "invalid connection matrix index ": cmat(lambda d: struct.pack_into("<H", d, 24, 0xFFFF)),
            "connection id outside CMAT": cmat(lambda d: struct.pack_into("<H", d, 24 + 2 * 40, 0xFFFF)),
        }
        for message, change in cases.items():
            with self.subTest(message), self.assertRaisesRegex(ValueError, f"^{re.escape(message.strip())}$"):
                dictbuild.verify(self.rebuild(image, change))


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


def zipped(name: str, text: str) -> bytes:
    data = io.BytesIO()
    with zipfile.ZipFile(data, "w") as archive:
        archive.writestr(name, text)
    return data.getvalue()


def sections_of(image: bytes) -> dict[bytes, bytes]:
    result = {}
    for i in range(struct.unpack_from("<I", image, 28)[0]):
        name, _, offset, length = struct.unpack_from("<4sIQQ", image, 64 + 24 * i)
        result[name] = image[offset:offset + length]
    return result


def matrix_text(header: str, cells: dict[tuple[int, int], int]) -> str:
    """matrix.def lines: right id of the previous word, left id of the next word, cost."""
    return header + "\n" + "".join(f"{r} {l} {c}\n" for (r, l), c in sorted(cells.items()))


class BundledBuildTests(unittest.TestCase):
    def archives(self) -> dict[str, bytes]:
        result = {}
        for name, rows in (
                ("small_lex.zip", [sudachi_row("東京", "4789", "3000", "東京",
                                             "名詞,固有名詞,地名,一般", "トウキョウ"),
                                   sudachi_row("は", "662", "3846", "は", "助詞,係助詞", "ハ")]),
                ("core_lex.zip", [sudachi_row("東京", "4789", "2000", "東京",
                                            "名詞,固有名詞,地名,一般", "トウキョウ"),
                                  sudachi_row("佐藤", "4790", "4000", "佐藤",
                                             "名詞,固有名詞,人名,姓", "サトウ"),
                                  sudachi_row("東", "5142", "5000", "東", "名詞,普通名詞,一般", "ヒガシ")])):
            csv_text = io.StringIO(newline="")
            csv.writer(csv_text).writerows(rows)
            result[name] = zipped("lexicon.csv", csv_text.getvalue())
        ids = (0, 662, 4789, 4790, 5142)
        result["matrix.def.zip"] = zipped("matrix.def", matrix_text(
            "5981 5981", {(r, l): r - l for r in ids for l in ids}))
        return result

    def test_builds_all_layers_twice_with_pinned_cache_and_identical_notices(self):
        archives = self.archives()
        pins = tuple((name, hashlib.sha256(data).hexdigest()) for name, data in archives.items())
        downloads = []

        def download(url: str, timeout: int) -> io.BytesIO:
            downloads.append(url)
            self.assertEqual(timeout, 600)
            return io.BytesIO(archives[url.rsplit("/", 1)[-1]])

        with tempfile.TemporaryDirectory() as temporary, \
                mock.patch.object(build_bundled, "SUDACHI_ARCHIVES", pins[:2]), \
                mock.patch.object(build_bundled, "MATRIX_ARCHIVE", pins[2]), \
                mock.patch.object(build_bundled, "PINNED_ARCHIVES", pins):
            path = Path(temporary)
            first, second, cache = path / "first", path / "different-path" / "second", path / "source-cache"
            with mock.patch.object(build_bundled.urllib.request, "urlopen", side_effect=download):
                build_bundled.build_bundle(first, cache, False)
            with mock.patch.object(build_bundled.urllib.request, "urlopen", side_effect=AssertionError("offline network")):
                build_bundled.build_bundle(second, cache, True)
            # The matrix sits directly under the raw distribution, outside the release directory.
            self.assertEqual(sorted(url.removeprefix(build_bundled.SUDACHI_BASE_URL) for url in downloads),
                             ["/20260723/core_lex.zip", "/20260723/small_lex.zip", "/matrix.def.zip"])
            self.assertEqual({p.name for p in (first / "dict").iterdir()},
                             {"sudachi_lexicon.azdic", "lattice_core.azdic", "technical_terms_lexicon.azdic"})
            self.assertEqual(check_bundle.check(first, None), [])
            self.assertEqual(check_bundle.compare(first, second), [])
            notices = (first / "ThirdPartyNotices.txt").read_text(encoding="utf-8")
            for source in ("sudachidict-core", "azookey-technical-terms"):
                self.assertEqual(notices.count(source), 1)
            self.assertIn("Copyright (c) 2017-2023 Works Applications Co., Ltd.", notices)
            self.assertIn("Copyright (c) 2026 azooKey-Desktop contributors", notices)
            self.assertIn((build_bundled.HERE / "notices" / "sudachidict-legal.txt").read_text(encoding="utf-8"), notices)
            self.assertIn(f"lattice_core.azdic\nDerived from the SudachiDict 20260723 raw lexicon and the "
                          f"SudachiDict matrix.def.zip (SHA256 {pins[2][1]}), part of UniDic", notices)
            self.assertIn("Attribution: the same as sudachi_lexicon.azdic above.", notices)
            for artifact in (first / "dict").iterdir():
                image = artifact.read_bytes()
                dictbuild.verify(image)
                layer, _ = check_bundle.read_header(artifact)
                self.assertEqual(layer, check_bundle.BUNDLED[artifact.name])
                self.assertNotIn(str(path).encode("utf-8"), image)
            sudachi = (first / "dict" / "sudachi_lexicon.azdic").read_bytes()
            self.assertEqual(struct.unpack_from("<I", sudachi, 20)[0], 3)  # Duplicate Tokyo merged.
            # Metadata carries input names and hashes, never output/work/cache absolute paths.
            meta = json.loads(sections_of(sudachi)[b"META"])
            self.assertEqual([item["name"] for item in meta["inputs"]],
                             ["sudachi_lexicon.conn.tsv", "sudachi_lexicon.lex.tsv"])
            self.assertTrue(all(len(item["sha256"]) == 64 for item in meta["inputs"]))
            space = "sudachidict-matrix:" + pins[2][1]
            self.assertEqual(meta["connection_space"], space)
            lattice = (first / "dict" / "lattice_core.azdic").read_bytes()
            lattice_meta = json.loads(sections_of(lattice)[b"META"])
            self.assertEqual(lattice_meta["connection_space"], space)
            self.assertEqual(lattice_meta["sources"], build_bundled.sudachi_metadata()["sources"])
            self.assertEqual([item["name"] for item in lattice_meta["inputs"]],
                             ["lattice_core.lex.tsv", "matrix.def.zip", "sudachi_lexicon.conn.tsv"])
            self.assertEqual(struct.unpack_from("<I", lattice, 20)[0], 1)  # は
            # CMAT keeps id 0 and every id of both layers, nothing else of the 5981 x 5981 matrix.
            self.assertEqual(struct.unpack_from("<4I", sections_of(lattice)[b"CMAT"]), (5981, 5981, 5, 5))
            receipt = json.loads((first / bundle_provenance.RECEIPT).read_text(encoding="utf-8"))
            self.assertEqual(receipt["sudachi"]["archives"], dict(pins))
            self.assertIn("dict/lattice_core.azdic", receipt["artifacts"])
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

    def test_accepts_lattice_core_only_under_the_sudachi_layer_id(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary)
            bundle = self.make_bundle(path)
            (bundle / "dict" / "lattice_core.azdic").write_bytes((path / "fixture" / "lattice_core.azdic").read_bytes())
            self.assertEqual(check_bundle.check(bundle, None), [])
            (bundle / "dict" / "lattice_core.azdic").write_bytes((path / "fixture" / "valid.azdic").read_bytes())
            self.assertIn("lattice_core.azdic: layer id 4 does not match the file name",
                          check_bundle.check(bundle, None))

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
                                        build_bundled.SUDACHI_REVISION, build_bundled.PINNED_ARCHIVES)

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
            lambda r: r["sudachi"]["archives"].pop("matrix.def.zip"),
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
