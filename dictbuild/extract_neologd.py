"""Extracts mecab-ipadic-NEologd seed CSVs into the dictbuild intermediate TSV."""

from __future__ import annotations

import argparse
import csv
import io
import lzma
from pathlib import Path
import sys
from typing import Iterable, Iterator

from extract_sudachi import is_reading, to_hiragana

SOURCE_ID = "mecab-ipadic-neologd"
# Seed columns (mecab-ipadic): surface, left id, right id, cost, four part-of-speech
# fields, conjugation type, conjugation form, base form, reading, pronunciation.
COLUMNS = 13
PROPER_NOUN_CATEGORIES = {"人名": "person_name", "地域": "place_name", "組織": "company_org"}


def category(pos: list[str]) -> str:
    if pos[0] == "名詞" and pos[1] == "固有名詞" and pos[2] in PROPER_NOUN_CATEGORIES:
        return PROPER_NOUN_CATEGORIES[pos[2]]
    return "neologism"


def convert(rows: Iterable[list[str]]) -> Iterator[tuple[str, str, str, int, str]]:
    """Yields (surface, reading, pos, cost, category) for rows an IME can look up by reading."""
    for row in rows:
        if len(row) != COLUMNS:
            raise ValueError(f"expected {COLUMNS} columns, got {len(row)}")
        surface = row[0]
        reading = to_hiragana(row[11])
        # Same exclusions as extract_sudachi: the builder parses the TSV with csv quoting
        # and treats a leading "#" as a comment.
        if (not is_reading(reading) or surface == reading or surface.startswith("#")
                or any(c in "\t\r\n\0\"" for c in surface)):
            continue
        pos = row[4:8]
        yield surface, reading, "-".join(p for p in pos if p != "*"), int(row[3]), category(pos)


def read_seed(path: Path) -> Iterator[list[str]]:
    if path.suffix == ".xz":
        with lzma.open(path, "rb") as raw:
            yield from csv.reader(io.TextIOWrapper(raw, encoding="utf-8", newline=""))
    else:
        with path.open(encoding="utf-8", newline="") as stream:
            yield from csv.reader(stream)


def write_tsv(seeds: list[Path], revision: str, output: Path) -> int:
    """Writes the intermediate TSV with its attribution header; returns the row count."""
    count = 0
    with output.open("w", encoding="utf-8", newline="\n") as out:
        out.write(f"# Derived from mecab-ipadic-NEologd {revision} seed "
                  f"({', '.join(p.name for p in seeds)}) by dictbuild/extract_neologd.py.\n"
                  "# SPDX-License-Identifier: Apache-2.0\n"
                  "# Attribution: download pack only, not in THIRD_PARTY_LICENSES; "
                  "see neologd_lexicon.manifest.json.\n"
                  "surface\treading\tpos\tcost\tfrequency\tcategory\tsource_id\n")
        for seed in seeds:
            for surface, reading, pos, cost, kind in convert(read_seed(seed)):
                out.write(f"{surface}\t{reading}\t{pos}\t{cost}\t\t{kind}\t{SOURCE_ID}\n")
                count += 1
    return count


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("seeds", nargs="+", type=Path, help="seed CSVs (.csv or the published .csv.xz)")
    parser.add_argument("--revision", required=True, help="NEologd seed release, e.g. 20200910")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        count = write_tsv(args.seeds, args.revision, args.output)
    except (OSError, ValueError, csv.Error, lzma.LZMAError) as exc:
        print(f"extract_neologd: {exc}", file=sys.stderr)
        return 1
    print(f"extract_neologd: wrote {count} rows", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
