"""Shared deterministic TSV grammar and key normalization for M62-C/D."""

import re
import unicodedata

MOZC_REVISION = "9fbd649bea4c5e99cd8ad5e487213b26a953a376"
CLDR_VERSION = "46.1"
PORTER_VERSION = "1"


def normalize_reading(text):
    text = unicodedata.normalize("NFKC", text)
    return "".join(chr(ord(c) - 0x60) if "ァ" <= c <= "ヶ" else c
                   for c in text if not c.isspace())


def normalize_trigger(text):
    return re.sub(r"[^a-z0-9_+\-]", "", text.translate(str.maketrans(
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ", "abcdefghijklmnopqrstuvwxyz")))


def kana_keys(values):
    return sorted({key for value in values if (key := normalize_reading(value))
                   and re.fullmatch(r"[ぁ-ゖー]+", key)})


def verify(text, emoji):
    if text.startswith("\ufeff") or "\r" in text or not text.endswith("\n"):
        raise ValueError("TSV must be BOM-free UTF-8 with LF endings")
    seen = set()
    for number, line in enumerate(text.splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        cols = line.split("\t")
        if len(cols) != (5 if emoji else 4):
            raise ValueError(f"line {number}: wrong column count")
        multi = {1, 2} if emoji else {1}
        for index, col in enumerate(cols):
            if any(ord(c) < 32 or ord(c) == 127 for c in col):
                raise ValueError(f"line {number}: control character")
            if index not in multi and "|" in col:
                raise ValueError(f"line {number}: separator in scalar")
            if index in multi and col:
                keys = col.split("|")
                norm = normalize_trigger if emoji and index == 2 else normalize_reading
                if any(not key or norm(key) != key for key in keys) or len(set(keys)) != len(keys):
                    raise ValueError(f"line {number}: invalid keys")
        if not cols[0] or not cols[-2] or cols[0] in seen:
            raise ValueError(f"line {number}: empty or duplicate surface/name")
        if not cols[1] and (not emoji or not cols[2]):
            raise ValueError(f"line {number}: no searchable key")
        if not re.fullmatch(r"[0-9]+", cols[-1]) or int(cols[-1]) > 0xffffffff:
            raise ValueError(f"line {number}: invalid rank")
        seen.add(cols[0])
    return len(seen)


def header(emoji):
    name = "emoji_data.tsv" if emoji else "symbol.tsv"
    lines = [f"# Derived from google/mozc {name}, revision {MOZC_REVISION}",
             "# Copyright 2010-2018, Google Inc. All rights reserved.",
             "# License: BSD-3-Clause; see THIRD_PARTY_LICENSES for full terms."]
    if emoji:
        lines += [f"# Unicode CLDR {CLDR_VERSION}; Copyright © 2004-2024 Unicode, Inc.",
                  "# License: Unicode-3.0; see THIRD_PARTY_LICENSES for full terms."]
    lines += [f"# azooKey porter version: {PORTER_VERSION}"]
    return "\n".join(lines) + "\n"


def serialize(rows, emoji, extra_header=""):
    rows.sort(key=lambda row: row[0])
    text = header(emoji) + extra_header + "".join("\t".join(row) + "\n" for row in rows)
    verify(text, emoji)
    return text
