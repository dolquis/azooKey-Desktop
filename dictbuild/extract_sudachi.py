"""Extracts SudachiDict raw lexicon CSVs into the dictbuild intermediate TSV."""

from __future__ import annotations

import argparse
import csv
import io
from pathlib import Path
import re
import sys
from typing import Iterable, Iterator
import zipfile

SOURCE_ID = "sudachidict-core"
# Raw lexicon columns: headword, left id, right id, cost, surface, six part-of-speech
# fields, reading, normalized form, then split information.
COLUMNS = 19
SKIPPED_POS = frozenset(("補助記号", "空白"))
# Keeps the layer inside the bundled size budget (auto-word-registration-spec section 15.9).
# Entries above it are mostly rare inflections and long proper nouns.
DEFAULT_MAX_COST = 10000
ESCAPE = re.compile(r"\\u([0-9A-Fa-f]{4})")


def unescape(text: str) -> str:
    return ESCAPE.sub(lambda match: chr(int(match.group(1), 16)), text)


def to_hiragana(reading: str) -> str:
    return "".join(chr(ord(c) - 0x60) if 0x30A1 <= ord(c) <= 0x30F6 else c for c in reading)


def is_reading(text: str) -> bool:
    return bool(text) and all(0x3041 <= ord(c) <= 0x3096 or c == "ー" for c in text)


def category(pos: list[str]) -> str:
    if pos[1] == "固有名詞" and pos[2] == "人名":
        return "person_name"
    if pos[1] == "固有名詞" and pos[2] == "地名":
        return "place_name"
    return "general"


def convert(rows: Iterable[list[str]],
            max_cost: int = DEFAULT_MAX_COST) -> Iterator[tuple[str, str, str, int, str]]:
    """Yields (surface, reading, pos, cost, category) for rows an IME can look up by reading."""
    for row in rows:
        if len(row) != COLUMNS:
            raise ValueError(f"expected {COLUMNS} columns, got {len(row)}")
        # A negative connection id marks an entry used only to split other entries.
        if row[1] == "-1" or row[5] in SKIPPED_POS or int(row[3]) > max_cost:
            continue
        surface = unescape(row[4])
        reading = to_hiragana(unescape(row[11]))
        # Symbol readings such as "キゴウ" and readings with Latin letters or digits are not
        # something a user types for that surface. The builder reads the TSV with csv quoting
        # and treats a leading "#" as a comment, so neither can pass through unchanged.
        if (not is_reading(reading) or surface == reading or surface.startswith("#")
                or any(c in "\t\r\n\0\"" for c in surface)):
            continue
        pos = row[5:9]
        yield surface, reading, "-".join(p for p in pos if p != "*"), int(row[3]), category(pos)


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


def write_tsv(lexicons: list[Path], revision: str, output: Path,
              max_cost: int = DEFAULT_MAX_COST) -> int:
    """Writes the intermediate TSV with its attribution header; returns the row count."""
    count = 0
    with output.open("w", encoding="utf-8", newline="\n") as out:
        out.write(f"# Derived from SudachiDict {revision} raw lexicon "
                  f"({', '.join(p.name for p in lexicons)}) by dictbuild/extract_sudachi.py "
                  f"--max-cost {max_cost}.\n"
                  "# SPDX-License-Identifier: Apache-2.0\n"
                  "# Attribution: see THIRD_PARTY_LICENSES and the generated ThirdPartyNotices.txt.\n"
                  "surface\treading\tpos\tcost\tfrequency\tcategory\tsource_id\n")
        for lexicon in lexicons:
            for surface, reading, pos, cost, kind in convert(read_lexicon(lexicon), max_cost):
                out.write(f"{surface}\t{reading}\t{pos}\t{cost}\t\t{kind}\t{SOURCE_ID}\n")
                count += 1
    return count


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("lexicons", nargs="+", type=Path,
                        help="small_lex and core_lex (.csv or the published .zip)")
    parser.add_argument("--revision", required=True, help="SudachiDict release, e.g. 20260723")
    parser.add_argument("--max-cost", type=int, default=DEFAULT_MAX_COST)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        count = write_tsv(args.lexicons, args.revision, args.output, args.max_cost)
    except (OSError, ValueError, csv.Error, zipfile.BadZipFile) as exc:
        print(f"extract_sudachi: {exc}", file=sys.stderr)
        return 1
    print(f"extract_sudachi: wrote {count} rows", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
