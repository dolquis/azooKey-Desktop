"""Builds and verifies the opt-in neologd_lexicon download pack (auto-word-registration-spec 15.14).

The pack is a regular .azdic v1 artifact for layer 2 plus a manifest that carries its size,
SHA256, download URL and the attribution the download screen presents before fetching it.
The app pins a manifest (dictbuild/packs/); empty url and sha256 mean no pack is published.
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
from pathlib import Path
import re
import struct
import sys

import dictbuild

HERE = Path(__file__).resolve().parent
PACK_ID = "neologd_lexicon"
LAYER_ID = dictbuild.LAYERS.index(PACK_ID)
MANIFEST_NAME = f"{PACK_ID}.manifest.json"
MANIFEST_VERSION = 1
METADATA = HERE / "sources" / f"{PACK_ID}.metadata.json"
PINNED = HERE / "packs" / MANIFEST_NAME
CATALOG = HERE / "notices"
REVISION = re.compile(r"[0-9A-Za-z][0-9A-Za-z._-]{0,63}")
SHA256 = re.compile(r"[0-9a-f]{64}")
FIELDS = {"manifest_version", "pack_id", "layer_id", "format", "format_version", "builder_version",
          "upstream_revision", "file_name", "size", "sha256", "url", "attribution"}


def pack_file_name(revision: str) -> str:
    # A versioned name lets the host fetch a new pack while the old one is still mapped.
    if not REVISION.fullmatch(revision):
        raise ValueError(f"invalid revision: {revision!r}")
    return f"{PACK_ID}-{revision}.azdic"


def with_revision(metadata: dict, revision: str) -> dict:
    pinned = copy.deepcopy(metadata)
    for source in pinned["sources"]:
        source["upstream_revision"] = revision
    return pinned


def attribution(metadata: dict, catalog: Path) -> dict:
    """The download screen shows notices; sources mirror the pack's META."""
    return {"sources": metadata["sources"], "notices": dictbuild.validate_sources(metadata, catalog)}


def make_manifest(metadata: dict, catalog: Path, revision: str = "", size: int = 0,
                  sha256: str = "", url: str = "") -> dict:
    return {
        "manifest_version": MANIFEST_VERSION,
        "pack_id": PACK_ID,
        "layer_id": LAYER_ID,
        "format": "azdic",
        "format_version": 1,
        "builder_version": "azdic-1",
        "upstream_revision": revision,
        "file_name": pack_file_name(revision) if revision else "",
        "size": size,
        "sha256": sha256,
        "url": url,
        "attribution": attribution(metadata, catalog),
    }


def validate_manifest(manifest: dict) -> bool:
    """Returns True when the manifest names a published pack, False when none is published."""
    if not isinstance(manifest, dict) or set(manifest) != FIELDS:
        raise ValueError("manifest fields do not match version 1")
    expected = {"manifest_version": MANIFEST_VERSION, "pack_id": PACK_ID, "layer_id": LAYER_ID,
                "format": "azdic", "format_version": 1, "builder_version": "azdic-1"}
    for field, value in expected.items():
        if manifest[field] != value or type(manifest[field]) is not type(value):
            raise ValueError(f"unsupported manifest {field}")
    for field in ("upstream_revision", "file_name", "sha256", "url"):
        if not isinstance(manifest[field], str):
            raise ValueError(f"manifest {field} must be a string")
    if type(manifest["size"]) is not int or manifest["size"] < 0:
        raise ValueError("manifest size must be a non-negative integer")
    sources = manifest["attribution"].get("sources") if isinstance(manifest["attribution"], dict) else None
    if not isinstance(sources, list) or not sources or not isinstance(manifest["attribution"].get("notices"), str):
        raise ValueError("manifest attribution is missing")
    if not (manifest["url"] or manifest["sha256"]):
        if manifest["upstream_revision"] or manifest["file_name"] or manifest["size"]:
            raise ValueError("unpublished manifest must not name a pack")
        return False
    if not manifest["url"].startswith("https://"):
        raise ValueError("pack url must be https")
    if not SHA256.fullmatch(manifest["sha256"]):
        raise ValueError("pack sha256 must be 64 lowercase hex digits")
    if not manifest["size"] or manifest["file_name"] != pack_file_name(manifest["upstream_revision"]):
        raise ValueError("published manifest must name a nonempty, versioned pack")
    return True


def verify_pack(manifest: dict, path: Path) -> None:
    """Checks a downloaded pack in the order the host does before loading it."""
    if not validate_manifest(manifest):
        raise ValueError("no pack is published")
    image = path.read_bytes()
    if len(image) != manifest["size"]:
        raise ValueError("pack size mismatch")
    if hashlib.sha256(image).hexdigest() != manifest["sha256"]:
        raise ValueError("pack sha256 mismatch")
    dictbuild.verify(image)
    if struct.unpack_from("<I", image, 16)[0] != LAYER_ID:
        raise ValueError("pack layer id mismatch")
    meta = read_meta(image)
    if meta["sources"] != manifest["attribution"]["sources"]:
        raise ValueError("pack attribution mismatch")
    if meta["builder_version"] != manifest["builder_version"]:
        raise ValueError("pack builder version mismatch")


def read_meta(image: bytes) -> dict:
    for i in range(struct.unpack_from("<I", image, 28)[0]):
        name, _, offset, length = struct.unpack_from("<4sIQQ", image, 64 + i * 24)
        if name == b"META":
            return json.loads(image[offset:offset + length])
    raise ValueError("META section missing")


def build_pack(inputs: list[Path], metadata: dict, revision: str, catalog: Path,
               output: Path, url: str) -> dict:
    """Writes the versioned pack and its manifest into output; returns the manifest."""
    metadata = with_revision(metadata, revision)
    image, _ = dictbuild.build(inputs, metadata, LAYER_ID, catalog)
    manifest = make_manifest(metadata, catalog, revision, len(image),
                             hashlib.sha256(image).hexdigest(), url)
    validate_manifest(manifest)
    output.mkdir(parents=True, exist_ok=True)
    (output / pack_file_name(revision)).write_bytes(image)
    write_manifest(manifest, output / MANIFEST_NAME)
    verify_pack(manifest, output / pack_file_name(revision))
    return manifest


def write_manifest(manifest: dict, path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
                    encoding="utf-8", newline="\n")


def check_pinned(manifest: dict, metadata: dict, catalog: Path) -> None:
    """The pinned attribution must be what the metadata and catalog generate today."""
    published = validate_manifest(manifest)
    source = with_revision(metadata, manifest["upstream_revision"]) if published else metadata
    if manifest["attribution"] != attribution(source, catalog):
        raise ValueError(f"{MANIFEST_NAME}: attribution drifted from {METADATA.name}; "
                         "regenerate with neologd_pack.py pin")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest="command", required=True)
    build = commands.add_parser("build", help="build the pack and its manifest from intermediate TSVs")
    build.add_argument("inputs", nargs="+", type=Path)
    build.add_argument("--revision", required=True, help="upstream seed release the TSVs came from")
    build.add_argument("--output", required=True, type=Path, help="directory for the pack and manifest")
    build.add_argument("--url", required=True, help="https URL the pack will be published at")
    verify = commands.add_parser("verify", help="verify a pack against its manifest")
    verify.add_argument("manifest", type=Path)
    verify.add_argument("pack", type=Path)
    pin = commands.add_parser("pin", help="write the pinned manifest (unpublished unless --from)")
    pin.add_argument("--from", dest="source", type=Path, help="manifest of a published pack")
    commands.add_parser("check-pinned", help="fail when the pinned manifest drifted")
    args = parser.parse_args()
    try:
        metadata = json.loads(METADATA.read_text(encoding="utf-8"))
        if args.command == "build":
            build_pack(args.inputs, metadata, args.revision, CATALOG, args.output, args.url)
        elif args.command == "verify":
            verify_pack(json.loads(args.manifest.read_text(encoding="utf-8")), args.pack)
        elif args.command == "pin":
            manifest = (json.loads(args.source.read_text(encoding="utf-8")) if args.source
                        else make_manifest(metadata, CATALOG))
            check_pinned(manifest, metadata, CATALOG)
            write_manifest(manifest, PINNED)
        else:
            check_pinned(json.loads(PINNED.read_text(encoding="utf-8")), metadata, CATALOG)
    except (OSError, ValueError, KeyError, TypeError, struct.error) as exc:
        print(f"neologd_pack: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
