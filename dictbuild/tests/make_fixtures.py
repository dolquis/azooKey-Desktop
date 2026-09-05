"""Synthetic, authored test data only; never copies installed/user dictionaries."""
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import dictbuild


def generate(directory: Path) -> None:
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "MIT.txt").write_text("Synthetic test license text. Not a release asset.\n", encoding="utf-8")
    source = dict(source_id="fixture", spdx="MIT", upstream_url="https://example.invalid/fixture",
                  upstream_revision="1", transform="synthetic", copyright="Test fixture", notice_ids=[])
    metadata = {"sources": [source]}
    tsv = directory / "fixture.lex.tsv"
    rows = [
        "都\tとう\t名詞\t5000\t0.3\tgeneral\tfixture",
        "東京\tとうきょう\t名詞\t4000\t0.4\tplace_name\tfixture",
        "東京都\tとうきょうと\t名詞\t3000\t0.5\tplace_name\tfixture",
        "場\tば\t名詞\t5000\t0.3\tgeneral\tfixture",
        "ヴァ\tヴァ\t名詞\t5000\t0.3\tgeneral\tfixture",
        "TensorRT\tてんそるあーるてぃー\t名詞\t4200\t0.72\ttechnical\tfixture",
        "短\taa\t名詞\t5000\t0.3\tgeneral\tfixture",
        "長\taaaaa\t名詞\t5000\t0.3\tgeneral\tfixture",
        "中\tab\t名詞\t5000\t0.3\tgeneral\tfixture",
        "緩\tかと\t名詞\t5000\t0.3\tgeneral\tfixture",
    ]
    # Many prefixes and Unicode keys allow an independent map-based reference.
    rows += [f"word{i}\tき{i:04d}\t名詞\t5000\t0.3\tgeneral\tfixture" for i in range(200)]
    tsv.write_text("# SPDX-License-Identifier: MIT; THIRD_PARTY_LICENSES; synthetic fixture\n"
                   "surface\treading\tpos\tcost\tfrequency\tcategory\tsource_id\n" +
                   "\n".join(rows) + "\n", encoding="utf-8", newline="\n")
    image, notices = dictbuild.build([tsv], metadata, 4, directory)
    repeated, _ = dictbuild.build([tsv], metadata, 4, directory)
    assert image == repeated
    (directory / "valid.azdic").write_bytes(image)
    (directory / "ThirdPartyNotices.txt").write_text(notices, encoding="utf-8")
    sections = {}
    for i in range(6):
        name, _, off, size = struct.unpack_from("<4sIQQ", image, 64 + i * 24)
        sections[name] = off, size
    mutations = {
        "magic": (0, b"B"), "version": (8, b"\x02"), "flags": (12, b"\x80"),
        "duplicate": (64 + 24, b"TRIE"), "unaligned": (64 + 8, struct.pack("<Q", 209)),
        "overflow": (64 + 16, struct.pack("<Q", 0xFFFFFFFFFFFFFFFF)),
        "hash": (32, b"\x00" * 8),
        "entry": (sections[b"EIDX"][0], struct.pack("<I", 0xFFFFFFFF)),
        "kind": (sections[b"EIDX"][0] + 4, b"\x03"),
        "string": (sections[b"ENTS"][0], struct.pack("<I", 0xFFFFFFFF)),
        "pos": (sections[b"ENTS"][0] + 16, b"\xff\xff"),
        "source": (sections[b"ENTS"][0] + 24, b"\x07"),
    }
    for name, (offset, replacement) in mutations.items():
        broken = bytearray(image)
        broken[offset:offset + len(replacement)] = replacement
        if name != "hash":
            struct.pack_into("<Q", broken, 32, dictbuild.fnv(broken[64:]))
        (directory / f"{name}.azdic").write_bytes(broken)
    (directory / "truncated.azdic").write_bytes(image[:63])
    (directory / "ready").write_text("ready", encoding="utf-8")


if __name__ == "__main__":
    generate(Path(sys.argv[1]))
