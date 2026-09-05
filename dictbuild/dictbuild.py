"""Deterministic .azdic v1 builder; inputs are local, attributed lexicon TSVs."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
import struct
import sys
from collections import deque

LAYERS = ("base_lexicon", "sudachi_lexicon", "neologd_lexicon",
          "named_entity_lexicon", "technical_terms_lexicon")
PRIORITY = (.20, .30, .35, .45, .50)
CATEGORIES = ("general", "person_name", "place_name", "station_name", "product_name",
              "software", "anime_game", "company_org", "technical", "neologism")
NONE = 0xFFFFFFFF
PAIRS = (("ゔぁ", "ば"), ("づ", "ず"), ("ぢ", "じ"))


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


def make_trie(keys: list[str]) -> bytes:
    root = {}
    for key_id, key in enumerate(keys):
        node = root
        for byte in key.encode("utf-8"):
            node = node.setdefault(byte, {})
        node[0] = key_id
    nodes = [[0, 0, NONE, 0]]
    pending = deque([(0, root)])
    free = 1
    while pending:
        index, children = pending.popleft()
        nodes[index][2] = children.get(0, NONE)
        labels = sorted(b for b in children if b)
        if not labels:
            continue
        base = max(1, free - labels[0])
        while any(base + b < len(nodes) and nodes[base + b][1] != NONE for b in labels):
            base += 1
        nodes[index][0] = base
        while len(nodes) <= base + labels[-1]:
            nodes.append([0, NONE, NONE, 0])
        for byte in labels:
            child = base + byte
            nodes[child][1] = index
            nodes[child][3] = nodes[index][3] + 1
            pending.append((child, children[byte]))
        while free < len(nodes) and nodes[free][1] != NONE:
            free += 1
    return b"".join(struct.pack("<IIII", *node) for node in nodes)


def build(inputs: list[Path], metadata: dict, layer: int, catalog: Path) -> tuple[bytes, str]:
    notices = validate_sources(metadata, catalog)
    entries = load_entries(inputs, metadata)
    positions = sorted({e["pos"] for e in entries}) or ["unknown"]
    if len(positions) > 65536:
        raise ValueError("too many parts of speech")
    pos_ids = {pos: i for i, pos in enumerate(positions)}
    metadata = dict(metadata, pos_table=positions, builder_version="azdic-1",
                    inputs=[{"name": p.name, "sha256": hashlib.sha256(p.read_bytes()).hexdigest()}
                            for p in sorted(inputs, key=lambda p: p.name)])
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
    sections = [(b"TRIE", make_trie(keys)), (b"KEYS", key_records), (b"EIDX", refs),
                (b"ENTS", records), (b"STRS", strings),
                (b"META", json.dumps(metadata, ensure_ascii=False, sort_keys=True,
                                     separators=(",", ":")).encode("utf-8"))]
    image = bytearray(64 + 24 * len(sections))
    for i, (name, data) in enumerate(sections):
        image.extend(b"\0" * (-len(image) % 8))
        struct.pack_into("<4sIQQ", image, 64 + i * 24, name, 0, len(image), len(data))
        image.extend(data)
    struct.pack_into("<8sHHIIIIIQ", image, 0, b"AZDIC1\0\0", 1, 64, 1, layer,
                     len(entries), len(keys), len(sections), fnv(image[64:]))
    verify(bytes(image))
    return bytes(image), notices


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
    if len(sections[b"KEYS"]) != key_count * 8 or len(sections[b"ENTS"]) != count * 32:
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
    for key_id, (offset, length) in enumerate(keys):
        for entry, kind, _ in refs[offset:offset + length]:
            key = key_texts[key_id]
            if (kind == 0 and key != readings[entry]) or (kind == 1 and (
                    key == readings[entry] or key not in aliases(readings[entry]))):
                raise ValueError("key does not match entry reading")


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
