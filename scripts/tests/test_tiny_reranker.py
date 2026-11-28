"""Exercise M56 training with artificial inputs and optional CPU model dependencies."""

from __future__ import annotations

import copy
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "reranker" / "training"))

from tiny_reranker import check_reproducibility, make_fixture, make_labels, prepare, publish
from training_corpus import TrainingError, fetch_asset, json_bytes, load_manifest
from training_data import build_exclusions, feature_vector, fixture_rows, read_jsonl, synthesize, write_jsonl


class TemporaryCase(unittest.TestCase):
    def setUp(self) -> None:
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.m52 = self.root / "eval"
        self.m52.mkdir()
        write_jsonl(self.m52 / "kana_kanji_eval.jsonl", [{
            "left_context": "評価専用", "input": "ほげ", "expected": "保下",
            "acceptable": ["保げ"], "provenance": "authored",
        }])
        write_jsonl(self.m52 / "typo_eval.jsonl", [{
            "left_context": "評価専用", "observed_reading": "せいせ", "intended_reading": "せいせい",
            "expected_surface": "生成", "raw_keys": "seise",
        }])


def archive_bytes(member: str, data: bytes) -> bytes:
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w") as archive:
        archive.writestr(member, data)
    return output.getvalue()


class CorpusTests(TemporaryCase):
    def make_sources(self, *, license_id: str = "PD") -> tuple[Path, bytes, bytes]:
        text = "作品名\n著者名\n\n｜山川《やまかわ》へ海《うみ》［＃改行］\n\n底本：紙の本"
        work = archive_bytes("body.txt", text.encode("cp932"))
        dictionary = archive_bytes("dict/system_core.dic", b"artificial dictionary")
        path = self.root / "manifest.json"
        path.write_bytes(json_bytes({
            "schema": 1, "corpora": [{
                "id": "synthetic", "url": "https://example.invalid/work.zip", "revision": "test-1",
                "sha256": hashlib.sha256(work).hexdigest(), "license": license_id,
                "notice": "Test fixture", "member": "body.txt",
            }],
            "reading_dictionary": {
                "id": "dictionary", "url": "https://example.invalid/dict.zip", "revision": "20260723",
                "sha256": hashlib.sha256(dictionary).hexdigest(), "license": "Apache-2.0",
                "notice": "Test fixture", "member": "dict/system_core.dic",
            },
        }))
        return path, work, dictionary

    def test_fetch_verifies_content_and_cache(self) -> None:
        path, content, _ = self.make_sources()
        works, _ = load_manifest(path)
        asset = works[0]
        cache = self.root / "source"
        with self.assertRaisesRegex(TrainingError, "SHA256"):
            fetch_asset(asset, cache, offline=False, opener=lambda _: io.BytesIO(b"bad data"))
        self.assertFalse(asset.cache_path(cache).exists())
        result = fetch_asset(asset, cache, offline=False, opener=lambda _: io.BytesIO(content))
        self.assertEqual(result.read_bytes(), content)
        self.assertEqual(result, fetch_asset(asset, cache, offline=True))
        result.write_bytes(b"changed")
        with self.assertRaisesRegex(TrainingError, "SHA256"):
            fetch_asset(asset, cache, offline=True)

    def test_rejects_unapproved_licenses(self) -> None:
        for license_id in ("CC-BY-SA-4.0", "GFDL-1.3-only"):
            with self.subTest(license_id=license_id):
                path, _, _ = self.make_sources(license_id=license_id)
                with self.assertRaisesRegex(TrainingError, "license"):
                    load_manifest(path)

    def test_prepare_prefers_ruby_and_uses_sudachi_for_plain_text(self) -> None:
        manifest, work_bytes, dictionary_bytes = self.make_sources()
        works, dictionary = load_manifest(manifest)
        works[0].cache_path(self.root).write_bytes(work_bytes)
        dictionary.cache_path(self.root).write_bytes(dictionary_bytes)
        seen = []

        def tokenize(text: str) -> list[SimpleNamespace]:
            seen.append(text)
            return [SimpleNamespace(surface=lambda: text, reading_form=lambda: "ヘ")]

        def mock_dictionary(*, dict: str) -> SimpleNamespace:
            self.assertEqual(Path(dict).read_bytes(), b"artificial dictionary")
            return SimpleNamespace(create=lambda: SimpleNamespace(tokenize=tokenize))

        module = SimpleNamespace(dictionary=SimpleNamespace(Dictionary=mock_dictionary))
        with patch.dict(sys.modules, {"sudachipy": module}):
            prepare(manifest, self.root, self.root)
        self.assertEqual(seen, ["へ"])
        rows = list(read_jsonl(self.root / "queries.jsonl"))
        self.assertEqual([r["input"] for r in rows], ["やまかわ", "へ", "うみ"])
        self.assertEqual([r["left_context"] for r in rows], ["", "山川", "山川へ"])


class SynthesisTests(TemporaryCase):
    def test_repository_eval_files_are_supported(self) -> None:
        manifest = build_exclusions(ROOT / "bench" / "data")
        self.assertGreater(len(manifest["keys"]), 0)
        self.assertIn("conversion_quality_smoke.jsonl", manifest["datasets"])

    def test_m52_overlap_covers_acceptable_and_both_typo_readings(self) -> None:
        manifest = build_exclusions(self.m52)
        for reading, surface in [("ホゲ", "保げ"), ("せいせ", "生成"), ("せいせい", "生成")]:
            with self.subTest(reading=reading):
                rows = list(fixture_rows())
                rows[0][0].update({"input": reading, "chosen": surface})
                with self.assertRaisesRegex(TrainingError, "overlap with M52"):
                    synthesize((q for q, _ in rows), (c for _, c in rows), manifest)
        self.assertNotIn("評価専用", json.dumps(manifest, ensure_ascii=False))

    def test_visible_limit_and_dedup(self) -> None:
        rows = list(fixture_rows())
        candidates = rows[0][1]["candidates"]
        assert isinstance(candidates, list)
        for index in range(7):
            surface = f"架空語{index}"
            vector = list(candidates[0]["features"])
            vector[6] = len(surface)
            candidates.append({"surface": surface, "features": vector})
        duplicate = copy.deepcopy(rows[0])
        duplicate[0]["id"] = duplicate[1]["id"] = "duplicate"
        rows.append(duplicate)
        labels, counts = synthesize((q for q, _ in rows), (c for _, c in rows), build_exclusions(self.m52))
        votes = [row for row in labels if row["occurrences"] == 2]
        self.assertEqual(len(votes), 9)
        self.assertEqual(sum(row["label"] for row in votes), 1)
        self.assertEqual(counts["tied_labels"], 0)

        # Move the positive beyond the displayed first page.
        candidates.append(candidates.pop(2))
        rows.pop()
        _, missed = synthesize((q for q, _ in rows), (c for _, c in rows), build_exclusions(self.m52))
        self.assertEqual(missed["chosen_not_shown"], 1)

    def test_tied_votes_are_dropped(self) -> None:
        rows = list(fixture_rows())
        other_query, other_batch = copy.deepcopy(rows[0])
        other_query["id"] = other_batch["id"] = "other-choice"
        other_query["chosen"] = "架空甲"
        rows.append((other_query, other_batch))
        _, counts = synthesize((q for q, _ in rows), (c for _, c in rows), build_exclusions(self.m52))
        self.assertEqual(counts["tied_labels"], 2)

    def test_bad_features_and_partial_publication(self) -> None:
        with self.assertRaisesRegex(TrainingError, "finite fp32"):
            feature_vector([float("nan")] * 12)
        vector = [-10, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0]
        self.assertEqual(feature_vector(vector, "𠮷")[6], 1.0)
        with self.assertRaisesRegex(TrainingError, "Unicode code points"):
            feature_vector(vector, "あい")

        def fail(staging: Path) -> None:
            (staging / "partial").write_bytes(b"data")
            raise TrainingError("failed before publication")

        result = self.root / "result"
        with self.assertRaisesRegex(TrainingError, "before publication"):
            publish(result, fail)
        self.assertFalse(result.exists())


class ModelTests(TemporaryCase):
    def test_train_export_ort_and_reproducibility(self) -> None:
        packages = ("lightgbm", "onnxmltools", "onnxruntime", "onnx", "torch", "sklearn")
        missing = [package for package in packages if importlib.util.find_spec(package) is None]
        if missing:
            self.skipTest("missing locked training dependencies: " + ", ".join(missing))
        import onnx

        from training_models import verify

        fixture, dataset, output = (self.root / name for name in ("fixture", "dataset", "models"))
        publish(fixture, make_fixture)
        publish(dataset, lambda staging: make_labels(
            fixture / "queries.jsonl", fixture / "candidates.jsonl", self.m52, staging,
        ))
        publish(output, lambda staging: check_reproducibility(dataset, staging))
        hashes = verify(output / "first")
        self.assertEqual(set(hashes), {"tiny_reranker_v0.onnx", "tiny_reranker.onnx"})
        self.assertEqual(hashes, verify(output / "second"))
        self.assertEqual(hashes, json.loads((output / "reproducibility.json").read_bytes())["models"])
        receipt = json.loads((output / "first" / "receipt.json").read_bytes())
        self.assertEqual(receipt["opsets"], {
            "tiny_reranker_v0.onnx": {"ai.onnx": 15, "ai.onnx.ml": 1},
            "tiny_reranker.onnx": {"ai.onnx": 20},
        })
        v0 = onnx.load(output / "first" / "tiny_reranker_v0.onnx")
        metadata = {item.key: item.value for item in v0.metadata_props}
        self.assertEqual(json.loads(metadata["opsets"]), receipt["opsets"]["tiny_reranker_v0.onnx"])
        (output / "first" / "tiny_reranker.onnx").write_bytes(b"wrong")
        with self.assertRaisesRegex(TrainingError, "SHA256 mismatch"):
            verify(output / "first")


if __name__ == "__main__":
    unittest.main(verbosity=2)
