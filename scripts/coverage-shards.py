#!/usr/bin/env python3
"""Plan deterministic CTest shards and check their execution and partition."""

import argparse
import json
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET


def names(value, label):
    if not isinstance(value, list) or not value:
        raise ValueError(f"{label} must be a nonempty list")
    if any(not isinstance(name, str) or not name.strip()
           or any(char in name for char in "\r\n\0") for name in value):
        raise ValueError(f"{label} contains an invalid test name")
    if len(set(value)) != len(value):
        raise ValueError(f"{label} contains duplicate test names")
    return value


def parameters(shard, count):
    if type(count) is not int or count < 1:
        raise ValueError("count must be a positive integer")
    if type(shard) is not int or not 1 <= shard <= count:
        raise ValueError("shard must be between 1 and count")


def read_json(path):
    with Path(path).open(encoding="utf-8-sig") as source:
        return json.load(source)


def read_manifest(path):
    manifest = read_json(path)
    if (not isinstance(manifest, dict) or type(manifest.get("version")) is not int
            or manifest["version"] != 1):
        raise ValueError("unsupported manifest version")
    parameters(manifest.get("shard"), manifest.get("count"))
    expected = names(manifest.get("expected"), "expected")
    selected = names(manifest.get("selected"), "selected")
    if expected != sorted(expected):
        raise ValueError("expected inventory must be sorted")
    if selected != expected[manifest["shard"] - 1::manifest["count"]]:
        raise ValueError("selected tests do not match the deterministic partition")
    return manifest


def plan(ctest_json, shard, count, output_dir):
    parameters(shard, count)
    inventory = read_json(ctest_json)
    if not isinstance(inventory, dict) or not isinstance(inventory.get("tests"), list):
        raise ValueError("CTest inventory must contain a tests array")
    tests = inventory["tests"]
    if any(not isinstance(test, dict) for test in tests):
        raise ValueError("CTest inventory contains an invalid test")
    expected = sorted(names([test.get("name") for test in tests], "CTest inventory"))
    if len(expected) < count:
        raise ValueError("test count must be at least shard count")
    selected = expected[shard - 1::count]
    manifest = dict(version=1, shard=shard, count=count,
                    expected=expected, selected=selected)
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    (output_dir / "tests.txt").write_text("\n".join(selected) + "\n", encoding="utf-8")
    (output_dir / "manifest.json").write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def verify_run(manifest_path, junit_path):
    manifest = read_manifest(manifest_path)
    root = ET.parse(junit_path).getroot()
    if root.tag not in ("testsuite", "testsuites"):
        raise ValueError("JUnit root must be testsuite or testsuites")
    cases = list(root.iter("testcase"))
    actual = names([case.get("name") for case in cases], "JUnit")
    for case in cases:
        skipped = case.findall("skipped")
        # CTest uses notrun for both intentional skips and execution problems.
        # Only these CompletionStatus values indicate a test ran and chose to skip.
        legitimate_skip = (
            case.get("status") == "notrun" and len(skipped) == 1
            and re.fullmatch(r"SKIP_REGULAR_EXPRESSION_MATCHED|SKIP_RETURN_CODE=[0-9]+",
                             skipped[0].get("message", "")) is not None
        )
        if (case.find("failure") is not None or case.find("error") is not None
                or case.get("status") in ("fail", "failed", "error", "disabled")
                or ((case.get("status") == "notrun" or skipped) and not legitimate_skip)):
            raise ValueError(f"JUnit test did not pass: {case.get('name')}")
    selected = set(manifest["selected"])
    if set(actual) != selected:
        raise ValueError(f"JUnit tests differ: missing={sorted(selected - set(actual))}, "
                         f"unexpected={sorted(set(actual) - selected)}")


def verify_all(root, count):
    parameters(1, count)
    expected = None
    seen = set()
    for shard in range(1, count + 1):
        manifest = read_manifest(Path(root) / f"shard-{shard}" / "manifest.json")
        if manifest["shard"] != shard or manifest["count"] != count:
            raise ValueError(f"shard-{shard} has incorrect shard/count")
        if expected is None:
            expected = manifest["expected"]
        elif manifest["expected"] != expected:
            raise ValueError("shards have different expected inventories")
        selected = set(manifest["selected"])
        if seen & selected:
            raise ValueError("shards overlap")
        seen.update(selected)
    if seen != set(expected):
        raise ValueError("shards do not cover the expected inventory")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    command = commands.add_parser("plan")
    command.add_argument("--ctest-json", required=True, type=Path)
    command.add_argument("--shard", required=True, type=int)
    command.add_argument("--count", required=True, type=int)
    command.add_argument("--output-dir", required=True, type=Path)
    command = commands.add_parser("verify-run")
    command.add_argument("--manifest", required=True, type=Path)
    command.add_argument("--junit", required=True, type=Path)
    command = commands.add_parser("verify-all")
    command.add_argument("--root", required=True, type=Path)
    command.add_argument("--count", required=True, type=int)
    args = parser.parse_args(argv)
    try:
        if args.command == "plan":
            plan(args.ctest_json, args.shard, args.count, args.output_dir)
        elif args.command == "verify-run":
            verify_run(args.manifest, args.junit)
        else:
            verify_all(args.root, args.count)
    except (ValueError, OSError, ET.ParseError) as error:
        parser.exit(1, f"coverage-shards: {error}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
