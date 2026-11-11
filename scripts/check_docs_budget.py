#!/usr/bin/env python3
"""Warn when a spec grows large, and pin the docs-lint baseline to a ceiling.

Two independent checks share this script.

Size budget (warning only)
  A spec (`docs/*-spec.md`) of `SPEC_LINE_WARN` lines or more, and a `## `
  section of `SECTION_LINE_WARN` lines or more, are reported. The report never
  changes the exit status: it makes growth visible early, it does not decide
  where a document is split. On GitHub Actions the report is emitted as
  `::warning` annotations as well.

Baseline cap (fails)
  `.docs-lint-baseline.json` freezes the docs-lint findings that existed when
  the baseline was taken, and docs-lint itself only stops an *increase*. That
  leaves a gap: once a finding is fixed, the number can creep back up to the old
  value. `scripts/docs-baseline-cap.json` holds the ceiling, in the same shape
  as the baseline. The two must be equal:
    - a count above the cap, or a file or category missing from the cap, fails
      (the baseline was raised);
    - a count below the cap, or a cap entry the baseline no longer has, fails
      too, and the message asks for the cap to be lowered to the new value, so
      that the reduction cannot be given back later.
  Zero counts are treated as absent.

The size check and the cap check read only the repository, so the script needs
no argument. `--root` is for tests.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import sys

SPEC_LINE_WARN = 1500
SECTION_LINE_WARN = 400

BASELINE = ".docs-lint-baseline.json"
CAP = "scripts/docs-baseline-cap.json"


def size_warnings(root: Path) -> list[tuple[str, str]]:
    """(relative path, message) for each spec or section over its budget."""
    out: list[tuple[str, str]] = []
    for path in sorted((root / "docs").glob("*-spec.md")):
        rel = path.relative_to(root).as_posix()
        lines = path.read_text(encoding="utf-8").splitlines()
        if len(lines) >= SPEC_LINE_WARN:
            out.append((rel, f"{rel}: {len(lines)} 行（警告の閾値 {SPEC_LINE_WARN} 行）。分割を検討してください"))
        heads = [i for i, l in enumerate(lines) if re.match(r"## ", l)] + [len(lines)]
        for start, end in zip(heads, heads[1:]):
            if end - start >= SECTION_LINE_WARN:
                out.append(
                    (rel, f"{rel}:{start + 1}: 節「{lines[start][3:].strip()}」が {end - start} 行"
                          f"（警告の閾値 {SECTION_LINE_WARN} 行）")
                )
    return out


def load_counts(path: Path) -> dict[str, dict[str, int]]:
    data = json.loads(path.read_text(encoding="utf-8"))
    counts: dict[str, dict[str, int]] = {}
    for category, files in data.items():
        if not isinstance(files, dict):
            raise ValueError(f"{path.name}: {category} がオブジェクトではありません")
        kept = {f: int(n) for f, n in files.items() if int(n) != 0}
        if kept:
            counts[category] = kept
    return counts


def cap_problems(root: Path) -> list[str]:
    baseline_path, cap_path = root / BASELINE, root / CAP
    for path in (baseline_path, cap_path):
        if not path.is_file():
            return [f"{path.relative_to(root).as_posix()}: 存在しません"]
    try:
        baseline, cap = load_counts(baseline_path), load_counts(cap_path)
    except (ValueError, json.JSONDecodeError) as err:
        return [f"JSON を読めません: {err}"]
    problems: list[str] = []
    for category in sorted(set(baseline) | set(cap)):
        have, ceiling = baseline.get(category, {}), cap.get(category, {})
        for name in sorted(set(have) | set(ceiling)):
            now, top = have.get(name, 0), ceiling.get(name, 0)
            if now > top:
                problems.append(
                    f"{BASELINE}: {category} の {name} が {now} 件で、上限 {top} 件（{CAP}）を超えています。"
                    "ベースラインを増やさず、docs の指摘を直してください"
                )
            elif now < top:
                problems.append(
                    f"{BASELINE}: {category} の {name} が {now} 件に減っています。"
                    f"{CAP} の上限を {now} 件へ下げてください（下げた分を戻さないため）"
                )
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args(argv)
    annotate = os.environ.get("GITHUB_ACTIONS") == "true"
    for rel, message in size_warnings(args.root):
        print(f"::warning file={rel}::{message}" if annotate else f"警告: {message}")
    problems = cap_problems(args.root)
    if problems:
        print("docs-lint ベースラインの上限が守られていません:", file=sys.stderr)
        for problem in problems:
            print(f"  - {problem}", file=sys.stderr)
        return 1
    print("docs-lint ベースラインは上限と一致しています。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
