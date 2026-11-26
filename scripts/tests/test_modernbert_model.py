"""Pins and input checks of the ModernBERT conversion script, without PyTorch or a network."""

import contextlib
import copy
import hashlib
import http.client
import importlib.util
import io
import json
from pathlib import Path
import re
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock
import urllib.request


SCRIPT = Path(__file__).resolve().parents[1] / "modernbert_model.py"
SPEC = importlib.util.spec_from_file_location("modernbert_model", SCRIPT)
MODEL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODEL)

SOURCE_FILES = ("LICENSE", "config.json", "model.safetensors", "tokenizer.model")


def pin(data: bytes) -> tuple[int, str]:
    return len(data), hashlib.sha256(data).hexdigest()


class FakeOpener:
    """Answers every request with the same bytes, or raises the given error."""

    def __init__(self, data=b"", error=None):
        self.data, self.error, self.requests = data, error, []

    def open(self, request, timeout):
        self.requests.append(request)
        if self.error is not None:
            raise self.error
        return io.BytesIO(self.data)


class TemporaryDirectoryTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)


class RepositoryPinTests(unittest.TestCase):
    def setUp(self):
        self.manifest = MODEL.load_manifest()

    def test_source_is_pinned_to_a_commit_and_lists_the_upstream_files(self):
        source = self.manifest["source"]
        self.assertEqual(source["repository"], "sbintuitions/modernbert-ja-70m")
        self.assertRegex(source["revision"], r"\A[0-9a-f]{40}\Z")
        self.assertEqual(source["license"], "MIT")
        self.assertEqual(sorted(entry["name"] for entry in source["files"]), sorted(SOURCE_FILES))

    def test_outputs_are_recorded_for_every_file(self):
        outputs = {entry["fileName"]: entry for entry in self.manifest["conversion"]["outputs"]}
        self.assertEqual(sorted(outputs), sorted(MODEL.OUTPUT_NAMES))
        tokenizer = next(entry for entry in self.manifest["source"]["files"]
                         if entry["name"] == MODEL.TOKENIZER_NAME)
        self.assertEqual(outputs[MODEL.TOKENIZER_NAME]["sha256"], tokenizer["sha256"])
        self.assertEqual(outputs[MODEL.TOKENIZER_NAME]["sizeBytes"], tokenizer["sizeBytes"])

    def test_recorded_toolchain_matches_the_script_pins_and_the_lock(self):
        header = SCRIPT.read_text(encoding="utf-8")
        pins = dict(re.findall(r'^#     "([A-Za-z0-9_-]+)==([^ ;"]+)', header, re.MULTILINE))
        toolchain = self.manifest["conversion"]["toolchain"]
        self.assertEqual(sorted(pins), sorted(MODEL.TOOLCHAIN_PACKAGES))
        self.assertEqual(toolchain["packages"], pins)
        self.assertIn(f'requires-python = "=={toolchain["python"]}"', header)
        self.assertEqual(toolchain["lockSha256"], MODEL.lock_digest())


class ManifestValidationTests(TemporaryDirectoryTest):
    def reject(self, mutate):
        manifest = copy.deepcopy(MODEL.load_manifest())
        mutate(manifest)
        path = self.root / "manifest.json"
        path.write_text(json.dumps(manifest), encoding="utf-8")
        with self.assertRaises(MODEL.ModelError):
            MODEL.load_manifest(path)

    def test_missing_or_broken_file_is_rejected(self):
        broken = self.root / "broken.json"
        broken.write_text("{", encoding="utf-8")
        for path in (broken, self.root / "absent.json"):
            with self.subTest(path=path.name), self.assertRaises(MODEL.ModelError):
                MODEL.load_manifest(path)

    def test_source_fields(self):
        mutations = {
            "schema": lambda m: m.update(schemaVersion=2),
            "repository": lambda m: m["source"].update(repository="https://example.invalid/x"),
            "branch revision": lambda m: m["source"].update(revision="main"),
            "no files": lambda m: m["source"].update(files=[]),
            "path in name": lambda m: m["source"]["files"][0].update(name="../LICENSE"),
            "hidden name": lambda m: m["source"]["files"][0].update(name=".."),
            "bool size": lambda m: m["source"]["files"][0].update(sizeBytes=True),
            "zero size": lambda m: m["source"]["files"][0].update(sizeBytes=0),
            "uppercase digest": lambda m: m["source"]["files"][0].update(sha256="A" * 64),
            "extra key": lambda m: m["source"]["files"][0].update(url="x"),
            "duplicate": lambda m: m["source"]["files"].append(m["source"]["files"][0]),
        }
        for name, mutate in mutations.items():
            with self.subTest(name):
                self.reject(mutate)

    def test_conversion_fields(self):
        def int8(**values):
            return lambda m: m["conversion"]["int8"].update(values)

        mutations = {
            "opset": lambda m: m["conversion"].update(opset="18"),
            "no int8": lambda m: m["conversion"].pop("int8"),
            "operators": int8(operators=[]),
            "weight type": int8(weightType="QInt4"),
            "per channel": int8(perChannel=1),
            "shape": int8(fp32LinearWeightShape=[1536]),
            "count": int8(fp32LinearCount=0),
            "missing output": lambda m: m["conversion"]["outputs"].pop(),
            "outputs without toolchain": lambda m: m["conversion"].update(toolchain=None),
        }
        for name, mutate in mutations.items():
            with self.subTest(name):
                self.reject(mutate)

    def test_outputs_may_be_unrecorded(self):
        manifest = MODEL.load_manifest()
        manifest["conversion"].update(toolchain=None, outputs=[])
        path = self.root / "manifest.json"
        path.write_text(json.dumps(manifest), encoding="utf-8")
        self.assertEqual(MODEL.load_manifest(path)["conversion"]["outputs"], [])


class FileCheckTests(TemporaryDirectoryTest):
    def test_check_file_accepts_only_the_pinned_bytes(self):
        path = self.root / "file"
        path.write_bytes(b"weights")
        MODEL.check_file(path, *pin(b"weights"))
        for size, digest in (pin(b"weightz"), pin(b"weights!")):
            with self.subTest(size=size), self.assertRaises(MODEL.ModelError):
                MODEL.check_file(path, size, digest)
        with self.assertRaises(MODEL.ModelError):
            MODEL.check_file(self.root / "absent", *pin(b"weights"))

    def test_output_directory_must_be_new_or_empty(self):
        MODEL.require_new_directory(self.root / "new" / "nested")
        self.assertTrue((self.root / "new" / "nested").is_dir())
        MODEL.require_new_directory(self.root / "new" / "nested")
        used = self.root / "used"
        used.mkdir()
        (used / "kept").write_bytes(b"x")
        for path in (used, used / "kept"):
            with self.subTest(path=path.name), self.assertRaises(MODEL.ModelError):
                MODEL.require_new_directory(path)
        self.assertEqual((used / "kept").read_bytes(), b"x")

    def test_lock_digest_ignores_line_endings(self):
        unix, windows = self.root / "unix.lock", self.root / "windows.lock"
        unix.write_bytes(b"a\nb\n")
        windows.write_bytes(b"a\r\nb\r\n")
        self.assertEqual(MODEL.lock_digest(unix), MODEL.lock_digest(windows))
        with self.assertRaises(MODEL.ModelError):
            MODEL.lock_digest(self.root / "absent.lock")


class DownloadTests(TemporaryDirectoryTest):
    URL = "https://huggingface.co/owner/name/resolve/revision/file"

    def download(self, opener, data=b"payload"):
        MODEL.download(self.URL, self.root / "file", *pin(data), opener=opener)

    def test_pinned_bytes_are_written_with_a_generic_user_agent(self):
        opener = FakeOpener(b"payload")
        self.download(opener)
        self.assertEqual((self.root / "file").read_bytes(), b"payload")
        self.assertEqual([path.name for path in self.root.iterdir()], ["file"])
        self.assertEqual(opener.requests[0].full_url, self.URL)
        self.assertEqual(opener.requests[0].get_header("User-agent"), "model-fetch")

    def test_other_bytes_leave_nothing_behind(self):
        for data in (b"payloae", b"pay", b"payload and more"):
            with self.subTest(data=data), self.assertRaises(MODEL.ModelError):
                self.download(FakeOpener(data))
            self.assertEqual(list(self.root.iterdir()), [])

    def test_transport_errors_are_reported(self):
        for error in (OSError("unreachable"), http.client.IncompleteRead(b"pay")):
            with self.subTest(error=type(error).__name__), self.assertRaises(MODEL.ModelError):
                self.download(FakeOpener(error=error))
            self.assertEqual(list(self.root.iterdir()), [])

    def test_redirects_must_stay_on_https(self):
        handler = MODEL._HttpsOnlyRedirect()
        request = urllib.request.Request(self.URL)
        followed = handler.redirect_request(
            request, None, 302, "Found", {}, "https://cdn.example.invalid/file")
        self.assertEqual(followed.full_url, "https://cdn.example.invalid/file")
        for location in ("http://cdn.example.invalid/file", "ftp://example.invalid/file"):
            with self.subTest(location=location), self.assertRaises(MODEL.ModelError):
                handler.redirect_request(request, None, 302, "Found", {}, location)

    def test_the_default_opener_uses_the_https_only_redirect_handler(self):
        opener = FakeOpener(b"payload")
        with mock.patch.object(urllib.request, "build_opener", return_value=opener) as build:
            MODEL.download(self.URL, self.root / "file", *pin(b"payload"))
        self.assertTrue(any(isinstance(handler, MODEL._HttpsOnlyRedirect)
                            for handler in build.call_args.args))

    def test_fetch_requests_each_pinned_file_at_the_revision(self):
        manifest = MODEL.load_manifest()
        with mock.patch.object(MODEL, "download") as download, \
                contextlib.redirect_stdout(io.StringIO()):
            MODEL.fetch(manifest, self.root / "input")
        prefix = ("https://huggingface.co/sbintuitions/modernbert-ja-70m/resolve/"
                  f"{manifest['source']['revision']}/")
        self.assertEqual([call.args[0] for call in download.call_args_list],
                         [prefix + entry["name"] for entry in manifest["source"]["files"]])
        self.assertEqual([call.args[2:] for call in download.call_args_list],
                         [(entry["sizeBytes"], entry["sha256"])
                          for entry in manifest["source"]["files"]])


class ConvertPinTests(TemporaryDirectoryTest):
    """The record and compare steps, with the model conversions replaced by fixed bytes."""

    def setUp(self):
        super().setUp()
        self.input = self.root / "input"
        self.input.mkdir()
        (self.input / MODEL.TOKENIZER_NAME).write_bytes(b"pieces")
        self.manifest = MODEL.load_manifest()
        self.manifest["conversion"].update(toolchain=None, outputs=[])
        self.manifest_path = self.root / "manifest.json"
        self.fp16 = b"fp16"
        replacements = {
            "check_inputs": lambda manifest, directory: None,
            "current_toolchain": lambda: {"python": "test"},
            "export_fp32": lambda manifest, source, work, to: to.write_bytes(b"fp32"),
            "convert_fp16": lambda source, to: to.write_bytes(self.fp16),
            "convert_int8": lambda settings, source, work, to: to.write_bytes(b"int8"),
        }
        for name, replacement in replacements.items():
            patcher = mock.patch.object(MODEL, name, replacement)
            patcher.start()
            self.addCleanup(patcher.stop)

    def convert(self, directory, record=False):
        self.stderr = io.StringIO()
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(self.stderr):
            return MODEL.convert(self.manifest, self.input, self.root / directory, record,
                                 self.manifest_path)

    def test_unrecorded_then_recorded_then_matching_then_different(self):
        self.assertEqual(self.convert("first"), 2)
        self.assertFalse(self.manifest_path.exists())

        self.assertEqual(self.convert("second", record=True), 0)
        recorded = json.loads(self.manifest_path.read_text(encoding="utf-8"))["conversion"]
        self.assertEqual(recorded["toolchain"], {"python": "test"})
        self.assertEqual([entry["fileName"] for entry in recorded["outputs"]],
                         list(MODEL.OUTPUT_NAMES))
        self.assertNotIn(b"\r", self.manifest_path.read_bytes())

        self.assertEqual(self.convert("third"), 0)
        self.assertEqual(sorted(path.name for path in (self.root / "third").iterdir()),
                         sorted(MODEL.OUTPUT_NAMES))

        self.fp16 = b"changed"
        written = self.manifest_path.read_bytes()
        self.assertEqual(self.convert("fourth"), 1)
        self.assertIn(MODEL.FP16_NAME, self.stderr.getvalue())
        self.assertNotIn(MODEL.INT8_NAME, self.stderr.getvalue())
        self.assertEqual(self.manifest_path.read_bytes(), written)

    def test_output_directory_with_files_is_left_alone(self):
        used = self.root / "used"
        used.mkdir()
        (used / MODEL.FP32_NAME).write_bytes(b"old")
        with self.assertRaises(MODEL.ModelError):
            self.convert("used", record=True)
        self.assertEqual((used / MODEL.FP32_NAME).read_bytes(), b"old")


class ModelDirectoryTests(TemporaryDirectoryTest):
    def test_convert_refuses_an_input_that_differs_from_its_pin(self):
        manifest = MODEL.load_manifest()
        source = self.root / "input"
        source.mkdir()
        for entry in manifest["source"]["files"]:
            data = entry["name"].encode()
            entry["sizeBytes"], entry["sha256"] = pin(data)
            (source / entry["name"]).write_bytes(data)
        (source / MODEL.TOKENIZER_NAME).write_bytes(b"altered")
        output = self.root / "output"
        with mock.patch.object(MODEL, "current_toolchain", return_value={}), \
                mock.patch.object(MODEL, "export_fp32") as export, \
                self.assertRaisesRegex(MODEL.ModelError, MODEL.TOKENIZER_NAME):
            MODEL.convert(manifest, source, output, False, self.root / "manifest.json")
        export.assert_not_called()
        self.assertFalse(output.exists())

    def test_a_missing_output_file_is_reported_by_name(self):
        (self.root / MODEL.FP32_NAME).write_bytes(b"fp32")
        with self.assertRaisesRegex(MODEL.ModelError, MODEL.FP16_NAME):
            MODEL.describe_outputs(self.root)


class GraphSelectionTests(unittest.TestCase):
    def setUp(self):
        def node(name, op_type, weight):
            return SimpleNamespace(name=name, op_type=op_type, input=["x", weight])

        self.graph = SimpleNamespace(
            initializer=[SimpleNamespace(name="out", dims=[1536, 384]),
                         SimpleNamespace(name="in", dims=[384, 1536])],
            node=[node("first", "MatMul", "out"), node("second", "MatMul", "in"),
                  node("third", "Gemm", "out"), node("fourth", "MatMul", "computed")])

    def test_only_matmul_nodes_with_the_weight_shape_are_selected(self):
        self.assertEqual(MODEL.fp32_linear_nodes(self.graph, [1536, 384], 1), ["first"])

    def test_unexpected_count_is_an_error(self):
        for count in (0, 2):
            with self.subTest(count=count), self.assertRaises(MODEL.ModelError):
                MODEL.fp32_linear_nodes(self.graph, [1536, 384], count)


class MaskedPositionTests(unittest.TestCase):
    def test_every_inner_position_of_a_short_sentence(self):
        self.assertEqual(MODEL.masked_positions(3), [1])
        self.assertEqual(MODEL.masked_positions(6), [1, 2, 3, 4])
        self.assertEqual(MODEL.masked_positions(34), list(range(1, 33)))

    def test_long_sentence_is_sampled_with_both_ends(self):
        for length in (35, 185, 8192):
            positions = MODEL.masked_positions(length)
            with self.subTest(length=length):
                self.assertEqual(len(positions), MODEL.MAX_ROWS_PER_SENTENCE)
                self.assertEqual(positions, sorted(set(positions)))
                self.assertEqual((positions[0], positions[-1]), (1, length - 2))


class CommandLineTests(TemporaryDirectoryTest):
    def test_a_rejected_input_exits_with_one_and_a_message(self):
        (self.root / "kept").write_bytes(b"x")
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            code = MODEL.main(["fetch", "--output-dir", str(self.root)])
        self.assertEqual(code, 1)
        self.assertIn("new or empty directory", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
