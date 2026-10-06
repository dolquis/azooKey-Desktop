"""Opt-in, CPU-only dictionary score analysis; never builds a production dictionary.

The mappings below are comparisons, not approved formulas, coefficients or defaults.
This independently authored decoder follows the pinned converter's binary layout;
it does not reuse converter code or perform LOUDS trie lookup. All output belongs
in an ignored build directory. No model, GPU, TIP or VM is used.
"""

from __future__ import annotations

import argparse
from collections import Counter
import csv
from dataclasses import dataclass
import hashlib
import json
import math
from pathlib import Path
import struct
import sys
import tarfile
import time
from typing import Iterator
import urllib.request

import extract_sudachi

STORAGE_REVISION = "4d418525b090cf49c219819d05a7e3cc2a4346eb"
CONVERTER_REVISION = "d59a28e4c7ca049aef04f29a91eae9677a7753f2"
BASE_URL = f"https://codeload.github.com/azooKey/azooKey_dictionary_storage/tar.gz/{STORAGE_REVISION}"
BASE_SHA256 = "9a4f66c3f4b6a80d59471eb7890209faf49a6c856e7a72c1a6f9b862266aa026"
SUDACHI_REVISION = "20260723"
SUDACHI_PINS = (
    ("small_lex.zip", "b578ac9545899783d5d7e30d5d78d5d9dcf40b36965d4af0663ec2eb041c1093"),
    ("core_lex.zip", "a2b39e1572adab08a649b1390b134517adc55f1d733c59358b113298788bf31c"),
)
BASE_PRIORITY = .20
SUDACHI_PRIORITY = .30
EXACT_BONUS = .10
MAPPINGS = ("constant_comparison", "fixed_affine_comparison", "observed_affine_comparison")
PROJECTIONS = ("float64", "float32", "frequency_wire_q16", "cost_int_reconstructed_frequency")
Group = dict[str, dict[str, float]]


@dataclass(frozen=True, slots=True)
class Entry:
    reading: str
    surface: str
    score: float
    lcid: int
    rcid: int
    mid: int


def parse_loudstxt3(data: bytes, source: str = "input", *,
                   max_cid: int = 65535, max_mid: int = 65535) -> Iterator[Entry]:
    """Decode validated LE offset/blob tables, retaining all finite score outliers.

    IDs are wire-format uint16, not translated POS IDs. Optional semantic bounds
    are caller supplied; this analysis makes no undocumented CID/MID assumption.
    Empty surface means the blob reading, as in the pinned official decoder.
    """
    if not 0 <= max_cid <= 65535 or not 0 <= max_mid <= 65535:
        raise ValueError("invalid ID bounds")
    if len(data) < 2:
        raise ValueError(f"{source}: truncated blob count")
    count = struct.unpack_from("<H", data)[0]
    header_end = 2 + 4 * count
    if header_end > len(data):
        raise ValueError(f"{source}: truncated offset table")
    if not count:
        if len(data) != 2:
            raise ValueError(f"{source}: data after empty offset table")
        return
    offsets = struct.unpack_from(f"<{count}I", data, 2)
    if offsets[0] != header_end:
        raise ValueError(f"{source}: first offset does not follow header")
    ends = (*offsets[1:], len(data))
    for node, (start, end) in enumerate(zip(offsets, ends)):
        location = f"{source}: blob {node}"
        if start < header_end or end > len(data) or end - start < 2:
            raise ValueError(f"{location}: offset bounds/order")
        entries = struct.unpack_from("<H", data, start)[0]
        text_start = start + 2 + entries * 10
        if text_start > end:
            raise ValueError(f"{location}: truncated entry/ID/float table")
        try:
            fields = data[text_start:end].decode("utf-8", errors="strict").split("\t")
        except UnicodeDecodeError as exc:
            raise ValueError(f"{location}: invalid UTF-8") from exc
        if len(fields) != entries + 1 or any("\0" in field for field in fields):
            raise ValueError(f"{location}: text field count or NUL")
        if not entries:
            if fields != [""]:
                raise ValueError(f"{location}: text after empty blob")
            continue
        reading = fields[0]
        if not reading:
            raise ValueError(f"{location}: empty reading")
        for index in range(entries):
            lcid, rcid, mid, score = struct.unpack_from("<HHHf", data, start + 2 + index * 10)
            if lcid > max_cid or rcid > max_cid or mid > max_mid:
                raise ValueError(f"{location}: ID outside supplied bounds")
            if not math.isfinite(score):
                raise ValueError(f"{location}: nonfinite score")
            yield Entry(reading, fields[index + 1] or reading, score, lcid, rcid, mid)


def normalize(reading: str) -> str:
    # Same key transformations as dictbuild.normalize, without importing builder.
    return "".join(chr(ord(c) - 0x60) if 0x30A1 <= ord(c) <= 0x30F6
                   else chr(ord(c) - 0xFEE0) if 0xFF01 <= ord(c) <= 0xFF5E else c
                   for c in reading)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def pinned_input(path: Path, url: str, expected: str, download: bool) -> dict:
    if not path.exists():
        if not download:
            raise ValueError(f"missing {path}; use --download explicitly")
        path.parent.mkdir(parents=True, exist_ok=True)
        partial = path.with_suffix(path.suffix + ".part")
        with urllib.request.urlopen(url, timeout=60) as response, partial.open("wb") as out:
            while block := response.read(1 << 20):
                out.write(block)
        if sha256(partial) != expected:
            raise ValueError(f"{partial}: downloaded SHA256 mismatch")
        partial.replace(path)
    actual = sha256(path)
    if actual != expected:
        raise ValueError(f"{path}: SHA256 mismatch")
    return {"url": url, "sha256": actual, "size_bytes": path.stat().st_size,
            "cache_path": str(path)}


def put(groups: Group, reading: str, surface: str, value: float) -> None:
    candidates = groups.setdefault(normalize(reading), {})
    candidates[surface] = max(value, candidates.get(surface, -math.inf))


def load_base(path: Path) -> tuple[Group, Counter, dict, list[dict]]:
    groups: Group = {}
    scores: Counter = Counter()
    counters = Counter()
    ids = {key: [65535, 0] for key in ("lcid", "rcid", "mid")}
    files = []
    with tarfile.open(path, "r:gz") as archive:
        members = sorted((m for m in archive.getmembers() if m.isfile()
                          and m.name.endswith(".loudstxt3")), key=lambda m: m.name)
        if not members:
            raise ValueError("archive has no .loudstxt3 files")
        licenses = [m for m in archive.getmembers() if m.isfile() and m.name.endswith("/LICENSE")]
        if len(licenses) != 1:
            raise ValueError("expected exactly one upstream LICENSE")
        license_text = archive.extractfile(licenses[0]).read().decode("utf-8")
        if "Copyright 2024 Miwa / ensan" not in license_text or "Apache License" not in license_text:
            raise ValueError("unexpected upstream license")
        for member in members:
            data = archive.extractfile(member).read()
            files.append({"path": member.name.split("/", 1)[1], "size_bytes": len(data),
                          "sha256": hashlib.sha256(data).hexdigest()})
            counters["blobs"] += struct.unpack_from("<H", data)[0]
            for entry in parse_loudstxt3(data, member.name):
                counters["entries"] += 1
                counters["surface_equals_reading"] += entry.surface == entry.reading
                scores[entry.score] += 1
                for key in ids:
                    value = getattr(entry, key)
                    ids[key][0] = min(ids[key][0], value)
                    ids[key][1] = max(ids[key][1], value)
                put(groups, entry.reading, entry.surface, entry.score)
            blob_count = struct.unpack_from("<H", data)[0]
            offsets = struct.unpack_from(f"<{blob_count}I", data, 2)
            counters["empty_blobs"] += sum(struct.unpack_from("<H", data, offset)[0] == 0 for offset in offsets)
    counters["files"] = len(files)
    counters["unique_normalized_readings"] = len(groups)
    counters["unique_reading_surface_pairs"] = sum(map(len, groups.values()))
    counters["multi_candidate_readings"] = sum(len(v) > 1 for v in groups.values())
    return groups, scores, {"counts": dict(counters), "wire_id_ranges": ids,
                           "id_semantic_mapping": "not assumed or evaluated",
                           "invalid_utf8_nonfinite_truncated_offset_errors": 0}, files


def quantiles(scores: Counter) -> dict:
    ordered = sorted(scores.items())
    total = sum(scores.values())
    result = {}
    for p in (0, .01, .05, .10, .25, .50, .75, .90, .95, .99, 1):
        target = max(1, math.ceil(p * total))
        accumulated = 0
        for score, count in ordered:
            accumulated += count
            if accumulated >= target:
                result[str(p)] = score
                break
    return result


def float32(value: float) -> float:
    return struct.unpack("<f", struct.pack("<f", value))[0]


def frequency_wire_q16(value: float) -> float:
    """Current .azdic frequency: round half up to uint16, decode to double.

    Source .loudstxt3 scores are float32; .azdic frequency is NOT float32.
    This reproduces dictbuild.py's writer and DoubleArrayTrie.cpp's reader.
    """
    if not math.isfinite(value) or not 0. <= value <= 1.:
        raise ValueError("frequency outside Q16 bounds")
    return int(value * 65535 + .5) / 65535.


def mapping_values(scores: Counter) -> tuple[dict, dict]:
    low, high = min(scores), max(scores)
    if low == high:
        raise ValueError("data-derived comparison needs a nonzero score range")
    functions = {
        # Existing builder fallback: cost 5000 -> (8000 - 5000) / 10000 = .3.
        "constant_comparison": lambda score: .3,
        "fixed_affine_comparison": lambda score: max(0., min(1., (score + 30.) / 30.)),
        "observed_affine_comparison": lambda score: .05 + .90 * (score - low) / (high - low),
    }
    values = {}
    for name, function in functions.items():
        raw = {score: function(score) for score in scores}
        values[name] = {
            "float64": raw,
            # Research reference only; production frequency uses uint16 below.
            "float32": {score: float32(value) for score, value in raw.items()},
            "frequency_wire_q16": {score: frequency_wire_q16(value) for score, value in raw.items()},
            # Illustrative inverse of the existing Sudachi cost->frequency formula.
            # Python ties-to-even rounding; this is NOT an approved Base cost rule.
            "cost_int_reconstructed_frequency": {
                score: (8000 - round(8000 - 10000 * value)) / 10000
                for score, value in raw.items()},
        }
    return values, {
        "constant_comparison": "frequency = 0.3 (existing builder cost=5000 fallback; comparison only)",
        "fixed_affine_comparison": "frequency = clamp((score + 30) / 30, 0, 1)",
        "observed_affine_comparison": f"frequency = 0.05 + 0.90 * (score - ({low})) / ({high} - ({low}))",
        "cost_projection": "cost = round(8000 - 10000 * frequency), ties to even; reconstructed frequency = (8000-cost)/10000; comparison only",
        "production_frequency_projection": "frequency_wire_q16 = int(frequency * 65535 + 0.5) / 65535.0 (half up, uint16 wire, double decode)",
        "float32_frequency_projection": "research reference only; source score is float32, production frequency is uint16",
        "q16_maximum_distinct_values": 65536,
        "observed_min": low, "observed_max": high,
        "observed_max_is_zero": high == 0.,
        "adoption_status": "NO formula, coefficient or production default is approved",
    }


def monotonicity(scores: Counter, mapped: dict[float, float]) -> dict:
    ordered = sorted(scores)
    target_counts = Counter()
    target_distinct = Counter(mapped.values())
    for score, count in scores.items():
        target_counts[mapped[score]] += count
    collapses = sum(mapped[a] == mapped[b] for a, b in zip(ordered, ordered[1:]))
    reversals = sum(mapped[a] > mapped[b] for a, b in zip(ordered, ordered[1:]))
    return {
        "weakly_monotonic_on_observed_scores": reversals == 0,
        "strictly_monotonic_on_distinct_observed_scores": collapses == 0 and reversals == 0,
        "adjacent_distinct_score_collapses": collapses, "reversals": reversals,
        "distinct_output_values": len(target_distinct),
        "new_tie_groups": sum(count > 1 for count in target_distinct.values()),
        "entries_in_new_tie_groups": sum(target_counts[v] for v, count in target_distinct.items() if count > 1),
        "output_quantiles": quantiles(target_counts),
    }


def ranking(candidates: dict[str, float], limit: int | None = None) -> list[str]:
    ordered = sorted(candidates, key=lambda surface: (-candidates[surface], surface))
    return ordered if limit is None else ordered[:limit]


def compare_readings(reference: Group, actual: Group) -> dict:
    counts = Counter()
    overlap_sum = displacement_sum = 0.
    multi_overlap_sum = 0.
    common_top_candidates = 0
    examples = []
    for reading in sorted(reference):
        before = ranking(reference[reading])
        after = ranking(actual.get(reading, {}))
        before5, after5 = before[:5], after[:5]
        counts["readings"] += 1
        counts["multi_candidate_readings"] += len(before) > 1
        counts["ordered_top5_changed"] += before5 != after5
        counts["top1_changed"] += before[:1] != after[:1]
        shared = set(before5) & set(after5)
        overlap_sum += len(shared) / len(before5)
        if len(before) > 1:
            multi_overlap_sum += len(shared) / len(before5)
        counts["top5_set_changed"] += set(before5) != set(after5)
        ranks = {surface: i for i, surface in enumerate(after)}
        for index, surface in enumerate(before5):
            if surface in ranks:
                displacement_sum += abs(index - ranks[surface])
                common_top_candidates += 1
        if before5 != after5 and len(examples) < 12:
            examples.append({"reading": reading, "reference_top5": before5, "actual_top5": after5})
    return {**dict(counts), "mean_top5_overlap_fraction": overlap_sum / len(reference) if reference else None,
            "mean_top5_overlap_fraction_multi_candidate_readings": multi_overlap_sum / counts["multi_candidate_readings"] if counts["multi_candidate_readings"] else None,
            "mean_reference_top5_rank_displacement": displacement_sum / common_top_candidates if common_top_candidates else None,
            "changed_examples": examples}


def project_groups(groups: Group, values: dict[float, float]) -> Group:
    return {reading: {surface: values[score] for surface, score in candidates.items()}
            for reading, candidates in groups.items()}


def load_sudachi(paths: list[Path]) -> tuple[Group, dict]:
    groups: Group = {}
    counts = Counter()
    for path in paths:
        for surface, reading, _pos, cost, _category in extract_sudachi.convert(extract_sudachi.read_lexicon(path)):
            # Match builder's signed-int16 cost bound and uint16 frequency storage.
            cost = max(-32768, min(32767, cost))
            put(groups, reading, surface, frequency_wire_q16(max(0., min(1., (8000 - cost) / 10000))))
            counts["filtered_entries"] += 1
    return groups, {**dict(counts), "unique_normalized_readings": len(groups),
                    "unique_reading_surface_pairs": sum(map(len, groups.values())),
                    "filter": "current extract_sudachi.convert, max_cost=10000",
                    "frequency": "frequency_wire_q16(clamp((8000 - signed_int16_cost) / 10000, 0, 1))"}


def merged_reading(base: dict[str, float], sudachi: dict[str, float],
                   mapped: dict[float, float]) -> tuple[dict[str, float], dict[str, str], Counter]:
    result = {surface: mapped[value] + BASE_PRIORITY for surface, value in base.items()}
    winners = {surface: "base" for surface in base}
    counts = Counter()
    for surface, frequency in sudachi.items():
        value = frequency + SUDACHI_PRIORITY
        if surface in result:
            counts["shared_pairs"] += 1
            # DictionaryStore iterates Base before Sudachi; equal Exact ties retain Base.
            winner = "sudachi" if value > result[surface] else "base"
            counts[f"dedup_{winner}_winner"] += 1
            counts["dedup_equal_score"] += value == result[surface]
        if surface not in result or value > result[surface]:
            result[surface] = value
            winners[surface] = "sudachi"
    return {surface: value + EXACT_BONUS for surface, value in result.items()}, winners, counts


def cross_layer(base: Group, sudachi: Group, values: dict) -> dict:
    readings = sorted(base.keys() & sudachi.keys())
    scenarios, metadata, dedup_winners = {}, {}, {}
    for name in MAPPINGS:
        groups: Group = {}
        counts = Counter()
        shared_winners = {}
        for reading in readings:
            merged, winners, dedup = merged_reading(base[reading], sudachi[reading], values[name]["frequency_wire_q16"])
            unrounded, unrounded_winners, _ = merged_reading(base[reading], sudachi[reading], values[name]["float64"])
            counts["q16_changes_ordered_top5_vs_unrounded_base_with_q16_sudachi"] += ranking(merged, 5) != ranking(unrounded, 5)
            for surface in base[reading].keys() & sudachi[reading].keys():
                shared_winners[(reading, surface)] = winners[surface]
                counts["q16_changes_dedup_winner_vs_unrounded_base_with_q16_sudachi"] += winners[surface] != unrounded_winners[surface]
            groups[reading] = merged
            counts.update(dedup)
            counts[f"top1_{winners[ranking(merged, 1)[0]]}"] += 1
            counts.update(f"top5_{winners[surface]}" for surface in ranking(merged, 5))
        scenarios[name] = groups
        metadata[name] = dict(counts)
        dedup_winners[name] = shared_winners
    for name in MAPPINGS:
        for reference in ("constant_comparison", "fixed_affine_comparison"):
            metadata[name][f"dedup_winner_changes_vs_{reference}"] = sum(
                winner != dedup_winners[reference][identity]
                for identity, winner in dedup_winners[name].items())
    return {"shared_normalized_readings": len(readings),
            "score": "decoded uint16 frequency / 65535.0 + layer priority + .10 Exact bonus; double arithmetic",
            "frequency_projection": "frequency_wire_q16 for both Base and Sudachi",
            "projection_comparison_reference": "Base frequency before quantization; Sudachi frequency remains Q16 in both arms",
            "layer_priority": {"base": BASE_PRIORITY, "sudachi": SUDACHI_PRIORITY},
            "category_bonus": 0, "obsolete_penalty": 0,
            "scenarios": {name: {**metadata[name],
                "versus_constant": compare_readings(scenarios["constant_comparison"], scenarios[name]),
                "versus_fixed_affine": compare_readings(scenarios["fixed_affine_comparison"], scenarios[name])}
                for name in MAPPINGS}}


def evaluate(path: Path, base: Group, sudachi: Group, values: dict) -> tuple[dict, list[dict]]:
    rows = [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines() if line.strip()]
    counts = {name: Counter() for name in MAPPINGS}
    details = []
    for row in rows:
        input_text = row.get("input", row.get("observed_reading"))
        expected_text = row.get("expected", row.get("expected_surface"))
        acceptable = row.get("acceptable", [])
        if (not isinstance(row.get("id"), str) or not isinstance(input_text, str)
                or not input_text or not isinstance(expected_text, str) or not expected_text
                or not isinstance(acceptable, list) or not all(isinstance(v, str) for v in acceptable)):
            raise ValueError(f"{path}: invalid evaluation row {row.get('id')}")
        reading = normalize(input_text)
        expected = {expected_text, *acceptable}
        for name in MAPPINGS:
            merged, _winners, _dedup = merged_reading(base.get(reading, {}), sudachi.get(reading, {}), values[name]["frequency_wire_q16"])
            ordered = ranking(merged)
            expected_rank = next((i + 1 for i, surface in enumerate(ordered) if surface in expected), None)
            counts[name]["rows"] += 1
            counts[name]["rows_with_dictionary_candidates"] += bool(ordered)
            counts[name]["expected_surface_in_dictionary"] += expected_rank is not None
            counts[name]["dictionary_only_top1_hits"] += expected_rank == 1
            counts[name]["dictionary_only_top5_hits"] += expected_rank is not None and expected_rank <= 5
            details.append({"id": row["id"], "domain": row.get("domain", ""), "input": input_text,
                            "input_field": "input" if "input" in row else "observed_reading",
                            "expected": expected_text, "mapping": name,
                            "frequency_projection": "frequency_wire_q16",
                            "expected_rank": expected_rank, "top5": ordered[:5]})
    return {"input_file": str(path), "sha256": sha256(path), "rows": len(rows),
            "frequency_projection": "frequency_wire_q16 for both Base and Sudachi",
            "meaning": "CPU dictionary-only exact lookup of input/observed_reading; contexts/profiles/models/segmentation/aliases/typo correction unused; NOT full conversion top5 quality acceptance",
            "scenarios": {name: {**dict(count),
                "dictionary_only_top5_hit_rate_all_rows": count["dictionary_only_top5_hits"] / len(rows) if rows else None}
                for name, count in counts.items()}}, details


def write_json(path: Path, value: dict) -> None:
    path.write_text(json.dumps(value, ensure_ascii=False, sort_keys=True, indent=2, allow_nan=False) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True, help="ignored analysis artifact directory")
    parser.add_argument("--download", action="store_true", help="explicitly permit pinned downloads into output/cache")
    parser.add_argument("--include-sudachi", action="store_true", help="compare the existing pinned bundled Sudachi scores")
    parser.add_argument("--eval", type=Path, action="append", default=[], help="authored JSONL; dictionary-only exact lookup")
    args = parser.parse_args()
    output = args.output.resolve()
    wall, cpu = time.perf_counter(), time.process_time()
    output.mkdir(parents=True, exist_ok=True)
    cache = output / "cache"
    base_path = cache / "azookey-storage-4d418525.tar.gz"
    try:
        inputs = {"base_archive": pinned_input(base_path, BASE_URL, BASE_SHA256, args.download)}
        base, scores, stats, files = load_base(base_path)
        values, formulas = mapping_values(scores)
        report = {"purpose": "opt-in G5 analysis only; no production integration or approved mapping",
                  "base": {**stats, "distinct_scores": len(scores), "score_quantiles": quantiles(scores),
                           "scores_below_minus30": sum(count for value, count in scores.items() if value < -30),
                           "scores_above_zero": sum(count for value, count in scores.items() if value > 0)},
                  "comparison_formulas": formulas,
                  "comparisons": {name: {projection: {
                      "monotonicity": monotonicity(scores, projected),
                      "base_reading_rank": compare_readings(base, project_groups(base, projected))}
                      for projection, projected in projections.items()}
                      for name, projections in values.items()}}
        sudachi: Group = {}
        if args.include_sudachi:
            paths = []
            inputs["sudachi_archives"] = []
            for name, digest in SUDACHI_PINS:
                url = f"https://sudachi.s3.ap-northeast-1.amazonaws.com/sudachidict-raw/{SUDACHI_REVISION}/{name}"
                path = cache / f"sudachi-{SUDACHI_REVISION}" / name
                inputs["sudachi_archives"].append(pinned_input(path, url, digest, args.download))
                paths.append(path)
            sudachi, report["sudachi"] = load_sudachi(paths)
            report["cross_layer"] = cross_layer(base, sudachi, values)
        report["evaluation"] = []
        evaluation_rows = []
        for path in args.eval:
            summary, details = evaluate(path.resolve(), base, sudachi, values)
            report["evaluation"].append(summary)
            evaluation_rows.extend({"dataset": path.name, **row} for row in details)
        report["limitations"] = [
            "All formulas and endpoints are illustrative comparisons, not production decisions.",
            "Raw score is a frequency proxy; finite outliers are retained, not rejected.",
            "Source scores are float32; production .azdic frequencies are uint16 with double decode. Float32 frequency is a research comparison only.",
            "76004 distinct source scores exceed Q16's 65536 possible values; strict global rank separation cannot survive production storage.",
            "CID/MID numeric ranges are observed only; POS mapping and connection costs are not evaluated.",
            "Ranking deduplicates normalized reading + surface by greatest score; equal ties sort by surface.",
            "Cross-layer evaluation is Base + current filtered Sudachi only, Exact matches and default category boosts.",
            "Dictionary-only JSONL hits are not full model/IME top5 quality or performance acceptance.",
            "Docs/dicdata_format.md has TBW for binary layout; the pinned extension LOUDS.swift is the format evidence.",
        ]
        manifest = {"inputs": inputs, "base_files": files,
                    "base_storage_revision": STORAGE_REVISION, "converter_format_revision": CONVERTER_REVISION,
                    "format_sources": [f"https://github.com/azooKey/AzooKeyKanaKanjiConverter/blob/{CONVERTER_REVISION}/Docs/dicdata_format.md",
                        f"https://github.com/azooKey/AzooKeyKanaKanjiConverter/blob/{CONVERTER_REVISION}/Sources/KanaKanjiConverterModule/DictionaryManagement/DataStructure/extension%20LOUDS.swift"],
                    "base_license": "Apache-2.0; Copyright 2024 Miwa / ensan; upstream LICENSE retained in source archive; no upstream NOTICE",
                    "command": [sys.executable, *sys.argv],
                    "analysis_script_sha256": sha256(Path(__file__))}
        write_json(output / "input-manifest.json", manifest)
        write_json(output / "score-analysis.json", report)
        if evaluation_rows:
            with (output / "evaluation-candidates.tsv").open("w", encoding="utf-8", newline="") as stream:
                writer = csv.DictWriter(stream, fieldnames=list(evaluation_rows[0]), delimiter="\t")
                writer.writeheader()
                for row in evaluation_rows:
                    writer.writerow({**row, "top5": json.dumps(row["top5"], ensure_ascii=False)})
        write_json(output / "cpu-analysis-timing.json", {"wall_seconds": time.perf_counter() - wall,
                   "process_cpu_seconds": time.process_time() - cpu,
                   "meaning": "one CPU analysis run, includes parsing/I/O; not a production benchmark or acceptance result"})
    except (OSError, ValueError, tarfile.TarError, csv.Error) as exc:
        print(f"analyze_base_scores: {exc}", file=sys.stderr)
        return 1
    print(f"analysis only: {output / 'score-analysis.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
