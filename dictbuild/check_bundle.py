"""Release guard for the bundled dictionary payload (auto-word-registration-spec section 14.10).

Fails when the payload carries anything but the bundled static layers (in particular a
standalone neologd_lexicon pack), when an artifact's layer id differs from its file name,
when ThirdPartyNotices.txt lacks a contributing source, or when the MSI ships a different
set of dictionaries than the payload holds.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import sys
import xml.etree.ElementTree as ElementTree

import dictbuild

BUNDLED = {f"{layer}.azdic": index for index, layer in enumerate(dictbuild.LAYERS)
           if layer != "neologd_lexicon"}
WIX = "{http://wixtoolset.org/schemas/v4/wxs}"


def read_header(path: Path) -> tuple[int, list[str]]:
    """Returns the layer id and META source ids without loading the whole artifact."""
    with path.open("rb") as stream:
        header = stream.read(64)
        if len(header) < 64 or header[:8] != b"AZDIC1\0\0":
            raise ValueError(f"{path.name}: not an .azdic v1 artifact")
        layer, section_count = struct.unpack_from("<I", header, 16)[0], struct.unpack_from("<I", header, 28)[0]
        if not 6 <= section_count <= 64:
            raise ValueError(f"{path.name}: invalid section table")
        table = stream.read(24 * section_count)
        for i in range(section_count):
            name, _, offset, length = struct.unpack_from("<4sIQQ", table, 24 * i)
            if name == b"META":
                stream.seek(offset)
                meta = json.loads(stream.read(length))
                return layer, [source["source_id"] for source in meta["sources"]]
    raise ValueError(f"{path.name}: META section missing")


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
    for name in found:
        if name not in BUNDLED:
            errors.append(f"{name}: not a bundled static layer (neologd_lexicon ships only as "
                          "a separate download pack)")
            continue
        try:
            layer, sources = read_header(directory / name)
        except (OSError, ValueError, KeyError, TypeError, struct.error) as exc:
            errors.append(str(exc))
            continue
        if layer != BUNDLED[name]:
            errors.append(f"{name}: layer id {layer} does not match the file name")
        errors.extend(f"{name}: source {source} is not attributed in ThirdPartyNotices.txt"
                      for source in sources if source not in notices)
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
    args = parser.parse_args()
    errors = check(args.bundle, args.package)
    if args.compare:
        errors.extend(check(args.compare, args.package))
        errors.extend(compare(args.bundle, args.compare))
    for error in errors:
        print(f"check_bundle: {error}", file=sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
