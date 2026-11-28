"""Train the two M56 models and verify their interchange format with ORT."""

from __future__ import annotations

from importlib.metadata import version
import json
import math
from pathlib import Path
import platform
from typing import cast

from google.protobuf.descriptor import FieldDescriptor
from google.protobuf.message import Message
from lightgbm import LGBMClassifier
import numpy as np
from numpy.typing import NDArray
import onnx
from onnx import TensorProto, helper, numpy_helper
import onnxmltools
from onnxmltools.convert.common.data_types import FloatTensorType
import onnxruntime as ort
import torch
from torch import nn

from training_corpus import TrainingError, json_bytes, sha256_file
from training_data import FEATURES, feature_vector, read_jsonl


MODEL_NAMES = {"v0": "tiny_reranker_v0.onnx", "v1": "tiny_reranker.onnx"}
INPUT = "features_v1"
OUTPUT = "rerank_score"
LIGHTGBM_TARGET_OPSET = 15
MODEL_OPSETS = {
    "v0": {"ai.onnx": 9, "ai.onnx.ml": 1},
    "v1": {"ai.onnx": 20},
}
IR_VERSION = 9
ORT_VERSION = "1.32.0"
SEED = 2153
WIDTH = len(FEATURES)
Array = NDArray[np.float32]

torch.set_num_threads(1)
torch.set_num_interop_threads(1)
torch.use_deterministic_algorithms(True)


def _meta(model_version: str, dataset_hash: str, opsets: dict[str, int]) -> dict[str, str]:
    metadata = {"model_version": model_version, "feature_schema": INPUT, "dataset_sha256": dataset_hash}
    if model_version == "v0":
        metadata["opsets"] = json.dumps(opsets, sort_keys=True)
    return metadata


def _opsets(model: onnx.ModelProto) -> dict[str, int]:
    return {item.domain or "ai.onnx": item.version for item in model.opset_import}


def _graph(
    variant: str, dataset_hash: str, mean: Array, scale: Array, nodes: list[onnx.NodeProto],
    initializers: list[onnx.TensorProto], *, opsets: dict[str, int],
) -> onnx.ModelProto:
    preprocessing = [
        helper.make_node("Sub", [INPUT, "feature_mean"], ["centered"]),
        helper.make_node("Div", ["centered", "feature_scale"], ["normalized"]),
    ]
    graph = helper.make_graph(
        [*preprocessing, *nodes], f"tiny_reranker_{variant}",
        [helper.make_tensor_value_info(INPUT, TensorProto.FLOAT, ["batch", WIDTH])],
        [helper.make_tensor_value_info(OUTPUT, TensorProto.FLOAT, ["batch", 1])],
        [numpy_helper.from_array(mean, "feature_mean"),
         numpy_helper.from_array(scale, "feature_scale"), *initializers],
    )
    imports = [
        helper.make_opsetid("" if domain == "ai.onnx" else domain, version)
        for domain, version in sorted(opsets.items())
    ]
    model = helper.make_model(graph, producer_name="azooKey M56", opset_imports=imports,
                              ir_version=IR_VERSION)
    helper.set_model_props(model, _meta(variant, dataset_hash, _opsets(model)))
    return model


def _v0(
    x: Array, y: Array, weights: Array, probe: Array, mean: Array, scale: Array, dataset_hash: str,
) -> tuple[onnx.ModelProto, Array]:
    estimator = LGBMClassifier(
        objective="binary", n_estimators=32, min_child_samples=4, n_jobs=1, random_state=SEED,
        deterministic=True, force_col_wise=True, verbosity=-1,
    )
    estimator.fit(x, y.astype(np.int32), sample_weight=weights)
    converted = onnxmltools.convert_lightgbm(
        estimator, name="tiny_lightgbm", initial_types=[("normalized", FloatTensorType([None, WIDTH]))],
        target_opset=LIGHTGBM_TARGET_OPSET, zipmap=False,
    )
    if [value.name for value in converted.graph.input] != ["normalized"] or len(converted.graph.output) != 2:
        raise TrainingError("unexpected LightGBM converter interface")
    if any(node.op_type not in {"Cast", "Identity", "Mul", "TreeEnsembleClassifier"}
           for node in converted.graph.node):
        raise TrainingError("unexpected LightGBM converter operator")
    if converted.functions or converted.graph.sparse_initializer:
        raise TrainingError("unexpected LightGBM converter graph")
    opsets = _opsets(converted)
    if opsets != MODEL_OPSETS["v0"]:
        raise TrainingError(f"unexpected LightGBM converter opsets: {opsets}")
    probabilities = converted.graph.output[1].name
    model = _graph(
        "v0", dataset_hash, mean, scale,
        [*converted.graph.node, helper.make_node("Gather", [probabilities, "positive_class"], [OUTPUT], axis=1)],
        [*converted.graph.initializer, numpy_helper.from_array(np.array([1], dtype=np.int64), "positive_class")],
        opsets=opsets,
    )
    expected = np.asarray(estimator.predict_proba(probe)[:, 1:2], dtype=np.float32)
    return model, expected


def _v1(
    x: Array, y: Array, weights: Array, probe: Array, mean: Array, scale: Array, dataset_hash: str,
) -> tuple[onnx.ModelProto, Array]:
    torch.manual_seed(SEED)
    network = nn.Sequential(nn.Linear(WIDTH, 64), nn.GELU(), nn.Linear(64, 64), nn.GELU(), nn.Linear(64, 1))
    optimizer = torch.optim.Adam(network.parameters(), lr=0.02, foreach=False)
    data = torch.from_numpy(x)
    targets = torch.from_numpy(y).reshape(-1, 1)
    loss = nn.BCEWithLogitsLoss(weight=torch.from_numpy(weights).reshape(-1, 1))
    for _ in range(40):
        optimizer.zero_grad()
        loss(network(data), targets).backward()
        optimizer.step()
    network.eval()
    with torch.no_grad():
        expected = torch.sigmoid(network(torch.from_numpy(probe))).numpy()

    # This fixed architecture maps directly to standard ONNX ops. Serializing the
    # learned tensors directly keeps export names and topology independent of tracing.
    nodes = []
    tensors = []
    previous = "normalized"
    for index, layer in enumerate(network):
        output = f"layer_{index}"
        if isinstance(layer, nn.Linear):
            weight, bias = f"{output}_weight", f"{output}_bias"
            tensors.append(numpy_helper.from_array(layer.weight.detach().numpy().T.copy(), weight))
            tensors.append(numpy_helper.from_array(layer.bias.detach().numpy(), bias))
            nodes.append(helper.make_node("Gemm", [previous, weight, bias], [output]))
        else:
            nodes.append(helper.make_node("Gelu", [previous], [output], approximate="none"))
        previous = output
    nodes.append(helper.make_node("Sigmoid", [previous], [OUTPUT]))
    model = _graph("v1", dataset_hash, mean, scale, nodes, tensors, opsets=MODEL_OPSETS["v1"])
    return model, cast(Array, expected)


def _has_external_tensor(message: Message) -> bool:
    if isinstance(message, TensorProto):
        return message.data_location == TensorProto.EXTERNAL
    for field, value in message.ListFields():
        if field.type == FieldDescriptor.TYPE_MESSAGE:
            children = value if field.is_repeated else (value,)
            if any(_has_external_tensor(child) for child in children):
                return True
    return False


def _session(path: Path, variant: str, dataset_hash: str) -> ort.InferenceSession:
    if ort.__version__ != ORT_VERSION:
        raise TrainingError(f"verification requires ONNX Runtime {ORT_VERSION}")
    model = onnx.load(path, load_external_data=False)
    opsets = _opsets(model)
    if model.ir_version != IR_VERSION or opsets != MODEL_OPSETS[variant]:
        raise TrainingError(f"unexpected {variant} ONNX IR or opsets")
    if _has_external_tensor(model):
        raise TrainingError("external ONNX weights are unsupported")
    onnx.checker.check_model(model)
    options = ort.SessionOptions()
    options.intra_op_num_threads = 1
    options.inter_op_num_threads = 1
    session = ort.InferenceSession(str(path), sess_options=options, providers=["CPUExecutionProvider"])
    inputs = [(item.name, item.type, item.shape) for item in session.get_inputs()]
    outputs = [(item.name, item.type, item.shape) for item in session.get_outputs()]
    if inputs != [(INPUT, "tensor(float)", ["batch", WIDTH])] or outputs != [
        (OUTPUT, "tensor(float)", ["batch", 1])
    ] or session.get_modelmeta().custom_metadata_map != _meta(variant, dataset_hash, opsets):
        raise TrainingError("ONNX feature schema, output, or metadata mismatch")
    return session


def _predict(session: ort.InferenceSession, inputs: Array) -> Array:
    result = np.asarray(session.run([OUTPUT], {INPUT: inputs})[0])
    if result.dtype != np.float32 or result.shape != (len(inputs), 1) or not np.isfinite(result).all():
        raise TrainingError("invalid fp32 ONNX scores")
    return cast(Array, result)


def _matrix(rows: list[tuple[tuple[float, ...], int, float]]) -> tuple[Array, Array, Array]:
    if {label for _, label, _ in rows} != {0, 1}:
        raise TrainingError("both label classes are required in each split")
    vectors, labels, weights = zip(*rows)
    return (np.asarray(vectors, dtype=np.float32), np.asarray(labels, dtype=np.float32),
            np.asarray(weights, dtype=np.float32))


def _training_inputs(directory: Path) -> tuple[
    tuple[Array, Array, Array], tuple[Array, Array, Array], str,
]:
    dataset = directory / "labels.jsonl"
    source_receipt = json.loads((directory / "synthesis.json").read_bytes())
    dataset_hash = sha256_file(dataset)
    if (source_receipt.get("schema") != 1 or source_receipt.get("features") != list(FEATURES)
        or source_receipt.get("dataset_sha256") != dataset_hash
        or source_receipt.get("m52_exclusions_sha256") != sha256_file(directory / "m52-exclusions.json")):
        raise TrainingError("synthesis receipt hash or schema mismatch")

    splits: dict[str, list[tuple[tuple[float, ...], int, float]]] = {"train": [], "holdout": []}
    for row in read_jsonl(dataset):
        split, label, weight = row.get("split"), row.get("label"), row.get("weight")
        if (not isinstance(split, str) or split not in splits or type(label) is not int
            or label not in (0, 1) or not isinstance(weight, (int, float)) or isinstance(weight, bool)
            or not math.isfinite(weight) or not 0 < weight <= 1):
            raise TrainingError("invalid labeled example")
        splits[split].append((feature_vector(row.get("features")), label, float(weight)))
    return _matrix(splits["train"]), _matrix(splits["holdout"]), dataset_hash


def train(dataset: Path, output: Path) -> None:
    (raw, labels, weights), (holdout, _, _), dataset_hash = _training_inputs(dataset)
    mean, scale = np.zeros(WIDTH, dtype=np.float32), np.ones(WIDTH, dtype=np.float32)
    mean[:6] = raw[:, :6].mean(axis=0, dtype=np.float64)
    deviation = raw[:, :6].std(axis=0, dtype=np.float64)
    scale[:6] = np.where(deviation > 0.0, deviation, 1.0)
    if not np.isfinite(scale).all() or not (scale > 0).all():
        raise TrainingError("invalid normalization statistics")
    normalized = (raw - mean) / scale
    probe = (holdout - mean) / scale

    model_hashes = {}
    model_opsets = {}
    for variant, fit in (("v0", _v0), ("v1", _v1)):
        model, expected = fit(normalized, labels, weights, probe, mean, scale, dataset_hash)
        path = output / MODEL_NAMES[variant]
        path.write_bytes(model.SerializeToString(deterministic=True))
        session = _session(path, variant, dataset_hash)
        actual = _predict(session, holdout)
        _predict(session, holdout[:1])
        _predict(session, np.resize(holdout, (30, WIDTH)))
        if not np.allclose(actual, expected, rtol=2e-5, atol=2e-5):
            raise TrainingError(f"{variant} ONNX predictions differ from the trained model")
        model_hashes[path.name] = sha256_file(path)
        model_opsets[path.name] = _opsets(model)
    receipt = {
        "schema": 1, "dataset_sha256": dataset_hash,
        "synthesis_sha256": sha256_file(dataset / "synthesis.json"),
        "lock_sha256": sha256_file(Path(__file__).with_name("tiny_reranker.py.lock")),
        "models": model_hashes, "opsets": model_opsets, "ir": IR_VERSION, "seed": SEED,
        "train_rows": len(raw), "holdout_rows": len(holdout),
        "normalization": {"mean": mean.tolist(), "scale": scale.tolist()},
        "python": platform.python_version(), "platform": platform.platform(),
        "packages": {name: version(name) for name in (
            "lightgbm", "numpy", "onnx", "onnxmltools", "onnxruntime", "scikit-learn", "torch",
        )},
    }
    (output / "receipt.json").write_bytes(json_bytes(receipt))


def verify(directory: Path) -> dict[str, str]:
    receipt = json.loads((directory / "receipt.json").read_bytes())
    expected_files = set(MODEL_NAMES.values())
    if receipt.get("schema") != 1 or set(receipt.get("models", ())) != expected_files:
        raise TrainingError("invalid model receipt")
    if receipt.get("opsets") != {name: MODEL_OPSETS[variant] for variant, name in MODEL_NAMES.items()}:
        raise TrainingError("model receipt opsets mismatch")
    hashes: dict[str, str] = {}
    for variant, name in MODEL_NAMES.items():
        path = directory / name
        hashes[name] = sha256_file(path)
        if receipt["models"][name] != hashes[name]:
            raise TrainingError(f"model SHA256 mismatch: {name}")
        session = _session(path, variant, receipt["dataset_sha256"])
        probe = np.zeros((30, WIDTH), dtype=np.float32)
        probe[:, 0] = -10
        probe[:, 6:8] = 1
        _predict(session, probe)
    return hashes
