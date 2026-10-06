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
    rows += [f"high{i}\tよ{i}\t名詞\t0\t1.0\ttechnical\tfixture" for i in range(8)]
    tsv.write_text("# SPDX-License-Identifier: MIT; THIRD_PARTY_LICENSES; synthetic fixture\n"
                   "surface\treading\tpos\tcost\tfrequency\tcategory\tsource_id\n" +
                   "\n".join(rows) + "\n", encoding="utf-8", newline="\n")
    image, notices = dictbuild.build([tsv], metadata, 4, directory)
    repeated, _ = dictbuild.build([tsv], metadata, 4, directory)
    assert image == repeated
    (directory / "valid.azdic").write_bytes(image)
    bundled = directory / "bundled"
    bundled.mkdir(exist_ok=True)
    (bundled / "base_lexicon.azdic").write_bytes(image)  # Deliberately wrong layer.
    (bundled / "technical_terms_lexicon.azdic").write_bytes(image)
    (directory / "ThirdPartyNotices.txt").write_text(notices, encoding="utf-8")
    sections = {}
    for i in range(struct.unpack_from("<I", image, 28)[0]):
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
    # Swap terminal ids and their references together: only global key order is invalid.
    for name, other in (("key_order_same_depth", 2), ("key_order_other_depth", 1)):
        broken = bytearray(image)
        trie_off, trie_size = sections[b"TRIE"]
        for off in range(trie_off, trie_off + trie_size, 16):
            key_id = struct.unpack_from("<I", broken, off + 8)[0]
            if key_id in (0, other):
                struct.pack_into("<I", broken, off + 8, other if key_id == 0 else 0)
        key_off = sections[b"KEYS"][0]
        a, b = key_off, key_off + other * 8
        broken[a:a + 8], broken[b:b + 8] = broken[b:b + 8], broken[a:a + 8]
        struct.pack_into("<Q", broken, 32, dictbuild.fnv(broken[64:]))
        (directory / f"{name}.azdic").write_bytes(broken)
    (directory / "truncated.azdic").write_bytes(image[:63])
    write_surface_index_fixtures(directory, image, sections)
    (directory / "ready").write_text("ready", encoding="utf-8")


def write_surface_index_fixtures(directory: Path, image: bytes, sections: dict) -> None:
    """Writes SIDX corruptions and the two compatibility shapes around it."""
    entry_count = struct.unpack_from("<I", image, 20)[0]
    key_count = struct.unpack_from("<I", image, 24)[0]
    ents_off, _ = sections[b"ENTS"]
    strs_off, _ = sections[b"STRS"]
    sidx_off, sidx_size = sections[b"SIDX"]

    def surface(index: int) -> str:
        off, length = struct.unpack_from("<II", image, ents_off + index * 32)
        return image[strs_off + off:strs_off + off + length].decode("utf-8")

    def section_bytes(name: bytes) -> bytes:
        off, size = sections[name]
        return image[off:off + size]

    def with_index(order: list[int]) -> bytes:
        broken = bytearray(image)
        struct.pack_into(f"<{len(order)}I", broken, sidx_off, *order)
        struct.pack_into("<Q", broken, 32, dictbuild.fnv(broken[64:]))
        return bytes(broken)

    tokyo = next(i for i in range(entry_count) if surface(i) == "東京")
    tokyo_to = next(i for i in range(entry_count) if surface(i) == "東京都")
    order = list(struct.unpack_from(f"<{entry_count}I", image, sidx_off))
    # Artifacts built before SIDX existed, and ones carrying a section this reader does not know.
    names = [name for name, _ in sorted(sections.items(), key=lambda item: item[1][0])]
    layer = struct.unpack_from("<I", image, 16)[0]
    (directory / "no_surface_index.azdic").write_bytes(dictbuild.pack(
        [(n, section_bytes(n)) for n in names if n != b"SIDX"], layer, entry_count, key_count))
    (directory / "unknown_section.azdic").write_bytes(dictbuild.pack(
        [(n, section_bytes(n)) for n in names] + [(b"ZZZZ", b"future")], layer, entry_count, key_count))
    # Structure: SIDX must hold exactly one u32 per ENTS record.
    broken = bytearray(image)
    table = next(64 + i * 24 for i in range(len(sections))
                 if image[64 + i * 24:68 + i * 24] == b"SIDX")
    struct.pack_into("<Q", broken, table + 16, sidx_size - 4)
    struct.pack_into("<Q", broken, 32, dictbuild.fnv(broken[64:]))
    (directory / "surface_index_size.azdic").write_bytes(broken)
    # References are checked when read, so every probe of these lookups hits the corruption.
    (directory / "surface_index_entry.azdic").write_bytes(with_index([0xFFFFFFFF] * entry_count))
    (directory / "surface_index_duplicate.azdic").write_bytes(with_index([tokyo] * entry_count))
    # Searching 東京 never probes slot 0, so the range [0, count) holds 東京都 there.
    (directory / "surface_index_mismatch.azdic").write_bytes(
        with_index([tokyo_to] + [tokyo] * (entry_count - 1)))
    # Only the global order is wrong; lookups of other surfaces may still succeed.
    swapped = order[:]
    swapped[0], swapped[1] = swapped[1], swapped[0]
    (directory / "surface_index_order.azdic").write_bytes(with_index(swapped))


if __name__ == "__main__":
    generate(Path(sys.argv[1]))
