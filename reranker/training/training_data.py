"""Build labeled exposures from offline queries and candidate features."""

from __future__ import annotations

from collections import Counter, defaultdict
from collections.abc import Iterable, Iterator
from dataclasses import dataclass
import hashlib
import json
import math
from pathlib import Path
from typing import TypedDict
import unicodedata

from training_corpus import TrainingError, json_bytes, sha256_file, to_hiragana


FEATURES = (
    "zenzai_score", "dictionary_score", "user_frequency", "recency_score", "typo_confidence",
    "app_profile_score", "candidate_length", "segment_count", "is_named_entity", "is_user_dict",
    "is_neologism", "is_typo_corrected",
)
SHOWN_LIMIT = 9
NEGATIVE_LIMIT = 8


def write_jsonl(path: Path, rows: Iterable[object]) -> None:
    with path.open("wb") as stream:
        for row in rows:
            stream.write(json_bytes(row))


def read_jsonl(path: Path) -> Iterator[dict[str, object]]:
    with path.open("r", encoding="utf-8") as stream:
        for number, line in enumerate(stream, 1):
            try:
                row = json.loads(line)
            except ValueError as exc:
                raise TrainingError(f"invalid JSON: {path.name}:{number}") from exc
            if not isinstance(row, dict):
                raise TrainingError(f"expected an object: {path.name}:{number}")
            yield row


def field(row: dict[str, object], key: str, *, allow_empty: bool = False) -> str:
    value = row.get(key)
    if not isinstance(value, str) or (not allow_empty and not value.strip()):
        raise TrainingError(f"invalid text field: {key}")
    return value


def normalize(text: str) -> str:
    return " ".join(unicodedata.normalize("NFKC", text).split())


def _reading_key(text: str) -> str:
    return to_hiragana(normalize(text)).casefold()


def _fingerprint(kind: str, *parts: str) -> str:
    return hashlib.sha256(json_bytes([kind, *parts])).hexdigest()


def exclusion_keys(context: str, readings: Iterable[str], surfaces: Iterable[str]) -> set[str]:
    left = normalize(context)
    surface_keys = list(map(normalize, surfaces))
    keys: set[str] = set()
    for reading in readings:
        kana = _reading_key(reading)
        if left:
            keys.add(_fingerprint("context-reading", left, kana))
        keys.update(_fingerprint("reading-surface", kana, surface) for surface in surface_keys)
    return keys


def _text_list(value: object, name: str) -> list[str]:
    if not isinstance(value, list) or any(not isinstance(s, str) or not s.strip() for s in value):
        raise TrainingError(f"invalid text list: {name}")
    return value


class Exclusions(TypedDict):
    schema: int
    datasets: dict[str, str]
    keys: list[str]


def build_exclusions(directory: Path) -> Exclusions:
    files = sorted(directory.glob("*.jsonl"))
    if not {"kana_kanji_eval.jsonl", "typo_eval.jsonl"} <= {file.name for file in files}:
        raise TrainingError("M52 directory must contain the kana-kanji and typo eval sets")
    datasets = {}
    keys: set[str] = set()
    for path in files:
        datasets[path.name] = sha256_file(path)
        for row in read_jsonl(path):
            if "expected" in row:
                context = field(row, "left_context", allow_empty=True)
                readings = [field(row, "input")]
                surfaces = [field(row, "expected"), *_text_list(row.get("acceptable", []), "acceptable")]
            elif "expected_surface" in row:
                context = field(row, "left_context", allow_empty=True)
                readings = [field(row, "observed_reading"), field(row, "intended_reading")]
                if row.get("raw_keys"):
                    readings.append(field(row, "raw_keys"))
                surfaces = [field(row, "expected_surface")]
            elif "expected_english" in row:
                context = ""
                readings = [field(row, "raw_romaji")]
                if "reading" in row:
                    readings.append(field(row, "reading"))
                segments = row["expected_english"]
                if not isinstance(segments, list) or any(not isinstance(segment, dict) for segment in segments):
                    raise TrainingError("invalid mixed-English expected segments")
                surfaces = [field(segment, "surface") for segment in segments]
                keys.update(_fingerprint("mixed-reading", _reading_key(reading)) for reading in readings)
            else:
                raise TrainingError(f"unsupported evaluation case in {path.name}")
            keys.update(exclusion_keys(context, readings, surfaces))
        if sha256_file(path) != datasets[path.name]:
            raise TrainingError(f"M52 evaluation file changed during scanning: {path.name}")
    if not keys:
        raise TrainingError("M52 exclusion manifest would be empty")
    return {"schema": 1, "datasets": datasets, "keys": sorted(keys)}


def feature_vector(value: object, surface: str | None = None) -> tuple[float, ...]:
    if not isinstance(value, list) or len(value) != len(FEATURES) or any(
        not isinstance(number, (int, float)) or isinstance(number, bool)
        or abs(number) > 3.4028234663852886e38 or not math.isfinite(number) for number in value
    ):
        raise TrainingError("expected 12 finite fp32 features")
    vector = tuple(float(number) for number in value)
    if vector[0] > 0 or not 0 <= vector[4] <= 1 or vector[5] != 0:
        raise TrainingError("invalid Zenzai, typo, or app-profile feature")
    if any(not number.is_integer() or number < 1 for number in vector[6:8]):
        raise TrainingError("invalid candidate length or segment count")
    if surface is not None and vector[6] != len(surface):
        raise TrainingError("candidate length must count Unicode code points")
    if any(flag not in (0, 1) for flag in vector[8:]) or (not vector[11] and vector[4] != 0):
        raise TrainingError("invalid feature flags or non-corrected typo confidence")
    return vector


@dataclass(frozen=True)
class Example:
    work: str
    features: tuple[float, ...]
    label: int
    weight: float


class Label(TypedDict):
    features: list[float]
    label: int
    weight: float
    occurrences: int
    split: str


def synthesize(
    queries: Iterable[dict[str, object]], batches: Iterable[dict[str, object]], exclusions: Exclusions,
) -> tuple[list[Label], dict[str, int]]:
    indexed: dict[str, dict[str, object]] = {}
    for candidate_batch in batches:
        query_id = field(candidate_batch, "id")
        if query_id in indexed:
            raise TrainingError("duplicate candidate batch id")
        indexed[query_id] = candidate_batch
    denied = set(exclusions["keys"])
    counts = Counter({"queries": 0, "chosen_not_shown": 0, "tied_labels": 0, "no_negatives": 0})
    groups: dict[tuple[str, str, str], list[Example]] = defaultdict(list)
    for query in sorted(queries, key=lambda item: field(item, "id")):
        counts["queries"] += 1
        batch = indexed.pop(field(query, "id"), None)
        if batch is None or not isinstance(batch.get("candidates"), list):
            raise TrainingError("a query is missing its unique candidate batch")
        work = field(query, "work")
        context = field(query, "left_context", allow_empty=True)
        reading = field(query, "input")
        chosen = field(query, "chosen")
        shown: dict[str, tuple[float, ...]] = {}
        candidates = batch["candidates"]
        assert isinstance(candidates, list)
        for candidate in candidates[:SHOWN_LIMIT]:
            if not isinstance(candidate, dict):
                raise TrainingError("candidate must be an object")
            surface = field(candidate, "surface")
            vector = feature_vector(candidate.get("features"), surface)
            shown.setdefault(surface, vector)
        if (_fingerprint("mixed-reading", _reading_key(reading)) in denied
            or denied & exclusion_keys(context, [reading], [chosen, *shown])):
            raise TrainingError("overlap with M52 evaluation data")
        if chosen not in shown:
            counts["chosen_not_shown"] += 1
            continue
        negatives = [surface for surface in shown if surface != chosen][:NEGATIVE_LIMIT]
        if not negatives:
            counts["no_negatives"] += 1
            continue
        for surface, label, weight in [
            (chosen, 1, 1.0), *((surface, 0, 1.0 / len(negatives)) for surface in negatives),
        ]:
            key = (normalize(context), reading, surface)
            groups[key].append(Example(work, shown[surface], label, weight))
    if indexed:
        raise TrainingError("candidate batches contain ids without a query")

    works = sorted({row.work for rows in groups.values() for row in rows}, key=lambda work: (
        _fingerprint("work-split", work), work,
    ))
    if len(works) < 2:
        raise TrainingError("synthesis needs usable exposures from at least two works")
    holdout = set(works[:max(1, len(works) // 5)])
    split_for_query: dict[tuple[str, str], str] = {}
    labels: list[Label] = []
    for (context, reading, _), rows in sorted(groups.items()):
        splits = {"holdout" if row.work in holdout else "train" for row in rows}
        split = next(iter(splits))
        if len(splits) != 1 or split_for_query.setdefault((context, reading), split) != split:
            raise TrainingError("duplicate conversion opportunity spans train and holdout works")
        votes = sum(row.label for row in rows)
        if votes * 2 == len(rows):
            counts["tied_labels"] += 1
            continue
        label = int(votes * 2 > len(rows))
        agreed = [row for row in rows if row.label == label]
        labels.append({
            "features": list(agreed[0].features), "label": label, "split": split,
            "weight": math.fsum(row.weight for row in agreed) / len(agreed), "occurrences": len(rows),
        })
    return labels, dict(counts)


def fixture_rows() -> Iterator[tuple[dict[str, object], dict[str, object]]]:
    """Artificial examples exercising both choice outcomes without corpus data."""
    for work in range(5):
        for number in range(12):
            query_id = f"fixture-{work}-{number}"
            good = "架空甲" if number % 2 else "架空乙"
            surfaces = [good, "架空乙" if good == "架空甲" else "架空甲", "架空丙"]
            candidates = []
            for index, surface in enumerate(surfaces):
                corrected = int((index + number) % 5 == 0)
                features = [
                    -10.0 if (index + number) % 7 == 0 else -0.4 - index - number * 0.02,
                    0.8 - index * 0.2, number * 0.03, 0.5 / (1 + index),
                    0.85 if corrected else 0.0, 0.0, len(surface), 1 + (number % 3),
                    int(index == 0 and number % 2 == 0), int(index == 1 and number % 4 == 0),
                    int(index == 2 and number % 3 == 0), corrected,
                ]
                candidates.append({"surface": surface, "features": features})
            if number % 2 == 0:
                candidates.reverse()
            yield (
                {"id": query_id, "work": f"fixture-{work}", "left_context": f"合成 {work}-{number}",
                 "input": "ふぃくすちゃ", "chosen": good},
                {"id": query_id, "candidates": candidates},
            )
