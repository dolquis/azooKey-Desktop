# /// script
# requires-python = "==3.13.15"
# dependencies = [
#     "lightgbm==4.6.0",
#     "mypy==1.18.2",
#     "numpy==2.5.5",
#     "onnx==1.23.2",
#     "onnxmltools==1.16.0",
#     "onnxruntime==1.32.0",
#     "scikit-learn==1.8.0",
#     "sudachipy==0.6.11",
#     "torch==2.15.0",
# ]
# ///
"""Offline training and verification for the M56 tiny reranker."""

from __future__ import annotations

import argparse
from collections.abc import Callable, Sequence
from pathlib import Path
import sys
import tempfile
import zipfile

from training_corpus import (
    TrainingError, fetch_asset, json_bytes, load_manifest, read_work, sha256_file, sudachi_reader,
)
from training_data import FEATURES, build_exclusions, fixture_rows, read_jsonl, synthesize, write_jsonl


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
BUILD = ROOT / "build" / "tiny-reranker"


def publish(output: Path, action: Callable[[Path], None]) -> None:
    """Leave an existing output alone and publish only complete new directories."""
    if output.exists():
        raise TrainingError("output directory already exists")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output.parent) as temporary:
        staging = Path(temporary) / "result"
        staging.mkdir()
        action(staging)
        staging.rename(output)


def make_fixture(output: Path) -> None:
    rows = list(fixture_rows())
    write_jsonl(output / "queries.jsonl", (query for query, _ in rows))
    write_jsonl(output / "candidates.jsonl", (batch for _, batch in rows))


def prepare(manifest: Path, cache: Path, output: Path) -> None:
    works, dictionary = load_manifest(manifest)
    if not works:
        raise TrainingError("source manifest has no approved corpus assets")
    with tempfile.TemporaryDirectory(dir=output) as temporary:
        read = sudachi_reader(dictionary, cache, Path(temporary))
        write_jsonl(output / "queries.jsonl", (row for work in works for row in read_work(work, cache, read)))


def make_labels(queries: Path, candidates: Path, m52: Path, output: Path) -> None:
    exclusions = build_exclusions(m52)
    query_hash, candidate_hash = sha256_file(queries), sha256_file(candidates)
    labels, counts = synthesize(read_jsonl(queries), read_jsonl(candidates), exclusions)
    if sha256_file(queries) != query_hash or sha256_file(candidates) != candidate_hash:
        raise TrainingError("queries or candidates changed during synthesis")
    labels_path = output / "labels.jsonl"
    exclusion_path = output / "m52-exclusions.json"
    write_jsonl(labels_path, labels)
    exclusion_path.write_bytes(json_bytes(exclusions))
    (output / "synthesis.json").write_bytes(json_bytes({
        "schema": 1, "features": list(FEATURES), "queries_sha256": query_hash,
        "candidates_sha256": candidate_hash, "m52_exclusions_sha256": sha256_file(exclusion_path),
        "dataset_sha256": sha256_file(labels_path), "counts": counts,
    }))


def check_reproducibility(dataset: Path, output: Path) -> None:
    from training_models import train, verify

    for name in ("first", "second"):
        (output / name).mkdir()
        train(dataset, output / name)
    first, second = (verify(output / name) for name in ("first", "second"))
    if first != second:
        raise TrainingError("retraining changed model SHA256: " + ", ".join(
            name for name in first if first[name] != second[name]
        ))
    if sha256_file(output / "first" / "receipt.json") != sha256_file(output / "second" / "receipt.json"):
        raise TrainingError("retraining changed the training receipt")
    (output / "reproducibility.json").write_bytes(json_bytes({"models": first, "repeated": 2}))


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    fetch = commands.add_parser("fetch", help="verify cached or explicitly download approved corpus archives")
    prepare_cmd = commands.add_parser("prepare", help="extract queries using cached Aozora and Sudachi assets")
    for command in (fetch, prepare_cmd):
        command.add_argument("--manifest", type=Path, default=HERE / "sources.json")
        command.add_argument("--cache-dir", type=Path, default=BUILD / "sources")
    fetch.add_argument("--download", action="store_true")
    fixture = commands.add_parser("fixture", help="write artificial queries and candidate features")
    synth = commands.add_parser("synthesize", help="reject M52 overlap and label shown candidates")
    synth.add_argument("--queries", type=Path, required=True)
    synth.add_argument("--candidates", type=Path, required=True)
    synth.add_argument("--m52-dir", type=Path, default=ROOT / "bench" / "data")
    train_cmd = commands.add_parser("train", help="train and verify both ONNX variants")
    repro = commands.add_parser("check-reproducibility", help="train twice and compare SHA256")
    for command in (train_cmd, repro):
        command.add_argument("--dataset-dir", type=Path, required=True)
    for command in (prepare_cmd, fixture, synth, train_cmd, repro):
        command.add_argument("--output-dir", type=Path, required=True)
    verify = commands.add_parser("verify", help="verify receipt hashes, tensor contract and CPU EP execution")
    verify.add_argument("--model-dir", type=Path, required=True)
    commands.add_parser("test", help="run the Python tests in the locked environment")
    commands.add_parser("typecheck", help="run mypy in the locked environment")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.command == "fetch":
            works, _ = load_manifest(args.manifest)
            if not works:
                raise TrainingError("source manifest has no approved corpus assets")
            for work in works:
                fetch_asset(work, args.cache_dir, offline=not args.download)
            print(f"verified corpus archives: {len(works)}")
        elif args.command == "prepare":
            publish(args.output_dir, lambda output: prepare(args.manifest, args.cache_dir, output))
        elif args.command == "fixture":
            publish(args.output_dir, make_fixture)
        elif args.command == "synthesize":
            publish(args.output_dir, lambda output: make_labels(args.queries, args.candidates, args.m52_dir, output))
        elif args.command == "train":
            from training_models import train

            publish(args.output_dir, lambda output: train(args.dataset_dir, output))
        elif args.command == "check-reproducibility":
            publish(args.output_dir, lambda output: check_reproducibility(args.dataset_dir, output))
        elif args.command == "verify":
            from training_models import verify

            print(json_bytes(verify(args.model_dir)).decode().strip())
        elif args.command == "test":
            import unittest

            tests = unittest.defaultTestLoader.discover(
                str(ROOT / "scripts" / "tests"), pattern="test_tiny_reranker.py",
            )
            return int(not unittest.TextTestRunner(verbosity=2).run(tests).wasSuccessful())
        elif args.command == "typecheck":
            from mypy import api

            stdout, stderr, status = api.run([
                "--ignore-missing-imports", "--cache-dir", str(BUILD / "mypy-cache"), str(HERE),
            ])
            print(stdout, end="")
            print(stderr, end="", file=sys.stderr)
            return status
    except (OSError, ValueError, ImportError, KeyError, zipfile.BadZipFile) as exc:
        print(f"tiny reranker: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
