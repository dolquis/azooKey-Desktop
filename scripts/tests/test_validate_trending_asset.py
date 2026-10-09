"""Runtime-contract and original-byte checksum tests for unpublished trending assets."""

import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[1] / "validate_trending_asset.py"
SPEC = importlib.util.spec_from_file_location("validate_trending_asset", SCRIPT)
VALIDATOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VALIDATOR)


def word(surface="新語", reading="しんご", rank=1):
    return {"surface": surface, "reading": reading, "rank": rank}


def asset(words=None, **overrides):
    root = {"version": 1, "generated_at": "2026-10-09T00:00:00Z",
            "words": [word()] if words is None else words}
    root.update(overrides)
    return json.dumps(root, ensure_ascii=False).encode("utf-8")


class ValidateTrendingAssetTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "candidate.json"
        self.output = self.root / "trending-words.json.sha256"

    def reject(self, data):
        with self.assertRaises(VALIDATOR.AssetError):
            VALIDATOR.validate_asset_bytes(data)

    def test_valid_unicode_and_allowed_kana_marks(self):
        reading = "".join(chr(cp) for cp in
                          (0x3041, 0x309F, 0x30A0, 0x30FF, 0x3099, 0x309A, 0x30FC, 0x30FB))
        data = asset([word("漢字🙂𠮷", reading)])
        result = VALIDATOR.validate_asset_bytes(data)
        self.assertEqual(result["accepted"], 1)
        self.assertEqual(result["sha256"], hashlib.sha256(data).hexdigest())

    def test_byte_limits_for_asset_fields_and_generated_at(self):
        for data in (asset([word("a" * 256)]), asset([word(reading="あ" * 85)]),
                     asset(generated_at="日" * 21 + "x")):
            with self.subTest(size=len(data)):
                self.assertEqual(VALIDATOR.validate_asset_bytes(data)["accepted"], 1)
        for data in (asset([word("a" * 257)]), asset([word(reading="あ" * 86)]),
                     asset(generated_at="日" * 21 + "xx"), asset(generated_at="")):
            with self.subTest(size=len(data)):
                self.reject(data)
        data = asset()
        self.assertEqual(VALIDATOR.validate_asset_bytes(
            data + b" " * (VALIDATOR.MAX_ASSET_BYTES - len(data)))["accepted"], 1)
        self.reject(data + b" " * (VALIDATOR.MAX_ASSET_BYTES + 1 - len(data)))
        self.reject(b"")

    def test_word_count_includes_skipped_readings(self):
        for skipped in (False, True):
            with self.subTest(skipped=skipped):
                words = [word()]
                words.extend(({"reading": ""} if i % 2 else {}) if skipped else word()
                             for i in range(9999))
                result = VALIDATOR.validate_asset_bytes(asset(words))
                self.assertEqual(result["entries"], 10000)
                self.assertEqual(result["accepted"], 1 if skipped else 10000)
                words.append({} if skipped else word())
                self.reject(asset(words))

    def test_missing_and_empty_reading_skip_other_word_fields(self):
        result = VALIDATOR.validate_asset_bytes(asset([
            {}, {"reading": "", "surface": None, "rank": False}, word()]))
        self.assertEqual((result["accepted"], result["skipped"]), (1, 2))
        for reading in (None, False, 1, [], {}):
            with self.subTest(reading=reading):
                self.reject(asset([word(reading=reading)]))

    def test_all_controls_and_bidi_rejected_in_both_fields(self):
        forbidden = (list(range(0x20)) + list(range(0x7F, 0xA0)) + [0x61C, 0x200E, 0x200F]
                     + list(range(0x202A, 0x202F)) + list(range(0x2066, 0x206A)))
        for cp in forbidden:
            for field in ("surface", "reading"):
                with self.subTest(codepoint=cp, field=field):
                    entry = word()
                    entry[field] += chr(cp)
                    self.reject(asset([entry]))

    def test_non_kana_and_empty_surface_rejected(self):
        for reading in ("abc", "ｼﾝｺﾞ", "漢字", "しん ご", "しん1ご", "\u3040", "\u3100"):
            with self.subTest(reading=reading):
                self.reject(asset([word(reading=reading)]))
        for surface in ("", None, 42):
            self.reject(asset([word(surface=surface)]))

    def test_invalid_utf8_and_surrogates_even_in_unknown_or_skipped_fields(self):
        prefix = b'{"version":1,"generated_at":"date","words":[],"unused":"'
        for bad in (b"\x80", b"\xe3\x81", b"\xc0\xaf", b"\xe0\x80\x80",
                    b"\xed\xa0\x80", b"\xf4\x90\x80\x80", b"\\ud800", b"\\udc00"):
            with self.subTest(bad=bad):
                self.reject(prefix + bad + b'"}')
        self.reject(b'{"version":1,"generated_at":"date",'
                    b'"words":[{"surface":"\\ud800"}]}')
        self.reject(b"\xef\xbb\xbf" + asset())

    def test_numeric_contract_rejects_bools_and_nonfinite_numbers(self):
        for version in (True, None, "1", 0, 2, 1.5):
            self.reject(asset(version=version))
        for rank in (True, None, "1", 0, -1, 1.5, 2**32):
            self.reject(asset([word(rank=rank)]))
        for rank in (1, 1.0, 2**32 - 1):
            self.assertEqual(VALIDATOR.validate_asset_bytes(asset([word(rank=rank)]))["accepted"], 1)
        numeric = b'{"version":1e0,"generated_at":"date",'
        self.assertEqual(VALIDATOR.validate_asset_bytes(
            numeric + b'"words":[{"surface":"x","reading":"\\u3042","rank":1e0}]}')
                         ["accepted"], 1)
        for token in (b"NaN", b"Infinity", b"-Infinity", b"1e999", b"1e-324"):
            self.reject(numeric + b'"words":[],"unused":' + token + b'}')
        self.assertEqual(VALIDATOR.validate_asset_bytes(
            numeric + b'"words":[],"unused":1e-400}')["entries"], 0)

    def test_duplicate_keys_match_runtime_first_wins_and_validate_unused_values(self):
        self.assertEqual(VALIDATOR.validate_asset_bytes(
            b'{"version":1,"version":2,"generated_at":"date","words":[]}')["entries"], 0)
        self.reject(b'{"version":2,"version":1,"generated_at":"date","words":[]}')
        self.reject(b'{"version":1,"generated_at":"date","words":[],"unused":0,'
                    b'"unused":"\\ud800"}')

    def test_depth_limit_includes_unknown_members(self):
        for depth, accepted in ((64, True), (65, False)):
            # Root object is container one, followed by depth - 1 nested arrays.
            data = (b'{"version":1,"generated_at":"date","words":[],"unused":'
                    + b"[" * (depth - 1) + b"0" + b"]" * (depth - 1) + b"}")
            if accepted:
                self.assertEqual(VALIDATOR.validate_asset_bytes(data)["entries"], 0)
            else:
                self.reject(data)

    def test_original_bytes_hashed_without_reformatting(self):
        data = b" \r\n" + asset() + b"\r\n"
        self.source.write_bytes(data)
        VALIDATOR.validate_file(self.source, self.output)
        self.assertEqual(self.source.read_bytes(), data)
        self.assertEqual(self.output.read_bytes(),
                         (hashlib.sha256(data).hexdigest() + "  trending-words.json\n").encode())

    def test_invalid_second_entry_never_creates_or_replaces_output(self):
        self.source.write_bytes(asset([word(), word(reading="invalid")]))
        with self.assertRaises(VALIDATOR.AssetError):
            VALIDATOR.validate_file(self.source, self.output)
        self.assertFalse(self.output.exists())
        self.output.write_bytes(b"previous checksum\n")
        with self.assertRaises(VALIDATOR.AssetError):
            VALIDATOR.validate_file(self.source, self.output)
        self.assertEqual(self.output.read_bytes(), b"previous checksum\n")
        self.assertEqual(list(self.root.glob(".trending-checksum-*")), [])

    def test_replace_failure_preserves_existing_sidecar_and_cleans_temp(self):
        self.source.write_bytes(asset())
        self.output.write_bytes(b"previous checksum\n")
        with mock.patch.object(VALIDATOR.os, "replace", side_effect=OSError("injected")):
            with self.assertRaises(OSError):
                VALIDATOR.validate_file(self.source, self.output)
        self.assertEqual(self.output.read_bytes(), b"previous checksum\n")
        self.assertEqual(list(self.root.glob(".trending-checksum-*")), [])

    def test_output_cannot_overwrite_input(self):
        data = asset()
        self.source.write_bytes(data)
        with self.assertRaises(VALIDATOR.AssetError):
            VALIDATOR.validate_file(self.source, self.source)
        self.assertEqual(self.source.read_bytes(), data)

    def test_cli_distinguishes_empty_format_success_from_publication_readiness(self):
        for words in ([], [{}], [word()]):
            with self.subTest(words=words):
                self.source.write_bytes(asset(words))
                output = io.StringIO()
                with contextlib.redirect_stdout(output):
                    self.assertEqual(VALIDATOR.main([str(self.source)]), 0)
                self.assertIn("Runtime format valid", output.getvalue())
                self.assertIn("Publication readiness is not assessed", output.getvalue())
                if not words or words == [{}]:
                    self.assertIn("does not provide a trending vocabulary", output.getvalue())
        self.source.write_bytes(b"not json")
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(VALIDATOR.main([str(self.source), "--checksum-out", str(self.output)]), 1)
        self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main()
