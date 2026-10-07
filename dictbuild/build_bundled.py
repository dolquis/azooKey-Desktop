"""Builds the bundled static layers from SHA256-pinned upstreams and authored seeds."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
import urllib.request

import dictbuild
import extract_sudachi

HERE = Path(__file__).resolve().parent
SUDACHI_REVISION = "20260723"
SUDACHI_BASE_URL = "https://sudachi.s3.ap-northeast-1.amazonaws.com/sudachidict-raw"
# Raw lexicons of the SudachiDict release; core = small_lex + core_lex.
SUDACHI_ARCHIVES = (
    ("small_lex.zip", "b578ac9545899783d5d7e30d5d78d5d9dcf40b36965d4af0663ec2eb041c1093"),
    ("core_lex.zip", "a2b39e1572adab08a649b1390b134517adc55f1d733c59358b113298788bf31c"),
)
LAYER = "sudachi_lexicon"
TECHNICAL_LAYER = "technical_terms_lexicon"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def fetch(cache: Path, name: str, expected: str, offline: bool) -> Path:
    path = cache / SUDACHI_REVISION / name
    if path.is_file() and sha256(path) == expected:
        return path
    if offline:
        raise ValueError(f"{path} is missing or does not match the pinned SHA256")
    path.parent.mkdir(parents=True, exist_ok=True)
    partial = path.with_suffix(".part")
    url = f"{SUDACHI_BASE_URL}/{SUDACHI_REVISION}/{name}"
    print(f"build_bundled: downloading {url}", file=sys.stderr)
    with urllib.request.urlopen(url, timeout=600) as response, partial.open("wb") as out:
        while block := response.read(1 << 20):
            out.write(block)
    actual = sha256(partial)
    if actual != expected:
        partial.unlink()
        raise ValueError(f"{name}: SHA256 {actual} does not match the pinned {expected}")
    partial.replace(path)
    return path


def build_sudachi(cache: Path, offline: bool, work: Path) -> tuple[bytes, str]:
    archives = [fetch(cache, name, digest, offline) for name, digest in SUDACHI_ARCHIVES]
    tsv = work / f"{LAYER}.lex.tsv"
    extract_sudachi.write_tsv(archives, SUDACHI_REVISION, tsv)
    metadata = json.loads((HERE / "sources" / f"{LAYER}.metadata.json").read_text(encoding="utf-8"))
    return dictbuild.build([tsv], metadata, dictbuild.LAYERS.index(LAYER), HERE / "notices")


def build_technical_terms() -> tuple[bytes, str]:
    metadata = json.loads((HERE / "sources" / f"{TECHNICAL_LAYER}.metadata.json").read_text(encoding="utf-8"))
    return dictbuild.build([HERE / "sources" / f"{TECHNICAL_LAYER}.lex.tsv"], metadata,
                          dictbuild.LAYERS.index(TECHNICAL_LAYER), HERE / "notices")


def build_bundle(output: Path, cache: Path, offline: bool) -> None:
    dictionaries = output / "dict"
    dictionaries.mkdir(parents=True, exist_ok=True)
    notices = ["Notices for the bundled dictionaries\n"]
    with tempfile.TemporaryDirectory() as work:
        builders = (
            (LAYER, lambda: build_sudachi(cache, offline, Path(work)),
             f"Derived from the SudachiDict {SUDACHI_REVISION} raw lexicon by "
             f"dictbuild/extract_sudachi.py (entries filtered by part of speech, reading and cost "
             f"<= {extract_sudachi.DEFAULT_MAX_COST}; readings converted to hiragana) and "
             "dictbuild/dictbuild.py. It is not the upstream distribution."),
            (TECHNICAL_LAYER, build_technical_terms,
             "Built from the project-authored Apache-2.0 seed in "
             "dictbuild/sources/technical_terms_lexicon.lex.tsv by dictbuild/dictbuild.py."),
        )
        for layer, builder, description in builders:
            image, layer_notices = builder()
            artifact = dictionaries / f"{layer}.azdic"
            artifact.write_bytes(image)
            dictbuild.verify(artifact.read_bytes())
            notices.append(f"{artifact.name}\n{description}\n\n{layer_notices}")
            print(f"build_bundled: wrote {artifact} ({len(image)} bytes)", file=sys.stderr)
    (output / "ThirdPartyNotices.txt").write_text("\n".join(notices), encoding="utf-8", newline="\n")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path,
                        help="receives dict/<layer>.azdic and ThirdPartyNotices.txt")
    parser.add_argument("--cache", required=True, type=Path, help="download cache for upstream archives")
    parser.add_argument("--offline", action="store_true", help="use only archives already in --cache")
    args = parser.parse_args()
    try:
        build_bundle(args.output, args.cache, args.offline)
    except (OSError, ValueError, KeyError, TypeError, struct.error) as exc:
        print(f"build_bundled: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
