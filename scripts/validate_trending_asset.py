"""Validate an unpublished trending asset and optionally write its SHA256 sidecar.

Matches docs/auto-word-registration-spec.md section 5-3 and TrendingWordFetcher.cpp.
Only runtime format is checked: this does not establish provenance, licensing,
editorial quality, publication readiness, or availability at a production URL.
The input is never rewritten; the digest covers its original bytes.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import sys
import tempfile


MAX_ASSET_BYTES = 1024 * 1024
MAX_JSON_DEPTH = 64
MAX_WORD_BYTES = 256
MAX_WORDS = 10_000


class AssetError(ValueError):
    """The asset does not satisfy the runtime input contract."""


class _Number:
    """Keep the token, as ipc::json::Number does, instead of treating bool as int."""

    def __init__(self, token: str):
        self.token = token
        self.value = float(token)
        if not math.isfinite(self.value):
            raise AssetError("JSON contains a non-finite number")
        # Json.cpp accepts underflow only when adjusted exponent is below -324.
        significand, _, exponent = token.lower().partition("e")
        digits = significand.lstrip("-").replace(".", "")
        nonzero = next((i for i, digit in enumerate(digits) if digit != "0"), None)
        if self.value == 0.0 and nonzero is not None:
            exponent_digits = exponent.lstrip("+-").lstrip("0")
            magnitude = int(exponent_digits or "0") if len(exponent_digits) < 5 else 10_000
            explicit = -magnitude if exponent.startswith("-") else magnitude
            before_dot = len(significand.lstrip("-").split(".")[0])
            if explicit + before_dot - nonzero - 1 >= -324:
                raise AssetError("JSON number underflow is outside the runtime parser contract")

    def uint(self) -> int | None:
        if not self.token.startswith("-") and self.token.isdecimal():
            if len(self.token) > 20:
                return None
            value = int(self.token)
            return value if value < 2**64 else None
        if self.value < 0 or self.value >= 2**64 or not self.value.is_integer():
            return None
        return int(self.value)


class _Object(dict):
    def __init__(self, pairs):
        super().__init__()
        self.pairs = pairs
        # Json.cpp uses map::emplace: the first duplicate key wins, but every
        # value is still parsed and validated, including otherwise unused ones.
        for key, value in pairs:
            self.setdefault(key, value)


def _reject_constant(_token: str):
    raise AssetError("JSON contains a non-finite number")


def _check_json_tree(value, depth: int = 0) -> None:
    if isinstance(value, str):
        value.encode("utf-8", errors="strict")  # Reject escaped lone surrogates too.
    elif isinstance(value, (_Object, list)):
        depth += 1
        if depth > MAX_JSON_DEPTH:
            raise AssetError("JSON nesting exceeds 64 containers")
        if isinstance(value, _Object):
            for key, member in value.pairs:
                _check_json_tree(key, depth)
                _check_json_tree(member, depth)
        else:
            for member in value:
                _check_json_tree(member, depth)


def _uint(value) -> int | None:
    return value.uint() if isinstance(value, _Number) else None


def _valid_word(value, reading: bool) -> bool:
    if not isinstance(value, str) or not 1 <= len(value.encode("utf-8")) <= MAX_WORD_BYTES:
        return False
    for char in value:
        cp = ord(char)
        if (cp <= 0x1F or 0x7F <= cp <= 0x9F or cp == 0x61C
                or 0x200E <= cp <= 0x200F or 0x202A <= cp <= 0x202E
                or 0x2066 <= cp <= 0x2069):
            return False
        if reading and not (0x3041 <= cp <= 0x309F or 0x30A0 <= cp <= 0x30FF):
            return False
    return True


def validate_asset_bytes(data: bytes) -> dict:
    """Return runtime validation counts and a digest of exactly data, or raise AssetError."""
    if not 1 <= len(data) <= MAX_ASSET_BYTES:
        raise AssetError("asset must contain 1 to 1048576 bytes")
    try:
        root = json.loads(data.decode("utf-8", errors="strict"), object_pairs_hook=_Object,
                          parse_int=_Number, parse_float=_Number,
                          parse_constant=_reject_constant)
        _check_json_tree(root)
    except (ValueError, UnicodeError, RecursionError) as exc:
        # Do not echo input content: diagnostics are suitable for release logs.
        raise AssetError("invalid JSON, UTF-8, numeric value, or nesting") from exc
    if not isinstance(root, _Object) or _uint(root.get("version")) != 1:
        raise AssetError("version must be numeric 1 in a JSON object")
    generated = root.get("generated_at")
    if not isinstance(generated, str) or not 1 <= len(generated.encode("utf-8")) <= 64:
        raise AssetError("generated_at must be a nonempty string of at most 64 UTF-8 bytes")
    words = root.get("words")
    if not isinstance(words, list) or len(words) > MAX_WORDS:
        raise AssetError("words must be an array of at most 10000 entries")
    accepted = 0
    for index, word in enumerate(words):
        if not isinstance(word, _Object):
            raise AssetError(f"words[{index}] must be an object")
        if "reading" in word and not isinstance(word["reading"], str):
            raise AssetError(f"words[{index}].reading must be a string when present")
        if not word.get("reading"):
            continue
        rank = _uint(word.get("rank"))
        if (not _valid_word(word.get("surface"), False)
                or not _valid_word(word["reading"], True)
                or rank is None or not 1 <= rank <= 2**32 - 1):
            raise AssetError(f"words[{index}] has an invalid surface, reading, or rank")
        accepted += 1
    return {"entries": len(words), "accepted": accepted, "skipped": len(words) - accepted,
            "sha256": hashlib.sha256(data).hexdigest()}


def validate_file(source: Path, checksum_out: Path | None = None) -> dict:
    """Validate before creating output; atomically replace a requested sidecar on success."""
    with source.open("rb") as stream:
        report = validate_asset_bytes(stream.read(MAX_ASSET_BYTES + 1))
    if checksum_out is not None:
        if (source.resolve() == checksum_out.resolve()
                or (checksum_out.exists() and source.samefile(checksum_out))):
            raise AssetError("checksum output must not overwrite the input asset")
        sidecar = (report["sha256"] + "  trending-words.json\n").encode("ascii")
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(mode="wb", dir=checksum_out.parent,
                                             prefix=".trending-checksum-", delete=False) as stream:
                temporary = Path(stream.name)
                stream.write(sidecar)
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temporary, checksum_out)
        finally:
            if temporary is not None:
                temporary.unlink(missing_ok=True)
    return report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="unpublished JSON asset to validate without rewriting")
    parser.add_argument("--checksum-out", type=Path,
                        help="atomically write a sidecar for trending-words.json after validation")
    args = parser.parse_args(argv)
    try:
        report = validate_file(args.input, args.checksum_out)
    except (AssetError, OSError) as exc:
        print(f"Validation failed: {exc}", file=sys.stderr)
        return 1
    print(f"Runtime format valid: entries={report['entries']}, accepted={report['accepted']}, "
          f"skipped={report['skipped']}; SHA256={report['sha256']}")
    if report["accepted"] == 0:
        print("No ingestible words: this asset does not provide a trending vocabulary.")
    print("Publication readiness is not assessed: provenance, licensing, content review, "
          "and production availability require separate verification.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
