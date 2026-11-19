"""Deterministic .azdic v1 builder; inputs are local, attributed lexicon TSVs."""

from __future__ import annotations

import argparse
from array import array
from collections import Counter, deque
import csv
import hashlib
import io
import json
import math
from pathlib import Path
import statistics
import struct
import sys
from typing import Iterable, Iterator, NamedTuple
import zipfile

LAYERS = ("base_lexicon", "sudachi_lexicon", "neologd_lexicon",
          "named_entity_lexicon", "technical_terms_lexicon")
PRIORITY = (.20, .30, .35, .45, .50)
CATEGORIES = ("general", "person_name", "place_name", "station_name", "product_name",
              "software", "anime_game", "company_org", "technical", "neologism")
NONE = 0xFFFFFFFF
PAIRS = (("ゔぁ", "ば"), ("づ", "ず"), ("ぢ", "じ"))
# Connection TSV (azdic spec section 15.12). conn_pos is the conn_pos_table string: the six
# upstream part-of-speech and conjugation fields joined by "," with "*" kept.
CONNECTION_COLUMNS = ("reading", "surface", "left_id", "right_id", "cost", "pos", "conjugation",
                      "normalized_form", "conn_pos")
# CONN attr (azdic spec section 15.5): bits 0-13 index META.conn_pos_table.
MAX_CONN_POS = 1 << 14
KATAKANA_ONLY = 1 << 14
NOT_NORMALIZED = 1 << 15
NO_INDEX = 0xFFFF
CONNECTION_SPACE_PREFIX = "sudachidict-matrix:"
# Initial lattice correction terms (lattice-conversion-spec sections 3.4 and 5.2), recalibrated in M52.
LATTICE_PENALTIES = {"P_kata_variant": 6000, "P_kata_own": 0, "P_node": 3000, "P_unk": 20000}
LATTICE_C = 10000
DEFAULT_POS = ("名詞", "普通名詞", "一般", "*")
TERMINAL_FORM = "終止形-一般"


def normalize(text: str) -> str:
    if not text or "\0" in text:
        raise ValueError("empty or NUL-containing text")
    text.encode("utf-8", errors="strict")
    return "".join(chr(ord(c) - 0x60) if 0x30A1 <= ord(c) <= 0x30F6 else
                   chr(ord(c) - 0xFEE0) if 0xFF01 <= ord(c) <= 0xFF5E else c for c in text)


def aliases(key: str) -> list[str]:
    found = {key}
    pending = deque([key])
    while pending:
        current = pending.popleft()
        for a, b in PAIRS:
            for old, new in ((a, b), (b, a)):
                start = 0
                while (pos := current.find(old, start)) >= 0:
                    candidate = current[:pos] + new + current[pos + len(old):]
                    if candidate not in found:
                        if len(found) == 8:
                            print("warning: alias expansion exceeds eight keys; exact key only", file=sys.stderr)
                            return [key]
                        found.add(candidate)
                        pending.append(candidate)
                    start = pos + len(old)
    return sorted(found)


def fnv(data: bytes) -> int:
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return value


def validate_sources(metadata: dict, catalog: Path) -> str:
    sources = metadata.get("sources")
    if not isinstance(sources, list) or not sources:
        raise ValueError("META.sources must be nonempty")
    notices = []
    seen = set()
    for source in sources:
        for field in ("source_id", "spdx", "upstream_url", "upstream_revision", "transform", "copyright"):
            if not isinstance(source.get(field), str) or not source[field].strip():
                raise ValueError(f"missing attribution: {field}")
        if source["source_id"] in seen:
            raise ValueError("duplicate source_id")
        seen.add(source["source_id"])
        ids = source.get("notice_ids")
        if not isinstance(ids, list):
            raise ValueError("notice_ids must be an array")
        notices.append("\n".join(source[field] for field in
                                ("source_id", "spdx", "upstream_url", "upstream_revision", "copyright")))
        # License text and upstream-specific notices are both catalog entries.
        for notice_id in [source["spdx"], *ids]:
            if not isinstance(notice_id, str) or not notice_id or any(
                    c not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_." for c in notice_id
            ) or notice_id in (".", ".."):
                raise ValueError("invalid notice id")
            path = catalog / (notice_id + ".txt")
            body = path.read_text(encoding="utf-8")
            if not body.strip():
                raise ValueError(f"empty notice: {notice_id}")
            notices.append(body)
    return "\n\n".join(notices) + "\n"


def load_entries(inputs: list[Path], metadata: dict) -> list[dict]:
    if len({p.name for p in inputs}) != len(inputs):
        raise ValueError("input basenames must be unique for reproducible tie breaking")
    source_ids = {s["source_id"] for s in metadata["sources"]}
    contributed = set()
    merged = {}
    for path in sorted(inputs, key=lambda p: p.name):
        lines = path.read_text(encoding="utf-8").splitlines()
        comments = "\n".join(line for line in lines if line.startswith("#"))
        if "THIRD_PARTY_LICENSES" not in comments or "SPDX-License-Identifier:" not in comments:
            raise ValueError(f"{path.name}: attribution header missing")
        reader = csv.DictReader((line for line in lines if not line.startswith("#")), delimiter="\t")
        for row in reader:
            if None in row or any(row.get(k) is None for k in ("surface", "reading", "pos", "source_id")):
                raise ValueError(f"{path.name}: invalid lexicon row")
            if row["source_id"] not in source_ids:
                raise ValueError("unattributed source_id")
            contributed.add(row["source_id"])
            key = normalize(row["reading"])
            normalize(row["surface"])
            if not row["pos"]:
                raise ValueError("empty part of speech")
            cost = max(-32768, min(32767, int(row.get("cost") or 5000)))
            frequency = float(row.get("frequency") or max(0, min(1, (8000 - cost) / 10000)))
            if not math.isfinite(frequency) or not 0 <= frequency <= 1:
                raise ValueError("invalid frequency")
            category = set(filter(None, (row.get("category") or "general").split(",")))
            if not category.issubset(CATEGORIES):
                raise ValueError("unknown category")
            entry = dict(surface=row["surface"], reading=row["reading"], key=key,
                         pos=row["pos"], cost=cost, frequency=frequency, category=category)
            identity = key, entry["surface"]
            previous = merged.get(identity)
            if previous:
                winner = entry if frequency > previous["frequency"] else previous
                winner["cost"] = min(cost, previous["cost"])
                winner["category"] = category | previous["category"]
                entry = winner
            merged[identity] = entry
    if contributed != source_ids:
        raise ValueError("META sources must match contributing inputs")
    return [merged[k] for k in sorted(merged)]


class Connection(NamedTuple):
    key: str
    reading: str
    surface: str
    left_id: int
    right_id: int
    cost: int
    conn_pos: str
    normalized_form: str

    @property
    def kana_only(self) -> bool:
        """Spelled in the hiragana of its reading: such words live in lattice_core only."""
        return self.surface == self.reading


def read_connections(path: Path) -> list[Connection]:
    """Reads a connection TSV. Fields are split on tabs without quoting."""
    lines = path.read_text(encoding="utf-8").splitlines()
    comments = "\n".join(line for line in lines if line.startswith("#"))
    if "THIRD_PARTY_LICENSES" not in comments or "SPDX-License-Identifier:" not in comments:
        raise ValueError(f"{path.name}: attribution header missing")
    body = [line for line in lines if not line.startswith("#")]
    if not body or tuple(body[0].split("\t")) != CONNECTION_COLUMNS:
        raise ValueError(f"{path.name}: unexpected connection columns")
    rows = []
    for line in body[1:]:
        fields = line.split("\t")
        if len(fields) != len(CONNECTION_COLUMNS):
            raise ValueError(f"{path.name}: invalid connection row")
        reading, surface, left, right, cost, pos, _, normalized, conn_pos = fields
        key = normalize(reading)
        normalize(surface)
        left_id, right_id = int(left), int(right)
        if not (0 <= left_id <= NO_INDEX and 0 <= right_id <= NO_INDEX):
            raise ValueError(f"{path.name}: connection id out of range")
        if not pos or len(conn_pos.split(",")) != 6 or not all(conn_pos.split(",")):
            raise ValueError(f"{path.name}: invalid connection part of speech")
        rows.append(Connection(key, reading, surface, left_id, right_id,
                               max(-32768, min(32767, int(cost))), sys.intern(conn_pos), normalized))
    return rows


def katakana_only(text: str) -> bool:
    # U+30A1..U+30FA, U+30FB, U+30FC and U+30FD..U+30FE: one contiguous range (azdic spec 15.5).
    return bool(text) and all(0x30A1 <= ord(c) <= 0x30FE for c in text)


def connection_section(entries: list[dict], rows: Iterable[Connection],
                       complete: bool = False) -> tuple[bytes, list[str]]:
    """CONN for `entries` (azdic spec section 15.5) and the conn_pos_table its attr indices name.

    Every row must belong to an entry by (normalized key, surface); with `complete`, every entry
    must also have a row. Identical variants of one entry are kept once; the rest are ordered by
    (cost, left_id, right_id, attr).
    """
    rows = list(rows)
    positions = sorted({row.conn_pos for row in rows})
    if len(positions) > MAX_CONN_POS:
        raise ValueError("too many connection parts of speech")
    position_ids = {pos: i for i, pos in enumerate(positions)}
    index = {(entry["key"], entry["surface"]): i for i, entry in enumerate(entries)}
    variants: list[set] = [set() for _ in entries]
    for row in rows:
        entry = index.get((row.key, row.surface))
        if entry is None:
            raise ValueError("connection row without a lexicon entry")
        attr = (position_ids[row.conn_pos] | (KATAKANA_ONLY if katakana_only(row.surface) else 0)
                | (NOT_NORMALIZED if row.surface != row.normalized_form else 0))
        variants[entry].add((row.cost, row.left_id, row.right_id, attr))
    if complete and not all(variants):
        raise ValueError("lexicon entry without connection rows")
    begin = [0]
    records = bytearray()
    for found in variants:
        for cost, left, right, attr in sorted(found):
            records.extend(struct.pack("<HHhH", left, right, cost, attr))
        begin.append(begin[-1] + len(found))
    table = bytearray(struct.pack(f"<{len(begin)}I", *begin))
    table.extend(b"\0" * (-len(table) % 8))
    return bytes(table + records), positions


def matrix_section(lines: Iterator[str], left_ids: Iterable[int], right_ids: Iterable[int]) -> bytes:
    """CMAT (azdic spec section 15.5) holding the matrix.def cells the given ids need.

    matrix.def lists `right id of the previous word, left id of the next word, cost`, after a
    header with the sizes of those two id spaces in the same order (the MeCab and Sudachi layout).
    Id 0 (sentence start and end) always gets an index. Every needed cell must be listed once.
    Ids whose rows (columns) hold the same costs share one index, in order of first use
    (lattice-conversion-spec section 9, step 1).
    """
    header = next(lines, "").split()
    if len(header) != 2:
        raise ValueError("matrix.def: invalid header")
    raw_right, raw_left = int(header[0]), int(header[1])
    if not (0 < raw_left <= 65536 and 0 < raw_right <= 65536):
        raise ValueError("matrix.def: invalid id space size")
    lefts, rights = sorted(set(left_ids) | {0}), sorted(set(right_ids) | {0})
    if lefts[0] < 0 or rights[0] < 0 or lefts[-1] >= raw_left or rights[-1] >= raw_right:
        raise ValueError("connection id outside matrix.def")
    if len(lefts) >= NO_INDEX or len(rights) >= NO_INDEX:
        raise ValueError("too many connection ids")
    left_index, right_index = array("H", [NO_INDEX]) * raw_left, array("H", [NO_INDEX]) * raw_right
    for i, value in enumerate(lefts):
        left_index[value] = i
    for i, value in enumerate(rights):
        right_index[value] = i
    width = len(lefts)
    cost = array("h", bytes(2 * width * len(rights)))
    seen = bytearray(width * len(rights))
    for line in lines:
        fields = line.split()
        if not fields:
            continue
        if len(fields) != 3:
            raise ValueError("matrix.def: invalid row")
        right, left, value = int(fields[0]), int(fields[1]), int(fields[2])
        if not (0 <= right < raw_right and 0 <= left < raw_left and -32768 <= value <= 32767):
            raise ValueError("matrix.def: row outside the header sizes or the cost range")
        r, l = right_index[right], left_index[left]
        if r == NO_INDEX or l == NO_INDEX:
            continue
        cell = r * width + l
        if seen[cell]:
            raise ValueError("matrix.def: duplicate cell")
        seen[cell] = 1
        cost[cell] = value
    if seen.count(0):
        raise ValueError("matrix.def: missing cells")
    cost, width, row_of, column_of = merge_matrix(cost, width, len(rights))
    for i, value in enumerate(lefts):
        left_index[value] = column_of[i]
    for i, value in enumerate(rights):
        right_index[value] = row_of[i]
    if sys.byteorder != "little":
        for values in (left_index, right_index, cost):
            values.byteswap()
    data = bytearray(struct.pack("<6I", raw_left, raw_right, width, len(cost) // width, 0, 0))
    data.extend(left_index.tobytes() + right_index.tobytes())
    data.extend(b"\0" * (-len(data) % 8))
    data.extend(cost.tobytes())
    return bytes(data)


def merge_matrix(cost: array, width: int, height: int) -> tuple[array, int, list[int], list[int]]:
    """Merges the rows, then the columns, of a `height` x `width` cost matrix that hold the same
    costs. Returns the merged matrix, its width, and the new index of each old row and column.
    Equal rows stay equal after the columns merge, so one pass over each axis is enough."""
    rows: dict[bytes, int] = {}
    row_of = [rows.setdefault(cost[r * width:(r + 1) * width].tobytes(), len(rows))
              for r in range(height)]
    kept = array("h")
    for row in rows:
        kept.frombytes(row)
    columns: dict[bytes, int] = {}
    column_of = [columns.setdefault(kept[c::width].tobytes(), len(columns)) for c in range(width)]
    first = {}
    for c, index in enumerate(column_of):
        first.setdefault(index, c)
    merged = array("h")
    for r in range(len(rows)):
        row = kept[r * width:(r + 1) * width]
        merged.extend(row[c] for c in first.values())
    return merged, len(columns), row_of, column_of


def representative(pairs: Counter) -> list[int]:
    """The most frequent (left_id, right_id); the smaller pair on a tie (lattice spec 8.3)."""
    return list(min(pairs.items(), key=lambda item: (-item[1], item[0]))[0])


def lattice_tables(rows: list[Connection]) -> dict:
    """META.lattice from every imported upstream row (azdic spec section 15.12)."""
    by_pos: dict[str, Counter] = {}
    for row in rows:
        by_pos.setdefault(row.conn_pos, Counter())[row.left_id, row.right_id] += 1
    name = lambda fields: "-".join(f for f in fields[:4] if f != "*")
    names = sorted({name(pos.split(",")) for pos in by_pos})
    name_ids = {value: i for i, value in enumerate(names)}
    terminal = [Counter() for _ in names]
    uninflected = [Counter() for _ in names]
    table = []
    for pos, pairs in by_pos.items():
        fields = pos.split(",")
        i = name_ids[name(fields)]
        table.append([i, fields[4], fields[5], *representative(pairs)])
        if fields[5] == TERMINAL_FORM:
            terminal[i].update(pairs)
        elif fields[5] == "*":
            uninflected[i].update(pairs)
    nouns = [row.cost for row in rows if tuple(row.conn_pos.split(",")[:4]) == DEFAULT_POS]
    if not nouns:
        raise ValueError(f"no {'-'.join(DEFAULT_POS[:3])} row for the default word cost")
    return {"pos_names": names, "pos_ids": sorted(table),
            "pos_name_ids": [representative(t or u) if t or u else None
                             for t, u in zip(terminal, uninflected)],
            "penalties": dict(LATTICE_PENALTIES), "C": LATTICE_C,
            "default_word_cost": statistics.median_low(nouns)}


def make_trie(keys: list[str]) -> bytes:
    # Sorted keys let each node own a contiguous range, so no per-node child map is
    # materialized; flat arrays keep a million-key lexicon within memory.
    encoded = [key.encode("utf-8") for key in keys]
    base, parent, key_ids, depth = (array("I") for _ in range(4))
    used = bytearray()

    def grow(size: int) -> None:
        extra = size - len(used)
        if extra > 0:
            extra = max(extra, len(used) // 2)
            base.extend(bytes(4 * extra))
            parent.frombytes(b"\xff" * (4 * extra))
            key_ids.frombytes(b"\xff" * (4 * extra))
            depth.extend(bytes(4 * extra))
            used.extend(bytes(extra))

    grow(256)
    parent[0], used[0] = 0, 1
    pending = deque([(0, 0, len(encoded), 0)])
    free = 1
    while pending:
        index, lo, hi, level = pending.popleft()
        if lo < hi and len(encoded[lo]) == level:
            key_ids[index] = lo
            lo += 1
        if lo == hi:
            continue
        labels, ranges = [], []
        while lo < hi:
            byte, end = encoded[lo][level], lo + 1
            while end < hi and encoded[end][level] == byte:
                end += 1
            labels.append(byte)
            ranges.append((lo, end))
            lo = end
        found = used.find(0, free)
        free = slot = found if found >= 0 else len(used)
        while True:
            offset = slot - labels[0]
            if offset >= 1:
                grow(offset + labels[-1] + 1)
                if not any(used[offset + byte] for byte in labels):
                    break
            slot = used.find(0, slot + 1)
            if slot < 0:
                slot = len(used)
        base[index] = offset
        for byte, (start, end) in zip(labels, ranges):
            child = offset + byte
            used[child] = 1
            parent[child] = index
            depth[child] = level + 1
            pending.append((child, start, end, level + 1))
    size = used.rfind(1) + 1
    nodes = array("I", bytes(16 * size))
    for field, values in enumerate((base, parent, key_ids, depth)):
        nodes[field::4] = values[:size]
    if sys.byteorder != "little":
        nodes.byteswap()
    return nodes.tobytes()


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def read_matrix(path: Path) -> Iterator[str]:
    """Lines of matrix.def, or of the only file in a .zip such as the upstream matrix.def.zip."""
    if path.suffix == ".zip":
        with zipfile.ZipFile(path) as archive:
            names = [n for n in archive.namelist() if not n.endswith("/")]
            if len(names) != 1:
                raise ValueError(f"{path.name}: expected exactly one file")
            with archive.open(names[0]) as raw:
                yield from io.TextIOWrapper(raw, encoding="utf-8")
    else:
        with path.open(encoding="utf-8") as stream:
            yield from stream


def connection_space(matrix: Path) -> str:
    """META.connection_space for connection ids from `matrix` (lattice-conversion-spec section 8.4)."""
    return CONNECTION_SPACE_PREFIX + file_sha256(matrix)


def build(inputs: list[Path], metadata: dict, layer: int, catalog: Path,
          connections: Path | None = None, space: str | None = None) -> tuple[bytes, str]:
    """Builds a layer from lexicon TSVs.

    With `connections` (a connection TSV) and its `space`, CONN carries the connection variants of
    every entry (azdic spec section 15.12). Rows of words spelled in the hiragana of their reading
    are left to lattice_core (build_lattice_core).
    """
    if connections is None:
        return assemble(inputs, metadata, layer, catalog, list(inputs))
    rows = [row for row in read_connections(connections) if not row.kana_only]
    return assemble(inputs, metadata, layer, catalog, [*inputs, connections], rows, space)


def build_lattice_core(inputs: list[Path], connections: Path, matrix: Path, metadata: dict,
                       layer: int, catalog: Path) -> tuple[bytes, str]:
    """Builds lattice_core.azdic (azdic spec section 15.12, lattice-conversion-spec section 8.2).

    `inputs` hold the words spelled in the hiragana of their reading. CMAT keeps the matrix cells
    of every connection id in `connections` (both lattice_core and the layer built from the other
    rows) and of the representative ids in META.lattice.
    """
    rows = read_connections(connections)
    tables = lattice_tables(rows)
    representatives = [row[3:5] for row in tables["pos_ids"]] + [p for p in tables["pos_name_ids"] if p]
    cmat = matrix_section(read_matrix(matrix), {row.left_id for row in rows} | {p[0] for p in representatives},
                          {row.right_id for row in rows} | {p[1] for p in representatives})
    return assemble(inputs, metadata, layer, catalog, [*inputs, connections, matrix],
                    [row for row in rows if row.kana_only], connection_space(matrix),
                    complete=True, extra_sections=[(b"CMAT", cmat)], extra_metadata={"lattice": tables})


def assemble(inputs: list[Path], metadata: dict, layer: int, catalog: Path, files: list[Path],
             rows: list[Connection] | None = None, space: str | None = None, complete: bool = False,
             extra_sections: Iterable[tuple[bytes, bytes]] = (), extra_metadata: dict | None = None
             ) -> tuple[bytes, str]:
    """Writes the .azdic image. `files` are recorded in META.inputs; `rows` become CONN."""
    notices = validate_sources(metadata, catalog)
    entries = load_entries(inputs, metadata)
    positions = sorted({e["pos"] for e in entries}) or ["unknown"]
    if len(positions) > 65536:
        raise ValueError("too many parts of speech")
    pos_ids = {pos: i for i, pos in enumerate(positions)}
    if len({p.name for p in files}) != len(files):
        raise ValueError("input basenames must be unique")
    metadata = dict(metadata, pos_table=positions, builder_version="azdic-1",
                    inputs=[{"name": p.name, "sha256": file_sha256(p)}
                            for p in sorted(files, key=lambda p: p.name)], **(extra_metadata or {}))
    conn = []
    if rows is not None:
        if not isinstance(space, str) or not space.startswith(CONNECTION_SPACE_PREFIX):
            raise ValueError("invalid connection space")
        data, metadata["conn_pos_table"] = connection_section(entries, rows, complete)
        metadata["connection_space"] = space
        conn = [(b"CONN", data)]
    strings = bytearray()
    string_ids = {}

    def intern(text: str) -> tuple[int, int]:
        if text not in string_ids:
            raw = text.encode("utf-8")
            string_ids[text] = len(strings), len(raw)
            strings.extend(raw)
        return string_ids[text]

    records = bytearray()
    references = {}
    for index, entry in enumerate(entries):
        category = sum(1 << CATEGORIES.index(c) for c in entry["category"])
        records.extend(struct.pack("<IIIIHHhHBB6x", *intern(entry["surface"]), *intern(entry["reading"]),
                                   pos_ids[entry["pos"]], category, entry["cost"],
                                   int(entry["frequency"] * 65535 + .5), layer,
                                   int(PRIORITY[layer] * 255 + .5)))
        for key in aliases(entry["key"]):
            references.setdefault(key, []).append((index, int(key != entry["key"])))
    keys = sorted(references)
    key_records = bytearray()
    refs = bytearray()
    for key in keys:
        key_records.extend(struct.pack("<II", len(refs) // 8, len(references[key])))
        for index, kind in references[key]:
            refs.extend(struct.pack("<IB3x", index, kind))
    # Surface index for reverse lookup: ENTS indices by (surface bytes, index).
    by_surface = sorted(range(len(entries)), key=lambda i: (entries[i]["surface"].encode("utf-8"), i))
    sections = [(b"TRIE", make_trie(keys)), (b"KEYS", key_records), (b"EIDX", refs),
                (b"ENTS", records), (b"STRS", strings),
                (b"SIDX", struct.pack(f"<{len(by_surface)}I", *by_surface)), *conn, *extra_sections,
                (b"META", json.dumps(metadata, ensure_ascii=False, sort_keys=True,
                                     separators=(",", ":")).encode("utf-8"))]
    image = pack(sections, layer, len(entries), len(keys))
    verify(image)
    return image, notices


def pack(sections: list[tuple[bytes, bytes]], layer: int, entry_count: int, key_count: int) -> bytes:
    image = bytearray(64 + 24 * len(sections))
    for i, (name, data) in enumerate(sections):
        image.extend(b"\0" * (-len(image) % 8))
        struct.pack_into("<4sIQQ", image, 64 + i * 24, name, 0, len(image), len(data))
        image.extend(data)
    struct.pack_into("<8sHHIIIIIQ", image, 0, b"AZDIC1\0\0", 1, 64, 1, layer,
                     entry_count, key_count, len(sections), fnv(image[64:]))
    return bytes(image)


def verify(image: bytes) -> None:
    if len(image) < 64:
        raise ValueError("short header")
    magic, version, header, flags, layer, count, key_count, section_count, digest = struct.unpack_from("<8sHHIIIIIQ", image)
    if magic != b"AZDIC1\0\0" or version != 1 or header != 64 or flags & ~1 or layer > 4 or any(image[40:64]):
        raise ValueError("invalid header")
    if not 6 <= section_count <= 64 or len(image) < 64 + section_count * 24 or fnv(image[64:]) != digest:
        raise ValueError("invalid table or hash")
    sections = {}
    ranges = []
    for i in range(section_count):
        name, reserved, offset, length = struct.unpack_from("<4sIQQ", image, 64 + i * 24)
        if reserved or name in sections or offset % 8 or offset < 64 + section_count * 24 or offset > len(image) or length > len(image) - offset:
            raise ValueError("invalid section")
        sections[name] = image[offset:offset + length]
        ranges.append((offset, offset + length))
    ranges.sort()
    if any(a[1] > b[0] for a, b in zip(ranges, ranges[1:])):
        raise ValueError("overlapping sections")
    if not set((b"TRIE", b"KEYS", b"EIDX", b"ENTS", b"STRS", b"META")).issubset(sections):
        raise ValueError("missing section")
    if len(sections[b"KEYS"]) != key_count * 8 or len(sections[b"ENTS"]) != count * 32 or (
            b"SIDX" in sections and len(sections[b"SIDX"]) != count * 4):
        raise ValueError("record count mismatch")
    nodes = list(struct.iter_unpack("<IIII", sections[b"TRIE"]))
    keys = list(struct.iter_unpack("<II", sections[b"KEYS"]))
    refs = list(struct.iter_unpack("<IB3s", sections[b"EIDX"]))
    meta = json.loads(sections[b"META"])
    if not nodes or nodes[0][1:] != (0, NONE, 0):
        raise ValueError("invalid root")
    seen = set()
    key_texts = {}
    for index, (base, parent, key, depth) in enumerate(nodes[1:], 1):
        if parent == NONE:
            continue
        if parent >= len(nodes) or nodes[parent][1] == NONE or not 1 <= index - nodes[parent][0] <= 255 or depth != nodes[parent][3] + 1:
            raise ValueError("invalid edge")
        if key != NONE:
            if key >= key_count or key in seen:
                raise ValueError("invalid key id")
            seen.add(key)
            word = bytearray()
            current = index
            while current:
                up = nodes[current][1]
                if up >= len(nodes) or nodes[up][3] >= nodes[current][3]:
                    raise ValueError("invalid ancestry")
                word.append(current - nodes[up][0])
                current = up
            key_texts[key] = bytes(reversed(word)).decode("utf-8", errors="strict")
            normalize(key_texts[key])
    if len(seen) != key_count:
        raise ValueError("unreachable keys")
    if any(key_texts[i - 1].encode("utf-8") >= key_texts[i].encode("utf-8")
           for i in range(1, key_count)):
        raise ValueError("invalid key order")
    for offset, length in keys:
        if not length or offset > len(refs) or length > len(refs) - offset:
            raise ValueError("invalid key references")
    for entry, kind, reserved in refs:
        if entry >= count or kind > 1 or any(reserved):
            raise ValueError("invalid entry reference")
    readings = []
    surfaces = []
    for off in range(0, len(sections[b"ENTS"]), 32):
        a, b, c, d, pos, category, cost, frequency, source, priority = struct.unpack_from("<IIIIHHhHBB", sections[b"ENTS"], off)
        if pos >= len(meta["pos_table"]) or source != layer or category & ~1023 or any(sections[b"ENTS"][off + 26:off + 32]):
            raise ValueError("invalid entry metadata")
        for offset, length in ((a, b), (c, d)):
            pool = sections[b"STRS"]
            if not length or offset > len(pool) or length > len(pool) - offset:
                raise ValueError("invalid string range")
            normalize(pool[offset:offset + length].decode("utf-8", errors="strict"))
        readings.append(normalize(sections[b"STRS"][c:c + d].decode("utf-8")))
        surfaces.append(sections[b"STRS"][a:a + b])
    if b"SIDX" in sections:
        order = [index for (index,) in struct.iter_unpack("<I", sections[b"SIDX"])]
        if any(index >= count for index in order):
            raise ValueError("invalid surface index")
        # Strictly increasing (surface, index) also makes SIDX a permutation of ENTS.
        if any((surfaces[a], a) >= (surfaces[b], b) for a, b in zip(order, order[1:])):
            raise ValueError("invalid surface index order")
    for key_id, (offset, length) in enumerate(keys):
        for entry, kind, _ in refs[offset:offset + length]:
            key = key_texts[key_id]
            if (kind == 0 and key != readings[entry]) or (kind == 1 and (
                    key == readings[entry] or key not in aliases(readings[entry]))):
                raise ValueError("key does not match entry reading")
    if b"CONN" in sections or b"CMAT" in sections:
        verify_connections(sections, meta, count, surfaces)


def matrix_indices(data: bytes) -> tuple[array, array]:
    """The two id-to-index tables of a CMAT section after its structure checks (azdic spec 15.5 (4))."""
    if len(data) < 24:
        raise ValueError("invalid connection matrix")
    raw_left, raw_right, left_count, right_count, *reserved = struct.unpack_from("<6I", data)
    if (any(reserved) or not 1 <= raw_left <= 65536 or not 1 <= raw_right <= 65536
            or not 1 <= left_count < NO_INDEX or not 1 <= right_count < NO_INDEX):
        raise ValueError("invalid connection matrix")
    head = 24 + 2 * (raw_left + raw_right)
    padded = head + (-head % 8)
    if len(data) != padded + 2 * left_count * right_count or any(data[head:padded]):
        raise ValueError("invalid connection matrix size")
    left, right = array("H"), array("H")
    left.frombytes(data[24:24 + 2 * raw_left])
    right.frombytes(data[24 + 2 * raw_left:head])
    if sys.byteorder != "little":
        left.byteswap()
        right.byteswap()
    for values, limit in ((left, left_count), (right, right_count)):
        if any(value != NO_INDEX and value >= limit for value in values) or values[0] == NO_INDEX:
            raise ValueError("invalid connection matrix index")
    return left, right


def verify_connections(sections: dict, meta: dict, count: int, surfaces: list[bytes]) -> None:
    """dictbuild --verify of CONN and CMAT: the structure of azdic spec 15.5 (4), and for CONN also
    the variant order and attr bit 14 against the surface. The upstream normalized form (bit 15)
    is not stored, so that bit is not checked."""
    matrix = matrix_indices(sections[b"CMAT"]) if b"CMAT" in sections else None
    if b"CONN" not in sections:
        return
    table = meta.get("conn_pos_table")
    if (not isinstance(table, list) or not table or len(table) > MAX_CONN_POS
            or not all(isinstance(pos, str) and pos for pos in table)):
        raise ValueError("invalid conn_pos_table")
    if not isinstance(meta.get("connection_space"), str) or not meta["connection_space"]:
        raise ValueError("invalid connection_space")
    data = sections[b"CONN"]
    head = 4 * (count + 1)
    padded = head + (-head % 8)
    if len(data) < padded:
        raise ValueError("invalid connection table")
    begin = struct.unpack_from(f"<{count + 1}I", data)
    if (begin[0] != 0 or any(a > b for a, b in zip(begin, begin[1:]))
            or len(data) != padded + 8 * begin[-1] or any(data[head:padded])):
        raise ValueError("invalid connection table")
    variants = list(struct.iter_unpack("<HHhH", data[padded:]))
    for entry in range(count):
        found = variants[begin[entry]:begin[entry + 1]]
        if any((a[2], a[0], a[1], a[3]) > (b[2], b[0], b[1], b[3]) for a, b in zip(found, found[1:])):
            raise ValueError("invalid connection variant order")
        katakana = katakana_only(surfaces[entry].decode("utf-8"))
        for left, right, _, attr in found:
            if (attr & (MAX_CONN_POS - 1)) >= len(table) or bool(attr & KATAKANA_ONLY) != katakana:
                raise ValueError("invalid connection variant attr")
            if matrix and (left >= len(matrix[0]) or matrix[0][left] == NO_INDEX
                           or right >= len(matrix[1]) or matrix[1][right] == NO_INDEX):
                raise ValueError("connection id outside CMAT")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inputs", nargs="*", type=Path)
    parser.add_argument("--metadata", type=Path)
    parser.add_argument("--layer", choices=LAYERS)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--notices", type=Path)
    parser.add_argument("--catalog", type=Path, default=Path(__file__).parent / "notices")
    parser.add_argument("--verify", type=Path)
    args = parser.parse_args()
    try:
        if args.verify:
            verify(args.verify.read_bytes())
        else:
            if not (args.inputs and args.metadata and args.layer and args.output and args.notices):
                parser.error("build requires inputs, --metadata, --layer, --output and --notices")
            image, notices = build(args.inputs, json.loads(args.metadata.read_text(encoding="utf-8")),
                                   LAYERS.index(args.layer), args.catalog)
            args.output.write_bytes(image)
            args.notices.write_text(notices, encoding="utf-8", newline="\n")
            verify(args.output.read_bytes())
    except (OSError, ValueError, KeyError, TypeError, struct.error) as exc:
        print(f"dictbuild: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
