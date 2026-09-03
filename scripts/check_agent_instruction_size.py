#!/usr/bin/env python3
"""Enforce the AGENTS.md byte budget defined by docs/dev-infrastructure-spec.md §4.3."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


TARGET_BYTES = 12 * 1024
MAX_BYTES = 16 * 1024


def classify(size: int, target: int = TARGET_BYTES, maximum: int = MAX_BYTES) -> str:
    if size > maximum:
        return "error"
    if size > target:
        return "warning"
    return "ok"


def default_agents_path() -> Path:
    return Path(__file__).resolve().parents[1] / "AGENTS.md"


def github_escape(value: object, *, property_value: bool = False) -> str:
    escaped = str(value).replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")
    if property_value:
        escaped = escaped.replace(":", "%3A").replace(",", "%2C")
    return escaped


def warning_annotation(path: Path, size: int, target: int, maximum: int) -> str:
    try:
        annotation_path = path.resolve().relative_to(Path.cwd().resolve())
    except ValueError:
        annotation_path = path
    location = github_escape(annotation_path, property_value=True)
    message = github_escape(
        f"AGENTS.md is {size} bytes; target is {target} bytes and maximum is {maximum} bytes"
    )
    return f"::warning file={location},title=Agent instruction budget::{message}"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", nargs="?", type=Path, default=default_agents_path())
    parser.add_argument("--target", type=int, default=TARGET_BYTES)
    parser.add_argument("--max", dest="maximum", type=int, default=MAX_BYTES)
    parser.add_argument("--json", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.target < 0 or args.maximum < args.target:
        raise SystemExit("target must be non-negative and max must be >= target")
    try:
        size = args.path.stat().st_size
    except OSError as exc:
        print(f"instruction-size: error path={args.path} reason={exc}")
        return 2

    status = classify(size, args.target, args.maximum)
    result = {
        "status": status,
        "path": str(args.path),
        "bytes": size,
        "target": args.target,
        "max": args.maximum,
    }
    if args.json:
        print(json.dumps(result, ensure_ascii=False, separators=(",", ":")))
    else:
        print("instruction-size: " + " ".join(f"{key}={value}" for key, value in result.items()))
        if status == "warning":
            print(warning_annotation(args.path, size, args.target, args.maximum))
    return 1 if status == "error" else 0


if __name__ == "__main__":
    raise SystemExit(main())
