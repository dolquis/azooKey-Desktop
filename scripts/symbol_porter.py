"""Re-port pinned Mozc symbols. Output is verified before writing anything."""

import argparse
import sys
import urllib.request
from pathlib import Path

from rewriter_data import MOZC_REVISION, kana_keys, serialize, verify


def convert(text):
    rows = []
    for line in text.splitlines():
        if not line or line.startswith("#") or line.startswith("POS\t"):
            continue
        cols = line.split("\t")
        if len(cols) < 3:
            raise ValueError("Mozc symbol row has fewer than three columns")
        if len(cols) < 4:
            continue  # Missing trailing description is an empty description.
        readings = kana_keys(cols[2].split())
        if not readings or not cols[3]:
            continue
        rows.append([cols[1], "|".join(readings), cols[3], ""])
    for index, row in enumerate(rows):
        row[-1] = str(len(rows) - index)
    return serialize(rows, False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path)
    parser.add_argument("--fetch", action="store_true")
    parser.add_argument("--verify", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.verify:
        print(f"verified {verify(args.verify.read_bytes().decode('utf-8'), False)} symbols")
        return
    if args.fetch:
        url = f"https://raw.githubusercontent.com/google/mozc/{MOZC_REVISION}/src/data/symbol/symbol.tsv"
        with urllib.request.urlopen(url, timeout=30) as response:
            source = response.read().decode("utf-8")
    elif args.source:
        source = args.source.read_text(encoding="utf-8")
    else:
        parser.error("provide --source, --fetch or --verify")
    result = convert(source)
    if args.output:
        args.output.write_bytes(result.encode("utf-8"))
    else:
        sys.stdout.buffer.write(result.encode("utf-8"))


if __name__ == "__main__":
    main()
