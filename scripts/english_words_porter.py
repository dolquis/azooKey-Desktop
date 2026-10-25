"""Generate data/english-words.tsv from pinned SCOWL, Google Books Ngram and linguist data.

Three stages, all deterministic given the pinned inputs:

  counts  stream the 24 Ngram 1-gram shards once, keep the 1990-2019 match counts of
          the candidate surfaces only (an offline step run by hand; ~13.4 GB download).
  build   SCOWL word list + tech terms + counts -> data/english-words.tsv.
  verify  check a TSV against docs/inline-english-candidate-spec.md section 4.4.

The Ngram step is not run by CI or cloud-setup.sh. The pinned SHA-256 values below let a
later run detect upstream drift. See THIRD_PARTY_LICENSES for the three sources.
"""

import argparse
import hashlib
import io
import math
import re
import sys
import urllib.request
import zipfile
import zlib
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

PORTER_VERSION = "1"
ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = ROOT / "data" / "english-words.tsv"

SCOWL_TAG = "rel-2026.02.25"
SCOWL_COMMIT = "7e99edab8e32f9f9ea2b15f249ca8d4d67237410"
SCOWL_URL = ("https://github.com/en-wl/wordlist/releases/download/"
             f"{SCOWL_TAG}/hunspell-en_US-2026.02.25.zip")
SCOWL_SHA256 = "ac8e73310e951d88c52c2cf2ba54ceaca34f8486a81630ac8a75dc5f931179f9"

LINGUIST_TAG = "v9.7.0"
LINGUIST_URL = ("https://raw.githubusercontent.com/github-linguist/linguist/"
                f"{LINGUIST_TAG}/lib/linguist/languages.yml")
LINGUIST_SHA256 = "7c2bc5b59662de6c5d09cd4990e82b2541d4cd2ea7c8c213537730474f24a5c7"

NGRAM_VERSION = "20200217"
NGRAM_SHARDS = 24
NGRAM_BASE = f"https://storage.googleapis.com/books/ngrams/books/{NGRAM_VERSION}/eng/"
NGRAM_YEARS = (1990, 2019)  # inclusive
# Compressed size and SHA-256 of every shard, in shard order. Filled in by `counts` on the
# first run (printed as a ready-to-paste block); later runs refuse any mismatch.
NGRAM_SHARD_BYTES = (
    265116429, 516407175, 469613220, 433796325, 470860746, 430527082, 479124157, 621693906,
    627251491, 568490258, 601722860, 614673843, 544641231, 611684090, 620141271, 549554806,
    627221886, 637782333, 593921274, 603614113, 612141103, 567682980, 650608487, 706657351)
NGRAM_SHARD_SHA256 = (
    "d2568b44db1bf33e02b76373aa2861517332c84137ba3a8cee657441496c4b45",
    "ea4828275463479b5acdb055e5f5bea9f9cd518f9613dc3a2ebd84c5d3061e3c",
    "1ab1dba104d181b0aa883b0d7831cde48060c1e432204be19bb995c8efa34c1b",
    "c4a1d91fb7f29089e247450c768ac7f76b91b5225713ca36ab48fcd1da3e4e06",
    "799f6e9514c988b7655c9601a8f3abb912bf10741d4ab238a198231b664bd154",
    "e59cb5e596ba2a929dbf95fc1a44cb6d8559fc4c857a30e46ed8112ad6205b03",
    "e0d13c8664bf917a4cf40805a5191125f31275dc2a3d7f106dc132493b94753a",
    "1b9371bccc875ca5cb2ef48cd32e9b86bfe5270f4d3f83257a5d0b98a64c3bb8",
    "684d05885d5e00e60bc85cf03fe857fa4938d1f8e967b037677c5f28384214be",
    "b473a861daa57500f40b4b222b9fc58f9d2f1f64cba34525c28266b0265b22b2",
    "1815a46f1fa2d01c6cffc02a4590ab7601171e7762e668af17140df3c16eca58",
    "6438a13d25af6208ad628db5b0fe89a6705fb09960673f90fd8111fd174f1c6d",
    "39c80b9d2e2dc51d8dfc4f7870141d0b3a0082fdead87ae55f495a3bc79f2e1d",
    "7d4b084f854870d3776f5c9ba253c5690b0ad5f74b992a6e323c4676680d7c44",
    "6267febda571239df3aa1ecd20bfa0bb6126e6dbf78cb35e94a55e1a4a21df82",
    "48781c66f2211f3ad36c8785195c4b4c7926a1813c60b93e79807ce79ab0a7a7",
    "6c6af5ae894951e040b9c5f3417fc4765a92d0f28925d635eab25421a45b1410",
    "e3febf59da2f720cea6c633d7bbd5b96120b36ad205b5f58ace487e8920c6875",
    "7f2715ec65466efdba2ce2e399dff741268b58dfec4f1ab4b4820367cac65ae9",
    "98b48b4ca15846a5908d51f199fa6a80d5b937ed3d0edd7b4d09eda8b3bb7cdf",
    "e6ef1b0f26f1519cc3b9cc9d6aa3ea018b82e868785029aa581e3ff32f3a2092",
    "ebe0439514fc965ededccdeaf71c8ce8486855418a3e635c82aa5e31f9998804",
    "db63aa244891dc2fd00cb996341c7b3268a22fe7bc44d22e2e1805c3e162f85b",
    "76fb6a9a9301d8dc3ef4083602e0dea6f5a5f0457100f224fb6c3867ad98865d",
)
# SHA-256 of the intermediate counts file written by `counts` (empty until first run).
COUNTS_SHA256 = "4fcfe3acb039668bb7ad97f44628d71f2c6c11ecd1e0d39a4d3a3368a6741779"

SCALE = 4096  # frequency = 1 + floor(SCALE * log2(1 + count)); order preserving, <= 2^31
FLOOR_FREQUENCY = 1  # surfaces with no Ngram match in the window
MAX_ENTRIES = 200_000  # docs/inline-english-candidate-spec.md section 4.4
MAX_BYTES = 64 * 1024 * 1024
UINT32_MAX = 2**32 - 1

# linguist language types worth offering; "prose" (natural languages) is excluded.
LINGUIST_TYPES = {"programming", "markup", "data"}
# Hand-written terms that linguist does not cover. All-uppercase entries are acronyms.
CUSTOM_TECH = (
    "API CLI CPU CSS CUDA DLL DNS GPU GUI HTML HTTP HTTPS IDE IME JSON JWT LLM NPM "
    "OAuth PDF REST SDK SQL SSH TCP TLS TSF UDP URL USB UTF XML YAML "
    "GitHub GitLab Git Linux macOS Windows Docker Kubernetes PowerShell WebSocket "
    "Vulkan OpenGL DirectX LLaMA GGUF ChatGPT OpenAI Anthropic Claude Gemini "
    "Azure AWS Nginx Redis Postgres PostgreSQL MySQL SQLite MongoDB GraphQL "
    "TypeScript JavaScript"
).split()

HEADER = """\
# english-words.tsv (UTF-8, LF). Generated by scripts/english_words_porter.py v{version}; do not edit by hand.
# Format: <surface>\\t<frequency>\\t[flags]  (docs/inline-english-candidate-spec.md section 4.4)
# SPDX-License-Identifier: LicenseRef-SCOWL AND CC-BY-3.0 AND MIT
# Derived data. Attribution and license texts: THIRD_PARTY_LICENSES and dictbuild/notices/.
# - Word list: SCOWL / ESDB hunspell en_US size 60 {scowl_tag} ({scowl_commit}),
#   https://github.com/en-wl/wordlist, LicenseRef-SCOWL (dictbuild/notices/scowl-en_US-copyright.txt).
#   Expanded by this script from en_US.dic / en_US.aff; modified (letters-only surfaces, taboo words dropped).
# - Frequency: Google Books Ngram Viewer English 1-grams, version {ngram_version}, CC BY 3.0 Unported
#   (https://creativecommons.org/licenses/by/3.0/). Modified: match counts of {y0}-{y1} summed per surface,
#   then rescaled as 1 + floor({scale} * log2(1 + count)); surfaces without a match get {floor}.
#   Source: https://storage.googleapis.com/books/ngrams/books/datasetsv3.html
# - Tech terms: GitHub linguist {linguist_tag} languages.yml (MIT) and a hand-written list.
# Regenerate: python scripts/english_words_porter.py counts && python scripts/english_words_porter.py build
# counts-sha256: {counts_sha}
"""


# ---------------------------------------------------------------------------------------
# SCOWL: hunspell expansion
# ---------------------------------------------------------------------------------------

def parse_affixes(aff_text):
    """Return ({flag: (cross_product, [(strip, add, condition_regex)])} for PFX and SFX)."""
    tables = {"PFX": {}, "SFX": {}}
    for line in aff_text.splitlines():
        parts = line.split()
        if len(parts) < 4 or parts[0] not in tables:
            continue
        kind, flag = parts[0], parts[1]
        if len(parts) == 4 and parts[3].isdigit():  # header: PFX A Y 1
            tables[kind][flag] = (parts[2] == "Y", [])
            continue
        if flag not in tables[kind] or len(parts) < 5:
            continue
        strip = "" if parts[2] == "0" else parts[2]
        add = "" if parts[3] == "0" else parts[3].split("/")[0]
        condition = parts[4]
        regex = re.compile(("^" + condition) if kind == "PFX" else (condition + "$"))
        tables[kind][flag][1].append((strip, add, regex))
    return tables


def _apply(kind, word, rules):
    for strip, add, regex in rules:
        if not regex.search(word):
            continue
        if kind == "PFX":
            if strip and not word.startswith(strip):
                continue
            yield add + word[len(strip):]
        else:
            if strip and not word.endswith(strip):
                continue
            yield word[:len(word) - len(strip)] + add


def expand_hunspell(dic_text, aff_text):
    """Expand a hunspell dictionary into its surface forms (flags are single characters)."""
    tables = parse_affixes(aff_text)
    only_compound = re.search(r"^ONLYINCOMPOUND\s+(\S)", aff_text, re.M)
    only_compound = only_compound.group(1) if only_compound else None
    nosuggest = re.search(r"^NOSUGGEST\s+(\S)", aff_text, re.M)
    nosuggest = nosuggest.group(1) if nosuggest else None
    words = set()
    lines = dic_text.splitlines()
    for line in lines[1:]:  # the first line is the entry count
        if not line or line.startswith("#"):
            continue
        word, _, flags = line.partition("/")
        flags = flags.split()[0] if flags.split() else ""
        if (only_compound and only_compound in flags) or (nosuggest and nosuggest in flags):
            continue
        words.add(word)
        prefixed = {word}
        for flag in flags:
            if flag in tables["PFX"]:
                cross, rules = tables["PFX"][flag]
                for form in _apply("PFX", word, rules):
                    words.add(form)
                    if cross:
                        prefixed.add(form)
        for flag in flags:
            if flag in tables["SFX"]:
                cross, rules = tables["SFX"][flag]
                for form in _apply("SFX", word, rules):
                    words.add(form)
                    if cross:
                        for base in prefixed - {word}:
                            for both in _apply("SFX", base, rules):
                                words.add(both)
    return words


def letters_only(words):
    return {w for w in words if re.fullmatch(r"[A-Za-z]+", w)}


# ---------------------------------------------------------------------------------------
# linguist: language names and aliases (languages.yml has a fixed, simple layout)
# ---------------------------------------------------------------------------------------

def parse_linguist(yml_text):
    """Return names and aliases of languages whose type is in LINGUIST_TYPES."""
    terms = set()
    name, kind, aliases, in_aliases = None, None, [], False

    def flush():
        if name and kind in LINGUIST_TYPES:
            terms.add(name)
            terms.update(aliases)

    for line in yml_text.splitlines():
        if not line or line.startswith("#"):
            continue
        if not line[0].isspace():
            flush()
            name = line.rstrip(":").strip().strip('"')
            kind, aliases, in_aliases = None, [], False
            continue
        stripped = line.strip()
        if stripped.startswith("type:"):
            kind = stripped.split(":", 1)[1].strip()
            in_aliases = False
        elif stripped == "aliases:":
            in_aliases = True
        elif in_aliases and stripped.startswith("- "):
            aliases.append(stripped[2:].strip().strip('"'))
        else:
            in_aliases = False
    flush()
    return {t for t in terms if re.fullmatch(r"[A-Za-z]{2,}", t)}


# ---------------------------------------------------------------------------------------
# Frequency and flags
# ---------------------------------------------------------------------------------------

def scale(count):
    if count <= 0:
        return FLOOR_FREQUENCY
    value = 1 + math.floor(SCALE * math.log2(1 + count))
    if value > 2**31:
        raise ValueError(f"scaled frequency {value} exceeds 2^31")
    return value


def flags_for(surface, tech):
    flags = []
    if len(surface) >= 2 and surface.isupper():
        flags.append("acronym")
    elif surface[0].isupper():
        flags.append("proper")
    if tech:
        flags.append("tech")
    return ",".join(flags)


def build_rows(scowl_words, tech_words, counts):
    """Return the final (surface, frequency, flags) rows, deterministic order."""
    rows = []
    for surface in set(scowl_words) | set(tech_words):
        rows.append((surface, scale(counts.get(surface, 0)),
                     flags_for(surface, surface in tech_words)))
    rows.sort(key=lambda r: (-r[1], r[0]))
    if len(rows) > MAX_ENTRIES:
        raise ValueError(f"{len(rows)} entries exceed the {MAX_ENTRIES} limit")
    return rows


def render(rows, counts_sha):
    header = HEADER.format(
        version=PORTER_VERSION, scowl_tag=SCOWL_TAG, scowl_commit=SCOWL_COMMIT,
        ngram_version=NGRAM_VERSION, y0=NGRAM_YEARS[0], y1=NGRAM_YEARS[1], scale=SCALE,
        floor=FLOOR_FREQUENCY, linguist_tag=LINGUIST_TAG, counts_sha=counts_sha)
    body = "".join(f"{s}\t{f}\t{fl}\n" if fl else f"{s}\t{f}\n" for s, f, fl in rows)
    text = header + body
    verify(text)
    return text


def verify(text):
    """Check the section 4.4 constraints a host would otherwise skip silently."""
    if text.startswith("\ufeff") or "\r" in text or not text.endswith("\n"):
        raise ValueError("TSV must be BOM-free UTF-8 with LF endings")
    if len(text.encode("utf-8")) > MAX_BYTES:
        raise ValueError("TSV exceeds 64 MiB")
    seen, entries = set(), 0
    for number, line in enumerate(text.splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        cols = line.split("\t")
        if len(cols) not in (2, 3):
            raise ValueError(f"line {number}: expected 2 or 3 columns")
        surface, frequency = cols[0], cols[1]
        if not re.fullmatch(r"[A-Za-z]+", surface):
            raise ValueError(f"line {number}: surface must be ASCII letters")
        if not (frequency.isdigit() and 0 < int(frequency) <= UINT32_MAX):
            raise ValueError(f"line {number}: frequency must be a positive uint32")
        if len(cols) == 3 and not set(cols[2].split(",")) <= {"proper", "acronym", "tech"}:
            raise ValueError(f"line {number}: unknown flag")
        if surface in seen:
            raise ValueError(f"line {number}: duplicate surface")
        seen.add(surface)
        entries += 1
    if entries > MAX_ENTRIES:
        raise ValueError(f"{entries} entries exceed the {MAX_ENTRIES} limit")
    return entries


# ---------------------------------------------------------------------------------------
# Inputs
# ---------------------------------------------------------------------------------------

def sha256(data):
    return hashlib.sha256(data).hexdigest()


def fetch(url, expected_sha256, cache):
    if cache.exists():
        data = cache.read_bytes()
    else:
        with urllib.request.urlopen(url, timeout=60) as response:
            data = response.read()
        cache.parent.mkdir(parents=True, exist_ok=True)
        cache.write_bytes(data)
    if sha256(data) != expected_sha256:
        raise ValueError(f"{url}: SHA-256 {sha256(data)} != pinned {expected_sha256}")
    return data


def load_candidates(work):
    zdata = fetch(SCOWL_URL, SCOWL_SHA256, work / "hunspell-en_US.zip")
    ldata = fetch(LINGUIST_URL, LINGUIST_SHA256, work / "languages.yml")
    with zipfile.ZipFile(io.BytesIO(zdata)) as archive:
        dic = archive.read("en_US.dic").decode("utf-8")
        aff = archive.read("en_US.aff").decode("utf-8")
    scowl = letters_only(expand_hunspell(dic, aff))
    tech = parse_linguist(ldata.decode("utf-8")) | set(CUSTOM_TECH)
    return scowl, tech


def count_shard(args):
    """Stream one Ngram shard; return (index, size, sha256, {surface: count})."""
    index, candidates_path = args
    candidates = set(Path(candidates_path).read_text(encoding="utf-8").split())
    url = f"{NGRAM_BASE}1-{index:05d}-of-{NGRAM_SHARDS:05d}.gz"
    digest, size = hashlib.sha256(), 0
    inflater = zlib.decompressobj(wbits=31)
    counts, tail = {}, b""
    lo, hi = NGRAM_YEARS

    def consume(chunk):
        nonlocal tail
        data = tail + chunk
        lines = data.split(b"\n")
        tail = lines.pop()
        for raw in lines:
            token, _, rest = raw.partition(b"\t")
            surface = token.decode("utf-8", "replace")
            if surface not in candidates:
                continue
            total = 0
            for item in rest.split(b"\t"):
                year, match, _volume = item.split(b",")
                if lo <= int(year) <= hi:
                    total += int(match)
            if total:
                counts[surface] = counts.get(surface, 0) + total

    with urllib.request.urlopen(url, timeout=120) as response:
        while chunk := response.read(1 << 20):
            digest.update(chunk)
            size += len(chunk)
            consume(inflater.decompress(chunk))
    consume(inflater.flush() + b"\n")
    return index, size, digest.hexdigest(), counts


def cmd_counts(args):
    work = Path(args.work)
    work.mkdir(parents=True, exist_ok=True)
    scowl, tech = load_candidates(work)
    candidates = work / "candidates.txt"
    candidates.write_text("\n".join(sorted(scowl | tech)) + "\n", encoding="utf-8")
    shards = range(NGRAM_SHARDS) if args.shard is None else [args.shard]
    totals, report = {}, []
    with ProcessPoolExecutor(max_workers=args.jobs) as pool:
        for index, size, digest, counts in pool.map(
                count_shard, [(i, str(candidates)) for i in shards]):
            if size != NGRAM_SHARD_BYTES[index]:
                raise ValueError(f"shard {index}: {size} bytes != pinned {NGRAM_SHARD_BYTES[index]}")
            if NGRAM_SHARD_SHA256 and digest != NGRAM_SHARD_SHA256[index]:
                raise ValueError(f"shard {index}: SHA-256 {digest} != pinned")
            report.append((index, size, digest))
            for surface, count in counts.items():
                totals[surface] = totals.get(surface, 0) + count
            print(f"shard {index}: {size} bytes sha256 {digest}", file=sys.stderr)
    text = "".join(f"{s}\t{totals[s]}\n" for s in sorted(totals))
    out = work / "ngram-counts.tsv"
    out.write_text(text, encoding="utf-8", newline="\n")
    print(f"counts: {len(totals)} surfaces, sha256 {sha256(text.encode('utf-8'))}", file=sys.stderr)
    if not NGRAM_SHARD_SHA256 and args.shard is None:
        print("NGRAM_SHARD_SHA256 = (", file=sys.stderr)
        for _i, _s, digest in report:
            print(f'    "{digest}",', file=sys.stderr)
        print(")", file=sys.stderr)


def cmd_build(args):
    work = Path(args.work)
    scowl, tech = load_candidates(work)
    counts_text = (work / "ngram-counts.tsv").read_text(encoding="utf-8")
    counts_sha = sha256(counts_text.encode("utf-8"))
    if COUNTS_SHA256 and counts_sha != COUNTS_SHA256:
        raise ValueError(f"counts SHA-256 {counts_sha} != pinned {COUNTS_SHA256}")
    counts = {}
    for line in counts_text.splitlines():
        surface, _, count = line.partition("\t")
        counts[surface] = int(count)
    text = render(build_rows(scowl, tech, counts), counts_sha)
    Path(args.output).write_text(text, encoding="utf-8", newline="\n")
    print(f"wrote {args.output}: sha256 {sha256(text.encode('utf-8'))}", file=sys.stderr)


def cmd_verify(args):
    text = Path(args.output).read_text(encoding="utf-8")
    print(f"{verify(text)} entries", file=sys.stderr)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)
    for name, func in (("counts", cmd_counts), ("build", cmd_build), ("verify", cmd_verify)):
        p = sub.add_parser(name)
        p.add_argument("--work", default="D:/tmp/dev1691", help="scratch directory")
        p.add_argument("--output", default=str(DEFAULT_OUTPUT))
        if name == "counts":
            p.add_argument("--shard", type=int, help="run a single shard (trial run)")
            p.add_argument("--jobs", type=int, default=2)
        p.set_defaults(func=func)
    args = parser.parse_args(argv)
    args.func(args)


if __name__ == "__main__":
    main()
