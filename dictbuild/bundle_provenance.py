"""Bind bundled dictionary artifacts to their exact, repository-relative inputs."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re

RECEIPT = "bundle-provenance.json"
LAYERS = ("sudachi_lexicon", "technical_terms_lexicon")
ARTIFACTS = tuple(f"dict/{layer}.azdic" for layer in LAYERS) + ("ThirdPartyNotices.txt",)
CODE_INPUTS = ("build_bundled.py", "dictbuild.py", "extract_sudachi.py", "bundle_provenance.py")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def canonical(value: object) -> bytes:
    return (json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")) + "\n").encode("utf-8")


def generation_inputs(repository: Path) -> dict[str, str]:
    """Use only known inputs and validated notice identifiers, never receipt paths."""
    paths = {f"dictbuild/{name}" for name in CODE_INPUTS}
    paths.add("dictbuild/sources/technical_terms_lexicon.lex.tsv")
    for layer in LAYERS:
        relative = f"dictbuild/sources/{layer}.metadata.json"
        paths.add(relative)
        metadata = json.loads((repository / relative).read_text(encoding="utf-8"))
        for source in metadata["sources"]:
            for notice_id in [source["spdx"], *source["notice_ids"]]:
                if not isinstance(notice_id, str) or not re.fullmatch(r"[A-Za-z0-9_.-]+", notice_id) or notice_id in (".", ".."):
                    raise ValueError("invalid notice id")
                paths.add(f"dictbuild/notices/{notice_id}.txt")
    return {relative: sha256(repository / relative) for relative in sorted(paths)}


def input_fingerprint(inputs: dict[str, str]) -> str:
    return hashlib.sha256(canonical(inputs)).hexdigest()


def make_receipt(bundle: Path, inputs: dict[str, str], revision: str,
                 archives: tuple[tuple[str, str], ...], artifact_hashes: dict[str, str] | None = None) -> dict:
    return {"schemaVersion": 1, "sudachi": {"revision": revision, "archives": dict(archives)},
            "inputs": inputs, "inputFingerprint": input_fingerprint(inputs),
            "artifacts": artifact_hashes if artifact_hashes is not None else
                         {relative: sha256(bundle / relative) for relative in ARTIFACTS}}


def write_receipt(bundle: Path, inputs: dict[str, str], revision: str,
                  archives: tuple[tuple[str, str], ...]) -> None:
    receipt = make_receipt(bundle, inputs, revision, archives)
    temporary = bundle / (RECEIPT + ".tmp")
    try:
        temporary.write_bytes(canonical(receipt))
        temporary.replace(bundle / RECEIPT)
    finally:
        temporary.unlink(missing_ok=True)


def _unique_object(pairs: list[tuple[str, object]]) -> dict:
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate provenance field")
        result[key] = value
    return result


def validate_receipt(bundle: Path, repository: Path, revision: str,
                     archives: tuple[tuple[str, str], ...], artifact_hashes: dict[str, str] | None = None) -> dict:
    path = bundle / RECEIPT
    if not path.is_file():
        raise ValueError(f"{RECEIPT}: missing generation receipt; rebuild bundled dictionaries")
    receipt_bytes = path.read_bytes()
    receipt = json.loads(receipt_bytes, object_pairs_hook=_unique_object)
    inputs = generation_inputs(repository)
    expected = make_receipt(bundle, inputs, revision, archives, artifact_hashes)
    if not isinstance(receipt, dict) or set(receipt) != set(expected) or type(receipt.get("schemaVersion")) is not int:
        raise ValueError(f"{RECEIPT}: invalid schema")
    for field in expected:
        if receipt[field] != expected[field]:
            raise ValueError(f"{RECEIPT}: {field} does not match current inputs or artifacts; rebuild bundled dictionaries")
    return {"schemaVersion": 1, "sudachiRevision": revision,
            "inputFingerprint": expected["inputFingerprint"],
            "provenanceSha256": hashlib.sha256(receipt_bytes).hexdigest()}
