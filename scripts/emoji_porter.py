"""Re-port pinned Mozc and CLDR annotations without changing emoji sequences."""

import argparse
import sys
import urllib.request
import xml.etree.ElementTree as ET
from pathlib import Path

from rewriter_data import MOZC_REVISION, kana_keys, normalize_trigger, serialize, verify

EMOJI_VERSION = "15.0"


def annotations(xml):
    result = {}
    for element in ET.fromstring(xml).iter("annotation"):
        surface = element.attrib["cp"]
        name, keys = result.setdefault(surface, ["", set()])
        if element.attrib.get("type") == "tts":
            result[surface][0] = element.text or ""
        else:
            keys.update(value.strip() for value in (element.text or "").split("|"))
    return result


def convert(mozc, ja_xml, en_xml, version=EMOJI_VERSION):
    ja, en = annotations(ja_xml), annotations(en_xml)
    ceiling = tuple(map(int, version.split(".")))
    rows = []
    for line in mozc.splitlines():
        if not line or line.startswith("#"):
            continue
        cols = line.split("\t")
        if len(cols) != 7:
            raise ValueError("Mozc emoji row must have seven columns")
        if tuple(map(int, cols[6].removeprefix("E").split("."))) > ceiling:
            continue
        surface = cols[1]  # Never normalize VS16, ZWJ, or skin-tone sequences.
        ja_name, ja_keys = ja.get(surface, ["", set()])
        en_name, en_keys = en.get(surface, ["", set()])
        readings = set(kana_keys(cols[2].split() + list(ja_keys)))
        triggers = {normalize_trigger(key.replace(" ", "_")) for key in en_keys | {en_name}}
        triggers.discard("")
        # Product aliases, independently authored to satisfy the public examples.
        if surface == "😄":
            readings.add("わらい")
            triggers.add("smile")
        if not readings and not triggers:
            continue
        name = ja_name or cols[4]
        if not name:
            continue
        rows.append([surface, "|".join(sorted(readings)), "|".join(sorted(triggers)), name, ""])
    for index, row in enumerate(rows):
        # Keep the documented smile example first among otherwise equal matches.
        row[-1] = str(len(rows) + 1 if row[0] == "😄" else len(rows) - index)
    return serialize(rows, True, f"# Emoji version ceiling: {version}\n"
                     "# Product aliases: 😄 = わらい / smile (preferred equal match).\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path)
    parser.add_argument("--ja", type=Path)
    parser.add_argument("--en", type=Path)
    parser.add_argument("--fetch", action="store_true")
    parser.add_argument("--verify", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.verify:
        print(f"verified {verify(args.verify.read_bytes().decode('utf-8'), True)} emoji")
        return
    if args.fetch:
        urls = [f"https://raw.githubusercontent.com/google/mozc/{MOZC_REVISION}/src/data/emoji/emoji_data.tsv",
                "https://raw.githubusercontent.com/unicode-org/cldr/release-46-1/common/annotations/ja.xml",
                "https://raw.githubusercontent.com/unicode-org/cldr/release-46-1/common/annotations/en.xml"]
        sources = []
        for url in urls:
            with urllib.request.urlopen(url, timeout=30) as response:
                sources.append(response.read().decode("utf-8"))
    elif args.source and args.ja and args.en:
        sources = [path.read_text(encoding="utf-8") for path in (args.source, args.ja, args.en)]
    else:
        parser.error("provide --source/--ja/--en, --fetch or --verify")
    result = convert(*sources)
    if args.output:
        args.output.write_bytes(result.encode("utf-8"))
    else:
        sys.stdout.buffer.write(result.encode("utf-8"))


if __name__ == "__main__":
    main()
