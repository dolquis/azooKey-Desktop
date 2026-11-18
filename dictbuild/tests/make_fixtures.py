"""Synthetic, authored test data only; never copies installed/user dictionaries."""
import hashlib
import json
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import dictbuild
import neologd_pack


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
        # One surface, two readings: reverse lookup returns both in ENTS order.
        "日本\tにほん\t名詞\t4000\t0.4\tplace_name\tfixture",
        "日本\tにっぽん\t名詞\t4500\t0.35\tplace_name\tfixture",
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
    # Same reading, score-ordered dictionary candidates for app-profile tests.
    # The ordinary spelling precedes the ASCII technical term before any boost.
    profile_tsv = directory / "profile.lex.tsv"
    profile_tsv.write_text(
        "# SPDX-License-Identifier: MIT; THIRD_PARTY_LICENSES; synthetic fixture\n"
        "surface\treading\tpos\tcost\tfrequency\tcategory\tsource_id\n"
        "テンソルRT\tてんそるあーるてぃー\t名詞\t1000\t0.99\tgeneral\tfixture\n"
        "TensorRT\tてんそるあーるてぃー\t名詞\t4200\t0.72\ttechnical\tfixture\n",
        encoding="utf-8", newline="\n")
    profile_image, _ = dictbuild.build([profile_tsv], metadata, 4, directory)
    profile_directory = directory / "profile"
    profile_directory.mkdir(exist_ok=True)
    (profile_directory / "technical_terms.azdic").write_bytes(profile_image)
    # DEV-1734: words that tell the readings of an ambiguous "nn" + vowel apart.
    ambiguous_tsv = directory / "ambiguous.lex.tsv"
    ambiguous_tsv.write_text(
        "# SPDX-License-Identifier: MIT; THIRD_PARTY_LICENSES; synthetic fixture\n"
        "surface\treading\tpos\tcost\tfrequency\tcategory\tsource_id\n"
        "内部\tないぶ\t名詞\t2000\t0.6\tgeneral\tfixture\n"
        "原因\tげんいん\t名詞\t1000\t0.7\tgeneral\tfixture\n"
        "現任\tげんにん\t名詞\t6000\t0.2\tgeneral\tfixture\n"
        "こんにちは\tこんにちは\t感動詞\t0\t0.8\tgeneral\tfixture\n"
        "懇意\tこんい\t名詞\t5000\t0.3\tgeneral\tfixture\n"
        "千年\tせんねん\t名詞\t4000\t0.4\tgeneral\tfixture\n"
        "遷延\tせんえん\t名詞\t6000\t0.2\tgeneral\tfixture\n",
        encoding="utf-8", newline="\n")
    ambiguous_image, _ = dictbuild.build([ambiguous_tsv], metadata, 1, directory)
    ambiguous_directory = directory / "ambiguous"
    ambiguous_directory.mkdir(exist_ok=True)
    (ambiguous_directory / "sudachi_lexicon.azdic").write_bytes(ambiguous_image)
    bundled = directory / "bundled"
    bundled.mkdir(exist_ok=True)
    (bundled / "base_lexicon.azdic").write_bytes(image)  # Deliberately wrong layer.
    (bundled / "technical_terms_lexicon.azdic").write_bytes(image)
    settings_layers = directory / "settings-layers"
    settings_layers.mkdir(exist_ok=True)
    for layer in (1, 3, 4):
        layer_image, _ = dictbuild.build([tsv], metadata, layer, directory)
        (settings_layers / f"{dictbuild.LAYERS[layer]}.azdic").write_bytes(layer_image)
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
    write_neologd_pack_fixtures(directory, tsv, metadata, image, directory / "hash.azdic")
    write_connection_fixtures(directory, tsv, metadata)
    write_lattice_fixtures(directory, metadata)
    (directory / "ready").write_text("ready", encoding="utf-8")


def write_connection_fixtures(directory: Path, tsv: Path, metadata: dict) -> None:
    """connections.azdic: valid.azdic's words plus CONN. lattice_core.azdic: kana words, CONN, CMAT.

    Ids, costs and the matrix are synthetic; the matrix is asymmetric so a reader that swaps the
    previous-right and next-left axes reads other costs.
    """
    header = "# SPDX-License-Identifier: MIT; THIRD_PARTY_LICENSES; synthetic fixture\n"
    connections = directory / "fixture.conn.tsv"
    rows = [
        # reading, surface, left, right, cost, pos, conjugation, normalized form, six POS fields
        ("とうきょう", "東京", 10, 10, 4000, "名詞-固有名詞-地名-一般", "", "東京", "名詞,固有名詞,地名,一般,*,*"),
        ("とうきょう", "東京", 11, 12, 3500, "名詞-固有名詞-地名-一般", "", "東京", "名詞,固有名詞,地名,一般,*,*"),
        ("とう", "都", 20, 20, 5000, "名詞-普通名詞-一般", "", "都", "名詞,普通名詞,一般,*,*,*"),
        ("にほん", "日本", 30, 30, 4000, "名詞-固有名詞-地名-一般", "", "日本", "名詞,固有名詞,地名,一般,*,*"),
        ("にっぽん", "日本", 30, 30, 4500, "名詞-固有名詞-地名-一般", "", "日本", "名詞,固有名詞,地名,一般,*,*"),
        # Spelled in the hiragana of the reading: lattice_core only.
        ("は", "は", 40, 40, 3846, "助詞-係助詞", "", "は", "助詞,係助詞,*,*,*,*"),
        ("です", "です", 50, 51, 2000, "助動詞", "助動詞-デス-終止形-一般", "です",
         "助動詞,*,*,*,助動詞-デス,終止形-一般"),
    ]
    connections.write_text(header + "\t".join(dictbuild.CONNECTION_COLUMNS) + "\n" + "".join(
        "\t".join(map(str, row)) + "\n" for row in rows), encoding="utf-8", newline="\n")
    kana = directory / "lattice_core.lex.tsv"
    kana.write_text(header + "surface\treading\tpos\tcost\tfrequency\tcategory\tsource_id\n"
                    "は\tは\t助詞-係助詞\t3846\t\tgeneral\tfixture\n"
                    "です\tです\t助動詞\t2000\t\tgeneral\tfixture\n", encoding="utf-8", newline="\n")
    lefts = sorted({0, *(row[2] for row in rows)})
    rights = sorted({0, *(row[3] for row in rows)})
    matrix = directory / "lattice_matrix.def"
    matrix.write_text("64 64\n" + "".join(f"{r} {l} {(r * 7 + l * 3) % 200 - 100}\n"
                                          for r in rights for l in lefts), encoding="utf-8", newline="\n")
    image, _ = dictbuild.build([tsv], metadata, 4, directory, connections, dictbuild.connection_space(matrix))
    (directory / "connections.azdic").write_bytes(image)
    lattice, _ = dictbuild.build_lattice_core([kana], connections, matrix, metadata, 1, directory)
    (directory / "lattice_core.azdic").write_bytes(lattice)


def write_lattice_fixtures(directory: Path, metadata: dict) -> None:
    """lattice/: a sentence vocabulary for the core lattice tests (DEV-2098) and broken copies.

    The matrix costs 1000 everywhere except along 今日|は|いい|天気|です, which costs 0, so that
    path is the best one. Each broken copy keeps the trie sound (content hash included) and breaks
    one check (4) rule of azdic spec section 15.5 or one META.lattice key.
    """
    out = directory / "lattice"
    out.mkdir(exist_ok=True)
    header = "# SPDX-License-Identifier: MIT; THIRD_PARTY_LICENSES; synthetic fixture\n"
    lexicon_columns = "surface\treading\tpos\tcost\tfrequency\tcategory\tsource_id\n"
    rows = [
        # reading, surface, left, right, cost, pos, conjugation, normalized form, six POS fields
        ("きょう", "今日", 10, 10, 3000, "名詞-普通名詞-副詞可能", "", "今日", "名詞,普通名詞,副詞可能,*,*,*"),
        ("きょう", "京", 20, 20, 6000, "名詞-固有名詞-地名-一般", "", "京", "名詞,固有名詞,地名,一般,*,*"),
        ("てんき", "天気", 30, 30, 2500, "名詞-普通名詞-一般", "", "天気", "名詞,普通名詞,一般,*,*,*"),
        ("てんき", "転機", 30, 30, 5000, "名詞-普通名詞-一般", "", "転機", "名詞,普通名詞,一般,*,*,*"),
        # Katakana spelling of 天気 (attr bits 14 and 15), and a loanword spelled so (bit 14 only).
        ("てんき", "テンキ", 30, 30, 2500, "名詞-普通名詞-一般", "", "天気", "名詞,普通名詞,一般,*,*,*"),
        ("こーひー", "コーヒー", 30, 30, 3000, "名詞-普通名詞-一般", "", "コーヒー", "名詞,普通名詞,一般,*,*,*"),
        # Spelled in the hiragana of the reading: lattice_core only.
        ("は", "は", 50, 50, 1000, "助詞-係助詞", "", "は", "助詞,係助詞,*,*,*,*"),
        ("です", "です", 60, 60, 1000, "助動詞", "助動詞-デス-終止形-一般", "です",
         "助動詞,*,*,*,助動詞-デス,終止形-一般"),
        ("いい", "いい", 40, 40, 2000, "形容詞-非自立可能", "形容詞-終止形-一般", "良い",
         "形容詞,非自立可能,*,*,形容詞,終止形-一般"),
        ("お", "お", 70, 70, 1500, "接頭辞", "", "御", "接頭辞,*,*,*,*,*"),
    ]
    connections = out / "sentence.conn.tsv"
    connections.write_text(header + "\t".join(dictbuild.CONNECTION_COLUMNS) + "\n" + "".join(
        "\t".join(map(str, row)) + "\n" for row in rows), encoding="utf-8", newline="\n")
    words = out / "sentence.lex.tsv"
    words.write_text(header + lexicon_columns + "".join(
        f"{surface}\t{reading}\t{pos}\t{cost}\t\tgeneral\tfixture\n"
        for reading, surface, _, _, cost, pos, *_ in rows if surface != reading),
        encoding="utf-8", newline="\n")
    kana = out / "lattice_core.lex.tsv"
    kana.write_text(header + lexicon_columns + "".join(
        f"{surface}\t{reading}\t{pos}\t{cost}\t\tgeneral\tfixture\n"
        for reading, surface, _, _, cost, pos, *_ in rows if surface == reading),
        encoding="utf-8", newline="\n")
    ids = sorted({0, *(row[2] for row in rows)})
    sentence = {(0, 10), (10, 50), (50, 40), (40, 30), (30, 60), (60, 0)}
    matrix = out / "sentence_matrix.def"
    matrix.write_text("80 80\n" + "".join(f"{r} {l} {0 if (r, l) in sentence else 1000}\n"
                                          for r in ids for l in ids), encoding="utf-8", newline="\n")
    space = dictbuild.connection_space(matrix)
    layer, _ = dictbuild.build([words], metadata, 1, directory, connections, space)
    core, _ = dictbuild.build_lattice_core([kana], connections, matrix, metadata, 1, directory)
    (out / "sudachi_lexicon.azdic").write_bytes(layer)
    (out / "lattice_core.azdic").write_bytes(core)
    # A layer without CONN: its words take the ids of their part of speech (lattice spec 8.3).
    technical = out / "technical.lex.tsv"
    technical.write_text(header + lexicon_columns + "テンソル\tてんそる\t名詞\t4000\t\ttechnical\tfixture\n",
                         encoding="utf-8", newline="\n")
    (out / "technical_terms_lexicon.azdic").write_bytes(dictbuild.build([technical], metadata, 4, directory)[0])

    def sections(image: bytes) -> list[tuple[bytes, int, int]]:
        found = []
        for i in range(struct.unpack_from("<I", image, 28)[0]):
            name, _, off, size = struct.unpack_from("<4sIQQ", image, 64 + i * 24)
            found.append((name, off, size))
        return found

    def offset(image: bytes, name: bytes) -> int:
        return next(off for section, off, _ in sections(image) if section == name)

    def patched(image: bytes, name: bytes, at: int, fmt: str, value: int) -> bytes:
        broken = bytearray(image)
        struct.pack_into(fmt, broken, offset(image, name) + at, value)
        struct.pack_into("<Q", broken, 32, dictbuild.fnv(broken[64:]))
        return bytes(broken)

    def repacked(image: bytes, edit_meta=None, drop: bytes = b"") -> bytes:
        parts = []
        for name, off, size in sections(image):
            data = image[off:off + size]
            if name == drop:
                continue
            if name == b"META" and edit_meta:
                meta = json.loads(data.decode("utf-8"))
                edit_meta(meta)
                data = json.dumps(meta, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf-8")
            parts.append((name, data))
        return dictbuild.pack(parts, *struct.unpack_from("<III", image, 16))

    def first_record(image: bytes) -> int:
        """Offset of the first CONN variant record inside the CONN section."""
        table = (struct.unpack_from("<I", image, 20)[0] + 1) * 4
        return table + (-table % 8)

    def without_penalty(meta: dict) -> None:
        del meta["lattice"]["penalties"]["P_unk"]

    def zero_scale(meta: dict) -> None:
        meta["lattice"]["C"] = 0

    def other_space(meta: dict) -> None:
        meta["connection_space"] = dictbuild.CONNECTION_SPACE_PREFIX + "0" * 64

    def without_conn_pos(meta: dict) -> None:
        del meta["conn_pos_table"]

    broken = {
        "core_cmat_reserved": patched(core, b"CMAT", 16, "<I", 1),
        "core_cmat_size": patched(core, b"CMAT", 8, "<I", len(ids) + 1),
        "core_cmat_index": patched(core, b"CMAT", 24 + 2 * 10, "<H", 0x7FFF),
        "core_conn_pos": patched(core, b"CONN", first_record(core) + 6, "<H", 0x3FFF),
        # EIDX[0] (the key いい) points outside ENTS: only a lookup of いい finds it (check (2)).
        "core_entry": patched(core, b"EIDX", 0, "<I", 0xFFFFFFFF),
        "core_no_cmat": repacked(core, drop=b"CMAT"),
        "core_meta_penalty": repacked(core, without_penalty),
        "core_meta_scale": repacked(core, zero_scale),
        "layer_conn_order": patched(layer, b"CONN", 4, "<I", 0xFFFF),
        "layer_conn_pos": patched(layer, b"CONN", first_record(layer) + 6, "<H", 0x3FFF),
        "layer_conn_id": patched(layer, b"CONN", first_record(layer), "<H", 79),
        "layer_space": repacked(layer, other_space),
        "layer_conn_pos_table": repacked(layer, without_conn_pos),
    }
    for name, image in broken.items():
        (out / f"{name}.azdic").write_bytes(image)


def write_neologd_pack_fixtures(directory: Path, tsv: Path, metadata: dict, other_layer: bytes,
                                corrupt: Path) -> None:
    """One directory per case, each holding the pack bytes and the manifest that names them."""
    revision = "1"
    pack, _ = dictbuild.build([tsv], metadata, neologd_pack.LAYER_ID, directory)
    foreign = neologd_pack.with_revision(metadata, "2")
    # Same records, but META names another builder: only the builder_version check rejects it.
    count = struct.unpack_from("<I", pack, 28)[0]
    sections = []
    for i in range(count):
        name, _, off, size = struct.unpack_from("<4sIQQ", pack, 64 + i * 24)
        data = pack[off:off + size]
        if name == b"META":
            meta = dict(neologd_pack.read_meta(pack), builder_version="azdic-2")
            data = json.dumps(meta, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf-8")
        sections.append((name, data))
    other_builder = dictbuild.pack(sections, neologd_pack.LAYER_ID, *struct.unpack_from("<II", pack, 20))
    cases = {
        "valid": (pack, metadata),
        "wrong_layer": (other_layer, metadata),
        "corrupt": (corrupt.read_bytes(), metadata),
        "foreign_attribution": (pack, foreign),
        "foreign_builder": (other_builder, metadata),
    }
    for name, (data, attributed) in cases.items():
        case = directory / "neologd" / name
        case.mkdir(parents=True, exist_ok=True)
        (case / neologd_pack.pack_file_name(revision)).write_bytes(data)
        manifest = neologd_pack.make_manifest(attributed, directory, revision, len(data),
                                              hashlib.sha256(data).hexdigest(),
                                              "https://example.invalid/neologd_lexicon-1.azdic")
        neologd_pack.write_manifest(manifest, case / neologd_pack.MANIFEST_NAME)


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
    # Searching 東京 yields the range [0, count), whose slot 0 holds 東京都.
    (directory / "surface_index_mismatch.azdic").write_bytes(
        with_index([tokyo_to] + [tokyo] * (entry_count - 1)))
    # Only the global order is wrong; lookups of other surfaces may still succeed.
    swapped = order[:]
    swapped[0], swapped[1] = swapped[1], swapped[0]
    (directory / "surface_index_order.azdic").write_bytes(with_index(swapped))


if __name__ == "__main__":
    generate(Path(sys.argv[1]))
