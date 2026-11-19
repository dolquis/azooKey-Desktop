"""Release guard for the bundled dictionary payload (auto-word-registration-spec section 14.10).

Fails when the payload carries anything but the bundled static layers and lattice_core (in
particular a standalone neologd_lexicon pack), when it lacks lattice_core, when an artifact's
layer id differs from its file name, when ThirdPartyNotices.txt lacks a contributing source, or
when the MSI ships a different set of dictionaries than the payload holds.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
from pathlib import Path
import re
import struct
import sys
import xml.etree.ElementTree as ElementTree

import dictbuild
import build_bundled
import bundle_provenance

HERE = Path(__file__).resolve().parent

BUNDLED = {f"{layer}.azdic": index for index, layer in enumerate(dictbuild.LAYERS)
           if layer != "neologd_lexicon"}
# lattice_core holds sudachi_lexicon words, so its header names that layer (azdic spec 15.12).
BUNDLED[f"{build_bundled.LATTICE_CORE}.azdic"] = dictbuild.LAYERS.index(build_bundled.LAYER)
WIX = "{http://wixtoolset.org/schemas/v4/wxs}"


def read_summary(path: Path, image: bytes | None = None) -> tuple[int, set[bytes], dict]:
    """Returns the layer id, the section names and META without loading the whole artifact."""
    with (io.BytesIO(image) if image is not None else path.open("rb")) as stream:
        header = stream.read(64)
        if len(header) < 64 or header[:8] != b"AZDIC1\0\0":
            raise ValueError(f"{path.name}: not an .azdic v1 artifact")
        layer, section_count = struct.unpack_from("<I", header, 16)[0], struct.unpack_from("<I", header, 28)[0]
        if not 6 <= section_count <= 64:
            raise ValueError(f"{path.name}: invalid section table")
        table = stream.read(24 * section_count)
        if len(table) != 24 * section_count:
            raise ValueError(f"{path.name}: truncated section table")
        sections = [struct.unpack_from("<4sIQQ", table, 24 * i) for i in range(section_count)]
        for name, _, offset, length in sections:
            if name == b"META":
                stream.seek(offset)
                return layer, {section[0] for section in sections}, json.loads(stream.read(length))
    raise ValueError(f"{path.name}: META section missing")


def read_header(path: Path, image: bytes | None = None) -> tuple[int, list[str]]:
    """Returns the layer id and META source ids without loading the whole artifact."""
    layer, _, meta = read_summary(path, image)
    return layer, [source["source_id"] for source in meta["sources"]]


def lattice_errors(lattice: tuple[set[bytes], dict], sudachi: tuple[set[bytes], dict] | None) -> list[str]:
    """lattice_core must carry CONN, CMAT and META.lattice in the connection space of sudachi_lexicon,
    or the lattice (or the CONN of sudachi_lexicon) is silently left unused (azdic spec 15.5 (4))."""
    name = f"{build_bundled.LATTICE_CORE}.azdic"
    sections, meta = lattice
    errors = []
    if not {b"CONN", b"CMAT"} <= sections or not isinstance(meta.get("lattice"), dict):
        errors.append(f"{name}: CONN, CMAT or META.lattice is missing")
    space = meta.get("connection_space")
    if sudachi is not None and (not space or sudachi[1].get("connection_space") != space):
        errors.append(f"{name}: connection_space differs from {build_bundled.LAYER}.azdic")
    return errors


def check_vm_package(bundle: Path) -> dict:
    """Validate exactly the payload selected by the VM packager's allowlist."""
    payload = {relative: (bundle / relative).read_bytes() for relative in bundle_provenance.ARTIFACTS}
    artifact_hashes = {relative: hashlib.sha256(data).hexdigest() for relative, data in payload.items()}
    result = bundle_provenance.validate_receipt(
        bundle, HERE.parent, build_bundled.SUDACHI_REVISION, build_bundled.PINNED_ARCHIVES, artifact_hashes)
    notices = payload["ThirdPartyNotices.txt"].decode("utf-8")
    # lattice_core has no metadata JSON of its own: it carries the sources of sudachi_lexicon.
    checked = [(f"{layer}.azdic", layer) for layer in bundle_provenance.LAYERS]
    checked.append((f"{build_bundled.LATTICE_CORE}.azdic", build_bundled.LAYER))
    summaries = {}
    for name, metadata_layer in checked:
        expected_layer = BUNDLED[name]
        artifact = bundle / "dict" / name
        image = payload[f"dict/{name}"]
        dictbuild.verify(image)
        layer, sources = read_header(artifact, image)
        summaries[name] = read_summary(artifact, image)[1:]
        if layer != expected_layer:
            raise ValueError(f"{name}: layer id does not match the file name")
        count = struct.unpack_from("<I", image, 28)[0]
        sections = [struct.unpack_from("<4sIQQ", image, 64 + 24 * i) for i in range(count)]
        _, _, offset, length = next(section for section in sections if section[0] == b"META")
        metadata = json.loads(image[offset:offset + length])
        expected = json.loads((HERE / "sources" / f"{metadata_layer}.metadata.json").read_text(encoding="utf-8"))
        if metadata.get("sources") != expected["sources"]:
            raise ValueError(f"{name}: META sources differ from the current bundled metadata")
        attribution = dictbuild.validate_sources(metadata, HERE / "notices")
        if not sources or attribution not in notices:
            raise ValueError(f"{name}: source attribution is missing from ThirdPartyNotices.txt")
    errors = lattice_errors(summaries[f"{build_bundled.LATTICE_CORE}.azdic"],
                            summaries[f"{build_bundled.LAYER}.azdic"])
    if errors:
        raise ValueError(errors[0])
    return result


def packaged_dictionaries(package: Path) -> set[str]:
    names = set()
    for element in ElementTree.parse(package).iter(f"{WIX}File"):
        source = element.get("Source", "")
        if re.search(r"[\\/]dict[\\/][^\\/]+$", source) or "neologd" in source.lower():
            # The installed name is what the host discovers; Name overrides the source basename.
            names.add(element.get("Name") or re.split(r"[\\/]", source)[-1])
    return names


def check(bundle: Path, package: Path | None) -> list[str]:
    errors = []
    directory = bundle / "dict"
    found = sorted(p.name for p in directory.iterdir()) if directory.is_dir() else []
    if not found:
        errors.append(f"{directory}: no bundled dictionary")
    notices_path = bundle / "ThirdPartyNotices.txt"
    notices = notices_path.read_text(encoding="utf-8") if notices_path.is_file() else ""
    if not notices.strip():
        errors.append(f"{notices_path}: missing or empty")
    summaries = {}
    for name in found:
        if name not in BUNDLED:
            errors.append(f"{name}: not a bundled static layer (neologd_lexicon ships only as "
                          "a separate download pack)")
            continue
        try:
            layer, sections, meta = read_summary(directory / name)
            sources = [source["source_id"] for source in meta["sources"]]
        except (OSError, ValueError, KeyError, TypeError, struct.error) as exc:
            errors.append(str(exc))
            continue
        summaries[name] = sections, meta
        if layer != BUNDLED[name]:
            errors.append(f"{name}: layer id {layer} does not match the file name")
        errors.extend(f"{name}: source {source} is not attributed in ThirdPartyNotices.txt"
                      for source in sources if source not in notices)
    lattice_name = f"{build_bundled.LATTICE_CORE}.azdic"
    lattice = summaries.get(lattice_name)
    if lattice is not None:
        errors.extend(lattice_errors(lattice, summaries.get(f"{build_bundled.LAYER}.azdic")))
    elif found and lattice_name not in found:
        # Without it the Host runs without the lattice conversion (lattice-conversion-spec 3.6).
        errors.append(f"{lattice_name}: missing from the payload")
    if package is not None:
        shipped = packaged_dictionaries(package)
        if shipped != set(found):
            errors.append(f"{package.name} ships {sorted(shipped)} but the payload holds {found}")
    return errors


def fingerprint(path: Path) -> tuple[int, int, str]:
    """Compare both the .azdic content_hash and every byte, without loading large layers."""
    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as stream:
        header = stream.read(64)
        if len(header) < 64:
            raise ValueError(f"{path.name}: short header")
        digest.update(header)
        size += len(header)
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
            size += len(block)
    return struct.unpack_from("<Q", header, 32)[0], size, digest.hexdigest()


def compare(first: Path, second: Path) -> list[str]:
    errors = []
    names = [{p.name for p in (bundle / "dict").iterdir()}
             if (bundle / "dict").is_dir() else set() for bundle in (first, second)]
    if names[0] != names[1]:
        errors.append(f"reproducibility: dictionary names differ: {sorted(names[0])} vs {sorted(names[1])}")
    for name in sorted(names[0] & names[1]):
        try:
            left, right = fingerprint(first / "dict" / name), fingerprint(second / "dict" / name)
            if left[0] != right[0]:
                errors.append(f"{name}: content_hash differs between builds")
            if left[1:] != right[1:]:
                errors.append(f"{name}: bytes differ between builds")
        except (OSError, ValueError, struct.error) as exc:
            errors.append(str(exc))
    try:
        if (first / "ThirdPartyNotices.txt").read_bytes() != (second / "ThirdPartyNotices.txt").read_bytes():
            errors.append("ThirdPartyNotices.txt: bytes differ between builds")
    except OSError as exc:
        errors.append(str(exc))
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("bundle", type=Path, help="directory holding dict/ and ThirdPartyNotices.txt")
    parser.add_argument("--package", type=Path, help="pkg/msi/Package.wxs to compare against")
    parser.add_argument("--compare", type=Path, help="second build whose content_hash, bytes and notices must match")
    parser.add_argument("--vm-package-json", action="store_true",
                        help="validate the VM allowlist and generation receipt; emit JSON")
    args = parser.parse_args()
    if args.vm_package_json:
        if args.package or args.compare:
            parser.error("--vm-package-json cannot be combined with --package or --compare")
        try:
            result = check_vm_package(args.bundle)
        except (OSError, ValueError, KeyError, TypeError, AttributeError, IndexError, struct.error) as exc:
            print(f"check_bundle: {exc}", file=sys.stderr)
            return 1
        print(json.dumps(result, sort_keys=True))
        return 0
    errors = check(args.bundle, args.package)
    if args.compare:
        errors.extend(check(args.compare, args.package))
        errors.extend(compare(args.bundle, args.compare))
    for error in errors:
        print(f"check_bundle: {error}", file=sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
