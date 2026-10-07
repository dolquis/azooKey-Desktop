"""Independently authored wire fixtures for the opt-in analysis tool."""

from collections import Counter
import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import analyze_base_scores as analysis


def wire_blob(reading, surfaces, scores, ids=None):
    ids = ids or [(1, 2, 3)] * len(scores)
    binary = struct.pack("<H", len(scores))
    for score, (left, right, mid) in zip(scores, ids):
        binary += struct.pack("<HHHf", left, right, mid, score)
    return binary + "\t".join([reading, *surfaces]).encode("utf-8")


def wire_file(*blobs):
    offset = 2 + 4 * len(blobs)
    table = b""
    for blob in blobs:
        table += struct.pack("<I", offset)
        offset += len(blob)
    return struct.pack("<H", len(blobs)) + table + b"".join(blobs)


class DecoderTests(unittest.TestCase):
    def test_little_endian_ids_score_and_empty_surface_fallback(self):
        payload = wire_file(wire_blob("ヨミ", ["表記", ""], [-7.25, -11.],
                                      [(65535, 0, 65535), (0, 65535, 0)]), b"\0\0")
        entries = list(analysis.parse_loudstxt3(payload))
        self.assertEqual([(e.reading, e.surface, e.score) for e in entries],
                         [("ヨミ", "表記", -7.25), ("ヨミ", "ヨミ", -11.)])
        self.assertEqual((entries[0].lcid, entries[0].rcid, entries[0].mid), (65535, 0, 65535))
        self.assertEqual(list(analysis.parse_loudstxt3(b"\0\0")), [])

    def test_supplied_id_bounds_and_invalid_bounds(self):
        for identifiers in ((1319, 0, 0), (0, 1319, 0), (0, 0, 502)):
            payload = wire_file(wire_blob("ヨミ", ["語"], [-2.], [identifiers]))
            with self.subTest(ids=identifiers), self.assertRaisesRegex(ValueError, "ID outside"):
                list(analysis.parse_loudstxt3(payload, max_cid=1318, max_mid=501))
        with self.assertRaisesRegex(ValueError, "invalid ID bounds"):
            list(analysis.parse_loudstxt3(b"\0\0", max_cid=-1))

    def test_offset_and_numeric_table_bounds(self):
        valid = wire_file(wire_blob("ヨミ", ["語"], [-3.]))
        bad = [b"", b"\1", b"\1\0", valid[:5], b"\0\0x",
               valid[:2] + struct.pack("<I", 0) + valid[6:],
               valid[:2] + struct.pack("<I", 0xFFFFFFFF) + valid[6:],
               valid[:6] + struct.pack("<H", 65535) + valid[8:],
               wire_file(b"\1\0" + b"\0" * 9)]
        two = wire_file(b"\0\0", b"\0\0")
        bad.append(two[:6] + struct.pack("<I", 10) + two[10:])
        for payload in bad:
            with self.subTest(payload=payload), self.assertRaises(ValueError):
                list(analysis.parse_loudstxt3(payload))

    def test_nonfinite_scores_rejected_and_finite_outliers_retained(self):
        for score in (float("nan"), float("inf"), float("-inf")):
            with self.subTest(score=score), self.assertRaisesRegex(ValueError, "nonfinite"):
                list(analysis.parse_loudstxt3(wire_file(wire_blob("ヨミ", ["語"], [score]))))
        scores = [-48.75, 2., -3.4028234663852886e38]
        entries = list(analysis.parse_loudstxt3(wire_file(wire_blob("ヨミ", ["甲", "乙", "丙"], scores))))
        self.assertEqual([e.score for e in entries], scores)

    def test_utf8_field_count_empty_reading_and_nul(self):
        good = wire_blob("ヨミ", ["語"], [-2.])
        malformed = [good[:12] + b"\xff\tword", good + b"\textra",
                     wire_blob("", ["語"], [-2.]), wire_blob("ヨミ", ["\0"], [-2.]),
                     b"\0\0extra"]
        for blob in malformed:
            with self.subTest(blob=blob), self.assertRaises(ValueError):
                list(analysis.parse_loudstxt3(wire_file(blob)))


class ScoreAnalysisTests(unittest.TestCase):
    def test_pinned_input_accepts_matching_cache_and_rejects_tampering(self):
        payload = b"independently authored analysis fixture"
        expected = hashlib.sha256(payload).hexdigest()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "input.bin"
            path.write_bytes(payload)
            result = analysis.pinned_input(
                path, "https://example.invalid/analysis-fixture", expected, False)
            self.assertEqual(result["sha256"], expected)
            self.assertEqual(result["size_bytes"], len(payload))
            path.write_bytes(payload + b" changed")
            with self.assertRaisesRegex(ValueError, "SHA256 mismatch"):
                analysis.pinned_input(path, "https://example.invalid/analysis-fixture", expected, False)
            path.unlink()
            with self.assertRaisesRegex(ValueError, "use --download explicitly"):
                analysis.pinned_input(path, "https://example.invalid/analysis-fixture", expected, False)

    def test_observed_affine_strict_fixed_clipping_and_cost_new_ties(self):
        scores = Counter({-48.75: 1, -40.: 1, -3.: 1, -2.99999: 2, -1.5634: 1})
        values, formulas = analysis.mapping_values(scores)
        self.assertEqual(set(values["constant_comparison"]["float64"].values()), {.3})
        observed = values["observed_affine_comparison"]["float64"]
        self.assertAlmostEqual(observed[min(scores)], .05)
        self.assertAlmostEqual(observed[max(scores)], .95)
        self.assertFalse(formulas["observed_max_is_zero"])
        self.assertTrue(analysis.monotonicity(scores, observed)["strictly_monotonic_on_distinct_observed_scores"])
        fixed = analysis.monotonicity(scores, values["fixed_affine_comparison"]["float64"])
        self.assertTrue(fixed["weakly_monotonic_on_observed_scores"])
        self.assertFalse(fixed["strictly_monotonic_on_distinct_observed_scores"])
        self.assertGreater(fixed["new_tie_groups"], 0)
        cost = analysis.monotonicity(scores, values["observed_affine_comparison"]["cost_int_reconstructed_frequency"])
        self.assertGreater(cost["adjacent_distinct_score_collapses"], 0)

    def test_float32_introduced_ties_are_visible(self):
        scores = Counter({-48.75: 1, -2.: 1, -2. + 1e-7: 1, -1.5634: 1})
        values, _ = analysis.mapping_values(scores)
        result = analysis.monotonicity(scores, values["observed_affine_comparison"]["float32"])
        self.assertGreater(result["adjacent_distinct_score_collapses"], 0)
        self.assertTrue(result["weakly_monotonic_on_observed_scores"])

    def test_normalization_dedup_and_deterministic_ties(self):
        groups = {}
        analysis.put(groups, "カナＡ", "語", -6.)
        analysis.put(groups, "かなA", "語", -3.)
        self.assertEqual(groups, {"かなA": {"語": -3.}})
        self.assertEqual(analysis.ranking({"乙": .5, "甲": .5}), ["乙", "甲"])
        reference = {"よみ": {"乙": -2., "甲": -1., "丙": -3.}}
        actual = {"よみ": {"甲": .5, "丙": .5, "乙": .5}}
        result = analysis.compare_readings(reference, actual)
        self.assertEqual(result["top1_changed"], 1)
        self.assertEqual(result["mean_top5_overlap_fraction"], 1.)
        self.assertEqual(json.dumps(result, sort_keys=True), json.dumps(analysis.compare_readings(reference, actual), sort_keys=True))

    def test_layer_dedup_uses_priority_and_given_frequency(self):
        merged, winners, counts = analysis.merged_reading({"共通": -2., "Base": -3.},
                                                         {"共通": .4, "Sudachi": .5},
                                                         {-2.: .6, -3.: .3})
        self.assertEqual(winners["共通"], "base")
        self.assertAlmostEqual(merged["共通"], .9)
        self.assertEqual(counts["dedup_base_winner"], 1)
        _merged, winners, _counts = analysis.merged_reading({"共通": -2.}, {"共通": .6}, {-2.: .6})
        self.assertEqual(winners["共通"], "sudachi")

    def test_q16_frequency_half_up_endpoints_and_bounds(self):
        self.assertEqual(analysis.frequency_wire_q16(0.), 0.)
        self.assertEqual(analysis.frequency_wire_q16(1.), 1.)
        self.assertEqual(analysis.frequency_wire_q16(.5), 32768 / 65535.)
        self.assertEqual(analysis.frequency_wire_q16(2.5 / 65535.), 3 / 65535.)
        for invalid in (-1e-12, 1.000000001, float("nan"), float("inf")):
            with self.subTest(value=invalid), self.assertRaisesRegex(ValueError, "Q16 bounds"):
                analysis.frequency_wire_q16(invalid)

    def test_q16_storage_changes_shared_surface_layer_winner(self):
        values, _ = analysis.mapping_values(Counter({-30.: 1, -12.: 1, 0.: 1}))
        # -12 fixed-affine -> .6; cost 3000 -> .5. Unrounded values tie,
        # preserving the earlier Base layer. Current Q16 storage raises .5.
        base = {"共通": -12.}
        _, old_winners, _ = analysis.merged_reading(
            base, {"共通": .5}, values["fixed_affine_comparison"]["float64"])
        self.assertEqual(old_winners["共通"], "base")
        merged, winners, _ = analysis.merged_reading(
            base, {"共通": analysis.frequency_wire_q16((8000 - 3000) / 10000)},
            values["fixed_affine_comparison"]["frequency_wire_q16"])
        self.assertEqual(winners["共通"], "sudachi")
        self.assertAlmostEqual(merged["共通"], 32768 / 65535. + .30 + .10)

    def test_q16_new_ties_and_lexical_top5_tie_handling(self):
        scores = Counter({-30.: 1, -12.: 1, -11.99999: 1, 0.: 1})
        values, _ = analysis.mapping_values(scores)
        projected = values["fixed_affine_comparison"]["frequency_wire_q16"]
        result = analysis.monotonicity(scores, projected)
        self.assertTrue(result["weakly_monotonic_on_observed_scores"])
        self.assertFalse(result["strictly_monotonic_on_distinct_observed_scores"])
        self.assertGreater(result["new_tie_groups"], 0)
        raw = {"よみ": {"乙": -12., "甲": -11.99999}}
        actual = analysis.project_groups(raw, projected)
        self.assertEqual(analysis.ranking(raw["よみ"]), ["甲", "乙"])
        self.assertEqual(analysis.ranking(actual["よみ"]), ["乙", "甲"])
        merged, winners, _ = analysis.merged_reading({"丙": -12., "乙": -12.},
                                                   {}, projected)
        self.assertEqual(analysis.ranking(merged), ["丙", "乙"])
        self.assertEqual(set(winners.values()), {"base"})

    def test_single_score_range_fails_instead_of_silent_default(self):
        with self.assertRaisesRegex(ValueError, "nonzero score range"):
            analysis.mapping_values(Counter({-2.: 3}))

    def test_evaluation_mixed_schema_uses_observed_reading_exactly(self):
        values, _ = analysis.mapping_values(Counter({-8.: 1, -2.: 1}))
        rows = [{"id": "clean", "input": "にほん", "expected": "日本", "acceptable": []},
                {"id": "typo", "observed_reading": "にほ", "intended_reading": "にほん",
                 "expected_surface": "日本"}]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "evaluation.jsonl"
            path.write_text("\n".join(json.dumps(row) for row in rows), encoding="utf-8")
            summary, details = analysis.evaluate(path, {"にほん": {"日本": -2.}}, {}, values)
            for result in summary["scenarios"].values():
                self.assertEqual(result["rows"], 2)
                self.assertEqual(result["dictionary_only_top5_hits"], 1)
            typo = [row for row in details if row["id"] == "typo"]
            self.assertTrue(all(row["top5"] == [] for row in typo))
            self.assertTrue(all(row["input_field"] == "observed_reading" for row in typo))
            path.write_text(json.dumps({"id": "bad"}), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "invalid evaluation row bad"):
                analysis.evaluate(path, {}, {}, values)


if __name__ == "__main__":
    unittest.main()
