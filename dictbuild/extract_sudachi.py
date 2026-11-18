"""Extracts SudachiDict raw lexicon CSVs into the dictbuild intermediate TSV."""

from __future__ import annotations

import argparse
import contextlib
import csv
import io
from pathlib import Path
import re
import sys
from typing import Iterable, Iterator
import zipfile

import dictbuild

SOURCE_ID = "sudachidict-core"
# Raw lexicon columns: headword, left id, right id, cost, surface, six part-of-speech
# fields, reading, normalized form, then split information.
COLUMNS = 19
SKIPPED_POS = frozenset(("補助記号", "空白"))
# Keeps the layer inside the bundled size budget (auto-word-registration-spec section 15.9).
# Entries above it are mostly rare inflections and long proper nouns.
DEFAULT_MAX_COST = 10000
ESCAPE = re.compile(r"\\u([0-9A-Fa-f]{4})")
# Characters that the Joyo Kanji Table (2010) lists as the standard form, although Sudachi
# normalizes them to a CP932 character (`剝がす` -> `剥がす`, `塡める` -> `填める`). They stay
# in the lexicon: a word spelled with one of them is an ordinary spelling, not a variant glyph.
STANDARD_FORM_EXCEPTIONS = frozenset("剝塡頰𠮟")


def unescape(text: str) -> str:
    return ESCAPE.sub(lambda match: chr(int(match.group(1), 16)), text)


def to_hiragana(reading: str) -> str:
    return "".join(chr(ord(c) - 0x60) if 0x30A1 <= ord(c) <= 0x30F6 else c for c in reading)


def is_reading(text: str) -> bool:
    return bool(text) and all(0x3041 <= ord(c) <= 0x3096 or c == "ー" for c in text)


def is_ideograph(char: str) -> bool:
    code = ord(char)
    return 0x3400 <= code <= 0x9FFF or 0xF900 <= code <= 0xFAFF or 0x20000 <= code <= 0x2FFFF


def in_cp932(char: str) -> bool:
    try:
        char.encode("cp932")
    except UnicodeEncodeError:
        return False
    return True


def variant_characters(rows: Iterable[list[str]]) -> frozenset[str]:
    """Ideographs outside CP932 that Sudachi's own normalized form replaces with a CP932 one.

    Compares the surface and the normalized form of each entry, position by position, when they
    have the same length: `硏究` -> `研究` gives `硏`, `仁俠` -> `任侠` gives `俠`. The evidence
    comes from the lexicon itself, so no hand-kept variant table is needed. The characters in
    STANDARD_FORM_EXCEPTIONS are left out.
    """
    found: set[str] = set()
    for row in rows:
        if len(row) != COLUMNS:
            raise ValueError(f"expected {COLUMNS} columns, got {len(row)}")
        surface, normalized = unescape(row[4]), unescape(row[12])
        if len(surface) != len(normalized):
            continue
        for char, standard in zip(surface, normalized):
            if (char != standard and is_ideograph(char) and not in_cp932(char)
                    and is_ideograph(standard) and in_cp932(standard)):
                found.add(char)
    return frozenset(found) - STANDARD_FORM_EXCEPTIONS


def category(pos: list[str]) -> str:
    if pos[1] == "固有名詞" and pos[2] == "人名":
        return "person_name"
    if pos[1] == "固有名詞" and pos[2] == "地名":
        return "place_name"
    return "general"


def imported(row: list[str], max_cost: int, variants: frozenset[str]) -> tuple[str, str] | None:
    """(surface, reading) of a row both intermediate TSVs take, or None for a skipped row.

    A common noun, verb, or other non-proper-noun entry whose surface holds one of `variants`
    (a variant glyph such as `硏` or `俠`, see variant_characters) is skipped. Proper nouns
    keep theirs: `髙島屋` is spelled that way.
    """
    if len(row) != COLUMNS:
        raise ValueError(f"expected {COLUMNS} columns, got {len(row)}")
    # A negative connection id marks an entry used only to split other entries.
    if row[1] == "-1" or row[5] in SKIPPED_POS or int(row[3]) > max_cost:
        return None
    surface = unescape(row[4])
    reading = to_hiragana(unescape(row[11]))
    # Symbol readings such as "キゴウ" and readings with Latin letters or digits are not
    # something a user types for that surface. The builder reads the TSV with csv quoting
    # and treats a leading "#" as a comment, so neither can pass through unchanged.
    if (not is_reading(reading) or surface.startswith("#")
            or any(c in "\t\r\n\0\"" for c in surface)):
        return None
    if "固有名詞" not in row[5:9] and any(char in variants for char in surface):
        return None
    return surface, reading


def lexicon_row(row: list[str], surface: str, reading: str) -> tuple[str, str, str, int, str]:
    pos = row[5:9]
    return surface, reading, "-".join(p for p in pos if p != "*"), int(row[3]), category(pos)


def connection_row(row: list[str], surface: str, reading: str) -> tuple[str, ...]:
    """The connection TSV columns (dictbuild.CONNECTION_COLUMNS) of one imported row."""
    normalized = unescape(row[12])
    # The connection TSV is split on tabs without quoting; a quote or a leading "#" is fine here.
    if any(c in "\t\r\n\0" for c in normalized):
        raise ValueError(f"normalized form of {surface} holds a tab, line break or NUL")
    return (reading, surface, row[1], row[2], row[3], "-".join(p for p in row[5:9] if p != "*"),
            "-".join(p for p in row[9:11] if p != "*"), normalized, ",".join(row[5:11]))


def convert(rows: Iterable[list[str]], max_cost: int = DEFAULT_MAX_COST,
            variants: frozenset[str] = frozenset(),
            kana_only: bool = False) -> Iterator[tuple[str, str, str, int, str]]:
    """Yields (surface, reading, pos, cost, category) for rows an IME can look up by reading.

    A word spelled in the hiragana of its own reading belongs to lattice_core only (azdic spec
    section 15.12); `kana_only` yields those words instead of the others.
    """
    for row in rows:
        kept = imported(row, max_cost, variants)
        if kept is not None and (kept[0] == kept[1]) == kana_only:
            yield lexicon_row(row, *kept)


def convert_connections(rows: Iterable[list[str]], max_cost: int = DEFAULT_MAX_COST,
                        variants: frozenset[str] = frozenset()) -> Iterator[tuple[str, ...]]:
    """Yields the connection TSV rows: every row that convert keeps with or without `kana_only`."""
    for row in rows:
        kept = imported(row, max_cost, variants)
        if kept is not None:
            yield connection_row(row, *kept)


def read_lexicon(path: Path) -> Iterator[list[str]]:
    if path.suffix == ".zip":
        with zipfile.ZipFile(path) as archive:
            names = [n for n in archive.namelist() if n.endswith(".csv")]
            if len(names) != 1:
                raise ValueError(f"{path.name}: expected exactly one CSV")
            with archive.open(names[0]) as raw:
                yield from csv.reader(io.TextIOWrapper(raw, encoding="utf-8", newline=""))
    else:
        with path.open(encoding="utf-8", newline="") as stream:
            yield from csv.reader(stream)


def header(revision: str, lexicons: list[Path], max_cost: int, columns: Iterable[str]) -> str:
    return (f"# Derived from SudachiDict {revision} raw lexicon "
            f"({', '.join(p.name for p in lexicons)}) by dictbuild/extract_sudachi.py "
            f"--max-cost {max_cost}.\n"
            "# SPDX-License-Identifier: Apache-2.0\n"
            "# Attribution: see THIRD_PARTY_LICENSES and the generated ThirdPartyNotices.txt.\n"
            + "\t".join(columns) + "\n")


LEXICON_COLUMNS = ("surface", "reading", "pos", "cost", "frequency", "category", "source_id")


def write_tsv(lexicons: list[Path], revision: str, output: Path, max_cost: int = DEFAULT_MAX_COST,
              connections: Path | None = None, kana: Path | None = None) -> int:
    """Writes the intermediate TSV with its attribution header; returns the row count.

    `connections` receives the connection TSV and `kana` the lexicon TSV of the words spelled in
    the hiragana of their reading, both for lattice_core (azdic spec section 15.12).
    """
    variants = frozenset().union(*(variant_characters(read_lexicon(lexicon))
                                   for lexicon in lexicons))
    count = 0
    with contextlib.ExitStack() as stack:
        def open_tsv(path: Path | None, columns: Iterable[str]):
            if path is None:
                return None
            stream = stack.enter_context(path.open("w", encoding="utf-8", newline="\n"))
            stream.write(header(revision, lexicons, max_cost, columns))
            return stream

        words = open_tsv(output, LEXICON_COLUMNS)
        kana_words = open_tsv(kana, LEXICON_COLUMNS)
        links = open_tsv(connections, dictbuild.CONNECTION_COLUMNS)
        for lexicon in lexicons:
            for row in read_lexicon(lexicon):
                kept = imported(row, max_cost, variants)
                if kept is None:
                    continue
                target = kana_words if kept[0] == kept[1] else words
                if target is not None:
                    surface, reading, pos, cost, kind = lexicon_row(row, *kept)
                    target.write(f"{surface}\t{reading}\t{pos}\t{cost}\t\t{kind}\t{SOURCE_ID}\n")
                    count += target is words
                if links is not None:
                    links.write("\t".join(connection_row(row, *kept)) + "\n")
    return count


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("lexicons", nargs="+", type=Path,
                        help="small_lex and core_lex (.csv or the published .zip)")
    parser.add_argument("--revision", required=True, help="SudachiDict release, e.g. 20260723")
    parser.add_argument("--max-cost", type=int, default=DEFAULT_MAX_COST)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--connections", type=Path, help="also write the connection TSV here")
    parser.add_argument("--kana", type=Path, help="also write the lattice_core lexicon TSV here")
    args = parser.parse_args()
    try:
        count = write_tsv(args.lexicons, args.revision, args.output, args.max_cost,
                          args.connections, args.kana)
    except (OSError, ValueError, csv.Error, zipfile.BadZipFile) as exc:
        print(f"extract_sudachi: {exc}", file=sys.stderr)
        return 1
    print(f"extract_sudachi: wrote {count} rows", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
