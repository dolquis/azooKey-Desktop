"""Coverage shard inventory and execution contract tests."""

import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET


SCRIPT = Path(__file__).resolve().parents[1] / "coverage-shards.py"
SPEC = importlib.util.spec_from_file_location("coverage_shards", SCRIPT)
SHARDS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SHARDS)


class CoverageShardsTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.inventory = self.root / "ctest.json"
        self.write_inventory(["delta", "alpha", "charlie", "bravo", "echo"])

    def write_inventory(self, names):
        self.inventory.write_text(json.dumps({"tests": [{"name": n} for n in names]}),
                                  encoding="utf-8")

    def plan(self):
        for shard in (1, 2):
            SHARDS.plan(self.inventory, shard, 2, self.root / f"shard-{shard}")
        return self.root / "shard-1" / "manifest.json"

    def mutate(self, field, value, shard=1):
        path = self.root / f"shard-{shard}" / "manifest.json"
        manifest = json.loads(path.read_text(encoding="utf-8"))
        manifest[field] = value
        path.write_text(json.dumps(manifest), encoding="utf-8")

    def junit(self, names, child=None, status="run", message=None):
        root = ET.Element("testsuites")
        suite = ET.SubElement(root, "testsuite")
        for name in names:
            case = ET.SubElement(suite, "testcase", name=name)
            if child:
                element = ET.SubElement(case, child)
                if message is not None:
                    element.set("message", message)
            if status:
                case.set("status", status)
        path = self.root / "junit.xml"
        ET.ElementTree(root).write(path, encoding="utf-8")
        return path

    def test_round_robin_is_deterministic_and_complete(self):
        manifest = self.plan()
        self.assertEqual(SHARDS.read_manifest(manifest)["selected"],
                         ["alpha", "charlie", "echo"])
        self.assertEqual((manifest.parent / "tests.txt").read_text(),
                         "alpha\ncharlie\necho\n")
        SHARDS.verify_all(self.root, 2)
        before = manifest.read_bytes()
        self.write_inventory(["echo", "bravo", "charlie", "alpha", "delta"])
        self.plan()
        self.assertEqual(manifest.read_bytes(), before)

    def test_invalid_inventory_names(self):
        for names in ([], ["a", "a"], [""], [" "], ["a\nb"], ["a\rb"],
                      ["a\0b"], [None], [1]):
            with self.subTest(names=names), self.assertRaises(ValueError):
                self.write_inventory(names)
                SHARDS.plan(self.inventory, 1, 1, self.root / "output")

    def test_invalid_parameters_and_empty_shards(self):
        for shard, count in ((0, 2), (3, 2), (1, 0), (1, -1), (1, 6),
                             (True, 2), (1, True)):
            with self.subTest(shard=shard, count=count), self.assertRaises(ValueError):
                SHARDS.plan(self.inventory, shard, count, self.root / "output")

    def test_invalid_inventory_structure(self):
        for value in ([], {}, {"tests": None}, {"tests": [None]}):
            with self.subTest(value=value), self.assertRaises(ValueError):
                self.inventory.write_text(json.dumps(value), encoding="utf-8")
                SHARDS.plan(self.inventory, 1, 1, self.root / "output")

    def test_run_accepts_exact_inventory_and_skips(self):
        manifest = self.plan()
        selected = ["echo", "alpha", "charlie"]
        SHARDS.verify_run(manifest, self.junit(selected))
        # CTest WriteJUnitXML emits status=notrun and CompletionStatus as message.
        for message in ("SKIP_REGULAR_EXPRESSION_MATCHED", "SKIP_RETURN_CODE=77"):
            with self.subTest(message=message):
                SHARDS.verify_run(manifest, self.junit(
                    selected, "skipped", status="notrun", message=message))

    def test_run_rejects_unintentional_or_malformed_skip(self):
        manifest = self.plan()
        selected = ["alpha", "charlie", "echo"]
        for message in (None, "", "Disabled", "Fixture dependency failed",
                        "Unable to find executable", "Failed to start",
                        "SKIP_INSUFFICIENT_RESOURCES", "SKIP_RETURN_CODE=",
                        "SKIP_RETURN_CODE=77 trailing", "SKIP_RETURN_CODE=-1"):
            with self.subTest(message=message), self.assertRaises(ValueError):
                SHARDS.verify_run(manifest, self.junit(
                    selected, "skipped", status="notrun", message=message))
        for status in (None, "run", "disabled", "fail"):
            with self.subTest(status=status), self.assertRaises(ValueError):
                SHARDS.verify_run(manifest, self.junit(
                    selected, "skipped", status=status,
                    message="SKIP_REGULAR_EXPRESSION_MATCHED"))

    def test_run_rejects_missing_unknown_duplicate_and_empty(self):
        manifest = self.plan()
        for names in (["alpha", "charlie"], ["alpha", "charlie", "unknown"],
                      ["alpha", "charlie", "echo", "echo"], []):
            with self.subTest(names=names), self.assertRaises(ValueError):
                SHARDS.verify_run(manifest, self.junit(names))

    def test_run_rejects_failed_error_and_disabled(self):
        manifest = self.plan()
        selected = ["alpha", "charlie", "echo"]
        for child in ("failure", "error"):
            with self.subTest(child=child), self.assertRaises(ValueError):
                SHARDS.verify_run(manifest, self.junit(selected, child))
        for status in ("fail", "failed", "error", "disabled", "notrun"):
            with self.subTest(status=status), self.assertRaises(ValueError):
                SHARDS.verify_run(manifest, self.junit(selected, status=status))

    def test_manifest_rejects_bad_partition_and_metadata(self):
        for field, value in (("selected", ["alpha"]), ("selected", ["alpha", "alpha"]),
                             ("selected", ["unknown"]), ("version", 2), ("version", True),
                             ("shard", 0), ("count", 0), ("expected", ["z", "a"])):
            with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                self.plan()
                self.mutate(field, value)
                SHARDS.verify_all(self.root, 2)

    def test_all_rejects_different_inventory(self):
        self.plan()
        self.write_inventory(["alpha", "bravo", "charlie", "delta", "foxtrot"])
        SHARDS.plan(self.inventory, 2, 2, self.root / "shard-2")
        with self.assertRaisesRegex(ValueError, "different expected"):
            SHARDS.verify_all(self.root, 2)

    def test_all_rejects_missing_shard_and_wrong_id(self):
        self.plan()
        SHARDS.plan(self.inventory, 1, 2, self.root / "shard-2")
        with self.assertRaisesRegex(ValueError, "incorrect shard/count"):
            SHARDS.verify_all(self.root, 2)
        (self.root / "shard-2" / "manifest.json").unlink()
        with self.assertRaises(OSError):
            SHARDS.verify_all(self.root, 2)

    def test_all_rejects_wrong_count(self):
        self.plan()
        SHARDS.plan(self.inventory, 1, 1, self.root / "shard-1")
        with self.assertRaisesRegex(ValueError, "incorrect shard/count"):
            SHARDS.verify_all(self.root, 2)

    def test_run_rejects_malformed_xml_and_non_junit_root(self):
        manifest = self.plan()
        path = self.root / "junit.xml"
        path.write_text("<broken", encoding="utf-8")
        with self.assertRaises(ET.ParseError):
            SHARDS.verify_run(manifest, path)
        path.write_text('<arbitrary><testcase name="alpha"/></arbitrary>', encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "JUnit root"):
            SHARDS.verify_run(manifest, path)

    def test_cli_success_and_failure_exit_codes(self):
        command = [sys.executable, str(SCRIPT), "plan", "--ctest-json", str(self.inventory),
                   "--shard", "1", "--count", "2", "--output-dir", str(self.root / "out")]
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.write_inventory(["duplicate", "duplicate"])
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(result.returncode, 1)
        self.assertIn("duplicate", result.stderr)
        self.assertNotIn("Traceback", result.stderr)


if __name__ == "__main__":
    unittest.main()
