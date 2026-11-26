# /// script
# requires-python = "==3.13.13"
# dependencies = [
#     "numpy==2.5.5 ; sys_platform == 'win32' and platform_machine == 'AMD64'",
#     "onnx==1.23.2 ; sys_platform == 'win32' and platform_machine == 'AMD64'",
#     "onnxruntime==1.32.0 ; sys_platform == 'win32' and platform_machine == 'AMD64'",
#     "onnxscript==0.7.2 ; sys_platform == 'win32' and platform_machine == 'AMD64'",
#     "safetensors==0.8.0 ; sys_platform == 'win32' and platform_machine == 'AMD64'",
#     "sentencepiece==0.2.3 ; sys_platform == 'win32' and platform_machine == 'AMD64'",
#     "torch==2.15.0 ; sys_platform == 'win32' and platform_machine == 'AMD64'",
#     "transformers==4.57.6 ; sys_platform == 'win32' and platform_machine == 'AMD64'",
# ]
# ///
"""Fetch modernbert-ja-70m at a pinned revision and convert it to ONNX (fp16 / int8).

Nothing here is part of the product build. The weights and the converted models are
written only to the directories named on the command line and are never committed.
The pins (upstream files, toolchain, output digests) live in the manifest next to
this file; docs/modernbert-ja-scoring-spec.md describes the model contract.

    uv run --no-config --locked --script scripts/modernbert_model.py \
        fetch --output-dir build/modernbert/input
    uv run --no-config --locked --script scripts/modernbert_model.py \
        convert --input-dir build/modernbert/input --output-dir build/modernbert/model
    uv run --no-config --locked --script scripts/modernbert_model.py \
        verify --input-dir build/modernbert/input --model-dir build/modernbert/model

An output directory must be new or empty.

Only the standard library is imported at module level.
"""

from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import http.client
import importlib.metadata
import json
import os
from pathlib import Path
import platform
import re
import shutil
import sys
import tempfile
import time
import urllib.request


MANIFEST_PATH = Path(__file__).resolve().with_name("modernbert-ja-70m.manifest.json")
LOCK_PATH = Path(__file__).resolve().with_name("modernbert_model.py.lock")
HUB_URL = "https://huggingface.co"
DOWNLOAD_CHUNK_BYTES = 1024 * 1024
DOWNLOAD_TIMEOUT_SECONDS = 60

FP32_NAME = "model-fp32.onnx"
FP16_NAME = "model-fp16.onnx"
INT8_NAME = "model-int8.onnx"
TOKENIZER_NAME = "tokenizer.model"
OUTPUT_NAMES = (FP32_NAME, FP16_NAME, INT8_NAME, TOKENIZER_NAME)

TOOLCHAIN_PACKAGES = (
    "numpy",
    "onnx",
    "onnxruntime",
    "onnxscript",
    "safetensors",
    "sentencepiece",
    "torch",
    "transformers",
)

SPECIAL_PIECES = {1: "<s>", 2: "</s>", 3: "<pad>", 5: "<mask>"}
BOS_ID, EOS_ID, PAD_ID, MASK_ID = 1, 2, 3, 5

# Largest accepted difference between PyTorch and the fp32 ONNX model.
PARITY_LOGIT_TOLERANCE = 1e-2
PARITY_SCORE_TOLERANCE = 1e-2
MAX_ROWS_PER_SENTENCE = 32
VERIFY_THREADS = 4
# The reference model returns every position; a few rows at a time bounds its memory.
REFERENCE_ROWS_PER_PASS = 4

# Sentences written for this script. Each pair differs only in a homophone.
SENTENCE_PAIRS = (
    ("川に架かる橋を渡る。", "川に架かる箸を渡る。"),
    ("今日は朝から雨が降っている。", "今日は朝から飴が降っている。"),
    ("来週、意見を交換する機会を設けます。", "来週、意見を交換する機械を設けます。"),
    ("彼は大学で経済を専攻している。", "彼は大学で経済を先行している。"),
    ("暑い夏の日が続いています。", "熱い夏の日が続いています。"),
    ("資料の内容に誤りがないか確認してください。",
     "資料の内容に誤りがないか確任してください。"),
)
# Longer than the 128-token local attention window.
LONG_SENTENCE = "".join(
    f"{index}番目の会議では、予定していた議題を順番に確認して、次の担当者を決めました。"
    for index in range(1, 13)
)


class ModelError(Exception):
    """An input, output or environment does not match the pinned conditions."""


# --- manifest ---------------------------------------------------------------

_SHA256 = re.compile(r"[0-9a-f]{64}")
_REVISION = re.compile(r"[0-9a-f]{40}")
_REPOSITORY = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]*/[A-Za-z0-9][A-Za-z0-9._-]*")
_FILE_NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]*")


def _check_file_entry(entry, name_key: str) -> None:
    if not isinstance(entry, dict) or set(entry) != {name_key, "sizeBytes", "sha256"}:
        raise ModelError(f"manifest: a file entry needs {name_key}, sizeBytes and sha256")
    name, size, digest = entry[name_key], entry["sizeBytes"], entry["sha256"]
    if not isinstance(name, str) or not _FILE_NAME.fullmatch(name):
        raise ModelError(f"manifest: {name!r} is not a plain file name")
    if type(size) is not int or size <= 0:
        raise ModelError(f"manifest: {name}: sizeBytes must be a positive integer")
    if not isinstance(digest, str) or not _SHA256.fullmatch(digest):
        raise ModelError(f"manifest: {name}: sha256 must be 64 lowercase hex digits")


def _check_int8_settings(settings) -> None:
    keys = {"operators", "weightType", "perChannel", "fp32LinearWeightShape", "fp32LinearCount"}
    if not isinstance(settings, dict) or set(settings) != keys:
        raise ModelError(f"manifest: conversion.int8 needs {', '.join(sorted(keys))}")
    operators, shape = settings["operators"], settings["fp32LinearWeightShape"]
    if not isinstance(operators, list) or not operators \
            or not all(isinstance(name, str) for name in operators):
        raise ModelError("manifest: conversion.int8.operators must list operator names")
    if settings["weightType"] not in ("QInt8", "QUInt8"):
        raise ModelError("manifest: conversion.int8.weightType must be QInt8 or QUInt8")
    if type(settings["perChannel"]) is not bool:
        raise ModelError("manifest: conversion.int8.perChannel must be true or false")
    if not isinstance(shape, list) or len(shape) != 2 \
            or not all(type(size) is int and size > 0 for size in shape):
        raise ModelError("manifest: conversion.int8.fp32LinearWeightShape must be two sizes")
    if type(settings["fp32LinearCount"]) is not int or settings["fp32LinearCount"] <= 0:
        raise ModelError("manifest: conversion.int8.fp32LinearCount must be a positive integer")


def load_manifest(path: Path = MANIFEST_PATH) -> dict:
    try:
        manifest = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise ModelError(f"cannot read the manifest {path}: {error}") from error
    if not isinstance(manifest, dict) or manifest.get("schemaVersion") != 1:
        raise ModelError("manifest: schemaVersion must be 1")
    source, conversion = manifest.get("source"), manifest.get("conversion")
    if not isinstance(source, dict) or not isinstance(conversion, dict):
        raise ModelError("manifest: source and conversion are required")
    if not _REPOSITORY.fullmatch(str(source.get("repository", ""))):
        raise ModelError("manifest: source.repository must be owner/name")
    if not _REVISION.fullmatch(str(source.get("revision", ""))):
        raise ModelError("manifest: source.revision must be a full commit id")
    files = source.get("files")
    if not isinstance(files, list) or not files:
        raise ModelError("manifest: source.files must be a non-empty list")
    for entry in files:
        _check_file_entry(entry, "name")
    if len({entry["name"] for entry in files}) != len(files):
        raise ModelError("manifest: source.files has a duplicate name")
    if type(conversion.get("opset")) is not int:
        raise ModelError("manifest: conversion.opset must be an integer")
    _check_int8_settings(conversion.get("int8"))
    outputs = conversion.get("outputs")
    if not isinstance(outputs, list):
        raise ModelError("manifest: conversion.outputs must be a list")
    for entry in outputs:
        _check_file_entry(entry, "fileName")
    if outputs:
        names = [entry["fileName"] for entry in outputs]
        if sorted(names) != sorted(OUTPUT_NAMES):
            raise ModelError(f"manifest: conversion.outputs must list {', '.join(OUTPUT_NAMES)}")
        if not isinstance(conversion.get("toolchain"), dict):
            raise ModelError("manifest: recorded outputs need the toolchain that made them")
    return manifest


# --- files ------------------------------------------------------------------

def file_digest(path: Path) -> tuple[int, str]:
    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as stream:
        while chunk := stream.read(DOWNLOAD_CHUNK_BYTES):
            digest.update(chunk)
            size += len(chunk)
    return size, digest.hexdigest()


def check_file(path: Path, size: int, digest: str) -> None:
    if not path.is_file():
        raise ModelError(f"{path} is missing")
    actual_size, actual_digest = file_digest(path)
    if (actual_size, actual_digest) != (size, digest):
        raise ModelError(
            f"{path.name} does not match the pin\n"
            f"  expected: {size} bytes, sha256 {digest}\n"
            f"  actual:   {actual_size} bytes, sha256 {actual_digest}"
        )


def check_inputs(manifest: dict, input_dir: Path) -> None:
    entries = manifest["source"]["files"]
    if not input_dir.is_dir():
        raise ModelError(f"{input_dir} is not a directory")
    # The model loader may read other files in the directory.
    pinned = {entry["name"] for entry in entries}
    unpinned = sorted(path.name for path in input_dir.iterdir() if path.name not in pinned)
    if unpinned:
        raise ModelError(f"{input_dir} holds files that are not pinned: {', '.join(unpinned)}")
    for entry in entries:
        check_file(input_dir / entry["name"], entry["sizeBytes"], entry["sha256"])


def require_new_directory(path: Path) -> None:
    if path.exists() and (not path.is_dir() or any(path.iterdir())):
        raise ModelError(f"{path} must be a new or empty directory")
    path.mkdir(parents=True, exist_ok=True)


def publish(staging: Path, output_dir: Path, names) -> None:
    """Give the finished files their final names: all of them, or none."""
    moved = []
    try:
        for name in names:
            os.replace(staging / name, output_dir / name)
            moved.append(output_dir / name)
    except BaseException:
        for path in moved:
            path.unlink(missing_ok=True)
        raise


# --- fetch ------------------------------------------------------------------

class _HttpsOnlyRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        if not newurl.lower().startswith("https://"):
            raise ModelError("refusing a redirect that is not HTTPS")
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def source_url(manifest: dict, name: str) -> str:
    source = manifest["source"]
    return f"{HUB_URL}/{source['repository']}/resolve/{source['revision']}/{name}"


def download(url: str, destination: Path, size: int, digest: str, opener=None) -> None:
    """Write url to destination only if it has exactly the pinned size and digest."""
    opener = opener or urllib.request.build_opener(_HttpsOnlyRedirect())
    request = urllib.request.Request(url, headers={"User-Agent": "model-fetch"})
    partial = destination.with_name(destination.name + ".part")
    try:
        received = 0
        with opener.open(request, timeout=DOWNLOAD_TIMEOUT_SECONDS) as response, \
                partial.open("wb") as stream:
            while chunk := response.read(DOWNLOAD_CHUNK_BYTES):
                received += len(chunk)
                if received > size:
                    raise ModelError(f"{destination.name}: the download is larger than {size} bytes")
                stream.write(chunk)
        check_file(partial, size, digest)
        os.replace(partial, destination)
    except (OSError, http.client.HTTPException) as error:
        raise ModelError(f"{destination.name}: download failed: {error}") from error
    finally:
        partial.unlink(missing_ok=True)


def fetch(manifest: dict, output_dir: Path) -> None:
    require_new_directory(output_dir)
    entries = manifest["source"]["files"]
    # A failed run leaves output_dir empty, so the same command can be run again.
    with tempfile.TemporaryDirectory(dir=output_dir) as temporary:
        staging = Path(temporary)
        for entry in entries:
            print(f"fetch {entry['name']} ({entry['sizeBytes']} bytes)", flush=True)
            download(source_url(manifest, entry["name"]), staging / entry["name"],
                     entry["sizeBytes"], entry["sha256"])
        publish(staging, output_dir, [entry["name"] for entry in entries])
    print(f"fetched {len(entries)} files into {output_dir}")


# --- environment ------------------------------------------------------------

def lock_digest(path: Path = LOCK_PATH) -> str:
    """SHA256 of the dependency lock, unaffected by the checkout's line endings."""
    try:
        data = path.read_bytes()
    except OSError as error:
        raise ModelError(f"cannot read the dependency lock {path}: {error}") from error
    return hashlib.sha256(data.replace(b"\r\n", b"\n")).hexdigest()


def current_toolchain() -> dict:
    packages = {}
    for name in TOOLCHAIN_PACKAGES:
        try:
            packages[name] = importlib.metadata.version(name)
        except importlib.metadata.PackageNotFoundError as error:
            raise ModelError(f"{name} is not installed; run this script with uv") from error
    return {
        "platform": f"{sys.platform}-{platform.machine()}",
        "python": platform.python_version(),
        "packages": packages,
        "lockSha256": lock_digest(),
    }


def go_offline() -> None:
    """Keep transformers from contacting the Hub while it reads the local directory."""
    os.environ["HF_HUB_OFFLINE"] = "1"
    os.environ["TRANSFORMERS_OFFLINE"] = "1"
    os.environ["HF_HUB_DISABLE_TELEMETRY"] = "1"


def load_model(input_dir: Path, attention: str):
    go_offline()
    import torch
    from transformers import AutoModelForMaskedLM

    model, info = AutoModelForMaskedLM.from_pretrained(
        str(input_dir),
        local_files_only=True,
        trust_remote_code=False,
        use_safetensors=True,
        dtype=torch.float32,
        attn_implementation=attention,
        reference_compile=False,
        output_loading_info=True,
    )
    problems = {key: sorted(value) for key, value in info.items() if value}
    if problems:
        raise ModelError(f"the checkpoint does not match the model class: {problems}")
    return model.eval()


def masked_logits_module(model):
    """The exported graph: logits of one position per row, [N, vocabulary]."""
    import torch

    class MaskedLogits(torch.nn.Module):
        def __init__(self, masked_lm):
            super().__init__()
            self.encoder = masked_lm.model
            self.head = masked_lm.head
            self.decoder = masked_lm.decoder

        def forward(self, input_ids, attention_mask, mask_positions):
            hidden = self.encoder(input_ids=input_ids, attention_mask=attention_mask)[0]
            index = mask_positions.view(-1, 1, 1).expand(-1, 1, hidden.shape[2])
            picked = torch.gather(hidden, 1, index).squeeze(1)
            return self.decoder(self.head(picked))

    return MaskedLogits(model).eval()


# --- convert ----------------------------------------------------------------

def _strip_graph(graph) -> None:
    graph.doc_string = ""
    del graph.metadata_props[:]
    for values in (graph.input, graph.output, graph.value_info, graph.initializer):
        for value in values:
            value.doc_string = ""
            del value.metadata_props[:]
    for node in graph.node:
        node.doc_string = ""
        del node.metadata_props[:]
        for attribute in node.attribute:
            attribute.doc_string = ""
            if attribute.HasField("g"):
                _strip_graph(attribute.g)
            for subgraph in attribute.graphs:
                _strip_graph(subgraph)


def write_normalized(model, path: Path) -> None:
    """Serialize without text that can carry local paths, in a stable byte order."""
    model.doc_string = ""
    del model.metadata_props[:]
    _strip_graph(model.graph)
    for function in model.functions:
        function.doc_string = ""
        del function.metadata_props[:]
        for node in function.node:
            node.doc_string = ""
            del node.metadata_props[:]
    path.write_bytes(model.SerializeToString(deterministic=True))


def export_fp32(manifest: dict, input_dir: Path, work_dir: Path, destination: Path) -> None:
    import onnx
    import torch

    torch.set_num_threads(1)
    torch.use_deterministic_algorithms(True)
    torch.manual_seed(0)
    # Eager attention exports as plain MatMul and Softmax nodes.
    wrapper = masked_logits_module(load_model(input_dir, attention="eager"))
    # Batch and length differ from 1 so that neither is specialized by the trace.
    input_ids = torch.full((2, 9), MASK_ID, dtype=torch.int64)
    attention_mask = torch.ones((2, 9), dtype=torch.int64)
    mask_positions = torch.tensor([1, 2], dtype=torch.int64)
    batch = torch.export.Dim("batch", min=1, max=1024)
    sequence = torch.export.Dim("sequence", min=2, max=8192)
    raw = work_dir / "exported.onnx"
    torch.onnx.export(
        wrapper,
        (input_ids, attention_mask, mask_positions),
        str(raw),
        input_names=["input_ids", "attention_mask", "mask_positions"],
        output_names=["logits"],
        opset_version=manifest["conversion"]["opset"],
        dynamo=True,
        dynamic_shapes={
            "input_ids": {0: batch, 1: sequence},
            "attention_mask": {0: batch, 1: sequence},
            "mask_positions": {0: batch},
        },
        external_data=False,
        verbose=False,
    )
    write_normalized(onnx.load(str(raw)), destination)
    onnx.checker.check_model(str(destination))


def convert_fp16(source: Path, destination: Path) -> None:
    import onnx
    from onnxruntime.transformers.float16 import convert_float_to_float16

    model = convert_float_to_float16(onnx.load(str(source)), keep_io_types=True)
    write_normalized(model, destination)
    onnx.checker.check_model(str(destination))


def fp32_linear_nodes(graph, shape: list[int], count: int) -> list[str]:
    """MatMul nodes that stay in fp32, selected by the shape of their weight."""
    shapes = {tensor.name: list(tensor.dims) for tensor in graph.initializer}
    names = [node.name for node in graph.node
             if node.op_type == "MatMul" and shapes.get(node.input[1]) == shape]
    if len(names) != count:
        raise ModelError(
            f"expected {count} MatMul nodes with a {shape} weight, found {len(names)}")
    # The quantizer excludes by name, so each name must point at exactly one node.
    uses = Counter(node.name for node in graph.node)
    if any(not name or uses[name] != 1 for name in names):
        raise ModelError("every MatMul node that stays in fp32 needs a name of its own")
    return names


def convert_int8(settings: dict, source: Path, work_dir: Path, destination: Path) -> None:
    import onnx
    from onnxruntime.quantization import QuantType, quantize_dynamic

    excluded = fp32_linear_nodes(onnx.load(str(source)).graph,
                                 settings["fp32LinearWeightShape"], settings["fp32LinearCount"])
    raw = work_dir / "quantized.onnx"
    quantize_dynamic(
        str(source),
        str(raw),
        op_types_to_quantize=list(settings["operators"]),
        weight_type=getattr(QuantType, settings["weightType"]),
        per_channel=settings["perChannel"],
        nodes_to_exclude=excluded,
    )
    write_normalized(onnx.load(str(raw)), destination)
    onnx.checker.check_model(str(destination))


def describe_outputs(output_dir: Path) -> list[dict]:
    outputs = []
    for name in OUTPUT_NAMES:
        path = output_dir / name
        if not path.is_file():
            raise ModelError(f"{path} is missing")
        size, digest = file_digest(path)
        outputs.append({"fileName": name, "sizeBytes": size, "sha256": digest})
    return outputs


def output_differences(recorded: list[dict], actual: list[dict]) -> list[str]:
    by_name = {entry["fileName"]: entry for entry in actual}
    differences = []
    for expected in recorded:
        found = by_name.get(expected["fileName"])
        if found != expected:
            got = "nothing" if found is None else f"{found['sizeBytes']} bytes, {found['sha256']}"
            differences.append(
                f"{expected['fileName']}: expected {expected['sizeBytes']} bytes, "
                f"{expected['sha256']}; got {got}"
            )
    return differences


def check_no_local_paths(models: list[Path], directories: list[Path]) -> None:
    """Fail when a serialized model names a directory of this machine."""
    texts = {"site-packages"}
    for directory in directories:
        texts.update((str(directory), directory.as_posix()))
    for model in models:
        # Windows paths ignore case.
        data = model.read_bytes().lower()
        found = sorted(text for text in texts if text.encode("utf-8").lower() in data)
        if found:
            raise ModelError(f"{model.name} contains local path text: {', '.join(found)}")


def write_manifest(manifest: dict, path: Path = MANIFEST_PATH) -> None:
    text = json.dumps(manifest, ensure_ascii=False, indent=2) + "\n"
    temporary = path.with_name(path.name + ".part")
    temporary.write_text(text, encoding="utf-8", newline="\n")
    os.replace(temporary, path)


def convert(manifest: dict, input_dir: Path, output_dir: Path, record: bool,
            manifest_path: Path = MANIFEST_PATH) -> int:
    check_inputs(manifest, input_dir)
    toolchain = current_toolchain()
    require_new_directory(output_dir)
    # Nothing gets its final name until every output exists and is accepted.
    with tempfile.TemporaryDirectory(dir=output_dir) as temporary:
        staging = Path(temporary)
        print("export fp32", flush=True)
        export_fp32(manifest, input_dir, staging, staging / FP32_NAME)
        print("convert fp16", flush=True)
        convert_fp16(staging / FP32_NAME, staging / FP16_NAME)
        print("quantize int8", flush=True)
        convert_int8(manifest["conversion"]["int8"], staging / FP32_NAME, staging,
                     staging / INT8_NAME)
        check_no_local_paths(
            [staging / name for name in (FP32_NAME, FP16_NAME, INT8_NAME)],
            [input_dir, output_dir, Path.home()])
        shutil.copyfile(input_dir / TOKENIZER_NAME, staging / TOKENIZER_NAME)
        outputs = describe_outputs(staging)
        for entry in outputs:
            print(f"{entry['fileName']}: {entry['sizeBytes']} bytes, sha256 {entry['sha256']}")

        if record:
            manifest["conversion"]["toolchain"] = toolchain
            manifest["conversion"]["outputs"] = outputs
            write_manifest(manifest, manifest_path)
            publish(staging, output_dir, OUTPUT_NAMES)
            print(f"recorded the outputs in {manifest_path.name}")
            return 0
        recorded = manifest["conversion"]["outputs"]
        if not recorded:
            print("the manifest has no recorded outputs, so none was kept; "
                  "rerun with --record to pin them", file=sys.stderr)
            return 2
        differences = output_differences(recorded, outputs)
        if differences:
            print("the outputs do not match the manifest, so none was kept:", file=sys.stderr)
            for line in differences:
                print(f"  {line}", file=sys.stderr)
            print(f"  recorded toolchain: {json.dumps(manifest['conversion']['toolchain'])}",
                  file=sys.stderr)
            print(f"  current toolchain:  {json.dumps(toolchain)}", file=sys.stderr)
            return 1
        publish(staging, output_dir, OUTPUT_NAMES)
    print("all outputs match the manifest")
    return 0


# --- verify -----------------------------------------------------------------

def masked_positions(length: int, limit: int = MAX_ROWS_PER_SENTENCE) -> list[int]:
    """Non-special positions; evenly spaced, ends included, when there are too many."""
    inner = length - 2
    if inner <= limit:
        return list(range(1, length - 1))
    return [1 + round(index * (inner - 1) / (limit - 1)) for index in range(limit)]


def sentence_batch(sentences: list[list[int]]):
    """One row per masked position of every sentence, padded to one length."""
    import numpy

    width = max(len(ids) for ids in sentences)
    rows, masks, positions, targets, owners = [], [], [], [], []
    for owner, ids in enumerate(sentences):
        for position in masked_positions(len(ids)):
            row = ids + [PAD_ID] * (width - len(ids))
            targets.append(row[position])
            row[position] = MASK_ID
            rows.append(row)
            masks.append([1] * len(ids) + [0] * (width - len(ids)))
            positions.append(position)
            owners.append(owner)
    return (
        {
            "input_ids": numpy.array(rows, dtype=numpy.int64),
            "attention_mask": numpy.array(masks, dtype=numpy.int64),
            "mask_positions": numpy.array(positions, dtype=numpy.int64),
        },
        numpy.array(targets),
        numpy.array(owners),
    )


def target_log_probabilities(logits, targets):
    import numpy

    logits = logits.astype(numpy.float64)
    peak = logits.max(axis=1, keepdims=True)
    normalizer = numpy.log(numpy.exp(logits - peak).sum(axis=1)) + peak[:, 0]
    return logits[numpy.arange(len(targets)), targets] - normalizer


def sentence_scores(values, owners, count: int) -> list[float]:
    """Sum of the masked log probabilities of each sentence."""
    return [float(values[owners == owner].sum()) for owner in range(count)]


def score_sentences(run, sentences: list[list[int]]):
    """The log probability of every masked position, and the sum for each sentence."""
    batch, targets, owners = sentence_batch(sentences)
    values = target_log_probabilities(run(batch), targets)
    return values, sentence_scores(values, owners, len(sentences))


def onnx_runner(path: Path):
    import onnxruntime

    options = onnxruntime.SessionOptions()
    options.intra_op_num_threads = VERIFY_THREADS
    options.inter_op_num_threads = 1
    session = onnxruntime.InferenceSession(
        str(path), sess_options=options, providers=["CPUExecutionProvider"])
    return lambda batch: session.run(["logits"], batch)[0]


def torch_runner(input_dir: Path):
    """The upstream masked LM, not the exported wrapper, read at the masked positions."""
    import torch

    torch.set_num_threads(VERIFY_THREADS)
    model = load_model(input_dir, attention="sdpa")

    def run(batch):
        tensors = {key: torch.from_numpy(value) for key, value in batch.items()}
        picked = []
        with torch.inference_mode():
            for start in range(0, len(tensors["mask_positions"]), REFERENCE_ROWS_PER_PASS):
                rows = slice(start, start + REFERENCE_ROWS_PER_PASS)
                logits = model(input_ids=tensors["input_ids"][rows],
                               attention_mask=tensors["attention_mask"][rows]).logits
                positions = tensors["mask_positions"][rows]
                picked.append(logits[torch.arange(len(positions)), positions].clone())
        return torch.cat(picked).numpy()

    return run


def large_initializers(path: Path, limit: int = 6) -> list[dict]:
    import numpy
    import onnx

    model = onnx.load(str(path))
    found = []
    for tensor in model.graph.initializer:
        count = int(numpy.prod(tensor.dims)) if tensor.dims else 1
        width = onnx.helper.tensor_dtype_to_np_dtype(tensor.data_type).itemsize
        found.append({
            "name": tensor.name,
            "shape": list(tensor.dims),
            "type": onnx.TensorProto.DataType.Name(tensor.data_type),
            "bytes": count * width,
        })
    return sorted(found, key=lambda item: -item["bytes"])[:limit]


def check_parity(reference, candidate, cases: dict[str, list[list[int]]]) -> dict:
    """Compare logits and sentence scores for batches of several shapes."""
    import numpy

    result = {}
    for name, sentences in cases.items():
        batch, targets, owners = sentence_batch(sentences)
        progress(f"parity {name} {list(batch['input_ids'].shape)}")
        expected, actual = reference(batch), candidate(batch)
        left = target_log_probabilities(expected, targets)
        right = target_log_probabilities(actual, targets)
        result[name] = {
            "shape": list(batch["input_ids"].shape),
            "maxLogitDifference": float(numpy.abs(expected - actual).max()),
            "maxScoreDifference": float(max(
                abs(left[owners == owner].sum() - right[owners == owner].sum())
                for owner in range(len(sentences)))),
        }
    return result


def progress(message: str) -> None:
    print(f"[{time.monotonic():.1f}] {message}", file=sys.stderr, flush=True)


def verify(manifest: dict, input_dir: Path, model_dir: Path) -> int:
    import sentencepiece

    check_inputs(manifest, input_dir)
    recorded = manifest["conversion"]["outputs"]
    actual = describe_outputs(model_dir)
    differences = output_differences(recorded, actual) if recorded else []

    processor = sentencepiece.SentencePieceProcessor(model_file=str(input_dir / TOKENIZER_NAME))
    for piece_id, piece in SPECIAL_PIECES.items():
        if processor.id_to_piece(piece_id) != piece:
            raise ModelError(f"tokenizer id {piece_id} is not {piece}")

    def encode(text):
        return [BOS_ID, *processor.encode(text), EOS_ID]

    pairs = [[encode(first), encode(second)] for first, second in SENTENCE_PAIRS]
    every = [ids for pair in pairs for ids in pair]
    long_ids = encode(LONG_SENTENCE)
    cases = {
        "singleShortRow": [[BOS_ID, every[0][1], EOS_ID]],
        "oneSentence": [every[0]],
        "paddedBatch": every,
        "longerThanLocalWindow": [long_ids],
        "paddedLongAndShort": [long_ids, every[1]],
    }

    progress("load PyTorch")
    torch_run = torch_runner(input_dir)
    progress("load fp32 ONNX")
    fp32_run = onnx_runner(model_dir / FP32_NAME)
    parity = check_parity(torch_run, fp32_run, cases)

    # First and last positions, including the special tokens.
    ids = every[2]
    import numpy
    edge_batch = {
        "input_ids": numpy.array([ids, ids], dtype=numpy.int64),
        "attention_mask": numpy.ones((2, len(ids)), dtype=numpy.int64),
        "mask_positions": numpy.array([0, len(ids) - 1], dtype=numpy.int64),
    }
    edge_difference = float(numpy.abs(torch_run(edge_batch) - fp32_run(edge_batch)).max())

    alone = [score_sentences(fp32_run, [ids])[1][0] for ids in every]
    together = score_sentences(fp32_run, every)[1]
    padding_difference = max(abs(a - b) for a, b in zip(alone, together))

    passed = (
        edge_difference <= PARITY_LOGIT_TOLERANCE
        and padding_difference <= PARITY_SCORE_TOLERANCE
        and all(case["maxLogitDifference"] <= PARITY_LOGIT_TOLERANCE
                and case["maxScoreDifference"] <= PARITY_SCORE_TOLERANCE
                for case in parity.values())
    )
    report = {
        "toolchain": current_toolchain(),
        "manifestDifferences": differences if recorded else "no recorded outputs",
        "torchAgainstFp32": {
            "passed": passed,
            "cases": parity,
            "firstAndLastPositionMaxLogitDifference": edge_difference,
            "paddedAgainstAloneMaxScoreDifference": padding_difference,
        },
        "variants": {},
    }

    scored = every + [long_ids]
    base_values, baseline = score_sentences(fp32_run, scored)
    base_margins = [baseline[2 * i] - baseline[2 * i + 1] for i in range(len(pairs))]
    failed_int8 = False
    for name in (FP32_NAME, FP16_NAME, INT8_NAME):
        entry = {
            "sizeBytes": (model_dir / name).stat().st_size,
            "largeInitializers": large_initializers(model_dir / name),
        }
        progress(f"score {name}")
        try:
            run = fp32_run if name == FP32_NAME else onnx_runner(model_dir / name)
            values, scores = score_sentences(run, scored)
            alone = [score_sentences(run, [ids])[1][0] for ids in scored]
        except Exception as error:  # ONNX Runtime raises its own unrelated types.
            entry["runsOnCpu"] = False
            entry["error"] = f"{type(error).__name__}: {error}"
            failed_int8 = failed_int8 or name == INT8_NAME
        else:
            entry["runsOnCpu"] = True
            margins = [scores[2 * i] - scores[2 * i + 1] for i in range(len(pairs))]
            entry["meanPositionDifferenceFromFp32"] = float(
                abs(values - base_values).mean())
            entry["maxScoreDifferenceFromFp32"] = max(
                abs(a - b) for a, b in zip(scores, baseline))
            # Dynamic quantization takes its activation range from the whole batch.
            entry["maxScoreDifferenceAloneAgainstBatch"] = max(
                abs(a - b) for a, b in zip(alone, scores))
            entry["maxPairMarginDifferenceFromFp32"] = max(
                abs(a - b) for a, b in zip(margins, base_margins))
            entry["pairsRankedAsFp32"] = sum(
                (a > 0) == (b > 0) for a, b in zip(margins, base_margins))
            entry["pairsPreferringFirst"] = sum(margin > 0 for margin in margins)
            entry["pairCount"] = len(pairs)
        report["variants"][name] = entry

    print(json.dumps(report, ensure_ascii=False, indent=2))
    if differences or not passed or failed_int8:
        return 1
    return 0


# --- command line -----------------------------------------------------------

def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest="command", required=True)
    fetch_parser = commands.add_parser("fetch", help="download the pinned upstream files")
    fetch_parser.add_argument("--output-dir", type=Path, required=True)
    convert_parser = commands.add_parser("convert", help="write the ONNX models")
    convert_parser.add_argument("--input-dir", type=Path, required=True)
    convert_parser.add_argument("--output-dir", type=Path, required=True)
    convert_parser.add_argument(
        "--record", action="store_true",
        help="pin the digests of this run in the manifest instead of comparing them")
    verify_parser = commands.add_parser("verify", help="compare the models with PyTorch")
    verify_parser.add_argument("--input-dir", type=Path, required=True)
    verify_parser.add_argument("--model-dir", type=Path, required=True)
    arguments = parser.parse_args(argv)

    # The exporter prints characters that a legacy Windows code page cannot encode.
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="backslashreplace")

    try:
        manifest = load_manifest()
        if arguments.command == "fetch":
            fetch(manifest, arguments.output_dir.resolve())
            return 0
        if arguments.command == "convert":
            return convert(manifest, arguments.input_dir.resolve(),
                           arguments.output_dir.resolve(), arguments.record)
        return verify(manifest, arguments.input_dir.resolve(), arguments.model_dir.resolve())
    except ModelError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
