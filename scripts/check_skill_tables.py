#!/usr/bin/env python3
"""Check hand-written Skill / agent lists against the CI files they summarise.

`check_skill_references.py` only looks at names a Skill already mentions. The
drift found in DEV-1990 had the opposite shape: every named item existed, but
something newly added to CI was missing from the list. This script reads both
sides and fails when they disagree. Like `check_doc_contracts.py` it only
*checks*; the lists carry prose that no generator can derive.

Two lists are checked.

windows.yml jobs
  List: the table under the heading「windows.yml の job」in
  `references/spec-routing.md` of the Skill `azookey-ci-workflows`, under both
  `.claude/skills/` and `.agents/skills/`. A row is
  `| `job-id` | 必須 or advisory | 内容 |`.
  Source of truth: the keys of `jobs:` in `.github/workflows/windows.yml` and
  the `needs` of its `ci-gate` job.
    - A row naming a job that `windows.yml` does not have fails.
    - A job of `windows.yml` that is neither in the table nor in
      `EXCLUDED_WORKFLOW_JOBS` fails.
    - `必須` must be exactly the jobs in `ci-gate.needs`; any other row must say
      `advisory`. A job added to `needs` but still marked `advisory` (or the
      reverse) fails, which is the sync the Skill lacked before.

quality-gate-runner gate table
  List: the body of `.claude/agents/quality-gate-runner.md` and
  `.codex/agents/quality-gate-runner.toml`, which name the checks by
  `python3 scripts/<name>.py`.
  Source of truth: the commands `.github/workflows/docs.yml` runs. A script
  directly under `scripts/` (not `scripts/tests/`) is a checker.
    - A checker that `docs.yml` runs but a definition never names fails
      (`GATE_EXCLUDED_CHECKERS` lists intentional omissions).
    - A checker a definition names that `docs.yml` does not run fails, unless
      `GATE_NOT_RUN_IN_DOCS_WORKFLOW` lists it with the reason.
  A checker is recognised only in the form `python3 scripts/<name>.py` (or
  `python scripts/<name>.py`). `uv run ... python scripts/x.py`, `python3 -u`
  and `python3 -m` are not recognised, so a workflow step or a gate table that
  uses them is invisible to this check; write the plain form in both places.
  Whole-line `#` comments are ignored.

Deliberately not checked: the test table of `tsf-tip-development`
(`references/itf-interfaces.md`) against `docs/test-inventory.md`.
`check_skill_references.py` already requires every named target to exist, and
requiring every `tsf_tip_*_tests` entry would need a long exclusion list for a
list that is an entry point, not an inventory.

Exit status is 0 when the lists agree and 1 otherwise.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys


sys.dont_write_bytecode = True
REPO_ROOT = Path(__file__).resolve().parents[1]

WINDOWS_WORKFLOW = ".github/workflows/windows.yml"
DOCS_WORKFLOW = ".github/workflows/docs.yml"
GATE_JOB = "ci-gate"
JOB_TABLE_HEADING = "windows.yml の job"
JOB_TABLE_FILES = (
    ".claude/skills/azookey-ci-workflows/references/spec-routing.md",
    ".agents/skills/azookey-ci-workflows/references/spec-routing.md",
)
GATE_DEFINITIONS = (
    ".claude/agents/quality-gate-runner.md",
    ".codex/agents/quality-gate-runner.toml",
)

REQUIRED = "必須"
ADVISORY = "advisory"

# windows.yml jobs the Skill table intentionally omits. job id -> reason.
EXCLUDED_WORKFLOW_JOBS: dict[str, str] = {
    GATE_JOB: "集約 job 自身で、自分の needs には入らない（役割は表の上の行で説明する）",
}

# docs.yml checkers the gate table intentionally omits. script -> reason.
GATE_EXCLUDED_CHECKERS: dict[str, str] = {
    "check_docs_impact.py": "PR 本文（--body-file）を要するので、ゲート表では実行せず親へ回す旨を本文に書く",
}

# Checkers a gate definition may name although docs.yml does not run them.
# script -> reason.
GATE_NOT_RUN_IN_DOCS_WORKFLOW: dict[str, str] = {}

JOB_KEY_PATTERN = re.compile(r"^  [\"']?([A-Za-z0-9_-]+)[\"']?:\s*(?:#.*)?$")
TOP_LEVEL_KEY_PATTERN = re.compile(r"^[A-Za-z0-9_-]+:")
JOB_ROW_PATTERN = re.compile(r"^\|\s*`([A-Za-z0-9_-]+)`\s*\|\s*([^|]*?)\s*\|")
HEADING_PATTERN = re.compile(r"^(#{1,6})\s+(.*?)\s*$")
SCRIPT_COMMAND_PATTERN = re.compile(r"\bpython3?\s+scripts/([A-Za-z0-9_-]+\.py)\b")


def parse_workflow_jobs(text: str) -> dict[str, list[str]]:
    """Return {job id: its lines} for the top-level `jobs:` mapping."""
    jobs: dict[str, list[str]] = {}
    in_jobs = False
    current: str | None = None
    for line in text.splitlines():
        if not in_jobs:
            in_jobs = line.rstrip() == "jobs:"
            continue
        if TOP_LEVEL_KEY_PATTERN.match(line):
            break
        match = JOB_KEY_PATTERN.match(line)
        if match:
            current = str(match.group(1))
            jobs[current] = []
        elif current is not None:
            jobs[current].append(line)
    return jobs


def parse_needs(job_lines: list[str]) -> set[str]:
    """Return the `needs` of a job, in block or inline list form."""
    needs: set[str] = set()
    collecting = False
    for line in job_lines:
        if collecting:
            item = re.match(r"^      -\s+[\"']?([A-Za-z0-9_-]+)[\"']?\s*(?:#.*)?$", line)
            if item:
                needs.add(item.group(1))
                continue
            if line.strip() == "" or line.strip().startswith("#"):
                continue
            break
        match = re.match(r"^    needs:\s*(.*?)\s*(?:#.*)?$", line)
        if not match:
            continue
        value = match.group(1)
        if not value:
            collecting = True
        elif value.startswith("[") and value.endswith("]"):
            parts = (part.strip().strip("\"'") for part in value[1:-1].split(","))
            needs.update(part for part in parts if part)
        elif not value.startswith("["):
            needs.add(value.strip("\"'"))
        if value:
            break
    return needs


def parse_job_table(text: str, duplicates: list[str] | None = None) -> dict[str, str]:
    """Return {job id: ci-gate column} for the table under JOB_TABLE_HEADING.

    A job that has more than one row is appended to `duplicates` when given.
    """
    rows: dict[str, str] = {}
    in_section = False
    section_level = 0
    for line in text.splitlines():
        heading = HEADING_PATTERN.match(line)
        if heading:
            if in_section and len(heading.group(1)) <= section_level:
                break
            if heading.group(2) == JOB_TABLE_HEADING:
                in_section = True
                section_level = len(heading.group(1))
            continue
        if not in_section:
            continue
        row = JOB_ROW_PATTERN.match(line)
        if row:
            if row.group(1) in rows and duplicates is not None:
                duplicates.append(row.group(1))
            rows[row.group(1)] = row.group(2)
    return rows


def check_job_table(repo_root: Path) -> list[str]:
    workflow_path = repo_root / WINDOWS_WORKFLOW
    jobs = parse_workflow_jobs(workflow_path.read_text(encoding="utf-8"))
    if GATE_JOB not in jobs:
        return [f"{WINDOWS_WORKFLOW}: `{GATE_JOB}` job が見つかりません"]
    required = parse_needs(jobs[GATE_JOB])
    if not required:
        return [
            f"{WINDOWS_WORKFLOW}: `{GATE_JOB}` の needs を解釈できません"
            "（block 形式か 1 行の inline 形式で書く。複数行の `[` 形式は読めない）"
        ]
    problems: list[str] = []

    for job in sorted(required - jobs.keys()):
        problems.append(f"{WINDOWS_WORKFLOW}: `{GATE_JOB}` の needs が存在しない job `{job}` を指しています")

    for relative in JOB_TABLE_FILES:
        path = repo_root / relative
        if not path.exists():
            problems.append(f"{relative}: ファイルがありません")
            continue
        duplicates: list[str] = []
        table = parse_job_table(path.read_text(encoding="utf-8"), duplicates)
        if not table:
            problems.append(f"{relative}: 「{JOB_TABLE_HEADING}」の見出しの下に job の表がありません")
            continue
        for job in sorted(set(duplicates)):
            problems.append(f"{relative}: `{job}` の行が表に複数あります")
        for job in sorted(table.keys() - jobs.keys()):
            problems.append(f"{relative}: {WINDOWS_WORKFLOW} に無い job `{job}` が表にあります")
        for job in sorted(jobs.keys() - table.keys() - EXCLUDED_WORKFLOW_JOBS.keys()):
            problems.append(
                f"{relative}: {WINDOWS_WORKFLOW} の job `{job}` が表にありません"
                f"（載せるか、{Path(__file__).name} の EXCLUDED_WORKFLOW_JOBS に理由付きで足す）"
            )
        for job, column in sorted(table.items()):
            if job not in jobs:
                continue
            if column not in (REQUIRED, ADVISORY):
                problems.append(
                    f"{relative}: `{job}` の ci-gate 列は `{REQUIRED}` か `{ADVISORY}` にしてください（現在 `{column}`）"
                )
            elif (column == REQUIRED) != (job in required):
                expected = REQUIRED if job in required else ADVISORY
                problems.append(
                    f"{relative}: `{job}` の ci-gate 列が `{column}` ですが、"
                    f"`{GATE_JOB}` の needs に従うと `{expected}` です"
                )
    return problems


def workflow_checkers(text: str) -> set[str]:
    """Return the checkers named in `text`, ignoring whole-line `#` comments."""
    lines = (line for line in text.splitlines() if not line.lstrip().startswith("#"))
    return set(SCRIPT_COMMAND_PATTERN.findall("\n".join(lines)))


def check_gate_definitions(repo_root: Path) -> list[str]:
    docs_path = repo_root / DOCS_WORKFLOW
    run_by_docs = workflow_checkers(docs_path.read_text(encoding="utf-8"))
    problems: list[str] = []
    for relative in GATE_DEFINITIONS:
        path = repo_root / relative
        if not path.exists():
            problems.append(f"{relative}: ファイルがありません")
            continue
        named = workflow_checkers(path.read_text(encoding="utf-8"))
        for script in sorted(run_by_docs - named - GATE_EXCLUDED_CHECKERS.keys()):
            problems.append(
                f"{relative}: {DOCS_WORKFLOW} が実行する `scripts/{script}` がゲート表にありません"
                f"（足すか、{Path(__file__).name} の GATE_EXCLUDED_CHECKERS に理由付きで足す）"
            )
        for script in sorted(named - run_by_docs - GATE_NOT_RUN_IN_DOCS_WORKFLOW.keys()):
            problems.append(
                f"{relative}: ゲート表の `scripts/{script}` を {DOCS_WORKFLOW} は実行していません"
                f"（{Path(__file__).name} の GATE_NOT_RUN_IN_DOCS_WORKFLOW に理由付きで足すか、表から外す）"
            )
    return problems


def check(repo_root: Path) -> list[str]:
    return check_job_table(repo_root) + check_gate_definitions(repo_root)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=REPO_ROOT,
        help="検査対象のリポジトリルート（既定: このスクリプトの親ディレクトリ）",
    )
    arguments = parser.parse_args(argv)
    problems = check(arguments.repo_root.resolve())
    if problems:
        print("Skill・agent 定義の一覧表が CI の実体と一致しません:", file=sys.stderr)
        for problem in problems:
            print(f"  - {problem}", file=sys.stderr)
        return 1
    print("Skill・agent 定義の一覧表は CI の実体と一致しています。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
